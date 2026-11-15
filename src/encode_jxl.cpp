// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Encoding a JPEG XL with libjxl from packed or sixteen-bit pixels, as libjxl's own cjxl drives it: the
// pixels put into the RGB order libjxl reads, at their own depth, the ICC profile or sRGB - or an HDR picture's
// description, its colour encoding and intensity target - the orientation in the codestream, and the EXIF and XMP in
// boxes of their own. Encoded on libjxl's thread runner. A JPEG is repacked as one without loss, and given back from
// one, by libjxl's own JPEG transcoder; and how one was compressed is read from its header.

#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "encode_internal.h"

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	using namespace encode_detail;

	namespace
	{
		struct encoder_destroy
		{
			void operator()(JxlEncoder* e) const noexcept { JxlEncoderDestroy(e); }
		};

		struct runner_destroy
		{
			void operator()(void* r) const noexcept { JxlThreadParallelRunnerDestroy(r); }
		};

		struct decoder_destroy
		{
			void operator()(JxlDecoder* d) const noexcept { JxlDecoderDestroy(d); }
		};

		struct resizable_runner_destroy
		{
			void operator()(void* r) const noexcept { JxlResizableParallelRunnerDestroy(r); }
		};

		// One image is encoded at a time and libjxl spreads it over groups of 256 pixels; more workers
		// than this mostly add each one's scratch memory, which a 32-bit process cannot spare.
		constexpr std::size_t max_worker_threads = 8;

		constexpr std::uint16_t tag_orientation = 0x0112;

		bool is_wide(const pixel_format format)
		{
			return format == pixel_format::rgbx16 || format == pixel_format::rgba16;
		}

		// libjxl reads red, green and blue in that order; bytes hold them blue first, sixteen-bit samples
		// already red first, which are kept as they are, the opaque fourth channel dropped.
		std::vector<std::uint8_t> to_rgb(const image_view& image, const std::size_t channels)
		{
			if (is_wide(image.format))
			{
				const auto row_samples = std::size_t{image.width} * channels;
				std::vector<std::uint8_t> rgb(row_samples * 2 * image.height);

				for (std::uint32_t y = 0; y < image.height; ++y)
				{
					const auto* src = reinterpret_cast<const std::uint16_t*>(image.pixels + image.stride * y);
					auto* dst = reinterpret_cast<std::uint16_t*>(rgb.data()) + row_samples * y;

					for (std::uint32_t x = 0; x < image.width; ++x, src += 4, dst += channels)
					{
						std::memcpy(dst, src, channels * 2);
					}
				}

				return rgb;
			}

			const auto row_bytes = std::size_t{image.width} * channels;
			std::vector<std::uint8_t> rgb(row_bytes * image.height);

			for (std::uint32_t y = 0; y < image.height; ++y)
			{
				const auto* src = image.pixels + image.stride * y;
				auto* dst = rgb.data() + row_bytes * y;

				for (std::uint32_t x = 0; x < image.width; ++x, src += 4, dst += channels)
				{
					dst[0] = src[2];
					dst[1] = src[1];
					dst[2] = src[0];
					if (channels == 4) dst[3] = src[3];
				}
			}

			return rgb;
		}

		// The 'Exif' box's contents: the offset of the TIFF header within what follows, here none, then the
		// TIFF stream without the signature a JPEG's APP1 begins with. Its orientation is made the
		// codestream's, which a JPEG XL reader follows whatever the EXIF says.
		std::vector<std::uint8_t> exif_box(std::span<const std::uint8_t> exif, const std::uint16_t orientation)
		{
			if (is_exif_signature(exif)) exif = exif.subspan(exif_signature.size());

			std::vector<std::uint8_t> box(4 + exif.size(), 0);
			std::memcpy(box.data() + 4, exif.data(), exif.size());

			const exif_number number{exif_directory::primary, tag_orientation, orientation};
			set_exif_numbers(std::span(box).subspan(4), std::span(&number, 1));
			return box;
		}

		// An HDR picture's description as libjxl's colour encoding: its primaries by name where JPEG XL has one for
		// them, else as numbers, with its white, and its transfer.
		JxlColorEncoding jxl_encoding_of(const hdr_description& hdr)
		{
			JxlColorEncoding e{};
			e.color_space = JXL_COLOR_SPACE_RGB;
			e.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
			e.transfer_function = hdr.transfer == hdr_transfer::hlg ? JXL_TRANSFER_FUNCTION_HLG
			                                                        : JXL_TRANSFER_FUNCTION_PQ;
			e.white_point = JXL_WHITE_POINT_D65;

			switch (hdr.colour_primaries)
			{
			case 1: e.primaries = JXL_PRIMARIES_SRGB; break;
			case 9: e.primaries = JXL_PRIMARIES_2100; break;
			case 11:
				e.primaries = JXL_PRIMARIES_P3;
				e.white_point = JXL_WHITE_POINT_DCI;
				break;
			case 12: e.primaries = JXL_PRIMARIES_P3; break;
			default:
			{
				const auto& p = hdr.primaries;
				e.primaries = JXL_PRIMARIES_CUSTOM;
				e.primaries_red_xy[0] = p.red_x;
				e.primaries_red_xy[1] = p.red_y;
				e.primaries_green_xy[0] = p.green_x;
				e.primaries_green_xy[1] = p.green_y;
				e.primaries_blue_xy[0] = p.blue_x;
				e.primaries_blue_xy[1] = p.blue_y;
				e.white_point = JXL_WHITE_POINT_CUSTOM;
				e.white_point_xy[0] = p.white_x;
				e.white_point_xy[1] = p.white_y;
				break;
			}
			}

			return e;
		}

		const char* describe(const JxlEncoderError error)
		{
			switch (error)
			{
			case JXL_ENC_ERR_OK: return "no error";
			case JXL_ENC_ERR_GENERIC: return "a generic error";
			case JXL_ENC_ERR_OOM: return "out of memory";
			case JXL_ENC_ERR_JBRD: return "JPEG reconstruction data";
			case JXL_ENC_ERR_BAD_INPUT: return "bad input";
			case JXL_ENC_ERR_NOT_SUPPORTED: return "an unsupported request";
			case JXL_ENC_ERR_API_USAGE: return "an API misuse";
			}

			return "an unknown error";
		}

		// What the encoder writes once its input is closed, into memory doubled as it fills.
		bool drain(JxlEncoder* const enc, std::vector<std::uint8_t>& out, const std::size_t first_size)
		{
			out.resize(first_size);
			auto* next = out.data();
			auto available = out.size();
			auto status = JXL_ENC_NEED_MORE_OUTPUT;

			while ((status = JxlEncoderProcessOutput(enc, &next, &available)) == JXL_ENC_NEED_MORE_OUTPUT)
			{
				const auto written = static_cast<std::size_t>(next - out.data());
				out.resize(out.size() * 2);
				next = out.data() + written;
				available = out.size() - written;
			}

			out.resize(status == JXL_ENC_SUCCESS ? static_cast<std::size_t>(next - out.data()) : 0);
			return status == JXL_ENC_SUCCESS;
		}

		struct attempt_result
		{
			std::vector<std::uint8_t> data;
			// The ICC profile, not the image, was what libjxl refused.
			bool icc_refused = false;
		};

		// One encode from a fresh encoder: a refused profile leaves it in a state nothing promises to
		// recover from, so a retry without one starts again.
		attempt_result attempt(const image_view& image, const std::vector<std::uint8_t>& rgb,
		                       const std::size_t channels, const JxlOrientation orientation,
		                       const embedded_metadata& metadata, const std::vector<std::uint8_t>& exif,
		                       const jxl_options& options, const bool use_icc, void* const runner,
		                       std::vector<std::string>& notes)
		{
			attempt_result result;

			const std::unique_ptr<JxlEncoder, encoder_destroy> encoder(JxlEncoderCreate(nullptr));
			auto* const enc = encoder.get();

			if (!enc)
			{
				notes.emplace_back("could not create a libjxl encoder");
				return result;
			}

			const auto fail = [&notes, enc](const char* step)
			{
				notes.push_back(std::format("libjxl failed to {}: {}", step, describe(JxlEncoderGetError(enc))));
			};

			if (runner && JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner) != JXL_ENC_SUCCESS)
			{
				fail("take its thread runner");
				return result;
			}

			auto* const settings = JxlEncoderFrameSettingsCreate(enc, nullptr);

			if (!settings)
			{
				fail("create its frame settings");
				return result;
			}

			const auto effort = std::clamp(options.effort, 1, 9);
			const auto quality = static_cast<float>(std::clamp(options.quality, 1, 100));
			// Quality 100 maps to distance 0, which libjxl reads as lossless; lossy stops just short.
			const auto distance = std::max(JxlEncoderDistanceFromQuality(quality), 0.1f);
			const auto use_boxes = !exif.empty() || !metadata.xmp.empty();

			if (JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, effort) != JXL_ENC_SUCCESS ||
			    (!options.lossless && JxlEncoderSetFrameDistance(settings, distance) != JXL_ENC_SUCCESS))
			{
				fail("take its settings");
				return result;
			}

			if (JxlEncoderUseContainer(enc, use_boxes ? JXL_TRUE : JXL_FALSE) != JXL_ENC_SUCCESS)
			{
				fail("use a container");
				return result;
			}

			JxlBasicInfo info;
			JxlEncoderInitBasicInfo(&info);
			const auto bits = is_wide(image.format) ? 16u : 8u;
			info.xsize = image.width;
			info.ysize = image.height;
			info.bits_per_sample = bits;
			info.exponent_bits_per_sample = 0;
			info.num_color_channels = 3;
			info.num_extra_channels = channels == 4 ? 1 : 0;
			info.alpha_bits = channels == 4 ? bits : 0;
			// Lossless keeps the pixels as they are; lossy turns them into libjxl's own colour space first.
			info.uses_original_profile = options.lossless ? JXL_TRUE : JXL_FALSE;
			info.orientation = orientation;
			// The light the brightest signal stands for, where the picture says: libjxl's own default otherwise.
			if (metadata.hdr && metadata.hdr->peak_nits() > 0.0)
				info.intensity_target = static_cast<float>(metadata.hdr->peak_nits());

			if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS)
			{
				fail("take the image's size");
				return result;
			}

			const auto level = JxlEncoderGetRequiredCodestreamLevel(enc);

			if (level < 0 || (level == 10 && JxlEncoderSetCodestreamLevel(enc, 10) != JXL_ENC_SUCCESS))
			{
				fail("find a codestream level for the image's size");
				return result;
			}

			if (options.lossless)
			{
				if (JxlEncoderSetFrameLossless(settings, JXL_TRUE) != JXL_ENC_SUCCESS)
				{
					fail("encode losslessly");
					return result;
				}
			}
			else if (channels == 4 && JxlEncoderSetExtraChannelDistance(settings, 0, 0.0f) != JXL_ENC_SUCCESS)
			{
				fail("keep alpha lossless");
				return result;
			}

			if (metadata.hdr)
			{
				// The description says what the pixels are, in place of a profile.
				const auto encoding = jxl_encoding_of(*metadata.hdr);

				if (JxlEncoderSetColorEncoding(enc, &encoding) != JXL_ENC_SUCCESS)
				{
					fail("take the HDR colour encoding");
					return result;
				}
			}
			else if (use_icc)
			{
				if (JxlEncoderSetICCProfile(enc, metadata.icc.data(), metadata.icc.size()) != JXL_ENC_SUCCESS)
				{
					result.icc_refused = true;
					return result;
				}
			}
			else
			{
				JxlColorEncoding srgb;
				JxlColorEncodingSetToSRGB(&srgb, JXL_FALSE);

				if (JxlEncoderSetColorEncoding(enc, &srgb) != JXL_ENC_SUCCESS)
				{
					fail("take the sRGB colour encoding");
					return result;
				}
			}

			if (use_boxes)
			{
				if (JxlEncoderUseBoxes(enc) != JXL_ENC_SUCCESS)
				{
					fail("use metadata boxes");
					return result;
				}

				if (!exif.empty() &&
				    JxlEncoderAddBox(enc, "Exif", exif.data(), exif.size(), JXL_FALSE) != JXL_ENC_SUCCESS)
				{
					fail("add the EXIF box");
					return result;
				}

				if (!metadata.xmp.empty() && JxlEncoderAddBox(enc, "xml ", metadata.xmp.data(), metadata.xmp.size(),
				                                              JXL_FALSE) != JXL_ENC_SUCCESS)
				{
					fail("add the XMP box");
					return result;
				}

				JxlEncoderCloseBoxes(enc);
			}

			const JxlPixelFormat format{static_cast<std::uint32_t>(channels),
			                            is_wide(image.format) ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

			if (JxlEncoderAddImageFrame(settings, &format, rgb.data(), rgb.size()) != JXL_ENC_SUCCESS)
			{
				fail("take the image");
				return result;
			}

			JxlEncoderCloseInput(enc);

			if (!drain(enc, result.data, std::max<std::size_t>(rgb.size() / 8, 64 * 1024)))
			{
				fail("encode the image");
			}

			return result;
		}
	}

	encoded encode_jxl(const image_view& image, const std::uint16_t orientation, const embedded_metadata& metadata,
	                   const jxl_options& options)
	{
		encoded result;

		if (image.format != pixel_format::bgrx8 && image.format != pixel_format::bgra8 && !is_wide(image.format))
		{
			result.notes.emplace_back("libfiles encodes JPEG XL from bgrx8, bgra8, rgbx16 or rgba16 pixels only");
			return result;
		}

		const auto pixel_bytes = is_wide(image.format) ? std::size_t{8} : std::size_t{4};

		if (!image.pixels || image.width == 0 || image.height == 0 || image.stride < pixel_bytes * image.width)
		{
			result.notes.emplace_back("the image has no pixels to encode");
			return result;
		}

		const std::size_t channels = image.format == pixel_format::bgra8 || image.format == pixel_format::rgba16 ? 4
		                                                                                                         : 3;

		if (image.height >
		    std::numeric_limits<std::size_t>::max() / (std::size_t{image.width} * channels * pixel_bytes))
		{
			result.notes.emplace_back("the image is too large to encode as JPEG XL here");
			return result;
		}

		// JxlOrientation numbers its turns as EXIF does.
		const auto turn = orientation >= 1 && orientation <= 8 ? orientation : std::uint16_t{1};

		try
		{
			const auto rgb = to_rgb(image, channels);
			const auto exif = metadata.exif.empty() ? std::vector<std::uint8_t>{} : exif_box(metadata.exif, turn);

			const std::unique_ptr<void, runner_destroy> runner(
				JxlThreadParallelRunnerCreate(nullptr, std::min(JxlThreadParallelRunnerDefaultNumWorkerThreads(),
			                                                    max_worker_threads)));

			auto encoded = attempt(image, rgb, channels, static_cast<JxlOrientation>(turn), metadata, exif, options,
			                       !metadata.icc.empty(), runner.get(), result.notes);

			if (encoded.icc_refused)
			{
				// A profile libjxl cannot read, or one for other pixels - CMYK or grey - than the RGB here.
				result.notes.emplace_back("libjxl refused the ICC profile, so the image is written as sRGB");
				encoded = attempt(image, rgb, channels, static_cast<JxlOrientation>(turn), metadata, exif, options,
				                  false, runner.get(), result.notes);
			}

			result.data = std::move(encoded.data);
		}
		catch (const std::exception& e)
		{
			// Out of memory, most likely, for the RGB copy or the output.
			result.data.clear();
			result.notes.push_back(std::format("the JPEG XL could not be encoded: {}", e.what()));
		}

		return result;
	}

	encoded transcode_jpeg_to_jxl(const std::span<const std::uint8_t> jpeg, const int effort)
	{
		encoded result;

		try
		{
			const std::unique_ptr<JxlEncoder, encoder_destroy> encoder(JxlEncoderCreate(nullptr));
			auto* const enc = encoder.get();

			if (!enc)
			{
				result.notes.emplace_back("could not create a libjxl encoder");
				return result;
			}

			const auto fail = [&result, enc](const char* step)
			{
				result.notes.push_back(std::format("libjxl failed to {}: {}", step, describe(JxlEncoderGetError(enc))));
			};

			const std::unique_ptr<void, runner_destroy> runner(
				JxlThreadParallelRunnerCreate(nullptr, std::min(JxlThreadParallelRunnerDefaultNumWorkerThreads(),
			                                                    max_worker_threads)));
			auto* const settings = JxlEncoderFrameSettingsCreate(enc, nullptr);

			// The box that gives the JPEG back needs a container. The boxes it is given back from are written
			// plain, as encode_jxl writes its own, so a reader without Brotli finds them.
			if ((runner &&
			     JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner.get()) != JXL_ENC_SUCCESS) ||
			    !settings || JxlEncoderUseContainer(enc, JXL_TRUE) != JXL_ENC_SUCCESS ||
			    JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE) != JXL_ENC_SUCCESS ||
			    JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, std::clamp(effort, 1, 9)) !=
			        JXL_ENC_SUCCESS ||
			    JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_JPEG_COMPRESS_BOXES, 0) !=
			        JXL_ENC_SUCCESS)
			{
				fail("take its settings");
				return result;
			}

			if (JxlEncoderAddJPEGFrame(settings, jpeg.data(), jpeg.size()) != JXL_ENC_SUCCESS)
			{
				fail("read the JPEG");
				return result;
			}

			// Level 5 holds no more than 2^28 pixels; a larger JPEG needs level 10.
			const auto level = JxlEncoderGetRequiredCodestreamLevel(enc);

			if (level < 0 || (level == 10 && JxlEncoderSetCodestreamLevel(enc, 10) != JXL_ENC_SUCCESS))
			{
				fail("find a codestream level for the JPEG's size");
				return result;
			}

			JxlEncoderCloseInput(enc);

			if (!drain(enc, result.data, std::max<std::size_t>(jpeg.size(), 64 * 1024))) fail("repack the JPEG");
		}
		catch (const std::exception& e)
		{
			result.data.clear();
			result.notes.push_back(std::format("the JPEG could not be repacked: {}", e.what()));
		}

		return result;
	}

	std::vector<std::uint8_t> reconstruct_jpeg(const std::span<const std::uint8_t> jxl)
	{
		try
		{
			const std::unique_ptr<JxlDecoder, decoder_destroy> decoder(JxlDecoderCreate(nullptr));
			const std::unique_ptr<void, resizable_runner_destroy> runner(JxlResizableParallelRunnerCreate(nullptr));

			if (!decoder || !runner ||
			    JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_JPEG_RECONSTRUCTION |
			                                                 JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
			    JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) !=
			        JXL_DEC_SUCCESS ||
			    JxlDecoderSetInput(decoder.get(), jxl.data(), jxl.size()) != JXL_DEC_SUCCESS)
			{
				return {};
			}

			JxlDecoderCloseInput(decoder.get());

			// A JPEG is given back from little more than the file repacked from it; a crafted file could
			// otherwise ask for memory without end.
			const auto limit = static_cast<std::size_t>(
				std::min<std::uint64_t>(std::uint64_t{jxl.size()} * 8 + (std::uint64_t{16} << 20),
			                            std::numeric_limits<std::size_t>::max() / 2));
			std::vector<std::uint8_t> jpeg;
			auto reconstructing = false;

			for (;;)
			{
				switch (JxlDecoderProcessInput(decoder.get()))
				{
				case JXL_DEC_BASIC_INFO:
				{
					JxlBasicInfo info = {};
					if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) return {};
					const auto threads = JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize);
					JxlResizableParallelRunnerSetThreads(runner.get(),
					                                     std::min<std::size_t>(threads, max_worker_threads));
					break;
				}
				case JXL_DEC_JPEG_RECONSTRUCTION:
					reconstructing = true;
					jpeg.resize(std::max<std::size_t>(jxl.size() + jxl.size() / 2, 64 * 1024));
					if (JxlDecoderSetJPEGBuffer(decoder.get(), jpeg.data(), jpeg.size()) != JXL_DEC_SUCCESS) return {};
					break;
				case JXL_DEC_JPEG_NEED_MORE_OUTPUT:
				{
					const auto used = jpeg.size() - JxlDecoderReleaseJPEGBuffer(decoder.get());
					if (jpeg.size() > limit / 2) return {};
					jpeg.resize(jpeg.size() * 2);
					if (JxlDecoderSetJPEGBuffer(decoder.get(), jpeg.data() + used, jpeg.size() - used) !=
					    JXL_DEC_SUCCESS)
						return {};
					break;
				}
				case JXL_DEC_FULL_IMAGE:
					if (!reconstructing) return {};
					jpeg.resize(jpeg.size() - JxlDecoderReleaseJPEGBuffer(decoder.get()));
					return jpeg;
				default:
					// A file with nothing to give a JPEG back from asks for memory for its picture instead. Or the file
					// is damaged or cut short, or its boxes no longer fit the JPEG's segments.
					return {};
				}
			}
		}
		catch (const std::bad_alloc&)
		{
			return {};
		}
	}

	std::optional<jxl_compression> read_jxl_compression(const std::span<const std::uint8_t> jxl)
	{
		try
		{
			const std::unique_ptr<JxlDecoder, decoder_destroy> decoder(JxlDecoderCreate(nullptr));

			if (!decoder ||
			    JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_JPEG_RECONSTRUCTION |
			                                                 JXL_DEC_FRAME) != JXL_DEC_SUCCESS ||
			    JxlDecoderSetInput(decoder.get(), jxl.data(), jxl.size()) != JXL_DEC_SUCCESS)
			{
				return {};
			}

			JxlDecoderCloseInput(decoder.get());
			std::optional<jxl_compression> result;

			for (;;)
			{
				switch (JxlDecoderProcessInput(decoder.get()))
				{
				case JXL_DEC_BASIC_INFO:
				{
					JxlBasicInfo info = {};
					if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) return {};
					result = jxl_compression{.lossless = info.uses_original_profile == JXL_TRUE};
					break;
				}
				case JXL_DEC_JPEG_RECONSTRUCTION:
					if (result) result->lossless = false;
					return result;
				default:
					// The first frame's header, which comes after what a JPEG is given back from; or the end of what
					// can be read.
					return result;
				}
			}
		}
		catch (const std::bad_alloc&)
		{
			return {};
		}
	}
}
