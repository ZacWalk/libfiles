// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for writing ASF files. Every update here is applied as a live file would take it, in
// place, unless it must rewrite the file; the media bytes are checked to survive, and the fields are
// read back with a parser of the tests' own.

#include "test.h"

#include "builders.h"
#include "interruptions.h"
#include "legacy.h"
#include "libfiles/write.h"

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	using guid = std::array<std::uint8_t, 16>;

	constexpr guid header_id = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	constexpr guid data_id = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	constexpr guid file_properties_id = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
	constexpr guid content_description_id = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	constexpr guid extended_content_id = {0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
	constexpr guid header_extension_id = {0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11, 0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
	constexpr guid padding_id = {0x74, 0xD4, 0x06, 0x18, 0xDF, 0xCA, 0x09, 0x45, 0xA4, 0xBA, 0x9A, 0xAB, 0xCB, 0x96, 0xAA, 0xE8};
	constexpr guid xmp_id = {0xCB, 0xCF, 0x7A, 0xBE, 0xA9, 0x97, 0xE8, 0x42, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};
	constexpr guid simple_index_id = {0x90, 0x08, 0x00, 0x33, 0xB1, 0xE5, 0xCF, 0x11, 0x89, 0xF4, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xCB};

	const bytes media = bytes(64, 0xAB);

	bytes le64(const std::uint64_t v)
	{
		bytes out;
		for (auto i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
		return out;
	}

	std::uint64_t load_le64(const bytes& b, const std::size_t at)
	{
		std::uint64_t v = 0;
		for (auto i = 7; i >= 0; --i) v = v << 8 | b[at + static_cast<std::size_t>(i)];
		return v;
	}

	unsigned load_le16(const bytes& b, const std::size_t at)
	{
		return b[at] | static_cast<unsigned>(b[at + 1]) << 8;
	}

	bytes object(const guid& id, const bytes& payload)
	{
		return builder().raw(bytes(id.begin(), id.end())).raw(le64(24 + payload.size())).raw(payload).b;
	}

	bool is(const bytes& file, const std::size_t at, const guid& id)
	{
		return file.size() >= at + 16 && std::equal(id.begin(), id.end(), file.begin() + static_cast<std::ptrdiff_t>(at));
	}

	bytes utf16z(const std::string_view text)
	{
		auto out = libfiles::detail::utf16le(text);
		out.push_back(0);
		out.push_back(0);
		return out;
	}

	libfiles::xmp::meta rated(const int rating)
	{
		libfiles::xmp::meta meta;
		meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", rating);
		return meta;
	}

	bytes xmp_object(const libfiles::xmp::meta& meta)
	{
		const auto packet = meta.serialize({});
		return object(xmp_id, bytes(packet.begin(), packet.end()));
	}

	struct file_parts
	{
		bool content_description = false;
		// The size of a padding object inside the header extension, as encoders leave one.
		std::uint32_t extension_padding = 0;
		bool broadcast = false;
		std::vector<int> packets;
	};

	bytes make_asf(const file_parts& p)
	{
		builder properties;
		properties.zeros(16).raw(le64(0)).raw(le64(0)).raw(le64(1)).zeros(24);
		properties.le32(p.broadcast ? 1 : 2).le32(64).le32(64).le32(1000);

		bytes extension_data;
		if (p.extension_padding != 0) extension_data = object(padding_id, bytes(p.extension_padding - 24, 0));

		builder extension;
		extension.raw(bytes{0x11, 0xD2, 0xD3, 0xAB, 0xBA, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65});
		extension.le16(6).le32(static_cast<std::uint32_t>(extension_data.size())).raw(extension_data);

		std::vector<bytes> children = {object(file_properties_id, properties.b), object(header_extension_id, extension.b)};

		if (p.content_description)
		{
			const auto title = utf16z("Old title");
			const auto rating = utf16z("PG");
			builder cd;
			cd.le16(static_cast<unsigned>(title.size())).le16(0).le16(0).le16(0).le16(static_cast<unsigned>(rating.size()));
			cd.raw(title).raw(rating);
			children.push_back(object(content_description_id, cd.b));
		}

		builder header;
		header.le32(static_cast<std::uint32_t>(children.size())).u8(1).u8(2);
		for (const auto& c : children) header.raw(c);

		auto file = object(header_id, header.b);
		const auto data = object(data_id, builder().zeros(16).raw(le64(1)).u8(1).u8(1).raw(media).b);
		file.insert(file.end(), data.begin(), data.end());

		const auto index = object(simple_index_id, bytes(32, 0x22));
		file.insert(file.end(), index.begin(), index.end());

		for (const auto rating : p.packets)
		{
			const auto x = xmp_object(rated(rating));
			file.insert(file.end(), x.begin(), x.end());
		}

		const auto size = le64(file.size());
		std::ranges::copy(size, file.begin() + 30 + 40);
		return file;
	}

	struct written
	{
		bytes file;
		libfiles::write_plan plan;
	};

	// Applied as to a live file when the plan allows it, otherwise rewritten in one pass.
	written update(const bytes& file, const libfiles::metadata_update& u)
	{
		libfiles::reader r(file);
		auto plan = libfiles::plan_update(r, u);

		if (plan.patchable())
		{
			libfiles::memory_stream live(file);
			libfiles::write_in_place(live, plan);
			return {live.bytes(), std::move(plan)};
		}

		libfiles::memory_stream out;
		if (plan.ok()) libfiles::write_updated(r, plan, out);
		return {out.bytes(), std::move(plan)};
	}

	// The types of the top-level objects, as FFmpeg walks them; empty when one runs past the end.
	std::vector<std::string> top_level(const bytes& file)
	{
		std::vector<std::string> names;
		std::size_t pos = 0;

		while (file.size() - pos >= 24)
		{
			const auto size = load_le64(file, pos + 16);
			if (size < 24 || size > file.size() - pos) return {};

			if (is(file, pos, header_id)) names.emplace_back("header");
			else if (is(file, pos, data_id)) names.emplace_back("data");
			else if (is(file, pos, xmp_id)) names.emplace_back("xmp");
			else if (is(file, pos, padding_id)) names.emplace_back("padding");
			else if (is(file, pos, simple_index_id)) names.emplace_back("index");
			else names.emplace_back("?");

			pos += static_cast<std::size_t>(size);
		}

		return names;
	}

	// The rating in the first packet after the data, the one FFmpeg reads.
	int ffmpeg_rating(const bytes& file)
	{
		std::size_t pos = 0;
		auto after_data = false;

		while (file.size() - pos >= 24)
		{
			const auto size = static_cast<std::size_t>(load_le64(file, pos + 16));
			if (size < 24 || size > file.size() - pos) return -99;

			if (after_data && is(file, pos, xmp_id))
			{
				libfiles::xmp::meta meta;
				meta.parse(std::string(file.begin() + static_cast<std::ptrdiff_t>(pos + 24), file.begin() + static_cast<std::ptrdiff_t>(pos + size)));
				std::int32_t rating = -98;
				meta.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating);
				return rating;
			}

			after_data = after_data || is(file, pos, data_id);
			pos += size;
		}

		return -100;
	}

	int rating_of(const bytes& file)
	{
		libfiles::reader r(file);
		const auto packet = libfiles::read_packet(r);
		if (!packet.readable) return -101;
		if (packet.packet.empty()) return -100;

		libfiles::xmp::meta meta;
		meta.parse(packet.packet);
		std::int32_t rating = -99;
		meta.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating);
		return rating;
	}

	bool media_kept(const bytes& file)
	{
		const std::string s(file.begin(), file.end());
		return s.find(std::string(media.begin(), media.end())) != std::string::npos;
	}

	// What the header holds, read the way a player reads it.
	struct header_view
	{
		std::uint64_t header_size = 0;
		std::uint32_t count = 0;
		std::uint64_t file_size = 0;
		std::uint64_t created = 0;
		std::vector<bytes> description_fields;
		std::vector<bytes> categories;
		std::optional<std::uint32_t> shared_rating;
		std::uint64_t padding = 0;
	};

	header_view read_header(const bytes& file)
	{
		header_view v;
		v.header_size = load_le64(file, 16);
		v.count = static_cast<std::uint32_t>(file[24] | file[25] << 8 | file[26] << 16 | file[27] << 24);
		std::size_t pos = 30;

		while (pos + 24 <= v.header_size)
		{
			const auto size = static_cast<std::size_t>(load_le64(file, pos + 16));

			if (is(file, pos, file_properties_id))
			{
				v.file_size = load_le64(file, pos + 40);
				v.created = load_le64(file, pos + 48);
			}
			else if (is(file, pos, content_description_id))
			{
				auto at = pos + 34;
				for (auto i = 0u; i < 5; ++i)
				{
					const auto length = load_le16(file, pos + 24 + 2 * i);
					v.description_fields.emplace_back(file.begin() + static_cast<std::ptrdiff_t>(at), file.begin() + static_cast<std::ptrdiff_t>(at + length));
					at += length;
				}
			}
			else if (is(file, pos, extended_content_id))
			{
				auto at = pos + 26;
				for (auto i = 0u; i < load_le16(file, pos + 24); ++i)
				{
					const auto name_length = load_le16(file, at);
					const bytes name(file.begin() + static_cast<std::ptrdiff_t>(at + 2), file.begin() + static_cast<std::ptrdiff_t>(at + 2 + name_length));
					const auto value_at = at + 2 + name_length + 4;
					const auto value_length = load_le16(file, value_at - 2);
					const bytes value(file.begin() + static_cast<std::ptrdiff_t>(value_at), file.begin() + static_cast<std::ptrdiff_t>(value_at + value_length));

					if (name == utf16z("WM/Category")) v.categories.push_back(value);
					if (name == utf16z("WM/SharedUserRating")) v.shared_rating = static_cast<std::uint32_t>(value[0] | value[1] << 8 | value[2] << 16 | value[3] << 24);
					at = value_at + value_length;
				}
			}
			else if (is(file, pos, header_extension_id))
			{
				for (auto at = pos + 46; at + 24 <= pos + size; at += static_cast<std::size_t>(load_le64(file, at + 16)))
				{
					if (is(file, at, padding_id)) v.padding += load_le64(file, at + 16);
				}
			}
			else if (is(file, pos, padding_id))
			{
				v.padding += size;
			}

			pos += size;
		}

		return v;
	}

	libfiles::metadata_update everything(const libfiles::xmp::meta* meta)
	{
		libfiles::metadata_update u{meta};
		u.legacy.title = "New title";
		u.legacy.creators = std::vector<std::string>{"Ann", "Bob"};
		u.legacy.copyright = "(c) someone";
		u.legacy.description = "A description";
		u.legacy.keywords = std::vector<std::string>{"beach", "Zürich"};
		u.legacy.rating = 4;
		u.legacy.date_digitized = "1601-01-02T00:00:01Z";
		return u;
	}
}

