// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What a file says about the picture or recording it holds, as one set of properties whatever the
// format and wherever the file keeps them: a photo's EXIF, IPTC and XMP, a recording's tags and XMP. Each
// block is read into the same properties by one table, which records what the block states, and the blocks
// are reconciled by one rule - the table and the rule Diffractor files a library by.

#pragma once

#include "libfiles/exif.h"
#include "libfiles/iptc.h"
#include "libfiles/reader.h"
#include "libfiles/types.h"
#include "libfiles/xmp.h"

#include <cstdint>
#include <optional>
#include <span>
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

	// The tag a photo's date was read from.
	enum class date_source : unsigned char
	{
		// EXIF's DateTimeOriginal, DateTimeDigitized and DateTime - when the file last changed - each with the
		// zone and the fraction of a second EXIF keeps beside it.
		exif_original,
		exif_digitized,
		exif_modified,
		// EXIF's GPSDateStamp and GPSTimeStamp: the moment of the fix, in UTC.
		exif_gps,
		// IPTC's Date Created and Digital Creation Date, each with its time.
		iptc_created,
		iptc_digitized,
		// XMP's photoshop:DateCreated, xmp:CreateDate, xmp:ModifyDate, exif:DateTimeDigitized and
		// exif:DateTimeOriginal.
		xmp_photoshop_created,
		xmp_create,
		xmp_modify,
		xmp_exif_digitized,
		xmp_exif_original,
	};

	// A date, and the tag it was read from.
	struct dated
	{
		date_source source = date_source::exif_original;
		date when;

		friend bool operator==(const dated&, const dated&) = default;
	};

	// What a picture declares itself a panorama in, as Google's photo sphere schema has it declare one.
	enum class panorama_projection : unsigned char
	{
		none,
		// A panorama, in a projection it does not name.
		unspecified,
		equirectangular,
		cylindrical,
	};

	// A property a block can state, as properties records it.
	enum class property : unsigned char
	{
		title,
		artist,
		album,
		album_artist,
		composer,
		performer,
		genre,
		publisher,
		copyright,
		description,
		synopsis,
		show,
		encoder,
		camera_make,
		camera_model,
		comments,
		keywords,
		rating,
		year,
		// The track, the disc and the episode each with what it is of.
		track,
		disc,
		season,
		episode,
		media_kind,
		original_release,
		location,
		altitude,
		speed,
		lens,
		f_number,
		exposure_time,
		focal_length,
		focal_length_35mm,
		iso_speed,
		orientation,
		city,
		state,
		country,
		credit,
		source,
		creators,
		copyright_url,
		label,
		raw_file_name,
		// The last: properties.cpp's last_property names it.
		panorama,
	};

	// What an organizer files a picture or a recording by. Text is UTF-8, as the file holds it; empty text,
	// zero and an invalid date are a property the file does not give.
	struct properties
	{
		std::string title;
		// Who made it: a recording's artist, or a photo's as EXIF's Artist and IPTC's by-lines name it; more
		// than one joined by "; ".
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
		// The year a block states - a recording's, or an xmp:CreateDate that is a year alone - else created's.
		int year = 0;
		int track = 0;
		int track_count = 0;
		int disc = 0;
		int disc_count = 0;
		int season = 0;
		int episode = 0;
		int episode_count = 0;
		// What an iTunes-tagged recording says it is, as MP4's stik atom numbers it: 9 a film, 10 a TV episode,
		// 6 a music video, 1 music. Absent when the file does not say.
		std::optional<int> media_kind;
		// When the picture was taken or the recording made, or failing that when the file was: of a photo's
		// dates, the first of EXIF's DateTimeOriginal, XMP's exif:DateTimeOriginal, IPTC's Date Created and
		// photoshop:DateCreated, else its digitized date.
		date created;
		// When the recording was first released, for a reissue or a rip.
		date original_release;
		std::optional<gps_position> location;
		// Metres above sea level, negative below it.
		std::optional<double> altitude;
		// Kilometres an hour, as the receiver measured it at the fix; 0 when unknown.
		double speed = 0.0;

		// A photo's lens and exposure: the f-number, the exposure in seconds and the focal length in
		// millimetres, 0 when unknown; and its orientation, EXIF's 1 to 8.
		std::string lens;
		double f_number = 0.0;
		double exposure_time = 0.0;
		double focal_length = 0.0;
		int focal_length_35mm = 0;
		int iso_speed = 0;
		std::optional<std::uint16_t> orientation;
		// When a photo was digitized - the first of EXIF's DateTimeDigitized, XMP's exif:DateTimeDigitized,
		// IPTC's Digital Creation Date and xmp:CreateDate - and last changed: EXIF's DateTime, else
		// xmp:ModifyDate.
		date digitized;
		date modified;
		// Where a photo was taken as IPTC names places, and whom to credit.
		std::string city;
		std::string state;
		std::string country;
		std::string credit;
		std::string source;
		// Whom XMP's dc:creator names, and where the rights are stated: xmpRights:WebStatement.
		std::vector<std::string> creators;
		std::string copyright_url;
		// The label an editor gives a photo: xmp:Label.
		std::string label;
		// The raw file a developed photo was made from: crs:RawFileName.
		std::string raw_file_name;
		panorama_projection panorama = panorama_projection::none;
		// Every date the blocks give, in the order they were read, each with its tag.
		std::vector<dated> dates;

		// The properties the blocks read state, one bit for each: what a block states replaces what the blocks
		// before it gave, even empty, as an XMP property present but empty clears one. Those it joins are
		// added to theirs instead: IPTC's keywords and by-lines join the keywords and artist before them. A
		// container's tags, as a media scan reads them, mark none.
		std::uint64_t stated = 0;
		std::uint64_t joined = 0;

		bool states(const property p) const noexcept { return (stated >> static_cast<unsigned>(p) & 1u) != 0; }
		bool joins(const property p) const noexcept { return (joined >> static_cast<unsigned>(p) & 1u) != 0; }
		void mark_stated(const property p) noexcept { stated |= std::uint64_t{1} << static_cast<unsigned>(p); }

		void mark_joined(const property p) noexcept
		{
			mark_stated(p);
			joined |= std::uint64_t{1} << static_cast<unsigned>(p);
		}

		friend bool operator==(const properties&, const properties&) = default;
	};

	// Each block's properties, read by libfiles' one table of where each block keeps each property - the
	// tables in docs/design.md, which the writers follow too - marking what the block states. EXIF and IPTC
	// state a property by holding text, a number or a date for it; XMP by holding the property at all, even
	// empty. A packet's property in another form than its schema gives - a simple value where the schema has
	// an array or alt-text - reads as none, and the rest of the packet still reads. Never throws for what a
	// file holds.
	properties properties_from_exif(const exif_metadata& exif);
	properties properties_from_iptc(const iptc_fields& iptc);
	properties properties_from_xmp(const xmp::meta& packet);
	// A packet as a scan gives it or a sidecar holds it, after a JPEG segment's signature if one leads it.
	// One that does not parse states nothing.
	properties properties_from_xmp(std::span<const std::uint8_t> packet);

	// The one rule blocks are reconciled by: from read over into, as a block over what the blocks before it
	// gave. What from states replaces into's, and what it joins is added to into's; its dates join into's.
	// created, digitized, modified and year then settle on what all the dates give.
	void overlay(properties& into, const properties& from);

	// A file's properties, from every block it keeps them in, by the one rule: a still's EXIF, then its IPTC,
	// then its XMP packet; or a recording's container tags, then its packet. A block that cannot be read gives
	// nothing and costs the others nothing. Empty for a file libfiles does not read. Never throws for a
	// malformed file.
	properties read_properties(reader& r, const scan_limits& limits = {});
}
