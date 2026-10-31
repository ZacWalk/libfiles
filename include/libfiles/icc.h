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
#include <span>
#include <vector>

namespace libfiles
{
	// A profile as an inspector lists it: the containers "Summary" (id "icc.summary"), "Header"
	// ("icc.header"), "Tags (n)" ("icc.tags") and, when any tag could not be read, "Unread tags (n)"
	// ("icc.unread"), each followed by its rows at depth 1. A tag's row is keyed by its signature, has
	// the id "icc.tag." and the signature, and holds the tag's first 4,096 bytes as its detail. Nothing
	// for a block that is not a profile - one shorter than its 128-byte header, without the 'acsp'
	// signature, declaring a size it does not have, or more than 1,000 tags. Never throws for a
	// malformed profile.
	std::vector<structure_row> list_icc(std::span<const std::uint8_t> profile);
}
