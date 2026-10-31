// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for the properties model: each block - EXIF, IPTC, XMP - read into the same properties by
// its table, the rule blocks are reconciled by, and a file's properties read whole.

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

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-9;
	}

	// A property of every kind set, each to a value no other has.
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
		p.created = day(2001, 2, 3);
		p.original_release = day(1999, 1, 1);
		p.location = libfiles::gps_position{1.5, -2.5};
		p.altitude = 30.0;
		p.lens = "lens";
		p.f_number = 2.8;
		p.exposure_time = 0.004;
		p.focal_length = 50.0;
		p.focal_length_35mm = 75;
		p.iso_speed = 200;
		p.orientation = 6;
		p.digitized = day(2001, 2, 4);
		p.modified = day(2001, 2, 5);
		p.city = "city";
		p.state = "state";
		p.country = "country";
		p.credit = "credit";
		p.source = "source";
		return p;
	}
}

TEST("EXIF reads into properties by the table")
{
	libfiles::exif_metadata e;
	e.title = "XP title";
	e.artist = "Ann Author";
	e.copyright = "(c) Ann";
	e.description = "A description";
	e.comment = "A comment";
	e.keywords = "alpha; beta;;gamma ";
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

	const auto p = libfiles::properties_from_exif(e);

	CHECK_STR(p.title, "XP title");
	CHECK_STR(p.artist, "Ann Author");
	CHECK_STR(p.copyright, "(c) Ann");
	CHECK_STR(p.description, "A description");
	CHECK(p.comments == (std::vector<libfiles::media_text>{{"A comment", ""}}));
	CHECK(p.keywords == (std::vector<std::string>{"alpha", "beta", "gamma"}));
	CHECK(p.rating == 3);
	CHECK_STR(p.encoder, "Editor 2");
	CHECK_STR(p.camera_make, "Make");
	CHECK_STR(p.camera_model, "Model");
	CHECK_STR(p.lens, "50mm");
	CHECK(near(p.f_number, 2.0) && near(p.exposure_time, 0.5) && near(p.focal_length, 50.0));
	CHECK_EQ(p.focal_length_35mm, 75);
	CHECK_EQ(p.iso_speed, 400);
	CHECK(p.orientation == 8);
	CHECK(p.created == zoned(at(day(2020, 5, 6), 7, 8, 9), 120));
	CHECK(p.digitized == at(day(2020, 5, 6), 7, 8, 10));
	CHECK(p.modified == at(day(2021, 1, 2), 3, 4, 5));
	CHECK_EQ(p.year, 2020);
	CHECK(p.location == (libfiles::gps_position{51.5, -0.125}));
	CHECK(p.altitude == -12.0);

	// A position never stands for one EXIF does not state, and its altitude goes with it.
	libfiles::exif_metadata no_fix;
	no_fix.altitude = 100.0f;
	CHECK(!libfiles::properties_from_exif(no_fix).location);
	CHECK(!libfiles::properties_from_exif(no_fix).altitude);
	CHECK(libfiles::properties_from_exif(no_fix) == libfiles::properties{});
}

TEST("IPTC reads into properties by the table")
{
	libfiles::iptc_fields i;
	i.object_name = " Object ";
	i.caption = "Caption\nsecond line";
	i.bylines = {"One", " ", "Two"};
	i.copyright = "IPTC copyright";
	i.keywords = {"k1", "", "k2"};
	i.city = "Cardiff";
	i.state = "Wales";
	i.country = "UK";
	i.credit = "Agency";
	i.source = "Archive";
	i.date_created = "20190807";
	i.time_created = "101112+0130";
	i.digital_creation_date = "20190808";

	const auto p = libfiles::properties_from_iptc(i);

	CHECK_STR(p.title, "Object");
	CHECK_STR(p.description, "Caption\nsecond line");
	CHECK_STR(p.artist, "One; Two");
	CHECK_STR(p.copyright, "IPTC copyright");
	CHECK(p.keywords == (std::vector<std::string>{"k1", "k2"}));
	CHECK_STR(p.city, "Cardiff");
	CHECK_STR(p.state, "Wales");
	CHECK_STR(p.country, "UK");
	CHECK_STR(p.credit, "Agency");
	CHECK_STR(p.source, "Archive");
	CHECK(p.created == zoned(at(day(2019, 8, 7), 10, 11, 12), 90));
	CHECK(p.digitized == day(2019, 8, 8));
	CHECK_EQ(p.year, 2019);
}

