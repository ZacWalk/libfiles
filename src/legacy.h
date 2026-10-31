// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Editing the metadata photos hold outside their XMP packet: EXIF directories, IPTC-IIM
// datasets and Photoshop image resources, and the mapping from a semantic edit onto them. Shared by
// the JPEG, TIFF and PSD writers.

#pragma once

#include "date_internal.h"
#include "text_internal.h"
#include "write_internal.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	using bytes = std::vector<std::uint8_t>;

	std::array<std::uint8_t, 16> md5(std::span<const std::uint8_t> data) noexcept;

	class tiff_bytes;

	// A TIFF stream - a whole TIFF file, or the EXIF block of a JPEG or PSD - edited without moving
	// anything it already holds. A changed value is written where it lies when it fits; what does not
	// fit, and any directory that gains or loses an entry, is appended at the end, and the pointer to
	// it is repointed. Every offset already in the stream, a MakerNote's included, stays valid.
	class tiff_editor
	{
	public:
		enum class directory : std::uint8_t { ifd0, exif, gps };

		static constexpr std::uint16_t type_byte = 1;
		static constexpr std::uint16_t type_ascii = 2;
		static constexpr std::uint16_t type_short = 3;
		static constexpr std::uint16_t type_long = 4;
		static constexpr std::uint16_t type_rational = 5;
		static constexpr std::uint16_t type_undefined = 7;

		// Reads the header and the IFD0, Exif and GPS directories. False if the stream is not
		// classic TIFF or IFD0 cannot be read.
		bool open(reader& r);

		// A stream with an empty IFD0, for a file that has no EXIF yet.
		static bytes empty_stream();

		bool little_endian() const { return _little_endian; }
		bool has(directory d, std::uint16_t tag) const;

		// Where a value lies in the stream as read: inline in its entry or out of line.
		std::optional<byte_range> value_range(directory d, std::uint16_t tag) const;

		// The value as the stream stores it, in its byte order.
		void set(directory d, std::uint16_t tag, std::uint16_t type, std::uint32_t count, bytes value);
		void set_ascii(directory d, std::uint16_t tag, std::string_view text);
		void set_short(directory d, std::uint16_t tag, std::uint16_t value);
		void set_rationals(directory d, std::uint16_t tag, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& values);
		void remove(directory d, std::uint16_t tag);

		bool changed() const { return !_edits.empty(); }

		// The edits as replacements of the stream. What is appended is one insertion at stream_size.
		std::vector<replacement> changes(std::uint64_t stream_size) const;

	private:
		struct entry
		{
			std::uint16_t tag = 0;
			std::uint16_t type = 0;
			std::uint32_t count = 0;
			std::array<std::uint8_t, 4> value{};
			std::uint64_t at = 0; // where the 12-byte entry lies in the stream
		};

		struct ifd
		{
			bool present = false;
			std::uint64_t offset = 0;
			std::vector<entry> entries;
			std::uint32_t next = 0;
		};

		struct edit
		{
			bool remove = false;
			std::uint16_t type = 0;
			std::uint32_t count = 0;
			bytes value;
		};

		static bool read_ifd(const tiff_bytes& data, std::uint64_t offset, ifd& out);
		std::uint16_t u16(const std::uint8_t* p) const;
		std::uint32_t u32(const std::uint8_t* p) const;
		void put16(bytes& to, std::uint16_t v) const;
		void put32(bytes& to, std::uint32_t v) const;

		bool _little_endian = true;
		std::uint64_t _size = 0;
		std::array<ifd, 3> _ifds;
		std::map<std::pair<directory, std::uint16_t>, edit> _edits;
	};

	// The block with the changes made: what tiff_editor::changes returned for it, applied in memory.
	bytes apply_changes(std::span<const std::uint8_t> block, const std::vector<replacement>& changes);

	// One IPTC-IIM dataset.
	struct iptc_dataset
	{
		std::uint8_t record = 0;
		std::uint8_t number = 0;
		bytes data;
	};

	// The datasets of an IPTC block, in order. Unreadable trailing bytes end the list.
	std::vector<iptc_dataset> parse_iptc(std::span<const std::uint8_t> block);
	bytes serialize_iptc(const std::vector<iptc_dataset>& datasets);

	// One Photoshop image resource, its name kept as stored.
	struct photoshop_resource
	{
		std::uint16_t id = 0;
		bytes name; // the Pascal string with its padding
		bytes data;
		// Where the data starts in the block it was read from.
		std::size_t data_offset = 0;
	};

	// False when the resources are malformed; what was read before the fault is kept.
	bool parse_photoshop_resources(std::span<const std::uint8_t> block, std::vector<photoshop_resource>& out);
	bytes serialize_photoshop_resources(const std::vector<photoshop_resource>& resources);

	// The resource with an id that the scans read and an edit changes: the first that holds anything,
	// else the first.
	photoshop_resource* find_photoshop_resource(std::vector<photoshop_resource>& resources, std::uint16_t id);

	// Sets the IPTC resource and the MD5 digest Adobe readers check it against.
	void set_iptc_resources(std::vector<photoshop_resource>& resources, const bytes& iptc);

	// What a photo's legacy blocks become after an edit.
	struct photo_legacy
	{
		photo_legacy() = default;

		explicit photo_legacy(tiff_editor* editor) : exif(editor)
		{
		}

		// The EXIF edits; for a TIFF file, the file's own directories.
		tiff_editor* exif = nullptr;
		// The IPTC block, set when it changes. Empty means the file gains none.
		std::optional<bytes> iptc;
	};

	// Writes the edit's fields into the EXIF editor and, unless the file must not carry IPTC, into the
	// IPTC block, creating one when a field it holds is written. existing_iptc is the file's block.
	void apply_photo_legacy(const legacy_edits& edits, std::span<const std::uint8_t> existing_iptc, bool allow_iptc,
	                        photo_legacy& out);

	// True when the edit sets a field EXIF holds.
	bool touches_exif(const legacy_edits& edits);

	// True when the edit sets a field IPTC holds.
	bool touches_iptc(const legacy_edits& edits);
}
