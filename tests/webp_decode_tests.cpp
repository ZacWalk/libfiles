// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for decoding WebPs: lossy and lossless, with and without alpha, to NV12 with an odd edge
// cropped, reduced while decoding to scaled_size_for's size, the orientation from an EXIF chunk, and what
// decode() reports for a file cut short, an animation or a refusal. The files are libwebp's own, written
// by its encoder and mux, and the pixels expected are what libwebp's simple decoder makes of them.

#include "test.h"

#ifdef LIBFILES_WEBP

#include "builders.h"
#include "libfiles/decode.h"

#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	// width x height of a gradient with noise, and alpha that varies when asked - never 0, under which
	// libwebp's lossless encoder may rewrite the colour, as it is not asked to keep it exact.
	bytes make_bgra(const int width, const int height, const bool alpha)
	{
		bytes bgra(static_cast<std::size_t>(width) * height * 4);
		std::uint32_t seed = 99;

		for (std::size_t i = 0; i < bgra.size(); ++i)
		{
			seed = seed * 1664525u + 1013904223u;
			const auto pixel = static_cast<int>(i / 4);
			const auto x = pixel % width;
			const auto y = pixel / width;
			const auto noise = static_cast<int>(seed >> 28);

			switch (i % 4)
			{
			case 0: bgra[i] = static_cast<std::uint8_t>(x * 255 / std::max(1, width - 1));
				break;
			case 1: bgra[i] = static_cast<std::uint8_t>(y * 255 / std::max(1, height - 1));
				break;
			case 2: bgra[i] = static_cast<std::uint8_t>(96 + noise);
				break;
			default: bgra[i] = alpha ? static_cast<std::uint8_t>(1 + (x * 37 + y * 11) % 255) : 0xFF;
				break;
			}
		}

		return bgra;
	}

	bytes encode(const int width, const int height, const bool alpha, const bool lossless)
	{
		const auto bgra = make_bgra(width, height, alpha);
		std::uint8_t* out = nullptr;
		const auto size = lossless
			                  ? WebPEncodeLosslessBGRA(bgra.data(), width, height, width * 4, &out)
			                  : WebPEncodeBGRA(bgra.data(), width, height, width * 4, 85.f, &out);
		bytes file(out, out + size);
		WebPFree(out);
		return file;
	}

	// The file with an EXIF chunk added by libwebp's mux.
	bytes with_exif(const bytes& file, const bytes& exif)
	{
		const WebPData image{file.data(), file.size()};
		auto* const mux = WebPMuxCreate(&image, 1);
		const WebPData chunk{exif.data(), exif.size()};
		WebPMuxSetChunk(mux, "EXIF", &chunk, 1);
		WebPData assembled{};
		WebPMuxAssemble(mux, &assembled);
		bytes result(assembled.bytes, assembled.bytes + assembled.size);
		WebPDataClear(&assembled);
		WebPMuxDelete(mux);
		return result;
	}

	bytes orientation_exif(const std::uint16_t value)
	{
		return builder().raw("II").le16(42).le32(8).le16(1).le16(0x0112).le16(3).le32(1).le16(value).le16(0)
		                .le32(0).b;
	}

	// Memory filled with a byte the decoder never writes, with padding after each row.
	struct webp_sink final : libfiles::image_sink
	{
		bytes memory;
		std::size_t stride = 0;
		bool refuse = false;
		int calls = 0;
		libfiles::frame_info told;
		bytes exif_told;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			++calls;
			told = frame;
			exif_told.assign(frame.exif.begin(), frame.exif.end());
			if (refuse) return {};

			const auto nv12 = frame.format == libfiles::pixel_format::nv12;
			stride = (nv12 ? std::size_t{1} : std::size_t{4}) * frame.width + 12;
			const auto rows = nv12 ? frame.height + frame.height / 2 : frame.height;
			memory.assign(stride * rows, 0xAB);
			return {memory.data(), stride};
		}

		// Every row's pixels, without the padding.
		bytes rows(const std::size_t row_bytes, const std::size_t count) const
		{
			bytes result;
			for (std::size_t y = 0; y < count; ++y)
				result.insert(result.end(), memory.begin() + static_cast<std::ptrdiff_t>(y * stride),
				              memory.begin() + static_cast<std::ptrdiff_t>(y * stride + row_bytes));
			return result;
		}

		bool padding_kept(const std::size_t row_bytes, const std::size_t count) const
		{
			for (std::size_t y = 0; y < count; ++y)
				for (auto x = row_bytes; x < stride; ++x)
					if (memory[y * stride + x] != 0xAB) return false;
			return true;
		}
	};

	libfiles::decode_result decode_file(const bytes& file, webp_sink& sink, const libfiles::decode_options& options = {})
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::decode(r, sink, options);
	}

	// What libwebp's simple decoder makes of the file, as tightly packed BGRA.
	bytes simple_bgra(const bytes& file)
	{
		int w = 0;
		int h = 0;
		auto* const p = WebPDecodeBGRA(file.data(), file.size(), &w, &h);
		bytes result(p, p + static_cast<std::size_t>(w) * h * 4);
		WebPFree(p);
		return result;
	}

	libfiles::decode_options targeted(const std::uint32_t width, const std::uint32_t height, const bool nv12 = false)
	{
		libfiles::decode_options options;
		options.target_width = width;
		options.target_height = height;
		options.allow_nv12 = nv12;
		return options;
	}
}

