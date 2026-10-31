// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The TIFF scanner. A TIFF file is a TIFF stream as an EXIF block is, and is read with the
// same directory reader: IFD0 for the primary image's dimensions and XMP, and - by the rules
// read_exif and summarize_exif keep - the camera, exposure, dates, orientation, thumbnail and GPS.

#include "scan_internal.h"
#include "tiff_internal.h"

#include <optional>

namespace libfiles::detail
{
	namespace
	{
		// The only two TIFF byte-order marks. Each reads the same either way round.
		constexpr std::uint16_t order_intel = 0x4949;
		constexpr std::uint16_t order_motorola = 0x4D4D;

		// An XMP packet's length is an unvalidated 32-bit field; this caps what a scan may be asked to read.
		constexpr std::uint32_t max_tiff_xmp_bytes = 8u * 1024u * 1024u;
		// IPTC is capped as a JPEG's or a PSD's is.
		constexpr std::uint32_t max_tiff_iptc_bytes = 16u * 1024u * 1024u;

		// What IFD0 says of its picture's samples. Unstated, a sample has one bit and YCbCr is sampled 2 by 2.
		struct sample_tags
		{
			std::optional<std::uint16_t> photometric;
			std::uint16_t bits = 1;
			std::uint16_t sampling_h = 2;
			std::uint16_t sampling_v = 2;
			bool alpha = false;
		};

		// YCbCrSubSampling as FFmpeg's TIFF decoder names it: 4:1:0 is a quarter of the chroma each way, and a
		// quarter one way by a half the other has no name.
		chroma_format ycbcr_chroma(const std::uint16_t h, const std::uint16_t v)
		{
			switch (h << 4 | v)
			{
			case 0x11: return chroma_format::yuv444;
			case 0x21: return chroma_format::yuv422;
			case 0x22: return chroma_format::yuv420;
			case 0x12: return chroma_format::yuv440;
			case 0x41: return chroma_format::yuv411;
			case 0x44: return chroma_format::yuv410;
			default: return chroma_format::unknown;
			}
		}

		// The picture a photometric interpretation names. Masks, a sensor's filter array and LogLuv name none.
		picture_format tiff_picture(const sample_tags& t)
		{
			picture_format p;
			if (!t.photometric) return p;

			switch (*t.photometric)
			{
			case 0: // white is zero
			case 1: p.chroma = chroma_format::gray;
				break;
			case 2: p.chroma = chroma_format::rgb;
				break;
			case 3: p.chroma = chroma_format::rgb;
				p.palette = true;
				break;
			case 5: p.chroma = chroma_format::cmyk;
				break;
			case 6: p.chroma = ycbcr_chroma(t.sampling_h, t.sampling_v);
				break;
			case 8: // CIE, ICC and ITU L*a*b*
			case 9:
			case 10: p.chroma = chroma_format::lab;
				break;
			default: return p;
			}

			if (t.bits <= 255) p.bit_depth = static_cast<std::uint8_t>(t.bits);
			p.alpha = t.alpha;
			return p;
		}
	}

	scan_result scan_tiff(reader& r)
	{
		scan_result result;

		const auto order = load_le16(r.view(0, 8).data());
		const auto size = r.size();

		if (size < 12u || (order != order_intel && order != order_motorola)) return result;

		const tiff_bytes data(r, order == order_intel);
		sample_tags samples;

		// Only classic TIFF; a BigTIFF reads as unscanned.
		if (const auto ifd0 = first_directory(data))
		{
			for_each_entry(data, *ifd0, directory_kind::exif, [&](const entry& e)
			{
				// The dimensions are SHORT or LONG. Reading a big-endian LONG as a SHORT picks up the high
				// half, which is 0 for any realistic size.
				const auto dimension = [&e]() -> std::uint32_t
				{
					switch (e.type)
					{
					case type_short: return e.field_u16();
					case type_long: return e.field_u32();
					default: return 0u;
					}
				};

				switch (e.tag)
				{
				case tag_image_width:
					result.width = dimension();
					break;
				case tag_image_length:
					result.height = dimension();
					break;
				case tag_bits_per_sample:
					samples.bits = e.first_u16();
					break;
				case tag_photometric:
					samples.photometric = e.first_u16();
					break;
				case tag_extra_samples:
					{
						// 1 and 2 are alpha, associated with the colour or not; 0 is a sample of no stated use.
						const auto use = e.first_u16();
						samples.alpha = use == 1 || use == 2;
					}
					break;
				case tag_ycbcr_subsampling:
					if (e.type == type_short && e.count >= 2)
					{
						samples.sampling_h = static_cast<std::uint16_t>(e.s16_at(0));
						samples.sampling_v = static_cast<std::uint16_t>(e.s16_at(1));
					}
					break;
				case tag_xmp:
					// The packet's bytes, however its type is stated. Four or fewer are stored in the entry
					// itself; only more put an offset there.
					if (e.count <= max_tiff_xmp_bytes)
					{
						const std::uint64_t at = e.count <= 4u ? e.offset + 8u : e.field_u32();
						if (r.contains(at, e.count)) result.metadata.xmp = r.read(at, e.count);
					}
					break;
				case tag_iptc:
					// The block the writer keeps here, as Photoshop does: bytes, or the LONGs Photoshop counts it in.
					if (e.size() != 0 && e.size() <= max_tiff_iptc_bytes)
					{
						const std::uint64_t at = e.size() <= 4u ? e.offset + 8u : e.field_u32();
						if (r.contains(at, e.size())) result.metadata.iptc = r.read(at, e.size());
					}
					break;
				default:
					break;
				}
			});

			const auto summary = summarize(data);
			result.orientation = summary.orientation;
			result.tiff_thumbnail = summary.thumbnail;
			result.tiff_metadata = read_metadata(data, 64u * 1024u);
			const auto& metadata = *result.tiff_metadata;
			if (metadata.latitude && metadata.longitude)
			{
				result.gps = gps_position{*metadata.latitude, *metadata.longitude};
			}
		}

		// A recognised byte order with an unusable magic, truncated or corrupt, must not read as a
		// successful 0x0 image. The dimensions are unvalidated 32-bit fields, so a nonsense one is
		// refused here rather than reaching a decoder's budget as a negative size.
		result.success = result.width != 0 && result.height != 0 &&
			result.width <= max_image_dimension && result.height <= max_image_dimension;

		if (result.success)
		{
			result.picture = tiff_picture(samples);
		}
		else
		{
			result.width = 0;
			result.height = 0;
		}

		return result;
	}
}
