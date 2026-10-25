// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for the reader, the zlib pieces and the still-image scanners. Each scanner reads a
// file built here byte by byte, so every field it reports is one the test put there.

#include "test.h"

#include "builders.h"
#include "libfiles/scan.h"
#include "photoshop_internal.h"
#include "scan_internal.h"

#include <cstring>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	// A stream that counts what it is asked for, as a network share would charge for it.
	class counting_stream final : public libfiles::stream
	{
	public:
		explicit counting_stream(bytes data) : _data(std::move(data))
		{
		}

		std::uint64_t size() override { return _data.size(); }

		std::size_t read(const std::uint64_t offset, void* data, const std::size_t size) override
		{
			++reads;
			if (offset >= _data.size()) return 0;
			const auto n = std::min<std::size_t>(size, _data.size() - static_cast<std::size_t>(offset));
			std::memcpy(data, _data.data() + offset, n);
			return truncate_reads ? n / 2 : n;
		}

		int reads = 0;
		bool truncate_reads = false;

	private:
		bytes _data;
	};

	// A little-endian TIFF with one orientation in IFD0, and an IFD1 with its own orientation and a
	// thumbnail pointer when asked.
	bytes make_exif(const int ifd0_orientation, const int ifd1_orientation, const bool thumbnail = false)
	{
		builder t;
		t.raw("II").le16(42).le32(8);

		// IFD0 at 8: one entry, then the IFD1 offset.
		const auto ifd0_entries = ifd0_orientation ? 1u : 0u;
		t.le16(ifd0_entries);
		if (ifd0_orientation) t.le16(0x0112).le16(3).le32(1).le16(ifd0_orientation).le16(0);
		const auto ifd1_offset = static_cast<std::uint32_t>(t.size() + 4);
		t.le32(ifd1_offset);

		// IFD1: orientation and, optionally, the thumbnail's offset and length.
		const auto entries = 1u + (thumbnail ? 2u : 0u);
		t.le16(entries);
		t.le16(0x0112).le16(3).le32(1).le16(ifd1_orientation).le16(0);
		const auto thumb_offset = static_cast<std::uint32_t>(t.size() + 12u * (entries - 1) + 4);
		if (thumbnail)
		{
			t.le16(0x0201).le16(4).le32(1).le32(thumb_offset);
			t.le16(0x0202).le16(4).le32(1).le32(20);
		}
		t.le32(0);
		if (thumbnail) t.u8(0xFF).u8(0xD8).zeros(18);
		t.zeros(16);
		return t.b;
	}

	void jpeg_segment(builder& j, const unsigned marker, const bytes& payload)
	{
		j.u8(0xFF).u8(marker).be16(static_cast<unsigned>(payload.size() + 2)).raw(payload);
	}

	bytes make_jpeg(const bool truncate_before_sos = false)
	{
		builder j;
		j.u8(0xFF).u8(0xD8);

		jpeg_segment(j, 0xE0, builder().raw(std::string_view("JFIF\0", 5)).zeros(9).b);
		jpeg_segment(j, 0xE0, builder().raw(std::string_view("JFXX\0", 5)).u8(0x10).u8(0xFF).u8(0xD8).zeros(14).b);
		jpeg_segment(j, 0xE1, builder().raw(std::string_view("Exif\0\0", 6)).raw(make_exif(6, 1)).b);
		jpeg_segment(j, 0xE1, builder().raw(std::string_view("http://ns.adobe.com/xap/1.0/\0", 29)).raw("<x:xmpmeta/>").b);

		// The ICC profile in two segments, stored out of order.
		jpeg_segment(j, 0xE2, builder().raw(std::string_view("ICC_PROFILE\0", 12)).u8(2).u8(2).raw("second").b);
		jpeg_segment(j, 0xE2, builder().raw(std::string_view("ICC_PROFILE\0", 12)).u8(1).u8(2).raw("first-").b);

		// APP13: an IPTC resource and one other resource.
		builder resources;
		resources.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(4).raw("iptc");
		resources.raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(2).raw("rs");
		jpeg_segment(j, 0xED, builder().raw(std::string_view("Photoshop 3.0\0", 14)).raw(resources.b).b);

		jpeg_segment(j, 0xFE, to_bytes("a comment"));

		// SOF0: 8 bits, 480 high, 640 wide, three components with 2x2 luma sampling.
		jpeg_segment(j, 0xC0, builder().u8(8).be16(480).be16(640).u8(3)
		                               .u8(1).u8(0x22).u8(0).u8(2).u8(0x11).u8(1).u8(3).u8(0x11).u8(1).b);

		if (!truncate_before_sos)
		{
			jpeg_segment(j, 0xDA, builder().u8(3).zeros(9).b);
			j.zeros(32).u8(0xFF).u8(0xD9);
		}

		return j.b;
	}

	const bytes fixed_stream = {
		0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xb1
	};

	const bytes stored_stream = {
		0x78, 0x01, 0x01, 0x0c, 0x00, 0xf3, 0xff, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x20, 0x62, 0x6c, 0x6f, 0x63,
		0x6b, 0x1f, 0x80, 0x04, 0xbd
	};

	const bytes dynamic_stream = {
		0x78, 0xda, 0x5d, 0x90, 0x41, 0x8b, 0x83, 0x30, 0x10, 0x85, 0xff, 0x4a, 0x60, 0x0f, 0x39, 0xe9, 0x54, 0x7b,
		0x32, 0xa8, 0xb0, 0x20, 0xcb, 0x9e, 0xf7, 0x1f, 0xa4, 0xc9, 0xd8, 0x86, 0x26, 0x31, 0x98, 0x11, 0xd3, 0x7f,
		0xdf, 0x58, 0xa5, 0x0b, 0xbd, 0xbd, 0xf7, 0x78, 0xf3, 0xf1, 0x98, 0x36, 0x89, 0xe4, 0x82, 0x43, 0x92, 0x2c,
		0x39, 0xeb, 0xa3, 0x48, 0x1d, 0x97, 0x7a, 0xba, 0xa0, 0xc8, 0x7a, 0x8b, 0x81, 0xf7, 0xed, 0xac, 0x47, 0xf1,
		0x37, 0xfc, 0x1c, 0x8d, 0xec, 0x3a, 0x7e, 0x23, 0x0a, 0x02, 0x60, 0x5d, 0xd7, 0x72, 0x3d, 0x97, 0xd3, 0x7c,
		0x85, 0xaa, 0x69, 0x1a, 0x38, 0xd5, 0x50, 0xd7, 0x45, 0x6e, 0x14, 0xf1, 0xe1, 0x49, 0xa6, 0xc2, 0xc7, 0xaf,
		0x03, 0x30, 0x60, 0x54, 0xb3, 0x09, 0x64, 0x26, 0xcf, 0x36, 0x2f, 0x2f, 0xd3, 0x42, 0x1d, 0xe7, 0x07, 0x55,
		0xab, 0x37, 0x34, 0x2c, 0xb3, 0x7d, 0x21, 0xb5, 0x02, 0xb4, 0xe8, 0xd0, 0x53, 0x84, 0xaa, 0xac, 0xb6, 0x29,
		0x5a, 0x09, 0x32, 0x64, 0x71, 0x67, 0x7e, 0x5b, 0xda, 0x85, 0x35, 0x1b, 0x46, 0x58, 0xe9, 0xaf, 0x1d, 0x4f,
		0x85, 0xc6, 0x51, 0x2e, 0x96, 0x78, 0x3f, 0x3c, 0xbc, 0x74, 0x46, 0xb1, 0xdf, 0x65, 0x1c, 0x9d, 0xf4, 0x8c,
		0x30, 0x12, 0x0b, 0x52, 0xdd, 0x91, 0x5a, 0xd8, 0x0f, 0xfb, 0x5d, 0xbc, 0x50, 0xf0, 0x8f, 0x87, 0x8f, 0xcd,
		0x47, 0x92, 0xdf, 0x90, 0xd5, 0xfb, 0x69, 0xfd, 0x13, 0xb8, 0xa2, 0x6e, 0x20
	};

	constexpr std::string_view dynamic_source =
		"<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description rdf:about='' xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:title><rdf:Alt>"
		"<rdf:li xml:lang='x-default'>Dynamic Huffman test packet</rdf:li></rdf:Alt></dc:title>"
		"</rdf:Description></rdf:RDF></x:xmpmeta>";

	libfiles::scan_result scan_bytes(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index)
	{
		libfiles::reader r(file);
		return libfiles::scan(r, intent);
	}

	bool has_row(const std::vector<libfiles::structure_row>& rows, const std::string_view key)
	{
		return std::ranges::any_of(rows, [key](const auto& row) { return row.key == key; });
	}
}

