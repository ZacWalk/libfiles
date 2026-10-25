// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif (libexif/olympus): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.

// Purpose: libexif's olympus maker note decoder: how it recognises the note, reads its entries and
// names and renders each value.

#include "libexif_makernote.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4996) // strcpy, strncpy and strncat, as libexif calls them
#pragma warning(disable : 4244 4267 4100 4189 4456 4457 4459 4702) // and its implicit narrowing and leftovers
#endif

namespace libfiles::detail::libexif
{
// libexif/olympus/mnote-olympus-tag.h

/* mnote-olympus-tag.h
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



typedef unsigned int _MnoteOlympusTag;
enum : unsigned int {

	/* Nikon v.2 */
	MNOTE_NIKON_TAG_FIRMWARE                = 0x0001,
	MNOTE_NIKON_TAG_ISO                     = 0x0002,
	MNOTE_NIKON_TAG_COLORMODE1              = 0x0003,
	MNOTE_NIKON_TAG_QUALITY                 = 0x0004,
	MNOTE_NIKON_TAG_WHITEBALANCE            = 0x0005,
	MNOTE_NIKON_TAG_SHARPENING              = 0x0006,
	MNOTE_NIKON_TAG_FOCUSMODE               = 0x0007,
	MNOTE_NIKON_TAG_FLASHSETTING            = 0x0008,
	MNOTE_NIKON_TAG_FLASHMODE               = 0x0009,
	MNOTE_NIKON_TAG_WHITEBALANCEFINE        = 0x000b,
	MNOTE_NIKON_TAG_WHITEBALANCERB          = 0x000c,
	MNOTE_NIKON_TAG_PROGRAMSHIFT            = 0x000d,
	MNOTE_NIKON_TAG_EXPOSUREDIFF            = 0x000e,
	MNOTE_NIKON_TAG_ISOSELECTION            = 0x000f,
	MNOTE_NIKON_TAG_PREVIEWIMAGE_IFD_POINTER= 0x0011,
	MNOTE_NIKON_TAG_FLASHEXPCOMPENSATION    = 0x0012,
	MNOTE_NIKON_TAG_ISO2                    = 0x0013,
	MNOTE_NIKON_TAG_IMAGEBOUNDARY           = 0x0016,
	MNOTE_NIKON_TAG_EXTERNALFLASHEXPCOMPENSATION = 0x0017,
	MNOTE_NIKON_TAG_FLASHEXPOSUREBRACKETVAL = 0x0018,
	MNOTE_NIKON_TAG_EXPOSUREBRACKETVAL      = 0x0019,
	MNOTE_NIKON_TAG_CROPHISPEED      	= 0x001b,
	MNOTE_NIKON_TAG_EXPOSURETUNING      	= 0x001c,
	MNOTE_NIKON_TAG_SERIALNUMBER      	= 0x001d,
	MNOTE_NIKON_TAG_COLORSPACE      	= 0x001e,
	MNOTE_NIKON_TAG_VRINFO      		= 0x001f,
	MNOTE_NIKON_TAG_IMAGEAUTHENTICATION 	= 0x0020,
	MNOTE_NIKON_TAG_FACEDETECT 		= 0x0021,
	MNOTE_NIKON_TAG_ACTIVEDLIGHTING 	= 0x0022,
	MNOTE_NIKON_TAG_PICTURECONTROLDATA 	= 0x0023,
	MNOTE_NIKON_TAG_WORLDTIME 		= 0x0024,
	MNOTE_NIKON_TAG_ISOINFO 		= 0x0025,
	MNOTE_NIKON_TAG_VIGNETTECONTROL 	= 0x002a,
	MNOTE_NIKON_TAG_DISTORTINFO 		= 0x002b,
	MNOTE_NIKON_TAG_SHUTTERMODE 		= 0x0034,
	MNOTE_NIKON_TAG_HDRINFO 		= 0x0035,
	MNOTE_NIKON_TAG_MECHANICALSHUTTERCOUNT 	= 0x0037,
	MNOTE_NIKON_TAG_LOCATIONINFO 		= 0x0039,
	MNOTE_NIKON_TAG_BLACKLEVEL 		= 0x003d,
	MNOTE_NIKON_TAG_IMAGESIZERAW 		= 0x003e,
	MNOTE_NIKON_TAG_CROPAREA 		= 0x0045,
	MNOTE_NIKON_TAG_NIKONSETTINGS 		= 0x004e,
	MNOTE_NIKON_TAG_COLORTEMPERATUREAUTO 	= 0x004f,
	MNOTE_NIKON_TAG_IMAGEADJUSTMENT         = 0x0080,
	MNOTE_NIKON_TAG_TONECOMPENSATION        = 0x0081,
	MNOTE_NIKON_TAG_ADAPTER                 = 0x0082,
	MNOTE_NIKON_TAG_LENSTYPE                = 0x0083,
	MNOTE_NIKON_TAG_LENS                    = 0x0084,
	MNOTE_NIKON_TAG_MANUALFOCUSDISTANCE     = 0x0085,
	MNOTE_NIKON_TAG_DIGITALZOOM             = 0x0086,
	MNOTE_NIKON_TAG_FLASHUSED               = 0x0087,
	MNOTE_NIKON_TAG_AFFOCUSPOSITION         = 0x0088,
	MNOTE_NIKON_TAG_BRACKETING              = 0x0089,	/* shootingmode? */
	MNOTE_NIKON_TAG_UNKNOWN_0X008A          = 0x008a,
	MNOTE_NIKON_TAG_LENS_FSTOPS             = 0x008b,
	MNOTE_NIKON_TAG_CURVE                   = 0x008c,
	MNOTE_NIKON_TAG_COLORMODE               = 0x008d,
	MNOTE_NIKON_TAG_LIGHTTYPE               = 0x0090,
	MNOTE_NIKON_TAG_SHOTINFO                = 0x0091,
	MNOTE_NIKON_TAG_HUE                     = 0x0092,
	MNOTE_NIKON_TAG_SATURATION              = 0x0094,
	MNOTE_NIKON_TAG_NOISEREDUCTION          = 0x0095,
	MNOTE_NIKON_TAG_COLORBALANCE            = 0x0097,
	MNOTE_NIKON_TAG_LENSDATA          	= 0x0098,
	MNOTE_NIKON_TAG_SENSORPIXELSIZE         = 0x009a,
	MNOTE_NIKON_TAG_UNKNOWN_0X009B          = 0x009b,
	MNOTE_NIKON_TAG_RETOUCHHISTORY          = 0x009e,
	MNOTE_NIKON_TAG_SERIALNUMBER2		= 0x00a0,
	MNOTE_NIKON_TAG_IMAGE_DATASIZE          = 0x00a2,
	MNOTE_NIKON_TAG_UNKNOWN_0X00A3          = 0x00a3,
	MNOTE_NIKON_TAG_TOTALPICTURES           = 0x00a7,	/* shuttercount */
	MNOTE_NIKON_TAG_FLASHINFO          	= 0x00a8,
	MNOTE_NIKON_TAG_OPTIMIZATION            = 0x00a9,
	MNOTE_NIKON_TAG_SATURATION2             = 0x00aa,
	MNOTE_NIKON_TAG_VARIPROGRAM             = 0x00ab,
	MNOTE_NIKON_TAG_MULTIEXPOSURE           = 0x00b0,
	MNOTE_NIKON_TAG_HIGHISONR           	= 0x00b1,
	MNOTE_NIKON_TAG_TONINGEFFECT            = 0x00b3,
	MNOTE_NIKON_TAG_POWERUPTIME             = 0x00b6,
	MNOTE_NIKON_TAG_AFINFO2             	= 0x00b7,
	MNOTE_NIKON_TAG_FILEINFO             	= 0x00b8,
	MNOTE_NIKON_TAG_RETOUCHINFO            	= 0x00bb,
	MNOTE_NIKON_TAG_CAPTUREEDITORDATA       = 0x0e01,
	MNOTE_NIKON_TAG_CAPTUREEDITORVER	= 0x0e09,
	MNOTE_NIKON_TAG_UNKNOWN_0X0E0E		= 0x0e0e,
	MNOTE_NIKON_TAG_UNKNOWN_0X0E10		= 0x0e10,

	/* Nikon v1: real values + our proprietary base to distinguish from v2 */
	MNOTE_NIKON1_TAG_BASE                   = 0x8000,
	MNOTE_NIKON1_TAG_UNKNOWN_0X0002         = 0x0002 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_QUALITY                = 0x0003 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_COLORMODE              = 0x0004 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_IMAGEADJUSTMENT        = 0x0005 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_CCDSENSITIVITY         = 0x0006 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_WHITEBALANCE           = 0x0007 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_FOCUS                  = 0x0008 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_UNKNOWN_0X0009         = 0x0009 + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_DIGITALZOOM            = 0x000a + MNOTE_NIKON1_TAG_BASE,
	MNOTE_NIKON1_TAG_CONVERTER              = 0x000b + MNOTE_NIKON1_TAG_BASE,

	/* Olympus and some Sanyo */
	MNOTE_OLYMPUS_TAG_THUMBNAILIMAGE	= 0x0100,
	MNOTE_OLYMPUS_TAG_MODE			= 0x0200,
	MNOTE_OLYMPUS_TAG_QUALITY		= 0x0201,
	MNOTE_OLYMPUS_TAG_MACRO			= 0x0202,
	MNOTE_OLYMPUS_TAG_BWMODE		= 0x0203,
	MNOTE_OLYMPUS_TAG_DIGIZOOM		= 0x0204,
	MNOTE_OLYMPUS_TAG_FOCALPLANEDIAGONAL	= 0x0205,
	MNOTE_OLYMPUS_TAG_LENSDISTORTION	= 0x0206,
	MNOTE_OLYMPUS_TAG_VERSION		= 0x0207,
	MNOTE_OLYMPUS_TAG_INFO			= 0x0208,
	MNOTE_OLYMPUS_TAG_ID			= 0x0209,
	MNOTE_OLYMPUS_TAG_PRECAPTUREFRAMES	= 0x0300,
	MNOTE_OLYMPUS_TAG_WHITEBOARD		= 0x0301,
	MNOTE_OLYMPUS_TAG_ONETOUCHWB		= 0x0302,
	MNOTE_OLYMPUS_TAG_WHITEBALANCEBRACKET	= 0x0303,
	MNOTE_OLYMPUS_TAG_WHITEBALANCEBIAS	= 0x0304,
	MNOTE_OLYMPUS_TAG_DATADUMP		= 0x0f00,
	MNOTE_OLYMPUS_TAG_UNKNOWN_4		= 0x0f04,
	MNOTE_OLYMPUS_TAG_SHUTTERSPEED		= 0x1000,
	MNOTE_OLYMPUS_TAG_ISOVALUE		= 0x1001,
	MNOTE_OLYMPUS_TAG_APERTUREVALUE		= 0x1002,
	MNOTE_OLYMPUS_TAG_BRIGHTNESSVALUE	= 0x1003,
	MNOTE_OLYMPUS_TAG_FLASHMODE		= 0x1004,
	MNOTE_OLYMPUS_TAG_FLASHDEVICE		= 0x1005,
	MNOTE_OLYMPUS_TAG_EXPOSURECOMP		= 0x1006,
	MNOTE_OLYMPUS_TAG_SENSORTEMPERATURE	= 0x1007,
	MNOTE_OLYMPUS_TAG_LENSTEMPERATURE	= 0x1008,
	MNOTE_OLYMPUS_TAG_LIGHTCONDITION	= 0x1009,
	MNOTE_OLYMPUS_TAG_FOCUSRANGE		= 0x100a,
	MNOTE_OLYMPUS_TAG_MANFOCUS		= 0x100b,
	MNOTE_OLYMPUS_TAG_FOCUSDIST		= 0x100c,
	MNOTE_OLYMPUS_TAG_ZOOMSTEPCOUNT		= 0x100d,
	MNOTE_OLYMPUS_TAG_FOCUSSTEPCOUNT	= 0x100e,
	MNOTE_OLYMPUS_TAG_SHARPNESS		= 0x100f,
	MNOTE_OLYMPUS_TAG_FLASHCHARGELEVEL	= 0x1010,
	MNOTE_OLYMPUS_TAG_COLORMATRIX		= 0x1011,
	MNOTE_OLYMPUS_TAG_BLACKLEVEL		= 0x1012,
	MNOTE_OLYMPUS_TAG_WBALANCE		= 0x1015,
	MNOTE_OLYMPUS_TAG_REDBALANCE		= 0x1017,
	MNOTE_OLYMPUS_TAG_BLUEBALANCE		= 0x1018,
	MNOTE_OLYMPUS_TAG_COLORMATRIXNUMBER	= 0x1019,
	MNOTE_OLYMPUS_TAG_SERIALNUMBER2		= 0x101a,
	MNOTE_OLYMPUS_TAG_FLASHEXPOSURECOMP	= 0x1023,
	MNOTE_OLYMPUS_TAG_INTERNALFLASHTABLE	= 0x1024,
	MNOTE_OLYMPUS_TAG_EXTERNALFLASHGVALUE	= 0x1025,
	MNOTE_OLYMPUS_TAG_EXTERNALFLASHBOUNCE	= 0x1026,
	MNOTE_OLYMPUS_TAG_EXTERNALFLASHZOOM	= 0x1027,
	MNOTE_OLYMPUS_TAG_EXTERNALFLASHMODE	= 0x1028,
	MNOTE_OLYMPUS_TAG_CONTRAST		= 0x1029,
	MNOTE_OLYMPUS_TAG_SHARPNESSFACTOR	= 0x102a,
	MNOTE_OLYMPUS_TAG_COLORCONTROL		= 0x102b,
	MNOTE_OLYMPUS_TAG_IMAGEWIDTH		= 0x102e,
	MNOTE_OLYMPUS_TAG_IMAGEHEIGHT		= 0x102f,
	MNOTE_OLYMPUS_TAG_SCENEDETECT		= 0x1030,
	MNOTE_OLYMPUS_TAG_COMPRESSIONRATIO	= 0x1034,
	MNOTE_OLYMPUS_TAG_PREVIEWIMAGEVALID	= 0x1035,
	MNOTE_OLYMPUS_TAG_AFRESULT		= 0x1038,
	MNOTE_OLYMPUS_TAG_CCDSCANMODE		= 0x1039,
	MNOTE_OLYMPUS_TAG_NOISEREDUCTION	= 0x103a,
	MNOTE_OLYMPUS_TAG_INFINITYLENSSTEP	= 0x103b,
	MNOTE_OLYMPUS_TAG_NEARLENSSTEP		= 0x103c,
	MNOTE_OLYMPUS_TAG_LIGHTVALUECENTER	= 0x103d,
	MNOTE_OLYMPUS_TAG_LIGHTVALUEPERIPHERY	= 0x103e,

