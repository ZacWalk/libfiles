// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The round trip every writable format is held to. Every edit libfiles writes outside the packet is
// written to a file of the format, patched where it lies and rewritten whole, and the properties the
// format's own blocks then read - a photo's EXIF and IPTC, a recording's tags - must be what was written,
// for exactly the properties the format keeps. One set of edits and one check, run from each writer's tests
// on the files they build, so a format that reads a field back other than it writes it fails as that format.

#pragma once

#include "test.h"

#include "builders.h"
#include "libfiles/media.h"
#include "libfiles/properties.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"

#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace test_build
{
	inline libfiles::legacy_edits every_edit()
	{
		libfiles::legacy_edits e;
		e.title = "Trip title";
		e.description = "Trip description";
		e.copyright = "Trip copyright";
		e.creators = std::vector<std::string>{"First Creator", "Second Creator"};
		e.keywords = std::vector<std::string>{"alpha", "beta"};
		e.rating = 4;
		e.date_original = "2021-06-15T10:20:30";
		e.date_digitized = "2021-06-16T11:21:31";
		e.gps_latitude = "51,30.5N";
		e.gps_longitude = "0,7.5W";
		e.orientation = 6;
		e.city = "Trip city";
		e.state = "Trip state";
		e.country = "Trip country";
		e.credit = "Trip credit";
		e.source = "Trip source";
		e.artist = "Trip artist";
		e.album = "Trip album";
		e.album_artist = "Trip album artist";
		e.genre = "Trip genre";
		e.comment = "Trip comment";
		e.track = "3/12";
		e.disc = "1/2";
		e.year = 2019;
		return e;
	}

	struct round_trip_field
	{
		std::string_view name;
		// Whether properties read back from the file hold what every_edit wrote.
		std::function<bool(const libfiles::properties&)> holds;
	};

	inline bool same_moment(const libfiles::date& d, const int year, const int month, const int day, const int hour,
	                        const int minute, const int second)
	{
		return d.year == year && d.month == month && d.day == day && d.has_time && d.hour == hour && d.minute == minute &&
			d.second == second;
	}

	inline const std::vector<round_trip_field>& round_trip_fields()
	{
		using p = libfiles::properties;

		static const std::vector<round_trip_field> fields = {
			{"title", [](const p& x) { return x.title == "Trip title"; }},
			{"description", [](const p& x) { return x.description == "Trip description"; }},
			{"copyright", [](const p& x) { return x.copyright == "Trip copyright"; }},
			{"creators", [](const p& x) { return x.artist == "First Creator; Second Creator"; }},
			{"artist", [](const p& x) { return x.artist == "Trip artist"; }},
			{"keywords", [](const p& x) { return x.keywords == std::vector<std::string>{"alpha", "beta"}; }},
			{"rating", [](const p& x) { return x.rating == 4; }},
			{"taken", [](const p& x) { return same_moment(x.created, 2021, 6, 15, 10, 20, 30); }},
			{"digitized", [](const p& x) { return same_moment(x.digitized, 2021, 6, 16, 11, 21, 31); }},
			{"recorded", [](const p& x) { return same_moment(x.created, 2021, 6, 16, 11, 21, 31); }},
			// A year alone gives way to the date the edit gives in full.
			{"year", [](const p& x) { return x.year == 2021; }},
			{
				"location", [](const p& x)
				{
					return x.location && std::abs(x.location->latitude - (51.0 + 30.5 / 60)) < 1e-6 &&
						std::abs(x.location->longitude + 7.5 / 60) < 1e-6;
				}
			},
			{"orientation", [](const p& x) { return x.orientation == 6; }},
			{"city", [](const p& x) { return x.city == "Trip city"; }},
			{"state", [](const p& x) { return x.state == "Trip state"; }},
			{"country", [](const p& x) { return x.country == "Trip country"; }},
			{"credit", [](const p& x) { return x.credit == "Trip credit"; }},
			{"source", [](const p& x) { return x.source == "Trip source"; }},
			{"album", [](const p& x) { return x.album == "Trip album"; }},
			{"album artist", [](const p& x) { return x.album_artist == "Trip album artist"; }},
			{"genre", [](const p& x) { return x.genre == "Trip genre"; }},
			{
				"comment", [](const p& x)
				{
					return std::ranges::any_of(x.comments, [](const libfiles::media_text& c) { return c.text == "Trip comment"; });
				}
			},
			{"track", [](const p& x) { return x.track == 3 && x.track_count == 12; }},
			{"disc", [](const p& x) { return x.disc == 1 && x.disc_count == 2; }},
		};

		return fields;
	}

	// What a file keeps outside its packet: a still's EXIF - a TIFF's own directories - and IPTC, or a
	// recording's container tags.
	inline libfiles::properties native_properties(const bytes& file)
	{
		libfiles::reader r(file);
		const auto& type = libfiles::info(libfiles::identify(r));

		if (type.media != libfiles::media_format::unknown) return libfiles::scan_media(r).properties;

		const auto scanned = libfiles::scan(r);
		libfiles::properties p;
		if (scanned.tiff_metadata) libfiles::fill_missing(p, libfiles::properties_from_exif(*scanned.tiff_metadata));
		else if (!scanned.metadata.exif.empty()) libfiles::fill_missing(p, libfiles::properties_from_exif(libfiles::read_exif(scanned.metadata.exif)));
		if (!scanned.metadata.iptc.empty()) libfiles::fill_missing(p, libfiles::properties_from_iptc(libfiles::read_iptc(scanned.metadata.iptc)));
		return p;
	}

	struct round_trip
	{
		// The file each way of writing it gave: rewritten in one pass, and patched where it lies when the
		// plan allows it.
		bytes rewritten;
		std::optional<bytes> patched;
		std::string error;
	};

	inline round_trip write_every_edit(const bytes& original, const libfiles::xmp::meta& packet)
	{
		libfiles::metadata_update u{&packet};
		u.legacy = every_edit();

		libfiles::reader r(original);
		const auto plan = libfiles::plan_update(r, u);
		if (!plan.ok()) return {{}, {}, plan.error};

		round_trip out;
		libfiles::memory_stream rewritten;
		libfiles::write_updated(r, plan, rewritten);
		out.rewritten = rewritten.bytes();

		if (plan.patchable())
		{
			libfiles::memory_stream live(original);
			libfiles::write_in_place(live, plan);
			out.patched = live.bytes();
		}

		return out;
	}

	// The names of the fields a file's own blocks hold as every_edit wrote them.
	inline std::set<std::string_view> fields_held(const bytes& file)
	{
		const auto p = native_properties(file);
		std::set<std::string_view> held;

		for (const auto& f : round_trip_fields())
		{
			if (f.holds(p)) held.insert(f.name);
		}

		return held;
	}

	inline std::string describe(const std::set<std::string_view>& names)
	{
		std::string out;

		for (const auto name : names)
		{
			if (!out.empty()) out += ", ";
			out += name;
		}

		return out;
	}

	// Why a file does not scan whole, or nothing.
	inline std::string scan_fault(const bytes& file)
	{
		libfiles::reader r(file);
		const auto& type = libfiles::info(libfiles::identify(r));

		if (type.media != libfiles::media_format::unknown)
		{
			const auto scanned = libfiles::scan_media(r);
			return scanned.success ? std::string() : "media scan: " + scanned.error;
		}

		const auto scanned = libfiles::scan(r);
		return scanned.success ? std::string() : "scan: " + scanned.error;
	}

	// Writes every edit to the file, both ways, and checks that its own blocks hold exactly the fields named
	// - no fewer, and none read back as something else - and that the file's properties take the packet's
	// title over its own. The file must scan whole, before and after, or the trip proves nothing.
	inline void check_round_trip(const std::string_view label, const bytes& original, const std::set<std::string_view>& kept)
	{
		const auto named = [label](const std::string& text) { return std::string(label) + ": " + text; };

		CHECK_STR(named(scan_fault(original)), named(""));

		libfiles::xmp::meta packet;
		packet.set_localized_text(libfiles::xmp::ns::dc, "title", "", "x-default", "Packet title");

		const auto trip = write_every_edit(original, packet);
		CHECK_STR(named(trip.error), named(""));
		if (!trip.error.empty()) return;

		CHECK_STR(named(scan_fault(trip.rewritten)), named(""));
		CHECK_STR(named(describe(fields_held(trip.rewritten))), named(describe(kept)));
		if (trip.patched) CHECK(*trip.patched == trip.rewritten);

		libfiles::reader r(trip.rewritten);
		CHECK_STR(named(libfiles::read_properties(r).title), named("Packet title"));
	}
}
