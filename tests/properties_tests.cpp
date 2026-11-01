// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for the properties model: each block - EXIF, IPTC, XMP - read into the same properties by
// its table with what it states, the values in them read as Diffractor read them, the rule blocks are
// reconciled by, and a file's properties read whole.

#include "test.h"

#include "builders.h"
#include "libfiles/media.h"
#include "libfiles/properties.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"

#include <cmath>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	namespace ns = libfiles::xmp::ns;
	using libfiles::date_source;
	using libfiles::property;

	libfiles::date day(const int year, const int month, const int d)
	{
		libfiles::date out;
		out.year = year;
		out.month = month;
		out.day = d;
		return out;
	}

	libfiles::date at(libfiles::date d, const int hour, const int minute, const int second)
	{
		d.has_time = true;
		d.hour = hour;
		d.minute = minute;
		d.second = second;
		return d;
	}

	libfiles::date zoned(libfiles::date d, const int offset_minutes)
	{
		d.zone = libfiles::date::time_zone::offset;
		d.offset_minutes = offset_minutes;
		return d;
	}

	libfiles::date utc(libfiles::date d)
	{
		d.zone = libfiles::date::time_zone::utc;
		return d;
	}

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-9;
	}

	// The date EXIF's DateTimeOriginal gives for the text, if any.
	std::optional<libfiles::date> exif_date(const std::string& text)
	{
		libfiles::exif_metadata e;
		e.original.text = text;
		const auto p = libfiles::properties_from_exif(e);
		if (p.dates.empty()) return {};
		return p.dates.front().when;
	}

	// The sources of the dates, in the order read.
	std::vector<date_source> sources(const libfiles::properties& p)
	{
		std::vector<date_source> out;
		for (const auto& d : p.dates) out.push_back(d.source);
		return out;
	}

	// A property of every kind set, each to a value no other has, and stated.
	libfiles::properties every_property()
	{
		libfiles::properties p;
		p.title = "title";
		p.artist = "artist";
		p.album = "album";
		p.album_artist = "album artist";
		p.composer = "composer";
		p.performer = "performer";
		p.genre = "genre";
		p.publisher = "publisher";
		p.copyright = "copyright";
		p.description = "description";
		p.synopsis = "synopsis";
		p.show = "show";
		p.encoder = "encoder";
		p.camera_make = "make";
		p.camera_model = "model";
		p.comments = {{"comment", "eng"}};
		p.keywords = {"one", "two"};
		p.rating = 4;
		p.year = 2001;
		p.track = 2;
		p.track_count = 3;
		p.disc = 4;
		p.disc_count = 5;
		p.season = 6;
		p.episode = 7;
		p.episode_count = 8;
		p.media_kind = 9;
		p.original_release = day(1999, 1, 1);
		p.location = libfiles::gps_position{1.5, -2.5};
		p.altitude = 30.0;
		p.speed = 12.5;
		p.lens = "lens";
		p.f_number = 2.8;
		p.exposure_time = 0.004;
		p.focal_length = 50.0;
		p.focal_length_35mm = 75;
		p.iso_speed = 200;
		p.orientation = 6;
		p.city = "city";
		p.state = "state";
		p.country = "country";
		p.credit = "credit";
		p.source = "source";
		p.creators = {"creator"};
		p.copyright_url = "url";
		p.label = "label";
		p.raw_file_name = "raw";
		p.panorama = libfiles::panorama_projection::cylindrical;
		p.dates = {{date_source::exif_original, at(day(2001, 2, 3), 4, 5, 6)}};
		p.created = at(day(2001, 2, 3), 4, 5, 6);

		for (auto i = 0u; i <= static_cast<unsigned>(property::panorama); ++i) p.mark_stated(static_cast<property>(i));
		return p;
	}
}

