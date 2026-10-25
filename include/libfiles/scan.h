// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Reading what a still image says about itself without decoding it: its format,
// dimensions and pixel format, the metadata blocks it carries, where its embedded thumbnails are,
// and - when asked - how the file is assembled. This is the indexing path, so a scan reads as
// little of the file as the format allows and leaves thumbnails where they are: it reports their
// location, and the caller reads one only if it wants it.

#pragma once

#include "libfiles/exif.h"
#include "libfiles/reader.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libfiles
{
	enum class image_format : std::uint8_t
	{
		unknown,
		jpeg,
		png,
		webp,
		gif,
		psd,
		bmp,
		tiff,
		heif,
		jxl,
		pcx,
		wpg,
	};

	// Names a still image format from a file's first bytes. Sixteen are enough for every format;
	// fewer are accepted and match less.
	image_format detect_image_format(std::span<const std::uint8_t> head) noexcept;

	// True for the formats scan() reads: all of them.
	bool can_scan(image_format format) noexcept;

	struct webp_header
	{
		std::int32_t width = 0;
		std::int32_t height = 0;
		bool has_alpha = false;
		bool has_animation = false;
		// 0 mixed/unknown, 1 lossy, 2 lossless, as in the WebP bitstream.
		int format = 0;
	};

	// Reads only the header; image data may be absent. No result means an invalid or incomplete header.
	std::optional<webp_header> probe_webp_header(reader& input);

	enum class scan_intent : std::uint8_t
	{
		// The cheapest read that still yields dimensions and metadata: what indexing wants.
		index,
		// Also report how the file is assembled, which costs extra reads and formatting.
		inspect,
	};

	// Decompresses a Brotli stream into at most max_size bytes, for the metadata boxes a JPEG XL
	// container keeps compressed in 'brob' boxes. box_type is the type of the box held, "Exif" or
	// "xml ". Returns nothing when the bytes are not one whole Brotli stream, or decompress to more
	// than max_size - which it should find by stopping there, not by decompressing the rest.
	using brotli_decompressor = std::function<std::optional<std::vector<std::uint8_t>>(
		std::span<const std::uint8_t> compressed, std::string_view box_type, std::size_t max_size)>;

	struct scan_limits
	{
		// The most a JPEG's APP13 segments may contribute in Photoshop resources; a resource that
		// would cross it is dropped whole.
		std::size_t jpeg_photoshop_resources = 16u * 1024u * 1024u;

		// libfiles carries no Brotli decoder, so a compressed JPEG XL metadata box is left out unless
		// the caller supplies one. A scan asks it for at most 16 MiB a box and 32 MiB in all; a box it
		// cannot decompress within what is left stops the scan of the boxes after it, as libjxl stops
		// at a box it cannot decompress.
		brotli_decompressor brotli;
	};

	// Metadata payloads as the file stores them, with the container's framing removed.
	struct metadata_blocks
	{
		std::vector<std::uint8_t> exif; // a TIFF stream, from its byte-order mark
		std::vector<std::uint8_t> xmp; // the packet
		std::vector<std::uint8_t> iptc; // IPTC-IIM records
		std::vector<std::uint8_t> icc; // the colour profile
		std::vector<std::uint8_t> photoshop; // other Photoshop image resources, as '8BIM' blocks
	};

	struct byte_range
	{
		std::uint64_t offset = 0;
		std::uint64_t size = 0;

		bool empty() const noexcept { return size == 0; }
		friend bool operator==(const byte_range&, const byte_range&) = default;
	};

	// Sixteen-bit values laid out in rows, such as a JPEG quantisation table.
	struct structure_table
	{
		std::vector<std::uint16_t> values;
		int columns = 0;

		friend bool operator==(const structure_table&, const structure_table&) = default;
	};

	// One row of a structure report: how a file is assembled, as its scanner read it. Rows are in
	// file order; depth and container give the hierarchy. The text is English and unformatted for
	// any particular display.
	struct structure_row
	{
		std::string key;
		std::string value;
		// The raw form, such as a marker code or a byte count.
		std::string shape;
		// Stable across scans of the same file, so a display can keep a row open.
		std::string id;
		// Expanded detail: prose, the leading bytes of a payload, or a table.
		std::variant<std::monostate, std::string, std::vector<std::uint8_t>, structure_table> detail;
		int depth = 0;
		bool container = false;
		// A section worth reading before anyone asks, whatever its size.
		bool open_by_default = false;
	};

	// Where a picture was taken, in signed decimal degrees: south and west are negative.
	struct gps_position
	{
		double latitude = 0.0;
		double longitude = 0.0;
	};

	struct scan_result
	{
		// What the file's signature says it is, whether or not the rest could be read.
		image_format format = image_format::unknown;
		bool success = false;

		std::uint32_t width = 0;
		std::uint32_t height = 0;
		// Named as ffmpeg names pixel formats where one applies: "yuv420", "rgba", "gray8", "pal8".
		// HEIF and JPEG XL keep the names Diffractor gave the formats libheif and libjxl decode to:
		// "grayscale", "yuv420", "yuv422", "yuv444", "rgb", "rgba", "rgb48" and "rgba64".
		std::string pixel_format;

		// The orientation as stored, normally 1 to 8: the EXIF block's - a TIFF file's own directories'
		// - as summarize_exif reads it, else what the image itself states - a JPEG XL codestream's own.
		// Absent when the file states none.
		std::optional<std::uint16_t> orientation;

		// True when the container itself turns the image - a HEIF primary item's irot or imir property -
		// so a decoder that honours the container delivers it already turned, and width and height are
		// those of the turned image. An EXIF orientation describes the same turn, and must then not be
		// applied again.
		bool container_transform = false;

		metadata_blocks metadata;

		// TIFF files keep EXIF in their own directories, not in a separate metadata block. Reading
		// those directories avoids copying the image data into metadata.exif. Individual metadata
		// values above 64 KiB are ignored.
		std::optional<exif_metadata> tiff_metadata;

		// Embedded thumbnails, located but not read. jfif is a JFIF extension thumbnail and tiff a
		// TIFF file's own IFD1 thumbnail, both as file offsets; exif is IFD1's thumbnail inside
		// metadata.exif, as offsets into that block. A HEIF thumbnail is a coded image item, which
		// a caller would have to decode, and is not located.
		byte_range jfif_thumbnail;
		byte_range exif_thumbnail;
		byte_range tiff_thumbnail;

		// TIFF files only: where the file's own GPS directory says the picture was taken, read as
		// read_exif reads a position - both coordinates, and never exactly 0, 0. Elsewhere the position
		// is inside metadata.exif.
		std::optional<gps_position> gps;

		// Only for scan_intent::inspect, and only from the formats that report one.
		std::vector<structure_row> structure;

		// Why the scan stopped, when the file was malformed or unreadable. Then nothing but format
		// is set: a scan reports a file whole or not at all.
		std::string error;
	};

	// Never throws for a malformed file; error says what went wrong. A file shorter than sixteen
	// bytes is not scanned.
	scan_result scan(reader& r, scan_intent intent = scan_intent::index, const scan_limits& limits = {});

	// What a scan takes from an EXIF block: the orientation - the last well-formed one in IFD0, else
	// the last in IFD1, which is read_exif's rule too - and the IFD1 thumbnail's place in the block.
	// Also for blocks found by scanners outside libfiles.
	struct exif_summary
	{
		std::optional<std::uint16_t> orientation;
		byte_range thumbnail;
	};

	exif_summary summarize_exif(std::span<const std::uint8_t> tiff) noexcept;

	// Splits Photoshop image resources: IPTC (0x0404), XMP (0x0424), EXIF (0x0422) and ICC (0x040F)
	// go to their own blocks, each from the first resource of its kind that holds anything, and every
	// other resource except 0x0425, the IPTC digest, is kept in metadata.photoshop. Each block is capped
	// at 16 MiB, and so is their total. The pad byte after the last resource's data may be missing;
	// metadata.photoshop keeps that resource padded. False when the resources are malformed, keeping
	// what was read before the fault.
	bool parse_photoshop_resources(metadata_blocks& metadata, std::span<const std::uint8_t> resources,
	                               bool has_photoshop_signature);
}
