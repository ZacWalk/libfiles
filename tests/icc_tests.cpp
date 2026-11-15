// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Tests of the ICC listing: what it says of a profile, the text it decodes, the tags it shares
// and refuses, what it lists nothing for, and its outcome over the corpus, whole, cut short and corrupted,
// held to the record Diffractor's listing left before it moved here. And of mapping the colours a profile
// describes to sRGB, and an edit of that rendition back into them.

#include "test.h"
#include "icc_corpus.h"

#include "libfiles/icc.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace libfiles;

namespace
{
	// The corpus' profiles, by their place in icc_corpus::profiles().
	enum corpus_profile : std::size_t
	{
		srgb,
		display_p3,
		adobe_rgb,
		rec2020,
		prophoto,
		unrecognised,
		greyscale,
		press,
		odd_text,
		lying_directory,
		overlapping_regions,
		overcounted_directory,
		short_block,
		small_size,
		large_size,
		unsigned_header,
		header_only,
		too_many_tags,
		a_thousand_tags,
		no_tags
	};

	const icc_corpus::bytes& corpus(const corpus_profile which)
	{
		static const auto all = icc_corpus::profiles();
		return all[which];
	}

	// The rows from the container with the given id to the next container.
	std::vector<structure_row> section(const std::vector<structure_row>& rows, const std::string_view id)
	{
		std::vector<structure_row> result;
		auto inside = false;

		for (const auto& row : rows)
		{
			if (row.container) inside = row.id == id;
			else if (inside) result.push_back(row);
		}

		return result;
	}

	std::string value_of(const std::vector<structure_row>& rows, const std::string_view key)
	{
		for (const auto& row : rows)
			if (row.key == key) return row.value;
		return "(absent)";
	}

	const structure_row* find_id(const std::vector<structure_row>& rows, const std::string_view id)
	{
		for (const auto& row : rows)
			if (row.id == id) return &row;
		return nullptr;
	}

	int count_ids(const std::vector<structure_row>& rows, const std::string_view prefix)
	{
		auto n = 0;
		for (const auto& row : rows)
			if (row.id.starts_with(prefix)) ++n;
		return n;
	}

	std::vector<icc_corpus::row> rows_of(const std::vector<structure_row>& listing)
	{
		std::vector<icc_corpus::row> rows;

		for (const auto& r : listing)
		{
			icc_corpus::row row;
			row.key = r.key;
			row.value = r.value;
			row.shape = r.shape;
			row.id = r.id;
			row.depth = r.depth;
			row.container = r.container;
			row.open_by_default = r.open_by_default;

			if (const auto* bytes = std::get_if<std::vector<std::uint8_t>>(&r.detail))
			{
				row.has_bytes = true;
				row.bytes = *bytes;
			}

			rows.push_back(row);
		}

		return rows;
	}
}

TEST("icc RGB declaration is bounded and distinct from whole-profile validation")
{
	CHECK(icc_declares_rgb(corpus(srgb)));
	CHECK(!icc_declares_rgb(corpus(press)));
	CHECK(!icc_declares_rgb(corpus(greyscale)));
	for (std::size_t length = 0; length < 20; ++length) CHECK(!icc_declares_rgb(std::span(corpus(srgb)).first(length)));
	CHECK(icc_declares_rgb(std::span(corpus(srgb)).first(20)));
}

TEST("icc summary names a profile, its space and primaries, tone response, white point and intent")
{
	const auto rows = list_icc(corpus(srgb));
	CHECK(!rows.empty());
	CHECK_STR(rows.front().key, "Summary");
	CHECK(rows.front().container);
	CHECK_STR(rows.front().id, "icc.summary");

	const auto summary = section(rows, "icc.summary");
	CHECK_STR(value_of(summary, "Profile"), "sRGB IEC61966-2.1");
	CHECK_STR(value_of(summary, "Device model"), "IEC 61966-2.1 Default RGB colour space - sRGB");
	CHECK_STR(value_of(summary, "Device manufacturer"), "IEC http://www.iec.ch");
	CHECK_STR(value_of(summary, "Copyright"), "Copyright (c) 1998 Hewlett-Packard Company");
	CHECK_STR(value_of(summary, "Colour space"), "RGB (RGB )");
	CHECK_STR(value_of(summary, "Primaries"), "approximately sRGB");
	CHECK_STR(value_of(summary, "Tone response"), "curve, 1024 points");
	CHECK_STR(value_of(summary, "White point"), "D50 (x 0.3457, y 0.3585)");
	CHECK_STR(value_of(summary, "Rendering intent"), "Perceptual");
	for (const auto& row : summary) CHECK_EQ(row.depth, 1);
}

