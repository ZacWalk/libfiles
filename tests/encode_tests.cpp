// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for encoding JPEGs, PNGs and WebPs and turning JPEGs losslessly: what each encoder
// writes, read back by libfiles' own decoders and by walking the file's markers, chunks or boxes; the
// metadata each embeds, and what each refuses.

#include "test.h"

#include "libfiles/decode.h"
#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "libfiles/reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jpeglib.h>

namespace
{
	using bytes = std::vector<std::uint8_t>;

	// A packed image, its rows padded so a stride is not taken for a width.
	struct picture
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::size_t stride = 0;
		libfiles::pixel_format format = libfiles::pixel_format::bgrx8;
		bytes pixels;

		libfiles::image_view view() const
		{
			return {pixels.data(), stride, width, height, format};
		}
	};

	picture gradient(const std::uint32_t width, const std::uint32_t height,
	                 const libfiles::pixel_format format = libfiles::pixel_format::bgrx8)
	{
		picture p{width, height, std::size_t{4} * width + 8, format, {}};
		p.pixels.assign(p.stride * height, 0x5A);

		for (std::uint32_t y = 0; y < height; ++y)
		{
			for (std::uint32_t x = 0; x < width; ++x)
			{
				auto* const px = p.pixels.data() + y * p.stride + std::size_t{4} * x;
				px[0] = static_cast<std::uint8_t>(x * 255 / (width > 1 ? width - 1 : 1));
				px[1] = static_cast<std::uint8_t>(y * 255 / (height > 1 ? height - 1 : 1));
				px[2] = static_cast<std::uint8_t>(128 + (x + y) % 64);
				px[3] = format == libfiles::pixel_format::bgra8 ? static_cast<std::uint8_t>(64 + (x * 3 + y) % 192) : 0x11;
			}
		}

		return p;
	}

	// NV12 whose luma is a ramp and whose chroma is constant.
	picture nv12(const std::uint32_t width, const std::uint32_t height, const std::uint8_t cb, const std::uint8_t cr)
	{
		picture p{width, height, std::size_t{width} + 4, libfiles::pixel_format::nv12, {}};
		p.pixels.assign(p.stride * (height + height / 2), 0);

		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
				p.pixels[y * p.stride + x] = static_cast<std::uint8_t>(16 + (x * 200) / (width > 1 ? width - 1 : 1));

		for (std::uint32_t y = 0; y < height / 2; ++y)
		{
			for (std::uint32_t x = 0; x < width / 2; ++x)
			{
				p.pixels[(height + y) * p.stride + 2 * x] = cb;
				p.pixels[(height + y) * p.stride + 2 * x + 1] = cr;
			}
		}

		return p;
	}

	class capture_sink final : public libfiles::image_sink
	{
	public:
		libfiles::frame_info told;
		bytes memory;
		std::size_t stride = 0;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			told = frame;
			told.exif = {};
			stride = std::size_t{4} * frame.width;
			memory.assign(stride * frame.height, 0);
			return {memory.data(), stride};
		}

		const std::uint8_t* at(const std::uint32_t x, const std::uint32_t y) const
		{
			return memory.data() + y * stride + std::size_t{4} * x;
		}
	};

	libfiles::decode_result decode_bytes(const bytes& file, capture_sink& sink)
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::decode(r, sink);
	}

	// The mean difference of the colour channels between what was encoded and what decoded.
	double mean_difference(const picture& encoded, const capture_sink& decoded)
	{
		double total = 0;

		for (std::uint32_t y = 0; y < encoded.height; ++y)
		{
			for (std::uint32_t x = 0; x < encoded.width; ++x)
			{
				const auto* const a = encoded.pixels.data() + y * encoded.stride + std::size_t{4} * x;
				const auto* const b = decoded.at(x, y);
				for (auto c = 0; c < 3; ++c) total += std::abs(a[c] - b[c]);
			}
		}

		return total / (3.0 * encoded.width * encoded.height);
	}

	// The largest difference of any colour channel between what was encoded and what decoded.
	int largest_difference(const picture& encoded, const capture_sink& decoded)
	{
		auto largest = 0;

		for (std::uint32_t y = 0; y < encoded.height; ++y)
		{
			for (std::uint32_t x = 0; x < encoded.width; ++x)
			{
				const auto* const a = encoded.pixels.data() + y * encoded.stride + std::size_t{4} * x;
				const auto* const b = decoded.at(x, y);
				for (auto c = 0; c < 3; ++c) largest = std::max(largest, std::abs(a[c] - b[c]));
			}
		}

		return largest;
	}

	bool starts_with(const bytes& b, const std::string_view s)
	{
		return b.size() >= s.size() && std::memcmp(b.data(), s.data(), s.size()) == 0;
	}

	bool contains(const bytes& b, const bytes& needle)
	{
		return std::search(b.begin(), b.end(), needle.begin(), needle.end()) != b.end();
	}

	// A JPEG's marker segments before its first scan.
	std::vector<std::pair<std::uint8_t, bytes>> jpeg_segments(const bytes& jpeg)
	{
		std::vector<std::pair<std::uint8_t, bytes>> result;
		std::size_t i = 2;

		while (i + 4 <= jpeg.size() && jpeg[i] == 0xFF && jpeg[i + 1] != 0xDA)
		{
			const auto length = static_cast<std::size_t>(jpeg[i + 2]) << 8 | jpeg[i + 3];
			if (length < 2 || i + 2 + length > jpeg.size()) break;
			result.emplace_back(jpeg[i + 1], bytes(jpeg.begin() + static_cast<std::ptrdiff_t>(i + 4),
			                                       jpeg.begin() + static_cast<std::ptrdiff_t>(i + 2 + length)));
			i += 2 + length;
		}

		return result;
	}

	// A PNG's chunks, by type.
	std::vector<std::pair<std::string, bytes>> png_chunks(const bytes& png)
	{
		std::vector<std::pair<std::string, bytes>> result;
		std::size_t i = 8;

		while (i + 12 <= png.size())
		{
			const auto length = static_cast<std::size_t>(png[i]) << 24 | static_cast<std::size_t>(png[i + 1]) << 16 |
				static_cast<std::size_t>(png[i + 2]) << 8 | png[i + 3];
			if (i + 12 + length > png.size()) break;
			result.emplace_back(std::string(png.begin() + static_cast<std::ptrdiff_t>(i + 4),
			                                png.begin() + static_cast<std::ptrdiff_t>(i + 8)),
			                    bytes(png.begin() + static_cast<std::ptrdiff_t>(i + 8),
			                          png.begin() + static_cast<std::ptrdiff_t>(i + 8 + length)));
			i += 12 + length;
		}

		return result;
	}

	// A WebP's top-level chunks, by fourcc.
	std::vector<std::string> webp_chunks(const bytes& webp)
	{
		std::vector<std::string> result;
		std::size_t i = 12;

		while (i + 8 <= webp.size())
		{
			const auto length = static_cast<std::size_t>(webp[i + 4]) | static_cast<std::size_t>(webp[i + 5]) << 8 |
				static_cast<std::size_t>(webp[i + 6]) << 16 | static_cast<std::size_t>(webp[i + 7]) << 24;
			result.emplace_back(webp.begin() + static_cast<std::ptrdiff_t>(i),
			                    webp.begin() + static_cast<std::ptrdiff_t>(i + 4));
			i += 8 + length + (length & 1);
		}

		return result;
	}

	// The smallest ICC profile libpng accepts for an RGB image: a header naming RGB data and the XYZ
	// connection space, and an empty tag table.
	bytes rgb_icc_profile()
	{
		bytes icc(132, 0);
		icc[3] = 132;
		icc[8] = 0x02;
		icc[9] = 0x10;
		std::memcpy(icc.data() + 12, "mntr", 4);
		std::memcpy(icc.data() + 16, "RGB ", 4);
		std::memcpy(icc.data() + 20, "XYZ ", 4);
		std::memcpy(icc.data() + 36, "acsp", 4);
		// The D50 illuminant, as s15Fixed16 numbers.
		const std::uint8_t d50[] = {0x00, 0x00, 0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xD3, 0x2D};
		std::memcpy(icc.data() + 68, d50, sizeof d50);
		return icc;
	}

	bytes encode_jpeg(const picture& p, const std::uint16_t orientation = 1,
	                  const libfiles::embedded_metadata& metadata = {}, const libfiles::jpeg_options& options = {})
	{
		libfiles::jpeg_encoder encoder;
		return encoder.encode(p.width, p.height, p.pixels.data(), p.stride, orientation, metadata, options);
	}
}

