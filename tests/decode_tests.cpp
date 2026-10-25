// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for decoding: PSD composite images in each colour mode, raw and PackBits, decoded into
// memory a test sink provides, and what decode() reports when it cannot.

#include "test.h"

#include "builders.h"
#include "libfiles/decode.h"
#include "libfiles/document.h"

#include <array>
#include <cstring>

using namespace test_build;

namespace
{
	// Memory a decode is asked for, filled first with a byte the decoder never writes, so the padding
	// past each row's pixels shows whether it was touched.
	struct test_sink final : libfiles::image_sink
	{
		std::vector<std::uint8_t> memory;
		std::size_t stride = 0;
		std::size_t padding = 0;
		bool refuse = false;
		int calls = 0;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			++calls;
			told = frame;
			if (refuse || frame.format != libfiles::pixel_format::bgrx8) return {};
			stride = std::size_t{4} * frame.width + padding;
			memory.assign(stride * frame.height, 0xAB);
			return {memory.data(), stride};
		}

		bool cancelled() override
		{
			return ++polls > cancel_after;
		}

		libfiles::frame_info told;
		int polls = 0;
		int cancel_after = 1 << 30;

		std::array<unsigned, 4> pixel(const std::size_t x, const std::size_t y) const
		{
			const auto* const p = memory.data() + y * stride + x * 4;
			return {p[0], p[1], p[2], p[3]};
		}
	};

	using pixel = std::array<unsigned, 4>;

	struct psd_spec
	{
		std::uint16_t mode = 3;
		std::uint16_t depth = 8;
		std::uint32_t width = 2;
		std::uint32_t height = 2;
		// One per channel: its rows, one after another.
		std::vector<bytes> planes;
		bytes colour_table;
		bool packbits = false;
	};

	// A row as PackBits literal runs, 128 bytes at most each.
	bytes packbits_row(const std::uint8_t* const row, const std::size_t n)
	{
		bytes out;
		for (std::size_t at = 0; at < n; at += 128)
		{
			const auto take = std::min<std::size_t>(128, n - at);
			out.push_back(static_cast<std::uint8_t>(take - 1));
			out.insert(out.end(), row + at, row + at + take);
		}
		return out;
	}

	bytes make_psd(const psd_spec& s)
	{
		builder p;
		p.raw("8BPS").be16(1).zeros(6).be16(static_cast<unsigned>(s.planes.size())).be32(s.height).be32(s.width);
		p.be16(s.depth).be16(s.mode);
		p.be32(static_cast<std::uint32_t>(s.colour_table.size())).raw(s.colour_table);
		p.be32(0);
		p.be32(0);
		p.be16(s.packbits ? 1 : 0);

		const auto row_bytes = s.depth == 1 ? (s.width + 7) / 8 : s.width;

		if (s.packbits)
		{
			std::vector<bytes> runs;
			for (const auto& plane : s.planes)
			{
				for (std::uint32_t y = 0; y < s.height; ++y) runs.push_back(packbits_row(plane.data() + y * row_bytes, row_bytes));
			}
			for (const auto& run : runs) p.be16(static_cast<unsigned>(run.size()));
			for (const auto& run : runs) p.raw(run);
		}
		else
		{
			for (const auto& plane : s.planes) p.raw(plane);
		}

		return p.b;
	}

	struct native_psd_sink final : libfiles::document_sink
	{
		libfiles::document_frame frame;
		bytes memory;
		std::vector<std::uint32_t> palette;
		std::size_t stride = 0;
		bool begin_frame(const libfiles::document_frame& value) override
		{
			frame = value;
			frame.palette = {};
			palette.assign(value.palette.begin(), value.palette.end());
			stride = libfiles::document_row_bytes(value.width, value.storage);
			memory.resize(stride * value.height);
			return true;
		}
		void row(std::uint32_t y, std::span<const std::uint8_t> bytes) override
		{
			std::copy_n(bytes.data(), stride, memory.data() + y * stride);
		}
	};

	libfiles::decode_result decode_bytes(const bytes& file, test_sink& sink)
	{
		libfiles::reader r(file);
		return libfiles::decode(r, sink);
	}
}

