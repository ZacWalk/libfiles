// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Decoding a HEIF or AVIF with libheif into memory the caller provides, as B8G8R8A8 with straight
// alpha where the image has alpha and B8G8R8X8 where it has none: the primary image, or its first
// thumbnail item. libheif applies the item's own rotation and mirror; otherwise the orientation is the
// primary's EXIF. An HEVC image can be steered to a decoder the caller names - Diffractor's FFmpeg plugin.
// The sink accepts the size the item declares before libheif decodes, and allocates the size decoded.
// Moved from Diffractor's loader unchanged.

#include "decode_internal.h"

#include "libfiles/scan.h"
#include "scan_internal.h"

#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		// What libheif reads through: the reader, from a position it moves.
		struct heif_read_stream
		{
			reader& r;
			std::uint64_t pos = 0;
		};

		std::int64_t get_position(void* userdata)
		{
			return static_cast<std::int64_t>(static_cast<heif_read_stream*>(userdata)->pos);
		}

		int read(void* data, const size_t size, void* userdata)
		{
			auto* const s = static_cast<heif_read_stream*>(userdata);
			if (s->pos > s->r.size() || size > s->r.size() - s->pos) return heif_error_Usage_error;

			try
			{
				s->r.read(s->pos, data, size);
				s->pos += size;
				return heif_error_Ok;
			}
			catch (...)
			{
				return heif_error_Usage_error;
			}
		}

		int seek(const std::int64_t position, void* userdata)
		{
			auto* const s = static_cast<heif_read_stream*>(userdata);
			if (position < 0 || static_cast<std::uint64_t>(position) > s->r.size()) return heif_error_Usage_error;
			s->pos = static_cast<std::uint64_t>(position);
			return heif_error_Ok;
		}

		heif_reader_grow_status wait_for_file_size(const std::int64_t target_size, void* userdata)
		{
			const auto* const s = static_cast<heif_read_stream*>(userdata);
			if (target_size < 0 || static_cast<std::uint64_t>(target_size) > s->r.size())
				return heif_reader_grow_status_size_beyond_eof;
			return heif_reader_grow_status_size_reached;
		}

		struct context_free
		{
			void operator()(heif_context* c) const noexcept { heif_context_free(c); }
		};

		struct handle_release
		{
			void operator()(heif_image_handle* h) const noexcept { heif_image_handle_release(h); }
		};

		struct image_release
		{
			void operator()(heif_image* i) const noexcept { heif_image_release(i); }
		};

		using context_ptr = std::unique_ptr<heif_context, context_free>;
		using handle_ptr = std::unique_ptr<heif_image_handle, handle_release>;
		using image_ptr = std::unique_ptr<heif_image, image_release>;
		using options_ptr = std::unique_ptr<heif_decoding_options, decltype(&heif_decoding_options_free)>;

		void fail(decode_result& result, const heif_error& error)
		{
			result.success = false;
			result.truncated = error.subcode == heif_suberror_End_of_data;
			result.error = error.message && *error.message ? error.message : "libheif could not read the file";
		}

		// Whether an item's pixels are HEVC: an hvc1 item, or a grid, overlay or identity image derived
		// from one. Only these are steered to a named decoder, because a named decoder is used for
		// whatever the item holds - a JPEG or AV1 item named to FFmpeg would change decoder too, and an
		// uncompressed one would not decode at all.
		bool is_hevc_item(const heif_context* ctx, const heif_item_id id, const int depth = 0)
		{
			const auto type = heif_item_get_item_type(ctx, id);
			if (type == heif_fourcc('h', 'v', 'c', '1')) return true;

			const auto derived = type == heif_fourcc('g', 'r', 'i', 'd') || type == heif_fourcc('i', 'o', 'v', 'l') ||
				type == heif_fourcc('i', 'd', 'e', 'n');
			if (!derived || depth >= 4) return false;

			// A derived image's 'dimg' reference lists its inputs, which share one codec in every writer
			// that exists, so the first stands for the rest.
			for (auto index = 0; index < 16; ++index)
			{
				std::uint32_t reference_type = 0;
				heif_item_id* references = nullptr;
				const auto count = heif_context_get_item_references(ctx, id, index, &reference_type, &references);
				const auto first = count > 0 ? references[0] : 0;
				heif_release_item_references(ctx, &references);

				if (count == 0) return false;
				if (reference_type == heif_fourcc('d', 'i', 'm', 'g')) return is_hevc_item(ctx, first, depth + 1);
			}

			return false;
		}

		// The registered HEVC decoder of this id - its name as libheif keeps it - or null.
		const char* registered_hevc_decoder(const std::string_view id)
		{
			if (id.empty()) return nullptr;

			std::array<const heif_decoder_descriptor*, 16> descriptors = {};
			const auto count = heif_get_decoder_descriptors(heif_compression_HEVC, descriptors.data(),
			                                                static_cast<int>(descriptors.size()));

			for (auto i = 0; i < count; ++i)
			{
				const auto* const name = heif_decoder_descriptor_get_id_name(descriptors[i]);
				if (name && std::string_view(name) == id) return name;
			}

			return nullptr;
		}

		// The options an item decodes with: the named decoder for HEVC, and null - libheif's defaults -
		// otherwise.
		options_ptr decoding_options_for(const heif_context* ctx, const heif_image_handle* handle,
		                                 const std::string_view hevc_decoder)
		{
			const auto* const id = registered_hevc_decoder(hevc_decoder);

			if (!id || !is_hevc_item(ctx, heif_image_handle_get_item_id(handle)))
			{
				return {nullptr, heif_decoding_options_free};
			}

			options_ptr options(heif_decoding_options_alloc(), heif_decoding_options_free);
			if (options) options->decoder_id = id;
			return options;
		}

		bool has_orientation_transform(const heif_context* context, const heif_image_handle* image_handle)
		{
			const auto item_id = heif_image_handle_get_item_id(image_handle);
			return heif_item_get_properties_of_type(context, item_id, heif_item_property_type_transform_rotation, nullptr, 0) > 0 ||
				heif_item_get_properties_of_type(context, item_id, heif_item_property_type_transform_mirror, nullptr, 0) > 0;
		}

		// The item's first EXIF block, its offset to the TIFF header and any JPEG signature removed.
		// libheif filters the blocks by type, so the XMP, IPTC and often large ICC blocks are not read.
		std::vector<std::uint8_t> extract_exif(const heif_image_handle* handle)
		{
			heif_item_id id = 0;
			if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &id, 1) != 1) return {};

			std::vector<std::uint8_t> raw(heif_image_handle_get_metadata_size(handle, id), 0);
			if (heif_image_handle_get_metadata(handle, id, raw.data()).code != heif_error_Ok) return {};

			strip_exif_offset(raw);
			return raw;
		}

		// RGBA as libheif gives it, rows heif_stride apart, written as BGRA - straight alpha where it was
		// premultiplied, each channel c * 255 / alpha rounded, and 0 where alpha is.
		void write_bgra(const std::uint8_t* const source, const int heif_stride, const pixel_buffer pixels,
		                const std::uint32_t width, const std::uint32_t height, const bool unpremultiply)
		{
			for (std::uint32_t y = 0; y < height; ++y)
			{
				const auto* src = source + static_cast<std::size_t>(heif_stride) * y;
				auto* dst = pixels.pixels + pixels.stride * y;

				for (std::uint32_t x = 0; x < width; ++x, src += 4, dst += 4)
				{
					dst[0] = src[2];
					dst[1] = src[1];
					dst[2] = src[0];
					dst[3] = src[3];

					if (unpremultiply)
					{
						const auto alpha = dst[3];

						for (auto channel = 0; channel < 3; ++channel)
						{
							dst[channel] = alpha == 0
								               ? 0
								               : static_cast<std::uint8_t>(std::min(255, (dst[channel] * 255 + alpha / 2) / alpha));
						}
					}
				}
			}
		}
	}

	void decode_heif(reader& r, image_sink& sink, const decode_options& options, decode_result& result)
	{
		heif_read_stream stream{r};
		heif_reader heif_reader_api = {};
		heif_reader_api.reader_api_version = 1;
		heif_reader_api.get_position = get_position;
		heif_reader_api.read = read;
		heif_reader_api.seek = seek;
		heif_reader_api.wait_for_file_size = wait_for_file_size;

		const context_ptr ctx(heif_context_alloc());

		if (!ctx)
		{
			result.error = "could not create a libheif context";
			return;
		}

		const auto opened = heif_context_read_from_reader(ctx.get(), &heif_reader_api, &stream, nullptr);

		if (opened.code != heif_error_Ok)
		{
			fail(result, opened);
			return;
		}

		heif_image_handle* primary_handle = nullptr;
		const auto got_primary = heif_context_get_primary_image_handle(ctx.get(), &primary_handle);
		const handle_ptr primary(primary_handle);

		if (got_primary.code != heif_error_Ok)
		{
			fail(result, got_primary);
			return;
		}

		handle_ptr thumbnail;
		const heif_image_handle* item = primary.get();

		if (options.heif_thumbnail)
		{
			heif_item_id thumbnail_id = 0;

			if (heif_image_handle_get_list_of_thumbnail_IDs(primary.get(), &thumbnail_id, 1) <= 0)
			{
				result.error = "the HEIF has no thumbnail item";
				return;
			}

			heif_image_handle* thumbnail_handle = nullptr;
			const auto got_thumbnail = heif_image_handle_get_thumbnail(primary.get(), thumbnail_id, &thumbnail_handle);
			thumbnail.reset(thumbnail_handle);

			if (got_thumbnail.code != heif_error_Ok)
			{
				fail(result, got_thumbnail);
				return;
			}

			item = thumbnail.get();
		}

		const auto declared_width = heif_image_handle_get_width(item);
		const auto declared_height = heif_image_handle_get_height(item);
		const auto has_alpha = heif_image_handle_has_alpha_channel(item) != 0;

		// libheif gives the item's 32-bit 'ispe' extent as an int; the cast restores it.
		frame_info frame;
		frame.source_width = static_cast<std::uint32_t>(declared_width);
		frame.source_height = static_cast<std::uint32_t>(declared_height);
		frame.width = frame.source_width;
		frame.height = frame.source_height;
		frame.format = has_alpha ? pixel_format::bgra8 : pixel_format::bgrx8;
		frame.transformed = has_orientation_transform(ctx.get(), item);

		const auto decoding_options = decoding_options_for(ctx.get(), item, options.heif_hevc_decoder);
		if (decoding_options) frame.heif_decoder = decoding_options->decoder_id;

		// libheif has already turned the pixels for the item's own 'irot'/'imir', so the EXIF orientation
		// that describes the same turn would turn the image twice. A thumbnail item's orientation is its
		// own transform alone.
		std::vector<std::uint8_t> exif;

		if (!options.heif_thumbnail)
		{
			exif = extract_exif(primary.get());
			frame.exif = exif;
			if (!frame.transformed) frame.orientation = summarize_exif(exif).orientation.value_or(1);
		}

		// The sink judges the size the item declares before libheif decodes anything, whatever that size
		// is: a malformed file may declare none, or one its codec does not decode to.
		result.frame = frame;
		result.frame.exif = {};

		if (!sink.accept(frame))
		{
			result.refused = true;
			return;
		}

		heif_image* decoded_image = nullptr;
		const auto decoded = heif_decode_image(item, &decoded_image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
		                                       decoding_options.get());
		const image_ptr image(decoded_image);

		if (decoded.code != heif_error_Ok)
		{
			fail(result, decoded);
			return;
		}

		// The plane about to be read, not the item: the two may differ, and trusting the item's extent
		// would read past the end of libheif's buffer.
		const auto plane_width = heif_image_get_width(image.get(), heif_channel_interleaved);
		const auto plane_height = heif_image_get_height(image.get(), heif_channel_interleaved);

		if (plane_width <= 0 || plane_height <= 0 || plane_width > max_image_dimension ||
			plane_height > max_image_dimension)
		{
			result.error = "the decoded HEIF image is larger than libfiles decodes, or has no size";
			return;
		}

		int heif_stride = 0;
		const auto* const plane = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &heif_stride);

		if (!plane || heif_stride <= 0 || static_cast<std::size_t>(heif_stride) < std::size_t{4} * plane_width)
		{
			result.error = "libheif gave no interleaved plane";
			return;
		}

		frame.width = static_cast<std::uint32_t>(plane_width);
		frame.height = static_cast<std::uint32_t>(plane_height);
		const auto pixels = sink.allocate(frame);
		result.frame = frame;
		result.frame.exif = {};

		if (!pixels.pixels)
		{
			result.refused = true;
			return;
		}

		write_bgra(plane, heif_stride, pixels, frame.width, frame.height,
		           has_alpha && heif_image_handle_is_premultiplied_alpha(item));
		result.success = true;
	}
}
