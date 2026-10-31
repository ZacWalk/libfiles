// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The decode framework: dispatch to each format's decoder, the decoder that keeps codec state
// between images, and the failures every decoder reports the same way.

#include "decode_internal.h"

#include <new>

namespace libfiles
{
	std::uint32_t scale_denominator_for(const std::uint32_t source_width, const std::uint32_t source_height,
	                                    const std::uint32_t target_width, const std::uint32_t target_height) noexcept
	{
		if (target_width == 0 && target_height == 0) return 1;

		for (const std::uint32_t d : {8u, 4u, 2u})
		{
			if (source_width / d >= target_width && source_height / d >= target_height) return d;
		}

		return 1;
	}

	image_size scaled_size_for(const std::uint32_t source_width, const std::uint32_t source_height,
	                           const std::uint32_t target_width, const std::uint32_t target_height) noexcept
	{
		const image_size source{source_width, source_height};
		if (source_width == 0 || source_height == 0 || target_width == 0 || target_height == 0) return source;

		// n * num / den rounded to the nearest, half up, as df::mul_div rounds.
		const auto mul_div = [](const std::int64_t n, const std::int64_t num, const std::int64_t den)
		{
			return (n * num + den / 2) / den;
		};

		constexpr std::int64_t denominator = 100000;
		const auto scale = std::min(mul_div(target_width, denominator, source_width),
		                            mul_div(target_height, denominator, source_height));
		const auto width = std::max<std::int64_t>(1, mul_div(source_width, scale, denominator));
		const auto height = std::max<std::int64_t>(1, mul_div(source_height, scale, denominator));

		if (width < source_width || height < source_height)
			return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};

		return source;
	}

	decoder::decoder() : _state(std::make_unique<state>())
	{
	}

	decoder::~decoder() = default;

	decode_result decoder::decode(reader& r, image_sink& sink, const decode_options& options)
	{
		decode_result result;

		try
		{
			std::uint8_t head[16] = {};
			const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(r.size(), sizeof(head)));
			r.read(0, head, n);

			const auto format = options.source_format == image_format::unknown
				? detect_image_format({head, n}) : options.source_format;
			switch (format)
			{
			case image_format::psd: detail::decode_psd(r, sink, result);
				break;
#ifdef LIBFILES_JPEG
			case image_format::jpeg: detail::decode_jpeg(r, sink, options, _state->jpeg, result);
				break;
#endif
#ifdef LIBFILES_PNG
			case image_format::png: detail::decode_png(r, sink, result);
				break;
#endif
#ifdef LIBFILES_WEBP
			case image_format::webp: detail::decode_webp(r, sink, options, result);
				break;
#endif
#ifdef LIBFILES_HEIF
			case image_format::heif: detail::decode_heif(r, sink, options, result);
				break;
#endif
#ifdef LIBFILES_JXL
			case image_format::jxl: detail::decode_jxl(r, sink, result);
				break;
#endif
			default:
				result.error = "this kind of image is not decoded by libfiles";
				break;
			}
		}
		catch (const read_error&)
		{
			result = {};
			result.truncated = true;
			result.error = "the file is cut short";
		}
		catch (const std::bad_alloc&)
		{
			result = {};
			result.error = "out of memory";
		}

		return result;
	}

	decode_result decode(reader& r, image_sink& sink, const decode_options& options)
	{
		decoder d;
		return d.decode(r, sink, options);
	}
}
