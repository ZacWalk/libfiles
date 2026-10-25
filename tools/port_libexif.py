"""Ports libexif's presentation code into libfiles: its types, tag names, titles and descriptions, value
text and maker note decoders, as src/libexif_*.

    python tools/port_libexif.py <libexif source tree>/libexif

The port is libexif's own text, wrapped for C++ and fed by libfiles' reader; re-running it over a newer
libexif carries its fixes over. It overwrites the files it writes, so changes made to them by hand are
made here instead.
"""
import re
import sys
from pathlib import Path

LIBEXIF = Path(sys.argv[1]) if len(sys.argv) > 1 else None
OUT = Path(__file__).resolve().parent.parent / 'src'

BANNER = '''// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Ported from libexif ({sources}): Copyright (c) 2001-2022 Lutz Mueller
// <lutz@users.sourceforge.net> and the libexif contributors, LGPL-2.0-or-later. The code is libexif's
// own, kept as close to it as C++ allows, so that what it renders reads as it did.
'''


def read(name):
    return (LIBEXIF / name).read_text(encoding='utf-8', errors='strict').replace('\r\n', '\n')


def function(src, name, signature_tail):
    """The text of a top-level C function: its return-type line, its name line and its body to the
    closing brace in column 0."""
    lines = src.split('\n')
    for i, line in enumerate(lines):
        if line.startswith(name) and signature_tail in line:
            start = i - 1
            for j in range(i, len(lines)):
                if lines[j] == '}':
                    return '\n'.join(lines[start:j + 1]) + '\n'
    raise ValueError(f'{name} not found')


def tag_enum():
    header = read('exif-tag.h')
    body = header[header.index('typedef enum {'):header.index('} ExifTag;')]
    entries = re.findall(r'^\s*(EXIF_TAG_\w+)\s*=\s*(0x[0-9a-fA-F]+)', body, re.M)
    defines = re.findall(r'^#define\s+(EXIF_TAG_\w+)\s+(0x[0-9a-fA-F]+|EXIF_TAG_\w+)', header, re.M)
    out = ['\tenum : unsigned int\n\t{']
    for name, value in entries:
        out.append(f'\t\t{name} = {value},')
    for name, value in defines:
        out.append(f'\t\t{name} = {value},')
    out.append('\t};\n\ttypedef unsigned int ExifTag;')
    return '\n'.join(out), {name: int(value, 16) for name, value in entries}, defines


def c_string(literals):
    """The text of adjacent C string literals, as libexif returns it: N_() and _() are identities."""
    out = []
    for literal in literals:
        body = literal[1:-1]
        i = 0
        while i < len(body):
            c = body[i]
            if c == '\\':
                nxt = body[i + 1]
                if nxt in '"\\\'':
                    out.append(nxt)
                elif nxt == 'n':
                    out.append('\n')
                else:
                    raise ValueError(f'escape \\{nxt} in {literal}')
                i += 2
            else:
                out.append(c)
                i += 1
    return ''.join(out)


def string_argument(text, pos):
    """The string argument at pos - a comma, then adjacent literals, perhaps inside N_() - and the
    position after it."""
    pos = re.compile(r'\s*,\s*').match(text, pos).end()
    wrapped = re.compile(r'N_\s*\(\s*').match(text, pos)
    if wrapped:
        pos = wrapped.end()
    literals = []
    literal = re.compile(r'\s*("(?:[^"\\]|\\.)*")')
    while m := literal.match(text, pos):
        literals.append(m.group(1))
        pos = m.end()
    if not literals:
        raise ValueError(f'no string at {text[pos:pos + 40]!r}')
    if wrapped:
        pos = re.compile(r'\s*\)').match(text, pos).end()
    return c_string(literals), pos


def cpp_literal(text, indent, width=100):
    """text as C++ string literals, split at spaces into adjacent literals of about width characters."""
    escaped = text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    if len(escaped) <= width:
        return f'"{escaped}"'
    pieces = []
    while len(escaped) > width:
        cut = escaped.rfind(' ', 0, width)
        if cut <= 0 or escaped[cut - 1] == '\\':
            cut = width
        pieces.append(escaped[:cut + 1])
        escaped = escaped[cut + 1:]
    pieces.append(escaped)
    return ('\n' + indent).join(f'"{piece}"' for piece in pieces if piece)


