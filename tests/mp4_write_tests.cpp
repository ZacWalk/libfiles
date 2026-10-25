// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for writing ISO base media and QuickTime files. Every update here is applied as a live
// file would be, in place, and the media bytes are checked to be exactly where they were.

#include "test.h"

#include "builders.h"
#include "interruptions.h"
#include "legacy.h"
#include "libfiles/write.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	bytes box(const std::string_view type, const bytes& payload)
	{
		return builder().be32(static_cast<std::uint32_t>(payload.size() + 8)).raw(type).raw(payload).b;
	}

	bytes join(std::initializer_list<bytes> parts)
	{
		bytes out;
		for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
		return out;
	}

	bytes mvhd_v0()
	{
		builder b;
		b.u8(0).u8(0).u8(0).u8(0).be32(100).be32(200).be32(600).be32(6000);
		b.zeros(80);
		return box("mvhd", b.b);
	}

	const bytes media = bytes(64, 0xAB);

	struct file_parts
	{
		std::string_view brand = "isom";
		bool udta = false;
		// The size of a free box in the user data, as other tools leave one.
		std::uint32_t udta_free = 0;
		bool mdat_to_end = false;
		bool moov_last = false;
	};

	bytes make_movie(const file_parts& p)
	{
		auto moov_payload = join({mvhd_v0(), box("trak", bytes(40, 0x11))});

		if (p.udta || p.udta_free != 0)
		{
			auto user_data = box("\xa9nam", builder().raw("name").b);
			if (p.udta_free != 0) user_data = join({user_data, box("free", bytes(p.udta_free - 8, 0))});
			moov_payload = join({moov_payload, box("udta", user_data)});
		}

		const auto moov = box("moov", moov_payload);
		const auto ftyp = box("ftyp", builder().raw(p.brand).be32(0).raw(p.brand).b);

		bytes mdat = box("mdat", media);
		if (p.mdat_to_end) std::memset(mdat.data(), 0, 4);

		if (p.moov_last) return join({ftyp, mdat, moov});
		return join({ftyp, moov, mdat});
	}

	std::size_t find(const bytes& file, const std::string_view text, const std::size_t from = 0)
	{
		const std::string s(file.begin(), file.end());
		return s.find(text, from);
	}

	// The types of the top-level boxes, as a reader walks them; empty when one runs past the end.
	std::vector<std::string> top_level(const bytes& file)
	{
		std::vector<std::string> types;
		std::size_t pos = 0;

		while (file.size() - pos >= 8)
		{
			std::uint64_t size = libfiles::detail::load_be32(file.data() + pos);
			if (size == 0) size = file.size() - pos;
			if (size < 8 || size > file.size() - pos) return {};
			types.emplace_back(reinterpret_cast<const char*>(file.data() + pos + 4), 4);
			pos += static_cast<std::size_t>(size);
		}

		return types;
	}

	std::size_t count_of(const std::vector<std::string>& types, const std::string_view type)
	{
		return static_cast<std::size_t>(std::ranges::count(types, type));
	}

	struct written
	{
		bytes file;
		libfiles::write_plan plan;
	};

	// Applied as to a live file: in place, appending at the end.
	written update_live(const bytes& file, const libfiles::metadata_update& u)
	{
		libfiles::reader r(file);
		auto plan = libfiles::plan_update(r, u);
		libfiles::memory_stream live(file);
		if (plan.patchable()) libfiles::write_in_place(live, plan);
		return {live.bytes(), std::move(plan)};
	}

	int rating_of(const bytes& file)
	{
		libfiles::reader r(file);
		const auto packet = libfiles::read_packet(r);
		if (!packet.readable || packet.packet.empty()) return -100;

		libfiles::xmp::meta meta;
		meta.parse(packet.packet);
		std::int32_t rating = -99;
		meta.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating);
		return rating;
	}

	libfiles::xmp::meta rated(const int rating)
	{
		libfiles::xmp::meta meta;
		meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", rating);
		return meta;
	}

	bool media_unmoved(const bytes& before, const bytes& after)
	{
		const auto at = find(before, std::string(media.begin(), media.end()));
		return at != std::string::npos && after.size() >= at + media.size() &&
			std::memcmp(after.data() + at, media.data(), media.size()) == 0;
	}

	// Checks that wherever the update is interrupted, the file reads whole, its media where it was and
	// its rating either the old or the new.
	std::size_t check_interruptions(const bytes& original, const libfiles::metadata_update& u, const int old_rating,
	                                const int new_rating, bytes* settled = nullptr)
	{
		libfiles::reader r(original);
		const auto plan = libfiles::plan_update(r, u);
		CHECK(plan.patchable());

		recording_stream live(original);
		libfiles::write_in_place(live, plan);

		const auto states = interrupted_states(original, live.writes);
		CHECK(!states.empty());

		for (const auto& state : states)
		{
			libfiles::reader sr(state);
			CHECK(libfiles::read_packet(sr).readable);
			CHECK(!top_level(state).empty());
			CHECK(count_of(top_level(state), "moov") >= 1);
			CHECK(media_unmoved(original, state));

			const auto rating = rating_of(state);
			CHECK(rating == old_rating || rating == new_rating);
		}

		CHECK_EQ(rating_of(states.back()), new_rating);
		if (settled) *settled = states.back();
		return states.size();
	}
}

