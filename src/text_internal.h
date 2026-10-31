// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Text rules shared by every reader and writer: whether bytes are UTF-8, what a Windows-1252 byte
// stands for, a code point written as UTF-8, UTF-16 read and written, and text as tags and blocks store it
// made UTF-8 - so that EXIF, IPTC, XMP and every container's tags decode the same bytes the same way.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	// Whether the bytes are well-formed UTF-8: no overlong forms, surrogates or code points past
	// U+10FFFF, and no sequence cut short.
	bool is_utf8(const std::uint8_t* p, std::size_t size);

	// The character a Windows-1252 byte stands for: from 0x80 to 0x9F the punctuation and letters
	// Windows puts there - the euro, curly quotes, dashes - but for the five bytes it leaves undefined,
	// which stay the C1 controls, as the WHATWG decoder reads them; every other byte is its Latin-1
	// character.
	char32_t windows_1252(std::uint8_t byte);

	void append_utf8(std::string& out, char32_t cp);

	// UTF-16 code units as UTF-8, a lone surrogate becoming U+FFFD. Every unit is converted, a NUL included.
	std::string utf8_from_utf16(std::u16string_view units);

	// UTF-8 as UTF-16LE bytes, a malformed sequence becoming U+FFFD. No terminator.
	std::vector<std::uint8_t> utf16le(std::string_view text);

	// Bytes as they are when they are well-formed UTF-8, else each byte as Windows-1252 reads it: the rule
	// for text that does not say how it is coded, IPTC's and many tags'. Every byte, a NUL included.
	std::string text_from_utf8_or_1252(std::span<const std::uint8_t> bytes);

	// Text as tags store it, made UTF-8. Each stops at the first NUL. A malformed sequence becomes U+FFFD.
	std::string text_from_windows_1252(std::span<const std::uint8_t> bytes);
	std::string text_from_utf8(std::span<const std::uint8_t> bytes);
	// A byte-order mark, when there is one, overrides big_endian and is dropped.
	std::string text_from_utf16(std::span<const std::uint8_t> bytes, bool big_endian);
	// text_from_utf8_or_1252 up to the first NUL: the rule for tags that do not say.
	std::string text_from_unknown(std::span<const std::uint8_t> bytes);

	// Leading and trailing spaces, tabs, line breaks and NULs removed.
	std::string_view trim_text(std::string_view text) noexcept;
}