TEST("an asf is told apart by its header object")
{
	CHECK(libfiles::detect_file_format(make_asf({})) == libfiles::file_format::asf);
	CHECK(libfiles::can_update(libfiles::file_format::asf));
}

TEST("an asf gains its packet in an object appended after everything")
{
	const auto original = make_asf({});
	const auto meta = rated(4);
	const auto result = update(original, {&meta});

	CHECK(result.plan.patchable());
	CHECK(!result.plan.in_place());
	CHECK_EQ(rating_of(result.file), 4);
	CHECK_EQ(ffmpeg_rating(result.file), 4);
	CHECK(top_level(result.file) == std::vector<std::string>({"header", "data", "index", "xmp"}));
	CHECK_EQ(read_header(result.file).file_size, result.file.size());
	CHECK(media_kept(result.file));
}

TEST("an asf packet that fits is patched where it lies")
{
	const auto first = rated(2);
	const auto once = update(make_asf({}), {&first});
	const auto again = rated(5);
	const auto twice = update(once.file, {&again});

	CHECK(twice.plan.in_place());
	CHECK_EQ(twice.file.size(), once.file.size());
	CHECK_EQ(rating_of(twice.file), 5);
}

TEST("an asf packet that outgrows its object moves to the end")
{
	const auto original = make_asf({.packets = {1}});
	auto bigger = rated(2);
	bigger.set_property(libfiles::xmp::ns::dc, "source", std::string(5000, 'x'));
	const auto result = update(original, {&bigger});

	CHECK(result.plan.patchable());
	CHECK(top_level(result.file) == std::vector<std::string>({"header", "data", "index", "padding", "xmp"}));
	CHECK_EQ(rating_of(result.file), 2);
	CHECK_EQ(ffmpeg_rating(result.file), 2);
	CHECK_EQ(read_header(result.file).file_size, result.file.size());
}