TEST("an mp4 is told apart from a photo in the same container")
{
	const auto movie = make_movie({});
	CHECK(libfiles::detect_file_format(movie) == libfiles::file_format::mp4);
	CHECK(libfiles::detect_file_format(make_movie({.brand = "qt  "})) == libfiles::file_format::mp4);
	CHECK(libfiles::detect_file_format(make_movie({.brand = "crx "})) == libfiles::file_format::unknown);
	CHECK(libfiles::detect_file_format(make_movie({.brand = "heic"})) == libfiles::file_format::unknown);
	CHECK(libfiles::detect_file_format(make_movie({.brand = "avif"})) == libfiles::file_format::unknown);
}

TEST("an mp4 gains its packet in a box appended at the end")
{
	const auto original = make_movie({});
	const auto meta = rated(4);
	const auto result = update_live(original, {&meta});

	CHECK(result.plan.ok());
	CHECK(result.plan.patchable());
	CHECK(!result.plan.in_place());
	CHECK_EQ(rating_of(result.file), 4);
	CHECK(std::memcmp(result.file.data(), original.data(), original.size()) == 0);
	CHECK(find(result.file, "uuid") > original.size() - 8);
}

TEST("an mp4 packet that fits is patched where it lies")
{
	const auto meta = rated(2);
	const auto once = update_live(make_movie({}), {&meta});

	const auto again = rated(5);
	const auto twice = update_live(once.file, {&again});

	CHECK(twice.plan.in_place());
	CHECK_EQ(twice.file.size(), once.file.size());
	CHECK_EQ(rating_of(twice.file), 5);
}

TEST("an mp4 packet always follows the movie box, where FFmpeg reads it")
{
	const auto after_movie = [](const bytes& file)
	{
		const auto types = top_level(file);
		const auto moov = std::ranges::find(types, "moov");
		const auto packet = std::ranges::find(types, "uuid");
		return count_of(types, "moov") == 1 && count_of(types, "uuid") == 1 && moov < packet;
	};

	// A packet that fits where it lies goes after the movie box when that moves to the end.
	const auto meta = rated(2);
	const auto once = update_live(make_movie({.udta = true}), {&meta});
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 2;
	const auto moved = update_live(once.file, u);
	CHECK(after_movie(moved.file));
	CHECK_EQ(rating_of(moved.file), 2);

	// So does the packet a file holds when the update leaves it as it is.
	libfiles::metadata_update legacy_only;
	legacy_only.legacy.keywords = std::vector<std::string>{"beach"};
	const auto kept = update_live(once.file, legacy_only);
	CHECK(after_movie(kept.file));
	CHECK_EQ(rating_of(kept.file), 2);

	// A packet another tool put before the movie box, where FFmpeg cannot see it, is moved after it.
	const auto packet = rated(1).serialize({});
	const auto uuid_box = box("uuid", join({bytes{0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94, 0x91,
	                                              0xE3, 0xAF, 0xAC}, bytes(packet.begin(), packet.end())}));
	const auto plain = make_movie({});
	const auto ftyp_size = libfiles::detail::load_be32(plain.data());
	auto before_movie = bytes(plain.begin(), plain.begin() + ftyp_size);
	before_movie = join({before_movie, uuid_box, bytes(plain.begin() + ftyp_size, plain.end())});
	CHECK_EQ(rating_of(before_movie), 1);

	const auto three = rated(3);
	const auto result = update_live(before_movie, {&three});
	CHECK(after_movie(result.file));
	CHECK_EQ(rating_of(result.file), 3);
}