	/* Epson */
	MNOTE_EPSON_TAG_IMAGE_WIDTH		= 0x020b,
	MNOTE_EPSON_TAG_IMAGE_HEIGHT		= 0x020c,
	MNOTE_EPSON_TAG_SOFTWARE		= 0x020d,

	/* Sanyo */
	MNOTE_SANYO_TAG_SEQUENTIALSHOT		= 0x020e,
	MNOTE_SANYO_TAG_WIDERANGE		= 0x020f,
	MNOTE_SANYO_TAG_COLORADJUSTMENTMODE	= 0x0210,
	MNOTE_SANYO_TAG_FOCUSMODE		= 0x0212,
	MNOTE_SANYO_TAG_QUICKSHOT		= 0x0213,
	MNOTE_SANYO_TAG_SELFTIMER		= 0x0214,
	MNOTE_SANYO_TAG_VOICEMEMO		= 0x0216,
	MNOTE_SANYO_TAG_RECORDSHUTTERRELEASE	= 0x0217,
	MNOTE_SANYO_TAG_FLICKERREDUCE		= 0x0218,
	MNOTE_SANYO_TAG_OPTICALZOOM		= 0x0219,
	MNOTE_SANYO_TAG_CCDSENSITIVITY		= 0x021a,
	MNOTE_SANYO_TAG_DIGITALZOOM		= 0x021b,
	MNOTE_SANYO_TAG_LIGHTSOURCESPECIAL	= 0x021d,
	MNOTE_SANYO_TAG_RESAVED			= 0x021e,
	MNOTE_SANYO_TAG_SCENESELECT		= 0x021f,
	MNOTE_SANYO_TAG_MANUALFOCUSDISTANCE	= 0x0223,
	MNOTE_SANYO_TAG_SEQUENCESHOTINTERVAL	= 0x0224
};
typedef unsigned int MnoteOlympusTag;

/* Don't use these definitions. They are here for compatibility only. */
#define MNOTE_OLYMPUS_TAG_UNKNOWN_1	MNOTE_OLYMPUS_TAG_BWMODE
#define MNOTE_OLYMPUS_TAG_UNKNOWN_2	MNOTE_OLYMPUS_TAG_FOCALPLANEDIAGONAL
#define MNOTE_OLYMPUS_TAG_UNKNOWN_3	MNOTE_OLYMPUS_TAG_LENSDISTORTION
#define MNOTE_OLYMPUS_TAG_UNKNOWN_5	MNOTE_OLYMPUS_TAG_DATADUMP
#define MNOTE_NIKON_TAG_PREVIEWIMAGE	MNOTE_NIKON_TAG_PREVIEWIMAGE_IFD_POINTER

/*! Return a textual name of the given tag within the Olympus-style MakerNote.
 * The name is a short, unique, non-localized text string containing only
 * US-ASCII alphanumeric characters.
 *
 * \param[in] tag Olympus-style MakerNote tag
 * \return textual name of the tag, or NULL if the tag is unknown
 */
const char *mnote_olympus_tag_get_name        (MnoteOlympusTag tag);

/*! Return a textual title of the given tag within the Olympus-style MakerNote.
 * The title is a short, localized description of the tag.
 *
 * \param[in] tag Olympus-style MakerNote tag
 * \return textual title of the tag, or NULL if the tag is unknown
 */
const char *mnote_olympus_tag_get_title       (MnoteOlympusTag tag);

/*! Return a verbose textual description of the given tag within the
 * Olympus-style MakerNote.
 * The description is a verbose, localized description of the tag.
 *
 * \param[in] tag EXIF tag
 * \return textual description of the tag, or NULL if the tag is unknown
 */
const char *mnote_olympus_tag_get_description (MnoteOlympusTag tag);

// libexif/olympus/mnote-olympus-entry.h

/* mnote-olympus-entry.h
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



typedef struct _MnoteOlympusEntry        MnoteOlympusEntry;

struct _MnoteOlympusEntry {
	MnoteOlympusTag tag;
	ExifFormat format;
	unsigned long components;

	unsigned char *data;
	unsigned int size;

	ExifByteOrder order;
};

char *mnote_olympus_entry_get_value (MnoteOlympusEntry *entry, char *val, unsigned int maxlen);

// libexif/olympus/exif-mnote-data-olympus.h

/* mnote-olympus-data.h
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



typedef unsigned int OlympusVersion;
enum : unsigned int {
	unrecognized = 0,
	nikonV1 = 1,
	nikonV2 = 2,
	olympusV1 = 3,
	olympusV2 = 4,
	sanyoV1 = 5,
	epsonV1 = 6,
	nikonV0 = 7
};


typedef struct _ExifMnoteDataOlympus ExifMnoteDataOlympus;

struct _ExifMnoteDataOlympus {
	ExifMnoteData parent;

	MnoteOlympusEntry *entries;
	unsigned int count;

	ExifByteOrder order;
	unsigned int offset;
	OlympusVersion version;
};

/*! Detect if MakerNote is recognized as one handled by the Olympus module.
 * 
 * \param[in] ed image #ExifData to identify as as an Olympus type
 * \param[in] e #ExifEntry for EXIF_TAG_MAKER_NOTE, from within ed but
 *   duplicated here for convenience
 * \return 0 if not recognized, nonzero if recognized. The specific nonzero 
 *   value returned may identify a subtype unique within this module.
 */
int exif_mnote_data_olympus_identify (const ExifData *ed, const ExifEntry *e);

ExifMnoteData *exif_mnote_data_olympus_new (ExifMem *);

// libexif/olympus/mnote-olympus-tag.c

