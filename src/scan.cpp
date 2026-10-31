// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Format detection, the scan entry point that dispatches to each format's scanner, and the
// structure-row helpers they share.

#include "scan_internal.h"

#include <array>
#include <exception>
#include <format>

namespace libfiles
{
	namespace
	{
		std::uint32_t head_le32(const std::span<const std::uint8_t> h, const std::size_t offset)
		{
			return detail::load_le32(h.data() + offset);
		}

		std::uint16_t head_le16(const std::span<const std::uint8_t> h)
		{
			return detail::load_le16(h.data());
		}

		constexpr std::array<std::array<std::uint8_t, 4>, 12> heif_brands = {
			{
				{'h', 'e', 'i', 'c'},
				{'h', 'e', 'i', 'x'},
				{'h', 'e', 'v', 'c'},
				{'h', 'e', 'v', 'x'},
				{'h', 'e', 'i', 'm'},
				{'h', 'e', 'i', 's'},
				{'h', 'e', 'v', 'm'},
				{'h', 'e', 'v', 's'},
				{'m', 'i', 'f', '1'},
				{'m', 's', 'f', '1'},
				{'a', 'v', 'i', 'f'},
				{'a', 'v', 'i', 's'},
			}
		};

		bool has_heif_brand(const std::span<const std::uint8_t> h)
		{
			if (h.size() < 12u) return false;
			if (!detail::starts_with(h.subspan(4), "ftyp")) return false;

			return std::ranges::any_of(heif_brands, [h](const auto& brand)
			{
				return std::equal(brand.begin(), brand.end(), h.begin() + 8);
			});
		}
	}

	image_format detect_image_format(const std::span<const std::uint8_t> h) noexcept
	{
		if (h.size() < 4) return image_format::unknown;

		if (head_le32(h, 0) == 0x53504238) return image_format::psd; // 8BPS
		if (head_le32(h, 0) == 0x38464947) return image_format::gif; // GIF8

		// A JPEG XL container (00 00 00 0C 'J' 'X' 'L' ' ') or a bare codestream (FF 0A).
		if (head_le32(h, 0) == 0x0C000000 && h.size() >= 8 && head_le32(h, 4) == 0x204C584A) return image_format::jxl;
		if (head_le16(h) == 0x0AFF) return image_format::jxl;

		// The WEBP FourCC keeps other RIFF payloads, WAV and AVI, from being taken for WebP.
		if (h.size() >= 12 && head_le32(h, 0) == 0x46464952 && head_le32(h, 8) == 0x50424557) return image_format::webp;

		if (head_le16(h) == 0xD8FF) return image_format::jpeg;
		if (head_le16(h) == 0x4D42) return image_format::bmp;
		if (head_le16(h) == 0x5089) return image_format::png;

		// The byte-order mark alone matches any file starting with II or MM, so the version must be
		// 42 for classic TIFF or 43 for BigTIFF too.
		const auto order = head_le16(h);

		if (order == 0x4949 || order == 0x4d4d)
		{
			const auto version = order == 0x4949 ? detail::load_le16(h.data() + 2) : detail::load_be16(h.data() + 2);
			if (version == 42u || version == 43u) return image_format::tiff;
		}

		if (has_heif_brand(h)) return image_format::heif;
		return image_format::unknown;
	}

	bool can_scan(const image_format format) noexcept
	{
		switch (format)
		{
		case image_format::jpeg:
		case image_format::png:
		case image_format::webp:
		case image_format::gif:
		case image_format::psd:
		case image_format::bmp:
		case image_format::tiff:
		case image_format::heif:
		case image_format::jxl:
			return true;
		default:
			return false;
		}
	}

