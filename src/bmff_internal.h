// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The box header every ISO base media file shares - MP4 and QuickTime, HEIF and AVIF, and the
// JPEG XL container - decoded once, in its 32-bit and 64-bit forms, and the four-character codes boxes
// and sample entries are named by. What a size means once read - one that runs to the end, one smaller
// than its header or larger than what holds it - stays each reader's rule, since each follows its own
// reference: FFmpeg for MP4, libheif for HEIF and libjxl for JPEG XL.

#pragma once

#include "libfiles/reader.h"
#include "scan_internal.h"

#include <cstdint>
#include <span>
#include <string>

namespace libfiles::detail::bmff
{
	// A four-character code as a big-endian read of its bytes gives it. RIFF reads its codes
	// little-endian, and keeps its own.
	constexpr std::uint32_t fourcc(const char (&name)[5])
	{
		return static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[0])) << 24 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[1])) << 16 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[2])) << 8 |
			static_cast<std::uint8_t>(name[3]);
	}

	// A code's four characters as they stand, NULs and all, as libheif and libjxl hand one to a caller.
	inline std::string fourcc_text(const std::uint32_t code)
	{
		return {static_cast<char>(code >> 24), static_cast<char>(code >> 16), static_cast<char>(code >> 8), static_cast<char>(code)};
	}

	struct box_header
	{
		std::uint32_t type = 0;
		// The size as written: the 32-bit field, or the 64-bit one after it when the 32-bit field is one.
		std::uint64_t size = 0;
		// Eight bytes, or sixteen with a 64-bit size.
		std::uint32_t header = 8;

		bool large() const { return header == 16; }
	};

	enum class header_status : std::uint8_t
	{
		box,
		// Fewer than eight bytes: no box starts here.
		none,
		// A 64-bit size the bytes left cannot hold.
		cut,
	};

	// The header at the start of bytes.
	inline header_status read_header(const std::span<const std::uint8_t> bytes, box_header& out)
	{
		out = {};
		if (bytes.size() < 8) return header_status::none;

		out.size = load_be32(bytes.data());
		out.type = load_be32(bytes.data() + 4);
		if (out.size != 1) return header_status::box;
		if (bytes.size() < 16) return header_status::cut;

		out.size = static_cast<std::uint64_t>(load_be32(bytes.data() + 8)) << 32 | load_be32(bytes.data() + 12);
		out.header = 16;
		return header_status::box;
	}

	// The header at pos, where left bytes remain of what holds it. Each field is read only once it is
	// needed, so a header costs a reader over a stream no more than its fields do.
	inline header_status read_header(reader& r, const std::uint64_t pos, const std::uint64_t left, box_header& out)
	{
		out = {};
		if (left < 8) return header_status::none;

		out.size = r.be32(pos);
		out.type = r.be32(pos + 4);
		if (out.size != 1) return header_status::box;
		if (left < 16) return header_status::cut;

		out.size = r.be64(pos + 8);
		out.header = 16;
		return header_status::box;
	}
}
