// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The one table of where each block keeps each property - EXIF's tags, IPTC's datasets and the
// XMP properties the Metadata Working Group, Adobe's media schema and Google's photo sphere schema name - read
// into libfiles' properties with what each block states, and the rule blocks are reconciled by: each read over
// those before it. The table and the rule are Diffractor's, as are the readers of the values in them
// (photo_internal.h), so a library files its photos where it always has.

#include "libfiles/properties.h"

#include "libfiles/identify.h"
#include "libfiles/media.h"
#include "libfiles/scan.h"
#include "photo_internal.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace libfiles
{
	namespace
	{
		namespace ns = xmp::ns;

		// The last property, which the stated and joined masks hold a bit for: a property added after it moves it.
		constexpr auto last_property = property::panorama;
		static_assert(static_cast<unsigned>(last_property) < 64, "properties::stated holds a bit for each property");

		char lower(const char c)
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c | 0x20) : c;
		}

		// Text compared without regard to the case of its ASCII letters.
		bool ascii_less(const std::string_view a, const std::string_view b)
		{
			return std::ranges::lexicographical_compare(a, b, [](const char x, const char y)
			{
				return static_cast<unsigned char>(lower(x)) < static_cast<unsigned char>(lower(y));
			});
		}

		bool ascii_equal(const std::string_view a, const std::string_view b)
		{
			return std::ranges::equal(a, b, [](const char x, const char y) { return lower(x) == lower(y); });
		}

		std::string_view trimmed(std::string_view text)
		{
			constexpr std::string_view white_space{" \n\r\t\f\v\0", 7};
			const auto start = text.find_first_not_of(white_space);
			if (start == std::string_view::npos) return {};
			return text.substr(start, text.find_last_not_of(white_space) - start + 1);
		}

		// Text as a packet holds it, but for NULs at either end.
		std::string without_nuls(const std::string_view text)
		{
			const auto start = text.find_first_not_of('\0');
			if (start == std::string_view::npos) return {};
			return std::string(text.substr(start, text.find_last_not_of('\0') - start + 1));
		}

		// Names, each trimmed, as "; " separates them.
		std::vector<std::string> names_in(std::string_view text)
		{
			std::vector<std::string> names;

			while (true)
			{
				const auto semicolon = text.find(';');
				if (const auto name = trimmed(text.substr(0, semicolon)); !name.empty()) names.emplace_back(name);
				if (semicolon == std::string_view::npos) break;
				text.remove_prefix(semicolon + 1);
			}

			return names;
		}

		// Names joined as an artist holds them, each once whatever its case, the first spelling kept.
		std::string joined_names(const std::vector<std::string>& names)
		{
			std::vector<std::string_view> kept;

			for (const auto& name : names)
			{
				const auto seen = std::ranges::any_of(kept, [&name](const std::string_view k) { return ascii_equal(k, name); });
				if (!seen) kept.push_back(name);
			}

			std::string out;

			for (const auto name : kept)
			{
				if (!out.empty()) out += "; ";
				out += name;
			}

			return out;
		}

		// Keywords sorted, each once whatever its case.
		void sort_keywords(std::vector<std::string>& keywords)
		{
			std::ranges::sort(keywords, ascii_less);
			keywords.erase(std::ranges::unique(keywords, ascii_equal).begin(), keywords.end());
		}

		// A float as EXIF gives it, stated unless it is +0: -0 is a reading, of an altitude at a reference
		// below sea level.
		bool holds(const float value)
		{
			return std::bit_cast<std::uint32_t>(value) != 0;
		}

		// created, digitized, modified and year, from the dates: the first of each by the one ladder, the tags
		// that record a moment directly ahead of those an editor rewrites.
		void settle(properties& p)
		{
			const auto first = [&p](const std::initializer_list<date_source> ladder) -> const date*
			{
				for (const auto source : ladder)
				{
					for (const auto& d : p.dates)
					{
						if (d.source == source) return &d.when;
					}
				}

				return nullptr;
			};

			const auto* const original = first({
				date_source::exif_original, date_source::xmp_exif_original, date_source::iptc_created,
				date_source::xmp_photoshop_created
			});
			const auto* const digitized = first({
				date_source::exif_digitized, date_source::xmp_exif_digitized, date_source::iptc_digitized,
				date_source::xmp_create
			});
			const auto* const modified = first({date_source::exif_modified, date_source::xmp_modify});

			if (original) p.created = *original;
			else if (digitized) p.created = *digitized;
			if (digitized) p.digitized = *digitized;
			if (modified) p.modified = *modified;
			if (!p.states(property::year) && p.created.valid()) p.year = p.created.year;
		}

		void copy(properties& into, const properties& from, const property p)
		{
			switch (p)
			{
			case property::title: into.title = from.title; break;
			case property::artist: into.artist = from.artist; break;
			case property::album: into.album = from.album; break;
			case property::album_artist: into.album_artist = from.album_artist; break;
			case property::composer: into.composer = from.composer; break;
			case property::performer: into.performer = from.performer; break;
			case property::genre: into.genre = from.genre; break;
			case property::publisher: into.publisher = from.publisher; break;
			case property::copyright: into.copyright = from.copyright; break;
			case property::description: into.description = from.description; break;
			case property::synopsis: into.synopsis = from.synopsis; break;
			case property::show: into.show = from.show; break;
			case property::encoder: into.encoder = from.encoder; break;
			case property::camera_make: into.camera_make = from.camera_make; break;
			case property::camera_model: into.camera_model = from.camera_model; break;
			case property::comments: into.comments = from.comments; break;
			case property::keywords: into.keywords = from.keywords; break;
			case property::rating: into.rating = from.rating; break;
			case property::year: into.year = from.year; break;
			case property::track:
				into.track = from.track;
				into.track_count = from.track_count;
				break;
			case property::disc:
				into.disc = from.disc;
				into.disc_count = from.disc_count;
				break;
			case property::season: into.season = from.season; break;
			case property::episode:
				into.episode = from.episode;
				into.episode_count = from.episode_count;
				break;
			case property::media_kind: into.media_kind = from.media_kind; break;
			case property::original_release: into.original_release = from.original_release; break;
			case property::location: into.location = from.location; break;
			case property::altitude: into.altitude = from.altitude; break;
			case property::speed: into.speed = from.speed; break;
			case property::lens: into.lens = from.lens; break;
			case property::f_number: into.f_number = from.f_number; break;
			case property::exposure_time: into.exposure_time = from.exposure_time; break;
			case property::focal_length: into.focal_length = from.focal_length; break;
			case property::focal_length_35mm: into.focal_length_35mm = from.focal_length_35mm; break;
			case property::iso_speed: into.iso_speed = from.iso_speed; break;
			case property::orientation: into.orientation = from.orientation; break;
			case property::city: into.city = from.city; break;
			case property::state: into.state = from.state; break;
			case property::country: into.country = from.country; break;
			case property::credit: into.credit = from.credit; break;
			case property::source: into.source = from.source; break;
			case property::creators: into.creators = from.creators; break;
			case property::copyright_url: into.copyright_url = from.copyright_url; break;
			case property::label: into.label = from.label; break;
			case property::raw_file_name: into.raw_file_name = from.raw_file_name; break;
			case property::panorama: into.panorama = from.panorama; break;
			}
		}

		// What a block that joins a property adds to it: IPTC's keywords to those before them, sorted; and its
		// by-lines to the artist's names, after them.
		void join(properties& into, const properties& from, const property p)
		{
			if (p == property::keywords)
			{
				auto keywords = from.keywords;
				keywords.insert(keywords.end(), into.keywords.begin(), into.keywords.end());
				sort_keywords(keywords);
				into.keywords = std::move(keywords);
			}
			else if (p == property::artist)
			{
				auto names = names_in(into.artist);
				for (auto& name : names_in(from.artist)) names.push_back(std::move(name));
				into.artist = joined_names(names);
			}
			else
			{
				copy(into, from, p);
			}
		}

		// ---- EXIF and IPTC dates -------------------------------------------------------------------------

		void add_exif_date(properties& p, const exif_date& e, const date_source source)
		{
			if (e.text.empty()) return;

			auto d = detail::parse_photo_date(e.text);
			if (!d) return;

			d->nanoseconds = detail::sub_second_nanoseconds(e.sub_second);
			detail::read_zone(e.offset, *d);
			p.dates.push_back({source, *d});
		}

		// GPSDateStamp's day, CCYY:MM:DD, at the time GPSTimeStamp gives in UTC: the moment of the fix.
		void add_gps_stamp(properties& p, const exif_metadata& m)
		{
			if (m.gps_date.empty() || !m.gps_time) return;

			auto d = detail::parse_photo_date(m.gps_date + " 00:00:00");
			if (!d) return;

			const auto& time = *m.gps_time;
			const auto seconds = static_cast<std::uint64_t>(time[0]) * 3600 + static_cast<std::uint64_t>(time[1]) * 60 +
				static_cast<std::uint64_t>(time[2]);
			if (seconds >= 24 * 3600) return;

			d->has_time = true;
			d->hour = static_cast<int>(seconds / 3600);
			d->minute = static_cast<int>(seconds / 60 % 60);
			d->second = static_cast<int>(seconds % 60);
			d->zone = date::time_zone::utc;
			p.dates.push_back({date_source::exif_gps, *d});
		}

		// IPTC's CCYYMMDD, and its HHMMSS±HHMM when there is one: a day alone is midnight, as the standard means
		// it, and a time with no day says nothing.
		void add_iptc_date(properties& p, const std::string_view day, const std::string_view time, const date_source source)
		{
			if (day.size() != 8 || !detail::is_number(day)) return;

			auto text = std::string(day.substr(0, 4)) + ':' + std::string(day.substr(4, 2)) + ':' + std::string(day.substr(6, 2));
			std::string zone;
			const auto timed = time.size() >= 6 && detail::is_number(time.substr(0, 6));

			if (timed)
			{
				text += ' ' + std::string(time.substr(0, 2)) + ':' + std::string(time.substr(2, 2)) + ':' + std::string(time.substr(4, 2));

				if (time.size() == 11 && (time[6] == '+' || time[6] == '-') && detail::is_number(time.substr(7, 4)))
				{
					zone = time[6] + std::string(time.substr(7, 2)) + ':' + std::string(time.substr(9, 2));
				}
			}
			else
			{
				text += " 00:00:00";
			}

			auto d = detail::parse_photo_date(text);
			if (!d) return;

			d->has_time = timed;
			if (!zone.empty()) detail::read_zone(zone, *d);
			p.dates.push_back({source, *d});
		}

		// ---- XMP ---------------------------------------------------------------------------------------

		// A property's text, as the packet holds it: present even as an array or a struct, which hold none.
		std::optional<std::string> held(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			std::string value;
			if (!m.get_property(schema, name, &value)) return {};
			return value;
		}

		// Whether a property is in the form its schema gives it.
		bool has_form(const xmp::meta& m, const std::string_view schema, const std::string_view name, bool (*const form)(xmp::option_bits))
		{
			xmp::option_bits options = 0;
			return m.get_property(schema, name, nullptr, &options) && form(options);
		}

		// An alt-text's x-default item, else its first. Alt-text whose items are not each simple with a
		// language - as parsing leaves an alias given a struct - is refused by the toolkit, and reads as none.
		std::optional<std::string> localized(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			try
			{
				std::string value;
				if (!m.get_localized_text(schema, name, "", "x-default", nullptr, &value)) return {};
				return without_nuls(value);
			}
			catch (const xmp::error&)
			{
				return {};
			}
		}

		// An array's items that hold text, or nothing for one absent or not an array.
		std::optional<std::vector<std::string>> items(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			if (!has_form(m, schema, name, xmp::is_array)) return {};

			std::vector<std::string> values;
			const auto count = m.count_array_items(schema, name);

			for (auto i = 1; i <= count; ++i)
			{
				std::string value;
				if (m.get_array_item(schema, name, i, &value) && !value.empty()) values.push_back(std::move(value));
			}

			return values;
		}

		std::optional<double> rational(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			const auto text = held(m, schema, name);
			if (!text) return {};
			const auto r = detail::parse_photo_rational(*text);
			if (!r) return {};
			return static_cast<double>(r->first) / static_cast<double>(r->second);
		}

		// What Google's photo sphere schema has the file declare, never what its shape suggests: the projection
		// it names, or a panorama in none for a writer that names none but sets a property only one writes.
		panorama_projection panorama_of(const xmp::meta& m)
		{
			if (const auto projection = held(m, ns::gpano, "ProjectionType"))
			{
				const auto text = trimmed(*projection);
				if (ascii_equal(text, "equirectangular")) return panorama_projection::equirectangular;
				if (ascii_equal(text, "cylindrical")) return panorama_projection::cylindrical;
				if (!text.empty()) return panorama_projection::unspecified;
			}

			for (const auto declares : {"UsePanoramaViewer", "FullPanoWidthPixels", "CroppedAreaImageWidthPixels"})
			{
				if (m.exists(ns::gpano, declares)) return panorama_projection::unspecified;
			}

			return panorama_projection::none;
		}
	}

	properties properties_from_exif(const exif_metadata& m)
	{
		properties p;

		const auto text = [&p](std::string& field, const std::string& value, const property id)
		{
			if (value.empty()) return;
			field = value;
			p.mark_stated(id);
		};

		const auto number = [&p](double& field, const float value, const property id)
		{
			if (!holds(value)) return;
			field = value;
			p.mark_stated(id);
		};

		if (m.orientation)
		{
			p.orientation = m.orientation;
			p.mark_stated(property::orientation);
		}

		text(p.camera_make, m.camera_make, property::camera_make);
		text(p.camera_model, m.camera_model, property::camera_model);
		text(p.lens, m.lens, property::lens);
		text(p.encoder, m.software, property::encoder);
		text(p.artist, m.artist, property::artist);
		text(p.copyright, m.copyright, property::copyright);
		text(p.description, m.description, property::description);
		text(p.title, m.title, property::title);

		if (!m.comment.empty())
		{
			p.comments.push_back({m.comment, {}});
			p.mark_stated(property::comments);
		}

		// Windows' XPKeywords: keywords separated by semicolons.
		if (auto keywords = names_in(m.keywords); !keywords.empty())
		{
			p.keywords = std::move(keywords);
			p.mark_stated(property::keywords);
		}

		number(p.f_number, m.f_number, property::f_number);
		number(p.exposure_time, m.exposure_time, property::exposure_time);
		number(p.focal_length, m.focal_length, property::focal_length);
		number(p.speed, m.speed, property::speed);

		if (m.focal_length_35mm != 0)
		{
			p.focal_length_35mm = m.focal_length_35mm;
			p.mark_stated(property::focal_length_35mm);
		}

		if (m.iso_speed != 0)
		{
			p.iso_speed = m.iso_speed;
			p.mark_stated(property::iso_speed);
		}

		if (m.rating)
		{
			p.rating = *m.rating;
			p.mark_stated(property::rating);
		}

		if (m.latitude && m.longitude)
		{
			p.location = gps_position{*m.latitude, *m.longitude};
			p.mark_stated(property::location);
		}

		if (holds(m.altitude))
		{
			p.altitude = m.altitude;
			p.mark_stated(property::altitude);
		}

		add_exif_date(p, m.original, date_source::exif_original);
		add_exif_date(p, m.digitized, date_source::exif_digitized);
		add_exif_date(p, m.modified, date_source::exif_modified);
		add_gps_stamp(p, m);
		settle(p);
		return p;
	}

	properties properties_from_iptc(const iptc_fields& f)
	{
		properties p;

		const auto text = [&p](std::string& field, const std::string& value, const property id)
		{
			if (value.empty()) return;
			field = value;
			p.mark_stated(id);
		};

		text(p.description, f.caption, property::description);
		text(p.title, f.object_name, property::title);
		text(p.city, f.city, property::city);
		text(p.state, f.state, property::state);
		text(p.country, f.country, property::country);
		text(p.credit, f.credit, property::credit);
		text(p.source, f.source, property::source);
		text(p.copyright, f.copyright, property::copyright);

		add_iptc_date(p, f.date_created, f.time_created, date_source::iptc_created);
		add_iptc_date(p, f.digital_creation_date, f.digital_creation_time, date_source::iptc_digitized);

		// Sorted and each once, as the join leaves them.
		for (const auto& keyword : f.keywords)
		{
			if (!keyword.empty()) p.keywords.push_back(keyword);
		}

		if (!p.keywords.empty())
		{
			sort_keywords(p.keywords);
			p.mark_joined(property::keywords);
		}

		std::vector<std::string> bylines;

		for (const auto& byline : f.bylines)
		{
			if (const auto name = trimmed(byline); !name.empty()) bylines.emplace_back(name);
		}

		if (!bylines.empty())
		{
			p.artist = joined_names(bylines);
			p.mark_joined(property::artist);
		}

		settle(p);
		return p;
	}

	properties properties_from_xmp(const xmp::meta& m)
	{
		properties p;

		const auto text = [&p, &m](std::string& field, const std::string_view schema, const std::string_view name, const property id)
		{
			const auto value = held(m, schema, name);
			if (!value) return;
			field = without_nuls(*value);
			p.mark_stated(id);
		};

		const auto alt_text = [&p, &m](std::string& field, const std::string_view schema, const std::string_view name, const property id)
		{
			auto value = localized(m, schema, name);
			if (!value) return;
			field = std::move(*value);
			p.mark_stated(id);
		};

		// A date with the zone its text ends in; the text of one that holds no date, for xmp:CreateDate's year.
		const auto date_of = [&p, &m](const std::string_view schema, const std::string_view name, const date_source source)
		{
			auto value = held(m, schema, name);
			if (!value) return std::optional<std::string>{};

			if (auto d = detail::parse_photo_date(*value))
			{
				detail::read_zone(*value, *d);
				p.dates.push_back({source, *d});
				return std::optional<std::string>{};
			}

			return value;
		};

		date_of(ns::photoshop, "DateCreated", date_source::xmp_photoshop_created);

		if (const auto year = date_of(ns::xmp, "CreateDate", date_source::xmp_create); year && detail::is_number(*year))
		{
			p.year = detail::leading_int(*year);
			p.mark_stated(property::year);
		}

		date_of(ns::xmp, "ModifyDate", date_source::xmp_modify);
		date_of(ns::exif, "DateTimeDigitized", date_source::xmp_exif_digitized);
		date_of(ns::exif, "DateTimeOriginal", date_source::xmp_exif_original);

		// A coordinate as a pair, or not at all: one half alone would pin the photo to a place neither block
		// states.
		const auto latitude = held(m, ns::exif, "GPSLatitude");
		const auto longitude = held(m, ns::exif, "GPSLongitude");
		const auto lat = latitude ? detail::parse_photo_coordinate(*latitude, true) : std::nullopt;
		const auto lon = longitude ? detail::parse_photo_coordinate(*longitude, false) : std::nullopt;

		if (lat && lon)
		{
			p.location = gps_position{*lat, *lon};
			p.mark_stated(property::location);
		}

		text(p.city, ns::photoshop, "City", property::city);
		text(p.state, ns::photoshop, "State", property::state);
		text(p.country, ns::photoshop, "Country", property::country);
		text(p.credit, ns::photoshop, "Credit", property::credit);
		text(p.copyright_url, ns::xmp_rights, "WebStatement", property::copyright_url);
		text(p.source, ns::photoshop, "Source", property::source);

		if (auto creators = items(m, ns::dc, "creator"); creators && !creators->empty())
		{
			p.creators = std::move(*creators);
			p.mark_stated(property::creators);
		}

		alt_text(p.copyright, ns::dc, "rights", property::copyright);
		alt_text(p.title, ns::dc, "title", property::title);

		// exif:UserComment, or dm:logComment over it.
		std::optional<std::string> comment = localized(m, ns::exif, "UserComment");
		if (const auto log = held(m, ns::dm, "logComment")) comment = without_nuls(*log);

		if (comment)
		{
			p.comments.push_back({std::move(*comment), {}});
			p.mark_stated(property::comments);
		}

		text(p.synopsis, ns::dm, "synopsis", property::synopsis);
		alt_text(p.description, ns::dc, "description", property::description);

		// xmp:Rating, -1 to 5; else Windows' MicrosoftPhoto:Rating, a percent read in fifths rounded up.
		if (const auto rating = held(m, ns::xmp, "Rating"))
		{
			p.rating = std::clamp(detail::leading_int(*rating), -1, 5);
			p.mark_stated(property::rating);
		}
		else if (const auto percent = held(m, ns::microsoft_photo, "Rating"))
		{
			const auto value = detail::leading_int(*percent);
			p.rating = std::clamp(value % 20 ? value / 20 + 1 : value / 20, -1, 5);
			p.mark_stated(property::rating);
		}

		text(p.label, ns::xmp, "Label", property::label);

		// An empty dc:subject is the file stating it has no keywords, as a writer that clears them leaves it.
		if (auto keywords = items(m, ns::dc, "subject"))
		{
			p.keywords = std::move(*keywords);
			p.mark_stated(property::keywords);
		}

		text(p.camera_make, ns::tiff, "Make", property::camera_make);
		text(p.camera_model, ns::tiff, "Model", property::camera_model);
		text(p.album, ns::dm, "album", property::album);
		text(p.album_artist, ns::dm, "albumArtist", property::album_artist);
		text(p.artist, ns::dm, "artist", property::artist);
		text(p.genre, ns::dm, "genre", property::genre);
		text(p.show, ns::dm, "show", property::show);

		const auto numbered = [&p, &m](int& of, int& total, const std::string_view name, const property id)
		{
			const auto value = held(m, ns::dm, name);
			if (!value) return;
			const auto [n, count] = detail::parse_number_pair(*value);
			of = std::max(n, 0);
			total = std::max(count, 0);
			p.mark_stated(id);
		};

		numbered(p.track, p.track_count, "trackNumber", property::track);
		numbered(p.disc, p.disc_count, "discNumber", property::disc);
		numbered(p.episode, p.episode_count, "episode", property::episode);

		if (const auto season = held(m, ns::dm, "season"))
		{
			p.season = detail::leading_int(*season);
			p.mark_stated(property::season);
		}

		// Whole millimetres, rounded half away from zero; none below zero, as one past an int's range rounds to.
		if (const auto focal35 = rational(m, ns::exif, "FocalLengthIn35mmFilm"))
		{
			p.focal_length_35mm = std::max(detail::round_to_int(*focal35), 0);
			p.mark_stated(property::focal_length_35mm);
		}

		const auto real = [&p, &m](double& field, const std::string_view name, const property id)
		{
			const auto value = rational(m, ns::exif, name);
			if (!value) return;
			field = *value;
			p.mark_stated(id);
		};

		real(p.f_number, "FNumber", property::f_number);
		real(p.focal_length, "FocalLength", property::focal_length);
		real(p.exposure_time, "ExposureTime", property::exposure_time);

		if (const auto orientation = held(m, ns::tiff, "Orientation"))
		{
			if (const auto o = detail::leading_int(*orientation); o >= 1 && o <= 8)
			{
				p.orientation = static_cast<std::uint16_t>(o);
				p.mark_stated(property::orientation);
			}
		}

		text(p.lens, ns::exif_aux, "Lens", property::lens);
		text(p.raw_file_name, ns::camera_raw, "RawFileName", property::raw_file_name);

		if (const auto panorama = panorama_of(m); panorama != panorama_projection::none)
		{
			p.panorama = panorama;
			p.mark_stated(property::panorama);
		}

		settle(p);
		return p;
	}

	properties properties_from_xmp(const std::span<const std::uint8_t> packet)
	{
		try
		{
			if (packet.empty()) return {};

			// A JPEG APP1 segment's signature, which ends in the NUL that ends the segment's header.
			constexpr std::string_view signature{"http://ns.adobe.com/xap/1.0/\0", 29};
			std::string_view text(reinterpret_cast<const char*>(packet.data()), packet.size());
			if (text.size() > signature.size() && text.starts_with(signature)) text.remove_prefix(signature.size());

			xmp::meta m;
			m.parse(text);
			return properties_from_xmp(m);
		}
		catch (const std::exception&)
		{
			return {};
		}
	}

	void overlay(properties& into, const properties& from)
	{
		for (auto i = 0u; i <= static_cast<unsigned>(last_property); ++i)
		{
			const auto p = static_cast<property>(i);
			if (!from.states(p)) continue;

			if (from.joins(p)) join(into, from, p);
			else copy(into, from, p);

			into.mark_stated(p);
		}

		into.dates.insert(into.dates.end(), from.dates.begin(), from.dates.end());
		settle(into);
	}

	properties read_properties(reader& r, const scan_limits& limits)
	{
		properties result;

		// Each block read over what the blocks before it gave. One that cannot be read gives nothing, and costs
		// the others nothing.
		const auto add = [&result](const auto& read)
		{
			try
			{
				overlay(result, read());
			}
			catch (const std::exception&)
			{
			}
		};

		try
		{
			if (r.size() < 16) return result;

			const auto& type = info(identify(r));

			if (type.media != media_format::unknown)
			{
				const auto scanned = scan_media(r, scan_intent::index, limits);
				if (!scanned.error.empty()) return result;

				// A recording's own year stands against the dates its packet gives.
				result = scanned.properties;
				if (result.year != 0) result.mark_stated(property::year);
				add([&] { return properties_from_xmp(scanned.xmp); });
			}
			else if (can_scan(type.image))
			{
				const auto scanned = scan(r, scan_intent::index, limits);
				if (!scanned.error.empty()) return result;

				if (scanned.tiff_metadata) add([&] { return properties_from_exif(*scanned.tiff_metadata); });
				else if (!scanned.metadata.exif.empty()) add([&] { return properties_from_exif(read_exif(scanned.metadata.exif)); });
				if (!scanned.metadata.iptc.empty()) add([&] { return properties_from_iptc(read_iptc(scanned.metadata.iptc)); });
				add([&] { return properties_from_xmp(scanned.metadata.xmp); });

				// A JPEG XL codestream's own orientation, where no block states one.
				if (!result.orientation) result.orientation = scanned.orientation;
			}
		}
		catch (const std::exception&)
		{
			// A stream that fails before the scan is done.
			return {};
		}

		return result;
	}
}
