// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The rule a GPS coordinate is read by in the text XMP writes it as - "DDD,MM,SSk" or "DDD,MM.mmk"
// - for the writer, which keeps it in EXIF as rationals, and for a reader of properties, which takes it in
// signed decimal degrees.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace libfiles::detail
{
	// Degrees, minutes and seconds as rationals - the seconds 0/1 when the minutes carry a fraction - and the
	// reference letter: N, S, E or W.
	struct gps_parts
	{
		std::vector<std::pair<std::uint32_t, std::uint32_t>> rationals;
		char reference = 0;
	};

	// Nothing for any other text, so a writer never guesses at what it was given.
	std::optional<gps_parts> parse_xmp_gps(std::string_view text);

	// The coordinate in signed decimal degrees: south and west are negative.
	double gps_degrees(const gps_parts& parts) noexcept;
}
