// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: A JPEG's gain map - the picture an Ultra HDR, Adobe, Apple or ISO 21496-1 JPEG holds after its
// primary, saying how much brighter than the primary each part of the scene is shown on an HDR display -
// found through the primary's multi-picture index and attached to a primary; the XMP that says one follows
// kept true to what the file holds; and a gain map's own metadata - ISO 21496-1's APP2, Adobe's hdrgm XMP -
// read, and written into a JPEG made a gain map.

#include "gain_map_internal.h"

#include "libfiles/encode.h"
#include "libfiles/hdr.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <string>

namespace libfiles
{
	namespace detail
	{
		namespace
		{
			constexpr std::uint8_t soi = 0xD8, eoi = 0xD9, sos = 0xDA, app1 = 0xE1, app2 = 0xE2, comment = 0xFE;

			constexpr std::string_view xmp_identifier{"http://ns.adobe.com/xap/1.0/\0", 29};
			constexpr std::string_view hdrgm_ns = "http://ns.adobe.com/hdr-gain-map/1.0/";
			constexpr std::string_view container_ns = "http://ns.google.com/photos/1.0/container/";
			constexpr std::string_view item_ns = "http://ns.google.com/photos/1.0/container/item/";
			constexpr std::string_view apple_gain_map_type = "urn:com:apple:photo:2020:aux:hdrgainmap";

			constexpr std::uint16_t mp_entry_tag = 0xB002;
			constexpr std::size_t mp_entry_bytes = 16;
			// The MP entry attribute Ultra HDR gives its primary: a baseline MP primary picture.
			constexpr std::uint32_t mp_primary = 0x030000;
			// A segment's length field counts itself, and holds at most 65535.
			constexpr std::size_t max_segment_payload = 65533;

			bool begins(const std::span<const std::uint8_t> payload, const std::string_view prefix)
			{
				return payload.size() >= prefix.size() &&
				       std::memcmp(payload.data(), prefix.data(), prefix.size()) == 0;
			}

			std::string_view text_of(const std::span<const std::uint8_t> payload)
			{
				return {reinterpret_cast<const char*>(payload.data()), payload.size()};
			}

			bool is_app(const std::uint8_t marker)
			{
				return marker >= 0xE0 && marker <= 0xEF;
			}

			struct segment
			{
				std::uint8_t marker = 0;
				// Where its marker begins.
				std::size_t start = 0;
				std::span<const std::uint8_t> payload;
			};

			// The segments before the first scan, and where that scan's marker lies: the end of data when
			// the header is all there is. False when data is not a JPEG's header, with the segments read
			// before the fault kept.
			bool split_header(const std::span<const std::uint8_t> data, std::vector<segment>& segments,
			                  std::size_t& scan)
			{
				scan = data.size();
				if (data.size() < 2 || data[0] != 0xFF || data[1] != soi) return false;
				std::size_t at = 2;

				while (at < data.size())
				{
					if (data[at] != 0xFF) return false;
					auto marker = at + 1;
					while (marker < data.size() && data[marker] == 0xFF) ++marker;
					if (marker >= data.size()) return true;

					const auto code = data[marker];

					if (code == sos || code == eoi)
					{
						scan = at;
						return true;
					}

					if (code == 0x00 || code == 0x01 || (code >= 0xD0 && code <= soi) || data.size() - marker < 3)
						return false;
					const std::size_t length = static_cast<std::size_t>(data[marker + 1]) << 8 | data[marker + 2];
					if (length < 2 || length > data.size() - marker - 1) return false;
					segments.push_back({code, at, data.subspan(marker + 3, length - 2)});
					at = marker + 1 + length;
				}

				return true;
			}

			std::uint16_t get16(const std::span<const std::uint8_t> b, const std::size_t at, const bool little_endian)
			{
				return little_endian ? static_cast<std::uint16_t>(b[at] | b[at + 1] << 8)
				                     : static_cast<std::uint16_t>(b[at] << 8 | b[at + 1]);
			}

