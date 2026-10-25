// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: MD5 (RFC 1321). Photoshop stores the MD5 of the IPTC block beside it, and an Adobe reader
// that finds the two disagree takes the IPTC as newer than the XMP, so an IPTC write updates it.

#include "legacy.h"

#include <cstring>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint32_t k[64] = {
			0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
			0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
			0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
			0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
			0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
			0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
			0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
			0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
		};

		constexpr int shifts[64] = {
			7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
			5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
			4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
			6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
		};

		std::uint32_t rotate_left(const std::uint32_t x, const int c)
		{
			return x << c | x >> (32 - c);
		}

		void transform(std::uint32_t state[4], const std::uint8_t block[64])
		{
			std::uint32_t m[16];
			for (auto i = 0; i < 16; ++i) m[i] = load_le32(block + i * 4);

			auto a = state[0];
			auto b = state[1];
			auto c = state[2];
			auto d = state[3];

			for (auto i = 0; i < 64; ++i)
			{
				std::uint32_t f = 0;
				auto g = 0;

				if (i < 16)
				{
					f = (b & c) | (~b & d);
					g = i;
				}
				else if (i < 32)
				{
					f = (d & b) | (~d & c);
					g = (5 * i + 1) % 16;
				}
				else if (i < 48)
				{
					f = b ^ c ^ d;
					g = (3 * i + 5) % 16;
				}
				else
				{
					f = c ^ (b | ~d);
					g = (7 * i) % 16;
				}

				const auto next = d;
				d = c;
				c = b;
				b = b + rotate_left(a + f + k[i] + m[g], shifts[i]);
				a = next;
			}

			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
		}
	}

	std::array<std::uint8_t, 16> md5(const std::span<const std::uint8_t> data) noexcept
	{
		std::uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

		std::size_t i = 0;
		for (; i + 64 <= data.size(); i += 64) transform(state, data.data() + i);

		// The tail, a 1 bit, zeros, and the length in bits - one or two final blocks.
		std::uint8_t tail[128] = {};
		const auto rest = data.size() - i;
		if (rest) std::memcpy(tail, data.data() + i, rest);
		tail[rest] = 0x80;

		const auto tail_blocks = rest < 56 ? 1u : 2u;
		const auto bits = static_cast<std::uint64_t>(data.size()) * 8u;
		for (auto b = 0; b < 8; ++b) tail[tail_blocks * 64 - 8 + b] = static_cast<std::uint8_t>(bits >> (8 * b));

		for (auto b = 0u; b < tail_blocks; ++b) transform(state, tail + b * 64);

		std::array<std::uint8_t, 16> digest{};
		for (auto w = 0; w < 4; ++w)
		{
			for (auto b = 0; b < 4; ++b) digest[w * 4 + b] = static_cast<std::uint8_t>(state[w] >> (8 * b));
		}

		return digest;
	}
}