TEST("WebP header probes match the codec without reading image data")
{
	for (const bool lossless : {false, true})
		for (const bool alpha : {false, true})
		{
			const auto file = encode(9, 8, alpha, lossless);
			for (std::size_t size = 0; size <= std::min<std::size_t>(64, file.size()); ++size)
			{
				WebPBitstreamFeatures expected{};
				const auto valid = WebPGetFeatures(file.data(), size, &expected) == VP8_STATUS_OK;
				libfiles::reader input(std::span<const std::uint8_t>(file.data(), size));
				const auto header = libfiles::probe_webp_header(input);
				CHECK(header.has_value() == valid);
				if (header && valid)
				{
					CHECK(header->width == expected.width && header->height == expected.height);
					CHECK(header->has_alpha == (expected.has_alpha != 0));
					CHECK(header->has_animation == (expected.has_animation != 0));
				}
			}
		}
}

TEST("WebP native thumbnails round down and bare bitstreams remain decodable")
{
	const auto file = encode(9, 8, false, true);
	libfiles::decode_options options;
	options.target_width = options.target_height = 4;
	options.round_webp_size_down = true;
	libfiles::reader input(file);
	webp_sink sink;
	const auto result = libfiles::decode(input, sink, options);
	CHECK(result.success);
	CHECK(result.frame.width == 4 && result.frame.height == 3);
	CHECK(file.size() > 20);
	if (file.size() <= 20) return;
	libfiles::reader bare(std::span<const std::uint8_t>(file).subspan(20));
	options.source_format = libfiles::image_format::webp;
	webp_sink raw_sink;
	const auto raw = libfiles::decode(bare, raw_sink, options);
	CHECK(raw.success);
	CHECK(raw_sink.memory == sink.memory);
}

TEST("scaled_size_for fits a source within a target as Diffractor's ui::scale_dimensions does")
{
	using size = libfiles::image_size;
	CHECK(libfiles::scaled_size_for(37, 23, 10, 10) == (size{10, 6}));
	CHECK(libfiles::scaled_size_for(4000, 3000, 1920, 1080) == (size{1440, 1080}));
	CHECK(libfiles::scaled_size_for(100, 50, 99, 1000) == (size{99, 50}));
	CHECK(libfiles::scaled_size_for(3, 1, 1, 1) == (size{1, 1}));
	CHECK(libfiles::scaled_size_for(16383, 1, 100, 100) == (size{100, 1}));

	// Not smaller in either dimension, or no target: the source.
	CHECK(libfiles::scaled_size_for(100, 50, 200, 200) == (size{100, 50}));
	CHECK(libfiles::scaled_size_for(100, 50, 100, 50) == (size{100, 50}));
	CHECK(libfiles::scaled_size_for(1, 1, 1, 1) == (size{1, 1}));
	CHECK(libfiles::scaled_size_for(100, 50, 0, 10) == (size{100, 50}));
	CHECK(libfiles::scaled_size_for(100, 50, 10, 0) == (size{100, 50}));
}

TEST("Should decode a WebP to BGRX, or BGRA with straight alpha, as libwebp decodes it")
{
	CHECK(libfiles::can_decode(libfiles::image_format::webp));

	for (const auto lossless : {false, true})
	{
		for (const auto alpha : {false, true})
		{
			const auto file = encode(37, 23, alpha, lossless);
			webp_sink sink;
			const auto result = decode_file(file, sink);

			CHECK(result.success);
			CHECK(!result.refused && !result.truncated && result.error.empty());
			CHECK_EQ(sink.calls, 1);
			CHECK(result.frame.format == (alpha ? libfiles::pixel_format::bgra8 : libfiles::pixel_format::bgrx8));
			CHECK(!result.frame.limited_range);
			CHECK_EQ(result.frame.width, 37u);
			CHECK_EQ(result.frame.height, 23u);
			CHECK_EQ(result.frame.source_width, 37u);
			CHECK_EQ(result.frame.orientation, 1u);
			CHECK(sink.rows(37 * 4, 23) == simple_bgra(file));
			CHECK(sink.padding_kept(37 * 4, 23));
		}
	}

	// Lossless keeps every byte it was given, alpha included.
	const auto lossless = encode(37, 23, true, true);
	webp_sink sink;
	CHECK(decode_file(lossless, sink).success);
	CHECK(sink.rows(37 * 4, 23) == make_bgra(37, 23, true));
}

