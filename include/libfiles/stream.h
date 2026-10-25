// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The file interfaces libfiles reads and writes through. The caller opens the file and
// implements them, so libfiles never names a path or calls the OS. Access is positional: there is no
// shared file pointer for one parser to move under another.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace libfiles
{
	// A file libfiles reads.
	class stream
	{
	public:
		virtual ~stream() = default;

		virtual std::uint64_t size() = 0;

		// Reads up to `size` bytes at `offset` and returns how many were read. Fewer than asked means
		// the end of the file was reached or the read failed.
		virtual std::size_t read(std::uint64_t offset, void* data, std::size_t size) = 0;
	};

	// A file libfiles may also change: patched in place, or written as a replacement.
	class writable_stream : public stream
	{
	public:
		// Writes all `size` bytes at `offset`, growing the file if needed. False if any were not written.
		virtual bool write(std::uint64_t offset, const void* data, std::size_t size) = 0;

		// Sets the file's length, cutting it short or extending it.
		virtual bool resize(std::uint64_t size) = 0;

		// Makes what was written so far durable. A patch that must survive interruption orders its
		// writes around this.
		virtual bool flush() = 0;
	};

	class memory_stream final : public writable_stream
	{
	public:
		memory_stream() = default;

		explicit memory_stream(std::vector<std::uint8_t> bytes) : _bytes(std::move(bytes))
		{
		}

		std::uint64_t size() override { return _bytes.size(); }
		std::size_t read(std::uint64_t offset, void* data, std::size_t size) override;
		bool write(std::uint64_t offset, const void* data, std::size_t size) override;
		bool resize(std::uint64_t size) override;
		bool flush() override { return true; }

		const std::vector<std::uint8_t>& bytes() const { return _bytes; }
		std::vector<std::uint8_t>& bytes() { return _bytes; }

	private:
		std::vector<std::uint8_t> _bytes;
	};
}
