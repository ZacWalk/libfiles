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
#include "libfiles/identify.h"
#include "libfiles/reader.h"
#include "libfiles/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	// Names a still image format from a file's first bytes. Sixteen are enough for every format;
	// fewer are accepted and match less. PCX and WPG, which carry no reliable signature here, are
	// named only by identify().
	image_format detect_image_format(std::span<const std::uint8_t> head) noexcept;

	// True for the formats scan() reads: all but PCX and WPG, which only decode_document() reads.
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

	// Metadata payloads as the file stores them, with the container's framing removed.
	struct metadata_blocks
	{
		std::vector<std::uint8_t> exif; // a TIFF stream, from its byte-order mark
		std::vector<std::uint8_t> xmp; // the packet
		std::vector<std::uint8_t> iptc; // IPTC-IIM records
		std::vector<std::uint8_t> icc; // the colour profile
		std::vector<std::uint8_t> photoshop; // other Photoshop image resources, as '8BIM' blocks
	};

	struct scan_result
	{
		// What the file's signature says it is, whether or not the rest could be read.
		image_format format = image_format::unknown;
		bool success = false;

		std::uint32_t width = 0;
		std::uint32_t height = 0;
		// The pixel format each scanner has always reported, kept so that a file reads as it did. Sampling is
		// named as HEIF, WebP, JPEG and video name it - "yuv420", "yuv444", "rgb", "rgba" - and the rest as the
		// reader libfiles replaced named it, which differs from format to format: "gray" (PNG), "gray8" (JPEG,
		// PSD) and "grayscale" (HEIF, JPEG XL); "palette" (PNG) and "pal8" (GIF, PSD); "rgb24" (JPEG), "rgb32"
		// (PSD), "RGB24" (BMP); and HEIF's "rgb48" and "rgba64" for 16 bits.
		std::string pixel_format;

		// The same, as one vocabulary for every format and for video (video_format::picture), with the depth
		// and alpha the header states: picture.name() is "gray" whichever format the file is. Unknown when
		// the scanner read no picture header.
		picture_format picture;

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
		// a caller would have to decode, and is not located. thumbnails() lists them as one kind.
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

	// The thumbnails a scan located, as the embedded images a recording's cover art is: the EXIF block's
	// first - its bytes copied out of metadata.exif, which holds them already - then a TIFF file's own and
	// the JFIF extension's, located in the file. Each is a JPEG, and its type 0, "other".
	std::vector<embedded_image> thumbnails(const scan_result& result);

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
