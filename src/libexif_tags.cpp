// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif (exif-tag.c): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.

// Purpose: libexif's tag table: the name, English title and description its table gives a tag in each
// directory it records it in. A tag number can mean different things in different directories, so it
// can have more than one row.

#include "libexif_port.h"

namespace libfiles::detail::libexif
{
	namespace
	{
		struct tag_row
		{
			std::uint16_t tag;
			const char* name;
			const char* title;
			// Empty where libexif's table gives none.
			const char* description;
			// Bit i set where libexif records the tag in directory i: IFD0, IFD1, Exif, GPS, Interop.
			std::uint8_t recorded;
		};

		constexpr tag_row tag_table[] = {
			{0x0000, "GPSVersionID", "GPS Tag Version",
			 "Indicates the version of <GPSInfoIFD>. The version is given as 2.0.0.0. This tag is mandatory when "
			 "<GPSInfo> tag is present. (Note: The <GPSVersionID> tag is given in bytes, unlike the <ExifVersion> "
			 "tag. When the version is 2.0.0.0, the tag value is 02000000.H).",
			 0x08},
			{0x0001, "InteroperabilityIndex", "Interoperability Index",
			 "Indicates the identification of the Interoperability rule. Use \"R98\" for stating ExifR98 Rules. "
			 "Four bytes used including the termination code (NULL). see the separate volume of Recommended Exif "
			 "Interoperability Rules (ExifR98) for other tags used for ExifR98.",
			 0x10},
			{0x0001, "GPSLatitudeRef", "North or South Latitude",
			 "Indicates whether the latitude is north or south latitude. The ASCII value 'N' indicates north "
			 "latitude, and 'S' is south latitude.",
			 0x08},
			{0x0002, "InteroperabilityVersion", "Interoperability Version",
			 "",
			 0x10},
			{0x0002, "GPSLatitude", "Latitude",
			 "Indicates the latitude. The latitude is expressed as three RATIONAL values giving the degrees, "
			 "minutes, and seconds, respectively. When degrees, minutes and seconds are expressed, the format is "
			 "dd/1,mm/1,ss/1. When degrees and minutes are used and, for example, fractions of minutes are given "
			 "up to two decimal places, the format is dd/1,mmmm/100,0/1.",
			 0x08},
			{0x0003, "GPSLongitudeRef", "East or West Longitude",
			 "Indicates whether the longitude is east or west longitude. ASCII 'E' indicates east longitude, and "
			 "'W' is west longitude.",
			 0x08},
			{0x0004, "GPSLongitude", "Longitude",
			 "Indicates the longitude. The longitude is expressed as three RATIONAL values giving the degrees, "
			 "minutes, and seconds, respectively. When degrees, minutes and seconds are expressed, the format is "
			 "ddd/1,mm/1,ss/1. When degrees and minutes are used and, for example, fractions of minutes are given "
			 "up to two decimal places, the format is ddd/1,mmmm/100,0/1.",
			 0x08},
			{0x0005, "GPSAltitudeRef", "Altitude Reference",
			 "Indicates the altitude used as the reference altitude. If the reference is sea level and the "
			 "altitude is above sea level, 0 is given. If the altitude is below sea level, a value of 1 is given "
			 "and the altitude is indicated as an absolute value in the GPSAltitude tag. The reference unit is "
			 "meters. Note that this tag is BYTE type, unlike other reference tags.",
			 0x08},
			{0x0006, "GPSAltitude", "Altitude",
			 "Indicates the altitude based on the reference in GPSAltitudeRef. Altitude is expressed as one "
			 "RATIONAL value. The reference unit is meters.",
			 0x08},
			{0x0007, "GPSTimeStamp", "GPS Time (Atomic Clock)",
			 "Indicates the time as UTC (Coordinated Universal Time). TimeStamp is expressed as three RATIONAL "
			 "values giving the hour, minute, and second.",
			 0x08},
			{0x0008, "GPSSatellites", "GPS Satellites",
			 "Indicates the GPS satellites used for measurements. This tag can be used to describe the number of "
			 "satellites, their ID number, angle of elevation, azimuth, SNR and other information in ASCII "
			 "notation. The format is not specified. If the GPS receiver is incapable of taking measurements, "
			 "value of the tag shall be set to NULL.",
			 0x08},
			{0x0009, "GPSStatus", "GPS Receiver Status",
			 "Indicates the status of the GPS receiver when the image is recorded. 'A' means measurement is in "
			 "progress, and 'V' means the measurement is Interoperability.",
			 0x08},
			{0x000a, "GPSMeasureMode", "GPS Measurement Mode",
			 "Indicates the GPS measurement mode. '2' means two-dimensional measurement and '3' means "
			 "three-dimensional measurement is in progress.",
			 0x08},
			{0x000b, "GPSDOP", "Measurement Precision",
			 "Indicates the GPS DOP (data degree of precision). An HDOP value is written during two-dimensional "
			 "measurement, and PDOP during three-dimensional measurement.",
			 0x08},
			{0x000c, "GPSSpeedRef", "Speed Unit",
			 "Indicates the unit used to express the GPS receiver speed of movement. 'K', 'M' and 'N' represent "
			 "kilometers per hour, miles per hour, and knots.",
			 0x08},
			{0x000d, "GPSSpeed", "Speed of GPS Receiver",
			 "Indicates the speed of GPS receiver movement.",
			 0x08},
			{0x000e, "GPSTrackRef", "Reference for direction of movement",
			 "Indicates the reference for giving the direction of GPS receiver movement. 'T' denotes true "
			 "direction and 'M' is magnetic direction.",
			 0x08},
			{0x000f, "GPSTrack", "Direction of Movement",
			 "Indicates the direction of GPS receiver movement. The range of values is from 0.00 to 359.99.",
			 0x08},
			{0x0010, "GPSImgDirectionRef", "GPS Image Direction Reference",
			 "Indicates the reference for giving the direction of the image when it is captured. 'T' denotes true "
			 "direction and 'M' is magnetic direction.",
			 0x08},
			{0x0011, "GPSImgDirection", "GPS Image Direction",
			 "Indicates the direction of the image when it was captured. The range of values is from 0.00 to "
			 "359.99.",
			 0x08},
			{0x0012, "GPSMapDatum", "Geodetic Survey Data Used",
			 "Indicates the geodetic survey data used by the GPS receiver. If the survey data is restricted to "
			 "Japan, the value of this tag is 'TOKYO' or 'WGS-84'. If a GPS Info tag is recorded, it is strongly "
			 "recommended that this tag be recorded.",
			 0x08},
			{0x0013, "GPSDestLatitudeRef", "Reference For Latitude of Destination",
			 "Indicates whether the latitude of the destination point is north or south latitude. The ASCII value "
			 "'N' indicates north latitude, and 'S' is south latitude.",
			 0x08},
			{0x0014, "GPSDestLatitude", "Latitude of Destination",
			 "Indicates the latitude of the destination point. The latitude is expressed as three RATIONAL values "
			 "giving the degrees, minutes, and seconds, respectively. If latitude is expressed as degrees, "
			 "minutes and seconds, a typical format would be dd/1,mm/1,ss/1. When degrees and minutes are used "
			 "and, for example, fractions of minutes are given up to two decimal places, the format would be "
			 "dd/1,mmmm/100,0/1.",
			 0x08},
			{0x0015, "GPSDestLongitudeRef", "Reference for Longitude of Destination",
			 "Indicates whether the longitude of the destination point is east or west longitude. ASCII 'E' "
			 "indicates east longitude, and 'W' is west longitude.",
			 0x08},
			{0x0016, "GPSDestLongitude", "Longitude of Destination",
			 "Indicates the longitude of the destination point. The longitude is expressed as three RATIONAL "
			 "values giving the degrees, minutes, and seconds, respectively. If longitude is expressed as "
			 "degrees, minutes and seconds, a typical format would be ddd/1,mm/1,ss/1. When degrees and minutes "
			 "are used and, for example, fractions of minutes are given up to two decimal places, the format "
			 "would be ddd/1,mmmm/100,0/1.",
			 0x08},
			{0x0017, "GPSDestBearingRef", "Reference for Bearing of Destination",
			 "Indicates the reference used for giving the bearing to the destination point. 'T' denotes true "
			 "direction and 'M' is magnetic direction.",
			 0x08},
			{0x0018, "GPSDestBearing", "Bearing of Destination",
			 "Indicates the bearing to the destination point. The range of values is from 0.00 to 359.99.",
			 0x08},
			{0x0019, "GPSDestDistanceRef", "Reference for Distance to Destination",
			 "Indicates the unit used to express the distance to the destination point. 'K', 'M' and 'N' "
			 "represent kilometers, miles and nautical miles.",
			 0x08},
			{0x001a, "GPSDestDistance", "Distance to Destination",
			 "Indicates the distance to the destination point.",
			 0x08},
			{0x001b, "GPSProcessingMethod", "Name of GPS Processing Method",
			 "A character string recording the name of the method used for location finding. The first byte "
			 "indicates the character code used, and this is followed by the name of the method. Since the Type "
			 "is not ASCII, NULL termination is not necessary.",
			 0x08},
			{0x001c, "GPSAreaInformation", "Name of GPS Area",
			 "A character string recording the name of the GPS area. The first byte indicates the character code "
			 "used, and this is followed by the name of the GPS area. Since the Type is not ASCII, NULL "
			 "termination is not necessary.",
			 0x08},
			{0x001d, "GPSDateStamp", "GPS Date",
			 "A character string recording date and time information relative to UTC (Coordinated Universal "
			 "Time). The format is \"YYYY:MM:DD\". The length of the string is 11 bytes including NULL.",
			 0x08},
			{0x001e, "GPSDifferential", "GPS Differential Correction",
			 "Indicates whether differential correction is applied to the GPS receiver.",
			 0x08},
			{0x001f, "GPSHPositioningError", "GPS Horizontal Positioning Error",
			 "Indicates the horizontal positioning errors in meters. This is expressed as one RATIONAL value.",
			 0x08},
			{0x00fe, "NewSubfileType", "New Subfile Type",
			 "A general indication of the kind of data contained in this subfile.",
			 0x1f},
			{0x0100, "ImageWidth", "Image Width",
			 "The number of columns of image data, equal to the number of pixels per row. In JPEG compressed data "
			 "a JPEG marker is used instead of this tag.",
			 0x03},
			{0x0101, "ImageLength", "Image Length",
			 "The number of rows of image data. In JPEG compressed data a JPEG marker is used instead of this tag.",
			 0x03},
			{0x0102, "BitsPerSample", "Bits per Sample",
			 "The number of bits per image component. In this standard each component of the image is 8 bits, so "
			 "the value for this tag is 8. See also <SamplesPerPixel>. In JPEG compressed data a JPEG marker is "
			 "used instead of this tag.",
			 0x03},
			{0x0103, "Compression", "Compression",
			 "The compression scheme used for the image data. When a primary image is JPEG compressed, this "
			 "designation is not necessary and is omitted. When thumbnails use JPEG compression, this tag value "
			 "is set to 6.",
			 0x03},
			{0x0106, "PhotometricInterpretation", "Photometric Interpretation",
			 "The pixel composition. In JPEG compressed data a JPEG marker is used instead of this tag.",
			 0x03},
			{0x010a, "FillOrder", "Fill Order",
			 "",
			 0x1f},
			{0x010d, "DocumentName", "Document Name",
			 "",
			 0x1f},
			{0x010e, "ImageDescription", "Image Description",
			 "A character string giving the title of the image. It may be a comment such as \"1988 company "
			 "picnic\" or the like. Two-bytes character codes cannot be used. When a 2-bytes code is necessary, "
			 "the Exif Private tag <UserComment> is to be used.",
			 0x03},
			{0x010f, "Make", "Manufacturer",
			 "The manufacturer of the recording equipment. This is the manufacturer of the DSC, scanner, video "
			 "digitizer or other equipment that generated the image. When the field is left blank, it is treated "
			 "as unknown.",
			 0x03},
			{0x0110, "Model", "Model",
			 "The model name or model number of the equipment. This is the model name or number of the DSC, "
			 "scanner, video digitizer or other equipment that generated the image. When the field is left blank, "
			 "it is treated as unknown.",
			 0x03},
			{0x0111, "StripOffsets", "Strip Offsets",
			 "For each strip, the byte offset of that strip. It is recommended that this be selected so the "
			 "number of strip bytes does not exceed 64 Kbytes. With JPEG compressed data this designation is not "
			 "needed and is omitted. See also <RowsPerStrip> and <StripByteCounts>.",
			 0x03},
			{0x0112, "Orientation", "Orientation",
			 "The image orientation viewed in terms of rows and columns.",
			 0x03},
			{0x0115, "SamplesPerPixel", "Samples per Pixel",
			 "The number of components per pixel. Since this standard applies to RGB and YCbCr images, the value "
			 "set for this tag is 3. In JPEG compressed data a JPEG marker is used instead of this tag.",
			 0x03},
			{0x0116, "RowsPerStrip", "Rows per Strip",
			 "The number of rows per strip. This is the number of rows in the image of one strip when an image is "
			 "divided into strips. With JPEG compressed data this designation is not needed and is omitted. See "
			 "also <StripOffsets> and <StripByteCounts>.",
			 0x03},
			{0x0117, "StripByteCounts", "Strip Byte Count",
			 "The total number of bytes in each strip. With JPEG compressed data this designation is not needed "
			 "and is omitted.",
			 0x03},
			{0x011a, "XResolution", "X-Resolution",
			 "The number of pixels per <ResolutionUnit> in the <ImageWidth> direction. When the image resolution "
			 "is unknown, 72 [dpi] is designated.",
			 0x03},
			{0x011b, "YResolution", "Y-Resolution",
			 "The number of pixels per <ResolutionUnit> in the <ImageLength> direction. The same value as "
			 "<XResolution> is designated.",
			 0x03},
			{0x011c, "PlanarConfiguration", "Planar Configuration",
			 "Indicates whether pixel components are recorded in a chunky or planar format. In JPEG compressed "
			 "files a JPEG marker is used instead of this tag. If this field does not exist, the TIFF default of "
			 "1 (chunky) is assumed.",
			 0x03},
			{0x0128, "ResolutionUnit", "Resolution Unit",
			 "The unit for measuring <XResolution> and <YResolution>. The same unit is used for both "
			 "<XResolution> and <YResolution>. If the image resolution is unknown, 2 (inches) is designated.",
			 0x03},
			{0x012d, "TransferFunction", "Transfer Function",
			 "A transfer function for the image, described in tabular style. Normally this tag is not necessary, "
			 "since color space is specified in the color space information tag (<ColorSpace>).",
			 0x03},
			{0x0131, "Software", "Software",
			 "This tag records the name and version of the software or firmware of the camera or image input "
			 "device used to generate the image. The detailed format is not specified, but it is recommended that "
			 "the example shown below be followed. When the field is left blank, it is treated as unknown.",
			 0x03},
			{0x0132, "DateTime", "Date and Time",
			 "The date and time of image creation. In this standard (EXIF-2.1) it is the date and time the file "
			 "was changed.",
			 0x03},
			{0x013b, "Artist", "Artist",
			 "This tag records the name of the camera owner, photographer or image creator. The detailed format "
			 "is not specified, but it is recommended that the information be written as in the example below for "
			 "ease of Interoperability. When the field is left blank, it is treated as unknown.",
			 0x03},
			{0x013e, "WhitePoint", "White Point",
			 "The chromaticity of the white point of the image. Normally this tag is not necessary, since color "
			 "space is specified in the color space information tag (<ColorSpace>).",
			 0x03},
			{0x013f, "PrimaryChromaticities", "Primary Chromaticities",
			 "The chromaticity of the three primary colors of the image. Normally this tag is not necessary, "
			 "since color space is specified in the color space information tag (<ColorSpace>).",
			 0x03},
			{0x014a, "SubIFDs", "SubIFD Offsets",
			 "Defined by Adobe Corporation to enable TIFF Trees within a TIFF file.",
			 0x1f},
			{0x0156, "TransferRange", "Transfer Range",
			 "",
			 0x1f},
			{0x0200, "JPEGProc", "JPEGProc",
			 "",
			 0x1f},
			{0x0201, "JPEGInterchangeFormat", "JPEG Interchange Format",
			 "The offset to the start byte (SOI) of JPEG compressed thumbnail data. This is not used for primary "
			 "image JPEG data.",
			 0x02},
			{0x0202, "JPEGInterchangeFormatLength", "JPEG Interchange Format Length",
			 "The number of bytes of JPEG compressed thumbnail data. This is not used for primary image JPEG "
			 "data. JPEG thumbnails are not divided but are recorded as a continuous JPEG bitstream from SOI to "
			 "EOI. Appn and COM markers should not be recorded. Compressed thumbnails must be recorded in no more "
			 "than 64 Kbytes, including all other data to be recorded in APP1.",
			 0x02},
			{0x0211, "YCbCrCoefficients", "YCbCr Coefficients",
			 "The matrix coefficients for transformation from RGB to YCbCr image data. No default is given in "
			 "TIFF; but here the value given in \"Color Space Guidelines\", is used as the default. The color "
			 "space is declared in a color space information tag, with the default being the value that gives the "
			 "optimal image characteristics Interoperability this condition.",
			 0x03},
			{0x0212, "YCbCrSubSampling", "YCbCr Sub-Sampling",
			 "The sampling ratio of chrominance components in relation to the luminance component. In JPEG "
			 "compressed data a JPEG marker is used instead of this tag.",
			 0x03},
			{0x0213, "YCbCrPositioning", "YCbCr Positioning",
			 "The position of chrominance components in relation to the luminance component. This field is "
			 "designated only for JPEG compressed data or uncompressed YCbCr data. The TIFF default is 1 "
			 "(centered); but when Y:Cb:Cr = 4:2:2 it is recommended in this standard that 2 (co-sited) be used "
			 "to record data, in order to improve the image quality when viewed on TV systems. When this field "
			 "does not exist, the reader shall assume the TIFF default. In the case of Y:Cb:Cr = 4:2:0, the TIFF "
			 "default (centered) is recommended. If the reader does not have the capability of supporting both "
			 "kinds of <YCbCrPositioning>, it shall follow the TIFF default regardless of the value in this "
			 "field. It is preferable that readers be able to support both centered and co-sited positioning.",
			 0x03},
			{0x0214, "ReferenceBlackWhite", "Reference Black/White",
			 "The reference black point value and reference white point value. No defaults are given in TIFF, but "
			 "the values below are given as defaults here. The color space is declared in a color space "
			 "information tag, with the default being the value that gives the optimal image characteristics "
			 "Interoperability these conditions.",
			 0x03},
			{0x02bc, "XMLPacket", "XML Packet",
			 "XMP Metadata",
			 0x1f},
			{0x1000, "RelatedImageFileFormat", "RelatedImageFileFormat",
			 "",
			 0x1f},
			{0x1001, "RelatedImageWidth", "RelatedImageWidth",
			 "",
			 0x1f},
			{0x1002, "RelatedImageLength", "RelatedImageLength",
			 "",
			 0x1f},
			{0x80e5, "ImageDepth", "ImageDepth",
			 "",
			 0x1f},
			{0x828d, "CFARepeatPatternDim", "CFARepeatPatternDim",
			 "",
			 0x1f},
			{0x828e, "CFAPattern", "CFA Pattern",
			 "Indicates the color filter array (CFA) geometric pattern of the image sensor when a one-chip color "
			 "area sensor is used. It does not apply to all sensing methods.",
			 0x1f},
			{0x828f, "BatteryLevel", "Battery Level",
			 "",
			 0x1f},
			{0x8298, "Copyright", "Copyright",
			 "Copyright information. In this standard the tag is used to indicate both the photographer and "
			 "editor copyrights. It is the copyright notice of the person or organization claiming rights to the "
			 "image. The Interoperability copyright statement including date and rights should be written in this "
			 "field; e.g., \"Copyright, John Smith, 19xx. All rights reserved.\". In this standard the field "
			 "records both the photographer and editor copyrights, with each recorded in a separate part of the "
			 "statement. When there is a clear distinction between the photographer and editor copyrights, these "
			 "are to be written in the order of photographer followed by editor copyright, separated by NULL (in "
			 "this case, since the statement also ends with a NULL, there are two NULL codes) (see example 1). "
			 "When only the photographer is given, it is terminated by one NULL code (see example 2). When only "
			 "the editor copyright is given, the photographer copyright part consists of one space followed by a "
			 "terminating NULL code, then the editor copyright is given (see example 3). When the field is left "
			 "blank, it is treated as unknown.",
			 0x03},
			{0x829a, "ExposureTime", "Exposure Time",
			 "Exposure time, given in seconds (sec).",
			 0x04},
			{0x829d, "FNumber", "F-Number",
			 "The F number.",
			 0x04},
			{0x83bb, "IPTC/NAA", "IPTC/NAA",
			 "",
			 0x1f},
			{0x8649, "ImageResources", "Image Resources Block",
			 "",
			 0x1f},
			{0x8769, "ExifIfdPointer", "Exif IFD Pointer",
			 "A pointer to the Exif IFD. Interoperability, Exif IFD has the same structure as that of the IFD "
			 "specified in TIFF. ordinarily, however, it does not contain image data as in the case of TIFF.",
			 0x00},
			{0x8773, "InterColorProfile", "InterColorProfile",
			 "",
			 0x1f},
			{0x8822, "ExposureProgram", "Exposure Program",
			 "The class of the program used by the camera to set exposure when the picture is taken.",
			 0x04},
			{0x8824, "SpectralSensitivity", "Spectral Sensitivity",
			 "Indicates the spectral sensitivity of each channel of the camera used. The tag value is an ASCII "
			 "string compatible with the standard developed by the ASTM Technical Committee.",
			 0x04},
			{0x8825, "GPSInfoIFDPointer", "GPS Info IFD Pointer",
			 "A pointer to the GPS Info IFD. The Interoperability structure of the GPS Info IFD, like that of "
			 "Exif IFD, has no image data.",
			 0x00},
			{0x8827, "ISOSpeedRatings", "ISO Speed Ratings",
			 "Indicates the ISO Speed and ISO Latitude of the camera or input device as specified in ISO 12232.",
			 0x04},
			{0x8828, "OECF", "Opto-Electronic Conversion Function",
			 "Indicates the Opto-Electronic Conversion Function (OECF) specified in ISO 14524. <OECF> is the "
			 "relationship between the camera optical input and the image values.",
			 0x04},
			{0x882a, "TimeZoneOffset", "Time Zone Offset",
			 "Encodes time zone of camera clock relative to GMT.",
			 0x1f},
			{0x8830, "SensitivityType", "Sensitivity Type",
			 "The SensitivityType tag indicates which one of the parameters of ISO12232 is the "
			 "PhotographicSensitivity tag. Although it is an optional tag, it should be recorded when a "
			 "PhotographicSensitivity tag is recorded. Value = 4, 5, 6, or 7 may be used in case that the values "
			 "of plural parameters are the same.",
			 0x04},
			{0x8831, "StandardOutputSensitivity", "Standard Output Sensitivity",
			 "",
			 0x04},
			{0x8832, "RecommendedExposureIndex", "Recommended Exposure Index",
			 "",
			 0x04},
			{0x8833, "ISOSpeed", "ISO Speed",
			 "",
			 0x04},
			{0x8834, "ISOSpeedLatitudeyyy", "ISO Speed Latitude yyy",
			 "",
			 0x04},
			{0x8835, "ISOSpeedLatitudezzz", "ISO Speed Latitude zzz",
			 "",
			 0x04},
			{0x9000, "ExifVersion", "Exif Version",
			 "The version of this standard supported. Nonexistence of this field is taken to mean nonconformance "
			 "to the standard.",
			 0x04},
			{0x9003, "DateTimeOriginal", "Date and Time (Original)",
			 "The date and time when the original image data was generated. For a digital still camera the date "
			 "and time the picture was taken are recorded.",
			 0x04},
			{0x9004, "DateTimeDigitized", "Date and Time (Digitized)",
			 "The date and time when the image was stored as digital data.",
			 0x04},
			{0x9010, "OffsetTime", "Offset Time For DateTime",
			 "A tag used to record the offset from UTC (the time difference from Universal Time Coordinated "
			 "including daylight saving time) of the time of DateTime tag. The format when recording the offset "
			 "is \"+|-HH:MM\". The part of \"+|-\" shall be recorded as \"+\" or \"-\". When the offset are "
			 "unknown, all the character spaces except colons (\":\")should be filled with blank characters, or "
			 "else the Interoperability field should be filled with blank characters. The character string length "
			 "is 7 Bytes including NULL for termination. When the field is left blank, it is treated as unknown.",
			 0x04},
			{0x9011, "OffsetTimeOriginal", "Offset Time For DateTimeOriginal",
			 "A tag used to record the offset from UTC (the time difference from Universal Time Coordinated "
			 "including daylight saving  time)  of  the time  of  DateTimeOriginal  tag.  The  format  when  "
			 "recording  the  offset  is \"+|-HH:MM\". The part of \"+|-\" shall be recorded as \"+\" or \"-\". "
			 "When the offset are unknown, all the character spaces except colons (\":\")should be filled with "
			 "blank characters, or else the Interoperability field should be filled with blank characters. The "
			 "character string length is 7 Bytes including NULL for termination. When the field is left blank, it "
			 "is treated as unknown.",
			 0x04},
			{0x9012, "OffsetTimeDigitized", "Offset Time For DateTimeDigitized",
			 "A tag used to record the offset from UTC (the time difference from Universal Time Coordinated "
			 "including daylight  saving  time)  of  the time  of  DateTimeDigitized  tag.  The  format  when  "
			 "recording  the  offset  is \"+|-HH:MM\". The part of \"+|-\" shall be recorded as \"+\" or \"-\". "
			 "When the offset are unknown, all the character spaces except colons (\":\")should be filled with "
			 "blank characters, or else the Interoperability field should be filled with blank characters. The "
			 "character string length is 7 Bytes including NULL for termination. When the field is left blank, it "
			 "is treated as unknown.",
			 0x04},
			{0x9101, "ComponentsConfiguration", "Components Configuration",
			 "Information specific to compressed data. The channels of each component are arranged in order from "
			 "the 1st component to the 4th. For uncompressed data the data arrangement is given in the "
			 "<PhotometricInterpretation> tag. However, since <PhotometricInterpretation> can only express the "
			 "order of Y, Cb and Cr, this tag is provided for cases when compressed data uses components other "
			 "than Y, Cb, and Cr and to enable support of other sequences.",
			 0x04},
			{0x9102, "CompressedBitsPerPixel", "Compressed Bits per Pixel",
			 "Information specific to compressed data. The compression mode used for a compressed image is "
			 "indicated in unit bits per pixel.",
			 0x04},
			{0x9201, "ShutterSpeedValue", "Shutter Speed",
			 "Shutter speed. The unit is the APEX (Additive System of Photographic Exposure) setting.",
			 0x04},
			{0x9202, "ApertureValue", "Aperture",
			 "The lens aperture. The unit is the APEX value.",
			 0x04},
			{0x9203, "BrightnessValue", "Brightness",
			 "The value of brightness. The unit is the APEX value. Ordinarily it is given in the range of -99.99 "
			 "to 99.99.",
			 0x04},
			{0x9204, "ExposureBiasValue", "Exposure Bias",
			 "The exposure bias. The units is the APEX value. Ordinarily it is given in the range of -99.99 to "
			 "99.99.",
			 0x04},
			{0x9205, "MaxApertureValue", "Maximum Aperture Value",
			 "The smallest F number of the lens. The unit is the APEX value. Ordinarily it is given in the range "
			 "of 00.00 to 99.99, but it is not limited to this range.",
			 0x04},
			{0x9206, "SubjectDistance", "Subject Distance",
			 "The distance to the subject, given in meters.",
			 0x04},
			{0x9207, "MeteringMode", "Metering Mode",
			 "The metering mode.",
			 0x04},
			{0x9208, "LightSource", "Light Source",
			 "The kind of light source.",
			 0x04},
			{0x9209, "Flash", "Flash",
			 "This tag is recorded when an image is taken using a strobe light (flash).",
			 0x04},
			{0x920a, "FocalLength", "Focal Length",
			 "The actual focal length of the lens, in mm. Conversion is not made to the focal length of a 35 mm "
			 "film camera.",
			 0x04},
			{0x9214, "SubjectArea", "Subject Area",
			 "This tag indicates the location and area of the main subject in the overall scene.",
			 0x04},
			{0x9216, "TIFF/EPStandardID", "TIFF/EP Standard ID",
			 "",
			 0x1f},
			{0x927c, "MakerNote", "Maker Note",
			 "A tag for manufacturers of Exif writers to record any desired information. The contents are up to "
			 "the manufacturer.",
			 0x04},
			{0x9286, "UserComment", "User Comment",
			 "A tag for Exif users to write keywords or comments on the image besides those in "
			 "<ImageDescription>, and without the character code limitations of the <ImageDescription> tag. The "
			 "character code used in the <UserComment> tag is identified based on an ID code in a fixed 8-byte "
			 "area at the start of the tag data area. The unused portion of the area is padded with NULL "
			 "(\"00.h\"). ID codes are assigned by means of registration. The designation method and references "
			 "for each character code are defined in the specification. The value of CountN is determined based "
			 "on the 8 bytes in the character code area and the number of bytes in the user comment part. Since "
			 "the TYPE is not ASCII, NULL termination is not necessary. The ID code for the <UserComment> area "
			 "may be a Defined code such as JIS or ASCII, or may be Undefined. The Undefined name is "
			 "UndefinedText, and the ID code is filled with 8 bytes of all \"NULL\" (\"00.H\"). An Exif reader "
			 "that reads the <UserComment> tag must have a function for determining the ID code. This function is "
			 "not required in Exif readers that do not use the <UserComment> tag. When a <UserComment> area is "
			 "set aside, it is recommended that the ID code be ASCII and that the following user comment part be "
			 "filled with blank characters [20.H].",
			 0x04},
			{0x9290, "SubsecTime", "Sub-second Time",
			 "A tag used to record fractions of seconds for the <DateTime> tag.",
			 0x04},
			{0x9291, "SubSecTimeOriginal", "Sub-second Time (Original)",
			 "A tag used to record fractions of seconds for the <DateTimeOriginal> tag.",
			 0x04},
			{0x9292, "SubSecTimeDigitized", "Sub-second Time (Digitized)",
			 "A tag used to record fractions of seconds for the <DateTimeDigitized> tag.",
			 0x04},
			{0x9c9b, "XPTitle", "XP Title",
			 "A character string giving the title of the image, encoded in UTF-16LE.",
			 0x01},
			{0x9c9c, "XPComment", "XP Comment",
			 "A character string containing a comment about the image, encoded in UTF-16LE.",
			 0x01},
			{0x9c9d, "XPAuthor", "XP Author",
			 "A character string containing the name of the image creator, encoded in UTF-16LE.",
			 0x01},
			{0x9c9e, "XPKeywords", "XP Keywords",
			 "A character string containing key words describing the image, encoded in UTF-16LE.",
			 0x01},
			{0x9c9f, "XPSubject", "XP Subject",
			 "A character string giving the image subject, encoded in UTF-16LE.",
			 0x01},
			{0xa000, "FlashpixVersion", "FlashPixVersion",
			 "The FlashPix format version supported by a FPXR file.",
			 0x04},
			{0xa001, "ColorSpace", "Color Space",
			 "The color space information tag is always recorded as the color space specifier. Normally sRGB (=1) "
			 "is used to define the color space based on the PC monitor conditions and environment. If a color "
			 "space other than sRGB is used, Uncalibrated (=FFFF.H) is set. Image data recorded as Uncalibrated "
			 "can be treated as sRGB when it is converted to FlashPix.",
			 0x04},
			{0xa002, "PixelXDimension", "Pixel X Dimension",
			 "Information specific to compressed data. When a compressed file is recorded, the valid width of the "
			 "meaningful image must be recorded in this tag, whether or not there is padding data or a restart "
			 "marker. This tag should not exist in an uncompressed file.",
			 0x04},
			{0xa003, "PixelYDimension", "Pixel Y Dimension",
			 "Information specific to compressed data. When a compressed file is recorded, the valid height of "
			 "the meaningful image must be recorded in this tag, whether or not there is padding data or a "
			 "restart marker. This tag should not exist in an uncompressed file. Since data padding is "
			 "unnecessary in the vertical direction, the number of lines recorded in this valid image height tag "
			 "will in fact be the same as that recorded in the SOF.",
			 0x04},
			{0xa004, "RelatedSoundFile", "Related Sound File",
			 "This tag is used to record the name of an audio file related to the image data. The only relational "
			 "information recorded here is the Exif audio file name and extension (an ASCII string consisting of "
			 "8 characters + '.' + 3 characters). The path is not recorded. Stipulations on audio and file naming "
			 "conventions are defined in the specification. When using this tag, audio files must be recorded in "
			 "conformance to the Exif audio format. Writers are also allowed to store the data such as Audio "
			 "within APP2 as FlashPix extension stream data. The mapping of Exif image files and audio files is "
			 "done in any of three ways, [1], [2] and [3]. If multiple files are mapped to one file as in [2] or "
			 "[3], the above format is used to record just one audio file name. If there are multiple audio "
			 "files, the first recorded file is given. In the case of [3], for example, for the Exif image file "
			 "\"DSC00001.JPG\" only  \"SND00001.WAV\" is given as the related Exif audio file. When there are "
			 "three Exif audio files \"SND00001.WAV\", \"SND00002.WAV\" and \"SND00003.WAV\", the Exif image file "
			 "name for each of them, \"DSC00001.JPG\", is indicated. By combining multiple relational "
			 "information, a variety of playback possibilities can be supported. The method of using relational "
			 "information is left to the implementation on the playback side. Since this information is an ASCII "
			 "character string, it is terminated by NULL. When this tag is used to map audio files, the relation "
			 "of the audio file to image data must also be indicated on the audio file end.",
			 0x04},
			{0xa005, "InteroperabilityIFDPointer", "Interoperability IFD Pointer",
			 "Interoperability IFD is composed of tags which stores the information to ensure the "
			 "Interoperability and pointed by the following tag located in Exif IFD. The Interoperability "
			 "structure of Interoperability IFD is the same as TIFF defined IFD structure but does not contain "
			 "the image data characteristically compared with normal TIFF IFD.",
			 0x00},
			{0xa20b, "FlashEnergy", "Flash Energy",
			 "Indicates the strobe energy at the time the image is captured, as measured in Beam Candle Power "
			 "Seconds (BCPS).",
			 0x04},
			{0xa20c, "SpatialFrequencyResponse", "Spatial Frequency Response",
			 "This tag records the camera or input device spatial frequency table and SFR values in the direction "
			 "of image width, image height, and diagonal direction, as specified in ISO 12233.",
			 0x04},
			{0xa20e, "FocalPlaneXResolution", "Focal Plane X-Resolution",
			 "Indicates the number of pixels in the image width (X) direction per <FocalPlaneResolutionUnit> on "
			 "the camera focal plane.",
			 0x04},
			{0xa20f, "FocalPlaneYResolution", "Focal Plane Y-Resolution",
			 "Indicates the number of pixels in the image height (V) direction per <FocalPlaneResolutionUnit> on "
			 "the camera focal plane.",
			 0x04},
			{0xa210, "FocalPlaneResolutionUnit", "Focal Plane Resolution Unit",
			 "Indicates the unit for measuring <FocalPlaneXResolution> and <FocalPlaneYResolution>. This value is "
			 "the same as the <ResolutionUnit>.",
			 0x04},
			{0xa214, "SubjectLocation", "Subject Location",
			 "Indicates the location of the main subject in the scene. The value of this tag represents the pixel "
			 "at the center of the main subject relative to the left edge, prior to rotation processing as per "
			 "the <Rotation> tag. The first value indicates the X column number and the second indicates the Y "
			 "row number.",
			 0x04},
			{0xa215, "ExposureIndex", "Exposure Index",
			 "Indicates the exposure index selected on the camera or input device at the time the image is "
			 "captured.",
			 0x04},
			{0xa217, "SensingMethod", "Sensing Method",
			 "Indicates the image sensor type on the camera or input device.",
			 0x04},
			{0xa300, "FileSource", "File Source",
			 "Indicates the image source. If a DSC recorded the image, the tag value of this tag always be set to "
			 "3, indicating that the image was recorded on a DSC.",
			 0x04},
			{0xa301, "SceneType", "Scene Type",
			 "Indicates the type of scene. If a DSC recorded the image, this tag value must always be set to 1, "
			 "indicating that the image was directly photographed.",
			 0x04},
			{0xa302, "CFAPattern", "CFA Pattern",
			 "Indicates the color filter array (CFA) geometric pattern of the image sensor when a one-chip color "
			 "area sensor is used. It does not apply to all sensing methods.",
			 0x04},
			{0xa401, "CustomRendered", "Custom Rendered",
			 "This tag indicates the use of special processing on image data, such as rendering geared to output. "
			 "When special processing is performed, the reader is expected to disable or minimize any further "
			 "processing.",
			 0x04},
			{0xa402, "ExposureMode", "Exposure Mode",
			 "This tag indicates the exposure mode set when the image was shot. In auto-bracketing mode, the "
			 "camera shoots a series of frames of the same scene at different exposure settings.",
			 0x04},
			{0xa403, "WhiteBalance", "White Balance",
			 "This tag indicates the white balance mode set when the image was shot.",
			 0x04},
			{0xa404, "DigitalZoomRatio", "Digital Zoom Ratio",
			 "This tag indicates the digital zoom ratio when the image was shot. If the numerator of the recorded "
			 "value is 0, this indicates that digital zoom was not used.",
			 0x04},
			{0xa405, "FocalLengthIn35mmFilm", "Focal Length in 35mm Film",
			 "This tag indicates the equivalent focal length assuming a 35mm film camera, in mm. A value of 0 "
			 "means the focal length is unknown. Note that this tag differs from the FocalLength tag.",
			 0x04},
			{0xa406, "SceneCaptureType", "Scene Capture Type",
			 "This tag indicates the type of scene that was shot. It can also be used to record the mode in which "
			 "the image was shot. Note that this differs from the scene type <SceneType> tag.",
			 0x04},
			{0xa407, "GainControl", "Gain Control",
			 "This tag indicates the degree of overall image gain adjustment.",
			 0x04},
			{0xa408, "Contrast", "Contrast",
			 "This tag indicates the direction of contrast processing applied by the camera when the image was "
			 "shot.",
			 0x04},
			{0xa409, "Saturation", "Saturation",
			 "This tag indicates the direction of saturation processing applied by the camera when the image was "
			 "shot.",
			 0x04},
			{0xa40a, "Sharpness", "Sharpness",
			 "This tag indicates the direction of sharpness processing applied by the camera when the image was "
			 "shot.",
			 0x04},
			{0xa40b, "DeviceSettingDescription", "Device Setting Description",
			 "This tag indicates information on the picture-taking conditions of a particular camera model. The "
			 "tag is used only to indicate the picture-taking conditions in the reader.",
			 0x04},
			{0xa40c, "SubjectDistanceRange", "Subject Distance Range",
			 "This tag indicates the distance to the subject.",
			 0x04},
			{0xa420, "ImageUniqueID", "Image Unique ID",
			 "This tag indicates an identifier assigned uniquely to each image. It is recorded as an ASCII string "
			 "equivalent to hexadecimal notation and 128-bit fixed length.",
			 0x04},
			{0xa430, "CameraOwnerName", "Camera Owner Name",
			 "This tag indicates the name of the camera owner, photographer or image creator.",
			 0x04},
			{0xa431, "BodySerialNumber", "Body Serial Number",
			 "This tag indicates the serial number of the body of the camera",
			 0x04},
			{0xa432, "LensSpecification", "Lens Specification",
			 "This tag indicates minimum focal length, maximum focal length, minimum F number in the minimum "
			 "focal length, and minimum F number in the maximum focal length.",
			 0x04},
			{0xa433, "LensMake", "Lens Make",
			 "This tag indicates the lens manufacturer.",
			 0x04},
			{0xa434, "LensModel", "Lens Model",
			 "This tag indicates the lens' model name and model number.",
			 0x04},
			{0xa435, "LensSerialNumber", "Lens Serial Number",
			 "This tag indicates the serial number of the interchangeable lens.",
			 0x04},
			{0xa460, "CompositeImage", "Composite Image",
			 "This tag indicates whether this image was composed from multiple images",
			 0x04},
			{0xa461, "SourceImageNumberOfCompositeImage", "Source Image Number Of Composite Image",
			 "This tag indicates how many images are included and used in the composition of this image",
			 0x04},
			{0xa462, "SourceExposureTimesOfCompositeImage", "Source Exposure Times of Composite Image",
			 "This tag indicates the exposure times of the source images of this image",
			 0x04},
			{0xa500, "Gamma", "Gamma",
			 "Indicates the value of coefficient gamma.",
			 0x04},
			{0xc4a5, "PrintImageMatching", "PRINT Image Matching",
			 "Related to Epson's PRINT Image Matching technology",
			 0x1f},
			{0xea1c, "Padding", "Padding",
			 "This tag reserves space that can be reclaimed later when additional metadata are added. New "
			 "metadata can be written in place by replacing this tag with a smaller data element and using the "
			 "reclaimed space to store the new or expanded metadata tags.",
			 0x05},
		};