/* mnote-olympus-tag.c:
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
	MnoteOlympusTag tag;
	const char *name;
	const char *title;
	const char *description;
} table[] = {
#ifndef NO_VERBOSE_TAG_STRINGS
	/* Nikon v2 */
	{MNOTE_NIKON_TAG_FIRMWARE,     "Firmware", N_("Firmware Version"), ""},
	{MNOTE_NIKON_TAG_ISO,          "ISO", N_("ISO Setting"), ""},
	{MNOTE_NIKON_TAG_COLORMODE1,   "ColorMode1", N_("Color Mode (?)"), ""},
	{MNOTE_NIKON_TAG_QUALITY,      "Quality", N_("Quality"), ""},
	{MNOTE_NIKON_TAG_WHITEBALANCE, "WhiteBalance", N_("White Balance"), ""},
	{MNOTE_NIKON_TAG_SHARPENING,   "Sharpening",   N_("Image Sharpening"), ""},
	{MNOTE_NIKON_TAG_FOCUSMODE,    "FocusMode",   N_("Focus Mode"), ""},
	{MNOTE_NIKON_TAG_FLASHSETTING, "FlashSetting",   N_("Flash Setting"), ""},
	{MNOTE_NIKON_TAG_FLASHMODE,    "FlashMode",    N_("Flash Mode"), ""},
	{MNOTE_NIKON_TAG_WHITEBALANCEFINE,"WhiteBalanceFine",N_("White Balance Fine Adjustment"), ""},
	{MNOTE_NIKON_TAG_WHITEBALANCERB,  "WhiteBalanceRB", N_("White Balance RB"), ""},
	{MNOTE_NIKON_TAG_PROGRAMSHIFT,  "ProgramShift", N_("Program Shift"), ""},
	{MNOTE_NIKON_TAG_ISOSELECTION,    "ISOSelection", N_("ISO Selection"), ""},
	{MNOTE_NIKON_TAG_PREVIEWIMAGE_IFD_POINTER, "PreviewImage", N_("Preview Image IFD"), N_("Offset of the preview image directory (IFD) inside the file.")},
	{MNOTE_NIKON_TAG_EXPOSUREDIFF,    "ExposureDiff", N_("Exposurediff ?"), ""},
	{MNOTE_NIKON_TAG_FLASHEXPCOMPENSATION, "FlashExpCompensation", N_("Flash Exposure Compensation"), ""},
	{MNOTE_NIKON_TAG_ISO2,            "ISO", N_("ISO Setting"), ""},
	{MNOTE_NIKON_TAG_IMAGEBOUNDARY,   "ImageBoundary", N_("Image Boundary"), ""},
	{MNOTE_NIKON_TAG_EXTERNALFLASHEXPCOMPENSATION,  "ExternalFlashExpCompensation", N_("External Flash Exposure Compensation"), ""},
	{MNOTE_NIKON_TAG_FLASHEXPOSUREBRACKETVAL,  "FlashExposureBracketVal", N_("Flash Exposure Bracket Value"), ""},
	{MNOTE_NIKON_TAG_EXPOSUREBRACKETVAL,  "ExposureBracketVal", N_("Exposure Bracket Value"), ""},
	{MNOTE_NIKON_TAG_IMAGEADJUSTMENT, "ImageAdjustment", N_("Image Adjustment"), ""},
	{MNOTE_NIKON_TAG_TONECOMPENSATION, "ToneCompensation", N_("Tone Compensation"), ""},
	{MNOTE_NIKON_TAG_ADAPTER,         "Adapter", N_("Adapter"), ""},
	{MNOTE_NIKON_TAG_LENSTYPE,        "LensType", N_("Lens Type"), ""},
	{MNOTE_NIKON_TAG_LENS,            "Lens", N_("Lens"), ""},
	{MNOTE_NIKON_TAG_MANUALFOCUSDISTANCE, "ManualFocusDistance", N_("Manual Focus Distance"), ""},
	{MNOTE_NIKON_TAG_DIGITALZOOM,     "DigitalZoom", N_("Digital Zoom"), ""},
	{MNOTE_NIKON_TAG_FLASHUSED,       "FlashUsed", N_("Flash Used"), ""},
	{MNOTE_NIKON_TAG_AFFOCUSPOSITION, "AFFocusPosition", N_("AF Focus Position"), ""},
	{MNOTE_NIKON_TAG_BRACKETING,      "Bracketing", N_("Bracketing"), ""},
	{MNOTE_NIKON_TAG_UNKNOWN_0X008A,  NULL, NULL, NULL},
	{MNOTE_NIKON_TAG_LENS_FSTOPS,     "LensFStops", N_("Lens F Stops"), ""},
	{MNOTE_NIKON_TAG_CURVE,           "Curve,", N_("Contrast Curve"), ""},
	{MNOTE_NIKON_TAG_COLORMODE,       "ColorMode,", N_("Color Mode"), ""},
	{MNOTE_NIKON_TAG_LIGHTTYPE,       "LightType,", N_("Light Type"), ""},
	{MNOTE_NIKON_TAG_SHOTINFO,	  "ShotInfo", N_("Shot Info"), ""},
	{MNOTE_NIKON_TAG_HUE,             "Hue", N_("Hue Adjustment"), ""},
	{MNOTE_NIKON_TAG_SATURATION,      "Saturation", N_("Saturation"), ""},
	{MNOTE_NIKON_TAG_NOISEREDUCTION,  "NoiseReduction,", N_("Noise Reduction"), ""},
	{MNOTE_NIKON_TAG_COLORBALANCE,    "ColorBalance", N_("Color Balance"), ""},
	{MNOTE_NIKON_TAG_LENSDATA,        "LensData", N_("Lens Data"), ""},
	{MNOTE_NIKON_TAG_SENSORPIXELSIZE, "SensorPixelSize", N_("Sensor Pixel Size"), ""},
	{MNOTE_NIKON_TAG_UNKNOWN_0X009B,  NULL, NULL, NULL},
	{MNOTE_NIKON_TAG_RETOUCHHISTORY,  "RetouchHistory", N_("Retouch History"), ""},
	{MNOTE_NIKON_TAG_SERIALNUMBER,    "SerialNumber", N_("Serial Number"), ""},
	{MNOTE_NIKON_TAG_IMAGE_DATASIZE,  "ImageDataSize", N_("Image Data Size"), N_("Size of compressed image data in bytes.")},
	{MNOTE_NIKON_TAG_UNKNOWN_0X00A3,  NULL, NULL, NULL},
	{MNOTE_NIKON_TAG_TOTALPICTURES,   "TotalPictures,", N_("Total Number of Pictures Taken"), ""},
	{MNOTE_NIKON_TAG_FLASHINFO,       "FlashInfo", N_("Flash Info"), ""},
	{MNOTE_NIKON_TAG_OPTIMIZATION,    "Optimization,", N_("Optimize Image"), ""},
	{MNOTE_NIKON_TAG_SATURATION,      "Saturation", N_("Saturation"), ""},
	{MNOTE_NIKON_TAG_VARIPROGRAM,     "VariProgram", N_("Vari Program"), ""},
	{MNOTE_NIKON_TAG_CAPTUREEDITORDATA, "CaptureEditorData", N_("Capture Editor Data"), ""},
	{MNOTE_NIKON_TAG_CAPTUREEDITORVER, "CaptureEditorVer", N_("Capture Editor Version"), ""},
	{MNOTE_NIKON_TAG_UNKNOWN_0X0E0E,  NULL, NULL, NULL},
	{MNOTE_NIKON_TAG_UNKNOWN_0X0E10,  NULL, NULL, NULL},
	{MNOTE_NIKON_TAG_CROPHISPEED,    "CropHiSpeed", N_("Crop HiSpeed"), ""},
	{MNOTE_NIKON_TAG_EXPOSURETUNING, "ExposureTuning", N_("Exposure Tuning"), ""},
	{MNOTE_NIKON_TAG_COLORSPACE,     "ColorSpace", N_("Color Space"), ""},
	{MNOTE_NIKON_TAG_VRINFO,         "VRInfo", N_("VR Info"), ""},
	{MNOTE_NIKON_TAG_IMAGEAUTHENTICATION, "ImageAuthentication", N_("Image Authentication"), ""},
	{MNOTE_NIKON_TAG_FACEDETECT,     "FaceDetect", N_("Face Detect"), ""},
	{MNOTE_NIKON_TAG_ACTIVEDLIGHTING,"ActiveDLighting", N_("Active DLighting"), ""},
	{MNOTE_NIKON_TAG_PICTURECONTROLDATA, "PictureControlData", N_("Picture Control Data"), ""},
	{MNOTE_NIKON_TAG_WORLDTIME,      "WorldTime", N_("World Time"), ""},
	{MNOTE_NIKON_TAG_ISOINFO,        "ISOInfo", N_("ISO Info"), ""},
	{MNOTE_NIKON_TAG_VIGNETTECONTROL,"VignetteControl", N_("Vignette Control"), ""},
	{MNOTE_NIKON_TAG_DISTORTINFO,    "DistortInfo", N_("Distort Info"), ""},
	{MNOTE_NIKON_TAG_SHUTTERMODE,    "ShutterMode", N_("Shutter Mode"), ""},
	{MNOTE_NIKON_TAG_HDRINFO,        "HDRInfo", N_("HDR Info"), ""},
	{MNOTE_NIKON_TAG_MECHANICALSHUTTERCOUNT, "MechanicalShutterCount", N_("Mechanical Shutter Count"), ""},
	{MNOTE_NIKON_TAG_LOCATIONINFO,   "LocationInfo", N_("MNOTE_NIKON_TAG_LOCATIONINFO"), ""},
	{MNOTE_NIKON_TAG_BLACKLEVEL,     "BlackLevel", N_("Black Level"), ""},
	{MNOTE_NIKON_TAG_IMAGESIZERAW,   "ImageSizeRaw", N_("Image Size Raw"), ""},
	{MNOTE_NIKON_TAG_CROPAREA,       "CropArea", N_("Crop Area"), ""},
	{MNOTE_NIKON_TAG_NIKONSETTINGS,  "NikonSettings", N_("Nikon Settings"), ""},
	{MNOTE_NIKON_TAG_COLORTEMPERATUREAUTO, "ColorTemperatureAuto", N_("Color Temperature Auto"), ""},
	{MNOTE_NIKON_TAG_SERIALNUMBER2,  "SerialNumber2", N_("Serial Number 2"), ""},
	{MNOTE_NIKON_TAG_SATURATION2,    "Saturation2", N_("Saturation 2"), ""},
	{MNOTE_NIKON_TAG_MULTIEXPOSURE,  "MultiExposure", N_("Multi Exposure"), ""},
	{MNOTE_NIKON_TAG_HIGHISONR,      "HighISONr", N_("High ISO Noise Reduction"), ""},
	{MNOTE_NIKON_TAG_TONINGEFFECT,   "ToningEffect", N_("Toning Effect"), ""},
	{MNOTE_NIKON_TAG_POWERUPTIME,    "PowerupTime", N_("Powerup Time"), ""},
	{MNOTE_NIKON_TAG_AFINFO2,        "AFInfo2", N_("AF Info 2"), ""},
	{MNOTE_NIKON_TAG_FILEINFO,       "FileInfo", N_("File Info"), ""},
	{MNOTE_NIKON_TAG_RETOUCHINFO,    "RetouchInfo", N_("Retouch Info"), ""},
	{MNOTE_NIKON_TAG_PREVIEWIMAGE,   "PreviewImage", N_("Preview Image"), ""},
	{MNOTE_NIKON1_TAG_UNKNOWN_0X0002, NULL, NULL, NULL},
	{MNOTE_NIKON1_TAG_QUALITY,        "Quality", N_("Quality"), ""},
	{MNOTE_NIKON1_TAG_COLORMODE,      "ColorMode,", N_("Color Mode"), ""},
	{MNOTE_NIKON1_TAG_IMAGEADJUSTMENT, "ImageAdjustment", N_("Image Adjustment"), ""},
	{MNOTE_NIKON1_TAG_CCDSENSITIVITY, "CCDSensitivity", N_("CCD Sensitivity"), ""},
	{MNOTE_NIKON1_TAG_WHITEBALANCE,   "WhiteBalance", N_("White Balance"), ""},
	{MNOTE_NIKON1_TAG_FOCUS,          "Focus", N_("Focus"), ""},
	{MNOTE_NIKON1_TAG_UNKNOWN_0X0009, NULL, NULL, NULL},
	{MNOTE_NIKON1_TAG_DIGITALZOOM,    "DigitalZoom", N_("Digital Zoom"), ""},
	{MNOTE_NIKON1_TAG_CONVERTER,      "Converter", N_("Converter"), ""},

	/* Olympus & some Sanyo */
	{MNOTE_OLYMPUS_TAG_THUMBNAILIMAGE, "ThumbnailImage", N_("Thumbnail Image"), ""},
	{MNOTE_OLYMPUS_TAG_MODE, "Mode", N_("Speed/Sequence/Panorama Direction"), ""},
	{MNOTE_OLYMPUS_TAG_QUALITY, "Quality", N_("Quality"), ""},
	{MNOTE_OLYMPUS_TAG_MACRO, "Macro", N_("Macro"), ""},
	{MNOTE_OLYMPUS_TAG_BWMODE, "BWMode", N_("Black & White Mode"), ""},
	{MNOTE_OLYMPUS_TAG_DIGIZOOM, "DigiZoom", N_("Digital Zoom"), ""},
	{MNOTE_OLYMPUS_TAG_FOCALPLANEDIAGONAL, "FocalPlaneDiagonal", N_("Focal Plane Diagonal"), ""},
	{MNOTE_OLYMPUS_TAG_LENSDISTORTION, "LensDistortionParams", N_("Lens Distortion Parameters"), ""},
	{MNOTE_OLYMPUS_TAG_VERSION, "FirmwareVersion", N_("Firmware Version"), ""},
	{MNOTE_OLYMPUS_TAG_INFO, "Info", N_("Info"), ""},
	{MNOTE_OLYMPUS_TAG_ID, "CameraID", N_("Camera ID"), ""},
	{MNOTE_OLYMPUS_TAG_PRECAPTUREFRAMES, "PreCaptureFrames", N_("Precapture Frames"), ""},
	{MNOTE_OLYMPUS_TAG_WHITEBOARD, "WhiteBoard", N_("White Board"), ""},
	{MNOTE_OLYMPUS_TAG_ONETOUCHWB, "OneTouchWB", N_("One Touch White Balance"), ""},
	{MNOTE_OLYMPUS_TAG_WHITEBALANCEBRACKET, "WhiteBalanceBracket", N_("White Balance Bracket"), ""},
	{MNOTE_OLYMPUS_TAG_WHITEBALANCEBIAS, "WhiteBalanceBias", N_("White Balance Bias"), ""},
	{MNOTE_OLYMPUS_TAG_DATADUMP, "DataDump", N_("Data Dump"), NULL},
	{MNOTE_OLYMPUS_TAG_UNKNOWN_4, NULL, NULL, NULL},
	{MNOTE_OLYMPUS_TAG_SHUTTERSPEED, "ShutterSpeed", N_("Shutter Speed"), ""},
	{MNOTE_OLYMPUS_TAG_ISOVALUE, "ISOValue", N_("ISO Value"), ""},
	{MNOTE_OLYMPUS_TAG_APERTUREVALUE, "ApertureValue", N_("Aperture Value"), ""},
	{MNOTE_OLYMPUS_TAG_BRIGHTNESSVALUE, "BrightnessValue", N_("Brightness Value"), ""},
	{MNOTE_OLYMPUS_TAG_FLASHMODE, "FlashMode", N_("Flash Mode"), ""},
	{MNOTE_OLYMPUS_TAG_FLASHDEVICE, "FlashDevice", N_("Flash Device"), ""},
	{MNOTE_OLYMPUS_TAG_EXPOSURECOMP, "ExposureCompensation", N_("Exposure Compensation"), ""},
	{MNOTE_OLYMPUS_TAG_SENSORTEMPERATURE, "SensorTemperature", N_("Sensor Temperature"), ""},
	{MNOTE_OLYMPUS_TAG_LENSTEMPERATURE, "LensTemperature", N_("Lens Temperature"), ""},
	{MNOTE_OLYMPUS_TAG_LIGHTCONDITION, "LightCondition", N_("Light Condition"), ""},
	{MNOTE_OLYMPUS_TAG_FOCUSRANGE, "FocusRange", N_("Focus Range"), ""},
	{MNOTE_OLYMPUS_TAG_MANFOCUS, "FocusMode", N_("Focus Mode"), "Automatic or manual focusing mode"},
	{MNOTE_OLYMPUS_TAG_FOCUSDIST, "ManualFocusDistance", N_("Manual Focus Distance"), ""},
	{MNOTE_OLYMPUS_TAG_ZOOMSTEPCOUNT, "ZoomStepCount", N_("Zoom Step Count"), ""},
	{MNOTE_OLYMPUS_TAG_FOCUSSTEPCOUNT, "FocusStepCount", N_("Focus Step Count"), ""},
	{MNOTE_OLYMPUS_TAG_SHARPNESS, "Sharpness", N_("Sharpness Setting"), ""},
	{MNOTE_OLYMPUS_TAG_FLASHCHARGELEVEL, "FlashChargeLevel", N_("Flash Charge Level"), ""},
	{MNOTE_OLYMPUS_TAG_COLORMATRIX, "ColorMatrix", N_("Color Matrix"), ""},
	{MNOTE_OLYMPUS_TAG_BLACKLEVEL, "BlackLevel", N_("Black Level"), ""},
	{MNOTE_OLYMPUS_TAG_WBALANCE, "WhiteBalance", N_("White Balance Setting"), ""},
	{MNOTE_OLYMPUS_TAG_REDBALANCE, "RedBalance", N_("Red Balance"), ""},
	{MNOTE_OLYMPUS_TAG_BLUEBALANCE, "BlueBalance", N_("Blue Balance"), ""},
	{MNOTE_OLYMPUS_TAG_COLORMATRIXNUMBER, "ColorMatrixNumber", N_("Color Matrix Number"), ""},
	{MNOTE_OLYMPUS_TAG_SERIALNUMBER2, "SerialNumber", N_("Serial Number"), ""},
	{MNOTE_OLYMPUS_TAG_FLASHEXPOSURECOMP, "FlashExposureComp", N_("Flash Exposure Comp"), ""},
	{MNOTE_OLYMPUS_TAG_INTERNALFLASHTABLE, "InternalFlashTable", N_("Internal Flash Table"), ""},
	{MNOTE_OLYMPUS_TAG_EXTERNALFLASHGVALUE, "ExternalFlashGValue", N_("External Flash G Value"), ""},
	{MNOTE_OLYMPUS_TAG_EXTERNALFLASHBOUNCE, "ExternalFlashBounce", N_("External Flash Bounce"), ""},
	{MNOTE_OLYMPUS_TAG_EXTERNALFLASHZOOM, "ExternalFlashZoom", N_("External Flash Zoom"), ""},
	{MNOTE_OLYMPUS_TAG_EXTERNALFLASHMODE, "ExternalFlashMode", N_("External Flash Mode"), ""},
	{MNOTE_OLYMPUS_TAG_CONTRAST, "Contrast", N_("Contrast Setting"), ""},
	{MNOTE_OLYMPUS_TAG_SHARPNESSFACTOR, "SharpnessFactor", N_("Sharpness Factor"), ""},
	{MNOTE_OLYMPUS_TAG_COLORCONTROL, "ColorControl", N_("Color Control"), ""},
	{MNOTE_OLYMPUS_TAG_IMAGEWIDTH, "OlympusImageWidth", N_("Olympus Image Width"), ""},
	{MNOTE_OLYMPUS_TAG_IMAGEHEIGHT, "OlympusImageHeight", N_("Olympus Image Height"), ""},
	{MNOTE_OLYMPUS_TAG_SCENEDETECT, "SceneDetect", N_("Scene Detect"), ""},
	{MNOTE_OLYMPUS_TAG_COMPRESSIONRATIO, "CompressionRatio", N_("Compression Ratio"), ""},
	{MNOTE_OLYMPUS_TAG_PREVIEWIMAGEVALID, "PreviewImageValid", N_("Preview Image Valid"), ""},
	{MNOTE_OLYMPUS_TAG_AFRESULT, "AFResult", N_("AF Result"), ""},
	{MNOTE_OLYMPUS_TAG_CCDSCANMODE, "CCDScanMode", N_("CCD Scan Mode"), ""},
	{MNOTE_OLYMPUS_TAG_NOISEREDUCTION, "NoiseReduction", N_("Noise Reduction"), ""},
	{MNOTE_OLYMPUS_TAG_INFINITYLENSSTEP, "InfinityLensStep", N_("Infinity Lens Step"), ""},
	{MNOTE_OLYMPUS_TAG_NEARLENSSTEP, "NearLensStep", N_("Near Lens Step"), ""},
	{MNOTE_OLYMPUS_TAG_LIGHTVALUECENTER, "LightValueCenter", N_("Light Value Center"), ""},
	{MNOTE_OLYMPUS_TAG_LIGHTVALUEPERIPHERY, "LightValuePeriphery", N_("Light Value Periphery"), ""},

	/* Sanyo */
	{MNOTE_SANYO_TAG_SEQUENTIALSHOT, "SequentialShot", N_("Sequential Shot"), ""},
	{MNOTE_SANYO_TAG_WIDERANGE, "WideRange", N_("Wide Range"), ""},
	{MNOTE_SANYO_TAG_COLORADJUSTMENTMODE, "ColorAdjustmentMode", N_("Color Adjustment Mode"), ""},
	{MNOTE_SANYO_TAG_FOCUSMODE, "FocusMode", N_("Focus Mode"), ""},
	{MNOTE_SANYO_TAG_QUICKSHOT, "QuickShot", N_("Quick Shot"), ""},
	{MNOTE_SANYO_TAG_SELFTIMER, "SelfTimer", N_("Self-timer"), ""},
	{MNOTE_SANYO_TAG_VOICEMEMO, "VoiceMemo", N_("Voice Memo"), ""},
	{MNOTE_SANYO_TAG_RECORDSHUTTERRELEASE, "RecordShutterRelease", N_("Record Shutter Release"), ""},
	{MNOTE_SANYO_TAG_FLICKERREDUCE, "FlickerReduce", N_("Flicker Reduce"), ""},
	{MNOTE_SANYO_TAG_OPTICALZOOM, "OpticalZoom", N_("Optical Zoom"), ""},
	{MNOTE_SANYO_TAG_DIGITALZOOM, "DigitalZoom", N_("Digital Zoom"), ""},
	{MNOTE_SANYO_TAG_LIGHTSOURCESPECIAL, "LightSourceSpecial", N_("Light Source Special"), ""},
	{MNOTE_SANYO_TAG_RESAVED, "Resaved", N_("Resaved"), ""},
	{MNOTE_SANYO_TAG_CCDSENSITIVITY, "CCDSensitivity", N_("CCD Sensitivity"), ""},
	{MNOTE_SANYO_TAG_SCENESELECT, "SceneSelect", N_("Scene Select"), ""},
	{MNOTE_SANYO_TAG_MANUALFOCUSDISTANCE, "ManualFocusDistance", N_("Manual Focus Distance"), ""},
	{MNOTE_SANYO_TAG_SEQUENCESHOTINTERVAL, "SequenceShotInterval", N_("Sequence Shot Interval"), ""},

	/* Epson */
	{MNOTE_EPSON_TAG_IMAGE_WIDTH, "EpsonImageWidth", N_("Epson Image Width"), ""},
	{MNOTE_EPSON_TAG_IMAGE_HEIGHT, "EpsonImageHeight", N_("Epson Image Height"), ""},
	{MNOTE_EPSON_TAG_SOFTWARE, "EpsonSoftware", N_("Epson Software Version"), ""},
