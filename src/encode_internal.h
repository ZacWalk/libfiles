// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What the JPEG, PNG and WebP encoders share: the signatures metadata blocks begin with, and
// the EXIF block written when an image has no EXIF but an orientation.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace libfiles::encode_detail
{
	using namespace std::string_view_literals;

	inline constexpr auto xmp_signature = "http://ns.adobe.com/xap/1.0/\0"sv;
	inline constexpr auto icc_signature = "ICC_PROFILE\0"sv;
	inline constexpr std::array<std::uint8_t, 6> exif_signature = {0x45, 0x78, 0x69, 0x66, 0x00, 0x00};
	inline constexpr auto photoshop_signature = "Photoshop 3.0\0"sv;

	inline bool is_exif_signature(const std::span<const std::uint8_t> block)
	{
		return block.size() >= exif_signature.size() &&
			std::memcmp(exif_signature.data(), block.data(), exif_signature.size()) == 0;
	}

	// One IFD holding the orientation and nothing after it, without the "Exif\0\0" signature.
	inline std::vector<std::uint8_t> orientation_exif(const std::uint16_t orientation)
	{
		return {
			0x49, 0x49,
			0x2A, 0x00,
			0x08, 0x00, 0x00, 0x00, // ifd0 offset
			0x01, 0x00, // ifd0 entrycount
			0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, static_cast<std::uint8_t>(orientation), 0x00, 0x00, 0x00,
			// orientation
			0x00, 0x00, 0x00, 0x00, // no next ifd
		};
	}

	// Runs a cleanup when it goes out of scope, whichever way.
	template <typename F>
	class on_exit
	{
		F _f;

	public:
		explicit on_exit(F f) : _f(std::move(f))
		{
		}

		~on_exit() { _f(); }

		on_exit(const on_exit&) = delete;
		on_exit& operator=(const on_exit&) = delete;
	};
}