TEST("EXIF reads into properties by the table, with what it states")
{
	libfiles::exif_metadata e;
	e.title = "XP title";
	e.artist = "Ann Author";
	e.copyright = "(c) Ann";
	e.description = "A description";
	e.comment = "A comment";
	e.keywords = "alpha; New York;;gamma ";
	e.rating = 3;
	e.software = "Editor 2";
	e.camera_make = "Make";
	e.camera_model = "Model";
	e.lens = "50mm";
	e.f_number = 2.0f;
	e.exposure_time = 0.5f;
	e.focal_length = 50.0f;
	e.focal_length_35mm = 75;
	e.iso_speed = 400;
	e.orientation = 8;
	e.original = {"2020:05:06 07:08:09", "+02:00", "12"};
	e.digitized = {"2020:05:06 07:08:10", "", ""};
	e.modified = {"2021:01:02 03:04:05", "", ""};
	e.latitude = 51.5;
	e.longitude = -0.125;
	e.altitude = -12.0f;
	e.speed = 30.0f;
	e.gps_date = "2020:05:06";
	e.gps_time = std::array<double, 3>{5.0, 7.0, 9.5};

	const auto p = libfiles::properties_from_exif(e);

	CHECK_STR(p.title, "XP title");
	CHECK_STR(p.artist, "Ann Author");
	CHECK_STR(p.copyright, "(c) Ann");
	CHECK_STR(p.description, "A description");
	CHECK(p.comments == (std::vector<libfiles::media_text>{{"A comment", ""}}));
	// XPKeywords holds keywords separated by semicolons, each kept whole.
	CHECK(p.keywords == (std::vector<std::string>{"alpha", "New York", "gamma"}));
	CHECK(p.rating == 3);
	CHECK_STR(p.encoder, "Editor 2");
	CHECK_STR(p.camera_make, "Make");
	CHECK_STR(p.camera_model, "Model");
	CHECK_STR(p.lens, "50mm");
	CHECK(near(p.f_number, 2.0) && near(p.exposure_time, 0.5) && near(p.focal_length, 50.0));
	CHECK_EQ(p.focal_length_35mm, 75);
	CHECK_EQ(p.iso_speed, 400);
	CHECK(p.orientation == 8);
	CHECK(p.location == (libfiles::gps_position{51.5, -0.125}));
	CHECK(p.altitude == -12.0);
	CHECK(near(p.speed, 30.0));

	// Each date with its tag, the fraction of a second SubSecTime gives and the zone OffsetTime does; and the
	// GPS stamp, in UTC, to the whole second.
	auto original = zoned(at(day(2020, 5, 6), 7, 8, 9), 120);
	original.nanoseconds = 120'000'000;
	CHECK(sources(p) == (std::vector{date_source::exif_original, date_source::exif_digitized, date_source::exif_modified,
		                     date_source::exif_gps}));
	CHECK(p.dates[0].when == original);
	CHECK(p.dates[3].when == utc(at(day(2020, 5, 6), 5, 7, 9)));
	CHECK(p.created == original);
	CHECK(p.digitized == at(day(2020, 5, 6), 7, 8, 10));
	CHECK(p.modified == at(day(2021, 1, 2), 3, 4, 5));
	CHECK_EQ(p.year, 2020);

	for (const auto stated : {property::title, property::keywords, property::rating, property::location, property::altitude,
	                          property::speed, property::orientation})
	{
		CHECK(p.states(stated));
	}

	CHECK(!p.states(property::year) && !p.states(property::city) && !p.joins(property::keywords));

	// An altitude is a reading whether or not a position is beside it; a position is never one EXIF does
	// not state.
	libfiles::exif_metadata no_fix;
	no_fix.altitude = 100.0f;
	CHECK(!libfiles::properties_from_exif(no_fix).location);
	CHECK(libfiles::properties_from_exif(no_fix).altitude == 100.0);
	CHECK(libfiles::properties_from_exif(libfiles::exif_metadata{}) == libfiles::properties{});
}

