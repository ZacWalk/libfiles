// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "libfiles/reader.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace libfiles
{
	std::size_t memory_stream::read(const std::uint64_t offset, void* data, const std::size_t size)
	{
		if (offset >= _bytes.size()) return 0;

		const auto available = _bytes.size() - static_cast<std::size_t>(offset);
		const auto n = std::min(available, size);
		std::memcpy(data, _bytes.data() + offset, n);
		return n;
	}

	bool memory_stream::write(const std::uint64_t offset, const void* data, const std::size_t size)
	{
		if (offset > std::numeric_limits<std::size_t>::max() - size) return false;

		const auto end = static_cast<std::size_t>(offset) + size;
		if (end > _bytes.size()) _bytes.resize(end);
		if (size) std::memcpy(_bytes.data() + offset, data, size);
		return true;
	}

	bool memory_stream::resize(const std::uint64_t size)
	{
		if (size > std::numeric_limits<std::size_t>::max()) return false;
		_bytes.resize(static_cast<std::size_t>(size));
		return true;
	}

	reader::reader(const std::span<const std::uint8_t> bytes) noexcept : _memory(bytes.data()), _size(bytes.size())
	{
	}

	reader::reader(stream& source, const std::size_t block_size) :
		_source(&source),
		_size(source.size()),
		_block_size(block_size ? block_size : default_block_size)
	{
	}

	std::span<const std::uint8_t> reader::resident() const noexcept
	{
		if (_source) return {};
		return {_memory, static_cast<std::size_t>(_size)};
	}

	void reader::check(const std::uint64_t offset, const std::uint64_t length) const
	{
		if (!contains(offset, length)) throw read_error("read past the end of the file");
	}

	void reader::fill(const std::uint64_t offset, std::uint8_t* const data, const std::size_t length)
	{
		if (length == 0) return;

		const auto got = _source->read(offset, data, length);
		++_source_reads;
		_source_bytes += got;

		// check() has already placed the range inside the file, so a short read means the file
		// failed or changed size under the scan.
		if (got != length) throw read_error("the file could not be read");
	}

	const std::uint8_t* reader::window(const std::uint64_t offset, const std::size_t length)
	{
		check(offset, length);

		if (!_source) return _memory + offset;

		const auto window_end = _window_start + _window.size();

		if (offset >= _window_start && offset + length <= window_end)
		{
			return _window.data() + (offset - _window_start);
		}

		const auto block = static_cast<std::uint64_t>(_block_size);
		const auto start = offset / block * block;
		const auto wanted_end = offset + length;
		const auto rounded_end = wanted_end / block * block + (wanted_end % block ? block : 0);
		const auto end = rounded_end < wanted_end ? _size : std::min(rounded_end, _size);
		const auto bytes = end - start;

		if (bytes > std::numeric_limits<std::size_t>::max()) throw read_error("read too large to hold");

		std::vector<std::uint8_t> next(static_cast<std::size_t>(bytes));

		// Whatever the old window already holds of the new one is kept, so a parser stepping over a
		// block boundary reads only the block it has not seen.
		const auto keep_start = std::max(start, _window_start);
		const auto keep_end = std::min(end, window_end);

		if (keep_start < keep_end)
		{
			std::memcpy(next.data() + (keep_start - start), _window.data() + (keep_start - _window_start),
			            static_cast<std::size_t>(keep_end - keep_start));
			fill(start, next.data(), static_cast<std::size_t>(keep_start - start));
			fill(keep_end, next.data() + (keep_end - start), static_cast<std::size_t>(end - keep_end));
		}
		else
		{
			fill(start, next.data(), next.size());
		}

		_window = std::move(next);
		_window_start = start;
		return _window.data() + (offset - _window_start);
	}

	std::span<const std::uint8_t> reader::view(const std::uint64_t offset, const std::size_t length)
	{
		return {window(offset, length), length};
	}

	void reader::read(const std::uint64_t offset, void* const data, const std::size_t length)
	{
		check(offset, length);
		if (length == 0) return;

		auto* const out = static_cast<std::uint8_t*>(data);

		if (_source && length >= _block_size)
		{
			const auto window_end = _window_start + _window.size();

			if (offset < _window_start || offset + length > window_end)
			{
				// A read this large goes straight into the caller's buffer: through the window it
				// would be read and then copied.
				fill(offset, out, length);
				return;
			}
		}

		std::memcpy(out, window(offset, length), length);
	}

	std::vector<std::uint8_t> reader::read(const std::uint64_t offset, const std::size_t length)
	{
		check(offset, length);
		std::vector<std::uint8_t> result(length);
		read(offset, result.data(), length);
		return result;
	}

	std::uint8_t reader::u8(const std::uint64_t offset)
	{
		return *window(offset, 1);
	}

	std::uint16_t reader::be16(const std::uint64_t offset)
	{
		const auto* const p = window(offset, 2);
		return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
	}

	std::uint16_t reader::le16(const std::uint64_t offset)
	{
		const auto* const p = window(offset, 2);
		return static_cast<std::uint16_t>(p[1] << 8 | p[0]);
	}

	std::uint32_t reader::be32(const std::uint64_t offset)
	{
		const auto* const p = window(offset, 4);
		return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
			static_cast<std::uint32_t>(p[2]) << 8 | p[3];
	}

	std::uint32_t reader::le32(const std::uint64_t offset)
	{
		const auto* const p = window(offset, 4);
		return static_cast<std::uint32_t>(p[3]) << 24 | static_cast<std::uint32_t>(p[2]) << 16 |
			static_cast<std::uint32_t>(p[1]) << 8 | p[0];
	}

	std::uint64_t reader::be64(const std::uint64_t offset)
	{
		const auto* const p = window(offset, 8);
		std::uint64_t v = 0;
		for (auto i = 0; i < 8; ++i) v = v << 8 | p[i];
		return v;
	}

	std::uint64_t reader::le64(const std::uint64_t offset)
	{
		const auto* const p = window(offset, 8);
		std::uint64_t v = 0;
		for (auto i = 7; i >= 0; --i) v = v << 8 | p[i];
		return v;
	}
}