def tag_table(values, defines):
    src = read('exif-tag.c')
    start = src.index('ExifTagTable[] = {')
    end = src.index('{0, NULL, NULL, NULL', start)
    body = src[start:end]
    alias = {}
    for name, value in defines:
        alias[name] = int(value, 16) if value.startswith('0x') else None
    starts = [m for m in re.finditer(r'\{\s*(EXIF_TAG_\w+)\s*,\s*"([^"]*)"', body)]
    rows = []
    ifd_count = 5
    for k, m in enumerate(starts):
        chunk = body[m.end():starts[k + 1].start() if k + 1 < len(starts) else len(body)]
        tag_name, name = m.group(1), m.group(2)
        tag = values.get(tag_name, alias.get(tag_name))
        if tag is None:
            raise ValueError(tag_name)
        title, pos = string_argument(chunk, 0)
        description, pos = string_argument(chunk, pos)
        tuple_match = re.search(r'\{\s*(ESL_\w+)\s*,\s*(ESL_\w+)\s*,\s*(ESL_\w+)\s*,\s*(ESL_\w+)\s*,\s*(ESL_\w+)\s*\}\s*\}', chunk)
        if tuple_match:
            codes = [c[4:] for c in tuple_match.groups()]
        elif re.search(r'\bESL_GPS\s*\}', chunk):
            codes = ['NNNN', 'NNNN', 'NNNN', 'OOOO', 'NNNN']
        elif re.search(r'\bESL_UNKNOWN\s*\}', chunk):
            codes = ['UUUU'] * ifd_count
        else:
            raise ValueError(f'support levels of {tag_name}')
        mask = 0
        for i, code in enumerate(codes):
            if code != 'NNNN':
                mask |= 1 << i
        rows.append((tag, name, title, description, mask))
    return rows


