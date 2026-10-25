// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test.h"
#include "libfiles/iptc.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using bytes = std::vector<std::uint8_t>;

	void dataset(bytes& out, const std::uint8_t record, const std::uint8_t number, const std::string_view text)
	{
		out.insert(out.end(), {0x1C, record, number, static_cast<std::uint8_t>(text.size() >> 8),
		                       static_cast<std::uint8_t>(text.size())});
		out.insert(out.end(), text.begin(), text.end());
	}

	const libfiles::iptc_listing_entry* listed(const std::vector<libfiles::iptc_listing_entry>& l,
	                                           const std::uint8_t record, const std::uint8_t number)
	{
		for (const auto& e : l)
			if (e.record == record && e.dataset == number) return &e;
		return nullptr;
	}
}

TEST("iptc reads the fields an organizer files")
{
	bytes block;
	dataset(block, 2, 25, "tower");
	dataset(block, 2, 25, "iron");
	dataset(block, 2, 80, "Ann");
	dataset(block, 2, 5, "Eiffel");
	dataset(block, 2, 120, std::string_view("Line one\rLine two\0\0", 19));
	dataset(block, 2, 90, "Paris");
	// A later dataset with nothing but padding leaves the field as it was.
	dataset(block, 2, 90, std::string_view("\0\0", 2));
	dataset(block, 2, 101, "Fran\xE7" "ais");
	dataset(block, 2, 116, "\xC2\xA9 Ann");
	dataset(block, 2, 55, " 20240506 ");
	dataset(block, 2, 60, "143015+0200");
	// Record 3 is not the application record.
	dataset(block, 3, 5, "Not a title");

	const auto f = libfiles::read_iptc(block);
	CHECK(f.keywords == std::vector<std::string>({"tower", "iron"}));
	CHECK(f.bylines == std::vector<std::string>({"Ann"}));
	CHECK_STR(f.object_name, "Eiffel");
	CHECK_STR(f.caption, "Line one\nLine two");
	CHECK_STR(f.city, "Paris");
	CHECK_STR(f.country, "Fran\xC3\xA7" "ais");
	CHECK_STR(f.copyright, "\xC2\xA9 Ann");
	CHECK_STR(f.date_created, "20240506");
	CHECK_STR(f.time_created, "143015+0200");
	CHECK(f.digital_creation_date.empty() && f.state.empty() && f.credit.empty());

	CHECK(libfiles::read_iptc({}).keywords.empty());
}

TEST("iptc lists every dataset but the binary ones, by its title")
{
	bytes block;
	dataset(block, 2, 0, std::string_view("\0\x04", 2));
	dataset(block, 2, 25, "tower");
	dataset(block, 2, 80, "Ann ");
	dataset(block, 2, 202, std::string_view("\xFF\xD8\xFF\xD9", 4));
	dataset(block, 2, 250, "mystery");
	dataset(block, 2, 120, std::string_view("\0\0", 2));

	const auto l = libfiles::list_iptc(block);
	CHECK_EQ(l.size(), 4u);
	const auto* const version = listed(l, 2, 0);
	CHECK(version && version->title == "Record Version" && version->value == "4");
	const auto* const keywords = listed(l, 2, 25);
	CHECK(keywords && keywords->title == "Keywords" && keywords->value == "tower");
	const auto* const byline = listed(l, 2, 80);
	CHECK(byline && byline->title == "By-line" && byline->value == "Ann ");
	const auto* const unknown = listed(l, 2, 250);
	CHECK(unknown && unknown->title == "?" && unknown->value == "mystery");
	// The preview is binary, and a caption of nothing but padding has no text to show.
	CHECK(!listed(l, 2, 202));
	CHECK(!listed(l, 2, 120));
}

TEST("iptc walk follows an extended length and stops at a header it cannot trust")
{
	// IIM 4.1: with the size field's top bit set, its low bits count the octets that hold the size.
	bytes block = {0x1C, 2, 120, 0x80, 0x02, 0x00, 0x05, 'H', 'e', 'l', 'l', 'o'};
	dataset(block, 2, 5, "After");
	CHECK_STR(libfiles::read_iptc(block).caption, "Hello");
	CHECK_STR(libfiles::read_iptc(block).object_name, "After");

	// What precedes the application record is passed over; what follows a byte that is not a
	// dataset's marker is not read.
	bytes prefixed = {'8', 'B', 'I', 'M'};
	dataset(prefixed, 2, 5, "Title");
	prefixed.push_back(0x00);
	dataset(prefixed, 2, 90, "Lost");
	const auto p = libfiles::read_iptc(prefixed);
	CHECK_STR(p.object_name, "Title");
	CHECK(p.city.empty());

	// A dataset longer than its block is not read, and nothing after it.
	bytes overlong;
	dataset(overlong, 2, 5, "Kept");
	overlong.insert(overlong.end(), {0x1C, 2, 90, 0x10, 0x00, 'x'});
	const auto o = libfiles::read_iptc(overlong);
	CHECK_STR(o.object_name, "Kept");
	CHECK(o.city.empty());

	// Every cut of a block reads without fault.
	for (std::size_t cut = 0; cut <= block.size(); ++cut)
		(void)libfiles::list_iptc({block.data(), cut});
}

TEST("iptc text is UTF-8 where it is valid and Windows-1252 where it is not")
{
	const bytes utf8 = {'c', 'a', 'f', 0xC3, 0xA9};
	CHECK_STR(libfiles::iptc_text(utf8), "caf\xC3\xA9");
	const bytes latin1 = {'c', 'a', 'f', 0xE9};
	CHECK_STR(libfiles::iptc_text(latin1), "caf\xC3\xA9");
	// Windows' punctuation, where Latin-1 has controls; a byte Windows-1252 leaves undefined stays one.
	const bytes windows = {'O', 0x92, 'B', ' ', 0x93, 'x', 0x94, ' ', 0x96, ' ', 0x80, ' ', 0x81};
	CHECK_STR(libfiles::iptc_text(windows), "O\xE2\x80\x99" "B \xE2\x80\x9Cx\xE2\x80\x9D \xE2\x80\x93 \xE2\x82\xAC \xC2\x81");
	// An overlong form is not UTF-8.
	const bytes overlong = {0xC0, 0xAF};
	CHECK_STR(libfiles::iptc_text(overlong), "\xC3\x80\xC2\xAF");
	CHECK(libfiles::iptc_text({}).empty());

	bytes block = {0x1C, 2, 80, 0x00, 0x0A};
	block.insert(block.end(), {'P', 'h', 'o', 't', 'o', 'g', 0x92, 's', ' ', 'X'});
	CHECK(libfiles::read_iptc(block).bylines == std::vector<std::string>({"Photog\xE2\x80\x99s X"}));
}