TEST("icc summary reads as the listing's summary, field by field")
{
	for (const auto which : {srgb, display_p3, adobe_rgb, greyscale, press, odd_text})
	{
		const auto& profile = corpus(which);
		const auto summary = libfiles::summarize_icc(profile);
		CHECK(summary.has_value());
		if (!summary) continue;

		// The listing leaves out what the summary leaves empty.
		const auto listed = section(list_icc(profile), "icc.summary");
		const auto value = [&listed](const std::string_view key)
		{
			const auto v = value_of(listed, key);
			return v == "(absent)" ? std::string{} : v;
		};
		CHECK_STR(summary->name, value("Profile"));
		CHECK_STR(summary->device_model, value("Device model"));
		CHECK_STR(summary->device_manufacturer, value("Device manufacturer"));
		CHECK_STR(summary->copyright, value("Copyright"));
		CHECK_STR(summary->colour_space, value("Colour space"));
		CHECK_STR(summary->primaries, value("Primaries"));
		CHECK_STR(summary->tone_response, value("Tone response"));
		CHECK_STR(summary->white_point, value("White point"));
		CHECK_STR(summary->rendering_intent, value("Rendering intent"));
	}

	CHECK_STR(libfiles::summarize_icc(corpus(srgb))->name, "sRGB IEC61966-2.1");
	CHECK_STR(libfiles::summarize_icc(corpus(srgb))->primaries, "approximately sRGB");
	CHECK(!libfiles::summarize_icc({}).has_value());
	CHECK(!libfiles::summarize_icc(corpus(short_block)).has_value());
	CHECK(!libfiles::summarize_icc(corpus(unsigned_header)).has_value());
}

TEST("icc header is listed whole, each signature with its meaning")
{
	const auto header = section(list_icc(corpus(srgb)), "icc.header");
	CHECK_STR(value_of(header, "Profile size"), "2688 bytes");
	CHECK_STR(value_of(header, "Preferred CMM"), "lcms");
	CHECK_STR(value_of(header, "Version"), "2.1.0");
	CHECK_STR(value_of(header, "Class"), "Display device (mntr)");
	CHECK_STR(value_of(header, "Data colour space"), "RGB (RGB )");
	CHECK_STR(value_of(header, "Connection space"), "CIEXYZ (XYZ )");
	CHECK_STR(value_of(header, "Created"), "2026-10-31 12:30:15");
	CHECK_STR(value_of(header, "File signature"), "acsp");
	CHECK_STR(value_of(header, "Primary platform"), "Microsoft (MSFT)");
	CHECK_STR(value_of(header, "Profile flags"), "0x00000000");
	CHECK_STR(value_of(header, "Device manufacturer"), "IEC ");
	CHECK_STR(value_of(header, "Device model"), "sRGB");
	CHECK_STR(value_of(header, "Device attributes"), "0x0000000000000000");
	CHECK_STR(value_of(header, "Rendering intent"), "Perceptual (0)");
	CHECK_STR(value_of(header, "PCS illuminant"), "0.9642, 1.0000, 0.8249");
	CHECK_STR(value_of(header, "Creator"), "HP  ");

	// Signatures nothing names are shown as their code, or as "(none)" when they are zero.
	const auto rows = list_icc(corpus(unrecognised));
	const auto odd = section(rows, "icc.header");
	CHECK_STR(value_of(odd, "Preferred CMM"), "(none)");
	CHECK_STR(value_of(odd, "Class"), "Colour space conversion (spac)");
	CHECK_STR(value_of(odd, "Data colour space"), "YCbCr (YCbr)");
	CHECK_STR(value_of(odd, "Connection space"), "CIELAB (Lab )");
	CHECK_STR(value_of(odd, "Created"), "(absent)");
	CHECK_STR(value_of(odd, "Primary platform"), "ZZZZ");
	CHECK_STR(value_of(odd, "Profile flags"), "0x80000001");
	CHECK_STR(value_of(odd, "Device attributes"), "0xfedcba9876543210");
	CHECK_STR(value_of(odd, "Rendering intent"), "7");
	CHECK_STR(value_of(odd, "Creator"), "(none)");
	CHECK_STR(value_of(section(rows, "icc.summary"), "Rendering intent"), "7");
	CHECK_STR(value_of(section(list_icc(corpus(greyscale)), "icc.header"), "Primary platform"), "Not identified");
	CHECK_STR(value_of(section(list_icc(corpus(press)), "icc.header"), "Created"), "2001-01-01 00:00:00");
}

