// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for metadata updates. Each writes a file built here, reads it back with the scanner,
// and checks both the packet and that nothing else in the file moved.

#include "test.h"

#include "builders.h"
#include "interruptions.h"
#include "libfiles/write.h"

#include <cstring>

using namespace test_build;

namespace
{
	libfiles::xmp::meta rated(const int rating, const std::string_view label = {})
	{
		libfiles::xmp::meta meta;
		meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", rating);
		if (!label.empty()) meta.set_property(libfiles::xmp::ns::xmp, "Label", label);
		return meta;
	}

	struct written
	{
		bytes file;
		libfiles::write_plan plan;
	};

	written update(const bytes& file, const libfiles::xmp::meta& meta)
	{
		libfiles::reader r(file);
		auto plan = libfiles::plan_update(r, {&meta});

		libfiles::memory_stream out;
		if (plan.ok()) libfiles::write_updated(r, plan, out);
		return {out.bytes(), std::move(plan)};
	}

	int rating_of(const bytes& file)
	{
		libfiles::reader r(file);
		const auto scanned = libfiles::scan(r);
		if (!scanned.error.empty() || scanned.metadata.xmp.empty()) return -100;

		libfiles::xmp::meta meta;
		meta.parse(to_string(scanned.metadata.xmp));

		std::int32_t rating = -99;
		meta.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating);
		return rating;
	}

	bytes make_png(const bool with_trailing_text_xmp = false)
	{
		builder p;
		p.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
		png_chunk(p, "IHDR", png_ihdr(16, 8, 2));
		png_chunk(p, "IDAT", bytes(40, 7));
		if (with_trailing_text_xmp) png_chunk(p, "tEXt", builder().raw(std::string_view("XML:com.adobe.xmp\0", 18)).raw("<old/>").b);
		png_chunk(p, "IEND", {});
		return p.b;
	}

	bytes make_gif(const std::string_view version = "GIF89a")
	{
		builder g;
		g.raw(version).le16(16).le16(8).u8(0x80).u8(0).u8(0).zeros(6);
		g.u8(0x2C).zeros(8).u8(0).u8(2).u8(3).raw("abc").u8(0);
		g.u8(0x3B);
		return g.b;
	}

	bytes make_simple_webp(const bool lossless)
	{
		builder w;
		if (lossless) riff_chunk(w, "VP8L", vp8l_frame(37, 21, true));
		else riff_chunk(w, "VP8 ", vp8_frame(300, 200));
		return finish_riff(w);
	}

	bool same_bytes(const bytes& a, const std::size_t a_from, const bytes& b, const std::size_t b_from, const std::size_t n)
	{
		return a.size() >= a_from + n && b.size() >= b_from + n && std::memcmp(a.data() + a_from, b.data() + b_from, n) == 0;
	}

	// False when a WebP's VP8X declares a packet that no chunk within its RIFF size holds.
	bool webp_holds_what_it_declares(const bytes& file)
	{
		const auto riff_end = std::min<std::size_t>(file.size(), 8 + std::size_t{libfiles::detail::load_le32(file.data() + 4)});
		auto declared = false;
		auto held = false;

		for (std::size_t pos = 12; pos + 8 <= riff_end;)
		{
			const auto size = std::size_t{libfiles::detail::load_le32(file.data() + pos + 4)};
			if (std::memcmp(file.data() + pos, "VP8X", 4) == 0) declared = (file[pos + 8] & 0x04u) != 0;
			if (std::memcmp(file.data() + pos, "XMP ", 4) == 0) held = true;
			pos += 8 + size + (size & 1u);
		}

		return !declared || held;
	}
}

