// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The two pieces of zlib that metadata reading needs: inflating a zlib stream, for PNG's
// compressed text and colour profiles, and CRC-32, for PNG's chunk checks. The inflater decodes a
// bit at a time in the manner of Mark Adler's reference decoder, puff: metadata streams are a few
// kilobytes, so clarity wins over speed. What it accepts is what zlib accepts - complete code sets,
// no distance reaching before the output, and a matching Adler-32 - so a stream that zlib rejects
// is rejected here too.

#include "scan_internal.h"

#include <array>

namespace libfiles::detail
{
	namespace
	{
		constexpr int max_bits = 15;
		constexpr int max_literal_codes = 286;
		constexpr int max_distance_codes = 30;
		constexpr int fixed_literal_codes = 288;

		struct inflate_failure
		{
		};

		// Thrown when a bounded inflate has produced all it was asked for.
		struct output_full
		{
		};

		struct huffman
		{
			std::array<short, max_bits + 1> count{};
			std::array<short, fixed_literal_codes> symbol{};
		};

		struct inflate_state
		{
			std::span<const std::uint8_t> in;
			std::size_t in_pos = 0;
			std::uint32_t bit_buffer = 0;
			int bit_count = 0;
			std::vector<std::uint8_t>& out;
			std::size_t max_output = 0;
			// Reaching max_output ends the inflate rather than failing it.
			bool stop_when_full = false;
		};

		int bits(inflate_state& s, const int need)
		{
			auto value = s.bit_buffer;

			while (s.bit_count < need)
			{
				if (s.in_pos == s.in.size()) throw inflate_failure{};
				value |= static_cast<std::uint32_t>(s.in[s.in_pos++]) << s.bit_count;
				s.bit_count += 8;
			}

			s.bit_buffer = value >> need;
			s.bit_count -= need;
			return static_cast<int>(value & ((1u << need) - 1u));
		}

		void put(inflate_state& s, const std::uint8_t b)
		{
			if (s.out.size() >= s.max_output)
			{
				if (s.stop_when_full) throw output_full{};
				throw inflate_failure{};
			}

			s.out.push_back(b);
		}

		void stored(inflate_state& s)
		{
			// A stored block starts on a byte boundary; the rest of the current byte is padding.
			s.bit_buffer = 0;
			s.bit_count = 0;

			if (s.in.size() - s.in_pos < 4) throw inflate_failure{};

			const auto len = static_cast<std::size_t>(s.in[s.in_pos] | s.in[s.in_pos + 1] << 8);
			const auto nlen = static_cast<std::size_t>(s.in[s.in_pos + 2] | s.in[s.in_pos + 3] << 8);
			s.in_pos += 4;

			if (len != (~nlen & 0xffffu)) throw inflate_failure{};

			// Byte by byte, so a bounded inflate stops exactly where it is full and a stored block cut
			// short yields what it holds before it fails.
			for (std::size_t i = 0; i < len; ++i)
			{
				if (s.in_pos == s.in.size()) throw inflate_failure{};
				put(s, s.in[s.in_pos++]);
			}
		}

		int decode(inflate_state& s, const huffman& h)
		{
			auto code = 0;
			auto first = 0;
			auto index = 0;

			for (auto len = 1; len <= max_bits; ++len)
			{
				code |= bits(s, 1);
				const auto count = h.count[len];
				if (code - count < first) return h.symbol[index + (code - first)];
				index += count;
				first += count;
				first <<= 1;
				code <<= 1;
			}

			throw inflate_failure{};
		}

		// Builds the decoding tables for a canonical code from its code lengths. Returns 0 for a
		// complete code, a positive count for an incomplete one and a negative for an over-subscribed one.
		int construct(huffman& h, const short* length, const int n)
		{
			h.count.fill(0);

			for (auto symbol = 0; symbol < n; ++symbol) ++h.count[length[symbol]];

			if (h.count[0] == n) return 0;

			auto left = 1;

			for (auto len = 1; len <= max_bits; ++len)
			{
				left <<= 1;
				left -= h.count[len];
				if (left < 0) return left;
			}

			std::array<short, max_bits + 1> offsets{};

			for (auto len = 1; len < max_bits; ++len)
			{
				offsets[len + 1] = static_cast<short>(offsets[len] + h.count[len]);
			}

			for (auto symbol = 0; symbol < n; ++symbol)
			{
				if (length[symbol] != 0) h.symbol[offsets[length[symbol]]++] = static_cast<short>(symbol);
			}

			return left;
		}

		void codes(inflate_state& s, const huffman& literal_code, const huffman& distance_code)
		{
			static constexpr short length_base[29] = {
				3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
				35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
			};
			static constexpr short length_extra[29] = {
				0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
			};
			static constexpr short distance_base[30] = {
				1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
				257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
			};
			static constexpr short distance_extra[30] = {
				0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
			};

			for (;;)
			{
				auto symbol = decode(s, literal_code);

				if (symbol < 256)
				{
					put(s, static_cast<std::uint8_t>(symbol));
				}
				else if (symbol == 256)
				{
					return;
				}
				else
				{
					symbol -= 257;
					if (symbol >= 29) throw inflate_failure{};

					const auto len = static_cast<std::size_t>(length_base[symbol] + bits(s, length_extra[symbol]));

					symbol = decode(s, distance_code);
					if (symbol >= 30) throw inflate_failure{};

					const auto distance = static_cast<std::size_t>(distance_base[symbol] + bits(s, distance_extra[symbol]));

					// There is no preset dictionary, so a distance may reach back only into what has
					// been written.
					if (distance > s.out.size()) throw inflate_failure{};

					for (std::size_t i = 0; i < len; ++i)
					{
						const auto b = s.out[s.out.size() - distance];
						put(s, b);
					}
				}
			}
		}

