// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The rules the values in a photo's EXIF, IPTC and XMP are read by - dates and their zones and
// fractions, numbers, rationals and coordinates - as Diffractor read them before its reading moved here, so
// a library of photos files them where it always has. Each reads text as the C library's sscanf does, by
// glibc's rules, and a date as Windows' calendar takes it.

#pragma once

#include "libfiles/types.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace libfiles::detail
{
	// A date as a camera or an editor writes one: "2006:01:14 15:51:31", "2006-01-14 15:51:31",
	// "2006-01-14T15:51:31.25", "20060114T155131", "20060114155131", "2006-01-14", "2006-jan-14", and the
	// "THU OCT 26 16:46:04 2006" some cameras write. The seconds are read as a real number and rounded down
	// unless within a tenth of the next. A zone the text goes on to state is not read: read_zone reads it.
	// Nothing for a day the calendar does not hold, or for one before 1800 or on the first of January 1970
	// or 1904 - a clock never set, counting from the C library's epoch or the Mac's. A month or a day of 0
	// is kept as none, and checked as January or the first.
	std::optional<date> parse_photo_date(std::string_view text);

	// The zone the end of a date's text states, onto the date: "+02:00", as EXIF's OffsetTime tags hold it
	// whole and an XMP date ends; or "Z", ending a date of more than ten characters. Trailing NULs and spaces
	// are ignored, as an EXIF value is padded with them. An offset past 14 hours, or of 60 minutes or more,
	// states none.
	void read_zone(std::string_view text, date& d) noexcept;

	// EXIF's SubSecTime: digits after an implied point, as nanoseconds - "5" is half a second. Read to the
	// first character that is not a digit, and to seven digits; 0 for none.
	int sub_second_nanoseconds(std::string_view digits) noexcept;

	// The leading integer of the text, as std::from_chars reads one: a minus sign and digits at its start,
	// and 0 for none or for one an int cannot hold.
	int leading_int(std::string_view text) noexcept;

	// Whether the text is a number: digits, with points and commas among them, and nothing else.
	bool is_number(std::string_view text) noexcept;

	// "3", or "3/12" with what it is of, as a track, a disc or an episode is numbered: a number alone
	// read by leading_int, else two integers about a slash. 0, 0 for anything else.
	std::pair<int, int> parse_number_pair(std::string_view text) noexcept;

	// A rational as XMP writes one, "33/10", or an integer, as numerator and denominator: nothing for a
	// denominator of 0, or for text that goes on after the rational.
	std::optional<std::pair<std::uint32_t, std::uint32_t>> parse_photo_rational(std::string_view text) noexcept;

	// Rounded half away from zero; INT_MIN for a value past what an int holds, and 0 for one that is not a
	// normal number.
	int round_to_int(double value) noexcept;

	// A coordinate as XMP writes one, "DDD,MM,SSk" or "DDD,MM.mmk", in signed decimal degrees: read about
	// white space, with a reference letter of either case, and nothing for a minute or second of 60 or
	// more, or for a latitude past 90 degrees or a longitude of 180 or more.
	std::optional<double> parse_photo_coordinate(std::string_view text, bool latitude) noexcept;
}
