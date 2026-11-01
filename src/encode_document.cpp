// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Native BMP, GIF, TIFF and PNG document encoders, plus the small document-to-JPEG
// adapter. Encoders consume libfiles' existing row and document types; they never flatten a
// native document through a display BGRA image merely to make an output fit.

#include "libfiles/encode.h"
#include "libfiles/xmp.h"
#include "encode_internal.h"
#include "legacy.h"
#include "tiff_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef LIBFILES_GIF
#include <gif_lib.h>
#endif

#ifdef LIBFILES_PNG
#include <png.h>
#endif

#ifdef LIBFILES_TIFF
#include <tiffio.h>
#endif

namespace libfiles
{
	namespace
	{
		constexpr std::uint64_t default_working_bytes = 256ull * 1024 * 1024;

		document_encode_result unsupported(std::string message)
		{
			document_encode_result result;
			result.unsupported = true;
			result.error = std::move(message);
			return result;
		}

		document_encode_result failed(std::string message)
		{
			document_encode_result result;
			result.error = std::move(message);
			return result;
		}

		bool cancelled(const document_encode_options& options)
		{
			return options.cancelled && options.cancelled();
		}

		void progress(const document_encode_options& options, const std::uint32_t current,
		              const std::uint32_t total)
		{
			if (options.progress) options.progress(current, total);
		}

		[[maybe_unused]] bool working_allowed(const document_encode_options& options, const std::uint64_t bytes)
		{
			return bytes <= options.maximum_working_bytes;
		}

		void fail_cancelled(document_encode_result& result)
		{
			result.cancelled = true;
			result.error = "encoding was cancelled";
		}

		bool valid_frame_shape(const decoded_frame& frame, std::string& error)
		{
			try
			{
				const auto row = document_row_bytes(frame.width, frame.storage);
				if (frame.width == 0 || frame.height == 0 || frame.stride < row ||
					frame.stride % 4 != 0 ||
					(frame.height != 0 && frame.stride > frame.pixels.size() / frame.height))
				{
					error = "invalid native frame rows";
					return false;
				}
				if (palette_entries(frame.storage) != 0 &&
					frame.palette.size() < palette_entries(frame.storage))
				{
					error = "native frame palette is too short";
					return false;
				}
				return true;
			}
			catch (const std::exception& exception)
			{
				error = exception.what();
				return false;
			}
		}

		bool valid_document(const decoded_document& document, const image_format format,
		                    std::string& error)
		{
			if (document.frames.empty())
			{
				error = "the native document has no frames";
				return false;
			}
			if (document.info.width == 0 || document.info.height == 0)
			{
				error = "the native document has no canvas";
				return false;
			}
			for (const auto& frame : document.frames)
			{
				if (!valid_frame_shape(frame, error)) return false;
				if (format != image_format::gif && (frame.left != 0 || frame.top != 0))
				{
					error = "the target format does not have frame offsets";
					return false;
				}
				if (frame.left < 0 || frame.top < 0 ||
					static_cast<std::uint64_t>(frame.left) + frame.width > document.info.width ||
					static_cast<std::uint64_t>(frame.top) + frame.height > document.info.height)
				{
					error = "native frame lies outside its canvas";
					return false;
				}
			}
			return true;
		}

		std::span<const std::uint8_t> metadata_block(const decoded_document& document,
		                                             const document_metadata kind)
		{
			std::span<const std::uint8_t> result;
			for (const auto& block : document.metadata)
				if (block.kind == kind) result = block.bytes;
			return result;
		}

		embedded_metadata embedded(const decoded_document& document)
		{
			embedded_metadata result;
			result.exif = metadata_block(document, document_metadata::exif);
			result.xmp = metadata_block(document, document_metadata::xmp);
			result.iptc = metadata_block(document, document_metadata::iptc);
			result.icc = metadata_block(document, document_metadata::icc);
			if (result.icc.empty()) result.icc = metadata_block(document, document_metadata::source_icc);
			result.photoshop = metadata_block(document, document_metadata::photoshop);
			// JPEG decoders retain a complete Photoshop APP13 block as opaque metadata. It must not
			// be mistaken for raw IPTC-IIM and wrapped a second time by the JPEG encoder.
			constexpr std::string_view photoshop_signature("Photoshop 3.0\0", 14);
			if (result.photoshop.empty() && result.iptc.size() >= photoshop_signature.size() &&
				std::memcmp(result.iptc.data(), photoshop_signature.data(), photoshop_signature.size()) == 0)
			{
				result.photoshop = result.iptc;
				result.iptc = {};
			}
			return result;
		}

		void normalize_orientation_metadata(embedded_metadata& metadata, const bool reset,
		                                   std::vector<std::uint8_t>& exif_storage,
		                                   std::vector<std::uint8_t>& xmp_storage,
		                                   const std::uint32_t width, const std::uint32_t height)
		{
			if (!reset) return;
			if (!metadata.exif.empty())
			{
				const auto offset = encode_detail::is_exif_signature(metadata.exif)
					? encode_detail::exif_signature.size() : 0u;
				if (metadata.exif.size() > offset)
				{
					const auto original = metadata.exif.subspan(offset);
					reader input(original);
					detail::tiff_editor validated;
					if (!validated.open(input))
						throw encode_error("EXIF metadata cannot be normalized after an upright edit");
					using directory = detail::tiff_editor::directory;
					if (validated.has(directory::ifd0, detail::tag_orientation))
						validated.set_short(directory::ifd0, detail::tag_orientation, 1);
					if (validated.has(directory::exif, detail::tag_pixel_x_dimension))
						validated.set_long(directory::exif, detail::tag_pixel_x_dimension, width);
					if (validated.has(directory::exif, detail::tag_pixel_y_dimension))
						validated.set_long(directory::exif, detail::tag_pixel_y_dimension, height);
					auto normalized = detail::apply_changes(original, validated.changes(original.size()));
					if (!unlink_exif_thumbnail(normalized))
						throw encode_error("the stale EXIF thumbnail cannot be removed after an upright edit");
					exif_storage.assign(metadata.exif.begin(), metadata.exif.begin() + offset);
					exif_storage.insert(exif_storage.end(), normalized.begin(), normalized.end());
				}
				else
					throw encode_error("EXIF metadata is empty after its signature");
				metadata.exif = exif_storage;
			}
			if (!metadata.xmp.empty())
			{
				try
				{
					xmp::meta packet;
					packet.parse(std::string_view(reinterpret_cast<const char*>(metadata.xmp.data()),
					                              metadata.xmp.size()));
					bool changed = false;
					const auto set = [&](const std::string_view ns, const char* name, const std::uint32_t value)
					{
						if (packet.exists(ns, name))
						{
							packet.set_property(ns, name, std::to_string(value));
							changed = true;
						}
					};
					set(xmp::ns::tiff, "Orientation", 1);
					set(xmp::ns::tiff, "ImageWidth", width);
					set(xmp::ns::tiff, "ImageLength", height);
					set(xmp::ns::exif, "PixelXDimension", width);
					set(xmp::ns::exif, "PixelYDimension", height);
					if (changed)
					{
						xmp_storage = [&]
						{
							const auto serialized = packet.serialize();
							return std::vector<std::uint8_t>(serialized.begin(), serialized.end());
						}();
						metadata.xmp = xmp_storage;
					}
				}
				catch (const xmp::error& error)
				{
					throw encode_error(std::string("XMP orientation cannot be normalized: ") + error.what());
				}
			}
		}

		std::span<const std::uint8_t> comment_block(const decoded_document& document)
		{
			return metadata_block(document, document_metadata::comment);
		}

		std::span<const std::uint8_t> author_block(const decoded_document& document)
		{
			return metadata_block(document, document_metadata::author);
		}

		[[maybe_unused]] std::vector<std::uint8_t> tiff_photoshop(const embedded_metadata& metadata)
		{
			constexpr std::string_view signature("Photoshop 3.0\0", 14);
			const auto opaque = metadata.photoshop.size() >= signature.size() &&
				std::memcmp(metadata.photoshop.data(), signature.data(), signature.size()) == 0
				                ? metadata.photoshop.subspan(signature.size())
				                : metadata.photoshop;
			std::vector<std::uint8_t> result(opaque.begin(), opaque.end());
			if (!metadata.iptc.empty())
			{
				const auto size = metadata.iptc.size();
				const auto start = result.size();
				result.resize(start + 12 + size + (size & 1), 0);
				std::memcpy(result.data() + start, "8BIM", 4);
				result[start + 4] = 4;
				result[start + 8] = static_cast<std::uint8_t>(size >> 24);
				result[start + 9] = static_cast<std::uint8_t>(size >> 16);
				result[start + 10] = static_cast<std::uint8_t>(size >> 8);
				result[start + 11] = static_cast<std::uint8_t>(size);
				std::copy(metadata.iptc.begin(), metadata.iptc.end(), result.begin() + start + 12);
			}
			return result;
		}

