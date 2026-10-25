// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for read_exif, for summarize_exif's agreement with it, and for the EXIF writers. Each
// EXIF block is laid out here entry by entry, in both byte orders, so every value the reader reports is
// one a test put there.

#include "test.h"

#include "builders.h"
#include "libfiles/exif.h"
#include "libfiles/exif_listing.h"
#include "libfiles/scan.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <format>
#include <string>
#include <string_view>
#include <vector>

using namespace test_build;

namespace
{
	constexpr std::uint16_t type_byte = 1;
	constexpr std::uint16_t type_ascii = 2;
	constexpr std::uint16_t type_short = 3;
	constexpr std::uint16_t type_long = 4;
	constexpr std::uint16_t type_rational = 5;
	constexpr std::uint16_t type_undefined = 7;
	constexpr std::uint16_t type_srational = 10;

	// One directory entry. Its value is given as bytes already in the block's byte order, or as the
	// index of another directory, whose offset becomes the value.
	struct field
	{
		std::uint16_t tag = 0;
		std::uint16_t type = 0;
		std::uint32_t count = 0;
		bytes value;
		int directory = -1;
	};

	using directory = std::vector<field>;

	// Lays out a TIFF stream: the header, then each directory followed by the values too large for its
	// entries. Directory 0 is IFD0; ifd1 names the directory IFD0 links to.
	class tiff
	{
	public:
		explicit tiff(const bool little) : _little(little)
		{
		}

		bool little() const { return _little; }

		bytes u16(const unsigned v) const
		{
			builder b;
			_little ? b.le16(v) : b.be16(v);
			return b.b;
		}

		bytes u32(const std::uint32_t v) const
		{
			builder b;
			_little ? b.le32(v) : b.be32(v);
			return b.b;
		}

		bytes rationals(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& values) const
		{
			bytes out;
			for (const auto& [n, d] : values)
			{
				const auto a = u32(n);
				const auto b = u32(d);
				out.insert(out.end(), a.begin(), a.end());
				out.insert(out.end(), b.begin(), b.end());
			}
			return out;
		}

		field short_field(const std::uint16_t tag, const unsigned v) const
		{
			auto value = u16(v);
			value.resize(4, 0);
			return {tag, type_short, 1, value};
		}

		field rational_field(const std::uint16_t tag, const std::uint32_t n, const std::uint32_t d,
		                     const std::uint16_t type = type_rational) const
		{
			return {tag, type, 1, rationals({{n, d}})};
		}

		static field ascii_field(const std::uint16_t tag, const std::string_view text)
		{
			bytes value(text.begin(), text.end());
			value.push_back(0);
			const auto count = static_cast<std::uint32_t>(value.size());
			if (value.size() < 4) value.resize(4, 0);
			return {tag, type_ascii, count, value};
		}

		static field bytes_field(const std::uint16_t tag, const std::uint16_t type, const bytes& data)
		{
			auto value = data;
			const auto count = static_cast<std::uint32_t>(value.size());
			if (value.size() < 4) value.resize(4, 0);
			return {tag, type, count, value};
		}

		static field pointer(const std::uint16_t tag, const int directory)
		{
			return {tag, type_long, 1, {}, directory};
		}

		bytes build(const std::vector<directory>& directories, const int ifd1 = -1) const
		{
			std::vector<std::uint32_t> offsets;
			std::uint32_t pos = 8;

			for (const auto& d : directories)
			{
				offsets.push_back(pos);
				pos += static_cast<std::uint32_t>(2 + 12 * d.size() + 4);
				for (const auto& f : d)
					if (f.directory < 0 && f.value.size() > 4) pos += static_cast<std::uint32_t>((f.value.size() + 1) & ~std::size_t{1});
			}

			bytes out = _little ? bytes{'I', 'I'} : bytes{'M', 'M'};
			append(out, u16(42));
			append(out, u32(8));

			for (std::size_t i = 0; i < directories.size(); ++i)
			{
				const auto& d = directories[i];
				auto data = static_cast<std::uint32_t>(offsets[i] + 2 + 12 * d.size() + 4);
				bytes tail;

				append(out, u16(static_cast<unsigned>(d.size())));

				for (const auto& f : d)
				{
					append(out, u16(f.tag));
					append(out, u16(f.type));
					append(out, u32(f.count));

					if (f.directory >= 0)
					{
						append(out, u32(offsets[static_cast<std::size_t>(f.directory)]));
					}
					else if (f.value.size() <= 4)
					{
						append(out, f.value);
					}
					else
					{
						append(out, u32(data));
						append(tail, f.value);
						if (f.value.size() & 1) tail.push_back(0);
						data += static_cast<std::uint32_t>((f.value.size() + 1) & ~std::size_t{1});
					}
				}

				append(out, u32(i == 0 && ifd1 > 0 ? offsets[static_cast<std::size_t>(ifd1)] : 0));
				append(out, tail);
			}

			// Room past the last value, as a real block has, so nothing in the test sits at its end.
			out.resize(out.size() + 16, 0);
			return out;
		}

	private:
		static void append(bytes& out, const bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

		bool _little;
	};

	bytes utf16(const std::u16string_view text, const bool little)
	{
		bytes out;
		for (const auto c : text)
		{
			if (little)
			{
				out.push_back(static_cast<std::uint8_t>(c));
				out.push_back(static_cast<std::uint8_t>(c >> 8));
			}
			else
			{
				out.push_back(static_cast<std::uint8_t>(c >> 8));
				out.push_back(static_cast<std::uint8_t>(c));
			}
		}
		return out;
	}

