// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The GIF and BMP scanners. GIF's logical screen gives the dimensions, and its block chain
// is walked for the "XMP DataXMP" application extension. BMP has only its info header to read.

#include "scan_internal.h"

#include <cstring>
#include <format>
#include <cstdlib>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::size_t gif_header_len = 6;
		constexpr std::size_t gif_app_id_len = 11;

		// A GIF XMP extension stores the packet raw, followed by a 258-byte magic trailer that begins
		// 0x01 0xFF. The search for that trailer is bounded so a packet without one cannot walk the
		// whole file a byte at a time.
		constexpr std::uint64_t max_gif_xmp_bytes = 8ull * 1024ull * 1024ull;

		constexpr std::uint8_t gif_image_descriptor = 0x2C;
		constexpr std::uint8_t gif_extension = 0x21;
		constexpr std::uint8_t gif_trailer = 0x3B;

		// Walks the GIF block chain looking for the XMP extension. Every read is guarded against the
		// end of the file, and the caller treats a throw from here as "no XMP" rather than a failed
		// scan: the dimensions are already read by then, and a truncated GIF must not lose them.
		void scan_gif_blocks(reader& r, scan_result& result, std::uint64_t offset)
		{
			const auto size = r.size();

			while (offset + 1 < size)
			{
				const auto block_type = r.u8(offset);
				offset += 1;

				if (block_type == gif_image_descriptor)
				{
					constexpr std::size_t image_desc_len = 9;

					if (offset + image_desc_len > size) break;

					// Left, top, width and height, two bytes each, then the packed fields.
					const auto fields = r.u8(offset + 8);
					offset += image_desc_len;

					// Skip the local colour table.
					if (fields & 0x80)
					{
						offset += (std::uint64_t{1} << ((fields & 0x07) + 1)) * 3u;
					}

					// The LZW minimum code size.
					offset += 1;

					if (offset >= size) break;

					auto sub_block_size = r.u8(offset);
					offset += 1;

					// Strictly less than: the loop reads at offset after advancing, so the last
					// sub-block must leave the following size byte inside the file.
					while (sub_block_size != 0 && offset + sub_block_size < size)
					{
						offset += sub_block_size;
						sub_block_size = r.u8(offset);
						offset += 1;
					}
				}
				else if (block_type == gif_extension)
				{
					if (offset + 2u > size) break;

					const auto label = r.u8(offset);
					auto sub_block_size = r.u8(offset + 1);
					auto skip_block = true;

					offset += 2;

					if (label == 0xFF && sub_block_size == gif_app_id_len && offset + gif_app_id_len < size &&
						std::memcmp(r.view(offset, gif_app_id_len).data(), "XMP DataXMP", gif_app_id_len) == 0)
					{
						offset += gif_app_id_len;
						const auto first_byte = r.u8(offset);

						if (first_byte == '<')
						{
							const auto start = offset;
							const auto search_end = std::min(size, start + max_gif_xmp_bytes);

							while (offset < search_end && r.u8(offset) != 0xff)
							{
								offset += 1;
							}

							if (offset < search_end)
							{
								const auto packet = r.read(start, static_cast<std::size_t>(offset - start - 1));
								const std::string_view text(reinterpret_cast<const char*>(packet.data()), packet.size());
								constexpr std::string_view end_tag = "</x:xmpmeta>";

								// Trim anything after the closing tag. find() cannot report a partial match,
								// so a payload shorter than the tag keeps its full length.
								const auto pos = text.find(end_tag);
								const auto xmp_size = pos == std::string_view::npos ? packet.size() : pos + end_tag.size();

								result.metadata.xmp.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(xmp_size));
							}
						}
						else
						{
							sub_block_size = first_byte;
							offset += 1;

							// The packet may be split over several sub-blocks, so they are joined.
							result.metadata.xmp.clear();

							while (sub_block_size != 0 && offset + sub_block_size < size)
							{
								// A packet past the ceiling is dropped rather than cut short: half an XMP
								// packet parses to nothing.
								if (result.metadata.xmp.size() + sub_block_size > max_gif_xmp_bytes)
								{
									result.metadata.xmp.clear();
									break;
								}

								const auto block = r.view(offset, sub_block_size);
								result.metadata.xmp.insert(result.metadata.xmp.end(), block.begin(), block.end());
								offset += sub_block_size;
								sub_block_size = r.u8(offset);
								offset += 1;
							}
						}

						skip_block = false;
					}

					if (skip_block)
					{
						while (sub_block_size != 0 && offset + sub_block_size < size)
						{
							offset += sub_block_size;
							sub_block_size = r.u8(offset);
							offset += 1;
						}
					}
				}
				else
				{
					// The trailer, or a block that is not GIF.
					break;
				}
			}
		}
	}

	scan_result scan_gif(reader& r)
	{
		scan_result result;

		constexpr std::size_t header_len = 11;
		const auto header = r.view(0, header_len);

		if (std::memcmp(header.data(), "GIF87a", gif_header_len) != 0 &&
			std::memcmp(header.data(), "GIF89a", gif_header_len) != 0)
		{
			throw format_error("no gif header");
		}

		result.width = load_le16(header.data() + 6);
		result.height = load_le16(header.data() + 8);
		result.pixel_format = "pal8";

		// The logical screen descriptor: width, height, packed fields, background colour and aspect
		// ratio, then the global colour table if the packed fields declare one.
		const auto fields = header[10];
		const auto table_size = (fields & 0x80) ? (1u << ((fields & 0x07) + 1)) * 3u : 0u;
		const auto offset = gif_header_len + 7u + table_size;

		try
		{
			scan_gif_blocks(r, result, offset);
		}
		catch (const std::exception&)
		{
			// The XMP is optional; the dimensions are not, and they are already read.
		}

		// A truncated or corrupt GIF must not read as a successful 0x0 image.
		result.success = result.width != 0 && result.height != 0;
		return result;
	}

	scan_result scan_bmp(reader& r)
	{
		scan_result result;

		constexpr std::uint64_t file_header_len = 14;
		constexpr std::uint64_t info_header_len = 40;

		// The signature guarantees only "BM", so the file must be long enough for the info header.
		if (r.size() < file_header_len + info_header_len) return result;

		const auto* const info = r.view(file_header_len, info_header_len).data();
		const auto header_size = load_le32(info);

		// The dimensions are signed, and a negative height means a top-down bitmap. The magnitude is
		// taken in 64 bits: the magnitude of INT32_MIN does not fit 32.
		const auto width = std::llabs(static_cast<std::int32_t>(load_le32(info + 4)));
		const auto height = std::llabs(static_cast<std::int32_t>(load_le32(info + 8)));
		const auto bit_count = load_le16(info + 14);

		if (header_size < info_header_len || width <= 0 || height <= 0 ||
			width > max_image_dimension || height > max_image_dimension)
		{
			return result;
		}

		result.width = static_cast<std::uint32_t>(width);
		result.height = static_cast<std::uint32_t>(height);
		result.pixel_format = std::format("RGB{}", bit_count);
		result.success = true;
		return result;
	}
}
