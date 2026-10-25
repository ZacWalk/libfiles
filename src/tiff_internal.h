// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Reading TIFF directories, which an EXIF block and a TIFF file are built from alike. Every
// TIFF reading in libfiles goes through these - read_exif, summarize_exif and with it the JPEG
// decoder's orientation, and the TIFF scanner - so one rule decides which entries count, which
// orientation stands, where IFD1's thumbnail is and where the picture was taken.

#pragma once

#include "libfiles/reader.h"
#include "libfiles/scan.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace libfiles::detail
{
	constexpr std::uint16_t tag_orientation = 0x0112;
	constexpr std::uint16_t tag_jpeg_interchange_format = 0x0201;
	constexpr std::uint16_t tag_jpeg_interchange_format_length = 0x0202;
	constexpr std::uint16_t tag_gps_ifd = 0x8825;

	constexpr std::uint32_t type_byte = 1;
	constexpr std::uint32_t type_ascii = 2;
	constexpr std::uint32_t type_short = 3;
	constexpr std::uint32_t type_long = 4;
	constexpr std::uint32_t type_rational = 5;
	constexpr std::uint32_t type_undefined = 7;

	// The bytes in one value of each type up to FLOAT. DOUBLE, 12, is not counted: an entry of that
	// type is not well formed here.
	constexpr std::uint32_t type_limit = 12;
	constexpr std::uint32_t bytes_per_type[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};

	// A TIFF stream's bytes in its own byte order: an EXIF block in memory, or a TIFF file however the
	// reader reads it. Every offset comes from the file, so the caller bounds each one.
	class tiff_bytes
	{
		reader& _reader;
		const bool _little_endian;

	public:
		tiff_bytes(reader& r, const bool little_endian) : _reader(r), _little_endian(little_endian)
		{
		}

		bool overflows(const std::uint64_t i, const std::uint64_t size) const
		{
			return !_reader.contains(i, size);
		}

		// Borrows [i, i + size), valid until the next read; null when that is not inside the stream.
		const std::uint8_t* at(const std::uint64_t i, const std::uint32_t size) const
		{
			return overflows(i, size) ? nullptr : _reader.view(i, size).data();
		}

		std::uint16_t u16(const std::uint64_t i) const
		{
			return _little_endian ? _reader.le16(i) : _reader.be16(i);
		}

		std::uint32_t u32(const std::uint64_t i) const
		{
			return _little_endian ? _reader.le32(i) : _reader.be32(i);
		}

		std::int16_t s16(const std::uint64_t i) const
		{
			return static_cast<std::int16_t>(u16(i));
		}

		// A text value. UserComment says how it is encoded in its first eight bytes; the rest is
		// recognised as UTF-16 or UTF-8, and anything else is shown as "?".
		std::string text(std::uint32_t i, std::uint32_t size) const;
	};

	enum class directory_kind
	{
		exif,
		gps,
		canon,
		thumbnail
	};

	// One directory entry: a tag, its type, how many values, and where they are.
	struct entry
	{
		directory_kind kind;
		const tiff_bytes& data;
		std::uint32_t offset;
		std::uint16_t tag;
		std::uint32_t type;
		std::uint32_t count;

		entry(const directory_kind k, const tiff_bytes& d, const std::uint32_t o) :
			kind(k), data(d), offset(o), tag(d.u16(o)), type(d.u16(o + 2)), count(d.u32(o + 4))
		{
		}

		// Sized in 32 bits, as the parser this keeps to sized it, so a vast count wraps.
		std::uint32_t size() const
		{
			if (type >= type_limit) return 0;
			return count * bytes_per_type[type];
		}

		std::uint32_t value_offset() const
		{
			return size() <= 4 ? offset + 8u : data.u32(offset + 8);
		}

		bool well_formed() const
		{
			return type < type_limit && count > 0 && !data.overflows(value_offset(), size());
		}

		// The value field, read as one number whatever the type.
		std::uint16_t field_u16() const { return data.u16(offset + 8); }
		std::int16_t field_s16() const { return data.s16(offset + 8); }
		std::uint32_t field_u32() const { return data.u32(offset + 8); }

		// The first value of a SHORT or a LONG where it lies - in the field, or where the field points
		// when the values do not fit in it - cut to sixteen bits. Any other type's field is read as one
		// number, as field_u16 reads it.
		std::uint16_t first_u16() const
		{
			const auto o = value_offset();
			if (type == type_short && !data.overflows(o, 2u)) return data.u16(o);
			if (type == type_long && !data.overflows(o, 4u)) return static_cast<std::uint16_t>(data.u32(o));
			return field_u16();
		}

		int s16_at(const std::uint32_t i) const
		{
			const auto o = value_offset() + i * 2;
			if (data.overflows(o, 2u)) return 0;
			return data.s16(o);
		}

		double rational_at(const std::uint32_t i = 0) const
		{
			const auto o = value_offset() + i * 8;
			if (data.overflows(o, 8u)) return 0.0;
			const auto numerator = data.u32(o);
			const auto denominator = data.u32(o + 4);
			return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
		}

		double signed_rational_at(const std::uint32_t i = 0) const
		{
			const auto o = value_offset() + i * 8;
			if (data.overflows(o, 8u)) return 0.0;
			const auto numerator = static_cast<std::int32_t>(data.u32(o));
			const auto denominator = static_cast<std::int32_t>(data.u32(o + 4));
			return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
		}

		// The value's bytes, as stored, valid until the next read.
		std::string_view raw_text() const
		{
			const auto len = size();
			const auto* const p = data.at(value_offset(), len);
			return p ? std::string_view{reinterpret_cast<const char*>(p), len} : std::string_view{};
		}

		std::uint8_t first_byte() const
		{
			const auto t = raw_text();
			return t.empty() ? 0u : static_cast<std::uint8_t>(t.front());
		}

		// The value as text, decoded. A number stored where text belongs reads as "?".
		std::string text() const;

		// An ASCII value spans its whole count, so it ends in a NUL and may be padded with spaces.
		std::string ascii() const
		{
			auto t = raw_text();
			while (!t.empty() && (t.back() == '\0' || t.back() == ' ')) t.remove_suffix(1);
			return std::string(t);
		}
	};

	// Where IFD0 is, or nothing when the stream is not classic TIFF.
	inline std::optional<std::uint32_t> first_directory(const tiff_bytes& data)
	{
		if (data.overflows(0, 8) || data.u16(2) != 42u) return {};
		return data.u32(4);
	}

	// The directory linked after the one at offset - IFD1 after IFD0 - or 0 for none.
	inline std::uint32_t next_directory(const tiff_bytes& data, const std::uint32_t offset)
	{
		if (data.overflows(offset, 12)) return 0;
		const auto link = offset + 2 + 12 * data.u16(offset);
		if (data.overflows(link, 12)) return 0;
		return data.u32(link);
	}

	// Calls visit with each well-formed entry of the directory at offset, in order. Its count is an
	// unvalidated field, so the entries stop where the stream does.
	template <typename Visit>
	void for_each_entry(const tiff_bytes& data, const std::uint32_t offset, const directory_kind kind, Visit&& visit)
	{
		if (data.overflows(offset, 2)) return;

		const auto count = data.u16(offset);

		for (auto i = 0u; i < count; ++i)
		{
			const auto pos = offset + 2ull + 12ull * i;
			if (pos > std::numeric_limits<std::uint32_t>::max() || data.overflows(pos, 12)) break;

			const entry e(kind, data, static_cast<std::uint32_t>(pos));
			if (e.well_formed()) visit(e);
		}
	}

	// The orientation and IFD1's thumbnail, by the one rule: the last well-formed Orientation in IFD0,
	// else the last in IFD1, and the thumbnail where one SHORT or LONG offset and length in IFD1 put it.
	// Orientation tags anywhere else are not the image's.
	exif_summary summarize(const tiff_bytes& data);

	exif_metadata read_metadata(const tiff_bytes& data, std::uint32_t value_limit);
}