#ifdef LIBFILES_JPEG

TEST("Should encode BGRX rows as a JPEG that decodes to them")
{
	const auto p = gradient(64, 48);
	libfiles::jpeg_encoder encoder;
	const auto jpeg = encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, {});

	CHECK(jpeg.size() > 100);
	CHECK_EQ(encoder.width(), 64u);
	CHECK_EQ(encoder.height(), 48u);
	CHECK(encoder.notes().empty());

	capture_sink sink;
	const auto result = decode_bytes(jpeg, sink);

	CHECK(result.success);
	CHECK_EQ(sink.told.width, 64u);
	CHECK_EQ(sink.told.height, 48u);
	CHECK_EQ(sink.told.orientation, std::uint16_t{1});
	CHECK(mean_difference(p, sink) < 3.0);
}

TEST("Should write a JPEG's EXIF, Photoshop resources, XMP and ICC profile in that order")
{
	const bytes exif = {'I', 'I', 0x2A, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	const bytes xmp = {'<', 'x', '/', '>'};
	const bytes iptc = {0x1C, 0x02, 0x05, 0x00, 0x03, 'c', 'a', 't'};
	const bytes icc(300, 0x42);

	libfiles::embedded_metadata metadata;
	metadata.exif = exif;
	metadata.xmp = xmp;
	metadata.iptc = iptc;
	metadata.icc = icc;

	const auto segments = jpeg_segments(encode_jpeg(gradient(16, 16), 1, metadata));

	std::vector<std::string> order;
	for (const auto& [marker, payload] : segments)
	{
		if (marker == 0xE1 && starts_with(payload, std::string_view("Exif\0\0", 6))) order.emplace_back("exif");
		if (marker == 0xE1 && starts_with(payload, std::string_view("http://ns.adobe.com/xap/1.0/\0", 29)))
		{
			order.emplace_back("xmp");
			CHECK(bytes(payload.begin() + 29, payload.end()) == xmp);
		}
		if (marker == 0xED)
		{
			order.emplace_back("photoshop");
			CHECK(starts_with(payload, std::string_view("Photoshop 3.0\0" "8BIM\x04\x04\0\0\0\0\0\x08", 26)));
		}
		if (marker == 0xE2 && starts_with(payload, std::string_view("ICC_PROFILE\0", 12)))
		{
			order.emplace_back("icc");
			CHECK_EQ(payload[12], std::uint8_t{1});
			CHECK_EQ(payload[13], std::uint8_t{1});
			CHECK_EQ(payload.size(), std::size_t{14 + 300});
		}
	}

	CHECK(order == (std::vector<std::string>{"exif", "photoshop", "xmp", "icc"}));
}

TEST("Should keep an EXIF that already carries its signature as it is")
{
	bytes exif = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2A, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	libfiles::embedded_metadata metadata;
	metadata.exif = exif;

	const auto segments = jpeg_segments(encode_jpeg(gradient(16, 16), 1, metadata));
	auto found = false;
	for (const auto& [marker, payload] : segments) found |= marker == 0xE1 && payload == exif;
	CHECK(found);
}

TEST("Should write an EXIF holding only the orientation when a JPEG has no other")
{
	capture_sink turned;
	CHECK(decode_bytes(encode_jpeg(gradient(16, 16), 6), turned).success);
	CHECK_EQ(turned.told.orientation, std::uint16_t{6});

	for (const std::uint16_t none : {std::uint16_t{0}, std::uint16_t{1}})
	{
		auto exif_segments = 0;
		for (const auto& [marker, payload] : jpeg_segments(encode_jpeg(gradient(16, 16), none)))
			exif_segments += marker == 0xE1;
		CHECK_EQ(exif_segments, 0);
	}
}

TEST("Should split a large ICC profile across numbered APP2 segments")
{
	bytes icc(100000);
	for (std::size_t i = 0; i < icc.size(); ++i) icc[i] = static_cast<std::uint8_t>(i * 7);
	libfiles::embedded_metadata metadata;
	metadata.icc = icc;

	bytes joined;
	std::vector<std::pair<int, int>> numbers;
	for (const auto& [marker, payload] : jpeg_segments(encode_jpeg(gradient(16, 16), 1, metadata)))
	{
		if (marker != 0xE2) continue;
		numbers.emplace_back(payload[12], payload[13]);
		joined.insert(joined.end(), payload.begin() + 14, payload.end());
	}

	CHECK(numbers == (std::vector<std::pair<int, int>>{{1, 2}, {2, 2}}));
	CHECK(joined == icc);
}

TEST("Should refuse metadata a JPEG cannot carry, and stay usable")
{
	libfiles::jpeg_encoder encoder;
	const auto p = gradient(16, 16);

	const bytes big_xmp(70000, 'x');
	libfiles::embedded_metadata xmp;
	xmp.xmp = big_xmp;

	auto refused = false;
	try
	{
		(void)encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, xmp, {});
	}
	catch (const libfiles::encode_error& e)
	{
		refused = std::string_view(e.what()) == "the xmp block is 70029 bytes, over the 65533 byte JPEG marker limit";
	}
	CHECK(refused);

	const bytes huge_icc(255 * 65519 + 1, 1);
	libfiles::embedded_metadata icc;
	icc.icc = huge_icc;
	CHECK_THROWS(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, icc, {}));

	capture_sink sink;
	CHECK(decode_bytes(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, {}), sink).success);
}