TEST("native PSD preserves RGB storage and refuses every incomplete plane")
{
	for (const bool packbits : {false, true})
	{
		psd_spec spec;
		spec.packbits = packbits;
		spec.planes = {{10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}};
		const auto file = make_psd(spec);
		libfiles::reader input(file);
		native_psd_sink sink;
		const auto result = libfiles::decode_document(input, libfiles::image_format::psd, sink);
		CHECK(result.success);
		CHECK(sink.frame.storage == libfiles::document_storage::bgr24);
		CHECK(sink.memory == bytes({90, 50, 10, 100, 60, 20, 0, 0, 110, 70, 30, 120, 80, 40, 0, 0}));
		for (std::size_t length = 0; length < file.size(); ++length)
		{
			libfiles::reader cut(std::span<const std::uint8_t>(file.data(), length));
			native_psd_sink failed;
			const auto rejected = libfiles::decode_document(cut, libfiles::image_format::psd, failed);
			CHECK(!rejected.success && !rejected.error.empty());
		}
	}
}

TEST("native PSD keeps palette indices grayscale and initialized bitmap pixels")
{
	psd_spec spec;
	spec.mode = 2;
	spec.planes = {{0, 1, 2, 255}};
	spec.colour_table.resize(768);
	for (unsigned i = 0; i < 256; ++i)
	{
		spec.colour_table[i] = static_cast<std::uint8_t>(i);
		spec.colour_table[i + 256] = static_cast<std::uint8_t>(255 - i);
	}
	auto file = make_psd(spec);
	libfiles::reader input(file);
	native_psd_sink indexed;
	CHECK(libfiles::decode_document(input, libfiles::image_format::psd, indexed).success);
	CHECK(indexed.frame.storage == libfiles::document_storage::indexed8);
	CHECK(indexed.palette[1] == 0xff01fe00u);
	CHECK(indexed.memory == bytes({0, 1, 0, 0, 2, 255, 0, 0}));
	spec.mode = 1;
	spec.colour_table.clear();
	file = make_psd(spec);
	libfiles::reader gray_input(file);
	native_psd_sink gray;
	CHECK(libfiles::decode_document(gray_input, libfiles::image_format::psd, gray).success);
	CHECK(gray.frame.storage == libfiles::document_storage::gray8);
	CHECK(gray.memory == indexed.memory);
	spec.mode = 0;
	spec.depth = 1;
	spec.width = 9;
	spec.height = 1;
	spec.planes = {{0x80, 0}};
	file = make_psd(spec);
	libfiles::reader bitmap_input(file);
	native_psd_sink bitmap;
	CHECK(libfiles::decode_document(bitmap_input, libfiles::image_format::psd, bitmap).success);
	CHECK(bitmap.frame.storage == libfiles::document_storage::indexed1);
	CHECK(bitmap.palette == std::vector<std::uint32_t>({0xffffffff, 0xff000000}));
	CHECK(bitmap.memory == bytes({0x80, 0, 0, 0}));
}

TEST("a psd's rgb composite decodes to bgrx, raw and packbits alike")
{
	for (const auto packbits : {false, true})
	{
		psd_spec s;
		s.planes = {{10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}};
		s.packbits = packbits;

		test_sink sink;
		const auto result = decode_bytes(make_psd(s), sink);
		CHECK(result.success);
		CHECK(result.error.empty());
		CHECK_EQ(result.frame.width, 2u);
		CHECK_EQ(result.frame.height, 2u);
		CHECK_EQ(sink.calls, 1);
		CHECK_EQ(sink.told.source_width, 2u);
		CHECK_EQ(sink.told.scale_denominator, 1u);
		CHECK(sink.pixel(0, 0) == pixel({90, 50, 10, 0}));
		CHECK(sink.pixel(1, 0) == pixel({100, 60, 20, 0}));
		CHECK(sink.pixel(1, 1) == pixel({120, 80, 40, 0}));
	}
}