		[[maybe_unused]] std::uint32_t resolution_x(const decoded_document& document,
		                            const document_encode_options& options)
		{
			return document.info.has_resolution ? document.info.pixels_per_meter_x : options.pixels_per_meter_x;
		}

		[[maybe_unused]] std::uint32_t resolution_y(const decoded_document& document,
		                            const document_encode_options& options)
		{
			return document.info.has_resolution ? document.info.pixels_per_meter_y : options.pixels_per_meter_y;
		}

		[[maybe_unused]] std::uint32_t index_at(const decoded_frame& frame, const std::uint8_t* row,
		                       const std::uint32_t x)
		{
			switch (frame.storage)
			{
			case document_storage::indexed1: return row[x >> 3] >> (7 - (x & 7)) & 1u;
			case document_storage::indexed2: return row[x >> 2] >> (6 - 2 * (x & 3)) & 3u;
			case document_storage::indexed4: return x & 1 ? row[x >> 1] & 0x0f : row[x >> 1] >> 4;
			case document_storage::indexed8:
			case document_storage::indexed8_alpha: return row[x];
			default: return 0;
			}
		}

		void put_le16(std::vector<std::uint8_t>& out, const std::uint16_t value)
		{
			out.push_back(static_cast<std::uint8_t>(value));
			out.push_back(static_cast<std::uint8_t>(value >> 8));
		}

		void put_le32(std::vector<std::uint8_t>& out, const std::uint32_t value)
		{
			out.push_back(static_cast<std::uint8_t>(value));
			out.push_back(static_cast<std::uint8_t>(value >> 8));
			out.push_back(static_cast<std::uint8_t>(value >> 16));
			out.push_back(static_cast<std::uint8_t>(value >> 24));
		}

		document_encode_result encode_bmp(const decoded_document& document,
		                                  const document_encode_options& options)
		{
			std::string error;
			if (!valid_document(document, image_format::bmp, error)) return failed(std::move(error));
			if (document.frames.size() != 1) return unsupported("BMP has one image frame");
			const auto& frame = document.frames.front();

			if (frame.storage != document_storage::indexed1 &&
				frame.storage != document_storage::indexed2 &&
				frame.storage != document_storage::indexed4 &&
				frame.storage != document_storage::indexed8 &&
				frame.storage != document_storage::indexed8_alpha &&
				frame.storage != document_storage::gray8 &&
				frame.storage != document_storage::bgr555 &&
				frame.storage != document_storage::bgr565 &&
				frame.storage != document_storage::bgr24 &&
				frame.storage != document_storage::bgrx32 &&
				frame.storage != document_storage::bgra32)
				return unsupported("BMP does not represent this native storage");

			const auto bits = storage_bits(frame.storage);
			const auto colors = frame.storage == document_storage::gray8 ? 256u : palette_entries(frame.storage);
			const auto bitfields = frame.storage == document_storage::bgr555 ||
				frame.storage == document_storage::bgr565;
			const auto row = document_row_bytes(frame.width, frame.storage);
			const auto header_size = std::uint64_t{14} + 40 + std::uint64_t{colors} * 4 +
				(bitfields ? 12 : 0);
			if (frame.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
				frame.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
				row > std::numeric_limits<std::uint64_t>::max() / frame.height)
				return unsupported("BMP dimensions or raster are too large");
			const auto pixel_size = std::uint64_t{row} * frame.height;
			if (header_size > std::numeric_limits<std::uint32_t>::max() ||
				pixel_size > std::numeric_limits<std::uint32_t>::max() - header_size ||
				header_size + pixel_size > std::numeric_limits<std::size_t>::max())
				return unsupported("BMP output is too large");

			document_encode_result result;
			auto& out = result.data;
			out.reserve(static_cast<std::size_t>(header_size + pixel_size));
			put_le16(out, 0x4D42);
			put_le32(out, static_cast<std::uint32_t>(header_size + pixel_size));
			put_le16(out, 0);
			put_le16(out, 0);
			put_le32(out, static_cast<std::uint32_t>(header_size));
			put_le32(out, 40);
			put_le32(out, frame.width);
			put_le32(out, frame.height);
			put_le16(out, 1);
			put_le16(out, static_cast<std::uint16_t>(bits));
			put_le32(out, bitfields ? 3u : 0u);
			put_le32(out, static_cast<std::uint32_t>(pixel_size));
			put_le32(out, document.info.has_resolution ? document.info.pixels_per_meter_x : 0);
			put_le32(out, document.info.has_resolution ? document.info.pixels_per_meter_y : 0);
			put_le32(out, static_cast<std::uint32_t>(colors));
			put_le32(out, static_cast<std::uint32_t>(colors));

			if (bitfields)
			{
				put_le32(out, frame.storage == document_storage::bgr565 ? 0xf800u : 0x7c00u);
				put_le32(out, frame.storage == document_storage::bgr565 ? 0x07e0u : 0x03e0u);
				put_le32(out, 0x001fu);
			}

			if (colors != 0)
			{
				for (std::size_t i = 0; i < colors; ++i)
				{
					const auto value = frame.storage == document_storage::gray8
						                   ? 0xff000000u | static_cast<std::uint32_t>(i) * 0x010101u
						                   : frame.palette[i];
					put_le32(out, value);
				}
				if (frame.storage == document_storage::indexed8_alpha ||
					frame.transparent.has_value())
					result.warnings.emplace_back("BMP stores palette alpha and transparency only in its reserved palette byte");
			}
			if (frame.storage == document_storage::bgra32)
				result.warnings.emplace_back("BMP BI_RGB readers may ignore the BGRA alpha byte");
			if (!embedded(document).exif.empty() || !embedded(document).xmp.empty() ||
				!embedded(document).iptc.empty() || !embedded(document).icc.empty() ||
				!embedded(document).photoshop.empty() || !comment_block(document).empty())
				result.warnings.emplace_back("BMP has no standard metadata blocks; native metadata was not embedded");

			for (std::uint32_t y = 0; y < frame.height; ++y)
			{
				if (cancelled(options))
				{
					result.data.clear();
					fail_cancelled(result);
					return result;
				}

				const auto* const source = frame.pixels.data() +
					(static_cast<std::size_t>(frame.height - 1 - y) * frame.stride);
				out.insert(out.end(), source, source + row);
				progress(options, y + 1, frame.height);
			}
			result.success = true;
			return result;
		}