TEST("Should drop a Photoshop resource too large for any marker, and note it")
{
	// One record of 70000 bytes, and one small one the JPEG keeps.
	bytes resources = {'8', 'B', 'I', 'M', 0x04, 0x0C, 0, 0, 0x00, 0x01, 0x11, 0x70};
	resources.resize(resources.size() + 70000, 0x33);
	const bytes kept = {'8', 'B', 'I', 'M', 0x03, 0xED, 0, 0, 0, 0, 0, 2, 0xAB, 0xCD};
	resources.insert(resources.end(), kept.begin(), kept.end());

	libfiles::embedded_metadata metadata;
	metadata.photoshop_resources = resources;

	libfiles::jpeg_encoder encoder;
	const auto p = gradient(16, 16);
	const auto jpeg = encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, metadata, {});

	CHECK(!encoder.notes().empty());
	CHECK(encoder.notes().front() == "dropping oversized Photoshop resource (70012 bytes)");

	auto photoshop = 0;
	for (const auto& [marker, payload] : jpeg_segments(jpeg))
	{
		if (marker != 0xED) continue;
		++photoshop;
		CHECK(payload.size() == 14 + kept.size());
		CHECK(contains(payload, kept));
	}
	CHECK_EQ(photoshop, 1);
}

TEST("Should end the Photoshop resources at one whose length reaches 4 GiB, on every target")
{
	// Its padded length passes 32 bits, which on x86 once wrapped to a 12-byte record and walked on.
	bytes resources = {'8', 'B', 'I', 'M', 0x03, 0xED, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
	const bytes next = {'8', 'B', 'I', 'M', 0x03, 0xED, 0, 0, 0, 0, 0, 2, 0xAB, 0xCD};
	resources.insert(resources.end(), next.begin(), next.end());

	libfiles::embedded_metadata metadata;
	metadata.photoshop_resources = resources;

	const auto segments = jpeg_segments(encode_jpeg(gradient(16, 16), 1, metadata));
	CHECK(std::ranges::none_of(segments, [](const auto& s) { return s.first == 0xED; }));
}

TEST("Should write a last Photoshop resource that lacks its pad byte with it")
{
	const bytes resources = {'8', 'B', 'I', 'M', 0x03, 0xED, 0, 0, 0, 0, 0, 3, 'r', 'e', 's'};

	libfiles::embedded_metadata metadata;
	metadata.photoshop_resources = resources;

	bytes expected = {'P', 'h', 'o', 't', 'o', 's', 'h', 'o', 'p', ' ', '3', '.', '0', 0};
	expected.insert(expected.end(), resources.begin(), resources.end());
	expected.push_back(0);

	auto photoshop = 0;
	for (const auto& [marker, payload] : jpeg_segments(encode_jpeg(gradient(16, 16), 1, metadata)))
	{
		if (marker != 0xED) continue;
		++photoshop;
		CHECK(payload == expected);
	}
	CHECK_EQ(photoshop, 1);
}

namespace
{
	// The quantization tables a JPEG declares, as its DQT segments hold them.
	bytes tables(const bytes& jpeg)
	{
		bytes result;
		for (const auto& [marker, payload] : jpeg_segments(jpeg))
			if (marker == 0xDB) result.insert(result.end(), payload.begin(), payload.end());
		return result;
	}

	// An 8-bit lossless JPEG: its header reads as plain YCbCr, and libjpeg refuses to copy its tables only
	// when asked to, through the encoder taking them.
	bytes lossless_ycbcr_jpeg(const std::uint32_t width, const std::uint32_t height)
	{
		jpeg_compress_struct c{};
		jpeg_error_mgr err{};
		c.err = jpeg_std_error(&err);
		jpeg_create_compress(&c);

		unsigned char* out = nullptr;
		unsigned long out_size = 0;
		jpeg_mem_dest(&c, &out, &out_size);

		c.image_width = width;
		c.image_height = height;
		c.input_components = 3;
		c.in_color_space = JCS_YCbCr;
		jpeg_set_defaults(&c);
		for (int ci = 0; ci < c.num_components; ++ci) c.comp_info[ci].h_samp_factor = c.comp_info[ci].v_samp_factor = 1;
		jpeg_enable_lossless(&c, 1, 0);
		jpeg_start_compress(&c, TRUE);

		std::vector<JSAMPLE> row(std::size_t{3} * width);
		while (c.next_scanline < c.image_height)
		{
			for (std::size_t i = 0; i < row.size(); ++i) row[i] = static_cast<JSAMPLE>(i * 7 + c.next_scanline * 3);
			JSAMPROW rows[1] = {row.data()};
			jpeg_write_scanlines(&c, rows, 1);
		}

		jpeg_finish_compress(&c);
		bytes result(out, out + out_size);
		jpeg_destroy_compress(&c);
		std::free(out);
		return result;
	}
}

TEST("Should adopt a source JPEG's quantization tables, and fall back to the quality")
{
	const auto p = gradient(32, 32);
	libfiles::jpeg_options coarse;
	coarse.quality = 20;
	const auto source = encode_jpeg(p, 1, {}, coarse);

	libfiles::jpeg_options adopting;
	adopting.quality = 95;
	adopting.source = source;
	CHECK(tables(encode_jpeg(p, 1, {}, adopting)) == tables(source));

	// A source whose header ends after its SOI: libjpeg suspends rather than fails, and the quality
	// stands.
	libfiles::jpeg_encoder encoder;
	const bytes header_cut_short(source.begin(), source.begin() + 2);
	adopting.source = header_cut_short;
	const auto fallback = encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, adopting);
	libfiles::jpeg_options plain;
	plain.quality = 95;
	CHECK(tables(fallback) == tables(encode_jpeg(p, 1, {}, plain)));
	CHECK(encoder.notes().empty());
}

