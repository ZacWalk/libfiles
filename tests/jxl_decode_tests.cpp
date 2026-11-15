// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Tests for decoding JPEG XL: images libjxl writes losslessly, whose every pixel is known -
// opaque, straight and premultiplied alpha, grey, turned by their codestream, animated, and in a
// container with an Exif box - and what decode() reports for a file cut short, refused, or read from a
// stream.

#include "test.h"

#ifdef LIBFILES_JXL

#include "builders.h"
#include "libfiles/decode.h"
#include "libfiles/scan.h"

#include <jxl/encode.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace test_build;

namespace
{
	struct jxl_spec
	{
		std::uint32_t width = 7;
		std::uint32_t height = 5;
		bool alpha = false;
		bool premultiplied = false;
		bool grey = false;
		std::uint32_t orientation = 1;
		int frames = 1;
		bytes exif;
	};

	// The straight colour and alpha of a written pixel, as RGBA.
	std::array<std::uint8_t, 4> source_pixel(const std::uint32_t x, const std::uint32_t y, const int frame,
	                                         const bool alpha)
	{
		return {static_cast<std::uint8_t>(x * 30 + frame * 7), static_cast<std::uint8_t>(y * 40),
		        static_cast<std::uint8_t>(200 - x * y),
		        alpha ? static_cast<std::uint8_t>((x + y) % 3 == 0 ? 0 : 60 + x * 20) : std::uint8_t{255}};
	}