TEST("reader borrows memory and refuses reads past the end")
{
	const bytes data = {1, 2, 3, 4, 5, 6, 7, 8};
	libfiles::reader r(data);

	CHECK_EQ(r.size(), 8u);
	CHECK(r.resident().data() == data.data());
	CHECK(r.contains(8, 0));
	CHECK(!r.contains(7, 2));
	CHECK(!r.contains(9, 0));
	CHECK(!r.contains(1, ~0ull));
	CHECK_EQ(r.u8(7), 8u);
	CHECK_EQ(r.be16(0), 0x0102u);
	CHECK_EQ(r.le16(0), 0x0201u);
	CHECK_EQ(r.be32(4), 0x05060708u);
	CHECK_EQ(r.le32(4), 0x08070605u);
	CHECK(r.be64(0) == 0x0102030405060708ull);
	CHECK(r.le64(0) == 0x0807060504030201ull);
	CHECK(r.view(2, 3).data() == data.data() + 2);
	CHECK_THROWS(r.u8(8));
	CHECK_THROWS(r.view(6, 3));
	CHECK_THROWS(r.read(~0ull, 2));
}

TEST("reader reads a stream a block at a time")
{
	bytes data(1000);
	for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);

	counting_stream s(data);
	libfiles::reader r(s, 256);

	CHECK(r.resident().empty());
	CHECK_EQ(r.u8(10), 10u);
	CHECK_EQ(r.u8(255), 255u);
	CHECK_EQ(s.reads, 1);

	// Crossing into the next block keeps the first and reads only the second.
	CHECK_EQ(r.be16(255), 0xFF00u);
	CHECK_EQ(s.reads, 2);
	CHECK_EQ(r.u8(300), 300u % 256u);
	CHECK_EQ(s.reads, 2);

	// The last block is cut at the end of the file.
	CHECK_EQ(r.u8(999), 999u % 256u);
	CHECK_EQ(s.reads, 3);

	// A read of a block or more goes straight to the caller's buffer.
	bytes out(512);
	r.read(100, out.data(), out.size());
	CHECK_EQ(s.reads, 4);
	CHECK_EQ(out[0], 100u);
	CHECK_EQ(r.source_reads(), 4u);

	CHECK_THROWS(r.u8(1000));
}

