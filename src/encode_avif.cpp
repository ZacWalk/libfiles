// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Encoding an AVIF with libheif and its libaom encoder, as libheif's own heif-enc drives it: the pixels put
// into the RGB order libheif reads, at eight bits a sample or ten or twelve from sixteen; coded as YCbCr 4:2:0, or
// losslessly as RGB itself at 4:4:4; with the ICC profile, or sRGB - or an HDR picture's description, its colour box's
// code points and light levels; the orientation written as the image's rotation and mirror, which a HEIF reader turns
// the picture by, and the EXIF's made to agree; the EXIF and XMP as items of their own; and a gain map as libavif
// writes one, which write_heif.cpp finishes.

#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "libfiles/hdr.h"
#include "encode_internal.h"
#include "hdr_internal.h"
#include "heif_internal.h"

#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace libfiles
{
	using namespace encode_detail;

	namespace
	{
		struct context_free
		{
			void operator()(heif_context* c) const noexcept { heif_context_free(c); }
		};

		struct encoder_release
		{
			void operator()(heif_encoder* e) const noexcept { heif_encoder_release(e); }
		};

		struct image_release
		{
			void operator()(heif_image* i) const noexcept { heif_image_release(i); }
		};

		struct handle_release
		{
			void operator()(heif_image_handle* h) const noexcept { heif_image_handle_release(h); }
		};

		struct options_free
		{
			void operator()(heif_encoding_options* o) const noexcept { heif_encoding_options_free(o); }
		};

		struct nclx_free
		{
			void operator()(heif_color_profile_nclx* n) const noexcept { heif_nclx_color_profile_free(n); }
		};

		using image_ptr = std::unique_ptr<heif_image, image_release>;
		using handle_ptr = std::unique_ptr<heif_image_handle, handle_release>;

		// As libjxl's encoder takes them: more workers mostly add each one's scratch memory, which a 32-bit process
		// cannot spare.
		constexpr int max_worker_threads = 8;

		// A gain map's quality: it is small beside the picture, and every step of its loss is a step in the HDR
		// rendition's brightness.
		constexpr int gain_map_quality = 90;

		constexpr std::uint16_t tag_orientation = 0x0112;

		bool is_wide(const pixel_format format)
		{
			return format == pixel_format::rgbx16 || format == pixel_format::rgba16;
		}

		// The picture as libheif takes it: interleaved red, green and blue, and alpha where there is any - bytes from
		// blue first, or the ten or twelve most significant bits of sixteen, little-endian, which give back exactly
		// what a decode of ten or twelve widened.
		heif_image* to_heif_image(const image_view& image, const bool alpha, const int bits, std::string& error)
		{
			const auto wide = is_wide(image.format);
			const auto chroma = wide ? (alpha ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBB_LE)
			                         : (alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB);
			const auto width = static_cast<int>(image.width);
			const auto height = static_cast<int>(image.height);

			heif_image* out = nullptr;
			if (heif_image_create(width, height, heif_colorspace_RGB, chroma, &out).code != heif_error_Ok ||
			    heif_image_add_plane(out, heif_channel_interleaved, width, height, wide ? bits : 8).code !=
			        heif_error_Ok)
			{
				if (out) heif_image_release(out);
				error = "libheif could not hold the image";
				return nullptr;
			}

			int stride = 0;
			auto* const plane = heif_image_get_plane(out, heif_channel_interleaved, &stride);
			const std::size_t channels = alpha ? 4 : 3;
			const auto shift = 16 - bits;

			for (std::uint32_t y = 0; y < image.height; ++y)
			{
				const auto* src = image.pixels + image.stride * y;
				auto* dst = plane + static_cast<std::size_t>(stride) * y;

				if (wide)
				{
					const auto* samples = reinterpret_cast<const std::uint16_t*>(src);

					for (std::uint32_t x = 0; x < image.width; ++x, samples += 4)
					{
						for (std::size_t c = 0; c < channels; ++c, dst += 2)
						{
							const auto v = static_cast<std::uint16_t>(samples[c] >> shift);
							dst[0] = static_cast<std::uint8_t>(v);
							dst[1] = static_cast<std::uint8_t>(v >> 8);
						}
					}
				}
				else
				{
					for (std::uint32_t x = 0; x < image.width; ++x, src += 4, dst += channels)
					{
						dst[0] = src[2];
						dst[1] = src[1];
						dst[2] = src[0];
						if (alpha) dst[3] = src[3];
					}
				}
			}

			return out;
		}

		// A gain map as libheif takes it: one grey plane where every pixel's channels agree, as Apple's and libfiles'
		// own maps are, else RGB.
		heif_image* gain_map_to_heif_image(const image_view& map, std::string& error)
		{
			auto grey = true;

			for (std::uint32_t y = 0; y < map.height && grey; ++y)
			{
				const auto* p = map.pixels + map.stride * y;
				for (std::uint32_t x = 0; x < map.width && grey; ++x, p += 4) grey = p[0] == p[1] && p[1] == p[2];
			}

			if (!grey) return to_heif_image(map, false, 8, error);

			heif_image* out = nullptr;
			const auto width = static_cast<int>(map.width);
			const auto height = static_cast<int>(map.height);

			if (heif_image_create(width, height, heif_colorspace_monochrome, heif_chroma_monochrome, &out).code !=
			        heif_error_Ok ||
			    heif_image_add_plane(out, heif_channel_Y, width, height, 8).code != heif_error_Ok)
			{
				if (out) heif_image_release(out);
				error = "libheif could not hold the gain map";
				return nullptr;
			}

			int stride = 0;
			auto* const plane = heif_image_get_plane(out, heif_channel_Y, &stride);

			for (std::uint32_t y = 0; y < map.height; ++y)
			{
				const auto* src = map.pixels + map.stride * y;
				auto* const dst = plane + static_cast<std::size_t>(stride) * y;
				for (std::uint32_t x = 0; x < map.width; ++x, src += 4) dst[x] = src[1];
			}

			return out;
		}

		// The TIFF stream libheif puts in its Exif item, without the signature a JPEG's APP1 begins with, its
		// orientation the one the image's rotation and mirror give, which a HEIF reader follows whatever the EXIF says.
		std::vector<std::uint8_t> exif_item(std::span<const std::uint8_t> exif, const std::uint16_t orientation)
		{
			if (is_exif_signature(exif)) exif = exif.subspan(exif_signature.size());

			std::vector<std::uint8_t> out(exif.begin(), exif.end());
			const exif_number number{exif_directory::primary, tag_orientation, orientation};
			set_exif_numbers(out, std::span(&number, 1));
			return out;
		}

		std::uint16_t chromaticity(const double v)
		{
			return static_cast<std::uint16_t>(std::clamp(std::lround(v / 0.00002), 0l, 50000l));
		}

		std::uint32_t luminance(const double nits)
		{
			return static_cast<std::uint32_t>(std::clamp(std::llround(nits / 0.0001), 0ll, 4294967295ll));
		}

		// An HDR picture's description, as the colour box, content light level and mastering display hold it: its
		// matrix BT.2020's for BT.2020 primaries, BT.709's for any other, identity for RGB coded losslessly.
		void describe_hdr(heif_image* picture, heif_color_profile_nclx& nclx, const hdr_description& hdr,
		                  const bool lossless)
		{
			nclx.color_primaries = static_cast<heif_color_primaries>(hdr.colour_primaries);
			nclx.transfer_characteristics = static_cast<heif_transfer_characteristics>(
				detail::code_point_of(hdr.transfer));
			nclx.matrix_coefficients = lossless ? heif_matrix_coefficients_RGB_GBR
			                           : hdr.colour_primaries == 9
			                               ? heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance
			                               : heif_matrix_coefficients_ITU_R_BT_709_5;
			nclx.full_range_flag = 1;
			heif_image_set_nclx_color_profile(picture, &nclx);

			if (hdr.max_cll > 0.0 || hdr.max_fall > 0.0)
			{
				heif_content_light_level level{};
				level.max_content_light_level = static_cast<std::uint16_t>(
					std::clamp(std::lround(hdr.max_cll), 0l, 65535l));
				level.max_pic_average_light_level = static_cast<std::uint16_t>(
					std::clamp(std::lround(hdr.max_fall), 0l, 65535l));
				heif_image_set_content_light_level(picture, &level);
			}

			if (hdr.mastering)
			{
				// Green, blue, red, as HEVC's SEI lists them.
				const auto& p = hdr.mastering->primaries;
				heif_mastering_display_colour_volume volume{};
				volume.display_primaries_x[0] = chromaticity(p.green_x);
				volume.display_primaries_y[0] = chromaticity(p.green_y);
				volume.display_primaries_x[1] = chromaticity(p.blue_x);
				volume.display_primaries_y[1] = chromaticity(p.blue_y);
				volume.display_primaries_x[2] = chromaticity(p.red_x);
				volume.display_primaries_y[2] = chromaticity(p.red_y);
				volume.white_point_x = chromaticity(p.white_x);
				volume.white_point_y = chromaticity(p.white_y);
				volume.max_display_mastering_luminance = luminance(hdr.mastering->max_luminance);
				volume.min_display_mastering_luminance = luminance(hdr.mastering->min_luminance);
				heif_image_set_mastering_display_colour_volume(picture, &volume);
			}
		}

		void append_be32(std::vector<std::uint8_t>& out, const std::uint32_t v)
		{
			for (auto shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
		}

		// The gain map, encoded as an image of its own, and the tone map item that applies it to the picture. The item
		// takes the picture's extent as stored, as libavif writes it, and no turn of its own: its inputs are turned.
		// What is added, or nothing when the gain map is not written, with a note.
		struct added_gain_map
		{
			heif_item_id tone_map = 0;
			heif_item_id image = 0;
		};

		std::optional<added_gain_map> add_gain_map(heif_context* const context, heif_encoder* const encoder,
		                                           heif_image_handle* const primary, const gain_map_image& gain_map,
		                                           const image_view& picture, const std::uint16_t turn,
		                                           std::vector<std::string>& notes)
		{
			const auto& map = gain_map.image;
			if (map.format != pixel_format::bgrx8 || !map.pixels || map.width == 0 || map.height == 0 ||
			    map.stride < std::size_t{4} * map.width || map.width > picture.width || map.height > picture.height)
			{
				notes.emplace_back("the gain map is not bgrx8 within the picture's size, and is left out");
				return {};
			}

			auto payload = write_gain_map_metadata(gain_map.metadata);
			if (payload.empty())
			{
				notes.emplace_back("the gain map's metadata cannot be written, and the gain map is left out");
				return {};
			}
			payload.insert(payload.begin(), std::uint8_t{0});

			std::string error;
			const image_ptr image(gain_map_to_heif_image(map, error));
			if (!image)
			{
				notes.push_back(error);
				return {};
			}

			if (heif_encoder_set_lossless(encoder, 0).code != heif_error_Ok ||
			    heif_encoder_set_lossy_quality(encoder, gain_map_quality).code != heif_error_Ok ||
			    heif_encoder_set_parameter_string(encoder, "chroma", "444").code != heif_error_Ok)
			{
				notes.emplace_back("libheif's AV1 encoder refused the gain map's settings, and it is left out");
				return {};
			}

			const std::unique_ptr<heif_encoding_options, options_free> encoding(heif_encoding_options_alloc());
			encoding->image_orientation = static_cast<heif_orientation>(turn);

			heif_image_handle* raw = nullptr;
			const auto encoded = heif_context_encode_image(context, image.get(), encoder, encoding.get(), &raw);
			const handle_ptr handle(raw);

			if (encoded.code != heif_error_Ok)
			{
				notes.push_back(std::format("libheif could not encode the gain map, which is left out: {}",
				                            encoded.message ? encoded.message : ""));
				return {};
			}

			added_gain_map added;
			added.image = heif_image_handle_get_item_id(handle.get());
			const auto base = heif_image_handle_get_item_id(primary);
			const std::array inputs{base, added.image};

			std::vector<std::uint8_t> ispe(4, 0);
			append_be32(ispe, picture.width);
			append_be32(ispe, picture.height);

			if (heif_context_add_item(context, "tmap", payload.data(), static_cast<int>(payload.size()),
			                          &added.tone_map)
			            .code != heif_error_Ok ||
			    heif_context_add_item_references(context, heif_fourcc('d', 'i', 'm', 'g'), added.tone_map,
			                                     inputs.data(), static_cast<int>(inputs.size()))
			            .code != heif_error_Ok ||
			    heif_item_add_raw_property(context, added.tone_map, heif_fourcc('i', 's', 'p', 'e'), nullptr,
			                               ispe.data(), ispe.size(), 0, nullptr)
			            .code != heif_error_Ok ||
			    heif_context_set_primary_image(context, primary).code != heif_error_Ok)
			{
				notes.emplace_back("libheif refused the gain map's tone map item");
				return {};
			}

			heif_context_add_compatible_brand(context, heif_fourcc_to_brand("tmap"));
			return added;
		}
	}

	encoded encode_avif(const image_view& image, const std::uint16_t orientation, const embedded_metadata& metadata,
	                    const avif_options& options)
	{
		encoded result;

		if (image.format != pixel_format::bgrx8 && image.format != pixel_format::bgra8 && !is_wide(image.format))
		{
			result.notes.emplace_back("libfiles encodes AVIF from bgrx8, bgra8, rgbx16 or rgba16 pixels only");
			return result;
		}

		const auto pixel_bytes = is_wide(image.format) ? std::size_t{8} : std::size_t{4};
		constexpr auto max_side = static_cast<std::uint32_t>(std::numeric_limits<int>::max());

		if (!image.pixels || image.width == 0 || image.height == 0 || image.width > max_side ||
		    image.height > max_side || image.stride < pixel_bytes * image.width)
		{
			result.notes.emplace_back("the image has no pixels to encode");
			return result;
		}

		const auto alpha = image.format == pixel_format::bgra8 || image.format == pixel_format::rgba16;
		const auto turn = orientation >= 1 && orientation <= 8 ? orientation : std::uint16_t{1};
		const auto bits = options.bit_depth == 12 ? 12 : 10;
		const auto with_gain_map = options.gain_map.has_value() && !metadata.hdr;
		if (options.gain_map && metadata.hdr)
			result.notes.emplace_back("an HDR picture's signal is written without the gain map given with it");

		try
		{
			const std::unique_ptr<heif_context, context_free> context(heif_context_alloc());
			heif_encoder* raw_encoder = nullptr;

			if (!context ||
			    heif_context_get_encoder_for_format(context.get(), heif_compression_AV1, &raw_encoder).code !=
			        heif_error_Ok)
			{
				result.notes.emplace_back("libheif has no AV1 encoder");
				return result;
			}

			const std::unique_ptr<heif_encoder, encoder_release> encoder(raw_encoder);
			const auto threads = static_cast<int>(
				std::clamp<unsigned>(std::thread::hardware_concurrency(), 1u, unsigned{max_worker_threads}));
			const auto configured =
				heif_encoder_set_parameter_integer(encoder.get(), "speed", std::clamp(options.speed, 0, 9)).code ==
					heif_error_Ok &&
				heif_encoder_set_parameter_integer(encoder.get(), "threads", threads).code == heif_error_Ok &&
				(options.lossless
			         ? heif_encoder_set_lossless(encoder.get(), 1).code == heif_error_Ok &&
			               heif_encoder_set_parameter_string(encoder.get(), "chroma", "444").code == heif_error_Ok
			         : heif_encoder_set_lossy_quality(encoder.get(), std::clamp(options.quality, 0, 100)).code ==
			               heif_error_Ok);

			if (!configured)
			{
				result.notes.emplace_back("libheif's AV1 encoder refused its settings");
				return result;
			}

			std::string error;
			const image_ptr picture(to_heif_image(image, alpha, bits, error));

			if (!picture)
			{
				result.notes.push_back(error);
				return result;
			}

			const std::unique_ptr<heif_encoding_options, options_free> encoding(heif_encoding_options_alloc());
			encoding->image_orientation = static_cast<heif_orientation>(turn);
			const std::unique_ptr<heif_color_profile_nclx, nclx_free> nclx(heif_nclx_color_profile_alloc());

			if (metadata.hdr)
			{
				// The description says what the pixels are, in place of a profile.
				if (!nclx)
				{
					result.notes.emplace_back("libheif could not describe the HDR picture");
					return result;
				}

				describe_hdr(picture.get(), *nclx, *metadata.hdr, options.lossless);
				encoding->output_nclx_profile = nclx.get();
			}
			else
			{
				if (!metadata.icc.empty() &&
				    heif_image_set_raw_color_profile(picture.get(), "prof", metadata.icc.data(), metadata.icc.size())
				            .code != heif_error_Ok)
				{
					result.notes.emplace_back("libheif refused the ICC profile, so the image is written as sRGB");
				}

				// Lossless is RGB itself: an identity matrix, which turns no sample into another.
				if (options.lossless && nclx)
				{
					nclx->color_primaries = heif_color_primaries_ITU_R_BT_709_5;
					nclx->transfer_characteristics = heif_transfer_characteristic_IEC_61966_2_1;
					nclx->matrix_coefficients = heif_matrix_coefficients_RGB_GBR;
					nclx->full_range_flag = 1;
					encoding->output_nclx_profile = nclx.get();
				}
			}

			heif_image_handle* raw_handle = nullptr;
			const auto encoded_image = heif_context_encode_image(context.get(), picture.get(), encoder.get(),
			                                                     encoding.get(), &raw_handle);
			const handle_ptr handle(raw_handle);

			if (encoded_image.code != heif_error_Ok)
			{
				result.notes.push_back(std::format("libheif could not encode the image: {}",
				                                   encoded_image.message ? encoded_image.message : ""));
				return result;
			}

			if (!metadata.exif.empty())
			{
				const auto exif = exif_item(metadata.exif, turn);

				if (exif.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
				    heif_context_add_exif_metadata(context.get(), handle.get(), exif.data(),
				                                   static_cast<int>(exif.size()))
				            .code != heif_error_Ok)
				{
					result.notes.emplace_back("libheif refused the EXIF, which is left out");
				}
			}

			if (!metadata.xmp.empty() &&
			    (metadata.xmp.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
			     heif_context_add_XMP_metadata(context.get(), handle.get(), metadata.xmp.data(),
			                                   static_cast<int>(metadata.xmp.size()))
			             .code != heif_error_Ok))
			{
				result.notes.emplace_back("libheif refused the XMP, which is left out");
			}

			const auto gain_map = with_gain_map ? add_gain_map(context.get(), encoder.get(), handle.get(),
			                                                   *options.gain_map, image, turn, result.notes)
			                                    : std::nullopt;

			heif_writer writer{};
			writer.writer_api_version = 1;
			writer.write = [](heif_context*, const void* data, const size_t size, void* userdata)
			{
				auto* const out = static_cast<std::vector<std::uint8_t>*>(userdata);
				const auto* const bytes = static_cast<const std::uint8_t*>(data);
				out->insert(out->end(), bytes, bytes + size);
				return heif_error{heif_error_Ok, heif_suberror_Unspecified, ""};
			};

			if (heif_context_write(context.get(), &writer, &result.data).code != heif_error_Ok)
			{
				result.data.clear();
				result.notes.emplace_back("libheif could not write the AVIF");
			}
			else if (gain_map)
			{
				result.data = detail::heif::finish_gain_map(result.data, gain_map->tone_map, gain_map->image);
			}
		}
		catch (const std::exception& e)
		{
			// Out of memory, most likely, for the picture or the output; or a file libheif wrote that the gain map
			// could not be finished in.
			result.data.clear();
			result.notes.push_back(std::format("the AVIF could not be encoded: {}", e.what()));
		}

		return result;
	}
}
