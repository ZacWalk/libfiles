// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing XMP to a GIF. The packet is the "XMP DataXMP" application extension of the XMP
// specification: the raw packet, then a 258-byte "magic trailer" that lets a GIF reader, which takes
// the packet's bytes for sub-block sizes, step over it. A packet that fits the old one's exact length
// is patched in place; a new one goes before the GIF trailer. GIF87a cannot carry an extension, so
// it is refused, as the toolkit refused it.

#include "write_internal.h"

#include <cstring>
#include <optional>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint8_t image_descriptor = 0x2C;
		constexpr std::uint8_t extension = 0x21;
		constexpr std::uint8_t trailer = 0x3B;
		constexpr std::string_view xmp_application_id = "XMP DataXMP";
		constexpr std::uint64_t magic_trailer_length = 258;

		struct byte_span
		{
			std::uint64_t offset = 0;
			std::uint64_t length = 0;
		};

		// Steps over a chain of sub-blocks, starting at the first size byte.
		void skip_sub_blocks(reader& r, std::uint64_t& pos)
		{
			for (;;)
			{
				const auto n = r.u8(pos);
				pos += 1;
				if (n == 0) return;
				pos += n;
			}
		}

		void append_magic_trailer(std::vector<std::uint8_t>& to)
		{
			to.push_back(0x01);
			for (auto b = 0xFF; b > 0; --b) to.push_back(static_cast<std::uint8_t>(b));
			to.push_back(0x00);
			to.push_back(0x00);
		}
	}

	void plan_gif(reader& r, const metadata_update& update, write_plan& plan)
	{
		if (!update.xmp) return;

		if (std::memcmp(r.view(0, 6).data(), "GIF89a", 6) != 0) throw format_error("XMP can be stored only in a GIF89a file");

		const auto fields = r.u8(10);
		const auto global_table = (fields & 0x80) ? 3ull << ((fields & 0x07) + 1) : 0ull;
		std::uint64_t pos = 13 + global_table;

		std::optional<byte_span> packet;
		std::optional<std::uint64_t> trailer_offset;

		while (pos < r.size())
		{
			const auto block = r.u8(pos);
			pos += 1;

			if (block == image_descriptor)
			{
				const auto packed = r.u8(pos + 8);
				pos += 9;
				if (packed & 0x80) pos += 3ull << ((packed & 0x07) + 1);
				pos += 1; // the LZW minimum code size
				skip_sub_blocks(r, pos);
			}
			else if (block == extension)
			{
				const auto label = r.u8(pos);
				const auto block_size = r.u8(pos + 1);
				pos += 2;

				if (label == 0xFF && block_size == xmp_application_id.size())
				{
					const auto is_xmp = std::memcmp(r.view(pos, xmp_application_id.size()).data(), xmp_application_id.data(),
					                                xmp_application_id.size()) == 0;
					pos += xmp_application_id.size();
					const auto start = pos;
					skip_sub_blocks(r, pos);

					// As the toolkit reads it: the last XMP extension is the packet.
					if (is_xmp)
					{
						if (pos - start < magic_trailer_length) throw format_error("a GIF XMP extension is damaged");
						packet = byte_span{start, pos - start - magic_trailer_length};
					}
				}
				else
				{
					pos += block_size;
					skip_sub_blocks(r, pos);
				}
			}
			else if (block == trailer)
			{
				trailer_offset = pos - 1;
				break;
			}
			else
			{
				throw format_error("the GIF holds a block that is not GIF");
			}
		}

		if (!trailer_offset) throw format_error("the GIF is cut short");

		if (packet)
		{
			const auto updated = to_bytes(serialize_packet(*update.xmp, packet->length));
			if (updated.size() == packet->length) push_changed(r, plan, packet->offset, updated);
			else plan.replacements.push_back({packet->offset, packet->length, updated});
			return;
		}

		std::vector<std::uint8_t> bytes = {extension, 0xFF, static_cast<std::uint8_t>(xmp_application_id.size())};
		append(bytes, xmp_application_id);
		append(bytes, serialize_packet(*update.xmp, 0));
		append_magic_trailer(bytes);
		plan.replacements.push_back({*trailer_offset, 0, std::move(bytes)});
	}
}