TEST("Should fall back to the quality from a source libjpeg fails on, and note why")
{
	const auto p = gradient(32, 32);
	libfiles::jpeg_options plain;
	plain.quality = 95;
	const auto expected = tables(encode_jpeg(p, 1, {}, plain));

	libfiles::jpeg_encoder encoder;
	auto adopting = plain;

	// Not a JPEG: libjpeg fails reading the source's header.
	const bytes not_jpeg = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
	adopting.source = not_jpeg;
	CHECK(tables(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, adopting)) == expected);
	CHECK_EQ(encoder.notes().size(), std::size_t{1});
	CHECK(encoder.notes().front().starts_with("Not a JPEG file"));

	// Lossless: libjpeg fails as the encoder takes the source's tables.
	const auto lossless = lossless_ycbcr_jpeg(16, 16);
	adopting.source = lossless;
	CHECK(tables(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, adopting)) == expected);
	CHECK_EQ(encoder.notes().size(), std::size_t{1});

	// The encoder's own errors still reach its caller, and it still encodes.
	CHECK_THROWS(encoder.encode(0, 0, p.pixels.data(), p.stride, 1, {}, plain));
	CHECK(tables(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, plain)) == expected);
}

TEST("Should reuse a JPEG encoder after an image it abandoned")
{
	libfiles::jpeg_encoder encoder;
	const auto p = gradient(32, 32);

	// start() leaves the encoder mid-compress, as a throw out of a row or a marker write would.
	encoder.start(p.width, p.height, 1, {}, {});
	CHECK_EQ(encoder.next_row(), 0u);

	capture_sink sink;
	CHECK(decode_bytes(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, {}), sink).success);
}

