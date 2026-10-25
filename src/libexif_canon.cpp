// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif (libexif/canon): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.

// Purpose: libexif's canon maker note decoder: how it recognises the note, reads its entries and
// names and renders each value.

#include "libexif_makernote.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4996) // strcpy, strncpy and strncat, as libexif calls them
#pragma warning(disable : 4244 4267 4100 4189 4456 4457 4459 4702) // and its implicit narrowing and leftovers
#endif

namespace libfiles::detail::libexif
{
// libexif/canon/mnote-canon-tag.h

/* mnote-canon-tag.h
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




typedef unsigned int _MnoteCanonTag;
enum : unsigned int {
	MNOTE_CANON_TAG_UNKNOWN_0	= 0x0,
	MNOTE_CANON_TAG_SETTINGS_1	= 0x1, /* subtree */
	MNOTE_CANON_TAG_FOCAL_LENGTH	= 0x2,
	MNOTE_CANON_TAG_UNKNOWN_3	= 0x3,
	MNOTE_CANON_TAG_SETTINGS_2	= 0x4, /* subtree */
	MNOTE_CANON_TAG_PANORAMA	= 0x5,
	MNOTE_CANON_TAG_IMAGE_TYPE	= 0x6,
	MNOTE_CANON_TAG_FIRMWARE	= 0x7,
	MNOTE_CANON_TAG_IMAGE_NUMBER	= 0x8,
	MNOTE_CANON_TAG_OWNER		= 0x9,
	MNOTE_CANON_TAG_UNKNOWN_10	= 0xa,
	MNOTE_CANON_TAG_SERIAL_NUMBER	= 0xc,
	MNOTE_CANON_TAG_CAMERA_INFO	= 0xd,
	MNOTE_CANON_TAG_FILE_LENGTH	= 0xe,
	MNOTE_CANON_TAG_CUSTOM_FUNCS	= 0xf,
	MNOTE_CANON_TAG_MODEL_ID	= 0x10,
	MNOTE_CANON_TAG_MOVIE_INFO	= 0x11, /* subtree */
	MNOTE_CANON_TAG_AF_INFO		= 0x12, /* subtree */
	MNOTE_CANON_TAG_THUMBNAIL_VALID_AREA		= 0x13,
	MNOTE_CANON_TAG_SERIAL_NUMBER_FORMAT		= 0x15,
	MNOTE_CANON_TAG_SUPER_MACRO	= 0x1a,
	MNOTE_CANON_TAG_DATE_STAMP_MODE	= 0x1c,
	MNOTE_CANON_TAG_MY_COLORS	= 0x1d,	/* subtree */
	MNOTE_CANON_TAG_FIRMWARE_REVISION	= 0x1e,
	MNOTE_CANON_TAG_CATEGORIES	= 0x23,
	MNOTE_CANON_TAG_FACEDETECT1	= 0x24,	/* subtree */
	MNOTE_CANON_TAG_FACEDETECT2	= 0x25,	/* subtree */
	MNOTE_CANON_TAG_AF_INFO2	= 0x26,	/* subtree */
	MNOTE_CANON_TAG_CONTRAST_INFO	= 0x27,	/* subtree */
	MNOTE_CANON_TAG_IMAGE_UNIQUE_ID	= 0x28,
	MNOTE_CANON_TAG_WB_INFO		= 0x29,	/* subtree */
	MNOTE_CANON_TAG_FACEDETECT3	= 0x2f,	/* subtree */
	MNOTE_CANON_TAG_TIMEINFO	= 0x35,	/* subtree */
	MNOTE_CANON_TAG_BATTERY_TYPE	= 0x38,
	MNOTE_CANON_TAG_AF_INFO3	= 0x3c,	/* subtree */
	MNOTE_CANON_TAG_RAW_DATA_OFFSET	= 0x81,
	MNOTE_CANON_TAG_ORIGINAL_DECISION_DATA_OFFSET	= 0x83,
	MNOTE_CANON_TAG_PERSONAL_FUNCTIONS		= 0x91,	/* subtree */
	MNOTE_CANON_TAG_PERSONAL_FUNCTIONS_VALUES	= 0x92,	/* subtree */
	MNOTE_CANON_TAG_FILE_INFO		= 0x93,	/* subtree */
	MNOTE_CANON_TAG_LENS_MODEL		= 0x95,
	MNOTE_CANON_TAG_CMOS_SERIAL		= 0x96,
	MNOTE_CANON_TAG_DUST_REMOVAL_DATA	= 0x97,
	MNOTE_CANON_TAG_CROP_INFO		= 0x98,	/* subtree */
	MNOTE_CANON_TAG_CUSTOM_FUNCTIONS2	= 0x99,	/* subtree */
	MNOTE_CANON_TAG_ASPECT_INFO		= 0x9a,	/* subtree */
	MNOTE_CANON_TAG_COLOR_INFORMATION	= 0xa0,	/* subtree */
	MNOTE_CANON_TAG_TONE_CURVE_TABLE	= 0xa1,
	MNOTE_CANON_TAG_SHARPNESS_TABLE		= 0xa2,
	MNOTE_CANON_TAG_SHARPNESS_FREQ_TABLE	= 0xa3,
	MNOTE_CANON_TAG_WHITEBALANCE_TABLE	= 0xa3,
	MNOTE_CANON_TAG_COLOR_BALANCE		= 0xa9,	/* subtree */
	MNOTE_CANON_TAG_MEASURED_COLOR		= 0xaa,	/* subtree */
	MNOTE_CANON_TAG_COLOR_TEMPERATURE	= 0xae,
	MNOTE_CANON_TAG_CANON_FLAGS		= 0xb0,	/* subtree */
	MNOTE_CANON_TAG_MODIFIED_INFO		= 0xb1,	/* subtree */
	MNOTE_CANON_TAG_TONECURVE_MATCHING	= 0xb2,
	MNOTE_CANON_TAG_WHITEBALANCE_MATCHING	= 0xb3,
	MNOTE_CANON_TAG_COLOR_SPACE		= 0xb4,
	MNOTE_CANON_TAG_PREVIEW_IMAGE_INFO	= 0xb6,	/* subtree */
	MNOTE_CANON_TAG_VRD_OFFSET		= 0xd0,
	MNOTE_CANON_TAG_SENSOR_INFO		= 0xe0,	/* subtree */
	MNOTE_CANON_TAG_WB_PACKET		= 0x4001, /* subtree */
	MNOTE_CANON_TAG_COLOR_INFO		= 0x4003, /* subtree */
	MNOTE_CANON_TAG_FLAVOR			= 0x4005,
	MNOTE_CANON_TAG_PICTURESTYLE_USERDEF	= 0x4008,
	MNOTE_CANON_TAG_PICTURESTYLE_PC		= 0x4009,
	MNOTE_CANON_TAG_CUSTOMPICTURE_STYLE_FN	= 0x4010,
	MNOTE_CANON_TAG_AF_MICRO_ADJUST		= 0x4013, /* subtree */
	MNOTE_CANON_TAG_VIGNETTING_CORRECT	= 0x4015, /* subtree */
	MNOTE_CANON_TAG_VIGNETTING_CORRECT2	= 0x4016, /* subtree */
	MNOTE_CANON_TAG_LIGHTINGOPT		= 0x4018, /* subtree */
	MNOTE_CANON_TAG_LENS_INFO		= 0x4019, /* subtree */
	MNOTE_CANON_TAG_AMBIENCE_INFO		= 0x4020, /* subtree */
	MNOTE_CANON_TAG_MULTI_EXPOSURE		= 0x4021, /* subtree */
	MNOTE_CANON_TAG_FILTER_INFO		= 0x4024, /* subtree */
	MNOTE_CANON_TAG_HDR_INFO		= 0x4025, /* subtree */
	MNOTE_CANON_TAG_AF_CONFIG		= 0x4028, /* subtree */
	MNOTE_CANON_TAG_RAW_BURST_INFO		= 0x403f  /* subtree */
};
typedef unsigned int MnoteCanonTag;

const char *mnote_canon_tag_get_name        (MnoteCanonTag);
const char *mnote_canon_tag_get_name_sub    (MnoteCanonTag, unsigned int, ExifDataOption);
const char *mnote_canon_tag_get_title       (MnoteCanonTag);
const char *mnote_canon_tag_get_title_sub   (MnoteCanonTag, unsigned int, ExifDataOption);
const char *mnote_canon_tag_get_description (MnoteCanonTag);

// libexif/canon/mnote-canon-entry.h

/* mnote-canon-entry.h
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



typedef struct _MnoteCanonEntry        MnoteCanonEntry;

struct _MnoteCanonEntry {
	MnoteCanonTag tag;
	ExifFormat format;
	unsigned long components;

	unsigned char *data;
	unsigned int size;

	ExifByteOrder order;
};

unsigned int mnote_canon_entry_count_values (const MnoteCanonEntry *);
char        *mnote_canon_entry_get_value    (const MnoteCanonEntry *, unsigned int t, char *val, unsigned int maxlen);

// libexif/canon/exif-mnote-data-canon.h

/* exif-mnote-data-canon.h
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



typedef struct _ExifMnoteDataCanon ExifMnoteDataCanon;


struct _ExifMnoteDataCanon {
	ExifMnoteData parent;

	MnoteCanonEntry *entries;
	unsigned int count;

	ExifByteOrder order;
	unsigned int offset;

	ExifDataOption options;
};

/*! Detect if MakerNote is recognized as one handled by the Canon module.
 * 
 * \param[in] ed image #ExifData to identify as as a Canon type
 * \param[in] e #ExifEntry for EXIF_TAG_MAKER_NOTE, from within ed but
 *   duplicated here for convenience
 * \return 0 if not recognized, nonzero if recognized. The specific nonzero 
 *   value returned may identify a subtype unique within this module.
 */
