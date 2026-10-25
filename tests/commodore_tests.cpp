// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for the Commodore 64 listings - D64, D71 and D81 directories, T64 tapes, CRT
// cartridges, P00 and PRG programs - and for drawing one in the C64's character set.

#include "test.h"

#include "builders.h"
#include "libfiles/commodore.h"

#include <array>
#include <string_view>

using namespace test_build;

namespace
{
	constexpr std::size_t d64_size = 174848; // 35 tracks
	constexpr std::size_t d71_size = 349696; // 70 tracks
	constexpr std::size_t d81_size = 819200; // 80 tracks

	// Where the drives keep the first directory sector: track 18 sector 1 on a 1541 and a 1571, track
	// 40 sector 3 on a 1581.
	constexpr std::size_t d64_directory = 0x16600;
	constexpr std::size_t d81_directory = 0x61B00;

	// A directory entry in the slot'th place of the sector at `sector`: its type, its name padded with
	// shifted spaces, and its size in blocks.
	void put_entry(bytes& image, const std::size_t sector, const std::size_t slot, const std::uint8_t type,
	               const std::string_view name, const std::uint16_t blocks)
	{
		const auto at = sector + slot * 32;
		image[at + 2] = type;
		for (std::size_t i = 0; i < 16; ++i) image[at + 5 + i] = i < name.size() ? static_cast<std::uint8_t>(name[i]) : 0xA0;
		image[at + 30] = static_cast<std::uint8_t>(blocks);
		image[at + 31] = static_cast<std::uint8_t>(blocks >> 8);
	}

	// Points a directory sector at the next; track 0 ends the chain.
	void link(bytes& image, const std::size_t sector, const std::uint8_t track, const std::uint8_t next_sector)
	{
		image[sector] = track;
		image[sector + 1] = next_sector;
	}

	std::vector<std::string> texts(const libfiles::c64_listing& listing)
	{
		std::vector<std::string> result;
		for (const auto& row : listing.rows) result.push_back(row.text);
		return result;
	}

	bool same(const std::vector<std::string>& actual, const std::vector<std::string>& expected)
	{
		if (actual == expected) return true;
		for (const auto& line : actual) std::printf("    row: '%s'\n", line.c_str());
		return false;
	}

	// A T64 whose description is `description`, with room for `room` entries and `used` of them used.
	struct t64_entry
	{
		std::uint8_t type = 1;
		std::uint8_t type_1541 = 0x82;
		std::uint16_t start = 0x0801;
		std::uint16_t end = 0x0801;
		std::string_view name;
	};

	bytes make_t64(const std::string_view description, const std::uint16_t room, const std::uint16_t used,
	               const std::vector<t64_entry>& entries)
	{
		builder t;
		t.raw(description).zeros(32 - description.size()).le16(0x0101).le16(room).le16(used).zeros(2);
		t.raw("TAPE").zeros(20);

		for (const auto& e : entries)
		{
			t.u8(e.type).u8(e.type_1541).le16(e.start).le16(e.end).le16(0).le32(0).le32(0);
			t.raw(e.name);
			for (auto i = e.name.size(); i < 16; ++i) t.u8(0x20);
		}

		return t.b;
	}
}

TEST("Should list a D64's directory as the C64 lists it")
{
	bytes image(d64_size, 0);
	link(image, d64_directory, 0, 0xFF);
	put_entry(image, d64_directory, 0, 0x82, "HELLO", 12);
	put_entry(image, d64_directory, 2, 0x81, "DATA", 3); // slot 1 is empty, and skipped

	const auto listing = libfiles::list_c64_media(image);

	CHECK(same(texts(listing), {"12  \"HELLO           \" PRG", "3   \"DATA            \" SEQ"}));
	CHECK_EQ(listing.columns, 26u);
	CHECK_EQ(listing.width(), 208u);
	CHECK_EQ(listing.height(), 16u);

	const std::vector<std::uint8_t> codes = {
		0x31, 0x32, 0x20, 0x20, 0x22, 0x08, 0x05, 0x0C, 0x0C, 0x0F, 0x20, 0x20, 0x20, 0x20, 0x20,
		0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x22, 0x20, 0x10, 0x12, 0x07
	};
	CHECK(listing.rows.size() == 2 && listing.rows[0].screen_codes == codes);
}

