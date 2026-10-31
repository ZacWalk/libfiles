// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The values every area of libfiles shares - so a photo, a raw file and a recording describe the
// same thing the same way, and a reader and a writer convert it by the same rule.

#pragma once

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
	// ---- Scans ---------------------------------------------------------------------------------------

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

	// What a scan of any kind may hold or be helped by.
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

		// The most scan_media inflates in all - compressed ID3 frames, a Matroska track's compressed codec
		// header - and never more than 16 MiB of one. What would pass it is described rather than read, so a
		// file cannot make a scan hold much more than the file itself.
		std::size_t media_inflated_bytes = 32u * 1024u * 1024u;
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

	// ---- Pictures ------------------------------------------------------------------------------------

	// How a picture's colour is coded, as its own header states it: the sampling of a YCbCr picture, or
	// the model of any other. Still images and video share it.
	enum class chroma_format : std::uint8_t
	{
		unknown,
		gray,
		yuv420,
		yuv422,
		yuv444,
		yuv411,
		yuv410,
		yuv440,
		rgb,
		// Printing inks; JPEG's Adobe YCCK, which a decoder turns into them; and CIE L*a*b*.
		cmyk,
		ycck,
		lab,
	};

	// What a picture's samples are, however its format names them.
	struct picture_format
	{
		chroma_format chroma = chroma_format::unknown;
		// Bits a sample - or a palette index - as stored; zero when the file does not say.
		std::uint8_t bit_depth = 0;
		bool alpha = false;
		// Each pixel an index into a table of colours, whose colours chroma gives.
		bool palette = false;

		// The one name libfiles gives a format, a still's or a video's: the chroma's - "gray", "yuv420",
		// "rgb", "cmyk" - with alpha marked as video names it - "graya", "yuva420", "rgba" - and a depth
		// past 8 bits after it, "yuv420 10bit"; or "palette". Empty when the chroma is unknown.
		std::string name() const;

		friend bool operator==(const picture_format&, const picture_format&) = default;
	};

	// ---- Embedded images -----------------------------------------------------------------------------

	// A picture a file carries besides its own: cover art a recording's tags hold, a still image's thumbnail.
	struct embedded_image
	{
		// The encoded image's bytes in the file. Empty when the image is not stored as one run of bytes -
		// base64 in an Ogg comment, an unsynchronised ID3 frame, a thumbnail inside an EXIF block - and data
		// holds it instead.
		byte_range location;
		std::vector<std::uint8_t> data;
		// As stated, "image/jpeg" or "image/png"; empty when the container does not say.
		std::string mime_type;
		// The ID3 and FLAC picture type; 3 is the front cover, 0 any other picture.
		int type = 3;
		std::string description;
	};

	// ---- Orientation ---------------------------------------------------------------------------------
	//
	// EXIF's orientation, 1 to 8, is the one way libfiles states how a picture is turned for display: a
	// still image states it, and a video's container gives it as a turn and a mirror.

	// The orientation of a clockwise turn of 0, 90, 180 or 270 degrees, after a horizontal mirror when
	// mirrored: 1, 6, 3 and 8, or mirrored 2, 7, 4 and 5. Any other turn counts as the quarter below it.
	constexpr std::uint16_t orientation_from_turn(const int clockwise_degrees, const bool mirrored) noexcept
	{
		constexpr std::uint16_t plain[] = {1, 6, 3, 8};
		constexpr std::uint16_t flipped[] = {2, 7, 4, 5};
		const auto quarter = (clockwise_degrees % 360 + 360) % 360 / 90;
		return mirrored ? flipped[quarter] : plain[quarter];
	}

	struct picture_turn
	{
		int clockwise_degrees = 0;
		// Mirrored horizontally before the turn.
		bool mirrored = false;

		friend bool operator==(const picture_turn&, const picture_turn&) = default;
	};

	// The turn an orientation stands for; one outside 1 to 8 is none.
	constexpr picture_turn turn_from_orientation(const std::uint16_t orientation) noexcept
	{
		switch (orientation)
		{
		case 2: return {0, true};
		case 3: return {180, false};
		case 4: return {180, true};
		case 5: return {270, true};
		case 6: return {90, false};
		case 7: return {90, true};
		case 8: return {270, false};
		default: return {};
		}
	}

	// ---- Places --------------------------------------------------------------------------------------

	// Where a picture was taken, in signed decimal degrees: south and west are negative.
	struct gps_position
	{
		double latitude = 0.0;
		double longitude = 0.0;

		friend bool operator==(const gps_position&, const gps_position&) = default;
	};

	// ---- Dates ---------------------------------------------------------------------------------------

	// A date as a file states it: a year alone, a day, or a time of day; in UTC, at a stated offset from
	// it, or in a zone the file does not name.
	struct date
	{
		enum class time_zone : unsigned char
		{
			unstated,
			utc,
			offset,
		};

		int year = 0;
		// 1 to 12, or 0 when only the year is known.
		int month = 0;
		// 1 to 31, or 0 when the day is not known.
		int day = 0;
		bool has_time = false;
		int hour = 0;
		int minute = 0;
		int second = 0;
		time_zone zone = time_zone::unstated;
		// East of UTC, when the zone is an offset.
		int offset_minutes = 0;

		bool valid() const noexcept { return year != 0; }
		// A zone the date states, UTC or an offset: what its offset_minutes are from UTC.
		bool zoned() const noexcept { return zone != time_zone::unstated; }

		friend bool operator==(const date&, const date&) = default;
	};

	// ---- Ratings -------------------------------------------------------------------------------------
	//
	// Stars run from 1 to 5, with 0 for none and -1 for rejected, as XMP's xmp:Rating has them. Windows keeps
	// a percent beside them - EXIF's RatingPercent, and the WM/SharedUserRating of ASF and of an MP4's Xtra
	// box - and Windows Media Player a byte in ID3's POPM frame. Each scale is written and read here, by one
	// rule each way, so a rating reads the same whichever container holds it.

	// Windows' percent for a number of stars, as Windows writes it: 1, 25, 50, 75 or 99; 0 for none or
	// rejected.
	constexpr int percent_from_stars(const int stars) noexcept
	{
		constexpr int percent[] = {0, 1, 25, 50, 75, 99};
		return stars <= 0 ? 0 : percent[stars > 5 ? 5 : stars];
	}

	// Stars from Windows' percent, as Windows reads it: 1 to 12 is one star, 13 to 37 two, 38 to 62 three,
	// 63 to 87 four, and 88 and above five; 0 and below is none.
	constexpr int stars_from_percent(const int percent) noexcept
	{
		if (percent <= 0) return 0;
		if (percent <= 12) return 1;
		if (percent <= 37) return 2;
		if (percent <= 62) return 3;
		if (percent <= 87) return 4;
		return 5;
	}

	// The POPM rating byte Windows Media Player writes for a number of stars: 1, 64, 128, 196 or 255; 0 for
	// none or rejected.
	constexpr int popm_from_stars(const int stars) noexcept
	{
		constexpr int value[] = {0, 1, 64, 128, 196, 255};
		return stars <= 0 ? 0 : value[stars > 5 ? 5 : stars];
	}

	// Stars from a POPM rating byte: 1 to 51 is one star, 52 to 103 two, 104 to 155 three, 156 to 207 four,
	// and 208 and above five; 0 is none.
	constexpr int stars_from_popm(const int value) noexcept
	{
		if (value <= 0) return 0;
		const auto stars = 1 + value / 52;
		return stars > 5 ? 5 : stars;
	}
}