	scan_result scan(reader& r, const scan_intent intent, const scan_limits& limits)
	{
		scan_result result;

		if (r.size() < 16) return result;

		auto format = image_format::unknown;

		try
		{
			std::uint8_t head[16];
			r.read(0, head, sizeof(head));
			format = detect_image_format(head);

			switch (format)
			{
			case image_format::jpeg: result = detail::scan_jpeg(r, intent, limits);
				break;
			case image_format::png: result = detail::scan_png(r);
				break;
			case image_format::webp: result = detail::scan_webp(r, intent);
				break;
			case image_format::gif: result = detail::scan_gif(r);
				break;
			case image_format::psd: result = detail::scan_psd(r);
				break;
			case image_format::bmp: result = detail::scan_bmp(r);
				break;
			case image_format::tiff: result = detail::scan_tiff(r);
				break;
			case image_format::heif: result = detail::scan_heif(r, intent);
				break;
			case image_format::jxl: result = detail::scan_jxl(r, intent, limits);
				break;
			default:
				break;
			}

			result.format = format;

			if (!result.metadata.exif.empty())
			{
				const auto summary = summarize_exif(result.metadata.exif);
				if (summary.orientation) result.orientation = summary.orientation;
				result.exif_thumbnail = summary.thumbnail;
			}
		}
		catch (const std::exception& e)
		{
			result = {};
			result.format = format;
			result.error = e.what();
		}

		return result;
	}

	std::vector<embedded_image> thumbnails(const scan_result& result)
	{
		std::vector<embedded_image> images;

		const auto add = [&images](std::string description) -> embedded_image&
		{
			auto& image = images.emplace_back();
			image.mime_type = "image/jpeg";
			image.type = 0;
			image.description = std::move(description);
			return image;
		};

		const auto& exif = result.metadata.exif;
		const auto& in_exif = result.exif_thumbnail;

		if (!in_exif.empty() && in_exif.offset <= exif.size() && in_exif.size <= exif.size() - in_exif.offset)
		{
			const auto first = exif.begin() + static_cast<std::ptrdiff_t>(in_exif.offset);
			add("EXIF thumbnail").data.assign(first, first + static_cast<std::ptrdiff_t>(in_exif.size));
		}

		if (!result.tiff_thumbnail.empty()) add("TIFF thumbnail").location = result.tiff_thumbnail;
		if (!result.jfif_thumbnail.empty()) add("JFIF thumbnail").location = result.jfif_thumbnail;
		return images;
	}
}

namespace libfiles::detail
{
	structure_row& add_row(std::vector<structure_row>& rows, std::string key, std::string value)
	{
		auto& row = rows.emplace_back();
		row.key = std::move(key);
		row.value = std::move(value);
		return row;
	}

	void add_structure_row(std::vector<structure_row>& rows, const std::string_view key, std::string value,
	                       const std::string_view shape, std::string detail)
	{
		auto& row = add_row(rows, std::string(key), std::move(value));
		row.depth = 1;
		if (!shape.empty()) row.shape = shape;
		if (!detail.empty()) row.detail = std::move(detail);
	}

	void add_structure_bytes(std::vector<structure_row>& rows, const std::string_view key, std::string value,
	                         const std::string_view shape, const std::span<const std::uint8_t> payload)
	{
		auto& row = add_row(rows, std::string(key), std::move(value));
		row.depth = 1;
		if (!shape.empty()) row.shape = shape;

		if (!payload.empty())
		{
			const auto kept = std::min(payload.size(), max_structure_bytes);
			row.detail = std::vector<std::uint8_t>(payload.begin(), payload.begin() + kept);
		}
	}

	void add_structure_section(std::vector<structure_row>& rows, const std::string_view key, const std::string_view id,
	                           const bool open_by_default)
	{
		auto& row = add_row(rows, std::string(key), {});
		row.container = true;
		row.id = id;
		row.open_by_default = open_by_default;
	}

	void finish_structure_sections(std::vector<structure_row>& rows)
	{
		std::vector<structure_row> kept;
		kept.reserve(rows.size());

		for (std::size_t i = 0; i < rows.size(); ++i)
		{
			if (rows[i].container)
			{
				std::size_t count = 0;
				while (i + 1 + count < rows.size() && !rows[i + 1 + count].container) ++count;
				if (count == 0) continue;

				rows[i].key = std::format("{} ({})", rows[i].key, count);
			}

			kept.emplace_back(std::move(rows[i]));
		}

		rows = std::move(kept);
	}
}
