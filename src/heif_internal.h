// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: What the HEIF scanner reads of a file's structure that its writer needs too: the items, where they
// lie and what refers to them, the boxes that hold them, and the metadata items the scan takes from the primary
// image. scan_heif.cpp is the one parser that reads them, for the scan and for the writer alike.

#pragma once

#include "libfiles/reader.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libfiles::detail::heif
{
	struct item_entry
	{
		std::uint32_t id = 0;
		std::uint32_t type = 0;
		bool hidden = false;
		std::string content_type;
		std::string content_encoding;
	};

	struct extent
	{
		// The item an extent of an item built from others takes its bytes from, where iloc gives one.
		std::uint64_t index = 0;
		std::uint64_t offset = 0;
		std::uint64_t length = 0;
	};

	struct location
	{
		std::uint32_t item = 0;
		std::uint8_t method = 0;
		// Zero for this file; any other names a file the data reference box lists.
		std::uint16_t data_reference = 0;
		std::uint64_t base = 0;
		std::vector<extent> extents;
	};

	// An item location box's version, and the width in bytes of each of its fields.
	struct location_format
	{
		std::uint8_t version = 0;
		unsigned offset_size = 0;
		unsigned length_size = 0;
		unsigned base_size = 0;
		unsigned index_size = 0;
	};

	struct reference
	{
		std::uint32_t type = 0;
		std::uint32_t from = 0;
		std::vector<std::uint32_t> to;
	};

	// A box where it lies in the file, from its header to its end.
	struct box_place
	{
		std::uint32_t type = 0;
		std::uint64_t offset = 0;
		std::uint64_t size = 0;
		// Eight bytes, or sixteen with a 64-bit size.
		std::uint32_t header = 8;
		// The size field, which is zero for a box that runs to the end of what holds it.
		std::uint64_t declared = 0;

		std::uint64_t end() const { return offset + size; }
	};

	// A metadata item the scan reads from the primary image.
	struct metadata_item
	{
		std::uint32_t id = 0;
		// Its data's length, as the scan reads it.
		std::uint64_t size = 0;
		// Its data, when the scan keeps a block that large.
		std::optional<std::vector<std::uint8_t>> data;
	};

	// What a writer needs of a file the scan reads whole.
	struct layout
	{
		std::uint64_t file_size = 0;
		// The top-level boxes the scan steps through, in order. The scan stops short of a box that begins in the
		// file's last 32 bytes, as libheif does.
		std::vector<box_place> boxes;
		// Whether a top-level box runs past the end of the file.
		bool cut = false;
		// Whether the file holds a movie box - an image sequence - whose chunk offsets a rewrite moves with the media.
		bool movie = false;

		// The meta box the scan reads - the last - with its payload: its full box header, then its children.
		box_place meta;
		std::vector<std::uint8_t> meta_payload;
		std::vector<box_place> children;
		// Which of the children the scan reads: the first item locations, item information and references.
		std::optional<std::size_t> iloc_child;
		std::optional<std::size_t> iinf_child;
		std::optional<std::size_t> iref_child;

		heif::location_format locations_format;
		std::vector<location> locations;
		std::uint8_t iinf_version = 0;
		// Where the item entries the item information declares end; nothing after them is read.
		std::uint64_t iinf_entries_end = 0;
		std::uint8_t iref_version = 0;
		std::vector<reference> references;
		std::map<std::uint32_t, item_entry> items;
		// Where the first idat box's data starts, which construction method 1 counts from.
		std::optional<std::uint64_t> idat_data;

		std::uint32_t primary = 0;
		// The largest id an item, a reference or an entity group gives, so a new item can take the next.
		std::uint32_t largest_id = 0;

		// The first Exif and the last XMP item attached to the primary image, as the scan takes them.
		std::optional<metadata_item> exif;
		std::optional<metadata_item> xmp;

		const location* location_of(const std::uint32_t id) const
		{
			for (const auto& l : locations)
				if (l.item == id) return &l;
			return nullptr;
		}
	};

	// The file as the scan reads it. Throws format_error for any file the scan refuses.
	layout read_layout(reader& r);

	// An AVIF libheif wrote with an ISO 21496-1 tone map item made into one that readers of gain maps find as libavif
	// writes it: the gain map's item hidden, and an 'altr' entity group of the tone map item and the primary image,
	// the tone map item first. Every item's data moves with the meta box. Throws format_error when the file is not
	// one libheif writes.
	std::vector<std::uint8_t> finish_gain_map(std::span<const std::uint8_t> file, std::uint32_t tone_map,
	                                          std::uint32_t gain_map);
}
