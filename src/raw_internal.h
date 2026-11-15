// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: LibRaw as libfiles' raw code includes it, and what that code shares: the listing of what LibRaw
// parsed, output brightness conversion, and the rule a GPS position it parsed must meet.

#pragma once

#include "libfiles/scan.h"

#define LIBRAW_NODLL 1
#define LIBRAW_LIBRARY_BUILD 1
#ifdef _WIN32
#define LIBRAW_WIN32_UNICODEPATHS 1
#endif
#define USE_DNGSDK 1

#include "dng_host.h"

// LibRaw redefines the DNG SDK's feature flags for its own headers; they are the values the SDK above was
// built with, so the redefinition is only silenced.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4005)
#endif
#include <libraw/libraw.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <optional>
#include <vector>

namespace libfiles::detail
{
	// LibRaw truncates output brightness fractions; non-normal values remain zero and casts are bounded.
	int raw_output_integer(float value);

	// A development's highlights: linear light x, in units of the auto-brightness white, to the light shown,
	// 0 to 1. Below four fifths of white it is x, as LibRaw's own developer shows it; above, the light up to
	// the brightest the image holds, x_max, rolls off into white along u / (1 + au), which meets the line with
	// its slope and reaches white at x_max - where LibRaw's developer clips its top percent. An image with
	// nothing past white is shown as LibRaw shows it.
	double raw_highlight_rolloff(double x, double x_max);

	// LibRaw's flip, 0 to 7, as the orientation it describes, numbered as EXIF numbers them; 1 for any
	// other.
	std::uint16_t orientation_of_flip(int flip);

	// Degrees, minutes and seconds as LibRaw parses them, in decimal degrees.
	double raw_gps_degrees(const float (&dms)[3]);

	// Whether LibRaw parsed a position: a GPS directory, both coordinates in range, and not exactly 0, 0.
	// LibRaw marks any GPS directory parsed, and many cameras write one holding only its version; a
	// coordinate on the equator or the meridian is a position.
	bool raw_gps_fix(bool parsed, double latitude, double longitude);
	bool raw_gps_fix(const libraw_gps_info_t& gps);

	// The position LibRaw parsed, south and west negative, where raw_gps_fix finds one.
	std::optional<gps_position> raw_gps_position(const libraw_gps_info_t& gps);

	// What LibRaw parsed, for an inspector, by section: each section a container row named with how many
	// rows it holds, and a section that gathered none left out.
	std::vector<structure_row> list_raw(const libraw_data_t& data);
}
