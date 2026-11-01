// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Listing an ICC colour profile for what it says: a summary a person reads - the profile's
// name, its colour space and primaries, its tone response, white point and rendering intent - then its
// header whole, and every tag in its directory with its type and size, a tag it cannot read named as
// such with the reason.

#pragma once

#include "libfiles/types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	// What a profile says it is, as a person asks: the summary list_icc opens with, field by field, in the
	// listing's own words. Text is UTF-8; what the profile does not state is empty.
	struct icc_summary
	{
		// The profile's name, its 'desc' tag: "sRGB IEC61966-2.1", "Display P3".
		std::string name;
		std::string device_model;
		std::string device_manufacturer;
		std::string copyright;
		// The data colour space, by its signature and what it means.
		std::string colour_space;
		// The gamut the colorants match - "approximately sRGB", "approximately Display P3" - or "custom";
		// empty for a profile without colorants.
		std::string primaries;
		std::string tone_response;
		std::string white_point;
		std::string rendering_intent;
	};

	// Nothing for a block that is not a profile, as list_icc lists nothing for one. Never throws for a
	// malformed profile.
	std::optional<icc_summary> summarize_icc(std::span<const std::uint8_t> profile);
	// A profile as an inspector lists it: the containers "Summary" (id "icc.summary"), "Header"
	// ("icc.header"), "Tags (n)" ("icc.tags") and, when any tag could not be read, "Unread tags (n)"
	// ("icc.unread"), each followed by its rows at depth 1. A tag's row is keyed by its signature, has
	// the id "icc.tag." and the signature, and holds the tag's first 4,096 bytes as its detail. Nothing
	// for a block that is not a profile - one shorter than its 128-byte header, without the 'acsp'
	// signature, declaring a size it does not have, or more than 1,000 tags. Never throws for a
	// malformed profile.
	std::vector<structure_row> list_icc(std::span<const std::uint8_t> profile);
}