TEST("reader reports a stream that returns less than it holds")
{
	counting_stream s(bytes(100, 7));
	s.truncate_reads = true;
	libfiles::reader r(s);

	CHECK_THROWS(r.u8(0));
}

TEST("memory stream writes, grows and shrinks")
{
	libfiles::memory_stream m(to_bytes("abcdef"));
	CHECK(m.write(4, "XYZ", 3));
	CHECK_STR(to_string(m.bytes()), "abcdXYZ");
	CHECK(m.resize(3));
	CHECK_STR(to_string(m.bytes()), "abc");

	char out[8] = {};
	CHECK_EQ(m.read(1, out, sizeof(out)), 2u);
	CHECK_EQ(m.read(5, out, sizeof(out)), 0u);
	CHECK(m.flush());
}

TEST("zlib streams inflate: stored, fixed and dynamic blocks")
{
	std::vector<std::uint8_t> out;

	CHECK(libfiles::detail::zlib_inflate(stored_stream, out, 1024));
	CHECK_STR(to_string(out), "stored block");

	CHECK(libfiles::detail::zlib_inflate(fixed_stream, out, 1024));
	CHECK_STR(to_string(out), "hello hello hello hello");

	CHECK(libfiles::detail::zlib_inflate(dynamic_stream, out, 1024));
	CHECK_STR(to_string(out), dynamic_source);
}

TEST("zlib streams that zlib would refuse are refused")
{
	std::vector<std::uint8_t> out;

	auto bad_check = dynamic_stream;
	bad_check.back() ^= 1;
	CHECK(!libfiles::detail::zlib_inflate(bad_check, out, 1024));

	const bytes truncated(dynamic_stream.begin(), dynamic_stream.end() - 10);
	CHECK(!libfiles::detail::zlib_inflate(truncated, out, 1024));

	const bytes no_checksum(fixed_stream.begin(), fixed_stream.end() - 4);
	CHECK(!libfiles::detail::zlib_inflate(no_checksum, out, 1024));

	// A preset dictionary, and a header whose check bits do not divide by 31.
	CHECK(!libfiles::detail::zlib_inflate(bytes{0x78, 0xbb, 0, 0, 0, 0}, out, 1024));
	CHECK(!libfiles::detail::zlib_inflate(bytes{0x78, 0xdb, 0, 0}, out, 1024));

	// Output beyond the caller's limit.
	CHECK(!libfiles::detail::zlib_inflate(dynamic_stream, out, 100));

	// A reserved block type.
	CHECK(!libfiles::detail::zlib_inflate(bytes{0x78, 0x01, 0x07, 0, 0, 0, 0}, out, 1024));
}

TEST("crc32 matches the published check value")
{
	CHECK_EQ(libfiles::detail::crc32(0, to_bytes("123456789")), 0xCBF43926u);
}

TEST("formats are told apart by their first bytes")
{
	using libfiles::image_format;
	const auto detect = [](const bytes& b) { return libfiles::detect_image_format(b); };

	CHECK(detect(to_bytes("8BPS")) == image_format::psd);
	CHECK(detect(to_bytes("GIF89a")) == image_format::gif);
	CHECK(detect(bytes{0, 0, 0, 0x0C, 'J', 'X', 'L', ' '}) == image_format::jxl);
	CHECK(detect(bytes{0xFF, 0x0A, 0, 0}) == image_format::jxl);
	CHECK(detect(lit("RIFF\0\0\0\0WEBP")) == image_format::webp);
	CHECK(detect(lit("RIFF\0\0\0\0WAVE")) == image_format::unknown);
	CHECK(detect(bytes{0xFF, 0xD8, 0xFF, 0xE0}) == image_format::jpeg);
	CHECK(detect(lit("BM\0\0")) == image_format::bmp);
	CHECK(detect(bytes{0x89, 'P', 'N', 'G'}) == image_format::png);
	CHECK(detect(bytes{'I', 'I', 42, 0}) == image_format::tiff);
	CHECK(detect(bytes{'M', 'M', 0, 43}) == image_format::tiff);
	CHECK(detect(bytes{'I', 'I', 41, 0}) == image_format::unknown);
	CHECK(detect(lit("\0\0\0\x18" "ftypheic")) == image_format::heif);
	CHECK(detect(lit("\0\0\0\x18" "ftypmp42")) == image_format::unknown);
	CHECK(detect(bytes{0xFF, 0xD8}) == image_format::unknown);
}

