// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Decoding a WebP with libwebp into memory the caller provides: B8G8R8A8 where the image has alpha
// and B8G8R8X8 where it has none; reduced by libwebp's scaler while it decodes, to the size scaled_size_for
// gives; or, for an opaque lossy image the caller takes as NV12, its own limited-range YCbCr, an odd edge
// cropped. An animation gives its first frame as its canvas shows it, unreduced. Moved from Diffractor's
// loader, the orientation read through libwebp's mux as it read it.

#include "decode_internal.h"

#include "libfiles/scan.h"

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/mux.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint8_t exif_signature[6] = {'E', 'x', 'i', 'f', 0, 0};

		// The file's first EXIF chunk, as libwebp's mux finds it - only in a file the mux can read
		// whole - less a JPEG "Exif\0\0" signature if one leads it. It lies in the file's memory.
		std::span<const std::uint8_t> exif_chunk(const std::span<const std::uint8_t> file)
		{
			const WebPData data{file.data(), file.size()};
			auto* const mux = WebPMuxCreate(&data, 0);
			if (!mux) return {};

			std::span<const std::uint8_t> exif;
			std::uint32_t flags = 0;
			WebPMuxGetFeatures(mux, &flags);

			if (flags & EXIF_FLAG)
			{
				WebPData chunk{};

				if (WebPMuxGetChunk(mux, "EXIF", &chunk) == WEBP_MUX_OK)
				{
					exif = {chunk.bytes, chunk.size};

					if (exif.size() >= sizeof(exif_signature) &&
					    std::equal(std::begin(exif_signature), std::end(exif_signature), exif.begin()))
					{
						exif = exif.subspan(sizeof(exif_signature));
					}
				}
			}

			WebPMuxDelete(mux);
			return exif;
		}

		const char* describe(const VP8StatusCode status)
		{
			switch (status)
			{
			case VP8_STATUS_OUT_OF_MEMORY: return "libwebp ran out of memory";
			case VP8_STATUS_INVALID_PARAM: return "libwebp was given an invalid parameter";
			case VP8_STATUS_BITSTREAM_ERROR: return "the WebP bitstream is malformed";
			case VP8_STATUS_UNSUPPORTED_FEATURE:
				return "the WebP uses a feature libwebp's still decoder does not read, an animation say";
			case VP8_STATUS_NOT_ENOUGH_DATA: return "the file is cut short";
			default: return "libwebp could not decode the image";
			}
		}

		void fail(decode_result& result, const VP8StatusCode status)
		{
			result.success = false;
			result.truncated = status == VP8_STATUS_NOT_ENOUGH_DATA;
			result.error = describe(status);
		}

		// libwebp writes the two chroma planes apart; NV12 interleaves them after the luma rows.
		VP8StatusCode decode_nv12(const std::span<const std::uint8_t> file, const pixel_buffer pixels,
		                          const int even_width, const int even_height)
		{
			const auto chroma_width = even_width / 2;
			const auto chroma_height = even_height / 2;
			const auto chroma_size = static_cast<std::size_t>(chroma_width) * chroma_height;
			std::vector<std::uint8_t> chroma(chroma_size * 2);
			auto* const u = chroma.data();
			auto* const v = u + chroma_size;

			WebPDecoderConfig config;
			if (!WebPInitDecoderConfig(&config)) return VP8_STATUS_INVALID_PARAM;

			// A crop origin has to be even for 4:2:0 chroma to stay aligned, which zero is.
			config.options.use_cropping = 1;
			config.options.crop_width = even_width;
			config.options.crop_height = even_height;

			const auto luma_stride = static_cast<int>(pixels.stride);
			config.output.colorspace = MODE_YUV;
			config.output.is_external_memory = 1;
			config.output.u.YUVA.y = pixels.pixels;
			config.output.u.YUVA.y_stride = luma_stride;
			config.output.u.YUVA.y_size = static_cast<std::size_t>(luma_stride) * even_height;
			config.output.u.YUVA.u = u;
			config.output.u.YUVA.u_stride = chroma_width;
			config.output.u.YUVA.u_size = chroma_size;
			config.output.u.YUVA.v = v;
			config.output.u.YUVA.v_stride = chroma_width;
			config.output.u.YUVA.v_size = chroma_size;

			const auto status = WebPDecode(file.data(), file.size(), &config);
			WebPFreeDecBuffer(&config.output);
			if (status != VP8_STATUS_OK) return status;

			auto* const uv = pixels.pixels + static_cast<std::size_t>(luma_stride) * even_height;

			for (auto y = 0; y < chroma_height; ++y)
			{
				auto* const dst = uv + static_cast<std::size_t>(y) * luma_stride;
				const auto* const src_u = u + static_cast<std::size_t>(y) * chroma_width;
				const auto* const src_v = v + static_cast<std::size_t>(y) * chroma_width;

				for (auto x = 0; x < chroma_width; ++x)
				{
					dst[x * 2] = src_u[x];
					dst[x * 2 + 1] = src_v[x];
				}
			}

			return VP8_STATUS_OK;
		}

		// BGRA, opaque images writing 255 in the fourth byte, reduced by libwebp's scaler when the frame
		// is smaller than the source.
		VP8StatusCode decode_bgra(const std::span<const std::uint8_t> file, const pixel_buffer pixels,
		                          const frame_info& frame)
		{
			WebPDecoderConfig config;
			if (!WebPInitDecoderConfig(&config)) return VP8_STATUS_INVALID_PARAM;

			if (frame.width != frame.source_width || frame.height != frame.source_height)
			{
				config.options.use_scaling = 1;
				config.options.scaled_width = static_cast<int>(frame.width);
				config.options.scaled_height = static_cast<int>(frame.height);
			}

			config.output.colorspace = MODE_BGRA;
			config.output.is_external_memory = 1;
			config.output.u.RGBA.rgba = pixels.pixels;
			config.output.u.RGBA.stride = static_cast<int>(pixels.stride);
			config.output.u.RGBA.size = pixels.stride * frame.height;

			const auto status = WebPDecode(file.data(), file.size(), &config);
			WebPFreeDecBuffer(&config.output);
			return status;
		}

		// An animation's first frame as its canvas shows once that frame is drawn, which libwebp's animation
		// decoder composites, copied to the caller's rows at the canvas's size: the animation decoder does not
		// scale. A canvas the frame leaves uncovered is transparent; an opaque image writes 255 there all the
		// same, as the fourth byte says nothing.
		VP8StatusCode decode_first_frame(const std::span<const std::uint8_t> file, const pixel_buffer pixels,
		                                 const frame_info& frame)
		{
			WebPAnimDecoderOptions options;
			if (!WebPAnimDecoderOptionsInit(&options)) return VP8_STATUS_INVALID_PARAM;
			options.color_mode = MODE_BGRA;
			options.use_threads = 0;

			const WebPData data{file.data(), file.size()};
			const std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)>
				decoder(WebPAnimDecoderNew(&data, &options), &WebPAnimDecoderDelete);

			// The demuxer reads only a whole file, so one cut short is told by the size its header gives.
			if (!decoder)
			{
				const auto declared = file.size() >= 8 ? (std::uint64_t{file[4]} | std::uint64_t{file[5]} << 8 |
				                                          std::uint64_t{file[6]} << 16 | std::uint64_t{file[7]} << 24) +
				                                             8u
				                                       : 0u;
				return declared > file.size() ? VP8_STATUS_NOT_ENOUGH_DATA : VP8_STATUS_BITSTREAM_ERROR;
			}

			WebPAnimInfo info;
			std::uint8_t* canvas = nullptr;
			auto timestamp = 0;

			if (!WebPAnimDecoderGetInfo(decoder.get(), &info) || info.canvas_width != frame.width ||
			    info.canvas_height != frame.height || !WebPAnimDecoderGetNext(decoder.get(), &canvas, &timestamp))
			{
				return VP8_STATUS_BITSTREAM_ERROR;
			}

			const auto row_bytes = static_cast<std::size_t>(frame.width) * 4u;

			for (std::uint32_t y = 0; y < frame.height; ++y)
			{
				auto* const row = pixels.pixels + static_cast<std::size_t>(y) * pixels.stride;
				std::copy_n(canvas + y * row_bytes, row_bytes, row);

				if (frame.format == pixel_format::bgrx8)
				{
					for (std::size_t x = 3; x < row_bytes; x += 4) row[x] = 0xFF;
				}
			}

			return VP8_STATUS_OK;
		}
	}

	void decode_webp(reader& r, image_sink& sink, const decode_options& options, decode_result& result)
	{
		// libwebp reads a whole file from memory: a reader over memory lends it, and one over a stream
		// reads it in.
		auto file = r.resident();
		std::vector<std::uint8_t> copy;

		if (file.empty() && r.size() != 0)
		{
			if (r.size() > std::numeric_limits<std::size_t>::max()) throw std::bad_alloc();
			copy = r.read(0, static_cast<std::size_t>(r.size()));
			file = copy;
		}

		WebPBitstreamFeatures features;
		const auto status = WebPGetFeatures(file.data(), file.size(), &features);

		if (status != VP8_STATUS_OK)
		{
			fail(result, status);
			return;
		}

		const auto width = static_cast<std::uint32_t>(features.width);
		const auto height = static_cast<std::uint32_t>(features.height);
		auto scaled = features.has_animation
		                  ? image_size{width, height}
		                  : scaled_size_for(width, height, options.target_width, options.target_height);
		if (options.round_webp_size_down && options.target_width && options.target_height && !features.has_animation)
		{
			const auto scale = std::min({1.0, static_cast<double>(options.target_width) / width,
			                             static_cast<double>(options.target_height) / height});
			scaled.width = std::max(1u, static_cast<std::uint32_t>(width * scale));
			scaled.height = std::max(1u, static_cast<std::uint32_t>(height * scale));
		}
		const auto is_scaled = scaled.width != width || scaled.height != height;

		// A reduced decode takes the packed path: NV12 exists to hand a renderer its native planes.
		const auto nv12 = options.allow_nv12 && features.format == 1 && !features.has_alpha &&
		                  !features.has_animation && width >= 2 && height >= 2 && !is_scaled;

		frame_info frame;
		frame.source_width = width;
		frame.source_height = height;
		frame.width = nv12 ? width & ~1u : scaled.width;
		frame.height = nv12 ? height & ~1u : scaled.height;
		frame.format = nv12 ? pixel_format::nv12 : features.has_alpha ? pixel_format::bgra8 : pixel_format::bgrx8;
		frame.limited_range = nv12;

		const auto exif = exif_chunk(file);
		frame.orientation = summarize_exif(exif).orientation.value_or(1);
		frame.exif = exif;

		const auto pixels = sink.allocate(frame);
		frame.exif = {};
		result.frame = frame;

		if (!pixels.pixels)
		{
			result.refused = true;
			return;
		}

		const auto decoded = nv12                     ? decode_nv12(file, pixels, static_cast<int>(frame.width),
		                                                            static_cast<int>(frame.height))
		                     : features.has_animation ? decode_first_frame(file, pixels, frame)
		                                              : decode_bgra(file, pixels, frame);

		if (decoded != VP8_STATUS_OK)
		{
			fail(result, decoded);
			return;
		}

		result.success = true;
	}
}
