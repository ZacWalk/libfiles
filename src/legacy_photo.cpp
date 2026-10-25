// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: A photo's legacy metadata after an edit. Each field the edit sets is written to EXIF and
// IPTC where the toolkit reconciled it, and nothing else in either block changes. IPTC is written as
// UTF-8 and declares it; each dataset's header in the block is read by the IPTC reader's rule, every
// dataset is kept, and text already in it is carried over as the reader reads it - UTF-8 when it is
// valid UTF-8, Windows-1252 otherwise - so no existing value changes meaning.

#include "legacy.h"

#include "iptc_internal.h"
#include "photoshop_internal.h"
#include "libfiles/iptc.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <tuple>

namespace libfiles::detail
{
	namespace
	{
		using directory = tiff_editor::directory;

		// EXIF tags.
		constexpr std::uint16_t tag_orientation = 0x0112;
		constexpr std::uint16_t tag_image_description = 0x010E;
		constexpr std::uint16_t tag_artist = 0x013B;
		constexpr std::uint16_t tag_copyright = 0x8298;
		constexpr std::uint16_t tag_rating = 0x4746;
		constexpr std::uint16_t tag_rating_percent = 0x4749;
		constexpr std::uint16_t tag_xp_keywords = 0x9C9E;
		constexpr std::uint16_t tag_date_time_original = 0x9003;
		constexpr std::uint16_t tag_date_time_digitized = 0x9004;
		constexpr std::uint16_t tag_gps_version = 0x0000;
		constexpr std::uint16_t tag_gps_latitude_ref = 0x0001;
		constexpr std::uint16_t tag_gps_latitude = 0x0002;
		constexpr std::uint16_t tag_gps_longitude_ref = 0x0003;
		constexpr std::uint16_t tag_gps_longitude = 0x0004;

		// IPTC-IIM datasets of record 2, with the most bytes each may hold.
		struct iim
		{
			std::uint8_t number;
			std::size_t max_bytes;
		};

		constexpr iim iim_object_name{5, 64};
		constexpr iim iim_keywords{25, 64};
		constexpr iim iim_date_created{55, 8};
		constexpr iim iim_time_created{60, 11};
		constexpr iim iim_digital_date{62, 8};
		constexpr iim iim_digital_time{63, 11};
		constexpr iim iim_by_line{80, 32};
		constexpr iim iim_city{90, 32};
		constexpr iim iim_state{95, 32};
		constexpr iim iim_country{101, 64};
		constexpr iim iim_credit{110, 32};
		constexpr iim iim_source{115, 32};
		constexpr iim iim_copyright{116, 128};
		constexpr iim iim_caption{120, 2000};

		bytes to_utf16le(const std::string_view text)
		{
			bytes out;
			const auto* const s = reinterpret_cast<const std::uint8_t*>(text.data());
			std::size_t i = 0;

			const auto put = [&out](const std::uint32_t unit)
			{
				out.push_back(static_cast<std::uint8_t>(unit));
				out.push_back(static_cast<std::uint8_t>(unit >> 8));
			};

			while (i < text.size())
			{
				const auto c = s[i];
				std::uint32_t cp = 0xFFFD;
				std::size_t n = 1;

				if (c < 0x80) cp = c;
				else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) { cp = (c & 0x1Fu) << 6 | (s[i + 1] & 0x3F); n = 2; }
				else if ((c & 0xF0) == 0xE0 && i + 2 < text.size())
				{
					cp = (c & 0x0Fu) << 12 | (s[i + 1] & 0x3Fu) << 6 | (s[i + 2] & 0x3F);
					n = 3;
				}
				else if ((c & 0xF8) == 0xF0 && i + 3 < text.size())
				{
					cp = (c & 0x07u) << 18 | (s[i + 1] & 0x3Fu) << 12 | (s[i + 2] & 0x3Fu) << 6 | (s[i + 3] & 0x3F);
					n = 4;
				}

				if (cp >= 0x10000)
				{
					cp -= 0x10000;
					put(0xD800 + (cp >> 10));
					put(0xDC00 + (cp & 0x3FF));
				}
				else
				{
					put(cp);
				}

				i += n;
			}

