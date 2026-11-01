// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The rules the values in a photo's EXIF, IPTC and XMP are read by, as photo_internal.h states them.

#include "photo_internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <system_error>

namespace libfiles::detail
{
	namespace
	{
		// Text read as the C library's sscanf reads it - by glibc's rules, which MSVC's break where a 0xFF byte
		// meets a number, reading it as the end of the text and swallowing it - for the conversions these values
		// use. White space is the C locale's, and the text ends at its length or a NUL.
		class scanned_text
		{
			std::string_view _s;
			std::size_t _at = 0;

			int peek() const
			{
				return _at < _s.size() ? static_cast<unsigned char>(_s[_at]) : -1;
			}

			static bool space(const int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
			static bool digit(const int c) { return c >= '0' && c <= '9'; }
			static bool hex_digit(const int c) { return digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
			static int lower(const int c) { return c >= 'A' && c <= 'Z' ? c | 0x20 : c; }

			// Where the first significant digit of a number from_chars found out of range lies, as an exponent of
			// its base: positive for one too large, negative for one too small.
			static std::int64_t leading_exponent(const std::string_view s, const bool hex)
			{
				const auto in_number = [hex](const char c)
				{
					return hex ? hex_digit(static_cast<unsigned char>(c)) : digit(c);
				};

				std::size_t i = 0;
				std::int64_t integer_digits = 0;
				std::int64_t fraction_zeros = 0;
				auto significant = false;

				for (; i < s.size() && in_number(s[i]); ++i)
				{
					if (significant || s[i] != '0')
					{
						significant = true;
						++integer_digits;
					}
				}

				if (i < s.size() && s[i] == '.')
				{
					for (++i; i < s.size() && in_number(s[i]); ++i)
					{
						if (significant) continue;
						if (s[i] == '0') ++fraction_zeros;
						else significant = true;
					}
				}

				std::int64_t exponent = 0;

				if (i < s.size() && lower(static_cast<unsigned char>(s[i])) == (hex ? 'p' : 'e'))
				{
					++i;
					const auto negative = i < s.size() && s[i] == '-';
					if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;

					for (; i < s.size() && digit(s[i]); ++i)
					{
						exponent = std::min<std::int64_t>(exponent * 10 + (s[i] - '0'), 1000000000000);
					}

					if (negative) exponent = -exponent;
				}

				const auto digits = integer_digits > 0 ? integer_digits - 1 : -fraction_zeros - 1;
				return (hex ? 4 * digits : digits) + exponent;
			}

			// What strtod makes of the number item begins with: false when it begins with none.
			static bool convert(std::string_view item, double& out)
			{
				const auto negative = !item.empty() && item[0] == '-';
				if (!item.empty() && (item[0] == '-' || item[0] == '+')) item.remove_prefix(1);

				const auto hex = item.size() >= 2 && item[0] == '0' && lower(static_cast<unsigned char>(item[1])) == 'x';
				const auto number = item.substr(hex ? 2 : 0);
				double value = 0;
				const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value,
				                                          hex ? std::chars_format::hex : std::chars_format::general);

				if (error == std::errc::invalid_argument)
				{
					// strtod reads "0x" with no digit after it as the 0 before it.
					if (!hex) return false;
					value = 0;
				}
				else if (error == std::errc::result_out_of_range)
				{
					value = leading_exponent(number.substr(0, static_cast<std::size_t>(end - number.data())), hex) > 0
						        ? std::numeric_limits<double>::infinity()
						        : 0.0;
				}

				out = negative ? -value : value;
				return true;
			}

			bool take_word(const std::string_view word, std::string& item)
			{
				for (const auto c : word)
				{
					if (lower(peek()) != c) return false;
					item += _s[_at++];
				}

				return true;
			}

		public:
			explicit scanned_text(const std::string_view s) : _s(s.substr(0, s.find('\0')))
			{
			}

			bool at_end() const
			{
				return peek() < 0;
			}

			// White space in a format: a run of it, or none.
			scanned_text& spaces()
			{
				while (space(peek())) ++_at;
				return *this;
			}

			// An ordinary character in a format, which white space before it does not match.
			bool is(const char c)
			{
				if (peek() != static_cast<unsigned char>(c)) return false;
				++_at;
				return true;
			}

			// %Ns, N one less than the buffer: after white space, 1 to N characters that are not.
			template <std::size_t N>
			bool word(char (&out)[N])
			{
				spaces();
				std::size_t n = 0;
				while (n + 1 < N && peek() >= 0 && !space(peek())) out[n++] = _s[_at++];
				out[n] = 0;
				return n != 0;
			}

			// %d, or %Nd for a width: after white space, a sign and digits within the width. A value a 64-bit long
			// cannot hold is held at its limit, then cut to an int, as glibc's strtol and assignment do.
			bool integer(int& out, const std::size_t width = 0)
			{
				spaces();
				auto left = width == 0 ? std::numeric_limits<std::size_t>::max() : width;
				auto negative = false;

				if (left != 0 && (peek() == '+' || peek() == '-'))
				{
					negative = peek() == '-';
					++_at;
					--left;
				}

				std::uint64_t magnitude = 0;
				auto digits = false;
				auto past = false;

				for (; left != 0 && digit(peek()); ++_at, --left)
				{
					const auto d = static_cast<std::uint64_t>(peek() - '0');
					if (magnitude > (std::numeric_limits<std::uint64_t>::max() - d) / 10) past = true;
					else magnitude = magnitude * 10 + d;
					digits = true;
				}

				if (!digits) return false;

				constexpr auto long_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
				const auto value = negative
					                   ? past || magnitude > long_max + 1
						                     ? std::numeric_limits<std::int64_t>::min()
						                     : static_cast<std::int64_t>(0 - magnitude)
					                   : past || magnitude > long_max
					                   ? std::numeric_limits<std::int64_t>::max()
					                   : static_cast<std::int64_t>(magnitude);
				out = static_cast<int>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
				return true;
			}

			// %lu: after white space, a sign and digits, as glibc's strtoul reads them into a 64-bit unsigned long - a
			// minus sign negating the value there, and a value past what it holds held at its limit - then cut to
			// the 32 bits a rational's terms hold.
			bool unsigned_integer(std::uint32_t& out)
			{
				spaces();
				auto negative = false;

				if (peek() == '+' || peek() == '-')
				{
					negative = peek() == '-';
					++_at;
				}

				std::uint64_t magnitude = 0;
				auto digits = false;
				auto past = false;

				for (; digit(peek()); ++_at)
				{
					const auto d = static_cast<std::uint64_t>(peek() - '0');
					if (magnitude > (std::numeric_limits<std::uint64_t>::max() - d) / 10) past = true;
					else magnitude = magnitude * 10 + d;
					digits = true;
				}

				if (!digits) return false;

				const auto value = past ? std::numeric_limits<std::uint64_t>::max() : negative ? 0 - magnitude : magnitude;
				out = static_cast<std::uint32_t>(value);
				return true;
			}

			// %lg: after white space, a sign, then "nan", "inf" or "infinity", or the longest run that a number could
			// go on from - digits, a point, an exponent, hexadecimal after 0x - read by strtod. As glibc does, a run
			// strtod reads none of fails, and so does a sign or a 0x with nothing after.
			bool real(double& out)
			{
				spaces();
				std::string item;
				auto c = peek();
				if (c < 0) return false;

				const auto signed_item = c == '-' || c == '+';

				if (signed_item)
				{
					item += static_cast<char>(c);
					++_at;
					c = peek();
					if (c < 0) return false;
				}

				if (lower(c) == 'n')
				{
					return take_word("nan", item) && convert(item, out);
				}

				if (lower(c) == 'i')
				{
					if (!take_word("inf", item)) return false;
					if (lower(peek()) == 'i' && !take_word("inity", item)) return false;
					return convert(item, out);
				}

				auto hex = false;
				auto got_digit = false;
				auto got_dot = false;
				auto got_e = false;
				auto exp_char = 'e';

				if (c == '0')
				{
					item += '0';
					++_at;
					c = peek();

					if (lower(c) == 'x')
					{
						item += static_cast<char>(c);
						++_at;
						c = peek();
						hex = true;
						exp_char = 'p';
					}
					else
					{
						got_digit = true;
					}
				}

				for (; c >= 0; ++_at, c = peek())
				{
					if (digit(c) || (hex && !got_e && hex_digit(c)))
					{
						item += static_cast<char>(c);
						got_digit = true;
					}
					else if (got_e && item.back() == exp_char && (c == '-' || c == '+'))
					{
						item += static_cast<char>(c);
					}
					else if (got_digit && !got_e && lower(c) == exp_char)
					{
						item += exp_char;
						got_e = got_dot = true;
					}
					else if (!got_dot && c == '.')
					{
						item += '.';
						got_dot = true;
					}
					else
					{
						break;
					}
				}

				const std::size_t sign = signed_item ? 1 : 0;
				if (item.size() == sign || (hex && item.size() == 2 + sign)) return false;
				return convert(item, out);
			}
		};