TEST("an asf header takes an edit's fields into its padding")
{
	const auto original = make_asf({.content_description = true, .extension_padding = 2048, .packets = {}});
	const auto meta = rated(4);
	const auto result = update(original, everything(&meta));

	CHECK(result.plan.patchable());
	const auto header = read_header(result.file);
	CHECK_EQ(header.header_size, read_header(original).header_size);
	CHECK(header.padding > 0 && header.padding < 2048);

	CHECK(header.description_fields.size() == 5);
	if (header.description_fields.size() == 5)
	{
		CHECK(header.description_fields[0] == utf16z("New title"));
		CHECK(header.description_fields[1] == utf16z("Ann; Bob"));
		CHECK(header.description_fields[2] == utf16z("(c) someone"));
		CHECK(header.description_fields[3] == utf16z("A description"));
		CHECK(header.description_fields[4] == utf16z("PG"));
	}

	CHECK(header.categories == std::vector<bytes>({utf16z("beach"), utf16z("Zürich")}));
	CHECK(header.shared_rating == std::optional<std::uint32_t>(75));
	CHECK_EQ(header.created, 86401ull * 10'000'000);
	CHECK_EQ(header.file_size, result.file.size());
	CHECK(media_kept(result.file));

	// A second edit's rating, the same size, changes only its value.
	libfiles::metadata_update v{&meta};
	v.legacy.rating = 5;
	const auto again = update(result.file, v);
	CHECK(again.plan.in_place());
	CHECK(read_header(again.file).shared_rating == std::optional<std::uint32_t>(99));
	CHECK(read_header(again.file).categories.size() == 2);
}

