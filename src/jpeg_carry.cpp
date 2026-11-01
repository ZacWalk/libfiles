// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: A JPEG's descriptive segments carried into a re-encoding of it, byte for byte but for what the
// re-encoding invalidated; and a JPEG's header read no further than its first scan. What ImageWalker saved
// a re-encoded photo with before it moved here.

#include "libfiles/encode.h"

#include "libfiles/exif.h"
#include "libfiles/scan.h"

#include <cstring>
#include <exception>
#include <string_view>
#include <utility>

namespace libfiles
{
	namespace
	{
		constexpr std::uint8_t soi = 0xD8, eoi = 0xD9, sos = 0xDA;
		constexpr std::uint8_t app0 = 0xE0, app1 = 0xE1, app2 = 0xE2, app14 = 0xEE, app15 = 0xEF, comment = 0xFE;
		// Metadata segments hold at most 64 KiB each; a header larger than this is not a photo's.
		constexpr std::size_t maximum_header_bytes = 32ull * 1024 * 1024;

		constexpr std::string_view jfif_identifier{"JFIF\0", 5};
		constexpr std::string_view exif_identifier{"Exif\0\0", 6};
		constexpr std::string_view xmp_identifier{"http://ns.adobe.com/xap/1.0/\0", 29};
		constexpr std::string_view icc_identifier{"ICC_PROFILE\0", 12};
		constexpr std::string_view mpf_identifier{"MPF\0", 4};

		struct segment
		{
			std::uint8_t marker = 0;
			std::span<const std::uint8_t> payload;
		};

		bool is_app(const std::uint8_t marker)
		{
			return marker >= app0 && marker <= app15;
		}

		// Standalone markers have no length, and never belong before the first scan.
		bool is_standalone(const std::uint8_t marker)
		{
			return marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= soi);
		}

		bool starts_with(const std::span<const std::uint8_t> payload, const std::string_view prefix)
		{
			return payload.size() >= prefix.size() && std::memcmp(payload.data(), prefix.data(), prefix.size()) == 0;
		}

		// SOI and every segment before the first scan; scan is where the scan starts, or the end of data
		// when it holds only a header.
		bool split_header(const std::span<const std::uint8_t> data, std::vector<segment>& segments, std::size_t& scan)
		{
			if (data.size() < 2 || data[0] != 0xFF || data[1] != soi) return false;
			std::size_t position = 2;

			while (position < data.size())
			{
				if (data[position] != 0xFF) return false;
				auto marker = position + 1;
				while (marker < data.size() && data[marker] == 0xFF) ++marker;
				if (marker >= data.size()) return false;

				const auto code = data[marker];

				if (code == sos || code == eoi)
				{
					scan = position;
					return true;
				}

				if (is_standalone(code) || data.size() - marker < 3) return false;
				const std::size_t length = static_cast<std::size_t>(data[marker + 1]) << 8 | data[marker + 2];
				if (length < 2 || length > data.size() - marker - 1) return false;
				segments.push_back({code, data.subspan(marker + 3, length - 2)});
				position = marker + 1 + length;
			}

			scan = position;
			return true;
		}

		// EXIF for upright pixels of the new size, corrected where its numbers stand: the orientation 1, the
		// pixel dimensions the new ones, and the thumbnail's directory, whose picture shows the old pixels,
		// unlinked. False when the structure cannot be followed far enough to be sure the orientation it
		// states is 1.
		bool correct_exif(std::vector<std::uint8_t>& payload, const int width, const int height)
		{
			const auto tiff = std::span(payload).subspan(exif_identifier.size());
			const exif_number numbers[] = {
				{exif_directory::primary, 0x0112, 1}, // Orientation
				{exif_directory::exif, 0xa002, static_cast<std::uint32_t>(width)}, // PixelXDimension
				{exif_directory::exif, 0xa003, static_cast<std::uint32_t>(height)}, // PixelYDimension
			};
			set_exif_numbers(tiff, numbers);
			if (!unlink_exif_thumbnail(tiff)) return false;
			const auto orientation = summarize_exif(tiff).orientation;
			return !orientation || *orientation == 1;
		}

		// XMP repeats the orientation as text: tiff:Orientation="6", or <tiff:Orientation>6<.
		void correct_xmp(std::vector<std::uint8_t>& payload)
		{
			constexpr std::string_view name{"tiff:Orientation"};
			const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());

			const auto skip_space = [&text](std::size_t at)
			{
				while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
				return at;
			};