#endif
	{0, NULL, NULL, NULL}
};

const char *
mnote_olympus_tag_get_name (MnoteOlympusTag t)
{
	unsigned int i;

	for (i = 0; i < sizeof (table) / sizeof (table[0]); i++)
		if (table[i].tag == t) return (table[i].name);
	return NULL;
}

const char *
mnote_olympus_tag_get_title (MnoteOlympusTag t)
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
mnote_olympus_tag_get_description (MnoteOlympusTag t)
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

// libexif/olympus/mnote-olympus-entry.c

/* mnote-olympus-entry.c
 *
 * Copyright (c) 2002-2009 Lutz Mueller <lutz@users.sourceforge.net> et. al.
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

#define CF2(format,target1,target2,v,maxlen)                    \
{                                                               \
        if ((format != target1) && (format != target2)) {       \
                snprintf (v, maxlen,	                        \
                        _("Invalid format '%s', "               \
                        "expected '%s' or '%s'."),              \
                        exif_format_get_name (format),          \
                        exif_format_get_name (target1),         \
                        exif_format_get_name (target2));        \
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
	if ((number < t1) || (number > t2)) {                           \
		snprintf (v, maxlen,                                    \
			_("Invalid number of components (%i, "          \
			"expected %i or %i)."), (int) number,		\
			(int) t1, (int) t2);  				\
		break;                                                  \
	}                                                               \
}

#define R2L(n) ((n).denominator ? (unsigned long)(n).numerator/(n).denominator : 0L)
#define R2D(n) ((n).denominator ? (double)(n).numerator/(n).denominator : 0.0)

static const struct {
	MnoteOlympusTag tag;
	ExifFormat fmt;
	struct {
		int index;
		const char *string;
	} elem[24];
} items[] = {
#ifndef NO_VERBOSE_TAG_DATA
  { MNOTE_NIKON_TAG_LENSTYPE, EXIF_FORMAT_BYTE,
    { {0, N_("AF non D lens")},
      {1, N_("Manual")},
      {2, N_("AF-D or AF-S lens")},
      {6, N_("AF-D G lens")},
      {10, N_("AF-D VR lens")},
      {14, N_("AF-D G VR lens")},
      {0, NULL}}},
  { MNOTE_NIKON_TAG_FLASHUSED, EXIF_FORMAT_BYTE,
    { {0, N_("Flash did not fire")},
      {4, N_("Flash unit unknown")},
      {7, N_("Flash is external")},
      {9, N_("Flash is on camera")},
      {0, NULL}}},
  { MNOTE_NIKON1_TAG_QUALITY, EXIF_FORMAT_SHORT,
    { {1, N_("VGA basic")},
      {2, N_("VGA normal")},
      {3, N_("VGA fine")},
      {4, N_("SXGA basic")},
      {5, N_("SXGA normal")},
      {6, N_("SXGA fine")},
      {10, N_("2 Mpixel basic")},
      {11, N_("2 Mpixel normal")},
      {12, N_("2 Mpixel fine")},
      {0, NULL}}},
  { MNOTE_NIKON1_TAG_COLORMODE, EXIF_FORMAT_SHORT,
    { {1, N_("Color")},
      {2, N_("Monochrome")},
      {0, NULL}}},
  { MNOTE_NIKON1_TAG_IMAGEADJUSTMENT, EXIF_FORMAT_SHORT,
    { {0, N_("Normal")},
      {1, N_("Bright+")},
      {2, N_("Bright-")},
      {3, N_("Contrast+")},
      {4, N_("Contrast-")},
      {0, NULL}}},
  { MNOTE_NIKON1_TAG_CCDSENSITIVITY, EXIF_FORMAT_SHORT,
    { {0, N_("ISO 80")},
      {2, N_("ISO 160")},
      {4, N_("ISO 320")},
      {5, N_("ISO 100")},
      {0, NULL}}},
  { MNOTE_NIKON1_TAG_WHITEBALANCE, EXIF_FORMAT_SHORT,
    { {0, N_("Auto")},
      {1, N_("Preset")},
      {2, N_("Daylight")},
      {3, N_("Incandescence")},
      {4, N_("Fluorescence")},
      {5, N_("Cloudy")},
      {6, N_("SpeedLight")},
      {0, NULL}}},
  { MNOTE_NIKON1_TAG_CONVERTER, EXIF_FORMAT_SHORT,
    { {0, N_("No fisheye")},
      {1, N_("Fisheye on")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_QUALITY, EXIF_FORMAT_SHORT,
    { {1, N_("Normal, SQ")},
      {2, N_("Normal, HQ")},
      {3, N_("Normal, SHQ")},
      {4, N_("Normal, RAW")},
      {5, N_("Normal, SQ1")},
      {6, N_("Normal, SQ2")},
      {7, N_("Normal, super high")},
      {17, N_("Normal, standard")},
      {0x101, N_("Fine, SQ")},
      {0x102, N_("Fine, HQ")},
      {0x103, N_("Fine, SHQ")},
      {0x104, N_("Fine, RAW")},
      {0x105, N_("Fine, SQ1")},
      {0x106, N_("Fine, SQ2")},
      {0x107, N_("Fine, super high")},
      {0x201, N_("Super fine, SQ")},
      {0x202, N_("Super fine, HQ")},
      {0x203, N_("Super fine, SHQ")},
      {0x204, N_("Super fine, RAW")},
      {0x205, N_("Super fine, SQ1")},
      {0x206, N_("Super fine, SQ2")},
      {0x207, N_("Super fine, super high")},
      {0x211, N_("Super fine, high")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_MACRO, EXIF_FORMAT_SHORT,
    { {0, N_("No")},
      {1, N_("Yes")},
      {2, N_("Super macro")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_BWMODE, EXIF_FORMAT_SHORT,
    { {0, N_("No")},
      {1, N_("Yes")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_ONETOUCHWB, EXIF_FORMAT_SHORT,
    { {0, N_("Off")},
      {1, N_("On")},
      {2, N_("On (Preset)")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_FLASHMODE, EXIF_FORMAT_SHORT,
    { {0, N_("Auto")},
      {1, N_("Red-eye reduction")},
      {2, N_("Fill")},
      {3, N_("Off")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_FLASHDEVICE, EXIF_FORMAT_SHORT,
    { {0, N_("None")},
      {1, N_("Internal")},
      {4, N_("External")},
      {5, N_("Internal + external")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_FOCUSRANGE, EXIF_FORMAT_SHORT,
    { {0, N_("Normal")},
      {1, N_("Macro")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_MANFOCUS, EXIF_FORMAT_SHORT,
    { {0, N_("Auto")},
      {1, N_("Manual")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_SHARPNESS, EXIF_FORMAT_SHORT,
    { {0, N_("Normal")},
      {1, N_("Hard")},
      {2, N_("Soft")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_EXTERNALFLASHBOUNCE, EXIF_FORMAT_SHORT,
    { {0, N_("No")},
      {1, N_("Yes")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_CONTRAST, EXIF_FORMAT_SHORT,
    { {0, N_("Hard")},
      {1, N_("Normal")},
      {2, N_("Soft")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_PREVIEWIMAGEVALID, EXIF_FORMAT_LONG,
    { {0, N_("No")},
      {1, N_("Yes")},
      {0, NULL}}},
  { MNOTE_OLYMPUS_TAG_CCDSCANMODE, EXIF_FORMAT_SHORT,
    { {0, N_("Interlaced")},
      {1, N_("Progressive")},
      {0, NULL}}},

  { MNOTE_SANYO_TAG_SEQUENTIALSHOT, EXIF_FORMAT_SHORT,
    { {0, N_("None")},
      {1, N_("Standard")},
      {2, N_("Best")},
      {3, N_("Adjust exposure")},
      {0, NULL}}},
  { MNOTE_SANYO_TAG_FOCUSMODE, EXIF_FORMAT_SHORT,
    { {1, N_("Spot focus")},
      {2, N_("Normal focus")},
      {0, NULL}}},
  { MNOTE_SANYO_TAG_RECORDSHUTTERRELEASE, EXIF_FORMAT_SHORT,
    { {0, N_("Record while down")},
      {1, N_("Press start, press stop")},
      {0, NULL}}},
  { MNOTE_SANYO_TAG_RESAVED, EXIF_FORMAT_SHORT,
    { {0, N_("No")},
      {1, N_("Yes")},
      {0, NULL}}},
  { MNOTE_SANYO_TAG_CCDSENSITIVITY, EXIF_FORMAT_SHORT,
    { {0, N_("Auto")},
      {1, N_("ISO 50")},
      {3, N_("ISO 100")},
      {4, N_("ISO 200")},
      {5, N_("ISO 400")},
      {0, NULL}}},
  { MNOTE_SANYO_TAG_SCENESELECT, EXIF_FORMAT_SHORT,
    { {0, N_("Off")},
      {1, N_("Sport")},
      {2, N_("TV")},
      {3, N_("Night")},
      {4, N_("User 1")},
      {5, N_("User 2")},
      {6, N_("Lamp")},
      {0, NULL}}},
  { MNOTE_SANYO_TAG_SEQUENCESHOTINTERVAL, EXIF_FORMAT_SHORT,
    { {0, N_("5 frames/sec")},
      {1, N_("10 frames/sec")},
      {2, N_("15 frames/sec")},
      {3, N_("20 frames/sec")},
      {0, NULL}}},
#endif
  { 0, 0, { { 0, NULL } } }
};

char *
mnote_olympus_entry_get_value (MnoteOlympusEntry *entry, char *v, unsigned int maxlen)
{
	char         buf[30];
	ExifLong     vl;
	ExifSLong    vsl;
	ExifShort    vs = 0;
	ExifSShort   vss = 0;
	ExifRational vr, vr2;
	ExifSRational vsr;
	int          i, j;
	double       r, b;

	if (!entry)
		return (NULL);
	if (maxlen < 1)
		return NULL;

	memset (v, 0, maxlen);
	maxlen--;

	if ((!entry->data) && (entry->components > 0)) 
		return (v);

	if ((!entry->data) && (entry->size > 0))
		return NULL;  /* internal inconsistency error */

	switch (entry->tag) {
	
	/* Nikon */
	case MNOTE_NIKON_TAG_FIRMWARE:
		CF (entry->format,  EXIF_FORMAT_UNDEFINED, v, maxlen)
		CC (entry->components, 4, v, maxlen)
		vl = exif_get_long (entry->data, EXIF_BYTE_ORDER_INTEL);
		if ((vl & 0xF0F0F0F0) == 0x30303030) {
			memcpy (v, entry->data, MIN (maxlen, 4));
		} else {
			snprintf (v, maxlen, "%04lx", (long unsigned int) vl);
		}
		break;
	case MNOTE_NIKON_TAG_ISO:
                CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
                CC (entry->components, 2, v, maxlen)
                /*vs = exif_get_short (entry->data, entry->order);*/
                vs = exif_get_short (entry->data + 2, entry->order);
                snprintf (v, maxlen, "ISO %hd", vs);
                break;
	case MNOTE_NIKON_TAG_ISO2:
                CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
                CC (entry->components, 2, v, maxlen)
                /*vs = exif_get_short (entry->data, entry->order);*/
                vs = exif_get_short (entry->data + 2, entry->order);
                snprintf (v, maxlen, "ISO2 %hd", vs);
                break;
	case MNOTE_NIKON_TAG_QUALITY:
	case MNOTE_NIKON_TAG_COLORMODE:
	case MNOTE_NIKON_TAG_COLORMODE1:
	case MNOTE_NIKON_TAG_WHITEBALANCE:
	case MNOTE_NIKON_TAG_SHARPENING:
	case MNOTE_NIKON_TAG_FOCUSMODE:
	case MNOTE_NIKON_TAG_FLASHSETTING:
	case MNOTE_NIKON_TAG_ISOSELECTION:
	case MNOTE_NIKON_TAG_FLASHMODE:
	case MNOTE_NIKON_TAG_IMAGEADJUSTMENT:
	case MNOTE_NIKON_TAG_ADAPTER:
	case MNOTE_NIKON_TAG_SATURATION2:
	case MNOTE_EPSON_TAG_SOFTWARE:
		CF (entry->format, EXIF_FORMAT_ASCII, v, maxlen)
		memcpy(v, entry->data, MIN (maxlen, entry->size));
		break;
	case MNOTE_NIKON_TAG_TOTALPICTURES:
	case MNOTE_EPSON_TAG_IMAGE_WIDTH:
	case MNOTE_EPSON_TAG_IMAGE_HEIGHT:
		CF (entry->format, EXIF_FORMAT_LONG, v, maxlen)
		CC (entry->components, 1, v, maxlen)
		vl =  exif_get_long (entry->data, entry->order);
		snprintf (v, maxlen, "%lu",  (long unsigned int) vl );
		break;
	case MNOTE_NIKON_TAG_LENS_FSTOPS:
	case MNOTE_NIKON_TAG_EXPOSUREDIFF: {
		unsigned char h,m,l;
		CF (entry->format, EXIF_FORMAT_UNDEFINED, v, maxlen)
		CC (entry->components, 4, v, maxlen)
		vl =  exif_get_long (entry->data, entry->order);
		h = (vl>>24)&0xff; m = (vl>>16)&0xff; l = (vl>>8)&0xff;
		snprintf (v, maxlen, "%.1f",  l?(double)h*((double)m/(double)l):0 );
		break;
	}
	case MNOTE_NIKON_TAG_FLASHEXPCOMPENSATION:
	case MNOTE_NIKON_TAG_FLASHEXPOSUREBRACKETVAL:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, v, maxlen)
		CC (entry->components, 4, v, maxlen)
		vl =  exif_get_long (entry->data, entry->order);
		snprintf (v, maxlen, "%.1f",  ((long unsigned int) vl>>24)/6.0 );
		break;
	case MNOTE_NIKON_TAG_SATURATION:
	case MNOTE_NIKON_TAG_WHITEBALANCEFINE:
	case MNOTE_NIKON_TAG_HUE:
	case MNOTE_OLYMPUS_TAG_SENSORTEMPERATURE:
	case MNOTE_OLYMPUS_TAG_LENSTEMPERATURE:
		CF (entry->format, EXIF_FORMAT_SSHORT, v, maxlen)
		CC (entry->components, 1, v, maxlen)
		vs = exif_get_short (entry->data, entry->order);
		snprintf (v, maxlen, "%hd", vs);
		break;
	case MNOTE_NIKON_TAG_WHITEBALANCERB:
		CF (entry->format, EXIF_FORMAT_RATIONAL, v, maxlen)
		CC (entry->components, 4, v, maxlen)
		vr = exif_get_rational (entry->data, entry->order);
		r = R2D(vr);
		vr = exif_get_rational (entry->data+8, entry->order);
		b = R2D(vr);
		snprintf (v, maxlen, _("Red Correction %f, blue Correction %f"), r,b);
		break;
	case MNOTE_NIKON_TAG_MANUALFOCUSDISTANCE:
		CF (entry->format, EXIF_FORMAT_RATIONAL, v, maxlen)
		CC (entry->components, 1, v, maxlen)
		vr = exif_get_rational (entry->data, entry->order);
		if (!vr.numerator || !vr.denominator) {
			strncpy (v, _("No manual focus selection"), maxlen);
		} else {
			r = R2D(vr);
			snprintf (v, maxlen, _("%2.2f meters"), r);
		}
		break;
	case MNOTE_NIKON_TAG_SENSORPIXELSIZE:
		CF (entry->format, EXIF_FORMAT_RATIONAL, v, maxlen)
		CC (entry->components, 2, v, maxlen)
		vr = exif_get_rational (entry->data, entry->order);
		vr2 = exif_get_rational (entry->data+8, entry->order);
		r = R2D(vr);
		b = R2D(vr2);
		snprintf (v, maxlen, "%2.2f x %2.2f um", r, b);
		break;
	case MNOTE_NIKON_TAG_BRACKETING:
		CF2 (entry->format, EXIF_FORMAT_BYTE, EXIF_FORMAT_SHORT, v, maxlen)
		CC (entry->components, 1, v, maxlen)
		if (EXIF_FORMAT_SHORT == entry->format) {
			vs = exif_get_short (entry->data, entry->order);
		} else {
			vs = entry->data[0];
		}
		snprintf (v, maxlen, "%hd", vs);
		break;
	case MNOTE_NIKON_TAG_AFFOCUSPOSITION:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, v, maxlen)
		CC (entry->components, 4, v, maxlen)
		switch (  *( entry->data+1)  ) {
		  	case  0: strncpy (v, _("AF position: center"), maxlen); break;
		  	case  1: strncpy (v, _("AF position: top"), maxlen); break;
		  	case  2: strncpy (v, _("AF position: bottom"), maxlen); break;
		  	case  3: strncpy (v, _("AF position: left"), maxlen); break;
		  	case  4: strncpy (v, _("AF position: right"), maxlen); break;
			case  5: strncpy (v, _("AF position: upper-left"), maxlen); break;
		  	case  6: strncpy (v, _("AF position: upper-right"), maxlen); break;
		  	case  7: strncpy (v, _("AF position: lower-left"), maxlen); break;
		  	case  8: strncpy (v, _("AF position: lower-right"), maxlen); break;
		  	case  9: strncpy (v, _("AF position: far left"), maxlen); break;
		  	case  10: strncpy (v, _("AF position: far right"), maxlen); break;
		  	default: strncpy (v, _("Unknown AF position"), maxlen);
		}     
		break;
	case MNOTE_OLYMPUS_TAG_FLASHDEVICE:
		CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
		CC (entry->components, 2, v, maxlen)
		vs = exif_get_short(entry->data, entry->order);
		/* search for the tag */
		for (i = 0; (items[i].tag && items[i].tag != entry->tag); i++)
			;
		if (!items[i].tag) {
		  	snprintf (v, maxlen, _("Internal error (unknown value %hi)"), vs);
		  	break;
		}
		CF (entry->format, items[i].fmt, v, maxlen)
		/* find the value */
		for (j = 0; items[i].elem[j].string &&
			    (items[i].elem[j].index < vs); j++);
		if (items[i].elem[j].index != vs) {
			snprintf (v, maxlen, _("Unknown value %hi"), vs);
			break;
		}
		strncpy (v, _(items[i].elem[j].string), maxlen);
		break;
	case MNOTE_OLYMPUS_TAG_DIGIZOOM:
		if (entry->format == EXIF_FORMAT_RATIONAL) {
			CC (entry->components, 1, v, maxlen)
			vr = exif_get_rational (entry->data, entry->order);
			if (!vr.numerator || !vr.denominator) {
				strncpy (v, _("None"), maxlen);
			} else {
				r = R2D(vr);
				snprintf (v, maxlen, "%2.2f", r);
			}
			break;
		}
		/* to handle SHORT version of this tag, */
		/* fall through */
	case MNOTE_NIKON_TAG_LENSTYPE:
	case MNOTE_NIKON_TAG_FLASHUSED:
	case MNOTE_NIKON1_TAG_QUALITY:
	case MNOTE_NIKON1_TAG_COLORMODE:
	case MNOTE_NIKON1_TAG_IMAGEADJUSTMENT:
	case MNOTE_NIKON1_TAG_CCDSENSITIVITY:
	case MNOTE_NIKON1_TAG_WHITEBALANCE:
	case MNOTE_NIKON1_TAG_CONVERTER:
	case MNOTE_OLYMPUS_TAG_QUALITY:
	case MNOTE_OLYMPUS_TAG_MACRO:
	case MNOTE_OLYMPUS_TAG_BWMODE:
	case MNOTE_OLYMPUS_TAG_ONETOUCHWB:
	case MNOTE_OLYMPUS_TAG_FLASHMODE:
	case MNOTE_OLYMPUS_TAG_FOCUSRANGE:
	case MNOTE_OLYMPUS_TAG_MANFOCUS:
	case MNOTE_OLYMPUS_TAG_SHARPNESS:
	case MNOTE_OLYMPUS_TAG_EXTERNALFLASHBOUNCE:
	case MNOTE_OLYMPUS_TAG_CONTRAST:
	case MNOTE_OLYMPUS_TAG_PREVIEWIMAGEVALID:
	case MNOTE_OLYMPUS_TAG_CCDSCANMODE:
	case MNOTE_SANYO_TAG_SEQUENTIALSHOT:
	case MNOTE_SANYO_TAG_FOCUSMODE:
	case MNOTE_SANYO_TAG_RECORDSHUTTERRELEASE:
	case MNOTE_SANYO_TAG_RESAVED:
	case MNOTE_SANYO_TAG_CCDSENSITIVITY:
	case MNOTE_SANYO_TAG_SCENESELECT:
	case MNOTE_SANYO_TAG_SEQUENCESHOTINTERVAL:
		CC (entry->components, 1, v, maxlen)
		switch (entry->format) {
		case EXIF_FORMAT_BYTE:
		case EXIF_FORMAT_UNDEFINED:
			vs = entry->data[0];
			break;
		case EXIF_FORMAT_SHORT:
			vs = exif_get_short(entry->data, entry->order);
			break;
		default:
			vs = 0;
			break;
		}
		/* search for the tag */
		for (i = 0; (items[i].tag && items[i].tag != entry->tag); i++)
			;
		if (!items[i].tag) {
		  	snprintf (v, maxlen, _("Internal error (unknown value %hi)"), vs);
		  	break;
		}
		CF (entry->format, items[i].fmt, v, maxlen)
		/* find the value */
		for (j = 0; items[i].elem[j].string &&
			    (items[i].elem[j].index < vs); j++);
		if (items[i].elem[j].index != vs) {
			snprintf (v, maxlen, _("Unknown value %hi"), vs);
			break;
		}
		strncpy (v, _(items[i].elem[j].string), maxlen);
		break;
	case MNOTE_OLYMPUS_TAG_NOISEREDUCTION:
	case MNOTE_SANYO_TAG_WIDERANGE:
	case MNOTE_SANYO_TAG_COLORADJUSTMENTMODE:
	case MNOTE_SANYO_TAG_QUICKSHOT:
	case MNOTE_SANYO_TAG_VOICEMEMO:
	case MNOTE_SANYO_TAG_FLICKERREDUCE:
	case MNOTE_SANYO_TAG_OPTICALZOOM:
	case MNOTE_SANYO_TAG_DIGITALZOOM:
	case MNOTE_SANYO_TAG_LIGHTSOURCESPECIAL:
		CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
		CC (entry->components, 1, v, maxlen)
		vs = exif_get_short (entry->data, entry->order);
		switch (vs) {
		case 0:
			strncpy (v, _("Off"), maxlen);
			break;
		case 1:
			strncpy (v, _("On"), maxlen);
			break;
		default:
			sprintf (buf, _("Unknown %hu"), vs);
			strncat (v, buf, maxlen - strlen (v));
			break;
		}
		break;
	case MNOTE_SANYO_TAG_SELFTIMER:
		CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
		CC (entry->components, 1, v, maxlen)
		vs = exif_get_short (entry->data, entry->order);
		switch (vs) {
		case 0:
			strncpy (v, _("Off"), maxlen);
			break;
		case 1:
			strncpy (v, _("On"), maxlen);
			break;
		case 2:
			strncpy (v, _("2 sec."), maxlen);
			break;
		default:
			sprintf (buf, _("Unknown %hu"), vs);
			strncat (v, buf, maxlen - strlen (v));
			break;
		}
		break;
	case MNOTE_NIKON_TAG_LENS:
		CF (entry->format, EXIF_FORMAT_RATIONAL, v, maxlen)
		CC (entry->components, 4, v, maxlen)
		{
			double ao,ac;
			unsigned long fs,fl;
			vr = exif_get_rational (entry->data, entry->order);
			fs = R2L(vr);
			vr = exif_get_rational (entry->data+8, entry->order);
			fl = R2L(vr);
			vr = exif_get_rational (entry->data+16, entry->order);
			ao = R2D(vr);
			vr = exif_get_rational (entry->data+24, entry->order);
			ac = R2D(vr);
			snprintf (v, maxlen, "%ld-%ldmm 1:%3.1f - %3.1f",fs,fl,ao,ac);
		}
		break;

	/* Olympus */
	case MNOTE_OLYMPUS_TAG_MODE:
		CF (entry->format, EXIF_FORMAT_LONG, v, maxlen)
		CC (entry->components, 3, v, maxlen)
		vl = exif_get_long (entry->data, entry->order);
		switch (vl) {
		case 0:
			strncpy (v, _("Normal"), maxlen);
			break;
		case 1:
			strncpy (v, _("Unknown"), maxlen);
			break;
		case 2:
			strncpy (v, _("Fast"), maxlen);
			break;
		case 3:
			strncpy (v, _("Panorama"), maxlen);
			break;
		default:
			snprintf (v, maxlen, "%li", (long int) vl);
		}
		vl = exif_get_long (entry->data + 4, entry->order);
		snprintf (buf, sizeof (buf), "/%li/", (long int) vl);
		strncat (v, buf, maxlen - strlen (v));
		vl = exif_get_long (entry->data + 8, entry->order);
		switch (vl) {
		case 1:
			strncat (v, _("Left to right"), maxlen - strlen (v));
			break;
		case 2:
			strncat (v, _("Right to left"), maxlen - strlen (v));
			break;
		case 3:
			strncat (v, _("Bottom to top"), maxlen - strlen (v));
			break;
		case 4:
			strncat (v, _("Top to bottom"), maxlen - strlen (v));
			break;
		default:
			snprintf (buf, sizeof (buf), "%li",
				  (long int) vl);
			strncat (v, buf, maxlen - strlen (v));
		}
		break;
	case MNOTE_OLYMPUS_TAG_LENSDISTORTION:
		if (entry->format == EXIF_FORMAT_SHORT) {
			/* Epson uses a single SHORT here */
			CC (entry->components, 1, v, maxlen)
			vs = exif_get_short (entry->data, entry->order);
			sprintf (buf, "%hu", vs);
			strncat (v, buf, maxlen - strlen (v));
		} else {
			/* Others use an array of SSHORT here */
			CC (entry->components, 6, v, maxlen)
			CF (entry->format, EXIF_FORMAT_SSHORT, v, maxlen)
			for (i=0; i < (int)entry->components; ++i) {
				vss = exif_get_sshort (entry->data+2*i, entry->order);
				sprintf (buf, "%hd ", vss);
				strncat (v, buf, maxlen - strlen (v));
			}
		}
		break;
	case MNOTE_OLYMPUS_TAG_COLORCONTROL:
		CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
		CC (entry->components, 6, v, maxlen)
		for (i=0; i < (int)entry->components; ++i) {
			vs = exif_get_short (entry->data+2*i, entry->order);
			sprintf (buf, "%hu ", vs);
			strncat (v, buf, maxlen - strlen (v));
		}
		break;
	case MNOTE_OLYMPUS_TAG_VERSION:
		CF (entry->format, EXIF_FORMAT_ASCII, v, maxlen)
		CC2 (entry->components, 5, 8, v, maxlen)
		strncpy (v, (char *)entry->data, MIN (maxlen, entry->size));
		break;
	case MNOTE_OLYMPUS_TAG_SERIALNUMBER2:
		CF (entry->format, EXIF_FORMAT_ASCII, v, maxlen)
		strncpy (v, (char *)entry->data, MIN (maxlen, entry->size));
		break;
	case MNOTE_OLYMPUS_TAG_INFO:
		CF (entry->format, EXIF_FORMAT_ASCII, v, maxlen)
		CC2 (entry->components, 52, 60, v, maxlen)
		strncpy (v, (char *)entry->data, MIN (maxlen, entry->size));
		break;
	case MNOTE_OLYMPUS_TAG_ID:
		CF (entry->format, EXIF_FORMAT_UNDEFINED, v, maxlen)
		CC (entry->components, 32, v, maxlen)
		strncpy (v, (char *)entry->data, MIN (maxlen, entry->size));
		break;
	case MNOTE_OLYMPUS_TAG_UNKNOWN_4:
		CF (entry->format, EXIF_FORMAT_LONG, v, maxlen)
		CC (entry->components, 30, v, maxlen)
		for (i=0; i < (int)entry->components; ++i) {
			vl = exif_get_long (entry->data+4*i, entry->order);
			sprintf (buf, "%lu ", (unsigned long)vl);
			strncat (v, buf, maxlen - strlen (v));
		}
		break;
	case MNOTE_OLYMPUS_TAG_FOCUSDIST:
		CF (entry->format, EXIF_FORMAT_RATIONAL, v, maxlen)
		CC (entry->components, 1, v, maxlen)
		vr = exif_get_rational (entry->data, entry->order);
		if (!vr.numerator || !vr.denominator) {
			strncpy (v, _("Unknown"), maxlen);
		}
		else {
			unsigned long tmp = vr.numerator / vr.denominator;
			snprintf (v, maxlen, "%li mm", tmp);
		}
		break;
	case MNOTE_OLYMPUS_TAG_WBALANCE:
		CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
		CC (entry->components, 2, v, maxlen)
		vs = exif_get_short (entry->data, entry->order);
		switch (vs) {
		case 1:
			strncpy (v, _("Automatic"), maxlen);
			break;
		case 2:
			{
				ExifShort v2 = exif_get_short (entry->data + 2, entry->order);
				unsigned long colorTemp = 0;
				switch (v2) {
				case 2:
					colorTemp = 3000;
					break;
				case 3:
					colorTemp = 3700;
					break;
				case 4:
					colorTemp = 4000;
					break;
				case 5:
					colorTemp = 4500;
					break;
				case 6:
					colorTemp = 5500;
					break;
				case 7:
					colorTemp = 6500;
					break;
				case 9:
					colorTemp = 7500;
					break;
				}
				if (colorTemp) {
					snprintf (v, maxlen, _("Manual: %liK"), colorTemp);
				}
				else {
					strncpy (v, _("Manual: unknown"), maxlen);
				}

			}
			break;
		case 3:
			strncpy (v, _("One-touch"), maxlen);
			break;
		default:
			strncpy (v, _("Unknown"), maxlen);
			break;
		}
		break;
	case MNOTE_OLYMPUS_TAG_REDBALANCE:
	case MNOTE_OLYMPUS_TAG_BLUEBALANCE:
		CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
		CC (entry->components, 2, v, maxlen)
		vs = exif_get_short (entry->data, entry->order);
		snprintf (v, maxlen, "%hu ", vs);
		vs = exif_get_short (entry->data + 2, entry->order);
		sprintf (buf, "%hu", vs);
		strncat (v, buf, maxlen - strlen (v));
		break;
	case MNOTE_OLYMPUS_TAG_BLACKLEVEL:
	case MNOTE_NIKON_TAG_IMAGEBOUNDARY:
		CC (entry->components, 4, v, maxlen)
		/* Fall through to COLORMATRIX */
		/* fall through */
	case MNOTE_OLYMPUS_TAG_COLORMATRIX:
		CF (entry->format, EXIF_FORMAT_SHORT, v, maxlen)
		if (entry->tag == MNOTE_OLYMPUS_TAG_COLORMATRIX)
			CC (entry->components, 9, v, maxlen)
		for (i=0; i < (int)entry->components; ++i) {
			vs = exif_get_short (entry->data+2*i, entry->order);
			sprintf (buf, "%hu ", vs);
			strncat (v, buf, maxlen - strlen (v));
		}
		break;
	case MNOTE_NIKON1_TAG_FOCUS:
	case MNOTE_NIKON_TAG_DIGITALZOOM:
	case MNOTE_NIKON1_TAG_DIGITALZOOM:
	case MNOTE_OLYMPUS_TAG_FOCALPLANEDIAGONAL:
		CF (entry->format, EXIF_FORMAT_RATIONAL, v, maxlen)
		/* Fall through to default handler for display */
		/* fall through */
	default:
		switch (entry->format) {
		case EXIF_FORMAT_ASCII:
			strncpy (v, (char *)entry->data, MIN (maxlen, entry->size));
			break;
		case EXIF_FORMAT_SHORT:
			CC (entry->components, 1, v, maxlen)
			vs = exif_get_short (entry->data, entry->order);
			snprintf (v, maxlen, "%hu", vs);
			break;
		case EXIF_FORMAT_SSHORT:
			CC (entry->components, 1, v, maxlen)
			vss = exif_get_sshort (entry->data, entry->order);
			snprintf (v, maxlen, "%hi", vss);
			break;
		case EXIF_FORMAT_LONG:
			CC (entry->components, 1, v, maxlen)
			vl = exif_get_long (entry->data, entry->order);
			snprintf (v, maxlen, "%lu", (long unsigned) vl);
			break;
		case EXIF_FORMAT_SLONG:
			CC (entry->components, 1, v, maxlen)
			vsl = exif_get_slong (entry->data, entry->order);
			snprintf (v, maxlen, "%li", (long int) vsl);
			break;
		case EXIF_FORMAT_RATIONAL:
			CC (entry->components, 1, v, maxlen)
			vr = exif_get_rational (entry->data, entry->order);
			if (!vr.denominator) {
				strncpy (v, _("Infinite"), maxlen);
			} else {
				r = R2D(vr);
				snprintf (v, maxlen, "%2.3f", r);
			}
			break;
		case EXIF_FORMAT_SRATIONAL:
			CC (entry->components, 1, v, maxlen)
			vsr = exif_get_srational (entry->data, entry->order);
			if (!vsr.denominator) {
				strncpy (v, _("Infinite"), maxlen);
			} else {
				r = R2D(vsr);
				snprintf (v, maxlen, "%2.3f", r);
			}
			break;
		case EXIF_FORMAT_UNDEFINED:
		default:
			snprintf (v, maxlen, _("%i bytes unknown data: "),
				  entry->size);
			for (i = 0; i < (int)entry->size; i++) {
				sprintf (buf, "%02x", entry->data[i]);
				strncat (v, buf, maxlen - strlen (v));
			}
			break;
		}
		break;
	}

	return (v);
}

