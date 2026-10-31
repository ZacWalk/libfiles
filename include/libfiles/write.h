// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Updating the metadata a file stores. An update is planned from a structural read of the
// file, as source byte ranges replaced in order. A plan that changes no length can be patched where
// the file lies; any other is written in one pass from the source to a destination the caller has
// staged, so the source is never modified partway.

#pragma once

#include "libfiles/reader.h"
#include "libfiles/scan.h"
#include "libfiles/stream.h"
#include "libfiles/types.h"
#include "libfiles/xmp.h"

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace libfiles
{
	// Thrown when a stream refuses a write or a flush.
	class write_error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// The bytes [offset, offset + length) of the source, replaced by data. A length of zero inserts.
	struct replacement
	{
		replacement() = default;

		replacement(const std::uint64_t at, const std::uint64_t replaced, std::vector<std::uint8_t> with,
		            const std::uint8_t order = 0) : offset(at), length(replaced), data(std::move(with)), step(order)
		{
		}

		std::uint64_t offset = 0;
		std::uint64_t length = 0;
		std::vector<std::uint8_t> data;
		// When the file is patched where it lies, patches are written a step at a time, lowest first,
		// each step flushed before the next begins. A one-pass rewrite ignores it.
		std::uint8_t step = 0;
	};

	// How a file changes: source ranges replaced, sorted and never overlapping. Everything between
	// them is kept as it is.
	struct write_plan
	{
		std::uint64_t source_size = 0;
		std::vector<replacement> replacements;
		// Why the file cannot be updated. Empty when it can.
		std::string error;

		bool ok() const noexcept { return error.empty(); }
		// True when no replacement changes length, so the file can be patched where it lies.
		bool in_place() const noexcept;
		// True when the file can be updated where it lies without moving anything it holds: every
		// replacement keeps its length, except what is added at the very end of the file.
		bool patchable() const noexcept;
		std::uint64_t result_size() const noexcept;
	};

	// What a file holds outside its XMP packet, written wherever its format keeps it: EXIF, IPTC and
	// Photoshop resources in a photo; ID3 frames, RIFF INFO, ASF attributes and MP4 boxes in audio and
	// video. A field that is set replaces what the file holds, and an empty text or list removes it; a
	// field that is not set is left as it is.
	struct legacy_edits
	{
		std::optional<std::string> title;
		std::optional<std::string> description;
		std::optional<std::string> copyright;
		std::optional<std::vector<std::string>> creators;
		std::optional<std::vector<std::string>> keywords;
		// Stars from 0 to 5, and the percent scale Windows reads beside them. Without a percent, a format that
		// keeps one takes the stars' own, percent_from_stars: always in ASF and MP4, which every rating writes,
		// and in EXIF where the block already holds a RatingPercent, which a reader would otherwise take over
		// the new stars.
		std::optional<int> rating;
		std::optional<int> rating_percent;
		// As XMP writes dates: "YYYY-MM-DDThh:mm:ss".
		std::optional<std::string> date_original;
		std::optional<std::string> date_digitized;
		// As XMP writes GPS coordinates: "DDD,MM,SSk" or "DDD,MM.mmk".
		std::optional<std::string> gps_latitude;
		std::optional<std::string> gps_longitude;
		// The EXIF orientation, 1 to 8.
		std::optional<int> orientation;
		std::optional<std::string> city;
		std::optional<std::string> state;
		std::optional<std::string> country;
		std::optional<std::string> credit;
		std::optional<std::string> source;
		// What audio and video tags hold - ID3 frames, RIFF INFO - beside the packet.
		std::optional<std::string> artist;
		std::optional<std::string> album;
		std::optional<std::string> album_artist;
		std::optional<std::string> genre;
		std::optional<std::string> comment;
		// "3", or "3/12" with the total.
		std::optional<std::string> track;
		std::optional<std::string> disc;
		std::optional<std::string> copyright_url;
		// A year alone, for a format that keeps a date; date_digitized takes its place when both are set.
		std::optional<int> year;
	};

	// What to store.
	struct metadata_update
	{
		metadata_update() = default;

		// Only a packet, every other field left as it is.
		metadata_update(const xmp::meta* packet) : xmp(packet)
		{
		}

		// The packet, serialized here in the form and place the container needs. Null leaves the
		// file's XMP as it is.
		const xmp::meta* xmp = nullptr;
		legacy_edits legacy;
	};

	// Sixteen bytes are enough; fewer match less. An ID3v2 tag reads as MPEG audio whatever follows it:
	// plan_update and read_packet go by identify(reader&), which looks past it.
	file_format detect_file_format(std::span<const std::uint8_t> head) noexcept;

	bool can_update(file_format format) noexcept;

	// The XMP packet a file holds, found where the update would write it and where scan() and scan_media()
	// find it. readable is false when the file's structure cannot be read, so its packet is unknown rather
	// than absent. The file is identified as identify(reader&) identifies it.
	//
	// Some formats libfiles reads but does not update have their packets read too: a HEIF's from the XMP
	// item - of type 'mime' and content type application/rdf+xml, or of the older type 'XMP' - attached to
	// its primary image, as scan() finds it; a JPEG XL container's from its last 'xml ' box; an AIFF's from
	// its last APPL chunk signed "XMP "; and a FLAC's from the ID3v2 tag some taggers put before it, by the
	// rule an MP3's is read by. When a JPEG XL's box is Brotli-compressed ('brob') the packet is not read
	// and readable is false: there is a packet, but read_packet cannot say what it holds. A bare JPEG XL
	// codestream has no packet.
	struct packet_reading
	{
		bool readable = false;
		std::string packet;
	};

	packet_reading read_packet(reader& source);

	// Never throws for a malformed file; the plan's error says why it cannot be updated.
	write_plan plan_update(reader& source, const metadata_update& update);

	// Applies a patchable plan to the file it was planned from. What the plan adds at the end is written
	// and flushed first, as bytes nothing refers to yet; then the patches, a step at a time, each step
	// flushed before the next. The planner orders the steps so that an interruption at any point leaves
	// a file that reads whole: what makes the appended bytes part of the file comes before what makes
	// them the ones read.
	void write_in_place(writable_stream& file, const write_plan& plan);

	// Writes the updated file to destination in one pass over the source, from offset 0, cuts the
	// destination to the result's size and flushes it. The destination must not be the source.
	void write_updated(reader& source, const write_plan& plan, writable_stream& destination);
}
