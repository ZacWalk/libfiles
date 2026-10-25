// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif (libexif/pentax): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.

// Purpose: libexif's pentax maker note decoder: how it recognises the note, reads its entries and
// names and renders each value.

#include "libexif_makernote.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4996) // strcpy, strncpy and strncat, as libexif calls them
#pragma warning(disable : 4244 4267 4100 4189 4456 4457 4459 4702) // and its implicit narrowing and leftovers
#endif

namespace libfiles::detail::libexif
{
// libexif/pentax/mnote-pentax-tag.h

/* mnote-pentax-tag.h
 *
 * Copyright (c) 2002, 2003 Lutz Mueller <lutz@users.sourceforge.net>
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



/*
 * Missing features which are probably in the unknowns somewhere ...
 * 1/ AF Area (Wide, Spot, Free)
 * 2/ AE Metering (Multi segment, Centre-weighted, Spot)
 * 3/ 
 */

typedef unsigned int _MnotePentaxTag;
enum : unsigned int {
	MNOTE_PENTAX_TAG_MODE		= 0x0001,
	MNOTE_PENTAX_TAG_QUALITY	= 0x0002,
	MNOTE_PENTAX_TAG_FOCUS		= 0x0003,
	MNOTE_PENTAX_TAG_FLASH		= 0x0004,
	MNOTE_PENTAX_TAG_UNKNOWN_05	= 0x0005,
	MNOTE_PENTAX_TAG_UNKNOWN_06	= 0x0006,
	MNOTE_PENTAX_TAG_WHITE_BALANCE	= 0x0007,
	MNOTE_PENTAX_TAG_UNKNOWN_08	= 0x0008,
	MNOTE_PENTAX_TAG_UNKNOWN_09	= 0x0009,
	MNOTE_PENTAX_TAG_ZOOM		= 0x000a,
	MNOTE_PENTAX_TAG_SHARPNESS	= 0x000b,
	MNOTE_PENTAX_TAG_CONTRAST	= 0x000c,
	MNOTE_PENTAX_TAG_SATURATION	= 0x000d,
	MNOTE_PENTAX_TAG_UNKNOWN_14	= 0x000e,
	MNOTE_PENTAX_TAG_UNKNOWN_15	= 0x000f,
	MNOTE_PENTAX_TAG_UNKNOWN_16	= 0x0010,
	MNOTE_PENTAX_TAG_UNKNOWN_17	= 0x0011,
	MNOTE_PENTAX_TAG_UNKNOWN_18	= 0x0012,
	MNOTE_PENTAX_TAG_UNKNOWN_19	= 0x0013,
	MNOTE_PENTAX_TAG_ISO_SPEED	= 0x0014,
	MNOTE_PENTAX_TAG_UNKNOWN_21	= 0x0015,
	MNOTE_PENTAX_TAG_COLOR		= 0x0017,
	MNOTE_PENTAX_TAG_UNKNOWN_24	= 0x0018,
	MNOTE_PENTAX_TAG_UNKNOWN_25	= 0x0019,
	MNOTE_PENTAX_TAG_PRINTIM	= 0x0e00,
	MNOTE_PENTAX_TAG_TZ_CITY	= 0x1000,
	MNOTE_PENTAX_TAG_TZ_DST		= 0x1001,

	/* Pentax v2, v3: real values + our proprietary base to distinguish from v1 */
	MNOTE_PENTAX2_TAG_BASE              = 0x4000,
	MNOTE_PENTAX2_TAG_MODE		    = 0x0001 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_PREVIEW_SIZE      = 0x0002 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_PREVIEW_LENGTH    = 0x0003 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_PREVIEW_START     = 0x0004 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_MODEL_ID          = 0x0005 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_DATE              = 0x0006 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_TIME              = 0x0007 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_QUALITY           = 0x0008 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_IMAGE_SIZE        = 0x0009 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_PICTURE_MODE      = 0x000b + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FLASH_MODE        = 0x000c + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FOCUS_MODE        = 0x000d + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_AFPOINT_SELECTED  = 0x000e + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_AUTO_AFPOINT      = 0x000f + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FOCUS_POSITION    = 0x0010 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_EXPOSURE_TIME     = 0x0012 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FNUMBER           = 0x0013 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_ISO               = 0x0014 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_EXPOSURE_COMPENSATION = 0x0016 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_METERING_MODE     = 0x0017 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_AUTO_BRACKETING   = 0x0018 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_WHITE_BALANCE     = 0x0019 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_WHITE_BALANCE_MODE= 0x001a + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_BLUE_BALANCE      = 0x001b + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_RED_BALANCE       = 0x001c + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FOCAL_LENGTH      = 0x001d + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_DIGITAL_ZOOM      = 0x001e + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_SATURATION        = 0x001f + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_CONTRAST          = 0x0020 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_SHARPNESS         = 0x0021 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_WORLDTIME_LOCATION = 0x0022 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_HOMETOWN_CITY     = 0x0023 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_DESTINATION_CITY  = 0x0024 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_HOMETOWN_DST      = 0x0025 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_DESTINATION_DST   = 0x0026 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FRAME_NUMBER      = 0x0029 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_IMAGE_PROCESSING  = 0x0032 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_PICTURE_MODE2     = 0x0033 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_DRIVE_MODE        = 0x0034 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_COLOR_SPACE       = 0x0037 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_IMAGE_AREA_OFFSET = 0x0038 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_RAW_IMAGE_SIZE    = 0x0039 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_AFPOINTS_USED     = 0x003c + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_LENS_TYPE         = 0x003f + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_CAMERA_TEMPERATURE = 0x0047 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_NOISE_REDUCTION   = 0x0049 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FLASH_EXPOSURE_COMP = 0x004d + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_IMAGE_TONE        = 0x004f + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_SHAKE_REDUCTION_INFO = 0x005c + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_BLACK_POINT       = 0x0200 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_WHITE_POINT       = 0x0201 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_AE_INFO           = 0x0206 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_LENS_INFO         = 0x0207 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_FLASH_INFO        = 0x0208 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_CAMERA_INFO       = 0x0215 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_BATTERY_INFO      = 0x0216 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_HOMETOWN_CITY_CODE = 0x1000 + MNOTE_PENTAX2_TAG_BASE,
	MNOTE_PENTAX2_TAG_DESTINATION_CITY_CODE = 0x1001 + MNOTE_PENTAX2_TAG_BASE,

