// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The one table of where each block keeps each property - EXIF's tags, IPTC's datasets and the
// XMP properties the Metadata Working Group and Adobe's media schema name - read into libfiles' properties,
// and the rule blocks are reconciled by: the XMP packet first, then what a file keeps outside it. Dates are
// read by the one reader's rule (parse_media_date) and coordinates by the writer's (parse_xmp_gps).

#include "libfiles/properties.h"

#include "date_internal.h"
#include "gps_internal.h"
#include "libfiles/identify.h"
#include "libfiles/media.h"
#include "libfiles/scan.h"
#include "text_internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

namespace libfiles
{
	namespace
	{
		namespace ns = xmp::ns;

		std::string trimmed(const std::string_view text)
		{
			return std::string(detail::trim_text(text));
		}

		std::string joined(const std::vector<std::string>& values)
		{
			std::string out;

			for (const auto& value : values)
			{
				const auto text = trimmed(value);
				if (text.empty()) continue;
				if (!out.empty()) out += "; ";
				out += text;
			}

			return out;
		}

		std::vector<std::string> nonempty(const std::vector<std::string>& values)
		{
			std::vector<std::string> out;

			for (const auto& value : values)
			{
				if (auto text = trimmed(value); !text.empty()) out.push_back(std::move(text));
			}

			return out;
		}

		// Windows' XPKeywords: keywords separated by semicolons.
		std::vector<std::string> split_keywords(std::string_view text)
		{
			std::vector<std::string> out;

			while (!text.empty())
			{
				const auto semicolon = text.find(';');
				if (auto keyword = trimmed(text.substr(0, semicolon)); !keyword.empty()) out.push_back(std::move(keyword));
				if (semicolon == std::string_view::npos) break;
				text.remove_prefix(semicolon + 1);
			}

			return out;
		}

		// A number as XMP writes one: an integer, a decimal, or a rational "n/d".
		std::optional<double> number(const std::string_view text)
		{
			const auto s = trimmed(text);
			const auto slash = s.find('/');

			const auto parse = [](const std::string_view part) -> std::optional<double>
			{
				auto value = 0.0;
				const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
				if (ec != std::errc{} || end != part.data() + part.size() || !std::isfinite(value)) return {};
				return value;
			};

			if (slash == std::string::npos) return parse(s);

			const auto numerator = parse(std::string_view(s).substr(0, slash));
			const auto denominator = parse(std::string_view(s).substr(slash + 1));
			if (!numerator || !denominator || *denominator == 0.0) return {};
			return *numerator / *denominator;
		}

		// "3", or "3/12" with the total, as a track or disc number is written.
		std::pair<int, int> numbered(const std::string_view text)
		{
			const auto s = trimmed(text);
			const auto slash = s.find('/');
			auto of = 0;
			auto total = 0;

			const auto part = std::string_view(s).substr(0, slash);
			std::from_chars(part.data(), part.data() + part.size(), of);

			if (slash != std::string::npos)
			{
				const auto rest = std::string_view(s).substr(slash + 1);
				std::from_chars(rest.data(), rest.data() + rest.size(), total);
			}

			return {of > 0 ? of : 0, total > 0 ? total : 0};
		}

		date exif_date_of(const exif_date& d)
		{
			if (d.text.empty()) return {};
			return detail::parse_media_date(d.text + d.offset);
		}

		// IPTC's CCYYMMDD, and its HHMMSS+HHMM when there is one.
		date iptc_date_of(const std::string& day, const std::string& time)
		{
			if (day.empty()) return {};
			return detail::parse_media_date(day + time);
		}

		// ---- XMP ---------------------------------------------------------------------------------------

		std::string simple(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			std::string value;
			if (!m.get_property(schema, name, &value)) return {};
			return trimmed(value);
		}

		// Whether a property is in the form its schema gives it. One in another - a simple value where the schema
		// has an array, as some writers put one, or a struct - reads as none, rather than as the toolkit's error.
		bool has_form(const xmp::meta& m, const std::string_view schema, const std::string_view name, bool (*const form)(xmp::option_bits))
		{
			xmp::option_bits options = 0;
			return m.get_property(schema, name, nullptr, &options) && form(options);
		}

