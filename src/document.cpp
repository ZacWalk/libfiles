// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Native image dispatch and portable pixel-storage sizes.

#include "document_internal.h"

#include <new>
#include <cmath>

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

	bool can_decode_document(const image_format format) noexcept
	{
#ifdef LIBFILES_JPEG
		if (format == image_format::jpeg) return true;
#endif
#ifdef LIBFILES_PNG
		if (format == image_format::png) return true;
#endif
#ifdef LIBFILES_GIF
		if (format == image_format::gif) return true;
#endif
#ifdef LIBFILES_TIFF
		if (format == image_format::tiff) return true;
#endif
		return format == image_format::pcx || format == image_format::bmp ||
			format == image_format::wpg || format == image_format::psd;
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