	/* Casio v2: some Casio v2 tags match Pentax v2 tags */
	MNOTE_CASIO2_TAG_BASE               = MNOTE_PENTAX2_TAG_BASE,
	MNOTE_CASIO2_TAG_PREVIEW_START      = 0x2000 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_WHITE_BALANCE_BIAS = 0x2011 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_WHITE_BALANCE      = 0x2012 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_OBJECT_DISTANCE    = 0x2022 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_FLASH_DISTANCE     = 0x2034 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_RECORD_MODE        = 0x3000 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_SELF_TIMER         = 0x3001 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_QUALITY            = 0x3002 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_FOCUS_MODE         = 0x3003 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_TIME_ZONE          = 0x3006 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_BESTSHOT_MODE      = 0x3007 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_CCS_ISO_SENSITIVITY = 0x3014 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_COLOR_MODE         = 0x3015 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_ENHANCEMENT        = 0x3016 + MNOTE_CASIO2_TAG_BASE,
	MNOTE_CASIO2_TAG_FINER              = 0x3017 + MNOTE_CASIO2_TAG_BASE
};
typedef unsigned int MnotePentaxTag;

const char *mnote_pentax_tag_get_name        (MnotePentaxTag tag);
const char *mnote_pentax_tag_get_title       (MnotePentaxTag tag);
const char *mnote_pentax_tag_get_description (MnotePentaxTag tag);

// libexif/pentax/mnote-pentax-entry.h

/* mnote-pentax-entry.h
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



typedef struct _MnotePentaxEntry        MnotePentaxEntry;

struct _MnotePentaxEntry {
	MnotePentaxTag tag;
	ExifFormat format;
	unsigned long components;

	unsigned char *data;
	unsigned int size;

	ExifByteOrder order;
};

char *mnote_pentax_entry_get_value (MnotePentaxEntry *entry, char *val, unsigned int maxlen);

// libexif/pentax/exif-mnote-data-pentax.h

/* exif-mnote-data-pentax.h
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



typedef unsigned int PentaxVersion;
enum : unsigned int {pentaxV1 = 1, pentaxV2 = 2, pentaxV3 = 3, casioV2 = 4 };

typedef struct _ExifMnoteDataPentax ExifMnoteDataPentax;

struct _ExifMnoteDataPentax {
	ExifMnoteData parent;

	MnotePentaxEntry *entries;
	unsigned int count;

	ExifByteOrder order;
	unsigned int offset;

	PentaxVersion version;
};

/*! Detect if MakerNote is recognized as one handled by the Pentax module.
 * 
 * \param[in] ed image #ExifData to identify as as a Pentax type
 * \param[in] e #ExifEntry for EXIF_TAG_MAKER_NOTE, from within ed but
 *   duplicated here for convenience
 * \return 0 if not recognized, nonzero if recognized. The specific nonzero 
 *   value returned may identify a subtype unique within this module.
 */
int exif_mnote_data_pentax_identify (const ExifData *ed, const ExifEntry *e);

ExifMnoteData *exif_mnote_data_pentax_new (ExifMem *);

// libexif/pentax/mnote-pentax-tag.c

/* mnote-pentax-tag.c:
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
	MnotePentaxTag tag;
	const char *name;
	const char *title;
	const char *description;
} table[] = {
#ifndef NO_VERBOSE_TAG_STRINGS
	{MNOTE_PENTAX_TAG_MODE, "Mode", N_("Capture Mode"), ""},
	{MNOTE_PENTAX_TAG_QUALITY, "Quality", N_("Quality Level"), ""},
	{MNOTE_PENTAX_TAG_FOCUS, "Focus", N_("Focus Mode"), ""},
	{MNOTE_PENTAX_TAG_FLASH, "Flash", N_("Flash Mode"), ""},
	{MNOTE_PENTAX_TAG_UNKNOWN_05, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_06, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_WHITE_BALANCE, "WhiteBalance", N_("White Balance"), ""},
	{MNOTE_PENTAX_TAG_UNKNOWN_08, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_09, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_ZOOM, "Zoom", N_("Zoom"), NULL},
	{MNOTE_PENTAX_TAG_SHARPNESS, "Sharpness", N_("Sharpness"), ""},
	{MNOTE_PENTAX_TAG_CONTRAST, "Contrast", N_("Contrast"), ""},
	{MNOTE_PENTAX_TAG_SATURATION, "Saturation", N_("Saturation"), ""},
	{MNOTE_PENTAX_TAG_UNKNOWN_14, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_15, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_16, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_17, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_18, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_19, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_ISO_SPEED, "ISOSpeed", N_("ISO Speed"), ""},
	{MNOTE_PENTAX_TAG_UNKNOWN_21, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_COLOR, "Color", N_("Colors"), ""},
	{MNOTE_PENTAX_TAG_UNKNOWN_24, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_UNKNOWN_25, NULL, NULL, NULL},
	{MNOTE_PENTAX_TAG_PRINTIM, "PrintIM", N_("PrintIM Settings"), ""},
	{MNOTE_PENTAX_TAG_TZ_CITY, "TimeZone", N_("Time Zone"), ""},
	{MNOTE_PENTAX_TAG_TZ_DST, "DaylightSavings", N_("Daylight Savings"), ""},

	{MNOTE_PENTAX2_TAG_MODE, "Mode", N_("Capture Mode"), ""},
	{MNOTE_PENTAX2_TAG_PREVIEW_SIZE, "PentaxPreviewSize", N_("Preview Size"), ""},
	{MNOTE_PENTAX2_TAG_PREVIEW_LENGTH, "PentaxPreviewLength", N_("Preview Length"), ""},
	{MNOTE_PENTAX2_TAG_PREVIEW_START, "PentaxPreviewStart", N_("Preview Start"), ""},
	{MNOTE_PENTAX2_TAG_MODEL_ID, "ModelID", N_("Model Identification"), ""},
	{MNOTE_PENTAX2_TAG_DATE, "Date", N_("Date"), ""},
	{MNOTE_PENTAX2_TAG_TIME, "Time", N_("Time"), ""},
	{MNOTE_PENTAX2_TAG_QUALITY, "Quality", N_("Quality Level"), ""},
	{MNOTE_PENTAX2_TAG_IMAGE_SIZE, "ImageSize", N_("Image Size"), ""},
	{MNOTE_PENTAX2_TAG_PICTURE_MODE, "PictureMode", N_("Picture Mode"), ""},
	{MNOTE_PENTAX2_TAG_FLASH_MODE, "FlashMode", N_("Flash Mode"), ""},
	{MNOTE_PENTAX2_TAG_FOCUS_MODE, "FocusMode", N_("Focus Mode"), ""},
	{MNOTE_PENTAX2_TAG_AFPOINT_SELECTED, "AFPointSelected", N_("AF Point Selected"), ""},
	{MNOTE_PENTAX2_TAG_AUTO_AFPOINT, "AutoAFPoint", N_("Auto AF Point"), ""},
	{MNOTE_PENTAX2_TAG_FOCUS_POSITION, "FocusPosition", N_("Focus Position"), ""},
	{MNOTE_PENTAX2_TAG_EXPOSURE_TIME, "ExposureTime", N_("Exposure Time"), ""},
	{MNOTE_PENTAX2_TAG_FNUMBER, "FNumber", N_("F-Number"), ""},
	{MNOTE_PENTAX2_TAG_ISO, "ISO", N_("ISO Number"), ""},
	{MNOTE_PENTAX2_TAG_EXPOSURE_COMPENSATION, "ExposureCompensation", N_("Exposure Compensation"), ""},
	{MNOTE_PENTAX2_TAG_METERING_MODE, "MeteringMode", N_("Metering Mode"), ""},
	{MNOTE_PENTAX2_TAG_AUTO_BRACKETING, "AutoBracketing", N_("Auto Bracketing"), ""},
	{MNOTE_PENTAX2_TAG_WHITE_BALANCE, "WhiteBalance", N_("White Balance"), ""},
	{MNOTE_PENTAX2_TAG_WHITE_BALANCE_MODE, "WhiteBalanceMode", N_("White Balance Mode"), ""},
	{MNOTE_PENTAX2_TAG_BLUE_BALANCE, "BlueBalance", N_("Blue Balance"), ""},
	{MNOTE_PENTAX2_TAG_RED_BALANCE, "RedBalance", N_("Red Balance"), ""},
	{MNOTE_PENTAX2_TAG_FOCAL_LENGTH, "FocalLength", N_("Focal Length"), ""},
	{MNOTE_PENTAX2_TAG_DIGITAL_ZOOM, "DigitalZoom", N_("Digital Zoom"), ""},
	{MNOTE_PENTAX2_TAG_SATURATION, "Saturation", N_("Saturation"), ""},
	{MNOTE_PENTAX2_TAG_CONTRAST, "Contrast", N_("Contrast"), ""},
	{MNOTE_PENTAX2_TAG_SHARPNESS, "Sharpness", N_("Sharpness"), ""},
	{MNOTE_PENTAX2_TAG_WORLDTIME_LOCATION, "WorldTimeLocation", N_("World Time Location"), ""},
	{MNOTE_PENTAX2_TAG_HOMETOWN_CITY, "HometownCity", N_("Hometown City"), ""},
	{MNOTE_PENTAX2_TAG_DESTINATION_CITY, "DestinationCity", N_("Destination City"), ""},
	{MNOTE_PENTAX2_TAG_HOMETOWN_DST, "HometownDST,", N_("Hometown DST"), N_("Home Daylight Savings Time")},
	{MNOTE_PENTAX2_TAG_DESTINATION_DST, "DestinationDST", N_("Destination DST"), N_("Destination Daylight Savings Time")},
	{MNOTE_PENTAX2_TAG_FRAME_NUMBER, "FrameNumber", N_("Frame Number"), ""},
	{MNOTE_PENTAX2_TAG_IMAGE_PROCESSING, "ImageProcessing", N_("Image Processing"), ""},
	{MNOTE_PENTAX2_TAG_PICTURE_MODE2, "PictureMode2", N_("Picture Mode (2)"), ""},
	{MNOTE_PENTAX2_TAG_DRIVE_MODE, "DriveMode", N_("Drive Mode"), ""},
	{MNOTE_PENTAX2_TAG_COLOR_SPACE, "ColorSpace", N_("Color Space"), ""},
	{MNOTE_PENTAX2_TAG_IMAGE_AREA_OFFSET, "ImageAreaOffset", N_("Image Area Offset"), ""},
	{MNOTE_PENTAX2_TAG_RAW_IMAGE_SIZE, "RawImageSize", N_("Raw Image Size"), ""},
	{MNOTE_PENTAX2_TAG_AFPOINTS_USED, "AfPointsUsed,", N_("Autofocus Points Used"), ""},
	{MNOTE_PENTAX2_TAG_LENS_TYPE, "LensType", N_("Lens Type"), ""},
	{MNOTE_PENTAX2_TAG_CAMERA_TEMPERATURE, "CameraTemperature", N_("Camera Temperature"), ""},
	{MNOTE_PENTAX2_TAG_NOISE_REDUCTION, "NoiseReduction", N_("Noise Reduction"), ""},
	{MNOTE_PENTAX2_TAG_FLASH_EXPOSURE_COMP, "FlashExposureComp", N_("Flash Exposure Compensation"), ""},
	{MNOTE_PENTAX2_TAG_IMAGE_TONE, "ImageTone", N_("Image Tone"), ""},
	{MNOTE_PENTAX2_TAG_SHAKE_REDUCTION_INFO, "ShakeReductionInfo,", N_("Shake Reduction Info"), ""},
	{MNOTE_PENTAX2_TAG_BLACK_POINT, "BlackPoint", N_("Black Point"), ""},
	{MNOTE_PENTAX2_TAG_WHITE_POINT, "WhitePoint", N_("White Point"), ""},
	{MNOTE_PENTAX2_TAG_AE_INFO, "AEInfo", N_("AE Info"), ""},
	{MNOTE_PENTAX2_TAG_LENS_INFO, "LensInfo", N_("Lens Info"), ""},
	{MNOTE_PENTAX2_TAG_FLASH_INFO, "FlashInfo", N_("Flash Info"), ""},
	{MNOTE_PENTAX2_TAG_CAMERA_INFO, "CameraInfo", N_("Camera Info"), ""},
	{MNOTE_PENTAX2_TAG_BATTERY_INFO, "BatteryInfo", N_("Battery Info"), ""},
	{MNOTE_PENTAX2_TAG_HOMETOWN_CITY_CODE, "HometownCityCode", N_("Hometown City Code"), ""},
	{MNOTE_PENTAX2_TAG_DESTINATION_CITY_CODE, "DestinationCityCode", N_("Destination City Code"), ""},

	{MNOTE_CASIO2_TAG_PREVIEW_START, "CasioPreviewStart", N_("Preview Start"), ""},
	{MNOTE_CASIO2_TAG_WHITE_BALANCE_BIAS, "WhiteBalanceBias", N_("White Balance Bias"), ""},
	{MNOTE_CASIO2_TAG_WHITE_BALANCE, "WhiteBalance", N_("White Balance"), ""},
	{MNOTE_CASIO2_TAG_OBJECT_DISTANCE, "ObjectDistance", N_("Object Distance"), N_("Distance of photographed object in millimeters.")},
	{MNOTE_CASIO2_TAG_FLASH_DISTANCE, "FlashDistance", N_("Flash Distance"), ""},
	{MNOTE_CASIO2_TAG_RECORD_MODE, "RecordMode", N_("Record Mode"), ""},
	{MNOTE_CASIO2_TAG_SELF_TIMER, "SelfTimer", N_("Self-timer"), ""},
	{MNOTE_CASIO2_TAG_QUALITY, "CasioQuality", N_("Quality Level"), ""},
	{MNOTE_CASIO2_TAG_FOCUS_MODE, "CasioFocusMode", N_("Focus Mode"), ""},
	{MNOTE_CASIO2_TAG_TIME_ZONE, "TimeZone", N_("Time Zone"), ""},
	{MNOTE_CASIO2_TAG_BESTSHOT_MODE, "BestshotMode", N_("Bestshot Mode"), ""},
	{MNOTE_CASIO2_TAG_CCS_ISO_SENSITIVITY, "CCSISOSensitivity", N_("CCS ISO Sensitivity"), ""},
	{MNOTE_CASIO2_TAG_COLOR_MODE, "ColorMode", N_("Color Mode"), ""},
	{MNOTE_CASIO2_TAG_ENHANCEMENT, "Enhancement", N_("Enhancement"), ""},
	{MNOTE_CASIO2_TAG_FINER, "Finer", N_("Finer"), ""},
#endif
	{0, NULL, NULL, NULL}
};

const char *
mnote_pentax_tag_get_name (MnotePentaxTag t)
{
	unsigned int i;

	for (i = 0; i < sizeof (table) / sizeof (table[0]); i++)
		if (table[i].tag == t) return (table[i].name);
	return NULL;
}

const char *
mnote_pentax_tag_get_title (MnotePentaxTag t)
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
mnote_pentax_tag_get_description (MnotePentaxTag t)
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

// libexif/pentax/mnote-pentax-entry.c

/* mnote-pentax-entry.c
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

#define CC2(number,t1,t2,v,maxlen)                                      \
{                                                                       \
	if ((number != t1) && (number != t2)) {                         \
		snprintf (v, maxlen,                                    \
			_("Invalid number of components (%i, "          \
			"expected %i or %i)."), (int) number,		\
			(int) t1, (int) t2);  				\
		break;                                                  \
	}                                                               \
}

static const struct {
	MnotePentaxTag tag;
	struct {
		int index;
		const char *string;
	} elem[33];
} items[] = {
#ifndef NO_VERBOSE_TAG_DATA
  { MNOTE_PENTAX_TAG_MODE,
    { {0, N_("Auto")},
      {1, N_("Night scene")},
      {2, N_("Manual")},
      {4, N_("Multi-exposure")},
      {0, NULL}}},
  { MNOTE_PENTAX_TAG_QUALITY,
    { {0, N_("Good")},
      {1, N_("Better")},
      {2, N_("Best")},{0,NULL}}},
  { MNOTE_PENTAX_TAG_FOCUS,
    { {2, N_("Custom")},
      {3, N_("Auto")},
      {0, NULL}}},
  { MNOTE_PENTAX_TAG_FLASH,
    { {1, N_("Auto")},
      {2, N_("Flash on")},
      {4, N_("Flash off")},
      {6, N_("Red-eye reduction")},
      {0, NULL}}},
  { MNOTE_PENTAX_TAG_WHITE_BALANCE,
    { {0, N_("Auto")},
      {1, N_("Daylight")},
      {2, N_("Shade")},
      {3, N_("Tungsten")},
      {4, N_("Fluorescent")},
      {5, N_("Manual")},
      {0, NULL}}},
  { MNOTE_PENTAX_TAG_SHARPNESS,
    { {0, N_("Normal")},
      {1, N_("Soft")},
      {2, N_("Hard")},
      {0, NULL}}},
  { MNOTE_PENTAX_TAG_CONTRAST,
    { {0, N_("Normal")},
      {1, N_("Low")},
      {2, N_("High")},
      {0, NULL}}},
  { MNOTE_PENTAX_TAG_SATURATION,
    { {0, N_("Normal")},
      {1, N_("Low")},
      {2, N_("High")},
      {0, NULL}}},
  { MNOTE_PENTAX_TAG_ISO_SPEED,
    { {10,  N_("100")},
      {16,  N_("200")},
      {100, N_("100")},
      {200, N_("200")},
      { 0,  NULL}}},
  { MNOTE_PENTAX_TAG_COLOR,
    { {1, N_("Full")},
      {2, N_("Black & white")},
      {3, N_("Sepia")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_MODE,
    { {0, N_("Auto")},
      {1, N_("Night scene")},
      {2, N_("Manual")},
      {4, N_("Multi-exposure")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_QUALITY,
    { {0, N_("Good")},
      {1, N_("Better")},
      {2, N_("Best")},
      {3, N_("TIFF")},
      {4, N_("RAW")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_IMAGE_SIZE,
    { {0, "640x480"},
      {1, N_("Full")},
      {2, "1024x768"},
      {3, "1280x960"},
      {4, "1600x1200"},
      {5, "2048x1536"},
      {8, N_("2560x1920 or 2304x1728")},
      {9, "3072x2304"},
      {10, "3264x2448"},
      {19, "320x240"},
      {20, "2288x1712"},
      {21, "2592x1944"},
      {22, N_("2304x1728 or 2592x1944")},
      {23, "3056x2296"},
      {25, N_("2816x2212 or 2816x2112")},
      {27, "3648x2736"},
      {36, "3008x2008"},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_PICTURE_MODE,
    { {0, N_("Program")},
      {2, N_("Program AE")},
      {3, N_("Manual")},
      {5, N_("Portrait")},
      {6, N_("Landscape")},
      {8, N_("Sport")},
      {9, N_("Night scene")},
      {11, N_("Soft")},
      {12, N_("Surf & snow")},
      {13, N_("Sunset or candlelight")},
      {14, N_("Autumn")},
      {15, N_("Macro")},
      {17, N_("Fireworks")},
      {18, N_("Text")},
      {19, N_("Panorama")},
      {30, N_("Self portrait")},
      {31, N_("Illustrations")},
      {33, N_("Digital filter")},
      {37, N_("Museum")},
      {38, N_("Food")},
      {40, N_("Green mode")},
      {49, N_("Light pet")},
      {50, N_("Dark pet")},
      {51, N_("Medium pet")},
      {53, N_("Underwater")},
      {54, N_("Candlelight")},
      {55, N_("Natural skin tone")},
      {56, N_("Synchro sound record")},
      {58, N_("Frame composite")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_FLASH_MODE,
    { {0x0000, N_("Auto, did not fire")},
      {0x0001, N_("Off")},
      {0x0003, N_("Auto, did not fire, red-eye reduction")},
      {0x0100, N_("Auto, fired")},
      {0x0102, N_("On")},
      {0x0103, N_("Auto, fired, red-eye reduction")},
      {0x0104, N_("On, red-eye reduction")},
      {0x0105, N_("On, wireless")},
      {0x0108, N_("On, soft")},
      {0x0109, N_("On, slow-sync")},
      {0x010a, N_("On, slow-sync, red-eye reduction")},
      {0x010b, N_("On, trailing-curtain sync")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_FOCUS_MODE,
    { {0, N_("Normal")},
      {1, N_("Macro")},
      {2, N_("Infinity")},
      {3, N_("Manual")},
      {5, N_("Pan focus")},
      {16, N_("AF-S")},
      {17, N_("AF-C")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_AFPOINT_SELECTED,
    { {1, N_("Upper-left")},
      {2, N_("Top")},
      {3, N_("Upper-right")},
      {4, N_("Left")},
      {5, N_("Mid-left")},
      {6, N_("Center")},
      {7, N_("Mid-right")},
      {8, N_("Right")},
      {9, N_("Lower-left")},
      {10, N_("Bottom")},
      {11, N_("Lower-right")},
      {0xfffe, N_("Fixed center")},
      {0xffff, N_("Auto")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_AUTO_AFPOINT,
    { {0, N_("Multiple")},
      {1, N_("Top-left")},
      {2, N_("Top-center")},
      {3, N_("Top-right")},
      {4, N_("Left")},
      {5, N_("Center")},
      {6, N_("Right")},
      {7, N_("Bottom-left")},
      {8, N_("Bottom-center")},
      {9, N_("Bottom-right")},
      {0xffff, N_("None")},
      {0, NULL}}},
  { MNOTE_PENTAX2_TAG_WHITE_BALANCE,
    { {0, N_("Auto")},
      {1, N_("Daylight")},
      {2, N_("Shade")},
      {3, N_("Fluorescent")},
      {4, N_("Tungsten")},
      {5, N_("Manual")},
      {6, N_("Daylight fluorescent")},
      {7, N_("Day white fluorescent")},
      {8, N_("White fluorescent")},
      {9, N_("Flash")},
      {10, N_("Cloudy")},
      {0xfffe, N_("Unknown")},
      {0xffff, N_("User selected")},
      {0, NULL}}},
  {MNOTE_CASIO2_TAG_BESTSHOT_MODE, 
    { {0, N_("Off")},
      {1, N_("On")},
      {0, NULL}}},
#endif
  {0, {{0, NULL}}}
};

/* Two-component values */
static const struct {
	MnotePentaxTag tag;
	struct {
		int index1, index2;
		const char *string;
	} elem[39];
} items2[] = {
#ifndef NO_VERBOSE_TAG_DATA
  { MNOTE_PENTAX2_TAG_IMAGE_SIZE,
    { {0, 0, "2304x1728"},
      {4, 0, "1600x1200"},
      {5, 0, "2048x1536"},
      {8, 0, "2560x1920"},
      {34, 0, "1536x1024"},
      {36, 0, N_("3008x2008 or 3040x2024")},
      {37, 0, "3008x2000"},
      {35, 1, "2400x1600"},
      {32, 2, "960x480"},
      {33, 2, "1152x768"},
      {34, 2, "1536x1024"},
      {0,  0, NULL}}},
  { MNOTE_PENTAX2_TAG_PICTURE_MODE,
    { {0,   0, N_("Auto")},
      {5,   0, N_("Portrait")},
      {53,  0, N_("Underwater")},
      {255, 0, N_("Digital filter?")},
      {5,   1, N_("Portrait")},
      {9,   1, N_("Night scene")},
      {13,  1, N_("Candlelight")},
      {15,  1, N_("Macro")},
      {53,  1, N_("Underwater")},
      {0,   2, N_("Program AE")},
      {5,   2, N_("Portrait")},
      {6,   2, N_("Landscape")},
      {0,   0, NULL}}},
#endif
  {0, {{0, 0, NULL}}}
};

char *
mnote_pentax_entry_get_value (MnotePentaxEntry *entry,
			      char *val, unsigned int maxlen)
{
	ExifLong vl;
	ExifSLong vsl;
	ExifShort vs, vs2;
	ExifSShort vss;
	int i = 0, j = 0;

	if (!entry) return (NULL);
	if (maxlen < 1) return (NULL);

	memset (val, 0, maxlen);
	maxlen--;

	switch (entry->tag) {
	  case MNOTE_PENTAX_TAG_MODE:
	  case MNOTE_PENTAX_TAG_QUALITY:
	  case MNOTE_PENTAX_TAG_FOCUS:
	  case MNOTE_PENTAX_TAG_FLASH:
	  case MNOTE_PENTAX_TAG_WHITE_BALANCE:
	  case MNOTE_PENTAX_TAG_SHARPNESS:
	  case MNOTE_PENTAX_TAG_CONTRAST:
	  case MNOTE_PENTAX_TAG_SATURATION:
	  case MNOTE_PENTAX_TAG_ISO_SPEED:
	  case MNOTE_PENTAX_TAG_COLOR:
	  case MNOTE_PENTAX2_TAG_MODE:
	  case MNOTE_PENTAX2_TAG_QUALITY:
	  case MNOTE_PENTAX2_TAG_FLASH_MODE:
	  case MNOTE_PENTAX2_TAG_FOCUS_MODE:
	  case MNOTE_PENTAX2_TAG_AFPOINT_SELECTED:
	  case MNOTE_PENTAX2_TAG_AUTO_AFPOINT:
	  case MNOTE_PENTAX2_TAG_WHITE_BALANCE:
	  case MNOTE_PENTAX2_TAG_PICTURE_MODE:
	  case MNOTE_PENTAX2_TAG_IMAGE_SIZE:
	  case MNOTE_CASIO2_TAG_BESTSHOT_MODE:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		CC2 (entry->components, 1, 2, val, maxlen)
		if (entry->components == 1) {
			vs = exif_get_short (entry->data, entry->order);

			/* search the tag */
			for (i = 0; (items[i].tag && items[i].tag != entry->tag); i++);
			if (!items[i].tag) {
				snprintf (val, maxlen,
					  _("Internal error (unknown value %hu)"), vs);
			  	break;
			}

			/* find the value */
			for (j = 0; items[i].elem[j].string &&
			    (items[i].elem[j].index < vs); j++);
			if (items[i].elem[j].index != vs) {
				snprintf (val, maxlen,
					  _("Internal error (unknown value %hu)"), vs);
				break;
			}
			strncpy (val, _(items[i].elem[j].string), maxlen);
		} else {
			/* Two-component values */
			CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
			CC2 (entry->components, 1, 2, val, maxlen)
			vs = exif_get_short (entry->data, entry->order);
			vs2 = ((unsigned int)exif_get_short (entry->data+2, entry->order)) << 16;

			/* search the tag */
			for (i = 0; (items2[i].tag && items2[i].tag != entry->tag); i++);
			if (!items2[i].tag) {
				snprintf (val, maxlen,
					  _("Internal error (unknown value %hu %hu)"), vs, vs2);
			  	break;
			}

			/* find the value */
			for (j = 0; items2[i].elem[j].string && ((items2[i].elem[j].index2 < vs2)
				|| ((items2[i].elem[j].index2 == vs2) && (items2[i].elem[j].index1 < vs))); j++);
			if ((items2[i].elem[j].index1 != vs) || (items2[i].elem[j].index2 != vs2)) {
				snprintf (val, maxlen,
					  _("Internal error (unknown value %hi %hi)"), vs, vs2);
				break;
			}
			strncpy (val, _(items2[i].elem[j].string), maxlen);
		}
		break;

	case MNOTE_PENTAX_TAG_ZOOM:
		CF (entry->format, EXIF_FORMAT_LONG, val, maxlen)
		CC (entry->components, 1, val, maxlen)
		vl = exif_get_long (entry->data, entry->order);
		snprintf (val, maxlen, "%lu", (long unsigned) vl);
		break;
	case MNOTE_PENTAX_TAG_PRINTIM:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, val, maxlen)
		CC (entry->components, 124, val, maxlen)
		snprintf (val, maxlen, _("%i bytes unknown data"),
			entry->size);
		break;
	case MNOTE_PENTAX_TAG_TZ_CITY:
	case MNOTE_PENTAX_TAG_TZ_DST:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, val, maxlen)
		CC (entry->components, 4, val, maxlen)
		strncpy (val, (char*)entry->data, MIN(maxlen, entry->size));
		break;
	case MNOTE_PENTAX2_TAG_DATE:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, val, maxlen)
		CC (entry->components, 4, val, maxlen)
		/* Note: format is UNDEFINED, not SHORT -> order is fixed: MOTOROLA */
		vs = exif_get_short (entry->data, EXIF_BYTE_ORDER_MOTOROLA);
		snprintf (val, maxlen, "%hi:%02i:%02i", vs, entry->data[2], entry->data[3]);
		break;
	case MNOTE_PENTAX2_TAG_TIME:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, val, maxlen)
		CC2 (entry->components, 3, 4, val, maxlen)
		snprintf (val, maxlen, "%02i:%02i:%02i", entry->data[0], entry->data[1], entry->data[2]);
		break;
	default:
		switch (entry->format) {
		case EXIF_FORMAT_ASCII:
		  strncpy (val, (char *)entry->data, MIN(maxlen, entry->size));
		  break;
		case EXIF_FORMAT_SHORT:
		  {
			const unsigned char *data = entry->data;
			size_t k, len = strlen(val), sizeleft;

			sizeleft = entry->size;
			for(k=0; k<entry->components; k++) {
				if (sizeleft < 2)
					break;
				vs = exif_get_short (data, entry->order);
				snprintf (val+len, maxlen-len, "%hu ", vs);
				len = strlen(val);
				data += 2;
				sizeleft -= 2;
			}
		  }
		  break;
		case EXIF_FORMAT_SSHORT:
		  {
			const unsigned char *data = entry->data;
			size_t k, len = strlen(val), sizeleft;

			sizeleft = entry->size;
			for(k=0; k<entry->components; k++) {
				if (sizeleft < 2)
					break;
				vss = exif_get_sshort (data, entry->order);
				snprintf (val+len, maxlen-len, "%hi ", vss);
				len = strlen(val);
				data += 2;
				sizeleft -= 2;
			}
		  }
		  break;
		case EXIF_FORMAT_LONG:
		  {
			const unsigned char *data = entry->data;
			size_t k, len = strlen(val), sizeleft;

			sizeleft = entry->size;
			for(k=0; k<entry->components; k++) {
				if (sizeleft < 4)
					break;
				vl = exif_get_long (data, entry->order);
				snprintf (val+len, maxlen-len, "%lu ", (long unsigned) vl);
				len = strlen(val);
				data += 4;
				sizeleft -= 4;
			}
		  }
		  break;
		case EXIF_FORMAT_SLONG:
		  {
			const unsigned char *data = entry->data;
			size_t k, len = strlen(val), sizeleft;

			sizeleft = entry->size;
			for(k=0; k<entry->components; k++) {
				if (sizeleft < 4)
					break;
				vsl = exif_get_slong (data, entry->order);
				snprintf (val+len, maxlen-len, "%li ", (long int) vsl);
				len = strlen(val);
				data += 4;
				sizeleft -= 4;
			}
		  }
		  break;
		case EXIF_FORMAT_UNDEFINED:
		default:
		  snprintf (val, maxlen, _("%i bytes unknown data"),
			  entry->size);
		  break;
		}
		break;
	}

	return val;
}

