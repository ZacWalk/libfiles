// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Encoding a PNG with libpng, its ICC profile, XMP and EXIF included. Moved from Diffractor's
// files_png.cpp.

#include "libfiles/encode.h"
#include "encode_internal.h"

#include <png.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace libfiles
{
	using namespace encode_detail;

	namespace
	{
		[[noreturn]] void PNGCBAPI png_error_handler(png_structp, const png_const_charp msg)
		{
			throw encode_error(msg);
		}

		// libpng's warnings go to the notes for a log rather than its default handler's stderr.
		void PNGCBAPI png_warning_handler(const png_structp png_ptr, const png_const_charp msg)
		{
			static_cast<std::vector<std::string>*>(png_get_error_ptr(png_ptr))->emplace_back(msg);
		}

		constexpr auto png_xmp_key = "XML:com.adobe.xmp";

		// png_destroy_write_struct only frees the info struct when it is passed in. Destroying the png
		// struct alone leaks the info struct and everything it owns - the iTXt (XMP), the iCCP profile and
		// eXIf - on every image. This owns both so that cannot drift apart, and so the pair survives the
		// C++ exceptions the error handler throws through libpng's C frames.
		class png_writer
		{
			png_structp _png = nullptr;
			png_infop _info = nullptr;

		public:
			explicit png_writer(std::vector<std::string>& notes)
			{
				_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &notes, png_error_handler, png_warning_handler);

				if (_png)
				{
					_info = png_create_info_struct(_png);
				}

				if (!_png || !_info)
				{
					png_destroy_write_struct(&_png, &_info);
					throw encode_error("could not create png writer");
				}
			}

			~png_writer()
			{
				png_destroy_write_struct(&_png, &_info);
			}

			png_writer(const png_writer&) = delete;
			png_writer& operator=(const png_writer&) = delete;

			png_structp get() const { return _png; }
			png_infop info() const { return _info; }
		};

		void PNGCBAPI png_write_callback(const png_structp png_ptr, const png_bytep data, const png_size_t length)
		{
			auto* p = static_cast<std::vector<std::uint8_t>*>(png_get_io_ptr(png_ptr));
			p->insert(p->end(), data, data + length);
		}
	}

	encoded encode_png(const image_view& image, const std::uint16_t orientation, const embedded_metadata& metadata)
	{
		encoded result;
		const png_writer png(result.notes);
		auto* const info_ptr = png.info();

		const auto is_rgb = image.format == pixel_format::bgrx8;

		png_set_write_fn(png.get(), &result.data, png_write_callback, nullptr);

		png_set_IHDR(png.get(), info_ptr, image.width, image.height, 8,
		             is_rgb ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGBA,
		             PNG_INTERLACE_NONE,
		             PNG_COMPRESSION_TYPE_DEFAULT,
		             PNG_FILTER_TYPE_DEFAULT);

		if (!metadata.icc.empty())
		{
			png_set_iCCP(png.get(), info_ptr, "icc", PNG_COMPRESSION_TYPE_BASE, metadata.icc.data(),
			             static_cast<png_uint_32>(metadata.icc.size()));
		}

		if (!metadata.xmp.empty())
		{
			png_text text_metadata = {};
			std::vector<std::uint8_t> xmp_text(metadata.xmp.begin(), metadata.xmp.end());
			xmp_text.push_back(0);

			text_metadata.compression = PNG_ITXT_COMPRESSION_NONE;
			text_metadata.key = const_cast<png_charp>(png_xmp_key);
			text_metadata.text = reinterpret_cast<png_charp>(xmp_text.data());
			png_set_text(png.get(), info_ptr, &text_metadata, 1);
		}

		if (!metadata.exif.empty())
		{
			const auto exif_skip = is_exif_signature(metadata.exif) ? exif_signature.size() : 0u;
			png_set_eXIf_1(png.get(), info_ptr, static_cast<png_uint_32>(metadata.exif.size() - exif_skip),
			               const_cast<png_bytep>(metadata.exif.data()) + exif_skip);
		}
		else if (orientation != 1)
		{
			auto exif = orientation_exif(orientation);
			const auto exif_skip = is_exif_signature(exif) ? exif_signature.size() : 0u;
			png_set_eXIf_1(png.get(), info_ptr, static_cast<png_uint_32>(exif.size() - exif_skip),
			               exif.data() + exif_skip);
		}

		std::vector<std::uint8_t*> rows(image.height);

		for (std::uint32_t y = 0; y < image.height; ++y)
		{
			rows[y] = const_cast<std::uint8_t*>(image.pixels) + y * image.stride;
		}

		png_set_compression_level(png.get(), 6);
		png_set_rows(png.get(), info_ptr, rows.data());

		png_write_png(png.get(), info_ptr,
		              PNG_TRANSFORM_BGR | (is_rgb ? PNG_TRANSFORM_STRIP_FILLER_AFTER : PNG_TRANSFORM_IDENTITY),
		              nullptr);

		return result;
	}
}