int exif_mnote_data_canon_identify (const ExifData *ed, const ExifEntry *e);

ExifMnoteData *exif_mnote_data_canon_new (ExifMem *mem, ExifDataOption o);

// libexif/canon/mnote-canon-tag.c

/* mnote-canon-tag.c
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
	MnoteCanonTag tag;
	const char *name;
	const char *title;
	const char *description;
} table[] = {
#ifndef NO_VERBOSE_TAG_STRINGS
	{MNOTE_CANON_TAG_SETTINGS_1, "Settings1", N_("Settings (First Part)"), ""},
	{MNOTE_CANON_TAG_FOCAL_LENGTH, "FocalLength", N_("Focal Length"), ""},
	{MNOTE_CANON_TAG_SETTINGS_2, "Settings2", N_("Settings (Second Part)"), ""},
	{MNOTE_CANON_TAG_PANORAMA, "Panorama", N_("Panorama"), ""},
	{MNOTE_CANON_TAG_IMAGE_TYPE, "ImageType", N_("Image Type"), ""},
	{MNOTE_CANON_TAG_FIRMWARE, "FirmwareVersion", N_("Firmware Version"), ""},
	{MNOTE_CANON_TAG_IMAGE_NUMBER, "ImageNumber", N_("Image Number"), ""},
	{MNOTE_CANON_TAG_OWNER, "OwnerName", N_("Owner Name"), ""},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, "ColorInformation", N_("Color Information"), ""},
	{MNOTE_CANON_TAG_SERIAL_NUMBER, "SerialNumber", N_("Serial Number"), ""},
	{MNOTE_CANON_TAG_CAMERA_INFO, "CameraInfo", N_("Camera Info"), ""},
	{MNOTE_CANON_TAG_FILE_LENGTH, "FileLength", N_("File Length"), ""},
	{MNOTE_CANON_TAG_CUSTOM_FUNCS, "CustomFunctions", N_("Custom Functions"), ""},
	{MNOTE_CANON_TAG_MODEL_ID,"ModelID", N_("Model ID"), ""},
	{MNOTE_CANON_TAG_MOVIE_INFO,"MovieInfo", N_("Movie Info"), ""},
	{MNOTE_CANON_TAG_AF_INFO,"AFInfo", N_("AF Info"), ""},
	{MNOTE_CANON_TAG_THUMBNAIL_VALID_AREA,"ThumbnailValidArea", N_("Thumbnail Valid Area"), ""},
	{MNOTE_CANON_TAG_SERIAL_NUMBER_FORMAT,"SerialNumberFormat", N_("Serial Number Format"), ""},
	{MNOTE_CANON_TAG_SUPER_MACRO,"SuperMacro", N_("Super Macro"), ""},
	{MNOTE_CANON_TAG_DATE_STAMP_MODE,"DateStampMode", N_("Date Stamp Mode"), ""},
	{MNOTE_CANON_TAG_MY_COLORS,"MyColors", N_("My Colors"), ""},
	{MNOTE_CANON_TAG_FIRMWARE_REVISION,"FirmwareRevision", N_("Firmware Revision"), ""},
	{MNOTE_CANON_TAG_CATEGORIES,"Categories", N_("Categories"), ""},
	{MNOTE_CANON_TAG_FACEDETECT1,"FaceDetect1", N_("Face Detect 1"), ""},
	{MNOTE_CANON_TAG_FACEDETECT2,"FaceDetect2", N_("Face Detect 2"), ""},
	{MNOTE_CANON_TAG_AF_INFO2,"AFInfo2", N_("AF Info 2"), ""},
	{MNOTE_CANON_TAG_CONTRAST_INFO,"ContrastInfo", N_("Contrast Info"), ""},
	{MNOTE_CANON_TAG_IMAGE_UNIQUE_ID,"ImageUniqueID", N_("Image Unique ID"), ""},
	{MNOTE_CANON_TAG_WB_INFO,"WBInfo", N_("WB Info"), ""},
	{MNOTE_CANON_TAG_FACEDETECT3,"FaceDetect3", N_("Face Detect 3"), ""},
	{MNOTE_CANON_TAG_TIMEINFO,"TimeInfo", N_("Time Info"), ""},
	{MNOTE_CANON_TAG_BATTERY_TYPE,"Battery Type", N_("Battery Type"), ""},
	{MNOTE_CANON_TAG_AF_INFO3,"AFInfo3", N_("AF Info 3"), ""},
	{MNOTE_CANON_TAG_RAW_DATA_OFFSET,"RawDataOffset", N_("Raw Data Offset"), ""},
	{MNOTE_CANON_TAG_ORIGINAL_DECISION_DATA_OFFSET,"OriginalDecisionDataOffset", N_("Original Decision Data Offset"), ""},
	{MNOTE_CANON_TAG_PERSONAL_FUNCTIONS,"PesonalFunctions", N_("Personal Functions"), ""},
	{MNOTE_CANON_TAG_PERSONAL_FUNCTIONS_VALUES,"PersonalFunctionsValues", N_("Personal Functions Values"), ""},
	{MNOTE_CANON_TAG_FILE_INFO,"FileInfo", N_("File Info"), ""},
	{MNOTE_CANON_TAG_LENS_MODEL,"LensModel", N_("Lens Model"), ""},
	{MNOTE_CANON_TAG_CMOS_SERIAL,"CMOSSerialNumber", N_("CMOS Serial Number"), ""},
	{MNOTE_CANON_TAG_DUST_REMOVAL_DATA,"DustRemovalData", N_("Dust Removal Data"), ""},
	{MNOTE_CANON_TAG_CROP_INFO,"CropInfo", N_("Crop Info"), ""},
	{MNOTE_CANON_TAG_CUSTOM_FUNCTIONS2,"CustomFunctions2", N_("Custom Functions 2"), ""},
	{MNOTE_CANON_TAG_ASPECT_INFO,"AspectInfo", N_("Aspect Info"), ""},
	{MNOTE_CANON_TAG_TONE_CURVE_TABLE,"ToneCurveTable", N_("Tone Curve Table"), ""},
	{MNOTE_CANON_TAG_SHARPNESS_TABLE,"SharpnessTable", N_("Sharpness Table"), ""},
	{MNOTE_CANON_TAG_SHARPNESS_FREQ_TABLE,"SharpnessFreqTable", N_("Sharpness Frequency Table"), ""},
	{MNOTE_CANON_TAG_WHITEBALANCE_TABLE,"WhitebalanceTable", N_("Whitebalance Table"), ""},
	{MNOTE_CANON_TAG_COLOR_BALANCE,"ColorBalance", N_("Color Balance"), ""},
	{MNOTE_CANON_TAG_MEASURED_COLOR,"MeasuredColor", N_("Measured Color"), ""},
	{MNOTE_CANON_TAG_COLOR_TEMPERATURE,"ColorTemperature", N_("Color Temperature"), ""},
	{MNOTE_CANON_TAG_CANON_FLAGS,"CanonFlags", N_("Canon Flags"), ""},
	{MNOTE_CANON_TAG_MODIFIED_INFO,"ModifiedInfo", N_("Modified Info"), ""},
	{MNOTE_CANON_TAG_TONECURVE_MATCHING,"TonecurveMatching", N_("Tonecurve Matching"), ""},
	{MNOTE_CANON_TAG_WHITEBALANCE_MATCHING,"WhitebalanceMatching", N_("Whitebalance Matching"), ""},
	{MNOTE_CANON_TAG_COLOR_SPACE,"ColorSpace", N_("Color Space"), ""},
	{MNOTE_CANON_TAG_PREVIEW_IMAGE_INFO,"PreviewImageInfo", N_("Preview Image Info"), ""},
	{MNOTE_CANON_TAG_VRD_OFFSET,"VRDOffset", N_("VRD Offset"), ""},
	{MNOTE_CANON_TAG_SENSOR_INFO,"SensorInfo", N_("Sensor Info"), ""},
	{MNOTE_CANON_TAG_WB_PACKET,"WBPacket", N_("WB Packet"), ""},
	{MNOTE_CANON_TAG_FLAVOR,"Flavor", N_("Flavor"), ""},
	{MNOTE_CANON_TAG_PICTURESTYLE_USERDEF,"PictureStyleUserDef", N_("Picture Style Userdefined"), ""},
	{MNOTE_CANON_TAG_PICTURESTYLE_PC,"PictureStylePC", N_("Picture Style PC"), ""},
	{MNOTE_CANON_TAG_CUSTOMPICTURE_STYLE_FN,"CustomPictureStyleFN", N_("Custom Picture Style Filename"), ""},
	{MNOTE_CANON_TAG_AF_MICRO_ADJUST,"AFMicroAdjust", N_("AF Micro Adjust"), ""},
	{MNOTE_CANON_TAG_VIGNETTING_CORRECT,"VignettingCorrect", N_("Vignetting Correct"), ""},
	{MNOTE_CANON_TAG_VIGNETTING_CORRECT2,"VignettingCorrect2", N_("Vignetting Correct 2"), ""},
	{MNOTE_CANON_TAG_LIGHTINGOPT,"LightingOpt", N_("LightingOpt"), ""},
	{MNOTE_CANON_TAG_LENS_INFO,"LensInfo", N_("Lens Info"), ""},
	{MNOTE_CANON_TAG_AMBIENCE_INFO,"AmbienceInfo", N_("Ambience_Info"), ""},
	{MNOTE_CANON_TAG_MULTI_EXPOSURE,"MultiExposure", N_("Multi Exposure"), ""},
	{MNOTE_CANON_TAG_FILTER_INFO,"FilterInfo", N_("Filter Info"), ""},
	{MNOTE_CANON_TAG_HDR_INFO,"HDRInfo", N_("HDR Info"), ""},
	{MNOTE_CANON_TAG_AF_CONFIG,"AFConfig", N_("AF Config"), ""},
	{MNOTE_CANON_TAG_RAW_BURST_INFO,"RawBurstInfo", N_("Raw Burst Info"), ""},
#endif
	{0, NULL, NULL, NULL}
};

static const struct {
	MnoteCanonTag tag;
	unsigned int subtag;
	const char *name;
} table_sub[] = {
#ifndef NO_VERBOSE_TAG_STRINGS
	{MNOTE_CANON_TAG_SETTINGS_1,  0, N_("Macro Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1,  1, N_("Self-timer")},
	{MNOTE_CANON_TAG_SETTINGS_1,  2, N_("Quality")},
	{MNOTE_CANON_TAG_SETTINGS_1,  3, N_("Flash Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1,  4, N_("Drive Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1,  6, N_("Focus Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1,  8, N_("Record Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1,  9, N_("Image Size")},
	{MNOTE_CANON_TAG_SETTINGS_1, 10, N_("Easy Shooting Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1, 11, N_("Digital Zoom")},
	{MNOTE_CANON_TAG_SETTINGS_1, 12, N_("Contrast")},
	{MNOTE_CANON_TAG_SETTINGS_1, 13, N_("Saturation")},
	{MNOTE_CANON_TAG_SETTINGS_1, 14, N_("Sharpness")},
	{MNOTE_CANON_TAG_SETTINGS_1, 15, N_("ISO")},
	{MNOTE_CANON_TAG_SETTINGS_1, 16, N_("Metering Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1, 17, N_("Focus Range")},
	{MNOTE_CANON_TAG_SETTINGS_1, 18, N_("AF Point")},
	{MNOTE_CANON_TAG_SETTINGS_1, 19, N_("Exposure Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1, 21, N_("Lens Type")},
	{MNOTE_CANON_TAG_SETTINGS_1, 22, N_("Long Focal Length of Lens")},
	{MNOTE_CANON_TAG_SETTINGS_1, 23, N_("Short Focal Length of Lens")},
	{MNOTE_CANON_TAG_SETTINGS_1, 24, N_("Focal Units per mm")},
	{MNOTE_CANON_TAG_SETTINGS_1, 25, N_("Maximal Aperture")},
	{MNOTE_CANON_TAG_SETTINGS_1, 26, N_("Minimal Aperture")},
	{MNOTE_CANON_TAG_SETTINGS_1, 27, N_("Flash Activity")},
	{MNOTE_CANON_TAG_SETTINGS_1, 28, N_("Flash Details")},
	{MNOTE_CANON_TAG_SETTINGS_1, 31, N_("Focus Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1, 32, N_("AE Setting")},
	{MNOTE_CANON_TAG_SETTINGS_1, 33, N_("Image Stabilization")},
	{MNOTE_CANON_TAG_SETTINGS_1, 34, N_("Display Aperture")},
	{MNOTE_CANON_TAG_SETTINGS_1, 35, N_("Zoom Source Width")},
	{MNOTE_CANON_TAG_SETTINGS_1, 36, N_("Zoom Target Width")},
	{MNOTE_CANON_TAG_SETTINGS_1, 38, N_("Spot Metering Mode")},
	{MNOTE_CANON_TAG_SETTINGS_1, 39, N_("Photo Effect")},
	{MNOTE_CANON_TAG_SETTINGS_1, 40, N_("Manual Flash Output")},
	{MNOTE_CANON_TAG_SETTINGS_1, 41, N_("Color Tone")},
	{MNOTE_CANON_TAG_SETTINGS_1, 45, N_("SRAW Quality")},
	{MNOTE_CANON_TAG_FOCAL_LENGTH, 0, N_("Focal Type")},
	{MNOTE_CANON_TAG_FOCAL_LENGTH, 1, N_("Focal Length")},
	{MNOTE_CANON_TAG_FOCAL_LENGTH, 2, N_("Focal Plane X Size")},
	{MNOTE_CANON_TAG_FOCAL_LENGTH, 3, N_("Focal Plane Y Size")},
	{MNOTE_CANON_TAG_SETTINGS_2, 0, N_("Auto ISO")},
	{MNOTE_CANON_TAG_SETTINGS_2, 1, N_("Shot ISO")},
	{MNOTE_CANON_TAG_SETTINGS_2, 2, N_("Measured EV")},
	{MNOTE_CANON_TAG_SETTINGS_2, 3, N_("Target Aperture")},
	{MNOTE_CANON_TAG_SETTINGS_2, 4, N_("Target Exposure Time")},
	{MNOTE_CANON_TAG_SETTINGS_2, 5, N_("Exposure Compensation")},
	{MNOTE_CANON_TAG_SETTINGS_2, 6, N_("White Balance")},
	{MNOTE_CANON_TAG_SETTINGS_2, 7, N_("Slow Shutter")},
	{MNOTE_CANON_TAG_SETTINGS_2, 8, N_("Sequence Number")},
	{MNOTE_CANON_TAG_SETTINGS_2, 9, N_("Optical Zoom Code")},
	{MNOTE_CANON_TAG_SETTINGS_2, 11, N_("Camera Temperature")},
	{MNOTE_CANON_TAG_SETTINGS_2, 12, N_("Flash Guide Number")},
	{MNOTE_CANON_TAG_SETTINGS_2, 13, N_("AF Point")},
	{MNOTE_CANON_TAG_SETTINGS_2, 14, N_("Flash Exposure Compensation")},
	{MNOTE_CANON_TAG_SETTINGS_2, 15, N_("AE Bracketing")},
	{MNOTE_CANON_TAG_SETTINGS_2, 16, N_("AE Bracket Value")},
	{MNOTE_CANON_TAG_SETTINGS_2, 17, N_("Control Mode")},
	{MNOTE_CANON_TAG_SETTINGS_2, 18, N_("Focus Distance Upper")},
	{MNOTE_CANON_TAG_SETTINGS_2, 19, N_("Focus Distance Lower")},
	{MNOTE_CANON_TAG_SETTINGS_2, 20, N_("F-Number")},
	{MNOTE_CANON_TAG_SETTINGS_2, 21, N_("Exposure Time")},
	{MNOTE_CANON_TAG_SETTINGS_2, 22, N_("Measured EV 2")},
	{MNOTE_CANON_TAG_SETTINGS_2, 23, N_("Bulb Duration")},
	{MNOTE_CANON_TAG_SETTINGS_2, 25, N_("Camera Type")},
	{MNOTE_CANON_TAG_SETTINGS_2, 26, N_("Auto Rotate")},
	{MNOTE_CANON_TAG_SETTINGS_2, 27, N_("ND Filter")},
	{MNOTE_CANON_TAG_SETTINGS_2, 28, N_("Self-timer")},
	{MNOTE_CANON_TAG_SETTINGS_2, 32, N_("Manual Flash Output")},
	{MNOTE_CANON_TAG_PANORAMA, 2, N_("Panorama Frame")},
	{MNOTE_CANON_TAG_PANORAMA, 5, N_("Panorama Direction")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 0, N_("Tone Curve")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 1, N_("Sharpness")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 2, N_("Sharpness Frequency")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 3, N_("Sensor Red Level")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 4, N_("Sensor Blue Level")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 5, N_("White Balance Red")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 6, N_("White Balance Blue")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 7, N_("White Balance")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 8, N_("Color Temperature")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 9, N_("Picture Style")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 10, N_("Digital Gain")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 11, N_("White Balance Shift AB")},
	{MNOTE_CANON_TAG_COLOR_INFORMATION, 12, N_("White Balance Shift GM")},
#endif
	{0, 0, NULL}
};

const char *
mnote_canon_tag_get_name (MnoteCanonTag t)
{
	unsigned int i;

	for (i = 0; i < sizeof (table) / sizeof (table[0]); i++)
		if (table[i].tag == t) return table[i].name; /* do not translate */
	return NULL;
}

