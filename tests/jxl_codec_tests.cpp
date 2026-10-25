// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for the JPEG XL codecs libfiles builds for its consumers: Brotli compressing and
// decompressing, libjxl encoding and decoding through its thread runner - losslessly to the same bytes,
// and lossy through Highway's SIMD to nearly the same - its colour profile read back by skcms, and skcms
// converting sRGB to linear light.

#include "test.h"

#ifdef LIBFILES_JXL

#include "builders.h"

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <jxl/cms.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/resizable_parallel_runner.h>
#include <skcms.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace test_build;

namespace
{
	// A gradient under noise, with alpha that varies.
	bytes make_rgba(const std::uint32_t width, const std::uint32_t height)
	{
		bytes rgba(std::size_t{4} * width * height);
		std::uint32_t seed = 2026;

		for (std::uint32_t y = 0; y < height; ++y)
		{
			for (std::uint32_t x = 0; x < width; ++x)
			{
				seed = seed * 1664525u + 1013904223u;
				auto* const p = rgba.data() + (std::size_t{y} * width + x) * 4;
				p[0] = static_cast<std::uint8_t>(x * 255 / width);
				p[1] = static_cast<std::uint8_t>(y * 255 / height);
				p[2] = static_cast<std::uint8_t>(128 + (seed >> 28));
				p[3] = static_cast<std::uint8_t>(255 - (x + y) % 64);
			}
		}

		return rgba;
	}

	bytes encode_jxl(const bytes& rgba, const std::uint32_t width, const std::uint32_t height, const bool lossless)
	{
		auto* const encoder = JxlEncoderCreate(nullptr);
		JxlBasicInfo info;
		JxlEncoderInitBasicInfo(&info);
		info.xsize = width;
		info.ysize = height;
		info.bits_per_sample = 8;
		info.num_color_channels = 3;
		info.num_extra_channels = 1;
		info.alpha_bits = 8;
		info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;

		bytes file;
		JxlColorEncoding color;
		JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
		auto* const settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
		const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

		if (JxlEncoderSetBasicInfo(encoder, &info) == JXL_ENC_SUCCESS &&
			JxlEncoderSetColorEncoding(encoder, &color) == JXL_ENC_SUCCESS &&
			(lossless ? JxlEncoderSetFrameLossless(settings, JXL_TRUE) : JxlEncoderSetFrameDistance(settings, 1.0f)) ==
			JXL_ENC_SUCCESS &&
			JxlEncoderAddImageFrame(settings, &format, rgba.data(), rgba.size()) == JXL_ENC_SUCCESS)
		{
			JxlEncoderCloseInput(encoder);
			file.resize(256);
			auto* next = file.data();
			auto available = file.size();
			auto status = JXL_ENC_NEED_MORE_OUTPUT;

			while ((status = JxlEncoderProcessOutput(encoder, &next, &available)) == JXL_ENC_NEED_MORE_OUTPUT)
			{
				const auto written = static_cast<std::size_t>(next - file.data());
				file.resize(file.size() * 2);
				next = file.data() + written;
				available = file.size() - written;
			}

			file.resize(status == JXL_ENC_SUCCESS ? static_cast<std::size_t>(next - file.data()) : 0);
		}

		JxlEncoderDestroy(encoder);
		return file;
	}

	struct decoded_jxl
	{
		bool success = false;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		bytes rgba;
		bytes icc;
	};

	decoded_jxl decode_jxl(const bytes& file)
	{
		decoded_jxl result;
		auto* const decoder = JxlDecoderCreate(nullptr);
		auto* const runner = JxlResizableParallelRunnerCreate(nullptr);
		JxlDecoderSetParallelRunner(decoder, JxlResizableParallelRunner, runner);
		JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE);
		JxlDecoderSetInput(decoder, file.data(), file.size());
		JxlDecoderCloseInput(decoder);
		const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
		auto finished = false;

		while (!finished)
		{
			switch (JxlDecoderProcessInput(decoder))
			{
			case JXL_DEC_BASIC_INFO:
				{
					JxlBasicInfo info;
					JxlDecoderGetBasicInfo(decoder, &info);
					result.width = info.xsize;
					result.height = info.ysize;
					JxlResizableParallelRunnerSetThreads(runner, JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
					break;
				}
			case JXL_DEC_COLOR_ENCODING:
				{
					std::size_t size = 0;

					if (JxlDecoderGetICCProfileSize(decoder, JXL_COLOR_PROFILE_TARGET_DATA, &size) == JXL_DEC_SUCCESS)
					{
						result.icc.resize(size);
						JxlDecoderGetColorAsICCProfile(decoder, JXL_COLOR_PROFILE_TARGET_DATA, result.icc.data(), size);
					}
					break;
				}
			case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
				{
					std::size_t size = 0;
					JxlDecoderImageOutBufferSize(decoder, &format, &size);
					result.rgba.resize(size);
					JxlDecoderSetImageOutBuffer(decoder, &format, result.rgba.data(), size);
					break;
				}
			case JXL_DEC_FULL_IMAGE:
				result.success = true;
				break;
			case JXL_DEC_SUCCESS:
				finished = true;
				break;
			default:
				result.success = false;
				finished = true;
				break;
			}
		}

		JxlResizableParallelRunnerDestroy(runner);
		JxlDecoderDestroy(decoder);
		return result;
	}
}

