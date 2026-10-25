// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Decoding a PNG with libpng into memory the caller provides, as B8G8R8A8: every colour type
// and bit depth brought to 8 bits a channel with an alpha channel - opaque where the file has none -
// and corrected for an sRGB display when the file gives its gamma and no sRGB chunk. Moved from
// Diffractor's loader unchanged, call for call.

#include "decode_internal.h"
#include "document_internal.h"

#include "libfiles/exif.h"
#include "libfiles/scan.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint8_t png_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

		// libpng's fatal errors leave it by this exception, which decode_png catches.
		struct png_failure
		{
			std::string message;
		};

		// What libpng's callbacks reach through their pointers: the file, read forward from just past the
		// signature, and the warnings libpng gives as it goes.
		struct png_source
		{
			reader& r;
			std::uint64_t pos = sizeof(png_signature);
			std::vector<std::string>& warnings;
			std::jmp_buf* jump = nullptr;
			char message[256]{};
			bool truncated = false;
			bool resource_limit = false;
		};

		[[noreturn]] void PNGCBAPI on_error(png_structp png, const png_const_charp message)
		{
			auto& source = *static_cast<png_source*>(png_get_error_ptr(png));
			if (source.jump)
			{
				std::snprintf(source.message, sizeof(source.message), "%s", message && *message ? message : "libpng failed");
				std::longjmp(*source.jump, 1);
			}
			throw png_failure{message && *message ? message : "libpng failed"};
		}

		// Benign errors arrive here too: a chunk whose CRC is wrong, say, which libpng then skips.
		void PNGCBAPI on_warning(const png_structp png, const png_const_charp message)
		{
			auto& source = *static_cast<png_source*>(png_get_error_ptr(png));
			try
			{
				source.warnings.emplace_back(message ? message : "");
				return;
			}
			catch (const std::bad_alloc&)
			{
				if (!source.jump) throw;
				source.resource_limit = true;
				std::snprintf(source.message, sizeof(source.message), "not enough memory for PNG diagnostics");
			}
			std::longjmp(*source.jump, 1);
		}

		// A read past the end throws read_error, which leaves libpng as its own errors do.
		void PNGCBAPI on_read(const png_structp png, const png_bytep data, const size_t length)
		{
			auto* const source = static_cast<png_source*>(png_get_io_ptr(png));
			try
			{
				source->r.read(source->pos, data, length);
				source->pos += length;
				return;
			}
			catch (const read_error& error)
			{
				if (!source->jump) throw;
				source->truncated = true;
				std::snprintf(source->message, sizeof(source->message), "%s", error.what());
			}
			catch (const std::bad_alloc&)
			{
				if (!source->jump) throw;
				source->resource_limit = true;
				std::snprintf(source->message, sizeof(source->message), "not enough memory to read the PNG");
			}
			std::longjmp(*source->jump, 1);
		}

		// png_destroy_read_struct frees the info struct only when it is given it: destroying the png struct
		// alone would leak the info struct and all it owns - text, the colour profile, eXIf, the palette.
		struct png_reader
		{
			png_structp png = nullptr;
			png_infop info = nullptr;

			explicit png_reader(png_source& source)
			{
				png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &source, on_error, on_warning);

				if (png)
				{
					info = png_create_info_struct(png);
					png_set_benign_errors(png, 1);
				}
			}

			~png_reader()
			{
				if (png) static_cast<png_source*>(png_get_error_ptr(png))->jump = nullptr;
				png_destroy_read_struct(&png, &info, nullptr);
			}

			png_reader(const png_reader&) = delete;
			png_reader& operator=(const png_reader&) = delete;
		};

		// Everything libpng reads, in a function with nothing of its own to destroy: libpng's errors are
		// thrown through its C frames, which a compiler told that C functions never throw need not unwind
		// a caller's objects across. The rows and the reader live in decode_png, which catches.
		void read_image(png_reader& p, png_source& source, image_sink& sink, frame_info& frame,
		                std::vector<png_bytep>& rows, decode_result& result)
		{
			png_set_read_fn(p.png, &source, on_read);
			png_set_sig_bytes(p.png, sizeof(png_signature));
			png_read_info(p.png, p.info);

			png_uint_32 width = 0, height = 0;
			int bit_depth = 0, color_type = 0;
			png_get_IHDR(p.png, p.info, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

			frame.source_width = width;
			frame.source_height = height;
			frame.width = width;
			frame.height = height;
			frame.format = pixel_format::bgra8;

			// Asked as soon as the header has given the size, before anything more is read: a caller that
			// cannot afford the image refuses it from the header alone.
			const auto pixels = sink.allocate(frame);
			frame.exif = {};

			if (!pixels.pixels)
			{
				result.refused = true;
				return;
			}

			if (bit_depth == 16)
				png_set_scale_16(p.png);

			// A PNG that declares its own transfer function has to be converted for an sRGB display, or it
			// shows washed out or over-contrasted. Without gAMA the file is already taken to be sRGB.
			double file_gamma = 0.0;

			if (png_get_valid(p.png, p.info, PNG_INFO_sRGB) == 0 &&
				png_get_gAMA(p.png, p.info, &file_gamma) != 0 && file_gamma > 0.0)
			{
				png_set_gamma(p.png, 2.2, file_gamma);
			}

			if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
			{
				png_set_expand_gray_1_2_4_to_8(p.png);
			}

			if (color_type == PNG_COLOR_TYPE_PALETTE)
			{
				png_set_palette_to_rgb(p.png);
			}

			if (png_get_valid(p.png, p.info, PNG_INFO_tRNS) != 0)
			{
				png_set_tRNS_to_alpha(p.png);
			}

			if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
			{
				png_set_gray_to_rgb(p.png);
			}

			// The filler is a fourth channel for a colour type without alpha, so every image comes out as
			// four bytes a pixel.
			if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_RGB ||
				color_type == PNG_COLOR_TYPE_PALETTE)
			{
				png_set_filler(p.png, 0xFF, PNG_FILLER_AFTER);
			}

			png_set_bgr(p.png);
			png_read_update_info(p.png, p.info);

			if (png_get_channels(p.png, p.info) != 4 || png_get_rowbytes(p.png, p.info) > pixels.stride)
			{
				result.error = "a decoded PNG row is not the size the image was given";
				return;
			}

			rows.resize(height);

			for (png_uint_32 y = 0; y < height; ++y)
			{
				rows[y] = pixels.pixels + static_cast<std::size_t>(y) * pixels.stride;
			}

			// An interlaced file is read with libpng's interlace handling turned on here rather than before
			// png_read_update_info, which libpng puts right and warns of.
			png_read_image(p.png, rows.data());
			result.success = true;
		}
	}

	void decode_png(reader& r, image_sink& sink, decode_result& result)
	{
		std::uint8_t signature[sizeof(png_signature)] = {};
		if (r.size() >= sizeof(signature)) r.read(0, signature, sizeof(signature));

		if (!std::equal(std::begin(signature), std::end(signature), std::begin(png_signature)))
		{
			result.error = "the PNG signature is missing or damaged";
			return;
		}

		// The orientation comes from the scan indexing uses: the last EXIF in the file, which may follow
		// the image data, beyond what libpng reads before the pixels.
		const auto scanned = scan(r);
		frame_info frame;
		frame.orientation = summarize_exif(scanned.metadata.exif).orientation.value_or(1);
		frame.exif = scanned.metadata.exif;

		png_source source{r, sizeof(png_signature), result.warnings};
		png_reader p(source);

		if (!p.png || !p.info)
		{
			result.error = "could not create a PNG reader";
			return;
		}

		std::vector<png_bytep> rows;

		try
		{
			read_image(p, source, sink, frame, rows, result);
		}
		catch (const png_failure& failure)
		{
			result.success = false;
			result.error = failure.message;
		}
		catch (const read_error&)
		{
			// Caught here rather than by decode(), so the warnings given before the end stay.
			result.success = false;
			result.truncated = true;
			result.error = "the file is cut short";
		}

		frame.exif = {};
		result.frame = frame;
	}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611) // all owning objects live outside the setjmp frame
