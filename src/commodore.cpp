// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Listing a Commodore 64 image - a D64, D71 or D81 disk's directory, a T64 tape's, a CRT
// cartridge's name, a P00 or PRG program - and drawing the listing in the C64's 8x8 character set.
// Moved from Diffractor unchanged, its fields read a byte at a time rather than through packed structs.

#include "libfiles/commodore.h"

#include "scan_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <unordered_set>

namespace
{
	using libfiles::detail::load_le16;

	struct media_entry
	{
		std::uint8_t file_type = 0;
		std::vector<std::uint8_t> pet_name;
		std::uint32_t file_size = 0;
	};

	using media_entries = std::vector<media_entry>;

	// 8x8 bitmaps of the C64 upper-case and graphics character set, indexed by screen code. Each glyph
	// is 8 rows; within a row the most significant bit is the left-most pixel. Screen codes 0x00-0x3F
	// cover @, A-Z, punctuation and digits - all a directory listing needs; 0x40-0x7F are the CBM
	// graphics characters, left blank, so they draw as spaces.
	constexpr std::uint8_t c64_font_8x8[128][8] = {
		{0x3C, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x3C, 0x00}, // 0x00 @
		{0x18, 0x3C, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}, // 0x01 A
		{0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00}, // 0x02 B
		{0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}, // 0x03 C
		{0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00}, // 0x04 D
		{0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x7E, 0x00}, // 0x05 E
		{0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x60, 0x00}, // 0x06 F
		{0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}, // 0x07 G
		{0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00}, // 0x08 H
		{0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, // 0x09 I
		{0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00}, // 0x0A J
		{0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00}, // 0x0B K
		{0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}, // 0x0C L
		{0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00}, // 0x0D M
		{0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00}, // 0x0E N
		{0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, // 0x0F O
		{0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}, // 0x10 P
		{0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x0E, 0x00}, // 0x11 Q
		{0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00}, // 0x12 R
		{0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}, // 0x13 S
		{0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, // 0x14 T
		{0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}, // 0x15 U
		{0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}, // 0x16 V
		{0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, // 0x17 W
		{0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00}, // 0x18 X
		{0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}, // 0x19 Y
		{0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00}, // 0x1A Z
		{0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00}, // 0x1B [
		{0x0C, 0x12, 0x30, 0x7C, 0x30, 0x62, 0xFC, 0x00}, // 0x1C British pound
		{0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00}, // 0x1D ]
		{0x00, 0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18, 0x00}, // 0x1E up arrow
		{0x00, 0x10, 0x30, 0x7F, 0x7F, 0x30, 0x10, 0x00}, // 0x1F left arrow
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 0x20 space
		{0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00}, // 0x21 !
		{0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, // 0x22 "
		{0x66, 0x66, 0xFF, 0x66, 0xFF, 0x66, 0x66, 0x00}, // 0x23 #
		{0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00}, // 0x24 $
		{0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0x00}, // 0x25 %
		{0x3C, 0x66, 0x3C, 0x38, 0x67, 0x66, 0x3F, 0x00}, // 0x26 &
		{0x06, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, // 0x27 '
		{0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00}, // 0x28 (
		{0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00}, // 0x29 )
		{0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, // 0x2A *
		{0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}, // 0x2B +
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, // 0x2C ,
		{0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, // 0x2D -
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, // 0x2E .
		{0x00, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00}, // 0x2F /
		{0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00}, // 0x30 0
		{0x18, 0x18, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00}, // 0x31 1
		{0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00}, // 0x32 2
		{0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}, // 0x33 3
		{0x06, 0x0E, 0x1E, 0x66, 0x7F, 0x06, 0x06, 0x00}, // 0x34 4
		{0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}, // 0x35 5
		{0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}, // 0x36 6
		{0x7E, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00}, // 0x37 7
		{0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}, // 0x38 8
		{0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00}, // 0x39 9
		{0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00}, // 0x3A :
		{0x00, 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30}, // 0x3B ;
		{0x0E, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0E, 0x00}, // 0x3C <
		{0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, // 0x3D =
		{0x70, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x70, 0x00}, // 0x3E >
		{0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00}, // 0x3F ?
		// 0x40-0x7F: the CBM graphics characters, blank.
	};

	// The C64 screen code, which indexes c64_font_8x8, for a PETSCII code.
	std::uint8_t petscii_to_screen_code(const std::uint8_t c)
	{
		if (c < 0x40) return c; // 0x20-0x3F: punctuation and digits
		if (c < 0x60) return static_cast<std::uint8_t>(c - 0x40); // 0x40-0x5F: @ A-Z [ pound ] up-arrow left-arrow
		if (c < 0x80) return static_cast<std::uint8_t>(c - 0x20); // 0x60-0x7F: graphics
		if (c < 0xC0) return static_cast<std::uint8_t>(c - 0x40); // 0xA0-0xBF: graphics
		if (c < 0xFF) return static_cast<std::uint8_t>(c - 0x80); // 0xC0-0xFE: graphics
		return 0x5E; // 0xFF
	}

	// The screen code a raw PETSCII or ASCII byte is drawn with. Control codes, space and shifted space
	// all draw as a blank cell.
	std::uint8_t to_glyph_code(const std::uint8_t c)
	{
		if (c <= 0x20 || c == 0xA0 || (c >= 0x80 && c < 0xA0)) return 0x20;
		return petscii_to_screen_code(c);
	}

	// A PETSCII byte in the plain-text form of the listing. Graphics and control codes have no ASCII
	// equivalent, so they become blanks and stay aligned with the glyph cells.
	char to_ascii(const std::uint8_t c)
	{
		if (c >= 0x20 && c <= 0x5A) return static_cast<char>(c);
		if (c >= 0xC1 && c <= 0xDA) return static_cast<char>(c - 0x80); // shifted A-Z
		return ' ';
	}

	// A real 1581 directory holds at most 296 entries, so this only bites on a corrupt or hostile image,
	// whose chain could otherwise yield thousands of rows for a caller that draws them as one image.
	constexpr std::size_t max_listing_rows = 512;

	libfiles::c64_listing dir_list(const media_entries& entries)
	{
		libfiles::c64_listing result;

		for (const auto& entry : entries)
		{
			if (result.rows.size() >= max_listing_rows) break;

			if (entry.file_type != 0)
			{
				std::string line;
				std::vector<std::uint8_t> codes;

				// A character for both the text and the glyphs.
				const auto push = [&](const std::uint8_t src_byte)
				{
					line += to_ascii(src_byte);
					codes.emplace_back(to_glyph_code(src_byte));
				};

				for (const auto c : std::to_string(entry.file_size))
				{
					push(static_cast<std::uint8_t>(c));
				}

				// Always at least one space, even after a four-digit 1581 block count.
				do
				{
					push(' ');
				}
				while (line.size() < 4);

				push('\"');

				for (const auto c : entry.pet_name)
				{
					push(c);
				}

				push('\"');

				// Pad out to the file-type column. A CRT name is 32 bytes and already runs past it, so fall
				// back to a single separator rather than butting up against the type.
				if (line.size() < 23)
				{
					while (line.size() < 23) push(' ');
				}
				else
				{
					push(' ');
				}

				std::string_view file_type;

				if (entry.file_type == 0x80) file_type = "DEL";
				if (entry.file_type == 0x81) file_type = "SEQ";
				if (entry.file_type == 0x82) file_type = "PRG";
				if (entry.file_type == 0x83) file_type = "USR";
				if (entry.file_type == 0x84) file_type = "REL";
				if (entry.file_type == 0x99) file_type = "CRT";

				for (const auto c : file_type)
				{
					push(static_cast<std::uint8_t>(c));
				}

				result.columns = std::max(result.columns, static_cast<std::uint32_t>(codes.size()));
				result.rows.push_back({std::move(line), std::move(codes)});
			}
		}

		return result;
	}

	enum class disk_format
	{
		unknown,
		d64, // 1541 single sided (35 or 40 tracks, optional error info)
		d71, // 1571 double sided (70 tracks, optional error info)
		d81 // 1581 3.5" (80 tracks)
	};

	disk_format classify_disk(const std::size_t len)
	{
		switch (len)
		{
		case 174848: // 35 tracks, no error info
		case 175531: // 35 tracks + 683 error bytes
		case 196608: // 40 tracks, no error info
		case 197376: // 40 tracks + 768 error bytes
			return disk_format::d64;
		case 349696: // 70 tracks, no error info
		case 351062: // 70 tracks + 1366 error bytes
			return disk_format::d71;
		case 819200: // 80 tracks
			return disk_format::d81;
		default:
			return disk_format::unknown;
		}
	}

	// The 1541 and 1571 record in zones, with a number of sectors per track that varies.
	int d64_sectors_per_track(const int track)
	{
		if (track <= 17) return 21;
		if (track <= 24) return 19;
		if (track <= 30) return 18;
		return 17;
	}

	// The byte offset of a (track, sector) within a disk image. Tracks count from 1.
	int disk_sector_offset(const disk_format fmt, const int track, const int sector)
	{
		constexpr int sector_size = 256;

		if (fmt == disk_format::d81)
		{
			return ((track - 1) * 40 + sector) * sector_size;
		}

		int sectors = 0;

		for (int t = 1; t < track; ++t)
		{
			// On a 1571 the second side (tracks 36 to 70) mirrors the zoning of side one.
			const int zone_track = (fmt == disk_format::d71 && t > 35) ? t - 35 : t;
			sectors += d64_sectors_per_track(zone_track);
		}

		return (sectors + sector) * sector_size;
	}

	media_entries parse_disk(const std::uint8_t* const data, const std::size_t data_len)
	{
		const auto fmt = classify_disk(data_len);

		if (fmt == disk_format::unknown)
			return {};

		constexpr int sector_size = 256;
		constexpr int dir_entry_size = 32;
		const int dir_track = (fmt == disk_format::d81) ? 40 : 18;
		const int dir_sector = (fmt == disk_format::d81) ? 3 : 1;

		media_entries disk;
		std::unordered_set<int> visited; // guards against a directory chain that loops

		int next_track = dir_track;
		int next_sector = dir_sector;

		while (next_track != 0)
		{
			const auto dir_sector_offset = disk_sector_offset(fmt, next_track, next_sector);

			if (dir_sector_offset < 0 ||
				dir_sector_offset + sector_size > static_cast<int>(data_len))
				break;

			if (!visited.insert(dir_sector_offset).second)
				break; // a sector already visited: the chain loops

			for (int i = 0; i < sector_size; i += dir_entry_size)
			{
				const auto dir_entry = dir_sector_offset + i;
				const std::uint8_t file_type = data[dir_entry + 2];

				if (file_type != 0)
				{
					media_entry entry;
					entry.file_type = file_type;
					entry.file_size = data[dir_entry + 30] + (data[dir_entry + 31] << 8);
					entry.pet_name.assign(data + dir_entry + 5, data + dir_entry + 5 + 16);
					disk.emplace_back(entry);
				}
			}
			next_track = data[dir_sector_offset];
			next_sector = data[dir_sector_offset + 1];
		}

		return disk;
	}

	// A T64 begins with a 64-byte header: a 32-byte description, the version, the number of entries the
	// directory has room for and the number used, then the tape's name. 32-byte entries follow: the
	// entry type, the 1541 file type, the start and end addresses, the offset of the data, and a 16-byte
	// name at 16.
	constexpr std::size_t t64_header_size = 64;
	constexpr std::size_t t64_entry_size = 32;

	// A CRT begins with a 64-byte header, the 32-byte cartridge name at 32.
	constexpr std::size_t crt_header_size = 64;

	media_entries parse_t64(const std::uint8_t* const data, const std::size_t data_len)
	{
		media_entries result;

		if (data_len < t64_header_size)
			return result;

		const std::uint16_t entry_room = load_le16(data + 34);
		const std::uint16_t used_entries = load_le16(data + 36);

		// The entries used, but no more than the directory has room for; a room of 0 sets no limit.
		std::uint32_t count = entry_room;
		if (count == 0 || count > used_entries) count = used_entries;

		for (std::uint32_t i = 0; i < count; ++i)
		{
			const auto dir_offset = t64_header_size + i * t64_entry_size;

			if (dir_offset + t64_entry_size > data_len)
				break;

			const auto* const file = data + dir_offset;

			if (file[0] == 0)
				continue; // a free entry

			const std::uint8_t type_1541 = file[1];
			const std::uint16_t start_address = load_le16(file + 2);
			const std::uint16_t end_address = load_le16(file + 4);

			media_entry entry;
			entry.file_type = type_1541 ? type_1541 : 0x82; // PRG unless it says otherwise
			entry.pet_name.assign(file + 16, file + 16 + 16);

			const std::uint32_t data_bytes = end_address > start_address
				                                 ? static_cast<std::uint32_t>(end_address - start_address)
				                                 : 0u;
			entry.file_size = (data_bytes + 2 + 253) / 254; // the load address and the data, in disk blocks
			result.push_back(entry);
		}

		return result;
	}

	media_entries parse_crt(const std::uint8_t* const data, const std::size_t data_len)
	{
		media_entries result;

		if (data_len < crt_header_size)
			return result;

		// A cartridge is one item however many ROM (CHIP) banks it holds, so it is listed once, by name.
		media_entry entry;
		entry.file_type = 0x99;
		entry.pet_name.assign(data + 32, data + 32 + 32);
		entry.file_size = static_cast<std::uint32_t>((data_len - crt_header_size + 253) / 254);
		result.push_back(entry);

		return result;
	}

	// A P00 wraps a single program: an 8-byte "C64File\0" signature, a 16-byte PETSCII name, a
	// record-size byte, then the program.
	media_entries parse_p00(const std::uint8_t* const data, const std::size_t data_len)
	{
		media_entries result;

		if (data_len < 26)
			return result;

		media_entry entry;
		entry.file_type = 0x82; // PRG
		entry.pet_name.assign(data + 8, data + 8 + 16);
		const std::size_t payload = data_len - 26;
		entry.file_size = static_cast<std::uint32_t>((payload + 253) / 254); // in disk blocks
		result.push_back(entry);

		return result;
	}

	// A PRG is just a 2-byte little-endian load address and the program. It carries no name, so it is
	// one unnamed program.
	media_entries parse_prg(const std::size_t data_len)
	{
		media_entries result;

		if (data_len < 2)
			return result;

		media_entry entry;
		entry.file_type = 0x82; // PRG
		entry.pet_name.assign(16, 0xA0); // shifted spaces, which draw as blanks
		// The load address counts towards the blocks, as it does inside a P00.
		entry.file_size = static_cast<std::uint32_t>((data_len + 253) / 254);
		result.push_back(entry);

		return result;
	}
}

libfiles::c64_listing libfiles::list_c64_media(const std::span<const std::uint8_t> file)
{
	const auto* const data = file.data();
	const auto len = file.size();

	media_entries media;

	if (len >= 8 &&
		std::memcmp(data, "C64File", 7) == 0)
	{
		media = parse_p00(data, len);
	}
	else if (len > 32 &&
		std::memcmp(data, "C64", 3) == 0 &&
		// The 32-byte description is not fixed text: writers emit "C64 tape image file", "C64S tape
		// image file" and "C64S tape file", so match what they share rather than one spelling.
		std::string_view(reinterpret_cast<const char*>(data), 32).find("tape") != std::string_view::npos)
	{
		media = parse_t64(data, len);
	}
	else if (len >= crt_header_size &&
		std::memcmp(data, "C64 CARTRIDGE   ", 16) == 0)
	{
		media = parse_crt(data, len);
	}
	else if (classify_disk(len) != disk_format::unknown)
	{
		media = parse_disk(data, len);
	}
	else if (len <= 65538)
	{
		// A PRG has no signature or fixed size, but it cannot exceed a C64's whole memory and its load
		// address. Anything larger that matched no format above is a damaged or unknown container, so
		// it lists nothing rather than one bogus unnamed row.
		media = parse_prg(len);
	}

	return dir_list(media);
}

void libfiles::draw_c64_listing(const c64_listing& listing, const pixel_buffer pixels, const std::uint32_t foreground,
                                const std::uint32_t background) noexcept
{
	if (!pixels.pixels) return;

	const auto bgrx = [](const std::uint32_t rgb)
	{
		return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(rgb), static_cast<std::uint8_t>(rgb >> 8),
		                                   static_cast<std::uint8_t>(rgb >> 16), 0};
	};

	const auto fg = bgrx(foreground);
	const auto bg = bgrx(background);
	const std::size_t width = listing.width();

	for (std::size_t li = 0; li < listing.rows.size(); ++li)
	{
		const auto& codes = listing.rows[li].screen_codes;
		const auto cells = std::min<std::size_t>(codes.size(), listing.columns);

		for (std::size_t row = 0; row < 8; ++row)
		{
			auto* const dst = pixels.pixels + (li * 8 + row) * pixels.stride;

			for (std::size_t x = 0; x < width; ++x) std::memcpy(dst + x * 4, bg.data(), 4);

			for (std::size_t ci = 0; ci < cells; ++ci)
			{
				const std::uint8_t bits = c64_font_8x8[codes[ci] & 0x7f][row];

				// Most rows of a directory listing are blank; skipping them saves eight tests a cell.
				if (bits == 0) continue;

				auto* const cell = dst + ci * 8 * 4;

				for (std::size_t col = 0; col < 8; ++col)
				{
					if (bits & (0x80 >> col)) std::memcpy(cell + col * 4, fg.data(), 4);
				}
			}
		}
	}
}
