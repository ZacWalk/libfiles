// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What a file says about the picture or recording it holds, as one set of properties whatever the
// format and wherever the file keeps them: a photo's EXIF, IPTC and XMP, a recording's tags and XMP. Each
// block is read into the same properties by one table, and the blocks are reconciled by one rule.

#pragma once

#include "libfiles/exif.h"
#include "libfiles/iptc.h"
#include "libfiles/reader.h"
#include "libfiles/types.h"
#include "libfiles/xmp.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libfiles
{
	// Text in the language a tag names.
	struct media_text
	{
		std::string text;
		// ISO 639-2, empty or "und" when the tag names none.
		std::string language;

		friend bool operator==(const media_text&, const media_text&) = default;
	};

	// What an organizer files a picture or a recording by. Text is UTF-8, trimmed; empty text, zero and an
	// invalid date are a property the file does not state.
	struct properties
	{
		std::string title;
		// Who made it: a recording's artist, a photo's creators, more than one joined by "; ".
		std::string artist;
		std::string album;
		std::string album_artist;
		std::string composer;
		std::string performer;
		std::string genre;
		std::string publisher;
		std::string copyright;
		std::string description;
		std::string synopsis;
		// The TV show an episode belongs to.
		std::string show;
		// The software that wrote the file.
		std::string encoder;
		std::string camera_make;
		std::string camera_model;
		std::vector<media_text> comments;
		std::vector<std::string> keywords;
		// 1 to 5 stars, 0 for none and -1 for rejected, as xmp:Rating has them; absent when the file keeps none.
		std::optional<int> rating;
		int year = 0;
		int track = 0;
		int track_count = 0;
		int disc = 0;
		int disc_count = 0;
		int season = 0;
		int episode = 0;
		// When the picture was taken or the recording made, or failing that when the file was.
		date created;
		// When the recording was first released, for a reissue or a rip.
		date original_release;
		std::optional<gps_position> location;
		// Metres above sea level, negative below it.
		std::optional<double> altitude;

		// A photo's lens and exposure: the f-number, the exposure in seconds and the focal length in
		// millimetres, 0 when unknown; and its orientation, EXIF's 1 to 8.
		std::string lens;
		double f_number = 0.0;
		double exposure_time = 0.0;
		double focal_length = 0.0;
		int focal_length_35mm = 0;
		int iso_speed = 0;
		std::optional<std::uint16_t> orientation;
		// When a photo was digitized - a scan's date, or the camera's again - and last changed.
		date digitized;
		date modified;
		// Where a photo was taken as IPTC names places, and whom to credit.
		std::string city;
		std::string state;
		std::string country;
		std::string credit;
		std::string source;

		friend bool operator==(const properties&, const properties&) = default;
	};

	// Each block's properties, read by libfiles' one table of where each block keeps each property: the
	// tables in docs/design.md, which the writers follow too. A packet's property in another form than its
	// schema gives - a simple value where the schema has an array, a struct where it has text - reads as none,
	// and the rest of the packet still reads. Never throws for what a file holds.
	properties properties_from_exif(const exif_metadata& exif);
	properties properties_from_iptc(const iptc_fields& iptc);
	properties properties_from_xmp(const xmp::meta& packet);

	// Gives into every property it lacks that from has, keeping those it has: how the blocks are reconciled,
	// the block read first winning.
	void fill_missing(properties& into, const properties& from);

	// A file's properties, from every block it keeps them in: its XMP packet first, as the Metadata Working
	// Group advises where blocks agree and as libfiles' writers keep them agreeing, then a still's EXIF and
	// IPTC, or a recording's container tags. A block that cannot be read gives nothing and costs the others
	// nothing. Empty for a file libfiles does not read. Never throws for a malformed file.
	properties read_properties(reader& r, const scan_limits& limits = {});
}