TEST("jpeg scan reads the frame and every metadata block")
{
	const auto file = make_jpeg();
	const auto result = scan_bytes(file);

	CHECK(result.success);
	CHECK(result.error.empty());
	CHECK(result.format == libfiles::image_format::jpeg);
	CHECK_EQ(result.width, 640u);
	CHECK_EQ(result.height, 480u);
	CHECK_STR(result.pixel_format, "yuv420");
	CHECK(result.orientation == 6);
	CHECK_STR(to_string(result.metadata.xmp), "<x:xmpmeta/>");
	CHECK_STR(to_string(result.metadata.icc), "first-second");
	CHECK_STR(to_string(result.metadata.iptc), "iptc");
	CHECK_EQ(result.metadata.photoshop.size(), 14u);
	CHECK(result.metadata.exif == make_exif(6, 1));
	CHECK_EQ(result.jfif_thumbnail.size, 16u);
	CHECK_EQ(file[static_cast<std::size_t>(result.jfif_thumbnail.offset)], 0xFFu);
	CHECK(result.structure.empty());
}

TEST("jpeg inspect scan reports its structure")
{
	const auto result = scan_bytes(make_jpeg(), libfiles::scan_intent::inspect);

	CHECK(has_row(result.structure, "Summary (10)"));
	CHECK(has_row(result.structure, "Segments (10)"));
	CHECK(has_row(result.structure, "Comments (1)"));
	CHECK(has_row(result.structure, "Chroma subsampling"));
	CHECK(!has_row(result.structure, "Huffman tables (0)"));

	const auto metadata = std::ranges::find(result.structure, std::string_view("Metadata carried"), &libfiles::structure_row::key);
	CHECK(metadata != result.structure.end() && metadata->value == "Exif XMP IPTC ICC");
}

TEST("jpeg cut short before its scan keeps what it read")
{
	const auto result = scan_bytes(make_jpeg(true));

	CHECK(!result.success);
	CHECK(result.error.empty());
	CHECK_EQ(result.width, 640u);
	CHECK(result.orientation == 6);
	CHECK(!result.metadata.xmp.empty());
}

TEST("jpeg photoshop resources stop at the limit on a whole resource")
{
	builder resources;
	resources.raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(100).zeros(100);
	resources.raw("8BIM").be16(0x03ee).u8(0).u8(0).be32(100).zeros(100);

	builder j;
	j.u8(0xFF).u8(0xD8);
	jpeg_segment(j, 0xED, builder().raw(std::string_view("Photoshop 3.0\0", 14)).raw(resources.b).b);
	j.u8(0xFF).u8(0xDA).zeros(16);

	libfiles::reader r(j.b);
	libfiles::scan_limits limits;
	limits.jpeg_photoshop_resources = 150;
	const auto result = libfiles::scan(r, libfiles::scan_intent::index, limits);

	CHECK_EQ(result.metadata.photoshop.size(), 112u);
}

TEST("exif summary prefers the primary orientation and finds the thumbnail")
{
	const auto both = libfiles::summarize_exif(make_exif(6, 1, true));
	CHECK(both.orientation == 6);
	CHECK_EQ(both.thumbnail.size, 20u);

	const auto thumbnail_only = libfiles::summarize_exif(make_exif(0, 8));
	CHECK(thumbnail_only.orientation == 8);
	CHECK(thumbnail_only.thumbnail.empty());

	CHECK(!libfiles::summarize_exif(to_bytes("not a tiff at all, really")).orientation);
}

TEST("png scan reads the header and metadata on both sides of the image data")
{
	builder p;
	p.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
	png_chunk(p, "IHDR", png_ihdr(320, 200, 6));
	png_chunk(p, "eXIf", builder().raw(std::string_view("Exif\0\0", 6)).raw(make_exif(3, 1)).b);
	png_chunk(p, "zTXt", builder().raw(std::string_view("XML:com.adobe.xmp\0\0", 19)).raw(dynamic_stream).b);
	png_chunk(p, "IDAT", bytes(64, 0));
	png_chunk(p, "iTXt", builder().raw(std::string_view("XML:com.adobe.xmp\0\0\0\0\0", 22)).raw("<later/>").b);
	png_chunk(p, "IEND", {});

	const auto result = scan_bytes(p.b);

	CHECK(result.success);
	CHECK_EQ(result.width, 320u);
	CHECK_EQ(result.height, 200u);
	CHECK_STR(result.pixel_format, "rgba");
	CHECK(result.metadata.exif == make_exif(3, 1));
	CHECK(result.orientation == 3);
	CHECK_STR(to_string(result.metadata.xmp), "<later/>");
}

