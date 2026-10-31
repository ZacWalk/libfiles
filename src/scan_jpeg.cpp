// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The JPEG scanner. One pass over the marker segments up to the first scan: the frame
// header for dimensions and sampling, APP0/APP14 for the colour convention, and the APP1, APP2 and
// APP13 metadata. An inspect scan also reports the tables, comments and multi-picture index, and
// what follows the end-of-image marker.

#include "scan_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <map>
#include <ranges>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint8_t M_SOF0 = 0xC0;
		constexpr std::uint8_t M_SOF1 = 0xC1;
		constexpr std::uint8_t M_SOF2 = 0xC2;
		constexpr std::uint8_t M_SOF3 = 0xC3;
		constexpr std::uint8_t M_SOF5 = 0xC5;
		constexpr std::uint8_t M_SOF6 = 0xC6;
		constexpr std::uint8_t M_SOF7 = 0xC7;
		constexpr std::uint8_t M_SOF9 = 0xC9;
		constexpr std::uint8_t M_SOF10 = 0xCA;
		constexpr std::uint8_t M_SOF11 = 0xCB;
		constexpr std::uint8_t M_SOF13 = 0xCD;
		constexpr std::uint8_t M_SOF14 = 0xCE;
		constexpr std::uint8_t M_SOF15 = 0xCF;
		constexpr std::uint8_t M_SOI = 0xD8;
		constexpr std::uint8_t M_EOI = 0xD9;
		constexpr std::uint8_t M_SOS = 0xDA;
		constexpr std::uint8_t M_DHT = 0xC4;
		constexpr std::uint8_t M_DQT = 0xDB;
		constexpr std::uint8_t M_DRI = 0xDD;
		constexpr std::uint8_t M_COM = 0xFE;
		constexpr std::uint8_t M_APP0 = 0xE0;
		constexpr std::uint8_t M_APP1 = 0xE1;
		constexpr std::uint8_t M_APP2 = 0xE2;
		constexpr std::uint8_t M_APP13 = 0xED;
		constexpr std::uint8_t M_APP14 = 0xEE;
		constexpr std::uint8_t M_APP15 = 0xEF;

		constexpr std::string_view exif_signature{"Exif\0\0", 6};
		constexpr std::string_view xmp_signature{"http://ns.adobe.com/xap/1.0/\0", 29};
		constexpr std::string_view icc_signature{"ICC_PROFILE\0", 12};
		// The signature, then the segment's sequence number and the count of segments.
		constexpr std::size_t icc_header_len = icc_signature.size() + 2;
		constexpr std::string_view photoshop_signature{"Photoshop 3.0\0", 14};

		// Zig-zag scan order: DQT stores coefficients in this sequence, the reference tables below
		// are in natural raster order.
		constexpr std::uint8_t zigzag_to_natural[64] = {
			0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
			12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
			35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
			58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
		};

		// The example tables from the JPEG specification, Annex K. Quality estimation scales these.
		constexpr std::uint16_t annex_k_luminance_quant[64] = {
			16, 11, 10, 16, 24, 40, 51, 61,
			12, 12, 14, 19, 26, 58, 60, 55,
			14, 13, 16, 24, 40, 57, 69, 56,
			14, 17, 22, 29, 51, 87, 80, 62,
			18, 22, 37, 56, 68, 109, 103, 77,
			24, 35, 55, 64, 81, 104, 113, 92,
			49, 64, 78, 87, 103, 121, 120, 101,
			72, 92, 95, 98, 112, 100, 103, 99
		};

		constexpr std::uint16_t annex_k_chrominance_quant[64] = {
			17, 18, 24, 47, 99, 99, 99, 99,
			18, 21, 26, 66, 99, 99, 99, 99,
			24, 26, 56, 99, 99, 99, 99, 99,
			47, 66, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99
		};

		// The code-length counts of the Annex K example Huffman tables. Camera firmware normally
		// writes these unchanged; software encoders usually fit tables to the image instead.
		constexpr std::uint8_t annex_k_dc_luminance_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
		constexpr std::uint8_t annex_k_dc_chrominance_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
		constexpr std::uint8_t annex_k_ac_luminance_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
		constexpr std::uint8_t annex_k_ac_chrominance_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};

		std::string_view marker_name(const std::uint8_t marker)
		{
			switch (marker)
			{
			case M_SOI: return "SOI (start of image)";
			case M_EOI: return "EOI (end of image)";
			case M_SOS: return "SOS (start of scan)";
			case M_SOF0: return "SOF0 (baseline DCT)";
			case M_SOF1: return "SOF1 (extended sequential DCT)";
			case M_SOF2: return "SOF2 (progressive DCT)";
			case M_SOF3: return "SOF3 (lossless)";
			case M_SOF5: return "SOF5 (differential sequential DCT)";
			case M_SOF6: return "SOF6 (differential progressive DCT)";
			case M_SOF7: return "SOF7 (differential lossless)";
			case M_SOF9: return "SOF9 (arithmetic sequential DCT)";
			case M_SOF10: return "SOF10 (arithmetic progressive DCT)";
			case M_SOF11: return "SOF11 (arithmetic lossless)";
			case M_SOF13: return "SOF13 (differential arithmetic sequential)";
			case M_SOF14: return "SOF14 (differential arithmetic progressive)";
			case M_SOF15: return "SOF15 (differential arithmetic lossless)";
			case 0xC4: return "DHT (Huffman tables)";
			case 0xCC: return "DAC (arithmetic conditioning)";
			case 0xDB: return "DQT (quantisation tables)";
			case 0xDC: return "DNL (define number of lines)";
			case 0xDD: return "DRI (restart interval)";
			case 0xDE: return "DHP (hierarchical progression)";
			case 0xDF: return "EXP (expand reference)";
			case 0xFE: return "COM (comment)";
			default: break;
			}

			if (marker >= M_APP0 && marker <= M_APP15) return "APP";
			return "unknown";
		}

		// The leading NUL-terminated identifier of an APPn segment, which is what tells an Exif APP1
		// from an XMP APP1 or an Adobe APP14.
		std::string app_identifier(const std::span<const std::uint8_t> data)
		{
			std::string result;

			for (std::size_t i = 0; i < data.size() && i < 32u; ++i)
			{
				if (data[i] == 0) break;
				result += (data[i] >= 0x20 && data[i] < 0x7f) ? static_cast<char>(data[i]) : '.';
			}

			return result;
		}

		// Half away from zero, and zero for anything not a normal number.
		int round_quality(const double d)
		{
			if (!std::isnormal(d)) return 0;
			return static_cast<int>(std::round(d));
		}

		// Recovers the IJG quality setting that would produce this table, by averaging the scale each
		// coefficient implies. Encoders that do not use the IJG scale still get a comparable number.
		int estimate_quality(const std::uint16_t* table, const bool chrominance)
		{
			const auto* const reference = chrominance ? annex_k_chrominance_quant : annex_k_luminance_quant;

			double total = 0.0;
			auto count = 0;

			for (auto i = 0; i < 64; ++i)
			{
				if (reference[i] == 0) continue;
				total += static_cast<double>(table[i]) * 100.0 / reference[i];
				++count;
			}

			if (count == 0) return 0;

			const auto scale = total / count;
			const auto quality = scale <= 100.0 ? (200.0 - scale) / 2.0 : 5000.0 / scale;

			return std::clamp(round_quality(quality), 1, 100);
		}

		// The sampling of a YCbCr frame, from its luma's factors.
		chroma_format ycbcr_chroma(const int luma_h, const int luma_v)
		{
			if (luma_h == 1 && luma_v == 1) return chroma_format::yuv444;
			if (luma_h == 2 && luma_v == 1) return chroma_format::yuv422;
			if (luma_h == 2 && luma_v == 2) return chroma_format::yuv420;
			if (luma_h == 1 && luma_v == 2) return chroma_format::yuv440;
			return chroma_format::unknown;
		}

		// A Multi-Picture APP2 segment indexes further whole JPEGs stored in the same file: the second
		// frame of a burst, a depth or gain map, or the full-size image behind a thumbnail. The index is
		// a TIFF directory whose MPEntry tag holds one 16-byte record per image. Offsets are relative to
		// this segment's TIFF header, except the first image, which is the file itself and stores zero.
		void parse_mpf_index(std::vector<structure_row>& rows, const std::uint8_t* data, const std::uint32_t len,
		                     const std::uint64_t tiff_offset)
		{
			constexpr std::uint32_t mpf_signature_len = 4u; // "MPF\0"

			if (len < mpf_signature_len + 8u) return;

			const auto* const tiff = data + mpf_signature_len;
			const std::uint64_t tiff_len = len - mpf_signature_len;

			const auto intel = tiff[0] == 'I' && tiff[1] == 'I';
			const auto motorola = tiff[0] == 'M' && tiff[1] == 'M';

			if (!intel && !motorola) return;

			// Every offset below is read from the file, so the bounds test is done in 64 bits: a 32-bit
			// offset near the top of the range would otherwise wrap past the check.
			const auto fits = [tiff_len](const std::uint64_t at, const std::uint64_t need)
			{
				return at + need <= tiff_len;
			};

			const auto read16 = [tiff, intel](const std::uint64_t at) -> std::uint32_t
			{
				return intel ? load_le16(tiff + at) : load_be16(tiff + at);
			};
			const auto read32 = [tiff, intel](const std::uint64_t at) -> std::uint32_t
			{
				return intel ? load_le32(tiff + at) : load_be32(tiff + at);
			};

			if (read16(2) != 0x002a) return;

			const std::uint64_t ifd_offset = read32(4);

			if (!fits(ifd_offset, 2u)) return;

			const auto entry_count = read16(ifd_offset);

			std::uint64_t entries_offset = 0;
			std::uint64_t image_count = 0;

			for (auto i = 0u; i < entry_count; ++i)
			{
				const auto entry = ifd_offset + 2u + static_cast<std::uint64_t>(i) * 12u;

				if (!fits(entry, 12u)) return;

				const auto tag = read16(entry);
				const auto count = read32(entry + 4);
				const auto value = read32(entry + 8);

				if (tag == 0xb001) image_count = value;
				// MPEntry is 16 bytes per image, so it never fits the inline value field.
				else if (tag == 0xb002) entries_offset = count > 4u ? value : 0u;
			}

			if (image_count == 0 || entries_offset == 0) return;

			// A malformed count must not be trusted into a read; the segment's own length bounds it.
			const auto available = entries_offset <= tiff_len ? (tiff_len - entries_offset) / 16u : 0u;
			const auto reported = image_count;

			image_count = std::min(image_count, available);

			for (auto i = 0u; i < image_count; ++i)
			{
				const auto at = entries_offset + static_cast<std::uint64_t>(i) * 16u;
				const auto attributes = read32(at);
				const auto size = read32(at + 4);
				const auto offset = read32(at + 8);

				// Bits 24-30 of the attributes carry the MP type code.
				const auto type = (attributes >> 24) & 0x7f;
				const auto representative = (attributes & 0x20000000u) != 0;

				std::string_view role;

				switch (type)
				{
				case 0x01: role = "large thumbnail, VGA";
					break;
				case 0x02: role = "large thumbnail, full HD";
					break;
				case 0x03: role = "multi-frame panorama";
					break;
				case 0x04: role = "multi-frame disparity";
					break;
				case 0x05: role = "multi-angle";
					break;
				case 0x30: role = "baseline primary image";
					break;
				default: role = "undefined type";
					break;
				}

				// Only the first individual image stores zero, so a later entry claiming it is naming
				// an offset it does not have rather than pointing at this file.
				auto& row = add_row(rows, std::format("Image {}", i + 1),
				                    (i == 0 && offset == 0)
					                    ? std::format("{}, {} bytes, this file", role, size)
					                    : std::format("{}, {} bytes, offset {}", role, size, tiff_offset + offset));

				row.depth = 1;
				row.shape = representative
					            ? std::format("0x{:08x}, representative", attributes)
					            : std::format("0x{:08x}", attributes);
				row.id = std::format("jpeg.mpf.{}", i);
			}

			if (reported > image_count)
			{
				auto& row = add_row(rows, "Unread images",
				                    std::format("{} of {} declared entries lie outside the segment",
				                                reported - image_count, reported));
				row.depth = 1;
				row.id = "jpeg.mpf.unread";
			}
		}

		void append_rows(std::vector<structure_row>& to, std::vector<structure_row>& from)
		{
			to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
		}
	}

	scan_result scan_jpeg(reader& r, const scan_intent intent, const scan_limits& limits)
	{
		scan_result result;

		constexpr auto app14_data_len = 12u;

		// The structure report is what an inspect scan is for; indexing skips the extra segment
		// reads, the trailer read and the formatting it would only throw away.
		const auto want_structure = intent == scan_intent::inspect;

		auto has_adobe_marker = false;
		auto has_jfif_marker = false;
		auto adobe_transform = 0;
		auto channels = 0;
		auto luma_h = 0;
		auto luma_v = 0;
		auto success = false;

		// Structure reporting state, gathered as the markers go past.
		std::vector<structure_row> segment_rows;
		std::vector<structure_row> quant_rows;
		std::vector<structure_row> huffman_rows;
		std::vector<structure_row> comment_rows;
		std::vector<structure_row> embedded_image_rows;
		auto sof_marker = 0;
		auto sof_precision = 0;
		auto restart_interval = 0;
		auto huffman_tables = 0;
		auto standard_huffman_tables = 0;
		auto luminance_quality = 0;
		auto has_exif = false;
		auto has_xmp = false;
		auto has_iptc = false;
		std::string components_text;
		std::string subsampling_text;

		const auto add_segment = [&segment_rows, want_structure](const std::uint8_t marker, const std::uint64_t offset,
		                                                         const std::uint64_t bytes,
		                                                         const std::string_view identifier)
		{
			if (!want_structure) return;

			auto name = std::string(marker_name(marker));

			if (name == "APP")
			{
				name = std::format("APP{}", marker - M_APP0);
			}

			auto value = std::format("offset {}, {} bytes", offset, bytes);

			if (!identifier.empty())
			{
				value = std::format("{}, {}", identifier, value);
			}

			auto& row = add_row(segment_rows, std::move(name), std::move(value));
			row.depth = 1;
			row.shape = std::format("0xFF{:02X}", marker);
			row.id = std::format("jpeg.segment.{}", offset);
		};

		const auto file_len = r.size();
		std::uint64_t block_offset = 2u;

		// An ICC profile may span several APP2 segments; they are keyed by their 1-based sequence
		// number so they can be joined in order afterwards.
		std::map<int, std::vector<std::uint8_t>> icc_segments;
		std::vector<std::uint8_t> photoshop_resources;
		const auto photoshop_resource_cap = limits.jpeg_photoshop_resources;
		auto photoshop_resources_capped = false;

		while (file_len >= block_offset + 2u)
		{
			if (r.u8(block_offset) != 0xFF) break;

			auto block_marker = r.u8(block_offset + 1);

			// Any marker may be preceded by any number of 0xFF fill bytes. Reading one as a marker would
			// take the following image data for a segment length and abandon the scan.
			while (block_marker == 0xFF && file_len >= block_offset + 3u)
			{
				++block_offset;
				block_marker = r.u8(block_offset + 1);
			}

			if (block_marker == 0xFF) break;

			// TEM and the restart markers stand alone: they carry no length field to skip.
			if (block_marker == 0x01 || (block_marker >= 0xD0 && block_marker <= 0xD7))
			{
				block_offset += 2u;
				continue;
			}

			if (block_marker == M_SOS || block_marker == M_EOI)
			{
				// The end of the metadata and the start of the image data.
				add_segment(block_marker, block_offset, 2, {});
				success = true;
				break;
			}

			// Truncated: no room for the segment length.
			if (file_len < block_offset + 4u) break;

			const std::uint32_t block_len = r.be16(block_offset + 2);

			if (block_len < 2u || block_len > file_len - block_offset - 2u) break;

			const auto block_data_len = block_len - 2u;
			const auto data_offset = block_offset + 4u;

			// APPn segments are told apart by their leading identifier, so it is read for every one
			// of them and not only for the few the scan needs.
			std::string segment_identifier;

			if (want_structure && block_marker >= M_APP0 && block_marker <= M_APP15)
			{
				const auto identifier_len = std::min(block_data_len, 32u);
				if (identifier_len) segment_identifier = app_identifier(r.view(data_offset, identifier_len));
			}

			switch (block_marker)
			{
			case M_SOF0:
			case M_SOF1:
			case M_SOF2:
			case M_SOF3:
			case M_SOF5:
			case M_SOF6:
			case M_SOF7:
			case M_SOF9:
			case M_SOF10:
			case M_SOF11:
			case M_SOF13:
			case M_SOF14:
			case M_SOF15:
				{
					if (block_data_len < 6u) break;

					const auto* const block_data = r.view(data_offset, block_data_len).data();
					const auto bits = *block_data;
					result.height = load_be16(block_data + 1);
					result.width = load_be16(block_data + 3);
					channels = block_data[5];

					sof_marker = block_marker;
					sof_precision = bits;

					// Each component contributes its identifier, sampling factors and quantisation table.
					// The luma factors are what chroma subsampling is read from, so they are collected
					// whether or not the structure report was asked for.
					for (auto c = 0; c < channels; ++c)
					{
						const auto component_offset = 6u + c * 3u;
						if (component_offset + 3u > block_data_len) break;

						const auto id = block_data[component_offset];
						const auto h = block_data[component_offset + 1] >> 4;
						const auto v = block_data[component_offset + 1] & 0x0f;
						const auto tq = block_data[component_offset + 2];

						if (c == 0)
						{
							luma_h = h;
							luma_v = v;
						}

						if (want_structure)
						{
							if (!components_text.empty()) components_text += ", ";
							components_text += std::format("{}:{}x{} q{}", id, h, v, tq);
						}
					}

					if (!want_structure) break;

					const auto h0 = luma_h;
					const auto v0 = luma_v;

					if (channels >= 3 && h0 > 0 && v0 > 0)
					{
						if (h0 == 1 && v0 == 1) subsampling_text = "4:4:4 (none)";
						else if (h0 == 2 && v0 == 1) subsampling_text = "4:2:2";
						else if (h0 == 2 && v0 == 2) subsampling_text = "4:2:0";
						else if (h0 == 1 && v0 == 2) subsampling_text = "4:4:0";
						else subsampling_text = std::format("{}x{} luma sampling", h0, v0);
					}
					else if (channels == 1)
					{
						subsampling_text = "single component";
					}
				}
				break;

			case M_DQT:
				{
					if (!want_structure) break;

					const auto* const block_data = r.view(data_offset, block_data_len).data();
					std::uint32_t p = 0;

					while (p < block_data_len)
					{
						const auto precision = block_data[p] >> 4;
						const auto table_id = block_data[p] & 0x0f;
						const auto entry_bytes = precision ? 2u : 1u;
						const auto table_bytes = 64u * entry_bytes;
						++p;

						if (p + table_bytes > block_data_len) break;

						std::uint16_t table[64] = {};

						for (auto i = 0; i < 64; ++i)
						{
							table[zigzag_to_natural[i]] = precision ? load_be16(block_data + p + i * 2) : block_data[p + i];
						}

						p += table_bytes;

						const auto chrominance = table_id != 0;
						const auto quality = estimate_quality(table, chrominance);
						if (!chrominance && luminance_quality == 0) luminance_quality = quality;

						const auto* const reference = chrominance ? annex_k_chrominance_quant : annex_k_luminance_quant;
						const auto is_annex_k = std::memcmp(table, reference, sizeof(table)) == 0;

						auto& row = add_row(quant_rows,
						                    std::format("Table {} ({})", table_id, chrominance ? "chrominance" : "luminance"),
						                    std::format("quality about {}{}", quality,
						                                is_annex_k ? ", the Annex K example table unscaled" : ""));
						row.depth = 1;
						row.shape = std::format("{}-bit, {} bytes", precision ? 16 : 8, table_bytes);
						row.detail = structure_table{std::vector<std::uint16_t>(std::begin(table), std::end(table)), 8};
					}
				}
				break;

			case M_DHT:
				{
					if (!want_structure) break;

					const auto* const block_data = r.view(data_offset, block_data_len).data();
					std::uint32_t p = 0;

					while (p + 17u <= block_data_len)
					{
						const auto table_class = block_data[p] >> 4;
						const auto table_id = block_data[p] & 0x0f;
						const auto* const bits = block_data + p + 1;

						auto code_count = 0u;
						for (auto i = 0; i < 16; ++i) code_count += bits[i];

						if (p + 17u + code_count > block_data_len) break;

						const auto* const reference = table_class == 0
							                              ? (table_id == 0 ? annex_k_dc_luminance_bits : annex_k_dc_chrominance_bits)
							                              : (table_id == 0 ? annex_k_ac_luminance_bits : annex_k_ac_chrominance_bits);

						const auto is_annex_k = table_id < 2 && std::memcmp(bits, reference, 16) == 0;

						++huffman_tables;
						if (is_annex_k) ++standard_huffman_tables;

						add_structure_bytes(huffman_rows,
						                    std::format("{} table {}", table_class == 0 ? "DC" : "AC", table_id),
						                    is_annex_k
							                    ? std::format("standard (Annex K), {} codes", code_count)
							                    : std::format("optimised, {} codes", code_count),
						                    std::format("{} bytes", 17u + code_count),
						                    {block_data + p, 17u + code_count});

						p += 17u + code_count;
					}
				}
				break;

			case M_DRI:
				if (want_structure && block_data_len >= 2u)
				{
					restart_interval = r.be16(data_offset);
				}
				break;

			case M_COM:
				{
					if (!want_structure) break;

					const auto block = r.view(data_offset, block_data_len);
					std::string text(block.begin(), block.end());
					add_structure_row(comment_rows, "Comment", std::move(text),
					                  std::format("offset {}, {} bytes", block_offset, block_data_len));
					comment_rows.back().id = std::format("jpeg.com.{}", block_offset);
				}
				break;

			case M_APP0:
				{
					const auto block = r.view(data_offset, block_data_len);

					// JFIF declares the colour convention YCbCr. It lives here, in APP0: a baseline SOF0
					// frame says nothing about it, and treating SOF0 as JFIF hid the Adobe APP14
					// transform that marks a 3-channel JPEG as RGB.
					if (starts_with(block, {"JFIF\0", 5}))
					{
						has_jfif_marker = true;
					}

					constexpr std::size_t jfxx_header_len = 6u;
					constexpr std::uint8_t jfxx_extension_jpeg = 0x10;

					// Extension codes 0x11 and 0x13 carry a raw palettised or RGB raster, not a JPEG
					// stream, so only 0x10 is a thumbnail an image loader can take.
					if (block.size() >= jfxx_header_len && starts_with(block, {"JFXX\0", 5}) &&
						block[5] == jfxx_extension_jpeg)
					{
						result.jfif_thumbnail = {data_offset + jfxx_header_len, block_data_len - jfxx_header_len};
					}
				}
				break;

			case M_APP1:
				{
					const auto block = r.view(data_offset, block_data_len);

					if (starts_with(block, exif_signature))
					{
						const auto exif = block.subspan(exif_signature.size());
						result.metadata.exif.assign(exif.begin(), exif.end());
						has_exif = true;
					}
					else if (starts_with(block, xmp_signature))
					{
						const auto xmp = block.subspan(xmp_signature.size());
						result.metadata.xmp.assign(xmp.begin(), xmp.end());
						has_xmp = true;
					}
				}
				break;

			case M_APP2:
				{
					const auto block = r.view(data_offset, block_data_len);

					if (starts_with(block, icc_signature) && block.size() >= icc_header_len)
					{
						const auto sequence = block[icc_signature.size()];
						const auto icc = block.subspan(icc_header_len);
						icc_segments.insert_or_assign(sequence, std::vector<std::uint8_t>(icc.begin(), icc.end()));
					}
					else if (want_structure && block_data_len >= 4u && std::memcmp(block.data(), "MPF\0", 4) == 0)
					{
						// MPF offsets are measured from this segment's TIFF header, which follows the identifier.
						parse_mpf_index(embedded_image_rows, block.data(), block_data_len, data_offset + 4u);
					}
				}
				break;

			case M_APP13:
				{
					const auto block = r.view(data_offset, block_data_len);

					if (starts_with(block, photoshop_signature))
					{
						const auto body = block.subspan(photoshop_signature.size());

						if (!photoshop_resources_capped)
						{
							const auto remaining = photoshop_resources.size() < photoshop_resource_cap
								                       ? photoshop_resource_cap - photoshop_resources.size()
								                       : std::size_t{0};
							const auto to_copy = std::min(remaining, body.size());
							photoshop_resources.insert(photoshop_resources.end(), body.begin(), body.begin() + to_copy);
							if (to_copy < body.size()) photoshop_resources_capped = true;
						}
					}
				}
				break;

			case M_APP14:
				if (block_data_len >= app14_data_len)
				{
					const auto block = r.view(data_offset, block_data_len);

					if (starts_with(block, "Adobe"))
					{
						has_adobe_marker = true;
						adobe_transform = block[11];
					}
				}
				break;

			default:
				break;
			}

			add_segment(block_marker, block_offset, block_len + 2u, segment_identifier);

			block_offset += block_len + 2u;
		}

		// Join the ICC profile from its APP2 segments, in ascending sequence.
		for (const auto& segment : icc_segments | std::views::values)
		{
			result.metadata.icc.insert(result.metadata.icc.end(), segment.begin(), segment.end());
		}

		if (!photoshop_resources.empty())
		{
			metadata_blocks app13;
			has_iptc = parse_photoshop_resources(app13, photoshop_resources, false) && !app13.iptc.empty();
			if (!app13.iptc.empty()) result.metadata.iptc = std::move(app13.iptc);
			if (!app13.photoshop.empty()) result.metadata.photoshop = std::move(app13.photoshop);
		}

		result.picture.bit_depth = static_cast<std::uint8_t>(sof_precision);

		switch (channels)
		{
		case 1:
			result.pixel_format = "gray8";
			result.picture.chroma = chroma_format::gray;
			break;

		case 3:
			if (has_jfif_marker || !has_adobe_marker || adobe_transform != 0)
			{
				// Named as HEIF, WebP and video name their sampling, so one search for it reaches them all.
				result.picture.chroma = ycbcr_chroma(luma_h, luma_v);
				result.pixel_format = result.picture.chroma == chroma_format::unknown
					                      ? "YCbCr"
					                      : picture_format{result.picture.chroma}.name();
			}
			else
			{
				result.pixel_format = "rgb24";
				result.picture.chroma = chroma_format::rgb;
			}
			break;

		case 4:
			result.pixel_format = has_adobe_marker && adobe_transform != 0 ? "ycck" : "cmyk";
			result.picture.chroma = has_adobe_marker && adobe_transform != 0 ? chroma_format::ycck : chroma_format::cmyk;
			break;

		default:
			result.pixel_format = "YCbCr";
			break;
		}

		// Report what the file says about its own construction. The scan data is not decoded, so only
		// the trailer is examined, which is where appended data would sit.
		if (success && want_structure)
		{
			std::uint64_t trailing_bytes = 0;
			auto eoi_found = false;

			constexpr std::uint64_t tail_search_bytes = 64ull * 1024ull;
			const auto tail_len = static_cast<std::size_t>(std::min<std::uint64_t>(file_len, tail_search_bytes));

			if (tail_len >= 2u)
			{
				const auto tail_offset = file_len - tail_len;
				const auto tail = r.read(tail_offset, tail_len);

				for (auto i = tail_len - 2u; i != static_cast<std::size_t>(-1); --i)
				{
					if (tail[i] == 0xFF && tail[i + 1] == M_EOI)
					{
						eoi_found = true;
						trailing_bytes = file_len - (tail_offset + i + 2u);
						break;
					}
				}
			}

			std::vector<structure_row> kv;

			add_structure_section(kv, "Summary", "jpeg.summary", true);
			add_structure_row(kv, "File size", std::format("{} bytes", file_len));

			if (sof_marker)
			{
				add_structure_row(kv, "Encoding", std::string(marker_name(static_cast<std::uint8_t>(sof_marker))),
				                  std::format("0xFF{:02X}", sof_marker));
				add_structure_row(kv, "Sample precision", std::format("{} bits", sof_precision));
			}

			add_structure_row(kv, "Dimensions", std::format("{} x {}", result.width, result.height));
			add_structure_row(kv, "Components", std::format("{}", channels));

			if (!components_text.empty())
				add_structure_row(kv, "Component sampling", components_text, "id:HxV quant-table");
			if (!subsampling_text.empty())
				add_structure_row(kv, "Chroma subsampling", subsampling_text);

			add_structure_row(kv, "Colour convention",
			                  has_jfif_marker
				                  ? "JFIF (APP0)"
				                  : has_adobe_marker
				                  ? std::format("Adobe APP14, transform {}", adobe_transform)
				                  : "not declared");

			if (restart_interval)
				add_structure_row(kv, "Restart interval", std::format("{} MCU rows", restart_interval));

			if (luminance_quality)
				add_structure_row(kv, "Estimated quality", std::format("about {}", luminance_quality));

			if (huffman_tables)
			{
				// Camera firmware writes the Annex K example tables; software encoders normally fit
				// tables to the image, so a mismatch marks a re-encode.
				add_structure_row(kv, "Huffman tables",
				                  standard_huffman_tables == huffman_tables
					                  ? std::format("{}, all standard (Annex K)", huffman_tables)
					                  : standard_huffman_tables == 0
					                  ? std::format("{}, all optimised", huffman_tables)
					                  : std::format("{}, {} standard and {} optimised", huffman_tables,
					                                standard_huffman_tables,
					                                huffman_tables - standard_huffman_tables));
			}

			add_structure_row(kv, "Metadata carried",
			                  std::format("{}{}{}{}",
			                              has_exif ? "Exif " : "",
			                              has_xmp ? "XMP " : "",
			                              has_iptc ? "IPTC " : "",
			                              result.metadata.icc.empty() ? "" : "ICC"));

			add_structure_row(kv, "End of image marker",
			                  eoi_found
				                  ? trailing_bytes == 0
					                    ? "present, nothing follows it"
					                    : std::format("present, {} bytes follow it", trailing_bytes)
				                  : "not found in the last 64 KB");

			add_structure_section(kv, "Segments", "jpeg.segments");
			append_rows(kv, segment_rows);

			add_structure_section(kv, "Quantisation tables", "jpeg.dqt");
			append_rows(kv, quant_rows);

			add_structure_section(kv, "Huffman tables", "jpeg.dht");
			append_rows(kv, huffman_rows);

			add_structure_section(kv, "Comments", "jpeg.com");
			append_rows(kv, comment_rows);

			add_structure_section(kv, "Embedded images", "jpeg.embedded");
			append_rows(kv, embedded_image_rows);

			finish_structure_sections(kv);
			result.structure = std::move(kv);
		}

		result.success = success;
		return result;
	}
}
