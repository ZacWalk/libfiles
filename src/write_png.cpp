// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing XMP to a PNG. The packet lives in an uncompressed iTXt chunk keyed
// "XML:com.adobe.xmp" with no language, as the XMP specification and the toolkit write it. A packet
// that fits the one such chunk is patched in place with its CRC. Otherwise every XMP text chunk, in
// whatever form, is removed and one new chunk follows IHDR: a reader that took a later chunk would
// otherwise never see the edit.

#include "write_internal.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint8_t png_signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

		// The keyword, then no compression, no method, no language and no translated keyword.
		constexpr std::string_view itxt_header{"XML:com.adobe.xmp\0\0\0\0\0", 22};
		constexpr std::string_view xmp_key = "XML:com.adobe.xmp";

		constexpr std::uint32_t IHDR = 0x49484452;
		constexpr std::uint32_t IEND = 0x49454E44;
		constexpr std::uint32_t tEXt = 0x74455874;
		constexpr std::uint32_t zTXt = 0x7A545874;
		constexpr std::uint32_t iTXt = 0x69545874;

		struct chunk
		{
			std::uint64_t start = 0;
			std::uint32_t length = 0;

			std::uint64_t size() const { return 12ull + length; }
		};

		bool is_xmp_text_chunk(reader& r, const chunk& c, const std::uint32_t type)
		{
			if (type != tEXt && type != zTXt && type != iTXt) return false;
			if (c.length < xmp_key.size() + 1) return false;

			const auto key = r.view(c.start + 8, xmp_key.size() + 1);
			return std::memcmp(key.data(), xmp_key.data(), xmp_key.size()) == 0 && key[xmp_key.size()] == 0;
		}

		bool is_toolkit_form(reader& r, const chunk& c, const std::uint32_t type)
		{
			if (type != iTXt || c.length < itxt_header.size()) return false;
			return std::memcmp(r.view(c.start + 8, itxt_header.size()).data(), itxt_header.data(), itxt_header.size()) == 0;
		}

		std::uint32_t chunk_crc(const std::string_view type, const std::vector<std::uint8_t>& data)
		{
			const auto crc = crc32(0, {reinterpret_cast<const std::uint8_t*>(type.data()), type.size()});
			return crc32(crc, data);
		}
	}

	void plan_png(reader& r, const metadata_update& update, write_plan& plan)
	{
		if (!update.xmp) return;

		const auto size = r.size();

		if (size < sizeof(png_signature) ||
			std::memcmp(r.view(0, sizeof(png_signature)).data(), png_signature, sizeof(png_signature)) != 0)
		{
			throw format_error("not a PNG file");
		}

		std::uint64_t pos = sizeof(png_signature);
		std::optional<std::uint64_t> ihdr_end;
		std::vector<chunk> xmp_chunks;
		auto toolkit_form = false;
		auto ended = false;

		while (size - pos >= 12u)
		{
			const chunk c{pos, r.be32(pos)};
			const auto type = r.be32(pos + 4);

			if (c.length > 0x7fffffffu || c.length > size - pos - 12u) throw format_error("a PNG chunk runs past the end of the file");

			if (!ihdr_end)
			{
				if (type != IHDR) throw format_error("the PNG does not start with IHDR");
				ihdr_end = pos + c.size();
			}

			if (is_xmp_text_chunk(r, c, type))
			{
				xmp_chunks.push_back(c);
				toolkit_form = is_toolkit_form(r, c, type);
			}

			pos += c.size();

			if (type == IEND)
			{
				ended = true;
				break;
			}
		}

		// A damaged file is not made worse by an edit: whatever follows the damage is unknown.
		if (!ended) throw format_error("the PNG is cut short");

		if (xmp_chunks.size() == 1 && toolkit_form)
		{
			const auto& old = xmp_chunks.front();
			const auto old_length = old.length - itxt_header.size();
			const auto packet = serialize_packet(*update.xmp, old_length);

			if (packet.size() == old_length)
			{
				auto data = to_bytes(itxt_header);
				append(data, packet);

				// The packet and the CRC after it, as one write, so the chunk never holds one without
				// the other.
				auto patch = to_bytes(packet);
				append_be32(patch, chunk_crc("iTXt", data));
				push_changed(r, plan, old.start + 8 + itxt_header.size(), patch);
				return;
			}
		}

		auto data = to_bytes(itxt_header);
		append(data, serialize_packet(*update.xmp, 0));

		std::vector<std::uint8_t> bytes;
		append_be32(bytes, static_cast<std::uint32_t>(data.size()));
		append(bytes, "iTXt");
		bytes.insert(bytes.end(), data.begin(), data.end());
		append_be32(bytes, chunk_crc("iTXt", data));

		// The new chunk follows IHDR, taking the place of an old one that did.
		auto inserted = false;

		for (const auto& old : xmp_chunks)
		{
			if (!inserted && old.start == *ihdr_end)
			{
				plan.replacements.push_back({old.start, old.size(), std::move(bytes)});
				inserted = true;
			}
			else
			{
				plan.replacements.push_back({old.start, old.size(), {}});
			}
		}

		if (!inserted) plan.replacements.push_back({*ihdr_end, 0, std::move(bytes)});
	}
}
