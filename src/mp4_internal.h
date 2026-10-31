// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The one reader of ISO base media and QuickTime structure, shared by the MP4 media scan and
// the MP4 writer: box headers as FFmpeg reads them - the decode HEIF and JPEG XL share, with a 32-bit size
// of zero running to the end - the top-level walk, lists of boxes with QuickTime's terminator, what makes
// a file QuickTime, the XMP uuid box and where read_packet takes a packet from, and the entries of the
// Xtra box Windows keeps tags and a rating in.

#pragma once

#include "bmff_internal.h"
#include "libfiles/reader.h"
#include "libfiles/scan.h"
#include "scan_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libfiles::detail::mp4
{
	using bmff::fourcc;

	inline constexpr auto ftyp = fourcc("ftyp");
	inline constexpr auto moov = fourcc("moov");
	inline constexpr auto mdat = fourcc("mdat");
	inline constexpr auto free_ = fourcc("free");
	inline constexpr auto skip = fourcc("skip");
	inline constexpr auto wide = fourcc("wide");
	inline constexpr auto pnot = fourcc("pnot");
	inline constexpr auto uuid = fourcc("uuid");
	inline constexpr auto udta = fourcc("udta");
	inline constexpr auto mvhd = fourcc("mvhd");
	inline constexpr auto xmp_ = fourcc("XMP_");
	inline constexpr auto xtra = fourcc("Xtra");
	inline constexpr auto cprt = fourcc("cprt");
	inline constexpr auto qt__ = fourcc("qt  ");

	inline constexpr std::array<std::uint8_t, 16> xmp_uuid = {
		0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC
	};

	// The largest movie box an update reads into memory. read_packet reads no packet from a file whose
	// movie box is larger, since an update could not write one there.
	inline constexpr std::uint64_t max_moov = 256ull * 1024 * 1024;

	struct box
	{
		std::uint64_t start = 0;
		std::uint64_t size = 0;
		std::uint32_t type = 0;
		std::uint32_t header = 8;
		bool to_end = false; // a size field of zero: the box runs to the end of what holds it

		std::uint64_t end() const { return start + size; }
		std::uint64_t payload() const { return start + header; }
		std::uint64_t payload_size() const { return size - header; }
	};

	enum class header_status : std::uint8_t
	{
		box,
		// Fewer than eight bytes are left, so no box starts here.
		none,
		// A 64-bit size the bytes left cannot hold.
		cut,
		// A size smaller than the box's own header, or larger than the bytes left.
		damaged,
	};

	// The header of the box at pos, in a list that ends at end.
	inline header_status read_box(reader& r, const std::uint64_t pos, const std::uint64_t end, box& out)
	{
		if (pos > end) return header_status::none;

		const auto left = end - pos;
		bmff::box_header h;
		const auto status = bmff::read_header(r, pos, left, h);
		if (status == bmff::header_status::none) return header_status::none;

		out = {};
		out.start = pos;
		out.type = h.type;
		if (status == bmff::header_status::cut) return header_status::cut;

		out.size = h.size;
		out.header = h.header;

		if (!h.large() && h.size == 0)
		{
			out.size = left;
			out.to_end = true;
		}

		return out.size < out.header || out.size > left ? header_status::damaged : header_status::box;
	}

	enum class list_end : std::uint8_t
	{
		whole,
		// QuickTime's four zero bytes ended the list.
		terminator,
		// What follows the last box is not a whole box.
		damaged,
	};

	// Passes each box from start to end to visit, in order, where every byte belongs to a box but for the
	// four zero bytes QuickTime may end a list with.
	template <typename Visit>
	list_end for_each_box(reader& r, const std::uint64_t start, const std::uint64_t end, Visit&& visit)
	{
		auto pos = start;

		while (pos < end)
		{
			const auto left = end - pos;
			if (left < 8) return left == 4 && r.be32(pos) == 0 ? list_end::terminator : list_end::damaged;

			box b;
			if (read_box(r, pos, end, b) != header_status::box) return list_end::damaged;

			visit(b);
			pos = b.end();
		}

		return list_end::whole;
	}

	struct box_list
	{
		std::vector<box> boxes;
		// The first user data box, found however many boxes come before it: a movie's packet is in its first,
		// whichever list of the movie's boxes it is looked for in.
		std::optional<box> first_user_data;
		bool terminator = false;
		bool damaged = false;
	};

	// The boxes for_each_box finds, of which at most max_boxes are kept and passed to visit as each is
	// read. The rest are still read, so that damage after them is found, and so is the first user data box.
	template <typename Visit>
	box_list list_boxes(reader& r, const std::uint64_t start, const std::uint64_t end, const std::size_t max_boxes, Visit&& visit)
	{
		box_list list;

		const auto how = for_each_box(r, start, end, [&list, &visit, max_boxes](const box& b)
		{
			if (b.type == udta && !list.first_user_data) list.first_user_data = b;
			if (list.boxes.size() >= max_boxes) return;
			list.boxes.push_back(b);
			visit(b);
		});

		list.terminator = how == list_end::terminator;
		list.damaged = how == list_end::damaged;
		return list;
	}

	inline box_list list_boxes(reader& r, const std::uint64_t start, const std::uint64_t end,
	                           const std::size_t max_boxes = std::numeric_limits<std::size_t>::max())
	{
		return list_boxes(r, start, end, max_boxes, [](const box&) {});
	}

	// Passes each top-level box to visit, in file order. Fewer than eight bytes after the last are not a
	// box. Returns none when the walk reached the end of the file, else the fault that stopped it.
	template <typename Visit>
	header_status for_each_top_level_box(reader& r, Visit&& visit)
	{
		std::uint64_t pos = 0;

		for (;;)
		{
			box b;
			const auto status = read_box(r, pos, r.size(), b);
			if (status != header_status::box) return status;

			visit(b);
			pos = b.end();
		}
	}

	// The top-level boxes, as an update needs them: a box that runs past the end of the file is a fault.
	inline std::vector<box> top_level_boxes(reader& r)
	{
		std::vector<box> boxes;
		const auto status = for_each_top_level_box(r, [&boxes](const box& b) { boxes.push_back(b); });

		if (status == header_status::cut) throw format_error("an MP4 box header runs past the end of the file");
		if (status == header_status::damaged) throw format_error("an MP4 box runs past the end of the file");
		return boxes;
	}

	inline bool is_xmp_uuid(reader& r, const box& b)
	{
		if (b.type != uuid || b.size < b.header + 16u) return false;
		const auto id = r.view(b.payload(), 16);
		return std::equal(xmp_uuid.begin(), xmp_uuid.end(), id.begin());
	}

	// A file is QuickTime when its first file type box names "qt  " as its major brand, or when it has
	// none: the oldest QuickTime files begin with their movie or their media.
	inline bool is_quicktime(reader& r, const box* const first_ftyp)
	{
		if (!first_ftyp || first_ftyp->size < first_ftyp->header + 4u) return true;
		return r.be32(first_ftyp->payload()) == qt__;
	}

	inline bool is_quicktime(reader& r, const std::vector<box>& boxes)
	{
		const auto f = std::ranges::find(boxes, ftyp, &box::type);
		return is_quicktime(r, f == boxes.end() ? nullptr : &*f);
	}

	// The XMP_ box a QuickTime file keeps its packet in: the first in the movie's first user data box, however
	// many boxes come before it. Throws format_error when the movie's boxes, or the user data's, are damaged,
	// as an update would find them.
	inline std::optional<box> movie_xmp(reader& r, const box_list& movie)
	{
		if (movie.damaged) throw format_error("an MP4 box holds a damaged child");
		if (!movie.first_user_data) return {};

		const auto& user_data = *movie.first_user_data;
		std::optional<box> found;

		const auto how = for_each_box(r, user_data.payload(), user_data.end(), [&found](const box& b)
		{
			if (!found && b.type == xmp_) found = b;
		});

		if (how == list_end::damaged) throw format_error("an MP4 box holds a damaged child");
		return found;
	}

	// The packet read_packet takes: a QuickTime file's from its movie's user data, an ISO file's from its
	// last XMP uuid box, each from the other place when that is the only one it has.
	inline std::optional<byte_range> packet_place(const bool quicktime, const std::optional<box>& in_movie, const box* const in_uuid)
	{
		const auto from_movie = [&in_movie] { return byte_range{in_movie->payload(), in_movie->payload_size()}; };
		const auto from_uuid = [in_uuid] { return byte_range{in_uuid->payload() + 16, in_uuid->payload_size() - 16}; };

		if (quicktime)
		{
			if (in_movie) return from_movie();
			if (in_uuid) return from_uuid();
		}
		else
		{
			if (in_uuid) return from_uuid();
			if (in_movie) return from_movie();
		}

		return {};
	}

	// One entry of an Xtra box: its size, its name's length and name, its value count, then the values.
	struct xtra_entry
	{
		// The entry whole, from its size field.
		std::span<const std::uint8_t> bytes;
		// Empty, with no values, when the name's length does not fit the entry.
		std::string_view name;
		std::uint32_t value_count = 0;
		std::span<const std::uint8_t> values;
	};

	// Passes each entry of an Xtra box's payload to visit, to the first whose size does not fit.
	template <typename Visit>
	void for_each_xtra_entry(const std::span<const std::uint8_t> content, Visit&& visit)
	{
		std::size_t pos = 0;

		while (content.size() - pos >= 12)
		{
			const auto size = load_be32(content.data() + pos);
			const auto name_length = load_be32(content.data() + pos + 4);
			if (size < 12 || size > content.size() - pos) break;

			xtra_entry entry;
			entry.bytes = content.subspan(pos, size);

			if (name_length > 0 && name_length <= size - 12)
			{
				entry.name = {reinterpret_cast<const char*>(content.data() + pos + 8), name_length};
				entry.value_count = load_be32(content.data() + pos + 8 + name_length);
				entry.values = content.subspan(pos + 12 + name_length, size - 12 - name_length);
			}

			visit(entry);
			pos += size;
		}
	}

	// One value of an Xtra entry: its size, a Windows VARTYPE, then its data, numbers little-endian and
	// text UTF-16LE.
	struct xtra_value
	{
		std::uint16_t type = 0;
		std::span<const std::uint8_t> data;
	};

	// Passes each of an entry's values to visit, to the first that does not fit the entry.
	template <typename Visit>
	void for_each_xtra_value(const xtra_entry& entry, Visit&& visit)
	{
		std::size_t pos = 0;

		for (std::uint32_t i = 0; i < entry.value_count && entry.values.size() - pos >= 6; ++i)
		{
			const auto size = load_be32(entry.values.data() + pos);
			if (size < 6 || size > entry.values.size() - pos) break;

			visit(xtra_value{load_be16(entry.values.data() + pos + 4), entry.values.subspan(pos + 6, size - 6)});
			pos += size;
		}
	}
}