TEST("A photo's date reads in the forms cameras and editors write, and a clock never set reads as none")
{
	CHECK(exif_date("2006:01:14 15:51:31") == at(day(2006, 1, 14), 15, 51, 31));
	CHECK(exif_date("2006-01-14 15:51:31") == at(day(2006, 1, 14), 15, 51, 31));
	CHECK(exif_date("2006-01-14T15:51:31") == at(day(2006, 1, 14), 15, 51, 31));
	CHECK(exif_date("20060114T155131") == at(day(2006, 1, 14), 15, 51, 31));
	CHECK(exif_date("20060114155131") == at(day(2006, 1, 14), 15, 51, 31));
	CHECK(exif_date("2006-01-14") == day(2006, 1, 14));
	CHECK(exif_date("2006-jan-14") == day(2006, 1, 14));
	CHECK(exif_date("THU OCT 26 16:46:04 2006") == at(day(2006, 10, 26), 16, 46, 4));
	CHECK(exif_date("Mon Mar 3 09:44:56 2008") == at(day(2008, 3, 3), 9, 44, 56));

	// Seconds are a real number, rounded down unless within a tenth of the next.
	CHECK(exif_date("2006:01:14 15:51:31.5") == at(day(2006, 1, 14), 15, 51, 31));
	CHECK(exif_date("2006:01:14 15:51:31.95") == at(day(2006, 1, 14), 15, 51, 32));

	// A month or a day of 0 is none, and the date stands as the calendar takes it.
	CHECK(exif_date("2006:00:00 10:00:00") == at(day(2006, 0, 0), 10, 0, 0));

	CHECK(!exif_date("2011"));
	CHECK(!exif_date("0000:00:00 00:00:00"));
	CHECK(!exif_date("2006:02:30 10:00:00"));
	CHECK(!exif_date("2006:13:01 10:00:00"));
	CHECK(!exif_date("2006:01:14 24:00:00"));
	CHECK(!exif_date("2006:01:14 15:51:59.95"));
	CHECK(!exif_date("1799:12:31 23:59:59"));
	CHECK(!exif_date("1970:01:01 12:00:00"));
	CHECK(!exif_date("1904:01:01 00:00:00"));
	CHECK(!exif_date("THU XYZ 26 16:46:04 2006"));
	CHECK(exif_date("1970:01:02 00:00:00") == at(day(1970, 1, 2), 0, 0, 0));
}

TEST("IPTC reads into properties by the table, joining its keywords and by-lines")
{
	libfiles::iptc_fields i;
	i.object_name = " Object ";
	i.caption = "Caption\nsecond line";
	i.bylines = {"One", " ", "Two", "one"};
	i.copyright = "IPTC copyright";
	i.keywords = {"k2", "", "k1"};
	i.city = "Cardiff";
	i.state = "Wales";
	i.country = "UK";
	i.credit = "Agency";
	i.source = "Archive";
	i.date_created = "20190807";
	i.time_created = "101112+0130";
	i.digital_creation_date = "20190808";

	const auto p = libfiles::properties_from_iptc(i);

	// Text as the dataset holds it; keywords sorted and each once; by-lines each once, whole, in their order.
	CHECK_STR(p.title, " Object ");
	CHECK_STR(p.description, "Caption\nsecond line");
	CHECK_STR(p.artist, "One; Two");
	CHECK_STR(p.copyright, "IPTC copyright");
	CHECK(p.keywords == (std::vector<std::string>{"k1", "k2"}));
	CHECK_STR(p.city, "Cardiff");
	CHECK_STR(p.state, "Wales");
	CHECK_STR(p.country, "UK");
	CHECK_STR(p.credit, "Agency");
	CHECK_STR(p.source, "Archive");
	CHECK(sources(p) == (std::vector{date_source::iptc_created, date_source::iptc_digitized}));
	CHECK(p.created == zoned(at(day(2019, 8, 7), 10, 11, 12), 90));
	CHECK(p.digitized == day(2019, 8, 8));
	CHECK_EQ(p.year, 2019);
	CHECK(p.joins(property::keywords) && p.joins(property::artist) && p.states(property::keywords));
	CHECK(!p.joins(property::title) && p.states(property::title));

	// A time with no day says nothing, and a day of other than eight digits is none.
	libfiles::iptc_fields bad;
	bad.time_created = "101112";
	bad.date_created = "2019087";
	CHECK(libfiles::properties_from_iptc(bad).dates.empty());
}