TEST("png compressed xmp inflates")
{
	builder p;
	p.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
	png_chunk(p, "IHDR", png_ihdr(1, 1, 0));
	png_chunk(p, "iTXt", builder().raw(std::string_view("XML:com.adobe.xmp\0\1\0\0\0", 22)).raw(dynamic_stream).b);
	png_chunk(p, "IDAT", bytes(8, 0));
	png_chunk(p, "IEND", {});

	const auto result = scan_bytes(p.b);

	CHECK(result.success);
	CHECK_STR(result.pixel_format, "gray");
	CHECK_STR(to_string(result.metadata.xmp), dynamic_source);
}

TEST("png that libpng would refuse is not scanned")
{
	const auto header_only = [](const std::string_view first_chunk, const bytes& data)
	{
		builder p;
		p.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
		png_chunk(p, first_chunk, data);
		png_chunk(p, "IDAT", bytes(8, 0));
		png_chunk(p, "IEND", {});
		return scan_bytes(p.b);
	};

	CHECK(!header_only("tEXt", to_bytes("a\0b")).error.empty());
	CHECK(!header_only("IHDR", png_ihdr(0, 10, 2)).error.empty());
	CHECK(!header_only("IHDR", png_ihdr(10, 10, 5)).error.empty());
	CHECK(!header_only("IHDR", png_ihdr(10, 10, 3)).error.empty());
	CHECK(header_only("IHDR", png_ihdr(10, 10, 2)).error.empty());

	builder damaged;
	damaged.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
	png_chunk(damaged, "IHDR", png_ihdr(10, 10, 2), true);
	png_chunk(damaged, "IDAT", bytes(8, 0));
	CHECK(!scan_bytes(damaged.b).error.empty());

	builder no_image;
	no_image.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
	png_chunk(no_image, "IHDR", png_ihdr(10, 10, 2));
	png_chunk(no_image, "IEND", {});
	const auto result = scan_bytes(no_image.b);
	CHECK(!result.error.empty());
	CHECK(result.format == libfiles::image_format::png);
	CHECK(!result.success);
	CHECK_EQ(result.width, 0u);
}

TEST("png metadata search ends at a damaged metadata chunk")
{
	builder p;
	p.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
	png_chunk(p, "IHDR", png_ihdr(4, 4, 2));
	png_chunk(p, "tEXt", builder().raw(std::string_view("XML:com.adobe.xmp\0", 18)).raw("<first/>").b);
	png_chunk(p, "IDAT", bytes(8, 0));
	png_chunk(p, "tEXt", builder().raw(std::string_view("XML:com.adobe.xmp\0", 18)).raw("<damaged/>").b, true);
	png_chunk(p, "tEXt", builder().raw(std::string_view("XML:com.adobe.xmp\0", 18)).raw("<after/>").b);
	png_chunk(p, "IEND", {});

	const auto result = scan_bytes(p.b);

	CHECK(result.success);
	CHECK_STR(to_string(result.metadata.xmp), "<first/>");
}

TEST("webp scan reads simple lossy and lossless files")
{
	builder lossy;
	riff_chunk(lossy, "VP8 ", vp8_frame(300, 200));
	const auto a = scan_bytes(finish_riff(lossy));
	CHECK(a.success);
	CHECK_EQ(a.width, 300u);
	CHECK_EQ(a.height, 200u);
	CHECK_STR(a.pixel_format, "yuv420");

	builder lossless;
	riff_chunk(lossless, "VP8L", vp8l_frame(17, 9, true));
	const auto b = scan_bytes(finish_riff(lossless));
	CHECK(b.success);
	CHECK_EQ(b.width, 17u);
	CHECK_EQ(b.height, 9u);
	CHECK_STR(b.pixel_format, "rgba");
}

TEST("webp extended file gives its metadata chunks, which follow the image")
{
	builder w;
	riff_chunk(w, "VP8X", vp8x(0x20 | 0x08 | 0x04, 300, 200));
	riff_chunk(w, "ICCP", to_bytes("profile"));
	riff_chunk(w, "ALPH", bytes(4, 0));
	riff_chunk(w, "VP8 ", vp8_frame(300, 200));
	riff_chunk(w, "EXIF", builder().raw(std::string_view("Exif\0\0", 6)).raw(make_exif(8, 1)).b);
	riff_chunk(w, "XMP ", to_bytes("<x/>"));
	const auto file = finish_riff(w);

	const auto result = scan_bytes(file, libfiles::scan_intent::inspect);

	CHECK(result.success);
	CHECK_STR(result.pixel_format, "yuva420");
	CHECK_STR(to_string(result.metadata.icc), "profile");
	CHECK(result.metadata.exif == make_exif(8, 1));
	CHECK(result.orientation == 8);
	CHECK_STR(to_string(result.metadata.xmp), "<x/>");
	CHECK(has_row(result.structure, "Chunks (6)"));
	CHECK(has_row(result.structure, "Compression"));

	// Over a stream, the image data between the chunk headers is never read.
	counting_stream s(file);
	libfiles::reader r(s, 16);
	const auto streamed = libfiles::scan(r);
	CHECK(streamed.metadata.xmp == result.metadata.xmp);
	CHECK(r.source_bytes() < file.size());
}