		// A month's English name in three letters, of either case: 1 to 12, or 0.
		int month_number(const std::string_view name)
		{
			static constexpr std::string_view names[] = {
				"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
			};

			if (name.size() != 3) return 0;

			for (std::size_t i = 0; i < std::size(names); ++i)
			{
				const auto same = std::ranges::equal(name, names[i], [](const char a, const char b)
				{
					return (a >= 'A' && a <= 'Z' ? static_cast<char>(a | 0x20) : a) == b;
				});

				if (same) return static_cast<int>(i) + 1;
			}

			return 0;
		}

		std::string_view trimmed(std::string_view text)
		{
			constexpr std::string_view white_space{" \n\r\t\f\v\0", 7};
			const auto start = text.find_first_not_of(white_space);
			if (start == std::string_view::npos) return {};
			return text.substr(start, text.find_last_not_of(white_space) - start + 1);
		}
	}

	std::optional<date> parse_photo_date(const std::string_view r)
	{
		if (r.empty()) return {};

		// Each form is tried in turn, and the one that reads sets every field it gives.
		int yyyy = 0, mm = 0, dd = 0, hh = 0, ss = 0, min = 0;
		double sec = 0.0;
		auto success = false;
		auto has_time = true;
		char day_name[4] = {};
		char month_name[4] = {};

		// "%3s %3s %d %d:%d:%d %d", or with a colon before the year: "%3s %3s %d %d:%d:%d: %d".
		const auto camera = [&](const bool colon)
		{
			scanned_text t(r);
			return t.word(day_name) && t.spaces().word(month_name) && t.spaces().integer(dd) && t.spaces().integer(hh) &&
				t.is(':') && t.integer(min) && t.is(':') && t.integer(ss) && (!colon || t.is(':')) && t.spaces().integer(yyyy);
		};

		// "%4d<a>%2d<a>%2d<b>%2d<c>%2d<c>%lg", each separator a character, a space or none.
		const auto numeric = [&](const char a, const char b, const char c)
		{
			const auto separator = [](scanned_text& t, const char s)
			{
				return s == 0 || (s == ' ' ? (t.spaces(), true) : t.is(s));
			};

			scanned_text t(r);
			return t.integer(yyyy, 4) && separator(t, a) && t.integer(mm, 2) && separator(t, a) && t.integer(dd, 2) &&
				separator(t, b) && t.integer(hh, 2) && separator(t, c) && t.integer(min, 2) && separator(t, c) &&
				t.real(sec);
		};

		// "%4d-%2d-%2d", or with the month's name: "%4d-%3s-%2d".
		const auto date_only = [&](const bool named)
		{
			scanned_text t(r);
			return t.integer(yyyy, 4) && t.is('-') && (named ? t.word(month_name) : t.integer(mm, 2)) && t.is('-') &&
				t.integer(dd, 2);
		};

		if (camera(false) || camera(true))
		{
			mm = month_number(month_name);
			sec = ss;
			success = mm != 0;
		}
		else if (numeric('-', ' ', ':') || // "2006-01-14 15:51:31"
			numeric(':', ' ', ':') || // "2006:01:14 15:51:31"
			numeric('-', 'T', ':') || // "2006-01-14T15:51:31"
			numeric(0, 'T', 0) || // "20060114T155131"
			numeric(0, 0, 0)) // "20060114155131"
		{
			success = true;
		}
		else if (date_only(false)) // "2006-01-14"
		{
			hh = 0, min = 0;
			has_time = false;
			success = true;
		}
		else if (date_only(true)) // "2006-jan-14", or a month of none for a name it is not
		{
			mm = month_number(month_name);
			hh = 0, min = 0;
			has_time = false;
			success = true;
		}

		// %lg reads "inf", "nan" and overflowing exponents, which are no clock's seconds.
		if (!success || !std::isfinite(sec) || sec < 0.0 || sec >= 61.0) return {};

		// As Windows' calendar takes a date, each field held in 16 bits and a month or a day of 0 read as 1.
		const auto year = static_cast<std::uint16_t>(yyyy);
		const auto month = static_cast<std::uint16_t>(mm);
		const auto day = static_cast<std::uint16_t>(dd);
		const auto hour = static_cast<std::uint16_t>(hh);
		const auto minute = static_cast<std::uint16_t>(min);
		const auto second = static_cast<std::uint16_t>(static_cast<int>(std::floor(sec + 0.1)));
		const auto checked_month = month == 0 ? 1 : month;
		const auto checked_day = day == 0 ? 1 : day;

		if (year < 1601 || year > 30827 || checked_month > 12 || hour > 23 || minute > 59 || second > 59) return {};

		static constexpr std::uint16_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
		const auto leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
		if (checked_day > days_in_month[checked_month - 1] + (checked_month == 2 && leap ? 1 : 0)) return {};

		// A clock never set.
		if (year < 1800 || ((year == 1970 || year == 1904) && checked_month == 1 && checked_day == 1)) return {};

		date d;
		d.year = year;
		d.month = month;
		d.day = day;
		d.has_time = has_time;
		d.hour = hour;
		d.minute = minute;
		d.second = second;
		return d;
	}

