// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Decoding a JPEG XL with libjxl into memory the caller provides: its first frame, as B8G8R8A8
// with straight alpha where it has alpha and B8G8R8X8 where it has none - libjxl writes 255 in the fourth
// byte - or, for a picture of more than eight bits and a caller that takes sixteen, as R16G16B16A16; left
// unturned, its codestream's orientation reported. libjxl writes RGBA into the caller's rows and the red and
// blue are swapped there for bytes. A picture whose colour encoding is PQ or HLG is an HDR still, decoded at
// sixteen bits and mapped to SDR as hdr_internal.h describes, or kept as its signal for a caller that keeps HDR.
// The sink is asked for memory as soon as the size is known, or, where keeping HDR decides the format, once the
// colour encoding has said whether the picture is HDR. libjxl's resizable runner spreads the
// decode over a thread for each group of 256 by 256 pixels, up to eight, the caller's own among them.

#include "decode_internal.h"
#include "hdr_internal.h"
#include "scan_internal.h"

#include <jxl/decode.h>
#include <jxl/resizable_parallel_runner.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		struct decoder_destroy
		{
			void operator()(JxlDecoder* d) const noexcept { JxlDecoderDestroy(d); }
		};

		struct runner_destroy
		{
			void operator()(void* r) const noexcept { JxlResizableParallelRunnerDestroy(r); }
		};

		// As for the encoder: more workers than this mostly add each one's scratch memory, which a 32-bit
		// process cannot spare, and a caller decoding several images at once has threads of its own.
		constexpr std::uint32_t max_threads = 8;

		// RGBA, as libjxl writes it, to BGRA in place: the pixels of each row, and not its padding.
		void swap_red_blue(const pixel_buffer pixels, const std::uint32_t width, const std::uint32_t height)
		{
			for (std::uint32_t y = 0; y < height; ++y)
			{
				auto* p = pixels.pixels + pixels.stride * y;

				for (std::uint32_t x = 0; x < width; ++x, p += 4)
				{
					const auto red = p[0];
					p[0] = p[2];
					p[2] = red;
				}
			}
		}
		// The chromaticities of the colour encoding libjxl decodes to, by its enumerations or its own numbers.
		chromaticities jxl_chromaticities(const JxlColorEncoding& encoding)
		{
			chromaticities result;

			switch (encoding.primaries)
			{
			case JXL_PRIMARIES_2100: result = *chromaticities_of(9); break;
			case JXL_PRIMARIES_P3: result = *chromaticities_of(12); break;
			case JXL_PRIMARIES_CUSTOM:
				result.red_x = encoding.primaries_red_xy[0];
				result.red_y = encoding.primaries_red_xy[1];
				result.green_x = encoding.primaries_green_xy[0];
				result.green_y = encoding.primaries_green_xy[1];
				result.blue_x = encoding.primaries_blue_xy[0];
				result.blue_y = encoding.primaries_blue_xy[1];
				break;
			default: break;
			}

			switch (encoding.white_point)
			{
			case JXL_WHITE_POINT_DCI:
				result.white_x = 0.314;
				result.white_y = 0.351;
				break;
			case JXL_WHITE_POINT_E: result.white_x = result.white_y = 1.0 / 3.0; break;
			case JXL_WHITE_POINT_CUSTOM:
				result.white_x = encoding.white_point_xy[0];
				result.white_y = encoding.white_point_xy[1];
				break;
			default:
				result.white_x = 0.3127;
				result.white_y = 0.3290;
				break;
			}

			return result;
		}

		// The H.273 code point naming the encoding's primaries: 1 for sRGB's, 9 for BT.2100's, 12 for P3's on D65 and
		// 11 for DCI-P3's own white; 2, unspecified, for primaries or a white given as numbers.
		std::uint8_t colour_primaries_of(const JxlColorEncoding& encoding)
		{
			constexpr std::uint8_t unspecified = 2;
			const auto d65 = encoding.white_point == JXL_WHITE_POINT_D65;

			switch (encoding.primaries)
			{
			case JXL_PRIMARIES_SRGB: return d65 ? std::uint8_t{1} : unspecified;
			case JXL_PRIMARIES_2100: return d65 ? std::uint8_t{9} : unspecified;
			case JXL_PRIMARIES_P3:
				return d65                                           ? std::uint8_t{12}
				       : encoding.white_point == JXL_WHITE_POINT_DCI ? std::uint8_t{11}
				                                                     : unspecified;
			default: return unspecified;
			}
		}
	}

	void decode_jxl(reader& r, image_sink& sink, const decode_options& options, decode_result& result)
	{
		// libjxl reads a whole file from memory: a reader over memory lends it, and one over a stream
		// reads it in.
		auto file = r.resident();
		std::vector<std::uint8_t> copy;

		if (file.empty() && r.size() != 0)
		{
			if (r.size() > std::numeric_limits<std::size_t>::max()) throw std::bad_alloc();
			copy = r.read(0, static_cast<std::size_t>(r.size()));
			file = copy;
		}

		const std::unique_ptr<JxlDecoder, decoder_destroy> decoder(JxlDecoderCreate(nullptr));
		// No threads until the image's size is known; one that fits a group is decoded on this thread alone.
		const std::unique_ptr<void, runner_destroy> runner(JxlResizableParallelRunnerCreate(nullptr));

		if (!decoder || !runner)
		{
			result.error = "could not create a libjxl decoder";
			return;
		}

		// The pixels as the codestream stores them, its orientation reported rather than applied, so the
		// size decoded is the size a scan reports; and alpha made straight where it is premultiplied.
		if (JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
		                                                 JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
		    JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
		    JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
		    JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS)
		{
			result.error = "libjxl refused the decoder's options";
			return;
		}

		JxlDecoderSetInput(decoder.get(), file.data(), file.size());
		JxlDecoderCloseInput(decoder.get());

		frame_info frame;
		pixel_buffer pixels;
		auto bits = 8;
		auto has_alpha = false;
		double intensity_target = 0.0;
		std::optional<hdr_mapping> mapping;
		// Sixteen-bit RGBA signal for a picture mapped to SDR, written to the sink once it is whole.
		std::vector<std::uint8_t> signal;

		for (;;)
		{
			const auto status = JxlDecoderProcessInput(decoder.get());

			if (status == JXL_DEC_BASIC_INFO)
			{
				JxlBasicInfo info = {};

				if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS)
				{
					result.error = "libjxl could not read the image's header";
					return;
				}

				if (info.xsize == 0 || info.ysize == 0 || info.xsize > max_image_dimension ||
				    info.ysize > max_image_dimension)
				{
					result.error = "the JPEG XL's image is larger than libfiles decodes, or has no size";
					return;
				}

				frame.source_width = info.xsize;
				frame.source_height = info.ysize;
				frame.width = info.xsize;
				frame.height = info.ysize;
				bits = static_cast<int>(info.bits_per_sample);
				has_alpha = info.alpha_bits > 0;
				// JxlOrientation numbers its turns as EXIF does.
				frame.orientation = info.orientation >= JXL_ORIENT_IDENTITY &&
				                            info.orientation <= JXL_ORIENT_ROTATE_90_CCW
				                        ? static_cast<std::uint16_t>(info.orientation)
				                        : std::uint16_t{1};
				// The light the brightest signal stands for, which a PQ picture's highlights roll off from where it is
				// below 10000, a bound that says nothing.
				intensity_target = info.intensity_target > 0.0f ? info.intensity_target : 0.0;

				const auto threads = JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize);
				JxlResizableParallelRunnerSetThreads(runner.get(), std::min(threads, max_threads));

				// The sink is asked for memory as soon as the size is known - unless the colour encoding, which says
				// whether the picture is HDR, is still to decide its format: for a caller that keeps HDR, of a picture
				// the header alone would not decode at sixteen bits.
				if (!options.keep_hdr || (bits > 8 && options.allow_16bit))
				{
					frame.format = output_format(bits, has_alpha, options, options.keep_hdr);
					pixels = sink.allocate(frame);
					result.frame = frame;

					if (!pixels.pixels)
					{
						result.refused = true;
						return;
					}
				}
			}
			else if (status == JXL_DEC_COLOR_ENCODING)
			{
				// The pixels' own colour encoding - for an XYB codestream, what libjxl decodes it to - which says
				// whether the picture is HDR. It follows the header, before any pixel.
				JxlColorEncoding encoding = {};

				if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_DATA, &encoding) ==
				        JXL_DEC_SUCCESS &&
				    encoding.color_space != JXL_COLOR_SPACE_XYB)
				{
					if (const auto transfer = hdr_transfer_of(static_cast<int>(encoding.transfer_function)))
					{
						hdr_description described;
						described.transfer = *transfer;
						described.colour_primaries = colour_primaries_of(encoding);
						described.primaries = jxl_chromaticities(encoding);
						described.intensity_target = intensity_target;
						frame.hdr = described;
					}
				}

				const auto keep = frame.hdr && options.keep_hdr;
				frame.tone_mapped = frame.hdr && !keep;
				if (frame.tone_mapped) mapping.emplace(*frame.hdr);

				if (!pixels.pixels)
				{
					frame.format = output_format(bits, has_alpha, options, keep);
					pixels = sink.allocate(frame);

					if (!pixels.pixels)
					{
						result.frame = frame;
						result.refused = true;
						return;
					}
				}

				result.frame = frame;
			}
			else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
			{
				if (!pixels.pixels)
				{
					result.error = "libjxl asked for the image before it gave its size";
					return;
				}

				const auto wide = frame.format == pixel_format::rgba16 || frame.format == pixel_format::rgbx16;
				JxlPixelFormat format{4, wide || mapping ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
				auto* out = pixels.pixels;
				auto stride = pixels.stride;

				if (mapping)
				{
					// Mapped as the frame completes: the signal goes into memory of its own first.
					stride = std::size_t{8} * frame.width;
					signal.resize(stride * frame.height);
					out = signal.data();
				}

				// RGBA rows stride apart, written where the sink or the signal wants them; the last row needs no
				// padding after it.
				format.align = stride;
				const auto size = stride * (frame.height - 1) +
				                  (format.data_type == JXL_TYPE_UINT16 ? 8 : 4) * std::size_t{frame.width};

				if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, out, size) != JXL_DEC_SUCCESS)
				{
					result.error = "libjxl refused the image's memory";
					return;
				}
			}
			else if (status == JXL_DEC_FULL_IMAGE)
			{
				// The first frame is the still image.
				if (mapping)
				{
					write_wide_rows(signal.data(), std::size_t{8} * frame.width, 65535, frame.width, frame.height,
					                false, &*mapping, frame.format, pixels);
				}
				else if (frame.format == pixel_format::bgra8 || frame.format == pixel_format::bgrx8)
				{
					swap_red_blue(pixels, frame.width, frame.height);
				}

				result.frame = frame;
				result.success = true;
				return;
			}
			else
			{
				// Done without a frame, or an error. Its input closed, libjxl reports a file cut short as an
				// error too, so the two are not told apart.
				result.error = "libjxl could not decode the image, which is malformed or cut short";
				return;
			}
		}
	}
}
