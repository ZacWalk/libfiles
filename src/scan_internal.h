// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What the format scanners share and do not export: byte-order loads, the structure-row
// helpers, each format's scanner, and the zlib inflate and CRC-32 that PNG needs.

#pragma once

#include "libfiles/scan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	// Thrown by a scanner for a file it cannot read as its format.
	class format_error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// The largest image edge a scan reports as read. The decoders that consume a scan bound their
	// allocations by it, so a header field past it is treated as a fault rather than a size.
	inline constexpr std::int64_t max_image_dimension = 65535;

	// A structure row keeps at most this much of a payload: as much as a hex listing would show.
	inline constexpr std::size_t max_structure_bytes = 4096;

	inline std::uint16_t load_be16(const std::uint8_t* const p)
	{
		return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
	}

	inline std::uint16_t load_le16(const std::uint8_t* const p)
	{
		return static_cast<std::uint16_t>(p[1] << 8 | p[0]);
	}

	inline std::uint32_t load_be32(const std::uint8_t* const p)
	{
		return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
			static_cast<std::uint32_t>(p[2]) << 8 | p[3];
	}

	inline std::uint32_t load_le32(const std::uint8_t* const p)
	{
		return static_cast<std::uint32_t>(p[3]) << 24 | static_cast<std::uint32_t>(p[2]) << 16 |
			static_cast<std::uint32_t>(p[1]) << 8 | p[0];
	}

	inline bool starts_with(const std::span<const std::uint8_t> bytes, const std::string_view prefix)
	{
		return bytes.size() >= prefix.size() &&
			std::equal(prefix.begin(), prefix.end(), bytes.begin(),
			           [](const char a, const std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; });
	}

	// The structure report's rows. A leaf row sits at depth 1 under the section before it.
	structure_row& add_row(std::vector<structure_row>& rows, std::string key, std::string value);
	void add_structure_row(std::vector<structure_row>& rows, std::string_view key, std::string value,
	                       std::string_view shape = {}, std::string detail = {});
	// As above, keeping up to max_structure_bytes of the payload for a hex listing.
	void add_structure_bytes(std::vector<structure_row>& rows, std::string_view key, std::string value,
	                         std::string_view shape, std::span<const std::uint8_t> payload);
	void add_structure_section(std::vector<structure_row>& rows, std::string_view key, std::string_view id,
	                           bool open_by_default = false);
	// Drops the sections a file had nothing to put in, and counts the rows of the rest.
	void finish_structure_sections(std::vector<structure_row>& rows);

	scan_result scan_jpeg(reader& r, scan_intent intent, const scan_limits& limits);
	scan_result scan_png(reader& r);
	scan_result scan_webp(reader& r, scan_intent intent);
	scan_result scan_gif(reader& r);
	scan_result scan_psd(reader& r);
	scan_result scan_bmp(reader& r);
	scan_result scan_tiff(reader& r);
	scan_result scan_heif(reader& r, scan_intent intent);
	scan_result scan_jxl(reader& r, scan_intent intent, const scan_limits& limits);

	// The XMP packet of a HEIF or JPEG XL file, found as scan() finds it but read alone. A file whose
	// structure cannot be read throws format_error or read_error; false means the packet is there but
	// cannot be read - a JPEG XL packet in a Brotli-compressed box, or one larger than a scan keeps.
	bool heif_packet(reader& r, std::vector<std::uint8_t>& packet);
	bool jxl_packet(reader& r, std::vector<std::uint8_t>& packet);

	// What one JPEG XL scan lets the caller's Brotli decompressor produce, across all of its
	// compressed boxes: twice the 16 MiB a scan keeps of one metadata block.
	inline constexpr std::size_t max_jxl_decompressed = 32u * 1024u * 1024u;

	// The most codestream parts a JPEG XL file's read holds at once - parts not yet read, and parts
	// held until their turn - which stays small however many 'jxlp' boxes the file has. For tests.
	std::size_t jxl_parts_held(reader& r);

	// Removes what HEIF and JPEG XL put before an Exif block's TIFF header, as Diffractor did: the
	// four-byte offset to that header, then a JPEG "Exif\0\0" signature if one follows it.
	void strip_exif_offset(std::vector<std::uint8_t>& exif);

	// The canvas a WebP's header declares, read as scan_webp reads it. False when it cannot be read.
	bool webp_canvas(reader& r, std::uint32_t& width, std::uint32_t& height, bool& has_alpha);

	// Inflates a zlib stream (RFC 1950): the whole stream, its Adler-32 checked, into at most
	// max_output bytes. False for anything malformed, truncated or larger, with out undefined.
	bool zlib_inflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t max_output);

	// Inflates only the first `length` bytes of a zlib stream, as libpng reads a colour profile: what
	// follows them, the Adler-32 included, is never examined. False if the stream fails or ends first.
	bool zlib_inflate_prefix(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t length);

	// The CRC-32 of ISO 3309, as PNG and zip use it. Pass 0 to start.
	std::uint32_t crc32(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept;
}