TEST("Should compress and decompress with Brotli")
{
	std::string text;
	for (auto i = 0; i < 400; ++i) text += "libfiles builds Brotli for libjxl and its consumers, line " + std::to_string(i % 7) + "\n";

	auto encoded_size = BrotliEncoderMaxCompressedSize(text.size());
	bytes encoded(encoded_size);
	CHECK(BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_TEXT, text.size(),
	                            reinterpret_cast<const std::uint8_t*>(text.data()), &encoded_size, encoded.data()));
	encoded.resize(encoded_size);
	CHECK(encoded.size() < text.size() / 20);

	std::string decoded(text.size(), '\0');
	auto decoded_size = decoded.size();
	CHECK_EQ(BrotliDecoderDecompress(encoded.size(), encoded.data(), &decoded_size,
	                                 reinterpret_cast<std::uint8_t*>(decoded.data())), BROTLI_DECODER_RESULT_SUCCESS);
	CHECK_EQ(decoded_size, text.size());
	CHECK(decoded == text);

	// Cut short, it says so rather than inventing the rest.
	std::string cut(text.size(), '\0');
	auto cut_size = cut.size();
	CHECK(BrotliDecoderDecompress(encoded.size() / 2, encoded.data(), &cut_size, reinterpret_cast<std::uint8_t*>(cut.data())) !=
		BROTLI_DECODER_RESULT_SUCCESS);
}

TEST("Should encode and decode a lossless JPEG XL with libjxl to the same pixels")
{
	const auto rgba = make_rgba(67, 41);
	const auto file = encode_jxl(rgba, 67, 41, true);
	CHECK(file.size() > 12);
	CHECK(file[0] == 0xFF && file[1] == 0x0A);

	const auto decoded = decode_jxl(file);
	CHECK(decoded.success);
	CHECK_EQ(decoded.width, 67u);
	CHECK_EQ(decoded.height, 41u);
	CHECK(decoded.rgba == rgba);

	// Cut short, it does not decode.
	const bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() / 2));
	CHECK(!decode_jxl(cut).success);
}

TEST("Should encode and decode a lossy JPEG XL with libjxl to nearly the same pixels")
{
	const auto rgba = make_rgba(256, 192);
	const auto file = encode_jxl(rgba, 256, 192, false);
	CHECK(file.size() > 12 && file.size() < rgba.size() / 4);

	const auto decoded = decode_jxl(file);
	CHECK(decoded.success);
	CHECK_EQ(decoded.rgba.size(), rgba.size());

	auto total = 0.0;
	auto worst = 0;

	for (std::size_t i = 0; i < rgba.size() && i < decoded.rgba.size(); ++i)
	{
		const auto difference = std::abs(static_cast<int>(rgba[i]) - static_cast<int>(decoded.rgba[i]));
		total += difference;
		worst = std::max(worst, difference);
	}

	CHECK(total / static_cast<double>(rgba.size()) < 3.0);
	CHECK(worst < 64);
}

TEST("Should read libjxl's colour profile with skcms, and convert sRGB to linear light with it")
{
	CHECK(JxlGetDefaultCms() != nullptr);

	const auto decoded = decode_jxl(encode_jxl(make_rgba(8, 8), 8, 8, true));
	CHECK(decoded.success);
	CHECK(!decoded.icc.empty());

	skcms_ICCProfile profile;
	CHECK(skcms_Parse(decoded.icc.data(), decoded.icc.size(), &profile));
	CHECK(skcms_ApproximatelyEqualProfiles(&profile, skcms_sRGB_profile()));

	auto linear = *skcms_sRGB_profile();
	const skcms_TransferFunction identity{1, 1, 0, 0, 0, 0, 0};
	skcms_SetTransferFunction(&linear, &identity);

	const std::uint8_t source[] = {128, 64, 255, 255, 0, 0, 0, 255};
	float converted[8] = {};
	CHECK(skcms_Transform(source, skcms_PixelFormat_RGBA_8888, skcms_AlphaFormat_Unpremul, skcms_sRGB_profile(), converted,
	                      skcms_PixelFormat_RGBA_ffff, skcms_AlphaFormat_Unpremul, &linear, 2));

	const auto expected = [](const double encoded) { return std::pow((encoded / 255.0 + 0.055) / 1.055, 2.4); };
	CHECK(std::abs(converted[0] - expected(128)) < 0.002);
	CHECK(std::abs(converted[1] - expected(64)) < 0.002);
	CHECK(std::abs(converted[2] - 1.0) < 0.002);
	CHECK(std::abs(converted[3] - 1.0) < 0.002);
	CHECK(std::abs(converted[4]) < 0.002);
}

#endif
