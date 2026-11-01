// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Encoding a JPEG XL with libjxl from packed pixels, as libjxl's own cjxl drives it: the pixels
// swapped into the RGB order libjxl reads, the ICC profile or sRGB, the orientation in the codestream,
// and the EXIF and XMP in boxes of their own. Encoded on libjxl's thread runner.

#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "encode_internal.h"

#include <jxl/encode.h>
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

		// One image is encoded at a time and libjxl spreads it over groups of 256 pixels; more workers
		// than this mostly add each one's scratch memory, which a 32-bit process cannot spare.
		constexpr std::size_t max_worker_threads = 8;

		constexpr std::uint16_t tag_orientation = 0x0112;

		// libjxl reads red, green and blue in that order; the image holds them blue first.
		std::vector<std::uint8_t> to_rgb(const image_view& image, const std::size_t channels)
		{
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
			info.xsize = image.width;
			info.ysize = image.height;
			info.bits_per_sample = 8;
			info.exponent_bits_per_sample = 0;
			info.num_color_channels = 3;
			info.num_extra_channels = channels == 4 ? 1 : 0;
			info.alpha_bits = channels == 4 ? 8 : 0;
			// Lossless keeps the pixels as they are; lossy turns them into libjxl's own colour space first.
			info.uses_original_profile = options.lossless ? JXL_TRUE : JXL_FALSE;
			info.orientation = orientation;

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

			if (use_icc)
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

				if (!exif.empty() && JxlEncoderAddBox(enc, "Exif", exif.data(), exif.size(), JXL_FALSE) != JXL_ENC_SUCCESS)
				{
					fail("add the EXIF box");
					return result;
				}

				if (!metadata.xmp.empty() &&
					JxlEncoderAddBox(enc, "xml ", metadata.xmp.data(), metadata.xmp.size(), JXL_FALSE) != JXL_ENC_SUCCESS)
				{
					fail("add the XMP box");
					return result;
				}

				JxlEncoderCloseBoxes(enc);
			}

			const JxlPixelFormat format{static_cast<std::uint32_t>(channels), JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

			if (JxlEncoderAddImageFrame(settings, &format, rgb.data(), rgb.size()) != JXL_ENC_SUCCESS)
			{
				fail("take the image");
				return result;
			}

			JxlEncoderCloseInput(enc);

			std::vector<std::uint8_t> out(std::max<std::size_t>(rgb.size() / 8, 64 * 1024));
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

			if (status != JXL_ENC_SUCCESS)
			{
				fail("encode the image");
				return result;
			}

			out.resize(static_cast<std::size_t>(next - out.data()));
			result.data = std::move(out);
			return result;
		}
	}

	encoded encode_jxl(const image_view& image, const std::uint16_t orientation, const embedded_metadata& metadata,
	                   const jxl_options& options)
	{
		encoded result;

		if (image.format != pixel_format::bgrx8 && image.format != pixel_format::bgra8)
		{
			result.notes.emplace_back("libfiles encodes JPEG XL from bgrx8 or bgra8 pixels only");
			return result;
		}

		if (!image.pixels || image.width == 0 || image.height == 0 || image.stride < std::size_t{4} * image.width)
		{
			result.notes.emplace_back("the image has no pixels to encode");
			return result;
		}

		const std::size_t channels = image.format == pixel_format::bgra8 ? 4 : 3;

		if (image.height > std::numeric_limits<std::size_t>::max() / (std::size_t{image.width} * channels))
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
}