		std::string localized(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			if (!has_form(m, schema, name, xmp::is_alt_text)) return {};

			// Alt-text whose items are not each simple with a language - as parsing leaves an alias given a struct,
			// as the toolkit leaves it - is refused by the toolkit's accessor, and reads as none too.
			try
			{
				std::string value;
				if (!m.get_localized_text(schema, name, "", "x-default", nullptr, &value)) return {};
				return trimmed(value);
			}
			catch (const xmp::error&)
			{
				return {};
			}
		}

		std::vector<std::string> items(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			std::vector<std::string> values;
			if (!has_form(m, schema, name, xmp::is_array)) return values;

			const auto count = m.count_array_items(schema, name);

			for (auto i = 1; i <= count; ++i)
			{
				std::string value;
				if (m.get_array_item(schema, name, i, &value)) values.push_back(std::move(value));
			}

			return nonempty(values);
		}

		date xmp_date(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			return detail::parse_media_date(simple(m, schema, name));
		}

		// The first of the properties that holds a date.
		date first_date(const xmp::meta& m, const std::initializer_list<std::pair<std::string_view, std::string_view>> names)
		{
			for (const auto& [schema, name] : names)
			{
				if (const auto d = xmp_date(m, schema, name); d.valid()) return d;
			}

			return {};
		}

		std::optional<double> xmp_number(const xmp::meta& m, const std::string_view schema, const std::string_view name)
		{
			const auto text = simple(m, schema, name);
			if (text.empty()) return {};
			return number(text);
		}

		// A count or a length, whole: none for a number below zero or past what an int holds.
		int whole(const std::optional<double> value)
		{
			constexpr auto past_int = static_cast<double>(std::numeric_limits<int>::max()) + 1.0;
			if (!value || !(*value >= 0.0 && *value < past_int)) return 0;
			return static_cast<int>(*value);
		}

		void take(std::string& into, const std::string& from)
		{
			if (into.empty()) into = from;
		}
	}

	properties properties_from_exif(const exif_metadata& exif)
	{
		properties p;
		p.title = exif.title;
		p.artist = exif.artist;
		p.copyright = exif.copyright;
		p.description = exif.description;
		if (!exif.comment.empty()) p.comments.push_back({exif.comment, {}});
		p.keywords = split_keywords(exif.keywords);
		if (exif.rating) p.rating = *exif.rating;
		p.encoder = exif.software;
		p.camera_make = exif.camera_make;
		p.camera_model = exif.camera_model;

		p.lens = exif.lens;
		p.f_number = exif.f_number;
		p.exposure_time = exif.exposure_time;
		p.focal_length = exif.focal_length;
		p.focal_length_35mm = exif.focal_length_35mm;
		p.iso_speed = exif.iso_speed;
		p.orientation = exif.orientation;

		p.created = exif_date_of(exif.original);
		p.digitized = exif_date_of(exif.digitized);
		p.modified = exif_date_of(exif.modified);
		p.year = p.created.year;

		if (exif.latitude && exif.longitude)
		{
			p.location = gps_position{*exif.latitude, *exif.longitude};

			// read_exif keeps no altitude apart from none, so sea level itself reads as no altitude.
			if (exif.altitude != 0.0f) p.altitude = exif.altitude;
		}

		return p;
	}

	properties properties_from_iptc(const iptc_fields& iptc)
	{
		properties p;
		p.title = trimmed(iptc.object_name);
		p.description = trimmed(iptc.caption);
		p.artist = joined(iptc.bylines);
		p.copyright = trimmed(iptc.copyright);
		p.keywords = nonempty(iptc.keywords);
		p.city = trimmed(iptc.city);
		p.state = trimmed(iptc.state);
		p.country = trimmed(iptc.country);
		p.credit = trimmed(iptc.credit);
		p.source = trimmed(iptc.source);
		p.created = iptc_date_of(iptc.date_created, iptc.time_created);
		p.digitized = iptc_date_of(iptc.digital_creation_date, iptc.digital_creation_time);
		p.year = p.created.year;
		return p;
	}

