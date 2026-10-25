// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Camera raw files, with LibRaw and Adobe's DNG SDK: their largest decodable embedded preview, or
// their raw data developed into the caller's memory, and their scan - the metadata LibRaw parses, and a
// thumbnail. Moved from Diffractor's loader and scan unchanged: the preview chosen and turned as it chose
// and turned them, and the development's auto-brightness, which replicates dcraw_make_mem_image's while
// writing straight into the caller's rows.

#include "libfiles/raw.h"

#include "libfiles/reader.h"
#include "libfiles/scan.h"
#include "raw_internal.h"

#include "dng_tag_values.h"

// LibRaw hands a DNG 1.7 JPEG XL image (TIFF compression 52546) to the SDK only while qDNGSupportJXL is
// visible and the SDK is 1.7 or newer - see valid_for_dngsdk in LibRaw's dngsdk_glue.cpp. In SDK 1.7 the
// macro survives only as a deprecated flag's sentinel, so an SDK upgrade could remove it and silently
// turn every JPEG XL DNG into a failure to open, with no other symptom.
#ifndef qDNGSupportJXL
#error "qDNGSupportJXL is not visible - LibRaw will reject JPEG XL DNG files"
#endif
static_assert(dngVersion_Current >= dngVersion_1_7_0_0, "the DNG SDK predates JPEG XL DNG support");

