// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif (libexif/fuji): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.

// Purpose: libexif's fuji maker note decoder: how it recognises the note, reads its entries and
// names and renders each value.

#include "libexif_makernote.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4996) // strcpy, strncpy and strncat, as libexif calls them
#pragma warning(disable : 4244 4267 4100 4189 4456 4457 4459 4702) // and its implicit narrowing and leftovers
#endif

namespace libfiles::detail::libexif
{
// libexif/fuji/mnote-fuji-tag.h

/* mnote-fuji-tag.h
 *
 * Copyright (c) 2002 Lutz Mueller <lutz@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */




typedef unsigned int _MnoteFujiTag;
enum : unsigned int {
	MNOTE_FUJI_TAG_VERSION                  = 0x0000,
	MNOTE_FUJI_TAG_SERIAL_NUMBER            = 0x0010,
	MNOTE_FUJI_TAG_QUALITY                  = 0x1000,
	MNOTE_FUJI_TAG_SHARPNESS                = 0x1001,
	MNOTE_FUJI_TAG_WHITE_BALANCE            = 0x1002,
	MNOTE_FUJI_TAG_COLOR                    = 0x1003,
	MNOTE_FUJI_TAG_TONE                     = 0x1004,
	MNOTE_FUJI_TAG_UNKNOWN_1006             = 0x1006,
	MNOTE_FUJI_TAG_UNKNOWN_1007             = 0x1007,
	MNOTE_FUJI_TAG_UNKNOWN_1008             = 0x1008,
	MNOTE_FUJI_TAG_UNKNOWN_1009             = 0x1009,
	MNOTE_FUJI_TAG_UNKNOWN_100A             = 0x100A,
	MNOTE_FUJI_TAG_UNKNOWN_100B             = 0x100B,
	MNOTE_FUJI_TAG_FLASH_MODE               = 0x1010,
	MNOTE_FUJI_TAG_FLASH_STRENGTH           = 0x1011,
	MNOTE_FUJI_TAG_MACRO                    = 0x1020,
	MNOTE_FUJI_TAG_FOCUS_MODE               = 0x1021,
	MNOTE_FUJI_TAG_UNKNOWN_1022             = 0x1022,
	MNOTE_FUJI_TAG_FOCUS_POINT              = 0x1023,
	MNOTE_FUJI_TAG_UNKNOWN_1024             = 0x1024,
	MNOTE_FUJI_TAG_UNKNOWN_1025             = 0x1025,
	MNOTE_FUJI_TAG_SLOW_SYNC                = 0x1030,
	MNOTE_FUJI_TAG_PICTURE_MODE             = 0x1031,
	MNOTE_FUJI_TAG_UNKNOWN_1032             = 0x1032,
	MNOTE_FUJI_TAG_CONT_TAKING              = 0x1100,
	MNOTE_FUJI_TAG_SEQUENCE_NUMBER          = 0x1101,
	MNOTE_FUJI_TAG_UNKNOWN_1200             = 0x1200,
	MNOTE_FUJI_TAG_FINEPIX_COLOR            = 0x1210,
	MNOTE_FUJI_TAG_BLUR_CHECK               = 0x1300,
	MNOTE_FUJI_TAG_FOCUS_CHECK              = 0x1301,
	MNOTE_FUJI_TAG_AUTO_EXPOSURE_CHECK      = 0x1302,
	MNOTE_FUJI_TAG_UNKNOWN_1303             = 0x1303,
	MNOTE_FUJI_TAG_DYNAMIC_RANGE            = 0x1400,
	MNOTE_FUJI_TAG_FILM_MODE                = 0x1401,
	MNOTE_FUJI_TAG_DYNAMIC_RANGE_SETTING    = 0x1402,
	MNOTE_FUJI_TAG_DEV_DYNAMIC_RANGE_SETTING= 0x1403,
	MNOTE_FUJI_TAG_MIN_FOCAL_LENGTH         = 0x1404,
	MNOTE_FUJI_TAG_MAX_FOCAL_LENGTH         = 0x1405,
	MNOTE_FUJI_TAG_MAX_APERT_AT_MIN_FOC     = 0x1406,
	MNOTE_FUJI_TAG_MAX_APERT_AT_MAX_FOC     = 0x1407,
	MNOTE_FUJI_TAG_UNKNOWN_1408             = 0x1408,
	MNOTE_FUJI_TAG_UNKNOWN_1409             = 0x1409,
	MNOTE_FUJI_TAG_UNKNOWN_140A             = 0x140A,
	MNOTE_FUJI_TAG_UNKNOWN_1410             = 0x1410,
	MNOTE_FUJI_TAG_UNKNOWN_1421             = 0x1421,
	MNOTE_FUJI_TAG_UNKNOWN_4100             = 0x4100,
	MNOTE_FUJI_TAG_UNKNOWN_4800             = 0x4800,
	MNOTE_FUJI_TAG_FILE_SOURCE              = 0x8000,
	MNOTE_FUJI_TAG_ORDER_NUMBER             = 0x8002,
	MNOTE_FUJI_TAG_FRAME_NUMBER             = 0x8003
};
typedef unsigned int MnoteFujiTag;

const char *mnote_fuji_tag_get_name        (MnoteFujiTag tag);
const char *mnote_fuji_tag_get_title       (MnoteFujiTag tag);
const char *mnote_fuji_tag_get_description (MnoteFujiTag tag);

// libexif/fuji/mnote-fuji-entry.h

/* mnote-fuji-entry.h
 *
 * Copyright (c) 2002 Lutz Mueller <lutz@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */



typedef struct _MnoteFujiEntry        MnoteFujiEntry;
typedef struct _MnoteFujiEntryPrivate MnoteFujiEntryPrivate;


struct _MnoteFujiEntry {
	MnoteFujiTag tag;
	ExifFormat format;
	unsigned long components;