TEST("Should name each file type as the C64 names it")
{
	bytes image(d64_size, 0);
	put_entry(image, d64_directory, 0, 0x80, "A", 1);
	put_entry(image, d64_directory, 1, 0x81, "B", 1);
	put_entry(image, d64_directory, 2, 0x82, "C", 1);
	put_entry(image, d64_directory, 3, 0x83, "D", 1);
	put_entry(image, d64_directory, 4, 0x84, "E", 1);
	put_entry(image, d64_directory, 5, 0xC2, "F", 1); // a locked PRG, which has no name here

	const auto listing = libfiles::list_c64_media(image);

	CHECK(same(texts(listing), {
		"1   \"A               \" DEL", "1   \"B               \" SEQ", "1   \"C               \" PRG",
		"1   \"D               \" USR", "1   \"E               \" REL", "1   \"F               \" "
	}));
	CHECK_EQ(listing.columns, 26u);
}

TEST("Should follow a disk's directory chain, and stop where it loops or leaves the disk")
{
	// (18,1) leads to (18,4), which leads back: each sector is read once.
	bytes looped(d64_size, 0);
	link(looped, d64_directory, 18, 4);
	link(looped, 0x16900, 18, 1);
	put_entry(looped, d64_directory, 0, 0x82, "FIRST", 1);
	put_entry(looped, 0x16900, 0, 0x82, "SECOND", 2);

	CHECK(same(texts(libfiles::list_c64_media(looped)),
	           {"1   \"FIRST           \" PRG", "2   \"SECOND          \" PRG"}));

	// Track 40 sector 0 lies past a 35-track disk's end, but within a 40-track one.
	for (const auto size : {d64_size, std::size_t{196608}})
	{
		bytes image(size, 0);
		link(image, d64_directory, 40, 0);
		put_entry(image, d64_directory, 0, 0x82, "NEAR", 1);
		if (size > d64_size) put_entry(image, 0x2EF00, 0, 0x82, "FAR", 2);

		const auto rows = texts(libfiles::list_c64_media(image));
		CHECK_EQ(rows.size(), size > d64_size ? 2u : 1u);
	}
}

TEST("Should list D71 and D81 directories where their drives keep them")
{
	// A 1571's second side starts at track 36 and mirrors the first side's zones: track 53 sector 0,
	// where it keeps its second BAM, is 0x41000 into the image.
	bytes d71(d71_size, 0);
	link(d71, d64_directory, 53, 0);
	put_entry(d71, d64_directory, 0, 0x82, "SIDE ONE", 1);
	put_entry(d71, 0x41000, 0, 0x82, "SIDE TWO", 2);

	CHECK(same(texts(libfiles::list_c64_media(d71)),
	           {"1   \"SIDE ONE        \" PRG", "2   \"SIDE TWO        \" PRG"}));

	// A 1581 keeps its directory on track 40, and its block counts can run to four digits.
	bytes d81(d81_size, 0);
	put_entry(d81, d81_directory, 0, 0x82, "BIG FILE", 3000);

	const auto listing = libfiles::list_c64_media(d81);
	CHECK(same(texts(listing), {"3000 \"BIG FILE        \" PRG"}));
	CHECK_EQ(listing.columns, 27u);

	// The sizes with a byte of error information for each sector are disks too.
	for (const auto size : {std::size_t{175531}, std::size_t{196608}, std::size_t{197376}, std::size_t{351062}})
	{
		bytes image(size, 0);
		put_entry(image, d64_directory, 0, 0x82, "ERRORS", 5);
		CHECK(same(texts(libfiles::list_c64_media(image)), {"5   \"ERRORS          \" PRG"}));
	}
}

