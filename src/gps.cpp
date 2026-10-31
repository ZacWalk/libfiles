// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: XMP's GPS coordinate text, read into the rationals EXIF keeps and the degrees a position is read in.

#include "gps_internal.h"

#include <charconv>
#include <string_view>

namespace libfiles::detail
{
	namespace
	{
		std::optional<std::pair<std::uint32_t, std::uint32_t>> rational(const std::string_view s)
		{
			const auto dot = s.find('.');
			std::uint64_t whole = 0;
			const auto int_part = s.substr(0, dot);
			if (int_part.empty() || std::from_chars(int_part.data(), int_part.data() + int_part.size(), whole).ec != std::errc{}) return {};
			if (dot == std::string_view::npos) return whole <= 0xFFFFFFFFu ? std::optional(std::pair{static_cast<std::uint32_t>(whole), 1u}) : std::nullopt;

			auto fraction = s.substr(dot + 1);
			if (fraction.size() > 6) fraction = fraction.substr(0, 6);

			std::uint64_t numerator = whole;
			std::uint32_t denominator = 1;

			for (const auto c : fraction)
			{
				if (c < '0' || c > '9') return {};
				numerator = numerator * 10 + static_cast<std::uint64_t>(c - '0');
				denominator *= 10;
			}

			if (numerator > 0xFFFFFFFFu) return {};
			return std::pair{static_cast<std::uint32_t>(numerator), denominator};
		}
	}

	std::optional<gps_parts> parse_xmp_gps(std::string_view s)
	{
		if (s.size() < 3) return {};

		gps_parts g;
		g.reference = s.back();
		if (g.reference != 'N' && g.reference != 'S' && g.reference != 'E' && g.reference != 'W') return {};
		s.remove_suffix(1);

		std::vector<std::string_view> parts;
		while (true)
		{
			const auto comma = s.find(',');
			parts.push_back(s.substr(0, comma));
			if (comma == std::string_view::npos) break;
			s.remove_prefix(comma + 1);
		}

		if (parts.size() != 2 && parts.size() != 3) return {};

		for (const auto part : parts)
		{
			const auto r = rational(part);
			if (!r) return {};
			g.rationals.push_back(*r);
		}

		if (g.rationals.size() == 2) g.rationals.emplace_back(0u, 1u);
		return g;
	}

	double gps_degrees(const gps_parts& parts) noexcept
	{
		auto degrees = 0.0;
		auto unit = 1.0;

		for (const auto& [numerator, denominator] : parts.rationals)
		{
			if (denominator != 0) degrees += static_cast<double>(numerator) / static_cast<double>(denominator) / unit;
			unit *= 60.0;
		}

		return parts.reference == 'S' || parts.reference == 'W' ? -degrees : degrees;
	}
}
