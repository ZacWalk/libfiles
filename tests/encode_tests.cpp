// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Tests for encoding JPEGs, PNGs, WebPs, JPEG XLs and AVIFs, turning JPEGs losslessly and repacking them as
// JPEG XLs: what each encoder writes, read back by libfiles' own decoders and by walking the file's markers,
// chunks or boxes; the metadata each embeds, what each refuses, and how a WebP or a JPEG XL was compressed.

#include "test.h"
#include "builders.h"

#include "gain_map_internal.h"
#include "libfiles/decode.h"
#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "libfiles/hdr.h"
#include "libfiles/reader.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"
#include "libfiles/xmp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jpeglib.h>

#ifdef LIBFILES_JXL
#include <jxl/decode.h>
#endif

#ifdef LIBFILES_HEIF
#include <libheif/heif.h>
#include <libheif/heif_brands.h>
#include <libheif/heif_entity_groups.h>
#include <libheif/heif_items.h>
#include <libheif/heif_metadata.h>
#include <libheif/heif_properties.h>
#endif

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

		libfiles::image_view view() const { return {pixels.data(), stride, width, height, format}; }
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
				px[3] = format == libfiles::pixel_format::bgra8 ? static_cast<std::uint8_t>(64 + (x * 3 + y) % 192)
				                                                : 0x11;
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

#if defined(LIBFILES_JXL) || defined(LIBFILES_HEIF)
	// A big-endian TIFF stream whose IFD0 holds an orientation and nothing else.
	bytes tiff_with_orientation(const std::uint8_t orientation)
	{
		return {'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, orientation, 0, 0, 0, 0, 0, 0};
	}

	libfiles::scan_result scan_bytes(const bytes& file)
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::scan(r);
	}
#endif

	// What an inspect scan's listing says in a row, or "(none)" where it has no such row.
	std::string listed(const bytes& file, const std::string_view key)
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		const auto rows = libfiles::scan(r, libfiles::scan_intent::inspect).structure;
		const auto it = std::ranges::find(rows, key, &libfiles::structure_row::key);
		return it == rows.end() ? std::string("(none)") : it->value;
	}

	// Sixteen bits a channel, red first, with odd values a byte-precision path could not carry.
	struct wide_picture
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		libfiles::pixel_format format = libfiles::pixel_format::rgbx16;
		std::vector<std::uint16_t> samples;

		libfiles::image_view view() const
		{
			return {reinterpret_cast<const std::uint8_t*>(samples.data()), std::size_t{8} * width, width, height,
			        format};
		}
	};

	wide_picture wide_gradient(const std::uint32_t width, const std::uint32_t height,
	                           const libfiles::pixel_format format)
	{
		wide_picture p{width, height, format, std::vector<std::uint16_t>(std::size_t{4} * width * height)};

		for (std::uint32_t y = 0; y < height; ++y)
		{
			for (std::uint32_t x = 0; x < width; ++x)
			{
				auto* const px = p.samples.data() + (std::size_t{y} * width + x) * 4;
				px[0] = static_cast<std::uint16_t>(x * 3001 + 7);
				px[1] = static_cast<std::uint16_t>(y * 4093 + 1);
				px[2] = static_cast<std::uint16_t>(65535 - (x + y) * 257);
				px[3] = format == libfiles::pixel_format::rgba16 ? static_cast<std::uint16_t>(1000 + x * y * 97)
				                                                 : 0xffff;
			}
		}

		return p;
	}

	// What a caller that takes sixteen bits is given.
	class wide_sink final : public libfiles::image_sink
	{
	public:
		libfiles::frame_info told;
		std::vector<std::uint16_t> samples;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			told = frame;
			told.exif = {};
			samples.assign(std::size_t{4} * frame.width * frame.height, 0);
			return {reinterpret_cast<std::uint8_t*>(samples.data()), std::size_t{8} * frame.width};
		}
	};

	libfiles::decode_result decode_wide(const bytes& file, wide_sink& sink)
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		libfiles::decode_options options;
		options.allow_16bit = true;
		return libfiles::decode(r, sink, options);
	}

	libfiles::decode_result decode_with(const bytes& file, libfiles::image_sink& sink,
	                                    const libfiles::decode_options& options)
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::decode(r, sink, options);
	}

	libfiles::decode_options keeping_hdr()
	{
		libfiles::decode_options options;
		options.keep_hdr = true;
		return options;
	}

	// An HLG picture in Display P3, with the light levels and display a HEIF and a PNG keep.
	libfiles::hdr_description hlg_p3()
	{
		libfiles::hdr_description hdr;
		hdr.transfer = libfiles::hdr_transfer::hlg;
		hdr.colour_primaries = 12;
		hdr.primaries = *libfiles::chromaticities_of(12);
		hdr.max_cll = 1200;
		hdr.max_fall = 300;
		hdr.mastering = libfiles::hdr_description::mastering_display{*libfiles::chromaticities_of(12), 1000.0, 0.005};
		return hdr;
	}

	// The light level and mastering display a description holds, as a HEIF's and a PNG's boxes and chunks give them
	// back - a HEIF's chromaticities to 0.00002 and its luminance to 0.0001.
	bool same_light(const libfiles::hdr_description& a, const libfiles::hdr_description& b)
	{
		if (a.max_cll != b.max_cll || a.max_fall != b.max_fall || a.mastering.has_value() != b.mastering.has_value())
			return false;
		if (!a.mastering) return true;
		const auto& m = *a.mastering;
		const auto& n = *b.mastering;
		return std::abs(m.max_luminance - n.max_luminance) < 1e-3 &&
		       std::abs(m.min_luminance - n.min_luminance) < 1e-3 &&
		       std::abs(m.primaries.red_x - n.primaries.red_x) < 1e-4 &&
		       std::abs(m.primaries.green_y - n.primaries.green_y) < 1e-4 &&
		       std::abs(m.primaries.blue_x - n.primaries.blue_x) < 1e-4 &&
		       std::abs(m.primaries.white_y - n.primaries.white_y) < 1e-4;
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
			                    static_cast<std::size_t>(webp[i + 6]) << 16 |
			                    static_cast<std::size_t>(webp[i + 7]) << 24;
			result.emplace_back(webp.begin() + static_cast<std::ptrdiff_t>(i),
			                    webp.begin() + static_cast<std::ptrdiff_t>(i + 4));
			i += 8 + length + (length & 1);
		}

		return result;
	}

	using test_build::rgb_icc_profile;

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
			CHECK(starts_with(payload, std::string_view("Photoshop 3.0\0"
			                                            "8BIM\x04\x04\0\0\0\0\0\x08",
			                                            26)));
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

