// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The rules a date is read and written by, in every form a file keeps one, so a reader and a writer
// of the same field agree: the forms tags write dates in, the strict form an edit gives one in, the counts of
// seconds containers keep, and the text EXIF, IPTC and an AVI's IDIT chunk hold.

#pragma once

#include "libfiles/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libfiles::detail
{
	// The forms tags write dates in: "2011", "2011-03", "2011-03-13", ISO 8601 with "T" or a space and an
	// optional zone, "2011:03:13 15:13:49" as cameras write it, and AVI's "Sun Mar 13 15:13:49 2011". What a
	// reader of a file takes a date from.
	date parse_media_date(std::string_view text) noexcept;

	// The form an edit gives a date in, as XMP writes dates: "YYYY-MM-DD[Thh:mm[:ss[.s]][Z|+hh:mm|-hh:mm]]".
	// Nothing for any other text, so a writer never guesses at what it was given.
	std::optional<date> parse_xmp_date(std::string_view text);

	// Days from 1970-01-01 to a day of the proleptic Gregorian calendar.
	std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept;

	// Seconds from the start of epoch_year, January 1, to the date, read as UTC unless it states a zone.
	// Negative when the date comes first.
	std::int64_t seconds_since(int epoch_year, const date& d) noexcept;

	// A count of seconds from 1970 in UTC, as a date. Out of range years give an invalid date.
	date utc_date_from_seconds(std::int64_t seconds_since_1970) noexcept;

	// "YYYY:MM:DD HH:MM:SS", as EXIF keeps a date and time.
	std::string exif_date_text(const date& d);
}
