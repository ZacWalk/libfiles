// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Camera raw files, through LibRaw and Adobe's DNG SDK: the image a raw file holds - its largest
// embedded preview a decoder can read, or its raw data developed - and what a scan reads of it without
// developing it: its metadata, an inspector's listing and a thumbnail. A raw format has no signature to
// dispatch on - a caller knows one by its name - so these take the file's path, and LibRaw opens it
// itself, as it would any raw file, a companion file a format keeps beside it included; or a reader over
// a stream the caller opened, with the path to find a companion file by.

#pragma once

#include "libfiles/decode.h"
#include "libfiles/scan.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace libfiles
{
	struct raw_load_result
	{
		// The image was found: in the sink, or as the preview's JPEG.
		bool success = false;
		// The embedded preview rather than the raw data developed.
		bool preview = false;
		// The JPEG preview as the file holds it, for the caller to decode as it decodes an image file; taken
		// once a scan finds it an image of some size. Empty when the image went to the sink.
		std::vector<std::uint8_t> jpeg;
		// The turn to show the image with, 1 to 8 - or as a damaged preview's EXIF states it, by its low
		// byte: the raw data's own for a development, and for a preview as raw_preview_orientation gives it.
		std::uint16_t orientation = 1;
		// What went to the sink: a bitmap preview, or the developed image. The sink is asked to accept
		// the raw data's declared size before LibRaw develops it, and refusing it refuses the image.
		decode_result decoded;
	};

	// The image a raw file holds. With prefer_preview, its largest embedded preview a decoder can read -
	// a CR3's HEVC one passed over for its JPEG - a JPEG one handed back as it is, a bitmap one written
	// to the sink. Otherwise, or without one, its raw data developed with the camera's white balance, as
	// B8G8R8X8 with LibRaw's auto-brightness, the fourth byte 0, left unturned: the orientation says how
	// to show it. Never throws for a malformed file.
	raw_load_result load_raw(const std::filesystem::path& path, image_sink& sink, bool prefer_preview);

	// The orientation to show a raw file's preview with: its own, unless the raw image is not square and
	// the preview, turned by its own, is not the raw image's shape turned by the raw data's - a preview
	// stored as the sensor saw it, or that states a turn it does not need - when the raw data's applies.
	// Shapes are width to height in tenths, and a preview of no size has none. The image is the raw
	// data's, as LibRaw sizes it, with its flip as an orientation.
	std::uint16_t raw_preview_orientation(std::uint32_t preview_width, std::uint32_t preview_height,
	                                      std::uint16_t preview_orientation, std::uint32_t image_width,
	                                      std::uint32_t image_height, std::uint16_t image_orientation);

	// A date as a camera's clock read it: a time of day with no zone.
	struct raw_date
	{
		int year = 0;
		int month = 0;
		int day = 0;
		int hour = 0;
		int minute = 0;
		int second = 0;
	};

	struct raw_scan_result
	{
		// LibRaw read the file and sized its image. Nothing but the thumbnail is set otherwise.
		bool success = false;

		std::uint32_t width = 0;
		std::uint32_t height = 0;
		// LibRaw's flip, 1 to 8, as EXIF numbers an orientation.
		std::uint16_t orientation = 1;
		// The XMP packet the file holds.
		std::vector<std::uint8_t> xmp;
		// When the picture was taken, by the camera's clock.
		std::optional<raw_date> created;
		// LibRaw's colour description, "RGBG" for a Bayer sensor; and the strings the camera wrote, each
		// with whitespace trimmed from its ends.
		std::string pixel_format;
		std::string camera_make;
		std::string camera_model;
		std::string comment;
		std::string artist;
		int iso_speed = 0;
		float exposure_time = 0;
		float f_number = 0;
		float focal_length = 0;
		// Where it was taken: both coordinates in range, and not exactly 0, 0.
		std::optional<gps_position> gps;
		// For scan_intent::inspect: what LibRaw parsed, by section - each a container row named with how
		// many rows it holds, and a section that gathered none left out.
		std::vector<structure_row> listing;

		// For a caller that gives a sink: the largest thumbnail a decoder reads - a JPEG one's bytes, read
		// only when the raw image has a size, for the caller to decode and turn with
		// raw_preview_orientation against the raw image below; or a bitmap one written to the sink,
		// turned, as thumbnail says.
		std::vector<std::uint8_t> thumbnail_jpeg;
		decode_result thumbnail;
		std::uint32_t image_width = 0;
		std::uint32_t image_height = 0;
		std::uint16_t image_orientation = 1;
	};

	// What a raw file holds besides its image, with LibRaw's parse alone: nothing is developed. Each
	// thread scans with an instance of LibRaw it keeps, as constructing one costs half a megabyte of
	// zeros. Never throws for a malformed file.
	raw_scan_result scan_raw(const std::filesystem::path& path, scan_intent intent, image_sink* thumbnail_sink);

	// The same, read through a reader: a raw file the caller opened - over a network share, held in memory -
	// read as every other format is, through the reader's window. name is the file's path, by which LibRaw
	// finds the companion file a few formats keep beside the raw one; empty when there is none to find. A read
	// the stream fails fails the scan or the load, as it fails a scan of any other format. Never throws for a
	// malformed file.
	raw_scan_result scan_raw(reader& source, scan_intent intent, image_sink* thumbnail_sink, const std::filesystem::path& name = {});
	raw_load_result load_raw(reader& source, image_sink& sink, bool prefer_preview, const std::filesystem::path& name = {});
}