TEST("Should read a JPEG's header and the coefficients it holds")
{
	libfiles::jpeg_transformer transformer;
	CHECK(transformer.read_header(encode_jpeg(gradient(64, 32))));
	CHECK_EQ(transformer.width(), 64u);
	CHECK_EQ(transformer.height(), 32u);
	// 4:2:0: 8 by 4 luma blocks and 4 by 2 of each chroma, 128 bytes a block.
	CHECK_EQ(transformer.coefficient_bytes(), std::int64_t{(32 + 8 + 8) * 128});
}

TEST("Should turn a JPEG losslessly, its EXIF brought up to date")
{
	const auto p = gradient(64, 32);
	libfiles::exif_fields fields;
	fields.orientation = std::uint16_t{6};
	const auto exif = libfiles::make_exif(fields);
	libfiles::embedded_metadata metadata;
	metadata.exif = exif;
	const auto source = encode_jpeg(p, 1, metadata);

	libfiles::jpeg_transformer transformer;
	libfiles::jpeg_encoder encoder;
	const auto turned = transformer.turn(source, encoder, 6, std::uint64_t{1} << 30);

	CHECK(!turned.data.empty());
	CHECK(!turned.over_budget);
	CHECK_EQ(turned.width, 64u);
	CHECK_EQ(turned.height, 32u);

	capture_sink before;
	capture_sink after;
	CHECK(decode_bytes(source, before).success);
	CHECK(decode_bytes(turned.data, after).success);
	CHECK_EQ(before.told.orientation, std::uint16_t{6});
	CHECK_EQ(after.told.orientation, std::uint16_t{1});
	CHECK_EQ(after.told.width, 32u);
	CHECK_EQ(after.told.height, 64u);

	// A quarter turn clockwise: the source's bottom-left pixel is the result's top-left.
	auto largest = 0;
	for (std::uint32_t y = 0; y < 64; ++y)
		for (std::uint32_t x = 0; x < 32; ++x)
			for (auto c = 0; c < 3; ++c)
				largest = std::max(largest, std::abs(after.at(x, y)[c] - before.at(y, 31 - x)[c]));
	// libjpeg's integer IDCT and its chroma upsampling round differently once turned.
	CHECK(largest <= 4);
}