TEST("icc summary names the colorant sets, tone curves and white points people recognise")
{
	struct expected
	{
		corpus_profile profile;
		std::string_view primaries;
		std::string_view curve;
		std::string_view white;
		std::string_view intent;
	};

	const expected cases[] = {
		{srgb, "approximately sRGB", "curve, 1024 points", "D50 (x 0.3457, y 0.3585)", "Perceptual"},
		{display_p3, "approximately Display P3", "sRGB style curve", "D50 (x 0.3457, y 0.3585)", "Perceptual"},
		{adobe_rgb, "approximately Adobe RGB (1998)", "gamma 2.20", "D65 (x 0.3127, y 0.3290)", "Perceptual"},
		{rec2020, "approximately Rec. 2020", "gamma 2.40", "D55 (x 0.3324, y 0.3474)", "Relative colorimetric"},
		{prophoto, "approximately ProPhoto RGB", "linear", "D93 (x 0.2831, y 0.2971)", "Saturation"},
		{unrecognised, "custom", "parametric, type 4", "x 0.3878, y 0.4082", "7"},
		{greyscale, "(absent)", "(absent)", "0.0000, 0.0000, 0.0000", "Perceptual"},
		{press, "(absent)", "(absent)", "D50 (x 0.3457, y 0.3585)", "Absolute colorimetric"},
	};

	for (const auto& c : cases)
	{
		const auto summary = section(list_icc(corpus(c.profile)), "icc.summary");
		CHECK_STR(value_of(summary, "Primaries"), c.primaries);
		CHECK_STR(value_of(summary, "Tone response"), c.curve);
		CHECK_STR(value_of(summary, "White point"), c.white);
		CHECK_STR(value_of(summary, "Rendering intent"), c.intent);
	}

	CHECK_STR(value_of(section(list_icc(corpus(greyscale)), "icc.summary"), "Colour space"), "Greyscale (GRAY)");
	CHECK_STR(value_of(section(list_icc(corpus(press)), "icc.summary"), "Colour space"), "CMYK (CMYK)");
}

TEST("icc lists every tag by signature with its type, size and leading bytes")
{
	const auto rows = list_icc(corpus(srgb));
	const auto* title = find_id(rows, "icc.tags");
	CHECK(title != nullptr && title->container);
	if (title)
	{
		CHECK_STR(title->key, "Tags (14)");
		CHECK_STR(title->value, "14 declared");
	}

	const auto tags = section(rows, "icc.tags");
	CHECK_EQ(tags.size(), std::size_t{14});
	if (tags.size() == 14)
	{
		CHECK_STR(tags.front().key, "bTRC");
		CHECK_STR(tags.back().key, "wtpt");
	}

	const auto* desc = find_id(rows, "icc.tag.desc");
	CHECK(desc != nullptr);
	if (desc)
	{
		CHECK_STR(desc->key, "desc");
		CHECK_STR(desc->value, "sRGB IEC61966-2.1");
		CHECK_STR(desc->shape, "desc, 30 bytes");
		CHECK_EQ(desc->depth, 1);
		const auto* bytes = std::get_if<std::vector<std::uint8_t>>(&desc->detail);
		CHECK(bytes != nullptr && bytes->size() == 26);
		if (bytes && bytes->size() == 26) CHECK_EQ((*bytes)[8], std::uint8_t{'s'});
	}

	CHECK_STR(value_of(tags, "lumi"), "76.0365, 80.0000, 87.1246");
	CHECK_STR(value_of(tags, "rXYZ"), "0.4360, 0.2225, 0.0139");
	CHECK_STR(value_of(tags, "tech"), "binary, 12 bytes");
	const auto* tech = find_id(rows, "icc.tag.tech");
	if (tech) CHECK_STR(tech->shape, "sig , 12 bytes");

	// An inspector is shown a tag's first 4,096 bytes, whatever its size.
	icc_corpus::header h;
	const auto big = list_icc(icc_corpus::profile(h, {{0x6269676Eu, icc_corpus::opaque(0x62696E79u, 5000, 4)}}));
	const auto* large = find_id(big, "icc.tag.bign");
	CHECK(large != nullptr);
	if (large)
	{
		CHECK_STR(large->value, "binary, 5008 bytes");
		const auto* bytes = std::get_if<std::vector<std::uint8_t>>(&large->detail);
		CHECK(bytes != nullptr && bytes->size() == 4096);
	}
}