def port_header(enum_text):
    return BANNER.format(sources='exif-tag.h, exif-format.h, exif-utils.h, exif-entry.h and exif-data.h') + '''
// Purpose: The libexif types the listing's ported code works on, and the libexif functions it calls.
// The block's entries come from libfiles' reading of it; these structures only carry them to libexif's
// presentation code in the shape it expects.

#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>

#define _(String) (String)
#define N_(String) (String)
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define exif_entry_log(...) ((void)0)
#define exif_log(...) ((void)0)
#define EXIF_LOG_NO_MEMORY(...) ((void)0)

namespace libfiles::detail::libexif
{
	using std::abs;
	using std::isprint;
	using std::log10;
	using std::memchr;
	using std::memcmp;
	using std::memcpy;
	using std::memset;
	using std::snprintf;
	using std::strchr;
	using std::strlen;
	using std::strncat;
	using std::strncpy;

	typedef std::uint8_t ExifByte;
	typedef std::int8_t ExifSByte;
	typedef std::uint16_t ExifShort;
	typedef std::int16_t ExifSShort;
	typedef std::uint32_t ExifLong;
	typedef std::int32_t ExifSLong;

	typedef struct
	{
		ExifLong numerator;
		ExifLong denominator;
	} ExifRational;

	typedef struct
	{
		ExifSLong numerator;
		ExifSLong denominator;
	} ExifSRational;

	typedef enum
	{
		EXIF_BYTE_ORDER_MOTOROLA,
		EXIF_BYTE_ORDER_INTEL
	} ExifByteOrder;

	enum : unsigned int
	{
		EXIF_FORMAT_BYTE = 1,
		EXIF_FORMAT_ASCII = 2,
		EXIF_FORMAT_SHORT = 3,
		EXIF_FORMAT_LONG = 4,
		EXIF_FORMAT_RATIONAL = 5,
		EXIF_FORMAT_SBYTE = 6,
		EXIF_FORMAT_UNDEFINED = 7,
		EXIF_FORMAT_SSHORT = 8,
		EXIF_FORMAT_SLONG = 9,
		EXIF_FORMAT_SRATIONAL = 10,
		EXIF_FORMAT_FLOAT = 11,
		EXIF_FORMAT_DOUBLE = 12
	};
	typedef unsigned int ExifFormat;

	typedef enum
	{
		EXIF_IFD_0 = 0,
		EXIF_IFD_1,
		EXIF_IFD_EXIF,
		EXIF_IFD_GPS,
		EXIF_IFD_INTEROPERABILITY,
		EXIF_IFD_COUNT
	} ExifIfd;

	enum : unsigned int
	{
		EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS = 1 << 0,
		EXIF_DATA_OPTION_FOLLOW_SPECIFICATION = 1 << 1,
		EXIF_DATA_OPTION_DONT_CHANGE_MAKER_NOTE = 1 << 2
	};
	typedef unsigned int ExifDataOption;

''' + enum_text + '''

	// Allocation is calloc's, as libexif's default allocator's is. What it returns converts to any
	// pointer, as C's void pointer does, so the ported code reads as libexif's.
	struct ExifMem
	{
	};

	struct exif_allocation
	{
		void* pointer;

		template <typename T>
		operator T*() const { return static_cast<T*>(pointer); }

		bool operator!() const { return pointer == nullptr; }
	};

	inline exif_allocation exif_mem_alloc(ExifMem* const mem, const ExifLong size)
	{
		return {mem ? std::calloc(static_cast<std::size_t>(size), 1) : nullptr};
	}

	inline exif_allocation exif_mem_realloc(ExifMem* const mem, void* const pointer, const ExifLong size)
	{
		return {mem ? std::realloc(pointer, static_cast<std::size_t>(size)) : nullptr};
	}

	inline void exif_mem_free(ExifMem*, void* const pointer)
	{
		std::free(pointer);
	}

	inline void exif_mem_ref(ExifMem*)
	{
	}

	inline void exif_mem_unref(ExifMem*)
	{
	}

	// libexif logs through this; the port does not log.
	struct ExifLog
	{
	};

	inline void exif_log_ref(ExifLog*)
	{
	}

	inline void exif_log_unref(ExifLog*)
	{
	}

	struct ExifEntryPrivate
	{
		ExifMem* mem;
	};

	struct ExifData;
	struct ExifContent;

	struct ExifEntry
	{
		ExifTag tag;
		ExifFormat format;
		unsigned long components;
		unsigned char* data;
		unsigned int size;
		ExifContent* parent;
		ExifEntryPrivate* priv;
	};

	struct ExifContent
	{
		ExifEntry** entries;
		unsigned int count;
		ExifData* parent;
	};

	struct ExifData
	{
		ExifContent* ifd[EXIF_IFD_COUNT];
		unsigned char* data;
		unsigned int size;
		ExifByteOrder order;
	};

	inline ExifByteOrder exif_data_get_byte_order(const ExifData* const data)
	{
		return data->order;
	}

	ExifEntry* exif_content_get_entry(ExifContent* content, ExifTag tag);
	ExifIfd exif_content_get_ifd(ExifContent* content);

	inline ExifIfd exif_entry_get_ifd(ExifEntry* const e)
	{
		return e ? exif_content_get_ifd(e->parent) : EXIF_IFD_COUNT;
	}

	inline ExifEntry* exif_data_get_entry(const ExifData* const d, const ExifTag t)
	{
		for (const auto ifd : {EXIF_IFD_0, EXIF_IFD_1, EXIF_IFD_EXIF, EXIF_IFD_GPS, EXIF_IFD_INTEROPERABILITY})
		{
			if (auto* const e = exif_content_get_entry(d->ifd[ifd], t)) return e;
		}

		return nullptr;
	}

	ExifSShort exif_get_sshort(const unsigned char* buf, ExifByteOrder order);
	ExifShort exif_get_short(const unsigned char* buf, ExifByteOrder order);
	ExifSLong exif_get_slong(const unsigned char* b, ExifByteOrder order);
	ExifLong exif_get_long(const unsigned char* buf, ExifByteOrder order);
	ExifRational exif_get_rational(const unsigned char* buf, ExifByteOrder order);
	ExifSRational exif_get_srational(const unsigned char* buf, ExifByteOrder order);
	void exif_convert_utf16_to_utf8(char* out, unsigned int outlen, const unsigned char* in, unsigned int inlen);
	void exif_set_sshort(unsigned char* b, ExifByteOrder order, ExifSShort value);
	void exif_set_short(unsigned char* b, ExifByteOrder order, ExifShort value);
	void exif_set_slong(unsigned char* b, ExifByteOrder order, ExifSLong value);
	void exif_set_long(unsigned char* b, ExifByteOrder order, ExifLong value);
	void exif_set_srational(unsigned char* buf, ExifByteOrder order, ExifSRational value);
	void exif_set_rational(unsigned char* buf, ExifByteOrder order, ExifRational value);
	void exif_array_set_byte_order(ExifFormat f, unsigned char* b, unsigned int n, ExifByteOrder o_orig, ExifByteOrder o_new);

	const char* exif_format_get_name(ExifFormat format);
	unsigned char exif_format_get_size(ExifFormat format);

	const char* exif_tag_get_name(ExifTag tag);
	const char* exif_tag_get_name_in_ifd(ExifTag tag, ExifIfd ifd);
	const char* exif_tag_get_title_in_ifd(ExifTag tag, ExifIfd ifd);
	const char* exif_tag_get_description_in_ifd(ExifTag tag, ExifIfd ifd);

	const char* exif_entry_get_value(ExifEntry* e, char* val, unsigned int maxlen);
}
'''