TEST("Should refuse a turn that would not be lossless, or is over the budget")
{
	libfiles::jpeg_transformer transformer;
	libfiles::jpeg_encoder encoder;

	const auto partial = transformer.turn(encode_jpeg(gradient(60, 30)), encoder, 6, std::uint64_t{1} << 30);
	CHECK(partial.data.empty());
	CHECK(!partial.over_budget);

	const auto source = encode_jpeg(gradient(64, 32));
	const auto refused = transformer.turn(source, encoder, 3, 1000);
	CHECK(refused.data.empty());
	CHECK(refused.over_budget);
	CHECK_EQ(refused.workspace_bytes, std::int64_t{2 * 48 * 128});

	// Neither left the codecs part way through.
	CHECK(!transformer.turn(source, encoder, 3, std::uint64_t{1} << 30).data.empty());
	capture_sink sink;
	const auto p = gradient(16, 16);
	CHECK(decode_bytes(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, {}), sink).success);
}

TEST("Should leave a JPEG unturned for orientation 1 and anything past 8")
{
	libfiles::jpeg_transformer transformer;
	libfiles::jpeg_encoder encoder;
	const auto source = encode_jpeg(gradient(64, 32));

	for (const std::uint16_t none : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{9}})
	{
		capture_sink sink;
		CHECK(decode_bytes(transformer.turn(source, encoder, none, std::uint64_t{1} << 30).data, sink).success);
		CHECK_EQ(sink.told.width, 64u);
		CHECK_EQ(sink.told.height, 32u);
	}
}