const char *
mnote_canon_tag_get_name_sub (MnoteCanonTag t, unsigned int s, ExifDataOption o)
{
	unsigned int i;
	int tag_found = 0;

	for (i = 0; i < sizeof (table_sub) / sizeof (table_sub[0]); i++) {
		if (table_sub[i].tag == t) {
			if (table_sub[i].subtag == s)
				return table_sub[i].name;
			tag_found = 1;
		}
	}
	if (!tag_found || !(o & EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS))
		return mnote_canon_tag_get_name (t);
	else
		return NULL;
}

const char *
mnote_canon_tag_get_title (MnoteCanonTag t)
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
mnote_canon_tag_get_title_sub (MnoteCanonTag t, unsigned int s, ExifDataOption o)
{
	unsigned int i;
	int tag_found = 0;

	for (i = 0; i < sizeof (table_sub) / sizeof (table_sub[0]); i++) {
		if (table_sub[i].tag == t) {
			if (table_sub[i].subtag == s) {
				if (!table_sub[i].name)
					return NULL;
				return _(table_sub[i].name);
			}
			tag_found = 1;
		}
	}
	if (!tag_found || !(o & EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS))
		return mnote_canon_tag_get_title (t);
	else
		return NULL;
}

const char *
mnote_canon_tag_get_description (MnoteCanonTag t)
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