TEST("webp canvas that disagrees with its image is not read")
{
	builder w;
	riff_chunk(w, "VP8X", vp8x(0, 301, 200));
	riff_chunk(w, "VP8 ", vp8_frame(300, 200));
	riff_chunk(w, "XMP ", to_bytes("<x/>"));
	const auto result = scan_bytes(finish_riff(w));

	CHECK(!result.success);
	CHECK(result.metadata.xmp.empty());

	builder animated;
	riff_chunk(animated, "VP8X", vp8x(0x02, 64, 48));
	riff_chunk(animated, "ANIM", bytes(6, 0));
	const auto anim = scan_bytes(finish_riff(animated));
	CHECK(anim.success);
	CHECK_EQ(anim.width, 64u);
	CHECK_STR(anim.pixel_format, "yuv420");
}

TEST("gif scan reads the screen and both forms of XMP extension")
{
	const auto gif = [](const bytes& extension)
	{
		builder g;
		g.raw("GIF89a").le16(120).le16(80).u8(0x80).u8(0).u8(0).zeros(6);
		g.raw(extension);
		g.u8(0x2C).zeros(8).u8(0).u8(2).u8(1).u8(0).u8(0).u8(0x3B);
		return g.b;
	};

	const auto raw = gif(builder().u8(0x21).u8(0xFF).u8(11).raw("XMP DataXMP").raw("<x:xmpmeta></x:xmpmeta>tail").u8(1).u8(0xFF).zeros(4).b);
	const auto a = scan_bytes(raw);
	CHECK(a.success);
	CHECK_EQ(a.width, 120u);
	CHECK_EQ(a.height, 80u);
	CHECK_STR(a.pixel_format, "pal8");
	CHECK_STR(to_string(a.metadata.xmp), "<x:xmpmeta></x:xmpmeta>");

	const auto blocks = gif(builder().u8(0x21).u8(0xFF).u8(11).raw("XMP DataXMP").u8(3).raw("abc").u8(2).raw("de").u8(0).b);
	CHECK_STR(to_string(scan_bytes(blocks).metadata.xmp), "abcde");

	CHECK(!scan_bytes(to_bytes("GIF87b0123456789")).error.empty());
}

namespace
{
	struct tiff_file
	{
		bytes b;
		std::uint32_t gps_offset = 0;
		std::uint32_t ifd1_offset = 0;
	};

	// A big-endian TIFF: IFD0 with the dimensions, orientation 5, XMP and a GPS directory - south
	// latitude 10 30' 0", west longitude 20 0' 36" - and IFD1 with a 32-byte thumbnail.
	tiff_file make_tiff()
	{
		builder t;
		t.raw("MM").be16(42).be32(8);

		t.be16(5);
		t.be16(0x0100).be16(4).be32(1).be32(4000);
		t.be16(0x0101).be16(3).be32(1).be16(3000).be16(0);
		t.be16(0x0112).be16(3).be32(1).be16(5).be16(0);
		const auto xmp_offset = 8u + 2u + 5u * 12u + 4u;
		t.be16(700).be16(1).be32(8).be32(xmp_offset);
		const auto gps_offset = xmp_offset + 8u;
		t.be16(0x8825).be16(4).be32(1).be32(gps_offset);
		const auto ifd1_offset = gps_offset + 2u + 4u * 12u + 4u + 48u;
		t.be32(ifd1_offset);
		t.raw("<x:xmp/>");

		const auto rationals = gps_offset + 2u + 4u * 12u + 4u;
		t.be16(4);
		t.be16(1).be16(2).be32(2).raw(std::string_view("S\0\0\0", 4));
		t.be16(2).be16(5).be32(3).be32(rationals);
		t.be16(3).be16(2).be32(2).raw(std::string_view("W\0\0\0", 4));
		t.be16(4).be16(5).be32(3).be32(rationals + 24u);
		t.be32(0);
		t.be32(10).be32(1).be32(30).be32(1).be32(0).be32(1);
		t.be32(20).be32(1).be32(0).be32(1).be32(36).be32(1);

		t.be16(2);
		t.be16(0x0201).be16(4).be32(1).be32(ifd1_offset + 2u + 24u + 4u);
		t.be16(0x0202).be16(4).be32(1).be32(32);
		t.be32(0);
		t.zeros(64);

		return {t.b, gps_offset, ifd1_offset};
	}
}

TEST("tiff scan reads its own directories")
{
	const auto tiff = make_tiff();
	const auto result = scan_bytes(tiff.b);

	CHECK(result.success);
	CHECK_EQ(result.width, 4000u);
	CHECK_EQ(result.height, 3000u);
	CHECK(result.orientation == 5);
	CHECK_STR(to_string(result.metadata.xmp), "<x:xmp/>");
	CHECK(result.gps.has_value());
	CHECK(result.gps && result.gps->latitude == -10.5);
	CHECK(result.gps && std::abs(result.gps->longitude + 20.01) < 1e-9);
	CHECK_EQ(result.tiff_thumbnail.size, 32u);
	CHECK_EQ(result.tiff_thumbnail.offset, tiff.ifd1_offset + 30u);

	// The rules are an EXIF block's: the same directories as a block read the same.
	CHECK(libfiles::summarize_exif(tiff.b).orientation == result.orientation);
	CHECK(libfiles::summarize_exif(tiff.b).thumbnail == result.tiff_thumbnail);
}