	properties properties_from_xmp(const xmp::meta& m)
	{
		properties p;
		p.title = localized(m, ns::dc, "title");
		p.description = localized(m, ns::dc, "description");
		p.copyright = localized(m, ns::dc, "rights");

		// A recording's artist, else the creators of a photo.
		p.artist = simple(m, ns::dm, "artist");
		if (p.artist.empty()) p.artist = joined(items(m, ns::dc, "creator"));

		p.album = simple(m, ns::dm, "album");
		p.album_artist = simple(m, ns::dm, "albumArtist");
		p.composer = simple(m, ns::dm, "composer");
		p.genre = simple(m, ns::dm, "genre");
		p.publisher = joined(items(m, ns::dc, "publisher"));
		p.synopsis = simple(m, ns::dm, "synopsis");
		p.show = simple(m, ns::dm, "show");
		p.encoder = simple(m, ns::xmp, "CreatorTool");
		p.camera_make = simple(m, ns::tiff, "Make");
		p.camera_model = simple(m, ns::tiff, "Model");
		p.keywords = items(m, ns::dc, "subject");

		auto comment = simple(m, ns::dm, "logComment");
		if (comment.empty()) comment = localized(m, ns::exif, "UserComment");
		if (!comment.empty()) p.comments.push_back({std::move(comment), {}});

		if (const auto rating = xmp_number(m, ns::xmp, "Rating"))
		{
			p.rating = static_cast<int>(std::lround(std::clamp(*rating, -1.0, 5.0)));
		}

		std::tie(p.track, p.track_count) = numbered(simple(m, ns::dm, "trackNumber"));
		std::tie(p.disc, p.disc_count) = numbered(simple(m, ns::dm, "discNumber"));
		p.season = numbered(simple(m, ns::dm, "season")).first;
		p.episode = numbered(simple(m, ns::dm, "episode")).first;

		// MWG's Date Created, which a recording keeps as its creation date; then Date Digitized and Date
		// Modified, each with its EXIF copy in the packet.
		p.created = first_date(m, {{ns::photoshop, "DateCreated"}, {ns::exif, "DateTimeOriginal"}, {ns::xmp, "CreateDate"}});
		p.digitized = first_date(m, {{ns::xmp, "CreateDate"}, {ns::exif, "DateTimeDigitized"}});
		p.modified = first_date(m, {{ns::xmp, "ModifyDate"}, {ns::tiff, "DateTime"}});
		p.original_release = xmp_date(m, ns::dm, "releaseDate");
		p.year = p.created.year;

		const auto latitude = detail::parse_xmp_gps(simple(m, ns::exif, "GPSLatitude"));
		const auto longitude = detail::parse_xmp_gps(simple(m, ns::exif, "GPSLongitude"));

		if (latitude && longitude)
		{
			const gps_position position{detail::gps_degrees(*latitude), detail::gps_degrees(*longitude)};

			// What a receiver with no fix writes, as read_exif refuses it.
			if (position.latitude != 0.0 || position.longitude != 0.0)
			{
				p.location = position;

				if (const auto altitude = xmp_number(m, ns::exif, "GPSAltitude"))
				{
					p.altitude = simple(m, ns::exif, "GPSAltitudeRef") == "1" ? -*altitude : *altitude;
				}
			}
		}

		p.lens = simple(m, ns::exif_ex, "LensModel");
		if (p.lens.empty()) p.lens = simple(m, ns::exif_aux, "Lens");
		p.f_number = xmp_number(m, ns::exif, "FNumber").value_or(0.0);
		p.exposure_time = xmp_number(m, ns::exif, "ExposureTime").value_or(0.0);
		p.focal_length = xmp_number(m, ns::exif, "FocalLength").value_or(0.0);
		p.focal_length_35mm = whole(xmp_number(m, ns::exif, "FocalLengthIn35mmFilm"));

		const auto iso = items(m, ns::exif, "ISOSpeedRatings");
		if (!iso.empty()) p.iso_speed = whole(number(iso.front()));

		if (const auto orientation = xmp_number(m, ns::tiff, "Orientation"); orientation && *orientation >= 1 && *orientation <= 8)
		{
			p.orientation = static_cast<std::uint16_t>(*orientation);
		}

		p.city = simple(m, ns::photoshop, "City");
		p.state = simple(m, ns::photoshop, "State");
		p.country = simple(m, ns::photoshop, "Country");
		p.credit = simple(m, ns::photoshop, "Credit");
		p.source = simple(m, ns::photoshop, "Source");
		return p;
	}