			for (auto at = text.find(name); at != std::string_view::npos; at = text.find(name, at + name.size()))
			{
				auto value = skip_space(at + name.size());
				auto close = '<';

				if (value < text.size() && text[value] == '=')
				{
					value = skip_space(value + 1);
					if (value >= text.size() || (text[value] != '"' && text[value] != '\'')) continue;
					close = text[value++];
				}
				else if (value < text.size() && text[value] == '>')
				{
					++value;
				}
				else
				{
					continue;
				}

				if (value + 1 < text.size() && text[value] >= '1' && text[value] <= '8' && text[value + 1] == close)
				{
					payload[value] = '1';
				}
			}
		}

		// ICC_PROFILE chunks carry a sequence number and a count before the profile's bytes; the first
		// starts with the profile's header, whose colour space is at offset 16.
		bool rgb_profile(const std::vector<segment>& segments)
		{
			constexpr auto chunk_header = icc_identifier.size() + 2;

			for (const auto& s : segments)
			{
				if (s.marker == app2 && starts_with(s.payload, icc_identifier) && s.payload.size() >= chunk_header + 20 &&
					s.payload[icc_identifier.size()] == 1)
				{
					return std::memcmp(s.payload.data() + chunk_header + 16, "RGB ", 4) == 0;
				}
			}

			return false;
		}
	}

	std::vector<std::uint8_t> read_jpeg_header(reader& input)
	{
		try
		{
			if (input.size() < 2 || input.u8(0) != 0xFF || input.u8(1) != soi) return {};
			std::vector<std::uint8_t> header{0xFF, soi};
			std::uint64_t at = 2;

			for (;;)
			{
				if (!input.contains(at, 1) || input.u8(at) != 0xFF) return {};
				++at;

				std::uint8_t marker = 0;

				do
				{
					if (!input.contains(at, 1)) return {};
					marker = input.u8(at++);
				}
				while (marker == 0xFF);

				if (marker == sos || marker == eoi) return header;
				if (is_standalone(marker) || !input.contains(at, 2)) return {};

				const std::size_t length = input.be16(at);
				if (length < 2 || header.size() + 2 + length > maximum_header_bytes || !input.contains(at + 2, length - 2)) return {};

				header.insert(header.end(), {0xFF, marker, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length & 0xFF)});
				const auto payload = input.read(at + 2, length - 2);
				header.insert(header.end(), payload.begin(), payload.end());
				at += length;
			}
		}
		catch (const std::exception&)
		{
			return {};
		}
	}

	std::vector<std::uint8_t> carry_jpeg_metadata(const std::span<const std::uint8_t> source,
	                                              const std::span<const std::uint8_t> encoded, const int width,
	                                              const int height)
	{
		std::vector<segment> source_segments, encoded_segments;
		std::size_t source_scan = 0, encoded_scan = 0;

		if (width <= 0 || height <= 0 || !split_header(source, source_segments, source_scan) ||
			!split_header(encoded, encoded_segments, encoded_scan) || encoded_scan >= encoded.size())
		{
			return {};
		}

		const auto keep_profile = rgb_profile(source_segments);
		std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> kept;
		auto jfif = false;
		auto exif = false;

		for (const auto& s : source_segments)
		{
			std::vector<std::uint8_t> payload(s.payload.begin(), s.payload.end());

			switch (s.marker)
			{
			case app0:
				// JFIF keeps its density but not its thumbnail; JFXX holds only thumbnails.
				if (jfif || !starts_with(s.payload, jfif_identifier) || payload.size() < 14) continue;
				payload.resize(14);
				payload[12] = payload[13] = 0;
				jfif = true;
				break;
			case app1:
				if (starts_with(s.payload, exif_identifier))
				{
					if (exif || !correct_exif(payload, width, height)) continue;
					exif = true;
				}
				else if (starts_with(s.payload, xmp_identifier))
				{
					correct_xmp(payload);
				}
				break;
			case app2:
				if (starts_with(s.payload, icc_identifier) ? !keep_profile : starts_with(s.payload, mpf_identifier)) continue;
				break;
			case app14:
				// Adobe's segment describes the old encoding's colour transform.
				continue;
			default:
				if (!is_app(s.marker) && s.marker != comment) continue;
			}

			kept.emplace_back(s.marker, std::move(payload));
		}

		std::vector<std::uint8_t> result{0xFF, soi};
		result.reserve(source.size() + encoded.size());

		const auto emit = [&result](const std::uint8_t marker, const std::span<const std::uint8_t> payload)
		{
			const auto length = payload.size() + 2;
			result.insert(result.end(), {0xFF, marker, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length & 0xFF)});
			result.insert(result.end(), payload.begin(), payload.end());
		};

		// A file with neither JFIF nor EXIF keeps the encoder's JFIF identification.
		if (!jfif && !exif)
		{
			for (const auto& s : encoded_segments)
			{
				if (s.marker == app0 && starts_with(s.payload, jfif_identifier))
				{
					emit(app0, s.payload);
					break;
				}
			}
		}

		for (const auto& [marker, payload] : kept) emit(marker, payload);

		for (const auto& s : encoded_segments)
		{
			if (!is_app(s.marker) && s.marker != comment) emit(s.marker, s.payload);
		}

		result.insert(result.end(), encoded.begin() + static_cast<std::ptrdiff_t>(encoded_scan), encoded.end());
		return result;
	}
}