		const std::pair<huffman, huffman>& fixed_codes()
		{
			static const auto tables = []
			{
				std::pair<huffman, huffman> result;
				std::array<short, fixed_literal_codes> lengths{};

				for (auto symbol = 0; symbol < fixed_literal_codes; ++symbol)
				{
					lengths[symbol] = static_cast<short>(symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8);
				}

				construct(result.first, lengths.data(), fixed_literal_codes);

				lengths.fill(5);
				construct(result.second, lengths.data(), max_distance_codes);
				return result;
			}();

			return tables;
		}

		void dynamic(inflate_state& s)
		{
			static constexpr short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

			std::array<short, max_literal_codes + max_distance_codes> lengths{};

			const auto literal_count = bits(s, 5) + 257;
			const auto distance_count = bits(s, 5) + 1;
			const auto code_count = bits(s, 4) + 4;

			if (literal_count > max_literal_codes || distance_count > max_distance_codes) throw inflate_failure{};

			auto index = 0;
			for (; index < code_count; ++index) lengths[order[index]] = static_cast<short>(bits(s, 3));
			for (; index < 19; ++index) lengths[order[index]] = 0;

			huffman literal_code;
			huffman distance_code;

			// The code-length code must be complete.
			if (construct(literal_code, lengths.data(), 19) != 0) throw inflate_failure{};

			index = 0;

			while (index < literal_count + distance_count)
			{
				auto symbol = decode(s, literal_code);

				if (symbol < 16)
				{
					lengths[index++] = static_cast<short>(symbol);
				}
				else
				{
					short repeat_length = 0;

					if (symbol == 16)
					{
						if (index == 0) throw inflate_failure{};
						repeat_length = lengths[index - 1];
						symbol = 3 + bits(s, 2);
					}
					else if (symbol == 17)
					{
						symbol = 3 + bits(s, 3);
					}
					else
					{
						symbol = 11 + bits(s, 7);
					}

					if (index + symbol > literal_count + distance_count) throw inflate_failure{};
					while (symbol--) lengths[index++] = repeat_length;
				}
			}

			// Without an end-of-block code no block can end.
			if (lengths[256] == 0) throw inflate_failure{};

			// An incomplete code is allowed only when it is a single code of one bit, as zlib allows.
			auto err = construct(literal_code, lengths.data(), literal_count);
			if (err && (err < 0 || literal_count != literal_code.count[0] + literal_code.count[1])) throw inflate_failure{};

			err = construct(distance_code, lengths.data() + literal_count, distance_count);
			if (err && (err < 0 || distance_count != distance_code.count[0] + distance_code.count[1]))
				throw inflate_failure{};

			codes(s, literal_code, distance_code);
		}

		std::uint32_t adler32(const std::span<const std::uint8_t> data)
		{
			std::uint32_t a = 1;
			std::uint32_t b = 0;

			for (const auto byte : data)
			{
				a = (a + byte) % 65521u;
				b = (b + a) % 65521u;
			}

			return b << 16 | a;
		}

		// Deflate with a window of at most 32K, a valid check, and no preset dictionary.
		bool valid_zlib_header(const std::span<const std::uint8_t> in)
		{
			if (in.size() < 2) return false;

			const auto cmf = in[0];
			const auto flg = in[1];
			return (cmf & 0x0f) == 8 && (cmf >> 4) <= 7 && (cmf << 8 | flg) % 31 == 0 && (flg & 0x20) == 0;
		}

		void inflate_blocks(inflate_state& s)
		{
			auto last = 0;

			do
			{
				last = bits(s, 1);

				switch (bits(s, 2))
				{
				case 0: stored(s);
					break;
				case 1: codes(s, fixed_codes().first, fixed_codes().second);
					break;
				case 2: dynamic(s);
					break;
				default: throw inflate_failure{};
				}
			}
			while (!last);
		}
	}

	bool zlib_inflate(const std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out,
	                  const std::size_t max_output)
	{
		out.clear();

		if (!valid_zlib_header(in)) return false;

		inflate_state s{in.subspan(2), 0, 0, 0, out, max_output};

		try
		{
			inflate_blocks(s);
		}
		catch (const inflate_failure&)
		{
			return false;
		}

		// The Adler-32 follows on the next byte boundary, which is where the unread input begins.
		if (s.in.size() - s.in_pos < 4) return false;
		return load_be32(s.in.data() + s.in_pos) == adler32(out);
	}

	bool zlib_inflate_prefix(const std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out,
	                         const std::size_t length)
	{
		out.clear();

		if (!valid_zlib_header(in)) return false;

		inflate_state s{in.subspan(2), 0, 0, 0, out, length};
		s.stop_when_full = true;

		try
		{
			inflate_blocks(s);
		}
		catch (const output_full&)
		{
		}
		catch (const inflate_failure&)
		{
			return false;
		}

		return out.size() == length;
	}

	std::uint32_t crc32(std::uint32_t crc, const std::span<const std::uint8_t> data) noexcept
	{
		static const auto table = []
		{
			std::array<std::uint32_t, 256> result{};

			for (std::uint32_t n = 0; n < 256; ++n)
			{
				auto c = n;
				for (auto k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
				result[n] = c;
			}

			return result;
		}();

		crc = ~crc;
		for (const auto byte : data) crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
		return ~crc;
	}
}
