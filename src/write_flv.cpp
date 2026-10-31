// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing metadata to FLV files. The packet lives where the toolkit kept it, as the "liveXML"
// string of an onXMPData script tag among the tags at time zero, after onMetaData. A packet that fits is
// patched where it lies; otherwise the file is rewritten in one pass with a new tag, given padding for
// the next edit. A rewrite moves the media, so the file positions onMetaData keeps for seeking - its
// keyframe index and file size - are moved with it, which the toolkit did not do.

#include "flv_internal.h"
#include "legacy.h"

#include <bit>
#include <cmath>

namespace libfiles::detail
{
	namespace
	{
		using flv::script_tag;
		using flv::tag_header;
		using flv_tag = flv::tag;

		// How many tags at time zero are looked through for onMetaData and onXMPData.
		constexpr int max_scan_tags = 64;

		struct layout
		{
			std::uint64_t first_tag = 0;
			std::optional<flv_tag> meta;
			std::optional<flv_tag> xmp;
			// The liveXML string's bytes, its terminator included.
			std::uint64_t xml_at = 0;
			std::uint64_t xml_length = 0;
		};

		layout read_layout(reader& r)
		{
			layout l;
			l.first_tag = flv::first_tag(r);
			auto pos = l.first_tag;

			for (auto n = 0; n < max_scan_tags; ++n)
			{
				const auto t = flv::read_tag(r, pos);
				if (!t || t->timestamp != 0) break;

				if (t->type == script_tag)
				{
					std::uint64_t after = 0;
					const auto name = flv::script_name(r, *t, after);

					if (name == "onMetaData" && !l.meta)
					{
						l.meta = t;
					}
					else if (name == "onXMPData" && !l.xmp)
					{
						l.xmp = t;
						flv::amf a(r, t->data() + t->data_size);
						a.on_value = [&l](const flv::amf_value& v)
						{
							const auto text = v.type == flv::amf_string || v.type == flv::amf_long_string;

							if (text && v.key == "liveXML" && l.xml_length == 0)
							{
								l.xml_at = v.at;
								l.xml_length = v.length;
							}
						};
						a.value(after);
					}
				}

				pos = t->end();
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

		flv::amf a(r, l.meta->data() + l.meta->data_size);
		a.on_value = [&](const flv::amf_value& v)
		{
			if (v.type != flv::amf_number) return;

			const auto value = std::bit_cast<double>(r.be64(v.at));
			if (!std::isfinite(value) || value < 0 || value != std::floor(value) || value > static_cast<double>(old_size)) return;

			const auto position = static_cast<std::uint64_t>(value);
			auto updated = position;

			if ((v.key == "filepositions" || v.key == "lastkeyframelocation") && position < old_size) updated = moved(position);
			else if (v.key == "filesize" && position == old_size) updated = new_size;

			if (updated != position) plan.replacements.push_back({v.at, 8, be_double(static_cast<double>(updated))});
		};

		std::uint64_t after = 0;
		if (!flv::script_name(r, *l.meta, after).empty()) a.value(after);
	}
}