#include <algorithm>
#include <bit>
#include <cmath>
#include <ctime>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace libfiles
{
	std::uint16_t detail::orientation_of_flip(const int flip)
	{
		constexpr std::uint16_t orientations[] = {1, 2, 4, 3, 5, 8, 6, 7};
		if (flip >= 0 && flip < static_cast<int>(std::size(orientations))) return orientations[flip];
		return 1;
	}

	namespace
	{
		// LibRaw 0.22 moved gamma_curve() and libraw_internal_data to a protected section. This re-exposes
		// them, so that development can replicate dcraw_make_mem_image's auto-brightness while reading
		// imgdata.image directly, without a second full-frame copy.
		class libraw_ex final : public LibRaw
		{
		public:
			using LibRaw::gamma_curve;
			using LibRaw::libraw_internal_data;
		};

		struct raw_processor
		{
			// Declared first so the host outlives the LibRaw instance: ~LibRaw runs recycle(), which
			// releases the dng_negative and dng_image it allocated through this host.
			std::unique_ptr<dng_host> dng;
			std::unique_ptr<libraw_ex> processor;
		};

		raw_processor create_decode_processor()
		{
			raw_processor result;
			result.processor = std::make_unique<libraw_ex>();

			// Installing the host is what activates the SDK - LibRaw already defaults rawparams.use_dngsdk
			// to LIBRAW_DNG_DEFAULT (float, linear, deflate and 8-bit DNG), and unpack() also routes DNG over
			// 2 GB, lossy DNG and JPEG XL DNG through it. Everything else, ordinary lossless-JPEG Bayer DNG
			// among it, stays on LibRaw's faster decoder.
			result.dng = std::make_unique<dng_host>();
			result.processor->set_dng_host(result.dng.get());

			// The camera's white balance, without which the image has a strong colour cast: LibRaw
			// defaults to none.
			result.processor->imgdata.params.use_camera_wb = 1;
			return result;
		}

		// df::round's rounding: of a float, 0 unless it is normal.
		int round_float(const float d)
		{
			if (!std::isnormal(d)) return 0;
			return static_cast<int>(std::round(d));
		}

		using detail::orientation_of_flip;

		bool flips_xy(const std::uint16_t orientation)
		{
			return orientation >= 5 && orientation <= 8;
		}

		// The preview's orientation, by raw_preview_orientation, against the raw data LibRaw has read.
		std::uint16_t calc_orientation(const std::uint32_t preview_width, const std::uint32_t preview_height,
		                               const std::uint16_t preview_orientation, const libraw_data_t& image_data)
		{
			return raw_preview_orientation(preview_width, preview_height, preview_orientation, image_data.sizes.width,
			                               image_data.sizes.height, orientation_of_flip(image_data.sizes.flip));
		}

		// The index of the largest embedded JPEG in LibRaw's thumbnail list, or -1. thumbs_list entries
		// carry LIBRAW_INTERNAL_THUMBNAIL_* codes, an enum apart from imgdata.thumbnail's
		// LIBRAW_THUMBNAIL_* values.
		int find_largest_jpeg_thumb(const libraw_thumbnail_list_t& list)
		{
			int best = -1;
			std::uint32_t best_area = 0;

			const auto count = std::min(static_cast<int>(list.thumbcount), static_cast<int>(std::size(list.thumblist)));

			for (auto i = 0; i < count; ++i)
			{
				const auto& item = list.thumblist[i];

				if (item.tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG && item.tlength > 0)
				{
					const auto area = static_cast<std::uint32_t>(item.twidth) * static_cast<std::uint32_t>(item.theight);

					if (best < 0 || area > best_area)
					{
						best = i;
						best_area = area;
					}
				}
			}

			return best;
		}

		// Unpacks the embedded thumbnail, preferring a format a decoder reads. The default, largest, may
		// be one it does not - a Canon CR3's H.265 preview, a CISZ-wrapped HEVC stream LibRaw flags but
		// does not decode - and then the largest embedded JPEG stands in. False only when no thumbnail
		// unpacks.
		bool unpack_decodable_thumb(LibRaw& processor)
		{
			if (processor.unpack_thumb() != LIBRAW_SUCCESS) return false;

			const auto format = processor.imgdata.thumbnail.tformat;

			if (format != LIBRAW_THUMBNAIL_JPEG && format != LIBRAW_THUMBNAIL_BITMAP)
			{
				const auto jpeg_index = find_largest_jpeg_thumb(processor.imgdata.thumbs_list);

				// unpack_thumb_ex overwrites the thumbnail's descriptor before it decodes, so on failure
				// tlength describes the JPEG entry while thumb still holds the old buffer: report that rather
				// than leave the caller to read the mismatched pair.
				return jpeg_index >= 0 && processor.unpack_thumb_ex(jpeg_index) == LIBRAW_SUCCESS;
			}

			return true;
		}

		// A JPEG preview, kept as it is when it scans as an image of some size, as a caller that decodes it
		// later would find it.
		bool take_jpeg_preview(const libraw_thumbnail_t& thumbnail, const libraw_data_t& image_data,
		                       raw_load_result& result)
		{
			const std::span<const std::uint8_t> bytes(std::bit_cast<const std::uint8_t*>(thumbnail.thumb),
			                                          thumbnail.tlength);
			if (bytes.size() <= 16) return false;

			reader r(bytes);
			const auto scanned = scan(r);
			if (scanned.format == image_format::unknown || scanned.width == 0 || scanned.height == 0) return false;

			// Taken by its low byte, as Diffractor, which holds an orientation in a byte, took one: only a
			// damaged EXIF states one past 255, and the choice below is then made on what the caller shows.
			const auto stated = static_cast<std::uint8_t>(scanned.orientation.value_or(1));

			result.jpeg.assign(bytes.begin(), bytes.end());
			result.orientation = calc_orientation(scanned.width, scanned.height, stated, image_data);
			return true;
		}

		// A bitmap thumbnail - grey, RGB or RGBA - written to the sink as B8G8R8X8, the fourth byte 0 or the
		// alpha, turned as raw_preview_orientation says. Any other channel count, no size, or a buffer too
		// short for its size, is not written.
		bool write_bitmap(const libraw_thumbnail_t& thumbnail, const libraw_data_t& image_data, image_sink& sink,
		                  decode_result& decoded)
		{
			const auto colors = thumbnail.tcolors;
			if (colors != 1 && colors != 3 && colors != 4) return false;
			if (thumbnail.twidth == 0 || thumbnail.theight == 0) return false;

			// 64-bit, so the product of two file-supplied 16-bit dimensions cannot wrap past the check.
			const auto expected_size = static_cast<std::uint64_t>(thumbnail.theight) * thumbnail.twidth * colors;
			if (expected_size > thumbnail.tlength) return false;

			frame_info frame;
			frame.source_width = thumbnail.twidth;
			frame.source_height = thumbnail.theight;
			frame.width = thumbnail.twidth;
			frame.height = thumbnail.theight;
			frame.format = pixel_format::bgrx8;
			frame.orientation = calc_orientation(thumbnail.twidth, thumbnail.theight, 1, image_data);

			const auto pixels = sink.allocate(frame);
			decoded.frame = frame;

			if (!pixels.pixels)
			{
				decoded.refused = true;
				return false;
			}

			for (auto y = 0; y < thumbnail.theight; ++y)
			{
				const auto* src = thumbnail.thumb + static_cast<std::size_t>(y) * thumbnail.twidth * colors;
				auto* dst = pixels.pixels + pixels.stride * static_cast<std::size_t>(y);

				for (auto x = 0; x < thumbnail.twidth; ++x)
				{
					if (colors == 1)
					{
						const auto g = static_cast<std::uint8_t>(src[0]);
						*dst++ = g;
						*dst++ = g;
						*dst++ = g;
						*dst++ = 0;
						src += 1;
					}
					else if (colors == 3)
					{
						*dst++ = static_cast<std::uint8_t>(src[2]);
						*dst++ = static_cast<std::uint8_t>(src[1]);
						*dst++ = static_cast<std::uint8_t>(src[0]);
						*dst++ = 0;
						src += 3;
					}
					else
					{
						*dst++ = static_cast<std::uint8_t>(src[2]);
						*dst++ = static_cast<std::uint8_t>(src[1]);
						*dst++ = static_cast<std::uint8_t>(src[0]);
						*dst++ = static_cast<std::uint8_t>(src[3]);
						src += 4;
					}
				}
			}

			decoded.success = true;
			return true;
		}

		// A bitmap preview, as write_bitmap writes it.
		bool take_bitmap_preview(const libraw_thumbnail_t& thumbnail, const libraw_data_t& image_data,
		                         image_sink& sink, raw_load_result& result)
		{
			if (!write_bitmap(thumbnail, image_data, sink, result.decoded)) return false;
			result.orientation = result.decoded.frame.orientation;
			return true;
		}

		// The raw data developed into the sink, the declared size accepted first: unpack, dcraw_process,
		// and dcraw_make_mem_image's auto-brightness replicated over imgdata.image as it is written.
		void develop(libraw_ex& processor, image_sink& sink, raw_load_result& result)
		{
			const auto& image_data = processor.imgdata;

			// Every other decoder asks before it allocates. LibRaw builds the whole frame inside unpack and
			// dcraw_process, so the question is put here, after the header is read and before a pixel is
			// committed.
			frame_info declared;
			declared.source_width = static_cast<std::uint32_t>(image_data.sizes.width);
			declared.source_height = static_cast<std::uint32_t>(image_data.sizes.height);
			declared.width = declared.source_width;
			declared.height = declared.source_height;
			declared.format = pixel_format::bgrx8;
			declared.orientation = orientation_of_flip(image_data.sizes.flip);
			result.decoded.frame = declared;

			if (!sink.accept(declared))
			{
				result.decoded.refused = true;
				return;
			}

			if (processor.unpack() != LIBRAW_SUCCESS || processor.dcraw_process() != LIBRAW_SUCCESS)
			{
				result.decoded.error = "LibRaw could not develop the raw data";
				return;
			}

			const auto& internal = processor.libraw_internal_data;
			const auto& S = image_data.sizes;
			const auto& IO = internal.internal_output_params;
			const auto& P1 = image_data.idata;
			const auto& O = image_data.params;
			const auto colors = P1.colors;

			if (colors != 1 && colors < 3)
			{
				result.decoded.error = "the raw data has a number of colours that is not developed";
				return;
			}

			// After dcraw_process, imgdata.image holds exactly sizes.height rows of sizes.width pixels;
			// a null, empty or shrunk image - half_size, aber or threshold set - would read out of bounds.
			if (!image_data.image || S.width <= 0 || S.height <= 0 || S.iwidth != S.width || S.iheight != S.height)
			{
				result.decoded.error = "LibRaw developed no image";
				return;
			}

			// dcraw_make_mem_image's auto-brightness (LibRaw's src/postprocessing/mem_image.cpp), which reaches
			// into libraw_internal_data: kept in step with it on a LibRaw upgrade.
			if (internal.output_data.histogram)
			{
				int val, total, t_white = 0x2000, c;
				// The 99th percentile white level.
				auto perc = round_float(static_cast<float>(static_cast<double>(S.width) * static_cast<double>(S.height) *
					O.auto_bright_thr));

				if (IO.fuji_width) perc /= 2;

				if (!(O.highlight & ~2 || O.no_auto_bright))
				{
					for (t_white = c = 0; c < P1.colors; c++)
					{
						for (val = 0x2000, total = 0; --val > 32;)
						{
							if ((total += internal.output_data.histogram[c][val]) > perc) break;
						}

						if (t_white < val) t_white = val;
					}
				}

				processor.gamma_curve(O.gamm[0], O.gamm[1], 2, round_float((t_white << 3) / O.bright));
			}

			frame_info frame = declared;
			frame.width = static_cast<std::uint32_t>(S.width);
			frame.height = static_cast<std::uint32_t>(S.height);
			frame.orientation = orientation_of_flip(S.flip);

			const auto pixels = sink.allocate(frame);
			result.decoded.frame = frame;

			if (!pixels.pixels)
			{
				result.decoded.refused = true;
				return;
			}

			const auto& color_curve = image_data.color.curve;
			std::size_t i = 0;

			for (auto y = 0; y < S.height; y++)
			{
				auto* row = pixels.pixels + pixels.stride * static_cast<std::size_t>(y);

				for (auto x = 0; x < S.width; x++, row += 4)
				{
					const auto* const id = image_data.image[i++];
					std::uint32_t value;

					if (colors == 1)
					{
						const auto grey = static_cast<std::uint32_t>(color_curve[id[0]] >> 8);
						value = grey | grey << 8 | grey << 16;
					}
					else
					{
						value = (color_curve[id[2]] >> 8) | (0xFF00u & color_curve[id[1]]) |
							(0xFF0000u & (static_cast<std::uint32_t>(color_curve[id[0]]) << 8));
					}

					row[0] = static_cast<std::uint8_t>(value);
					row[1] = static_cast<std::uint8_t>(value >> 8);
					row[2] = static_cast<std::uint8_t>(value >> 16);
					row[3] = static_cast<std::uint8_t>(value >> 24);
				}
			}

			result.decoded.success = true;
			result.orientation = frame.orientation;
		}

		// Borrows the calling thread's LibRaw instance for one scan. Constructing LibRaw allocates and
		// zero-fills some half a megabyte a scan never reads - libraw_data_t carries color.curve[0x10000],
		// LibRaw_TLS ahd_data.cbrt[0x10000] - which a scan of each file paid twice, in the constructor and
		// in the destructor's recycle(). recycle() restores the state after construction, so one instance
		// kept for each scanning thread is the same, and pays it once; the cost is that instance, kept. A
		// scan reads nothing that scans, so leases never nest.
		class scan_lease
		{
		public:
			scan_lease() : _processor(thread_processor())
			{
			}

			~scan_lease()
			{
				// recycle() also closes the file, so it is not left open past the lease.
				_processor.recycle();
			}

			scan_lease(const scan_lease&) = delete;
			scan_lease& operator=(const scan_lease&) = delete;

			LibRaw* operator->() const { return &_processor; }
			LibRaw& operator*() const { return _processor; }

		private:
			static LibRaw& thread_processor()
			{
				static thread_local LibRaw processor;
				return processor;
			}

			LibRaw& _processor;
		};

		std::string trimmed(const char* const text)
		{
			constexpr std::string_view whitespace{" \n\r\t\f\v\0", 7};
			const std::string_view s(text);
			const auto start = s.find_first_not_of(whitespace);
			if (start == std::string_view::npos) return {};
			return std::string(s.substr(start, s.find_last_not_of(whitespace) - start + 1));
		}

		// What an index takes from LibRaw's parse.
		void read_index(const libraw_data_t& data, raw_scan_result& result)
		{
			result.width = data.sizes.width;
			result.height = data.sizes.height;
			result.orientation = orientation_of_flip(data.sizes.flip);

			if (data.idata.xmpdata && data.idata.xmplen > 0)
			{
				const auto* const xmp = std::bit_cast<const std::uint8_t*>(data.idata.xmpdata);
				result.xmp.assign(xmp, xmp + data.idata.xmplen);
			}

			// LibRaw makes its timestamp by passing the EXIF DateTimeOriginal - a time of day with no zone -
			// through mktime(), which takes it as the process's local time. Taken back the same way, the
			// date is the camera's, whatever the zone the scan runs in.
			if (data.other.timestamp != 0)
			{
				std::tm local{};
#ifdef _WIN32
				const auto taken = localtime_s(&local, &data.other.timestamp) == 0;
#else
				const auto taken = localtime_r(&data.other.timestamp, &local) != nullptr;
#endif
				if (taken)
				{
					result.created = raw_date{
						local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
						local.tm_sec
					};
				}
			}

			if (data.idata.cdesc[0]) result.pixel_format = trimmed(data.idata.cdesc);
			if (data.idata.make[0]) result.camera_make = trimmed(data.idata.make);
			if (data.idata.model[0]) result.camera_model = trimmed(data.idata.model);
			if (data.other.desc[0]) result.comment = trimmed(data.other.desc);
			if (data.other.artist[0]) result.artist = trimmed(data.other.artist);

			result.iso_speed = round_float(data.other.iso_speed);
			result.exposure_time = data.other.shutter;
			result.f_number = data.other.aperture;
			result.focal_length = data.other.focal_len;

			result.gps = detail::raw_gps_position(data.other.parsed_gps);
		}

		// The largest thumbnail a decoder reads: a JPEG one's bytes, when the raw image has a size, and a
		// bitmap one written to the sink; and the raw image a JPEG one is turned against once decoded.
		void read_thumbnail(LibRaw& processor, image_sink& sink, raw_scan_result& result)
		{
			if (!unpack_decodable_thumb(processor)) return;

			const auto& image_data = processor.imgdata;
			const auto& thumbnail = image_data.thumbnail;
			result.image_width = image_data.sizes.width;
			result.image_height = image_data.sizes.height;
			result.image_orientation = orientation_of_flip(image_data.sizes.flip);

			if (LIBRAW_THUMBNAIL_JPEG == thumbnail.tformat && thumbnail.tlength > 0 && image_data.sizes.width > 0 &&
				image_data.sizes.height > 0)
			{
				const auto* const bytes = std::bit_cast<const std::uint8_t*>(thumbnail.thumb);
				result.thumbnail_jpeg.assign(bytes, bytes + thumbnail.tlength);
			}
			else if (LIBRAW_THUMBNAIL_BITMAP == thumbnail.tformat && thumbnail.tlength > 0)
			{
				write_bitmap(thumbnail, image_data, sink, result.thumbnail);
			}
		}
	}

	raw_scan_result scan_raw(const std::filesystem::path& path, const scan_intent intent, image_sink* const thumbnail_sink)
	{
		raw_scan_result result;

		try
		{
			const scan_lease processor;

			if (processor->open_file(path.c_str()) == LIBRAW_SUCCESS)
			{
				if (processor->adjust_sizes_info_only() == LIBRAW_SUCCESS)
				{
					read_index(processor->imgdata, result);
					if (intent == scan_intent::inspect) result.listing = detail::list_raw(processor->imgdata);
					result.success = true;
				}

				if (thumbnail_sink) read_thumbnail(*processor, *thumbnail_sink, result);
			}
		}
		catch (const std::bad_alloc&)
		{
			result = {};
		}

		return result;
	}

	raw_load_result load_raw(const std::filesystem::path& path, image_sink& sink, const bool prefer_preview)
	{
		raw_load_result result;

		try
		{
			const auto rp = create_decode_processor();
			if (rp.processor->open_file(path.c_str()) != LIBRAW_SUCCESS)
			{
				result.decoded.error = "LibRaw could not open the file";
				return result;
			}

			const auto& image_data = rp.processor->imgdata;

			// The preview is often large enough, and costs a fraction of a development.
			if (prefer_preview && unpack_decodable_thumb(*rp.processor))
			{
				const auto& thumbnail = image_data.thumbnail;

				if (thumbnail.tlength > 0)
				{
					if (LIBRAW_THUMBNAIL_JPEG == thumbnail.tformat)
						result.preview = take_jpeg_preview(thumbnail, image_data, result);
					else if (LIBRAW_THUMBNAIL_BITMAP == thumbnail.tformat)
						result.preview = take_bitmap_preview(thumbnail, image_data, sink, result);
				}

				if (result.preview)
				{
					result.success = true;
					return result;
				}

				result.decoded = {};
			}

			develop(*rp.processor, sink, result);
			result.success = result.decoded.success;
		}
		catch (const std::bad_alloc&)
		{
			result = {};
			result.decoded.error = "out of memory";
		}

		return result;
	}

	std::uint16_t raw_preview_orientation(const std::uint32_t preview_width, const std::uint32_t preview_height,
	                                      const std::uint16_t preview_orientation, const std::uint32_t image_width,
	                                      const std::uint32_t image_height, const std::uint16_t image_orientation)
	{
		if (image_width == image_height) return preview_orientation;

		// Width to height in tenths, as shown.
		const auto shape = [](std::uint64_t cx, std::uint64_t cy, const std::uint16_t orientation) -> std::uint64_t
		{
			if (flips_xy(orientation)) std::swap(cx, cy);
			return cx == 0 || cy == 0 ? 0 : cx * 10 / cy;
		};

		return shape(image_width, image_height, image_orientation) !=
		       shape(preview_width, preview_height, preview_orientation)
			       ? image_orientation
			       : preview_orientation;
	}
}
