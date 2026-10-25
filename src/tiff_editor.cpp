// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The TIFF directory editor. It edits IFD0 and the Exif and GPS directories of a TIFF stream
// without moving a byte the stream already holds: maker notes keep the absolute offsets they rely on,
// and a thumbnail stays where IFD1 says it is.

#include "legacy.h"
#include "tiff_internal.h"

#include <algorithm>
#include <set>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint16_t tag_exif_ifd = 0x8769;

		std::uint64_t type_size(const std::uint16_t type)
		{
			switch (type)
			{
			case 1: case 2: case 6: case 7: return 1;
			case 3: case 8: return 2;
			case 4: case 9: case 11: case 13: return 4;
			case 5: case 10: case 12: return 8;
			default: return 0;
			}
		}

		std::size_t index(const tiff_editor::directory d)
		{
			return static_cast<std::size_t>(d);
		}
	}

	std::uint16_t tiff_editor::u16(const std::uint8_t* p) const
	{
		return _little_endian ? load_le16(p) : load_be16(p);
	}

	std::uint32_t tiff_editor::u32(const std::uint8_t* p) const
	{
		return _little_endian ? load_le32(p) : load_be32(p);
	}

	void tiff_editor::put16(bytes& to, const std::uint16_t v) const
	{
		if (_little_endian)
		{
			to.push_back(static_cast<std::uint8_t>(v));
			to.push_back(static_cast<std::uint8_t>(v >> 8));
		}
		else
		{
			to.push_back(static_cast<std::uint8_t>(v >> 8));
			to.push_back(static_cast<std::uint8_t>(v));
		}
	}

	void tiff_editor::put32(bytes& to, const std::uint32_t v) const
	{
		if (_little_endian) append_le32(to, v);
		else append_be32(to, v);
	}

	// Every entry, as stored: the editor keeps what it does not change, well formed or not.
	bool tiff_editor::read_ifd(const tiff_bytes& data, const std::uint64_t offset, ifd& out)
	{
		if (offset < 8 || data.overflows(offset, 2)) return false;

		const auto count = data.u16(offset);
		if (count == 0 || data.overflows(offset + 2, 12ull * count + 4)) return false;

		out.present = true;
		out.offset = offset;
		out.entries.clear();

		for (std::uint32_t i = 0; i < count; ++i)
		{
			const auto at = offset + 2 + 12ull * i;
			entry e;
			e.tag = data.u16(at);
			e.type = data.u16(at + 2);
			e.count = data.u32(at + 4);
			std::copy_n(data.at(at + 8, 4), 4, e.value.begin());
			e.at = at;
			out.entries.push_back(e);
		}

		out.next = data.u32(offset + 2 + 12ull * count);
		return true;
	}

	bool tiff_editor::open(reader& r)
	{
		_size = r.size();
		_edits.clear();
		_ifds = {};

		if (_size < 8) return false;

		const auto* const header = r.view(0, 8).data();
		if (header[0] == 'I' && header[1] == 'I') _little_endian = true;
		else if (header[0] == 'M' && header[1] == 'M') _little_endian = false;
		else return false;

		const tiff_bytes data(r, _little_endian);

		if (data.u16(2) != 42) return false;
		if (!read_ifd(data, data.u32(4), _ifds[index(directory::ifd0)])) return false;

		// A sub-directory that cannot be read is treated as absent: an edit to it creates a new one.
		for (const auto& e : _ifds[index(directory::ifd0)].entries)
		{
			if (e.count != 1 || (e.type != 4 && e.type != 13)) continue;

			const auto offset = u32(e.value.data());
			if (e.tag == tag_exif_ifd) read_ifd(data, offset, _ifds[index(directory::exif)]);
			else if (e.tag == tag_gps_ifd) read_ifd(data, offset, _ifds[index(directory::gps)]);
		}

		return true;
	}

	bytes tiff_editor::empty_stream()
	{
		// Little-endian, IFD0 at 8 holding one entry, since an empty directory is not valid TIFF: the
		// orientation, top-left. An edit replaces or adds to it.
		return {
			'I', 'I', 42, 0, 8, 0, 0, 0,
			1, 0,
			0x12, 0x01, 3, 0, 1, 0, 0, 0, 1, 0, 0, 0,
			0, 0, 0, 0
		};
	}

	bool tiff_editor::has(const directory d, const std::uint16_t tag) const
	{
		if (const auto it = _edits.find({d, tag}); it != _edits.end()) return !it->second.remove;

		const auto& entries = _ifds[index(d)].entries;
		return std::ranges::any_of(entries, [tag](const entry& e) { return e.tag == tag; });
	}

	std::optional<byte_range> tiff_editor::value_range(const directory d, const std::uint16_t tag) const
	{
		for (const auto& e : _ifds[index(d)].entries)
		{
			if (e.tag != tag) continue;

			const auto size = type_size(e.type) * e.count;
			if (size <= 4) return byte_range{e.at + 8, size};

			const std::uint64_t offset = u32(e.value.data());
			if (offset > _size || size > _size - offset) return {};
			return byte_range{offset, size};
		}

		return {};
	}

	void tiff_editor::set(const directory d, const std::uint16_t tag, const std::uint16_t type, const std::uint32_t count,
	                      bytes value)
	{
		_edits[{d, tag}] = edit{false, type, count, std::move(value)};
	}

	void tiff_editor::set_ascii(const directory d, const std::uint16_t tag, const std::string_view text)
	{
		bytes value(text.begin(), text.end());
		value.push_back(0);
		const auto count = static_cast<std::uint32_t>(value.size());
		set(d, tag, type_ascii, count, std::move(value));
	}

	void tiff_editor::set_short(const directory d, const std::uint16_t tag, const std::uint16_t value)
	{
		bytes encoded;
		put16(encoded, value);
		set(d, tag, type_short, 1, std::move(encoded));
	}

	void tiff_editor::set_rationals(const directory d, const std::uint16_t tag,
	                                const std::vector<std::pair<std::uint32_t, std::uint32_t>>& values)
	{
		bytes encoded;

		for (const auto& [numerator, denominator] : values)
		{
			put32(encoded, numerator);
			put32(encoded, denominator);
		}

		set(d, tag, type_rational, static_cast<std::uint32_t>(values.size()), std::move(encoded));
	}

	void tiff_editor::remove(const directory d, const std::uint16_t tag)
	{
		_edits[{d, tag}] = edit{true, 0, 0, {}};
	}

	std::vector<replacement> tiff_editor::changes(const std::uint64_t stream_size) const
	{
		// Patched where a file lies, a value is written before the entry that names it, and a directory
		// appended before the pointer to it: what is appended goes first, then the values written where
		// they lie, then the entries and the header's pointer. An interruption between the two leaves the
		// old entry naming the new value, which reads as the new value.
		constexpr std::uint8_t entry_step = 1;

		std::vector<replacement> out;
		bytes appended;
		std::vector<std::pair<std::uint64_t, std::uint64_t>> claimed;

		// Values and tables are word aligned, as TIFF requires.
		const auto append_aligned = [&](const bytes& data) -> std::uint64_t
		{
			if ((stream_size + appended.size()) & 1u) appended.push_back(0);
			const auto offset = stream_size + appended.size();
			appended.insert(appended.end(), data.begin(), data.end());
			return offset;
		};

		const auto claim = [&claimed](const std::uint64_t offset, const std::uint64_t size)
		{
			for (const auto& [o, s] : claimed)
			{
				if (offset < o + s && o < offset + size) return false;
			}

			claimed.emplace_back(offset, size);
			return true;
		};

		std::array<std::optional<std::uint64_t>, 3> moved;
		auto edits = _edits;

		for (const auto d : {directory::gps, directory::exif, directory::ifd0})
		{
			if (d == directory::ifd0)
			{
				for (const auto& [sub, tag] : {std::pair{directory::exif, tag_exif_ifd}, std::pair{directory::gps, tag_gps_ifd}})
				{
					if (!moved[index(sub)]) continue;

					bytes pointer;
					put32(pointer, static_cast<std::uint32_t>(*moved[index(sub)]));
					edits[{directory::ifd0, tag}] = edit{false, type_long, 1, std::move(pointer)};
				}
			}

			const auto& old = _ifds[index(d)];
			std::map<std::uint16_t, std::array<std::uint8_t, 4>> fields;
			std::set<std::uint16_t> removed;

			for (const auto& [key, e] : edits)
			{
				if (key.first != d) continue;

				const auto tag = key.second;

				if (e.remove)
				{
					removed.insert(tag);
					continue;
				}

				std::array<std::uint8_t, 4> field{};

				if (e.value.size() <= 4)
				{
					std::copy(e.value.begin(), e.value.end(), field.begin());
				}
				else
				{
					// Where the old value lay out of line with room for the new one, it is written there: one
					// the same size, or shorter text, which the zeros after it end however many the old
					// entry counts. A shorter value of any other type is appended, since the old count would
					// read the zeros as part of it.
					std::optional<std::uint64_t> at;
					const auto zeros_end = e.type == type_ascii || e.type == type_byte;

					for (const auto& oe : old.entries)
					{
						if (oe.tag != tag) continue;

						const auto old_size = type_size(oe.type) * oe.count;
						const std::uint64_t old_offset = u32(oe.value.data());
						const auto fits = old_size == e.value.size() || (zeros_end && old_size > e.value.size());

						if (old_size > 4 && fits && old_offset <= stream_size &&
							old_size <= stream_size - old_offset && claim(old_offset, old_size))
						{
							auto data = e.value;
							data.resize(static_cast<std::size_t>(old_size), 0);
							out.push_back({old_offset, old_size, std::move(data)});
							at = old_offset;
						}

						break;
					}

					if (!at) at = append_aligned(e.value);
					if (*at > 0xFFFFFFFFu) throw format_error("the TIFF stream would exceed 4 GB");

					bytes encoded;
					put32(encoded, static_cast<std::uint32_t>(*at));
					std::copy(encoded.begin(), encoded.end(), field.begin());
				}

				fields[tag] = field;
			}

			if (fields.empty() && removed.empty()) continue;

			const auto encode_entry = [this](const std::uint16_t tag, const std::uint16_t type, const std::uint32_t count,
			                                 const std::array<std::uint8_t, 4>& field)
			{
				bytes e;
				put16(e, tag);
				put16(e, type);
				put32(e, count);
				e.insert(e.end(), field.begin(), field.end());
				return e;
			};

			// The same tags as before: each changed entry is rewritten where it lies.
			auto same_tags = old.present;

			if (same_tags)
			{
				for (const auto tag : removed)
				{
					if (std::ranges::any_of(old.entries, [tag](const entry& oe) { return oe.tag == tag; })) same_tags = false;
				}

				for (const auto& [tag, field] : fields)
				{
					if (std::ranges::none_of(old.entries, [tag](const entry& oe) { return oe.tag == tag; })) same_tags = false;
				}
			}

			if (same_tags)
			{
				for (const auto& oe : old.entries)
				{
					const auto it = fields.find(oe.tag);
					if (it == fields.end()) continue;

					const auto& e = edits.at({d, oe.tag});
					auto rewritten = encode_entry(oe.tag, e.type, e.count, it->second);
					if (rewritten != encode_entry(oe.tag, oe.type, oe.count, oe.value)) out.push_back({oe.at, 12, std::move(rewritten), entry_step});
				}

				continue;
			}

			// Otherwise a new directory, sorted by tag, is appended and its parent repointed. Entries
			// that did not change keep their values where they are.
			std::map<std::uint16_t, bytes> table_entries;

			for (const auto& oe : old.entries)
			{
				if (removed.contains(oe.tag) || fields.contains(oe.tag)) continue;
				table_entries[oe.tag] = encode_entry(oe.tag, oe.type, oe.count, oe.value);
			}

			for (const auto& [tag, field] : fields)
			{
				const auto& e = edits.at({d, tag});
				table_entries[tag] = encode_entry(tag, e.type, e.count, field);
			}

			if (table_entries.empty() && d != directory::ifd0)
			{
				// A sub-directory left with nothing is dropped, with its pointer.
				edits[{directory::ifd0, d == directory::exif ? tag_exif_ifd : tag_gps_ifd}] = edit{true, 0, 0, {}};
				continue;
			}

			if (table_entries.size() > 0xFFFF) throw format_error("a TIFF directory would hold too many entries");

			bytes table;
			put16(table, static_cast<std::uint16_t>(table_entries.size()));
			for (const auto& [tag, e] : table_entries) table.insert(table.end(), e.begin(), e.end());
			put32(table, old.present ? old.next : 0);

			const auto offset = append_aligned(table);
			if (offset > 0xFFFFFFFFu) throw format_error("the TIFF stream would exceed 4 GB");

			moved[index(d)] = offset;

			if (d == directory::ifd0)
			{
				bytes header_offset;
				put32(header_offset, static_cast<std::uint32_t>(offset));
				out.push_back({4, 4, std::move(header_offset), entry_step});
			}
		}

		if (!appended.empty()) out.push_back({stream_size, 0, std::move(appended)});

		std::ranges::sort(out, {}, &replacement::offset);
		return out;
	}

	bytes apply_changes(const std::span<const std::uint8_t> block, const std::vector<replacement>& changes)
	{
		bytes result;
		result.reserve(block.size());
		std::uint64_t in = 0;

		for (const auto& c : changes)
		{
			result.insert(result.end(), block.begin() + static_cast<std::ptrdiff_t>(in),
			              block.begin() + static_cast<std::ptrdiff_t>(c.offset));
			result.insert(result.end(), c.data.begin(), c.data.end());
			in = c.offset + c.length;
		}

		result.insert(result.end(), block.begin() + static_cast<std::ptrdiff_t>(in), block.end());
		return result;
	}
}
