// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Decoding a HEIF or AVIF with libheif into memory the caller provides, as B8G8R8A8 with straight
// alpha where the image has alpha and B8G8R8X8 where it has none - or, for an image of 10 or 12 bits and a
// caller that takes sixteen, as R16G16B16A16 - the primary image, its first thumbnail item, or its gain map. An
// image described as PQ or HLG is an HDR still, mapped to SDR as hdr_internal.h describes, or kept as its signal
// for a caller that keeps HDR. The gain map is the input of an ISO 21496-1 tone map item that takes the primary,
// else Apple's HDR gain map auxiliary image, whose values are rewritten in ISO 21496-1's terms. libheif applies
// the item's own rotation and mirror; otherwise the orientation is the primary's EXIF. An HEVC image can be
// steered to a decoder the caller names - Diffractor's FFmpeg plugin. The sink accepts the size the item
// declares before libheif decodes, and allocates the size decoded.

#include "decode_internal.h"
#include "gain_map_internal.h"
#include "hdr_internal.h"

#include "libfiles/hdr.h"
#include "libfiles/scan.h"
#include "scan_internal.h"

#include <libheif/heif.h>
#include <libheif/heif_aux_images.h>
#include <libheif/heif_items.h>
#include <libheif/heif_metadata.h>
#include <libheif/heif_properties.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
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
			return heif_item_get_properties_of_type(context, item_id, heif_item_property_type_transform_rotation,
			                                        nullptr, 0) > 0 ||
			       heif_item_get_properties_of_type(context, item_id, heif_item_property_type_transform_mirror, nullptr,
			                                        0) > 0;
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
							dst[channel] = alpha == 0 ? 0
							                          : static_cast<std::uint8_t>(
															std::min(255, (dst[channel] * 255 + alpha / 2) / alpha));
						}
					}
				}
			}
		}

		// What the item's colour box says of an HDR picture, when its transfer is PQ or HLG. libheif converts the
		// item's YCbCr to R'G'B' by the same description, so the planes it gives are the signal. The light levels
		// are the decoded image's, added once it is decoded.
		std::optional<hdr_description> describe_hdr(const heif_image_handle* item)
		{
			heif_color_profile_nclx* nclx = nullptr;
			if (heif_image_handle_get_nclx_color_profile(item, &nclx).code != heif_error_Ok || !nclx) return {};

			std::optional<hdr_description> described;

			if (const auto transfer = hdr_transfer_of(nclx->transfer_characteristics))
			{
				described.emplace();
				described->transfer = *transfer;
				described->colour_primaries = static_cast<std::uint8_t>(nclx->color_primaries);
				described->primaries = {nclx->color_primary_red_x,   nclx->color_primary_red_y,
				                        nclx->color_primary_green_x, nclx->color_primary_green_y,
				                        nclx->color_primary_blue_x,  nclx->color_primary_blue_y,
				                        nclx->color_primary_white_x, nclx->color_primary_white_y};
			}

			heif_nclx_color_profile_free(nclx);
			return described;
		}

		// The picture's content light level and mastering display, which libheif gives with the decoded image.
		void add_light_levels(const heif_image* image, hdr_description& hdr)
		{
			if (heif_image_has_content_light_level(image))
			{
				heif_content_light_level level{};
				heif_image_get_content_light_level(image, &level);
				hdr.max_cll = level.max_content_light_level;
				hdr.max_fall = level.max_pic_average_light_level;
			}

			if (heif_image_has_mastering_display_colour_volume(image))
			{
				heif_mastering_display_colour_volume volume{};
				heif_decoded_mastering_display_colour_volume display{};
				heif_image_get_mastering_display_colour_volume(image, &volume);

				if (heif_mastering_display_colour_volume_decode(&volume, &display).code == heif_error_Ok)
				{
					// The box lists the display's primaries green, blue, red, as HEVC's SEI does.
					hdr_description::mastering_display mastering;
					mastering.primaries = {display.display_primaries_x[2], display.display_primaries_y[2],
					                       display.display_primaries_x[0], display.display_primaries_y[0],
					                       display.display_primaries_x[1], display.display_primaries_y[1],
					                       display.white_point_x,          display.white_point_y};
					mastering.max_luminance = display.max_display_mastering_luminance;
					mastering.min_luminance = display.min_display_mastering_luminance;
					hdr.mastering = mastering;
				}
			}
		}

		constexpr std::string_view apple_gain_map_type = "urn:com:apple:photo:2020:aux:hdrgainmap";

		// The primary image's gain map: the input an ISO 21496-1 tone map item takes beside the primary, with the
		// metadata the item holds; else Apple's HDR gain map auxiliary image, with the headroom Apple's metadata
		// gives it, its values rewritten once decoded.
		struct gain_map_source
		{
			handle_ptr handle;
			gain_map_metadata metadata;
			// Apple's map: its headroom, above 1.
			double apple_headroom = 0;
		};

		std::optional<gain_map_source> find_iso_gain_map(heif_context* ctx, const heif_image_handle* primary)
		{
			const auto primary_id = heif_image_handle_get_item_id(primary);
			const auto count = heif_context_get_number_of_items(ctx);
			if (count <= 0) return {};

			std::vector<heif_item_id> ids(static_cast<std::size_t>(count));
			ids.resize(static_cast<std::size_t>(heif_context_get_list_of_item_IDs(ctx, ids.data(), count)));

			for (const auto id : ids)
			{
				if (heif_item_get_item_type(ctx, id) != heif_fourcc('t', 'm', 'a', 'p')) continue;

				// Its 'dimg' reference lists the base image, then the gain map.
				heif_item_id gain_map = 0;

				for (auto index = 0; index < 16; ++index)
				{
					std::uint32_t type = 0;
					heif_item_id* to = nullptr;
					const auto n = heif_context_get_item_references(ctx, id, index, &type, &to);
					if (type == heif_fourcc('d', 'i', 'm', 'g') && n == 2 && to[0] == primary_id) gain_map = to[1];
					heif_release_item_references(ctx, &to);
					if (n == 0 || gain_map != 0) break;
				}

				if (gain_map == 0) continue;

				std::uint8_t* data = nullptr;
				std::size_t size = 0;
				std::optional<gain_map_metadata> metadata;

				if (heif_item_get_item_data(ctx, id, nullptr, &data, &size).code == heif_error_Ok && data && size > 1 &&
				    data[0] == 0)
				{
					metadata = read_gain_map_metadata({data + 1, size - 1});
				}

				heif_release_item_data(ctx, &data);
				if (!metadata) continue;

				heif_image_handle* handle = nullptr;
				if (heif_context_get_image_handle(ctx, gain_map, &handle).code != heif_error_Ok) continue;

				return gain_map_source{handle_ptr(handle), *metadata, 0};
			}

			return {};
		}

		std::optional<gain_map_source> find_apple_gain_map(const heif_image_handle* primary,
		                                                   const std::span<const std::uint8_t> exif)
		{
			constexpr int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA | LIBHEIF_AUX_IMAGE_FILTER_OMIT_DEPTH;
			const auto count = heif_image_handle_get_number_of_auxiliary_images(primary, filter);
			if (count <= 0) return {};

			std::vector<heif_item_id> ids(static_cast<std::size_t>(count));
			ids.resize(static_cast<std::size_t>(
				heif_image_handle_get_list_of_auxiliary_image_IDs(primary, filter, ids.data(), count)));

			for (const auto id : ids)
			{
				heif_image_handle* raw = nullptr;
				if (heif_image_handle_get_auxiliary_image_handle(primary, id, &raw).code != heif_error_Ok) continue;
				handle_ptr handle(raw);

				const char* type = nullptr;
				if (heif_image_handle_get_auxiliary_type(handle.get(), &type).code != heif_error_Ok) continue;
				const auto is_gain_map = type && std::string_view(type) == apple_gain_map_type;
				heif_image_handle_release_auxiliary_type(handle.get(), &type);
				if (!is_gain_map) continue;

				// The headroom is in the gain map's own XMP, or the photo's maker note.
				std::vector<std::uint8_t> xmp;
				heif_item_id blocks[8] = {};
				const auto n = heif_image_handle_get_list_of_metadata_block_IDs(handle.get(), "mime", blocks, 8);

				for (auto i = 0; i < n && xmp.empty(); ++i)
				{
					const auto* const content = heif_image_handle_get_metadata_content_type(handle.get(), blocks[i]);
					if (!content || std::string_view(content) != "application/rdf+xml") continue;

					xmp.resize(heif_image_handle_get_metadata_size(handle.get(), blocks[i]));
					if (heif_image_handle_get_metadata(handle.get(), blocks[i], xmp.data()).code != heif_error_Ok)
						xmp.clear();
				}

				const auto headroom = apple_gain_map_headroom(xmp, exif);
				if (!headroom) continue;

				return gain_map_source{std::move(handle), {}, *headroom};
			}

			return {};
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
		// The primary's EXIF, which an Apple gain map's headroom may come from, and the orientation besides.
		std::vector<std::uint8_t> exif;
		if (!options.heif_thumbnail) exif = extract_exif(primary.get());
		std::optional<gain_map_source> gain_map;

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
		else if (options.heif_gain_map)
		{
			gain_map = find_iso_gain_map(ctx.get(), primary.get());
			if (!gain_map) gain_map = find_apple_gain_map(primary.get(), exif);

			if (!gain_map)
			{
				result.error = "the HEIF has no gain map";
				return;
			}

			item = gain_map->handle.get();
		}

		const auto declared_width = heif_image_handle_get_width(item);
		const auto declared_height = heif_image_handle_get_height(item);
		// A gain map's values are not a picture's colours, and no alpha goes with them.
		const auto has_alpha = !gain_map && heif_image_handle_has_alpha_channel(item) != 0;
		const auto luma_bits = heif_image_handle_get_luma_bits_per_pixel(item);
		const auto apple = gain_map && gain_map->apple_headroom > 0.0;

		// An item described as PQ or HLG is an HDR still: mapped to SDR as hdr_internal.h describes, or kept as its
		// signal for a caller that keeps HDR.
		auto hdr = gain_map ? std::optional<hdr_description>{} : describe_hdr(item);
		const auto keep = hdr && options.keep_hdr;

		// libheif gives the item's 32-bit 'ispe' extent as an int; the cast restores it.
		frame_info frame;
		frame.source_width = static_cast<std::uint32_t>(declared_width);
		frame.source_height = static_cast<std::uint32_t>(declared_height);
		frame.width = frame.source_width;
		frame.height = frame.source_height;
		// Apple's map is rewritten a byte at a time, as it is coded.
		frame.format = apple ? pixel_format::bgrx8 : output_format(luma_bits, has_alpha, options, keep);
		frame.transformed = has_orientation_transform(ctx.get(), item);
		frame.hdr = hdr;
		if (gain_map) frame.gain_map = apple ? apple_gain_map_metadata(gain_map->apple_headroom) : gain_map->metadata;

		const auto decoding_options = decoding_options_for(ctx.get(), item, options.heif_hevc_decoder);
		if (decoding_options) frame.heif_decoder = decoding_options->decoder_id;

		// libheif has already turned the pixels for the item's own 'irot'/'imir', so the EXIF orientation
		// that describes the same turn would turn the image twice. A thumbnail item's orientation is its
		// own transform alone. A gain map lines up with the primary as the file stores both, so it takes the
		// primary's EXIF orientation where it is not turned by its own.
		if (!options.heif_thumbnail)
		{
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

		// Sixteen bits are decoded where they are kept or mapped. An 8-bit HDR item - rare - is widened below.
		const auto wide = frame.format == pixel_format::rgba16 || frame.format == pixel_format::rgbx16;
		const auto decode_wide = (wide || hdr) && luma_bits > 8;

		heif_image* decoded_image = nullptr;
		const auto decoded = heif_decode_image(item, &decoded_image, heif_colorspace_RGB,
		                                       decode_wide ? heif_chroma_interleaved_RRGGBBAA_LE
		                                                   : heif_chroma_interleaved_RGBA,
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

		if (!plane || heif_stride <= 0 ||
		    static_cast<std::size_t>(heif_stride) < std::size_t{decode_wide ? 8u : 4u} * plane_width)
		{
			result.error = "libheif gave no interleaved plane";
			return;
		}

		if (hdr) add_light_levels(image.get(), *hdr);

		frame.width = static_cast<std::uint32_t>(plane_width);
		frame.height = static_cast<std::uint32_t>(plane_height);
		frame.hdr = hdr;
		frame.tone_mapped = hdr && !keep;
		const auto pixels = sink.allocate(frame);
		result.frame = frame;
		result.frame.exif = {};

		if (!pixels.pixels)
		{
			result.refused = true;
			return;
		}

		const auto premultiplied = has_alpha && heif_image_handle_is_premultiplied_alpha(item);

		if (!decode_wide && !hdr)
		{
			write_bgra(plane, heif_stride, pixels, frame.width, frame.height, premultiplied);
			if (apple) convert_apple_gain_map(pixels, frame.width, frame.height, gain_map->apple_headroom);
			result.success = true;
			return;
		}

		// Mapped to SDR unless the caller keeps the signal.
		std::optional<hdr_mapping> mapping;
		if (hdr && !keep) mapping.emplace(*hdr);

		if (decode_wide)
		{
			const auto bits = heif_image_get_bits_per_pixel_range(image.get(), heif_channel_interleaved);
			const auto max_value = bits > 0 && bits <= 16 ? (1u << bits) - 1u : 65535u;
			write_wide_rows(plane, static_cast<std::size_t>(heif_stride), max_value, frame.width, frame.height,
			                premultiplied, mapping ? &*mapping : nullptr, frame.format, pixels);
		}
		else
		{
			// An 8-bit HDR item: its bytes widened, row by row, to the signal the mapping reads or the caller keeps.
			std::vector<std::uint16_t> row(std::size_t{4} * frame.width);

			for (std::uint32_t y = 0; y < frame.height; ++y)
			{
				const auto* const source = plane + static_cast<std::size_t>(heif_stride) * y;
				for (std::size_t i = 0; i < row.size(); ++i) row[i] = static_cast<std::uint16_t>(source[i] * 257u);

				const pixel_buffer target{pixels.pixels + pixels.stride * y, pixels.stride};
				write_wide_rows(reinterpret_cast<const std::uint8_t*>(row.data()), row.size() * 2, 65535, frame.width,
				                1, premultiplied, mapping ? &*mapping : nullptr, frame.format, target);
			}
		}

		result.success = true;
	}
}
