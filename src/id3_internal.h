// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The one reader of an ID3v2 tag's structure - its header, its extended header and the headers
// of its frames, how each frame's data lies behind its flags, the faults that make a tag one an update
// cannot take, and the rule for which frame holds a file's XMP packet - shared by the media scan, which
// reads the frames, read_packet, which reads the packet, and the MP3 writer, which keeps the frames.

#pragma once

#include "libfiles/reader.h"
#include "libfiles/stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	inline constexpr std::uint64_t id3_header_size = 10;

	// The most a tag reader holds of one frame - room for a 16 MiB picture and the fields before it - and
	// the most frames one tag is read for: far past any tagger's, and short of what a crafted tag could make
	// a reader hold.
	inline constexpr std::size_t id3_max_frame = (16u << 20) + (64u << 10);
	inline constexpr std::size_t id3_max_frames = 65536;

	// Seven bits a byte, most significant first.
	std::uint32_t id3_syncsafe(const std::uint8_t* p) noexcept;

	struct id3_header
	{
		// The major version: 2, 3 or 4 for ID3v2.2, 2.3 and 2.4.
		int version = 0;
		std::uint8_t revision = 0;
		std::uint8_t flags = 0;
		// What follows the header up to any footer: the extended header, the frames and the padding.
		std::uint64_t size = 0;
		// False when a byte of the size has its high bit set, which a syncsafe size never has: damage.
		bool size_syncsafe = false;

		// A version libfiles reads and writes: 2.2, 2.3 or 2.4, its revision not 0xFF.
		bool known_version() const noexcept { return version >= 2 && version <= 4 && revision != 0xFF; }
		// ID3v2.2 and 2.3 unsynchronise the whole tag after its header; ID3v2.4 each frame.
		bool unsynchronised() const noexcept { return (flags & 0x80) != 0; }
		bool extended() const noexcept { return version >= 3 && (flags & 0x40) != 0; }
		// ID3v2.2's compression, for which no scheme was ever defined: such a tag cannot be read.
		bool compressed() const noexcept { return version == 2 && (flags & 0x40) != 0; }
		bool has_footer() const noexcept { return version == 4 && (flags & 0x10) != 0; }
		// Where the frames and padding end, from the start of the tag.
		std::uint64_t end() const noexcept { return id3_header_size + size; }
		// What the whole tag takes, its footer included.
		std::uint64_t total() const noexcept { return end() + (has_footer() ? id3_header_size : 0); }
	};

	// The header in the first ten bytes; nothing when they do not begin "ID3".
	std::optional<id3_header> parse_id3_header(std::span<const std::uint8_t> bytes) noexcept;

	// What an ID3v2 tag at the start of a file's first bytes takes, footer included, as identify() reads it:
	// a version from 2 to 4 and a syncsafe size; 0 when there is none.
	std::uint64_t leading_id3_size(std::span<const std::uint8_t> head) noexcept;

	// Where the frames begin in [begin, end) of what r reads, after the extended header when the tag has
	// one; nothing when the extended header's size is damaged.
	std::optional<std::uint64_t> id3_frames_begin(reader& r, const id3_header& header, std::uint64_t begin, std::uint64_t end);

	struct id3_frame
	{
		// Four characters, or three in ID3v2.2.
		char id[4] = {};
		std::size_t id_length = 4;
		// Where the frame's header lies in what the walk reads, and the size of its body as stored there.
		std::uint64_t offset = 0;
		std::size_t header_size = 10;
		std::uint64_t size = 0;
		// ID3v2.3 and 2.4's status and format flags, the format flags in the low byte. ID3v2.2 has none.
		std::uint16_t flags = 0;

		std::string_view name() const noexcept { return {id, id_length}; }
		std::uint64_t body() const noexcept { return offset + header_size; }
	};

	// How an ID3v2.4 tag's frame sizes are written: syncsafe as the standard has them - though a size
	// with a high bit set can only be a plain number - or all as plain numbers, as iTunes wrote some.
	enum class id3_sizes : std::uint8_t
	{
		standard,
		plain,
	};

	// The frames of a tag, front to back, in [begin, end) of what r reads.
	class id3_frame_walker
	{
	public:
		id3_frame_walker(reader& r, int version, std::uint64_t begin, std::uint64_t end,
		                 id3_sizes sizes = id3_sizes::standard) noexcept;

		// The next frame; nothing at the padding or the end, at bytes that are not a frame's header, or at
		// a frame that claims more than the tag has left. Throws read_error when r cannot supply a header.
		std::optional<id3_frame> next();

		// The walk ended at a frame that runs past the end of the tag.
		bool overran() const noexcept { return _overran; }
		// The walk ended at bytes that are neither a frame, the padding nor the end.
		bool stray() const noexcept { return _stray; }

	private:
		reader& _r;
		int _version;
		std::uint64_t _position;
		std::uint64_t _end;
		id3_sizes _sizes;
		bool _overran = false;
		bool _stray = false;
	};

	// Whether reading every ID3v2.4 frame size of the tag at offset as a plain number walks its frames to
	// their end.
	bool id3_walks_with_plain_sizes(reader& r, std::uint64_t offset, const id3_header& tag);

	// The one rule for when a tag's frames are walked again with plain sizes: iTunes wrote ID3v2.4 tags with
	// ID3v2.3's plain sizes, which read as syncsafe send the walk astray. Every reader of a tag - the scan,
	// read_packet and the writer - asks this after a standard walk, so all of them see the same frames.
	bool id3_retry_with_plain_sizes(reader& r, std::uint64_t offset, const id3_header& tag, bool standard_walk_damaged);

	// Why the tag at offset cannot be updated, or read for its packet, before its frames are walked: an
	// unknown version, a damaged size, ID3v2.2's compression, or a tag that runs past the end of the file.
	// Null when there is no such fault.
	const char* id3_fault(const id3_header& tag, std::uint64_t offset, std::uint64_t file_size);

	// The faults a walk finds.
	inline constexpr const char* id3_damaged_extended_header = "the ID3 tag's extended header is damaged";
	inline constexpr const char* id3_frame_overran = "an ID3 frame runs past the end of its tag";

	// The bytes after an ID3v2.2 or 2.3 tag's header with the tag's unsynchronisation undone - each zero
	// that follows an 0xFF dropped - read on demand, so that a large picture is never held whole.
	class id3_resynchronised final : public stream
	{
	public:
		id3_resynchronised(reader& file, std::uint64_t begin, std::uint64_t end);

		std::uint64_t size() override { return _size; }
		std::size_t read(std::uint64_t offset, void* data, std::size_t size) override;

	private:
		static constexpr std::uint64_t block = 64 * 1024;

		// Where a block of the tag starts in the file, how many bytes are kept before it, and whether the
		// byte before it is an 0xFF, whose zero the block would begin with.
		struct block_mark
		{
			std::uint64_t at = 0;
			std::uint64_t kept = 0;
			bool after_ff = false;
		};

		std::size_t chunk(const std::uint64_t at) const { return static_cast<std::size_t>(std::min(block, _end - at)); }

		reader& _file;
		std::uint64_t _end;
		std::uint64_t _size = 0;
		std::vector<block_mark> _marks;
	};

	// Drops the zero unsynchronisation put after each 0xFF of an ID3v2.4 frame.
	void id3_resynchronise(std::vector<std::uint8_t>& bytes);

	// How a frame's data is stored behind its header.
	struct id3_frame_layout
	{
		// The bytes before the data: a group, an encryption method and a data length.
		std::uint64_t prefix = 0;
		bool unsynchronised = false;
		bool compressed = false;
		bool encrypted = false;

		bool plain() const noexcept { return !unsynchronised && !compressed && !encrypted; }
	};

	id3_frame_layout id3_layout_of(const id3_header& tag, const id3_frame& f) noexcept;

	// A frame's data with its unsynchronisation and compression undone and the bytes before it dropped;
	// nothing when it is encrypted, damaged, or more than max_size before or after inflating.
	std::optional<std::vector<std::uint8_t>> id3_frame_data(reader& source, const id3_frame& f, const id3_frame_layout& l,
	                                                        std::size_t max_size);

	// What follows the owner in a PRIV frame's data when the owner, NUL-terminated, is the one named.
	std::optional<std::span<const std::uint8_t>> id3_owned_payload(std::span<const std::uint8_t> data, std::string_view owner);

	// A PRIV frame's packet when its owner is "XMP", trailing NULs removed: the toolkit's place for one.
	std::optional<std::vector<std::uint8_t>> id3_xmp_payload(std::span<const std::uint8_t> data);

	// PRIV, or PRV, the MP3 writer's name for a packet frame in ID3v2.2, which defined none.
	bool is_id3_private_frame(std::string_view id) noexcept;

	struct id3_packet_reading
	{
		// False when the tag is damaged so that its packet is unknown rather than absent.
		bool readable = true;
		std::vector<std::uint8_t> packet;
	};

	// The XMP packet of the ID3v2 tag at offset, by the rule the scan, read_packet and the writer share: the
	// first PRIV frame owned by "XMP" whose data decodes, of a tag walked as id3_retry_with_plain_sizes
	// says. A tag with an id3_fault, a damaged extended header or a frame that runs past its end has none
	// readable. No tag there reads as a readable tag with no packet.
	id3_packet_reading id3_packet(reader& r, std::uint64_t offset);
}
