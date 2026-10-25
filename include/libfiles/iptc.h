// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Reading an IPTC-IIM block for what it says: the fields an organizer files, and the listing
// an inspector shows. The walk follows IIM 4.1's extended lengths and stops at a header it cannot
// trust; text is UTF-8 where it is valid and Windows-1252 where it is not, as Photoshop and the wire
// services wrote it for years.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	// The application record's datasets an organizer files. A single one holds the text of the last
	// dataset that had any, with its padding of NULs removed; the repeatable ones keep every dataset,
	// in order.
	struct iptc_fields
	{
		std::vector<std::string> keywords; // 2:25
		std::vector<std::string> bylines; // 2:80
		std::string object_name; // 2:5
		std::string caption; // 2:120, its CR line ends read as LF
		std::string city; // 2:90
		std::string state; // 2:95
		std::string country; // 2:101
		std::string credit; // 2:110
		std::string source; // 2:115
		std::string copyright; // 2:116

		// CCYYMMDD and HHMMSS+HHMM as the last dataset of each stored them, whitespace trimmed.
		std::string date_created; // 2:55
		std::string time_created; // 2:60
		std::string digital_creation_date; // 2:62
		std::string digital_creation_time; // 2:63
	};

	iptc_fields read_iptc(std::span<const std::uint8_t> block);

	struct iptc_listing_entry
	{
		std::uint8_t record = 0;
		std::uint8_t dataset = 0;
		// The dataset's title in the IIM table - "Keywords", "By-line" - or "?" for one it lacks.
		std::string title;
		std::string value;
	};

	// Every dataset in order but the binary ones - a preview, the character set's escape - with its
	// title and its value as text: a number for the numeric formats, else the text. One with no text
	// left once its NULs are removed is not listed.
	std::vector<iptc_listing_entry> list_iptc(std::span<const std::uint8_t> block);

	// IPTC text: as it is where it is valid UTF-8, and read as Windows-1252 where it is not.
	std::string iptc_text(std::span<const std::uint8_t> bytes);
}
