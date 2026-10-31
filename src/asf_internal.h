// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: How ASF's objects are read, by the media scan and the writer alike: the GUIDs that name them,
// the walk over a run of objects and the layout of the file's top level, the File Properties' fields, the
// Content Description's five fields, and the descriptors of the Extended Content Description, Metadata and
// Metadata Library objects. Each walk stops at the first object or descriptor that does not fit where it
// lies and says so: the writer then refuses the header, and the scan keeps what came before.

#pragma once

#include "libfiles/reader.h"
#include "scan_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libfiles::detail::asf
{
	using guid = std::array<std::uint8_t, 16>;

	// The top-level objects. The XMP object is Adobe's, which its toolkit writes after the data.
	inline constexpr guid header_id = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	inline constexpr guid data_id = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	inline constexpr guid simple_index_id = {0x90, 0x08, 0x00, 0x33, 0xB1, 0xE5, 0xCF, 0x11, 0x89, 0xF4, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xCB};
	inline constexpr guid index_id = {0xD3, 0x29, 0xE2, 0xD6, 0xDA, 0x35, 0xD1, 0x11, 0x90, 0x34, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xBE};
	inline constexpr guid media_object_index_id = {0xF8, 0x03, 0xB1, 0xFE, 0xAD, 0x12, 0x64, 0x4C, 0x84, 0x0F, 0x2A, 0x1D, 0x2F, 0x7A, 0xD4, 0x8C};
	inline constexpr guid timecode_index_id = {0xD0, 0x3F, 0xB7, 0x3C, 0x4A, 0x0C, 0x03, 0x48, 0x95, 0x3D, 0xED, 0xF7, 0xB6, 0x22, 0x8F, 0x0C};
	inline constexpr guid xmp_id = {0xCB, 0xCF, 0x7A, 0xBE, 0xA9, 0x97, 0xE8, 0x42, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};

	// The header's objects.
	inline constexpr guid file_properties_id = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
	inline constexpr guid stream_properties_id = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
	inline constexpr guid header_extension_id = {0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11, 0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
	inline constexpr guid codec_list_id = {0x40, 0x52, 0xD1, 0x86, 0x1D, 0x31, 0xD0, 0x11, 0xA3, 0xA4, 0x00, 0xA0, 0xC9, 0x03, 0x48, 0xF6};
	inline constexpr guid script_command_id = {0x30, 0x1A, 0xFB, 0x1E, 0x62, 0x0B, 0xD0, 0x11, 0xA3, 0x9B, 0x00, 0xA0, 0xC9, 0x03, 0x48, 0xF6};
	inline constexpr guid marker_id = {0x01, 0xCD, 0x87, 0xF4, 0x51, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
	inline constexpr guid bitrate_mutual_exclusion_id = {0xDC, 0x29, 0xE2, 0xD6, 0xDA, 0x35, 0xD1, 0x11, 0x90, 0x34, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xBE};
	inline constexpr guid error_correction_id = {0x35, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	inline constexpr guid content_description_id = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	inline constexpr guid extended_content_id = {0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
	inline constexpr guid content_branding_id = {0xFA, 0xB3, 0x11, 0x22, 0x23, 0xBD, 0xD2, 0x11, 0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E};
	inline constexpr guid stream_bitrate_id = {0xCE, 0x75, 0xF8, 0x7B, 0x8D, 0x46, 0xD1, 0x11, 0x8D, 0x82, 0x00, 0x60, 0x97, 0xC9, 0xA2, 0xB2};
	inline constexpr guid content_encryption_id = {0xFB, 0xB3, 0x11, 0x22, 0x23, 0xBD, 0xD2, 0x11, 0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E};
	inline constexpr guid extended_content_encryption_id = {0x14, 0xE6, 0x8A, 0x29, 0x22, 0x26, 0x17, 0x4C, 0xB9, 0x35, 0xDA, 0xE0, 0x7E, 0xE9, 0x28, 0x9C};
	inline constexpr guid digital_signature_id = {0xFC, 0xB3, 0x11, 0x22, 0x23, 0xBD, 0xD2, 0x11, 0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E};
	inline constexpr guid padding_id = {0x74, 0xD4, 0x06, 0x18, 0xDF, 0xCA, 0x09, 0x45, 0xA4, 0xBA, 0x9A, 0xAB, 0xCB, 0x96, 0xAA, 0xE8};

	// The header extension's objects.
	inline constexpr guid extended_stream_properties_id = {0xCB, 0xA5, 0xE6, 0x14, 0x72, 0xC6, 0x32, 0x43, 0x83, 0x99, 0xA9, 0x69, 0x52, 0x06, 0x5B, 0x5A};
	inline constexpr guid advanced_mutual_exclusion_id = {0xCF, 0x49, 0x86, 0xA0, 0x75, 0x47, 0x70, 0x46, 0x8A, 0x16, 0x6E, 0x35, 0x35, 0x75, 0x66, 0xCD};
	inline constexpr guid group_mutual_exclusion_id = {0x40, 0x5A, 0x46, 0xD1, 0x79, 0x5A, 0x38, 0x43, 0xB7, 0x1B, 0xE3, 0x6B, 0x8F, 0xD6, 0xC2, 0x49};
	inline constexpr guid stream_prioritization_id = {0x5B, 0xD1, 0xFE, 0xD4, 0xD3, 0x88, 0x4F, 0x45, 0x81, 0xF0, 0xED, 0x5C, 0x45, 0x99, 0x9E, 0x24};
	inline constexpr guid bandwidth_sharing_id = {0xE6, 0x09, 0x96, 0xA6, 0x7B, 0x51, 0xD2, 0x11, 0xB6, 0xAF, 0x00, 0xC0, 0x4F, 0xD9, 0x08, 0xE9};
	inline constexpr guid language_list_id = {0xA9, 0x46, 0x43, 0x7C, 0xE0, 0xEF, 0xFC, 0x4B, 0xB2, 0x29, 0x39, 0x3E, 0xDE, 0x41, 0x5C, 0x85};
	inline constexpr guid metadata_id = {0xEA, 0xCB, 0xF8, 0xC5, 0xAF, 0x5B, 0x77, 0x48, 0x84, 0x67, 0xAA, 0x8C, 0x44, 0xFA, 0x4C, 0xCA};
	inline constexpr guid metadata_library_id = {0x94, 0x1C, 0x23, 0x44, 0x98, 0x94, 0xD1, 0x49, 0xA1, 0x41, 0x1D, 0x13, 0x4E, 0x45, 0x70, 0x54};
	inline constexpr guid index_parameters_id = {0xDF, 0x29, 0xE2, 0xD6, 0xDA, 0x35, 0xD1, 0x11, 0x90, 0x34, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xBE};
	inline constexpr guid media_object_index_parameters_id = {0xAD, 0x3B, 0x20, 0x6B, 0x11, 0x3F, 0xE4, 0x48, 0xAC, 0xA8, 0xD7, 0x61, 0x3D, 0xE2, 0xCF, 0xA7};
	inline constexpr guid timecode_index_parameters_id = {0x6D, 0x49, 0x5E, 0xF5, 0x97, 0x97, 0x5D, 0x4B, 0x8C, 0x8B, 0x60, 0x4D, 0xFE, 0x9B, 0xFB, 0x24};
	inline constexpr guid compatibility_id = {0x5D, 0x8B, 0xF1, 0x26, 0x84, 0x45, 0xEC, 0x47, 0x9F, 0x5F, 0x0E, 0x65, 0x1F, 0x04, 0x52, 0xC9};
	inline constexpr guid advanced_content_encryption_id = {0x33, 0x85, 0x05, 0x43, 0x81, 0x69, 0xE6, 0x49, 0x9B, 0x74, 0xAD, 0x12, 0xCB, 0x86, 0xD5, 0x8C};

	// What a stream carries: the stream type a Stream Properties object names.
	inline constexpr guid audio_media_id = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
	inline constexpr guid video_media_id = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
	inline constexpr guid command_media_id = {0xC0, 0xCF, 0xDA, 0x59, 0xE6, 0x59, 0xD0, 0x11, 0xA3, 0xAC, 0x00, 0xA0, 0xC9, 0x03, 0x48, 0xF6};
	inline constexpr guid jfif_media_id = {0x00, 0xE1, 0x1B, 0xB6, 0x4E, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
	inline constexpr guid degradable_jpeg_media_id = {0xE0, 0x7D, 0x90, 0x35, 0x15, 0xE4, 0xCF, 0x11, 0xA9, 0x17, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
	inline constexpr guid file_transfer_media_id = {0x2C, 0x22, 0xBD, 0x91, 0x1C, 0xF2, 0x7A, 0x49, 0x8B, 0x6D, 0x5A, 0xA8, 0x6B, 0xFC, 0x01, 0x85};
	inline constexpr guid binary_media_id = {0xE2, 0x65, 0xFB, 0x3A, 0xEF, 0x47, 0xF2, 0x40, 0xAC, 0x2C, 0x70, 0xA9, 0x0D, 0x71, 0xD3, 0x43};

	// An object's GUID and 64-bit size.
	inline constexpr std::size_t object_header = 24;
	// The header object's own fields: its object header, a child count and two reserved bytes.
	inline constexpr std::size_t header_fixed = 30;
	// The header extension's: its object header, a reserved GUID and word, and its data size.
	inline constexpr std::size_t extension_fixed = 46;
	inline constexpr std::size_t extension_data_size_at = 42;

	// The File Properties object's fields, from its start, and its size.
	inline constexpr std::size_t file_size_at = 40;
	inline constexpr std::size_t creation_date_at = 48;
	inline constexpr std::size_t data_packets_at = 56;
	inline constexpr std::size_t play_duration_at = 64;
	inline constexpr std::size_t send_duration_at = 72;
	inline constexpr std::size_t preroll_at = 80;
	inline constexpr std::size_t flags_at = 88;
	inline constexpr std::size_t min_packet_size_at = 92;
	inline constexpr std::size_t max_packet_size_at = 96;
	inline constexpr std::size_t max_bitrate_at = 100;
	inline constexpr std::size_t file_properties_size = 104;

	// The fixed part of a Stream Properties object, up to its type-specific data.
	inline constexpr std::size_t stream_properties_fixed = 78;

	// The types a descriptor's value takes. A BOOL is 32 bits in an Extended Content Description and 16
	// in a Metadata or Metadata Library object; a GUID is only in the Metadata Library.
	inline constexpr std::uint16_t unicode_value = 0;
	inline constexpr std::uint16_t byte_array_value = 1;
	inline constexpr std::uint16_t bool_value = 2;
	inline constexpr std::uint16_t dword_value = 3;
	inline constexpr std::uint16_t qword_value = 4;
	inline constexpr std::uint16_t word_value = 5;
	inline constexpr std::uint16_t guid_value = 6;

	inline std::uint64_t load_le64(const std::uint8_t* const p)
	{
		std::uint64_t v = 0;
		for (auto i = 7; i >= 0; --i) v = v << 8 | p[i];
		return v;
	}

	inline bool is(const std::span<const std::uint8_t> at, const guid& id)
	{
		return at.size() >= id.size() && std::equal(id.begin(), id.end(), at.begin());
	}

	// Whether a File Properties object says the file is still being written, so that its date, duration
	// and size are not to be relied on.
	inline bool broadcast(const std::span<const std::uint8_t> file_properties)
	{
		return file_properties.size() >= file_properties_size && (file_properties[flags_at] & 1) != 0;
	}

	struct object
	{
		std::uint64_t start = 0;
		std::uint64_t size = 0;
		guid id{};

		std::uint64_t end() const { return start + size; }
	};

	// Calls visit with each object of those that fill [begin, end), in order. Returns where the walk
	// stopped: end when the objects fill the range, else where the first that cannot be one starts - in
	// fewer bytes than an object header, or with a size too small for one or running past end.
	template <typename Visit>
	std::uint64_t walk_objects(reader& r, const std::uint64_t begin, const std::uint64_t end, Visit&& visit)
	{
		auto pos = begin;

		while (pos <= end && end - pos >= object_header)
		{
			const auto h = r.view(pos, object_header);
			object o;
			o.start = pos;
			std::copy_n(h.begin(), o.id.size(), o.id.begin());
			o.size = load_le64(h.data() + 16);

			if (o.size < object_header || o.size > end - pos) break;

			visit(o);
			pos += o.size;
		}

		return pos;
	}

	// The largest header the writer takes on, and so whose file's packet is read.
	inline constexpr std::uint64_t max_header = 16ull * 1024 * 1024;

	// The top-level objects, in the order of the file.
	struct layout
	{
		std::vector<object> objects;
		std::size_t data_at = 0;
		// The XMP objects after the data, in the order of the file.
		std::vector<std::size_t> xmp_at;

		const object& header() const { return objects.front(); }
		// The packet readers take: the first after the data.
		const object* xmp() const { return xmp_at.empty() ? nullptr : &objects[xmp_at.front()]; }
	};

	// The file's top-level objects. They must fill it, but for fewer bytes than an object header at its
	// end, start with the header object and hold a data object, and the header must be one the writer
	// takes on; else this throws format_error. A caller that has read the header object at the start of
	// the file passes it, so that it is not read again.
	inline layout read_layout(reader& r, const std::optional<object>& header = std::nullopt)
	{
		layout l;
		const auto size = r.size();
		auto found_data = false;

		const auto add = [&](const object& o)
		{
			if (o.id == data_id && !found_data)
			{
				l.data_at = l.objects.size();
				found_data = true;
			}
			else if (o.id == xmp_id && found_data)
			{
				l.xmp_at.push_back(l.objects.size());
			}

			l.objects.push_back(o);
		};

		std::uint64_t begin = 0;

		if (header)
		{
			add(*header);
			begin = header->end();
		}

		const auto stop = walk_objects(r, begin, size, add);

		// Fewer bytes than an object header may follow the last object; anything larger is damage.
		if (size - stop >= object_header) throw format_error("an ASF object runs past the end of the file");

		if (l.objects.empty() || l.objects.front().id != header_id) throw format_error("the ASF has no header object");
		if (!found_data) throw format_error("the ASF has no data object");
		if (l.header().size > max_header) throw format_error("the ASF header is too large to update");
		return l;
	}

	// A descriptor of an Extended Content Description object, or a description record of a Metadata or
	// Metadata Library object, each part where it lies.
	struct descriptor
	{
		std::uint64_t start = 0;
		std::uint64_t name_at = 0;
		// In bytes, its terminator included.
		std::uint32_t name_length = 0;
		std::uint16_t type = 0;
		std::uint64_t value_at = 0;
		std::uint64_t value_length = 0;
		// A record's stream, 0 for the file; and a Metadata Library record's index into the Language List.
		std::uint16_t stream = 0;
		std::uint16_t language = 0;

		std::uint64_t end() const { return value_at + value_length; }
	};

	// Calls visit with each descriptor of the Extended Content Description object whose payload, after
	// its object header, is [begin, end). False when the count or a descriptor runs past end, after
	// visiting those before it. lavf_lengths reads an odd name length, and an odd length of a text value,
	// as one byte more, as FFmpeg does for the files early versions of it wrote one byte short.
	template <typename Visit>
	bool walk_content_descriptors(reader& r, const std::uint64_t begin, const std::uint64_t end, Visit&& visit,
	                              const bool lavf_lengths = false)
	{
		if (begin > end || end - begin < 2) return false;

		const auto count = r.le16(begin);
		auto pos = begin + 2;

		for (unsigned i = 0; i < count; ++i)
		{
			if (end - pos < 2) return false;

			descriptor d;
			d.start = pos;
			d.name_length = r.le16(pos);
			if (lavf_lengths && (d.name_length & 1) != 0) ++d.name_length;
			if (end - pos < 6u + d.name_length) return false;

			d.name_at = pos + 2;
			d.type = r.le16(d.name_at + d.name_length);
			d.value_length = r.le16(d.name_at + d.name_length + 2);
			if (lavf_lengths && d.type == unicode_value && (d.value_length & 1) != 0) ++d.value_length;
			d.value_at = d.name_at + d.name_length + 4;
			if (end - d.value_at < d.value_length) return false;

			visit(d);
			pos = d.end();
		}

		return true;
	}

	// Calls visit with each description record of the Metadata or Metadata Library object whose payload is
	// [begin, end). False when the count or a record runs past end, after visiting those before it.
	template <typename Visit>
	bool walk_metadata_records(reader& r, const std::uint64_t begin, const std::uint64_t end, Visit&& visit)
	{
		if (begin > end || end - begin < 2) return false;

		const auto count = r.le16(begin);
		auto pos = begin + 2;

		for (unsigned i = 0; i < count; ++i)
		{
			if (end - pos < 12) return false;

			const auto h = r.view(pos, 12);
			descriptor d;
			d.start = pos;
			d.language = load_le16(h.data());
			d.stream = load_le16(h.data() + 2);
			d.name_length = load_le16(h.data() + 4);
			d.type = load_le16(h.data() + 6);
			d.value_length = load_le32(h.data() + 8);
			d.name_at = pos + 12;
			if (end - d.name_at < d.name_length) return false;

			d.value_at = d.name_at + d.name_length;
			if (end - d.value_at < d.value_length) return false;

			visit(d);
			pos = d.end();
		}

		return true;
	}

	// The Content Description object's fields - title, author, copyright, description and rating, in that
	// order - where each lies, from its payload [begin, end). Returns how many lie whole inside it, the
	// fields before the first that runs past end.
	inline std::size_t content_description_fields(reader& r, const std::uint64_t begin, const std::uint64_t end,
	                                              std::array<byte_range, 5>& fields)
	{
		if (begin > end || end - begin < 10) return 0;

		auto pos = begin + 10;

		for (std::size_t i = 0; i < fields.size(); ++i)
		{
			const std::uint64_t length = r.le16(begin + 2 * i);
			if (length > end - pos) return i;

			fields[i] = {pos, length};
			pos += length;
		}

		return fields.size();
	}
}