def tags_cpp(rows):
    indent = '\t\t\t '
    table = '\n'.join(
        f'\t\t\t{{0x{tag:04x}, "{name}", {cpp_literal(title, indent)},\n{indent}{cpp_literal(description, indent)},\n{indent}0x{mask:02x}}},'
        for tag, name, title, description, mask in rows)
    return BANNER.format(sources='exif-tag.c') + '''
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
''' + table + '''
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
'''


def values_cpp():
    entry = read('exif-entry.c')
    lines = entry.split('\n')
    # From exif_entry_format_value's comment to the end of exif_entry_get_value, without exif_entry_dump.
    first = next(i for i, l in enumerate(lines) if l.startswith('exif_entry_format_value(ExifEntry *e'))
    while not lines[first].startswith('/*!'):
        first -= 1
    get_value = next(i for i, l in enumerate(lines) if l.startswith('exif_entry_get_value (ExifEntry *e'))
    last = next(j for j in range(get_value, len(lines)) if lines[j] == '}')
    text = '\n'.join(lines[first:last + 1]) + '\n'
    text = text.replace(function(entry, 'exif_entry_dump', '(ExifEntry *e, unsigned int indent)'), '')
    text, removed = re.subn(r'^[ \t]*\(void\)\s*bindtextdomain\s*\([^)]*\);[ \t]*\n', '', text, flags=re.M)
    assert removed == 1, removed
    for old, new in (('  { 0, {NULL}}\n};', '  { (ExifTag) 0, {NULL}}\n};'),
                     ('  {0, { { 0, {NULL}}} }\n};', '  {(ExifTag) 0, { { 0, {NULL}}} }\n};'),
                     ('const unsigned char *tagdata = memchr(e->data, 0, e->size);',
                      'const unsigned char *tagdata = (const unsigned char *) memchr(e->data, 0, e->size);')):
        assert text.count(old) == 1, old
        text = text.replace(old, new)

    utils = read('exif-utils.c')
    helpers = ''.join(function(utils, n, '(') for n in (
        'exif_get_sshort', 'exif_get_short', 'exif_get_slong', 'exif_get_long', 'exif_get_srational',
        'exif_get_rational', 'exif_convert_utf16_to_utf8', 'exif_set_sshort', 'exif_set_short', 'exif_set_slong',
        'exif_set_long', 'exif_set_srational', 'exif_set_rational', 'exif_array_set_byte_order'))

    fmt = read('exif-format.c')
    table_start = fmt.index('static const struct {')
    format_code = fmt[table_start:]
    assert format_code.count('{0, NULL, 0}') == 1, format_code
    format_code = format_code.replace('{0, NULL, 0}', '{(ExifFormat) 0, NULL, 0}')
    format_code, removed = re.subn(r'^[ \t]*\(void\)\s*bindtextdomain\s*\([^)]*\);[ \t]*\n', '', format_code, flags=re.M)
    assert removed == 1, removed

    return BANNER.format(sources='exif-entry.c, exif-utils.c and exif-format.c') + '''
// Purpose: How libexif renders an entry's value as text: its per-tag forms - exposure as a fraction of a
// second, the enumerations' names, versions, the Windows XP tags' UTF-16 - and its generic form for the
// rest, by type.

#include "libexif_port.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4996) // strncpy and strncat, as libexif calls them
#pragma warning(disable : 4244 4267) // and its implicit narrowing
#endif

namespace libfiles::detail::libexif
{
''' + helpers + '\n' + format_code + '\n' + text + '''}
'''


MNOTE_HEADER = BANNER.format(sources='exif-mnote-data.h and exif-mnote-data-priv.h') + """
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
"""