	bool near(const double a, const double b) { return std::fabs(a - b) < 1e-6; }
}

TEST("exif reads the camera, the exposure and the dates, in both byte orders")
{
	for (const auto little : {true, false})
	{
		const tiff t(little);
		const directory ifd0 = {
			t.ascii_field(0x010e, "  A harbour  "),
			t.ascii_field(0x010f, "Canon"),
			t.ascii_field(0x0110, "EOS R5"),
			t.short_field(0x0112, 6),
			t.ascii_field(0x0131, "Firmware 1.8"),
			t.ascii_field(0x0132, "2024:05:06 07:08:09"),
			t.ascii_field(0x013b, "Ansel"),
			t.ascii_field(0x8298, "(c) Ansel"),
			tiff::pointer(0x8769, 1),
		};
		const directory exif = {
			t.rational_field(0x829a, 1, 250, type_srational),
			t.rational_field(0x829d, 28, 10),
			t.short_field(0x8827, 400),
			t.ascii_field(0x9003, "2024:05:06 07:08:00 "),
			t.ascii_field(0x9011, "+02:00"),
			t.ascii_field(0x9291, "125"),
			t.rational_field(0x920a, 50, 1),
			t.short_field(0xa405, 75),
			t.ascii_field(0xa434, "RF 50mm F1.8"),
		};

		const auto m = libfiles::read_exif(t.build({ifd0, exif}));

		CHECK(m.orientation == 6);
		CHECK_STR(m.camera_make, "Canon");
		CHECK_STR(m.camera_model, "EOS R5");
		CHECK_STR(m.description, "A harbour");
		CHECK_STR(m.software, "Firmware 1.8");
		CHECK_STR(m.artist, "Ansel");
		CHECK_STR(m.copyright, "(c) Ansel");
		CHECK_STR(m.lens, "RF 50mm F1.8");
		CHECK(near(m.exposure_time, 1.0 / 250));
		CHECK(near(m.f_number, 2.8));
		CHECK_EQ(m.iso_speed, 400u);
		CHECK(near(m.focal_length, 50.0));
		CHECK_EQ(m.focal_length_35mm, 75u);
		CHECK_STR(m.modified.text, "2024:05:06 07:08:09");
		CHECK_STR(m.original.text, "2024:05:06 07:08:00");
		CHECK_STR(m.original.offset, "+02:00");
		CHECK_STR(m.original.sub_second, "125");
		CHECK(m.digitized.text.empty());
		CHECK(!m.latitude);
	}
}

TEST("tiff scan reads camera exposure and dates without reading image data")
{
	for (const auto little : {true, false})
	{
		const tiff t(little);
		const directory ifd0 = {
			t.short_field(0x0100, 640),
			t.short_field(0x0101, 480),
			t.ascii_field(0x010f, "Canon"),
			t.ascii_field(0x0110, "EOS R5"),
			t.short_field(0x0112, 6),
			t.ascii_field(0x0132, "2024:05:06 07:08:09"),
			tiff::pointer(0x8769, 1),
		};
		const directory exif = {
			t.rational_field(0x829a, 1, 250),
			t.rational_field(0x829d, 28, 10),
			t.short_field(0x8827, 400),
			t.ascii_field(0x9003, "2024:05:06 07:08:00"),
			t.ascii_field(0x9004, "2024:05:06 07:08:01"),
			t.ascii_field(0x9011, "+02:00"),
			t.ascii_field(0x9291, "125"),
			t.ascii_field(0xa434, "RF 50mm F1.8"),
		};
		auto data = t.build({ifd0, exif});
		data.resize(2u * 1024u * 1024u, 0xaa);
		libfiles::memory_stream source(data);
		libfiles::reader r(source);
		const auto result = libfiles::scan(r);
		CHECK(result.success);
		CHECK_EQ(result.width, 640u);
		CHECK_EQ(result.height, 480u);
		CHECK(result.tiff_metadata.has_value());
		const auto& m = result.tiff_metadata.value();
		CHECK(m.orientation == 6);
		CHECK_STR(m.camera_make, "Canon");
		CHECK_STR(m.camera_model, "EOS R5");
		CHECK_STR(m.lens, "RF 50mm F1.8");
		CHECK(near(m.exposure_time, 1.0 / 250));
		CHECK(near(m.f_number, 2.8));
		CHECK_EQ(m.iso_speed, 400u);
		CHECK_STR(m.original.text, "2024:05:06 07:08:00");
		CHECK_STR(m.original.offset, "+02:00");
		CHECK_STR(m.original.sub_second, "125");
		CHECK_STR(m.digitized.text, "2024:05:06 07:08:01");
		CHECK_STR(m.modified.text, "2024:05:06 07:08:09");
		CHECK(result.metadata.exif.empty());
		CHECK_EQ(r.source_reads(), 1u);
		CHECK_EQ(r.source_bytes(), libfiles::reader::default_block_size);

		libfiles::reader resident(data);
		const auto in_memory = libfiles::scan(resident);
		CHECK_STR(in_memory.tiff_metadata.value().camera_model, m.camera_model);
		CHECK_STR(in_memory.tiff_metadata.value().original.text, m.original.text);
	}
}

TEST("tiff scan bounds metadata values and keeps readable fields")
{
	const tiff t(true);
	auto data = t.build({{
		t.short_field(0x0100, 640),
		t.short_field(0x0101, 480),
		t.ascii_field(0x010f, "Canon"),
		t.ascii_field(0x0110, std::string(64u * 1024u, 'x')),
		t.short_field(0x0112, 6),
	}});
	libfiles::memory_stream source(std::move(data));
	libfiles::reader r(source);
	const auto result = libfiles::scan(r);
	CHECK(result.success);
	CHECK(result.error.empty());
	CHECK_STR(result.tiff_metadata.value().camera_make, "Canon");
	CHECK(result.tiff_metadata.value().camera_model.empty());
	CHECK(result.orientation == 6);
	CHECK_EQ(r.source_reads(), 1u);
	CHECK_EQ(r.source_bytes(), libfiles::reader::default_block_size);
}

TEST("exif decodes text however a camera or Windows wrote it")
{
	for (const auto little : {true, false})
	{
		const tiff t(little);

		bytes ascii_comment = {'A', 'S', 'C', 'I', 'I', 0, 0, 0};
		for (const auto c : std::string_view("  shot at dawn\0\0", 16)) ascii_comment.push_back(static_cast<std::uint8_t>(c));

		bytes unicode_comment = {'U', 'N', 'I', 'C', 'O', 'D', 'E', 0};
		const auto text = utf16(u"caf\u00e9 \U0001F600", little);
		unicode_comment.insert(unicode_comment.end(), text.begin(), text.end());

		// XP tags are UTF-16LE whatever the block's order: an UNDEFINED one is recognised from its zero
		// bytes, and a BYTE one - as Windows writes them - is read as UTF-16LE to its NUL.
		const directory ifd0 = {
			tiff::bytes_field(0x9c9b, type_undefined, utf16(u"Title\0", true)),
			tiff::bytes_field(0x9c9e, type_byte, utf16(u"Keywords\0", true)),
			tiff::pointer(0x8769, 1),
		};
		const directory exif = {tiff::bytes_field(0x9286, type_undefined, ascii_comment)};

		const auto m = libfiles::read_exif(t.build({ifd0, exif}));
		CHECK_STR(m.comment, "shot at dawn");
		CHECK_STR(m.title, "Title");
		CHECK_STR(m.keywords, "Keywords");

		const directory unicode_exif = {tiff::bytes_field(0x9286, type_undefined, unicode_comment)};
		const auto u = libfiles::read_exif(t.build({{tiff::pointer(0x8769, 1)}, unicode_exif}));
		CHECK_STR(u.comment, "caf\xc3\xa9 \xf0\x9f\x98\x80");
	}

	const tiff t(true);

	// Samsung's binary records and control characters read as empty; bytes that are neither UTF-8
	// nor UTF-16 read as "?"; a number where text belongs reads as "?".
	const directory junk = {
		tiff::bytes_field(0x010f, type_ascii, {0x12, 0xf8, 0x0f, 0x3b, 1, 2, 3, 4}),
		tiff::bytes_field(0x0110, type_ascii, {'a', 1, 'b', 0}),
		tiff::bytes_field(0x0131, type_ascii, {0xc0, 0x80, 0xc1, 0x81, 0xf8, 0x41}),
		t.short_field(0x013b, 5),
	};
	const auto j = libfiles::read_exif(t.build({junk}));
	CHECK(j.camera_make.empty());
	CHECK(j.camera_model.empty());
	CHECK_STR(j.software, "?");
	CHECK_STR(j.artist, "?");
}

TEST("exif takes a tag's fallbacks only when the tag itself is missing")
{
	const tiff t(true);

	// APEX aperture 5 is f/5.66 and shutter speed 8 is 1/256 s; XPAuthor, XPSubject and XPComment
	// stand in for the tags Windows did not write.
	const directory ifd0 = {
		tiff::bytes_field(0x9c9c, type_undefined, utf16(u"xp comment\0", true)),
		tiff::bytes_field(0x9c9d, type_undefined, utf16(u"xp author\0", true)),
		tiff::bytes_field(0x9c9f, type_undefined, utf16(u"xp subject\0", true)),
		tiff::pointer(0x8769, 1),
	};
	const directory exif = {
		t.rational_field(0x9201, 8, 1, type_srational),
		t.rational_field(0x9202, 5, 1),
	};

	const auto m = libfiles::read_exif(t.build({ifd0, exif}));
	CHECK(near(m.f_number, std::exp(std::log(2.0) * 5 / 2)));
	CHECK(near(m.exposure_time, 1.0 / 256));
	CHECK_STR(m.artist, "xp author");
	CHECK_STR(m.description, "xp subject");
	CHECK_STR(m.comment, "xp comment");

	// With the tags themselves, the fallbacks are not taken.
	const directory both = {
		t.ascii_field(0x010e, "description"),
		t.ascii_field(0x013b, "artist"),
		tiff::pointer(0x8769, 1),
		tiff::bytes_field(0x9c9d, type_undefined, utf16(u"xp author\0", true)),
		tiff::bytes_field(0x9c9f, type_undefined, utf16(u"xp subject\0", true)),
	};
	const directory exposure = {
		t.rational_field(0x829a, 1, 60, type_srational),
		t.rational_field(0x829d, 4, 1),
		t.rational_field(0x9201, 8, 1, type_srational),
		t.rational_field(0x9202, 5, 1),
	};

	const auto n = libfiles::read_exif(t.build({both, exposure}));
	CHECK(near(n.f_number, 4.0));
	CHECK(near(n.exposure_time, 1.0 / 60));
	CHECK_STR(n.artist, "artist");
	CHECK_STR(n.description, "description");
}

TEST("exif reads windows' rating, as stars or as a percentage")
{
	const tiff t(true);
	CHECK(libfiles::read_exif(t.build({{t.short_field(0x4746, 4)}})).rating == 4);
	CHECK(libfiles::read_exif(t.build({{t.short_field(0x4746, 0xffff)}})).rating == -1);
	CHECK(libfiles::read_exif(t.build({{t.short_field(0x4749, 50)}})).rating == 3);
	CHECK(!libfiles::read_exif(t.build({{t.short_field(0x0112, 1)}})).rating);
}

TEST("exif reads where a photograph was taken, and how high and fast")
{
	for (const auto little : {true, false})
	{
		const tiff t(little);
		const directory ifd0 = {tiff::pointer(0x8825, 1)};
		const directory gps = {
			t.ascii_field(0x0001, "S"),
			{0x0002, type_rational, 3, t.rationals({{33, 1}, {51, 1}, {3540, 100}})},
			t.ascii_field(0x0003, "W"),
			{0x0004, type_rational, 3, t.rationals({{151, 1}, {12, 1}, {0, 1}})},
			tiff::bytes_field(0x0005, type_byte, {1}),
			t.rational_field(0x0006, 105, 10),
			{0x0007, type_rational, 3, t.rationals({{14, 1}, {30, 1}, {15, 1}})},
			t.ascii_field(0x000c, "N"),
			t.rational_field(0x000d, 10, 1),
			t.ascii_field(0x001d, "2024:05:06"),
		};

		const auto m = libfiles::read_exif(t.build({ifd0, gps}));
		CHECK(m.latitude && near(*m.latitude, -(33 + 51 / 60.0 + 35.4 / 3600.0)));
		CHECK(m.longitude && near(*m.longitude, -(151 + 12 / 60.0)));
		CHECK(near(m.altitude, -10.5));
		CHECK(near(m.speed, 18.52));
		CHECK_STR(m.gps_date, "2024:05:06");
		CHECK(m.gps_time && near((*m.gps_time)[0], 14) && near((*m.gps_time)[1], 30) && near((*m.gps_time)[2], 15));
	}

	const tiff t(true);

	// The equator is a place; exactly 0, 0 is a receiver with no fix; one coordinate alone is none.
	const auto equator = libfiles::read_exif(t.build({{tiff::pointer(0x8825, 1)},
	                                                  {{0x0002, type_rational, 3, t.rationals({{0, 1}, {0, 1}, {0, 1}})},
	                                                   {0x0004, type_rational, 3, t.rationals({{36, 1}, {49, 1}, {0, 1}})}}}));
	CHECK(equator.latitude && *equator.latitude == 0.0);
	CHECK(equator.longitude && near(*equator.longitude, 36 + 49 / 60.0));

	const auto no_fix = libfiles::read_exif(t.build({{tiff::pointer(0x8825, 1)},
	                                                 {{0x0002, type_rational, 3, t.rationals({{0, 1}, {0, 1}, {0, 1}})},
	                                                  {0x0004, type_rational, 3, t.rationals({{0, 1}, {0, 1}, {0, 1}})}}}));
	CHECK(!no_fix.latitude && !no_fix.longitude);

	const auto alone = libfiles::read_exif(t.build({{tiff::pointer(0x8825, 1)},
	                                                {{0x0002, type_rational, 3, t.rationals({{10, 1}, {0, 1}, {0, 1}})}}}));
	CHECK(!alone.latitude && !alone.longitude);

	// A time stamp of fewer than three rationals is not a time.
	const auto short_time = libfiles::read_exif(t.build({{tiff::pointer(0x8825, 1)},
	                                                     {{0x0007, type_rational, 2, t.rationals({{14, 1}, {30, 1}})}}}));
	CHECK(!short_time.gps_time);
}

TEST("exif names a canon lens from its maker note, unless LensModel follows")
{
	const tiff t(true);

	// CameraSettings: 28 or more SHORTs, the lens id at 22, the focal range at 23 and 24, its unit at 25.
	const auto settings = [&](const int id, const int high, const int low, const int units)
	{
		bytes values;
		for (auto i = 0; i < 30; ++i)
		{
			const auto v = i == 22 ? id : i == 23 ? high : i == 24 ? low : i == 25 ? units : 0;
			const auto s = t.u16(static_cast<unsigned>(v));
			values.insert(values.end(), s.begin(), s.end());
		}
		return field{0x0001, type_short, 30, values};
	};

	const auto read = [&](const field& camera_settings, const bool lens_model)
	{
		directory exif = {{0x927c, type_undefined, 4, {}, 2}};
		if (lens_model) exif.push_back(t.ascii_field(0xa434, "RF 24-105mm"));
		return libfiles::read_exif(t.build({{t.ascii_field(0x010f, "CANON"), tiff::pointer(0x8769, 1)}, exif, {camera_settings}}));
	};

	// Id 4 names two lenses; the first is taken.
	CHECK_STR(read(settings(4, 0, 0, 0), false).lens, "Canon EF 35-105mm f/3.5-4.5");
	CHECK_STR(read(settings(0, 200, 70, 1), false).lens, std::format("{:.1}f-{:.1}mm", 70.0, 200.0));
	CHECK_STR(read(settings(0, 50, 50, 1), false).lens, std::format("{:.1}mm", 50.0));
	CHECK_STR(read(settings(4, 0, 0, 0), true).lens, "RF 24-105mm");

	// Not Canon's camera, so not Canon's maker note.
	const auto other = libfiles::read_exif(t.build({{t.ascii_field(0x010f, "Nikon"), tiff::pointer(0x8769, 1)},
	                                               {{0x927c, type_undefined, 4, {}, 2}}, {settings(4, 0, 0, 0)}}));
	CHECK(other.lens.empty());
}

TEST("exif reads orientation from ifd0, else ifd1, and nowhere else")
{
	const tiff t(true);

	const std::vector<bytes> blocks = {
		t.build({{t.short_field(0x0112, 3)}, {t.short_field(0x0112, 8)}}, 1),
		t.build({{t.ascii_field(0x010f, "x")}, {t.short_field(0x0112, 8)}}, 1),
		t.build({{t.short_field(0x0112, 3), tiff::pointer(0x8769, 1)}, {t.short_field(0x0112, 5)}}),
		t.build({{t.ascii_field(0x010f, "x"), tiff::pointer(0x8769, 1)}, {t.short_field(0x0112, 5)}}),
		t.build({{t.ascii_field(0x010f, "x")}}),
	};

	CHECK(libfiles::read_exif(blocks[0]).orientation == 3);
	CHECK(libfiles::read_exif(blocks[1]).orientation == 8);
	// The tag belongs to IFD0: a copy in a directory IFD0 points to is not the image's.
	CHECK(libfiles::read_exif(blocks[2]).orientation == 3);
	CHECK(!libfiles::read_exif(blocks[3]).orientation);
	CHECK(!libfiles::read_exif(blocks[4]).orientation);

	// A scan and the JPEG decoder take the orientation summarize_exif reads, which is this one.
	for (const auto& b : blocks) CHECK(libfiles::summarize_exif(b).orientation == libfiles::read_exif(b).orientation);
}

TEST("exif summary and reading agree on the orientation of a damaged block")
{
	for (const auto little_endian : {true, false})
	{
		const tiff t(little_endian);
		auto block = t.build({{t.ascii_field(0x010f, "Canon"), t.short_field(0x0112, 6), tiff::pointer(0x8769, 1)},
		                      {t.short_field(0x0112, 3), t.rational_field(0x829d, 28, 10)},
		                      {t.short_field(0x0112, 8)}}, 2);
		auto readings = 0;
		auto disagreements = 0;

		for (std::size_t at = 0; at < block.size(); ++at)
		{
			const auto original = block[at];

			for (const auto value : {0, 1, 3, 8, 12, 0xff, original ^ 0xff})
			{
				block[at] = static_cast<std::uint8_t>(value);
				if (libfiles::summarize_exif(block).orientation != libfiles::read_exif(block).orientation) ++disagreements;
				++readings;
			}

			block[at] = original;
		}

		CHECK(readings > 500);
		CHECK_EQ(disagreements, 0);
	}
}

TEST("exif counts only well-formed entries, and reads a damaged block as far as it can")
{
	const tiff t(true);

	// An unknown type, DOUBLE, no values, or values past the end are not counted.
	const directory ifd0 = {
		{0x010f, 0xfc, 2, {'a', 0, 0, 0}},
		{0x0110, 12, 1, t.rationals({{1, 1}})},
		{0x0112, type_short, 0, {6, 0, 0, 0}},
		{0x0131, type_ascii, 0x100000, {8, 0, 0, 0}},
		t.ascii_field(0x013b, "ok"),
	};
	const auto m = libfiles::read_exif(t.build({ifd0}));
	CHECK(m.camera_make.empty());
	CHECK(m.camera_model.empty());
	CHECK(!m.orientation);
	CHECK(!libfiles::summarize_exif(t.build({ifd0})).orientation);
	CHECK(m.software.empty());
	CHECK_STR(m.artist, "ok");

	// A directory that points back at itself, or at IFD0, is read once.
	const auto loop = libfiles::read_exif(t.build({{t.ascii_field(0x010f, "make"), tiff::pointer(0x8769, 1)},
	                                              {tiff::pointer(0x8769, 1), tiff::pointer(0xa005, 0), t.short_field(0x8827, 200)}}));
	CHECK_STR(loop.camera_make, "make");
	CHECK_EQ(loop.iso_speed, 200u);

	// Not TIFF, or too short to be: nothing.
	CHECK(libfiles::read_exif(to_bytes("not a tiff at all, really")).camera_make.empty());
	CHECK(!libfiles::read_exif(bytes{'I', 'I', 42, 0}).orientation);

	// Cut anywhere, a block reads without fault, and keeps what lies before the cut.
	const auto whole = t.build({{t.ascii_field(0x010f, "Canon"), t.short_field(0x0112, 6), tiff::pointer(0x8769, 1)},
	                            {t.rational_field(0x829d, 28, 10), t.ascii_field(0x9003, "2024:05:06 07:08:09")}});
	for (std::size_t cut = 0; cut <= whole.size(); ++cut)
	{
		const auto part = libfiles::read_exif({whole.data(), cut});
		if (part.camera_make.size()) CHECK_STR(part.camera_make, "Canon");
	}
	CHECK(near(libfiles::read_exif(whole).f_number, 2.8));
}

TEST("exif written fresh reads back as written")
{
	libfiles::exif_fields f;
	f.orientation = std::uint16_t{6};
	f.f_number = 2.8;
	f.exposure_time = 1.0 / 60.0;
	f.iso_speed = std::uint16_t{400};
	f.focal_length = 50.0;
	f.focal_length_35mm = std::uint16_t{75};
	f.camera_make = "Canon";
	f.camera_model = "EOS R5";
	f.artist = "AB";
	f.lens = "RF 50mm F1.2L";
	f.copyright = "(c) 2026";
	f.description = "A test";
	f.comment = "a comment";
	f.original = "2024:05:06 07:08:09";
	f.modified = "2024:05:07 07:08:09";
	f.digitized = "2024:05:06 07:08:10";

	const auto m = libfiles::read_exif(libfiles::make_exif(f));
	CHECK(m.orientation == 6);
	CHECK(near(m.f_number, 2.8));
	CHECK(near(m.exposure_time, 1.0 / 60.0));
	CHECK_EQ(m.iso_speed, 400u);
	CHECK(near(m.focal_length, 50.0));
	CHECK_EQ(m.focal_length_35mm, 75u);
	CHECK_STR(m.camera_make, "Canon");
	CHECK_STR(m.camera_model, "EOS R5");
	CHECK_STR(m.artist, "AB");
	CHECK_STR(m.lens, "RF 50mm F1.2L");
	CHECK_STR(m.copyright, "(c) 2026");
	CHECK_STR(m.description, "A test");
	CHECK_STR(m.comment, "a comment");
	CHECK_STR(m.original.text, "2024:05:06 07:08:09");
	CHECK_STR(m.modified.text, "2024:05:07 07:08:09");
	CHECK_STR(m.digitized.text, "2024:05:06 07:08:10");

	// A long exposure is a rational to four places; one too long for a rational is not written.
	libfiles::exif_fields slow;
	slow.exposure_time = 2.5;
	slow.f_number = 1e9;
	const auto s = libfiles::read_exif(libfiles::make_exif(slow));
	CHECK(near(s.exposure_time, 2.5));
	CHECK(near(s.f_number, 0.0));

	// Nothing set: only the entries every block carries.
	const auto empty = libfiles::read_exif(libfiles::make_exif({}));
	CHECK(!empty.orientation);
	CHECK(empty.camera_make.empty());
	CHECK(near(empty.f_number, 0.0));
}

namespace
{
	std::uint32_t read_number(const bytes& b, const bool little, const std::size_t at, const int size)
	{
		std::uint32_t v = 0;

		for (int i = 0; i < size; ++i)
		{
			v = v << 8 | b[at + static_cast<std::size_t>(little ? size - 1 - i : i)];
		}

		return v;
	}