TEST("tiff scan keeps an image whose gps directory is damaged")
{
	// The latitude's values point past the end of the file: the position goes, and the image stays.
	auto tiff = make_tiff();
	const auto latitude_values = tiff.gps_offset + 2u + 12u + 8u;
	tiff.b[latitude_values] = 0x7f;

	const auto result = scan_bytes(tiff.b);

	CHECK(result.success);
	CHECK(result.error.empty());
	CHECK_EQ(result.width, 4000u);
	CHECK(!result.gps);
	CHECK(result.orientation == 5);
}

TEST("psd scan reads the header and its image resources")
{
	builder p;
	p.raw("8BPS").be16(1).zeros(6).be16(3).be32(50).be32(70).be16(8).be16(3);
	p.be32(0);

	builder resources;
	resources.raw("8BIM").be16(0x0424).u8(0).u8(0).be32(5).raw("<xmp>").u8(0);
	resources.raw("8BIM").be16(0x0404).u8(3).raw("abc").be32(4).raw("iptc");
	p.be32(static_cast<std::uint32_t>(resources.size())).raw(resources.b);
	p.zeros(16);

	const auto result = scan_bytes(p.b);

	CHECK(result.success);
	CHECK_EQ(result.width, 70u);
	CHECK_EQ(result.height, 50u);
	CHECK_STR(result.pixel_format, "rgb32");
	CHECK_STR(to_string(result.metadata.xmp), "<xmp>");
	CHECK_STR(to_string(result.metadata.iptc), "iptc");
}

TEST("bmp scan reads the info header")
{
	builder b;
	b.raw("BM").zeros(12);
	b.le32(40).le32(64).le32(static_cast<std::uint32_t>(-32)).le16(1).le16(24).zeros(24);

	const auto result = scan_bytes(b.b);

	CHECK(result.success);
	CHECK_EQ(result.width, 64u);
	CHECK_EQ(result.height, 32u);
	CHECK_STR(result.pixel_format, "RGB24");
}

TEST("photoshop resources sort into blocks and drop the IPTC digest")
{
	builder r;
	r.raw(std::string_view("Photoshop 3.0\0", 14));
	r.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(3).raw("abc").u8(0);
	r.raw("8BIM").be16(0x0425).u8(0).u8(0).be32(2).raw("dg");
	r.raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(2).raw("rs");

	libfiles::metadata_blocks blocks;
	CHECK(libfiles::parse_photoshop_resources(blocks, r.b, true));
	CHECK_STR(to_string(blocks.iptc), "abc");
	CHECK_EQ(blocks.photoshop.size(), 14u);

	libfiles::metadata_blocks unsigned_blocks;
	CHECK(!libfiles::parse_photoshop_resources(unsigned_blocks, r.b, false));
}

TEST("photoshop resources end at one whose length reaches 4 GiB, on every target")
{
	// Its padded length passes 32 bits, which on x86 once wrapped to 0 and walked on into its data.
	builder r;
	r.raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(0xFFFFFFFFu);
	r.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(3).raw("abc").u8(0);

	libfiles::metadata_blocks blocks;
	CHECK(!libfiles::parse_photoshop_resources(blocks, r.b, false));
	CHECK(blocks.iptc.empty());
	CHECK(blocks.photoshop.empty());
}

TEST("photoshop resources are read by one rule, the name padded to an even size and the last pad byte optional")
{
	using libfiles::detail::read_photoshop_resource;

	// Three letters and their length byte fill four bytes; two take a pad byte to fill four.
	builder odd;
	odd.raw("8BIM").be16(0x0404).u8(3).raw("abc").be32(5).raw("hello").u8(0);
	const auto odd_resource = read_photoshop_resource(odd.b);
	CHECK(odd_resource.has_value());
	if (odd_resource)
	{
		CHECK_EQ(odd_resource->id, std::uint16_t{0x0404});
		CHECK_EQ(odd_resource->name_size, std::size_t{4});
		CHECK_EQ(odd_resource->data_offset, std::size_t{14});
		CHECK_EQ(odd_resource->length, std::uint32_t{5});
		CHECK_EQ(odd_resource->size, std::size_t{20});
		CHECK(!odd_resource->lacks_pad());
	}

	builder even;
	even.raw("8BIM").be16(0x0424).u8(2).raw("ab").u8(0).be32(0);
	const auto even_resource = read_photoshop_resource(even.b);
	CHECK(even_resource && even_resource->name_size == 4u && even_resource->data_offset == 14u && even_resource->size == 14u);

	builder longest;
	longest.raw("8BIM").be16(0x0bb7).u8(255).raw(std::string(255, 'n')).be32(0);
	const auto longest_resource = read_photoshop_resource(longest.b);
	CHECK(longest_resource && longest_resource->data_offset == libfiles::detail::max_photoshop_resource_header);

	// The last resource may lack the pad byte after its data, but not its data.
	const auto unpadded = read_photoshop_resource(std::span(odd.b).first(19));
	CHECK(unpadded && unpadded->size == 19u && unpadded->lacks_pad() && unpadded->padded_size() == 20u);
	CHECK(!read_photoshop_resource(std::span(odd.b).first(18)));

	// A 4 GiB resource is measured past 32 bits on every target: its data does not fit 4 GiB.
	builder huge;
	huge.raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(0xFFFFFFFFu);
	CHECK(!read_photoshop_resource(huge.b, 0xFFFFFFFFu));

	// Nothing is read from bytes that end inside the header or do not start with the signature.
	CHECK(!read_photoshop_resource(std::span(odd.b).first(13)));
	CHECK(!read_photoshop_resource(std::span(even.b).first(11)));
	auto unsigned_block = odd.b;
	unsigned_block[3] = 'X';
	CHECK(!read_photoshop_resource(unsigned_block));
}