TEST("Should decode an opaque lossy WebP to limited-range NV12 when asked, an odd edge cropped")
{
	const auto file = encode(37, 23, false, false);
	webp_sink sink;
	const auto result = decode_file(file, sink, targeted(0, 0, true));

	CHECK(result.success);
	CHECK(result.frame.format == libfiles::pixel_format::nv12);
	CHECK(result.frame.limited_range);
	CHECK_EQ(result.frame.width, 36u);
	CHECK_EQ(result.frame.height, 22u);
	CHECK_EQ(result.frame.source_width, 37u);
	CHECK_EQ(result.frame.source_height, 23u);

	// libwebp's own planes, cropped to the even size, and the chroma interleaved.
	int w = 0;
	int h = 0;
	int y_stride = 0;
	int uv_stride = 0;
	std::uint8_t* u = nullptr;
	std::uint8_t* v = nullptr;
	auto* const y = WebPDecodeYUV(file.data(), file.size(), &w, &h, &u, &v, &y_stride, &uv_stride);
	CHECK(y != nullptr && w == 37 && h == 23);
	if (!y) return;

	bytes expected;
	for (auto row = 0; row < 22; ++row) expected.insert(expected.end(), y + row * y_stride, y + row * y_stride + 36);

	for (auto row = 0; row < 11; ++row)
	{
		for (auto x = 0; x < 18; ++x)
		{
			expected.push_back(u[row * uv_stride + x]);
			expected.push_back(v[row * uv_stride + x]);
		}
	}

	WebPFree(y);
	CHECK(sink.rows(36, 33) == expected);
	CHECK(sink.padding_kept(36, 33));
}

TEST("Should not decode to NV12 with alpha, lossless, reduced, too small or unasked")
{
	const auto format_for = [](const bytes& file, const libfiles::decode_options& options)
	{
		webp_sink sink;
		const auto result = decode_file(file, sink, options);
		return result.success ? result.frame.format : libfiles::pixel_format::nv12;
	};

	CHECK(format_for(encode(37, 23, true, false), targeted(0, 0, true)) == libfiles::pixel_format::bgra8);
	CHECK(format_for(encode(37, 23, false, true), targeted(0, 0, true)) == libfiles::pixel_format::bgrx8);
	CHECK(format_for(encode(37, 23, false, false), targeted(10, 10, true)) == libfiles::pixel_format::bgrx8);
	CHECK(format_for(encode(1, 23, false, false), targeted(0, 0, true)) == libfiles::pixel_format::bgrx8);
	CHECK(format_for(encode(37, 23, false, false), targeted(0, 0, false)) == libfiles::pixel_format::bgrx8);

	// A target the image already fits does not reduce it, so NV12 stays.
	CHECK(format_for(encode(37, 23, false, false), targeted(500, 500, true)) == libfiles::pixel_format::nv12);
}

TEST("Should reduce a WebP while it decodes, to scaled_size_for's size")
{
	for (const auto alpha : {false, true})
	{
		const auto file = encode(37, 23, alpha, false);
		webp_sink sink;
		const auto result = decode_file(file, sink, targeted(10, 10));

		CHECK(result.success);
		CHECK_EQ(result.frame.width, 10u);
		CHECK_EQ(result.frame.height, 6u);
		CHECK_EQ(result.frame.source_width, 37u);
		CHECK_EQ(result.frame.scale_denominator, 1u);

		// libwebp's scaler, asked directly for the same size.
		WebPDecoderConfig config;
		WebPInitDecoderConfig(&config);
		config.options.use_scaling = 1;
		config.options.scaled_width = 10;
		config.options.scaled_height = 6;
		config.output.colorspace = MODE_BGRA;
		CHECK_EQ(WebPDecode(file.data(), file.size(), &config), VP8_STATUS_OK);
		const bytes expected(config.output.u.RGBA.rgba, config.output.u.RGBA.rgba + 10 * 6 * 4);
		WebPFreeDecBuffer(&config.output);

		CHECK(sink.rows(10 * 4, 6) == expected);
		CHECK(sink.padding_kept(10 * 4, 6));
	}

	// A target with a zero edge, or one the image already fits, decodes it whole.
	for (const auto& options : {targeted(0, 10), targeted(10, 0), targeted(37, 23), targeted(100, 100)})
	{
		webp_sink sink;
		const auto result = decode_file(encode(37, 23, false, false), sink, options);
		CHECK(result.success && result.frame.width == 37 && result.frame.height == 23);
	}
}