	// The value field of the entry with the tag in the directory at offset, read as a number.
	std::optional<std::uint32_t> field_value(const bytes& b, const bool little, const std::uint32_t directory,
	                                         const std::uint16_t tag, const int size)
	{
		const auto count = read_number(b, little, directory, 2);

		for (std::uint32_t i = 0; i < count; ++i)
		{
			const auto at = directory + 2 + 12 * i;
			if (read_number(b, little, at, 2) == tag) return read_number(b, little, at + 8, size);
		}

		return {};
	}
}

TEST("exif reads a number's first value where it lies, at its type's width")
{
	for (const auto little : {true, false})
	{
		const tiff t(little);
		const auto shorts = [&t](const std::initializer_list<unsigned> values)
		{
			bytes out;
			for (const auto v : values)
			{
				const auto b = t.u16(v);
				out.insert(out.end(), b.begin(), b.end());
			}
			return out;
		};

		// ISOSpeedRatings may hold several values; four lie beyond the entry, whose field says where.
		const auto several = t.build({
			{tiff::pointer(0x8769, 1)},
			{{0x8827, type_short, 4, shorts({100, 200, 300, 400})}, {0xa405, type_short, 2, shorts({35, 70})}},
		});
		const auto s = libfiles::read_exif(several);
		CHECK_EQ(s.iso_speed, 100u);
		CHECK_EQ(s.focal_length_35mm, 35u);

		// A LONG, whose value in big-endian order lies in the field's last two bytes.
		const auto longs = t.build({
			{tiff::pointer(0x8769, 1)},
			{{0x8827, type_long, 1, t.u32(400)}, {0xa405, type_long, 1, t.u32(50)}},
		});
		const auto l = libfiles::read_exif(longs);
		CHECK_EQ(l.iso_speed, 400u);
		CHECK_EQ(l.focal_length_35mm, 50u);
	}
}

TEST("exif numbers are overwritten where they stand, and nothing else changes")
{
	using libfiles::exif_directory;

	for (const auto little : {true, false})
	{
		const tiff t(little);
		const auto original = t.build({
			{t.short_field(0x0112, 6), tiff::pointer(0x8769, 1)},
			{
				{0xa002, type_long, 1, t.u32(4000)},
				t.short_field(0xa003, 3000),
				tiff::pointer(0xa005, 2),
				tiff::bytes_field(0x927c, type_undefined, to_bytes("maker note, offsets inside")),
				t.rational_field(0x829d, 28, 10),
			},
			{t.short_field(0x1001, 4000), {0x1002, type_long, 1, t.u32(3000)}},
			{t.short_field(0x0112, 8)},
		}, 3);

		auto block = original;
		const libfiles::exif_number numbers[] = {
			{exif_directory::primary, 0x0112, 1},
			{exif_directory::thumbnail, 0x0112, 1},
			{exif_directory::exif, 0xa002, 300},
			{exif_directory::exif, 0xa003, 70000},
			{exif_directory::interoperability, 0x1001, 300},
			{exif_directory::interoperability, 0x1002, 200},
			// A rational is not a number this writes, a directory the block lacks has nothing to write,
			// and an entry the block lacks is not added.
			{exif_directory::exif, 0x829d, 5},
			{exif_directory::gps, 0x0001, 1},
			{exif_directory::primary, 0x0100, 7},
		};

		CHECK_EQ(libfiles::set_exif_numbers(block, numbers), 6u);
		CHECK_EQ(block.size(), original.size());

		const auto exif = *field_value(block, little, 8, 0x8769, 4);
		const auto interop = *field_value(block, little, exif, 0xa005, 4);
		const auto ifd1 = read_number(block, little, 8 + 2 + 12 * 2, 4);

		CHECK(field_value(block, little, 8, 0x0112, 2) == 1u);
		CHECK(field_value(block, little, ifd1, 0x0112, 2) == 1u);
		CHECK(field_value(block, little, exif, 0xa002, 4) == 300u);
		// Cut to sixteen bits for a SHORT, as libexif cut it.
		CHECK(field_value(block, little, exif, 0xa003, 2) == 70000u % 65536u);
		CHECK(field_value(block, little, interop, 0x1001, 2) == 300u);
		CHECK(field_value(block, little, interop, 0x1002, 4) == 200u);
		CHECK(!field_value(block, little, 8, 0x0100, 2));
		CHECK(near(libfiles::read_exif(block).f_number, 2.8));

		// Only the six values' bytes changed: the maker note and everything else are as they were.
		std::size_t changed = 0;
		for (std::size_t i = 0; i < block.size(); ++i)
			if (block[i] != original[i]) ++changed;
		CHECK(changed > 0 && changed <= 2 + 2 + 4 + 2 + 2 + 4);
	}

	// Not TIFF: nothing to find, and nothing written.
	auto not_tiff = to_bytes("not a tiff at all, really");
	const auto before = not_tiff;
	const libfiles::exif_number orientation[] = {{exif_directory::primary, 0x0112, 1}};
	CHECK_EQ(libfiles::set_exif_numbers(not_tiff, orientation), 0u);
	CHECK(not_tiff == before);
}

TEST("exif numbers overwrite every entry with their tag, so the block reads back as written")
{
	using libfiles::exif_directory;

	for (const auto little : {true, false})
	{
		const tiff t(little);
		// A block that repeats its Orientation, and its Exif directory its PixelYDimension.
		auto block = t.build({
			{t.short_field(0x0112, 6), t.short_field(0x0112, 6), tiff::pointer(0x8769, 1)},
			{t.short_field(0xa003, 3000), {0xa003, type_long, 1, t.u32(3000)}},
		});
		CHECK(libfiles::read_exif(block).orientation == 6);

		const libfiles::exif_number numbers[] = {
			{exif_directory::primary, 0x0112, 1},
			{exif_directory::exif, 0xa003, 400},
		};

		CHECK_EQ(libfiles::set_exif_numbers(block, numbers), 4u);
		// The reader takes the last Orientation in IFD0, so the writer has to reach it.
		CHECK(libfiles::read_exif(block).orientation == 1);
		const auto exif = *field_value(block, little, 8, 0x8769, 4);
		CHECK_EQ(read_number(block, little, exif + 2 + 8, 2), 400u);
		CHECK_EQ(read_number(block, little, exif + 2 + 12 + 8, 4), 400u);
	}
}

TEST("exif's thumbnail directory is unlinked where it stands, and nothing else changes")
{
	for (const auto little : {true, false})
	{
		const tiff t(little);
		const auto original = t.build({
			{t.short_field(0x0112, 1), tiff::pointer(0x8769, 1)},
			{t.short_field(0xa002, 4000)},
			{t.short_field(0x0112, 8)},
		}, 2);

		// IFD0's Orientation is the picture's, whatever IFD1 says of the thumbnail.
		auto block = original;
		CHECK(libfiles::read_exif(block).orientation == 1);
		CHECK(libfiles::unlink_exif_thumbnail(block));

		const auto link = 8 + 2 + 12 * 2;
		CHECK_EQ(read_number(block, little, link, 4), 0u);
		std::size_t changed = 0;
		for (std::size_t i = 0; i < block.size(); ++i)
			if (block[i] != original[i]) ++changed;
		CHECK(changed > 0 && changed <= 4);
		CHECK(libfiles::read_exif(block).orientation == 1);

		// With none in IFD0, IFD1's is read - until IFD1 is unlinked.
		const auto thumbnail_only = t.build({{tiff::pointer(0x8769, 1)}, {t.short_field(0xa002, 4000)},
		                                     {t.short_field(0x0112, 8)}}, 2);
		auto unlinked = thumbnail_only;
		CHECK(libfiles::read_exif(unlinked).orientation == 8);
		CHECK(libfiles::unlink_exif_thumbnail(unlinked));
		CHECK(!libfiles::read_exif(unlinked).orientation);

		// IFD0 cut short of its link, and IFD0 inside the header, are refused, and nothing is written.
		auto cut = bytes(original.begin(), original.begin() + link + 3);
		const auto cut_before = cut;
		CHECK(!libfiles::unlink_exif_thumbnail(cut));
		CHECK(cut == cut_before);

		auto inside = original;
		inside[little ? 4 : 7] = 4;
		CHECK(!libfiles::unlink_exif_thumbnail(inside));
	}

	auto not_tiff = to_bytes("not a tiff at all, really");
	CHECK(!libfiles::unlink_exif_thumbnail(not_tiff));
}

TEST("exif written fresh is laid out as libexif lays out a new block")
{
	// IFD0 at 8 with the four entries libexif makes mandatory and the Exif pointer, sorted by tag, its
	// two resolutions after it; then the Exif directory with its six.
	const bytes expected = {
		'I', 'I', 42, 0, 8, 0, 0, 0,
		5, 0,
		0x1a, 0x01, 5, 0, 1, 0, 0, 0, 74, 0, 0, 0,
		0x1b, 0x01, 5, 0, 1, 0, 0, 0, 82, 0, 0, 0,
		0x28, 0x01, 3, 0, 1, 0, 0, 0, 2, 0, 0, 0,
		0x13, 0x02, 3, 0, 1, 0, 0, 0, 1, 0, 0, 0,
		0x69, 0x87, 4, 0, 1, 0, 0, 0, 90, 0, 0, 0,
		0, 0, 0, 0,
		72, 0, 0, 0, 1, 0, 0, 0,
		72, 0, 0, 0, 1, 0, 0, 0,
		6, 0,
		0x00, 0x90, 7, 0, 4, 0, 0, 0, '0', '2', '1', '0',
		0x01, 0x91, 7, 0, 4, 0, 0, 0, 1, 2, 3, 0,
		0x00, 0xa0, 7, 0, 4, 0, 0, 0, '0', '1', '0', '0',
		0x01, 0xa0, 3, 0, 1, 0, 0, 0, 0xff, 0xff, 0, 0,
		0x02, 0xa0, 4, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		0x03, 0xa0, 4, 0, 1, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0,
	};

	CHECK(libfiles::make_exif({}) == expected);

	// A short text lies in its entry; a longer one after the directory, padded to an even length.
	libfiles::exif_fields f;
	f.camera_make = "LG";
	f.camera_model = "abcd";
	const auto b = libfiles::make_exif(f);
	CHECK_EQ(b.size(), expected.size() + 24u + 6u);
	CHECK(b[8] == 7);
	const bytes make_tag = {0x0f, 0x01};
	const auto make = std::search(b.begin(), b.end(), make_tag.begin(), make_tag.end());
	CHECK(make != b.end() && std::equal(make + 8, make + 12, bytes{'L', 'G', 0, 0}.begin()));
}

namespace
{
	// IFD0 with the make and orientation; the Exif directory with an exposure, an unknown tag, a DOUBLE
	// and a Canon maker note of one entry; IFD1 with a compression and a four-byte thumbnail.
	bytes listed_block(const tiff& t)
	{
		bytes note;
		const auto append = [](bytes& out, const bytes& more) { out.insert(out.end(), more.begin(), more.end()); };
		append(note, t.u16(1));
		append(note, t.u16(6));
		append(note, t.u16(2));
		append(note, t.u32(4));
		append(note, bytes{'I', 'M', 'G', 0});
		append(note, t.u32(0));
		const auto build = [&](const std::uint32_t thumbnail)
		{
			return t.build({
				{t.ascii_field(0x010f, "Canon"), t.short_field(0x0112, 6), tiff::pointer(0x8769, 1)},
				{
					t.rational_field(0x829a, 1, 60),
					t.rational_field(0x829d, 28, 10),
					t.short_field(0x8827, 400),
					t.short_field(0xfde8, 0x1234),
					{0xfde9, 12, 1, bytes(8, 0)},
					tiff::bytes_field(0x927c, type_undefined, note),
				},
				{t.short_field(0x0103, 6), {0x0201, type_long, 1, t.u32(thumbnail)}, {0x0202, type_long, 1, t.u32(4)}},
			}, 2);
		};

		// The thumbnail lies in the room the builder leaves after the last value.
		auto b = build(0);
		const auto at = static_cast<std::uint32_t>(b.size() - 16);
		b = build(at);
		const bytes jpeg = {0xff, 0xd8, 0xff, 0xd9};
		std::copy(jpeg.begin(), jpeg.end(), b.begin() + at);
		return b;
	}