// libexif/canon/mnote-canon-entry.c

/* mnote-canon-entry.c
 *
 * Copyright (c) 2002 Lutz Mueller <lutz@users.sourceforge.net>
 * Copyright (c) 2003 Matthieu Castet <mat-c@users.sourceforge.net>
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
                snprintf (v, maxlen,                            \
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

#define UNDEFINED 0xFF
    
static const struct canon_entry_table_t {
  unsigned int subtag;
  ExifShort value;
  const char *name;
} entries_settings_1 [] = {
#ifndef NO_VERBOSE_TAG_DATA
  { 0,  1, N_("Macro")},
  { 0,  2, N_("Normal")},
  { 2,  1, N_("Economy")},
  { 2,  2, N_("Normal")},  
  { 2,  3, N_("Fine")},
  { 2,  4, N_("RAW")},
  { 2,  5, N_("Superfine")},
  { 3,  0, N_("Off")},
  { 3,  1, N_("Auto")},
  { 3,  2, N_("On")},
  { 3,  3, N_("Red-eye reduction")},
  { 3,  4, N_("Slow synchro")},
  { 3,  5, N_("Auto, red-eye reduction")},
  { 3,  6, N_("On, red-eye reduction")},
  { 3, 16, N_("External flash")},
  { 4,  0, N_("Single")},
  { 4,  1, N_("Continuous")},
  { 4,  2, N_("Movie")},
  { 4,  3, N_("Continuous, speed priority")},
  { 4,  4, N_("Continuous, low")},
  { 4,  5, N_("Continuous, high")},
  { 6,  0, N_("One-shot AF")},
  { 6,  1, N_("AI servo AF")},
  { 6,  2, N_("AI focus AF")},
  { 6,  3, N_("Manual focus")},
  { 6,  4, N_("Single")},
  { 6,  5, N_("Continuous")},
  { 6,  6, N_("Manual focus")},
  { 6,  16, N_("Pan focus")},
  { 8,  1, N_("JPEG")},
  { 8,  2, N_("CRW+THM")},
  { 8,  3, N_("AVI+THM")},
  { 8,  4, N_("TIF")},
  { 8,  5, N_("TIF+JPEG")},
  { 8,  6, N_("CR2")},
  { 8,  7, N_("CR2+JPEG")},
  { 9,  0, N_("Large")},
  { 9,  1, N_("Medium")},
  { 9,  2, N_("Small")},
  { 9,  5, N_("Medium 1")},
  { 9,  6, N_("Medium 2")},
  { 9,  7, N_("Medium 3")},
  { 9,  8, N_("Postcard")},
  { 9,  9, N_("Widescreen")},
  {10,  0, N_("Full auto")},
  {10,  1, N_("Manual")},
  {10,  2, N_("Landscape")},
  {10,  3, N_("Fast shutter")},
  {10,  4, N_("Slow shutter")},
  {10,  5, N_("Night")},
  {10,  6, N_("Grayscale")},
  {10,  7, N_("Sepia")},
  {10,  8, N_("Portrait")},
  {10,  9, N_("Sports")},
  {10, 10, N_("Macro")},
  {10, 11, N_("Black & white")},
  {10, 12, N_("Pan focus")},
  {10, 13, N_("Vivid")},
  {10, 14, N_("Neutral")},
  {10, 15, N_("Flash off")},
  {10, 16, N_("Long shutter")},
  {10, 17, N_("Super macro")},
  {10, 18, N_("Foliage")},
  {10, 19, N_("Indoor")},
  {10, 20, N_("Fireworks")},
  {10, 21, N_("Beach")},
  {10, 22, N_("Underwater")},
  {10, 23, N_("Snow")},
  {10, 24, N_("Kids & pets")},
  {10, 25, N_("Night snapshot")},
  {10, 26, N_("Digital macro")},
  {10, 27, N_("My colors")},
  {10, 28, N_("Still image")},
  {10, 30, N_("Color accent")},
  {10, 31, N_("Color swap")},
  {10, 32, N_("Aquarium")},
  {10, 33, N_("ISO 3200")},
  {11, 0, N_("None")},
  {11, 1, N_("2x")},
  {11, 2, N_("4x")},
  {11, 3, N_("Other")},
  {12, 0x0000, N_("Normal")},
  {12, 0x0001, N_("High")},
  {12, 0xffff, N_("Low")},
  {13, 0x0000, N_("Normal")},
  {13, 0x0001, N_("High")},
  {13, 0xffff, N_("Low")},
  {14, 0x0000, N_("Normal")},
  {14, 0x0001, N_("High")},
  {14, 0xffff, N_("Low")},
  {15, 14, N_("Auto high")},
  {15, 15, N_("Auto")},
  {15, 16, N_("50")},
  {15, 17, N_("100")},
  {15, 18, N_("200")},
  {15, 19, N_("400")},
  {15, 20, N_("800")},
  {16,  0, N_("Default")},
  {16,  1, N_("Spot")},
  {16,  2, N_("Average")},	
  {16,  3, N_("Evaluative")},
  {16,  4, N_("Partial")},
  {16,  5, N_("Center-weighted average")},
  {17,  0, N_("Manual")},
  {17,  1, N_("Auto")},
  {17,  2, N_("Not known")},
  {17,  3, N_("Macro")},
  {17,  4, N_("Very close")},
  {17,  5, N_("Close")},
  {17,  6, N_("Middle range")},
  {17,  7, N_("Far range")},
  {17,  8, N_("Pan focus")},
  {17,  9, N_("Super macro")},
  {17,  10, N_("Infinity")},
  {18, 0x2005, N_("Manual AF point selection")},
  {18, 0x3000, N_("None (MF)")},
  {18, 0x3001, N_("Auto-selected")},
  {18, 0x3002, N_("Right")},
  {18, 0x3003, N_("Center")},
  {18, 0x3004, N_("Left")},
  {18, 0x4001, N_("Auto AF point selection")},
  {19,  0, N_("Easy shooting")},
  {19,  1, N_("Program")},
  {19,  2, N_("Tv-priority")},
  {19,  3, N_("Av-priority")},
  {19,  4, N_("Manual")},
  {19,  5, N_("A-DEP")},
  {19,  6, N_("M-DEP")},
  {21,   1, N_("Canon EF 50mm f/1.8")},
  {21,   2, N_("Canon EF 28mm f/2.8")},
  {21,   4, N_("Sigma UC Zoom 35-135mm f/4-5.6")},
  {21,   6, N_("Tokina AF193-2 19-35mm f/3.5-4.5")},
  {21,   7, N_("Canon EF 100-300mm F5.6L")},
  {21,  10, N_("Sigma 50mm f/2.8 EX or 28mm f/1.8")},
  {21,  11, N_("Canon EF 35mm f/2")},
  {21,  13, N_("Canon EF 15mm f/2.8")},
  {21,  21, N_("Canon EF 80-200mm f/2.8L")},
  {21,  22, N_("Tokina AT-X280AF PRO 28-80mm F2.8 Aspherical")},
  {21,  26, N_("Cosina 100mm f/3.5 Macro AF")},
  {21,  28, N_("Tamron AF Aspherical 28-200mm f/3.8-5.6")},
  {21,  29, N_("Canon EF 50mm f/1.8 MkII")},
  {21,  31, N_("Tamron SP AF 300mm f/2.8 LD IF")},
  {21,  32, N_("Canon EF 24mm f/2.8 or Sigma 15mm f/2.8 EX Fisheye")},
  {21,  37, N_("Canon EF 35-80mm f/4-5.6")},
  {21,  39, N_("Canon EF 75-300mm f/4-5.6")},
  {21,  40, N_("Canon EF 28-80mm f/3.5-5.6")},
  {21,  43, N_("Canon EF 28-105mm f/4-5.6")},
  {21,  45, N_("Canon EF-S 18-55mm f/3.5-5.6")},
  {21,  52, N_("Canon EF-S 18-55mm f/3.5-5.6 IS II")},
  {21, 124, N_("Canon MP-E 65mm f/2.8 1-5x Macro Photo")},
  {21, 125, N_("Canon TS-E 24mm f/3.5L")},
  {21, 126, N_("Canon TS-E 45mm f/2.8")},
  {21, 127, N_("Canon TS-E 90mm f/2.8")},
  {21, 130, N_("Canon EF 50mm f/1.0L")},
  {21, 131, N_("Sigma 17-35mm f2.8-4 EX Aspherical HSM")},
  {21, 134, N_("Canon EF 600mm f/4L IS")},
  {21, 135, N_("Canon EF 200mm f/1.8L")},
  {21, 136, N_("Canon EF 300mm f/2.8L")},
  {21, 137, N_("Canon EF 85mm f/1.2L")},
  {21, 139, N_("Canon EF 400mm f/2.8L")},
  {21, 141, N_("Canon EF 500mm f/4.5L")},
  {21, 142, N_("Canon EF 300mm f/2.8L IS")},
  {21, 143, N_("Canon EF 500mm f/4L IS")},
  {21, 149, N_("Canon EF 100mm f/2")},
  {21, 150, N_("Sigma 20mm EX f/1.8")},
  {21, 151, N_("Canon EF 200mm f/2.8L")},
  {21, 152, N_("Sigma 10-20mm F4-5.6 or 12-24mm f/4.5-5.6 or 14mm f/2.8")},
  {21, 153, N_("Canon EF 35-350mm f/3.5-5.6L")},
  {21, 155, N_("Canon EF 85mm f/1.8 USM")},
  {21, 156, N_("Canon EF 28-105mm f/3.5-4.5 USM")},
  {21, 160, N_("Canon EF 20-35mm f/3.5-4.5 USM")},
  {21, 161, N_("Canon EF 28-70mm f/2.8L or Sigma 24-70mm EX f/2.8")},
  {21, 165, N_("Canon EF 70-200mm f/2.8 L")},
  {21, 166, N_("Canon EF 70-200mm f/2.8 L + x1.4")},
  {21, 167, N_("Canon EF 70-200mm f/2.8 L + x2")},
  {21, 168, N_("Canon EF 28mm f/1.8 USM")},
  {21, 169, N_("Sigma 15-30mm f/3.5-4.5 EX DG Aspherical")},
  {21, 170, N_("Canon EF 200mm f/2.8L II")},
  {21, 173, N_("Canon EF 180mm Macro f/3.5L or Sigma 180mm EX HSM Macro f/3.5")},
  {21, 174, N_("Canon EF 135mm f/2L")},
  {21, 176, N_("Canon EF 24-85mm f/3.5-4.5 USM")},
  {21, 177, N_("Canon EF 300mm f/4L IS")},
  {21, 178, N_("Canon EF 28-135mm f/3.5-5.6 IS")},
  {21, 180, N_("Canon EF 35mm f/1.4L")},
  {21, 181, N_("Canon EF 100-400mm f/4.5-5.6L IS + x1.4")},
  {21, 182, N_("Canon EF 100-400mm f/4.5-5.6L IS + x2")},
  {21, 183, N_("Canon EF 100-400mm f/4.5-5.6L IS")},
  {21, 184, N_("Canon EF 400mm f/2.8L + x2")},
  {21, 186, N_("Canon EF 70-200mm f/4L")},
  {21, 190, N_("Canon EF 100mm f/2.8 Macro")},
  {21, 191, N_("Canon EF 400mm f/4 DO IS")},
  {21, 197, N_("Canon EF 75-300mm f/4-5.6 IS")},
  {21, 198, N_("Canon EF 50mm f/1.4")},
  {21, 202, N_("Canon EF 28-80 f/3.5-5.6 USM IV")},
  {21, 211, N_("Canon EF 28-200mm f/3.5-5.6")},
  {21, 213, N_("Canon EF 90-300mm f/4.5-5.6")},
  {21, 214, N_("Canon EF-S 18-55mm f/3.5-4.5 USM")},
  {21, 224, N_("Canon EF 70-200mm f/2.8L IS USM")},
  {21, 225, N_("Canon EF 70-200mm f/2.8L IS USM + x1.4")},
  {21, 226, N_("Canon EF 70-200mm f/2.8L IS USM + x2")},
  {21, 229, N_("Canon EF 16-35mm f/2.8L")},
  {21, 230, N_("Canon EF 24-70mm f/2.8L")},
  {21, 231, N_("Canon EF 17-40mm f/4L")},
  {21, 232, N_("Canon EF 70-300mm f/4.5-5.6 DO IS USM")},
  {21, 234, N_("Canon EF-S 17-85mm f4-5.6 IS USM")},
  {21, 235, N_("Canon EF-S10-22mm F3.5-4.5 USM")},
  {21, 236, N_("Canon EF-S60mm F2.8 Macro USM")},
  {21, 237, N_("Canon EF 24-105mm f/4L IS")},
  {21, 238, N_("Canon EF 70-300mm F4-5.6 IS USM")},
  {21, 241, N_("Canon EF 50mm F1.2L USM")},
  {21, 242, N_("Canon EF 70-200mm f/4L IS USM")},
  {21, 251, N_("Canon EF 70-200mm f/2.8L IS II USM")},
  {28, 0, N_("Manual")},
  {28, 1, N_("TTL")},
  {28, 2, N_("A-TTL")},
  {28, 3, N_("E-TTL")},
  {28, 4, N_("FP sync enabled")},
  {28, 7, N_("2nd-curtain sync used")},
  {28, 11, N_("FP sync used")},
  {28, 13, N_("Internal")},
  {28, 14, N_("External")},
  {31,  0, N_("Single")},
  {31,  1, N_("Continuous")},
  {32, 0, N_("Normal AE")},
  {32, 1, N_("Exposure compensation")},
  {32, 2, N_("AE lock")},
  {32, 3, N_("AE lock + exposure compensation")},
  {32, 4, N_("No AE")},
  {33, 0, N_("Off")},
  {33, 1, N_("On")},
  {33, 2, N_("On, shot only")},
  {39, 0, N_("Off")},
  {39, 1, N_("Vivid")},
  {39, 2, N_("Neutral")},
  {39, 3, N_("Smooth")},
  {39, 4, N_("Sepia")},
  {39, 5, N_("Black & white")},
  {39, 6, N_("Custom")},
  {39, 100, N_("My color data")},
  {40, 0, N_("Off")},
  {40, 0x0500, N_("Full")},
  {40, 0x0502, N_("2/3")},
  {40, 0x0504, N_("1/3")},
#endif
  { 0,  0, NULL}
},
entries_focal_length [] = {
#ifndef NO_VERBOSE_TAG_DATA
	{0, 1, N_("Fixed")},
	{0, 2, N_("Zoom")},
#endif
	{0, 0, NULL}
},
entries_settings_2 [] = {
#ifndef NO_VERBOSE_TAG_DATA
  { 6,  0, N_("Auto")},
  { 6,  1, N_("Sunny")},
  { 6,  2, N_("Cloudy")},
  { 6,  3, N_("Tungsten")},
  { 6,  4, N_("Fluorescent")},
  { 6,  5, N_("Flash")},
  { 6,  6, N_("Custom")},
  { 6,  7, N_("Black & white")},
  { 6,  8, N_("Shade")},
  { 6,  9, N_("Manual temperature (Kelvin)")},
  { 6,  10, N_("PC set 1")},
  { 6,  11, N_("PC set 2")},
  { 6,  12, N_("PC set 3")},
  { 6,  14, N_("Daylight fluorescent")},
  { 6,  15, N_("Custom 1")},
  { 6,  16, N_("Custom 2")},
  { 6,  17, N_("Underwater")},
  { 7,  0, N_("Off")},
  { 7,  1, N_("Night scene")},
  { 7,  2, N_("On")},
  { 7,  3, N_("None")},
  { 13,  0x3000, N_("None (MF)")},
  { 13,  0x3001, N_("Right")},
  { 13,  0x3002, N_("Center")},
  { 13,  0x3003, N_("Center-right")},
  { 13,  0x3004, N_("Left")},
  { 13,  0x3005, N_("Left-right")},
  { 13,  0x3006, N_("Left-center")},
  { 13,  0x3007, N_("All")},
  { 15,  0, N_("Off")},
  { 15,  1, N_("On (shot 1)")},
  { 15,  2, N_("On (shot 2)")},
  { 15,  3, N_("On (shot 3)")},
  { 15,  0xffff, N_("On")},
  { 25,  248, N_("EOS high-end")},
  { 25,  250, N_("Compact")},
  { 25,  252, N_("EOS mid-range")},
  { 26,  0, N_("None")},
  { 26,  1, N_("Rotate 90 CW")},
  { 26,  2, N_("Rotate 180")},
  { 26,  3, N_("Rotate 270 CW")},
  { 26,  0xffff, N_("Rotated by software")},
  { 27,  0, N_("Off")},
  { 27,  1, N_("On")}, 
  { 32,  0, N_("Off")},
  { 32,  0x0014, N_("1/3")},
  { 32,  0x008c, N_("2/3")},
  { 32,  0x07d0, N_("Full")},
#endif
  {0, 0, NULL}
},
entries_panorama [] = {
#ifndef NO_VERBOSE_TAG_DATA
	{0, 0, N_("Left to right")},
	{0, 1, N_("Right to left")},
	{0, 2, N_("Bottom to top")},
	{0, 3, N_("Top to bottom")},
	{0, 4, N_("2x2 matrix (clockwise)")},
#endif
	{0, 0, NULL}
},
color_information [] = {
#ifndef NO_VERBOSE_TAG_DATA
  {0, 0, N_("Standard")},
  {0, 1, N_("Manual")},
  {0, 2, N_("Custom")},
  {2, 0, N_("N/A")},
  {2, 1, N_("Lowest")},
  {2, 2, N_("Low")},
  {2, 3, N_("Standard")},
  {2, 4, N_("High")},
  {2, 5, N_("Highest")},
  {7,  0, N_("Auto")},
  {7,  1, N_("Daylight")},
  {7,  2, N_("Cloudy")},
  {7,  3, N_("Tungsten")},
  {7,  4, N_("Fluorescent")},
  {7,  5, N_("Flash")},
  {7,  6, N_("Custom")},
  {7,  7, N_("Black & white")},
  {7,  8, N_("Shade")},
  {7,  9, N_("Manual temperature (Kelvin)")},
  {7, 10, N_("PC set 1")},
  {7, 11, N_("PC set 2")},
  {7, 12, N_("PC set 3")},
  {7, 14, N_("Daylight fluorescent")},
  {7, 15, N_("Custom 1")},
  {7, 16, N_("Custom 2")},
  {7, 17, N_("Underwater")},
  {9, 0x00, N_("None")},
  {9, 0x01, N_("Standard")},
  {9, 0x02, N_("Set 1")},
  {9, 0x03, N_("Set 2")},
  {9, 0x04, N_("Set 3")},
  {9, 0x21, N_("User def. 1")},
  {9, 0x22, N_("User def. 2")},
  {9, 0x23, N_("User def. 3")},
  {9, 0x41, N_("External 1")},
  {9, 0x42, N_("External 2")},
  {9, 0x43, N_("External 3")},
  {9, 0x81, N_("Standard")},
  {9, 0x82, N_("Portrait")},
  {9, 0x83, N_("Landscape")},
  {9, 0x84, N_("Neutral")},
  {9, 0x85, N_("Faithful")},
  {9, 0x86, N_("Monochrome")},
#endif
  {0, 0, NULL}
};

static void
canon_search_table_value (const struct canon_entry_table_t table[],
    unsigned int t, ExifShort vs, char *val, unsigned int maxlen)
{
	unsigned int j;

	/* Search the table for the first matching subtag and value. */
	for (j = 0; table[j].name && ((table[j].subtag < t) ||
			((table[j].subtag == t) && table[j].value <= vs)); j++) {
		if ((table[j].subtag == t) && (table[j].value == vs)) {
			break;
		}
	}
	if ((table[j].subtag == t) && (table[j].value == vs) && table[j].name) {
		/* Matching subtag and value found. */
		strncpy (val, _(table[j].name), maxlen);
	} else {
		/* No matching subtag and/or value found. */
		snprintf (val, maxlen, "0x%04x", vs);
	}
}