TEST("Should take a WebP's orientation from its first EXIF chunk, with or without a JPEG signature")
{
	const auto plain = with_exif(encode(37, 23, false, false), orientation_exif(6));
	webp_sink sink;
	const auto result = decode_file(plain, sink);
	CHECK(result.success);
	CHECK_EQ(result.frame.orientation, 6u);
	CHECK_EQ(sink.told.orientation, 6u);
	CHECK(sink.exif_told == orientation_exif(6));
	CHECK(result.frame.exif.empty());

	auto signed_exif = lit("Exif\0\0");
	const auto tiff = orientation_exif(3);
	signed_exif.insert(signed_exif.end(), tiff.begin(), tiff.end());
	webp_sink signed_sink;
	CHECK_EQ(decode_file(with_exif(encode(37, 23, true, true), signed_exif), signed_sink).frame.orientation, 3u);
	CHECK(signed_sink.exif_told == tiff);

	// A chunk with no orientation, and none at all, leave the image as stored.
	webp_sink none_sink;
	CHECK_EQ(decode_file(with_exif(encode(37, 23, false, false), lit("II*\0\x08\0\0\0\0\0")), none_sink).frame.orientation, 1u);
}

TEST("Should report a WebP cut short or damaged, and refuse what the sink refuses")
{
	const auto file = encode(64, 48, false, false);

	auto cut = file;
	cut.resize(cut.size() / 2);
	webp_sink cut_sink;
	const auto cut_result = decode_file(cut, cut_sink);
	CHECK(!cut_result.success);
	CHECK(cut_result.truncated);
	CHECK(!cut_result.error.empty());
	CHECK_EQ(cut_sink.calls, 1);

	// Too short for its header: not asked for memory at all.
	bytes header(file.begin(), file.begin() + 20);
	webp_sink header_sink;
	const auto header_result = decode_file(header, header_sink);
	CHECK(!header_result.success && !header_result.error.empty());
	CHECK_EQ(header_sink.calls, 0);

	webp_sink refusing;
	refusing.refuse = true;
	const auto refused = decode_file(file, refusing, targeted(0, 0, true));
	CHECK(!refused.success && refused.refused && refused.error.empty());
	CHECK_EQ(refusing.calls, 1);
	CHECK(refusing.told.format == libfiles::pixel_format::nv12);
}

TEST("Should not decode an animated WebP")
{
	WebPAnimEncoderOptions options;
	WebPAnimEncoderOptionsInit(&options);
	auto* const encoder = WebPAnimEncoderNew(16, 16, &options);

	for (auto frame = 0; frame < 3; ++frame)
	{
		WebPPicture picture;
		WebPPictureInit(&picture);
		picture.width = 16;
		picture.height = 16;
		picture.use_argb = 1;
		WebPPictureAlloc(&picture);
		for (auto i = 0; i < 16 * 16; ++i) picture.argb[i] = 0xFF000000u | static_cast<std::uint32_t>(frame * 0x404040);
		WebPConfig config;
		WebPConfigInit(&config);
		WebPAnimEncoderAdd(encoder, &picture, frame * 100, &config);
		WebPPictureFree(&picture);
	}

	WebPAnimEncoderAdd(encoder, nullptr, 300, nullptr);
	WebPData assembled{};
	CHECK(WebPAnimEncoderAssemble(encoder, &assembled) != 0);
	const bytes file(assembled.bytes, assembled.bytes + assembled.size);
	WebPDataClear(&assembled);
	WebPAnimEncoderDelete(encoder);

	webp_sink sink;
	const auto result = decode_file(file, sink);
	CHECK(!result.success);
	CHECK(!result.truncated);
	CHECK(!result.error.empty());
	CHECK_EQ(sink.calls, 1);
}

TEST("Should decode a WebP from a stream as from memory")
{
	const auto file = with_exif(encode(37, 23, true, false), orientation_exif(8));

	webp_sink from_memory;
	const auto memory_result = decode_file(file, from_memory);

	libfiles::memory_stream source(file);
	libfiles::reader r(source, 1024);
	CHECK(r.resident().empty());
	webp_sink from_stream;
	const auto stream_result = libfiles::decode(r, from_stream);

	CHECK(memory_result.success && stream_result.success);
	CHECK_EQ(stream_result.frame.orientation, 8u);
	CHECK(from_stream.memory == from_memory.memory);
}

#endif