	void read_zone(std::string_view text, date& d) noexcept
	{
		while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) text.remove_suffix(1);

		if (text.size() > 10 && (text.back() == 'Z' || text.back() == 'z'))
		{
			d.zone = date::time_zone::utc;
			d.offset_minutes = 0;
			return;
		}

		if (text.size() < 6) return;

		const auto tail = text.substr(text.size() - 6);
		const auto two_digits = [](const std::string_view s) { return s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9'; };
		const auto value = [](const std::string_view s) { return (s[0] - '0') * 10 + (s[1] - '0'); };

		if ((tail[0] != '+' && tail[0] != '-') || tail[3] != ':') return;
		if (!two_digits(tail.substr(1, 2)) || !two_digits(tail.substr(4, 2))) return;

		const auto hours = value(tail.substr(1, 2));
		const auto minutes = value(tail.substr(4, 2));

		// No zone is further than 14 hours from Greenwich, and 75 minutes is not a reading.
		if (hours > 14 || minutes > 59) return;

		d.zone = date::time_zone::offset;
		d.offset_minutes = (tail[0] == '-' ? -1 : 1) * (hours * 60 + minutes);
	}

	int sub_second_nanoseconds(const std::string_view digits) noexcept
	{
		constexpr auto digits_read = 7;
		std::int64_t value = 0;
		auto used = 0;

		for (const auto c : digits)
		{
			if (c < '0' || c > '9' || used == digits_read) break;
			value = value * 10 + (c - '0');
			++used;
		}

		if (used == 0) return 0;
		for (auto i = used; i < 9; ++i) value *= 10;
		return static_cast<int>(value);
	}