			std::uint32_t get32(const std::span<const std::uint8_t> b, const std::size_t at, const bool little_endian)
			{
				std::uint32_t v = 0;
				for (std::size_t i = 0; i < 4; ++i)
					v |= static_cast<std::uint32_t>(b[at + i]) << (little_endian ? 8 * i : 24 - 8 * i);
				return v;
			}

			void put32(const std::span<std::uint8_t> b, const std::size_t at, const std::uint32_t v,
			           const bool little_endian)
			{
				for (std::size_t i = 0; i < 4; ++i)
					b[at + i] = static_cast<std::uint8_t>(v >> (little_endian ? 8 * i : 24 - 8 * i));
			}

			void emit(std::vector<std::uint8_t>& out, const std::uint8_t marker,
			          const std::span<const std::uint8_t> payload)
			{
				const auto length = payload.size() + 2;
				out.insert(out.end(), {0xFF, marker, static_cast<std::uint8_t>(length >> 8),
				                       static_cast<std::uint8_t>(length & 0xFF)});
				out.insert(out.end(), payload.begin(), payload.end());
			}

			// The packet as an APP1 segment holds it: as the data model serializes it when that fits, else
			// compact with no padding. Empty when even that will not fit.
			std::string fit_packet(const xmp::meta& meta)
			{
				auto packet = meta.serialize();
				if (packet.size() + xmp_identifier.size() <= max_segment_payload) return packet;

				xmp::serialize_options tight;
				tight.use_compact_format = true;
				tight.omit_all_formatting = true;
				tight.read_only_packet = true;
				packet = meta.serialize(tight);
				return packet.size() + xmp_identifier.size() <= max_segment_payload ? packet : std::string{};
			}
		}

		std::optional<mpf_index> read_mpf_index(const std::span<const std::uint8_t> index)
		{
			if (index.size() < 8) return {};

			mpf_index mpf;
			if (index[0] == 'I' && index[1] == 'I' && index[2] == 0x2A && index[3] == 0) mpf.little_endian = true;
			else if (index[0] != 'M' || index[1] != 'M' || index[2] != 0 || index[3] != 0x2A) return {};

			const auto le = mpf.little_endian;
			const std::size_t ifd = get32(index, 4, le);
			if (ifd > index.size() - 2) return {};
			const std::size_t count = get16(index, ifd, le);
			if ((index.size() - ifd - 2) / 12 < count) return {};

			for (std::size_t i = 0; i < count; ++i)
			{
				const auto tag = ifd + 2 + i * 12;
				if (get16(index, tag, le) != mp_entry_tag) continue;

				const std::size_t bytes = get32(index, tag + 4, le);
				const std::size_t at = get32(index, tag + 8, le);
				if (bytes == 0 || bytes % mp_entry_bytes != 0 || at > index.size() || index.size() - at < bytes)
					return {};

				for (auto e = at; e < at + bytes; e += mp_entry_bytes)
				{
					mpf.entries.push_back({get32(index, e, le), get32(index, e + 4, le), get32(index, e + 8, le), e});
				}

				return mpf;
			}

			return {};
		}

		void write_mpf_entry(const std::span<std::uint8_t> index, const mpf_index& mpf, const mpf_entry& entry)
		{
			put32(index, entry.at + 4, entry.size, mpf.little_endian);
			put32(index, entry.at + 8, entry.offset, mpf.little_endian);
		}

