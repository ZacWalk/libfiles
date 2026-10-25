// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Listing an EXIF block for an inspector: every entry of every directory with its name, its
// English title and description, and its value as text, the maker note decoded where its maker's format
// is known, and the thumbnail. The listing reads as libexif's did - the entries it took from a damaged
// block, its names, titles and words - so that an application that listed EXIF with libexif shows the
// same.

#pragma once

#include "libfiles/exif.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	struct exif_listing_entry
	{
		std::uint16_t tag = 0;
		std::uint16_t type = 0;
		std::uint32_t count = 0;
		// The tag's name in its directory - "ExposureTime", "GPSLatitude" - and none for a tag libexif's
		// table does not record there.
		std::optional<std::string> name;
		// libexif's English title for the tag in its directory - "Exposure Time", "Latitude" - and its
		// description, empty where the table gives none; both none where the name is.
		std::optional<std::string> title;
		std::optional<std::string> description;
		// The value as text: "1/60 sec.", "f/2.8", "Top-left", "72"; for a blob, "4 bytes undefined data".
		std::string value;
		// The value as stored.
		std::vector<std::uint8_t> bytes;
	};

	struct exif_listing_directory
	{
		exif_directory directory = exif_directory::primary;
		std::vector<exif_listing_entry> entries;
	};

	// One value of a maker note: Canon's, Fuji's, Nikon's, Olympus', Pentax's and the formats like
	// them. An array of settings is listed a value at a time.
	struct exif_maker_note_value
	{
		std::uint32_t id = 0;
		std::optional<std::string> name;
		// The decoder's English title and description for the value, where it gives them.
		std::optional<std::string> title;
		std::optional<std::string> description;
		std::string value;
	};

	struct exif_listing
	{
		bool little_endian = true;
		// Each directory that holds an entry, in the order IFD0, IFD1, Exif, GPS, Interoperability.
		std::vector<exif_listing_directory> directories;
		std::vector<exif_maker_note_value> maker_note;
		// The thumbnail the JPEGInterchangeFormat entries point to.
		std::vector<std::uint8_t> thumbnail;
	};

	struct exif_listing_options
	{
		// libexif's EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS, which its defaults set: an entry whose tag libexif's
		// table does not name in its directory is left out, and a maker note decoder gives a value it does
		// not know no name or title - as Canon's does for an unknown part of its settings.
		bool ignore_unknown_tags = false;
	};

	// Lists an EXIF block, from its "Exif\0\0" signature or its byte-order mark. Only the first 65,534
	// bytes are read for directories, as libexif read them; a maker note may lie anywhere.
	exif_listing list_exif(std::span<const std::uint8_t> block, const exif_listing_options& options = {});

	// The name of a TIFF type, as the listing gives it: "Short", "Rational", "ASCII"; none for a type
	// past DOUBLE.
	const char* exif_type_name(std::uint16_t type) noexcept;
}