TEST("icc decodes text tags exactly, whatever their length")
{
	const std::string long_profile(512, 'P');
	const std::string long_copyright(512, 'C');
	icc_corpus::header h;
	const auto rows = list_icc(icc_corpus::profile(h, {{0x64657363u, icc_corpus::desc(long_profile)},
	                                                   {0x63707274u, icc_corpus::text(long_copyright)}}));

	const auto summary = section(rows, "icc.summary");
	CHECK_STR(value_of(summary, "Profile"), long_profile);
	CHECK_STR(value_of(summary, "Copyright"), long_copyright);
	const auto tags = section(rows, "icc.tags");
	CHECK_STR(value_of(tags, "desc"), long_profile);
	CHECK_STR(value_of(tags, "cprt"), long_copyright);
}

TEST("icc text tags read to their first NUL, and a tag without text lists by its size")
{
	const auto rows = list_icc(corpus(odd_text));
	const auto summary = section(rows, "icc.summary");
	CHECK_STR(value_of(summary, "Profile"), "(absent)");
	CHECK_STR(value_of(summary, "Copyright"), "(absent)");
	CHECK_STR(value_of(summary, "Device manufacturer"), "(absent)");
	CHECK_STR(value_of(summary, "Device model"), "Model");

	const auto tags = section(rows, "icc.tags");
	CHECK_STR(value_of(tags, "desc"), "binary, 17 bytes"); // a count of nothing
	CHECK_STR(value_of(tags, "cprt"), "binary, 22 bytes"); // a count beyond the tag
	CHECK_STR(value_of(tags, "dmnd"), "binary, 21 bytes"); // a NUL first
	CHECK_STR(value_of(tags, "dmdd"), "Model");            // no NUL at all
	CHECK_STR(value_of(tags, "tst1"), "binary, 12 bytes"); // nothing but NULs
	CHECK_STR(value_of(tags, "tst7"), "Inner");            // a NUL inside
	CHECK_STR(value_of(tags, "tst<"), "binary, 8 bytes");  // a desc too short for its count
	CHECK_STR(value_of(tags, "tst="), "binary, 8 bytes");  // an mluc too short for its records
}

TEST("icc decodes UTF-16 text by each record's offset")
{
	icc_corpus::header h;
	const auto profile_of = [&h](icc_corpus::bytes tag)
	{
		return list_icc(icc_corpus::profile(h, {{0x64657363u, std::move(tag)}}));
	};

	{
		const auto rows = profile_of(icc_corpus::mluc({u"AB"}));
		CHECK_STR(value_of(section(rows, "icc.summary"), "Profile"), "AB");
		CHECK_STR(value_of(section(rows, "icc.tags"), "desc"), "AB");
	}

	const std::u16string long_text(300, u'L');
	CHECK_STR(value_of(section(profile_of(icc_corpus::mluc({long_text})), "icc.summary"), "Profile"),
	          std::string(300, 'L'));
	CHECK_STR(value_of(section(profile_of(icc_corpus::mluc({u"First", u"Second"})), "icc.summary"), "Profile"),
	          "First");

	{
		auto odd = icc_corpus::mluc({u"Odd"});
		icc_corpus::put32(odd, 20, 5);
		CHECK_STR(value_of(section(profile_of(odd), "icc.tags"), "desc"), "binary, 34 bytes");
	}

	{
		auto beyond = icc_corpus::mluc({u"Bad"});
		icc_corpus::put32(beyond, 24, 0xFFFFFFFF);
		CHECK_STR(value_of(section(profile_of(beyond), "icc.tags"), "desc"), "binary, 34 bytes");
	}

	const auto tags = section(list_icc(corpus(odd_text)), "icc.tags");
	CHECK_STR(value_of(tags, "tst2"), "A\xEF\xBF\xBD"
	                                  "B");                      // a lead surrogate alone
	CHECK_STR(value_of(tags, "tst3"), "\xEF\xBF\xBDx");          // a trail surrogate alone
	CHECK_STR(value_of(tags, "tst4"), "\xF0\x9F\x98\x80 smile"); // a pair
	CHECK_STR(value_of(tags, "tst5"), "Wide");                   // records larger than the minimum
	CHECK_STR(value_of(tags, "tst6"), "binary, 24 bytes");       // records smaller than it
	CHECK_STR(value_of(tags, "tst9"), "Later");                  // the first record's offset inside its header
	CHECK_STR(value_of(tags, "tst;"), "binary, 36 bytes");       // more records than the tag holds
}