TEST("an mp4 whose movie box must grow moves it to the end")
{
	const auto original = make_movie({.udta = true});
	const auto meta = rated(4);
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 4;
	u.legacy.keywords = std::vector<std::string>{"beach", "Zürich"};

	const auto result = update_live(original, u);

	CHECK(result.plan.patchable());
	CHECK(media_unmoved(original, result.file));
	CHECK_EQ(rating_of(result.file), 4);

	// The old movie box is free space now, and the new one, at the end, holds the Xtra entries.
	const auto old_moov = find(original, "moov");
	CHECK(std::memcmp(result.file.data() + old_moov, "free", 4) == 0);
	CHECK(find(result.file, "moov") > original.size());

	const auto xtra = find(result.file, "Xtra");
	CHECK(xtra != std::string::npos);
	CHECK(find(result.file, "WM/Category", xtra) != std::string::npos);
	CHECK(find(result.file, "WM/SharedUserRating", xtra) != std::string::npos);

	const auto z = libfiles::detail::utf16le("Zürich");
	CHECK(find(result.file, std::string(z.begin(), z.end()), xtra) != std::string::npos);
}

TEST("an mp4's user data grows into the free space it holds")
{
	const auto original = make_movie({.udta_free = 512});
	const auto meta = rated(1);
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 3;
	u.legacy.keywords = std::vector<std::string>{"beach"};

	const auto result = update_live(original, u);

	CHECK(result.plan.patchable());
	CHECK(media_unmoved(original, result.file));
	CHECK_EQ(find(result.file, "moov"), find(original, "moov"));
	CHECK(find(result.file, "Xtra") < find(result.file, "mdat"));
	CHECK_EQ(libfiles::detail::load_be32(result.file.data() + find(result.file, "moov") - 4),
	         libfiles::detail::load_be32(original.data() + find(original, "moov") - 4));
}

TEST("an mp4 movie box that moved has room for the next edit")
{
	const auto meta = rated(3);
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 3;
	const auto once = update_live(make_movie({.udta = true}), u);

	libfiles::metadata_update v{&meta};
	v.legacy.keywords = std::vector<std::string>{"one", "two", "three", "four"};
	const auto twice = update_live(once.file, v);

	CHECK(twice.plan.in_place());
	CHECK_EQ(twice.file.size(), once.file.size());
	CHECK(find(twice.file, "WM/Category") != std::string::npos);
	CHECK(find(twice.file, "WM/SharedUserRating") != std::string::npos);
	CHECK_EQ(count_of(top_level(twice.file), "moov"), 1u);
}

TEST("an mp4 whose last box runs to the end of the file is closed at its end")
{
	const auto original = make_movie({.mdat_to_end = true});
	const auto meta = rated(3);
	const auto result = update_live(original, {&meta});

	const auto mdat = find(original, "mdat") - 4;
	CHECK(libfiles::detail::load_be32(result.file.data() + mdat) == media.size() + 8);
	CHECK_EQ(rating_of(result.file), 3);
}