TEST("a png gains its packet after IHDR and keeps every other chunk")
{
	const auto original = make_png();
	const auto first = update(original, rated(4));

	CHECK(first.plan.ok());
	CHECK(!first.plan.in_place());
	CHECK_EQ(rating_of(first.file), 4);

	// IHDR is 8 + 25 bytes in; the new chunk follows it, and IDAT and IEND follow that unchanged.
	CHECK(std::memcmp(first.file.data() + 33 + 4, "iTXt", 4) == 0);
	const auto tail = original.size() - 33;
	CHECK(same_bytes(first.file, first.file.size() - tail, original, 33, tail));
}

TEST("a png packet that still fits is patched in place")
{
	const auto first = update(make_png(), rated(1));

	libfiles::reader r(first.file);
	const auto meta = rated(5, "Approved");
	const auto plan = libfiles::plan_update(r, {&meta});

	CHECK(plan.ok());
	CHECK(plan.in_place());
	CHECK_EQ(plan.result_size(), first.file.size());

	libfiles::memory_stream file(first.file);
	libfiles::write_in_place(file, plan);
	CHECK_EQ(file.bytes().size(), first.file.size());
	CHECK_EQ(rating_of(file.bytes()), 5);
}

TEST("a png keeps one packet when another XMP chunk follows the image")
{
	const auto result = update(make_png(true), rated(2));

	CHECK(result.plan.ok());
	CHECK_EQ(rating_of(result.file), 2);

	const auto text = to_string(result.file);
	CHECK(text.find("tEXt") == std::string::npos);
	CHECK(text.find("<old/>") == std::string::npos);
}

TEST("a damaged png is not updated")
{
	auto cut = make_png();
	cut.resize(cut.size() - 12);

	libfiles::reader r(cut);
	const auto meta = rated(3);
	const auto plan = libfiles::plan_update(r, {&meta});
	CHECK(!plan.ok());
	CHECK(plan.replacements.empty());
}

TEST("a gif gains an XMP extension before its trailer, then is patched in place")
{
	const auto original = make_gif();
	const auto first = update(original, rated(3));

	CHECK(first.plan.ok());
	CHECK_EQ(rating_of(first.file), 3);
	CHECK(same_bytes(first.file, 0, original, 0, original.size() - 1));
	CHECK_EQ(first.file.back(), 0x3Bu);

	libfiles::reader r(first.file);
	const auto meta = rated(1);
	const auto plan = libfiles::plan_update(r, {&meta});
	CHECK(plan.in_place());

	libfiles::memory_stream file(first.file);
	libfiles::write_in_place(file, plan);
	CHECK_EQ(rating_of(file.bytes()), 1);
}

TEST("a gif87a cannot hold XMP")
{
	const auto gif87 = make_gif("GIF87a");
	libfiles::reader r(gif87);
	const auto meta = rated(3);
	CHECK(!libfiles::plan_update(r, {&meta}).ok());
}

TEST("a simple webp becomes extended, with its canvas from the image header")
{
	for (const auto lossless : {false, true})
	{
		const auto result = update(make_simple_webp(lossless), rated(4));

		CHECK(result.plan.ok());
		CHECK_EQ(rating_of(result.file), 4);

		libfiles::reader r(result.file);
		const auto scanned = libfiles::scan(r);
		CHECK_EQ(scanned.width, lossless ? 37u : 300u);
		CHECK_EQ(scanned.height, lossless ? 21u : 200u);
		CHECK_STR(scanned.pixel_format, lossless ? "rgba" : "yuv420");

		// VP8X first, declaring XMP and, for the lossless image, alpha.
		CHECK(std::memcmp(result.file.data() + 12, "VP8X", 4) == 0);
		CHECK_EQ(result.file[20], lossless ? 0x14u : 0x04u);

		// The RIFF size counts everything after itself.
		CHECK_EQ(static_cast<std::size_t>(libfiles::detail::load_le32(result.file.data() + 4)) + 8, result.file.size());
	}
}

