// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The one reader of an ID3v2 tag's structure, which the media scan, read_packet and the MP3
// writer share: the header, the extended header, the frame walk and the plain sizes iTunes wrote, how a
// frame's data lies behind its flags, the faults that make a tag unreadable to an update, and the rule
// for which frame holds a file's XMP packet.

#include "id3_internal.h"
#include "scan_internal.h"

#include <algorithm>
#include <iterator>

namespace libfiles::detail
{
	std::uint32_t id3_syncsafe(const std::uint8_t* const p) noexcept
	{
		return static_cast<std::uint32_t>(p[0]) << 21 | static_cast<std::uint32_t>(p[1]) << 14 |
			static_cast<std::uint32_t>(p[2]) << 7 | p[3];
	}

	std::optional<id3_header> parse_id3_header(const std::span<const std::uint8_t> bytes) noexcept
	{
		if (bytes.size() < id3_header_size || !starts_with(bytes, "ID3")) return {};

		id3_header h;
		h.version = bytes[3];
		h.revision = bytes[4];
		h.flags = bytes[5];
		h.size_syncsafe = ((bytes[6] | bytes[7] | bytes[8] | bytes[9]) & 0x80) == 0;
		h.size = id3_syncsafe(bytes.data() + 6);
		return h;
	}

	std::uint64_t leading_id3_size(const std::span<const std::uint8_t> head) noexcept
	{
		if (head.size() < 10 || head[0] != 'I' || head[1] != 'D' || head[2] != '3') return 0;
		if (head[3] < 2 || head[3] > 4 || ((head[6] | head[7] | head[8] | head[9]) & 0x80)) return 0;

		const auto size = std::uint64_t{id3_syncsafe(head.data() + 6)};
		return 10 + size + (head[3] == 4 && (head[5] & 0x10) ? 10 : 0);
	}

	std::optional<std::uint64_t> id3_frames_begin(reader& r, const id3_header& header, const std::uint64_t begin, const std::uint64_t end)
	{
		if (!header.extended()) return begin;
		if (end < begin || end - begin < 4) return {};

		// ID3v2.3 counts the extended header's size without its own four bytes; ID3v2.4 with them, syncsafe.
		const auto field = r.view(begin, 4);
		const std::uint64_t length = header.version == 3 ? load_be32(field.data()) + 4ull : id3_syncsafe(field.data());
		if (length < 4 || length > end - begin) return {};
		return begin + length;
	}

	id3_frame_walker::id3_frame_walker(reader& r, const int version, const std::uint64_t begin, const std::uint64_t end,
	                                   const id3_sizes sizes) noexcept :
		_r(r),
		_version(version),
		_position(begin),
		_end(end),
		_sizes(sizes)
	{
	}

	std::optional<id3_frame> id3_frame_walker::next()
	{
		const std::size_t header = _version == 2 ? 6 : 10;
		if (_position > _end || _end - _position < header) return {};

		const auto h = _r.view(_position, header);
		if (h[0] == 0) return {};

		id3_frame f;
		f.id_length = _version == 2 ? 3 : 4;

		for (std::size_t i = 0; i < f.id_length; ++i)
		{
			const auto c = h[i];

			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			{
				_stray = true;
				return {};
			}

			f.id[i] = static_cast<char>(c);
		}

		if (_version == 2) f.size = static_cast<std::uint64_t>(h[3]) << 16 | static_cast<std::uint64_t>(h[4]) << 8 | h[5];
		else if (_version == 3 || _sizes == id3_sizes::plain || ((h[4] | h[5] | h[6] | h[7]) & 0x80)) f.size = load_be32(h.data() + 4);
		else f.size = id3_syncsafe(h.data() + 4);

		if (f.size > _end - _position - header)
		{
			_overran = true;
			return {};
		}

		if (_version != 2) f.flags = static_cast<std::uint16_t>(h[8] << 8 | h[9]);
		f.offset = _position;
		f.header_size = header;
		_position += header + f.size;
		return f;
	}