		std::vector<std::uint8_t> make_mpf_payload(const std::uint64_t primary_size, const std::uint64_t gain_map_size,
		                                           const std::uint64_t index_at)
		{
			std::vector<std::uint8_t> p(mpf_identifier.begin(), mpf_identifier.end());
			const auto be16 = [&p](const std::uint32_t v)
			{
				p.insert(p.end(), {static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)});
			};
			const auto be32 = [&p](const std::uint64_t v)
			{
				p.insert(p.end(), {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16),
				                   static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)});
			};

			// As Ultra HDR writes it: a big-endian index whose one directory holds the version, the number
			// of pictures and their entries, which follow the directory.
			p.insert(p.end(), {'M', 'M', 0, 0x2A});
			be32(8);
			be16(3);
			be16(0xB000);
			be16(7);
			be32(4);
			p.insert(p.end(), {'0', '1', '0', '0'});
			be16(0xB001);
			be16(4);
			be32(1);
			be32(2);
			be16(mp_entry_tag);
			be16(7);
			be32(2 * mp_entry_bytes);
			be32(8 + 2 + 3 * 12 + 4);
			be32(0);

			be32(mp_primary);
			be32(primary_size);
			be32(0);
			be32(0);
			be32(0);
			be32(gain_map_size);
			be32(primary_size - index_at);
			be32(0);
			return p;
		}

		gain_map_schemes read_gain_map_schemes(const std::span<const std::uint8_t> header)
		{
			std::vector<segment> segments;
			std::size_t scan = 0;
			split_header(header, segments, scan);

			gain_map_schemes schemes;

			for (const auto& s : segments)
			{
				if (s.marker == app1 && begins(s.payload, xmp_identifier))
				{
					const auto text = text_of(s.payload);
					schemes.adobe = schemes.adobe || text.find(hdrgm_ns) != std::string_view::npos;
					schemes.apple = schemes.apple || text.find(apple_gain_map_type) != std::string_view::npos;
				}
				else if (s.marker == app2 && begins(s.payload, iso_gain_map_identifier) &&
				         !is_iso_version_segment(s.payload))
				{
					schemes.iso = true;
				}
			}

			return schemes;
		}

		bool is_iso_version_segment(const std::span<const std::uint8_t> payload) noexcept
		{
			return payload.size() == iso_gain_map_identifier.size() + iso_version_bytes &&
			       std::memcmp(payload.data(), iso_gain_map_identifier.data(), iso_gain_map_identifier.size()) == 0;
		}

		std::size_t jpeg_end(const std::span<const std::uint8_t> data)
		{
			if (data.size() < 4 || data[0] != 0xFF || data[1] != soi) return 0;
			std::size_t at = 2;
			auto in_scan = false;

			while (at < data.size())
			{
				if (data[at] != 0xFF)
				{
					// Only entropy-coded data runs between markers.
					if (!in_scan) return 0;
					const auto* const next = static_cast<const std::uint8_t*>(
						std::memchr(data.data() + at, 0xFF, data.size() - at));
					if (!next) return 0;
					at = static_cast<std::size_t>(next - data.data());
					continue;
				}

				if (data.size() - at < 2) return 0;
				const auto code = data[at + 1];

				if (code == 0xFF)
				{
					++at;
				}
				else if (code == eoi)
				{
					return at + 2;
				}
				else if (code == 0x00 || code == 0x01 || (code >= 0xD0 && code <= 0xD7))
				{
					// A stuffed byte or a restart within a scan.
					at += 2;
				}
				else
				{
					if (code == soi || data.size() - at < 4) return 0;
					const std::size_t length = static_cast<std::size_t>(data[at + 2]) << 8 | data[at + 3];
					if (length < 2 || length > data.size() - at - 2) return 0;
					at += 2 + length;
					in_scan = code == sos;
				}
			}

			return 0;
		}

		bool may_signal_gain_map(const std::string_view packet) noexcept
		{
			return packet.find("hdr-gain-map") != std::string_view::npos ||
			       packet.find("photos/1.0/container") != std::string_view::npos;
		}

		bool signals_adobe_gain_map(const std::string_view packet) noexcept
		{
			return packet.find(hdrgm_ns) != std::string_view::npos;
		}

		bool signal_gain_map(xmp::meta& primary, const std::optional<gain_map_signal>& gain_map)
		{
			const auto container = xmp::register_namespace(container_ns, "Container");
			const auto item = xmp::register_namespace(item_ns, "Item");
			xmp::register_namespace(hdrgm_ns, "hdrgm");

			if (primary.exists(hdrgm_ns, "GainMapMax")) return false;

			const auto field = [&](const int i, const std::string_view name)
			{
				return std::format("Directory[{}]/{}Item/{}{}", i, container, item, name);
			};

			std::vector<int> gain_maps;
			auto others = 0;
			const auto count = primary.count_array_items(container_ns, "Directory");

			for (auto i = 1; i <= count; ++i)
			{
				std::string semantic;
				primary.get_property(container_ns, field(i, "Semantic"), &semantic);
				if (semantic == "GainMap") gain_maps.push_back(i);
				else if (semantic != "Primary") ++others;
			}

			auto changed = false;

			if (gain_map)
			{
				const auto length = std::to_string(gain_map->length);

				for (const auto i : gain_maps)
				{
					std::string old;
					if (primary.get_property(container_ns, field(i, "Length"), &old) && old == length) continue;
					primary.set_property(container_ns, field(i, "Length"), length);
					changed = true;
				}

				if (gain_map->adobe && !primary.exists(hdrgm_ns, "Version"))
				{
					primary.set_property(hdrgm_ns, "Version", "1.0");
					changed = true;
				}

				return changed;
			}

			if (primary.exists(hdrgm_ns, "Version"))
			{
				primary.delete_property(hdrgm_ns, "Version");
				changed = true;
			}

			if (!gain_maps.empty())
			{
				if (others == 0)
				{
					primary.delete_property(container_ns, "Directory");
				}
				else
				{
					for (auto i = gain_maps.rbegin(); i != gain_maps.rend(); ++i)
					{
						primary.delete_property(container_ns, std::format("Directory[{}]", *i));
					}
				}

				changed = true;
			}

			return changed;
		}

		std::vector<std::uint8_t> signalled_xmp(const std::span<const std::uint8_t> payload,
		                                        const std::optional<gain_map_signal>& gain_map)
		{
			if (!payload.empty() && !begins(payload, xmp_identifier)) return {};
			const auto packet = payload.empty() ? std::string_view{} : text_of(payload.subspan(xmp_identifier.size()));
			if (!may_signal_gain_map(packet) && !(gain_map && gain_map->adobe)) return {};

			try
			{
				xmp::meta meta;
				meta.parse(packet);
				if (!signal_gain_map(meta, gain_map)) return {};

				const auto updated = fit_packet(meta);
				if (updated.empty()) return {};

				std::vector<std::uint8_t> result(xmp_identifier.begin(), xmp_identifier.end());
				result.insert(result.end(), updated.begin(), updated.end());
				return result;
			}
			catch (const std::exception&)
			{
				return {};
			}
		}
	}

	std::span<const std::uint8_t> find_jpeg_gain_map(const std::span<const std::uint8_t> jpeg)
	{
		std::vector<detail::segment> segments;
		std::size_t scan = 0;
		if (!detail::split_header(jpeg, segments, scan)) return {};

		for (const auto& s : segments)
		{
			if (s.marker != detail::app2 || !detail::begins(s.payload, detail::mpf_identifier)) continue;

			const auto index_at = static_cast<std::size_t>(s.payload.data() - jpeg.data()) +
			                      detail::mpf_identifier.size();
			const auto mpf = detail::read_mpf_index(s.payload.subspan(detail::mpf_identifier.size()));
			if (!mpf) return {};

			for (std::size_t e = 1; e < mpf->entries.size(); ++e)
			{
				const auto offset = mpf->entries[e].offset;
				if (offset == 0 || offset >= jpeg.size() - index_at) continue;

				const auto rest = jpeg.subspan(index_at + offset);
				const auto end = detail::jpeg_end(rest);
				if (end == 0) continue;

				const auto picture = rest.first(end);
				if (detail::read_gain_map_schemes(picture).any()) return picture;
			}

			return {};
		}

		return {};
	}

	std::vector<std::uint8_t> attach_jpeg_gain_map(const std::span<const std::uint8_t> primary,
	                                               const std::span<const std::uint8_t> gain_map)
	{
		using namespace detail;

		std::vector<segment> segments;
		std::size_t scan = 0;
		const auto end = jpeg_end(primary);
		if (end == 0 || !split_header(primary, segments, scan) || scan >= end) return {};

		std::span<const std::uint8_t> picture;
		std::optional<gain_map_signal> signal;
		std::span<const std::uint8_t> iso_versions;

		if (!gain_map.empty())
		{
			const auto gain_map_end = jpeg_end(gain_map);
			if (gain_map_end == 0) return {};
			picture = gain_map.first(gain_map_end);

			const auto schemes = read_gain_map_schemes(picture);
			signal = gain_map_signal{picture.size(), schemes.adobe};

			std::vector<segment> gain_map_segments;
			std::size_t gain_map_scan = 0;
			split_header(picture, gain_map_segments, gain_map_scan);

			for (const auto& s : gain_map_segments)
			{
				if (s.marker == app2 && begins(s.payload, iso_gain_map_identifier) &&
				    s.payload.size() >= iso_gain_map_identifier.size() + iso_version_bytes)
				{
					iso_versions = s.payload.subspan(iso_gain_map_identifier.size(), iso_version_bytes);
					break;
				}
			}
		}

		// The signalling goes after the primary's application segments and comments, before its tables. Every
		// segment is copied, wherever it lies, but for the signalling it replaces.
		std::vector<std::uint8_t> head{0xFF, soi};
		std::vector<std::uint8_t> tail;
		tail.reserve(end - scan + 1024);
		auto past_applications = false;
		auto has_xmp = false;

		for (const auto& s : segments)
		{
			past_applications = past_applications || (!is_app(s.marker) && s.marker != comment);
			auto& to = past_applications ? tail : head;

			if (s.marker == app2 && (begins(s.payload, mpf_identifier) || is_iso_version_segment(s.payload))) continue;

			if (s.marker == app1 && !has_xmp && begins(s.payload, xmp_identifier))
			{
				has_xmp = true;

				if (const auto updated = signalled_xmp(s.payload, signal); !updated.empty())
				{
					emit(to, s.marker, updated);
					continue;
				}
			}

			emit(to, s.marker, s.payload);
		}

		tail.insert(tail.end(), primary.begin() + static_cast<std::ptrdiff_t>(scan),
		            primary.begin() + static_cast<std::ptrdiff_t>(end));

		if (!has_xmp && signal && signal->adobe)
		{
			const auto created = signalled_xmp({}, signal);
			if (!created.empty()) emit(head, app1, created);
		}

		if (signal && !iso_versions.empty())
		{
			std::vector<std::uint8_t> versions(iso_gain_map_identifier.begin(), iso_gain_map_identifier.end());
			versions.insert(versions.end(), iso_versions.begin(), iso_versions.end());
			emit(head, app2, versions);
		}

		auto out = std::move(head);

		if (signal)
		{
			// The marker, the length and make_mpf_payload's 86 bytes.
			constexpr std::size_t mpf_segment_bytes = 4 + 86;
			const auto primary_size = out.size() + mpf_segment_bytes + tail.size();
			const auto index_at = out.size() + 4 + mpf_identifier.size();
			if (primary_size > std::numeric_limits<std::uint32_t>::max() ||
			    picture.size() > std::numeric_limits<std::uint32_t>::max())
			{
				return {};
			}

			emit(out, app2, make_mpf_payload(primary_size, picture.size(), index_at));
		}

		out.reserve(out.size() + tail.size() + picture.size());
		out.insert(out.end(), tail.begin(), tail.end());
		out.insert(out.end(), picture.begin(), picture.end());
		return out;
	}

	namespace
	{
		std::optional<double> parse_double(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '+')) text.remove_prefix(1);
			double v = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v);
			if (error != std::errc{} || !std::isfinite(v)) return {};
			return v;
		}

		// One of Adobe's hdrgm values, for each channel: one value said for all three, or a sequence of three.
		std::optional<std::array<double, 3>> hdrgm_values(const xmp::meta& meta, const std::string_view name,
		                                                  const std::optional<double> fallback)
		{
			std::string text;
			xmp::option_bits options = 0;

			if (!meta.get_property(detail::hdrgm_ns, name, &text, &options))
			{
				if (!fallback) return {};
				return std::array{*fallback, *fallback, *fallback};
			}

			if (!xmp::is_array(options))
			{
				const auto v = parse_double(text);
				if (!v) return {};
				return std::array{*v, *v, *v};
			}

			if (meta.count_array_items(detail::hdrgm_ns, name) != 3) return {};

			std::array<double, 3> values{};
			for (auto i = 0; i < 3; ++i)
			{
				if (!meta.get_array_item(detail::hdrgm_ns, name, i + 1, &text)) return {};
				const auto v = parse_double(text);
				if (!v) return {};
				values[i] = *v;
			}

			return values;
		}

		// Adobe's hdrgm values as a gain map's own XMP states them, which Ultra HDR writes too: log2 gains, each
		// value's gamma, the offsets, and the renditions' headrooms as HDR capacities. Nothing where the base is HDR.
		std::optional<gain_map_metadata> read_hdrgm(const std::string_view packet)
		{
			xmp::register_namespace(detail::hdrgm_ns, "hdrgm");
			xmp::meta meta;
			meta.parse(packet);

			std::string base_is_hdr;
			if (meta.get_property(detail::hdrgm_ns, "BaseRenditionIsHDR", &base_is_hdr) &&
			    (base_is_hdr == "True" || base_is_hdr == "true"))
			{
				return {};
			}

			const auto min = hdrgm_values(meta, "GainMapMin", 0.0);
			const auto max = hdrgm_values(meta, "GainMapMax", std::nullopt);
			const auto gamma = hdrgm_values(meta, "Gamma", 1.0);
			const auto offset_sdr = hdrgm_values(meta, "OffsetSDR", 1.0 / 64.0);
			const auto offset_hdr = hdrgm_values(meta, "OffsetHDR", 1.0 / 64.0);
			const auto capacity_min = hdrgm_values(meta, "HDRCapacityMin", 0.0);
			const auto capacity_max = hdrgm_values(meta, "HDRCapacityMax", std::nullopt);
			if (!min || !max || !gamma || !offset_sdr || !offset_hdr || !capacity_min || !capacity_max) return {};

			gain_map_metadata m;
			m.gain_min = *min;
			m.gain_max = *max;
			m.gamma = *gamma;
			m.base_offset = *offset_sdr;
			m.alternate_offset = *offset_hdr;
			m.base_headroom = std::max((*capacity_min)[0], 0.0);
			m.alternate_headroom = std::max((*capacity_max)[0], 0.0);
			for (const auto g : m.gamma)
				if (!(g > 0.0)) return {};
			return m;
		}

		// A gain map's own XMP in Adobe's scheme, as Ultra HDR writes it: one value for all channels where they
		// agree, else a sequence of three.
		std::string write_hdrgm(const gain_map_metadata& m)
		{
			xmp::register_namespace(detail::hdrgm_ns, "hdrgm");
			xmp::meta meta;

			const auto number = [](const double v)
			{
				return std::format("{}", v);
			};
			const auto put = [&](const std::string_view name, const std::array<double, 3>& v)
			{
				if (v[0] == v[1] && v[1] == v[2])
				{
					meta.set_property(detail::hdrgm_ns, name, number(v[0]));
					return;
				}

				for (const auto c : v) meta.append_array_item(detail::hdrgm_ns, name, xmp::array_is_ordered, number(c));
			};

			meta.set_property(detail::hdrgm_ns, "Version", "1.0");
			put("GainMapMin", m.gain_min);
			put("GainMapMax", m.gain_max);
			put("Gamma", m.gamma);
			put("OffsetSDR", m.base_offset);
			put("OffsetHDR", m.alternate_offset);
			meta.set_property(detail::hdrgm_ns, "HDRCapacityMin", number(m.base_headroom));
			meta.set_property(detail::hdrgm_ns, "HDRCapacityMax", number(m.alternate_headroom));
			meta.set_property(detail::hdrgm_ns, "BaseRenditionIsHDR", "False");

			xmp::serialize_options compact;
			compact.use_compact_format = true;
			compact.omit_all_formatting = true;
			compact.read_only_packet = true;
			return meta.serialize(compact);
		}
	}

	std::optional<gain_map_metadata> read_jpeg_gain_map_metadata(const std::span<const std::uint8_t> gain_map)
	{
		using namespace detail;

		std::vector<segment> segments;
		std::size_t scan = 0;
		split_header(gain_map, segments, scan);

		for (const auto& s : segments)
		{
			if (s.marker == app2 && begins(s.payload, iso_gain_map_identifier) && !is_iso_version_segment(s.payload))
			{
				if (auto m = read_gain_map_metadata(s.payload.subspan(iso_gain_map_identifier.size()))) return m;
			}
		}

		for (const auto& s : segments)
		{
			if (s.marker != app1 || !begins(s.payload, xmp_identifier)) continue;
			const auto packet = text_of(s.payload.subspan(xmp_identifier.size()));
			if (packet.find(hdrgm_ns) == std::string_view::npos) continue;

			try
			{
				if (auto m = read_hdrgm(packet)) return m;
			}
			catch (const std::exception&)
			{
				// A packet the toolkit rejects whole says nothing of the gain map.
			}
		}

		return {};
	}

	std::vector<std::uint8_t> mark_jpeg_gain_map(const std::span<const std::uint8_t> gain_map,
	                                             const gain_map_metadata& metadata)
	{
		using namespace detail;

		std::vector<segment> segments;
		std::size_t scan = 0;
		const auto end = jpeg_end(gain_map);
		if (end == 0 || !split_header(gain_map, segments, scan) || scan >= end) return {};

		const auto iso = write_gain_map_metadata(metadata);
		if (iso.empty()) return {};

		std::string packet;

		try
		{
			packet = write_hdrgm(metadata);
		}
		catch (const std::exception&)
		{
			return {};
		}

		if (packet.size() + xmp_identifier.size() > max_segment_payload) return {};

		std::vector<std::uint8_t> xmp(xmp_identifier.begin(), xmp_identifier.end());
		xmp.insert(xmp.end(), packet.begin(), packet.end());
		std::vector<std::uint8_t> iso_segment(iso_gain_map_identifier.begin(), iso_gain_map_identifier.end());
		iso_segment.insert(iso_segment.end(), iso.begin(), iso.end());

		// After the SOI and a JFIF APP0, as Ultra HDR lays a gain map out; whatever said how to apply it before goes.
		std::vector<std::uint8_t> out{0xFF, soi};
		auto marked = false;

		for (const auto& s : segments)
		{
			const auto jfif = s.marker == 0xE0 && begins(s.payload, {"JFIF\0", 5});
			if (!marked && !jfif)
			{
				emit(out, app1, xmp);
				emit(out, app2, iso_segment);
				marked = true;
			}

			if ((s.marker == app1 && begins(s.payload, xmp_identifier)) ||
			    (s.marker == app2 && begins(s.payload, iso_gain_map_identifier)))
			{
				continue;
			}

			emit(out, s.marker, s.payload);
		}

		if (!marked)
		{
			emit(out, app1, xmp);
			emit(out, app2, iso_segment);
		}

		out.insert(out.end(), gain_map.begin() + static_cast<std::ptrdiff_t>(scan),
		           gain_map.begin() + static_cast<std::ptrdiff_t>(end));
		return out;
	}
}