TEST("Should list at most 512 rows, however long the directory")
{
	// Seventy sectors of a 1581's tracks 40 and 41 chained in order: 560 entries.
	bytes d81(d81_size, 0);

	for (std::size_t s = 0; s < 70; ++s)
	{
		const auto sector = d81_directory + s * 256;
		const auto next = s + 4; // the sector after this one, counted from track 40 sector 0
		if (s + 1 < 70) link(d81, sector, static_cast<std::uint8_t>(40 + next / 40), static_cast<std::uint8_t>(next % 40));
		for (std::size_t slot = 0; slot < 8; ++slot) put_entry(d81, sector, slot, 0x82, "ENTRY", 1);
	}

	CHECK_EQ(libfiles::list_c64_media(d81).rows.size(), 512u);
}

TEST("Should list a T64 tape's directory")
{
	const std::vector<t64_entry> entries = {
		{1, 0x82, 0x0801, 0x0801 + 1000, "GAME"},
		{0, 0x82, 0x0801, 0x0900, "FREE"}, // a free entry, skipped
		{1, 0x00, 0xC000, 0xC000 + 10, "TOOL"}, // no 1541 type: a PRG
		{1, 0x81, 0x1000, 0x0800, "NOTES"}, // ends before it starts: no data
	};

	const std::vector<std::string> all = {
		"4   \"GAME            \" PRG", "1   \"TOOL            \" PRG", "1   \"NOTES           \" SEQ"
	};

	// The descriptions writers use differ, but all say "tape".
	for (const auto description : {"C64S tape image file", "C64 tape image file", "C64S tape file"})
	{
		CHECK(same(texts(libfiles::list_c64_media(make_t64(description, 30, 4, entries))), all));
	}

	// The entries used, but no more than there is room for; a room of 0 sets no limit.
	CHECK(same(texts(libfiles::list_c64_media(make_t64("C64S tape image file", 2, 4, entries))), {all[0]}));
	CHECK(same(texts(libfiles::list_c64_media(make_t64("C64S tape image file", 0, 4, entries))), all));
	CHECK(texts(libfiles::list_c64_media(make_t64("C64S tape image file", 30, 0, entries))).empty());

	// Entries claimed past the end of the file are not read.
	CHECK(same(texts(libfiles::list_c64_media(make_t64("C64S tape image file", 30, 10, entries))), all));

	// "C64" without "tape" is no tape, so it is taken for a program.
	CHECK(same(texts(libfiles::list_c64_media(make_t64("C64 cassette image", 30, 4, entries))),
	           {"1   \"                \" PRG"}));
}

TEST("Should list a CRT cartridge once, by its name")
{
	builder crt;
	crt.raw("C64 CARTRIDGE   ").be32(0x40).be16(0x0100).be16(0).u8(0).u8(1).zeros(6);
	crt.raw("SUPER GAME").zeros(22);
	crt.raw("CHIP").be32(0x2010).be16(0).be16(0).be16(0x8000).be16(0x2000).zeros(0x2000);

	const auto listing = libfiles::list_c64_media(crt.b);
	CHECK(same(texts(listing), {"33  \"SUPER GAME                      \" CRT"}));
	CHECK_EQ(listing.columns, 42u);

	// Cut inside its header, it is no cartridge, and is taken for a program.
	crt.b.resize(40);
	CHECK(same(texts(libfiles::list_c64_media(crt.b)), {"1   \"                \" PRG"}));
}

TEST("Should list a P00 or a PRG as one program")
{
	builder p00;
	p00.raw(lit("C64File\0")).raw("HELLO").zeros(11).u8(0).u8(0).zeros(600);
	CHECK(same(texts(libfiles::list_c64_media(p00.b)), {"3   \"HELLO           \" PRG"}));

	// A P00 too short for its header lists nothing.
	p00.b.resize(20);
	CHECK(texts(libfiles::list_c64_media(p00.b)).empty());

	// A PRG has no name; its load address counts towards its blocks.
	CHECK(same(texts(libfiles::list_c64_media(bytes(1002, 0x11))), {"4   \"                \" PRG"}));
	CHECK(same(texts(libfiles::list_c64_media(bytes(65538, 0x11))), {"259 \"                \" PRG"}));
	CHECK(texts(libfiles::list_c64_media(bytes(65539, 0x11))).empty());
	CHECK(texts(libfiles::list_c64_media(bytes(1, 0x11))).empty());
	CHECK(texts(libfiles::list_c64_media({})).empty());

	// A disk with nothing in its directory lists nothing.
	CHECK(texts(libfiles::list_c64_media(bytes(d64_size, 0))).empty());
}

