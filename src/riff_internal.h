// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The structure RIFF files share - WAV, AVI and WebP: forms and the chunks and lists in them, with
// the 64-bit sizes of RF64 and BW64 - read by the one chunk walker the scans and the writers use, the chunk
// ids, and the rule for which `_PMX` chunk holds a WAV's or AVI's packet.
//
// A writer takes a file only as it stands whole, and refuses one that does not; a scan reads what it can
// of a file cut short. So the walker refuses nothing: it visits a chunk that runs past the end of what
// holds it, as the last, and its caller decides. walk_layout keeps a WAV's or AVI's form or chunk cut
// there, and notes as the layout's fault why the writer would not take the file.

#pragma once

#include "libfiles/reader.h"
#include "scan_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace libfiles::detail::riff
{
	constexpr std::uint32_t fourcc(const char (&s)[5])
	{
		return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24;
	}

	inline constexpr auto riff_id = fourcc("RIFF");
	inline constexpr auto rf64_id = fourcc("RF64");
	inline constexpr auto bw64_id = fourcc("BW64");
	inline constexpr auto list_id = fourcc("LIST");
	inline constexpr auto junk_id = fourcc("JUNK");
	inline constexpr auto ds64_id = fourcc("ds64");
	inline constexpr auto data_id = fourcc("data");
	inline constexpr auto pmx_id = fourcc("_PMX");
	inline constexpr auto disp_id = fourcc("DISP");
	inline constexpr auto idit_id = fourcc("IDIT");
	inline constexpr auto wave_type = fourcc("WAVE");
	inline constexpr auto avi_type = fourcc("AVI ");
	inline constexpr auto avix_type = fourcc("AVIX");
	inline constexpr auto info_type = fourcc("INFO");
	inline constexpr auto hdrl_type = fourcc("hdrl");
	inline constexpr auto movi_type = fourcc("movi");

	// WebP's form and chunks.
	inline constexpr auto webp_type = fourcc("WEBP");
	inline constexpr auto vp8x_id = fourcc("VP8X");
	inline constexpr auto vp8_id = fourcc("VP8 ");
	inline constexpr auto vp8l_id = fourcc("VP8L");
	inline constexpr auto alph_id = fourcc("ALPH");
	inline constexpr auto anim_id = fourcc("ANIM");
	inline constexpr auto anmf_id = fourcc("ANMF");
	inline constexpr auto iccp_id = fourcc("ICCP");
	inline constexpr auto exif_id = fourcc("EXIF");
	inline constexpr auto xmp_id = fourcc("XMP ");

	// The 32-bit size an RF64 or BW64 chunk gives when its ds64 chunk holds the real one.
	inline constexpr std::uint32_t size_in_ds64 = 0xFFFFFFFF;

	struct chunk
	{
		std::uint64_t start = 0;
		std::uint32_t id = 0;
		// The payload's size: the header's, or the ds64 chunk's for an RF64 or BW64 chunk that leaves it
		// there.
		std::uint64_t size = 0;
		// A LIST's or RIFF's type.
		std::uint32_t type = 0;

		std::uint64_t payload() const { return start + 8; }
		std::uint64_t padded_size() const { return 8ull + size + (size & 1); }
		std::uint64_t end() const { return start + padded_size(); }
	};

	// The 64-bit sizes of an RF64 or BW64 file (EBU Tech 3306, ITU-R BS.2088), from the ds64 chunk that
	// comes first in its form.
	struct sizes_64
	{
		std::uint64_t riff = 0;
		std::uint64_t data = 0;
		std::uint64_t samples = 0;
		// Any other chunk's, by id.
		std::vector<std::pair<std::uint32_t, std::uint64_t>> table;
	};

	struct form
	{
		chunk header;
		std::vector<chunk> children;
		// Where the walk of its chunks ended: where its size says, or at the end of the file for a form that
		// runs past it.
		std::uint64_t end = 0;

		std::uint64_t data_end() const { return header.start + 8 + header.size; }
	};

	struct layout
	{
		std::vector<form> forms;
		bool avi = false;
		// Where the first form's movie data ends, or 0.
		std::uint64_t movi_end = 0;
		// An RF64 or BW64 file's 64-bit sizes.
		std::optional<sizes_64> sizes;
		// The first reason the writer cannot take the file as it stands, or null.
		const char* fault = nullptr;

		const form& first() const { return forms.front(); }
	};

	// How a walk over chunks ended: at the end of what held them, at a chunk that runs past that end -
	// visited, and the last - or where the visitor stopped it.
	enum class walk_end : std::uint8_t
	{
		whole,
		cut,
		stopped,
	};

	// No file is this large, and a 64-bit size past it would overflow the arithmetic on it.
	inline constexpr std::uint64_t max_size_64 = std::uint64_t{1} << 62;

	// The ds64 chunk at pos, when there is one whole enough to read.
	inline std::optional<sizes_64> read_ds64(reader& r, const std::uint64_t pos)
	{
		if (!r.contains(pos, 8) || r.le32(pos) != ds64_id) return {};

		const auto size = r.le32(pos + 4);
		if (size < 24 || !r.contains(pos + 8, 24)) return {};

		sizes_64 s;
		s.riff = std::min(r.le64(pos + 8), max_size_64);
		s.data = std::min(r.le64(pos + 16), max_size_64);
		s.samples = r.le64(pos + 24);

		if (size >= 28 && r.contains(pos + 32, 4))
		{
			constexpr std::uint32_t max_entries = 64;
			const auto stated = r.le32(pos + 32);
			const auto room = (std::min<std::uint64_t>(size, r.size() - pos - 8) - 28) / 12;
			const auto count = std::min<std::uint64_t>({stated, room, max_entries});

			for (std::uint64_t i = 0; i < count; ++i)
			{
				const auto at = pos + 36 + i * 12;
				s.table.emplace_back(r.le32(at), std::min(r.le64(at + 4), max_size_64));
			}
		}

		return s;
	}

	inline std::uint64_t chunk_size(const std::uint32_t id, const std::uint32_t size, const sizes_64* const sizes)
	{
		if (!sizes || size != size_in_ds64) return size;
		if (id == data_id) return sizes->data;

		for (const auto& [table_id, table_size] : sizes->table)
		{
			if (table_id == id) return table_size;
		}

		return size;
	}

	// True when the chunk's payload lies wholly before end. Its pad byte need not.
	inline bool fits(const chunk& c, const std::uint64_t end)
	{
		return c.payload() <= end && c.size <= end - c.payload();
	}

	// Calls visit with each chunk in [pos, end), in order, until it returns false. A chunk that runs past
	// end, or past the end of the file, is visited too, as the last.
	template <typename Visit>
	walk_end for_each_chunk(reader& r, std::uint64_t pos, std::uint64_t end, const sizes_64* const sizes, Visit&& visit)
	{
		end = std::min(end, r.size());

		while (end > pos && end - pos >= 8)
		{
			chunk c;
			c.start = pos;
			c.id = r.le32(pos);
			c.size = chunk_size(c.id, r.le32(pos + 4), sizes);
			if ((c.id == list_id || c.id == riff_id) && c.size >= 4 && end - pos >= 12) c.type = r.le32(pos + 8);

			const auto whole = fits(c, end);
			if (!visit(c)) return walk_end::stopped;
			if (!whole) return walk_end::cut;
			pos = c.end();
		}

		return walk_end::whole;
	}

	// The chunks in [pos, end) as the writer takes them, refused when one runs past end.
	inline std::vector<chunk> parse_children(reader& r, const std::uint64_t pos, std::uint64_t end)
	{
		end = std::min(end, r.size());
		std::vector<chunk> out;
		const auto ended = for_each_chunk(r, pos, end, nullptr, [&out, end](const chunk& c)
		{
			if (fits(c, end)) out.push_back(c);
			return true;
		});

		if (ended == walk_end::cut) throw format_error("a RIFF chunk runs past the end of its form");
		return out;
	}

	// The forms of a WAV or AVI file - the first, and the AVIX forms an AVI continues in - and the chunks
	// at the top of each, in file order. visit(form, chunk, end) sees each chunk as it is read, with the
	// index of its form and where that form's chunks end inside the file, so a caller can read inside it
	// before the walk moves on; when it returns false the walk ends there.
	template <typename Visit>
	layout walk_layout(reader& r, Visit&& visit)
	{
		layout l;
		const auto size = r.size();
		std::uint64_t pos = 0;

		const auto fault = [&l](const char* const why)
		{
			if (!l.fault) l.fault = why;
		};

		while (pos <= size && size - pos >= 12)
		{
			form f;
			f.header.start = pos;
			f.header.id = r.le32(pos);
			f.header.type = r.le32(pos + 8);
			std::uint64_t form_size = r.le32(pos + 4);

			if (l.forms.empty())
			{
				if (f.header.id == rf64_id || f.header.id == bw64_id)
				{
					// The writer keeps to 32-bit sizes.
					fault("the file is not a RIFF file");
					l.sizes = read_ds64(r, pos + 12);
					if (l.sizes && form_size == size_in_ds64) form_size = l.sizes->riff;
				}
				else if (f.header.id != riff_id)
				{
					break;
				}

				if (f.header.type != wave_type && f.header.type != avi_type)
				{
					fault("the RIFF file is neither WAV nor AVI");
					return l;
				}

				l.avi = f.header.type == avi_type;
			}
			else if (f.header.id != riff_id || !l.avi || f.header.type != avix_type)
			{
				break;
			}

			f.header.size = form_size;
			f.end = f.data_end();
			const auto cut = form_size < 4 || form_size > size - pos - 8;

			if (cut)
			{
				fault("a RIFF form runs past the end of the file");
				f.end = size;
			}

			const auto end = f.end;
			const auto index = l.forms.size();
			const auto* const sizes = index == 0 && l.sizes ? &*l.sizes : nullptr;
			const auto ended = for_each_chunk(r, pos + 12, end, sizes, [&](const chunk& c)
			{
				f.children.push_back(c);
				return visit(index, c, end);
			});

			if (ended == walk_end::cut) fault("a RIFF chunk runs past the end of its form");
			l.forms.push_back(std::move(f));
			if (cut || ended != walk_end::whole) break;

			pos = end + (form_size & 1);
		}

		if (l.forms.empty())
		{
			fault("the file is not a RIFF file");
			return l;
		}

		for (const auto& c : l.first().children)
		{
			if (c.id == list_id && c.type == movi_type)
			{
				l.movi_end = c.end();
				break;
			}
		}

		if (l.avi && l.movi_end == 0) fault("the AVI has no movie data");
		return l;
	}

	// The layout the writer takes: walk_layout's, refused with format_error when the file is not whole.
	inline layout read_layout(reader& r)
	{
		auto l = walk_layout(r, [](std::size_t, const chunk&, std::uint64_t) { return true; });
		if (l.fault) throw format_error(l.fault);
		return l;
	}

	// The packet chunk the scan and read_packet read. An AVI's is the first `_PMX` after `movi` in its first form,
	// where the writer keeps it; otherwise, and in a WAV, it is the last there is, as the toolkit reads it.
	inline const chunk* current_packet(const layout& l)
	{
		const chunk* last = nullptr;

		for (const auto& c : l.first().children)
		{
			if (c.id != pmx_id) continue;
			if (l.avi && c.start >= l.movi_end) return &c;
			last = &c;
		}

		if (l.avi)
		{
			for (std::size_t i = 1; i < l.forms.size(); ++i)
			{
				for (const auto& c : l.forms[i].children) if (c.id == pmx_id) last = &c;
			}
		}

		return last;
	}

	// The packet read_packet gives a whole file: the payload of the chunk current_packet names, without
	// the NULs that may end it. Empty when there is none.
	inline std::vector<std::uint8_t> read_current_packet(reader& r, const layout& l)
	{
		std::vector<std::uint8_t> packet;
		const auto* const c = current_packet(l);
		if (!c) return packet;

		packet = r.read(c->payload(), static_cast<std::size_t>(c->size));
		while (!packet.empty() && packet.back() == 0) packet.pop_back();
		return packet;
	}
}
