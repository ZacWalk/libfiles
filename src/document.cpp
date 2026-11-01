// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Native image dispatch and portable pixel-storage sizes; a document decoded whole, and any
// document's rows as BGRA.

#include "document_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace libfiles
{
	std::uint32_t storage_bits(const document_storage format) noexcept
	{
		switch (format)
		{
		case document_storage::indexed1: return 1;
		case document_storage::indexed2: return 2;
		case document_storage::indexed4: return 4;
		case document_storage::indexed8:
		case document_storage::indexed8_alpha:
		case document_storage::gray8: return 8;
		case document_storage::bgr555:
		case document_storage::bgr565: return 16;
		case document_storage::bgr24: return 24;
		default: return 32;
		}
	}

	std::size_t palette_entries(const document_storage format) noexcept
	{
		switch (format)
		{
		case document_storage::indexed1: return 2;
		case document_storage::indexed2: return 4;
		case document_storage::indexed4: return 16;
		case document_storage::indexed8:
		case document_storage::indexed8_alpha: return 256;
		default: return 0;
		}
	}

	std::size_t document_row_bytes(const std::uint32_t width, const document_storage format)
	{
		const auto size = ((static_cast<std::uint64_t>(width) * storage_bits(format) + 31) / 32) * 4;
		if (size > std::numeric_limits<std::size_t>::max()) throw std::bad_alloc();
		return static_cast<std::size_t>(size);
	}

	document_result decode_document(reader& input, const image_format format, document_sink& output,
	                                const document_options& options)
	{
		document_result result;
		detail::document_decode decode{input, output, options, result, {}};
		try
		{
			decode.check_cancel();
			switch (format)
			{
			case image_format::pcx: detail::document_pcx(decode); break;
			case image_format::bmp: detail::document_bmp(decode); break;
			case image_format::wpg: detail::document_wpg(decode); break;
			case image_format::psd: detail::document_psd(decode); break;
#ifdef LIBFILES_JPEG
			case image_format::jpeg: detail::document_jpeg(decode); break;
#endif
#ifdef LIBFILES_PNG
			case image_format::png: detail::document_png(decode); break;
#endif
#ifdef LIBFILES_GIF
			case image_format::gif: detail::document_gif(decode); break;
#endif
#ifdef LIBFILES_TIFF
			case image_format::tiff: detail::document_tiff(decode); break;
#endif
			default: decode.fail("this kind of native image is not decoded by libfiles");
			}
			result.success = true;
		}
		catch (const detail::document_stop&) {}
		catch (const read_error& error)
		{
			result.truncated = true;
			result.error = error.what();
		}
		catch (const std::bad_alloc&)
		{
			result.resource_limit = true;
			result.error = "not enough memory to decode the image";
		}
		return result;
	}

	document_result decode_document(reader& input, const image_format format, decoded_document& output,
	                                const document_options& options)
	{
		document_keeper keeper;
		auto result = decode_document(input, format, keeper, options);
		keeper.document.info = result.info;
		output = std::move(keeper.document);
		return result;
	}

	bool document_keeper::begin_frame(const document_frame& frame)
	{
		decoded_frame kept;
		kept.left = frame.left;
		kept.top = frame.top;
		kept.width = frame.width;
		kept.height = frame.height;
		kept.storage = frame.storage;
		kept.stride = document_row_bytes(frame.width, frame.storage);
		if (frame.height != 0 && kept.stride > std::numeric_limits<std::size_t>::max() / frame.height) throw std::bad_alloc();
		kept.pixels.resize(kept.stride * frame.height);
		kept.palette.assign(frame.palette.begin(), frame.palette.end());
		kept.transparent = frame.transparent;
		kept.background = frame.background;
		kept.delay_ms = frame.delay_ms;
		kept.disposal = frame.disposal;
		document.frames.push_back(std::move(kept));
		return true;
	}

	void document_keeper::row(const std::uint32_t y, const std::span<const std::uint8_t> bytes)
	{
		auto& frame = document.frames.back();
		if (y >= frame.height) return;
		std::memcpy(frame.pixels.data() + y * frame.stride, bytes.data(), std::min(bytes.size(), frame.stride));
	}

	void document_keeper::metadata(const document_metadata kind, const std::span<const std::uint8_t> bytes)
	{
		document.metadata.push_back({kind, {bytes.begin(), bytes.end()}});
	}

	pixel_rows decoded_frame::rows() const noexcept
	{
		return {width, height, storage, stride, false, pixels, palette, transparent};
	}

	void copy_bgra(const pixel_rows& source, const std::span<std::uint32_t> destination, const std::size_t stride,
	               const bool bottom_up)
	{
		const auto width = static_cast<std::size_t>(source.width);
		const auto height = static_cast<std::size_t>(source.height);

		if (static_cast<unsigned>(source.storage) > static_cast<unsigned>(document_storage::bgra32) || width == 0 ||
			height == 0 || source.stride == 0 || source.stride % 4 != 0 ||
			source.stride < document_row_bytes(source.width, source.storage) ||
			source.stride > source.pixels.size() / height)
		{
			throw std::invalid_argument("Invalid source pixel storage.");
		}

		if (stride < width || stride > std::numeric_limits<std::size_t>::max() / height ||
			destination.size() < stride * height)
		{
			throw std::invalid_argument("Invalid destination pixel storage.");
		}

		const auto in_at = reinterpret_cast<std::uintptr_t>(source.pixels.data());
		const auto out_at = reinterpret_cast<std::uintptr_t>(destination.data());
		if (in_at < out_at + destination.size_bytes() && out_at < in_at + source.pixels.size())
		{
			throw std::invalid_argument("Source and destination pixel storage overlap.");
		}

		const auto indexed = palette_entries(source.storage) != 0;
		const auto read32 = [](const std::uint8_t* p)
		{
			return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
				static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
		};

		for (std::size_t y = 0; y < height; ++y)
		{
			const auto* const in = source.pixels.data() + (source.bottom_up ? height - 1 - y : y) * source.stride;
			auto* const out = destination.data() + (bottom_up ? height - 1 - y : y) * stride;

			for (std::size_t x = 0; x < width; ++x)
			{
				std::uint32_t value = 0;

				switch (source.storage)
				{
				case document_storage::indexed1: value = in[x >> 3] >> (7 - (x & 7)) & 1u; break;
				case document_storage::indexed2: value = in[x >> 2] >> (6 - 2 * (x & 3)) & 3u; break;
				case document_storage::indexed4: value = x & 1 ? in[x >> 1] & 0x0fu : in[x >> 1] >> 4u; break;
				case document_storage::indexed8:
				case document_storage::indexed8_alpha: value = in[x]; break;
				case document_storage::gray8: value = in[x] * 0x010101u; break;
				case document_storage::bgr555:
				{
					const auto v = static_cast<std::uint32_t>(in[2 * x] | in[2 * x + 1] << 8);
					value = (v & 0x001fu) << 3 | (v & 0x03e0u) << 6 | (v & 0x7c00u) << 9;
					break;
				}
				case document_storage::bgr565:
				{
					const auto v = static_cast<std::uint32_t>(in[2 * x] | in[2 * x + 1] << 8);
					value = (v & 0x001fu) << 3 | (v & 0x07e0u) << 5 | (v & 0xf800u) << 8;
					break;
				}
				case document_storage::bgr24: value = in[3 * x] | in[3 * x + 1] << 8 | in[3 * x + 2] << 16; break;
				case document_storage::bgrx32:
				case document_storage::bgra32: value = read32(in + 4 * x); break;
				}

				if (indexed)
				{
					if (value >= source.palette.size()) throw std::invalid_argument("Image palette index is out of range.");
					const auto index = value;
					value = source.palette[index];
					if (source.transparent && index == *source.transparent) value &= 0x00ffffffu;
				}
				else if (source.storage != document_storage::bgra32)
				{
					value |= 0xff000000u;
				}

				out[x] = value;
			}
		}
	}
}