// A tag names a region of the profile, and several tags may name the same one: the sRGB profile most
// cameras embed points all three tone curves at a single curve. A budget charging every tag for its
// region refused two of the three as though the profile claimed more bytes than it holds. Regions that
// overlap without being the same are each charged, so a directory cannot copy many times the profile's
// own size.
TEST("icc reads tags that share a region, and charges regions that only overlap")
{
	const auto put32 = icc_corpus::put32;
	const auto make_profile = [&put32](const std::size_t tag_count, const std::size_t payload_bytes)
	{
		icc_corpus::bytes profile(128 + 4 + tag_count * 12 + payload_bytes, 0);
		put32(profile, 0, static_cast<std::uint32_t>(profile.size()));
		put32(profile, 36, 0x61637370u);
		put32(profile, 128, static_cast<std::uint32_t>(tag_count));
		return profile;
	};

	{
		constexpr std::size_t curve_bytes = 12 + 1000 * 2;
		auto profile = make_profile(3, curve_bytes);
		constexpr auto curve_at = static_cast<std::uint32_t>(128 + 4 + 3 * 12);
		put32(profile, curve_at, 0x63757276u);
		put32(profile, curve_at + 8, 1000);
		constexpr std::uint32_t curves[] = {0x72545243u, 0x67545243u, 0x62545243u};

		for (std::size_t i = 0; i < 3; ++i)
		{
			put32(profile, icc_corpus::entry(i), curves[i]);
			put32(profile, icc_corpus::entry(i) + 4, curve_at);
			put32(profile, icc_corpus::entry(i) + 8, static_cast<std::uint32_t>(curve_bytes));
		}

		const auto rows = list_icc(profile);
		CHECK_EQ(count_ids(rows, "icc.tag."), 3);
		CHECK_EQ(count_ids(rows, "icc.unread"), 0);
	}

	{
		constexpr std::size_t tag_count = 100;
		constexpr std::size_t payload_bytes = 4000;
		auto profile = make_profile(tag_count, payload_bytes);
		constexpr auto payload_at = 128 + 4 + tag_count * 12;
		constexpr auto region_bytes = static_cast<std::uint32_t>(payload_bytes - (tag_count - 1));

		for (std::size_t i = 0; i < tag_count; ++i)
		{
			put32(profile, icc_corpus::entry(i), 0x74000000u + static_cast<std::uint32_t>(i));
			put32(profile, icc_corpus::entry(i) + 4, static_cast<std::uint32_t>(payload_at + i));
			put32(profile, icc_corpus::entry(i) + 8, region_bytes);
		}

		const auto read = count_ids(list_icc(profile), "icc.tag.");
		CHECK(read >= 1);
		CHECK(static_cast<std::size_t>(read) * region_bytes <= profile.size());
	}
}