		document_encode_result encode_bmp_image(const image_view& image, const document_info& info,
		                                        const document_encode_options& options)
		{
			if (image.format != pixel_format::bgrx8 && image.format != pixel_format::bgra8)
				return unsupported("BMP row encoding accepts only four-byte BGR rows");
			if (image.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
				image.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
				return unsupported("BMP dimensions are too large");
			const auto row = static_cast<std::uint64_t>(image.width) * 4;
			if (image.height != 0 && row > std::numeric_limits<std::uint64_t>::max() / image.height)
				return unsupported("BMP raster is too large");
			const auto pixels = row * image.height;
			const auto header = std::uint64_t{14} + 40;
			if (pixels > std::numeric_limits<std::uint32_t>::max() - header ||
				header + pixels > std::numeric_limits<std::size_t>::max())
				return unsupported("BMP output is too large");
			document_encode_result result;
			result.data.reserve(static_cast<std::size_t>(header + pixels));
			put_le16(result.data, 0x4D42);
			put_le32(result.data, static_cast<std::uint32_t>(header + pixels));
			put_le16(result.data, 0);
			put_le16(result.data, 0);
			put_le32(result.data, static_cast<std::uint32_t>(header));
			put_le32(result.data, 40);
			put_le32(result.data, image.width);
			put_le32(result.data, image.height);
			put_le16(result.data, 1);
			put_le16(result.data, 32);
			put_le32(result.data, 0);
			put_le32(result.data, static_cast<std::uint32_t>(pixels));
			put_le32(result.data, info.has_resolution ? info.pixels_per_meter_x : options.pixels_per_meter_x);
			put_le32(result.data, info.has_resolution ? info.pixels_per_meter_y : options.pixels_per_meter_y);
			put_le32(result.data, 0);
			put_le32(result.data, 0);
			if (image.format == pixel_format::bgra8)
				result.warnings.emplace_back("BMP BI_RGB readers may ignore the BGRA alpha byte");
			for (std::uint32_t y = 0; y < image.height; ++y)
			{
				if (cancelled(options))
				{
					result.data.clear();
					fail_cancelled(result);
					return result;
				}
				const auto* const source = image.pixels +
					static_cast<std::size_t>(image.height - 1 - y) * image.stride;
				result.data.insert(result.data.end(), source, source + row);
				progress(options, y + 1, image.height);
			}
			result.success = true;
			return result;
		}

#ifdef LIBFILES_PNG
		[[noreturn]] void png_encode_error(png_structp, png_const_charp message)
		{
			throw encode_error(message ? message : "PNG encoder failed");
		}

		void png_encode_warning(png_structp png, png_const_charp message)
		{
			auto* const warnings = static_cast<std::vector<std::string>*>(png_get_error_ptr(png));
			if (warnings && message) warnings->emplace_back(message);
		}

		struct png_writer
		{
			png_structp png = nullptr;
			png_infop info = nullptr;

			explicit png_writer(std::vector<std::string>& warnings)
			{
				png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &warnings, png_encode_error,
				                               png_encode_warning);
				if (png) info = png_create_info_struct(png);
				if (!png || !info)
				{
					png_destroy_write_struct(&png, &info);
					throw encode_error("could not create PNG writer");
				}
			}

			~png_writer() { png_destroy_write_struct(&png, &info); }
			png_writer(const png_writer&) = delete;
			png_writer& operator=(const png_writer&) = delete;
		};

		void PNGCBAPI png_write_bytes(png_structp png, png_bytep bytes, png_size_t size)
		{
			auto* const output = static_cast<std::vector<std::uint8_t>*>(png_get_io_ptr(png));
			output->insert(output->end(), bytes, bytes + size);
		}

		void add_png_unknown(std::vector<png_unknown_chunk>& chunks,
		                     std::vector<std::vector<std::uint8_t>>& storage,
		                     const char name[5], const std::span<const std::uint8_t> bytes)
		{
			if (bytes.empty()) return;
			storage.emplace_back(bytes.begin(), bytes.end());
			png_unknown_chunk chunk{};
			std::memcpy(chunk.name, name, 4);
			chunk.data = storage.back().data();
			chunk.size = storage.back().size();
			chunk.location = PNG_HAVE_IHDR;
			chunks.push_back(chunk);
		}