	void fill_missing(properties& into, const properties& from)
	{
		take(into.title, from.title);
		take(into.artist, from.artist);
		take(into.album, from.album);
		take(into.album_artist, from.album_artist);
		take(into.composer, from.composer);
		take(into.performer, from.performer);
		take(into.genre, from.genre);
		take(into.publisher, from.publisher);
		take(into.copyright, from.copyright);
		take(into.description, from.description);
		take(into.synopsis, from.synopsis);
		take(into.show, from.show);
		take(into.encoder, from.encoder);
		take(into.camera_make, from.camera_make);
		take(into.camera_model, from.camera_model);
		if (into.comments.empty()) into.comments = from.comments;
		if (into.keywords.empty()) into.keywords = from.keywords;
		if (!into.rating) into.rating = from.rating;

		const std::pair<int*, int> numbers[] = {
			{&into.year, from.year}, {&into.track, from.track}, {&into.track_count, from.track_count}, {&into.disc, from.disc},
			{&into.disc_count, from.disc_count}, {&into.season, from.season}, {&into.episode, from.episode},
			{&into.focal_length_35mm, from.focal_length_35mm}, {&into.iso_speed, from.iso_speed},
		};

		for (const auto& [mine, theirs] : numbers)
		{
			if (*mine == 0) *mine = theirs;
		}

		const std::pair<date*, const date*> dates[] = {
			{&into.created, &from.created}, {&into.original_release, &from.original_release}, {&into.digitized, &from.digitized},
			{&into.modified, &from.modified},
		};

		for (const auto& [mine, theirs] : dates)
		{
			if (!mine->valid()) *mine = *theirs;
		}

		if (!into.location)
		{
			into.location = from.location;
			if (!into.altitude) into.altitude = from.altitude;
		}

		take(into.lens, from.lens);
		if (into.f_number == 0.0) into.f_number = from.f_number;
		if (into.exposure_time == 0.0) into.exposure_time = from.exposure_time;
		if (into.focal_length == 0.0) into.focal_length = from.focal_length;
		if (!into.orientation) into.orientation = from.orientation;
		take(into.city, from.city);
		take(into.state, from.state);
		take(into.country, from.country);
		take(into.credit, from.credit);
		take(into.source, from.source);
	}

	properties read_properties(reader& r, const scan_limits& limits)
	{
		properties result;

		// Each block reconciled into what the blocks before it gave. One that cannot be read gives nothing, and
		// costs the others nothing.
		const auto add = [&result](const auto& read)
		{
			try
			{
				fill_missing(result, read());
			}
			catch (const std::exception&)
			{
			}
		};

		const auto packet = [](const std::vector<std::uint8_t>& bytes)
		{
			if (bytes.empty()) return properties{};
			xmp::meta m;
			m.parse({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
			return properties_from_xmp(m);
		};

		try
		{
			if (r.size() < 16) return result;

			const auto& type = info(identify(r));

			if (type.media != media_format::unknown)
			{
				const auto scanned = scan_media(r, scan_intent::index, limits);
				if (!scanned.error.empty()) return result;

				add([&] { return packet(scanned.xmp); });
				fill_missing(result, scanned.properties);
			}
			else if (can_scan(type.image))
			{
				const auto scanned = scan(r, scan_intent::index, limits);
				if (!scanned.error.empty()) return result;

				add([&] { return packet(scanned.metadata.xmp); });
				if (scanned.tiff_metadata) add([&] { return properties_from_exif(*scanned.tiff_metadata); });
				else if (!scanned.metadata.exif.empty()) add([&] { return properties_from_exif(read_exif(scanned.metadata.exif)); });
				if (!scanned.metadata.iptc.empty()) add([&] { return properties_from_iptc(read_iptc(scanned.metadata.iptc)); });

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
