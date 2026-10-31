// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Building audio files and their tags byte by byte - ID3v2 tags and frames, MPEG audio frames,
// FLAC metadata blocks, Vorbis comments and AIFF forms - shared by the scan, write and agreement tests, so
// a scanner's tests and its writer's tests read the same files.

#pragma once

#include "builders.h"

#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace test_build
{
	inline void put_be24(builder& b, const std::uint32_t v)
	{
		b.u8(v >> 16).u8(v >> 8).u8(v);
	}

	inline void put_syncsafe(builder& b, const std::uint64_t v)
	{
		b.u8(v >> 21 & 0x7F).u8(v >> 14 & 0x7F).u8(v >> 7 & 0x7F).u8(v & 0x7F);
	}

	// ---- ID3v2 ---------------------------------------------------------------------------------------

	// A frame: its size syncsafe in ID3v2.4 unless plain_size, as iTunes wrote some, plain in ID3v2.3, and
	// 24 bits in ID3v2.2, which has no flags.
	inline bytes id3_frame(const unsigned version, const std::string_view id, const bytes& body, const std::uint16_t flags = 0,
	                       const bool plain_size = false)
	{
		builder b;
		b.raw(id);
		const auto size = static_cast<std::uint32_t>(body.size());
		if (version == 2) put_be24(b, size);
		else if (version == 4 && !plain_size) put_syncsafe(b, size);
		else b.be32(size);
		if (version != 2) b.be16(flags);
		b.raw(body);
		return b.b;
	}

	// A text frame in Latin-1.
	inline bytes id3_text_frame(const unsigned version, const std::string_view id, const std::string_view text)
	{
		return id3_frame(version, id, builder().u8(0).raw(text).b);
	}

	// A PRIV frame - PRV in ID3v2.2 - of an owner and its data.
	inline bytes id3_private_frame(const unsigned version, const std::string_view owner, const bytes& data, const std::uint16_t flags = 0)
	{
		return id3_frame(version, version == 2 ? "PRV" : "PRIV", builder().raw(owner).u8(0).raw(data).b, flags);
	}

	// A tag of frames and padding. Its header's flags as given: 0x80 unsynchronisation, 0x40 an extended
	// header, 0x10 a footer.
	inline bytes id3_tag(const unsigned version, const std::vector<bytes>& frames, const std::size_t padding = 0, const std::uint8_t flags = 0)
	{
		builder body;
		for (const auto& f : frames) body.raw(f);
		body.zeros(padding);

		builder b;
		b.raw("ID3").u8(version).u8(0).u8(flags);
		put_syncsafe(b, body.size());
		b.raw(body.b);
		return b.b;
	}

	// The bytes with an 0x00 put after each 0xFF, as ID3 unsynchronisation stores them.
	inline bytes unsynchronised(const bytes& in)
	{
		bytes out;
		for (const auto c : in)
		{
			out.push_back(c);
			if (c == 0xFF) out.push_back(0);
		}

		return out;
	}

	// A zlib stream that stores its data uncompressed in one block, which inflates to the same bytes.
	inline bytes zlib_stored_block(const bytes& data)
	{
		std::uint32_t a = 1;
		std::uint32_t b = 0;

		for (const auto byte : data)
		{
			a = (a + byte) % 65521;
			b = (b + a) % 65521;
		}

		const auto length = static_cast<unsigned>(data.size());
		return builder().u8(0x78).u8(0x01).u8(0x01).le16(length).le16(~length & 0xFFFF).raw(data).be32(b << 16 | a).b;
	}

	// An ID3v2.3 frame compressed as its flags say: the inflated size, then a zlib stream.
	inline bytes id3_compressed_frame(const std::string_view id, const bytes& data)
	{
		return id3_frame(3, id, builder().be32(static_cast<std::uint32_t>(data.size())).raw(zlib_stored_block(data)).b, 0x0080);
	}

	// ---- MPEG audio ----------------------------------------------------------------------------------

	// MPEG-1 Layer III at 128 kbit/s and 48 kHz, joint stereo: 384 bytes and 1152 samples a frame.
	inline constexpr std::uint32_t mp3_frame_header = 0xFFFB9440;
	inline constexpr std::size_t mp3_frame_size = 384;

	inline bytes mp3_frames(const std::size_t count)
	{
		builder b;
		for (std::size_t i = 0; i < count; ++i) b.be32(mp3_frame_header).zeros(mp3_frame_size - 4);
		return b.b;
	}

	// ---- FLAC ----------------------------------------------------------------------------------------

	inline bytes flac_streaminfo(const std::uint32_t rate, const unsigned channels, const unsigned bits, const std::uint64_t total)
	{
		builder b;
		b.be16(4096).be16(4096);
		put_be24(b, 0);
		put_be24(b, 0);
		const auto packed = std::uint64_t{rate} << 44 | std::uint64_t{channels - 1} << 41 | std::uint64_t{bits - 1} << 36 | total;
		b.be32(static_cast<std::uint32_t>(packed >> 32)).be32(static_cast<std::uint32_t>(packed)).zeros(16);
		return b.b;
	}

	inline bytes flac_block(const unsigned type, const bytes& data, const bool last = false)
	{
		builder b;
		b.u8((last ? 0x80 : 0) | type);
		put_be24(b, static_cast<std::uint32_t>(data.size()));
		b.raw(data);
		return b.b;
	}

	inline bytes vorbis_comment_block(const std::vector<std::string_view>& fields, const std::string_view vendor = "libfiles test")
	{
		builder b;
		b.le32(static_cast<std::uint32_t>(vendor.size())).raw(vendor).le32(static_cast<std::uint32_t>(fields.size()));
		for (const auto f : fields) b.le32(static_cast<std::uint32_t>(f.size())).raw(f);
		return b.b;
	}

	// ---- AIFF ----------------------------------------------------------------------------------------

	inline bytes aiff_chunk(const std::string_view id, const bytes& payload)
	{
		builder b;
		b.raw(id).be32(static_cast<std::uint32_t>(payload.size())).raw(payload);
		if (payload.size() & 1) b.u8(0);
		return b.b;
	}

	inline bytes aiff_form(const std::string_view type, const std::vector<bytes>& chunks)
	{
		builder body;
		body.raw(type);
		for (const auto& c : chunks) body.raw(c);
		return builder().raw("FORM").be32(static_cast<std::uint32_t>(body.size())).raw(body.b).b;
	}

	// The COMM chunk: channels, sample frames, sample size, and the rate as an IEEE 754 80-bit extended number.
	inline bytes aiff_comm(const unsigned channels, const std::uint32_t frames, const unsigned bits, const double rate)
	{
		auto exponent = 0;
		const auto significand = static_cast<std::uint64_t>(std::ldexp(std::frexp(rate, &exponent), 64));
		return aiff_chunk("COMM", builder().be16(channels).be32(frames).be16(bits).be16(static_cast<unsigned>(exponent - 1 + 16383))
		                                   .be32(static_cast<std::uint32_t>(significand >> 32)).be32(static_cast<std::uint32_t>(significand)).b);
	}

	// The toolkit's packet chunk: APPL signed "XMP ".
	inline bytes aiff_xmp_chunk(const std::string_view packet)
	{
		return aiff_chunk("APPL", builder().raw("XMP ").raw(packet).b);
	}
}