		document_encode_result encode_png_document(const decoded_document& document,
		                                           const document_encode_options& options)
		{
			std::string error;
			if (!valid_document(document, image_format::png, error)) return failed(std::move(error));
			if (document.frames.size() != 1) return unsupported("PNG has one image frame");
			const auto& frame = document.frames.front();
			const auto indexed = palette_entries(frame.storage) != 0;
			const auto is_gray = frame.storage == document_storage::gray8;
			const auto rgb = frame.storage == document_storage::bgr24;
			const auto bgrx = frame.storage == document_storage::bgrx32;
			const auto bgra = frame.storage == document_storage::bgra32;
			if (!indexed && !is_gray && !rgb && !bgrx && !bgra)
				return unsupported("PNG does not represent this native storage without conversion");

			document_encode_result result;
			png_writer writer(result.warnings);
			png_set_write_fn(writer.png, &result.data, png_write_bytes, nullptr);
			const auto depth = indexed || is_gray ? static_cast<int>(storage_bits(frame.storage)) : 8;
			const auto color = indexed ? PNG_COLOR_TYPE_PALETTE :
				is_gray ? PNG_COLOR_TYPE_GRAY : (bgra ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB);
			png_set_IHDR(writer.png, writer.info, frame.width, frame.height, depth, color,
			             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

			std::vector<png_color> palette;
			std::vector<png_byte> alpha;
			if (indexed)
			{
				const auto entries = palette_entries(frame.storage);
				palette.resize(entries);
				for (std::size_t i = 0; i < entries; ++i)
				{
					const auto value = frame.palette[i];
					palette[i] = {static_cast<png_byte>(value >> 16), static_cast<png_byte>(value >> 8),
					              static_cast<png_byte>(value)};
				}
				bool has_alpha = frame.storage == document_storage::indexed8_alpha;
				for (std::size_t i = 0; i < entries; ++i) has_alpha |= static_cast<std::uint8_t>(frame.palette[i] >> 24) != 255;
				has_alpha |= frame.transparent.has_value();
				png_set_PLTE(writer.png, writer.info, palette.data(), static_cast<int>(entries));
				if (has_alpha)
				{
					alpha.resize(entries, 255);
					for (std::size_t i = 0; i < entries; ++i) alpha[i] = static_cast<png_byte>(frame.palette[i] >> 24);
					if (frame.transparent && *frame.transparent < alpha.size()) alpha[*frame.transparent] = 0;
					png_set_tRNS(writer.png, writer.info, alpha.data(), static_cast<int>(alpha.size()), nullptr);
				}
			}

			const auto x = document.info.has_resolution ? document.info.pixels_per_meter_x : options.pixels_per_meter_x;
			const auto y = document.info.has_resolution ? document.info.pixels_per_meter_y : options.pixels_per_meter_y;
			if (x > 0 && y > 0) png_set_pHYs(writer.png, writer.info, x, y, PNG_RESOLUTION_METER);

			auto meta = embedded(document);
			std::vector<std::uint8_t> normalized_exif, normalized_xmp;
			normalize_orientation_metadata(meta, options.reset_orientation_metadata,
			                                normalized_exif, normalized_xmp, frame.width, frame.height);
			if (!meta.icc.empty())
			{
				if (meta.icc.size() > std::numeric_limits<png_uint_32>::max())
					return unsupported("PNG ICC profile is too large");
				png_set_iCCP(writer.png, writer.info, "icc", PNG_COMPRESSION_TYPE_BASE,
				             const_cast<png_bytep>(meta.icc.data()), static_cast<png_uint_32>(meta.icc.size()));
			}
			if (!meta.exif.empty())
			{
				const auto skip = encode_detail::is_exif_signature(meta.exif) ? encode_detail::exif_signature.size() : 0u;
				if (meta.exif.size() - skip > std::numeric_limits<png_uint_32>::max())
					return unsupported("PNG EXIF block is too large");
				png_set_eXIf_1(writer.png, writer.info, static_cast<png_uint_32>(meta.exif.size() - skip),
				               const_cast<png_bytep>(meta.exif.data()) + skip);
			}

			std::vector<std::vector<std::uint8_t>> unknown_storage;
			unknown_storage.reserve(2 + document.metadata.size());
			std::vector<png_unknown_chunk> unknown;
			add_png_unknown(unknown, unknown_storage, "iPTC", meta.iptc);
			add_png_unknown(unknown, unknown_storage, "iPSH", meta.photoshop);
			for (const auto& block : document.metadata)
			{
				if (block.kind != document_metadata::png_text) continue;
				const auto& bytes = block.bytes;
				if (bytes.size() < 6 ||
					(std::memcmp(bytes.data(), "tEXt", 4) != 0 && std::memcmp(bytes.data(), "iTXt", 4) != 0))
					return failed("invalid retained PNG text chunk");
				const auto key_end = std::find(bytes.begin() + 4, bytes.end(), 0);
				if (key_end == bytes.end() || key_end == bytes.begin() + 4 ||
					key_end - bytes.begin() > 83)
					return failed("invalid retained PNG text keyword");
				if (std::memcmp(bytes.data(), "iTXt", 4) == 0)
				{
					const auto header = key_end + 1;
					if (bytes.end() - header < 4 || header[0] != 0 || header[1] != 0)
						return failed("invalid retained PNG international text header");
					const auto language_end = std::find(header + 2, bytes.end(), 0);
					if (language_end == bytes.end() ||
						std::find(language_end + 1, bytes.end(), 0) == bytes.end())
						return failed("invalid retained PNG international text language");
				}
				add_png_unknown(unknown, unknown_storage, reinterpret_cast<const char*>(bytes.data()),
					std::span(bytes).subspan(4));
			}
			if (!unknown.empty())
				png_set_unknown_chunks(writer.png, writer.info, unknown.data(), static_cast<int>(unknown.size()));

			const auto comment = comment_block(document);
			const auto author = author_block(document);
			png_text text[3]{};
			std::vector<std::string> text_storage;
			text_storage.reserve(std::size(text));
			int text_count = 0;
			if (!meta.xmp.empty())
			{
				text_storage.emplace_back(reinterpret_cast<const char*>(meta.xmp.data()), meta.xmp.size());
				text[text_count].compression = PNG_ITXT_COMPRESSION_NONE;
				text[text_count].key = const_cast<png_charp>("XML:com.adobe.xmp");
				text[text_count].text = text_storage.back().data();
				text[text_count].text_length = text_storage.back().size();
				text[text_count].itxt_length = text_storage.back().size();
				++text_count;
			}
			if (!comment.empty())
			{
				text_storage.emplace_back(reinterpret_cast<const char*>(comment.data()), comment.size());
				text[text_count].compression = PNG_TEXT_COMPRESSION_NONE;
				text[text_count].key = const_cast<png_charp>("Comment");
				text[text_count].text = text_storage.back().data();
				text[text_count].text_length = text_storage.back().size();
				++text_count;
			}
			if (!author.empty())
			{
				text_storage.emplace_back(reinterpret_cast<const char*>(author.data()), author.size());
				text[text_count].compression = PNG_TEXT_COMPRESSION_NONE;
				text[text_count].key = const_cast<png_charp>("Author");
				text[text_count].text = text_storage.back().data();
				text[text_count].text_length = text_storage.back().size();
				++text_count;
			}
			if (text_count) png_set_text(writer.png, writer.info, text, text_count);

			if (!working_allowed(options, static_cast<std::uint64_t>(frame.height) * sizeof(png_bytep)))
				return unsupported("PNG row pointers exceed the encoder working-memory limit");
			std::vector<png_bytep> rows(frame.height);
			for (std::uint32_t row = 0; row < frame.height; ++row)
				rows[row] = const_cast<png_bytep>(frame.pixels.data() + static_cast<std::size_t>(row) * frame.stride);
			png_set_rows(writer.png, writer.info, rows.data());
			int transform = 0;
			if (rgb || bgrx || bgra) transform |= PNG_TRANSFORM_BGR;
			if (bgrx) transform |= PNG_TRANSFORM_STRIP_FILLER_AFTER;
			png_write_png(writer.png, writer.info, transform, nullptr);
			for (std::uint32_t row = 0; row < frame.height; ++row) progress(options, row + 1, frame.height);
			result.success = true;
			return result;
		}
#endif

#ifdef LIBFILES_GIF
		struct gif_output
		{
			std::vector<std::uint8_t> bytes;
			bool failed = false;
		};

		int gif_write(GifFileType* file, const GifByteType* bytes, const int count)
		{
			auto& output = *static_cast<gif_output*>(file->UserData);
			if (count < 0) return 0;
			try
			{
				output.bytes.insert(output.bytes.end(), bytes, bytes + count);
				return count;
			}
			catch (...)
			{
				output.failed = true;
				return 0;
			}
		}

		ColorMapObject* make_gif_palette(const std::vector<std::uint32_t>& source)
		{
			if (source.empty() || source.size() > 256 || (source.size() & (source.size() - 1)) != 0) return nullptr;
			auto* const map = GifMakeMapObject(static_cast<int>(source.size()), nullptr);
			if (!map) return nullptr;
			for (std::size_t i = 0; i < source.size(); ++i)
			{
				map->Colors[i].Red = static_cast<GifByteType>(source[i] >> 16);
				map->Colors[i].Green = static_cast<GifByteType>(source[i] >> 8);
				map->Colors[i].Blue = static_cast<GifByteType>(source[i]);
			}
			return map;
		}

		bool same_gif_palette(const ColorMapObject* map, const std::vector<std::uint32_t>& source)
		{
			if (!map || map->ColorCount != static_cast<int>(source.size())) return false;
			for (std::size_t i = 0; i < source.size(); ++i)
				if (map->Colors[i].Red != static_cast<GifByteType>(source[i] >> 16) ||
					map->Colors[i].Green != static_cast<GifByteType>(source[i] >> 8) ||
					map->Colors[i].Blue != static_cast<GifByteType>(source[i]))
					return false;
			return true;
		}

		bool gif_transparency(const decoded_frame& frame, int& transparent, std::string& error)
		{
			transparent = frame.transparent ? static_cast<int>(*frame.transparent) : -1;
			if (transparent >= static_cast<int>(frame.palette.size())) { error = "GIF transparency index is out of range"; return false; }
			for (std::size_t i = 0; i < frame.palette.size(); ++i)
			{
				const auto alpha = static_cast<std::uint8_t>(frame.palette[i] >> 24);
				if (alpha != 0 && alpha != 255)
				{
					error = "GIF cannot represent partial palette alpha";
					return false;
				}
				if (alpha == 0)
				{
					if (transparent >= 0 && transparent != static_cast<int>(i))
					{
						error = "GIF has more than one transparent palette entry";
						return false;
					}
					transparent = static_cast<int>(i);
				}
			}
			return transparent < 0 || transparent < static_cast<int>(frame.palette.size());
		}

		document_encode_result encode_gif_document(const decoded_document& document,
		                                           const document_encode_options& options)
		{
			std::string error;
			if (!valid_document(document, image_format::gif, error)) return failed(std::move(error));
			if (document.info.width > 65535 || document.info.height > 65535)
				return unsupported("GIF canvas exceeds its 16-bit dimensions");
			for (const auto& frame : document.frames)
			{
				if (palette_entries(frame.storage) == 0)
					return unsupported("GIF encoding requires indexed frames");
				if (frame.palette.empty() || frame.palette.size() > 256)
					return unsupported("GIF frame palette is invalid");
				if (frame.left > 65535 || frame.top > 65535 || frame.width > 65535 || frame.height > 65535)
					return unsupported("GIF frame exceeds its 16-bit dimensions");
			}

			document_encode_result result;
			for (const auto& block : document.metadata)
				if (block.kind != document_metadata::comment)
				{
					result.warnings.emplace_back("GIF has no standard metadata blocks beyond comments; opaque metadata was not embedded");
					break;
				}
			gif_output output;
			int error_code = 0;
			GifFileType* file = EGifOpen(&output, gif_write, &error_code);
			if (!file) return failed("could not create GIF writer");
			ColorMapObject* global = make_gif_palette(document.frames.front().palette);
			if (!global)
			{
				EGifCloseFile(file, &error_code);
				return unsupported("GIF global palette is invalid");
			}
			const auto close = [&]
			{
				if (global) GifFreeMapObject(global);
				if (file) EGifCloseFile(file, &error_code);
			};
			if (EGifPutScreenDesc(file, static_cast<int>(document.info.width),
			                      static_cast<int>(document.info.height), global->BitsPerPixel, 0, global) == GIF_ERROR)
			{
				close();
				return failed("could not write GIF screen descriptor");
			}

			for (const auto& block : document.metadata)
			{
				if (block.kind != document_metadata::comment || block.bytes.empty()) continue;
				for (std::size_t at = 0; at < block.bytes.size();)
				{
					const auto count = std::min<std::size_t>(255, block.bytes.size() - at);
					if (EGifPutExtension(file, COMMENT_EXT_FUNC_CODE, static_cast<int>(count),
					                     block.bytes.data() + at) == GIF_ERROR)
					{
						close();
						return failed("could not write GIF comment");
					}
					at += count;
				}
			}
			if (document.frames.size() > 1)
			{
				const std::uint8_t application[] = {'N','E','T','S','C','A','P','E','2','.', '0'};
				const std::uint8_t loop[] = {1, 0, 0};
				if (EGifPutExtensionLeader(file, APPLICATION_EXT_FUNC_CODE) == GIF_ERROR ||
					EGifPutExtensionBlock(file, sizeof(application), application) == GIF_ERROR ||
					EGifPutExtensionBlock(file, sizeof(loop), loop) == GIF_ERROR ||
					EGifPutExtensionTrailer(file) == GIF_ERROR)
				{
					close();
					return failed("could not write GIF animation extension");
				}
			}

			for (std::size_t frame_index = 0; frame_index < document.frames.size(); ++frame_index)
			{
				const auto& frame = document.frames[frame_index];
				int transparent = -1;
				if (!gif_transparency(frame, transparent, error))
				{
					close();
					return unsupported(std::move(error));
				}
				const auto delay = std::min<std::uint32_t>(65535, frame.delay_ms.value_or(0) / 10);
				const auto disposal = std::min<std::uint32_t>(7, frame.disposal.value_or(0));
				const std::uint8_t control[] = {
					static_cast<std::uint8_t>((disposal & 7) << 2 | (transparent >= 0 ? 1 : 0)),
					static_cast<std::uint8_t>(delay), static_cast<std::uint8_t>(delay >> 8),
					static_cast<std::uint8_t>(transparent < 0 ? 0 : transparent)
				};
				if (EGifPutExtension(file, GRAPHICS_EXT_FUNC_CODE, sizeof(control), control) == GIF_ERROR)
				{
					close();
					return failed("could not write GIF frame control");
				}
				ColorMapObject* local = same_gif_palette(global, frame.palette) ? nullptr :
					make_gif_palette(frame.palette);
				if (!same_gif_palette(global, frame.palette) && !local)
				{
					close();
					return unsupported("GIF local palette is invalid");
				}
				const auto descriptor = EGifPutImageDesc(file, frame.left, frame.top,
					static_cast<int>(frame.width), static_cast<int>(frame.height), false, local);
				if (descriptor == GIF_ERROR)
				{
					if (local) GifFreeMapObject(local);
					close();
					return failed("could not write GIF frame descriptor");
				}
					if (!working_allowed(options, frame.width))
					{
						if (local) GifFreeMapObject(local);
						close();
						return unsupported("GIF row exceeds the encoder working-memory limit");
					}
					std::vector<GifByteType> indices(frame.width);
				for (std::uint32_t y = 0; y < frame.height; ++y)
				{
					if (cancelled(options))
					{
						if (local) GifFreeMapObject(local);
						close();
						result.data.clear();
						fail_cancelled(result);
						return result;
					}
					const auto* const source = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride;
					for (std::uint32_t x = 0; x < frame.width; ++x) indices[x] = static_cast<GifByteType>(index_at(frame, source, x));
					if (EGifPutLine(file, indices.data(), static_cast<int>(indices.size())) == GIF_ERROR)
					{
						if (local) GifFreeMapObject(local);
						close();
						return failed("could not write GIF pixels");
					}
					progress(options, static_cast<std::uint32_t>(frame_index * frame.height + y + 1),
					         static_cast<std::uint32_t>(document.frames.size() * frame.height));
				}
				if (local) GifFreeMapObject(local);
			}
			if (EGifCloseFile(file, &error_code) == GIF_ERROR)
			{
				file = nullptr;
				GifFreeMapObject(global);
				return failed("could not close GIF output");
			}
			file = nullptr;
			GifFreeMapObject(global);
			result.data = std::move(output.bytes);
			result.success = true;
			return result;
		}
#endif

#ifdef LIBFILES_TIFF
		struct tiff_output
		{
			std::vector<std::uint8_t> bytes;
			std::uint64_t position = 0;
			bool failed = false;
		};

		tsize_t tiff_write(thandle_t handle, tdata_t data, const tsize_t size)
		{
			auto& output = *static_cast<tiff_output*>(handle);
			if (size < 0 || static_cast<std::uint64_t>(size) >
				std::numeric_limits<std::size_t>::max() - output.position)
			{
				output.failed = true;
				return 0;
			}
			try
			{
				const auto end = output.position + static_cast<std::uint64_t>(size);
				if (end > output.bytes.size()) output.bytes.resize(static_cast<std::size_t>(end));
				std::memcpy(output.bytes.data() + output.position, data, static_cast<std::size_t>(size));
				output.position = end;
				return size;
			}
			catch (...)
			{
				output.failed = true;
				return 0;
			}
		}

		tsize_t tiff_read(thandle_t handle, tdata_t data, const tsize_t size)
		{
			auto& output = *static_cast<tiff_output*>(handle);
			if (size <= 0 || output.position >= output.bytes.size()) return 0;
			const auto count = std::min<std::size_t>(static_cast<std::size_t>(size),
			                                         output.bytes.size() - static_cast<std::size_t>(output.position));
			std::memcpy(data, output.bytes.data() + output.position, count);
			output.position += count;
			return static_cast<tsize_t>(count);
		}

		toff_t tiff_seek(thandle_t handle, const toff_t offset, const int whence)
		{
			auto& output = *static_cast<tiff_output*>(handle);
			std::int64_t signed_offset = 0;
			std::memcpy(&signed_offset, &offset, sizeof(offset));
			const auto base = whence == SEEK_SET ? 0ull : whence == SEEK_CUR ? output.position : output.bytes.size();
			if (signed_offset < 0 && static_cast<std::uint64_t>(-signed_offset) > base) return static_cast<toff_t>(-1);
			const auto next = signed_offset < 0 ? base - static_cast<std::uint64_t>(-signed_offset) :
				base + static_cast<std::uint64_t>(signed_offset);
			output.position = next;
			return static_cast<toff_t>(next);
		}

		toff_t tiff_size(thandle_t handle)
		{
			return static_cast<toff_t>(static_cast<tiff_output*>(handle)->bytes.size());
		}

		bool tiff_set_resolution(TIFF* file, const decoded_document& document,
		                         const document_encode_options& options)
		{
			const auto x = resolution_x(document, options);
			const auto y = resolution_y(document, options);
			if (options.legacy_compatibility)
			{
				const auto inch = [](const std::uint32_t ppm)
				{
					return static_cast<float>((static_cast<std::uint64_t>(ppm) * 100 + 3937 / 2) / 3937);
				};
				return TIFFSetField(file, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH) &&
					TIFFSetField(file, TIFFTAG_XRESOLUTION, inch(x)) &&
					TIFFSetField(file, TIFFTAG_YRESOLUTION, inch(y));
			}
			if (x == 0 || y == 0) return true;
			return TIFFSetField(file, TIFFTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER) &&
				TIFFSetField(file, TIFFTAG_XRESOLUTION, static_cast<float>(x) / 100.0f) &&
				TIFFSetField(file, TIFFTAG_YRESOLUTION, static_cast<float>(y) / 100.0f);
		}

		void carry_tiff_exif(std::vector<std::uint8_t>& output,
		                     const std::span<const std::uint8_t> exif,
		                     const document_encode_options& options)
		{
			if (exif.empty() || (options.legacy_compatibility && !options.reset_orientation_metadata))
				return;
			const auto skip = encode_detail::is_exif_signature(exif) ? encode_detail::exif_signature.size() : 0u;
			try
			{
				reader source(exif.subspan(skip));
				reader destination(output);
				detail::tiff_editor editor;
				if (!editor.open(destination)) throw encode_error("the encoded TIFF directory cannot be read");
				editor.import_metadata(source, false);
				output = detail::apply_changes(output, editor.changes(output.size()));
			}
			catch (const detail::format_error& error)
			{
				throw encode_error(std::string("TIFF EXIF metadata could not be carried: ") + error.what());
			}
		}

		document_encode_result encode_tiff_image(const image_view& image, const document_info& info,
		                                         const embedded_metadata& metadata,
		                                         const document_encode_options& options)
		{
			if (image.format != pixel_format::bgrx8 && image.format != pixel_format::bgra8)
				return unsupported("TIFF row encoding accepts BGRX8 or BGRA8");
			auto normalized_metadata = metadata;
			std::vector<std::uint8_t> normalized_exif, normalized_xmp;
			normalize_orientation_metadata(normalized_metadata, options.reset_orientation_metadata,
			                                normalized_exif, normalized_xmp, image.width, image.height);
			tiff_output output;
			TIFF* file = TIFFClientOpen("libfiles", "w", &output, tiff_read, tiff_write, tiff_seek,
			                            [](thandle_t) -> int { return 0; }, tiff_size,
			                            [](thandle_t, void**, toff_t*) -> int { return 0; },
			                            [](thandle_t, void*, toff_t) {});
			if (!file) return failed("could not create TIFF writer");
			const auto close = [&] { TIFFClose(file); file = nullptr; };
			const auto samples = image.format == pixel_format::bgra8 ? 4 : 3;
			if (!TIFFSetField(file, TIFFTAG_IMAGEWIDTH, image.width) ||
				!TIFFSetField(file, TIFFTAG_IMAGELENGTH, image.height) ||
				!TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, samples) ||
				!TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8) ||
				!TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) ||
				!TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
				!TIFFSetField(file, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) ||
				!TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS))
			{
				close();
				return failed("could not set TIFF page fields");
			}
			const auto x = info.has_resolution ? info.pixels_per_meter_x : options.pixels_per_meter_x;
			const auto y = info.has_resolution ? info.pixels_per_meter_y : options.pixels_per_meter_y;
			if (options.legacy_compatibility)
			{
				const auto inch = [](const std::uint32_t ppm)
				{
					return static_cast<float>((static_cast<std::uint64_t>(ppm) * 100 + 3937 / 2) / 3937);
				};
				if (!TIFFSetField(file, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH) ||
					!TIFFSetField(file, TIFFTAG_XRESOLUTION, inch(x)) ||
					!TIFFSetField(file, TIFFTAG_YRESOLUTION, inch(y)))
				{
					close();
					return failed("could not set TIFF resolution");
				}
			}
			else if (x > 0 && y > 0 &&
				(!TIFFSetField(file, TIFFTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER) ||
				 !TIFFSetField(file, TIFFTAG_XRESOLUTION, static_cast<float>(x) / 100.0f) ||
				 !TIFFSetField(file, TIFFTAG_YRESOLUTION, static_cast<float>(y) / 100.0f)))
			{
				close();
				return failed("could not set TIFF resolution");
			}
			if (samples == 4)
			{
				const std::uint16_t extra = EXTRASAMPLE_UNASSALPHA;
				if (!TIFFSetField(file, TIFFTAG_EXTRASAMPLES, 1, &extra))
				{
					close();
					return failed("could not set TIFF alpha");
				}
			}
			if (!normalized_metadata.xmp.empty()) TIFFSetField(file, TIFFTAG_XMLPACKET,
				static_cast<std::uint32_t>(normalized_metadata.xmp.size()), normalized_metadata.xmp.data());
			if (!normalized_metadata.icc.empty()) TIFFSetField(file, TIFFTAG_ICCPROFILE,
				static_cast<std::uint32_t>(normalized_metadata.icc.size()), normalized_metadata.icc.data());
			const auto photoshop = tiff_photoshop(normalized_metadata);
			if (!photoshop.empty()) TIFFSetField(file, TIFFTAG_PHOTOSHOP,
				static_cast<std::uint32_t>(photoshop.size()), photoshop.data());
			const auto row_bytes = static_cast<std::uint64_t>(image.width) * samples;
			if (!working_allowed(options, row_bytes))
			{
				close();
				return unsupported("TIFF row exceeds the encoder working-memory limit");
			}
			std::vector<std::uint8_t> row(static_cast<std::size_t>(row_bytes));
			document_encode_result result;
			for (std::uint32_t y_row = 0; y_row < image.height; ++y_row)
			{
				if (cancelled(options))
				{
					close();
					fail_cancelled(result);
					return result;
				}
				const auto* const source = image.pixels + static_cast<std::size_t>(y_row) * image.stride;
				for (std::uint32_t x_pixel = 0; x_pixel < image.width; ++x_pixel)
				{
					row[x_pixel * samples] = source[x_pixel * 4 + 2];
					row[x_pixel * samples + 1] = source[x_pixel * 4 + 1];
					row[x_pixel * samples + 2] = source[x_pixel * 4];
					if (samples == 4) row[x_pixel * samples + 3] = source[x_pixel * 4 + 3];
				}
				if (TIFFWriteScanline(file, row.data(), y_row, 0) != 1)
				{
					close();
					return failed("could not write TIFF pixels");
				}
				progress(options, y_row + 1, image.height);
			}
			if (TIFFWriteDirectory(file) != 1)
			{
				close();
				return failed("could not write TIFF directory");
			}
			close();
			if (output.failed) return failed("could not write TIFF output");
			result.data = std::move(output.bytes);
			carry_tiff_exif(result.data, normalized_metadata.exif, options);
			result.success = true;
			return result;
		}

		document_encode_result encode_tiff_document(const decoded_document& document,
		                                            const document_encode_options& options)
		{
			std::string error;
			if (!valid_document(document, image_format::tiff, error)) return failed(std::move(error));

			document_encode_result result;
			tiff_output output;
			TIFF* file = TIFFClientOpen("libfiles", "w", &output, tiff_read, tiff_write, tiff_seek,
			                            [](thandle_t) -> int { return 0; }, tiff_size,
			                            [](thandle_t, void**, toff_t*) -> int { return 0; },
			                            [](thandle_t, void*, toff_t) {});
			if (!file) return failed("could not create TIFF writer");
			const auto close = [&] { TIFFClose(file); file = nullptr; };
			auto meta = embedded(document);
			std::vector<std::uint8_t> normalized_exif, normalized_xmp;
			normalize_orientation_metadata(meta, options.reset_orientation_metadata,
			                                normalized_exif, normalized_xmp,
			                                document.frames.front().width, document.frames.front().height);
			const auto comment = comment_block(document);
			const auto photoshop = tiff_photoshop(meta);
			for (std::size_t page = 0; page < document.frames.size(); ++page)
			{
				const auto& frame = document.frames[page];
				const auto palette_storage = palette_entries(frame.storage) != 0;
				const auto packed_storage = frame.storage == document_storage::bgr555 ||
					frame.storage == document_storage::bgr565;
				auto palette_alpha = frame.storage == document_storage::indexed8_alpha ||
					frame.transparent.has_value();
				if (palette_storage)
					for (const auto color : frame.palette)
						palette_alpha |= static_cast<std::uint8_t>(color >> 24) != 255;
				const auto indexed = palette_storage && !palette_alpha;
				const auto gray = frame.storage == document_storage::gray8;
				const auto alpha = frame.storage == document_storage::bgra32 || palette_alpha;
				const auto samples = indexed || gray ? 1 : alpha ? 4 : 3;
				const auto bits = indexed ? storage_bits(frame.storage) : 8;
				const auto photometric = indexed ? PHOTOMETRIC_PALETTE : gray ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB;
				if (!TIFFSetField(file, TIFFTAG_IMAGEWIDTH, frame.width) ||
					!TIFFSetField(file, TIFFTAG_IMAGELENGTH, frame.height) ||
					!TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, samples) ||
					!TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, bits) ||
					!TIFFSetField(file, TIFFTAG_PHOTOMETRIC, photometric) ||
					!TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
					!TIFFSetField(file, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) ||
					!TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS) ||
					!tiff_set_resolution(file, document, options))
				{
					close();
					return failed("could not set TIFF page fields");
				}
				if (options.legacy_compatibility)
				{
					const auto original_bytes = std::max<std::uint64_t>(
						1, (static_cast<std::uint64_t>(frame.width) * storage_bits(frame.storage)) / 8);
					const auto rows_per_strip = static_cast<std::uint32_t>(
						std::max<std::uint64_t>(1, (1024u * 8u) / original_bytes));
					if (!TIFFSetField(file, TIFFTAG_ROWSPERSTRIP,
					                  TIFFDefaultStripSize(file, rows_per_strip)) ||
						!TIFFSetField(file, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE))
					{
						close();
						return failed("could not set historical TIFF page fields");
					}
					char page_name[20]{};
					std::snprintf(page_name, sizeof(page_name), "Page %zu", page + 2);
					if (!TIFFSetField(file, TIFFTAG_PAGENUMBER,
					                  static_cast<std::uint16_t>(page + 2),
					                  static_cast<std::uint16_t>(document.frames.size())) ||
						!TIFFSetField(file, TIFFTAG_PAGENAME, page_name))
					{
						close();
						return failed("could not set historical TIFF page numbering");
					}
				}
				if (samples == 4)
				{
					const std::uint16_t extra = EXTRASAMPLE_UNASSALPHA;
					if (!TIFFSetField(file, TIFFTAG_EXTRASAMPLES, 1, &extra))
					{
						close();
						return failed("could not set TIFF alpha");
					}
				}
				if (!options.legacy_compatibility && document.frames.size() > 1)
				{
					TIFFSetField(file, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
					TIFFSetField(file, TIFFTAG_PAGENUMBER, static_cast<std::uint16_t>(page),
					             static_cast<std::uint16_t>(document.frames.size()));
				}
				if (indexed)
				{
					const auto entries = palette_entries(frame.storage);
					std::vector<std::uint16_t> red(entries), green(entries), blue(entries);
					for (std::size_t i = 0; i < entries; ++i)
					{
						red[i] = static_cast<std::uint16_t>((((frame.palette[i] >> 16) & 255) * 65535u + 127) / 255);
						green[i] = static_cast<std::uint16_t>((((frame.palette[i] >> 8) & 255) * 65535u + 127) / 255);
						blue[i] = static_cast<std::uint16_t>(((frame.palette[i] & 255) * 65535u + 127) / 255);
					}
					if (!TIFFSetField(file, TIFFTAG_COLORMAP, red.data(), green.data(), blue.data()))
					{
						close();
						return failed("could not set TIFF palette");
					}
				}
				if (!meta.xmp.empty()) TIFFSetField(file, TIFFTAG_XMLPACKET,
					static_cast<std::uint32_t>(meta.xmp.size()), meta.xmp.data());
				if (!meta.icc.empty()) TIFFSetField(file, TIFFTAG_ICCPROFILE,
					static_cast<std::uint32_t>(meta.icc.size()), meta.icc.data());
				if (!comment.empty())
				{
					std::string text(reinterpret_cast<const char*>(comment.data()), comment.size());
					TIFFSetField(file, TIFFTAG_IMAGEDESCRIPTION, text.c_str());
				}
				if (!photoshop.empty())
				{
					TIFFSetField(file, TIFFTAG_PHOTOSHOP, static_cast<std::uint32_t>(photoshop.size()), photoshop.data());
				}
				const auto packed_bytes = (static_cast<std::uint64_t>(frame.width) * bits + 7) / 8;
				const auto converted_bytes = static_cast<std::uint64_t>(frame.width) * samples;
				if (!indexed && !gray && !working_allowed(options, converted_bytes))
				{
					close();
					return unsupported("TIFF row exceeds the encoder working-memory limit");
				}
				std::vector<std::uint8_t> converted(
					indexed || gray ? 0 : static_cast<std::size_t>(converted_bytes));
				for (std::uint32_t y = 0; y < frame.height; ++y)
				{
					if (cancelled(options))
					{
						close();
						result.data.clear();
						fail_cancelled(result);
						return result;
					}
					const auto* const source = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride;
					const std::uint8_t* line = source;
					std::size_t line_size = static_cast<std::size_t>(packed_bytes);
					if (!indexed && !gray)
					{
						for (std::uint32_t x = 0; x < frame.width; ++x)
						{
							std::uint8_t blue = 0, green = 0, red = 0, opacity = 255;
							if (packed_storage)
							{
								const auto value = static_cast<std::uint16_t>(
									source[x * 2] | static_cast<std::uint16_t>(source[x * 2 + 1]) << 8);
								blue = static_cast<std::uint8_t>((value & 0x001fu) << 3);
								green = static_cast<std::uint8_t>(
									frame.storage == document_storage::bgr565
										? (value & 0x07e0u) >> 3
										: (value & 0x03e0u) >> 2);
								red = static_cast<std::uint8_t>(
									frame.storage == document_storage::bgr565
										? (value & 0xf800u) >> 8
										: (value & 0x7c00u) >> 7);
							}
							else if (palette_storage)
							{
								const auto index = index_at(frame, source, x);
								if (index >= frame.palette.size())
								{
									close();
									return failed("native frame palette index is out of range");
								}
								const auto color = frame.palette[index];
								blue = static_cast<std::uint8_t>(color);
								green = static_cast<std::uint8_t>(color >> 8);
								red = static_cast<std::uint8_t>(color >> 16);
								opacity = static_cast<std::uint8_t>(color >> 24);
								if (frame.transparent && index == *frame.transparent) opacity = 0;
							}
							else
							{
								const auto source_stride = frame.storage == document_storage::bgr24 ? 3u : 4u;
								blue = source[x * source_stride];
								green = source[x * source_stride + 1];
								red = source[x * source_stride + 2];
								if (samples == 4) opacity = source[x * source_stride + 3];
							}
							converted[x * samples] = red;
							converted[x * samples + 1] = green;
							converted[x * samples + 2] = blue;
							if (samples == 4) converted[x * samples + 3] = opacity;
						}
						line = converted.data();
						line_size = converted.size();
					}
					if (TIFFWriteScanline(file, const_cast<std::uint8_t*>(line), y, 0) != 1)
					{
						close();
						return failed("could not write TIFF pixels");
					}
					(void)line_size;
					progress(options, static_cast<std::uint32_t>(page * frame.height + y + 1),
					         static_cast<std::uint32_t>(document.frames.size() * frame.height));
				}
				if (TIFFWriteDirectory(file) != 1)
				{
					close();
					return failed("could not write TIFF directory");
				}
			}
			close();
			if (output.failed) return failed("could not write TIFF output");
			result.data = std::move(output.bytes);
			carry_tiff_exif(result.data, meta.exif, options);
			result.success = true;
			return result;
		}
