// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The dataset table is libiptcdata's (iptc-tag.c): Copyright (c) 2005 David Moore <dcm@acm.org>,
// LGPL-2.0-or-later.

// Purpose: Reading an IPTC-IIM block: the walk over its datasets, the IIM table that names them, and
// the fields and listing read from them. Moved from Diffractor, which read IPTC this way.

#include "libfiles/iptc.h"

#include "iptc_internal.h"
#include "text_internal.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace libfiles
{
	namespace
	{
		enum iptc_record
		{
			IPTC_RECORD_OBJECT_ENV = 1,
			IPTC_RECORD_APP_2 = 2,
			IPTC_RECORD_APP_3 = 3,
			IPTC_RECORD_APP_4 = 4,
			IPTC_RECORD_APP_5 = 5,
			IPTC_RECORD_APP_6 = 6,
			IPTC_RECORD_PREOBJ_DATA = 7,
			IPTC_RECORD_OBJ_DATA = 8,
			IPTC_RECORD_POSTOBJ_DATA = 9
		};

		enum iptc_tag
		{
			IPTC_TAG_MODEL_VERSION = 0,
			/* Begin record 1 tags */
			IPTC_TAG_DESTINATION = 5,
			IPTC_TAG_FILE_FORMAT = 20,
			IPTC_TAG_FILE_VERSION = 22,
			IPTC_TAG_SERVICE_ID = 30,
			IPTC_TAG_ENVELOPE_NUM = 40,
			IPTC_TAG_PRODUCT_ID = 50,
			IPTC_TAG_ENVELOPE_PRIORITY = 60,
			IPTC_TAG_DATE_SENT = 70,
			IPTC_TAG_TIME_SENT = 80,
			IPTC_TAG_CHARACTER_SET = 90,
			IPTC_TAG_UNO = 100,
			IPTC_TAG_ARM_ID = 120,
			IPTC_TAG_ARM_VERSION = 122,
			/* End record 1 tags */
			IPTC_TAG_RECORD_VERSION = 0,
			/* Begin record 2 tags */
			IPTC_TAG_OBJECT_TYPE = 3,
			IPTC_TAG_OBJECT_ATTRIBUTE = 4,
			IPTC_TAG_OBJECT_NAME = 5,
			IPTC_TAG_EDIT_STATUS = 7,
			IPTC_TAG_EDITORIAL_UPDATE = 8,
			IPTC_TAG_URGENCY = 10,
			IPTC_TAG_SUBJECT_REFERENCE = 12,
			IPTC_TAG_CATEGORY = 15,
			IPTC_TAG_SUPPL_CATEGORY = 20,
			IPTC_TAG_FIXTURE_ID = 22,
			IPTC_TAG_KEYWORDS = 25,
			IPTC_TAG_CONTENT_LOC_CODE = 26,
			IPTC_TAG_CONTENT_LOC_NAME = 27,
			IPTC_TAG_RELEASE_DATE = 30,
			IPTC_TAG_RELEASE_TIME = 35,
			IPTC_TAG_EXPIRATION_DATE = 37,
			IPTC_TAG_EXPIRATION_TIME = 38,
			IPTC_TAG_SPECIAL_INSTRUCTIONS = 40,
			IPTC_TAG_ACTION_ADVISED = 42,
			IPTC_TAG_REFERENCE_SERVICE = 45,
			IPTC_TAG_REFERENCE_DATE = 47,
			IPTC_TAG_REFERENCE_NUMBER = 50,
			IPTC_TAG_DATE_CREATED = 55,
			IPTC_TAG_TIME_CREATED = 60,
			IPTC_TAG_DIGITAL_CREATION_DATE = 62,
			IPTC_TAG_DIGITAL_CREATION_TIME = 63,
			IPTC_TAG_ORIGINATING_PROGRAM = 65,
			IPTC_TAG_PROGRAM_VERSION = 70,
			IPTC_TAG_OBJECT_CYCLE = 75,
			IPTC_TAG_BYLINE = 80,
			IPTC_TAG_BYLINE_TITLE = 85,
			IPTC_TAG_CITY = 90,
			IPTC_TAG_SUBLOCATION = 92,
			IPTC_TAG_STATE = 95,
			IPTC_TAG_COUNTRY_CODE = 100,
			IPTC_TAG_COUNTRY_NAME = 101,
			IPTC_TAG_ORIG_TRANS_REF = 103,
			IPTC_TAG_HEADLINE = 105,
			IPTC_TAG_CREDIT = 110,
			IPTC_TAG_SOURCE = 115,
			IPTC_TAG_COPYRIGHT_NOTICE = 116,
			IPTC_TAG_PICASA_UNKNOWN = 117,
			IPTC_TAG_CONTACT = 118,
			IPTC_TAG_FLICKRID = 219,
			IPTC_TAG_CAPTION = 120,
			IPTC_TAG_WRITER_EDITOR = 122,
			IPTC_TAG_RASTERIZED_CAPTION = 125,
			IPTC_TAG_IMAGE_TYPE = 130,
			IPTC_TAG_IMAGE_ORIENTATION = 131,
			IPTC_TAG_LANGUAGE_ID = 135,
			IPTC_TAG_AUDIO_TYPE = 150,
			IPTC_TAG_AUDIO_SAMPLING_RATE = 151,
			IPTC_TAG_AUDIO_SAMPLING_RES = 152,
			IPTC_TAG_AUDIO_DURATION = 153,
			IPTC_TAG_AUDIO_OUTCUE = 154,
			IPTC_TAG_PREVIEW_FORMAT = 200,
			IPTC_TAG_PREVIEW_FORMAT_VER = 201,
			IPTC_TAG_PREVIEW_DATA = 202,
			/* End record 2 tags */
			IPTC_TAG_SIZE_MODE = 10,
			/* Begin record 7 tags */
			IPTC_TAG_MAX_SUBFILE_SIZE = 20,
			IPTC_TAG_SIZE_ANNOUNCED = 90,
			IPTC_TAG_MAX_OBJECT_SIZE = 95,
			/* End record 7 tags */
			IPTC_TAG_SUBFILE = 10,
			/* Record 8 tags */
			IPTC_TAG_CONFIRMED_DATA_SIZE = 10 /* Record 9 tags */
		};

		enum iptc_format
		{
			IPTC_FORMAT_UNKNOWN,
			IPTC_FORMAT_BINARY,
			IPTC_FORMAT_BYTE,
			IPTC_FORMAT_SHORT,
			IPTC_FORMAT_LONG,
			IPTC_FORMAT_STRING,
			IPTC_FORMAT_NUMERIC_STRING,
			IPTC_FORMAT_DATE,
			IPTC_FORMAT_TIME
		};


		enum iptc_mandatory
		{
			IPTC_OPTIONAL = 0,
			IPTC_MANDATORY = 1
		};

		enum iptc_repeatable
		{
			IPTC_NOT_REPEATABLE = 0,
			IPTC_REPEATABLE = 1
		};

		struct iptc_tag_info
		{
			iptc_record record;
			iptc_tag tag;
			std::string_view name;
			std::string_view title;
			std::string_view description;
			iptc_format format;
			iptc_mandatory mandatory;
			iptc_repeatable repeatable;
			uint32_t minbytes;
			uint32_t maxbytes;
		};

		static const iptc_tag_info iptc_tag_table[] = {
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_MODEL_VERSION,
				"ModelVersion",
				"Model Version",
				"Version of IIM part 1.",
				IPTC_FORMAT_SHORT, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_DESTINATION,
				"Destination",
				"Destination",
				"Routing information.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 1024
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_FILE_FORMAT,
				"FileFormat",
				"File Format",
				"File format of the data described by this metadata.",
				IPTC_FORMAT_SHORT, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_FILE_VERSION,
				"FileVersion",
				"File Version",
				"Version of the file format.",
				IPTC_FORMAT_SHORT, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_SERVICE_ID,
				"ServiceID",
				"Service Identifier",
				"Identifies the provider and product.",
				IPTC_FORMAT_STRING, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 0, 10
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_ENVELOPE_NUM,
				"EnvelopeNum",
				"Envelope Number",
				"A number unique for the date in 1:70 and the service ID in 1:30.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_PRODUCT_ID,
				"ProductID",
				"Product I.D.",
				"Allows a provider to identify subsets of its overall service.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_ENVELOPE_PRIORITY,
				"EnvelopePriority",
				"Envelope Priority",
				"Specifies the envelope handling priority and not the editorial urgency.  '1' for most urgent, '5' for normal, and '8' for least urgent.  '9' is user-defined.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 1, 1
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_DATE_SENT,
				"DateSent",
				"Date Sent",
				"The day the service sent the material.",
				IPTC_FORMAT_DATE, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_TIME_SENT,
				"TimeSent",
				"Time Sent",
				"The time the service sent the material.",
				IPTC_FORMAT_TIME, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 11, 11
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_CHARACTER_SET,
				"CharacterSet",
				"Coded Character Set",
				"Control functions used for the announcement, invocation or designation of coded character sets.",
				IPTC_FORMAT_BINARY, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_UNO,
				"UNO",
				"Unique Name of Object",
				"An eternal, globally unique identification for the object, independent of provider and for any media form.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 14, 80
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_ARM_ID,
				"ARMID",
				"ARM Identifier",
				"Identifies the Abstract Relationship Method (ARM).",
				IPTC_FORMAT_SHORT, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_OBJECT_ENV, IPTC_TAG_ARM_VERSION,
				"ARMVersion",
				"ARM Version",
				"Identifies the version of the Abstract Relationship Method (ARM).",
				IPTC_FORMAT_SHORT, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_RECORD_VERSION,
				"RecordVersion",
				"Record Version",
				"Identifies the version of the IIM, Part 2",
				IPTC_FORMAT_SHORT, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_OBJECT_TYPE,
				"ObjectType",
				"Object Type Reference",
				"Distinguishes between different types of objects within the IIM.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 3, 67
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_OBJECT_ATTRIBUTE,
				"ObjectAttribute",
				"Object Attribute Reference",
				"Defines the nature of the object independent of the subject.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 4, 68
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_OBJECT_NAME,
				"ObjectName",
				"Object Name",
				"A shorthand reference for the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 64
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_EDIT_STATUS,
				"EditStatus",
				"Edit Status",
				"Status of the object, according to the practice of the provider.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 64
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_EDITORIAL_UPDATE,
				"EditorialUpdate",
				"Editorial Update",
				"Indicates the type of update this object provides to a previous object.  The link to the previous object is made using the ARM.  '01' indicates an additional language.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_URGENCY,
				"Urgency",
				"Urgency",
				"Specifies the editorial urgency of content and not necessarily the envelope handling priority.  '1' is most urgent, '5' normal, and '8' least urgent.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 1, 1
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_SUBJECT_REFERENCE,
				"SubjectRef",
				"Subject Reference",
				"A structured definition of the subject matter.  It must contain an IPR, an 8 digit Subject Reference Number and an optional Subject Name, Subject Matter Name, and Subject Detail Name each separated by a colon (:).",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 13, 236
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_CATEGORY,
				"Category",
				"Category",
				"Identifies the subject of the object in the opinion of the provider (Deprecated).",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 3
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_SUPPL_CATEGORY,
				"SupplCategory",
				"Supplemental Category",
				"Further refines the subject of the object (Deprecated).",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_FIXTURE_ID,
				"FixtureID",
				"Fixture Identifier",
				"Identifies objects that recur often and predictably, enabling users to immediately find or recall such an object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_KEYWORDS,
				"Keywords",
				"Keywords",
				"Used to indicate specific information retrieval words.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 64
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_CONTENT_LOC_CODE,
				"ContentLocCode",
				"Content Location Code",
				"Indicates the code of a country/geographical location referenced by the content of the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 3, 3
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_CONTENT_LOC_NAME,
				"ContentLocName",
				"Content Location Name",
				"A full, publishable name of a country/geographical location referenced by the content of the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 64
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_RELEASE_DATE,
				"ReleaseDate",
				"Release Date",
				"Designates the earliest date the provider intends the object to be used.",
				IPTC_FORMAT_DATE, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_RELEASE_TIME,
				"ReleaseTime",
				"Release Time",
				"Designates the earliest time the provider intends the object to be used.",
				IPTC_FORMAT_TIME, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 11, 11
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_EXPIRATION_DATE,
				"ExpirationDate",
				"Expiration Date",
				"Designates the latest date the provider intends the object to be used.",
				IPTC_FORMAT_DATE, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_EXPIRATION_TIME,
				"ExpirationTime",
				"Expiration Time",
				"Designates the latest time the provider intends the object to be used.",
				IPTC_FORMAT_TIME, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 11, 11
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_SPECIAL_INSTRUCTIONS,
				"SpecialInstructions",
				"Special Instructions",
				"Other editorial instructions concerning the use of the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 256
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_ACTION_ADVISED,
				"ActionAdvised",
				"Action Advised",
				"The type of action that this object provides to a previous object.  '01' Object Kill, '02' Object Replace, '03' Object Append, '04' Object Reference.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_REFERENCE_SERVICE,
				"RefService",
				"Reference Service",
				"The Service Identifier of a prior envelope to which the current object refers.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 10
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_REFERENCE_DATE,
				"RefDate",
				"Reference Date",
				"The date of a prior envelope to which the current object refers.",
				IPTC_FORMAT_DATE, IPTC_OPTIONAL, IPTC_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_REFERENCE_NUMBER,
				"RefNumber",
				"Reference Number",
				"The Envelope Number of a prior envelope to which the current object refers.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_DATE_CREATED,
				"DateCreated",
				"Date Created",
				"The date the intellectual content of the object was created rather than the date of the creation of the physical representation.",
				IPTC_FORMAT_DATE, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_TIME_CREATED,
				"TimeCreated",
				"Time Created",
				"The time the intellectual content of the object was created rather than the date of the creation of the physical representation.",
				IPTC_FORMAT_TIME, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 11, 11
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_DIGITAL_CREATION_DATE,
				"DigitalCreationDate",
				"Digital Creation Date",
				"The date the digital representation of the object was created.",
				IPTC_FORMAT_DATE, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 8, 8
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_DIGITAL_CREATION_TIME,
				"DigitalCreationTime",
				"Digital Creation Time",
				"The time the digital representation of the object was created.",
				IPTC_FORMAT_TIME, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 11, 11
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_ORIGINATING_PROGRAM,
				"OriginatingProgram",
				"Originating Program",
				"The type of program used to originate the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_PROGRAM_VERSION,
				"ProgramVersion",
				"Program Version",
				"The version of the originating program.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 10
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_OBJECT_CYCLE,
				"ObjectCycle",
				"Object Cycle",
				"Where 'a' is morning, 'b' is evening, 'b' is both.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 1, 1
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_BYLINE,
				"Byline",
				"By-line",
				"Name of the creator of the object, e.g. writer, photographer or graphic artist.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_BYLINE_TITLE,
				"BylineTitle",
				"By-line Title",
				"Title of the creator or creators of the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_CITY,
				"City",
				"City",
				"City of object origin.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_SUBLOCATION,
				"Sublocation",
				"Sub-location",
				"The location within a city from which the object originates.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_STATE,
				"State",
				"Province/State",
				"The Province/State where the object originates.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_COUNTRY_CODE,
				"CountryCode",
				"Country Code",
				"The code of the country/primary location where the object was created.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 3, 3
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_COUNTRY_NAME,
				"CountryName",
				"Country Name",
				"The name of the country/primary location where the object was created.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 64
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_ORIG_TRANS_REF,
				"OrigTransRef",
				"Original Transmission Reference",
				"A code representing the location of original transmission.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_HEADLINE,
				"Headline",
				"Headline",
				"A publishable entry providing a synopsis of the contents of the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 256
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_CREDIT,
				"Credit",
				"Credit",
				"Identifies the provider of the object, not necessarily the owner/creator.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_SOURCE,
				"Source",
				"Source",
				"The original owner of the intellectual content of the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_COPYRIGHT_NOTICE,
				"CopyrightNotice",
				"Copyright Notice",
				"Any necessary copyright notice.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 128
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_CONTACT,
				"Contact",
				"Contact",
				"The person or organization which can provide further background information on the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 128
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_CAPTION,
				"Caption",
				"Caption/Abstract",
				"A textual description of the data",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 2000
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_WRITER_EDITOR,
				"WriterEditor",
				"Writer/Editor",
				"The name of the person involved in the writing, editing or correcting the object or caption/abstract",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_REPEATABLE, 0, 32
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_RASTERIZED_CAPTION,
				"RasterizedCaption",
				"Rasterized Caption",
				"Contains rasterized object description and is used where characters that have not been coded are required for the caption.",
				IPTC_FORMAT_BINARY, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 7360, 7360
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_IMAGE_TYPE,
				"ImageType",
				"Image Type",
				"Indicates the data format of the image object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_IMAGE_ORIENTATION,
				"ImageOrientation",
				"Image Orientation",
				"The layout of the image area: 'P' for portrait, 'L' for landscape, and 'S' for square.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 1, 1
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_LANGUAGE_ID,
				"LanguageID",
				"Language Identifier",
				"The major national language of the object, according to the 2-letter codes of ISO 639:1988.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 3
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_AUDIO_TYPE,
				"AudioType",
				"Audio Type",
				"The number of channels and type of audio (music, text, etc.) in the object.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_AUDIO_SAMPLING_RATE,
				"AudioSamplingRate",
				"Audio Sampling Rate",
				"The sampling rate in Hz of the audio data.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 6, 6
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_AUDIO_SAMPLING_RES,
				"AudioSamplingRes",
				"Audio Sampling Resolution",
				"The number of bits in each audio sample.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_AUDIO_DURATION,
				"AudioDuration",
				"Audio Duration",
				"The running time of the audio data in the form HHMMSS.",
				IPTC_FORMAT_NUMERIC_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 6, 6
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_AUDIO_OUTCUE,
				"AudioOutcue",
				"Audio Outcue",
				"The content at the end of the audio data.",
				IPTC_FORMAT_STRING, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 64
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_PREVIEW_FORMAT,
				"PreviewFileFormat",
				"Preview File Format",
				"Binary value indicating the file format of the object preview data in dataset 2:202.",
				IPTC_FORMAT_SHORT, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_PREVIEW_FORMAT_VER,
				"PreviewFileFormatVer",
				"Preview File Format Version",
				"The version of the preview file format specified in 2:200.",
				IPTC_FORMAT_SHORT, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 2, 2
			},
			{
				IPTC_RECORD_APP_2, IPTC_TAG_PREVIEW_DATA,
				"PreviewData",
				"Preview Data",
				"The object preview data",
				IPTC_FORMAT_BINARY, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 256000
			},
			{
				IPTC_RECORD_PREOBJ_DATA, IPTC_TAG_SIZE_MODE,
				"SizeMode",
				"Size Mode",
				"Set to 0 if the size of the object is known and 1 if not known.",
				IPTC_FORMAT_BYTE, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 1, 1
			},
			{
				IPTC_RECORD_PREOBJ_DATA, IPTC_TAG_MAX_SUBFILE_SIZE,
				"MaxSubfileSize",
				"Max Subfile Size",
				"The maximum size for a subfile dataset (8:10) containing a portion of the object data.",
				IPTC_FORMAT_LONG, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 4, 4
			},
			{
				IPTC_RECORD_PREOBJ_DATA, IPTC_TAG_SIZE_ANNOUNCED,
				"ObjectSizeAnnounced",
				"Object Size Announced",
				"The total size of the object data if it is known.",
				IPTC_FORMAT_LONG, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 4, 4
			},
			{
				IPTC_RECORD_PREOBJ_DATA, IPTC_TAG_MAX_OBJECT_SIZE,
				"MaxObjectSize",
				"Maximum Object Size",
				"The largest possible size of the object if the size is not known.",
				IPTC_FORMAT_LONG, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 4, 4
			},
			{
				IPTC_RECORD_OBJ_DATA, IPTC_TAG_SUBFILE,
				"Subfile",
				"Subfile",
				"The object data itself.  Subfiles must be sequential so that the subfiles may be reassembled.",
				IPTC_FORMAT_BINARY, IPTC_MANDATORY, IPTC_REPEATABLE, 0, UINT32_MAX
			},
			{
				IPTC_RECORD_POSTOBJ_DATA, IPTC_TAG_CONFIRMED_DATA_SIZE,
				"ConfirmedDataSize",
				"Confirmed Data Size",
				"Total size of the object data.",
				IPTC_FORMAT_LONG, IPTC_MANDATORY, IPTC_NOT_REPEATABLE, 4, 4
			},
			{static_cast<iptc_record>(0), static_cast<iptc_tag>(0), {}, {}, {}, IPTC_FORMAT_UNKNOWN, IPTC_OPTIONAL, IPTC_NOT_REPEATABLE, 0, 0}
		};

		const iptc_tag_info* find_tag_info(const iptc_record r, const iptc_tag t)
		{
			for (const auto* p = iptc_tag_table; p->record != 0; ++p)
			{
				if (r == p->record && t == p->tag) return p;
			}

			return nullptr;
		}

		// IPTC records carry no dependable encoding marker, so text that is not valid UTF-8 is read as
		// Windows-1252 - what Photoshop and the wire services wrote for years, and what ExifTool assumes.
		// Its curly quotes, dashes and euro sit at 0x80 to 0x9F, where Latin-1 has only controls.
		std::string decode_iptc_text(const std::string_view sv)
		{
			if (sv.empty() || detail::is_utf8(reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()))
			{
				return std::string(sv);
			}

			std::string result;
			result.reserve(sv.size() + sv.size() / 2);

			for (const auto ch : sv)
			{
				detail::append_utf8(result, detail::windows_1252(static_cast<std::uint8_t>(ch)));
			}

			return result;
		}

		// A dataset's text is padded with NULs, which go; its spaces stay.
		std::string_view strip_nul(const std::string_view s)
		{
			constexpr std::string_view nul{"\0", 1};
			const auto start = s.find_first_not_of(nul);
			if (start == std::string_view::npos) return {};
			return s.substr(start, s.find_last_not_of(nul) - start + 1);
		}

		std::string_view trim(const std::string_view s)
		{
			constexpr std::string_view whitespace{" \n\r\t\f\v\0", 7};
			const auto start = s.find_first_not_of(whitespace);
			if (start == std::string_view::npos) return {};
			return s.substr(start, s.find_last_not_of(whitespace) - start + 1);
		}

		std::string iptc_string(const std::string_view sv)
		{
			return std::string(strip_nul(decode_iptc_text(sv)));
		}

		// IPTC ends its lines with CR, read as LF.
		std::string iptc_multiline(const std::string_view sv)
		{
			auto result = decode_iptc_text(sv);
			for (auto& ch : result)
			{
				if (ch == '\r') ch = '\n';
			}
			return std::string(strip_nul(result));
		}

		// Walks the datasets in an IPTC binary block and calls handler(record, dataset, bytes, length) for
		// each non-empty payload. Lengths are attacker controlled, so a malformed or over-long header
		// stops the walk rather than being trusted.
		template <typename Handler>
		void walk_iptc_datasets(const std::span<const std::uint8_t> block, Handler handler)
		{
			std::size_t i = 0;

			// Find the beginning of the IPTC portion of the binary data.
			while (i + 1 < block.size() && (block[i] != 0x1c || block[i + 1] != 0x02))
			{
				i += 1;
			}

			while (const auto h = detail::read_iim_header(block, i))
			{
				// The largest dataset the IIM allows.
				if (h->length > 256000)
				{
					break;
				}

				if (h->length > 0)
				{
					handler(static_cast<iptc_record>(h->record), static_cast<iptc_tag>(h->number), block.data() + h->data_offset,
					        static_cast<std::uint32_t>(h->length));
				}

				i = h->data_offset + h->length;
			}
		}

		std::string_view as_text(const std::uint8_t* const bytes, const std::size_t length)
		{
			return {reinterpret_cast<const char*>(bytes), length};
		}

		void assign_if_set(std::string& field, std::string value)
		{
			if (!value.empty()) field = std::move(value);
		}
	}

	iptc_fields read_iptc(const std::span<const std::uint8_t> block)
	{
		iptc_fields fields;

		walk_iptc_datasets(block, [&fields](const iptc_record record, const iptc_tag dataset, const std::uint8_t* const bytes,
		                                    const std::uint32_t length)
		{
			if (record != IPTC_RECORD_APP_2) return;

			const auto sv = as_text(bytes, length);

			switch (dataset)
			{
			case IPTC_TAG_KEYWORDS: fields.keywords.push_back(iptc_string(sv));
				break;
			case IPTC_TAG_BYLINE: fields.bylines.push_back(iptc_string(sv));
				break;
			case IPTC_TAG_CAPTION: assign_if_set(fields.caption, iptc_multiline(sv));
				break;
			case IPTC_TAG_OBJECT_NAME: assign_if_set(fields.object_name, iptc_string(sv));
				break;
			case IPTC_TAG_CITY: assign_if_set(fields.city, iptc_string(sv));
				break;
			case IPTC_TAG_STATE: assign_if_set(fields.state, iptc_string(sv));
				break;
			case IPTC_TAG_COUNTRY_NAME: assign_if_set(fields.country, iptc_string(sv));
				break;
			case IPTC_TAG_CREDIT: assign_if_set(fields.credit, iptc_string(sv));
				break;
			case IPTC_TAG_SOURCE: assign_if_set(fields.source, iptc_string(sv));
				break;
			case IPTC_TAG_COPYRIGHT_NOTICE: assign_if_set(fields.copyright, iptc_string(sv));
				break;
			case IPTC_TAG_DATE_CREATED: fields.date_created = std::string(trim(sv));
				break;
			case IPTC_TAG_TIME_CREATED: fields.time_created = std::string(trim(sv));
				break;
			case IPTC_TAG_DIGITAL_CREATION_DATE: fields.digital_creation_date = std::string(trim(sv));
				break;
			case IPTC_TAG_DIGITAL_CREATION_TIME: fields.digital_creation_time = std::string(trim(sv));
				break;
			default:
				break;
			}
		});

		return fields;
	}

	std::vector<iptc_listing_entry> list_iptc(const std::span<const std::uint8_t> block)
	{
		std::vector<iptc_listing_entry> result;

		walk_iptc_datasets(block, [&result](const iptc_record record, const iptc_tag tag, const std::uint8_t* const bytes,
		                                    const std::uint32_t length)
		{
			const auto* const info = find_tag_info(record, tag);

			// Preview images and the coded character set escape are binary; rendering them as text fills
			// a listing with unreadable bytes.
			if (info && info->format == IPTC_FORMAT_BINARY) return;

			std::string value;

			if (info && (info->format == IPTC_FORMAT_BYTE || info->format == IPTC_FORMAT_SHORT ||
				info->format == IPTC_FORMAT_LONG))
			{
				std::uint64_t n = 0;
				for (auto b = 0u; b < length && b < 8u; ++b) n = n << 8 | bytes[b];
				value = std::to_string(n);
			}
			else
			{
				value = std::string(strip_nul(decode_iptc_text(as_text(bytes, length))));
			}

			if (!value.empty())
			{
				result.push_back({static_cast<std::uint8_t>(record), static_cast<std::uint8_t>(tag),
				                  info ? std::string(info->title) : std::string("?"), std::move(value)});
			}
		});

		return result;
	}

	std::string iptc_text(const std::span<const std::uint8_t> bytes)
	{
		return decode_iptc_text(as_text(bytes.data(), bytes.size()));
	}
}

namespace libfiles::detail
{
	std::optional<iim_header> read_iim_header(const std::span<const std::uint8_t> block, const std::size_t offset)
	{
		const auto size = block.size();
		if (offset >= size || size - offset < 5 || block[offset] != 0x1C) return std::nullopt;

		iim_header h;
		h.record = block[offset + 1];
		h.number = block[offset + 2];
		h.data_offset = offset + 5;

		const auto field = static_cast<std::size_t>(block[offset + 3]) << 8 | block[offset + 4];

		if (field & 0x8000)
		{
			// IIM 4.1: with the top bit set, the low 15 bits of the size field are not the size - they
			// count the octets that hold it, which follow the field. Zero octets describe no length,
			// and more than four exceed what a reader can hold.
			const auto octets = field & 0x7FFF;
			if (octets == 0 || octets > 4 || octets > size - h.data_offset) return std::nullopt;

			for (std::size_t b = 0; b < octets; ++b) h.length = h.length << 8 | block[h.data_offset + b];
			h.data_offset += octets;
		}
		else
		{
			h.length = field;
		}

		if (h.length > size - h.data_offset) return std::nullopt;
		return h;
	}
}
