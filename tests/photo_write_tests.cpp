// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for writing photo metadata: the TIFF directory editor, IPTC and Photoshop resources,
// and JPEG, TIFF and PSD files read back through the scanner after an update.

#include "test.h"

#include "builders.h"
#include "interruptions.h"
#include "legacy.h"
#include "libfiles/iptc.h"
#include "libfiles/write.h"

#include <cstring>
#include <format>
#include <functional>
#include <optional>

using namespace test_build;

namespace
{
	using libfiles::detail::tiff_editor;
	using directory = tiff_editor::directory;

	// A little-endian TIFF stream: IFD0 with an orientation, a rating, a long description and a
	// pointer to an Exif directory holding DateTimeOriginal, then IFD1.
	bytes make_tiff_stream()
	{
		builder t;
		t.raw("II").le16(42).le32(8);

		// IFD0 at 8: four entries, then the IFD1 pointer.
		const std::uint32_t description_at = 8 + 2 + 4 * 12 + 4;
		const std::uint32_t exif_at = description_at + 24;
		t.le16(4);
		t.le16(0x010E).le16(2).le32(24).le32(description_at);
		t.le16(0x0112).le16(3).le32(1).le16(6).le16(0);
		t.le16(0x4746).le16(3).le32(1).le16(1).le16(0);
		t.le16(0x8769).le16(4).le32(1).le32(exif_at);
		t.le32(0);
		t.raw(std::string_view("a description of 23 ch\0\0", 24));

		// The Exif directory: DateTimeOriginal out of line.
		const std::uint32_t date_at = exif_at + 2 + 12 + 4;
		t.le16(1);
		t.le16(0x9003).le16(2).le32(20).le32(date_at);
		t.le32(0);
		t.raw(std::string_view("2001:02:03 04:05:06\0", 20));
		t.zeros(4);
		return t.b;
	}

	std::uint16_t le16(const bytes& b, const std::size_t at) { return libfiles::detail::load_le16(b.data() + at); }
	std::uint32_t le32(const bytes& b, const std::size_t at) { return libfiles::detail::load_le32(b.data() + at); }

	// A little-endian stream's value for a tag, from IFD0 or the directory a pointer tag names.
	std::optional<bytes> tiff_value(const bytes& t, const std::uint16_t tag, const std::uint16_t via = 0)
	{
		std::uint32_t ifd = le32(t, 4);

		if (via != 0)
		{
			const auto pointer = tiff_value(t, via);
			if (!pointer) return {};
			ifd = le32(*pointer, 0);
		}

		const auto count = le16(t, ifd);

		for (std::uint32_t i = 0; i < count; ++i)
		{
			const auto e = ifd + 2 + 12 * i;
			if (le16(t, e) != tag) continue;

			const auto type = le16(t, e + 2);
			const auto n = le32(t, e + 4);
			const std::size_t unit = type == 3 ? 2 : type == 4 ? 4 : type == 5 ? 8 : 1;
			const auto size = unit * n;
			const auto at = size <= 4 ? e + 8 : le32(t, e + 8);
			return bytes(t.begin() + at, t.begin() + at + size);
		}

		return {};
	}

	std::string ascii(const std::optional<bytes>& v)
	{
		if (!v) return "(absent)";
		auto s = to_string(*v);
		if (!s.empty() && s.back() == '\0') s.pop_back();
		return s;
	}

	bytes edited(const bytes& stream, const std::function<void(tiff_editor&)>& edit, bool* in_place = nullptr)
	{
		libfiles::reader r(stream);
		tiff_editor editor;
		if (!editor.open(r)) return {};
		edit(editor);
		const auto changes = editor.changes(stream.size());
		if (in_place) *in_place = std::ranges::all_of(changes, [](const auto& c) { return c.length == c.data.size(); });
		return libfiles::detail::apply_changes(stream, changes);
	}