def mnote_cpp():
    base = read('exif-mnote-data.c')
    start = base.index('struct _ExifMnoteDataPriv')
    code = base[start:]
    return BANNER.format(sources='exif-mnote-data.c and exif-data.c') + """
// Purpose: The maker note interface's dispatch, and libexif's choice of decoder: Olympus' family
// first, then Canon's, Fuji's and last Pentax's, whose test is the most general.

#include "libexif_makernote.h"

namespace libfiles::detail::libexif
{
""" + code + """
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
"""


def strip_header(text):
    lines = text.split('\n')
    for i, line in enumerate(lines):
        m = re.match(r'#ifndef (\w+)$', line)
        if m and i + 1 < len(lines) and lines[i + 1].strip() == f'#define {m.group(1)}':
            del lines[i:i + 2]
            break
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].startswith('#endif'):
            del lines[i]
            break
    return '\n'.join(lines)


def vendor_cpp(vendor):
    folder = LIBEXIF / vendor
    names = [f'mnote-{vendor}-tag.h', f'mnote-{vendor}-entry.h', f'exif-mnote-data-{vendor}.h',
             f'mnote-{vendor}-tag.c', f'mnote-{vendor}-entry.c', f'exif-mnote-data-{vendor}.c']
    parts = []
    for name in names:
        text = (folder / name).read_text(encoding='utf-8', errors='strict').replace('\r\n', '\n')
        if name.endswith('.h'):
            text = strip_header(text)
        text = re.sub(r'^#include .*\n', '', text, flags=re.M)
        text = re.sub(r'#ifdef __cplusplus[ \t]*\nextern "C" \{[^\n]*\n#endif[^\n]*\n', '', text)
        text = re.sub(r'#ifdef __cplusplus[ \t]*\n\}[^\n]*\n#endif[^\n]*\n', '', text)
        text = re.sub(r'typedef enum (_Mnote\w+) (Mnote\w+);', r'typedef unsigned int \2;', text)
        text = re.sub(r'^[ \t]*\(void\)\s*bindtextdomain\s*\([^)]*\);[ \t]*\n', '', text, flags=re.M)
        parts.append(f'// libexif/{vendor}/{name}\n\n' + text.strip() + '\n')
    body = '\n'.join(parts)
    for named in re.findall(r'^enum (\w+) \{', body, flags=re.M):
        body = re.sub(rf'^enum {named} \{{', f'typedef unsigned int {named};\nenum : unsigned int {{', body, flags=re.M)
        body = re.sub(rf'\benum {named}\b', named, body)
    return BANNER.format(sources=f'libexif/{vendor}') + f"""
// Purpose: libexif's {vendor} maker note decoder: how it recognises the note, reads its entries and
// names and renders each value.

#include "libexif_makernote.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4996) // strcpy, strncpy and strncat, as libexif calls them
#pragma warning(disable : 4244 4267 4100 4189 4456 4457 4459 4702) // and its implicit narrowing and leftovers
#endif

namespace libfiles::detail::libexif
{{
""" + body + """
}
"""


def main():
    if LIBEXIF is None or not (LIBEXIF / 'exif-entry.c').exists():
        sys.exit('usage: port_libexif.py <libexif source tree>/libexif')
    enum_text, values, defines = tag_enum()
    rows = tag_table(values, defines)
    (OUT / 'libexif_port.h').write_text(port_header(enum_text).replace('\n', '\r\n'), encoding='utf-8', newline='')
    (OUT / 'libexif_tags.cpp').write_text(tags_cpp(rows).replace('\n', '\r\n'), encoding='utf-8', newline='')
    (OUT / 'libexif_values.cpp').write_text(values_cpp().replace('\n', '\r\n'), encoding='utf-8', newline='')
    (OUT / 'libexif_makernote.h').write_text(MNOTE_HEADER.replace('\n', '\r\n'), encoding='utf-8', newline='')
    (OUT / 'libexif_makernote.cpp').write_text(mnote_cpp().replace('\n', '\r\n'), encoding='utf-8', newline='')
    for vendor in ('canon', 'olympus', 'pentax', 'fuji'):
        (OUT / f'libexif_{vendor}.cpp').write_text(vendor_cpp(vendor).replace('\n', '\r\n'), encoding='utf-8', newline='')
    print(len(rows), 'tag rows,', len(values), 'tags,', len(defines), 'defines')


if __name__ == '__main__':
    main()
