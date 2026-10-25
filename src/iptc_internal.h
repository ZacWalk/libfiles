// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: An IPTC-IIM dataset's header, read alike by the reader and the writer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace libfiles::detail
{
	struct iim_header
	{
		std::uint8_t record = 0;
		std::uint8_t number = 0;
		// Where the dataset's data starts in the block, and how many bytes it holds.
		std::size_t data_offset = 0;
		std::size_t length = 0;
	};

	// The header of the dataset at offset, when there is one to trust: the 0x1C marker, the record and
	// dataset numbers, and a size whose data lies inside the block - two octets or, with the top bit
	// set, IIM 4.1's count of the one to four octets that follow and hold it. Otherwise nothing, and a
	// walk stops there.
	std::optional<iim_header> read_iim_header(std::span<const std::uint8_t> block, std::size_t offset);
}
