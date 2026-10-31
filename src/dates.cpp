// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The date rules readers and writers share: the strict form an edit gives a date in, the calendar
// arithmetic every count of seconds a container keeps is converted by, in both directions, and EXIF's text.
// The lenient parser of the forms tags write is media_tags.cpp's, beside the helpers it shares with them.

#include "date_internal.h"

#include <charconv>
#include <format>

namespace libfiles::detail
{
	namespace
	{
		bool parse_number(const std::string_view s, const std::size_t at, const std::size_t digits, int& out)
		{
			if (s.size() < at + digits) return false;
			const auto* const first = s.data() + at;
			const auto [end, ec] = std::from_chars(first, first + digits, out);
			return ec == std::errc{} && end == first + digits;
		}
	}

	std::optional<date> parse_xmp_date(const std::string_view s)
	{
		date d;
		if (!parse_number(s, 0, 4, d.year)) return {};
		if (s.size() < 10 || s[4] != '-' || s[7] != '-') return {};
		if (!parse_number(s, 5, 2, d.month) || !parse_number(s, 8, 2, d.day)) return {};
		if (s.size() == 10) return d;

		if (s[10] != 'T' || s.size() < 16 || s[13] != ':') return {};
		if (!parse_number(s, 11, 2, d.hour) || !parse_number(s, 14, 2, d.minute)) return {};
		d.has_time = true;

		std::size_t at = 16;

		if (at < s.size() && s[at] == ':')
		{
			if (!parse_number(s, at + 1, 2, d.second)) return {};
			at += 3;
			if (at < s.size() && s[at] == '.')
			{
				++at;
				while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
			}
		}

		if (at == s.size()) return d;

		if (s[at] == 'Z')
		{
			d.zone = date::time_zone::utc;
			return at + 1 == s.size() ? std::optional(d) : std::nullopt;
		}

		int zone_hour = 0;
		int zone_minute = 0;
		if ((s[at] != '+' && s[at] != '-') || s.size() != at + 6 || s[at + 3] != ':') return {};
		if (!parse_number(s, at + 1, 2, zone_hour) || !parse_number(s, at + 4, 2, zone_minute)) return {};

		d.zone = date::time_zone::offset;
		d.offset_minutes = (zone_hour * 60 + zone_minute) * (s[at] == '-' ? -1 : 1);
		return d;
	}

	std::int64_t days_from_civil(int y, const unsigned m, const unsigned d) noexcept
	{
		// Howard Hinnant's days_from_civil.
		y -= m <= 2;
		const auto era = (y >= 0 ? y : y - 399) / 400;
		const auto yoe = static_cast<unsigned>(y - era * 400);
		const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
	}

	std::int64_t seconds_since(const int epoch_year, const date& d) noexcept
	{
		const auto days = days_from_civil(d.year, static_cast<unsigned>(d.month), static_cast<unsigned>(d.day)) - days_from_civil(epoch_year, 1, 1);
		auto seconds = days * 86400 + d.hour * 3600 + d.minute * 60 + d.second;
		if (d.zoned()) seconds -= static_cast<std::int64_t>(d.offset_minutes) * 60;
		return seconds;
	}

	date utc_date_from_seconds(const std::int64_t seconds_since_1970) noexcept
	{
		// Howard Hinnant's civil_from_days, over whole days and the seconds left in the last.
		constexpr std::int64_t day = 86400;
		auto days = seconds_since_1970 / day;
		auto rest = seconds_since_1970 % day;

		if (rest < 0)
		{
			rest += day;
			--days;
		}

		const auto z = days + 719468;
		const auto era = (z >= 0 ? z : z - 146096) / 146097;
		const auto doe = z - era * 146097;
		const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const auto mp = (5 * doy + 2) / 153;
		const auto dd = doy - (153 * mp + 2) / 5 + 1;
		const auto m = mp < 10 ? mp + 3 : mp - 9;
		const auto y = yoe + era * 400 + (m <= 2 ? 1 : 0);

		date result;
		if (y < 1 || y > 9999) return result;

		result.year = static_cast<int>(y);
		result.month = static_cast<int>(m);
		result.day = static_cast<int>(dd);
		result.has_time = true;
		result.hour = static_cast<int>(rest / 3600);
		result.minute = static_cast<int>(rest / 60 % 60);
		result.second = static_cast<int>(rest % 60);
		result.zone = date::time_zone::utc;
		return result;
	}

	std::string exif_date_text(const date& d)
	{
		return std::format("{:04}:{:02}:{:02} {:02}:{:02}:{:02}", d.year, d.month, d.day, d.hour, d.minute, d.second);
	}
}