TEST("XMP reads into properties by the table")
{
	libfiles::xmp::meta m;
	m.set_localized_text(ns::dc, "title", "", "x-default", "Packet title");
	m.set_localized_text(ns::dc, "description", "", "x-default", "Packet description");
	m.set_localized_text(ns::dc, "rights", "", "x-default", "Packet rights");
	m.append_array_item(ns::dc, "creator", libfiles::xmp::array_is_ordered, "First");
	m.append_array_item(ns::dc, "creator", libfiles::xmp::array_is_ordered, "Second");
	m.append_array_item(ns::dc, "subject", libfiles::xmp::value_is_array, "red");
	m.append_array_item(ns::dc, "subject", libfiles::xmp::value_is_array, "green");
	m.append_array_item(ns::dc, "publisher", libfiles::xmp::value_is_array, "Label");
	m.set_property(ns::dm, "album", "Album");
	m.set_property(ns::dm, "albumArtist", "Band");
	m.set_property(ns::dm, "composer", "Composer");
	m.set_property(ns::dm, "genre", "Jazz");
	m.set_property(ns::dm, "logComment", "Logged");
	m.set_property(ns::dm, "trackNumber", "3/12");
	m.set_property(ns::dm, "discNumber", "1/2");
	m.set_property(ns::dm, "releaseDate", "1999");
	m.set_property(ns::xmp, "Rating", "4.0");
	m.set_property(ns::xmp, "CreatorTool", "Tool 1");
	m.set_property(ns::xmp, "CreateDate", "2018-03-04T05:06:07Z");
	m.set_property(ns::xmp, "ModifyDate", "2018-03-05");
	m.set_property(ns::photoshop, "DateCreated", "2018-03-03T01:02:03.250+01:00");
	m.set_property(ns::photoshop, "City", "Paris");
	m.set_property(ns::photoshop, "State", "IDF");
	m.set_property(ns::photoshop, "Country", "France");
	m.set_property(ns::photoshop, "Credit", "Credit");
	m.set_property(ns::photoshop, "Source", "Source");
	m.set_property(ns::tiff, "Make", "Make");
	m.set_property(ns::tiff, "Model", "Model");
	m.set_property(ns::tiff, "Orientation", "6");
	m.set_property(ns::exif, "GPSLatitude", "51,30.5N");
	m.set_property(ns::exif, "GPSLongitude", "0,7,30W");
	m.set_property(ns::exif, "GPSAltitude", "250/10");
	m.set_property(ns::exif, "GPSAltitudeRef", "1");
	m.set_property(ns::exif, "FNumber", "28/10");
	m.set_property(ns::exif, "ExposureTime", "1/250");
	m.set_property(ns::exif, "FocalLength", "50");
	m.set_property(ns::exif, "FocalLengthIn35mmFilm", "75");
	m.append_array_item(ns::exif, "ISOSpeedRatings", libfiles::xmp::array_is_ordered, "200");
	m.set_property(ns::exif_ex, "LensModel", "Lens");

	const auto p = libfiles::properties_from_xmp(m);

	CHECK_STR(p.title, "Packet title");
	CHECK_STR(p.description, "Packet description");
	CHECK_STR(p.copyright, "Packet rights");
	CHECK_STR(p.artist, "First; Second");
	CHECK(p.keywords == (std::vector<std::string>{"red", "green"}));
	CHECK_STR(p.publisher, "Label");
	CHECK_STR(p.album, "Album");
	CHECK_STR(p.album_artist, "Band");
	CHECK_STR(p.composer, "Composer");
	CHECK_STR(p.genre, "Jazz");
	CHECK(p.comments == (std::vector<libfiles::media_text>{{"Logged", ""}}));
	CHECK(p.track == 3 && p.track_count == 12 && p.disc == 1 && p.disc_count == 2);
	CHECK(p.original_release == day(1999, 0, 0));
	CHECK(p.rating == 4);
	CHECK_STR(p.encoder, "Tool 1");

	// MWG's Date Created before the packet's own creation date, which is the date digitized.
	CHECK(p.created == zoned(at(day(2018, 3, 3), 1, 2, 3), 60));
	CHECK(p.digitized.year == 2018 && p.digitized.day == 4 && p.digitized.zone == libfiles::date::time_zone::utc);
	CHECK(p.modified == day(2018, 3, 5));
	CHECK_EQ(p.year, 2018);

	CHECK_STR(p.city, "Paris");
	CHECK_STR(p.state, "IDF");
	CHECK_STR(p.country, "France");
	CHECK_STR(p.credit, "Credit");
	CHECK_STR(p.source, "Source");
	CHECK_STR(p.camera_make, "Make");
	CHECK_STR(p.camera_model, "Model");
	CHECK(p.orientation == 6);
	CHECK(p.location && near(p.location->latitude, 51.5 + 0.5 / 60) && near(p.location->longitude, -(7.0 / 60 + 30.0 / 3600)));
	CHECK(p.altitude == -25.0);
	CHECK(near(p.f_number, 2.8) && near(p.exposure_time, 0.004) && near(p.focal_length, 50.0));
	CHECK_EQ(p.focal_length_35mm, 75);
	CHECK_EQ(p.iso_speed, 200);
	CHECK_STR(p.lens, "Lens");

	// A recording's artist before a photo's creators.
	m.set_property(ns::dm, "artist", "Performer");
	CHECK_STR(libfiles::properties_from_xmp(m).artist, "Performer");

	// Without Date Created, the packet's creation date stands for it; a rating past the scale is clamped.
	libfiles::xmp::meta bare;
	bare.set_property(ns::xmp, "CreateDate", "2011");
	bare.set_property(ns::xmp, "Rating", "-1");
	CHECK(libfiles::properties_from_xmp(bare).created == day(2011, 0, 0));
	CHECK(libfiles::properties_from_xmp(bare).rating == -1);
	CHECK(libfiles::properties_from_xmp(libfiles::xmp::meta{}) == libfiles::properties{});
}