TEST("XMP reads into properties by the table, a property present stating it even empty")
{
	libfiles::xmp::meta m;
	m.set_localized_text(ns::dc, "title", "", "x-default", "Packet title");
	m.set_localized_text(ns::dc, "description", "", "x-default", "Packet description");
	m.set_localized_text(ns::dc, "rights", "", "x-default", "Packet rights");
	m.append_array_item(ns::dc, "creator", libfiles::xmp::array_is_ordered, "First");
	m.append_array_item(ns::dc, "creator", libfiles::xmp::array_is_ordered, "Second Person");
	m.append_array_item(ns::dc, "subject", libfiles::xmp::value_is_array, "red");
	m.append_array_item(ns::dc, "subject", libfiles::xmp::value_is_array, "green");
	m.set_property(ns::dm, "album", "Album");
	m.set_property(ns::dm, "albumArtist", "Band");
	m.set_property(ns::dm, "genre", "Jazz");
	m.set_property(ns::dm, "show", "Show");
	m.set_property(ns::dm, "synopsis", "Synopsis");
	m.set_property(ns::dm, "logComment", "Logged");
	m.set_localized_text(ns::exif, "UserComment", "", "x-default", "Under the log");
	m.set_property(ns::dm, "trackNumber", "3/12");
	m.set_property(ns::dm, "discNumber", "1");
	m.set_property(ns::dm, "episode", "4/10");
	m.set_property(ns::dm, "season", "2");
	m.set_property(ns::xmp, "Rating", "4.0");
	m.set_property(ns::xmp, "Label", "Red");
	m.set_property(ns::xmp, "CreateDate", "2018-03-04T05:06:07Z");
	m.set_property(ns::xmp, "ModifyDate", "2018-03-05");
	m.set_property(ns::photoshop, "DateCreated", "2018-03-03T01:02:03.250+01:00");
	m.set_property(ns::photoshop, "City", " Paris ");
	m.set_property(ns::photoshop, "State", "IDF");
	m.set_property(ns::photoshop, "Country", "France");
	m.set_property(ns::photoshop, "Credit", "Credit");
	m.set_property(ns::photoshop, "Source", "Source");
	m.set_property(ns::xmp_rights, "WebStatement", "https://example.com/rights");
	m.set_property(ns::tiff, "Make", "Make");
	m.set_property(ns::tiff, "Model", "Model");
	m.set_property(ns::tiff, "Orientation", "6");
	m.set_property(ns::exif, "GPSLatitude", "51,30.5N");
	m.set_property(ns::exif, "GPSLongitude", "0,7,30w");
	m.set_property(ns::exif, "FNumber", "28/10");
	m.set_property(ns::exif, "ExposureTime", "1/250");
	m.set_property(ns::exif, "FocalLength", "50");
	m.set_property(ns::exif, "FocalLengthIn35mmFilm", "75");
	m.set_property(ns::exif_aux, "Lens", "Lens");
	m.set_property(ns::camera_raw, "RawFileName", "IMG_1.CR2");
	m.set_property(ns::gpano, "ProjectionType", " Equirectangular ");

	const auto p = libfiles::properties_from_xmp(m);

	CHECK_STR(p.title, "Packet title");
	CHECK_STR(p.description, "Packet description");
	CHECK_STR(p.copyright, "Packet rights");
	CHECK(p.creators == (std::vector<std::string>{"First", "Second Person"}));
	CHECK(p.artist.empty() && !p.states(property::artist));
	CHECK(p.keywords == (std::vector<std::string>{"red", "green"}));
	CHECK_STR(p.album, "Album");
	CHECK_STR(p.album_artist, "Band");
	CHECK_STR(p.genre, "Jazz");
	CHECK_STR(p.show, "Show");
	CHECK_STR(p.synopsis, "Synopsis");
	CHECK(p.comments == (std::vector<libfiles::media_text>{{"Logged", ""}}));
	CHECK(p.track == 3 && p.track_count == 12 && p.disc == 1 && p.disc_count == 0);
	CHECK(p.episode == 4 && p.episode_count == 10 && p.season == 2);
	CHECK(p.rating == 4);
	CHECK_STR(p.label, "Red");
	CHECK_STR(p.copyright_url, "https://example.com/rights");
	CHECK_STR(p.raw_file_name, "IMG_1.CR2");
	CHECK(p.panorama == libfiles::panorama_projection::equirectangular);

	// Each date with its tag and the zone its text ends in; Date Created, then the packet's own creation
	// date, which is the date digitized - and which exif:DateTimeDigitized, its alias, reads too.
	CHECK(sources(p) == (std::vector{date_source::xmp_photoshop_created, date_source::xmp_create, date_source::xmp_modify,
		                     date_source::xmp_exif_digitized}));
	CHECK(p.created == zoned(at(day(2018, 3, 3), 1, 2, 3), 60));
	CHECK(p.digitized == utc(at(day(2018, 3, 4), 5, 6, 7)));
	CHECK(p.modified == day(2018, 3, 5));
	CHECK_EQ(p.year, 2018);
	CHECK(!p.states(property::year));

	// Text as the packet holds it.
	CHECK_STR(p.city, " Paris ");
	CHECK_STR(p.state, "IDF");
	CHECK_STR(p.country, "France");
	CHECK_STR(p.credit, "Credit");
	CHECK_STR(p.source, "Source");
	CHECK_STR(p.camera_make, "Make");
	CHECK_STR(p.camera_model, "Model");
	CHECK(p.orientation == 6);
	CHECK(p.location && near(p.location->latitude, 51.5 + 0.5 / 60) && near(p.location->longitude, -(7.0 / 60 + 30.0 / 3600)));
	CHECK(!p.altitude);
	CHECK(near(p.f_number, 2.8) && near(p.exposure_time, 0.004) && near(p.focal_length, 50.0));
	CHECK_EQ(p.focal_length_35mm, 75);
	CHECK_STR(p.lens, "Lens");

	// dm:artist is the artist; a property present but empty states it empty, and clears what is before it.
	m.set_property(ns::dm, "artist", "Performer");
	m.set_property(ns::photoshop, "Country", "");
	const auto q = libfiles::properties_from_xmp(m);
	CHECK_STR(q.artist, "Performer");
	CHECK(q.states(property::country) && q.country.empty());

	// An xmp:CreateDate that is a year alone is the year, and no date; Windows' rating is a percent read in
	// fifths, rounded up.
	libfiles::xmp::meta bare;
	bare.set_property(ns::xmp, "CreateDate", "2011");
	bare.set_property(ns::microsoft_photo, "Rating", "50");
	const auto year_alone = libfiles::properties_from_xmp(bare);
	CHECK(year_alone.dates.empty() && !year_alone.created.valid());
	CHECK(year_alone.states(property::year) && year_alone.year == 2011);
	CHECK(year_alone.rating == 3);

	// A panorama a writer declares without naming its projection.
	libfiles::xmp::meta pano;
	pano.set_property(ns::gpano, "UsePanoramaViewer", "True");
	CHECK(libfiles::properties_from_xmp(pano).panorama == libfiles::panorama_projection::unspecified);
	CHECK(libfiles::properties_from_xmp(libfiles::xmp::meta{}) == libfiles::properties{});

	// A packet's bytes, after the signature a JPEG segment gives them.
	const std::string text = std::string("http://ns.adobe.com/xap/1.0/\0", 29) + m.serialize();
	const auto from_bytes = libfiles::properties_from_xmp(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
	CHECK_STR(from_bytes.artist, "Performer");
	CHECK(libfiles::properties_from_xmp(std::span<const std::uint8_t>{}) == libfiles::properties{});
}

TEST("A packet's property in another form than its schema gives reads as none, and the rest of the packet still reads")
{
	// A simple value where the schema has an array or alt-text, as some writers put them, and a struct where
	// it has either.
	libfiles::xmp::meta m;
	m.set_property(ns::dc, "subject", "red");
	m.set_property(ns::exif, "UserComment", "A simple comment");
	m.set_composite(ns::dc, "title", libfiles::xmp::value_is_struct);
	m.set_composite(ns::dc, "creator", libfiles::xmp::value_is_struct);
	m.set_property(ns::photoshop, "City", "Paris");
	m.set_property(ns::xmp, "Rating", "3");

	const auto p = libfiles::properties_from_xmp(m);
	CHECK(p.comments.empty() && !p.states(property::comments));
	CHECK(p.title.empty() && !p.states(property::title));
	CHECK(p.keywords.empty() && !p.states(property::keywords));
	CHECK(p.creators.empty() && !p.states(property::creators));
	CHECK_STR(p.city, "Paris");
	CHECK(p.rating == 3);

	// As a file holds them: an alias of an alt-text item given a struct, which parsing moves into the array as
	// it is, as the toolkit moves it.
	libfiles::xmp::meta parsed;
	parsed.parse(R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about="" xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/" photoshop:City="Lyon">)"
		R"(<photoshop:Caption rdf:parseType="Resource"><photoshop:Credit>inside</photoshop:Credit></photoshop:Caption>)"
		R"(</rdf:Description></rdf:RDF></x:xmpmeta>)");

	const auto from_file = libfiles::properties_from_xmp(parsed);
	CHECK(from_file.description.empty() && !from_file.states(property::description));
	CHECK_STR(from_file.city, "Lyon");
}

TEST("A packet's numbers read as far as sscanf reads them, and a rating past the scale as its end")
{
	libfiles::xmp::meta m;
	m.set_property(ns::exif, "FocalLengthIn35mmFilm", "1e10");
	m.set_property(ns::exif, "FNumber", "28/10 ");
	m.set_property(ns::exif, "FocalLength", "50/0");
	m.set_property(ns::exif, "ExposureTime", " 1/ 4");
	m.set_property(ns::xmp, "Rating", "1e300");

	const auto large = libfiles::properties_from_xmp(m);
	CHECK_EQ(large.focal_length_35mm, 1);
	CHECK(!large.states(property::f_number) && !large.states(property::focal_length));
	CHECK(near(large.exposure_time, 0.25));
	CHECK(large.rating == 1);

	libfiles::xmp::meta negative;
	negative.set_property(ns::exif, "FocalLengthIn35mmFilm", "-35");
	negative.set_property(ns::xmp, "Rating", "-7");
	negative.set_property(ns::dm, "trackNumber", "-3/12");

	const auto small = libfiles::properties_from_xmp(negative);
	CHECK(small.states(property::focal_length_35mm) && small.focal_length_35mm == 0);
	CHECK(small.rating == -1);
	CHECK(small.track == 0 && small.track_count == 12);
}

TEST("A block read over others replaces what it states, joins what it joins, and keeps every date")
{
	const auto full = every_property();

	libfiles::properties empty;
	libfiles::overlay(empty, full);
	CHECK(empty == full);

	// What a later block states stands, even empty; what it does not state stays.
	libfiles::properties first;
	first.title = "first title";
	first.album = "first album";
	first.mark_stated(property::title);
	first.mark_stated(property::album);

	libfiles::properties later;
	later.mark_stated(property::title);
	later.rating = 0;
	later.mark_stated(property::rating);

	libfiles::overlay(first, later);
	CHECK(first.title.empty() && first.states(property::title));
	CHECK_STR(first.album, "first album");
	CHECK(first.rating == 0);

	// IPTC's keywords join those before them, sorted and each once whatever its case; its by-lines join the
	// artist's names, after them.
	libfiles::exif_metadata exif;
	exif.keywords = "beach;Holiday";
	exif.artist = "Jane Doe";
	exif.original = {"2010:01:02 03:04:05", "", ""};
	libfiles::iptc_fields iptc;
	iptc.keywords = {"holiday", "Alps"};
	iptc.bylines = {"John Smith", "jane doe"};
	iptc.date_created = "20110101";

	auto photo = libfiles::properties_from_exif(exif);
	libfiles::overlay(photo, libfiles::properties_from_iptc(iptc));
	CHECK(photo.keywords == (std::vector<std::string>{"Alps", "beach", "holiday"}));
	CHECK_STR(photo.artist, "Jane Doe; John Smith");

	// Every date is kept, and created settles on the first by the ladder, not on the block read last.
	libfiles::xmp::meta packet;
	packet.set_property(ns::photoshop, "DateCreated", "2012-01-01T00:00:00");
	packet.append_array_item(ns::dc, "subject", libfiles::xmp::value_is_array, "packet");
	libfiles::overlay(photo, libfiles::properties_from_xmp(packet));
	CHECK(sources(photo) == (std::vector{date_source::exif_original, date_source::iptc_created, date_source::xmp_photoshop_created}));
	CHECK(photo.created == at(day(2010, 1, 2), 3, 4, 5));
	CHECK_EQ(photo.year, 2010);
	CHECK(photo.keywords == (std::vector<std::string>{"packet"}));
}

TEST("A file's properties are what its EXIF and IPTC state, under what its packet states")
{
	// A JPEG whose edit wrote the packet and, beside it, EXIF and IPTC: each block holds one value of its own.
	builder j;
	j.u8(0xFF).u8(0xD8);
	j.u8(0xFF).u8(0xC0).be16(11).u8(8).be16(48).be16(64).u8(1).u8(1).u8(0x11).u8(0);
	j.u8(0xFF).u8(0xDA).be16(8).u8(1).u8(1).u8(0).u8(0).u8(63).u8(0);
	j.zeros(16).u8(0xFF).u8(0xD9);

	libfiles::xmp::meta packet;
	packet.set_localized_text(ns::dc, "title", "", "x-default", "From the packet");

	libfiles::metadata_update u{&packet};
	u.legacy.title = "From the legacy blocks";
	u.legacy.description = "Only outside the packet";
	u.legacy.city = "Only in IPTC";

	libfiles::reader original(j.b);
	const auto plan = libfiles::plan_update(original, u);
	CHECK(plan.ok());
	libfiles::memory_stream out;
	libfiles::write_updated(original, plan, out);
	const auto file = out.bytes();

	libfiles::reader r(file);
	const auto p = libfiles::read_properties(r);
	CHECK_STR(p.title, "From the packet");
	CHECK_STR(p.description, "Only outside the packet");
	CHECK_STR(p.city, "Only in IPTC");

	// A property the packet states in another form than its schema gives costs the file nothing else.
	packet.set_property(ns::dc, "subject", "simple");
	packet.set_property(ns::exif, "UserComment", "A simple comment");
	libfiles::reader again(j.b);
	const auto mismatched_plan = libfiles::plan_update(again, u);
	CHECK(mismatched_plan.ok());
	libfiles::memory_stream mismatched_out;
	libfiles::write_updated(again, mismatched_plan, mismatched_out);
	const auto mismatched_file = mismatched_out.bytes();

	libfiles::reader mismatched(mismatched_file);
	const auto q = libfiles::read_properties(mismatched);
	CHECK_STR(q.title, "From the packet");
	CHECK_STR(q.description, "Only outside the packet");
	CHECK_STR(q.city, "Only in IPTC");

	// Not a file libfiles reads, and too short to be one: no properties.
	const bytes text = to_bytes("plain text, not a picture or a recording at all");
	libfiles::reader not_media(text);
	CHECK(libfiles::read_properties(not_media) == libfiles::properties{});
	const bytes two = {0xFF, 0xD8};
	libfiles::reader tiny(two);
	CHECK(libfiles::read_properties(tiny) == libfiles::properties{});
}