	bool id3_walks_with_plain_sizes(reader& r, const std::uint64_t offset, const id3_header& tag)
	{
		try
		{
			const auto end = std::min(offset + tag.end(), r.size());
			const auto first = id3_frames_begin(r, tag, offset + id3_header_size, end);
			if (!first) return false;

			id3_frame_walker frames(r, tag.version, *first, end, id3_sizes::plain);
			while (frames.next())
			{
			}

			return !frames.overran() && !frames.stray();
		}
		catch (const read_error&)
		{
			return false;
		}
	}

	bool id3_retry_with_plain_sizes(reader& r, const std::uint64_t offset, const id3_header& tag, const bool standard_walk_damaged)
	{
		return standard_walk_damaged && tag.version == 4 && id3_walks_with_plain_sizes(r, offset, tag);
	}

	const char* id3_fault(const id3_header& tag, const std::uint64_t offset, const std::uint64_t file_size)
	{
		if (!tag.known_version()) return "the ID3 tag's version is not one libfiles writes";
		if (!tag.size_syncsafe) return "the ID3 tag's size is damaged";
		if (tag.compressed()) return "a compressed ID3v2.2 tag cannot be updated";
		if (offset > file_size || tag.total() > file_size - offset) return "the ID3 tag runs past the end of the file";
		return nullptr;
	}

	id3_resynchronised::id3_resynchronised(reader& file, const std::uint64_t begin, const std::uint64_t end) : _file(file), _end(end)
	{
		// One pass counts what the bytes come to, marking where each block of them starts.
		auto after_ff = false;

		for (auto at = begin; at < end; at += block)
		{
			_marks.push_back({at, _size, after_ff});

			for (const auto b : _file.view(at, chunk(at)))
			{
				if (!after_ff || b != 0) ++_size;
				after_ff = b == 0xFF;
			}
		}
	}

	std::size_t id3_resynchronised::read(const std::uint64_t offset, void* const data, const std::size_t size)
	{
		if (offset >= _size || size == 0) return 0;

		const auto mark = std::prev(std::ranges::upper_bound(_marks, offset, {}, &block_mark::kept));
		auto skip = offset - mark->kept;
		auto after_ff = mark->after_ff;
		auto* const out = static_cast<std::uint8_t*>(data);
		std::size_t copied = 0;

		for (auto at = mark->at; at < _end && copied < size; at += block)
		{
			for (const auto b : _file.view(at, chunk(at)))
			{
				const auto kept = !after_ff || b != 0;
				after_ff = b == 0xFF;

				if (!kept) continue;

				if (skip > 0)
				{
					--skip;
					continue;
				}

				out[copied++] = b;
				if (copied == size) break;
			}
		}

		return copied;
	}

	void id3_resynchronise(std::vector<std::uint8_t>& bytes)
	{
		std::size_t kept = 0;
		auto after_ff = false;

		for (std::size_t i = 0; i < bytes.size(); ++i)
		{
			const auto b = bytes[i];
			if (!after_ff || b != 0) bytes[kept++] = b;
			after_ff = b == 0xFF;
		}

		bytes.resize(kept);
	}

	id3_frame_layout id3_layout_of(const id3_header& tag, const id3_frame& f) noexcept
	{
		id3_frame_layout l;
		const auto format = static_cast<std::uint8_t>(f.flags);

		if (tag.version == 3)
		{
			// The decompressed size, the encryption method and the group, in the order of their flags.
			l.compressed = (format & 0x80) != 0;
			l.encrypted = (format & 0x40) != 0;
			l.prefix = (l.compressed ? 4u : 0u) + (l.encrypted ? 1u : 0u) + ((format & 0x20) ? 1u : 0u);
		}
		else if (tag.version == 4)
		{
			// The group, the encryption method and the data length, which compression requires.
			l.compressed = (format & 0x08) != 0;
			l.encrypted = (format & 0x04) != 0;
			l.unsynchronised = (format & 0x02) != 0 || tag.unsynchronised();
			l.prefix = ((format & 0x40) ? 1u : 0u) + (l.encrypted ? 1u : 0u) + ((format & 0x01) || l.compressed ? 4u : 0u);
		}

		return l;
	}