#endif

#ifdef LIBFILES_JPEG
		document_encode_result encode_jpeg_document(const decoded_document& document,
		                                            const document_encode_options& options)
		{
			std::string error;
			if (!valid_document(document, image_format::jpeg, error)) return failed(std::move(error));
			if (document.frames.size() != 1) return unsupported("JPEG has one image frame");
			const auto& frame = document.frames.front();
			if (frame.storage != document_storage::gray8 && frame.storage != document_storage::bgr24 &&
				frame.storage != document_storage::bgrx32 && frame.storage != document_storage::bgra32)
				return unsupported("JPEG does not represent indexed or packed native storage");

			document_encode_result result;
			try
			{
				jpeg_encoder encoder;
				jpeg_options jpeg;
				jpeg.quality = options.quality;
				jpeg.progressive = options.progressive;
				jpeg.optimize = options.optimize;
				jpeg.pixels_per_meter_x = resolution_x(document, options);
				jpeg.pixels_per_meter_y = resolution_y(document, options);
				jpeg.legacy_density = options.legacy_compatibility;
				auto metadata = embedded(document);
				std::vector<std::uint8_t> normalized_exif, normalized_xmp;
				normalize_orientation_metadata(metadata, options.reset_orientation_metadata,
				                                normalized_exif, normalized_xmp, frame.width, frame.height);
				if (frame.storage == document_storage::gray8)
				{
					jpeg.grayscale = true;
				}
				if (frame.storage == document_storage::bgr24) jpeg.bgr24 = true;

				encoder.start(frame.width, frame.height, 1, metadata, jpeg);
				std::vector<std::uint8_t*> rows(1);
				while (encoder.next_row() < frame.height)
				{
					if (cancelled(options))
					{
						encoder.complete(true);
						fail_cancelled(result);
						return result;
					}
					const auto y = encoder.next_row();
					const auto* const source = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride;
					rows[0] = const_cast<std::uint8_t*>(source);
					encoder.write_rows(rows.data(), 1);
					progress(options, encoder.next_row(), frame.height);
				}
				result.data = encoder.complete();
				result.warnings = encoder.notes();
				result.success = !result.data.empty();
				if (!result.success) result.error = "JPEG encoder produced no data";
				return result;
			}
			catch (const std::bad_alloc&)
			{
				result.resource_limit = true;
				result.error = "not enough memory for JPEG output";
				return result;
			}
			catch (const encode_error& exception)
			{
				result.error = exception.what();
				return result;
			}
		}

		document_encode_result encode_jpeg_image(const image_view& image, const document_info& info,
		                                         const embedded_metadata& metadata,
		                                         const document_encode_options& options)
		{
			if (image.format != pixel_format::bgrx8 && image.format != pixel_format::bgra8)
				return unsupported("JPEG row encoding accepts BGRX8 or BGRA8");
			try
			{
				auto normalized_metadata = metadata;
				std::vector<std::uint8_t> normalized_exif, normalized_xmp;
				normalize_orientation_metadata(normalized_metadata, options.reset_orientation_metadata,
				                                normalized_exif, normalized_xmp, image.width, image.height);
				jpeg_encoder encoder;
				jpeg_options jpeg;
				jpeg.quality = options.quality;
				jpeg.progressive = options.progressive;
				jpeg.optimize = options.optimize;
				jpeg.legacy_density = options.legacy_compatibility;
				jpeg.pixels_per_meter_x = info.has_resolution ? info.pixels_per_meter_x : options.pixels_per_meter_x;
				jpeg.pixels_per_meter_y = info.has_resolution ? info.pixels_per_meter_y : options.pixels_per_meter_y;
				encoder.start(image.width, image.height, 1, normalized_metadata, jpeg);
				std::vector<std::uint8_t*> rows(1);
				while (encoder.next_row() < image.height)
				{
					if (cancelled(options))
					{
						encoder.complete(true);
						document_encode_result result;
						fail_cancelled(result);
						return result;
					}
					const auto y = encoder.next_row();
					rows[0] = const_cast<std::uint8_t*>(image.pixels + static_cast<std::size_t>(y) * image.stride);
					encoder.write_rows(rows.data(), 1);
					progress(options, encoder.next_row(), image.height);
				}
				document_encode_result result;
				result.data = encoder.complete();
				result.warnings = encoder.notes();
				result.success = !result.data.empty();
				if (image.format == pixel_format::bgra8)
					result.warnings.emplace_back("JPEG does not carry alpha; the colour channels were encoded");
				if (!result.success) result.error = "JPEG encoder produced no data";
				return result;
			}
			catch (const std::bad_alloc&)
			{
				document_encode_result result;
				result.resource_limit = true;
				result.error = "not enough memory for JPEG output";
				return result;
			}
			catch (const encode_error& exception)
			{
				return failed(exception.what());
			}
		}