// libexif/olympus/exif-mnote-data-olympus.c

/* exif-mnote-data-olympus.c
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




/* Uncomment this to fix a problem with Sanyo MakerNotes. It's probably best
 * not to in most cases because it seems to only affect the thumbnail tag
 * which is duplicated in IFD 1, and fixing the offset could actually cause
 * problems with other software that expects the broken form.
 */
/*#define EXIF_OVERCOME_SANYO_OFFSET_BUG */

#define CHECKOVERFLOW(offset,datasize,structsize) (( (offset) >= (datasize)) || ((structsize) > (datasize)) || ((offset) > (datasize) - (structsize) ))

static OlympusVersion
exif_mnote_data_olympus_identify_variant (const unsigned char *buf,
		unsigned int buf_size);


static void
exif_mnote_data_olympus_clear (ExifMnoteDataOlympus *n)
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
exif_mnote_data_olympus_free (ExifMnoteData *n)
{
	if (!n) return;

	exif_mnote_data_olympus_clear ((ExifMnoteDataOlympus *) n);
}

static char *
exif_mnote_data_olympus_get_value (ExifMnoteData *d, unsigned int i, char *val, unsigned int maxlen)
{
	ExifMnoteDataOlympus *n = (ExifMnoteDataOlympus *) d;

	if (!d || !val) return NULL;
	if (i >= n->count) return NULL;
/*
	exif_log (d->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataOlympus",
		  "Querying value for tag '%s'...",
		  mnote_olympus_tag_get_name (n->entries[i].tag));
*/
	return mnote_olympus_entry_get_value (&n->entries[i], val, maxlen);
}




