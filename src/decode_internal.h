// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What the format decoders share and do not export: each format's decoder and the cursor
// they read headers with.

#pragma once

#include "libfiles/decode.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace libfiles::detail
{
	// A position in a file, read forward. A read or skip past the end throws read_error, which
	// decode() reports as a file cut short.
	struct cursor
	{
		reader& r;
		std::uint64_t pos = 0;

		std::uint64_t remaining() const noexcept { return r.size() - pos; }

		std::uint8_t u8()
		{
			const auto v = r.u8(pos);
			pos += 1;
			return v;
		}

		std::uint16_t be16()
		{
			const auto v = r.be16(pos);
			pos += 2;
			return v;
		}

		std::uint32_t be32()
		{
			const auto v = r.be32(pos);
			pos += 4;
			return v;
		}

		std::uint16_t le16()
		{
			const auto v = r.le16(pos);
			pos += 2;
			return v;
		}

		std::uint32_t le32()
		{
			const auto v = r.le32(pos);
			pos += 4;
			return v;
		}

		void read(std::uint8_t* const data, const std::size_t size)
		{
			r.read(pos, data, size);
			pos += size;
		}

		void skip(const std::uint64_t size)
		{
			if (size > remaining()) throw read_error("a skip runs past the end of the file");
			pos += size;
		}

		void seek(const std::uint64_t to)
		{
			if (to > r.size()) throw read_error("a seek runs past the end of the file");
			pos = to;
		}
	};

	void decode_psd(reader& r, image_sink& sink, decode_result& result);
	void decode_png(reader& r, image_sink& sink, decode_result& result);
	void decode_webp(reader& r, image_sink& sink, const decode_options& options, decode_result& result);
	void decode_heif(reader& r, image_sink& sink, const decode_options& options, decode_result& result);
	void decode_jxl(reader& r, image_sink& sink, decode_result& result);

	// libjpeg's decompressor, kept by a decoder between images.
	struct jpeg_state;

	struct jpeg_state_deleter
	{
		void operator()(jpeg_state* state) const noexcept;
	};

	using jpeg_state_ptr = std::unique_ptr<jpeg_state, jpeg_state_deleter>;

	void decode_jpeg(reader& r, image_sink& sink, const decode_options& options, jpeg_state_ptr& state,
	                 decode_result& result);
}

struct libfiles::decoder::state
{
	detail::jpeg_state_ptr jpeg;
};