	int leading_int(const std::string_view text) noexcept
	{
		auto value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		return error == std::errc{} ? value : 0;
	}

	bool is_number(const std::string_view text) noexcept
	{
		auto digits = 0;

		for (const auto c : text)
		{
			if (c >= '0' && c <= '9') ++digits;
			else if (c != '.' && c != ',') return false;
		}

		return digits > 0;
	}

	std::pair<int, int> parse_number_pair(const std::string_view text) noexcept
	{
		if (is_number(text)) return {leading_int(text), 0};

		// "%d/%d"
		scanned_text t(text);
		auto of = 0;
		auto total = 0;
		if (t.integer(of) && t.is('/') && t.integer(total)) return {of, total};
		return {0, 0};
	}

	std::optional<std::pair<std::uint32_t, std::uint32_t>> parse_photo_rational(const std::string_view text) noexcept
	{
		// "%lu/%lu%c": two items, or one and a denominator of 1; a third - anything after the rational - fails it.
		scanned_text t(text);
		std::uint32_t numerator = 0;
		std::uint32_t denominator = 1;
		if (!t.unsigned_integer(numerator)) return {};

		if (t.is('/'))
		{
			std::uint32_t read = 0;

			if (t.unsigned_integer(read))
			{
				if (!t.at_end()) return {};
				denominator = read;
			}
		}

		if (denominator == 0) return {};
		return std::pair{numerator, denominator};
	}