/** 
 * @brief save the MnoteData from ne to buf
 * 
 * @param ne extract the data from this structure 
 * @param *buf write the mnoteData to this buffer (buffer will be allocated)
 * @param buf_size the size of the buffer
 */
static void
exif_mnote_data_olympus_save (ExifMnoteData *ne,
		unsigned char **buf, unsigned int *buf_size)
{
	ExifMnoteDataOlympus *n = (ExifMnoteDataOlympus *) ne;
	size_t i, o, s, doff, base = 0, o2 = 6 + 2;
	size_t datao = 0;
	unsigned char *t;
	size_t ts;

	if (!n || !buf || !buf_size) return;

	/*
	 * Allocate enough memory for all entries and the number of entries.
	 */
	*buf_size = 6 + 2 + 2 + n->count * 12;
	switch (n->version) {
	case olympusV1:
	case sanyoV1:
	case epsonV1:
		*buf = exif_mem_alloc (ne->mem, *buf_size);
		if (!*buf) {
			EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataOlympus", *buf_size);
			return;
		}

		/* Write the header and the number of entries. */
		strcpy ((char *)*buf, n->version==sanyoV1?"SANYO":
					(n->version==epsonV1?"EPSON":"OLYMP"));
		exif_set_short (*buf + 6, n->order, (ExifShort) 1);
		datao = n->offset;
		break;

	case olympusV2:
		*buf_size += 8-6 + 4;
		*buf = exif_mem_alloc (ne->mem, *buf_size);
		if (!*buf) {
			EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataOlympus", *buf_size);
			return;
		}

		/* Write the header and the number of entries. */
		strcpy ((char *)*buf, "OLYMPUS");
		exif_set_short (*buf + 8, n->order, (ExifShort) (
			(n->order == EXIF_BYTE_ORDER_INTEL) ?
			('I' << 8) | 'I' :
			('M' << 8) | 'M'));
		exif_set_short (*buf + 10, n->order, (ExifShort) 3);
		o2 += 4;
		break;

	case nikonV1: 
		base = MNOTE_NIKON1_TAG_BASE;

		/* v1 has offsets based to main IFD, not makernote IFD */
		datao += n->offset + 10;
		/* subtract the size here, so the increment in the next case will not harm us */
		*buf_size -= 8 + 2;
		/* fall through */
	/* Fall through to nikonV2 handler */
	case nikonV2: 
	/* Write out V0 files in V2 format */
	case nikonV0: 
		*buf_size += 8 + 2;
		*buf_size += 4; /* Next IFD pointer */
		*buf = exif_mem_alloc (ne->mem, *buf_size);
		if (!*buf) {
			EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataOlympus", *buf_size);
			return;
		}

		/* Write the header and the number of entries. */
		strcpy ((char *)*buf, "Nikon");
		(*buf)[6] = (unsigned char)n->version;

		if (n->version != nikonV1) {
			exif_set_short (*buf + 10, n->order, (ExifShort) (
				(n->order == EXIF_BYTE_ORDER_INTEL) ?
				('I' << 8) | 'I' :
				('M' << 8) | 'M'));
			exif_set_short (*buf + 12, n->order, (ExifShort) 0x2A);
			exif_set_long (*buf + 14, n->order, (ExifShort) 8);
			o2 += 2 + 8;
		}
		datao -= 10;
		/* Reset next IFD pointer */
		exif_set_long (*buf + o2 + 2 + n->count * 12, n->order, 0);
		break;

	default:
		return;
	}

	exif_set_short (*buf + o2, n->order, (ExifShort) n->count);
	o2 += 2;

	/* Save each entry */
	for (i = 0; i < n->count; i++) {
		o = o2 + i * 12;
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
			doff = *buf_size;
			ts = *buf_size + s;
			t = exif_mem_realloc (ne->mem, *buf,
						 sizeof (char) * ts);
			if (!t) {
				EXIF_LOG_NO_MEMORY(ne->log, "ExifMnoteDataOlympus", ts);
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
}

static void
exif_mnote_data_olympus_load (ExifMnoteData *en,
			      const unsigned char *buf, unsigned int buf_size)
{
	ExifMnoteDataOlympus *n = (ExifMnoteDataOlympus *) en;
	ExifShort c;
	size_t i, tcount, o, o2, datao = 6, base = 0;

	if (!n) return;

	if (!buf || !buf_size) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteDataOlympus", "Short MakerNote");
		return;
	}
	o2 = 6 + n->offset; /* Start of interesting data */
	if (CHECKOVERFLOW(o2,buf_size,10)) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteDataOlympus", "Short MakerNote");
		return;
	}

	/*
	 * Olympus headers start with "OLYMP" and need to have at least
	 * a size of 22 bytes (6 for 'OLYMP', 2 other bytes, 2 for the
	 * number of entries, and 12 for one entry.
	 *
	 * Sanyo format is identical and uses identical tags except that
	 * header starts with "SANYO".
	 *
	 * Epson format is identical and uses identical tags except that
	 * header starts with "EPSON".
	 *
	 * Nikon headers start with "Nikon" (6 bytes including '\0'), 
	 * version number (1 or 2).
	 * 
	 * Version 1 continues with 0, 1, 0, number_of_tags,
	 * or just with number_of_tags (models D1H, D1X...).
	 * 
	 * Version 2 continues with an unknown byte (0 or 10),
	 * two unknown bytes (0), "MM" or "II", another byte 0 and 
	 * lastly 0x2A.
	 */
	n->version = exif_mnote_data_olympus_identify_variant(buf+o2, buf_size-o2);
	switch (n->version) {
	case olympusV1:
	case sanyoV1:
	case epsonV1:
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataOlympus",
			"Parsing Olympus/Sanyo/Epson maker note v1...");

		/* The number of entries is at position 8. */
		if (buf[o2 + 6] == 1)
			n->order = EXIF_BYTE_ORDER_INTEL;
		else if (buf[o2 + 6 + 1] == 1)
			n->order = EXIF_BYTE_ORDER_MOTOROLA;
		o2 += 8;
		c = exif_get_short (buf + o2, n->order);
		if ((!(c & 0xFF)) && (c > 0x500)) {
			if (n->order == EXIF_BYTE_ORDER_INTEL) {
				n->order = EXIF_BYTE_ORDER_MOTOROLA;
			} else {
				n->order = EXIF_BYTE_ORDER_INTEL;
			}
		}
		break;

	case olympusV2:
		/* Olympus S760, S770 */
		datao = o2;
		o2 += 8;
		if (CHECKOVERFLOW(o2,buf_size,4)) return;
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataOlympus",
			"Parsing Olympus maker note v2 (0x%02x, %02x, %02x, %02x)...",
			buf[o2 + 0], buf[o2 + 1], buf[o2 + 2], buf[o2 + 3]);

		if ((buf[o2] == 'I') && (buf[o2 + 1] == 'I'))
			n->order = EXIF_BYTE_ORDER_INTEL;
		else if ((buf[o2] == 'M') && (buf[o2 + 1] == 'M'))
			n->order = EXIF_BYTE_ORDER_MOTOROLA;

		/* The number of entries is at position 8+4. */
		o2 += 4;
		break;

	case nikonV1:
		o2 += 6;
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataOlympus",
			"Parsing Nikon maker note v1 (0x%02x, %02x, %02x, "
			"%02x)...",
			buf[o2 + 0], buf[o2 + 1], buf[o2 + 2], buf[o2 + 3]);

		/* Skip version number */
		o2 += 1;

		/* Skip an unknown byte (00 or 0A). */
		o2 += 1;

		base = MNOTE_NIKON1_TAG_BASE;
		/* Fix endianness, if needed */
		c = exif_get_short (buf + o2, n->order);
		if ((!(c & 0xFF)) && (c > 0x500)) {
			if (n->order == EXIF_BYTE_ORDER_INTEL) {
				n->order = EXIF_BYTE_ORDER_MOTOROLA;
			} else {
				n->order = EXIF_BYTE_ORDER_INTEL;
			}
		}
		break;

	case nikonV2:
		o2 += 6;
		if (CHECKOVERFLOW(o2,buf_size,12)) return;
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataOlympus",
			"Parsing Nikon maker note v2 (0x%02x, %02x, %02x, "
			"%02x, %02x, %02x, %02x, %02x)...",
			buf[o2 + 0], buf[o2 + 1], buf[o2 + 2], buf[o2 + 3],
			buf[o2 + 4], buf[o2 + 5], buf[o2 + 6], buf[o2 + 7]);

		/* Skip version number */
		o2 += 1;

		/* Skip an unknown byte (00 or 0A). */
		o2 += 1;

		/* Skip 2 unknown bytes (00 00). */
		o2 += 2;

		/*
		 * Byte order. From here the data offset
		 * gets calculated.
		 */
		datao = o2;
		if (!strncmp ((char *)&buf[o2], "II", 2))
			n->order = EXIF_BYTE_ORDER_INTEL;
		else if (!strncmp ((char *)&buf[o2], "MM", 2))
			n->order = EXIF_BYTE_ORDER_MOTOROLA;
		else {
			exif_log (en->log, EXIF_LOG_CODE_DEBUG,
				"ExifMnoteDataOlympus", "Unknown "
				"byte order '%c%c'", buf[o2],
				buf[o2 + 1]);
			return;
		}
		o2 += 2;

		/* Skip 2 unknown bytes (00 2A). */
		o2 += 2;

		/* Go to where the number of entries is. */
		if (CHECKOVERFLOW(o2,buf_size,exif_get_long (buf + o2, n->order))) return;
		o2 = datao + exif_get_long (buf + o2, n->order);
		break;

	case nikonV0:
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataOlympus",
			"Parsing Nikon maker note v0 (0x%02x, %02x, %02x, "
			"%02x, %02x, %02x, %02x, %02x)...",
			buf[o2 + 0], buf[o2 + 1], buf[o2 + 2], buf[o2 + 3], 
			buf[o2 + 4], buf[o2 + 5], buf[o2 + 6], buf[o2 + 7]);
		/* 00 1b is # of entries in Motorola order - the rest should also be in MM order */
		n->order = EXIF_BYTE_ORDER_MOTOROLA;
		break;

	default:
		exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteDataOlympus",
			"Unknown Olympus variant %i.", n->version);
		return;
	}

	/* Sanity check the offset */
	if (CHECKOVERFLOW(o2,buf_size,2)) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
			  "ExifMnoteOlympus", "Short MakerNote");
		return;
	}

	/* Read the number of tags */
	c = exif_get_short (buf + o2, n->order);
	o2 += 2;

	/* Just use an arbitrary max tag limit here to avoid needing to much memory or time. There are 150 named tags currently.
	 * The format allows specifying the same range of memory as often as it can, so this multiplies quickly. */
	if (c > 300) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA, "ExifMnoteOlympus", "Too much tags (%d) in Olympus MakerNote", c);
		return;
	}

	/* Remove any old entries */
	exif_mnote_data_olympus_clear (n);

	/* Reserve enough space for all the possible MakerNote tags */
	n->entries = exif_mem_alloc (en->mem, sizeof (MnoteOlympusEntry) * c);
	if (!n->entries) {
		EXIF_LOG_NO_MEMORY(en->log, "ExifMnoteOlympus", sizeof (MnoteOlympusEntry) * c);
		return;
	}

	/* Parse all c entries, storing ones that are successfully parsed */
	tcount = 0;
	for (i = c, o = o2; i; --i, o += 12) {
		size_t s;
		memset(&n->entries[tcount], 0, sizeof(MnoteOlympusEntry));
		if (CHECKOVERFLOW(o, buf_size, 12)) {
			exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA,
				  "ExifMnoteOlympus", "Short MakerNote");
			break;
		}

	    n->entries[tcount].tag        = exif_get_short (buf + o, n->order) + base;
	    n->entries[tcount].format     = exif_get_short (buf + o + 2, n->order);
	    n->entries[tcount].components = exif_get_long (buf + o + 4, n->order);
	    n->entries[tcount].order      = n->order;

	    exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteOlympus",
		      "Loading entry 0x%x ('%s')...", n->entries[tcount].tag,
		      mnote_olympus_tag_get_name (n->entries[tcount].tag));