TEST("an extended webp keeps its chunks in order and replaces its packet")
{
	builder w;
	riff_chunk(w, "VP8X", vp8x(0x08 | 0x20, 300, 200));
	riff_chunk(w, "ICCP", to_bytes("profile"));
	riff_chunk(w, "VP8 ", vp8_frame(300, 200));
	riff_chunk(w, "XMP ", to_bytes("<x:xmpmeta xmlns:x='adobe:ns:meta/'/>"));
	riff_chunk(w, "EXIF", to_bytes("II*\0exif"));
	const auto original = finish_riff(w);

	const auto result = update(original, rated(2));
	CHECK(result.plan.ok());
	CHECK_EQ(rating_of(result.file), 2);

	// The flags gain XMP; ICCP and the image keep their place and bytes; EXIF still follows XMP.
	CHECK_EQ(libfiles::detail::load_le32(result.file.data() + 20), 0x08u | 0x20u | 0x04u);
	CHECK(same_bytes(result.file, 30, original, 30, 8 + 8 + 8 + 210));

	const auto text = to_string(result.file);
	CHECK(text.find("XMP ") < text.find("EXIF"));
	CHECK(text.find("II*") != std::string::npos);
}

TEST("formats that cannot be updated say so")
{
	const auto bmp = builder().raw("BM").zeros(12).le32(40).le32(4).le32(4).le16(1).le16(24).zeros(24).b;
	libfiles::reader r(bmp);
	const auto meta = rated(1);
	const auto plan = libfiles::plan_update(r, {&meta});
	CHECK(!plan.ok());
	CHECK(!libfiles::can_update(libfiles::detect_file_format(bmp)));
	CHECK(libfiles::detect_file_format(bmp) == libfiles::file_format::unknown);
	CHECK(libfiles::can_update(libfiles::file_format::jpeg));
	CHECK(libfiles::can_update(libfiles::file_format::mp4));
}

TEST("a plan is applied only to the file it was made for")
{
	const auto first = update(make_png(), rated(1));
	libfiles::reader r(first.file);
	const auto meta = rated(2);
	const auto plan = libfiles::plan_update(r, {&meta});

	libfiles::memory_stream shorter(bytes(first.file.begin(), first.file.end() - 1));
	CHECK_THROWS(libfiles::write_in_place(shorter, plan));

	const auto grow = update(make_png(), rated(1));
	libfiles::memory_stream target(make_png());
	CHECK_THROWS(libfiles::write_in_place(target, grow.plan));

	// A destination holding more than the result is cut to it.
	const auto png = make_png();
	libfiles::reader source(png);
	libfiles::memory_stream larger(bytes(10000, 1));
	libfiles::write_updated(source, grow.plan, larger);
	CHECK(larger.bytes() == grow.file);
}

TEST("interrupted png, gif and webp patches read whole")
{
	const auto replay = [](const bytes& file, const int old_rating, const int new_rating, const auto& also)
	{
		const auto rewritten = update(file, rated(new_rating));
		const auto states = patch_states(file, rewritten.plan);
		CHECK(states.size() >= 2);
		CHECK(!states.empty() && states.back() == rewritten.file);

		for (const auto& state : states)
		{
			libfiles::reader r(state);
			CHECK(libfiles::scan(r).success);
			const auto rating = rating_of(state);
			CHECK(rating == old_rating || rating == new_rating);
			also(state);
		}
	};

	const auto nothing_more = [](const bytes&) {};
	replay(update(make_png(), rated(1)).file, 1, 5, nothing_more);
	replay(update(make_gif(), rated(3)).file, 3, 1, nothing_more);

	// An extended WebP takes its first packet at the end, and declares it once the RIFF holds it.
	builder w;
	riff_chunk(w, "VP8X", vp8x(0, 300, 200));
	riff_chunk(w, "VP8 ", vp8_frame(300, 200));
	const auto extended = finish_riff(w);
	const auto holds_what_it_declares = [](const bytes& state) { CHECK(webp_holds_what_it_declares(state)); };
	replay(extended, -100, 2, holds_what_it_declares);
	replay(update(extended, rated(2)).file, 2, 4, holds_what_it_declares);
}