// libexif/pentax/exif-mnote-data-pentax.c

/* exif-mnote-data-pentax.c
 *
 * Copyright (c) 2002, 2003 Lutz Mueller <lutz@users.sourceforge.net>
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

static void
exif_mnote_data_pentax_clear (ExifMnoteDataPentax *n)
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
exif_mnote_data_pentax_free (ExifMnoteData *n)
{
	if (!n) return;

	exif_mnote_data_pentax_clear ((ExifMnoteDataPentax *) n);
}

static char *
exif_mnote_data_pentax_get_value (ExifMnoteData *d, unsigned int i, char *val, unsigned int maxlen)
{
	ExifMnoteDataPentax *n = (ExifMnoteDataPentax *) d;

	if (!n) return NULL;
	if (n->count <= i) return NULL;
	return mnote_pentax_entry_get_value (&n->entries[i], val, maxlen);
}

/** 
 * @brief save the MnoteData from ne to buf
 * 
 * @param ne extract the data from this structure 
 * @param *buf write the mnoteData to this buffer (buffer will be allocated)
 * @param buf_size the final size of the buffer
 */
static void
exif_mnote_data_pentax_save (ExifMnoteData *ne,
		unsigned char **buf, unsigned int *buf_size)
{
	ExifMnoteDataPentax *n = (ExifMnoteDataPentax *) ne;
	size_t i, datao,
	  base = 0,	/* internal MakerNote tag number offset */
	  o2 = 4 + 2;  	/* offset to first tag entry, past header */

	if (!n || !buf || !buf_size) return;
	datao = n->offset; /* this MakerNote style uses offsets
			      based on main IFD, not makernote IFD */

	/*
	 * Allocate enough memory for header, the number of entries, entries,
	 * and next IFD pointer
	 */
	*buf_size = o2 + 2 + n->count * 12 + 4;
	switch (n->version) {
	case casioV2:
		base = MNOTE_PENTAX2_TAG_BASE;
		*buf = exif_mem_alloc (ne->mem, *buf_size);
		if (!*buf) {
			EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataPentax", *buf_size);
			return;
		}
		/* Write the magic header */
		strcpy ((char *)*buf, "QVC");
		exif_set_short (*buf + 4, n->order, (ExifShort) 0);

		break;

	case pentaxV3:
		base = MNOTE_PENTAX2_TAG_BASE;
		*buf = exif_mem_alloc (ne->mem, *buf_size);
		if (!*buf) {
			EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataPentax", *buf_size);
			return;
		}

		/* Write the magic header */
		strcpy ((char *)*buf, "AOC");
		exif_set_short (*buf + 4, n->order, (ExifShort) (
			(n->order == EXIF_BYTE_ORDER_INTEL) ?
			('I' << 8) | 'I' :
			('M' << 8) | 'M'));
		break;

	case pentaxV2:
		base = MNOTE_PENTAX2_TAG_BASE;
		*buf = exif_mem_alloc (ne->mem, *buf_size);
		if (!*buf) {
			EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataPentax", *buf_size);
			return;
		}

		/* Write the magic header */
		strcpy ((char *)*buf, "AOC");
		exif_set_short (*buf + 4, n->order, (ExifShort) 0);
		break;

	case pentaxV1:
		/* It looks like this format doesn't have a magic header as
		 * such, just has a fixed number of entries equal to 0x001b */
		*buf_size -= 6;
		o2 -= 6;
		*buf = exif_mem_alloc (ne->mem, *buf_size);
		if (!*buf) {
			EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataPentax", *buf_size);
			return;
		}
		break;

	default:
		/* internal error */
		return;
	}

	/* Write the number of entries. */
	exif_set_short (*buf + o2, n->order, (ExifShort) n->count);
	o2 += 2;

	/* Save each entry */
	for (i = 0; i < n->count; i++) {
		size_t doff;	/* offset to current data portion of tag */
		size_t s;
		unsigned char *t;
		size_t o = o2 + i * 12;   /* current offset into output buffer */
		exif_set_short (*buf + o + 0, n->order,
				(ExifShort) (n->entries[i].tag - base));
		exif_set_short (*buf + o + 2, n->order,
				(ExifShort) n->entries[i].format);
		exif_set_long  (*buf + o + 4, n->order,
				n->entries[i].components);
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
			size_t ts = *buf_size + s;
			doff = *buf_size;
			t = exif_mem_realloc (ne->mem, *buf,
						 sizeof (char) * ts);
			if (!t) {
				EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataPentax", ts);
				return;
			}
			*buf = t;
			*buf_size = ts;
			exif_set_long (*buf + o, n->order, datao + doff);
		} else
			doff = o;

		/* Write the data. */
		if (n->entries[i].data) {
			memcpy (*buf + doff, n->entries[i].data, s);
		} else {
			/* Most certainly damaged input file */
			memset (*buf + doff, 0, s);
		}
	}

	/* Sanity check the buffer size */
	if (*buf_size < (o2 + n->count * 12 + 4)) {
		exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA, "ExifMnoteDataPentax",
			"Buffer overflow");
	}

	/* Reset next IFD pointer */
	exif_set_long (*buf + o2 + n->count * 12, n->order, 0);
}