static void
canon_search_table_bitfield (const struct canon_entry_table_t table[],
    unsigned int t, ExifShort vs, char *val, unsigned int maxlen)
{
	unsigned int j;

	/* Search the table for the first matching subtag. */
	for (j = 0; table[j].name && (table[j].subtag <= t); j++) {
		if (table[j].subtag == t) {
			break;
		}
	}
	if ((table[j].subtag == t) && table[j].name) {
		unsigned int i, bit, lastbit = 0;

		/*
		 * Search the table for the last matching bit, because
		 * that one needs no additional comma appended.
		 */
		for (i = j; table[i].name && (table[i].subtag == t); i++) {
			bit = table[i].value;
			if ((vs >> bit) & 1) {
				lastbit = bit;
			}
		}
		/* Search the table for all matching bits. */
		for (i = j; table[i].name && (table[i].subtag == t); i++) {
			bit = table[i].value;
			if ((vs >> bit) & 1) {
				strncat(val, _(table[i].name), maxlen - strlen (val));
				if (bit != lastbit) 
					strncat (val, _(", "), maxlen - strlen (val));
			}
		}
	} else {
		/* No matching subtag found. */
		snprintf (val, maxlen, "0x%04x", vs);
	}
}

unsigned int
mnote_canon_entry_count_values (const MnoteCanonEntry *entry)
{
	unsigned int  val;

	if (!entry) return 0;

	switch (entry->tag) {
	case MNOTE_CANON_TAG_FOCAL_LENGTH:
	case MNOTE_CANON_TAG_PANORAMA:
		return entry->components;
	case MNOTE_CANON_TAG_SETTINGS_1:
	case MNOTE_CANON_TAG_SETTINGS_2:
	case MNOTE_CANON_TAG_CUSTOM_FUNCS:
	case MNOTE_CANON_TAG_COLOR_INFORMATION:
		if (entry->format != EXIF_FORMAT_SHORT) return 0;

		val = exif_get_short (entry->data, entry->order);
		/* val is buffer size, i.e. # of values plus 1 */
		return MIN (entry->size - 2, val) / 2;
	default:
		return 1;
	}
}