			return out;
		}

		// Cut to at most max bytes, on a character boundary.
		std::string truncate_utf8(std::string text, const std::size_t max)
		{
			if (text.size() <= max) return text;

			auto cut = max;
			while (cut > 0 && (static_cast<std::uint8_t>(text[cut]) & 0xC0) == 0x80) --cut;
			text.resize(cut);
			return text;
		}

		// IIM ends lines with CR.
		std::string with_iim_line_ends(const std::string_view text)
		{
			std::string out;
			out.reserve(text.size());

			for (std::size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
				out.push_back(text[i] == '\n' ? '\r' : text[i]);
			}

			return out;
		}

		std::string join(const std::vector<std::string>& items, const std::string_view separator)
		{
			std::string out;

			for (const auto& item : items)
			{
				if (!out.empty()) out += separator;
				out += item;
			}

			return out;
		}

		bool parse_number(const std::string_view s, const std::size_t at, const std::size_t digits, int& out)
		{
			if (s.size() < at + digits) return false;
			const auto* const first = s.data() + at;
			const auto [end, ec] = std::from_chars(first, first + digits, out);
			return ec == std::errc{} && end == first + digits;
		}

		std::optional<date_parts> parse_date_text(const std::string_view s)
		{
			date_parts d;
			if (!parse_number(s, 0, 4, d.year)) return {};
			if (s.size() < 10 || s[4] != '-' || s[7] != '-') return {};
			if (!parse_number(s, 5, 2, d.month) || !parse_number(s, 8, 2, d.day)) return {};
			if (s.size() == 10) return d;

			if (s[10] != 'T' || s.size() < 16 || s[13] != ':') return {};
			if (!parse_number(s, 11, 2, d.hour) || !parse_number(s, 14, 2, d.minute)) return {};
			d.has_time = true;

			std::size_t at = 16;

			if (at < s.size() && s[at] == ':')
			{
				if (!parse_number(s, at + 1, 2, d.second)) return {};
				at += 3;
				if (at < s.size() && s[at] == '.')
				{
					++at;
					while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
				}
			}

			if (at == s.size()) return d;

			if (s[at] == 'Z')
			{
				d.has_zone = true;
				return at + 1 == s.size() ? std::optional(d) : std::nullopt;
			}

			int zone_hour = 0;
			int zone_minute = 0;
			if ((s[at] != '+' && s[at] != '-') || s.size() != at + 6 || s[at + 3] != ':') return {};
			if (!parse_number(s, at + 1, 2, zone_hour) || !parse_number(s, at + 4, 2, zone_minute)) return {};

			d.has_zone = true;
			d.zone_minutes = (zone_hour * 60 + zone_minute) * (s[at] == '-' ? -1 : 1);
			return d;
		}

