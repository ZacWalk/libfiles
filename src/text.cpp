// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Text rules shared by every reader and writer: whether bytes are UTF-8, what a Windows-1252 byte
// stands for, a code point written as UTF-8, UTF-16 read and written, and text as tags and blocks store it
// made UTF-8.

#include "text_internal.h"

#include <algorithm>

namespace libfiles::detail
{
	bool is_utf8(const std::uint8_t* p, const std::size_t size)
	{
		static constexpr int byte_class[256] = {
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
			2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
			3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
			3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
			4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
			5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
			6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 7,
			9, 10, 10, 10, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
		};

		enum state : int { start, a, b, c, d, e, f, g, error, state_count };

		static constexpr state transitions[] = {
			start, error, error, error, error, error, error, error, error,
			error, start, a, error, a, b, error, b, error,
			error, start, a, error, a, b, b, error, error,
			error, start, a, a, error, b, b, error, error,
			error, error, error, error, error, error, error, error, error,
			a, error, error, error, error, error, error, error, error,
			c, error, error, error, error, error, error, error, error,
			b, error, error, error, error, error, error, error, error,
			d, error, error, error, error, error, error, error, error,
			f, error, error, error, error, error, error, error, error,
			e, error, error, error, error, error, error, error, error,
			g, error, error, error, error, error, error, error, error,
		};

		auto current = start;
		const auto* const limit = p + size;

		while (p < limit && current != error)
		{
			current = transitions[byte_class[*p++] * state_count + current];
		}

		return current == start;
	}

	char32_t windows_1252(const std::uint8_t byte)
	{
		static constexpr char16_t high[32] = {
			0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
			0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
			0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
		};

		return byte >= 0x80 && byte < 0xA0 ? high[byte - 0x80] : byte;
	}

	void append_utf8(std::string& out, const char32_t cp)
	{
		if (cp < 0x80)
		{
			out += static_cast<char>(cp);
		}
		else if (cp < 0x800)
		{
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	std::string utf8_from_utf16(const std::u16string_view units)
	{
		std::string result;
		result.reserve(units.size() * 3);

		for (std::size_t i = 0; i < units.size();)
		{
			char32_t cp = units[i++];

			if (cp >= 0xD800 && cp <= 0xDBFF)
			{
				if (i < units.size() && units[i] >= 0xDC00 && units[i] <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i++] - 0xDC00);
				else cp = 0xFFFD;
			}
			else if (cp >= 0xDC00 && cp <= 0xDFFF)
			{
				cp = 0xFFFD;
			}

			append_utf8(result, cp);
		}

		return result;
	}

	std::vector<std::uint8_t> utf16le(const std::string_view text)
	{
		std::vector<std::uint8_t> out;
		const auto* const s = reinterpret_cast<const std::uint8_t*>(text.data());
		std::size_t i = 0;

		const auto put = [&out](const std::uint32_t unit)
		{
			out.push_back(static_cast<std::uint8_t>(unit));
			out.push_back(static_cast<std::uint8_t>(unit >> 8));
		};

		while (i < text.size())
		{
			const auto c = s[i];
			std::uint32_t cp = 0xFFFD;
			std::size_t n = 1;

			if (c < 0x80) cp = c;
			else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) { cp = (c & 0x1Fu) << 6 | (s[i + 1] & 0x3F); n = 2; }
			else if ((c & 0xF0) == 0xE0 && i + 2 < text.size())
			{
				cp = (c & 0x0Fu) << 12 | (s[i + 1] & 0x3Fu) << 6 | (s[i + 2] & 0x3F);
				n = 3;
			}
			else if ((c & 0xF8) == 0xF0 && i + 3 < text.size())
			{
				cp = (c & 0x07u) << 18 | (s[i + 1] & 0x3Fu) << 12 | (s[i + 2] & 0x3Fu) << 6 | (s[i + 3] & 0x3F);
				n = 4;
			}

			if (cp >= 0x10000)
			{
				cp -= 0x10000;
				put(0xD800 + (cp >> 10));
				put(0xDC00 + (cp & 0x3FF));
			}
			else
			{
				put(cp);
			}

			i += n;
		}

		return out;
	}

	std::string text_from_utf8_or_1252(const std::span<const std::uint8_t> bytes)
	{
		if (bytes.empty() || is_utf8(bytes.data(), bytes.size())) return {bytes.begin(), bytes.end()};

		std::string result;
		result.reserve(bytes.size() + bytes.size() / 2);
		for (const auto b : bytes) append_utf8(result, windows_1252(b));
		return result;
	}

	namespace
	{
		// The bytes before the first NUL.
		std::span<const std::uint8_t> before_nul(const std::span<const std::uint8_t> bytes)
		{
			const auto end = std::ranges::find(bytes, std::uint8_t{0});
			return bytes.first(static_cast<std::size_t>(end - bytes.begin()));
		}
	}

	std::string text_from_windows_1252(const std::span<const std::uint8_t> bytes)
	{
		std::string out;
		for (const auto b : before_nul(bytes)) append_utf8(out, windows_1252(b));
		return out;
	}

	std::string text_from_utf8(const std::span<const std::uint8_t> bytes)
	{
		const auto text = before_nul(bytes);
		if (is_utf8(text.data(), text.size())) return {text.begin(), text.end()};

		// Lenient: each byte that does not begin a well-formed sequence stands for U+FFFD.
		std::string out;
		std::size_t i = 0;

		while (i < text.size())
		{
			const auto c = text[i];
			const std::size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;

			if (length != 0 && i + length <= text.size() && is_utf8(text.data() + i, length))
			{
				out.append(reinterpret_cast<const char*>(text.data() + i), length);
				i += length;
			}
			else
			{
				append_utf8(out, 0xFFFD);
				++i;
			}
		}

		return out;
	}

	std::string text_from_utf16(std::span<const std::uint8_t> bytes, bool big_endian)
	{
		if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
		{
			big_endian = true;
			bytes = bytes.subspan(2);
		}
		else if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
		{
			big_endian = false;
			bytes = bytes.subspan(2);
		}

		std::u16string units;

		for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
		{
			const auto unit = static_cast<char16_t>(big_endian ? bytes[i] << 8 | bytes[i + 1] : bytes[i + 1] << 8 | bytes[i]);
			if (unit == 0) break;
			units.push_back(unit);
		}

		return utf8_from_utf16(units);
	}

	std::string text_from_unknown(const std::span<const std::uint8_t> bytes)
	{
		return text_from_utf8_or_1252(before_nul(bytes));
	}

	std::string_view trim_text(std::string_view text) noexcept
	{
		constexpr std::string_view space = std::string_view(" \t\r\n\0", 5);
		const auto first = text.find_first_not_of(space);
		if (first == std::string_view::npos) return {};
		const auto last = text.find_last_not_of(space);
		return text.substr(first, last - first + 1);
	}
}