static void
exif_mnote_data_pentax_load (ExifMnoteData *en,
		const unsigned char *buf, unsigned int buf_size)
{
	ExifMnoteDataPentax *n = (ExifMnoteDataPentax *) en;
	size_t i, tcount, o, datao, base = 0;
	ExifShort c;

	if (!n) return;

	if (!buf || !buf_size) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteDataPentax", "Short MakerNote");
		return;
	}
	datao = 6 + n->offset;
	if (CHECKOVERFLOW(datao, buf_size, 8)) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteDataPentax", "Short MakerNote");
		return;
	}

	/* Detect variant of Pentax/Casio MakerNote found */
	if (!memcmp(buf + datao, "AOC", 4)) {
		if ((buf[datao + 4] == 'I') && (buf[datao + 5] == 'I')) {
			n->version = pentaxV3;
			n->order = EXIF_BYTE_ORDER_INTEL;
		} else if ((buf[datao + 4] == 'M') && (buf[datao + 5] == 'M')) {
			n->version = pentaxV3;
			n->order = EXIF_BYTE_ORDER_MOTOROLA;
		} else {
			/* Uses Casio v2 tags */
			n->version = pentaxV2;
		}
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataPentax",
			"Parsing Pentax maker note v%d...", (int)n->version);
		datao += 4 + 2;
		base = MNOTE_PENTAX2_TAG_BASE;
	} else if (!memcmp(buf + datao, "QVC", 4)) {
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataPentax",
			"Parsing Casio maker note v2...");
		n->version = casioV2;
		base = MNOTE_CASIO2_TAG_BASE;
		datao += 4 + 2;
	} else {
		/* probably assert(!memcmp(buf + datao, "\x00\x1b", 2)) */
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataPentax",
			"Parsing Pentax maker note v1...");
		n->version = pentaxV1;
	}

	/* Read the number of tags */
	c = exif_get_short (buf + datao, n->order);
	datao += 2;

	/* Just use an arbitrary max tag limit here to avoid needing to much memory or time. There are 102 named tags currently.
	 * The format allows specifying the same range of memory as often as it can, so this multiplies quickly. */
	if (c > 200) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA, "ExifMnoteDataPentax", "Too much tags (%d) in Pentax MakerNote", c);
		return;
	}


	/* Remove any old entries */
	exif_mnote_data_pentax_clear (n);

	/* Reserve enough space for all the possible MakerNote tags */
	n->entries = exif_mem_alloc (en->mem, sizeof (MnotePentaxEntry) * c);
	if (!n->entries) {
		EXIF_LOG_NO_MEMORY(en->log, "ExifMnoteDataPentax", sizeof (MnotePentaxEntry) * c);
		return;
	}

	/* Parse all c entries, storing ones that are successfully parsed */
	tcount = 0;
	for (i = c, o = datao; i; --i, o += 12) {
		size_t s;

		memset(&n->entries[tcount], 0, sizeof(MnotePentaxEntry));
		if (CHECKOVERFLOW(o,buf_size,12)) {
			exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
				  "ExifMnoteDataPentax", "Short MakerNote");
			break;
		}

		n->entries[tcount].tag        = exif_get_short (buf + o + 0, n->order) + base;
		n->entries[tcount].format     = exif_get_short (buf + o + 2, n->order);
		n->entries[tcount].components = exif_get_long  (buf + o + 4, n->order);
		n->entries[tcount].order      = n->order;

		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnotePentax",
			  "Loading entry 0x%x ('%s')...", n->entries[tcount].tag,
			  mnote_pentax_tag_get_name (n->entries[tcount].tag));

		/* Check if we overflow the multiplication. Use buf_size as the max size for integer overflow detection,
		 * we will check the buffer sizes closer later. */
		if (	exif_format_get_size (n->entries[tcount].format) &&
			buf_size / exif_format_get_size (n->entries[tcount].format) < n->entries[tcount].components
		) {
			exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
				  "ExifMnoteDataPentax", "Tag size overflow detected (%u * %lu)", exif_format_get_size (n->entries[tcount].format), n->entries[tcount].components);
			break;
		}
		/*
		 * Size? If bigger than 4 bytes, the actual data is not
		 * in the entry but somewhere else (offset).
		 */
		s = exif_format_get_size (n->entries[tcount].format) *
                                      n->entries[tcount].components;
		n->entries[tcount].size = s;
		if (s) {
			size_t dataofs = o + 8;
			if (s > 4)
				/* The data in this case is merely a pointer */
			   	dataofs = exif_get_long (buf + dataofs, n->order) + 6;

			if (CHECKOVERFLOW(dataofs, buf_size, s)) {
				exif_log (en->log, EXIF_LOG_CODE_DEBUG,
						  "ExifMnoteDataPentax", "Tag data past end "
					  "of buffer (%u > %u)", (unsigned)(dataofs + s), buf_size);
				continue;
			}

			n->entries[tcount].data = exif_mem_alloc (en->mem, s);
			if (!n->entries[tcount].data) {
				EXIF_LOG_NO_MEMORY(en->log, "ExifMnoteDataPentax", s);
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
exif_mnote_data_pentax_count (ExifMnoteData *n)
{
	return n ? ((ExifMnoteDataPentax *) n)->count : 0;
}

static unsigned int
exif_mnote_data_pentax_get_id (ExifMnoteData *d, unsigned int n)
{
	ExifMnoteDataPentax *note = (ExifMnoteDataPentax *) d;

	if (!note) return 0;
	if (note->count <= n) return 0;
	return note->entries[n].tag;
}

static const char *
exif_mnote_data_pentax_get_name (ExifMnoteData *d, unsigned int n)
{
	ExifMnoteDataPentax *note = (ExifMnoteDataPentax *) d;

	if (!note) return NULL;
	if (note->count <= n) return NULL;
	return mnote_pentax_tag_get_name (note->entries[n].tag);
}

static const char *
exif_mnote_data_pentax_get_title (ExifMnoteData *d, unsigned int n)
{
	ExifMnoteDataPentax *note = (ExifMnoteDataPentax *) d;

	if (!note) return NULL;
	if (note->count <= n) return NULL;
	return mnote_pentax_tag_get_title (note->entries[n].tag);
}

static const char *
exif_mnote_data_pentax_get_description (ExifMnoteData *d, unsigned int n)
{
	ExifMnoteDataPentax *note = (ExifMnoteDataPentax *) d;
	
	if (!note) return NULL;
	if (note->count <= n) return NULL;
	return mnote_pentax_tag_get_description (note->entries[n].tag);
}

static void
exif_mnote_data_pentax_set_offset (ExifMnoteData *d, unsigned int o)
{
	if (d) ((ExifMnoteDataPentax *) d)->offset = o;
}

static void
exif_mnote_data_pentax_set_byte_order (ExifMnoteData *d, ExifByteOrder o)
{
	ExifByteOrder o_orig;
	ExifMnoteDataPentax *n = (ExifMnoteDataPentax *) d;
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

int
exif_mnote_data_pentax_identify (const ExifData *ed, const ExifEntry *e)
{
	(void) ed;  /* unused */
	if ((e->size >= 8) && !memcmp (e->data, "AOC", 4)) {
		if (((e->data[4] == 'I') && (e->data[5] == 'I')) ||
		    ((e->data[4] == 'M') && (e->data[5] == 'M')))
			return pentaxV3;
		else
			/* Uses Casio v2 tags */
			return pentaxV2;
	}

	if ((e->size >= 8) && !memcmp (e->data, "QVC", 4))
		return casioV2;

	/* This isn't a very robust test, so make sure it's done last */
	/* Maybe we should additionally check for a make of Asahi or Pentax */
	if ((e->size >= 2) && (e->data[0] == 0x00) && (e->data[1] == 0x1b))
		return pentaxV1;

	return 0;
}

ExifMnoteData *
exif_mnote_data_pentax_new (ExifMem *mem)
{
	ExifMnoteData *d;

	if (!mem) return NULL;

	d = exif_mem_alloc (mem, sizeof (ExifMnoteDataPentax));
	if (!d) return NULL;

	exif_mnote_data_construct (d, mem);

	/* Set up function pointers */
	d->methods.free            = exif_mnote_data_pentax_free;
	d->methods.set_byte_order  = exif_mnote_data_pentax_set_byte_order;
	d->methods.set_offset      = exif_mnote_data_pentax_set_offset;
	d->methods.load            = exif_mnote_data_pentax_load;
	d->methods.save            = exif_mnote_data_pentax_save;
	d->methods.count           = exif_mnote_data_pentax_count;
	d->methods.get_id          = exif_mnote_data_pentax_get_id;
	d->methods.get_name        = exif_mnote_data_pentax_get_name;
	d->methods.get_title       = exif_mnote_data_pentax_get_title;
	d->methods.get_description = exif_mnote_data_pentax_get_description;
	d->methods.get_value       = exif_mnote_data_pentax_get_value;

	return d;
}

}