TEST("the scans read a last photoshop resource that lacks its pad byte, and keep it padded")
{
	builder r;
	r.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(3).raw("abc").u8(0);
	r.raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(3).raw("res");

	libfiles::metadata_blocks blocks;
	CHECK(libfiles::parse_photoshop_resources(blocks, r.b, false));
	CHECK_STR(to_string(blocks.iptc), "abc");
	CHECK(blocks.photoshop == builder().raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(3).raw("res").u8(0).b);

	// A PSD's resources end where their length says: the last one's pad byte is missing though the
	// file goes on.
	builder resources;
	resources.raw("8BIM").be16(0x03ed).u8(0).u8(0).be32(2).raw("rs");
	resources.raw("8BIM").be16(0x0424).u8(0).u8(0).be32(5).raw("<xmp>");

	builder p;
	p.raw("8BPS").be16(1).zeros(6).be16(3).be32(50).be32(70).be16(8).be16(3);
	p.be32(0);
	p.be32(static_cast<std::uint32_t>(resources.size())).raw(resources.b);
	p.zeros(16);

	const auto result = scan_bytes(p.b);
	CHECK(result.success);
	CHECK_STR(to_string(result.metadata.xmp), "<xmp>");
}

TEST("a psd whose last resource header runs past the end of the file keeps what was read before it")
{
	builder p;
	p.raw("8BPS").be16(1).zeros(6).be16(3).be32(50).be32(70).be16(8).be16(3);
	p.be32(0);

	// The second resource's 20-byte name runs past the resources and the file.
	builder resources;
	resources.raw("8BIM").be16(0x0424).u8(0).u8(0).be32(5).raw("<xmp>").u8(0);
	resources.raw("8BIM").be16(0x0404).u8(20).raw("abcdef");
	p.be32(static_cast<std::uint32_t>(resources.size())).raw(resources.b);

	const auto result = scan_bytes(p.b);
	CHECK(result.success);
	CHECK(result.error.empty());
	CHECK_EQ(result.width, 70u);
	CHECK_STR(to_string(result.metadata.xmp), "<xmp>");
	CHECK(result.metadata.iptc.empty());
}

TEST("the first photoshop resource of each kind that holds anything is the one read")
{
	// An empty IPTC resource, then two holding records, and two XMP resources.
	builder r;
	r.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(0);
	r.raw("8BIM").be16(0x0424).u8(0).u8(0).be32(4).raw("<a/>");
	r.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(5).raw("first").u8(0);
	r.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(4).raw("last");
	r.raw("8BIM").be16(0x0424).u8(0).u8(0).be32(4).raw("<b/>");

	libfiles::metadata_blocks blocks;
	CHECK(libfiles::parse_photoshop_resources(blocks, r.b, false));
	CHECK_STR(to_string(blocks.iptc), "first");
	CHECK_STR(to_string(blocks.xmp), "<a/>");
	CHECK(blocks.photoshop.empty());

	builder p;
	p.raw("8BPS").be16(1).zeros(6).be16(3).be32(50).be32(70).be16(8).be16(3);
	p.be32(0);
	p.be32(static_cast<std::uint32_t>(r.size())).raw(r.b);
	p.zeros(16);

	const auto result = scan_bytes(p.b);
	CHECK(result.success);
	CHECK_STR(to_string(result.metadata.iptc), "first");
	CHECK_STR(to_string(result.metadata.xmp), "<a/>");
}

TEST("files too short to identify are not scanned")
{
	const auto result = scan_bytes(bytes{0xFF, 0xD8, 0xFF, 0xD9});
	CHECK(result.format == libfiles::image_format::unknown);
	CHECK(!result.success);
	CHECK(libfiles::can_scan(libfiles::image_format::jpeg));
	CHECK(libfiles::can_scan(libfiles::image_format::heif));
	CHECK(libfiles::can_scan(libfiles::image_format::jxl));
	CHECK(!libfiles::can_scan(libfiles::image_format::unknown));
}

TEST("a small jpeg over a stream costs one read")
{
	counting_stream s(make_jpeg());
	libfiles::reader r(s);
	const auto result = libfiles::scan(r);

	CHECK(result.success);
	CHECK_EQ(s.reads, 1);
}
