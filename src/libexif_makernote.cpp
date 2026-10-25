// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif (exif-mnote-data.c and exif-data.c): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.

// Purpose: The maker note interface's dispatch, and libexif's choice of decoder: Olympus' family
// first, then Canon's, Fuji's and last Pentax's, whose test is the most general.

#include "libexif_makernote.h"

namespace libfiles::detail::libexif
{
struct _ExifMnoteDataPriv
{
	unsigned int ref_count;
};

void
exif_mnote_data_construct (ExifMnoteData *d, ExifMem *mem)
{
	if (!d || !mem) return;
	if (d->priv) return;
	d->priv = exif_mem_alloc (mem, sizeof (ExifMnoteDataPriv));
	if (!d->priv) return;

	d->priv->ref_count = 1;

	d->mem = mem;
	exif_mem_ref (mem);
}

void
exif_mnote_data_ref (ExifMnoteData *d)
{
	if (d && d->priv) d->priv->ref_count++;
}

static void
exif_mnote_data_free (ExifMnoteData *d)
{
	ExifMem *mem = d ? d->mem : NULL;

	if (!d) return;
	if (d->priv) {
		if (d->methods.free) d->methods.free (d);
		exif_mem_free (mem, d->priv);
		d->priv = NULL;
	}
	exif_log_unref (d->log);
	exif_mem_free (mem, d);
	exif_mem_unref (mem);
}

void
exif_mnote_data_unref (ExifMnoteData *d)
{
	if (!d || !d->priv) return;
	if (d->priv->ref_count > 0) d->priv->ref_count--;
	if (!d->priv->ref_count)
		exif_mnote_data_free (d);
}

void
exif_mnote_data_load (ExifMnoteData *d, const unsigned char *buf,
		      unsigned int buf_size)
{
	if (!d || !d->methods.load) return;
	d->methods.load (d, buf, buf_size);
}

void
exif_mnote_data_save (ExifMnoteData *d, unsigned char **buf,
		      unsigned int *buf_size)
{
	if (!d || !d->methods.save) return;
	d->methods.save (d, buf, buf_size);
}

void
exif_mnote_data_set_byte_order (ExifMnoteData *d, ExifByteOrder o)
{
	if (!d || !d->methods.set_byte_order) return;
	d->methods.set_byte_order (d, o);
}

void
exif_mnote_data_set_offset (ExifMnoteData *d, unsigned int o)
{
	if (!d || !d->methods.set_offset) return;
	d->methods.set_offset (d, o);
}

unsigned int
exif_mnote_data_count (ExifMnoteData *d)
{
	if (!d || !d->methods.count) return 0;
	return d->methods.count (d);
}

unsigned int
exif_mnote_data_get_id (ExifMnoteData *d, unsigned int n)
{
	if (!d || !d->methods.get_id) return 0;
	return d->methods.get_id (d, n);
}

const char *
exif_mnote_data_get_name (ExifMnoteData *d, unsigned int n)
{
	if (!d || !d->methods.get_name) return NULL;
	return d->methods.get_name (d, n);
}

const char *
exif_mnote_data_get_title (ExifMnoteData *d, unsigned int n)
{
	if (!d || !d->methods.get_title) return NULL;
	return d->methods.get_title (d, n);
}
	
const char *
exif_mnote_data_get_description (ExifMnoteData *d, unsigned int n)
{
	if (!d || !d->methods.get_description) return NULL;
	return d->methods.get_description (d, n);
}
	
char *
exif_mnote_data_get_value (ExifMnoteData *d, unsigned int n, char *val, unsigned int maxlen)
{
	if (!d || !d->methods.get_value) return NULL;
	return d->methods.get_value (d, n, val, maxlen);
}

void
exif_mnote_data_log (ExifMnoteData *d, ExifLog *log)
{
	if (!d) return;
	exif_log_unref (d->log);
	d->log = log;
	exif_log_ref (log);
}

	ExifMnoteData* interpret_maker_note(ExifData& data, ExifMem* const mem, const unsigned int offset,
	                                    const unsigned char* const d, const unsigned int ds, const ExifDataOption options)
	{
		ExifEntry* const e = exif_data_get_entry(&data, EXIF_TAG_MAKER_NOTE);
		if (!e) return nullptr;

		ExifMnoteData* md = nullptr;

		if (exif_mnote_data_olympus_identify(&data, e) != 0) md = exif_mnote_data_olympus_new(mem);
		else if (exif_mnote_data_canon_identify(&data, e) != 0) md = exif_mnote_data_canon_new(mem, options);
		else if (exif_mnote_data_fuji_identify(&data, e) != 0) md = exif_mnote_data_fuji_new(mem);
		else if (exif_mnote_data_pentax_identify(&data, e) != 0) md = exif_mnote_data_pentax_new(mem);

		if (md)
		{
			exif_mnote_data_set_byte_order(md, data.order);
			exif_mnote_data_set_offset(md, offset);
			exif_mnote_data_load(md, d, ds);
		}

		return md;
	}
}