/*
 * For reference, see Exif 2.1 specification (Appendix C), 
 * or http://en.wikipedia.org/wiki/APEX_system
 */
static double
apex_value_to_aperture (double x)
{
	return pow (2, x / 2.);
}

static double
apex_value_to_shutter_speed(double x)
{
	return 1.0 / pow (2, x);
}

static double
apex_value_to_iso_speed (double x)
{
	return 3.125 * pow (2, x);
}

char *
mnote_canon_entry_get_value (const MnoteCanonEntry *entry, unsigned int t, char *val, unsigned int maxlen)
{
	char buf[128];
	ExifLong vl;
	ExifSLong vsl;
	ExifShort vs, n;
	ExifSShort vss;
	unsigned char *data;
	size_t	size;
	double d;

	if (!entry) 
		return NULL;
	if (maxlen < 1)
		return NULL;

	data = entry->data;
	size = entry->size;

	memset (val, 0, maxlen);
	maxlen--;

	switch (entry->tag) {
	case MNOTE_CANON_TAG_SETTINGS_1:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)

		if (size < 2) return NULL;
		n = exif_get_short (data, entry->order) / 2;
		if (t >= n) return NULL;

		CC (entry->components, n, val, maxlen)

		if (size < 2 + t*2 + 2) return NULL;
		vs = exif_get_short (data + 2 + t * 2, entry->order);
		switch (t) {
		case 1:
			if (!vs) {
				strncpy(val, _("Off"), maxlen);
				break;
			}
			snprintf (val, maxlen, _("%i (ms)"), vs * 100);
			break;
		case 15:
			if (((vs & 0xC000) == 0x4000) && (vs != 0x7FFF)) {
				/* Canon S3 IS - directly specified value */
				snprintf (val, maxlen, "%i", vs & ~0x4000);
			} else {
				/* Standard Canon - index into lookup table */
				canon_search_table_value (entries_settings_1, t, vs, val, maxlen);
			}
			break;
		case 22:
		case 23:
		case 24:
			snprintf (val, maxlen, "%u", vs);
			break;
		case 25:
		case 26:
			snprintf (val, maxlen, "%.2f", apex_value_to_aperture (vs / 32.0));
			break;
		case 28:
			canon_search_table_bitfield(entries_settings_1, t, vs, val, maxlen);
			break;
		case 34:
			snprintf (val, maxlen, "%.2f", vs / 10.0);
			break;
		case 35:
		case 36:
			snprintf (val, maxlen, "%u", vs);
			break;
		default:
			canon_search_table_value (entries_settings_1, t, vs, val, maxlen);
		}
		break;

	case MNOTE_CANON_TAG_FOCAL_LENGTH:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		if (size < t*2 + 2) return NULL;
		vs = exif_get_short (data + t * 2, entry->order);
		switch (t) {
		case 1:
			snprintf (val, maxlen, "%u", vs);
			break;
		case 2:
		case 3:
			snprintf (val, maxlen, _("%.2f mm"), vs * 25.4 / 1000);
			break;
		default:
			canon_search_table_value (entries_focal_length, t, vs, val, maxlen);
		}
		break;

	case MNOTE_CANON_TAG_SETTINGS_2:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		if (size < 2) return NULL;
		n = exif_get_short (data, entry->order) / 2;
		if (t >= n) return NULL;
		CC (entry->components, n, val, maxlen)
		if (size < 2 + t*2 + 2) return NULL;
		vss = exif_get_sshort (data + 2 + t * 2, entry->order);
		switch (t) {
		case 0:
			snprintf (val, maxlen, "%.3f", pow (2, vss / 32.0));
			break;
		case 1:
			snprintf (val, maxlen, "%.0f", apex_value_to_iso_speed (vss / 32.0));
			break;
		case 2:
		case 5:
		case 14:
		case 16:
			snprintf (val, maxlen, _("%.2f EV"), vss / 32.0);
			break;
		case 3:
		case 20:
			snprintf (val, maxlen, "%.2f", apex_value_to_aperture (vss / 32.0));
			break;
		case 4:
		case 21:
			d = apex_value_to_shutter_speed (vss / 32.0);
			if (d < 1)
				snprintf (val, maxlen, _("1/%.0f"), 1.0 / d);
			else
				snprintf (val, maxlen, "%.0f", d);
			break;
		case 8:
			snprintf (val, maxlen, "%u", vss);
			break;
		case 12:
			snprintf (val, maxlen, "%.2f", vss / 32.0);
			break;
		case 18:
		case 19:
			snprintf (val, maxlen, _("%u mm"), vss);
			break;
		case 28:
			if (vss <= 0) {
				strncpy(val, _("Off"), maxlen);
				break;
			}
			snprintf (val, maxlen, _("%i (ms)"), vss * 100);
			break;
		default:
			canon_search_table_value (entries_settings_2, t, vss, val, maxlen);
		}
		break;

	case MNOTE_CANON_TAG_PANORAMA:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		if (size < t*2 + 2) return NULL;
		vs = exif_get_short (data + t * 2, entry->order);
		canon_search_table_value (entries_panorama, t, vs, val, maxlen);
		break;

	case MNOTE_CANON_TAG_OWNER:
		CC (entry->components, 32, val, maxlen)
		/* Fall through; ImageType can have many sizes */
		/* fall through */
	case MNOTE_CANON_TAG_IMAGE_TYPE:
		CF (entry->format, EXIF_FORMAT_ASCII, val, maxlen)
		strncpy (val, (char *)data, MIN (entry->size, maxlen));
		break;

	case MNOTE_CANON_TAG_FIRMWARE:
		CF (entry->format, EXIF_FORMAT_ASCII, val, maxlen)
/*		CC2 (entry->components, 24, 32, val, maxlen); Can also be 22 */
		strncpy (val, (char *)data, MIN (entry->size, maxlen));
		break;

	case MNOTE_CANON_TAG_IMAGE_NUMBER:
		CF (entry->format, EXIF_FORMAT_LONG, val, maxlen)
		CC (entry->components, 1, val, maxlen)
		if (size < 4) return NULL;
		vl = exif_get_long (data, entry->order);
		snprintf (val, maxlen, "%03lu-%04lu",
				(unsigned long) vl/10000,
				(unsigned long) vl%10000);
		break;

	case MNOTE_CANON_TAG_SERIAL_NUMBER:
		CF (entry->format, EXIF_FORMAT_LONG, val, maxlen)
		CC (entry->components, 1, val, maxlen)
		if (size < 4) return NULL;
		vl = exif_get_long (data, entry->order);
		snprintf (val, maxlen, "%04X-%05d", (int)vl>>16,(int)vl&0xffff);
		break;

	case MNOTE_CANON_TAG_CUSTOM_FUNCS:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		if (size < 2) return NULL;
		n = exif_get_short (data, entry->order) / 2;
		if (t >= n) return NULL;
		CC (entry->components, n, val, maxlen)
		if (size < 2 + t*2 + 2) return NULL;
		vs = exif_get_short (data + 2 + t * 2, entry->order);
		snprintf (buf, sizeof (buf), "%u", vs);
		strncat (val, buf, maxlen - strlen (val));
		break;

	case MNOTE_CANON_TAG_COLOR_INFORMATION:
		CF (entry->format, EXIF_FORMAT_SHORT, val, maxlen)
		if (size < 2) return NULL;
		n = exif_get_short (data, entry->order) / 2;
		if (t >= n) return NULL;
		CC (entry->components, n, val, maxlen)
		if (size < 2 + t*2 + 2) return NULL;
		vs = exif_get_short (data + 2 + t * 2, entry->order);
		canon_search_table_value (color_information, t, vs, val, maxlen);
		break;

	default:
		switch (entry->format) {
		case EXIF_FORMAT_SHORT:
		  {
			size_t i, len = strlen(val);
			for(i=0; i<entry->components; i++) {
				if (size < 2)
					break;
				vs = exif_get_short (data, entry->order);
				snprintf (val+len, maxlen-len, "%hu ", vs);
				len = strlen(val);
				data += 2;
				size -= 2;
			}
		  }
		  break;
		case EXIF_FORMAT_SSHORT:
		  {
			size_t i, len = strlen(val);
			for(i=0; i<entry->components; i++) {
				if (size < 2)
					break;
				vss = exif_get_sshort (data, entry->order);
				snprintf (val+len, maxlen-len, "%hi ", vss);
				data += 2;
				size -= 2;
			}
		  }
		  break;
		case EXIF_FORMAT_LONG:
		  {
			size_t i, len = strlen(val);
			for(i=0; i<entry->components; i++) {
				if (size < 4)
					break;
				vl = exif_get_long (data, entry->order);
				snprintf (val+len, maxlen-len, "%lu ", (long unsigned) vl);
				data += 4;
				size -= 4;
			}
		  }
		  break;
		case EXIF_FORMAT_SLONG:
		  {
			size_t i, len = strlen(val);
			for(i=0; i<entry->components; i++) {
				if (size < 4)
					break;
				vsl = exif_get_slong (data, entry->order);
				snprintf (val+len, maxlen-len, "%li ", (long int) vsl);
				data += 4;
				size -= 4;
			}
		  }
		  break;
		case EXIF_FORMAT_ASCII:
			strncpy (val, (char *)data, MIN (entry->size, maxlen));
			break;
		default:
		  snprintf (val, maxlen, _("%i bytes unknown data"),
			  entry->size);
		  break;
		}
		break;
	}
	return val;
}