	// An image libjxl writes losslessly, the colour premultiplied where asked.
	bytes write_jxl(const jxl_spec& s)
	{
		auto* const encoder = JxlEncoderCreate(nullptr);
		JxlBasicInfo info;
		JxlEncoderInitBasicInfo(&info);
		info.xsize = s.width;
		info.ysize = s.height;
		info.bits_per_sample = 8;
		info.num_color_channels = s.grey ? 1 : 3;
		info.num_extra_channels = s.alpha ? 1 : 0;
		info.alpha_bits = s.alpha ? 8 : 0;
		info.alpha_premultiplied = s.premultiplied ? JXL_TRUE : JXL_FALSE;
		info.uses_original_profile = JXL_TRUE;
		info.orientation = static_cast<JxlOrientation>(s.orientation);

		if (s.frames > 1)
		{
			info.have_animation = JXL_TRUE;
			info.animation.tps_numerator = 10;
			info.animation.tps_denominator = 1;
		}

		JxlColorEncoding color;
		JxlColorEncodingSetToSRGB(&color, s.grey ? JXL_TRUE : JXL_FALSE);
		bytes file;

		// Boxes make the file a container; without them it is a bare codestream.
		const auto boxes = !s.exif.empty();

		if ((!boxes || JxlEncoderUseBoxes(encoder) == JXL_ENC_SUCCESS) &&
		    JxlEncoderSetBasicInfo(encoder, &info) == JXL_ENC_SUCCESS &&
		    JxlEncoderSetColorEncoding(encoder, &color) == JXL_ENC_SUCCESS)
		{
			if (boxes)
			{
				JxlEncoderAddBox(encoder, "Exif", s.exif.data(), s.exif.size(), JXL_FALSE);
				JxlEncoderCloseBoxes(encoder);
			}

			auto* const settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
			JxlEncoderSetFrameLossless(settings, JXL_TRUE);
			const auto channels = (s.grey ? 1u : 3u) + (s.alpha ? 1u : 0u);
			const JxlPixelFormat format{channels, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

			for (auto f = 0; f < s.frames; ++f)
			{
				if (s.frames > 1)
				{
					JxlFrameHeader header;
					JxlEncoderInitFrameHeader(&header);
					header.duration = 1;
					JxlEncoderSetFrameHeader(settings, &header);
				}

				bytes pixels;

				for (std::uint32_t y = 0; y < s.height; ++y)
				{
					for (std::uint32_t x = 0; x < s.width; ++x)
					{
						auto p = source_pixel(x, y, f, s.alpha);
						if (s.premultiplied)
							for (auto c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>((p[c] * p[3] + 127) / 255);
						if (s.grey) pixels.push_back(p[1]);
						else pixels.insert(pixels.end(), p.begin(), p.begin() + 3);
						if (s.alpha) pixels.push_back(p[3]);
					}
				}

				JxlEncoderAddImageFrame(settings, &format, pixels.data(), pixels.size());
			}

			JxlEncoderCloseInput(encoder);
			file.resize(1024);
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

	// An Exif box's payload: the offset to its TIFF header, then a little-endian TIFF stream whose one
	// entry is the orientation.
	bytes exif_box(const std::uint16_t orientation)
	{
		return builder()
		    .be32(0)
		    .raw("II")
		    .le16(42)
		    .le32(8)
		    .le16(1)
		    .le16(0x0112)
		    .le16(3)
		    .le32(1)
		    .le16(orientation)
		    .le16(0)
		    .le32(0)
		    .b;
	}

	// Memory filled with a byte the decoder never writes, with padding after each row.
	struct jxl_sink final : libfiles::image_sink
	{
		bytes memory;
		std::size_t stride = 0;
		bool refuse = false;
		int calls = 0;
		libfiles::frame_info told;

		std::size_t pixel_bytes() const
		{
			return told.format == libfiles::pixel_format::rgba16 || told.format == libfiles::pixel_format::rgbx16 ? 8
			                                                                                                      : 4;
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			++calls;
			told = frame;
			if (refuse) return {};
			stride = pixel_bytes() * frame.width + 12;
			memory.assign(stride * frame.height, 0xAB);
			return {memory.data(), stride};
		}

		const std::uint8_t* pixel(const std::uint32_t x, const std::uint32_t y) const
		{
			return memory.data() + y * stride + x * 4;
		}

		std::array<std::uint16_t, 4> wide_pixel(const std::uint32_t x, const std::uint32_t y) const
		{
			std::array<std::uint16_t, 4> channels{};
			std::memcpy(channels.data(), memory.data() + y * stride + x * 8, 8);
			return channels;
		}

		bool padding_kept() const
		{
			for (std::size_t y = 0; y < told.height; ++y)
				for (auto x = pixel_bytes() * told.width; x < stride; ++x)
					if (memory[y * stride + x] != 0xAB) return false;
			return true;
		}
	};

	libfiles::decode_result decode_file(const bytes& file, jxl_sink& sink, const libfiles::decode_options& options = {})
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::decode(r, sink, options);
	}

	// A one-row RGB image of 16-bit samples, written losslessly in the colour encoding given, so the decoder
	// gives the samples back as they are in that encoding.
	bytes write_jxl16(const std::vector<std::uint16_t>& rgb, const JxlColorEncoding& color)
	{
		auto* const encoder = JxlEncoderCreate(nullptr);
		JxlBasicInfo info;
		JxlEncoderInitBasicInfo(&info);
		info.xsize = static_cast<std::uint32_t>(rgb.size() / 3);
		info.ysize = 1;
		info.bits_per_sample = 16;
		info.num_color_channels = 3;
		info.uses_original_profile = JXL_TRUE;
		bytes file;

		if (JxlEncoderSetBasicInfo(encoder, &info) == JXL_ENC_SUCCESS &&
		    JxlEncoderSetColorEncoding(encoder, &color) == JXL_ENC_SUCCESS)
		{
			auto* const settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
			JxlEncoderSetFrameLossless(settings, JXL_TRUE);
			const JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
			JxlEncoderAddImageFrame(settings, &format, rgb.data(), rgb.size() * 2);
			JxlEncoderCloseInput(encoder);

			file.resize(1024);
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

	// Every pixel of a decode is the written one, as BGRA, the colour within a level where it was
	// premultiplied and is made straight again.
	bool pixels_are(const jxl_sink& sink, const jxl_spec& s, const int frame = 0)
	{
		for (std::uint32_t y = 0; y < s.height; ++y)
		{
			for (std::uint32_t x = 0; x < s.width; ++x)
			{
				const auto expected = source_pixel(x, y, frame, s.alpha);
				const auto* const p = sink.pixel(x, y);
				if (p[3] != expected[3]) return false;
				if (s.premultiplied && expected[3] == 0) continue;

				const std::array<std::uint8_t, 3> colour = s.grey
				                                               ? std::array<std::uint8_t, 3>{expected[1], expected[1],
				                                                                             expected[1]}
				                                               : std::array<std::uint8_t, 3>{expected[2], expected[1],
				                                                                             expected[0]};
				const auto tolerance = s.premultiplied ? 2 : 0;

				for (auto c = 0; c < 3; ++c)
					if (std::abs(static_cast<int>(p[c]) - static_cast<int>(colour[c])) > tolerance) return false;
			}
		}

		return true;
	}
}

TEST("Should decode a lossless JPEG XL's exact pixels as BGRX, and as BGRA where it has alpha")
{
	for (const auto alpha : {false, true})
	{
		jxl_spec spec;
		spec.width = 33;
		spec.height = 17;
		spec.alpha = alpha;
		const auto file = write_jxl(spec);
		CHECK(!file.empty());

		jxl_sink sink;
		const auto result = decode_file(file, sink);
		CHECK(result.success);
		CHECK_EQ(sink.calls, 1);
		CHECK_EQ(result.frame.width, 33u);
		CHECK_EQ(result.frame.height, 17u);
		CHECK_EQ(result.frame.source_width, 33u);
		CHECK(result.frame.format == (alpha ? libfiles::pixel_format::bgra8 : libfiles::pixel_format::bgrx8));
		CHECK_EQ(result.frame.orientation, 1u);
		CHECK(pixels_are(sink, spec));
		CHECK(sink.padding_kept());
	}
}

TEST("Should make a JPEG XL's premultiplied alpha straight")
{
	jxl_spec spec;
	spec.width = 9;
	spec.height = 6;
	spec.alpha = true;
	spec.premultiplied = true;

	jxl_sink sink;
	const auto result = decode_file(write_jxl(spec), sink);
	CHECK(result.success);
	CHECK(pixels_are(sink, spec));
}

TEST("Should report a JPEG XL's orientation from its codestream, and leave its pixels unturned")
{
	jxl_spec spec;
	spec.orientation = 6;
	spec.exif = exif_box(3);

	jxl_sink sink;
	const auto file = write_jxl(spec);
	CHECK(file.size() > 12 && file[4] == 'J' && file[5] == 'X' && file[6] == 'L');
	const auto result = decode_file(file, sink);
	CHECK(result.success);
	CHECK_EQ(result.frame.orientation, 6u);
	CHECK_EQ(result.frame.width, 7u);
	CHECK_EQ(result.frame.height, 5u);
	CHECK(pixels_are(sink, spec));
}

TEST("Should decode a grey JPEG XL as BGRX, and an animated one's first frame, which a scan calls an animation")
{
	jxl_spec grey;
	grey.grey = true;
	const auto grey_file = write_jxl(grey);
	jxl_sink grey_sink;
	CHECK(decode_file(grey_file, grey_sink).success);
	CHECK(pixels_are(grey_sink, grey));

	jxl_spec animated;
	animated.frames = 2;
	const auto animated_file = write_jxl(animated);
	jxl_sink animated_sink;
	CHECK(decode_file(animated_file, animated_sink).success);
	CHECK(pixels_are(animated_sink, animated, 0));
	CHECK(!pixels_are(animated_sink, animated, 1));

	libfiles::reader grey_reader{std::span<const std::uint8_t>(grey_file)};
	CHECK(!libfiles::scan(grey_reader).animated);
	libfiles::reader animated_reader{std::span<const std::uint8_t>(animated_file)};
	CHECK(libfiles::scan(animated_reader).animated);
}

TEST("Should not decode a JPEG XL cut short, and refuse what the sink refuses")
{
	jxl_spec spec;
	spec.width = 64;
	spec.height = 48;
	const auto file = write_jxl(spec);

	const bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() * 3 / 4));
	jxl_sink cut_sink;
	const auto cut_result = decode_file(cut, cut_sink);
	CHECK(!cut_result.success && !cut_result.error.empty());
	CHECK_EQ(cut_sink.calls, 1);

	const bytes header(file.begin(), file.begin() + 4);
	jxl_sink header_sink;
	const auto header_result = decode_file(header, header_sink);
	CHECK(!header_result.success);
	CHECK_EQ(header_sink.calls, 0);

	jxl_sink refusing;
	refusing.refuse = true;
	const auto refused = decode_file(file, refusing);
	CHECK(!refused.success && refused.refused && refused.error.empty());
	CHECK_EQ(refusing.calls, 1);
}

TEST("Should decode a JPEG XL from a stream as from memory")
{
	jxl_spec spec;
	spec.width = 40;
	spec.height = 30;
	spec.alpha = true;
	const auto file = write_jxl(spec);

	jxl_sink from_memory;
	const auto memory_result = decode_file(file, from_memory);

	libfiles::memory_stream source(file);
	libfiles::reader r(source, 4096);
	jxl_sink from_stream;
	const auto stream_result = libfiles::decode(r, from_stream);

	CHECK(memory_result.success && stream_result.success);
	CHECK(from_stream.memory == from_memory.memory);
}

TEST("Should keep a JPEG XL's sixteen bits for a caller that takes them")
{
	JxlColorEncoding srgb;
	JxlColorEncodingSetToSRGB(&srgb, JXL_FALSE);
	const auto file = write_jxl16({0x1234, 0xABCD, 0x0102, 0xFFFF, 0x0000, 0x8000}, srgb);
	CHECK(!file.empty());

	libfiles::decode_options options;
	options.allow_16bit = true;
	jxl_sink sink;
	CHECK(decode_file(file, sink, options).success);
	CHECK(sink.told.format == libfiles::pixel_format::rgbx16);
	CHECK(sink.wide_pixel(0, 0) == (std::array<std::uint16_t, 4>{0x1234, 0xABCD, 0x0102, 0xFFFF}));
	CHECK(sink.wide_pixel(1, 0) == (std::array<std::uint16_t, 4>{0xFFFF, 0x0000, 0x8000, 0xFFFF}));
	CHECK(sink.padding_kept());

	// Without the option, bytes, as before.
	jxl_sink byte_sink;
	CHECK(decode_file(file, byte_sink).success);
	CHECK(byte_sink.told.format == libfiles::pixel_format::bgrx8);
	CHECK(std::abs(byte_sink.pixel(0, 0)[2] - 18) <= 1);
}

// A neutral at 50 nits of PQ comes out at 50 / 203 of SDR white, sRGB-encoded, as a PQ PNG's does.
TEST("Should map a JPEG XL whose colour encoding is PQ to SDR")
{
	JxlColorEncoding pq;
	JxlColorEncodingSetToSRGB(&pq, JXL_FALSE);
	pq.primaries = JXL_PRIMARIES_2100;
	pq.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
	const auto file = write_jxl16({28854, 28854, 28854, 65535, 65535, 65535}, pq);
	CHECK(!file.empty());

	jxl_sink sink;
	const auto result = decode_file(file, sink);
	CHECK(result.success);
	CHECK(result.frame.tone_mapped);
	CHECK(sink.told.format == libfiles::pixel_format::bgrx8);

	const auto* const grey = sink.pixel(0, 0);
	CHECK(std::abs(grey[0] - 136) <= 1 && std::abs(grey[1] - 136) <= 1 && std::abs(grey[2] - 136) <= 1);
	CHECK(sink.pixel(1, 0)[1] == 255);
	CHECK(sink.padding_kept());
}

#endif