TEST("a psd's channels after the third share the fourth byte")
{
	psd_spec s;
	s.width = 1;
	s.height = 1;
	s.planes = {{1}, {2}, {3}, {0x0F}, {0xF0}};

	test_sink sink;
	CHECK(decode_bytes(make_psd(s), sink).success);
	CHECK(sink.pixel(0, 0) == pixel({3, 2, 1, 0xFF}));
}

TEST("grayscale, duotone, bitmap, indexed and multichannel psds map from their one plane")
{
	psd_spec gray;
	gray.mode = 1;
	gray.planes = {{0, 128, 255, 7}};
	test_sink g;
	CHECK(decode_bytes(make_psd(gray), g).success);
	CHECK(g.pixel(1, 0) == pixel({128, 128, 128, 0}));
	CHECK(g.pixel(1, 1) == pixel({7, 7, 7, 0}));

	// A duotone's table is not read; its plane is gray.
	auto duotone = gray;
	duotone.mode = 8;
	duotone.colour_table = bytes(40, 9);
	test_sink d;
	CHECK(decode_bytes(make_psd(duotone), d).success);
	CHECK(d.pixel(0, 1) == pixel({255, 255, 255, 0}));

	// Ten pixels a row in two bytes, a set bit black.
	psd_spec bits;
	bits.mode = 0;
	bits.depth = 1;
	bits.width = 10;
	bits.height = 1;
	bits.planes = {{0b10100000, 0b01000000}};
	bits.packbits = true;
	test_sink b;
	CHECK(decode_bytes(make_psd(bits), b).success);
	CHECK(b.pixel(0, 0) == pixel({0, 0, 0, 0}));
	CHECK(b.pixel(1, 0) == pixel({255, 255, 255, 0}));
	CHECK(b.pixel(2, 0) == pixel({0, 0, 0, 0}));
	CHECK(b.pixel(9, 0) == pixel({0, 0, 0, 0}));
	CHECK(b.pixel(8, 0) == pixel({255, 255, 255, 0}));

	// Every red, then every green, then every blue.
	psd_spec indexed;
	indexed.mode = 2;
	indexed.colour_table = bytes(768, 0);
	indexed.colour_table[5] = 200;
	indexed.colour_table[256 + 5] = 100;
	indexed.colour_table[512 + 5] = 50;
	indexed.planes = {{5, 0, 0, 5}};
	test_sink i;
	CHECK(decode_bytes(make_psd(indexed), i).success);
	CHECK(i.pixel(0, 0) == pixel({50, 100, 200, 0}));
	CHECK(i.pixel(1, 0) == pixel({0, 0, 0, 0}));

	// An indexed image without a full table is not decoded.
	indexed.colour_table.resize(300);
	test_sink short_table;
	CHECK(!decode_bytes(make_psd(indexed), short_table).success);

	// Only the first of a multichannel image's channels is read, into the first byte.
	psd_spec multi;
	multi.mode = 7;
	multi.planes = {{9, 9, 9, 9}, {4, 4, 4, 4}};
	test_sink m;
	CHECK(decode_bytes(make_psd(multi), m).success);
	CHECK(m.pixel(0, 0) == pixel({9, 0, 0, 0}));
}

TEST("cmyk and lab psds convert to rgb")
{
	psd_spec cmyk;
	cmyk.mode = 4;
	cmyk.planes = {{0, 255, 0, 100}, {0, 255, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}};
	test_sink c;
	CHECK(decode_bytes(make_psd(cmyk), c).success);
	CHECK(c.pixel(0, 0) == pixel({255, 255, 255, 255}));
	CHECK(c.pixel(1, 0) == pixel({0, 0, 0, 255}));
	CHECK(c.pixel(0, 1) == pixel({0, 0, 0, 255}));
	CHECK(c.pixel(1, 1) == pixel({255, 255, 155, 255}));

	// L at 100 with a and b at 0 is white; L at 0 is black.
	psd_spec lab;
	lab.mode = 9;
	lab.planes = {{255, 0, 255, 0}, {128, 128, 128, 128}, {128, 128, 128, 128}};
	test_sink l;
	CHECK(decode_bytes(make_psd(lab), l).success);
	CHECK(l.pixel(0, 0) == pixel({255, 255, 255, 255}));
	CHECK(l.pixel(1, 0) == pixel({0, 0, 0, 255}));
}

