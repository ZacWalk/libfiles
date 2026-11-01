// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Building test files byte by byte, shared by the scan and write tests, so every field a test
// reads back is one it put there.

#pragma once

#include "scan_internal.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace test_build
{
	using bytes = std::vector<std::uint8_t>;

	struct builder
	{
		bytes b;

		builder& raw(const std::string_view s)
		{
			// Byte by byte: GCC 13, optimizing vector::insert into an empty vector, warns of a memmove from null.
			for (const auto c : s) b.push_back(static_cast<std::uint8_t>(c));
			return *this;
		}

		builder& raw(const bytes& v)
		{
			b.insert(b.end(), v.begin(), v.end());
			return *this;
		}

		builder& u8(const unsigned v)
		{
			b.push_back(static_cast<std::uint8_t>(v));
			return *this;
		}

		builder& be16(const unsigned v) { return u8(v >> 8).u8(v); }
		builder& le16(const unsigned v) { return u8(v).u8(v >> 8); }
		builder& be32(const std::uint32_t v) { return be16(v >> 16).be16(v & 0xffff); }
		builder& le32(const std::uint32_t v) { return le16(v & 0xffff).le16(v >> 16); }

		builder& zeros(const std::size_t n)
		{
			b.insert(b.end(), n, 0);
			return *this;
		}

		std::size_t size() const { return b.size(); }
	};

	inline bytes to_bytes(const std::string_view s)
	{
		return {s.begin(), s.end()};
	}

	// Every character of a literal, NULs included.
	template <std::size_t N>
	bytes lit(const char (&s)[N])
	{
		return bytes(s, s + N - 1);
	}

	inline std::string to_string(const std::vector<std::uint8_t>& v)
	{
		return {v.begin(), v.end()};
	}

	inline void png_chunk(builder& p, const std::string_view type, const bytes& data, const bool damage = false)
	{
		p.be32(static_cast<std::uint32_t>(data.size()));
		const auto start = p.size();
		p.raw(type).raw(data);
		auto crc = libfiles::detail::crc32(0, std::span(p.b).subspan(start));
		if (damage) crc ^= 1;
		p.be32(crc);
	}

	inline bytes png_ihdr(const std::uint32_t width, const std::uint32_t height, const unsigned color_type)
	{
		return builder().be32(width).be32(height).u8(8).u8(color_type).u8(0).u8(0).u8(0).b;
	}

	inline void riff_chunk(builder& w, const std::string_view fourcc, const bytes& data)
	{
		w.raw(fourcc).le32(static_cast<std::uint32_t>(data.size())).raw(data);
		if (data.size() & 1) w.u8(0);
	}

	inline bytes finish_riff(const builder& body)
	{
		builder w;
		w.raw("RIFF").le32(static_cast<std::uint32_t>(body.size() + 4)).raw("WEBP").raw(body.b);
		return w.b;
	}

	// A VP8 key frame header for a width x height image.
	inline bytes vp8_frame(const unsigned width, const unsigned height)
	{
		// Key frame, version 0, shown, first partition 16 bytes.
		const std::uint32_t bits = 0u | (0u << 1) | (1u << 4) | (16u << 5);
		return builder().u8(bits & 0xff).u8((bits >> 8) & 0xff).u8(bits >> 16).u8(0x9d).u8(0x01).u8(0x2a)
		                .le16(width).le16(height).zeros(200).b;
	}

	// A VP8L header for a width x height image.
	inline bytes vp8l_frame(const unsigned width, const unsigned height, const bool alpha)
	{
		const auto bits = (width - 1) | (height - 1) << 14 | (alpha ? 1u : 0u) << 28;
		return builder().u8(0x2f).le32(bits).zeros(8).b;
	}

	inline bytes vp8x(const std::uint32_t flags, const unsigned width, const unsigned height)
	{
		const auto w = width - 1;
		const auto h = height - 1;
		return builder().le32(flags).u8(w & 0xff).u8((w >> 8) & 0xff).u8(w >> 16).u8(h & 0xff).u8((h >> 8) & 0xff).u8(h >> 16).b;
	}

	// An ISO base media box, as HEIF and the JPEG XL container are made of.
	inline bytes iso_box(const std::string_view type, const bytes& payload)
	{
		return builder().be32(static_cast<std::uint32_t>(payload.size() + 8)).raw(type).raw(payload).b;
	}

	inline bytes iso_full_box(const std::string_view type, const unsigned version, const std::uint32_t flags, const bytes& payload)
	{
		return iso_box(type, builder().u8(version).u8(flags >> 16).u8(flags >> 8).u8(flags).raw(payload).b);
	}

	// Bits written least significant first, as a JPEG XL codestream holds them.
	struct bit_writer
	{
		bytes b;
		std::size_t bits = 0;

		bit_writer& put(const std::uint64_t value, const unsigned count)
		{
			for (unsigned i = 0; i < count; ++i, ++bits)
			{
				if (bits % 8 == 0) b.push_back(0);
				b.back() = static_cast<std::uint8_t>(b.back() | ((value >> i) & 1u) << (bits % 8));
			}

			return *this;
		}

		bit_writer& pad()
		{
			while (bits % 8) put(0, 1);
			return *this;
		}
	};

	// --- JPEG XL

	struct jxl_header
	{
		std::uint32_t width = 64;
		std::uint32_t height = 48;
		unsigned orientation = 1;
		bool grey = false;
		bool alpha = false;
		bool premultiplied = false;
		bool small = false;
		// Alpha channels with names of name_length bytes, in place of the one alpha says.
		unsigned named_channels = 0;
		unsigned name_length = 48;
	};

	// A codestream: its signature, size header, image metadata and transform data, then bytes that
	// stand for the frames, which a scan never reads.
	inline bytes codestream(const jxl_header& h)
	{
		bit_writer w;
		w.put(0xFF, 8).put(0x0A, 8);

		if (h.small)
		{
			w.put(1, 1).put(h.height / 8 - 1, 5).put(0, 3).put(h.width / 8 - 1, 5);
		}
		else
		{
			w.put(0, 1).put(1, 2).put(h.height - 1, 13).put(0, 3).put(1, 2).put(h.width - 1, 13);
		}

		// Not all default, with the extra fields: the orientation, and no size, preview or animation.
		w.put(0, 1).put(1, 1).put(h.orientation - 1, 3).put(0, 1).put(0, 1).put(0, 1);
		// Eight-bit integer samples, and the modular buffer flag.
		w.put(0, 1).put(0, 2).put(1, 1);

		if (h.named_channels > 0)
		{
			// Alpha, eight bits, full size, and a name of 48 to 1071 bytes.
			w.put(3, 2).put(h.named_channels - 1, 12);

			for (unsigned c = 0; c < h.named_channels; ++c)
			{
				w.put(0, 1).put(0, 2).put(0, 1).put(0, 2).put(0, 2).put(3, 2).put(h.name_length - 48, 10);
				for (unsigned i = 0; i < h.name_length; ++i) w.put('a' + i % 26, 8);
				w.put(0, 1);
			}
		}
		else if (h.alpha)
		{
			// One extra channel: alpha, eight bits, full size, unnamed.
			w.put(1, 2).put(0, 1).put(0, 2).put(0, 1).put(0, 2).put(0, 2).put(0, 2).put(h.premultiplied ? 1 : 0, 1);
		}
		else
		{
			w.put(0, 2);
		}

		w.put(1, 1);

		if (h.grey)
		{
			// Grey, D65, the sRGB transfer function, relative intent.
			w.put(0, 1).put(0, 1).put(1, 2).put(1, 2).put(0, 1).put(2, 2).put(13 - 2, 4).put(1, 2);
		}
		else
		{
			w.put(1, 1);
		}

		// Default tone mapping, no extensions, default transform data.
		w.put(1, 1).put(0, 2).put(1, 1).pad();
		w.b.insert(w.b.end(), 48, 0x5A);
		return w.b;
	}

	inline const bytes jxl_signature = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};

	inline bytes jxl_ftyp() { return iso_box("ftyp", builder().raw("jxl ").be32(0).raw("jxl ").b); }

	inline bytes jxlp(const std::uint32_t index, const bool last, const std::span<const std::uint8_t> part)
	{
		return iso_box("jxlp", builder().be32(index | (last ? 0x80000000u : 0u)).raw(bytes(part.begin(), part.end())).b);
	}

	// A stand-in for Brotli: "Z" then the bytes as they are.
	inline bytes brob(const std::string_view type, const bytes& contents)
	{
		return iso_box("brob", builder().raw(type).raw("Z").raw(contents).b);
	}

	inline std::optional<bytes> fake_brotli(const std::span<const std::uint8_t> compressed, std::string_view, const std::size_t max_size)
	{
		if (compressed.empty() || compressed[0] != 'Z' || compressed.size() - 1 > max_size) return {};
		return bytes(compressed.begin() + 1, compressed.end());
	}

	inline libfiles::scan_limits with_brotli()
	{
		libfiles::scan_limits limits;
		limits.brotli = fake_brotli;
		return limits;
	}

	inline bytes container(const std::initializer_list<bytes> boxes)
	{
		builder file;
		file.raw(jxl_signature).raw(jxl_ftyp());
		for (const auto& box : boxes) file.raw(box);
		return file.b;
	}
}