#endif
	}

	bool can_encode_document(const image_format format) noexcept
	{
		switch (format)
		{
		case image_format::bmp:
			return true;
		case image_format::jpeg:
#ifdef LIBFILES_JPEG
			return true;
#else
			return false;
#endif
		case image_format::gif:
#ifdef LIBFILES_GIF
			return true;
#else
			return false;
#endif
		case image_format::png:
#ifdef LIBFILES_PNG
			return true;
#else
			return false;
#endif
		case image_format::tiff:
#ifdef LIBFILES_TIFF
			return true;
#else
			return false;
#endif
		default:
			return false;
		}
	}

	document_encode_result encode_document(const decoded_document& document, const image_format format,
	                                        const document_encode_options& options)
	{
		if (cancelled(options))
		{
			document_encode_result result;
			fail_cancelled(result);
			return result;
		}
		try
		{
			switch (format)
			{
			case image_format::bmp: return encode_bmp(document, options);
			case image_format::jpeg:
#ifdef LIBFILES_JPEG
				return encode_jpeg_document(document, options);
#else
				return unsupported("libfiles was built without JPEG encoding");
#endif
			case image_format::png:
#ifdef LIBFILES_PNG
				return encode_png_document(document, options);
#else
				return unsupported("libfiles was built without PNG encoding");
#endif
			case image_format::gif:
#ifdef LIBFILES_GIF
				return encode_gif_document(document, options);
#else
				return unsupported("libfiles was built without GIF encoding");
#endif
			case image_format::tiff:
#ifdef LIBFILES_TIFF
				return encode_tiff_document(document, options);
#else
				return unsupported("libfiles was built without TIFF encoding");
#endif
			default:
				return unsupported("libfiles has no native encoder for this format");
			}
		}
		catch (const std::bad_alloc&)
		{
			document_encode_result result;
			result.resource_limit = true;
			result.error = "not enough memory for native output";
			return result;
		}
		catch (const encode_error& exception)
		{
			return failed(exception.what());
		}
	}

	document_encode_result encode_document(const image_view& image, const image_format format,
	                                        const document_info& info, const embedded_metadata& metadata,
	                                        const document_encode_options& options)
	{
		if (!image.pixels || image.width == 0 || image.height == 0)
			return failed("invalid image rows");
		const auto bpp = image.format == pixel_format::nv12 ? 1u : 4u;
		if (image.format == pixel_format::nv12 ||
			image.width > std::numeric_limits<std::size_t>::max() / bpp ||
			image.stride < static_cast<std::size_t>(image.width) * bpp)
			return unsupported("native row encoding does not accept this pixel format");
		if (image.height != 0 && image.stride > std::numeric_limits<std::size_t>::max() / image.height)
			return failed("image rows are too large");
		if (format == image_format::bmp) return encode_bmp_image(image, info, options);
		if (format == image_format::jpeg)
		{
#ifdef LIBFILES_JPEG
			return encode_jpeg_image(image, info, metadata, options);
#else
			return unsupported("libfiles was built without JPEG encoding");
#endif
		}
		if (format == image_format::png)
		{
#ifdef LIBFILES_PNG
			try
			{
				auto row_metadata = metadata;
				std::vector<std::uint8_t> normalized_exif, normalized_xmp;
				normalize_orientation_metadata(row_metadata, options.reset_orientation_metadata,
				                                normalized_exif, normalized_xmp, image.width, image.height);
				row_metadata.pixels_per_meter_x = info.has_resolution ? info.pixels_per_meter_x :
					options.pixels_per_meter_x;
				row_metadata.pixels_per_meter_y = info.has_resolution ? info.pixels_per_meter_y :
					options.pixels_per_meter_y;
				const auto result = encode_png(image, 1, row_metadata);
				document_encode_result encoded_result;
				encoded_result.data = result.data;
				encoded_result.warnings = result.notes;
				if (!row_metadata.iptc.empty() || !row_metadata.photoshop.empty())
					encoded_result.warnings.emplace_back("PNG row encoding does not carry IPTC or Photoshop resources");
				encoded_result.success = !encoded_result.data.empty();
				if (!encoded_result.success) encoded_result.error = "PNG encoder produced no data";
				if (options.cancelled && options.cancelled())
				{
					encoded_result.data.clear();
					fail_cancelled(encoded_result);
				}
				else
					progress(options, image.height, image.height);
				return encoded_result;
			}
			catch (const std::bad_alloc&)
			{
				document_encode_result result;
				result.resource_limit = true;
				result.error = "not enough memory for PNG output";
				return result;
			}
			catch (const encode_error& exception)
			{
				return failed(exception.what());
			}
#else
			return unsupported("libfiles was built without PNG encoding");
#endif
		}
		if (format == image_format::tiff)
		{
#ifdef LIBFILES_TIFF
			try
			{
				return encode_tiff_image(image, info, metadata, options);
			}
			catch (const std::bad_alloc&)
			{
				document_encode_result result;
				result.resource_limit = true;
				result.error = "not enough memory for TIFF output";
				return result;
			}
			catch (const encode_error& exception)
			{
				return failed(exception.what());
			}
#else
			return unsupported("libfiles was built without TIFF encoding");
#endif
		}
		if (format == image_format::gif)
			return unsupported("GIF row encoding requires an indexed document frame");

		decoded_document document;
		document.info = info;
		document.info.width = image.width;
		document.info.height = image.height;
		document.info.storage = image.format == pixel_format::bgra8 ? document_storage::bgra32 :
			document_storage::bgrx32;
		document.info.bits_per_pixel = 32;
		document.info.frames = 1;
		document.info.has_resolution = info.has_resolution ||
			(options.pixels_per_meter_x > 0 && options.pixels_per_meter_y > 0);
		document.info.pixels_per_meter_x = info.has_resolution ? info.pixels_per_meter_x : options.pixels_per_meter_x;
		document.info.pixels_per_meter_y = info.has_resolution ? info.pixels_per_meter_y : options.pixels_per_meter_y;
		decoded_frame frame;
		frame.width = image.width;
		frame.height = image.height;
		frame.storage = document.info.storage;
		frame.stride = image.stride;
		const auto image_bytes = image.stride * static_cast<std::size_t>(image.height);
		frame.pixels.assign(image.pixels, image.pixels + image_bytes);
		document.frames.push_back(std::move(frame));
		const auto add = [&document](const document_metadata kind, const std::span<const std::uint8_t> bytes)
		{
			if (!bytes.empty()) document.metadata.push_back({kind, {bytes.begin(), bytes.end()}});
		};
		add(document_metadata::exif, metadata.exif);
		add(document_metadata::xmp, metadata.xmp);
		add(document_metadata::iptc, metadata.iptc);
		add(document_metadata::icc, metadata.icc);
		add(document_metadata::photoshop, metadata.photoshop);
		return encode_document(document, format, options);
	}

	document_encode_result encode_document(writable_stream& destination, const decoded_document& document,
	                                        const image_format format, const document_encode_options& options)
	{
		auto result = encode_document(document, format, options);
		if (!result.success) return result;
		if (!destination.resize(result.data.size()) ||
			!destination.write(0, result.data.data(), result.data.size()) ||
			!destination.flush())
		{
			result.success = false;
			result.error = "the encoded document could not be written";
			return result;
		}
		return result;
	}
}