TEST("Should leave a JPEG cut short unturned, and throw for one that is not a JPEG")
{
	auto source = encode_jpeg(gradient(64, 32));
	source.resize(source.size() / 2);

	libfiles::jpeg_transformer transformer;
	libfiles::jpeg_encoder encoder;

	// The source manager has nothing more to give, so libjpeg suspends rather than fails.
	const auto cut = transformer.turn(source, encoder, 6, std::uint64_t{1} << 30);
	CHECK(cut.data.empty());
	CHECK(!cut.over_budget);

	const bytes garbage = {0x00, 0x01, 0x02, 0x03};
	CHECK_THROWS(transformer.turn(garbage, encoder, 6, std::uint64_t{1} << 30));
}

#endif

#ifdef LIBFILES_PNG

TEST("Should encode BGRX as an RGB PNG and BGRA as an RGBA one, losslessly")
{
	for (const auto format : {libfiles::pixel_format::bgrx8, libfiles::pixel_format::bgra8})
	{
		const auto p = gradient(20, 10, format);
		const auto png = libfiles::encode_png(p.view(), 1, {});
		const auto chunks = png_chunks(png.data);

		CHECK(!chunks.empty());
		CHECK(chunks.front().first == "IHDR");
		CHECK_EQ(chunks.front().second[9], format == libfiles::pixel_format::bgrx8 ? std::uint8_t{2} : std::uint8_t{6});

		capture_sink sink;
		CHECK(decode_bytes(png.data, sink).success);
		CHECK_EQ(largest_difference(p, sink), 0);

		if (format == libfiles::pixel_format::bgra8)
		{
			auto alpha_kept = true;
			for (std::uint32_t y = 0; y < p.height; ++y)
				for (std::uint32_t x = 0; x < p.width; ++x)
					alpha_kept &= sink.at(x, y)[3] == p.pixels[y * p.stride + std::size_t{4} * x + 3];
			CHECK(alpha_kept);
		}
	}
}

TEST("Should embed a PNG's ICC profile, XMP and EXIF")
{
	const auto icc = rgb_icc_profile();
	const bytes xmp = {'<', 'x', '/', '>'};
	const bytes exif = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2A, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	libfiles::embedded_metadata metadata;
	metadata.icc = icc;
	metadata.xmp = xmp;
	metadata.exif = exif;

	std::vector<std::string> types;
	for (const auto& [type, data] : png_chunks(libfiles::encode_png(gradient(8, 8).view(), 1, metadata).data))
	{
		types.push_back(type);
		if (type == "eXIf") CHECK(data == bytes(exif.begin() + 6, exif.end()));
		if (type == "iTXt") CHECK(starts_with(data, std::string_view("XML:com.adobe.xmp\0", 18)));
		if (type == "iCCP") CHECK(starts_with(data, std::string_view("icc\0", 4)));
	}

	CHECK(std::find(types.begin(), types.end(), "iCCP") != types.end());
	CHECK(std::find(types.begin(), types.end(), "iTXt") != types.end());
	CHECK(std::find(types.begin(), types.end(), "eXIf") != types.end());
}

TEST("Should write a PNG's orientation as an eXIf, and none for orientation 1")
{
	capture_sink turned;
	CHECK(decode_bytes(libfiles::encode_png(gradient(8, 8).view(), 8, {}).data, turned).success);
	CHECK_EQ(turned.told.orientation, std::uint16_t{8});

	auto exif_chunks = 0;
	for (const auto& [type, data] : png_chunks(libfiles::encode_png(gradient(8, 8).view(), 1, {}).data))
		exif_chunks += type == "eXIf";
	CHECK_EQ(exif_chunks, 0);
}