	int round_to_int(const double value) noexcept
	{
		if (!std::isnormal(value)) return 0;
		const auto rounded = std::round(value);

		// What MSVC's conversion makes of a value past an int's range.
		if (!(rounded >= -2147483648.0 && rounded < 2147483648.0)) return std::numeric_limits<int>::min();
		return static_cast<int>(rounded);
	}

	std::optional<double> parse_photo_coordinate(const std::string_view raw, const bool latitude) noexcept
	{
		const auto text = trimmed(raw);
		const auto len = text.size();
		if (len < 4) return {};

		const auto reference = text[len - 1];
		const auto north = reference == 'N' || reference == 'n';
		const auto south = reference == 'S' || reference == 's';
		const auto east = reference == 'E' || reference == 'e';
		const auto west = reference == 'W' || reference == 'w';
		if (latitude ? !(north || south) : !(east || west)) return {};

		const auto body = text.substr(0, len - 1);
		const auto first_comma = body.find(',');
		if (first_comma == std::string_view::npos) return {};

		const auto second_comma = body.find(',', first_comma + 1);
		if (second_comma != std::string_view::npos && body.find(',', second_comma + 1) != std::string_view::npos) return {};

		const auto part = [](const std::string_view s, double& out)
		{
			if (s.empty()) return false;
			const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), out);
			return error == std::errc{} && end == s.data() + s.size() && out >= 0.0;
		};

		auto degrees = 0.0;
		auto minutes = 0.0;
		auto seconds = 0.0;
		if (!part(body.substr(0, first_comma), degrees)) return {};

		if (second_comma == std::string_view::npos)
		{
			if (!part(body.substr(first_comma + 1), minutes)) return {};
		}
		else
		{
			if (!part(body.substr(first_comma + 1, second_comma - first_comma - 1), minutes)) return {};
			if (!part(body.substr(second_comma + 1), seconds)) return {};
		}

		if (minutes >= 60.0 || seconds >= 60.0) return {};

		const auto magnitude = degrees + minutes / 60.0 + seconds / 3600.0;
		const auto coordinate = south || west ? -magnitude : magnitude;
		if (latitude ? !(std::abs(coordinate) <= 90.0) : !(std::abs(coordinate) < 180.0)) return {};
		return coordinate;
	}
}