TEST("A packet's property in another form than its schema gives reads as none, and the rest of the packet still reads")
{
	// A simple value where the schema has an array or alt-text, as some writers put them, and a struct where
	// it has either.
	libfiles::xmp::meta m;
	m.set_property(ns::exif, "ISOSpeedRatings", "100");
	m.set_property(ns::exif, "UserComment", "A simple comment");
	m.set_composite(ns::dc, "title", libfiles::xmp::value_is_struct);
	m.set_composite(ns::dc, "subject", libfiles::xmp::value_is_struct);
	m.set_composite(ns::dc, "creator", libfiles::xmp::value_is_struct);
	m.set_composite(ns::dc, "publisher", libfiles::xmp::value_is_struct);
	m.set_property(ns::photoshop, "City", "Paris");
	m.set_property(ns::xmp, "Rating", "3");

	const auto p = libfiles::properties_from_xmp(m);
	CHECK_EQ(p.iso_speed, 0);
	CHECK(p.comments.empty());
	CHECK(p.title.empty() && p.keywords.empty() && p.artist.empty() && p.publisher.empty());
	CHECK_STR(p.city, "Paris");
	CHECK(p.rating == 3);

	// As a file holds them: an alias of an alt-text item given a struct, which parsing moves into the array as
	// it is, as the toolkit moves it; and a simple value where the schema has an array.
	libfiles::xmp::meta parsed;
	parsed.parse(R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about="" xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/" xmlns:exif="http://ns.adobe.com/exif/1.0/")"
		R"( photoshop:City="Lyon" exif:ISOSpeedRatings="100">)"
		R"(<photoshop:Caption rdf:parseType="Resource"><photoshop:Credit>inside</photoshop:Credit></photoshop:Caption>)"
		R"(</rdf:Description></rdf:RDF></x:xmpmeta>)");

	const auto from_file = libfiles::properties_from_xmp(parsed);
	CHECK(from_file.description.empty());
	CHECK_EQ(from_file.iso_speed, 0);
	CHECK_STR(from_file.city, "Lyon");
}

TEST("A packet's number past what its property holds reads as none, and a rating past the scale as its end")
{
	libfiles::xmp::meta m;
	m.set_property(ns::exif, "FocalLengthIn35mmFilm", "1e10");
	m.append_array_item(ns::exif, "ISOSpeedRatings", libfiles::xmp::array_is_ordered, "3000000000");
	m.set_property(ns::xmp, "Rating", "1e300");

	const auto large = libfiles::properties_from_xmp(m);
	CHECK_EQ(large.focal_length_35mm, 0);
	CHECK_EQ(large.iso_speed, 0);
	CHECK(large.rating == 5);

	libfiles::xmp::meta negative;
	negative.set_property(ns::exif, "FocalLengthIn35mmFilm", "-35");
	negative.set_property(ns::xmp, "Rating", "-1e300");

	const auto small = libfiles::properties_from_xmp(negative);
	CHECK_EQ(small.focal_length_35mm, 0);
	CHECK(small.rating == -1);
}

TEST("Blocks are reconciled by filling what the first lacks")
{
	const auto full = every_property();

	libfiles::properties empty;
	libfiles::fill_missing(empty, full);
	CHECK(empty == full);

	// What the first states stands; its position keeps its own altitude, or none.
	libfiles::properties first;
	first.title = "kept";
	first.rating = 0;
	first.location = libfiles::gps_position{9.0, 9.0};
	libfiles::fill_missing(first, full);
	CHECK_STR(first.title, "kept");
	CHECK(first.rating == 0);
	CHECK(first.location == (libfiles::gps_position{9.0, 9.0}));
	CHECK(!first.altitude);
	CHECK_STR(first.album, "album");
}

TEST("A file's properties are its packet's, then what it keeps outside the packet")
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
	packet.set_property(ns::exif, "ISOSpeedRatings", "100");
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
