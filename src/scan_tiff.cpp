// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The TIFF scanner. A TIFF file is a TIFF stream as an EXIF block is, and is read with the
// same directory reader: IFD0 for the primary image's dimensions and XMP, and - by the rules
// read_exif and summarize_exif keep - the camera, exposure, dates, orientation, thumbnail and GPS.

#include "scan_internal.h"
#include "tiff_internal.h"

namespace libfiles::detail
{
	namespace
	{
		// The only two TIFF byte-order marks. Each reads the same either way round.
		constexpr std::uint16_t order_intel = 0x4949;
		constexpr std::uint16_t order_motorola = 0x4D4D;

		constexpr std::uint16_t tag_image_width = 0x0100;
		constexpr std::uint16_t tag_image_length = 0x0101;
		constexpr std::uint16_t tag_xmp = 700;

		// An XMP packet's length is an unvalidated 32-bit field; this caps what a scan may be asked to read.
		constexpr std::uint32_t max_tiff_xmp_bytes = 8u * 1024u * 1024u;
	}

	scan_result scan_tiff(reader& r)
	{
		scan_result result;

		const auto order = load_le16(r.view(0, 8).data());
		const auto size = r.size();

		if (size < 12u || (order != order_intel && order != order_motorola)) return result;

		const tiff_bytes data(r, order == order_intel);

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
				case tag_xmp:
					// The packet's bytes, however its type is stated. Four or fewer are stored in the entry
					// itself; only more put an offset there.
					if (e.count <= max_tiff_xmp_bytes)
					{
						const std::uint64_t at = e.count <= 4u ? e.offset + 8u : e.field_u32();
						if (r.contains(at, e.count)) result.metadata.xmp = r.read(at, e.count);
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

		if (!result.success)
		{
			result.width = 0;
			result.height = 0;
		}

		return result;
	}
}