	unsigned char *data;
	unsigned int size;

	ExifByteOrder order;
};

char *mnote_fuji_entry_get_value (MnoteFujiEntry *entry, char *val, unsigned int maxlen);

// libexif/fuji/exif-mnote-data-fuji.h

/* exif-mnote-data-fuji.h
 *
 * Copyright (c) 2002 Lutz Mueller <lutz@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */



typedef struct _ExifMnoteDataFuji        ExifMnoteDataFuji;

struct _ExifMnoteDataFuji {
	ExifMnoteData parent;

	MnoteFujiEntry *entries;
	unsigned int count;

	ExifByteOrder order;
	unsigned int offset;
};

/*! Detect if MakerNote is recognized as one handled by the Fuji module.
 * 
 * \param[in] ed image #ExifData to identify as as a Fuji type
 * \param[in] e #ExifEntry for EXIF_TAG_MAKER_NOTE, from within ed but
 *   duplicated here for convenience
 * \return 0 if not recognized, nonzero if recognized. The specific nonzero 
 *   value returned may identify a subtype unique within this module.
 */
int exif_mnote_data_fuji_identify (const ExifData *ed, const ExifEntry *e);

ExifMnoteData *exif_mnote_data_fuji_new (ExifMem *);

// libexif/fuji/mnote-fuji-tag.c

/* mnote-fuji-tag.c
 *
 * Copyright (c) 2002 Lutz Mueller <lutz@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */





static const struct {
	MnoteFujiTag tag;
	const char *name;
	const char *title;
	const char *description;
} table[] = {
#ifndef NO_VERBOSE_TAG_STRINGS
	{MNOTE_FUJI_TAG_VERSION, "Version", N_("Maker Note Version"), ""},
	{MNOTE_FUJI_TAG_SERIAL_NUMBER, "SerialNumber", N_("Serial Number"), N_("This number is unique and based on the date of manufacture.")},
	{MNOTE_FUJI_TAG_QUALITY, "Quality", N_("Quality"), ""},
	{MNOTE_FUJI_TAG_SHARPNESS, "Sharpness", N_("Sharpness"), ""},
	{MNOTE_FUJI_TAG_WHITE_BALANCE, "WhiteBalance", N_("White Balance"), ""},
	{MNOTE_FUJI_TAG_COLOR, "ChromaticitySaturation", N_("Chromaticity Saturation"), ""},
	{MNOTE_FUJI_TAG_TONE, "Contrast", N_("Contrast"), ""},
	{MNOTE_FUJI_TAG_FLASH_MODE, "FlashMode", N_("Flash Mode"), ""},
	{MNOTE_FUJI_TAG_FLASH_STRENGTH, "FlashStrength", N_("Flash Firing Strength Compensation"), ""},
	{MNOTE_FUJI_TAG_MACRO, "MacroMode", N_("Macro Mode"), ""},
	{MNOTE_FUJI_TAG_FOCUS_MODE, "FocusingMode", N_("Focusing Mode"), ""},
	{MNOTE_FUJI_TAG_FOCUS_POINT, "FocusPoint", N_("Focus Point"), ""},
	{MNOTE_FUJI_TAG_SLOW_SYNC, "SlowSynchro", N_("Slow Synchro Mode"), ""},
	{MNOTE_FUJI_TAG_PICTURE_MODE, "PictureMode", N_("Picture Mode"), ""},
	{MNOTE_FUJI_TAG_CONT_TAKING, "ContinuousTaking", N_("Continuous Taking"), ""},
	{MNOTE_FUJI_TAG_SEQUENCE_NUMBER, "ContinuousSequence", N_("Continuous Sequence Number"), ""},
	{MNOTE_FUJI_TAG_FINEPIX_COLOR, "FinePixColor", N_("FinePix Color"), ""},
	{MNOTE_FUJI_TAG_BLUR_CHECK, "BlurCheck", N_("Blur Check"), ""},
	{MNOTE_FUJI_TAG_FOCUS_CHECK, "AutoFocusCheck", N_("Auto Focus Check"), ""},
	{MNOTE_FUJI_TAG_AUTO_EXPOSURE_CHECK, "AutoExposureCheck", N_("Auto Exposure Check"), ""},
	{MNOTE_FUJI_TAG_DYNAMIC_RANGE, "DynamicRange", N_("Dynamic Range"), ""},
	{MNOTE_FUJI_TAG_FILM_MODE, "FilmMode", N_("Film Simulation Mode"), ""},
	{MNOTE_FUJI_TAG_DYNAMIC_RANGE_SETTING, "DRangeMode", N_("Dynamic Range Wide Mode"), ""},
	{MNOTE_FUJI_TAG_DEV_DYNAMIC_RANGE_SETTING, "DevDRangeMode", N_("Development Dynamic Range Wide Mode"), ""},
	{MNOTE_FUJI_TAG_MIN_FOCAL_LENGTH, "MinFocalLen", N_("Minimum Focal Length"), ""},
	{MNOTE_FUJI_TAG_MAX_FOCAL_LENGTH, "MaxFocalLen", N_("Maximum Focal Length"), ""},
	{MNOTE_FUJI_TAG_MAX_APERT_AT_MIN_FOC, "MaxApertAtMinFoc", N_("Maximum Aperture at Minimum Focal"), ""},
	{MNOTE_FUJI_TAG_MAX_APERT_AT_MAX_FOC, "MaxApertAtMaxFoc", N_("Maximum Aperture at Maximum Focal"), ""},
	{MNOTE_FUJI_TAG_FILE_SOURCE, "FileSource", N_("File Source"), ""},
	{MNOTE_FUJI_TAG_ORDER_NUMBER, "OrderNumber", N_("Order Number"), ""},
	{MNOTE_FUJI_TAG_FRAME_NUMBER, "FrameNumber", N_("Frame Number"), ""},
#endif
	{0, NULL, NULL, NULL}
};

const char *
mnote_fuji_tag_get_name (MnoteFujiTag t)
{
	unsigned int i;

	for (i = 0; i < sizeof (table) / sizeof (table[0]); i++)
		if (table[i].tag == t) return (table[i].name);
	return NULL;
}

const char *
mnote_fuji_tag_get_title (MnoteFujiTag t)
{
	unsigned int i;

	for (i = 0; i < sizeof (table) / sizeof (table[0]); i++)
		if (table[i].tag == t) {
			if (!table[i].title)
				return NULL;
			return _(table[i].title);
		}
	return NULL;
}

const char *
mnote_fuji_tag_get_description (MnoteFujiTag t)
{
	unsigned int i;

	for (i = 0; i < sizeof (table) / sizeof (table[0]); i++)
		if (table[i].tag == t) {
			if (!table[i].description || !*table[i].description)
				return "";
			return _(table[i].description);
		}
	return NULL;
}

// libexif/fuji/mnote-fuji-entry.c

/* mnote-fuji-entry.c
 *
 * Copyright (c) 2002 Lutz Mueller <lutz@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */





#define CF(format,target,v,maxlen)                              \
{                                                               \
        if (format != target) {                                 \
                snprintf (v, maxlen,	                        \
                        _("Invalid format '%s', "               \
                        "expected '%s'."),                      \
                        exif_format_get_name (format),          \
                        exif_format_get_name (target));         \
                break;                                          \
        }                                                       \
}

#define CC(number,target,v,maxlen)                                      \
{                                                                       \
        if (number != target) {                                         \
                snprintf (v, maxlen,                                    \
                        _("Invalid number of components (%i, "          \
                        "expected %i)."), (int) number, (int) target);  \
                break;                                                  \
        }                                                               \
}

static const struct {
	MnoteFujiTag tag;
	struct {
		int index;
		const char *string;
	} elem[22];
} items[] = {
#ifndef NO_VERBOSE_TAG_DATA
  { MNOTE_FUJI_TAG_SHARPNESS,
    { {1, N_("Softest")},
      {2, N_("Soft")},
      {3, N_("Normal")},
      {4, N_("Hard")},
      {5, N_("Hardest")},
      {0x0082, N_("Medium soft")},
      {0x0084, N_("Medium hard")},
      {0x8000, N_("Film simulation mode")},
      {0xFFFF, N_("Off")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_WHITE_BALANCE,
    { {0, N_("Auto")},
      {0x100, N_("Daylight")},
      {0x200, N_("Cloudy")},
      {0x300, N_("Daylight fluorescent")},
      {0x301, N_("Day white fluorescent")},
      {0x302, N_("White fluorescent")},
      {0x400, N_("Incandescent")},
      {0x500, N_("Flash")},
      {0xF00, N_("Custom")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_COLOR,
    { {0, N_("Standard")},
      {0x0080, N_("Medium high")},
      {0x0100, N_("High")},
      {0x0180, N_("Medium low")},
      {0x0200, N_("Original")},
      {0x0300, N_("Black & white")},
      {0x8000, N_("Film simulation mode")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_TONE,
    { {0, N_("Standard")},
      {0x0080, N_("Medium hard")},
      {0x0100, N_("Hard")},
      {0x0180, N_("Medium soft")},
      {0x0200, N_("Original")},
      {0x8000, N_("Film simulation mode")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_FLASH_MODE,
    { {0, N_("Auto")},
      {1, N_("On")},
      {2, N_("Off")},
      {3, N_("Red-eye reduction")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_MACRO,
    { {0, N_("Off")},
      {1, N_("On")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_FOCUS_MODE,
    { {0, N_("Auto")},
      {1, N_("Manual")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_SLOW_SYNC,
    { {0, N_("Off")},
      {1, N_("On")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_PICTURE_MODE,
    { {0, N_("Auto")},
      {1, N_("Portrait")},
      {2, N_("Landscape")},
      {4, N_("Sports")},
      {5, N_("Night")},
      {6, N_("Program AE")},
      {7, N_("Natural photo")},
      {8, N_("Vibration reduction")},
      {0x000A, N_("Sunset")},
      {0x000B, N_("Museum")},
      {0x000C, N_("Party")},
      {0x000D, N_("Flower")},
      {0x000E, N_("Text")},
      {0x000F, N_("NP & flash")},
      {0x0010, N_("Beach")},
      {0x0011, N_("Snow")},
      {0x0012, N_("Fireworks")},
      {0x0013, N_("Underwater")},
      {0x0100, N_("Aperture priority AE")},
      {0x0200, N_("Shutter priority AE")},
      {0x0300, N_("Manual exposure")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_CONT_TAKING,
    { {0, N_("Off")},
      {1, N_("On")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_FINEPIX_COLOR,
    { {0x00, N_("F-Standard")},
      {0x10, N_("F-Chrome")},
      {0x30, N_("F-B&W")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_BLUR_CHECK,
    { {0, N_("No blur")},
      {1, N_("Blur warning")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_FOCUS_CHECK,
    { {0, N_("Focus good")},
      {1, N_("Out of focus")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_AUTO_EXPOSURE_CHECK,
    { {0, N_("AE good")},
      {1, N_("Over exposed")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_DYNAMIC_RANGE,
    { {1, N_("Standard")},
      {3, N_("Wide")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_FILM_MODE,
    { {0, N_("F0/Standard")},
      {0x0100, N_("F1/Studio portrait")},
      {0x0110, N_("F1a/Professional portrait")},
      {0x0120, N_("F1b/Professional portrait")},
      {0x0130, N_("F1c/Professional portrait")},
      {0x0200, N_("F2/Fujichrome")},
      {0x0300, N_("F3/Studio portrait Ex")},
      {0x0400, N_("F4/Velvia")},
      {0, NULL}}},
  { MNOTE_FUJI_TAG_DYNAMIC_RANGE_SETTING,
    { {0, N_("Auto (100-400%)")},
      {1, N_("RAW")},
      {0x0100, N_("Standard (100%)")},
      {0x0200, N_("Wide1 (230%)")},
      {0x0201, N_("Wide2 (400%)")},
      {0x8000, N_("Film simulation mode")},
      {0, NULL}}},
#endif
  {0, {{0, NULL}}}
};


char *
mnote_fuji_entry_get_value (MnoteFujiEntry *entry,
			      char *val, unsigned int maxlen)
{
	ExifLong  vl;
	ExifSLong vsl;
	ExifShort vs, vs2;
	ExifSShort vss;
	ExifRational vr;
	ExifSRational vsr;
	int i, j;

	if (!entry) return (NULL);
	if (maxlen < 1) return NULL;

	memset (val, 0, maxlen);
	maxlen--;

	switch (entry->tag) {
	  case MNOTE_FUJI_TAG_VERSION:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, val, maxlen)
		CC (entry->components, 4, val, maxlen)
		memcpy (val, entry->data, MIN(maxlen, entry->size));
		break;
	  case MNOTE_FUJI_TAG_SHARPNESS:
	  case MNOTE_FUJI_TAG_WHITE_BALANCE:
	  case MNOTE_FUJI_TAG_COLOR:
  	  case MNOTE_FUJI_TAG_TONE:
	  case MNOTE_FUJI_TAG_FLASH_MODE:
	  case MNOTE_FUJI_TAG_MACRO:
	  case MNOTE_FUJI_TAG_FOCUS_MODE:
	  case MNOTE_FUJI_TAG_SLOW_SYNC:
	  case MNOTE_FUJI_TAG_PICTURE_MODE:
	  case MNOTE_FUJI_TAG_CONT_TAKING:
	  case MNOTE_FUJI_TAG_FINEPIX_COLOR:
	  case MNOTE_FUJI_TAG_BLUR_CHECK:
	  case MNOTE_FUJI_TAG_FOCUS_CHECK:
	  case MNOTE_FUJI_TAG_AUTO_EXPOSURE_CHECK:
	  case MNOTE_FUJI_TAG_DYNAMIC_RANGE:
	  case MNOTE_FUJI_TAG_FILM_MODE:
	  case MNOTE_FUJI_TAG_DYNAMIC_RANGE_SETTING:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		CC (entry->components, 1, val, maxlen)
		vs = exif_get_short (entry->data, entry->order);

		/* search the tag */
		for (i = 0; (items[i].tag && items[i].tag != entry->tag); i++);
		if (!items[i].tag) {
			snprintf (val, maxlen,
				  _("Internal error (unknown value %i)"), vs);
		  	break;
		}

		/* find the value */
		for (j = 0; items[i].elem[j].string &&
		    (items[i].elem[j].index < vs); j++);
		if (items[i].elem[j].index != vs) {
			snprintf (val, maxlen,
				  _("Internal error (unknown value %i)"), vs);
			break;
		}
		strncpy (val, _(items[i].elem[j].string), maxlen);
		break;
	  case MNOTE_FUJI_TAG_FOCUS_POINT:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		CC (entry->components, 2, val, maxlen)
		vs = exif_get_short (entry->data, entry->order);
		vs2 = exif_get_short (entry->data+2, entry->order);
		snprintf (val, maxlen, "%i, %i", vs, vs2);
		break;
	  case MNOTE_FUJI_TAG_MIN_FOCAL_LENGTH:
	  case MNOTE_FUJI_TAG_MAX_FOCAL_LENGTH:
		CF (entry->format, EXIF_FORMAT_RATIONAL, val, maxlen)
		CC (entry->components, 1, val, maxlen)
		vr = exif_get_rational (entry->data, entry->order);
		if (!vr.denominator) break;
		snprintf (val, maxlen, _("%2.2f mm"), (double) vr.numerator /
			  vr.denominator);
		break;

	default:
		switch (entry->format) {
		case EXIF_FORMAT_ASCII:
		  strncpy (val, (char *)entry->data, MIN(maxlen, entry->size));
		  break;
		case EXIF_FORMAT_SHORT:
		  vs = exif_get_short (entry->data, entry->order);
		  snprintf (val, maxlen, "%hu", vs);
		  break;
		case EXIF_FORMAT_SSHORT:
		  vss = exif_get_sshort (entry->data, entry->order);
		  snprintf (val, maxlen, "%hi", vss);
		  break;
		case EXIF_FORMAT_LONG:
		  vl = exif_get_long (entry->data, entry->order);
		  snprintf (val, maxlen, "%lu", (long unsigned) vl);
		  break;
		case EXIF_FORMAT_SLONG:
		  vsl = exif_get_slong (entry->data, entry->order);
		  snprintf (val, maxlen, "%li", (long int) vsl);
		  break;
		case EXIF_FORMAT_RATIONAL:
		  vr = exif_get_rational (entry->data, entry->order);
		  if (!vr.denominator) break;
		  snprintf (val, maxlen, "%2.4f", (double) vr.numerator /
						    vr.denominator);
		  break;
		case EXIF_FORMAT_SRATIONAL:
		  vsr = exif_get_srational (entry->data, entry->order);
		  if (!vsr.denominator) break;
		  snprintf (val, maxlen, "%2.4f", (double) vsr.numerator /
			  vsr.denominator);
		  break;
		case EXIF_FORMAT_UNDEFINED:
		default:
		  snprintf (val, maxlen, _("%i bytes unknown data"),
 			  entry->size);
		  break;
		}
		break;
	}

	return (val);
}

// libexif/fuji/exif-mnote-data-fuji.c

/* exif-mnote-data-fuji.c
 *
 * Copyright (c) 2002 Lutz Mueller <lutz@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details. 
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */





#define CHECKOVERFLOW(offset,datasize,structsize) (( (offset) >= (datasize)) || ((structsize) > (datasize)) || ((offset) > (datasize) - (structsize) ))

struct _MNoteFujiDataPrivate {
	ExifByteOrder order;
};

static void
exif_mnote_data_fuji_clear (ExifMnoteDataFuji *n)
{
	ExifMnoteData *d = (ExifMnoteData *) n;
	unsigned int i;

	if (!n) return;

	if (n->entries) {
		for (i = 0; i < n->count; i++)
			if (n->entries[i].data) {
				exif_mem_free (d->mem, n->entries[i].data);
				n->entries[i].data = NULL;
			}
		exif_mem_free (d->mem, n->entries);
		n->entries = NULL;
		n->count = 0;
	}
}

static void
exif_mnote_data_fuji_free (ExifMnoteData *n)
{
	if (!n) return;

	exif_mnote_data_fuji_clear ((ExifMnoteDataFuji *) n);
}

static char *
exif_mnote_data_fuji_get_value (ExifMnoteData *d, unsigned int i, char *val, unsigned int maxlen)
{
	ExifMnoteDataFuji *n = (ExifMnoteDataFuji *) d;

	if (!d || !val) return NULL;
	if (i >= n->count) return NULL;
/*
	exif_log (d->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataFuji",
		  "Querying value for tag '%s'...",
		  mnote_fuji_tag_get_name (n->entries[i].tag));
*/
	return mnote_fuji_entry_get_value (&n->entries[i], val, maxlen);
}

static void
exif_mnote_data_fuji_save (ExifMnoteData *ne, unsigned char **buf,
			   unsigned int *buf_size)
{
	ExifMnoteDataFuji *n = (ExifMnoteDataFuji *) ne;
	size_t i, o, s, doff;
	unsigned char *t;
	size_t ts;

	if (!n || !buf || !buf_size) return;

	/*
	 * Allocate enough memory for all entries and the number
	 * of entries.
	 */
	*buf_size = 8 + 4 + 2 + n->count * 12 + 4;
	*buf = exif_mem_alloc (ne->mem, *buf_size);
	if (!*buf) {
		*buf_size = 0;
		return;
	}

	/*
	 * Header: "FUJIFILM" and 4 bytes offset to the first entry.
	 * As the first entry will start right thereafter, the offset is 12.
	 */
	memcpy (*buf, "FUJIFILM", 8);
	exif_set_long (*buf + 8, n->order, 12);

	/* Save the number of entries */
	exif_set_short (*buf + 8 + 4, n->order, (ExifShort) n->count);
	
	/* Save each entry */
	for (i = 0; i < n->count; i++) {
		o = 8 + 4 + 2 + i * 12;
		exif_set_short (*buf + o + 0, n->order, (ExifShort) n->entries[i].tag);
		exif_set_short (*buf + o + 2, n->order, (ExifShort) n->entries[i].format);
		exif_set_long  (*buf + o + 4, n->order, n->entries[i].components);
		o += 8;
		s = exif_format_get_size (n->entries[i].format) *
						n->entries[i].components;
		if (s > 65536) {
			/* Corrupt data: EXIF data size is limited to the
			 * maximum size of a JPEG segment (64 kb).
			 */
			continue;
		}
		if (s > 4) {
			ts = *buf_size + s;

			/* Ensure even offsets. Set padding bytes to 0. */
			if (s & 1) ts += 1;
			t = exif_mem_realloc (ne->mem, *buf, ts);
			if (!t) {
				return;
			}
			*buf = t;
			*buf_size = ts;
			doff = *buf_size - s;
			if (s & 1) { doff--; *(*buf + *buf_size - 1) = '\0'; }
			exif_set_long (*buf + o, n->order, doff);
		} else
			doff = o;

		/*
		 * Write the data. Fill unneeded bytes with 0. Do not
		 * crash if data is NULL.
		 */
		if (!n->entries[i].data) memset (*buf + doff, 0, s);
		else memcpy (*buf + doff, n->entries[i].data, s);
	}
}

static void
exif_mnote_data_fuji_load (ExifMnoteData *en,
	const unsigned char *buf, unsigned int buf_size)
{
	ExifMnoteDataFuji *n = (ExifMnoteDataFuji*) en;
	ExifLong c;
	size_t i, tcount, o, datao;

	if (!n) return;

	if (!buf || !buf_size) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteDataFuji", "Short MakerNote");
		return;
	}
	if (CHECKOVERFLOW(n->offset, buf_size, 6+8+4)) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteDataFuji", "Short MakerNote");
		return;
	}
	datao = 6 + n->offset;

	n->order = EXIF_BYTE_ORDER_INTEL;

	datao += exif_get_long (buf + datao + 8, EXIF_BYTE_ORDER_INTEL);
	if (CHECKOVERFLOW(datao, buf_size, 2)) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteDataFuji", "Short MakerNote");
		return;
	}

	/* Read the number of tags */
	c = exif_get_short (buf + datao, EXIF_BYTE_ORDER_INTEL);
	datao += 2;

	/* Just use an arbitrary max tag limit here to avoid needing to much memory or time. There are 50 named tags currently.
	 * Fuji XT2 had 56 entries or so, GFX500 has 68. 150 seems a safe upper bound currently.
	 * The format allows specifying the same range of memory as often as it can, so this multiplies quickly. */
	if (c > 150) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA, "ExifMnoteDataFuji", "Too much tags (%d) in Fuji MakerNote", c);
		return;
	}

	/* Remove any old entries */
	exif_mnote_data_fuji_clear (n);

	/* Reserve enough space for all the possible MakerNote tags */
	n->entries = exif_mem_alloc (en->mem, sizeof (MnoteFujiEntry) * c);
	if (!n->entries) {
		EXIF_LOG_NO_MEMORY(en->log, "ExifMnoteDataFuji", sizeof (MnoteFujiEntry) * c);
		return;
	}

	/* Parse all c entries, storing ones that are successfully parsed */
	tcount = 0;
	for (i = c, o = datao; i; --i, o += 12) {
		size_t s;

		memset(&n->entries[tcount], 0, sizeof(MnoteFujiEntry));
		if (CHECKOVERFLOW(o, buf_size, 12)) {
			exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
				  "ExifMnoteDataFuji", "Short MakerNote");
			break;
		}

		n->entries[tcount].tag        = exif_get_short (buf + o, n->order);
		n->entries[tcount].format     = exif_get_short (buf + o + 2, n->order);
		n->entries[tcount].components = exif_get_long (buf + o + 4, n->order);
		n->entries[tcount].order      = n->order;

		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataFuji",
			  "Loading entry 0x%x ('%s')...", n->entries[tcount].tag,
			  mnote_fuji_tag_get_name (n->entries[tcount].tag));

		/* Check if we overflow the multiplication. Use buf_size as the max size for integer overflow detection,
		 * we will check the buffer sizes closer later. */
		if (	exif_format_get_size (n->entries[tcount].format) &&
			buf_size / exif_format_get_size (n->entries[tcount].format) < n->entries[tcount].components
		) {
			exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
					  "ExifMnoteDataFuji", "Tag size overflow detected (%u * %lu)", exif_format_get_size (n->entries[tcount].format), n->entries[tcount].components);
			continue;
		}
		/*
		 * Size? If bigger than 4 bytes, the actual data is not
		 * in the entry but somewhere else (offset).
		 */
		s = exif_format_get_size (n->entries[tcount].format) * n->entries[tcount].components;
		n->entries[tcount].size = s;
		if (s) {
			size_t dataofs = o + 8;
			if (s > 4)
				/* The data in this case is merely a pointer */
				dataofs = exif_get_long (buf + dataofs, n->order) + 6 + n->offset;

			if (CHECKOVERFLOW(dataofs, buf_size, s)) {
				exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
						  "ExifMnoteDataFuji", "Tag data past end of "
					  "buffer (%u >= %u)", (unsigned)(dataofs + s), buf_size);
				continue;
			}

			n->entries[tcount].data = exif_mem_alloc (en->mem, s);
			if (!n->entries[tcount].data) {
				EXIF_LOG_NO_MEMORY(en->log, "ExifMnoteDataFuji", s);
				continue;
			}
			memcpy (n->entries[tcount].data, buf + dataofs, s);
		}

		/* Tag was successfully parsed */
		++tcount;
	}
	/* Store the count of successfully parsed tags */
	n->count = tcount;
}

static unsigned int
exif_mnote_data_fuji_count (ExifMnoteData *n)
{
	return n ? ((ExifMnoteDataFuji *) n)->count : 0;
}

static unsigned int
exif_mnote_data_fuji_get_id (ExifMnoteData *d, unsigned int n)
{
	ExifMnoteDataFuji *note = (ExifMnoteDataFuji *) d;

	if (!note) return 0;
	if (note->count <= n) return 0;
	return note->entries[n].tag;
}

static const char *
exif_mnote_data_fuji_get_name (ExifMnoteData *d, unsigned int i)
{
	ExifMnoteDataFuji *n = (ExifMnoteDataFuji *) d;

	if (!n) return NULL;
	if (i >= n->count) return NULL;
	return mnote_fuji_tag_get_name (n->entries[i].tag);
}

static const char *
exif_mnote_data_fuji_get_title (ExifMnoteData *d, unsigned int i)
{
	ExifMnoteDataFuji *n = (ExifMnoteDataFuji *) d;
	
	if (!n) return NULL;
	if (i >= n->count) return NULL;
        return mnote_fuji_tag_get_title (n->entries[i].tag);
}

static const char *
exif_mnote_data_fuji_get_description (ExifMnoteData *d, unsigned int i)
{
	ExifMnoteDataFuji *n = (ExifMnoteDataFuji *) d;
	
	if (!n) return NULL;
	if (i >= n->count) return NULL;
        return mnote_fuji_tag_get_description (n->entries[i].tag);
}

static void
exif_mnote_data_fuji_set_byte_order (ExifMnoteData *d, ExifByteOrder o)
{
	ExifByteOrder o_orig;
	ExifMnoteDataFuji *n = (ExifMnoteDataFuji *) d;
	unsigned int i;

	if (!n) return;

	o_orig = n->order;
	n->order = o;
	for (i = 0; i < n->count; i++) {
		if (n->entries[i].components && (n->entries[i].size/n->entries[i].components < exif_format_get_size (n->entries[i].format)))
			continue;
		n->entries[i].order = o;
		exif_array_set_byte_order (n->entries[i].format, n->entries[i].data,
				n->entries[i].components, o_orig, o);
	}
}

static void
exif_mnote_data_fuji_set_offset (ExifMnoteData *n, unsigned int o)
{
	if (n) ((ExifMnoteDataFuji *) n)->offset = o;
}

int
exif_mnote_data_fuji_identify (const ExifData *ed, const ExifEntry *e)
{
	(void) ed;  /* unused */
	return ((e->size >= 12) && !memcmp (e->data, "FUJIFILM", 8));
}

ExifMnoteData *
exif_mnote_data_fuji_new (ExifMem *mem)
{
	ExifMnoteData *d;

	if (!mem) return NULL;

	d = exif_mem_alloc (mem, sizeof (ExifMnoteDataFuji));
	if (!d) return NULL;

	exif_mnote_data_construct (d, mem);

	/* Set up function pointers */
	d->methods.free            = exif_mnote_data_fuji_free;
	d->methods.set_byte_order  = exif_mnote_data_fuji_set_byte_order;
	d->methods.set_offset      = exif_mnote_data_fuji_set_offset;
	d->methods.load            = exif_mnote_data_fuji_load;
	d->methods.save            = exif_mnote_data_fuji_save;
	d->methods.count           = exif_mnote_data_fuji_count;
	d->methods.get_id          = exif_mnote_data_fuji_get_id;
	d->methods.get_name        = exif_mnote_data_fuji_get_name;
	d->methods.get_title       = exif_mnote_data_fuji_get_title;
	d->methods.get_description = exif_mnote_data_fuji_get_description;
	d->methods.get_value       = exif_mnote_data_fuji_get_value;

	return d;
}

}