TEST("icc lists the tags it cannot read, with the reason")
{
	{
		const auto rows = list_icc(corpus(lying_directory));
		const auto* title = find_id(rows, "icc.tags");
		CHECK(title != nullptr);
		if (title)
		{
			CHECK_STR(title->key, "Tags (3)");
			CHECK_STR(title->value, "8 declared");
		}

		// Of two tags with one signature, the later is the profile's.
		CHECK_STR(value_of(section(rows, "icc.summary"), "Profile"), "Directory again");

		const auto* unread_title = find_id(rows, "icc.unread");
		CHECK(unread_title != nullptr);
		if (unread_title) CHECK_STR(unread_title->key, "Unread tags (4)");

		const auto unread = section(rows, "icc.unread");
		CHECK_EQ(unread.size(), std::size_t{4});
		if (unread.size() == 4)
		{
			CHECK_STR(unread[0].value, "dir0: out of range (offset 602, size 48)");
			CHECK_STR(unread[1].value, "dir1: out of range (offset 300, size 0)");
			CHECK_STR(unread[2].value, "dir2: out of range (offset 352, size 592)");
			CHECK_STR(unread[3].value, "dir3: truncated");
			for (const auto& row : unread) CHECK_STR(row.key, "Unread tag");
		}
	}

	{
		const auto rows = list_icc(corpus(overlapping_regions));
		CHECK_EQ(count_ids(rows, "icc.tag."), 1);
		const auto unread = section(rows, "icc.unread");
		CHECK_EQ(unread.size(), std::size_t{12});
		if (!unread.empty())
		{
			CHECK_STR(unread.front().value, "ovl1: beyond the profile's own size (1000 bytes)");
			CHECK_STR(unread.back().value, "pad1: beyond the profile's own size (1208 bytes)");
		}
	}

	{
		const auto unread = section(list_icc(icc_corpus::oversized()), "icc.unread");
		CHECK_EQ(unread.size(), std::size_t{1});
		if (!unread.empty()) CHECK_STR(unread.front().value, "bigo: too large to read (1048600 bytes)");
	}
}

TEST("icc lists nothing for a block that is not a profile")
{
	CHECK(list_icc({}).empty());
	CHECK(list_icc(corpus(short_block)).empty());
	CHECK(list_icc(corpus(small_size)).empty());
	CHECK(list_icc(corpus(large_size)).empty());
	CHECK(list_icc(corpus(unsigned_header)).empty());
	CHECK(list_icc(corpus(header_only)).empty());
	CHECK(list_icc(corpus(too_many_tags)).empty());

	const auto thousand_rows = list_icc(corpus(a_thousand_tags));
	const auto* thousand = find_id(thousand_rows, "icc.tags");
	CHECK(thousand != nullptr);
	if (thousand) CHECK_STR(thousand->value, "1000 declared");

	const auto none = list_icc(corpus(no_tags));
	const auto* title = find_id(none, "icc.tags");
	CHECK(title != nullptr);
	if (title)
	{
		CHECK_STR(title->key, "Tags (0)");
		CHECK_STR(title->value, "0 declared");
	}
	CHECK(find_id(none, "icc.unread") == nullptr);
}

// The listing moved from Diffractor, whose metadata_icc.cpp listed this corpus - 7,480 profiles whole, cut
// short and corrupted - as these figures record. Both agreed row for row on it, and on Diffractor's
// fixtures' and Windows' own profiles likewise, before Diffractor's copy was removed.
TEST("icc listing holds to Diffractor's record whole, cut short and corrupted")
{
	icc_corpus::fingerprint print;

	for (const auto& profile : icc_corpus::profiles())
	{
		for (const auto& variant : icc_corpus::variants(profile)) print.add(rows_of(list_icc(variant)));
	}

	for (const auto& variant : icc_corpus::variants(icc_corpus::oversized(), 4)) print.add(rows_of(list_icc(variant)));

	CHECK_EQ(print.listings(), std::size_t{7480});
	CHECK_EQ(print.rows(), std::size_t{94993});
	CHECK_EQ(print.value(), std::uint64_t{4300244743721149556ull});
}

namespace
{
	using rgb = std::array<int, 3>;

	// One row of bgrx8 pixels, each given red first, with a fourth byte a mapping must leave as it is.
	std::vector<std::uint8_t> bgrx_row(const std::vector<rgb>& colours)
	{
		std::vector<std::uint8_t> row;

		for (const auto& [r, g, b] : colours)
		{
			row.insert(row.end(), {static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(g),
			                       static_cast<std::uint8_t>(r), std::uint8_t{0x11}});
		}

		return row;
	}