/*	    exif_log (en->log, EXIF_LOG_CODE_DEBUG, "ExifMnoteOlympus",
			    "0x%x %d %ld*(%d)",
		    n->entries[tcount].tag,
		    n->entries[tcount].format,
		    n->entries[tcount].components,
		    (int)exif_format_get_size(n->entries[tcount].format)); */

	    /* Check if we overflow the multiplication. Use buf_size as the max size for integer overflow detection,
	     * we will check the buffer sizes closer later. */
	    if (exif_format_get_size (n->entries[tcount].format) &&
		buf_size / exif_format_get_size (n->entries[tcount].format) < n->entries[tcount].components
	    ) {
		exif_log (en->log, EXIF_LOG_CODE_CORRUPT_DATA, "ExifMnoteOlympus", "Tag size overflow detected (%u * %lu)", exif_format_get_size (n->entries[tcount].format), n->entries[tcount].components);
		continue;
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
			if (s > 4) {
				/* The data in this case is merely a pointer */
				dataofs = exif_get_long (buf + dataofs, n->order) + datao;
#ifdef EXIF_OVERCOME_SANYO_OFFSET_BUG
				/* Some Sanyo models (e.g. VPC-C5, C40) suffer from a bug when
				 * writing the offset for the MNOTE_OLYMPUS_TAG_THUMBNAILIMAGE
				 * tag in its MakerNote. The offset is actually the absolute
				 * position in the file instead of the position within the IFD.
				 */
			    if (dataofs > (buf_size - s) && n->version == sanyoV1) {
					/* fix pointer */
					dataofs -= datao + 6;
					exif_log (en->log, EXIF_LOG_CODE_DEBUG,
						  "ExifMnoteOlympus",
						  "Inconsistent thumbnail tag offset; attempting to recover");
			    }
#endif
			}
			if (CHECKOVERFLOW(dataofs, buf_size, s)) {
				exif_log (en->log, EXIF_LOG_CODE_DEBUG,
					  "ExifMnoteOlympus",
					  "Tag data past end of buffer (%u > %u)",
					  (unsigned)(dataofs + s), buf_size);
				continue;
			}

			n->entries[tcount].data = exif_mem_alloc (en->mem, s);
			if (!n->entries[tcount].data) {
				EXIF_LOG_NO_MEMORY(en->log, "ExifMnoteOlympus", s);
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
exif_mnote_data_olympus_count (ExifMnoteData *n)
{
	return n ? ((ExifMnoteDataOlympus *) n)->count : 0;
}

static unsigned int
exif_mnote_data_olympus_get_id (ExifMnoteData *d, unsigned int n)
{
	ExifMnoteDataOlympus *note = (ExifMnoteDataOlympus *) d;

	if (!note) return 0;
	if (note->count <= n) return 0;
	return note->entries[n].tag;
}

static const char *
exif_mnote_data_olympus_get_name (ExifMnoteData *d, unsigned int i)
{
	ExifMnoteDataOlympus *n = (ExifMnoteDataOlympus *) d;

	if (!n) return NULL;
	if (i >= n->count) return NULL;
	return mnote_olympus_tag_get_name (n->entries[i].tag);
}

static const char *
exif_mnote_data_olympus_get_title (ExifMnoteData *d, unsigned int i)
{
	ExifMnoteDataOlympus *n = (ExifMnoteDataOlympus *) d;
	
	if (!n) return NULL;
	if (i >= n->count) return NULL;
        return mnote_olympus_tag_get_title (n->entries[i].tag);
}

static const char *
exif_mnote_data_olympus_get_description (ExifMnoteData *d, unsigned int i)
{
	ExifMnoteDataOlympus *n = (ExifMnoteDataOlympus *) d;
	
	if (!n) return NULL;
	if (i >= n->count) return NULL;
        return mnote_olympus_tag_get_description (n->entries[i].tag);
}

static void
exif_mnote_data_olympus_set_byte_order (ExifMnoteData *d, ExifByteOrder o)
{
	ExifByteOrder o_orig;
	ExifMnoteDataOlympus *n = (ExifMnoteDataOlympus *) d;
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
exif_mnote_data_olympus_set_offset (ExifMnoteData *n, unsigned int o)
{
	if (n) ((ExifMnoteDataOlympus *) n)->offset = o;
}

static OlympusVersion
exif_mnote_data_olympus_identify_variant (const unsigned char *buf,
		unsigned int buf_size)
{
	/* Olympus, Nikon, Sanyo, Epson */
	if (buf_size >= 8) {
		/* Match the terminating NUL character, too */
		if (!memcmp (buf, "OLYMPUS", 8))
			   return olympusV2;
		else if (!memcmp (buf, "OLYMP", 6))
			   return olympusV1;
		else if (!memcmp (buf, "SANYO", 6))
			   return sanyoV1;
		else if (!memcmp (buf, "EPSON", 6))
			   return epsonV1;
		else if (!memcmp (buf, "Nikon", 6)) {
			switch (buf[6]) {
				case 1:  return nikonV1;
				case 2:  return nikonV2;
				default: return 0; /* Unrecognized Nikon variant */
			}
		}
	}

	/* Another variant of Nikon */
	if ((buf_size >= 2) && (buf[0] == 0x00) && (buf[1] == 0x1b)) {
		return nikonV0;
	}

	return unrecognized;
}

int
exif_mnote_data_olympus_identify (const ExifData *ed, const ExifEntry *e)
{
	int variant = exif_mnote_data_olympus_identify_variant(e->data, e->size);

	if (variant == nikonV0) {
		/* This variant needs some extra checking with the Make */
		char value[5];
		ExifEntry *em = exif_data_get_entry (ed, EXIF_TAG_MAKE);
		variant = unrecognized;

		if (em) {
			const char *v = exif_entry_get_value (em, value, sizeof(value));
			if (v && (!strncmp (v, "Nikon", sizeof(value)) || 
					  !strncmp (v, "NIKON", sizeof(value)) ))
				/* When saved, this variant will be written out like the
				 * alternative nikonV2 form above instead
				 */
				variant = nikonV0;
		}
	}

	return variant;
}


ExifMnoteData *
exif_mnote_data_olympus_new (ExifMem *mem)
{
	ExifMnoteData *d;

	if (!mem) return NULL;
	
	d = exif_mem_alloc (mem, sizeof (ExifMnoteDataOlympus));
	if (!d) return NULL;

	exif_mnote_data_construct (d, mem);

	/* Set up function pointers */
	d->methods.free            = exif_mnote_data_olympus_free;
	d->methods.set_byte_order  = exif_mnote_data_olympus_set_byte_order;
	d->methods.set_offset      = exif_mnote_data_olympus_set_offset;
	d->methods.load            = exif_mnote_data_olympus_load;
	d->methods.save            = exif_mnote_data_olympus_save;
	d->methods.count           = exif_mnote_data_olympus_count;
	d->methods.get_id          = exif_mnote_data_olympus_get_id;
	d->methods.get_name        = exif_mnote_data_olympus_get_name;
	d->methods.get_title       = exif_mnote_data_olympus_get_title;
	d->methods.get_description = exif_mnote_data_olympus_get_description;
	d->methods.get_value       = exif_mnote_data_olympus_get_value;

	return d;
}

}
