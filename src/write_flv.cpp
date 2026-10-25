// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing metadata to FLV files. The packet lives where the toolkit kept it, as the "liveXML"
// string of an onXMPData script tag among the tags at time zero, after onMetaData. A packet that fits is
// patched where it lies; otherwise the file is rewritten in one pass with a new tag, given padding for
// the next edit. A rewrite moves the media, so the file positions onMetaData keeps for seeking - its
// keyframe index and file size - are moved with it, which the toolkit did not do.

#include "legacy.h"

#include <bit>
#include <cmath>
#include <functional>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint8_t script_tag = 18;
		constexpr std::uint64_t tag_header = 11;
		// How many tags at time zero are looked through for onMetaData and onXMPData.
		constexpr int max_scan_tags = 64;

		struct flv_tag
		{
			std::uint64_t start = 0;
			std::uint32_t data_size = 0;

			std::uint64_t data() const { return start + tag_header; }
			// The tag and the size field that follows it.
			std::uint64_t end() const { return start + tag_header + data_size + 4; }
		};

		struct layout
		{
			std::uint64_t first_tag = 0;
			std::optional<flv_tag> meta;
			std::optional<flv_tag> xmp;
			// The liveXML string's bytes, its terminator included.
			std::uint64_t xml_at = 0;
			std::uint64_t xml_length = 0;
		};

		// AMF0 values, walked to find the strings and numbers given under a key.
		class amf
		{
		public:
			amf(reader& r, const std::uint64_t end) : _r(r), _end(end)
			{
			}

			std::function<void(std::string_view key, std::uint64_t at)> on_number;
			std::function<void(std::string_view key, std::uint64_t at, std::uint64_t length)> on_string;

			// Where the value at pos ends, or nothing when it cannot be followed.
			std::optional<std::uint64_t> value(std::uint64_t pos, const std::string_view key, const int depth = 0)
			{
				if (depth > 16 || pos >= _end) return {};

				const auto type = _r.view(pos++, 1)[0];

				switch (type)
				{
				case 0: // number
					if (_end - pos < 8) return {};
					if (on_number) on_number(key, pos);
					return pos + 8;
				case 1: // boolean
					return skip(pos, 1);
				case 2: // string
				case 12: // long string
				case 15: // XML document
					{
						const auto wide = type != 2;
						if (_end - pos < (wide ? 4u : 2u)) return {};
						const std::uint64_t length = wide ? _r.be32(pos) : _r.be16(pos);
						pos += wide ? 4 : 2;
						if (_end - pos < length) return {};
						if (type != 15 && on_string) on_string(key, pos, length);
						return pos + length;
					}
				case 3: // object
					return properties(pos, depth);
				case 5: // null
				case 6: // undefined
					return pos;
				case 7: // reference
					return skip(pos, 2);
				case 8: // ECMA array: a count, then named values
					if (_end - pos < 4) return {};
					return properties(pos + 4, depth);
				case 10: // strict array: a count, then values under the array's key
					{
						if (_end - pos < 4) return {};
						const auto count = _r.be32(pos);
						pos += 4;

						for (std::uint32_t i = 0; i < count; ++i)
						{
							const auto next = value(pos, key, depth + 1);
							if (!next) return {};
							pos = *next;
						}

						return pos;
					}
				case 11: // date
					return skip(pos, 10);
				case 16: // typed object: a class name, then named values
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

		private:
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
						if (pos < _end && _r.view(pos, 1)[0] == 9) return pos + 1;
						return {};
					}

					if (_end - pos < length) return {};
					const auto name = _r.read(pos, length);
					pos += length;

					const auto next = value(pos, std::string_view(reinterpret_cast<const char*>(name.data()), name.size()), depth + 1);
					if (!next) return {};
					pos = *next;
				}

				if (pos == _end) return pos;
				return {};
			}

			reader& _r;
			std::uint64_t _end;
		};

		// The name a script tag's data starts with.
		std::string script_name(reader& r, const flv_tag& t, std::uint64_t& after)
		{
			amf a(r, t.data() + t.data_size);
			std::string name;

			a.on_string = [&](std::string_view, const std::uint64_t at, const std::uint64_t length)
			{
				const auto text = r.read(at, static_cast<std::size_t>(length));
				name.assign(text.begin(), text.end());
			};

			const auto end = a.value(t.data(), {});
			if (!end) return {};

			after = *end;
			while (!name.empty() && name.back() == '\0') name.pop_back();
			return name;
		}

		layout read_layout(reader& r)
		{
			layout l;
			const auto size = r.size();
			if (size < 13) throw format_error("the FLV is cut short");

			const auto header = r.read(0, 9);
			const auto data_offset = load_be32(header.data() + 5);
			if (data_offset < 9 || data_offset > size - 4) throw format_error("the FLV header is damaged");

			l.first_tag = data_offset + 4ull;
			auto pos = l.first_tag;

			for (auto n = 0; n < max_scan_tags && size - pos >= tag_header + 4; ++n)
			{
				const auto h = r.read(pos, tag_header);
				flv_tag t;
				t.start = pos;
				t.data_size = static_cast<std::uint32_t>(h[1]) << 16 | static_cast<std::uint32_t>(h[2]) << 8 | h[3];
				const auto timestamp = static_cast<std::uint32_t>(h[7]) << 24 | static_cast<std::uint32_t>(h[4]) << 16 |
					static_cast<std::uint32_t>(h[5]) << 8 | h[6];

				if (t.end() > size || timestamp != 0) break;

				if ((h[0] & 0x1F) == script_tag)
				{
					std::uint64_t after = 0;
					const auto name = script_name(r, t, after);

					if (name == "onMetaData" && !l.meta)
					{
						l.meta = t;
					}
					else if (name == "onXMPData" && !l.xmp)
					{
						l.xmp = t;
						amf a(r, t.data() + t.data_size);
						a.on_string = [&l](const std::string_view key, const std::uint64_t at, const std::uint64_t length)
						{
							if (key == "liveXML" && l.xml_length == 0)
							{
								l.xml_at = at;
								l.xml_length = length;
							}
						};
						a.value(after, {});
					}
				}

				pos = t.end();
			}

			return l;
		}

		bytes be_double(const double v)
		{
			bytes out;
			const auto bits = std::bit_cast<std::uint64_t>(v);
			append_be32(out, static_cast<std::uint32_t>(bits >> 32));
			append_be32(out, static_cast<std::uint32_t>(bits));
			return out;
		}
	}

	bool is_flv(const std::span<const std::uint8_t> head) noexcept
	{
		return head.size() >= 9 && head[0] == 'F' && head[1] == 'L' && head[2] == 'V' && head[3] == 1 && load_be32(head.data() + 5) >= 9;
	}

	void read_flv_packet(reader& r, packet_reading& out)
	{
		const auto l = read_layout(r);

		if (l.xmp && l.xml_length > 0)
		{
			const auto packet = r.read(l.xml_at, static_cast<std::size_t>(l.xml_length));
			out.packet.assign(packet.begin(), packet.end());
			while (!out.packet.empty() && out.packet.back() == '\0') out.packet.pop_back();
		}

		out.readable = true;
	}

	void plan_flv(reader& r, const metadata_update& update, write_plan& plan)
	{
		if (!update.xmp) return;

		const auto l = read_layout(r);

		if (l.xmp && l.xml_length > 0)
		{
			auto old_length = l.xml_length;
			if (r.view(l.xml_at + old_length - 1, 1)[0] == 0) --old_length;

			const auto packet = serialize_packet(*update.xmp, old_length);

			if (packet.size() == old_length)
			{
				push_changed(r, plan, l.xml_at, to_bytes(packet));
				return;
			}
		}

		// The tag: the name, then an array holding the packet and its terminator, as the toolkit wrote it.
		const auto packet = serialize_packet(*update.xmp, 0);
		bytes data;
		data.reserve(packet.size() + 32);
		data.insert(data.end(), {0x02, 0x00, 0x09});
		append(data, "onXMPData");
		data.push_back(0x08);
		append_be32(data, 1);
		data.insert(data.end(), {0x00, 0x07});
		append(data, "liveXML");

		const auto length = packet.size() + 1;

		if (length <= 0xFFFF)
		{
			data.insert(data.end(), {0x02, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)});
		}
		else
		{
			data.push_back(0x0C);
			append_be32(data, static_cast<std::uint32_t>(length));
		}

		append(data, packet);
		data.push_back(0);
		// The array's end: an empty name and the end marker.
		data.insert(data.end(), {0x00, 0x00, 0x09});
		if (data.size() > 0xFFFFFF) throw format_error("the XMP packet is too large for an FLV tag");

		const auto data_size = static_cast<std::uint32_t>(data.size());
		bytes unit = {script_tag, static_cast<std::uint8_t>(data_size >> 16), static_cast<std::uint8_t>(data_size >> 8),
		              static_cast<std::uint8_t>(data_size), 0, 0, 0, 0, 0, 0, 0};
		unit.insert(unit.end(), data.begin(), data.end());
		append_be32(unit, static_cast<std::uint32_t>(tag_header + data_size));

		const auto insert_at = l.meta ? l.meta->end() : l.first_tag;
		const auto added = static_cast<std::uint64_t>(unit.size());
		const auto removed = l.xmp ? l.xmp->end() - l.xmp->start : 0;

		if (l.xmp && l.xmp->start == insert_at)
		{
			plan.replacements.push_back({insert_at, removed, std::move(unit)});
		}
		else
		{
			plan.replacements.push_back({insert_at, 0, std::move(unit)});
			if (l.xmp) plan.replacements.push_back({l.xmp->start, removed, {}});
		}

		if (!l.meta) return;

		// Where a byte of the file lands once the tags have moved.
		const auto moved = [&](const std::uint64_t p)
		{
			auto q = p;
			if (p >= insert_at) q += added;
			if (l.xmp && p >= l.xmp->end()) q -= removed;
			return q;
		};

		const auto old_size = r.size();
		const auto new_size = old_size + added - removed;

		amf a(r, l.meta->data() + l.meta->data_size);
		a.on_number = [&](const std::string_view key, const std::uint64_t at)
		{
			const auto value = std::bit_cast<double>(r.be64(at));
			if (!std::isfinite(value) || value < 0 || value != std::floor(value) || value > static_cast<double>(old_size)) return;

			const auto position = static_cast<std::uint64_t>(value);
			auto updated = position;

			if ((key == "filepositions" || key == "lastkeyframelocation") && position < old_size) updated = moved(position);
			else if (key == "filesize" && position == old_size) updated = new_size;

			if (updated != position) plan.replacements.push_back({at, 8, be_double(static_cast<double>(updated))});
		};

		std::uint64_t after = 0;
		if (!script_name(r, *l.meta, after).empty()) a.value(after, {});
	}
}