	image_view row_view(const std::vector<std::uint8_t>& row)
	{
		return {row.data(), row.size(), static_cast<std::uint32_t>(row.size() / 4), 1, pixel_format::bgrx8};
	}

	pixel_buffer row_buffer(std::vector<std::uint8_t>& row)
	{
		return {row.data(), row.size()};
	}

	rgb colour_at(const std::vector<std::uint8_t>& row, const std::size_t x)
	{
		return {row[x * 4 + 2], row[x * 4 + 1], row[x * 4]};
	}

	bool near(const rgb& a, const rgb& b, const int levels)
	{
		for (auto c = 0; c < 3; ++c)
		{
			if (std::abs(a[c] - b[c]) > levels) return false;
		}

		return true;
	}
}

TEST("icc mapping to sRGB is for colours other than sRGB's, of a profile of colorants and curves or a grey curve")
{
	CHECK(!icc_maps_to_srgb(icc_corpus::srgb_parametric()));
	CHECK(icc_maps_to_srgb(corpus(display_p3)));
	CHECK(icc_maps_to_srgb(corpus(adobe_rgb)));
	CHECK(icc_maps_to_srgb(corpus(rec2020)));
	CHECK(icc_maps_to_srgb(corpus(prophoto)));
	CHECK(icc_maps_to_srgb(corpus(greyscale)));

	// A press's CMYK tables, a Lab connection space, and what is no profile.
	CHECK(!icc_maps_to_srgb(corpus(press)));
	CHECK(!icc_maps_to_srgb(corpus(unrecognised)));
	CHECK(!icc_maps_to_srgb(corpus(header_only)));
	CHECK(!icc_maps_to_srgb({}));

	// sRGB's own colours need no mapping, which is not the same as a profile that cannot be mapped.
	CHECK(icc_srgb_mapping(icc_corpus::srgb_parametric()) == srgb_mapping::srgb);
	CHECK(icc_srgb_mapping(corpus(display_p3)) == srgb_mapping::mapped);
	CHECK(icc_srgb_mapping(corpus(press)) == srgb_mapping::unmapped);
	CHECK(icc_srgb_mapping({}) == srgb_mapping::unmapped);

	// What needs no mapping is refused, and nothing written.
	const auto row = bgrx_row({{187, 105, 62}});
	std::vector<std::uint8_t> out(row.size(), 0xAB);
	CHECK(!map_icc_to_srgb(row_view(row), icc_corpus::srgb_parametric(), row_buffer(out)));
	CHECK(out == std::vector<std::uint8_t>(row.size(), 0xAB));

	// Every profile of the corpus, whole, cut short and corrupted, is answered without a fault, and mapped where
	// it says it can be.
	auto mapped_all = true;

	for (const auto& profile : icc_corpus::profiles())
	{
		for (const auto& variant : icc_corpus::variants(profile, 8))
		{
			if (!icc_maps_to_srgb(variant)) continue;
			auto pixels = bgrx_row({{0, 0, 0}, {128, 64, 200}, {255, 255, 255}});
			mapped_all = mapped_all && map_icc_to_srgb(row_view(pixels), variant, row_buffer(pixels));
		}
	}

	CHECK(mapped_all);
}

TEST("icc mapping takes Display P3's colours to sRGB's, white to white, and clips what sRGB cannot show")
{
	const auto& p3 = corpus(display_p3);

	// P3's (187, 105, 62) is sRGB's (200, 100, 50). P3's red is past sRGB's, which it clips to, as colour-managed
	// viewers show it, rather than paling towards grey.
	auto row = bgrx_row({{255, 255, 255}, {0, 0, 0}, {128, 128, 128}, {187, 105, 62}, {255, 0, 0}});
	std::vector<std::uint8_t> out(row.size(), 0xAB);
	CHECK(map_icc_to_srgb(row_view(row), p3, row_buffer(out)));

	CHECK(colour_at(out, 0) == rgb({255, 255, 255}));
	CHECK(colour_at(out, 1) == rgb({0, 0, 0}));
	CHECK(near(colour_at(out, 2), {128, 128, 128}, 1));
	CHECK(near(colour_at(out, 3), {200, 100, 50}, 2));
	CHECK(colour_at(out, 4) == rgb({255, 0, 0}));
	CHECK(out[3] == 0x11 && out[19] == 0x11);

	// In place, to the same.
	CHECK(map_icc_to_srgb(row_view(row), p3, row_buffer(row)));
	CHECK(row == out);
}