#endif
	static void read_document_png(document_decode& decode, png_source& source, png_reader& owner,
		std::vector<std::uint8_t>& rows, std::array<std::uint32_t, 256>& palette)
	{
		std::jmp_buf jump;
		source.jump = &jump;
		if (setjmp(jump))
		{
			decode.result.truncated = source.truncated;
			decode.result.resource_limit = source.resource_limit;
			decode.fail(source.message);
		}
		{
			const auto png = owner.png;
			const auto info = owner.info;
			png_set_read_fn(png, &source, on_read);
			png_set_sig_bytes(png, static_cast<int>(sizeof(png_signature)));
			png_read_info(png, info);
			png_uint_32 width = 0, height = 0;
			int depth = 0, color = 0;
			png_get_IHDR(png, info, &width, &height, &depth, &color, nullptr, nullptr, nullptr);
			auto& properties = decode.result.info;
			properties.width = width;
			properties.height = height;
			properties.bits_per_pixel = depth * png_get_channels(png, info);

			// Native documents keep their stored samples, not the display decoder's gamma mapping.
			png_set_strip_16(png);
			if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
			if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
			png_set_bgr(png);
			const auto passes = png_set_interlace_handling(png);
			properties.interlace_passes = static_cast<std::uint8_t>(passes);
			png_read_update_info(png, info);
			const auto bits = png_get_bit_depth(png, info) * png_get_channels(png, info);
			properties.storage = storage_from_bits(bits);
			document_frame frame;
			frame.width = width;
			frame.height = height;
			frame.storage = properties.storage;
			frame.sequential = passes == 1;

			if (color == PNG_COLOR_TYPE_PALETTE && png_get_valid(png, info, PNG_INFO_PLTE))
			{
				png_colorp colors = nullptr;
				int count = 0;
				png_get_PLTE(png, info, &colors, &count);
				const auto entries = palette_entries(frame.storage);
				for (int i = 0; i < count && static_cast<std::size_t>(i) < entries; ++i)
					palette[i] = 0xff000000u | (colors[i].red << 16) | (colors[i].green << 8) | colors[i].blue;
				if (png_get_valid(png, info, PNG_INFO_tRNS))
				{
					png_bytep alpha = nullptr;
					png_color_16p transparent = nullptr;
					int count_alpha = 0;
					png_get_tRNS(png, info, &alpha, &count_alpha, &transparent);
					for (int i = 0; i < count_alpha && static_cast<std::size_t>(i) < entries; ++i)
						palette[i] = (palette[i] & 0xffffff) | (static_cast<std::uint32_t>(alpha[i]) << 24);
					if (frame.storage == document_storage::indexed8) frame.storage = document_storage::indexed8_alpha;
				}
				frame.palette = {palette.data(), entries};
			}
			else if (color == PNG_COLOR_TYPE_RGB_ALPHA || color == PNG_COLOR_TYPE_GRAY_ALPHA)
			{
				frame.storage = document_storage::bgra32;
			}

			png_uint_32 density_x = 0, density_y = 0;
			int unit = 0;
			if (png_get_pHYs(png, info, &density_x, &density_y, &unit) && unit == PNG_RESOLUTION_METER)
			{
				properties.has_resolution = true;
				properties.pixels_per_meter_x = density_x;
				properties.pixels_per_meter_y = density_y;
			}
			const auto stride = document_row_bytes(width, frame.storage);
			if (png_get_rowbytes(png, info) > stride) decode.fail("invalid PNG row layout");
			decode.working_bytes(static_cast<std::uint64_t>(stride) * (passes == 1 ? 1 : height));
			decode.begin(frame);
			rows.resize(stride * (passes == 1 ? 1 : height), 0);
			for (int pass = 0; pass < passes; ++pass)
			{
				for (std::uint32_t y = 0; y < height; ++y)
				{
					auto* row = rows.data() + (passes == 1 ? 0 : y * stride);
					png_read_row(png, row, nullptr);
					decode.row(y, {row, stride});
					decode.output.progress(pass * height + y, passes * height);
				}
			}
			decode.end();
			png_read_end(png, info);
			png_textp text = nullptr;
			int count = 0;
			png_get_text(png, info, &text, &count);
			for (int i = 0; i < count; ++i)
			{
				const auto size = std::max(text[i].text_length, text[i].itxt_length);
				if (size && std::strcmp(text[i].key, "XML:com.adobe.xmp") == 0)
					decode.output.metadata(document_metadata::xmp,
						{reinterpret_cast<const std::uint8_t*>(text[i].text), size});
			}
			decode.output.metadata_complete();
		}
	}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	void document_png(document_decode& decode)
	{
		std::array<std::uint8_t, 8> signature{};
		decode.input.read(0, signature.data(), signature.size());
		if (!std::equal(signature.begin(), signature.end(), std::begin(png_signature)))
			decode.fail("invalid PNG signature");
		png_source source{decode.input, signature.size(), decode.result.warnings};
		png_reader owner(source);
		if (!owner.png || !owner.info) throw std::bad_alloc();
		std::vector<std::uint8_t> rows;
		std::array<std::uint32_t, 256> palette{};
		read_document_png(decode, source, owner, rows, palette);
	}
}