// libexif/canon/exif-mnote-data-canon.c

/* exif-mnote-data-canon.c
 *
 * Copyright (c) 2002, 2003 Lutz Mueller <lutz@users.sourceforge.net>
 * Copyright (c) 2003 Matthieu Castet <mat-c@users.sourceforge.net>
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

/* Total size limit to prevent abuse by DoS */
#define FAILSAFE_SIZE_MAX 1000000L

static void
exif_mnote_data_canon_clear (ExifMnoteDataCanon *n)
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
exif_mnote_data_canon_free (ExifMnoteData *n)
{
	if (!n) return;

	exif_mnote_data_canon_clear ((ExifMnoteDataCanon *) n);
}

static void
exif_mnote_data_canon_get_tags (ExifMnoteDataCanon *dc, unsigned int n,
		unsigned int *m, unsigned int *s)
{
	unsigned int from = 0, to;

	if (s) *s = 0;

	if (!dc || !m) return;
	for (*m = 0; *m < dc->count; (*m)++) {
		to = from + mnote_canon_entry_count_values (&dc->entries[*m]);
		if (to > n) {
			if (s) *s = n - from;
			break;
		}
		from = to;
	}
}

static char *
exif_mnote_data_canon_get_value (ExifMnoteData *note, unsigned int n, char *val, unsigned int maxlen)
{
	ExifMnoteDataCanon *dc = (ExifMnoteDataCanon *) note;
	unsigned int m, s;

	if (!dc) return NULL;
	exif_mnote_data_canon_get_tags (dc, n, &m, &s);
	if (m >= dc->count) return NULL;
	return mnote_canon_entry_get_value (&dc->entries[m], s, val, maxlen);
}

static void
exif_mnote_data_canon_set_byte_order (ExifMnoteData *d, ExifByteOrder o)
{
	ExifByteOrder o_orig;
	ExifMnoteDataCanon *n = (ExifMnoteDataCanon *) d;
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
exif_mnote_data_canon_set_offset (ExifMnoteData *n, unsigned int o)
{
	if (n) ((ExifMnoteDataCanon *) n)->offset = o;
}

static void
exif_mnote_data_canon_save (ExifMnoteData *ne, 
	unsigned char **buf, unsigned int *buf_size)
{
	ExifMnoteDataCanon *n = (ExifMnoteDataCanon *) ne;
	size_t i, o, s, doff;
	unsigned char *t;
	size_t ts;

	if (!n || !buf || !buf_size) return;

	/*
	 * Allocate enough memory for all entries and the number
	 * of entries.
	 */
	*buf_size = 2 + n->count * 12 + 4;
	*buf = exif_mem_alloc (ne->mem, sizeof (char) * *buf_size);
	if (!*buf) {
		EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteCanon", *buf_size);
		return;
	}

	/* Save the number of entries */
	exif_set_short (*buf, n->order, (ExifShort) n->count);
	
	/* Save each entry */
	for (i = 0; i < n->count; i++) {
		o = 2 + i * 12;
		exif_set_short (*buf + o + 0, n->order, (ExifShort) n->entries[i].tag);
		exif_set_short (*buf + o + 2, n->order, (ExifShort) n->entries[i].format);
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
			ts = *buf_size + s;

			/* Ensure even offsets. Set padding bytes to 0. */
			if (s & 1) ts += 1;
			t = exif_mem_realloc (ne->mem, *buf,
						 sizeof (char) * ts);
			if (!t) {
				EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteCanon", ts);
				return;
			}
			*buf = t;
			*buf_size = ts;
			doff = *buf_size - s;
			if (s & 1) { doff--; *(*buf + *buf_size - 1) = '\0'; }
			exif_set_long (*buf + o, n->order, n->offset + doff);
		} else
			doff = o;

		/*
		 * Write the data. Fill unneeded bytes with 0. Do not
		 * crash if data is NULL.
		 */
		if (!n->entries[i].data) memset (*buf + doff, 0, s);
		else memcpy (*buf + doff, n->entries[i].data, s);
		if (s < 4) memset (*buf + doff + s, 0, (4 - s));
	}
}

