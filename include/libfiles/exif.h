// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Reading an EXIF block - the TIFF stream a JPEG's APP1 segment, a PNG's eXIf chunk, a WebP's
// EXIF chunk or a HEIF's Exif item carries - for what an application shows of a photograph: the camera
// and lens, the exposure, the dates, where it was taken, its rating and how it is turned. And writing
// one: a fresh block for a picture whose file has none, and the numbers a turned picture changes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	// A date as EXIF records it: the "YYYY:MM:DD HH:MM:SS" text, without its trailing NULs and spaces,
	// and the OffsetTime and SubSecTime that go with it. Left as text, for the caller's own date type.
	struct exif_date
	{
		std::string text;
		std::string offset;
		std::string sub_second;
	};

	// What an EXIF block says about its picture. Text is UTF-8: read from ASCII and UTF-8, from the
	// UTF-16 of Windows' XP tags and of a UserComment marked "UNICODE", and trimmed. Text that turns
	// out to be binary - some cameras put debug data in text tags - reads as empty, and a text tag
	// stored as a number reads as "?", but for the XP tags, which Windows writes as bytes.
	struct exif_metadata
	{
		// As stored: the last Orientation in IFD0, else the last in IFD1, which describes the thumbnail -
		// the rule summarize_exif keeps, and with it the scan and the JPEG decoder. A copy in any other
		// directory is not the image's.
		std::optional<std::uint16_t> orientation;

		std::string camera_make;
		std::string camera_model;
		// LensModel, or what a Canon maker note says if it comes first.
		std::string lens;
		std::string software;
		// Artist, else the XPAuthor Windows writes.
		std::string artist;
		std::string copyright;
		// ImageDescription, else XPSubject.
		std::string description;
		// UserComment, else XPComment.
		std::string comment;
		// XPTitle and XPKeywords.
		std::string title;
		std::string keywords;

		// FNumber, else ApertureValue or MaxApertureValue, converted from APEX.
		float f_number = 0.0f;
		// In seconds: ExposureTime, else ShutterSpeedValue, converted from APEX.
		float exposure_time = 0.0f;
		float focal_length = 0.0f;
		std::uint16_t focal_length_35mm = 0;
		std::uint16_t iso_speed = 0;
		// Windows' Rating, or its RatingPercent: -1 for rejected, else 0 to 5 stars.
		std::optional<std::int16_t> rating;

		exif_date modified;
		exif_date original;
		exif_date digitized;

		// Where it was taken, in signed decimal degrees: both or neither, and never exactly 0, 0, which
		// is what a receiver with no fix writes.
		std::optional<double> latitude;
		std::optional<double> longitude;
		// Metres, negative below sea level.
		float altitude = 0.0f;
		// Kilometres an hour.
		float speed = 0.0f;
		// GPSDateStamp and GPSTimeStamp: the moment of the fix, in UTC.
		std::string gps_date;
		std::optional<std::array<double, 3>> gps_time;
	};

	// Reads a TIFF stream: an EXIF block after its "Exif\0\0" signature. It walks IFD0 and the Exif,
	// GPS and Interoperability directories it points to and a Canon maker note, and IFD1 for the
	// orientation alone. An entry counts only if it is well formed - a type no wider than FLOAT, at
	// least one value, and its values inside the block - and where two tags record the same thing, the
	// one read later wins unless this header says otherwise. A block that is not TIFF reads as empty,
	// and a damaged one as far as it can be read.
	exif_metadata read_exif(std::span<const std::uint8_t> tiff);

	// What make_exif writes for a picture whose file carries no EXIF. A field left empty is not
	// written. Text is written as given: an ASCII entry with its NUL, and UserComment behind its
	// eight-byte ASCII mark. An entry that would pass 65,536 bytes is not written, as libexif writes none.
	struct exif_fields
	{
		std::optional<std::uint16_t> orientation;
		// Written as a rational to four decimal places, reduced; zero, negative or past 429,496 is not
		// written.
		std::optional<double> f_number;
		// In seconds: under one second as 1/n, the nearest n, else as f_number is.
		std::optional<double> exposure_time;
		std::optional<std::uint16_t> iso_speed;
		std::optional<double> focal_length;
		std::optional<std::uint16_t> focal_length_35mm;
		std::string camera_make;
		std::string camera_model;
		std::string artist;
		std::string lens;
		std::string copyright;
		std::string description;
		// UserComment, marked ASCII.
		std::string comment;
		// "YYYY:MM:DD HH:MM:SS": DateTimeOriginal, DateTime and DateTimeDigitized.
		std::string original;
		std::string modified;
		std::string digitized;
	};

	// A little-endian TIFF stream, from its byte-order mark, laid out byte for byte as libexif lays
	// out a new block: the entries libexif makes mandatory for a compressed image - 72 dpi, centred
	// YCbCr positioning, Exif version 2.1, Y Cb Cr components, Flashpix 1.0, an uncalibrated colour space
	// and pixel dimensions of 0 - then the fields, IFD0 and the Exif directory each sorted by tag, the
	// values longer than four bytes after their directory in the order above, padded to even lengths.
	std::vector<std::uint8_t> make_exif(const exif_fields& fields);

	// The directories of an EXIF block, found as libexif finds them: IFD0, the IFD1 it links to, and
	// the Exif, GPS and Interoperability directories the first pointer to each leads to.
	enum class exif_directory : std::uint8_t
	{
		primary,
		thumbnail,
		exif,
		gps,
		interoperability,
	};

	struct exif_number
	{
		exif_directory directory;
		std::uint16_t tag;
		std::uint32_t value;
	};

	// Overwrites numbers the block already holds, where they are: every well-formed entry with each tag
	// in its directory, if it is a SHORT, a LONG or an SLONG, takes the value as its first - cut to
	// sixteen bits for a SHORT. A block that repeats a tag reads back as written whichever copy a
	// reader takes, read_exif's last included. Nothing else in the block changes or moves, so maker
	// notes keep the offsets they rely on; an entry the block lacks is not added. Returns how many were
	// overwritten.
	std::size_t set_exif_numbers(std::span<std::uint8_t> tiff, std::span<const exif_number> numbers);

	// Unlinks IFD1, the thumbnail's directory, where it stands: the link that ends IFD0 becomes 0, so
	// a reader no longer finds the thumbnail - which shows the picture as it was before an edit - and
	// nothing else in the block changes. False when the block is not TIFF, or IFD0 and its link do
	// not lie wholly in it.
	bool unlink_exif_thumbnail(std::span<std::uint8_t> tiff);
}