	std::optional<std::vector<std::uint8_t>> id3_frame_data(reader& source, const id3_frame& f, const id3_frame_layout& l,
	                                                        const std::size_t max_size)
	{
		if (l.encrypted || f.size > max_size) return {};

		auto bytes = source.read(f.body(), static_cast<std::size_t>(f.size));
		if (l.unsynchronised) id3_resynchronise(bytes);
		if (bytes.size() < l.prefix) return {};
		bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(l.prefix));
		if (!l.compressed) return bytes;

		std::vector<std::uint8_t> inflated;
		if (!zlib_inflate(bytes, inflated, max_size)) return {};
		return inflated;
	}

	std::optional<std::span<const std::uint8_t>> id3_owned_payload(const std::span<const std::uint8_t> data, const std::string_view owner)
	{
		if (data.size() <= owner.size() || !starts_with(data, owner) || data[owner.size()] != 0) return {};
		return data.subspan(owner.size() + 1);
	}

	std::optional<std::vector<std::uint8_t>> id3_xmp_payload(const std::span<const std::uint8_t> data)
	{
		auto packet = id3_owned_payload(data, "XMP");
		if (!packet) return {};

		auto bytes = *packet;
		while (!bytes.empty() && bytes.back() == 0) bytes = bytes.first(bytes.size() - 1);
		return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
	}

	bool is_id3_private_frame(const std::string_view id) noexcept
	{
		// PRV is the MP3 writer's name for a packet frame in ID3v2.2, which defined none.
		return id == "PRIV" || id == "PRV";
	}

	id3_packet_reading id3_packet(reader& r, const std::uint64_t offset)
	{
		id3_packet_reading result;
		if (!r.contains(offset, id3_header_size)) return result;

		const auto tag = parse_id3_header(r.view(offset, id3_header_size));
		if (!tag) return result;

		if (id3_fault(*tag, offset, r.size()))
		{
			result.readable = false;
			return result;
		}

		const auto end = offset + tag->end();
		std::optional<id3_resynchronised> whole;
		std::optional<reader> resynchronised_reader;
		auto* source = &r;
		auto begin = offset + id3_header_size;
		auto stop = end;

		if (tag->unsynchronised() && tag->version < 4)
		{
			whole.emplace(r, begin, end);
			resynchronised_reader.emplace(*whole);
			source = &*resynchronised_reader;
			begin = 0;
			stop = whole->size();
		}

		const auto first = id3_frames_begin(*source, *tag, begin, stop);

		if (!first)
		{
			result.readable = false;
			return result;
		}

		const auto walk = [&](const id3_sizes sizes, std::optional<std::vector<std::uint8_t>>& packet)
		{
			id3_frame_walker frames(*source, tag->version, *first, stop, sizes);
			std::size_t count = 0;

			while (const auto f = frames.next())
			{
				if (!packet && f->size != 0 && is_id3_private_frame(f->name()))
				{
					const auto l = id3_layout_of(*tag, *f);
					if (const auto data = id3_frame_data(*source, *f, l, id3_max_frame)) packet = id3_xmp_payload(*data);
				}

				if (++count == id3_max_frames) return std::pair{false, false};
			}

			return std::pair{frames.overran(), frames.stray()};
		};

		std::optional<std::vector<std::uint8_t>> packet;
		auto [overran, stray] = walk(id3_sizes::standard, packet);

		if (source == &r && id3_retry_with_plain_sizes(r, offset, *tag, overran || stray))
		{
			packet.reset();
			overran = walk(id3_sizes::plain, packet).first;
		}

		if (overran)
		{
			result.readable = false;
			return result;
		}

		if (packet) result.packet = std::move(*packet);
		return result;
	}
}