TEST("Should embed the blocks a scan read as they are, and keep them through a clone")
{
	libfiles::metadata_blocks written;
	written.exif = {'I', 'I', 0x2A, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	written.xmp = {'<', 'x', '/', '>'};
	written.iptc = {0x1C, 0x02, 0x05, 0x00, 0x03, 'c', 'a', 't'};
	written.icc.assign(300, 0x42);

	const auto jpeg = encode_jpeg(gradient(16, 16), 1, written);
	libfiles::reader r(jpeg);
	auto scanned = libfiles::scan(r);
	CHECK(scanned.success);
	CHECK(scanned.metadata.exif == written.exif);
	CHECK(scanned.metadata.xmp == written.xmp);
	CHECK(scanned.metadata.iptc == written.iptc);
	CHECK(scanned.metadata.icc == written.icc);

	// The clone owns copies; the original moves out whole.
	const auto copy = scanned.metadata.clone();
	CHECK(copy.icc == written.icc);
	CHECK(copy.icc.data() != scanned.metadata.icc.data());
	const auto moved = std::move(scanned.metadata);
	CHECK(moved.xmp == written.xmp);
	CHECK(copy.xmp == written.xmp);
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

TEST("Should refuse an HDR picture's signal as a JPEG, and stay usable")
{
	const auto p = gradient(8, 8);
	libfiles::embedded_metadata hdr;
	hdr.hdr = libfiles::hdr_description{};

	libfiles::jpeg_encoder encoder;
	CHECK_THROWS(encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, hdr, {}));
	CHECK(!encoder.encode(p.width, p.height, p.pixels.data(), p.stride, 1, {}, {}).empty());
}

TEST("Should drop a Photoshop resource too large for any marker, and note it")
{
	// One record of 70000 bytes, and one small one the JPEG keeps.
	bytes resources = {'8', 'B', 'I', 'M', 0x04, 0x0C, 0, 0, 0x00, 0x01, 0x11, 0x70};
	resources.resize(resources.size() + 70000, 0x33);
	const bytes kept = {'8', 'B', 'I', 'M', 0x03, 0xED, 0, 0, 0, 0, 0, 2, 0xAB, 0xCD};
	resources.insert(resources.end(), kept.begin(), kept.end());

	libfiles::embedded_metadata metadata;
	metadata.photoshop = resources;

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
	metadata.photoshop = resources;

	const auto segments = jpeg_segments(encode_jpeg(gradient(16, 16), 1, metadata));
	CHECK(std::ranges::none_of(segments, [](const auto& s) { return s.first == 0xED; }));
}

TEST("Should write a last Photoshop resource that lacks its pad byte with it")
{
	const bytes resources = {'8', 'B', 'I', 'M', 0x03, 0xED, 0, 0, 0, 0, 0, 3, 'r', 'e', 's'};

	libfiles::embedded_metadata metadata;
	metadata.photoshop = resources;

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

TEST("Should encode sixteen bits a channel as a 16-bit PNG, losslessly")
{
	for (const auto format : {libfiles::pixel_format::rgbx16, libfiles::pixel_format::rgba16})
	{
		const auto p = wide_gradient(20, 10, format);
		const auto png = libfiles::encode_png(p.view(), 1, {});
		const auto chunks = png_chunks(png.data);

		CHECK(!chunks.empty());
		CHECK_EQ(chunks.front().second[8], std::uint8_t{16});
		CHECK_EQ(chunks.front().second[9],
		         format == libfiles::pixel_format::rgbx16 ? std::uint8_t{2} : std::uint8_t{6});

		wide_sink sink;
		CHECK(decode_wide(png.data, sink).success);
		CHECK(sink.told.format == format);
		CHECK(sink.samples == p.samples);
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

TEST("Should write an HDR picture's cICP, cLLI and mDCV in a PNG in place of its profile, and read them back")
{
	const auto wide = wide_gradient(6, 4, libfiles::pixel_format::rgbx16);
	const auto icc = rgb_icc_profile();
	libfiles::embedded_metadata metadata;
	metadata.icc = icc;
	metadata.hdr = hlg_p3();
	const auto png = libfiles::encode_png(wide.view(), 1, metadata).data;

	std::string types;
	bytes cicp;
	for (const auto& [type, data] : png_chunks(png))
	{
		types += type + " ";
		if (type == "cICP") cicp = data;
	}
	CHECK(types.find("iCCP") == std::string::npos && types.find("cLLI") != std::string::npos &&
	      types.find("mDCV") != std::string::npos);
	CHECK(cicp == bytes({12, 18, 0, 1}));

	// The listing says what the chunks say, as a HEIF's and a JPEG XL's do.
	CHECK_STR(listed(png, "Colour"), "Display P3 primaries, HLG transfer (HDR), full range");
	CHECK_STR(listed(png, "Content light level"), "brightest pixel 1200 nits, brightest frame average 300 nits");
	CHECK_STR(listed(png, "Mastering display"), "0.005 nits to 1000 nits");
	CHECK_STR(listed(png, "Colour profile"), "cICP (coded primaries and transfer)");
	CHECK_STR(listed(png, "Bit depth"), "16 bits a sample");
	CHECK_STR(listed(png, "Colour type"), "RGB");
	CHECK_STR(listed(png, "cICP"), "4 bytes");

	wide_sink signal;
	const auto kept = decode_with(png, signal, keeping_hdr());
	CHECK(kept.success && kept.frame.hdr.has_value());
	CHECK(signal.samples == wide.samples);
	if (kept.frame.hdr)
	{
		CHECK(kept.frame.hdr->transfer == libfiles::hdr_transfer::hlg && kept.frame.hdr->colour_primaries == 12);
		CHECK(same_light(*kept.frame.hdr, hlg_p3()));
	}
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

TEST("Should keep the colour under a WebP's transparent pixels when asked to, at the effort asked")
{
	auto p = gradient(16, 8, libfiles::pixel_format::bgra8);
	for (std::uint32_t y = 0; y < p.height; ++y) p.pixels[y * p.stride + 3] = 0;

	libfiles::webp_options options;
	options.lossless = true;
	options.exact = true;
	options.method = 4;
	options.quality = 100;
	const auto webp = libfiles::encode_webp(p.view(), 1, {}, options);
	CHECK(!webp.data.empty());

	capture_sink sink;
	CHECK(decode_bytes(webp.data, sink).success);

	for (std::uint32_t y = 0; y < p.height; ++y)
	{
		CHECK(std::equal(sink.at(0, y), sink.at(0, y) + 4, p.pixels.data() + y * p.stride));
	}
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

TEST("Should refuse an HDR picture's signal as a WebP")
{
	libfiles::embedded_metadata hdr;
	hdr.hdr = libfiles::hdr_description{};
	const auto refused = libfiles::encode_webp(gradient(8, 8).view(), 1, hdr, {});
	CHECK(refused.data.empty() && !refused.notes.empty());
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

#ifdef LIBFILES_JXL

namespace
{
	// The profile libjxl itself describes sRGB with, read from a JPEG XL encoded without one.
	bytes srgb_icc_from_libjxl()
	{
		const auto plain = libfiles::encode_jxl(gradient(8, 8).view(), 1, {}, {}).data;
		bytes icc;
		auto* const decoder = JxlDecoderCreate(nullptr);

		if (decoder && JxlDecoderSubscribeEvents(decoder, JXL_DEC_COLOR_ENCODING) == JXL_DEC_SUCCESS &&
		    JxlDecoderSetInput(decoder, plain.data(), plain.size()) == JXL_DEC_SUCCESS)
		{
			JxlDecoderCloseInput(decoder);
			std::size_t size = 0;

			if (JxlDecoderProcessInput(decoder) == JXL_DEC_COLOR_ENCODING &&
			    JxlDecoderGetICCProfileSize(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &size) == JXL_DEC_SUCCESS)
			{
				icc.resize(size);
				if (JxlDecoderGetColorAsICCProfile(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, icc.data(),
				                                   icc.size()) != JXL_DEC_SUCCESS)
				{
					icc.clear();
				}
			}
		}

		JxlDecoderDestroy(decoder);
		return icc;
	}
}

TEST("Should encode a JPEG XL losslessly, alpha and all")
{
	const auto p = gradient(24, 12, libfiles::pixel_format::bgra8);
	libfiles::jxl_options options;
	options.lossless = true;
	const auto jxl = libfiles::encode_jxl(p.view(), 1, {}, options);

	CHECK(!jxl.data.empty());
	CHECK(jxl.notes.empty());

	capture_sink sink;
	CHECK(decode_bytes(jxl.data, sink).success);
	CHECK(sink.told.format == libfiles::pixel_format::bgra8);
	CHECK_EQ(largest_difference(p, sink), 0);

	auto alpha_differs = 0;
	for (std::uint32_t y = 0; y < p.height; ++y)
		for (std::uint32_t x = 0; x < p.width; ++x)
			alpha_differs += p.pixels[y * p.stride + std::size_t{4} * x + 3] != sink.at(x, y)[3];
	CHECK_EQ(alpha_differs, 0);
}

TEST("Should encode a JPEG XL of a single pixel, and of an odd size, row padding and all")
{
	for (const auto& [width, height] : {std::pair{1u, 1u}, std::pair{7u, 3u}})
	{
		const auto p = gradient(width, height, libfiles::pixel_format::bgra8);
		libfiles::jxl_options options;
		options.lossless = true;
		const auto jxl = libfiles::encode_jxl(p.view(), 1, {}, options);
		CHECK(!jxl.data.empty());

		capture_sink sink;
		CHECK(decode_bytes(jxl.data, sink).success);
		CHECK_EQ(sink.told.width, p.width);
		CHECK_EQ(sink.told.height, p.height);
		CHECK_EQ(largest_difference(p, sink), 0);
	}
}

TEST("Should encode a JPEG XL lossily, red and blue where they were, smaller at a lower quality")
{
	const auto p = gradient(128, 96);
	libfiles::jxl_options fine_options;
	fine_options.quality = 95;
	libfiles::jxl_options coarse_options;
	coarse_options.quality = 30;

	const auto fine = libfiles::encode_jxl(p.view(), 1, {}, fine_options);
	const auto coarse = libfiles::encode_jxl(p.view(), 1, {}, coarse_options);
	CHECK(fine.notes.empty());

	capture_sink sink;
	CHECK(decode_bytes(fine.data, sink).success);
	CHECK(sink.told.format == libfiles::pixel_format::bgrx8);
	// Red and blue swapped would differ by around fifty on average.
	CHECK(mean_difference(p, sink) < 4.0);
	CHECK(!coarse.data.empty());
	CHECK(coarse.data.size() < fine.data.size());

	// Quality 100 stays lossy, where libjxl alone would read its distance of 0 as lossless and refuse
	// it for an image whose profile is not kept.
	libfiles::jxl_options top;
	top.quality = 100;
	CHECK(!libfiles::encode_jxl(p.view(), 1, {}, top).data.empty());
}

TEST("Should box a JPEG XL's EXIF and XMP, the EXIF turned as the codestream is")
{
	const auto tiff = tiff_with_orientation(1);
	const bytes xmp = {'<', 'x', ':', 'x', 'm', 'p', 'm', 'e', 't', 'a', '/', '>'};
	libfiles::embedded_metadata metadata;
	metadata.exif = tiff;
	metadata.xmp = xmp;

	const auto jxl = libfiles::encode_jxl(gradient(16, 16).view(), 6, metadata, {});
	CHECK(!jxl.data.empty());
	CHECK(starts_with(jxl.data, std::string_view("\0\0\0\x0CJXL \r\n\x87\n", 12)));

	const auto scanned = scan_bytes(jxl.data);
	CHECK(scanned.success);
	CHECK(scanned.orientation == 6);
	CHECK(scanned.metadata.exif == tiff_with_orientation(6));
	CHECK(scanned.metadata.xmp == xmp);

	capture_sink sink;
	CHECK(decode_bytes(jxl.data, sink).success);
	CHECK_EQ(sink.told.orientation, std::uint16_t{6});

	// The signature a JPEG's APP1 begins with is not part of the box.
	bytes signed_exif = {'E', 'x', 'i', 'f', 0, 0};
	signed_exif.insert(signed_exif.end(), tiff.begin(), tiff.end());
	metadata.exif = signed_exif;
	CHECK(scan_bytes(libfiles::encode_jxl(gradient(16, 16).view(), 3, metadata, {}).data).metadata.exif ==
	      tiff_with_orientation(3));

	// Nothing to box: a bare codestream.
	const auto bare = libfiles::encode_jxl(gradient(16, 16).view(), 1, {}, {}).data;
	CHECK(bare.size() > 2 && bare[0] == 0xFF && bare[1] == 0x0A);
}

TEST("Should keep a JPEG XL's ICC profile, and write sRGB in place of one libjxl refuses")
{
	const auto srgb = srgb_icc_from_libjxl();
	CHECK(!srgb.empty());

	for (const auto lossless : {false, true})
	{
		libfiles::jxl_options options;
		options.lossless = lossless;
		libfiles::embedded_metadata metadata;
		metadata.icc = srgb;

		const auto kept = libfiles::encode_jxl(gradient(16, 16).view(), 1, metadata, options);
		CHECK(!kept.data.empty());
		CHECK(kept.notes.empty());

		// A CMYK profile cannot describe RGB pixels, and these bytes are no profile at all.
		for (const auto& refused : {rgb_icc_profile(), bytes(64, 0x42)})
		{
			auto cmyk = refused;
			if (cmyk.size() >= 20) std::memcpy(cmyk.data() + 16, "CMYK", 4);
			metadata.icc = cmyk;

			const auto written = libfiles::encode_jxl(gradient(16, 16).view(), 1, metadata, options);
			CHECK(!written.data.empty());
			CHECK(!written.notes.empty());

			capture_sink sink;
			CHECK(decode_bytes(written.data, sink).success);
		}
	}
}

TEST("Should refuse a JPEG XL of a pixel format it does not encode, or of no pixels")
{
	const auto planes = nv12(16, 16, 128, 128);
	const auto refused = libfiles::encode_jxl(planes.view(), 1, {}, {});
	CHECK(refused.data.empty());
	CHECK(!refused.notes.empty());

	auto empty = gradient(8, 8);
	auto view = empty.view();
	view.width = 0;
	CHECK(libfiles::encode_jxl(view, 1, {}, {}).data.empty());
}

TEST("Should encode sixteen bits a channel as a 16-bit JPEG XL, losslessly")
{
	libfiles::jxl_options lossless;
	lossless.lossless = true;

	for (const auto format : {libfiles::pixel_format::rgbx16, libfiles::pixel_format::rgba16})
	{
		const auto p = wide_gradient(20, 10, format);
		const auto written = libfiles::encode_jxl(p.view(), 1, {}, lossless);
		CHECK(!written.data.empty());

		wide_sink sink;
		CHECK(decode_wide(written.data, sink).success);
		CHECK(sink.told.format == format);
		CHECK(sink.samples == p.samples);
	}
}

TEST("Should write an HDR picture's colour encoding and peak in a JPEG XL, and read them back")
{
	libfiles::jxl_options lossless;
	lossless.lossless = true;
	const auto p = wide_gradient(20, 10, libfiles::pixel_format::rgbx16);

	libfiles::hdr_description pq;
	pq.colour_primaries = 9;
	pq.primaries = *libfiles::chromaticities_of(9);
	pq.max_cll = 1000;
	const auto icc = rgb_icc_profile();
	libfiles::embedded_metadata metadata;
	metadata.icc = icc;
	metadata.hdr = pq;
	const auto written = libfiles::encode_jxl(p.view(), 1, metadata, lossless);
	CHECK(!written.data.empty() && written.notes.empty());
	CHECK_STR(listed(written.data, "Colour"), "BT.2020 primaries, PQ transfer (HDR)");
	CHECK_STR(listed(written.data, "Intensity target"), "1000 nits");

	wide_sink signal;
	const auto kept = decode_with(written.data, signal, keeping_hdr());
	CHECK(kept.success && !kept.frame.tone_mapped && kept.frame.hdr.has_value());
	CHECK(signal.samples == p.samples);
	if (kept.frame.hdr)
	{
		const auto& hdr = *kept.frame.hdr;
		CHECK(hdr.transfer == libfiles::hdr_transfer::pq && hdr.colour_primaries == 9 &&
		      hdr.primaries == *libfiles::chromaticities_of(9));
		// Its peak is written as the intensity target, which the mapping takes it from.
		CHECK(hdr.intensity_target == 1000.0 && hdr.peak_nits() == 1000.0);
	}

	// HLG in Display P3, and primaries of its own.
	auto custom = hlg_p3();
	custom.colour_primaries = 2;
	custom.primaries = {0.69, 0.30, 0.20, 0.75, 0.14, 0.05, 0.3127, 0.3290};
	metadata.hdr = custom;
	wide_sink own;
	const auto other = decode_with(libfiles::encode_jxl(p.view(), 1, metadata, lossless).data, own, keeping_hdr());
	CHECK(other.success && other.frame.hdr && other.frame.hdr->transfer == libfiles::hdr_transfer::hlg);
	CHECK(other.frame.hdr && other.frame.hdr->colour_primaries == 2 &&
	      std::abs(other.frame.hdr->primaries.green_y - 0.75) < 1e-6);
}

#ifdef LIBFILES_JPEG

namespace
{
	// The four-character types of a JPEG XL container's boxes, in order, each followed by a space.
	std::string box_types(const bytes& file)
	{
		std::string types;
		std::size_t pos = 12;

		while (pos + 8 <= file.size())
		{
			std::uint64_t size = std::uint64_t{file[pos]} << 24 | std::uint64_t{file[pos + 1]} << 16 |
			                     std::uint64_t{file[pos + 2]} << 8 | file[pos + 3];
			types.append(reinterpret_cast<const char*>(file.data() + pos + 4), 4).push_back(' ');
			if (size == 1 && pos + 16 <= file.size())
				for (std::size_t i = 0; i < 8; ++i) size = (i ? size << 8 : 0) | file[pos + 8 + i];
			if (size < 8) break;
			pos += static_cast<std::size_t>(size);
		}

		return types;
	}
}

TEST("Should repack a JPEG as a JPEG XL that gives it back byte for byte, its EXIF and XMP in boxes of their own")
{
	const auto tiff = tiff_with_orientation(6);
	const std::string_view packet = "<x:xmpmeta xmlns:x='adobe:ns:meta/'/>";
	const bytes xmp(packet.begin(), packet.end());
	const bytes iptc = {0x1C, 0x02, 0x05, 0x00, 0x03, 'c', 'a', 't'};
	libfiles::embedded_metadata metadata;
	metadata.exif = tiff;
	metadata.xmp = xmp;
	metadata.iptc = iptc;

	const auto picture = gradient(64, 48);
	const auto jpeg = encode_jpeg(picture, 6, metadata);
	const auto repacked = libfiles::transcode_jpeg_to_jxl(jpeg);
	CHECK(!repacked.data.empty());
	CHECK(repacked.notes.empty());
	CHECK(libfiles::reconstruct_jpeg(repacked.data) == jpeg);

	// Plain boxes, which a reader without Brotli finds, and the IPTC kept only for the JPEG given back.
	const auto types = box_types(repacked.data);
	CHECK(types.find("jbrd ") != std::string::npos);
	CHECK(types.find("Exif ") != std::string::npos);
	CHECK(types.find("xml ") != std::string::npos);
	CHECK(types.find("brob ") == std::string::npos);

	const auto scanned = scan_bytes(repacked.data);
	CHECK(scanned.success);
	CHECK(scanned.orientation == 6);
	CHECK(scanned.metadata.exif == tiff);
	CHECK(scanned.metadata.xmp == xmp);
	CHECK(scanned.metadata.iptc.empty());

	// Its picture is the JPEG's, decoded by libjxl rather than libjpeg-turbo.
	capture_sink from_jpeg;
	capture_sink from_jxl;
	CHECK(decode_bytes(jpeg, from_jpeg).success);
	CHECK(decode_bytes(repacked.data, from_jxl).success);
	CHECK_EQ(from_jxl.told.width, picture.width);
	CHECK_EQ(from_jxl.told.height, picture.height);
	CHECK(mean_difference(picture, from_jxl) < 3.0);
}

TEST("Should repack nothing libjxl cannot keep, and give back no JPEG from a JPEG XL made from pixels")
{
	const auto jpeg = encode_jpeg(gradient(64, 48));
	const bytes cut(jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(jpeg.size() / 2));

	for (const auto& refused : {bytes{1, 2, 3}, cut})
	{
		const auto repacked = libfiles::transcode_jpeg_to_jxl(refused);
		CHECK(repacked.data.empty());
		CHECK(!repacked.notes.empty());
	}

	const auto repacked = libfiles::transcode_jpeg_to_jxl(jpeg).data;
	CHECK(!repacked.empty());
	const bytes repacked_cut(repacked.begin(), repacked.begin() + static_cast<std::ptrdiff_t>(repacked.size() / 2));

	for (const auto& file : {libfiles::encode_jxl(gradient(16, 16).view(), 1, {}, {}).data, repacked_cut, jpeg})
	{
		CHECK(libfiles::reconstruct_jpeg(file).empty());
	}
}

// A JPEG XL written again as itself keeps how it was compressed: losslessly where libjxl coded its colour as the file
// describes it - which a JPEG repacked as one is too, but holding that JPEG's loss, so it is lossy.
TEST("Should read whether a JPEG XL was compressed losslessly")
{
	const auto picture = gradient(32, 24);
	libfiles::jxl_options exact;
	exact.lossless = true;

	const auto lossless = libfiles::read_jxl_compression(libfiles::encode_jxl(picture.view(), 1, {}, exact).data);
	CHECK(lossless.has_value() && lossless->lossless);
	const auto lossy = libfiles::read_jxl_compression(libfiles::encode_jxl(picture.view(), 1, {}, {}).data);
	CHECK(lossy.has_value() && !lossy->lossless);

	const auto repacked = libfiles::transcode_jpeg_to_jxl(encode_jpeg(gradient(64, 48))).data;
	CHECK(!repacked.empty());
	const auto from_jpeg = libfiles::read_jxl_compression(repacked);
	CHECK(from_jpeg.has_value() && !from_jpeg->lossless);

	CHECK(!libfiles::read_jxl_compression(bytes{1, 2, 3}).has_value());
}

#endif

#endif

#ifdef LIBFILES_HEIF

TEST("Should encode an AVIF lossily, red and blue where they were, smaller at a lower quality")
{
	const auto p = gradient(64, 48);
	libfiles::avif_options fine_options;
	fine_options.quality = 90;
	libfiles::avif_options coarse_options;
	coarse_options.quality = 20;

	const auto fine = libfiles::encode_avif(p.view(), 1, {}, fine_options);
	const auto coarse = libfiles::encode_avif(p.view(), 1, {}, coarse_options);
	CHECK(fine.notes.empty());
	CHECK(!fine.data.empty() && !coarse.data.empty());
	CHECK(coarse.data.size() < fine.data.size());

	const auto scanned = scan_bytes(fine.data);
	CHECK(scanned.success);
	CHECK_EQ(scanned.width, p.width);
	CHECK_EQ(scanned.height, p.height);

	capture_sink sink;
	CHECK(decode_bytes(fine.data, sink).success);
	CHECK(sink.told.format == libfiles::pixel_format::bgrx8);
	// Red and blue swapped would differ by around fifty on average.
	CHECK(mean_difference(p, sink) < 4.0);
}

TEST("Should encode an AVIF losslessly, alpha and all, and ten bits a sample from sixteen")
{
	const auto p = gradient(24, 12, libfiles::pixel_format::bgra8);
	libfiles::avif_options options;
	options.lossless = true;
	const auto avif = libfiles::encode_avif(p.view(), 1, {}, options);
	CHECK(!avif.data.empty());
	CHECK(avif.notes.empty());

	capture_sink sink;
	CHECK(decode_bytes(avif.data, sink).success);
	CHECK(sink.told.format == libfiles::pixel_format::bgra8);
	CHECK_EQ(largest_difference(p, sink), 0);

	auto alpha_differs = 0;
	for (std::uint32_t y = 0; y < p.height; ++y)
		for (std::uint32_t x = 0; x < p.width; ++x)
			alpha_differs += p.pixels[y * p.stride + std::size_t{4} * x + 3] != sink.at(x, y)[3];
	CHECK_EQ(alpha_differs, 0);

	// Sixteen bits keep their ten most significant, which is what AVIF's main profile holds.
	const auto wide = wide_gradient(20, 10, libfiles::pixel_format::rgbx16);
	const auto deep = libfiles::encode_avif(wide.view(), 1, {}, options);
	CHECK(!deep.data.empty());

	wide_sink decoded;
	CHECK(decode_wide(deep.data, decoded).success);
	CHECK(decoded.told.format == libfiles::pixel_format::rgbx16);

	auto differing = 0;
	for (std::size_t i = 0; i < wide.samples.size(); ++i)
	{
		if (i % 4 == 3) continue;
		const auto ten = (decoded.samples[i] * 1023u + 32767u) / 65535u;
		differing += ten != (static_cast<unsigned>(wide.samples[i]) >> 6u);
	}
	CHECK_EQ(differing, 0);
}

TEST("Should give an AVIF its EXIF and XMP as items, turned by its image's rotation as the EXIF says")
{
	const auto tiff = tiff_with_orientation(1);
	const bytes xmp = {'<', 'x', ':', 'x', 'm', 'p', 'm', 'e', 't', 'a', '/', '>'};
	libfiles::embedded_metadata metadata;
	metadata.exif = tiff;
	metadata.xmp = xmp;

	const auto p = gradient(32, 16);
	const auto avif = libfiles::encode_avif(p.view(), 6, metadata, {});
	CHECK(!avif.data.empty());
	CHECK(avif.notes.empty());

	// libheif turns it by the rotation written, and the EXIF it holds describes that turn.
	const auto scanned = scan_bytes(avif.data);
	CHECK(scanned.success);
	CHECK(scanned.metadata.exif == tiff_with_orientation(6));
	CHECK(scanned.metadata.xmp == xmp);

	capture_sink sink;
	CHECK(decode_bytes(avif.data, sink).success);
	CHECK_EQ(sink.told.width, p.height);
	CHECK_EQ(sink.told.height, p.width);
}

TEST("Should refuse to encode an AVIF from nothing, or from pixels it does not take")
{
	CHECK(libfiles::encode_avif({}, 1, {}, {}).data.empty());
	const auto p = nv12(16, 16, 128, 128);
	const auto refused = libfiles::encode_avif(p.view(), 1, {}, {});
	CHECK(refused.data.empty());
	CHECK(!refused.notes.empty());
}

namespace
{
	// Each colour sample's twelve most significant bits differ from what a decode of twelve widened, how many times.
	int differing_from_twelve_bits(const wide_picture& written, const wide_sink& decoded)
	{
		auto differing = 0;
		for (std::size_t i = 0; i < written.samples.size(); ++i)
		{
			if (i % 4 == 3) continue;
			const auto twelve = (decoded.samples[i] * 4095u + 32767u) / 65535u;
			differing += twelve != (static_cast<unsigned>(written.samples[i]) >> 4u);
		}
		return differing;
	}
}

TEST("Should write an AVIF at twelve bits a sample, and an HDR picture's description in place of its profile")
{
	const auto wide = wide_gradient(20, 10, libfiles::pixel_format::rgbx16);
	libfiles::avif_options options;
	options.lossless = true;
	options.bit_depth = 12;

	const auto plain = libfiles::encode_avif(wide.view(), 1, {}, options);
	CHECK(!plain.data.empty() && plain.notes.empty());
	CHECK_EQ(scan_bytes(plain.data).picture.bit_depth, 12);
	wide_sink twelve;
	CHECK(decode_wide(plain.data, twelve).success);
	CHECK_EQ(differing_from_twelve_bits(wide, twelve), 0);

	const auto icc = rgb_icc_profile();
	libfiles::embedded_metadata metadata;
	metadata.icc = icc;
	metadata.hdr = hlg_p3();
	const auto avif = libfiles::encode_avif(wide.view(), 1, metadata, options);
	CHECK(!avif.data.empty() && avif.notes.empty());
	CHECK(scan_bytes(avif.data).metadata.icc.empty());
	// The scan says what the decode finds: the nclx names HLG.
	CHECK(scan_bytes(avif.data).hdr && !scan_bytes(plain.data).hdr);
	// And the listing names it, with the light levels.
	CHECK_STR(listed(avif.data, "Colour"),
	          "Display P3 primaries, HLG transfer (HDR), identity matrix (RGB), full range");
	CHECK_STR(listed(avif.data, "Content light level"), "brightest pixel 1200 nits, brightest frame average 300 nits");
	CHECK_STR(listed(avif.data, "Mastering display"), "0.005 nits to 1000 nits");
	CHECK_STR(listed(plain.data, "Content light level"), "(none)");

	libfiles::decode_options keep;
	keep.keep_hdr = true;
	wide_sink signal;
	const auto kept = decode_with(avif.data, signal, keep);
	CHECK(kept.success && !kept.frame.tone_mapped && kept.frame.hdr.has_value());
	CHECK(signal.told.format == libfiles::pixel_format::rgbx16);
	CHECK_EQ(differing_from_twelve_bits(wide, signal), 0);

	if (kept.frame.hdr)
	{
		const auto& hdr = *kept.frame.hdr;
		CHECK(hdr.transfer == libfiles::hdr_transfer::hlg);
		CHECK_EQ(hdr.colour_primaries, 12);
		CHECK(hdr.max_cll == 1200.0 && hdr.max_fall == 300.0);
		CHECK(hdr.mastering && std::abs(hdr.mastering->max_luminance - 1000.0) < 0.01 &&
		      std::abs(hdr.mastering->min_luminance - 0.005) < 1e-3 &&
		      std::abs(hdr.mastering->primaries.red_x - 0.68) < 1e-4 &&
		      std::abs(hdr.mastering->primaries.green_y - 0.69) < 1e-4);
	}

	// Shown, as before, it is mapped to SDR.
	capture_sink shown;
	CHECK(decode_bytes(avif.data, shown).frame.tone_mapped);
}

namespace
{
	// A grey gain map: its value in every channel.
	picture grey_map(const std::uint32_t width, const std::uint32_t height)
	{
		picture p{width, height, std::size_t{4} * width, libfiles::pixel_format::bgrx8, {}};
		p.pixels.resize(p.stride * height);

		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
			{
				auto* const px = p.pixels.data() + y * p.stride + std::size_t{4} * x;
				const auto v = static_cast<std::uint8_t>(std::min(255u, x * 15 + y * 9));
				px[0] = px[1] = px[2] = v;
				px[3] = 255;
			}

		return p;
	}

	libfiles::gain_map_metadata two_stops()
	{
		libfiles::gain_map_metadata m;
		m.alternate_headroom = 2.0;
		m.gain_min.fill(0.0);
		m.gain_max.fill(2.0);
		return m;
	}

	// The mean difference between a gain map and what decoded of it, turned as an EXIF orientation of 1 or 6 turns it.
	double map_difference(const picture& map, const capture_sink& decoded, const int orientation)
	{
		double total = 0;
		const auto width = orientation == 6 ? map.height : map.width;
		const auto height = orientation == 6 ? map.width : map.height;

		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
			{
				const auto sx = orientation == 6 ? y : x;
				const auto sy = orientation == 6 ? map.height - 1 - x : y;
				total += std::abs(map.pixels[sy * map.stride + std::size_t{4} * sx + 1] - decoded.at(x, y)[1]);
			}

		return total / (static_cast<double>(width) * height);
	}
}

TEST("Should write an AVIF's gain map as libavif does - hidden, and applied by a tone map item grouped with the "
     "picture - and decode it back turned as the picture is")
{
	const auto p = gradient(32, 16);
	const auto map = grey_map(16, 8);
	libfiles::avif_options options;
	options.gain_map = libfiles::gain_map_image{map.view(), two_stops()};

	const auto avif = libfiles::encode_avif(p.view(), 6, {}, options);
	CHECK(!avif.data.empty() && avif.notes.empty());
	// The listing names it, its size as it is shown, turned with the picture.
	CHECK_STR(listed(avif.data, "Gain map"), "ISO 21496-1, 8 x 16, up to 4 times SDR white (2.00 stops)");
	// An index scan knows it is there from the tone map item alone.
	CHECK(scan_bytes(avif.data).gain_map);

	// One picture to a reader that does not apply gain maps: the gain map is hidden, and the tone map item and the
	// primary are alternatives.
	auto* const ctx = heif_context_alloc();
	CHECK(heif_context_read_from_memory_without_copy(ctx, avif.data.data(), avif.data.size(), nullptr).code ==
	      heif_error_Ok);
	CHECK_EQ(heif_context_get_number_of_top_level_images(ctx), 1);
	CHECK(heif_has_compatible_brand(avif.data.data(), static_cast<int>(avif.data.size()), "tmap") == 1);
	heif_item_id primary = 0;
	heif_context_get_primary_image_ID(ctx, &primary);
	auto groups = 0;
	auto* const altr = heif_context_get_entity_groups(ctx, heif_fourcc('a', 'l', 't', 'r'), 0, &groups);
	CHECK_EQ(groups, 1);
	if (groups == 1)
	{
		CHECK(altr[0].num_entities == 2 && altr[0].entities[1] == primary &&
		      heif_item_get_item_type(ctx, altr[0].entities[0]) == heif_fourcc('t', 'm', 'a', 'p'));
	}
	heif_entity_groups_release(altr, groups);
	heif_context_free(ctx);

	capture_sink shown;
	const auto picture_decoded = decode_bytes(avif.data, shown);
	CHECK(picture_decoded.success && shown.told.width == 16 && shown.told.height == 32);
	CHECK(!shown.told.gain_map);

	libfiles::decode_options options_for_map;
	options_for_map.heif_gain_map = true;
	capture_sink gain;
	const auto decoded = decode_with(avif.data, gain, options_for_map);
	CHECK(decoded.success);
	CHECK(gain.told.format == libfiles::pixel_format::bgrx8);
	CHECK(gain.told.gain_map && *gain.told.gain_map == two_stops());
	CHECK(gain.told.width == 8 && gain.told.height == 16);
	CHECK(map_difference(map, gain, 6) < 3.0);

	// A picture without one says so, and allocates nothing.
	capture_sink none;
	const auto missing = decode_with(libfiles::encode_avif(p.view(), 1, {}, {}).data, none, options_for_map);
	CHECK(!missing.success && !missing.error.empty() && none.memory.empty());

	// An HDR picture's signal needs none, and is written without it.
	const auto wide = wide_gradient(32, 16, libfiles::pixel_format::rgbx16);
	libfiles::embedded_metadata hdr;
	hdr.hdr = hlg_p3();
	const auto signal = libfiles::encode_avif(wide.view(), 1, hdr, options);
	CHECK(!signal.data.empty() && !signal.notes.empty());
	CHECK(!heif_has_compatible_brand(signal.data.data(), static_cast<int>(signal.data.size()), "tmap"));
	CHECK(!scan_bytes(signal.data).gain_map);
	CHECK(scan_bytes(signal.data).hdr);
	CHECK_EQ(scan_bytes(signal.data).colour_primaries, 12);
}

namespace
{
	heif_image* grey_heif_image(const int width, const int height, const std::uint8_t value)
	{
		heif_image* image = nullptr;
		heif_image_create(width, height, heif_colorspace_monochrome, heif_chroma_monochrome, &image);
		heif_image_add_plane(image, heif_channel_Y, width, height, 8);
		int stride = 0;
		auto* const plane = heif_image_get_plane(image, heif_channel_Y, &stride);
		for (auto y = 0; y < height; ++y) std::memset(plane + static_cast<std::ptrdiff_t>(stride) * y, value, width);
		return image;
	}

	// A picture with Apple's HDR gain map as an iPhone writes one - an auxiliary image of the type Apple names, its
	// headroom in its own XMP - coded as AV1, which libheif decodes as it would the HEVC Apple codes.
	bytes apple_style(const std::uint8_t map_value, const std::string_view headroom)
	{
		auto* const ctx = heif_context_alloc();
		heif_encoder* encoder = nullptr;
		heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &encoder);
		heif_encoder_set_lossless(encoder, 1);

		auto* const photo = grey_heif_image(32, 16, 0x60);
		auto* const map = grey_heif_image(16, 8, map_value);
		heif_image_handle* primary = nullptr;
		heif_image_handle* aux = nullptr;
		heif_context_encode_image(ctx, photo, encoder, nullptr, &primary);
		heif_context_encode_image(ctx, map, encoder, nullptr, &aux);
		heif_context_set_primary_image(ctx, primary);

		const auto aux_id = heif_image_handle_get_item_id(aux);
		heif_context_add_item_reference(ctx, heif_fourcc('a', 'u', 'x', 'l'), aux_id,
		                                heif_image_handle_get_item_id(primary));
		bytes auxc(4, 0);
		const std::string_view type("urn:com:apple:photo:2020:aux:hdrgainmap\0", 40);
		auxc.insert(auxc.end(), type.begin(), type.end());
		heif_item_add_raw_property(ctx, aux_id, heif_fourcc('a', 'u', 'x', 'C'), nullptr, auxc.data(), auxc.size(), 0,
		                           nullptr);

		const auto packet = std::format(
			R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
			R"(<rdf:Description rdf:about="" xmlns:HDRGainMap="http://ns.apple.com/HDRGainMap/1.0/" )"
			R"(HDRGainMap:HDRGainMapVersion="65536" HDRGainMap:HDRGainMapHeadroom="{}"/></rdf:RDF></x:xmpmeta>)",
			headroom);
		heif_context_add_XMP_metadata(ctx, aux, packet.data(), static_cast<int>(packet.size()));

		bytes out;
		heif_writer writer{};
		writer.writer_api_version = 1;
		writer.write = [](heif_context*, const void* data, const size_t size, void* userdata)
		{
			auto* const file = static_cast<bytes*>(userdata);
			file->insert(file->end(), static_cast<const std::uint8_t*>(data),
			             static_cast<const std::uint8_t*>(data) + size);
			return heif_error{heif_error_Ok, heif_suberror_Unspecified, ""};
		};
		heif_context_write(ctx, &writer, &out);

		heif_image_handle_release(aux);
		heif_image_handle_release(primary);
		heif_image_release(map);
		heif_image_release(photo);
		heif_encoder_release(encoder);
		heif_context_free(ctx);
		return out;
	}
}

TEST("Should decode Apple's HDR gain map with its values said in ISO 21496-1's terms, its headroom from its XMP")
{
	libfiles::decode_options options;
	options.heif_gain_map = true;

	const auto file = apple_style(200, "4");
	CHECK_STR(listed(file, "Gain map"), "Apple, 16 x 8, up to 4 times SDR white (2.00 stops)");
	CHECK(scan_bytes(file).gain_map);
	capture_sink gain;
	const auto decoded = decode_with(file, gain, options);
	CHECK(decoded.success);
	CHECK(gain.told.width == 16 && gain.told.height == 8 && gain.told.format == libfiles::pixel_format::bgrx8);

	std::vector<std::uint8_t> expected{200, 200, 200, 255};
	const auto metadata = libfiles::convert_apple_gain_map({expected.data(), 4}, 1, 1, 4.0);
	CHECK(gain.told.gain_map && *gain.told.gain_map == metadata);
	CHECK(gain.at(3, 2)[0] == expected[0] && gain.at(3, 2)[1] == expected[1] && gain.at(15, 7)[2] == expected[2]);

	// The picture decodes as it would without one.
	capture_sink shown;
	CHECK(decode_bytes(file, shown).success && shown.told.width == 32 && !shown.told.gain_map);

	// A headroom that brightens nothing gives no gain map to apply.
	capture_sink flat;
	CHECK(!decode_with(apple_style(200, "1"), flat, options).success);
}

#endif

TEST("Should carry a JPEG's descriptive segments into its re-encoding, upright and of its new size")
{
	using namespace std::string_view_literals;

	const auto segment = [](bytes& out, const std::uint8_t marker, const std::string_view payload)
	{
		const auto length = payload.size() + 2;
		out.insert(out.end(),
		           {0xFF, marker, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)});
		out.insert(out.end(), payload.begin(), payload.end());
	};

	// Orientation 6, in IFD0 alone; and XMP stating it twice over.
	const auto exif = "Exif\0\0II*\0\x08\0\0\0\x01\0\x12\x01\x03\0\x01\0\0\0\x06\0\0\0\0\0\0\0"sv;
	const auto xmp = "http://ns.adobe.com/xap/1.0/\0<x:xmpmeta tiff:Orientation=\"6\">"
					 "<tiff:Orientation>8</tiff:Orientation></x:xmpmeta>"sv;

	bytes source{0xFF, 0xD8};
	segment(source, 0xE0, "JFIF\0\x01\x02\x01\0\x48\0\x48\x02\x01RGBRGB"sv);
	segment(source, 0xE1, exif);
	segment(source, 0xE1, xmp);
	segment(source, 0xE2, "MPF\0index"sv);
	segment(source, 0xEE, "Adobe\0\x64"sv);
	segment(source, 0xFE, "A comment"sv);
	segment(source, 0xDB, "old tables"sv);
	const bytes source_scan{0xFF, 0xDA, 0x00, 0x02, 0x11, 0x22, 0xFF, 0xD9};
	source.insert(source.end(), source_scan.begin(), source_scan.end());

	bytes encoded{0xFF, 0xD8};
	segment(encoded, 0xE0, "JFIF\0\x01\x01\0\0\x01\0\x01\0\0"sv);
	segment(encoded, 0xDB, "new tables"sv);
	segment(encoded, 0xC0, "frame"sv);
	const bytes scan{0xFF, 0xDA, 0x00, 0x02, 0x33, 0x44, 0xFF, 0xD9};
	encoded.insert(encoded.end(), scan.begin(), scan.end());

	// The header alone, read no further than the scan.
	libfiles::reader r{std::span<const std::uint8_t>(source)};
	const auto header = libfiles::read_jpeg_header(r);
	CHECK(header == bytes(source.begin(), source.end() - static_cast<std::ptrdiff_t>(source_scan.size())));

	// The source's segments - JFIF without its thumbnail, EXIF and XMP upright, the comment - and the encoding's
	// own tables, frame and scan; the old encoding's Adobe segment and tables, and the multi-picture index, gone.
	auto upright = std::string(exif);
	upright[24] = 1;
	auto upright_xmp = std::string(xmp);
	upright_xmp.replace(upright_xmp.find("\"6\""), 3, "\"1\"");
	upright_xmp.replace(upright_xmp.find(">8<"), 3, ">1<");

	bytes expected{0xFF, 0xD8};
	segment(expected, 0xE0, "JFIF\0\x01\x02\x01\0\x48\0\x48\0\0"sv);
	segment(expected, 0xE1, upright);
	segment(expected, 0xE1, upright_xmp);
	segment(expected, 0xFE, "A comment"sv);
	segment(expected, 0xDB, "new tables"sv);
	segment(expected, 0xC0, "frame"sv);
	expected.insert(expected.end(), scan.begin(), scan.end());

	CHECK(libfiles::carry_jpeg_metadata(header, encoded, 640, 480) == expected);
	CHECK(libfiles::carry_jpeg_metadata(source, encoded, 640, 480) == expected);

	// Not JPEGs, a header cut short, and no size: nothing.
	CHECK(libfiles::carry_jpeg_metadata(header, scan, 640, 480).empty());
	CHECK(libfiles::carry_jpeg_metadata(header, encoded, 0, 480).empty());
	const bytes cut(source.begin(), source.begin() + 30);
	libfiles::reader short_reader{std::span<const std::uint8_t>(cut)};
	CHECK(libfiles::read_jpeg_header(short_reader).empty());
	libfiles::reader not_jpeg{std::span<const std::uint8_t>(scan)};
	CHECK(libfiles::read_jpeg_header(not_jpeg).empty());
}

namespace
{
	constexpr std::string_view hdrgm_ns = "http://ns.adobe.com/hdr-gain-map/1.0/";
	constexpr std::string_view container_ns = "http://ns.google.com/photos/1.0/container/";
	constexpr std::string_view mpf_signature{"MPF\0", 4};
	constexpr std::string_view iso_signature{"urn:iso:std:iso:ts:21496:-1\0", 28};

	libfiles::xmp::meta primary_packet(const bytes& jpeg)
	{
		libfiles::reader r{std::span<const std::uint8_t>(jpeg)};
		libfiles::xmp::meta meta;
		meta.parse(libfiles::read_packet(r).packet);
		return meta;
	}

	std::string directory_field(const libfiles::xmp::meta& meta, const int item, const std::string_view field)
	{
		const auto container = libfiles::xmp::register_namespace(container_ns, "Container");
		const auto item_prefix = libfiles::xmp::register_namespace("http://ns.google.com/photos/1.0/container/item/",
		                                                           "Item");
		std::string value;
		meta.get_property(container_ns, std::format("Directory[{}]/{}Item/{}{}", item, container, item_prefix, field),
		                  &value);
		return value;
	}

	std::vector<bytes> app2_segments(const bytes& jpeg, const std::string_view signature)
	{
		std::vector<bytes> found;

		for (const auto& [marker, payload] : test_build::jpeg_segments(jpeg))
		{
			if (marker == 0xE2 && payload.size() >= signature.size() &&
			    std::equal(signature.begin(), signature.end(), payload.begin()))
			{
				found.push_back(payload);
			}
		}

		return found;
	}
}

TEST("Should attach a gain map after a JPEG's primary, signalled as its own metadata asks, and find it there")
{
	const auto gain_map = test_build::jpeg_of({{0xE1, test_build::xmp_app1(test_build::gain_map_description)},
	                                           {0xE2, test_build::iso_gain_map_app2({0, 0, 0, 1, 7, 7, 7})}},
	                                          0x22);

	// A re-encoded primary, still saying what its source said: a gain map of another length, a version segment
	// and an index into the old file.
	const auto primary = test_build::jpeg_of(
		{{0xE0, test_build::lit("JFIF\0\x01\x02\0\0\x01\0\x01\0\0")},
	     {0xE1, test_build::xmp_app1(test_build::ultra_hdr_primary_description(12345))},
	     {0xE2, test_build::iso_gain_map_app2({0, 0, 0, 0})},
	     {0xE2, test_build::lit("MPF\0stale")}});
	CHECK(libfiles::find_jpeg_gain_map(primary).empty());

	const auto attached = libfiles::attach_jpeg_gain_map(primary, gain_map);
	CHECK(!attached.empty());

	const auto found = libfiles::find_jpeg_gain_map(attached);
	CHECK(bytes(found.begin(), found.end()) == gain_map);
	CHECK(found.data() == attached.data() + attached.size() - gain_map.size());

	// One index, by which the primary ends where the gain map begins, and one version segment: the gain map's.
	const auto indexes = app2_segments(attached, mpf_signature);
	CHECK_EQ(indexes.size(), 1u);
	const auto index = libfiles::detail::read_mpf_index(std::span(indexes.front()).subspan(mpf_signature.size()));
	CHECK(index.has_value());
	CHECK_EQ(index->entries.size(), 2u);
	CHECK_EQ(index->entries[0].size, attached.size() - gain_map.size());
	CHECK_EQ(index->entries[1].size, gain_map.size());
	const auto index_at = static_cast<std::size_t>(std::search(attached.begin(), attached.end(),
	                                                           indexes.front().begin(), indexes.front().end()) -
	                                               attached.begin()) +
	                      mpf_signature.size();
	CHECK_EQ(index_at + index->entries[1].offset, attached.size() - gain_map.size());
	const auto versions = app2_segments(attached, iso_signature);
	CHECK_EQ(versions.size(), 1u);
	CHECK(versions.front() == test_build::iso_gain_map_app2({0, 0, 0, 1}));

	// The primary's packet gives the gain map's own length, and its tables, frame and scan are its own.
	const auto meta = primary_packet(attached);
	CHECK(meta.exists(hdrgm_ns, "Version"));
	CHECK_STR(directory_field(meta, 1, "Semantic"), "Primary");
	CHECK_STR(directory_field(meta, 2, "Length"), std::to_string(gain_map.size()));
	const bytes tables{0xFF, 0xDB};
	const auto primary_tail = std::search(primary.begin(), primary.end(), tables.begin(), tables.end());
	CHECK(std::equal(primary_tail, primary.end(),
	                 attached.end() - static_cast<std::ptrdiff_t>(gain_map.size()) - (primary.end() - primary_tail)));

	// Attached again, the new gain map takes the old one's place, with no version segment: it has no ISO metadata.
	const auto other = test_build::jpeg_of({{0xE1, test_build::xmp_app1(test_build::gain_map_description)}}, 0x33);
	const auto again = libfiles::attach_jpeg_gain_map(attached, other);
	const auto found_again = libfiles::find_jpeg_gain_map(again);
	CHECK(bytes(found_again.begin(), found_again.end()) == other);
	CHECK(found_again.data() == again.data() + again.size() - other.size());
	CHECK_EQ(app2_segments(again, mpf_signature).size(), 1u);
	CHECK(app2_segments(again, iso_signature).empty());

	// A primary with no XMP is given one saying an Adobe gain map follows; an ISO 21496-1 gain map needs only
	// the version segment.
	const auto bare = test_build::jpeg_of({});
	CHECK(primary_packet(libfiles::attach_jpeg_gain_map(bare, gain_map)).exists(hdrgm_ns, "Version"));
	const auto iso_only = test_build::jpeg_of({{0xE2, test_build::iso_gain_map_app2({0, 0, 0, 0, 1, 2, 3})}}, 0x44);
	const auto with_iso = libfiles::attach_jpeg_gain_map(bare, iso_only);
	const auto found_iso = libfiles::find_jpeg_gain_map(with_iso);
	CHECK(bytes(found_iso.begin(), found_iso.end()) == iso_only);
	CHECK(primary_packet(with_iso).empty());
	CHECK_EQ(app2_segments(with_iso, iso_signature).size(), 1u);

	// A picture after the primary that is not a gain map is not found as one.
	const auto plain_picture = test_build::jpeg_of({}, 0x55);
	CHECK(libfiles::find_jpeg_gain_map(libfiles::attach_jpeg_gain_map(bare, plain_picture)).empty());

	// Signalling a writer left after the tables is replaced there too.
	test_build::builder late;
	const auto late_segment = [&late](const unsigned marker, const bytes& payload)
	{
		late.u8(0xFF).u8(marker).be16(static_cast<unsigned>(payload.size() + 2)).raw(payload);
	};
	late.u8(0xFF).u8(0xD8);
	late_segment(0xDB, test_build::to_bytes("tables"));
	late_segment(0xE2, test_build::lit("MPF\0stale"));
	late_segment(0xE2, test_build::iso_gain_map_app2({0, 0, 0, 0}));
	late_segment(0xC0, test_build::to_bytes("frame"));
	late_segment(0xDA, test_build::to_bytes("scan"));
	late.u8(0x11).u8(0xFF).u8(0xD9);

	const auto late_attached = libfiles::attach_jpeg_gain_map(late.b, gain_map);
	const auto late_found = libfiles::find_jpeg_gain_map(late_attached);
	CHECK(bytes(late_found.begin(), late_found.end()) == gain_map);
	CHECK_EQ(app2_segments(late_attached, mpf_signature).size(), 1u);
	CHECK_EQ(app2_segments(late_attached, iso_signature).size(), 1u);
}

TEST("Should leave a JPEG's primary saying no gain map follows when none is attached")
{
	const auto motion_photo = std::string(
		R"(<rdf:Description rdf:about="" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" )"
		R"(xmlns:Container="http://ns.google.com/photos/1.0/container/" )"
		R"(xmlns:Item="http://ns.google.com/photos/1.0/container/item/" hdrgm:Version="1.0">)"
		R"(<Container:Directory><rdf:Seq>)"
		R"(<rdf:li rdf:parseType="Resource"><Container:Item Item:Semantic="Primary" Item:Mime="image/jpeg"/></rdf:li>)"
		R"(<rdf:li rdf:parseType="Resource"><Container:Item Item:Semantic="GainMap" Item:Mime="image/jpeg" )"
		R"(Item:Length="100"/></rdf:li>)"
		R"(<rdf:li rdf:parseType="Resource"><Container:Item Item:Semantic="MotionPhoto" Item:Mime="video/mp4" )"
		R"(Item:Length="2000"/></rdf:li>)"
		R"(</rdf:Seq></Container:Directory></rdf:Description>)");

	const auto primary = test_build::jpeg_of({{0xE1, test_build::xmp_app1(motion_photo)},
	                                          {0xE2, test_build::iso_gain_map_app2({0, 0, 0, 0})},
	                                          {0xE2, test_build::lit("MPF\0stale")}});
	const auto alone = libfiles::attach_jpeg_gain_map(primary, {});
	CHECK(!alone.empty());
	CHECK(app2_segments(alone, mpf_signature).empty());
	CHECK(app2_segments(alone, iso_signature).empty());

	// The motion photo the directory also lists stays in it.
	const auto meta = primary_packet(alone);
	CHECK(!meta.exists(hdrgm_ns, "Version"));
	CHECK_EQ(meta.count_array_items(container_ns, "Directory"), 2);
	CHECK_STR(directory_field(meta, 2, "Semantic"), "MotionPhoto");

	// One with nothing to say of a gain map comes back as it was; what is not a JPEG, not at all.
	const auto plain = test_build::jpeg_of(
		{{0xE1, test_build::xmp_app1(R"(<rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/">)"
	                                 R"(<dc:format>image/jpeg</dc:format></rdf:Description>)")}});
	CHECK(libfiles::attach_jpeg_gain_map(plain, {}) == plain);
	CHECK(libfiles::attach_jpeg_gain_map(bytes{1, 2, 3}, {}).empty());
	CHECK(libfiles::attach_jpeg_gain_map(plain, bytes{1, 2, 3}).empty());
	CHECK(libfiles::find_jpeg_gain_map(bytes{1, 2, 3}).empty());
}

TEST("Should not carry what says a gain map follows into a JPEG's re-encoding")
{
	auto description = test_build::ultra_hdr_primary_description(500);
	description.insert(description.find("hdrgm:Version"),
	                   R"(xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmp:Rating="3" )");

	const auto source = test_build::jpeg_of({{0xE1, test_build::xmp_app1(description)},
	                                         {0xE2, test_build::iso_gain_map_app2({0, 0, 0, 0})},
	                                         {0xE2, test_build::lit("MPF\0index")}});
	const auto encoded = test_build::jpeg_of({}, 0x33);
	const auto carried = libfiles::carry_jpeg_metadata(source, encoded, 8, 8);
	CHECK(!carried.empty());

	const auto meta = primary_packet(carried);
	CHECK(meta.exists(libfiles::xmp::ns::xmp, "Rating"));
	CHECK(!meta.exists(hdrgm_ns, "Version"));
	CHECK(!meta.exists(container_ns, "Directory"));
	CHECK(app2_segments(carried, iso_signature).empty());
	CHECK(app2_segments(carried, mpf_signature).empty());
}

TEST("Should mark a JPEG a gain map as Ultra HDR does, and read its metadata back in either scheme")
{
	libfiles::gain_map_metadata m;
	m.alternate_headroom = 2.75;
	m.gain_min = {-0.5, -0.25, 0.0};
	m.gain_max = {2.5, 2.75, 3.0};
	m.gamma = {1.0, 1.0, 1.5};

	const auto map = test_build::jpeg_of({{0xE0, test_build::lit("JFIF\0\1\1")},
	                                      {0xE2, test_build::iso_gain_map_app2({0, 0, 0, 0, 9})}},
	                                     0x66);
	const auto marked = libfiles::mark_jpeg_gain_map(map, m);
	CHECK(!marked.empty());

	// Its XMP and ISO 21496-1's segment after the JFIF segment, in place of what it said before.
	const auto segments = test_build::jpeg_segments(marked);
	CHECK(segments.size() >= 3 && segments[0].first == 0xE0 && segments[1].first == 0xE1 && segments[2].first == 0xE2);
	CHECK_EQ(app2_segments(marked, iso_signature).size(), 1u);
	const auto read = libfiles::read_jpeg_gain_map_metadata(marked);
	CHECK(read && *read == m);

	// Adobe's XMP alone says the same.
	if (segments.size() >= 3)
	{
		const auto xmp_only = test_build::jpeg_of({{0xE1, segments[1].second}}, 0x66);
		const auto from_xmp = libfiles::read_jpeg_gain_map_metadata(xmp_only);
		CHECK(from_xmp && *from_xmp == m);
	}

	// Attached after a primary, it is found there, and the primary says one follows.
	const auto ultra_hdr = libfiles::attach_jpeg_gain_map(test_build::jpeg_of({}), marked);
	const auto found = libfiles::find_jpeg_gain_map(ultra_hdr);
	CHECK(bytes(found.begin(), found.end()) == marked);
	CHECK(primary_packet(ultra_hdr).exists(hdrgm_ns, "Version"));
	// The listing names it, and its image in the MPF index.
	CHECK_STR(listed(ultra_hdr, "Gain map"), "Ultra HDR, image 2, up to 6.73 times SDR white (2.75 stops)");
	CHECK(listed(ultra_hdr, "Image 2").starts_with("gain map, "));
	CHECK_STR(listed(test_build::jpeg_of({}), "Gain map"), "(none)");

	// One value for all three channels, and defaults for what Adobe's scheme leaves out.
	const auto adobe = libfiles::read_jpeg_gain_map_metadata(
		test_build::jpeg_of({{0xE1, test_build::xmp_app1(test_build::gain_map_description)}}));
	CHECK(adobe && adobe->gain_max[0] == 2.5 && adobe->gain_max[2] == 2.5 && adobe->gain_min[1] == 0.0 &&
	      adobe->gamma[1] == 1.0 && adobe->base_offset[2] == 1.0 / 64.0 && adobe->alternate_headroom == 2.5);

	// A base that is HDR, and a JPEG that is no gain map, say nothing libfiles applies.
	const auto hdr_base = test_build::jpeg_of(
		{{0xE1,
	      test_build::xmp_app1(
			  R"(<rdf:Description rdf:about="" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" hdrgm:Version="1.0" )"
			  R"(hdrgm:GainMapMax="2" hdrgm:HDRCapacityMax="2" hdrgm:BaseRenditionIsHDR="True"/>)")}});
	CHECK(!libfiles::read_jpeg_gain_map_metadata(hdr_base));
	CHECK(!libfiles::read_jpeg_gain_map_metadata(test_build::jpeg_of({})));
	CHECK(libfiles::mark_jpeg_gain_map(bytes{1, 2, 3}, m).empty());
}

namespace
{
	// A file that remembers how far into it anything was read.
	class furthest_read_stream final : public libfiles::stream
	{
	public:
		explicit furthest_read_stream(bytes data) : _data(std::move(data)) {}

		std::uint64_t size() override { return _data.size(); }

		std::size_t read(const std::uint64_t offset, void* data, const std::size_t size) override
		{
			if (offset >= _data.size()) return 0;
			const auto n = std::min<std::size_t>(size, _data.size() - static_cast<std::size_t>(offset));
			std::memcpy(data, _data.data() + offset, n);
			furthest = std::max<std::uint64_t>(furthest, offset + n);
			return n;
		}

		std::uint64_t furthest = 0;

	private:
		bytes _data;
	};

	libfiles::scan_result scan_jpeg_bytes(const bytes& file,
	                                      const libfiles::scan_intent intent = libfiles::scan_intent::index)
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::scan(r, intent);
	}

	// A primary whose multi-picture index lists second after it, as Apple writes one, saying nothing more itself.
	bytes listing_after(const bytes& second)
	{
		const auto layout = [&second](const std::uint64_t primary_size)
		{
			return test_build::jpeg_of({{0xE2, libfiles::detail::make_mpf_payload(primary_size, second.size(), 10)}});
		};

		auto file = layout(layout(0).size());
		file.insert(file.end(), second.begin(), second.end());
		return file;
	}
}

TEST("Should say a JPEG carries a gain map by what its primary says, and else by its listed image's own segments")
{
	// Ultra HDR's primary says one follows, which attach writes, and an index scan reads no further to believe it.
	const auto gain_map = test_build::jpeg_of({{0xE1, test_build::xmp_app1(test_build::gain_map_description)}}, 0x22);
	const auto ultra_hdr = libfiles::attach_jpeg_gain_map(test_build::jpeg_of({}), gain_map);
	CHECK(scan_jpeg_bytes(ultra_hdr).gain_map);
	CHECK(scan_jpeg_bytes(ultra_hdr, libfiles::scan_intent::inspect).gain_map);

	furthest_read_stream ultra_hdr_stream(ultra_hdr);
	libfiles::reader ultra_hdr_reader(ultra_hdr_stream, 16);
	CHECK(libfiles::scan(ultra_hdr_reader).gain_map);
	CHECK(ultra_hdr_stream.furthest <= ultra_hdr.size() - gain_map.size());

	// ISO 21496-1's primary says so by its versions alone.
	const auto iso_map = test_build::jpeg_of({{0xE2, test_build::iso_gain_map_app2({0, 0, 0, 0, 1, 2, 3})}}, 0x44);
	CHECK(scan_jpeg_bytes(libfiles::attach_jpeg_gain_map(test_build::jpeg_of({}), iso_map)).gain_map);

	// Apple's says so in the gain map alone, so an index scan reads the head of the image the index lists.
	const auto apple_map = test_build::jpeg_of(
		{{0xE1,
	      test_build::xmp_app1(R"(<rdf:Description rdf:about="" xmlns:apdi="http://ns.apple.com/pixeldatainfo/1.0/" )"
	                           R"(apdi:AuxiliaryImageType="urn:com:apple:photo:2020:aux:hdrgainmap"/>)")}},
		0x66);
	const auto apple = listing_after(apple_map);
	CHECK(scan_jpeg_bytes(apple).gain_map);
	CHECK(scan_jpeg_bytes(apple, libfiles::scan_intent::inspect).gain_map);

	furthest_read_stream apple_stream(apple);
	libfiles::reader apple_reader(apple_stream, 16);
	CHECK(libfiles::scan(apple_reader).gain_map);
	CHECK(apple_stream.furthest > apple.size() - apple_map.size());

	// A second picture that is no gain map carries none, nor does a JPEG with no index - whatever its XMP says.
	CHECK(!scan_jpeg_bytes(listing_after(test_build::jpeg_of({}, 0x55))).gain_map);
	CHECK(!scan_jpeg_bytes(listing_after(test_build::jpeg_of({}, 0x55)), libfiles::scan_intent::inspect).gain_map);
	CHECK(!scan_jpeg_bytes(test_build::jpeg_of({})).gain_map);
	CHECK(!scan_jpeg_bytes(
			   test_build::jpeg_of({{0xE1, test_build::xmp_app1(test_build::ultra_hdr_primary_description(100))}}))
	           .gain_map);
}
