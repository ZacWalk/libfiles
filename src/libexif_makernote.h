// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif (exif-mnote-data.h and exif-mnote-data-priv.h): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.

// Purpose: libexif's maker note interface, which each ported maker note decoder implements, and the
// step that picks the decoder for a block's maker note.

#pragma once

#include "libexif_port.h"

namespace libfiles::detail::libexif
{
	typedef struct _ExifMnoteData ExifMnoteData;
	typedef struct _ExifMnoteDataMethods ExifMnoteDataMethods;
	typedef struct _ExifMnoteDataPriv ExifMnoteDataPriv;

	struct _ExifMnoteDataMethods
	{
		void (*free)(ExifMnoteData*);
		void (*save)(ExifMnoteData*, unsigned char**, unsigned int*);
		void (*load)(ExifMnoteData*, const unsigned char*, unsigned int);
		void (*set_offset)(ExifMnoteData*, unsigned int);
		void (*set_byte_order)(ExifMnoteData*, ExifByteOrder);
		unsigned int (*count)(ExifMnoteData*);
		unsigned int (*get_id)(ExifMnoteData*, unsigned int);
		const char* (*get_name)(ExifMnoteData*, unsigned int);
		const char* (*get_title)(ExifMnoteData*, unsigned int);
		const char* (*get_description)(ExifMnoteData*, unsigned int);
		char* (*get_value)(ExifMnoteData*, unsigned int, char* val, unsigned int maxlen);
	};

	struct _ExifMnoteData
	{
		ExifMnoteDataPriv* priv;
		ExifMnoteDataMethods methods;
		ExifLog* log;
		ExifMem* mem;
	};

	void exif_mnote_data_construct(ExifMnoteData*, ExifMem* mem);
	void exif_mnote_data_ref(ExifMnoteData*);
	void exif_mnote_data_unref(ExifMnoteData*);
	void exif_mnote_data_load(ExifMnoteData*, const unsigned char*, unsigned int);
	void exif_mnote_data_save(ExifMnoteData*, unsigned char**, unsigned int*);
	void exif_mnote_data_set_byte_order(ExifMnoteData*, ExifByteOrder);
	void exif_mnote_data_set_offset(ExifMnoteData*, unsigned int);
	unsigned int exif_mnote_data_count(ExifMnoteData*);
	unsigned int exif_mnote_data_get_id(ExifMnoteData*, unsigned int);
	const char* exif_mnote_data_get_name(ExifMnoteData*, unsigned int);
	const char* exif_mnote_data_get_title(ExifMnoteData*, unsigned int);
	const char* exif_mnote_data_get_description(ExifMnoteData*, unsigned int);
	char* exif_mnote_data_get_value(ExifMnoteData*, unsigned int, char* val, unsigned int maxlen);
	void exif_mnote_data_log(ExifMnoteData*, ExifLog*);

	int exif_mnote_data_olympus_identify(const ExifData* ed, const ExifEntry* e);
	ExifMnoteData* exif_mnote_data_olympus_new(ExifMem* mem);
	int exif_mnote_data_canon_identify(const ExifData* ed, const ExifEntry* e);
	ExifMnoteData* exif_mnote_data_canon_new(ExifMem* mem, ExifDataOption o);
	int exif_mnote_data_fuji_identify(const ExifData* ed, const ExifEntry* e);
	ExifMnoteData* exif_mnote_data_fuji_new(ExifMem* mem);
	int exif_mnote_data_pentax_identify(const ExifData* ed, const ExifEntry* e);
	ExifMnoteData* exif_mnote_data_pentax_new(ExifMem* mem);

	// libexif's interpret_maker_note: the decoder for the block's maker note, loaded from the whole
	// block d of ds bytes, or none. offset is where the last maker note entry loaded put its value.
	ExifMnoteData* interpret_maker_note(ExifData& data, ExifMem* mem, unsigned int offset, const unsigned char* d,
	                                    unsigned int ds, ExifDataOption options);
}
