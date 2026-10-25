// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Listing what a Commodore 64 disk, tape, cartridge or program image holds, as the C64 lists a
// disk's directory, and drawing that listing in the C64's own character set.

#pragma once

#include "libfiles/decode.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	// One row of a listing: a file's size in disk blocks, its name in quotes, and its type.
	struct c64_listing_row
	{
		// The row as plain text, a character for each screen code: PETSCII's letters, digits and
		// punctuation as ASCII, and its graphics and control characters as spaces.
		std::string text;
		// The row as the C64 draws it: a screen code for each character, as draw_c64_listing draws them.
		std::vector<std::uint8_t> screen_codes;
	};

	struct c64_listing
	{
		// One row for each file, in the order the image holds them, and at most 512, however long a
		// damaged directory claims to be: 4,096 pixels drawn.
		std::vector<c64_listing_row> rows;
		// The longest row's length in characters.
		std::uint32_t columns = 0;

		// The size draw_c64_listing draws the listing at: eight pixels square for each character.
		std::uint32_t width() const noexcept { return columns * 8; }
		std::uint32_t height() const noexcept { return static_cast<std::uint32_t>(rows.size()) * 8; }
	};

	// Lists a D64, D71 or D81 disk's directory, a T64 tape's, a CRT cartridge as one row by its name, or
	// a P00 or PRG program as one row. A disk is known by its size, a P00, T64 or CRT by its signature,
	// and anything else of at most 65,538 bytes - a C64's memory and a load address - is taken for a
	// PRG; anything larger lists nothing.
	c64_listing list_c64_media(std::span<const std::uint8_t> file);

	// Draws a listing as the C64 draws it, into width() by height() pixels of bgrx8, writing nothing past
	// them: each character an 8 by 8 glyph of the C64's upper-case character set in foreground, on
	// background, with the graphics characters drawn as spaces. Colours are 0xRRGGBB.
	void draw_c64_listing(const c64_listing& listing, pixel_buffer pixels, std::uint32_t foreground,
	                      std::uint32_t background) noexcept;
}