	const libfiles::exif_listing_entry* listed(const libfiles::exif_listing& l, const libfiles::exif_directory d,
	                                           const std::uint16_t tag)
	{
		for (const auto& directory : l.directories)
		{
			if (directory.directory != d) continue;
			for (const auto& e : directory.entries)
				if (e.tag == tag) return &e;
		}

		return nullptr;
	}

	// A Canon maker note whose first settings array has five parts libexif's table names - the first, its
	// macro mode, set to normal - and a sixth it does not.
	bytes canon_settings_block(const tiff& t)
	{
		const auto build = [&](const std::uint32_t at)
		{
			bytes note;
			const auto append = [&note](const bytes& more) { note.insert(note.end(), more.begin(), more.end()); };
			append(t.u16(1));
			append(t.u16(1));
			append(t.u16(3));
			append(t.u32(7));
			append(t.u32(at));
			append(t.u32(0));
			// The array's size in bytes, then its six parts.
			for (const unsigned v : {14u, 2u, 0u, 0u, 0u, 0u, 7u}) append(t.u16(v));
			return t.build({
				{tiff::ascii_field(0x010f, "Canon"), tiff::pointer(0x8769, 1)},
				{tiff::bytes_field(0x927c, type_undefined, note), t.short_field(0xfde8, 1)},
			});
		};

		// The array lies 18 bytes into the note, wherever the builder put the note: its offset is from the
		// TIFF header, where the block starts.
		auto b = build(0);
		bytes pattern;
		for (const auto& part : {t.u16(1), t.u16(1), t.u16(3), t.u32(7)}) pattern.insert(pattern.end(), part.begin(), part.end());
		const auto found = std::search(b.begin(), b.end(), pattern.begin(), pattern.end());
		return build(static_cast<std::uint32_t>(found - b.begin()) + 18);
	}
}

TEST("exif listing names and renders every entry, and decodes the maker note")
{
	using libfiles::exif_directory;

	for (const auto little : {true, false})
	{
		const tiff t(little);
		const auto block = listed_block(t);
		const auto l = libfiles::list_exif(block);

		CHECK(l.little_endian == little);
		CHECK_EQ(l.directories.size(), 3u);
		CHECK(l.directories.size() == 3 && l.directories[0].directory == exif_directory::primary &&
			l.directories[1].directory == exif_directory::thumbnail && l.directories[2].directory == exif_directory::exif);

		const auto* const make = listed(l, exif_directory::primary, 0x010f);
		CHECK(make && make->name == "Make" && make->value == "Canon" && make->type == 2 && make->count == 6);
		const auto* const orientation = listed(l, exif_directory::primary, 0x0112);
		CHECK(orientation && orientation->name == "Orientation" && orientation->value == "Right-top");

		const auto* const exposure = listed(l, exif_directory::exif, 0x829a);
		CHECK(exposure && exposure->name == "ExposureTime" && exposure->value == "1/60 sec.");
		const auto* const f_number = listed(l, exif_directory::exif, 0x829d);
		CHECK(f_number && f_number->value == "f/2.8");
		const auto* const iso = listed(l, exif_directory::exif, 0x8827);
		CHECK(iso && iso->name == "ISOSpeedRatings" && iso->value == "400");
		const auto* const unknown = listed(l, exif_directory::exif, 0xfde8);
		CHECK(unknown && !unknown->name && unknown->value == "4660");
		const auto* const real = listed(l, exif_directory::exif, 0xfde9);
		CHECK(real && real->type == 12 && real->value == "8 bytes unsupported data type" && real->bytes.size() == 8);

		// The pointers are structure, not entries.
		CHECK(!listed(l, exif_directory::primary, 0x8769));
		CHECK(!listed(l, exif_directory::thumbnail, 0x0201));

		CHECK_EQ(l.maker_note.size(), 1u);
		CHECK(!l.maker_note.empty() && l.maker_note[0].id == 6 && l.maker_note[0].name == "ImageType" &&
			l.maker_note[0].value == "IMG");

		CHECK(l.thumbnail == bytes({0xff, 0xd8, 0xff, 0xd9}));

		// The same block behind its signature lists the same.
		bytes signed_block = {'E', 'x', 'i', 'f', 0, 0};
		signed_block.insert(signed_block.end(), block.begin(), block.end());
		const auto s = libfiles::list_exif(signed_block);
		CHECK(s.directories.size() == l.directories.size() && s.maker_note.size() == l.maker_note.size() &&
			s.thumbnail == l.thumbnail);
	}

	CHECK_STR(std::string(libfiles::exif_type_name(3)), "Short");
	CHECK_STR(std::string(libfiles::exif_type_name(5)), "Rational");
	CHECK_STR(std::string(libfiles::exif_type_name(2)), "ASCII");
	CHECK_STR(std::string(libfiles::exif_type_name(12)), "Double");
	CHECK(!libfiles::exif_type_name(13));
}

TEST("exif listing gives libexif's titles and descriptions, by the entry's directory")
{
	using libfiles::exif_directory;

	for (const auto little : {true, false})
	{
		const tiff t(little);
		const auto l = libfiles::list_exif(listed_block(t));

		const auto* const make = listed(l, exif_directory::primary, 0x010f);
		CHECK(make && make->title == "Manufacturer");
		CHECK(make && make->description ==
			"The manufacturer of the recording equipment. This is the manufacturer of the DSC, scanner, video "
			"digitizer or other equipment that generated the image. When the field is left blank, it is "
			"treated as unknown.");
		const auto* const exposure = listed(l, exif_directory::exif, 0x829a);
		CHECK(exposure && exposure->title == "Exposure Time" &&
			exposure->description == "Exposure time, given in seconds (sec).");
		const auto* const unknown = listed(l, exif_directory::exif, 0xfde8);
		CHECK(unknown && !unknown->title && !unknown->description);

		CHECK(!l.maker_note.empty() && l.maker_note[0].title == "Image Type" && l.maker_note[0].description == "");

		// 0x0001 is the latitude's hemisphere in the GPS directory and the rule in the
		// Interoperability one; an entry is titled for the directory it is in.
		const auto block = t.build({
			{tiff::pointer(0x8825, 1), tiff::pointer(0x8769, 2)},
			{tiff::ascii_field(0x0001, "N")},
			{tiff::pointer(0xa005, 3)},
			{tiff::ascii_field(0x0001, "R98"), tiff::ascii_field(0x0002, "0100")},
		});
		const auto g = libfiles::list_exif(block);
		const auto* const hemisphere = listed(g, exif_directory::gps, 0x0001);
		CHECK(hemisphere && hemisphere->name == "GPSLatitudeRef" && hemisphere->title == "North or South Latitude");
		const auto* const rule = listed(g, exif_directory::interoperability, 0x0001);
		CHECK(rule && rule->name == "InteroperabilityIndex" && rule->title == "Interoperability Index");
		const auto* const version = listed(g, exif_directory::interoperability, 0x0002);
		CHECK(version && version->title == "Interoperability Version" && version->description == "");
	}
}

TEST("exif listing leaves out what libexif's defaults ignored, when asked to")
{
	using libfiles::exif_directory;

	for (const auto little : {true, false})
	{
		const tiff t(little);
		const auto block = canon_settings_block(t);
		const auto all = libfiles::list_exif(block);
		const auto known = libfiles::list_exif(block, {.ignore_unknown_tags = true});

		// The unknown entry is listed, without a name, unless unknown tags are ignored.
		const auto* const unknown = listed(all, exif_directory::exif, 0xfde8);
		CHECK(unknown && !unknown->name);
		CHECK(!listed(known, exif_directory::exif, 0xfde8));
		CHECK(listed(known, exif_directory::exif, 0x927c));

		// Every part of the settings is listed either way; the part Canon's table does not know is named
		// for its array only while unknown tags are not ignored.
		CHECK_EQ(all.maker_note.size(), 6u);
		CHECK_EQ(known.maker_note.size(), 6u);

		if (all.maker_note.size() == 6 && known.maker_note.size() == 6)
		{
			CHECK(all.maker_note[0].name == "Macro Mode" && all.maker_note[0].value == "Normal");
			CHECK(known.maker_note[0].name == "Macro Mode" && known.maker_note[0].title == "Macro Mode");
			CHECK(all.maker_note[5].name == "Settings1" && all.maker_note[5].title == "Settings (First Part)" &&
				all.maker_note[5].value == "0x0007");
			CHECK(!known.maker_note[5].name && !known.maker_note[5].title && known.maker_note[5].value == "0x0007");
		}
	}

	// A directory with nothing left in it is not listed.
	const tiff t(true);
	const auto only_unknown = t.build({{t.short_field(0xfde8, 1)}});
	CHECK_EQ(libfiles::list_exif(only_unknown).directories.size(), 1u);
	CHECK(libfiles::list_exif(only_unknown, {.ignore_unknown_tags = true}).directories.empty());
}

TEST("exif listing reads a damaged block as far as libexif read it")
{
	const tiff t(true);
	const auto block = listed_block(t);

	for (std::size_t cut = 0; cut <= block.size(); ++cut)
	{
		const auto part = libfiles::list_exif({block.data(), cut});
		for (const auto& d : part.directories) CHECK(!d.entries.empty());
	}

	CHECK(libfiles::list_exif(to_bytes("not a tiff at all, really")).directories.empty());
	CHECK(libfiles::list_exif({}).directories.empty());
}