	void jpeg_segment(builder& j, const unsigned marker, const bytes& payload)
	{
		j.u8(0xFF).u8(marker).be16(static_cast<unsigned>(payload.size() + 2)).raw(payload);
	}

	bytes make_jpeg(const bool with_exif)
	{
		builder j;
		j.u8(0xFF).u8(0xD8);
		jpeg_segment(j, 0xE0, builder().raw(std::string_view("JFIF\0", 5)).zeros(9).b);
		if (with_exif) jpeg_segment(j, 0xE1, builder().raw(std::string_view("Exif\0\0", 6)).raw(make_tiff_stream()).b);
		jpeg_segment(j, 0xC0, builder().u8(8).be16(20).be16(30).u8(1).u8(1).u8(0x11).u8(0).b);
		jpeg_segment(j, 0xDA, builder().u8(1).zeros(5).b);
		j.zeros(16).u8(0xFF).u8(0xD9);
		return j.b;
	}

	libfiles::scan_result scan_bytes(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::scan(r);
	}

	bytes update(const bytes& file, const libfiles::metadata_update& u, libfiles::write_plan* plan_out = nullptr)
	{
		libfiles::reader r(file);
		auto plan = libfiles::plan_update(r, u);
		libfiles::memory_stream out;
		if (plan.ok()) libfiles::write_updated(r, plan, out);
		if (plan_out) *plan_out = std::move(plan);
		return out.bytes();
	}

	std::string iptc_value(const bytes& iptc, const std::uint8_t number)
	{
		for (const auto& d : libfiles::detail::parse_iptc(iptc))
		{
			if (d.record == 2 && d.number == number) return to_string(d.data);
		}

		return "(absent)";
	}

	std::string md5_hex(const std::string_view text)
	{
		const auto digest = libfiles::detail::md5(to_bytes(text));
		std::string hex;
		for (const auto b : digest) hex += std::format("{:02x}", b);
		return hex;
	}
}

namespace
{
	// A value as a reader that stops at a zero takes it.
	std::string c_string(const std::optional<bytes>& v)
	{
		if (!v) return "(absent)";
		const auto s = to_string(*v);
		return s.substr(0, s.find('\0'));
	}

	// A TIFF file: IFD0 with the image's size, its strip and a 24-byte description out of line, then
	// the strip at 86.
	bytes make_tiff_file()
	{
		builder t;
		t.raw("II").le16(42).le32(8);

		const std::uint32_t description_at = 8 + 2 + 4 * 12 + 4;
		const std::uint32_t strip_at = description_at + 24;
		t.le16(4);
		t.le16(0x0100).le16(3).le32(1).le16(64).le16(0);
		t.le16(0x0101).le16(3).le32(1).le16(32).le16(0);
		t.le16(0x010E).le16(2).le32(24).le32(description_at);
		t.le16(0x0111).le16(4).le32(1).le32(strip_at);
		t.le32(0);
		t.raw(std::string_view("a description of 23 ch\0\0", 24));
		t.raw(bytes(100, 0x55));
		return t.b;
	}

	std::string packet_of(const bytes& file)
	{
		return to_string(scan_bytes(file).metadata.xmp);
	}
}