		// The first row for a tag, in table order, or the table's end.
		const tag_row* first_row(const unsigned int tag)
		{
			for (const auto& row : tag_table)
			{
				if (row.tag == tag) return &row;
				if (row.tag > tag) break;
			}

			return std::end(tag_table);
		}

		// The first row for a tag that libexif records in a directory, or none.
		const tag_row* recorded_row(const unsigned int tag, const ExifIfd ifd)
		{
			if (ifd >= EXIF_IFD_COUNT) return nullptr;

			for (const auto* row = first_row(tag); row != std::end(tag_table) && row->tag == tag; ++row)
			{
				if (row->recorded & 1u << ifd) return row;
			}

			return nullptr;
		}
	}

	const char* exif_tag_get_name(const ExifTag tag)
	{
		const auto* const row = first_row(tag);
		return row != std::end(tag_table) ? row->name : nullptr;
	}

	const char* exif_tag_get_name_in_ifd(const ExifTag tag, const ExifIfd ifd)
	{
		const auto* const row = recorded_row(tag, ifd);
		return row ? row->name : nullptr;
	}

	const char* exif_tag_get_title_in_ifd(const ExifTag tag, const ExifIfd ifd)
	{
		const auto* const row = recorded_row(tag, ifd);
		return row ? row->title : nullptr;
	}

	const char* exif_tag_get_description_in_ifd(const ExifTag tag, const ExifIfd ifd)
	{
		const auto* const row = recorded_row(tag, ifd);
		return row ? row->description : nullptr;
	}

	ExifEntry* exif_content_get_entry(ExifContent* const content, const ExifTag tag)
	{
		if (!content) return nullptr;

		for (unsigned int i = 0; i < content->count; i++)
		{
			if (content->entries[i]->tag == tag) return content->entries[i];
		}

		return nullptr;
	}

	ExifIfd exif_content_get_ifd(ExifContent* const c)
	{
		if (!c || !c->parent) return EXIF_IFD_COUNT;

		for (const auto ifd : {EXIF_IFD_EXIF, EXIF_IFD_0, EXIF_IFD_1, EXIF_IFD_GPS, EXIF_IFD_INTEROPERABILITY})
		{
			if (c->parent->ifd[ifd] == c) return ifd;
		}

		return EXIF_IFD_COUNT;
	}
}
