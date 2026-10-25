// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Bounds-checked, positional reading for the format parsers. Over memory it borrows the
// bytes. Over a stream it reads through one block-aligned window, so a parser walking a file's
// headers costs one read per block it touches rather than one per field - and over a network share
// every read is a round trip.

#pragma once

#include "libfiles/stream.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace libfiles
{
	// Thrown when a parser asks for bytes the file does not have, or the stream fails to supply them.
	class read_error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	class reader
	{
	public:
		static constexpr std::size_t default_block_size = 64 * 1024;

		explicit reader(std::span<const std::uint8_t> bytes) noexcept;
		explicit reader(stream& source, std::size_t block_size = default_block_size);

		// A reader borrows memory, so it cannot be given a temporary to outlive.
		explicit reader(std::vector<std::uint8_t>&&) = delete;

		reader(const reader&) = delete;
		reader& operator=(const reader&) = delete;

		std::uint64_t size() const noexcept { return _size; }

		// The whole file without copying when the reader is over memory, otherwise empty.
		std::span<const std::uint8_t> resident() const noexcept;

		// True when [offset, offset + length) lies inside the file. Both come from untrusted fields,
		// so the sum is never formed.
		bool contains(const std::uint64_t offset, const std::uint64_t length) const noexcept
		{
			return offset <= _size && length <= _size - offset;
		}

		// Borrows [offset, offset + length). Valid until the next call on this reader.
		std::span<const std::uint8_t> view(std::uint64_t offset, std::size_t length);

		void read(std::uint64_t offset, void* data, std::size_t length);
		std::vector<std::uint8_t> read(std::uint64_t offset, std::size_t length);

		std::uint8_t u8(std::uint64_t offset);
		std::uint16_t be16(std::uint64_t offset);
		std::uint16_t le16(std::uint64_t offset);
		std::uint32_t be32(std::uint64_t offset);
		std::uint32_t le32(std::uint64_t offset);
		std::uint64_t be64(std::uint64_t offset);
		std::uint64_t le64(std::uint64_t offset);

		// What the reads so far cost the stream: the measure of a scan over a network share.
		std::uint64_t source_reads() const noexcept { return _source_reads; }
		std::uint64_t source_bytes() const noexcept { return _source_bytes; }

	private:
		void check(std::uint64_t offset, std::uint64_t length) const;
		const std::uint8_t* window(std::uint64_t offset, std::size_t length);
		void fill(std::uint64_t offset, std::uint8_t* data, std::size_t length);

		stream* _source = nullptr;
		const std::uint8_t* _memory = nullptr;
		std::uint64_t _size = 0;
		std::size_t _block_size = default_block_size;
		std::vector<std::uint8_t> _window;
		std::uint64_t _window_start = 0;
		std::uint64_t _source_reads = 0;
		std::uint64_t _source_bytes = 0;
	};
}
