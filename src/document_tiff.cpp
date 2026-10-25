// Copyright (C) 1998-2026 Zac Walker.
// SPDX-License-Identifier: MIT

// Purpose: Native TIFF pages, palettes, strips, tiles and metadata through libtiff.

#include "document_internal.h"

#include <tiffio.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace libfiles::detail
{
	namespace
	{
		struct tiff_input
		{
			document_decode& decode;
			std::uint64_t position = 0;
		};

		tmsize_t tiff_read(thandle_t handle, void* bytes, const tmsize_t requested)
		{
			auto& source = *static_cast<tiff_input*>(handle);
			if (requested <= 0) return 0;
			const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
				requested, source.decode.input.size() - source.position));
			try
			{
				source.decode.input.read(source.position, bytes, count);
				source.position += count;
				return static_cast<tmsize_t>(count);
			}
			catch (const read_error&) { return 0; }
			catch (const std::bad_alloc&)
			{
				source.decode.result.resource_limit = true;
				return 0;
			}
		}

		toff_t tiff_seek(thandle_t handle, const toff_t offset, const int whence)
		{
			auto& source = *static_cast<tiff_input*>(handle);
			const auto size = source.decode.input.size();
			const auto base = whence == SEEK_CUR ? source.position : whence == SEEK_END ? size : 0;
			std::uint64_t position = 0;
			if (whence != SEEK_SET && offset > static_cast<std::uint64_t>(INT64_MAX))
			{
				const auto backwards = ~offset + 1;
				if (backwards > base) return static_cast<toff_t>(-1);
				position = base - backwards;
			}
			else
			{
				if (offset > size - base) return static_cast<toff_t>(-1);
				position = base + offset;
			}
			source.position = position;
			return position;
		}

		int tiff_message(TIFF*, void* user, const char* module, const char* format, va_list args)
		{
			auto& decode = *static_cast<document_decode*>(user);
			char text[512]{};
			std::vsnprintf(text, sizeof(text), format, args);
			try
			{
				decode.result.warnings.emplace_back(std::string(module ? module : "TIFF") + ": " + text);
			}
			catch (const std::bad_alloc&) { decode.result.resource_limit = true; }
			return 1;
		}

		std::uint32_t density(const float value, const std::uint16_t unit, document_decode& decode)
		{
			const double result = unit == RESUNIT_INCH ? (value * 3937.0) / 100.0 : value * 100.0;
			// Keep the old signed-32-bit conversion defined, including MSVC's indefinite value.
			if (!std::isfinite(result) || result < INT32_MIN || result >= static_cast<double>(INT32_MAX) + 1)
			{
				decode.result.warnings.emplace_back("an unrepresentable TIFF resolution retained its legacy value");
				return 0x80000000u;
			}
			return static_cast<std::uint32_t>(static_cast<std::int32_t>(result));
		}
	}

	void document_tiff(document_decode& decode)
	{
		decode.working_bytes(1);
		tiff_input source{decode};
		std::unique_ptr<TIFFOpenOptions, decltype(&TIFFOpenOptionsFree)> options(
			TIFFOpenOptionsAlloc(), TIFFOpenOptionsFree);
		if (!options) throw std::bad_alloc();
		TIFFOpenOptionsSetErrorHandlerExtR(options.get(), tiff_message, &decode);
		TIFFOpenOptionsSetWarningHandlerExtR(options.get(), tiff_message, &decode);
		TIFFOpenOptionsSetMaxSingleMemAlloc(options.get(), static_cast<tmsize_t>(std::min<std::uint64_t>(
			decode.options.maximum_working_bytes, std::numeric_limits<tmsize_t>::max())));
		std::unique_ptr<TIFF, decltype(&TIFFClose)> owner(
			TIFFClientOpenExt("MemSource", "r", &source, tiff_read,
				[](thandle_t, void*, tmsize_t) -> tmsize_t { return 0; },
				tiff_seek, [](thandle_t) { return 0; },
				[](thandle_t handle) -> toff_t { return static_cast<tiff_input*>(handle)->decode.input.size(); },
				[](thandle_t, void**, toff_t*) { return 0; },
				[](thandle_t, void*, toff_t) {}, options.get()), TIFFClose);
		if (!owner) decode.fail("the TIFF header could not be read");
		auto* file = owner.get();
		std::vector<std::uint8_t> tile_buffer, strip, row;
		std::vector<std::uint32_t> raster;
		auto& info = decode.result.info;
		do
		{
			decode.check_cancel();
			std::uint32_t width = 0, height = 0;
			std::uint16_t compression = 0, bits = 0, samples = 0, photometric = 0;
			TIFFGetField(file, TIFFTAG_IMAGEWIDTH, &width);
			TIFFGetField(file, TIFFTAG_IMAGELENGTH, &height);
			TIFFGetFieldDefaulted(file, TIFFTAG_COMPRESSION, &compression);
			TIFFGetFieldDefaulted(file, TIFFTAG_BITSPERSAMPLE, &bits);
			TIFFGetFieldDefaulted(file, TIFFTAG_SAMPLESPERPIXEL, &samples);
			TIFFGetFieldDefaulted(file, TIFFTAG_PHOTOMETRIC, &photometric);
			if (!width || !height || width > INT32_MAX || height > INT32_MAX || !bits || !samples)
			{
				if (!info.frames) decode.fail("invalid TIFF dimensions or sample layout");
				decode.result.warnings.emplace_back("a later TIFF page has an invalid layout; complete pages were kept");
				break;
			}
			if (!info.frames)
			{
				info.bits_per_pixel = bits * samples;
				info.encoding = compression;
				info.color_model = photometric;
			}
			std::uint16_t planar = 0, orientation = 0, extras = 0;
			std::uint16_t* extra_types = nullptr;
			TIFFGetFieldDefaulted(file, TIFFTAG_PLANARCONFIG, &planar);
			TIFFGetFieldDefaulted(file, TIFFTAG_ORIENTATION, &orientation);
			TIFFGetField(file, TIFFTAG_EXTRASAMPLES, &extras, &extra_types);
			const bool rgba = TIFFIsTiled(file) || bits > 8 || planar == PLANARCONFIG_SEPARATE ||
				orientation != ORIENTATION_TOPLEFT || photometric == PHOTOMETRIC_SEPARATED ||
				photometric == PHOTOMETRIC_YCBCR ||
				((photometric == PHOTOMETRIC_MINISWHITE || photometric == PHOTOMETRIC_MINISBLACK) && samples > 1) ||
				(extras && extra_types && extra_types[0] == EXTRASAMPLE_ASSOCALPHA);
			std::uint32_t length = 0;
			void* data = nullptr;
			if (TIFFGetField(file, TIFFTAG_PHOTOSHOP, &length, &data) && data && length)
			{
				decode.working_bytes(length);
				metadata_blocks blocks;
				parse_photoshop_resources(blocks, {static_cast<const std::uint8_t*>(data), length}, false);
				if (!blocks.iptc.empty()) decode.output.metadata(document_metadata::iptc, blocks.iptc);
			}
			if (TIFFGetField(file, TIFFTAG_XMLPACKET, &length, &data) && data && length)
			{
				decode.working_bytes(length);
				decode.output.metadata(document_metadata::xmp, {static_cast<const std::uint8_t*>(data), length});
			}
			decode.output.metadata_complete();
			if (TIFFGetField(file, TIFFTAG_ICCPROFILE, &length, &data))
			{
				if (!data || !length) decode.fail("invalid TIFF colour profile");
				decode.working_bytes(length);
				const auto* profile = static_cast<const std::uint8_t*>(data);
				const bool rgb = length >= 20 && std::memcmp(profile + 16, "RGB ", 4) == 0;
				decode.output.metadata(rgb ? document_metadata::icc : document_metadata::source_icc, {profile, length});
				if (!rgb) decode.result.warnings.emplace_back(
					"The source TIFF colour profile is retained as opaque metadata, not attached to RGB pixels.");
			}
			document_frame frame;
			frame.width = width;
			frame.height = height;
			frame.storage = rgba ? document_storage::bgra32 : storage_from_bits(samples * bits);
			std::array<std::uint32_t, 256> palette{};
			const auto entries = palette_entries(frame.storage);
			for (std::size_t i = 0; i < entries; ++i)
				palette[i] = static_cast<std::uint32_t>(entries == 2 ? i * 255 : i) * 0x010101;
			if (!rgba && (photometric == PHOTOMETRIC_MINISWHITE || photometric == PHOTOMETRIC_MINISBLACK))
				decode.fail("the TIFF palette could not be read");
			if (!rgba)
			{
				const bool rgb = photometric == PHOTOMETRIC_RGB && bits == 8 && (samples == 3 || samples == 4);
				const bool indexed = photometric == PHOTOMETRIC_PALETTE && samples == 1 &&
					(bits == 1 || bits == 2 || bits == 4 || bits == 8);
				const bool lab = (photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB ||
					photometric == PHOTOMETRIC_ITULAB) && bits == 8 && samples == 3;
				if (!rgb && !indexed && !lab) decode.fail("unsupported TIFF sample layout");
			}
			if (!rgba && photometric == PHOTOMETRIC_PALETTE)
			{
				std::uint16_t *red = nullptr, *green = nullptr, *blue = nullptr;
				if (TIFFGetField(file, TIFFTAG_COLORMAP, &red, &green, &blue) && red && green && blue)
				{
					const auto colors = 1u << bits;
					bool wide = false;
					for (unsigned i = 0; i < colors; ++i) wide |= red[i] >= 256 || green[i] >= 256 || blue[i] >= 256;
					for (std::size_t i = 0; i < entries; ++i)
					{
						const auto convert = [wide](const unsigned value) { return wide ? value * 255 / 65535 : value; };
						palette[i] = 0xff000000u | (convert(red[i]) << 16) | (convert(green[i]) << 8) | convert(blue[i]);
					}
				}
			}
			frame.palette = {palette.data(), entries};
			info.width = width;
			info.height = height;
			info.storage = frame.storage;
			info.has_resolution = false;
			float x = 0, y = 0;
			std::uint16_t unit = RESUNIT_NONE;
			TIFFGetField(file, TIFFTAG_RESOLUTIONUNIT, &unit);
			if (TIFFGetField(file, TIFFTAG_XRESOLUTION, &x) && TIFFGetField(file, TIFFTAG_YRESOLUTION, &y) &&
				(unit == RESUNIT_INCH || unit == RESUNIT_CENTIMETER))
			{
				info.has_resolution = true;
				info.pixels_per_meter_x = density(x, unit, decode);
				info.pixels_per_meter_y = density(y, unit, decode);
			}
			decode.begin(frame);
			const auto stride = document_row_bytes(width, frame.storage);
			if (rgba)
			{
				decode.working_bytes(static_cast<std::uint64_t>(width) * height * 4);
				raster.resize(static_cast<std::size_t>(width) * height);
				const bool exact_tiles = TIFFIsTiled(file) && compression == COMPRESSION_NONE;
				tmsize_t tile_bytes = 0;
				if (exact_tiles)
				{
					tile_bytes = TIFFTileSize(file);
					if (tile_bytes <= 0) decode.fail("invalid TIFF tile size");
					decode.working_bytes(tile_bytes);
					tile_buffer.resize(static_cast<std::size_t>(tile_bytes));
					if (!TIFFReadBufferSetup(file, tile_buffer.data(), tile_bytes)) throw std::bad_alloc();
				}
				const auto complete = TIFFReadRGBAImageOriented(file, width, height, raster.data(), ORIENTATION_TOPLEFT, 1);
				if (exact_tiles && !TIFFReadBufferSetup(file, nullptr, tile_bytes)) throw std::bad_alloc();
				if (!complete) decode.fail("the TIFF raster could not be decoded");
				for (unsigned line = 0; line < height; ++line)
				{
					auto* pixels = raster.data() + static_cast<std::size_t>(line) * width;
					for (unsigned column = 0; column < width; ++column)
					{
						const auto value = pixels[column];
						const auto alpha = TIFFGetA(value);
						const auto straight = [alpha](const unsigned sample)
						{
							return alpha == 0 ? 0u : std::min(255u, (sample * 255 + alpha / 2) / alpha);
						};
						pixels[column] = (alpha << 24) | (straight(TIFFGetR(value)) << 16) |
							(straight(TIFFGetG(value)) << 8) | straight(TIFFGetB(value));
					}
					decode.row(line, {reinterpret_cast<const std::uint8_t*>(pixels), stride});
					decode.output.progress(line + 1, height);
				}
			}
			else
			{
				std::uint32_t rows_per_strip = 0;
				TIFFGetFieldDefaulted(file, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
				if (rows_per_strip == UINT32_MAX) rows_per_strip = height;
				const auto strip_size = TIFFStripSize(file);
				const auto scanline = TIFFScanlineSize(file);
				if (!rows_per_strip || strip_size <= 0 || scanline <= 0 ||
					static_cast<std::uint64_t>(strip_size) > decode.options.maximum_working_bytes / 4)
					decode.fail("invalid TIFF strip or scanline size");
				const auto rows = std::min(rows_per_strip, height);
				const auto read_bytes = std::max<std::uint64_t>(static_cast<std::uint64_t>(rows) * scanline, strip_size) + stride;
				const auto row_bytes = std::max<std::uint64_t>(stride, static_cast<std::uint64_t>(width) * 4);
				decode.working_bytes(read_bytes + row_bytes);
				strip.resize(static_cast<std::size_t>(read_bytes));
				row.resize(static_cast<std::size_t>(row_bytes), 0);
				for (std::uint32_t first = 0; first < height;)
				{
					const auto count = std::min(rows_per_strip, height - first);
					const auto wanted = static_cast<std::uint64_t>(count) * scanline;
					if (wanted > static_cast<std::uint64_t>(std::numeric_limits<tmsize_t>::max()))
						decode.fail("a TIFF strip exceeds the codec's size limit");
					const auto expected = static_cast<tmsize_t>(wanted);
					if (TIFFReadEncodedStrip(file, TIFFComputeStrip(file, first, 0), strip.data(), expected) != expected)
						decode.fail("the TIFF strip is incomplete");
					for (unsigned line = 0; line < count; ++line)
					{
						const auto* pixels = strip.data() + static_cast<std::size_t>(line) * scanline;
						if (photometric == PHOTOMETRIC_RGB)
						{
							if (samples == 4)
								for (unsigned column = 0; column < width; ++column)
								{
									const auto at = column * 4;
									row[at] = pixels[at + 2];
									row[at + 1] = pixels[at + 1];
									row[at + 2] = pixels[at];
									row[at + 3] = pixels[at + 3];
								}
							else
								for (unsigned column = 0; column < width; ++column)
								{
									const auto at = column * 3;
									row[at] = pixels[at + 2];
									row[at + 1] = pixels[at + 1];
									row[at + 2] = pixels[at];
								}
							decode.row(first + line, row);
						}
						else if (photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB ||
							photometric == PHOTOMETRIC_ITULAB)
						{
							if (bits != 8 || samples != 3) decode.fail("unsupported TIFF Lab sample layout");
							for (unsigned column = 0; column < width; ++column)
								native_lab_to_bgr(pixels[column * 3], pixels[column * 3 + 1],
									pixels[column * 3 + 2], row.data() + column * 3);
							decode.row(first + line, row);
						}
						else if (photometric == PHOTOMETRIC_PALETTE)
							decode.row(first + line, {pixels, stride});
						else decode.fail("unsupported TIFF photometric interpretation");
					}
					first += count;
				}
			}
			decode.end();
			if (TIFFLastDirectory(file)) break;
			if (!TIFFReadDirectory(file))
			{
				decode.result.warnings.emplace_back("a later TIFF page could not be read; complete pages before it were kept");
				break;
			}
		}
		while (true);
		if (decode.result.resource_limit) decode.fail("not enough memory to decode the TIFF image");
	}
}