#endif

#ifdef LIBFILES_WEBP

TEST("Should encode a WebP losslessly, alpha and all")
{
	const auto p = gradient(24, 12, libfiles::pixel_format::bgra8);
	libfiles::webp_options options;
	options.lossless = true;
	const auto webp = libfiles::encode_webp(p.view(), 1, {}, options);

	CHECK(!webp.data.empty());
	CHECK(webp.notes.empty());

	capture_sink sink;
	CHECK(decode_bytes(webp.data, sink).success);
	CHECK_EQ(largest_difference(p, sink), 0);
}

TEST("Should encode a WebP lossily from packed pixels and from even NV12")
{
	const auto packed = gradient(32, 32);
	libfiles::webp_options options;
	options.quality = 90;

	capture_sink from_packed;
	CHECK(decode_bytes(libfiles::encode_webp(packed.view(), 1, {}, options).data, from_packed).success);
	CHECK(largest_difference(packed, from_packed) < 40);

	// Mid-grey chroma: a ramp of greys, whatever the range.
	for (const auto limited : {false, true})
	{
		auto planes = nv12(32, 16, 128, 128);
		auto view = planes.view();
		view.limited_range = limited;

		capture_sink sink;
		CHECK(decode_bytes(libfiles::encode_webp(view, 1, {}, options).data, sink).success);
		CHECK_EQ(sink.told.width, 32u);

		const auto* const left = sink.at(0, 8);
		const auto* const right = sink.at(31, 8);
		CHECK(right[1] > left[1] + 100);
		CHECK(std::abs(left[0] - left[2]) < 12);
	}

	auto odd = nv12(31, 16, 128, 128);
	CHECK(libfiles::encode_webp(odd.view(), 1, {}, options).data.empty());

	options.lossless = true;
	auto even = nv12(32, 16, 128, 128);
	CHECK(libfiles::encode_webp(even.view(), 1, {}, options).data.empty());
}

TEST("Should mux a WebP's ICC profile, EXIF and XMP")
{
	const bytes icc(64, 0x42);
	const bytes xmp = {'<', 'x', '/', '>'};
	const bytes exif = {'I', 'I', 0x2A, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	libfiles::embedded_metadata metadata;
	metadata.icc = icc;
	metadata.xmp = xmp;
	metadata.exif = exif;

	const auto chunks = webp_chunks(libfiles::encode_webp(gradient(16, 16).view(), 1, metadata, {}).data);
	CHECK(chunks.size() >= 4);
	CHECK(chunks.front() == "VP8X");
	CHECK(std::find(chunks.begin(), chunks.end(), "ICCP") != chunks.end());
	CHECK(std::find(chunks.begin(), chunks.end(), "EXIF") != chunks.end());
	CHECK(std::find(chunks.begin(), chunks.end(), "XMP ") != chunks.end());

	capture_sink turned;
	CHECK(decode_bytes(libfiles::encode_webp(gradient(16, 16).view(), 3, {}, {}).data, turned).success);
	CHECK_EQ(turned.told.orientation, std::uint16_t{3});
}

TEST("Should fail a WebP whose metadata chunk will not go in")
{
	const bytes xmp = {'<', 'x', '/', '>'};
	libfiles::embedded_metadata metadata;
	metadata.xmp = xmp;

	libfiles::test_hooks::fail_next_webp_chunk("XMP ");
	const auto failed = libfiles::encode_webp(gradient(16, 16).view(), 1, metadata, {});
	CHECK(failed.data.empty());
	CHECK(!failed.notes.empty());

	// Once, then cleared.
	CHECK(!libfiles::encode_webp(gradient(16, 16).view(), 1, metadata, {}).data.empty());
}

TEST("Should refuse a WebP of a pixel format it does not encode")
{
	auto p = gradient(8, 8);
	p.format = libfiles::pixel_format::bgra8;
	auto view = p.view();
	view.format = static_cast<libfiles::pixel_format>(0x7F);
	CHECK(libfiles::encode_webp(view, 1, {}, {}).data.empty());
}

#endif