TEST("Should write PETSCII names as text and as screen codes")
{
	const bytes name = {0x41, 0xC1, 0x61, 0xFF, 0x5B, 0x0D, 0xA0, 0x40, 0x85, 0xB0, 0x31, 0x20, 0x5A, 0xDA, 0xDB, 0x1F};
	builder p00;
	p00.raw(lit("C64File\0")).raw(name).u8(0).u8(0);

	const auto listing = libfiles::list_c64_media(p00.b);
	CHECK_EQ(listing.rows.size(), 1u);
	if (listing.rows.size() != 1) return;

	const auto& row = listing.rows[0];
	CHECK_STR(row.text, "0   \"AA     @  1 ZZ  \" PRG");
	CHECK_EQ(row.text.size(), row.screen_codes.size());

	// Shifted letters are letters as text, but graphics characters - blank - when drawn.
	const std::vector<std::uint8_t> name_codes(row.screen_codes.begin() + 5, row.screen_codes.begin() + 21);
	CHECK(name_codes == std::vector<std::uint8_t>({0x01, 0x41, 0x41, 0x5E, 0x1B, 0x20, 0x20, 0x00, 0x20, 0x70, 0x31,
		0x20, 0x1A, 0x5A, 0x5B, 0x20}));
}

TEST("Should draw a listing in the C64's character set")
{
	bytes image(d64_size, 0);
	put_entry(image, d64_directory, 0, 0x82, "1", 1);
	put_entry(image, d64_directory, 1, 0xC2, "2", 1); // three cells shorter: no type

	const auto listing = libfiles::list_c64_media(image);
	CHECK_EQ(listing.columns, 26u);
	CHECK_EQ(listing.rows.size(), 2u);

	constexpr std::uint32_t foreground = 0x5F53FE;
	constexpr std::uint32_t background = 0x211BAE;
	constexpr std::array<std::uint8_t, 4> fg = {0xFE, 0x53, 0x5F, 0x00};
	constexpr std::array<std::uint8_t, 4> bg = {0xAE, 0x1B, 0x21, 0x00};

	// Twelve bytes of padding after each row, which must not be touched.
	const std::size_t width = listing.width();
	const std::size_t stride = width * 4 + 12;
	bytes memory(stride * listing.height(), 0xAB);
	libfiles::draw_c64_listing(listing, {memory.data(), stride}, foreground, background);

	const auto pixel = [&](const std::size_t x, const std::size_t y)
	{
		const auto* const p = memory.data() + y * stride + x * 4;
		return std::array<std::uint8_t, 4>{p[0], p[1], p[2], p[3]};
	};

	auto padding_kept = true;
	auto only_two_colours = true;
	auto fg_in_first_cell = 0;

	for (std::size_t y = 0; y < listing.height(); ++y)
	{
		for (std::size_t x = width * 4; x < stride; ++x) padding_kept &= memory[y * stride + x] == 0xAB;

		for (std::size_t x = 0; x < width; ++x)
		{
			const auto p = pixel(x, y);
			only_two_colours &= p == fg || p == bg;
			if (y < 8 && x < 8 && p == fg) ++fg_in_first_cell;
		}
	}

	CHECK(padding_kept);
	CHECK(only_two_colours);

	// The first cell is '1': 0x18, 0x18, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00, the left-most pixel the
	// most significant bit.
	CHECK_EQ(fg_in_first_cell, 19);
	CHECK(pixel(3, 0) == fg && pixel(4, 0) == fg && pixel(2, 0) == bg && pixel(5, 0) == bg);
	CHECK(pixel(1, 6) == fg && pixel(7, 6) == bg && pixel(0, 7) == bg);

	// The second row's last three cells lie past its end: background.
	auto past_end_blank = true;
	for (std::size_t y = 8; y < 16; ++y)
		for (std::size_t x = 23 * 8; x < width; ++x) past_end_blank &= pixel(x, y) == bg;
	CHECK(past_end_blank);

	// No memory, nothing drawn.
	libfiles::draw_c64_listing(listing, {}, foreground, background);
}
