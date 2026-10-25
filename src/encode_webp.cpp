// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Encoding a WebP with libwebp - lossless, or lossy from packed pixels or straight from NV12
// planes - its ICC profile, EXIF and XMP muxed in. Moved from Diffractor's files_webp.cpp.

#include "libfiles/encode.h"
#include "encode_internal.h"

#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles
{
	using namespace encode_detail;

	namespace
	{
		std::string s_fail_next_webp_chunk;

		// VP8 encodes YCbCr 4:2:0, so libwebp converts any ARGB picture it is given back to planes before
		// it starts. An NV12 image already is those planes, and handing them over directly skips both
		// that conversion and the one a JPEG decode did to produce packed pixels in the first place.
		//
		// The one thing that does not carry over is range. libwebp's YUV is limited-range BT.601 and VP8
		// signals no range at all, so every decoder applies the limited-range inverse - a full-range
		// source handed over untouched comes back with crushed blacks and blown highlights.
		bool import_nv12(WebPPicture& picture, const image_view& image)
		{
			picture.use_argb = 0;
			picture.colorspace = WEBP_YUV420;

			if (!WebPPictureAlloc(&picture)) return false;

			const auto full_range = !image.limited_range;

			// 219 of 256 luma codes and 224 chroma codes, offset to 16. Rounded in floating point because
			// integer division truncates toward zero, which biases the whole lower half of the chroma range
			// by a code. Built once rather than per sample - a thumbnail is far more pixels than entries.
			std::array<std::uint8_t, 256> luma_map{};
			std::array<std::uint8_t, 256> chroma_map{};

			for (auto i = 0; i < 256; ++i)
			{
				luma_map[i] = full_range
					              ? static_cast<std::uint8_t>(std::clamp(16 + std::lround(i * 219.0 / 255.0), 16L, 235L))
					              : static_cast<std::uint8_t>(i);
				chroma_map[i] = full_range
					                ? static_cast<std::uint8_t>(std::clamp(128 + std::lround((i - 128) * 224.0 / 255.0),
					                                                       16L, 240L))
					                : static_cast<std::uint8_t>(i);
			}

			const auto src_stride = image.stride;
			const auto* const src_luma = image.pixels;
			const auto* const src_chroma = src_luma + src_stride * image.height;

			for (std::uint32_t y = 0; y < image.height; ++y)
			{
				const auto* const src = src_luma + src_stride * y;
				auto* const dst = picture.y + static_cast<std::size_t>(picture.y_stride) * y;

				for (std::uint32_t x = 0; x < image.width; ++x) dst[x] = luma_map[src[x]];
			}

			// NV12 interleaves U and V; libwebp wants them apart, so the de-interleave rides along with
			// the range map rather than costing a pass of its own.
			const auto chroma_width = image.width / 2;
			const auto chroma_height = image.height / 2;

			for (std::uint32_t y = 0; y < chroma_height; ++y)
			{
				const auto* const src = src_chroma + src_stride * y;
				auto* const dst_u = picture.u + static_cast<std::size_t>(picture.uv_stride) * y;
				auto* const dst_v = picture.v + static_cast<std::size_t>(picture.uv_stride) * y;

				for (std::uint32_t x = 0; x < chroma_width; ++x)
				{
					dst_u[x] = chroma_map[src[x * 2]];
					dst_v[x] = chroma_map[src[x * 2 + 1]];
				}
			}

			return true;
		}
	}

	void test_hooks::fail_next_webp_chunk(const std::string_view fourcc)
	{
		s_fail_next_webp_chunk = fourcc;
	}

	encoded encode_webp(const image_view& image, const std::uint16_t orientation, const embedded_metadata& metadata,
	                    const webp_options& options)
	{
		encoded result;

		// Lossless is an ARGB codec; there is no planar form of it. And one chroma pair per 2x2 luma
		// block: an odd side would leave libwebp's last chroma column unwritten.
		const auto planar = image.format == pixel_format::nv12 && !options.lossless &&
			((image.width | image.height) & 1) == 0;

		if (!planar && image.format != pixel_format::bgrx8 && image.format != pixel_format::bgra8)
		{
			return result;
		}

		const std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux(WebPMuxNew(), &WebPMuxDelete);

		if (mux)
		{
			std::vector<std::uint8_t> rotate_exif;

			const auto use_alpha = image.format == pixel_format::bgra8;

			WebPPicture picture;
			WebPPictureInit(&picture);

			picture.width = static_cast<int>(image.width);
			picture.height = static_cast<int>(image.height);

			bool ok;

			if (planar)
			{
				ok = import_nv12(picture, image);
			}
			else
			{
				picture.use_argb = true;
				ok = use_alpha
					     ? WebPPictureImportBGRA(&picture, image.pixels, static_cast<int>(image.stride)) != 0
					     : WebPPictureImportBGRX(&picture, image.pixels, static_cast<int>(image.stride)) != 0;
			}

			if (ok)
			{
				WebPMemoryWriter memory_writer;
				WebPMemoryWriterInit(&memory_writer);

				WebPConfig config;
				WebPConfigInit(&config);

				if (options.lossless)
				{
					WebPConfigLosslessPreset(&config, 7);
					config.thread_level = 1;
				}
				else
				{
					// A thumbnail is a rebuildable cache entry written for every indexed item, so it takes a
					// much cheaper search than a file the user asked to save.
					config.thread_level = options.fast ? 0 : 1;
					config.lossless = false;
					config.quality = static_cast<float>(options.quality);
					config.method = options.fast ? 2 : 6;
					config.use_sharp_yuv = options.fast ? 0 : 1;
					// https://groups.google.com/a/webmproject.org/forum/#!topic/webp-discuss/7dV1qXrdQ2Y
					config.alpha_quality = options.lossy_alpha ? options.quality : 100;
				}

				const auto valid_config = WebPValidateConfig(&config) != 0;

				if (!valid_config) result.notes.emplace_back("rejected webp encoder configuration");

				picture.writer = WebPMemoryWrite;
				picture.custom_ptr = &memory_writer;

				const int success = valid_config && WebPEncode(&config, &picture);

				if (valid_config && !success)
				{
					result.notes.push_back(std::format("webp encode failed with error {}",
					                                   static_cast<int>(picture.error_code)));
				}

				if (success)
				{
					WebPData image_data = {memory_writer.mem, memory_writer.size};
					const WebPMuxError img_err = WebPMuxSetImage(mux.get(), &image_data, 0);

					if (img_err == WEBP_MUX_OK)
					{
						auto metadata_ok = true;
						const auto set_chunk = [&mux, &metadata_ok, &result](const char fourcc[5],
						                                                     const WebPData& chunk_data)
						{
							if (s_fail_next_webp_chunk == fourcc)
							{
								s_fail_next_webp_chunk.clear();
								result.notes.push_back(std::format("webp {} chunk insertion failed by test hook",
								                                   fourcc));
								metadata_ok = false;
								return;
							}

							const auto err = WebPMuxSetChunk(mux.get(), fourcc, &chunk_data, 0);
							if (err != WEBP_MUX_OK)
							{
								result.notes.push_back(std::format("webp {} chunk insertion failed with error {}",
								                                   fourcc, static_cast<int>(err)));
								metadata_ok = false;
							}
						};

						if (!metadata.icc.empty())
						{
							WebPData chunk_data;
							chunk_data.bytes = metadata.icc.data();
							chunk_data.size = metadata.icc.size();
							set_chunk("ICCP", chunk_data);
						}

						if (!metadata.exif.empty())
						{
							const auto exif_skip = is_exif_signature(metadata.exif) ? exif_signature.size() : 0u;
							WebPData chunk_data;
							chunk_data.bytes = metadata.exif.data() + exif_skip;
							chunk_data.size = metadata.exif.size() - exif_skip;
							set_chunk("EXIF", chunk_data);
						}
						else if (orientation != 1)
						{
							rotate_exif = orientation_exif(orientation);
							const auto exif_skip = is_exif_signature(rotate_exif) ? exif_signature.size() : 0u;
							WebPData chunk_data;
							chunk_data.bytes = rotate_exif.data() + exif_skip;
							chunk_data.size = rotate_exif.size() - exif_skip;
							set_chunk("EXIF", chunk_data);
						}

						if (!metadata.xmp.empty())
						{
							WebPData chunk_data;
							chunk_data.bytes = metadata.xmp.data();
							chunk_data.size = metadata.xmp.size();
							set_chunk("XMP ", chunk_data);
						}

						if (metadata_ok)
						{
							WebPData output_data;
							WebPDataInit(&output_data);

							const WebPMuxError err = WebPMuxAssemble(mux.get(), &output_data);

							if (err == WEBP_MUX_OK)
							{
								result.data.assign(output_data.bytes, output_data.bytes + output_data.size);
								WebPDataClear(&output_data);
							}
						}
					}
				}

				WebPMemoryWriterClear(&memory_writer);
			}

			WebPPictureFree(&picture);
		}

		return result;
	}
}