		// "DDD,MM,SSk" or "DDD,MM.mmk", as rationals and a reference letter.
		struct gps_parts
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> rationals;
			char reference = 0;
		};

		std::optional<std::pair<std::uint32_t, std::uint32_t>> rational(const std::string_view s)
		{
			const auto dot = s.find('.');
			std::uint64_t whole = 0;
			const auto int_part = s.substr(0, dot);
			if (int_part.empty() || std::from_chars(int_part.data(), int_part.data() + int_part.size(), whole).ec != std::errc{}) return {};
			if (dot == std::string_view::npos) return whole <= 0xFFFFFFFFu ? std::optional(std::pair{static_cast<std::uint32_t>(whole), 1u}) : std::nullopt;

			auto fraction = s.substr(dot + 1);
			if (fraction.size() > 6) fraction = fraction.substr(0, 6);

			std::uint64_t numerator = whole;
			std::uint32_t denominator = 1;

			for (const auto c : fraction)
			{
				if (c < '0' || c > '9') return {};
				numerator = numerator * 10 + static_cast<std::uint64_t>(c - '0');
				denominator *= 10;
			}

			if (numerator > 0xFFFFFFFFu) return {};
			return std::pair{static_cast<std::uint32_t>(numerator), denominator};
		}

		std::optional<gps_parts> parse_xmp_gps(std::string_view s)
		{
			if (s.size() < 3) return {};

			gps_parts g;
			g.reference = s.back();
			if (g.reference != 'N' && g.reference != 'S' && g.reference != 'E' && g.reference != 'W') return {};
			s.remove_suffix(1);

			std::vector<std::string_view> parts;
			while (true)
			{
				const auto comma = s.find(',');
				parts.push_back(s.substr(0, comma));
				if (comma == std::string_view::npos) break;
				s.remove_prefix(comma + 1);
			}

			if (parts.size() != 2 && parts.size() != 3) return {};

			for (const auto part : parts)
			{
				const auto r = rational(part);
				if (!r) return {};
				g.rationals.push_back(*r);
			}

			if (g.rationals.size() == 2) g.rationals.emplace_back(0u, 1u);
			return g;
		}

		void replace_all(std::vector<iptc_dataset>& datasets, const iim field, const std::vector<std::string>& values)
		{
			std::erase_if(datasets, [field](const iptc_dataset& d) { return d.record == 2 && d.number == field.number; });

			for (const auto& value : values)
			{
				if (value.empty()) continue;
				const auto text = truncate_utf8(with_iim_line_ends(value), field.max_bytes);
				datasets.push_back({2, field.number, bytes(text.begin(), text.end())});
			}
		}

		void replace_one(std::vector<iptc_dataset>& datasets, const iim field, const std::optional<std::string>& value)
		{
			if (!value) return;
			replace_all(datasets, field, {*value});
		}

		void write_iptc_date(std::vector<iptc_dataset>& datasets, const iim date, const iim time, const std::string& xmp_date)
		{
			const auto d = parse_xmp_date(xmp_date);
			if (!d) return;

			replace_all(datasets, date, {std::format("{:04}{:02}{:02}", d->year, d->month, d->day)});

			if (!d->has_time)
			{
				replace_all(datasets, time, {});
			}
			else if (d->has_zone)
			{
				const auto zone = std::abs(d->zone_minutes);
				replace_all(datasets, time, {std::format("{:02}{:02}{:02}{}{:02}{:02}", d->hour, d->minute, d->second,
				                                         d->zone_minutes < 0 ? '-' : '+', zone / 60, zone % 60)});
			}
			else
			{
				replace_all(datasets, time, {std::format("{:02}{:02}{:02}", d->hour, d->minute, d->second)});
			}
		}

		bool is_binary_dataset(const iptc_dataset& d)
		{
			// The record version, the rasterized caption and the preview.
			return d.record != 2 || d.number == 0 || d.number == 125 || d.number >= 200;
		}
	}

	bytes utf16le(const std::string_view text)
	{
		return to_utf16le(text);
	}

	std::optional<date_parts> parse_xmp_date(const std::string_view text)
	{
		return parse_date_text(text);
	}

	std::int64_t seconds_since(const int epoch_year, const date_parts& date)
	{
		// Days from 1970-01-01 in the proleptic Gregorian calendar.
		const auto days_from_civil = [](int y, const unsigned m, const unsigned d) -> std::int64_t
		{
			y -= m <= 2;
			const auto era = (y >= 0 ? y : y - 399) / 400;
			const auto yoe = static_cast<unsigned>(y - era * 400);
			const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
			const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
			return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
		};

		const auto days = days_from_civil(date.year, static_cast<unsigned>(date.month), static_cast<unsigned>(date.day)) -
			days_from_civil(epoch_year, 1, 1);
		auto seconds = days * 86400 + date.hour * 3600 + date.minute * 60 + date.second;
		if (date.has_zone) seconds -= static_cast<std::int64_t>(date.zone_minutes) * 60;
		return seconds;
	}

	bool touches_iptc(const legacy_edits& e)
	{
		return e.title || e.description || e.copyright || e.creators || e.keywords || e.city || e.state || e.country ||
			e.credit || e.source || e.date_original || e.date_digitized;
	}

	bool touches_exif(const legacy_edits& e)
	{
		return e.description || e.copyright || e.creators || e.rating || e.rating_percent || e.keywords || e.orientation ||
			e.date_original || e.date_digitized || e.gps_latitude || e.gps_longitude;
	}

	std::vector<iptc_dataset> parse_iptc(const std::span<const std::uint8_t> block)
	{
		std::vector<iptc_dataset> out;
		std::size_t i = 0;

		while (const auto h = read_iim_header(block, i))
		{
			const auto data = block.subspan(h->data_offset, h->length);
			out.push_back({h->record, h->number, bytes(data.begin(), data.end())});
			i = h->data_offset + h->length;
		}

		return out;
	}

	bytes serialize_iptc(const std::vector<iptc_dataset>& datasets)
	{
		bytes out;

		for (const auto& d : datasets)
		{
			out.push_back(0x1C);
			out.push_back(d.record);
			out.push_back(d.number);

			if (d.data.size() < 0x8000)
			{
				out.push_back(static_cast<std::uint8_t>(d.data.size() >> 8));
				out.push_back(static_cast<std::uint8_t>(d.data.size()));
			}
			else
			{
				out.push_back(0x80);
				out.push_back(0x04);
				append_be32(out, static_cast<std::uint32_t>(d.data.size()));
			}

			out.insert(out.end(), d.data.begin(), d.data.end());
		}

		return out;
	}

	bool parse_photoshop_resources(const std::span<const std::uint8_t> block, std::vector<photoshop_resource>& out)
	{
		std::size_t pos = 0;

		while (pos < block.size())
		{
			const auto resource = read_photoshop_resource(block.subspan(pos));
			if (!resource) return false;

			const auto data = pos + resource->data_offset;
			photoshop_resource r;
			r.id = resource->id;
			r.name.assign(block.begin() + static_cast<std::ptrdiff_t>(pos + 6),
			              block.begin() + static_cast<std::ptrdiff_t>(pos + 6 + resource->name_size));
			r.data.assign(block.begin() + static_cast<std::ptrdiff_t>(data),
			              block.begin() + static_cast<std::ptrdiff_t>(data + resource->length));
			r.data_offset = data;
			out.push_back(std::move(r));
			pos += resource->size;
		}

		return true;
	}

	photoshop_resource* find_photoshop_resource(std::vector<photoshop_resource>& resources, const std::uint16_t id)
	{
		photoshop_resource* first = nullptr;

		for (auto& r : resources)
		{
			if (r.id != id) continue;
			if (!r.data.empty()) return &r;
			if (!first) first = &r;
		}

		return first;
	}

	bytes serialize_photoshop_resources(const std::vector<photoshop_resource>& resources)
	{
		bytes out;

		for (const auto& r : resources)
		{
			append(out, "8BIM");
			out.push_back(static_cast<std::uint8_t>(r.id >> 8));
			out.push_back(static_cast<std::uint8_t>(r.id));

			if (r.name.empty())
			{
				out.push_back(0);
				out.push_back(0);
			}
			else
			{
				out.insert(out.end(), r.name.begin(), r.name.end());
			}

			append_be32(out, static_cast<std::uint32_t>(r.data.size()));
			out.insert(out.end(), r.data.begin(), r.data.end());
			if (r.data.size() & 1u) out.push_back(0);
		}

		return out;
	}

	void apply_photo_legacy(const legacy_edits& e, const std::span<const std::uint8_t> existing_iptc, const bool allow_iptc,
	                        photo_legacy& out)
	{
		if (auto* const x = out.exif)
		{
			const auto ascii = [x](const directory d, const std::uint16_t tag, const std::optional<std::string>& value)
			{
				if (!value) return;
				if (value->empty()) x->remove(d, tag);
				else x->set_ascii(d, tag, *value);
			};

			ascii(directory::ifd0, tag_image_description, e.description);
			ascii(directory::ifd0, tag_copyright, e.copyright);
			if (e.creators) ascii(directory::ifd0, tag_artist, join(*e.creators, "; "));

			if (e.rating) x->set_short(directory::ifd0, tag_rating, static_cast<std::uint16_t>(std::clamp(*e.rating, 0, 5)));
			if (e.rating_percent)
				x->set_short(directory::ifd0, tag_rating_percent, static_cast<std::uint16_t>(std::clamp(*e.rating_percent, 0, 100)));

			if (e.keywords)
			{
				if (e.keywords->empty())
				{
					x->remove(directory::ifd0, tag_xp_keywords);
				}
				else
				{
					auto value = utf16le(join(*e.keywords, ";"));
					value.push_back(0);
					value.push_back(0);
					const auto count = static_cast<std::uint32_t>(value.size());
					x->set(directory::ifd0, tag_xp_keywords, tiff_editor::type_byte, count, std::move(value));
				}
			}

			if (e.orientation && *e.orientation >= 1 && *e.orientation <= 8)
			{
				x->set_short(directory::ifd0, tag_orientation, static_cast<std::uint16_t>(*e.orientation));
			}

			for (const auto& [value, tag] : {std::pair{&e.date_original, tag_date_time_original},
			                                 std::pair{&e.date_digitized, tag_date_time_digitized}})
			{
				if (!*value) continue;
				const auto d = parse_xmp_date(**value);
				if (!d) continue;
				x->set_ascii(directory::exif, tag, std::format("{:04}:{:02}:{:02} {:02}:{:02}:{:02}", d->year, d->month, d->day,
				                                                d->hour, d->minute, d->second));
			}

			for (const auto& [value, ref_tag, tag] : {
				     std::tuple{&e.gps_latitude, tag_gps_latitude_ref, tag_gps_latitude},
				     std::tuple{&e.gps_longitude, tag_gps_longitude_ref, tag_gps_longitude}
			     })
			{
				if (!*value) continue;
				const auto g = parse_xmp_gps(**value);
				if (!g) continue;

				x->set_ascii(directory::gps, ref_tag, std::string(1, g->reference));
				x->set_rationals(directory::gps, tag, g->rationals);
				if (!x->has(directory::gps, tag_gps_version)) x->set(directory::gps, tag_gps_version, tiff_editor::type_byte, 4, {2, 2, 0, 0});
			}
		}

		if (!allow_iptc || !touches_iptc(e)) return;

		auto datasets = parse_iptc(existing_iptc);

		// Everything written is UTF-8, so the block says so, and text already in it is carried over as
		// the reader reads it.
		for (auto& d : datasets)
		{
			if (is_binary_dataset(d)) continue;
			const auto text = libfiles::iptc_text(d.data);
			d.data.assign(text.begin(), text.end());
		}

		std::erase_if(datasets, [](const iptc_dataset& d) { return d.record == 1 && d.number == 90; });
		datasets.push_back({1, 90, {0x1B, 0x25, 0x47}});

		if (std::ranges::none_of(datasets, [](const iptc_dataset& d) { return d.record == 2 && d.number == 0; }))
		{
			datasets.push_back({2, 0, {0x00, 0x04}});
		}

		replace_one(datasets, iim_object_name, e.title);
		replace_one(datasets, iim_caption, e.description);
		replace_one(datasets, iim_copyright, e.copyright);
		replace_one(datasets, iim_city, e.city);
		replace_one(datasets, iim_state, e.state);
		replace_one(datasets, iim_country, e.country);
		replace_one(datasets, iim_credit, e.credit);
		replace_one(datasets, iim_source, e.source);
		if (e.creators) replace_all(datasets, iim_by_line, *e.creators);
		if (e.keywords) replace_all(datasets, iim_keywords, *e.keywords);
		if (e.date_original) write_iptc_date(datasets, iim_date_created, iim_time_created, *e.date_original);
		if (e.date_digitized) write_iptc_date(datasets, iim_digital_date, iim_digital_time, *e.date_digitized);

		// Record by record, and within a record by dataset, keeping the order of repeated datasets.
		std::ranges::stable_sort(datasets, [](const iptc_dataset& a, const iptc_dataset& b)
		{
			return a.record != b.record ? a.record < b.record : a.number < b.number;
		});

		out.iptc = serialize_iptc(datasets);
	}
}