TEST("icc mapping keeps sixteen bits a channel, and takes a grey profile's curve to sRGB's")
{
	std::vector<std::uint16_t> wide = {65535, 65535, 65535, 7, 187 * 257, 105 * 257, 62 * 257, 9};
	const image_view view{reinterpret_cast<const std::uint8_t*>(wide.data()), 16, 2, 1, pixel_format::rgbx16};
	CHECK(map_icc_to_srgb(view, corpus(display_p3), {reinterpret_cast<std::uint8_t*>(wide.data()), 16}));

	CHECK(wide[0] == 65535 && wide[1] == 65535 && wide[2] == 65535 && wide[3] == 7 && wide[7] == 9);
	CHECK(near({wide[4] / 257, wide[5] / 257, wide[6] / 257}, {200, 100, 50}, 2));
	// Sixteen bits are kept: not every channel lands on a multiple of 257.
	CHECK(wide[4] % 257 != 0 || wide[5] % 257 != 0 || wide[6] % 257 != 0);

	// Gray Gamma 2.2 is darker than sRGB in the shadows, and as light at white.
	auto grey = bgrx_row({{0, 0, 0}, {10, 10, 10}, {128, 128, 128}, {255, 255, 255}});
	CHECK(map_icc_to_srgb(row_view(grey), corpus(greyscale), row_buffer(grey)));
	CHECK(colour_at(grey, 0) == rgb({0, 0, 0}));
	CHECK(colour_at(grey, 1) == rgb({3, 3, 3}));
	CHECK(colour_at(grey, 2) == rgb({129, 129, 129}));
	CHECK(colour_at(grey, 3) == rgb({255, 255, 255}));
}

TEST("icc carrying an sRGB edit into a profile's colours keeps what it left, and shows what it made")
{
	const auto& p3 = corpus(display_p3);

	// Colours of modest saturation, inside sRGB once mapped, so the rendition loses nothing of them.
	std::vector<rgb> colours;
	for (auto i = 0; i < 16; ++i) colours.push_back({100 + i * 5, 90 + i * 4, 120 - i * 3});

	const auto original = bgrx_row(colours);
	auto before = original;
	CHECK(map_icc_to_srgb(row_view(original), p3, row_buffer(before)));

	// The edit darkens the first eight pixels of the rendition and leaves the rest.
	auto after = before;
	for (std::size_t i = 0; i < 8 * 4; ++i)
	{
		if (i % 4 != 3) after[i] = static_cast<std::uint8_t>(after[i] * 3 / 4);
	}

	auto carried = original;
	CHECK(carry_srgb_edit(row_view(original), p3, row_view(before), row_view(after), row_buffer(carried)));
	CHECK(std::equal(carried.begin() + 32, carried.end(), original.begin() + 32));
	CHECK(colour_at(carried, 0)[0] < colour_at(original, 0)[0]);

	auto shown = carried;
	CHECK(map_icc_to_srgb(row_view(carried), p3, row_buffer(shown)));
	auto as_edited = true;
	for (std::size_t x = 0; x < 8; ++x) as_edited = as_edited && near(colour_at(shown, x), colour_at(after, x), 2);
	CHECK(as_edited);

	// Sixteen-bit pixels take an edit of an 8-bit rendition alike.
	std::vector<std::uint16_t> wide;
	for (const auto& [r, g, b] : colours)
		wide.insert(wide.end(), {std::uint16_t(r * 257 + 3), std::uint16_t(g * 257), std::uint16_t(b * 257), 0xFFFF});
	const image_view wide_view{reinterpret_cast<const std::uint8_t*>(wide.data()), wide.size() * 2, 16, 1,
	                           pixel_format::rgbx16};
	auto wide_carried = wide;
	CHECK(carry_srgb_edit(wide_view, p3, row_view(before), row_view(after),
	                      {reinterpret_cast<std::uint8_t*>(wide_carried.data()), wide.size() * 2}));
	CHECK(std::equal(wide_carried.begin() + 32, wide_carried.end(), wide.begin() + 32));
	CHECK(wide_carried[0] < wide[0]);
}
