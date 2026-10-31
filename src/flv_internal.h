// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: How FLV's tags and script data are read, by the media scan and the writer alike: the header,
// each tag's header, and the AMF0 values a script tag holds, walked without trusting any length to lie
// inside the tag.

#pragma once

#include "libfiles/reader.h"
#include "scan_internal.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace libfiles::detail::flv
{
	inline constexpr std::uint8_t audio_tag = 8;
	inline constexpr std::uint8_t video_tag = 9;
	inline constexpr std::uint8_t script_tag = 18;
	// A tag's header: its type, data size, timestamp and stream id.
	inline constexpr std::uint64_t tag_header = 11;

	// The header's flags.
	inline constexpr std::uint8_t has_audio = 4;
	inline constexpr std::uint8_t has_video = 1;

	struct tag
	{
		std::uint64_t start = 0;
		// The low five bits of the first byte; the bit above them marks a tag whose data is encrypted.
		std::uint8_t type = 0;
		bool filtered = false;
		std::uint32_t data_size = 0;
		// Milliseconds, the extension byte above the 24 bits.
		std::uint32_t timestamp = 0;

		std::uint64_t data() const { return start + tag_header; }
		// The tag and the size field that follows it.
		std::uint64_t end() const { return start + tag_header + data_size + 4; }
	};

	// Where the first tag starts: after the header, and the size field that follows it. Throws
	// format_error for a file too short to hold them or a header that points past its end.
	inline std::uint64_t first_tag(reader& r)
	{
		const auto size = r.size();
		if (size < 13) throw format_error("the FLV is cut short");

		const auto data_offset = r.be32(5);
		if (data_offset < 9 || data_offset > size - 4) throw format_error("the FLV header is damaged");
		return data_offset + 4ull;
	}

	// The tag at pos, when its header, its data and the size field after them lie in the file.
	inline std::optional<tag> read_tag(reader& r, const std::uint64_t pos)
	{
		const auto size = r.size();
		if (pos > size || size - pos < tag_header + 4) return std::nullopt;

		const auto h = r.view(pos, tag_header);
		tag t;
		t.start = pos;
		t.type = h[0] & 0x1F;
		t.filtered = (h[0] & 0x20) != 0;
		t.data_size = static_cast<std::uint32_t>(h[1]) << 16 | static_cast<std::uint32_t>(h[2]) << 8 | h[3];
		t.timestamp = static_cast<std::uint32_t>(h[7]) << 24 | static_cast<std::uint32_t>(h[4]) << 16 |
			static_cast<std::uint32_t>(h[5]) << 8 | h[6];

		if (t.end() > size) return std::nullopt;
		return t;
	}

	// AMF0's value types.
	inline constexpr std::uint8_t amf_number = 0;
	inline constexpr std::uint8_t amf_boolean = 1;
	inline constexpr std::uint8_t amf_string = 2;
	inline constexpr std::uint8_t amf_object = 3;
	inline constexpr std::uint8_t amf_null = 5;
	inline constexpr std::uint8_t amf_undefined = 6;
	inline constexpr std::uint8_t amf_reference = 7;
	inline constexpr std::uint8_t amf_ecma_array = 8;
	inline constexpr std::uint8_t amf_object_end = 9;
	inline constexpr std::uint8_t amf_strict_array = 10;
	inline constexpr std::uint8_t amf_date = 11;
	inline constexpr std::uint8_t amf_long_string = 12;
	inline constexpr std::uint8_t amf_xml_document = 15;
	inline constexpr std::uint8_t amf_typed_object = 16;

	// A number, boolean, string, date or XML document the walk passes.
	struct amf_value
	{
		// The name it is given; an element of a strict array has its array's.
		std::string_view key;
		std::uint8_t type = 0;
		// Its payload: a number's eight bytes, a boolean's byte, a string's bytes after its length, a
		// date's eight bytes of milliseconds and two of time zone.
		std::uint64_t at = 0;
		std::uint64_t length = 0;
		// 0 for the value walked, 1 for its members, and so on.
		int depth = 0;
		bool in_array = false;
	};

	// AMF0 values, walked to find what each holds under its name.
	class amf
	{
	public:
		amf(reader& r, const std::uint64_t end) : _r(r), _end(end)
		{
		}

		std::function<void(const amf_value&)> on_value;

		// Where the value at pos ends, or nothing when it cannot be followed.
		std::optional<std::uint64_t> value(const std::uint64_t pos, const std::string_view key = {})
		{
			return walk(pos, key, 0, false);
		}

	private:
		std::optional<std::uint64_t> walk(std::uint64_t pos, const std::string_view key, const int depth, const bool element)
		{
			if (depth > 16 || pos >= _end) return {};

			const auto type = _r.view(pos++, 1)[0];

			const auto found = [&](const std::uint64_t at, const std::uint64_t length)
			{
				if (on_value) on_value({key, type, at, length, depth, element});
			};

			switch (type)
			{
			case amf_number:
				if (_end - pos < 8) return {};
				found(pos, 8);
				return pos + 8;
			case amf_boolean:
				if (_end - pos < 1) return {};
				found(pos, 1);
				return pos + 1;
			case amf_string:
			case amf_long_string:
			case amf_xml_document:
				{
					const auto wide = type != amf_string;
					if (_end - pos < (wide ? 4u : 2u)) return {};
					const std::uint64_t length = wide ? _r.be32(pos) : _r.be16(pos);
					pos += wide ? 4 : 2;
					if (_end - pos < length) return {};
					found(pos, length);
					return pos + length;
				}
			case amf_object:
				return properties(pos, depth);
			case amf_null:
			case amf_undefined:
				return pos;
			case amf_reference:
				return skip(pos, 2);
			case amf_ecma_array: // a count, then named values
				if (_end - pos < 4) return {};
				return properties(pos + 4, depth);
			case amf_strict_array: // a count, then values under the array's name
				{
					if (_end - pos < 4) return {};
					const auto count = _r.be32(pos);
					pos += 4;

					for (std::uint32_t i = 0; i < count; ++i)
					{
						const auto next = walk(pos, key, depth + 1, true);
						if (!next) return {};
						pos = *next;
					}

					return pos;
				}
			case amf_date:
				if (_end - pos < 10) return {};
				found(pos, 10);
				return pos + 10;
			case amf_typed_object: // a class name, then named values
				{
					if (_end - pos < 2) return {};
					const auto length = _r.be16(pos);
					if (_end - pos - 2 < length) return {};
					return properties(pos + 2 + length, depth);
				}
			default:
				return {};
			}
		}

		std::optional<std::uint64_t> skip(const std::uint64_t pos, const std::uint64_t n) const
		{
			if (_end - pos < n) return {};
			return pos + n;
		}

		// Named values until an empty name and the end marker. Some writers end the outermost array at
		// the end of the data without one.
		std::optional<std::uint64_t> properties(std::uint64_t pos, const int depth)
		{
			while (_end - pos >= 2)
			{
				const auto length = _r.be16(pos);
				pos += 2;

				if (length == 0)
				{
					if (pos < _end && _r.view(pos, 1)[0] == amf_object_end) return pos + 1;
					return {};
				}

				if (_end - pos < length) return {};
				const auto name = _r.read(pos, length);
				pos += length;

				const auto next = walk(pos, std::string_view(reinterpret_cast<const char*>(name.data()), name.size()), depth + 1, false);
				if (!next) return {};
				pos = *next;
			}

			if (pos == _end) return pos;
			return {};
		}

		reader& _r;
		std::uint64_t _end;
	};

	// The name a script tag's data starts with - "onMetaData", "onXMPData" - and where the value after
	// it starts. Empty when the data does not start with a value, or with one too long to be a name.
	inline std::string script_name(reader& r, const tag& t, std::uint64_t& after)
	{
		constexpr std::uint64_t max_name = 64 * 1024;
		amf a(r, t.data() + t.data_size);
		std::string name;
		auto too_long = false;

		a.on_value = [&](const amf_value& v)
		{
			if (v.type != amf_string && v.type != amf_long_string) return;

			too_long = v.length > max_name;
			if (too_long) return;

			const auto text = r.view(v.at, static_cast<std::size_t>(v.length));
			name.assign(text.begin(), text.end());
		};

		const auto end = a.value(t.data());
		if (!end) return {};

		after = *end;
		if (too_long) return {};
		while (!name.empty() && name.back() == '\0') name.pop_back();
		return name;
	}
}