namespace libfiles::detail
{
	void native_lab_to_bgr(const int lightness, const int a, const int b, std::uint8_t* output)
	{
		double fy = std::pow((lightness + 16.0) / 116.0, 3.0);
		if (fy < 0.008856) fy = lightness / 903.3;
		double y = fy;
		fy = fy > 0.008856 ? std::pow(fy, 1.0 / 3.0) : 7.787 * fy + 16.0 / 116.0;
		const double fx = a / 500.0 + fy;
		double x = fx > 0.206893 ? std::pow(fx, 3.0) : (fx - 16.0 / 116.0) / 7.787;
		const double fz = fy - b / 200.0;
		double z = fz > 0.206893 ? std::pow(fz, 3.0) : (fz - 16.0 / 116.0) / 7.787;
		x *= 0.950456 * 255;
		y *= 255;
		z *= 1.088754 * 255;
		const auto byte = [](const double value)
		{
			return static_cast<std::uint8_t>(std::clamp(static_cast<int>(value + 0.5), 0, 255));
		};
		output[0] = byte(0.055648 * x - 0.204043 * y + 1.057311 * z);
		output[1] = byte(-0.969256 * x + 1.875992 * y + 0.041556 * z);
		output[2] = byte(3.240479 * x - 1.537150 * y - 0.498535 * z);
	}

	document_storage storage_from_bits(const std::uint32_t bits) noexcept
	{
		switch (bits)
		{
		case 1: return document_storage::indexed1;
		case 2: return document_storage::indexed2;
		case 4: return document_storage::indexed4;
		case 8: return document_storage::indexed8;
		case 15: return document_storage::bgr555;
		case 16: return document_storage::bgr555;
		case 24: return document_storage::bgr24;
		default: return document_storage::bgrx32;
		}
	}
}