/* XXX
 * FIXME: exif_mnote_data_canon_load() may fail and there is no
 *        semantics to express that.
 *        See bug #1054323 for details, especially the comment by liblit
 *        after it has supposedly been fixed:
 *
 *        https://sourceforge.net/tracker/?func=detail&aid=1054323&group_id=12272&atid=112272
 *        Unfortunately, the "return" statements aren't commented at
 *        all, so it isn't trivial to find out what is a normal
 *        return, and what is a reaction to an error condition.
 */

static void
exif_mnote_data_canon_load (ExifMnoteData *ne,
	const unsigned char *buf, unsigned int buf_size)
{
	ExifMnoteDataCanon *n = (ExifMnoteDataCanon *) ne;
	ExifShort c;
	size_t i, tcount, o, datao;
	long failsafe_size = 0;

	if (!n) return;

	if (!buf || !buf_size) {
		exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteCanon", "Short MakerNote");
		return;
	}
	if (CHECKOVERFLOW(n->offset, buf_size, 8)) {
		exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteCanon", "Short MakerNote");
		return;
	}
	datao = 6 + n->offset;

	/* Read the number of tags */
	c = exif_get_short (buf + datao, n->order);
	datao += 2;
	/* Just use an arbitrary max tag limit here to avoid needing to much memory or time. There are 24 named tags currently.
	 * current 2020 camera EOS M6 Mark 2 had 156 entries.
	 * The format allows specifying the same range of memory as often as it can, so this multiplies quickly. */
	if (c > 250) {
		exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA, "ExifMnoteCanon", "Too much tags (%d) in Canon MakerNote", c);
		return;
	}

	/* Remove any old entries */
	exif_mnote_data_canon_clear (n);

	/* Reserve enough space for all the possible MakerNote tags */
	n->entries = exif_mem_alloc (ne->mem, sizeof (MnoteCanonEntry) * c);
	if (!n->entries) {
		EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteCanon", sizeof (MnoteCanonEntry) * c);
		return;
	}

	/* Parse the entries */
	tcount = 0;
	for (i = c, o = datao; i; --i, o += 12) {
		size_t s;

		memset(&n->entries[tcount], 0, sizeof(MnoteCanonEntry));
		if (CHECKOVERFLOW(o,buf_size,12)) {
			exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA,
				"ExifMnoteCanon", "Short MakerNote");
			break;
		}

		n->entries[tcount].tag        = exif_get_short (buf + o, n->order);
		n->entries[tcount].format     = exif_get_short (buf + o + 2, n->order);
		n->entries[tcount].components = exif_get_long (buf + o + 4, n->order);
		n->entries[tcount].order      = n->order;

		exif_log (ne->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteCanon",
			"Loading entry 0x%x ('%s')...", n->entries[tcount].tag,
			 mnote_canon_tag_get_name (n->entries[tcount].tag));

		/* Check if we overflow the multiplication. Use buf_size as the max size for integer overflow detection,
		 * we will check the buffer sizes closer later. */
		if (	exif_format_get_size (n->entries[tcount].format) &&
			buf_size / exif_format_get_size (n->entries[tcount].format) < n->entries[tcount].components
		) {
			exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA,
				  "ExifMnoteCanon", "Tag size overflow detected (%u * %lu)", exif_format_get_size (n->entries[tcount].format), n->entries[tcount].components);
			continue;
		}

		/*
		 * Size? If bigger than 4 bytes, the actual data is not
		 * in the entry but somewhere else (offset).
		 */
		s = exif_format_get_size (n->entries[tcount].format) * 
								  n->entries[tcount].components;
		n->entries[tcount].size = s;
		if (!s) {
			exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA,
				  "ExifMnoteCanon",
				  "Invalid zero-length tag size");
			continue;

		} else {
			size_t dataofs = o + 8;
			if (s > 4) dataofs = exif_get_long (buf + dataofs, n->order) + 6;

			if (CHECKOVERFLOW(dataofs, buf_size, s)) {
				exif_log (ne->log, EXIF_LOG_CODE_DEBUG,
					"ExifMnoteCanon",
					"Tag data past end of buffer (%u > %u)",
					(unsigned)(dataofs + s), buf_size);
				continue;
			}

			n->entries[tcount].data = exif_mem_alloc (ne->mem, s);
			if (!n->entries[tcount].data) {
				EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteCanon", s);
				continue;
			}
			memcpy (n->entries[tcount].data, buf + dataofs, s);
		}

		/* Track the size of decoded tag data. A malicious file could
		 * be crafted to cause extremely large values here without
		 * tripping any buffer range checks.  This is especially bad
		 * with the libexif representation of Canon MakerNotes because
		 * some arrays are turned into individual tags that the
		 * application must loop around. */
		failsafe_size += mnote_canon_entry_count_values(&n->entries[tcount]);

		if (failsafe_size > FAILSAFE_SIZE_MAX) {
			/* Abort if the total size of the data in the tags extraordinarily large, */
			exif_mem_free (ne->mem, n->entries[tcount].data);
			exif_log (ne->log, EXIF_LOG_CODE_CORRUPT_DATA,
					  "ExifMnoteCanon", "Failsafe tag size overflow (%lu > %ld)",
					  failsafe_size, FAILSAFE_SIZE_MAX);
			break;
		}

		/* Tag was successfully parsed */
		++tcount;
	}
	/* Store the count of successfully parsed tags */
	n->count = tcount;
}

static unsigned int
exif_mnote_data_canon_count (ExifMnoteData *n)
{
	ExifMnoteDataCanon *dc = (ExifMnoteDataCanon *) n;
	unsigned int i, c;

	for (i = c = 0; dc && (i < dc->count); i++)
		c += mnote_canon_entry_count_values (&dc->entries[i]);
	return c;
}

static unsigned int
exif_mnote_data_canon_get_id (ExifMnoteData *d, unsigned int i)
{
	ExifMnoteDataCanon *dc = (ExifMnoteDataCanon *) d;
	unsigned int m;

	if (!dc) return 0;
	exif_mnote_data_canon_get_tags (dc, i, &m, NULL);
	if (m >= dc->count) return 0;
	return dc->entries[m].tag;
}

static const char *
exif_mnote_data_canon_get_name (ExifMnoteData *note, unsigned int i)
{
	ExifMnoteDataCanon *dc = (ExifMnoteDataCanon *) note;
	unsigned int m, s;

	if (!dc) return NULL;
	exif_mnote_data_canon_get_tags (dc, i, &m, &s);
	if (m >= dc->count) return NULL;
	return mnote_canon_tag_get_name_sub (dc->entries[m].tag, s, dc->options);
}

static const char *
exif_mnote_data_canon_get_title (ExifMnoteData *note, unsigned int i)
{
	ExifMnoteDataCanon *dc = (ExifMnoteDataCanon *) note;
	unsigned int m, s;

	if (!dc) return NULL;
	exif_mnote_data_canon_get_tags (dc, i, &m, &s);
	if (m >= dc->count) return NULL;
	return mnote_canon_tag_get_title_sub (dc->entries[m].tag, s, dc->options);
}

static const char *
exif_mnote_data_canon_get_description (ExifMnoteData *note, unsigned int i)
{
	ExifMnoteDataCanon *dc = (ExifMnoteDataCanon *) note;
	unsigned int m;

	if (!dc) return NULL;
	exif_mnote_data_canon_get_tags (dc, i, &m, NULL);
	if (m >= dc->count) return NULL;
	return mnote_canon_tag_get_description (dc->entries[m].tag);
}

int
exif_mnote_data_canon_identify (const ExifData *ed, const ExifEntry *e)
{
	char value[8];

	ExifEntry *em = exif_data_get_entry (ed, EXIF_TAG_MAKE);
	if (!em) 
		return 0;

	(void) e;  /* unused */
	return !strcmp (exif_entry_get_value (em, value, sizeof (value)), "Canon");
}

ExifMnoteData *
exif_mnote_data_canon_new (ExifMem *mem, ExifDataOption o)
{
	ExifMnoteData *d;
	ExifMnoteDataCanon *dc;

	if (!mem) return NULL;

	d = exif_mem_alloc (mem, sizeof (ExifMnoteDataCanon));
	if (!d)
		return NULL;

	exif_mnote_data_construct (d, mem);

	/* Set up function pointers */
	d->methods.free            = exif_mnote_data_canon_free;
	d->methods.set_byte_order  = exif_mnote_data_canon_set_byte_order;
	d->methods.set_offset      = exif_mnote_data_canon_set_offset;
	d->methods.load            = exif_mnote_data_canon_load;
	d->methods.save            = exif_mnote_data_canon_save;
	d->methods.count           = exif_mnote_data_canon_count;
	d->methods.get_id          = exif_mnote_data_canon_get_id;
	d->methods.get_name        = exif_mnote_data_canon_get_name;
	d->methods.get_title       = exif_mnote_data_canon_get_title;
	d->methods.get_description = exif_mnote_data_canon_get_description;
	d->methods.get_value       = exif_mnote_data_canon_get_value;

	dc = (ExifMnoteDataCanon*)d;
	dc->options = o;
	return d;
}

}
