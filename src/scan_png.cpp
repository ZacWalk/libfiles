// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The PNG scanner: one walk over the chunks. IHDR gives the dimensions and pixel format;
// iCCP, eXIf and the "XML:com.adobe.xmp" text chunk give the metadata. Metadata may follow the image
// data, so the walk continues to IEND - over chunk headers only, never the image data itself.
//
// The rules are those of the reader this replaced, which was two: libpng's header read, up to the
// first IDAT, and a walk of every chunk for metadata. What makes a file unreadable is what made
// libpng 1.6.59 fail - a bad signature or IHDR, an unknown critical chunk, a misplaced IEND, or a
// palette image without a valid PLTE. The colour profile is libpng's. The XMP and Exif are the walk's:
// the last valid chunk of each, with a damaged one ending the search. Where the walk ended before
// finding one, the chunk libpng accepted before the first IDAT stands in.

#include "scan_internal.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint8_t png_signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		constexpr std::string_view png_xmp_key = "XML:com.adobe.xmp";

		// What a metadata chunk, and what it inflates to, may hold before the walk refuses it.
		constexpr std::size_t max_metadata_chunk = 16u * 1024u * 1024u;
		constexpr std::size_t max_decompressed_text = 16u * 1024u * 1024u;

		// libpng's limits: an image edge, and its allocation limit, which bounds profiles, eXIf and
		// inflated text.
		constexpr std::uint32_t max_png_dimension = 1000000u;
		constexpr std::size_t png_chunk_max = 8000000u;
		constexpr std::uint32_t max_png_uint31 = 0x7fffffffu;

		// The smallest zlib stream: header, an empty stored block, and the Adler-32.
		constexpr std::size_t lz77_min = 2u + 5u + 4u;

		constexpr std::uint32_t chunk_type(const char (&name)[5])
		{
			return static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[0])) << 24 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[1])) << 16 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[2])) << 8 |
				static_cast<std::uint8_t>(name[3]);
		}

		constexpr auto IHDR = chunk_type("IHDR");
		constexpr auto PLTE = chunk_type("PLTE");
		constexpr auto IDAT = chunk_type("IDAT");
		constexpr auto IEND = chunk_type("IEND");
		constexpr auto iCCP = chunk_type("iCCP");
		constexpr auto eXIf = chunk_type("eXIf");
		constexpr auto tEXt = chunk_type("tEXt");
		constexpr auto zTXt = chunk_type("zTXt");
		constexpr auto iTXt = chunk_type("iTXt");

		bool is_critical(const std::uint32_t type)
		{
			return (type & 0x20000000u) == 0;
		}

		bool is_valid_type(const std::uint32_t type)
		{
			for (auto shift = 0; shift < 32; shift += 8)
			{
				const auto c = static_cast<std::uint8_t>(type >> shift);
				if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
			}

			return true;
		}

		bool crc_matches(reader& r, const std::uint64_t type_pos, const std::uint32_t length)
		{
			const auto checked = r.view(type_pos, static_cast<std::size_t>(length) + 8u);
			const auto expected = load_be32(checked.data() + 4u + length);
			return crc32(0, checked.first(4u + length)) == expected;
		}

		bool is_tiff_header(const std::span<const std::uint8_t> data)
		{
			if (data.size() < 4) return false;
			const auto header = load_be32(data.data());
			return header == 0x49492A00 || header == 0x4D4D002A;
		}

		// The walk takes an eXIf with or without the JPEG "Exif\0\0" signature some writers put first.
		std::span<const std::uint8_t> walk_exif_payload(std::span<const std::uint8_t> data)
		{
			if (starts_with(data, {"Exif\0\0", 6})) data = data.subspan(6);
			return is_tiff_header(data) ? data : std::span<const std::uint8_t>{};
		}

		std::span<const std::uint8_t> after(const std::span<const std::uint8_t> data,
		                                    const std::span<const std::uint8_t>::iterator at)
		{
			return data.subspan(static_cast<std::size_t>(at - data.begin()));
		}

		bool has_xmp_key(const std::span<const std::uint8_t> data, std::span<const std::uint8_t>::iterator& key_end)
		{
			key_end = std::find(data.begin(), data.end(), std::uint8_t{0});
			return key_end != data.end() &&
				std::string_view(reinterpret_cast<const char*>(data.data()), static_cast<std::size_t>(key_end - data.begin())) ==
				png_xmp_key;
		}

		// The walk's reading of an XMP text chunk. A packet that fails to inflate reads as empty, and
		// replaces any earlier one. Nothing is returned for a chunk that is not XMP or is malformed.
		std::optional<std::vector<std::uint8_t>> walk_text_chunk(const std::uint32_t type,
		                                                         const std::span<const std::uint8_t> data)
		{
			std::span<const std::uint8_t>::iterator key_end;
			if (!has_xmp_key(data, key_end)) return {};

			const auto rest = after(data, key_end);
			std::vector<std::uint8_t> text;

			if (type == tEXt)
			{
				return std::vector<std::uint8_t>(rest.begin() + 1, rest.end());
			}

			if (type == zTXt)
			{
				if (rest.size() < 2 || rest[1] != 0) return {};
				if (!zlib_inflate(rest.subspan(2), text, max_decompressed_text)) text.clear();
				return text;
			}

			if (rest.size() < 3) return {};

			const auto compressed = rest[1] != 0;
			if (rest[2] != 0) return {};

			const auto language_end = std::find(rest.begin() + 3, rest.end(), std::uint8_t{0});
			if (language_end == rest.end()) return {};

			const auto translated_end = std::find(language_end + 1, rest.end(), std::uint8_t{0});
			if (translated_end == rest.end()) return {};

			const auto body = after(rest, translated_end + 1);

			if (compressed && !zlib_inflate(body, text, max_decompressed_text)) text.clear();
			if (!compressed) text.assign(body.begin(), body.end());
			return text;
		}

		// libpng's reading of the same chunk, before the first IDAT. It keeps only what it could read
		// whole: a compressed packet that fails to inflate is no packet at all.
		std::optional<std::vector<std::uint8_t>> libpng_text_chunk(const std::uint32_t type,
		                                                           const std::span<const std::uint8_t> data)
		{
			std::span<const std::uint8_t>::iterator key_end;
			if (!has_xmp_key(data, key_end)) return {};

			const auto rest = after(data, key_end);
			std::vector<std::uint8_t> text;

			if (type == tEXt)
			{
				return std::vector<std::uint8_t>(rest.begin() + 1, rest.end());
			}

			if (type == zTXt)
			{
				if (rest.size() < 3 || rest[1] != 0) return {};
				if (!zlib_inflate(rest.subspan(2), text, png_chunk_max)) return {};
				return text;
			}

			// iTXt: compression flag 0, or 1 with method 0, then the language and translated keyword.
			if (rest.size() < 5) return {};

			const auto flag = rest[1];
			if (flag != 0 && (flag != 1 || rest[2] != 0)) return {};

			const auto language_end = std::find(rest.begin() + 3, rest.end(), std::uint8_t{0});
			if (language_end == rest.end()) return {};

			const auto translated_end = std::find(language_end + 1, rest.end(), std::uint8_t{0});
			if (translated_end == rest.end()) return {};

			const auto body = after(rest, translated_end + 1);

			if (flag == 0) return std::vector<std::uint8_t>(body.begin(), body.end());
			if (body.empty() || !zlib_inflate(body, text, png_chunk_max)) return {};
			return text;
		}

		std::uint32_t icc_field(const std::vector<std::uint8_t>& profile, const std::size_t offset)
		{
			return load_be32(profile.data() + offset);
		}

		// The colour profile in an iCCP chunk, as libpng 1.6.59 accepts one: the declared number of
		// bytes inflates, the header is a well-formed ICC header whose colour space suits the image,
		// and every tag lies inside the profile. libpng examines nothing past the profile - not the
		// rest of the stream, nor the chunk's CRC.
		std::vector<std::uint8_t> read_icc_profile(const std::span<const std::uint8_t> data, const std::uint8_t color_type)
		{
			// The keyword is read from the first 81 bytes, and at least a minimal zlib stream must follow those.
			const auto read_length = std::min<std::size_t>(81u, data.size());
			if (data.size() - read_length < lz77_min) return {};

			std::size_t key_len = 0;
			while (key_len < 80 && key_len < read_length && data[key_len] != 0) ++key_len;

			if (key_len < 1 || key_len > 79) return {};
			if (key_len + 1 >= read_length || data[key_len + 1] != 0) return {};

			const auto stream = data.subspan(key_len + 2);
			constexpr std::size_t header_size = 132;

			std::vector<std::uint8_t> profile;
			if (!zlib_inflate_prefix(stream, profile, header_size)) return {};

			const auto profile_length = icc_field(profile, 0);

			if (profile_length < header_size || profile_length > png_chunk_max) return {};
			if (profile[8] > 3 && (profile_length & 3u) != 0) return {}; // version 4 profiles are padded

			const auto tag_count = icc_field(profile, 128);
			if (tag_count > 357913930u || profile_length < header_size + 12u * tag_count) return {};

			if (icc_field(profile, 64) >= 0xffff) return {}; // rendering intent
			if (icc_field(profile, 36) != 0x61637370) return {}; // 'acsp'

			const auto color_space = icc_field(profile, 16);
			const auto has_color = (color_type & 2) != 0;

			if (has_color && color_space != 0x52474220) return {}; // 'RGB '
			if (!has_color && color_space != 0x47524159) return {}; // 'GRAY'

			const auto device_class = icc_field(profile, 12);
			if (device_class == 0x61627374 || device_class == 0x6c696e6b) return {}; // 'abst', 'link'

			const auto pcs = icc_field(profile, 20);
			if (pcs != 0x58595a20 && pcs != 0x4c616220) return {}; // 'XYZ ', 'Lab '

			if (!zlib_inflate_prefix(stream, profile, profile_length)) return {};

			for (std::uint32_t i = 0; i < tag_count; ++i)
			{
				const auto entry = header_size + 12u * i;
				const auto tag_offset = icc_field(profile, entry + 4);
				const auto tag_length = icc_field(profile, entry + 8);

				if (tag_offset > profile_length || tag_length > profile_length - tag_offset) return {};
			}

			return profile;
		}

		bool is_valid_ihdr(const std::uint8_t* p)
		{
			const auto width = load_be32(p);
			const auto height = load_be32(p + 4);
			const auto bit_depth = p[8];
			const auto color_type = p[9];

			if (width == 0 || height == 0) return false;
			if (width > max_png_uint31 || height > max_png_uint31) return false;
			if (width > max_png_dimension || height > max_png_dimension) return false;
			if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8 && bit_depth != 16) return false;
			if (color_type == 1 || color_type == 5 || color_type > 6) return false;
			if (color_type == 3 && bit_depth > 8) return false;
			if ((color_type == 2 || color_type == 4 || color_type == 6) && bit_depth < 8) return false;
			if (p[10] != 0 || p[11] != 0 || p[12] > 1) return false; // compression, filter, interlace

			return true;
		}
	}

	scan_result scan_png(reader& r)
	{
		scan_result result;

		if (r.size() < sizeof(png_signature) ||
			std::memcmp(r.view(0, sizeof(png_signature)).data(), png_signature, sizeof(png_signature)) != 0)
		{
			throw format_error("invalid png header");
		}

		const auto file_size = r.size();
		std::uint64_t pos = sizeof(png_signature);

		auto seen_ihdr = false;
		auto seen_plte = false;
		auto seen_idat = false;
		auto have_profile = false;
		std::uint8_t color_type = 0;

		// The walk's finds, and libpng's, kept apart because the walk's take precedence.
		auto searching = true;
		std::optional<std::vector<std::uint8_t>> walk_exif;
		std::optional<std::vector<std::uint8_t>> walk_xmp;
		std::optional<std::vector<std::uint8_t>> libpng_exif;
		std::optional<std::vector<std::uint8_t>> libpng_xmp;

		for (;;)
		{
			// Before the first IDAT the header read must succeed, so running out of file is a fault -
			// though the IDAT chunk needs only its header, which is where libpng's read stops. After
			// that only the walk is left, and it simply stops.
			if (file_size - pos < (seen_idat ? 12u : 8u))
			{
				if (!seen_idat) throw format_error("png ends before its image data");
				break;
			}

			const auto length = r.be32(pos);
			const auto type = r.be32(pos + 4);
			const auto data_pos = pos + 8u;
			const auto chunk_fits = length <= file_size - data_pos && file_size - data_pos - length >= 4u;

			if (!seen_idat)
			{
				if (length > max_png_uint31) throw format_error("png chunk length out of range");
				if (!is_valid_type(type)) throw format_error("invalid png chunk type");
				if (!seen_ihdr && type != IHDR) throw format_error("png is missing IHDR");

				// Every chunk before the image data is read whole, its CRC included.
				if (type != IDAT && !chunk_fits) throw format_error("png chunk runs past the end of the file");

				const auto palette = color_type == 3;

				if (type == IHDR)
				{
					if (seen_ihdr || length != 13u || !crc_matches(r, pos + 4, length)) throw format_error("invalid png IHDR");

					const auto* const ihdr = r.view(data_pos, 13).data();
					if (!is_valid_ihdr(ihdr)) throw format_error("invalid png IHDR");

					seen_ihdr = true;
					result.width = load_be32(ihdr);
					result.height = load_be32(ihdr + 4);
					color_type = ihdr[9];

					// The depth is a sample's, or a palette index's.
					result.picture.chroma = (color_type & 2) != 0 ? chroma_format::rgb : chroma_format::gray;
					result.picture.bit_depth = ihdr[8];
					result.picture.alpha = (color_type & 4) != 0;
					result.picture.palette = color_type == 3;

					switch (color_type)
					{
					case 0: result.pixel_format = "gray";
						break;
					case 3: result.pixel_format = "palette";
						break;
					case 2: result.pixel_format = "rgb";
						break;
					case 6: result.pixel_format = "rgba";
						break;
					default:
						break;
					}
				}
				else if (type == PLTE)
				{
					// A palette is fatal to get wrong only in an image that needs one. Elsewhere a bad
					// one is ignored, and a damaged CRC is only a warning.
					if (seen_plte || length > 768u || length % 3u != 0)
					{
						if (palette) throw format_error("invalid png PLTE");
					}
					else if ((color_type & 2) != 0)
					{
						if (palette && !crc_matches(r, pos + 4, length)) throw format_error("damaged png PLTE");
						seen_plte = true;
					}
				}
				else if (type == IDAT)
				{
					if (palette && !seen_plte) throw format_error("png palette image has no PLTE");
					seen_idat = true;
					result.success = true;
				}
				else if (type == IEND)
				{
					throw format_error("png has no image data");
				}
				else if (is_critical(type))
				{
					throw format_error("unknown critical png chunk");
				}
				else if (type == iCCP)
				{
					// One profile, which must precede PLTE. A profile libpng refuses leaves room for a
					// later one.
					if (!have_profile && !seen_plte && length >= 3u + lz77_min)
					{
						auto profile = read_icc_profile(r.view(data_pos, length), color_type);

						if (!profile.empty())
						{
							result.metadata.icc = std::move(profile);
							have_profile = true;
						}
					}
				}
				else if (type == eXIf)
				{
					if (!libpng_exif && length >= 4u && length <= png_chunk_max && crc_matches(r, pos + 4, length))
					{
						const auto data = r.view(data_pos, length);
						if (is_tiff_header(data)) libpng_exif.emplace(data.begin(), data.end());
					}
				}
				else if (type == tEXt || type == zTXt || type == iTXt)
				{
					if (crc_matches(r, pos + 4, length))
					{
						auto text = libpng_text_chunk(type, r.view(data_pos, length));
						if (text) libpng_xmp = std::move(text);
					}
				}
			}

			// The metadata walk reads whole chunks, and stops at the first that runs past the end.
			if (file_size - pos < 12u || !chunk_fits) break;

			if (searching && (type == eXIf || type == tEXt || type == zTXt || type == iTXt))
			{
				// A metadata chunk that is oversized or damaged ends the walk's search.
				if (length > max_metadata_chunk || !crc_matches(r, pos + 4, length))
				{
					searching = false;
				}
				else if (type == eXIf)
				{
					const auto exif = walk_exif_payload(r.view(data_pos, length));
					if (!exif.empty()) walk_exif.emplace(exif.begin(), exif.end());
				}
				else
				{
					auto text = walk_text_chunk(type, r.view(data_pos, length));
					if (text) walk_xmp = std::move(text);
				}
			}

			pos = data_pos + length + 4u;
			if (type == IEND || (seen_idat && !searching)) break;
		}

		if (walk_exif) result.metadata.exif = std::move(*walk_exif);
		else if (libpng_exif) result.metadata.exif = std::move(*libpng_exif);

		if (walk_xmp) result.metadata.xmp = std::move(*walk_xmp);
		else if (libpng_xmp) result.metadata.xmp = std::move(*libpng_xmp);

		return result;
	}
}