TEST("an interrupted mp4 update leaves a file that reads whole")
{
	const auto meta = rated(4);
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 4;
	u.legacy.keywords = std::vector<std::string>{"beach"};

	// A new packet and a movie box that moves, after a last box that runs to the end of the file.
	CHECK(check_interruptions(make_movie({.udta = true, .mdat_to_end = true}), u, -100, 4) > 4);

	// A quicktime movie box that moves, holding the packet.
	CHECK(check_interruptions(make_movie({.brand = "qt  ", .moov_last = true}), u, -100, 4) > 2);

	// A packet that outgrows its box, and user data that grows into its free space.
	const auto first = update_live(make_movie({.udta_free = 512}), {&meta});
	libfiles::xmp::meta bigger = rated(2);
	bigger.set_property(libfiles::xmp::ns::dc, "source", std::string(5000, 'x'));
	libfiles::metadata_update v{&bigger};
	v.legacy.rating = 2;
	CHECK(check_interruptions(first.file, v, 4, 2) > 2);
}

TEST("an mp4 update frees what an interrupted one left behind")
{
	const auto original = make_movie({.udta = true});
	const auto meta = rated(4);
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 4;

	// Interrupted after the new boxes were written and made part of the file, before the old ones
	// were freed: two movie boxes, of which readers take the first.
	libfiles::reader r(original);
	const auto plan = libfiles::plan_update(r, u);
	recording_stream live(original);
	libfiles::write_in_place(live, plan);

	auto interrupted = original;
	for (const auto& w : live.writes) if (w.flushes_before == 0) recording_stream::apply(interrupted, w);
	CHECK_EQ(count_of(top_level(interrupted), "moov"), 2u);
	CHECK_EQ(rating_of(interrupted), 4);

	const auto again = rated(5);
	libfiles::metadata_update v{&again};
	v.legacy.rating = 5;
	const auto result = update_live(interrupted, v);

	const auto types = top_level(result.file);
	CHECK_EQ(count_of(types, "moov"), 1u);
	CHECK_EQ(count_of(types, "uuid"), 1u);
	CHECK_EQ(rating_of(result.file), 5);
	CHECK(media_unmoved(original, result.file));
}

TEST("a quicktime file keeps its packet in the movie's user data")
{
	const auto original = make_movie({.brand = "qt  ", .moov_last = true});
	const auto meta = rated(5);
	const auto result = update_live(original, {&meta});

	CHECK(result.plan.patchable());
	CHECK(media_unmoved(original, result.file));
	CHECK_EQ(rating_of(result.file), 5);
	CHECK(find(result.file, "XMP_") != std::string::npos);
	CHECK(find(result.file, "uuid") == std::string::npos);

	// The second packet fits the first.
	const auto again = rated(1);
	const auto twice = update_live(result.file, {&again});
	CHECK(twice.plan.in_place());
	CHECK_EQ(rating_of(twice.file), 1);
}

TEST("an mp4 takes a created date in its movie header and a copyright in cprt")
{
	const auto original = make_movie({.udta = true});
	const auto meta = rated(1);
	libfiles::metadata_update u{&meta};
	u.legacy.date_digitized = "1904-01-02T00:00:10";
	u.legacy.copyright = "(c) someone";

	const auto result = update_live(original, u);
	const auto mvhd = find(result.file, "mvhd", original.size());
	CHECK(mvhd != std::string::npos);
	CHECK_EQ(libfiles::detail::load_be32(result.file.data() + mvhd + 8), 86410u);

	const auto c = find(result.file, "cprt");
	CHECK(c != std::string::npos && find(result.file, "(c) someone", c) != std::string::npos);
}

TEST("an mp4 rating that keeps the Xtra box's size is a small patch")
{
	const auto meta = rated(3);
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 3;
	const auto once = update_live(make_movie({.udta = true}), u);

	const auto again = rated(5);
	libfiles::metadata_update v{&again};
	v.legacy.rating = 5;
	const auto twice = update_live(once.file, v);

	CHECK(twice.plan.in_place());
	std::uint64_t written = 0;
	for (const auto& r : twice.plan.replacements) written += r.data.size();
	CHECK(written < 3000);
}