TEST("md5 matches the RFC 1321 test suite")
{
	CHECK_STR(md5_hex(""), "d41d8cd98f00b204e9800998ecf8427e");
	CHECK_STR(md5_hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
	CHECK_STR(md5_hex("abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b");
	CHECK_STR(md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
	          "57edf4a22be3c955ac49da2e2107b67a");
}

TEST("a tiff value that fits is patched where it lies")
{
	const auto stream = make_tiff_stream();
	auto in_place = false;

	const auto result = edited(stream, [](tiff_editor& e)
	{
		e.set_short(directory::ifd0, 0x4746, 4);
		e.set_ascii(directory::ifd0, 0x010E, "shorter");
		e.set_ascii(directory::exif, 0x9003, "2020:01:01 00:00:00");
	}, &in_place);

	CHECK(in_place);
	CHECK_EQ(result.size(), stream.size());
	CHECK_EQ(le16(*tiff_value(result, 0x4746), 0), 4u);
	CHECK_STR(ascii(tiff_value(result, 0x010E)), "shorter");
	CHECK_STR(ascii(tiff_value(result, 0x9003, 0x8769)), "2020:01:01 00:00:00");
	CHECK_EQ(le16(*tiff_value(result, 0x0112), 0), 6u);
}

TEST("a tiff value that does not fit is appended, and nothing moves")
{
	const auto stream = make_tiff_stream();
	auto in_place = true;

	const auto result = edited(stream, [](tiff_editor& e)
	{
		e.set_ascii(directory::ifd0, 0x010E, "a description far longer than the twenty-four bytes it had");
	}, &in_place);

	CHECK(!in_place);
	CHECK(result.size() > stream.size());
	CHECK(std::memcmp(result.data() + 4, stream.data() + 4, 4) == 0); // IFD0 did not move
	CHECK(std::memcmp(result.data() + 64, stream.data() + 64, stream.size() - 64) == 0);
	CHECK_STR(ascii(tiff_value(result, 0x010E)), "a description far longer than the twenty-four bytes it had");
}

TEST("a tiff directory that gains an entry is appended and repointed")
{
	const auto stream = make_tiff_stream();

	const auto result = edited(stream, [](tiff_editor& e)
	{
		e.set_ascii(directory::ifd0, 0x8298, "(c) someone");
		e.set_rationals(directory::gps, 0x0002, {{40, 1}, {42, 1}, {51, 1}});
		e.set_ascii(directory::gps, 0x0001, "N");
		e.remove(directory::ifd0, 0x4746);
	});

	CHECK(le32(result, 4) >= stream.size()); // IFD0 now lies in what was appended
	CHECK(std::memcmp(result.data() + 8, stream.data() + 8, stream.size() - 8) == 0);
	CHECK_STR(ascii(tiff_value(result, 0x8298)), "(c) someone");
	CHECK(!tiff_value(result, 0x4746));
	CHECK_STR(ascii(tiff_value(result, 0x0001, 0x8825)), "N");
	CHECK_EQ(le32(*tiff_value(result, 0x0002, 0x8825), 16), 51u);
	CHECK_STR(ascii(tiff_value(result, 0x9003, 0x8769)), "2001:02:03 04:05:06");
	CHECK_EQ(le16(*tiff_value(result, 0x0112), 0), 6u);
}

TEST("iptc is written as UTF-8 and existing 8-bit text keeps its meaning")
{
	builder old;
	old.u8(0x1C).u8(2).u8(0).be16(2).u8(0).u8(4);
	old.u8(0x1C).u8(2).u8(80).be16(4).raw(std::string_view("Ren\xe9", 4));
	old.u8(0x1C).u8(2).u8(110).be16(7).raw(std::string_view("O\x92" "Brien", 7));
	old.u8(0x1C).u8(2).u8(25).be16(3).raw("old");

	libfiles::legacy_edits e;
	e.keywords = std::vector<std::string>{"Zürich", "two"};
	e.description = "line one\nline two";
	e.city = "Oslo";

	libfiles::detail::photo_legacy out;
	libfiles::detail::apply_photo_legacy(e, old.b, true, out);
	CHECK(out.iptc.has_value());

	const auto datasets = libfiles::detail::parse_iptc(*out.iptc);
	CHECK(datasets.front().record == 1 && datasets.front().number == 90);
	CHECK_STR(iptc_value(*out.iptc, 80), "Ren\xc3\xa9");
	CHECK_STR(iptc_value(*out.iptc, 110), "O\xe2\x80\x99" "Brien");
	CHECK_STR(iptc_value(*out.iptc, 120), "line one\rline two");
	CHECK_STR(iptc_value(*out.iptc, 90), "Oslo");

	auto keywords = 0;
	for (const auto& d : datasets) if (d.record == 2 && d.number == 25) ++keywords;
	CHECK_EQ(keywords, 2);
	CHECK(to_string(*out.iptc).find("old") == std::string::npos);

	libfiles::detail::photo_legacy refused;
	libfiles::detail::apply_photo_legacy(e, old.b, false, refused);
	CHECK(!refused.iptc.has_value());
}

TEST("the iptc writer walks a block as the reader does, keeping its empty datasets")
{
	builder block;
	block.u8(0x1C).u8(2).u8(5).be16(0);
	// IIM 4.1: the size field's top bit set, its low bits count the octets that hold the size.
	block.u8(0x1C).u8(2).u8(120).be16(0x8002).be16(5).raw("Hello");
	// Zero octets hold no size, so neither walk goes past it.
	block.u8(0x1C).u8(2).u8(90).be16(0x8000).raw("x");
	block.u8(0x1C).u8(2).u8(25).be16(4).raw("lost");

	const auto datasets = libfiles::detail::parse_iptc(block.b);
	CHECK_EQ(datasets.size(), 2u);
	CHECK(datasets[0].number == 5 && datasets[0].data.empty());
	CHECK(datasets[1].number == 120 && to_string(datasets[1].data) == "Hello");

	// The reader passes over the empty title.
	const auto rows = libfiles::list_iptc(block.b);
	CHECK_EQ(rows.size(), 1u);
	CHECK(rows[0].dataset == 120 && rows[0].value == "Hello");

	// A dataset longer than the IIM allows ends the reader's walk; the writer keeps it, and what
	// follows it, for the round trip.
	builder big;
	big.u8(0x1C).u8(2).u8(120).be16(0x8004).be32(256001).raw(std::string(256001, 'a'));
	big.u8(0x1C).u8(2).u8(5).be16(5).raw("Title");
	CHECK(libfiles::list_iptc(big.b).empty());
	CHECK_EQ(libfiles::detail::parse_iptc(big.b).size(), 2u);
}

TEST("a jpeg update reconciles EXIF and IPTC and adds what it lacks")
{
	libfiles::xmp::meta meta;
	meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", 3);

	libfiles::metadata_update u{&meta};
	u.legacy.description = "A lake at dawn";
	u.legacy.rating = 3;
	u.legacy.rating_percent = 50;
	u.legacy.keywords = std::vector<std::string>{"lake", "dawn"};
	u.legacy.gps_latitude = "40,42,51N";
	u.legacy.gps_longitude = "74,0,22W";
	u.legacy.city = "Big Apple";
	u.legacy.date_original = "2021-06-07T08:09:10";

	for (const auto with_exif : {true, false})
	{
		const auto original = make_jpeg(with_exif);
		libfiles::write_plan plan;
		const auto result = update(original, u, &plan);
		CHECK(plan.ok());

		const auto scanned = scan_bytes(result);
		CHECK(scanned.success);
		CHECK_EQ(scanned.width, 30u);
		CHECK(!scanned.metadata.xmp.empty());
		CHECK_STR(iptc_value(scanned.metadata.iptc, 90), "Big Apple");
		CHECK_STR(iptc_value(scanned.metadata.iptc, 120), "A lake at dawn");
		CHECK_STR(iptc_value(scanned.metadata.iptc, 55), "20210607");

		const auto& exif = scanned.metadata.exif;
		CHECK_STR(ascii(tiff_value(exif, 0x010E)), "A lake at dawn");
		CHECK_EQ(le16(*tiff_value(exif, 0x4746), 0), 3u);
		CHECK_EQ(le16(*tiff_value(exif, 0x4749), 0), 50u);
		CHECK_STR(ascii(tiff_value(exif, 0x9003, 0x8769)), "2021:06:07 08:09:10");
		CHECK_STR(ascii(tiff_value(exif, 0x0003, 0x8825)), "W");
		CHECK_EQ(le32(*tiff_value(exif, 0x0004, 0x8825), 16), 22u);
		CHECK(tiff_value(exif, 0x9C9E).has_value());

		// The orientation and the frame are untouched.
		CHECK(scanned.orientation == (with_exif ? 6 : 1));
		CHECK(std::memcmp(result.data() + result.size() - 18, original.data() + original.size() - 18, 18) == 0);
	}
}

TEST("a jpeg packet that fits is a patch")
{
	libfiles::xmp::meta first;
	first.set_property_int(libfiles::xmp::ns::xmp, "Rating", 1);
	const auto once = update(make_jpeg(true), {&first});

	libfiles::xmp::meta second;
	second.set_property_int(libfiles::xmp::ns::xmp, "Rating", 5);
	libfiles::reader r(once);
	const auto plan = libfiles::plan_update(r, {&second});

	CHECK(plan.ok());
	CHECK(plan.in_place());
}

TEST("a tiff file is updated by appending")
{
	builder t;
	t.raw("II").le16(42).le32(8);
	t.le16(3);
	t.le16(0x0100).le16(3).le32(1).le16(64).le16(0);
	t.le16(0x0101).le16(3).le32(1).le16(32).le16(0);
	t.le16(0x0111).le16(4).le32(1).le32(50);
	t.le32(0);
	t.zeros(50 - t.size()).raw(bytes(100, 0x55));
	const auto original = t.b;

	libfiles::xmp::meta meta;
	meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", 2);
	libfiles::metadata_update u{&meta};
	u.legacy.gps_latitude = "10,30,0S";
	u.legacy.gps_longitude = "20,0,36W";

	const auto result = update(original, u);
	const auto scanned = scan_bytes(result);

	CHECK(scanned.success);
	CHECK(!scanned.metadata.xmp.empty());
	CHECK(scanned.gps && scanned.gps->latitude == -10.5);
	CHECK(scanned.gps && scanned.gps->longitude < -20.0);
	CHECK(std::memcmp(result.data() + 8, original.data() + 8, original.size() - 8) == 0);
}

TEST("a webp's EXIF takes the edit, keeping its signature")
{
	builder w;
	riff_chunk(w, "VP8X", vp8x(0x08, 300, 200));
	riff_chunk(w, "VP8 ", vp8_frame(300, 200));
	riff_chunk(w, "EXIF", builder().raw(std::string_view("Exif\0\0", 6)).raw(make_tiff_stream()).b);
	const auto original = finish_riff(w);

	libfiles::xmp::meta meta;
	libfiles::metadata_update u{&meta};
	u.legacy.description = "A webp described at length, past its old slot";
	u.legacy.title = "IPTC only";

	const auto result = update(original, u);
	const auto scanned = scan_bytes(result);

	CHECK(scanned.success);
	CHECK_STR(ascii(tiff_value(scanned.metadata.exif, 0x010E)), "A webp described at length, past its old slot");
	CHECK(scanned.metadata.iptc.empty());
	CHECK(to_string(result).find(std::string_view("EXIF")) != std::string::npos);
	CHECK(to_string(result).find(std::string("Exif\0\0II", 8)) != std::string::npos);
	CHECK_EQ(static_cast<std::size_t>(libfiles::detail::load_le32(result.data() + 4)) + 8, result.size());

	// A WebP without EXIF does not gain one.
	builder plain;
	riff_chunk(plain, "VP8 ", vp8_frame(300, 200));
	const auto bare = finish_riff(plain);
	CHECK(scan_bytes(update(bare, u)).metadata.exif.empty());
}

TEST("a psd update rebuilds its resources, or patches a packet that fits")
{
	builder p;
	p.raw("8BPS").be16(1).zeros(6).be16(3).be32(4).be32(4).be16(8).be16(3);
	p.be32(0);

	builder resources;
	resources.raw("8BIM").be16(0x03ED).u8(0).u8(0).be32(2).raw("rs");
	p.be32(static_cast<std::uint32_t>(resources.size())).raw(resources.b);
	p.be32(0).be16(0).zeros(48);
	const auto original = p.b;

	libfiles::xmp::meta meta;
	meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", 4);
	libfiles::metadata_update u{&meta};
	u.legacy.title = "Cherries";
	u.legacy.copyright = "(c) me";

	const auto result = update(original, u);
	const auto scanned = scan_bytes(result);

	CHECK(scanned.success);
	CHECK_EQ(scanned.width, 4u);
	CHECK(!scanned.metadata.xmp.empty());
	CHECK_STR(iptc_value(scanned.metadata.iptc, 5), "Cherries");
	CHECK_STR(ascii(tiff_value(scanned.metadata.exif, 0x8298)), "(c) me");
	CHECK(std::memcmp(result.data() + result.size() - 54, original.data() + original.size() - 54, 54) == 0);

	libfiles::xmp::meta again;
	again.set_property_int(libfiles::xmp::ns::xmp, "Rating", 1);
	libfiles::reader r(result);
	CHECK(libfiles::plan_update(r, {&again}).in_place());
}

TEST("an iptc edit starts from and changes the resource a scan reads: the first that holds anything")
{
	const auto keyword = [](const std::string_view word)
	{
		return builder().u8(0x1C).u8(2).u8(25).be16(static_cast<unsigned>(word.size())).raw(word).b;
	};

	// An empty IPTC resource, then one holding a keyword, then a stale one.
	builder resources;
	resources.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(0);
	for (const auto* word : {"kept", "stale"})
	{
		const auto iim = keyword(word);
		resources.raw("8BIM").be16(0x0404).u8(0).u8(0).be32(static_cast<std::uint32_t>(iim.size())).raw(iim);
		if (iim.size() & 1) resources.u8(0);
	}

	builder j;
	j.u8(0xFF).u8(0xD8);
	jpeg_segment(j, 0xE0, builder().raw(std::string_view("JFIF\0", 5)).zeros(9).b);
	jpeg_segment(j, 0xED, builder().raw(std::string_view("Photoshop 3.0\0", 14)).raw(resources.b).b);
	jpeg_segment(j, 0xC0, builder().u8(8).be16(20).be16(30).u8(1).u8(1).u8(0x11).u8(0).b);
	jpeg_segment(j, 0xDA, builder().u8(1).zeros(5).b);
	j.zeros(16).u8(0xFF).u8(0xD9);
	CHECK_STR(iptc_value(scan_bytes(j.b).metadata.iptc, 25), "kept");

	libfiles::metadata_update u;
	u.legacy.title = "Cherries";
	const auto scanned = scan_bytes(update(j.b, u));

	CHECK(scanned.success);
	CHECK_STR(iptc_value(scanned.metadata.iptc, 5), "Cherries");
	CHECK_STR(iptc_value(scanned.metadata.iptc, 25), "kept");
}

TEST("an interrupted jpeg patch reads whole, each field old or new")
{
	libfiles::xmp::meta first;
	first.set_property_int(libfiles::xmp::ns::xmp, "Rating", 1);
	const auto original = update(make_jpeg(true), {&first});

	libfiles::xmp::meta second;
	second.set_property_int(libfiles::xmp::ns::xmp, "Rating", 5);
	libfiles::metadata_update u{&second};
	u.legacy.rating = 5;
	u.legacy.orientation = 3;

	libfiles::write_plan plan;
	const auto rewritten = update(original, u, &plan);
	const auto states = patch_states(original, plan);
	CHECK(states.size() > 4);
	CHECK(!states.empty() && states.back() == rewritten);

	// A rating in a packet that keeps its length is a byte of it, and two entries.
	std::uint64_t written = 0;
	for (const auto& c : plan.replacements) written += c.data.size();
	CHECK(written < 32);

	const auto old_packet = packet_of(original);
	const auto new_packet = packet_of(rewritten);
	CHECK(old_packet != new_packet);

	for (const auto& state : states)
	{
		const auto scanned = scan_bytes(state);
		CHECK(scanned.success);
		const auto packet = to_string(scanned.metadata.xmp);
		CHECK(packet == old_packet || packet == new_packet);
		const auto rating = tiff_value(scanned.metadata.exif, 0x4746);
		CHECK(rating && (le16(*rating, 0) == 1 || le16(*rating, 0) == 5));
		CHECK(scanned.orientation == 6 || scanned.orientation == 3);
	}
}

TEST("an interrupted tiff update reads whole, each value old or new")
{
	// First the packet and the IPTC the file lacks are appended with a new IFD0 while the description
	// shrinks where it lies; then it shrinks again under the same entries. Either way an interruption
	// between the value and what names it leaves the old entry over the new text.
	const auto original = make_tiff_file();
	auto file = original;

	for (const auto& [rating, description] : {std::pair{2, "shorter"}, std::pair{4, "short"}})
	{
		libfiles::xmp::meta meta;
		meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", rating);
		libfiles::metadata_update u{&meta};
		u.legacy.description = description;

		libfiles::write_plan plan;
		const auto rewritten = update(file, u, &plan);
		const auto states = patch_states(file, plan);
		CHECK(states.size() > 4);
		CHECK(!states.empty() && states.back() == rewritten);

		const auto old_packet = packet_of(file);
		const auto old_description = c_string(tiff_value(file, 0x010E));
		const auto new_packet = packet_of(rewritten);

		for (const auto& state : states)
		{
			const auto scanned = scan_bytes(state);
			CHECK(scanned.success);
			CHECK_EQ(scanned.width, 64u);
			const auto packet = to_string(scanned.metadata.xmp);
			CHECK(packet == old_packet || packet == new_packet);
			const auto text = c_string(tiff_value(state, 0x010E));
			CHECK(text == old_description || text == description);
		}

		CHECK_STR(c_string(tiff_value(rewritten, 0x010E)), description);
		CHECK(std::memcmp(rewritten.data() + 86, original.data() + 86, 100) == 0);
		file = rewritten;
	}
}

TEST("a shorter binary value is appended rather than left to read as the old length")
{
	const auto stream = make_tiff_stream();
	auto in_place = true;

	// The 24 bytes of the description, as an undefined value of 20: the old entry would read 24.
	const auto result = edited(stream, [](tiff_editor& e)
	{
		e.set(directory::ifd0, 0x010E, tiff_editor::type_undefined, 20, bytes(20, 'u'));
	}, &in_place);

	CHECK(!in_place);
	CHECK(result.size() > stream.size());
	CHECK(std::memcmp(result.data() + 62, stream.data() + 62, 24) == 0);
	CHECK_EQ(tiff_value(result, 0x010E)->size(), 20u);
}

TEST("an interrupted psd packet patch reads whole")
{
	builder p;
	p.raw("8BPS").be16(1).zeros(6).be16(3).be32(4).be32(4).be16(8).be16(3);
	p.be32(0);

	builder resources;
	resources.raw("8BIM").be16(0x03ED).u8(0).u8(0).be32(2).raw("rs");
	p.be32(static_cast<std::uint32_t>(resources.size())).raw(resources.b);
	p.be32(0).be16(0).zeros(48);

	libfiles::xmp::meta first;
	first.set_property_int(libfiles::xmp::ns::xmp, "Rating", 4);
	const auto original = update(p.b, {&first});

	libfiles::xmp::meta again;
	again.set_property_int(libfiles::xmp::ns::xmp, "Rating", 1);
	libfiles::write_plan plan;
	const auto rewritten = update(original, {&again}, &plan);
	const auto states = patch_states(original, plan);
	CHECK(states.size() >= 2);
	CHECK(!states.empty() && states.back() == rewritten);

	for (const auto& state : states)
	{
		const auto scanned = scan_bytes(state);
		CHECK(scanned.success);
		const auto packet = to_string(scanned.metadata.xmp);
		CHECK(packet == packet_of(original) || packet == packet_of(rewritten));
	}
}
