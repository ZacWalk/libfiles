// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Text rules shared by the readers: whether bytes are UTF-8, what a Windows-1252 byte
// stands for, and a code point written as UTF-8.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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
}
