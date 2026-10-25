// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: How a Photoshop image resource is read, by every walker of a block of them: the PSD scan,
// parse_photoshop_resources for JPEG's APP13, the writers and the JPEG encoder. Its data must lie
// inside the block; the pad byte after the last resource's data may be missing, as some writers leave
// it, and every writer here puts it back.

#pragma once

#include "scan_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace libfiles::detail
{
	// The longest header a resource has: the '8BIM' signature, its id, a Pascal name of 255 bytes with
	// its length byte, and the length of its data.
	inline constexpr std::size_t max_photoshop_resource_header = 4u + 2u + 256u + 4u;

	// A resource, its offsets counted from the signature.
	struct photoshop_resource_header
	{
		std::uint16_t id = 0;
		// The Pascal name: its length byte and the name, padded to an even size.
		std::size_t name_size = 0;
		std::size_t data_offset = 0;
		// The data's length, not counting the pad byte that follows odd data.
		std::uint32_t length = 0;
		// Where the next resource starts: past the data's pad byte, or at the end of the block when the
		// last resource lacks it.
		std::size_t size = 0;

		// Its size with the pad byte, as it is written. Counted in 64 bits, so a 4 GiB resource cannot
		// wrap on x86.
		std::uint64_t padded_size() const noexcept
		{
			return std::uint64_t{data_offset} + length + (length & 1u);
		}

		bool lacks_pad() const noexcept { return padded_size() > size; }
	};

	// The resource at the start of a block with available bytes from there, of which head holds the
	// first: all of them, or at least max_photoshop_resource_header. Nothing when the block does not
	// start with '8BIM', or the header or the data runs past its end.
	inline std::optional<photoshop_resource_header> read_photoshop_resource(const std::span<const std::uint8_t> head,
	                                                                        const std::uint64_t available) noexcept
	{
		if (head.size() < 12u || std::memcmp(head.data(), "8BIM", 4) != 0) return std::nullopt;

		photoshop_resource_header resource;
		resource.id = load_be16(head.data() + 4);
		const std::size_t name_length = head[6];
		resource.name_size = 1u + name_length + ((name_length & 1u) ? 0u : 1u);
		resource.data_offset = 4u + 2u + resource.name_size + 4u;
		if (resource.data_offset > head.size()) return std::nullopt;

		resource.length = load_be32(head.data() + resource.data_offset - 4u);
		if (std::uint64_t{resource.data_offset} + resource.length > available) return std::nullopt;

		resource.size = static_cast<std::size_t>(std::min(resource.padded_size(), available));
		return resource;
	}

	inline std::optional<photoshop_resource_header> read_photoshop_resource(const std::span<const std::uint8_t> block) noexcept
	{
		return read_photoshop_resource(block, block.size());
	}
}