TEST("a decode writes each row's pixels and nothing past them")
{
	psd_spec s;
	s.planes = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
	test_sink sink;
	sink.padding = 8;
	CHECK(decode_bytes(make_psd(s), sink).success);

	for (std::size_t y = 0; y < 2; ++y)
	{
		for (std::size_t i = 8; i < sink.stride; ++i) CHECK_EQ(static_cast<unsigned>(sink.memory[y * sink.stride + i]), 0xABu);
	}

	CHECK(sink.pixel(1, 1) == pixel({12, 8, 4, 0}));
}

TEST("a psd the sink refuses is reported, not decoded")
{
	psd_spec s;
	s.planes = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
	test_sink sink;
	sink.refuse = true;
	const auto result = decode_bytes(make_psd(s), sink);
	CHECK(!result.success);
	CHECK(result.refused);
	CHECK(!result.truncated);
	CHECK(result.error.empty());
}

TEST("a psd decode stops when the sink asks")
{
	psd_spec s;
	s.planes = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
	test_sink sink;
	sink.cancel_after = 2;
	const auto result = decode_bytes(make_psd(s), sink);
	CHECK(!result.success);
	CHECK(result.cancelled);
	CHECK(result.error.empty());
}

TEST("a psd cut short says so, and a malformed one does not")
{
	psd_spec s;
	s.planes = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
	const auto whole = make_psd(s);

	// Cut in the header, past the signature that names it a PSD and before the sink is asked, and in
	// the pixels after.
	for (const auto cut : {std::size_t{10}, std::size_t{20}, whole.size() - 1})
	{
		const bytes part(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut));
		test_sink sink;
		const auto result = decode_bytes(part, sink);
		CHECK(!result.success);
		CHECK(result.truncated);
		CHECK(!result.error.empty());
	}

	// A PackBits run that overfills its row is malformed, not short.
	s.packbits = true;
	auto bad_run = make_psd(s);
	bad_run[bad_run.size() - 3] = 0x7F;
	test_sink sink;
	const auto result = decode_bytes(bad_run, sink);
	CHECK(!result.success);
	CHECK(!result.truncated);
	CHECK(!result.error.empty());

	// Image resources longer than the file is malformed too: the length is checked, not read past.
	auto long_resources = make_psd(s);
	long_resources[26 + 4 + 3] = 0xFF;
	test_sink resources_sink;
	const auto resources_result = decode_bytes(long_resources, resources_sink);
	CHECK(!resources_result.success);
	CHECK(!resources_result.truncated);
}

TEST("formats libfiles does not decode say so")
{
	CHECK(libfiles::can_decode(libfiles::image_format::psd));
	CHECK(!libfiles::can_decode(libfiles::image_format::gif));
#ifdef LIBFILES_JPEG
	CHECK(libfiles::can_decode(libfiles::image_format::jpeg));
#else
	CHECK(!libfiles::can_decode(libfiles::image_format::jpeg));
#endif

	const bytes gif = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0, 0x2C, 0, 0};
	test_sink sink;
	const auto result = decode_bytes(gif, sink);
	CHECK(!result.success);
	CHECK(!result.truncated);
	CHECK(!result.error.empty());
	CHECK_EQ(sink.calls, 0);
}

TEST("the reduction for a target is the largest power of two that still covers it")
{
	CHECK_EQ(libfiles::scale_denominator_for(4000, 3000, 0, 0), 1u);
	CHECK_EQ(libfiles::scale_denominator_for(4000, 3000, 500, 375), 8u);
	CHECK_EQ(libfiles::scale_denominator_for(4000, 3000, 501, 375), 4u);
	CHECK_EQ(libfiles::scale_denominator_for(4000, 3000, 1000, 0), 4u);
	CHECK_EQ(libfiles::scale_denominator_for(4000, 3000, 0, 1500), 2u);
	CHECK_EQ(libfiles::scale_denominator_for(4000, 3000, 2001, 10), 1u);
	CHECK_EQ(libfiles::scale_denominator_for(7, 7, 1, 1), 4u);
}