TEST("an asf header without room is rewritten in one pass, with room for the next edit")
{
	const auto original = make_asf({.content_description = true, .packets = {1}});
	const auto meta = rated(3);
	const auto result = update(original, everything(&meta));

	CHECK(result.plan.ok());
	CHECK(!result.plan.patchable());
	CHECK(media_kept(result.file));
	CHECK(top_level(result.file) == std::vector<std::string>({"header", "data", "index", "xmp"}));
	CHECK_EQ(rating_of(result.file), 3);

	const auto header = read_header(result.file);
	CHECK(header.header_size > read_header(original).header_size);
	CHECK(header.padding >= 2048);
	CHECK_EQ(header.file_size, result.file.size());

	libfiles::metadata_update v{&meta};
	v.legacy.title = "Another title";
	v.legacy.keywords = std::vector<std::string>{"one", "two", "three"};
	const auto again = update(result.file, v);
	CHECK(again.plan.in_place());
	const auto fields = read_header(again.file).description_fields;
	CHECK(!fields.empty() && fields[0] == utf16z("Another title"));
}

TEST("an interrupted asf update leaves a file that reads whole")
{
	const auto original = make_asf({.content_description = true, .extension_padding = 2048, .packets = {1}});
	auto bigger = rated(2);
	bigger.set_property(libfiles::xmp::ns::dc, "source", std::string(5000, 'x'));
	const auto u = everything(&bigger);

	libfiles::reader r(original);
	const auto plan = libfiles::plan_update(r, u);
	CHECK(plan.patchable());

	recording_stream live(original);
	libfiles::write_in_place(live, plan);

	const auto states = interrupted_states(original, live.writes);
	CHECK(states.size() > 4);

	for (const auto& state : states)
	{
		CHECK(!top_level(state).empty());
		CHECK(media_kept(state));
		const auto rating = rating_of(state);
		CHECK(rating == 1 || rating == 2);
		CHECK_EQ(ffmpeg_rating(state), rating);
	}

	CHECK_EQ(rating_of(states.back()), 2);
}

TEST("an asf update turns packets after the first into padding")
{
	const auto original = make_asf({.packets = {1, 3}});
	CHECK_EQ(rating_of(original), 1);

	const auto meta = rated(2);
	const auto result = update(original, {&meta});
	const auto objects = top_level(result.file);
	CHECK_EQ(std::ranges::count(objects, std::string("xmp")), 1);
	CHECK_EQ(rating_of(result.file), 2);
	CHECK_EQ(ffmpeg_rating(result.file), 2);
}

TEST("a broadcast asf keeps the date and size it does not know")
{
	const auto original = make_asf({.extension_padding = 2048, .broadcast = true, .packets = {}});
	const auto meta = rated(4);
	const auto result = update(original, everything(&meta));

	const auto header = read_header(result.file);
	CHECK_EQ(header.created, 0u);
	CHECK_EQ(header.file_size, read_header(original).file_size);
	CHECK(header.shared_rating == std::optional<std::uint32_t>(75));
}
