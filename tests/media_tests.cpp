// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the media scan's front: container detection, the pixel format names, the text
// rules, star scales and dates every scanner shares, and how a scan is finished.

#include "test.h"
#include "builders.h"

#include "libfiles/media.h"
#include "media_internal.h"

#include <cmath>

namespace
{
	using namespace test_build;
	using libfiles::media_format;

	media_format detect(const bytes& head)
	{
		return libfiles::detect_media_format(head);
	}

	bytes padded(bytes b, const std::size_t size = 64)
	{
		if (b.size() < size) b.resize(size, 0);
		return b;
	}

	bytes transport_stream(const std::size_t packet, const std::size_t prefix, const std::size_t count)
	{
		bytes b(packet * count, 0xFF);
		for (std::size_t i = 0; i < count; ++i) b[i * packet + prefix] = 0x47;
		return b;
	}
}

TEST("media detection tells the containers apart")
{
	CHECK(detect(padded(builder().raw("RIFF").le32(100).raw("WAVE").b)) == media_format::wav);
	CHECK(detect(padded(builder().raw("RF64").le32(0xFFFFFFFF).raw("WAVE").b)) == media_format::wav);
	CHECK(detect(padded(builder().raw("BW64").le32(0xFFFFFFFF).raw("WAVE").b)) == media_format::wav);
	CHECK(detect(padded(builder().raw("RIFF").le32(100).raw("AVI ").b)) == media_format::avi);
	CHECK(detect(padded(builder().raw("FORM").be32(100).raw("AIFF").b)) == media_format::aiff);
	CHECK(detect(padded(builder().raw("FORM").be32(100).raw("AIFC").b)) == media_format::aiff);
	CHECK(detect(padded(builder().u8(0x1A).u8(0x45).u8(0xDF).u8(0xA3).b)) == media_format::matroska);
	CHECK(detect(padded(builder().raw("fLaC").b)) == media_format::flac);
	CHECK(detect(padded(builder().raw("OggS").b)) == media_format::ogg);
	CHECK(detect(padded(builder().raw("FLV").u8(1).u8(5).be32(9).b)) == media_format::flv);
	CHECK(detect(padded(builder().be32(24).raw("ftypisom").b)) == media_format::mp4);
	CHECK(detect(padded(builder().be32(20).raw("ftypqt  ").b)) == media_format::mp4);
	CHECK(detect(padded(builder().be32(8).raw("wide").b)) == media_format::mp4);
	CHECK(detect(padded(builder().u8(0).u8(0).u8(1).u8(0xBA).b)) == media_format::mpeg_ps);
	CHECK(detect(padded(builder().raw("ID3").u8(3).u8(0).u8(0).be32(0).b)) == media_format::mpeg_audio);
	CHECK(detect(padded(builder().u8(0xFF).u8(0xFB).u8(0x90).u8(0x64).b)) == media_format::mpeg_audio);

	const bytes asf = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	CHECK(detect(padded(asf)) == media_format::asf);
}

TEST("media detection knows a transport stream by its sync bytes")
{
	CHECK(detect(transport_stream(188, 0, 4)) == media_format::mpeg_ts);
	CHECK(detect(transport_stream(192, 4, 4)) == media_format::mpeg_ts);

	// Two packets are not enough to tell.
	CHECK(detect(transport_stream(188, 0, 2)) == media_format::unknown);

	auto broken = transport_stream(188, 0, 4);
	broken[376] = 0;
	CHECK(detect(broken) == media_format::unknown);
}

TEST("media detection leaves photos and other files alone")
{
	CHECK(detect(padded(builder().be32(24).raw("ftypheic").raw("mif1heic").b)) == media_format::unknown);
	CHECK(detect(padded(builder().be32(24).raw("ftypcrx ").b)) == media_format::unknown);
	CHECK(detect(padded(builder().raw("RIFF").le32(100).raw("WEBP").b)) == media_format::unknown);
	CHECK(detect(padded(builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xE0).b)) == media_format::unknown);
	CHECK(detect(padded(builder().raw("PK").u8(3).u8(4).b)) == media_format::unknown);
	CHECK(detect(bytes{0x1A, 0x45}) == media_format::unknown);
}

TEST("media pixel formats name the chroma and a depth past 8 bits")
{
	libfiles::video_format v;
	CHECK_STR(v.pixel_format(), "");

	v.chroma = libfiles::chroma_format::yuv420;
	CHECK_STR(v.pixel_format(), "yuv420");
	v.bit_depth = 8;
	CHECK_STR(v.pixel_format(), "yuv420");
	v.bit_depth = 10;
	CHECK_STR(v.pixel_format(), "yuv420 10bit");

	v.chroma = libfiles::chroma_format::yuv422;
	v.bit_depth = 12;
	CHECK_STR(v.pixel_format(), "yuv422 12bit");

	v.chroma = libfiles::chroma_format::yuv444;
	v.alpha = true;
	v.bit_depth = 0;
	CHECK_STR(v.pixel_format(), "yuva444");

	v.chroma = libfiles::chroma_format::rgb;
	CHECK_STR(v.pixel_format(), "rgba");
	v.alpha = false;
	CHECK_STR(v.pixel_format(), "rgb");

	v.chroma = libfiles::chroma_format::gray;
	CHECK_STR(v.pixel_format(), "gray");
	v.chroma = libfiles::chroma_format::yuv411;
	CHECK_STR(v.pixel_format(), "yuv411");
}

TEST("media hdr transfer names PQ and HLG")
{
	libfiles::video_format v;
	CHECK(v.hdr_transfer().empty());
	v.transfer = 16;
	CHECK_STR(std::string(v.hdr_transfer()), "PQ");
	v.transfer = 18;
	CHECK_STR(std::string(v.hdr_transfer()), "HLG");
	v.transfer = 1;
	CHECK(v.hdr_transfer().empty());
}

TEST("media text decodes as tags store it")
{
	using namespace libfiles::detail;

	CHECK_STR(text_from_windows_1252(lit("caf\xE9\0rest")), "caf\xC3\xA9");
	CHECK_STR(text_from_windows_1252(lit("\x80")), "\xE2\x82\xAC");
	CHECK_STR(text_from_utf8(lit("caf\xC3\xA9")), "caf\xC3\xA9");
	CHECK_STR(text_from_utf8(lit("a\xFF" "b")), "a\xEF\xBF\xBD" "b");
	CHECK_STR(text_from_utf16(lit("\xFF\xFEh\0i\0\0\0x\0"), true), "hi");
	CHECK_STR(text_from_utf16(lit("\0h\0i"), true), "hi");
	CHECK_STR(text_from_utf16(lit("h\0i\0"), false), "hi");
	CHECK_STR(text_from_utf16(lit("\x3D\xD8\x00\xDE"), false), "\xF0\x9F\x98\x80");
	CHECK_STR(text_from_utf16(lit("\x3D\xD8x\0"), false), "\xEF\xBF\xBDx");
	CHECK_STR(text_from_unknown(lit("caf\xC3\xA9")), "caf\xC3\xA9");
	CHECK_STR(text_from_unknown(lit("caf\xE9")), "caf\xC3\xA9");
	CHECK_STR(std::string(trim_text(std::string_view(" \tname\r\n\0", 9))), "name");
	CHECK(trim_text("  ").empty());
}

TEST("media FourCC names are printable or hex")
{
	using namespace libfiles::detail;

	CHECK_STR(fourcc_text(load_le32(lit("XVID").data())), "XVID");
	CHECK_STR(fourcc_text(0x00000001), "0x00000001");
	CHECK_STR(box_type_text(load_be32(lit("avc1").data())), "avc1");
	CHECK_STR(box_type_text(load_be32(lit("\xA9nam").data())), "\xC2\xA9nam");
	CHECK_STR(box_type_text(0x00000000), "0x00000000");
}

TEST("media star scales invert the ones libfiles writes")
{
	using namespace libfiles;

	for (auto stars = 0; stars <= 5; ++stars)
	{
		CHECK_EQ(stars_from_percent(percent_from_stars(stars)), stars);
		CHECK_EQ(stars_from_popm(popm_from_stars(stars)), stars);
	}

	// Windows writes these, and reads its percent by the ranges propkey.h states for each number of stars.
	CHECK_EQ(percent_from_stars(1), 1);
	CHECK_EQ(percent_from_stars(4), 75);
	CHECK_EQ(percent_from_stars(9), 99);
	CHECK_EQ(percent_from_stars(-1), 0);
	CHECK_EQ(popm_from_stars(3), 128);
	CHECK_EQ(popm_from_stars(-1), 0);
	CHECK_EQ(stars_from_percent(12), 1);
	CHECK_EQ(stars_from_percent(13), 2);
	CHECK_EQ(stars_from_percent(37), 2);
	CHECK_EQ(stars_from_percent(38), 3);
	CHECK_EQ(stars_from_percent(62), 3);
	CHECK_EQ(stars_from_percent(63), 4);
	CHECK_EQ(stars_from_percent(87), 4);
	CHECK_EQ(stars_from_percent(88), 5);
	CHECK_EQ(stars_from_percent(100), 5);
	CHECK_EQ(stars_from_percent(-3), 0);
	CHECK_EQ(stars_from_popm(51), 1);
	CHECK_EQ(stars_from_popm(52), 2);
	CHECK_EQ(stars_from_popm(-1), 0);
}

TEST("media dates count seconds from 1970 in UTC")
{
	using libfiles::detail::utc_date_from_seconds;

	const auto epoch = utc_date_from_seconds(0);
	CHECK_EQ(epoch.year, 1970);
	CHECK_EQ(epoch.month, 1);
	CHECK_EQ(epoch.day, 1);
	CHECK(epoch.has_time);
	CHECK(epoch.zone == libfiles::media_date::time_zone::utc);

	// 2011-03-13T15:13:49Z
	const auto d = utc_date_from_seconds(1300029229);
	CHECK_EQ(d.year, 2011);
	CHECK_EQ(d.month, 3);
	CHECK_EQ(d.day, 13);
	CHECK_EQ(d.hour, 15);
	CHECK_EQ(d.minute, 13);
	CHECK_EQ(d.second, 49);

	// 1904-01-01, QuickTime's epoch, and a leap day.
	const auto qt = utc_date_from_seconds(-2082844800);
	CHECK_EQ(qt.year, 1904);
	CHECK_EQ(qt.month, 1);
	CHECK_EQ(qt.day, 1);

	const auto leap = utc_date_from_seconds(951782400);
	CHECK_EQ(leap.month, 2);
	CHECK_EQ(leap.day, 29);

	CHECK(!utc_date_from_seconds(-62200000000).valid());
}

TEST("media scans are finished with duration, bit rate and completeness")
{
	using namespace libfiles;

	media_scan_result r;
	media_stream v;
	v.kind = media_stream_kind::video;
	v.codec = "h264";
	v.video.width = 640;
	v.video.height = 480;
	v.video.rotation = -90;
	v.duration = 9.5;

	media_stream a;
	a.kind = media_stream_kind::audio;
	a.codec = "aac";
	a.audio.sample_rate = 48000;
	a.audio.channels = 2;
	a.duration = 10.0;

	media_stream d;
	d.kind = media_stream_kind::data;
	d.duration = 60.0;

	r.streams = {v, a, d};
	detail::finish_media_scan(r, 1000000);
	CHECK(std::abs(r.duration - 10.0) < 1e-9);
	CHECK_EQ(r.bit_rate, std::uint64_t{800000});
	CHECK(r.complete);
	CHECK_EQ(r.streams[0].video.rotation, 270);

	// The container's own duration stands.
	media_scan_result stated;
	stated.duration = 12.0;
	stated.streams = {v};
	detail::finish_media_scan(stated, 1200000);
	CHECK(std::abs(stated.duration - 12.0) < 1e-9);
	CHECK_EQ(stated.bit_rate, std::uint64_t{800000});

	// An audio stream without a rate leaves the scan incomplete, and so does a missing duration.
	media_scan_result partial;
	auto mute = a;
	mute.audio.sample_rate = 0;
	partial.streams = {mute};
	detail::finish_media_scan(partial, 1000);
	CHECK(!partial.complete);

	media_scan_result timeless;
	auto still = v;
	still.duration = 0;
	timeless.streams = {still};
	detail::finish_media_scan(timeless, 1000);
	CHECK(!timeless.complete);
	CHECK_EQ(timeless.bit_rate, std::uint64_t{0});

	// Nothing to play is not complete either.
	media_scan_result data_only;
	data_only.duration = 5;
	data_only.streams = {d};
	detail::finish_media_scan(data_only, 1000);
	CHECK(!data_only.complete);
}

TEST("media scans refuse what they cannot read")
{
	const auto short_file = lit("RIFF\0\0\0\0WAVE");
	libfiles::reader tiny(short_file);
	const auto t = libfiles::scan_media(tiny);
	CHECK(!t.success);
	CHECK(!t.error.empty());

	const auto jpeg = padded(builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xE0).b);
	libfiles::reader photo(jpeg);
	const auto p = libfiles::scan_media(photo);
	CHECK(!p.success);
	CHECK(p.format == media_format::unknown);
	CHECK(!p.error.empty());
}
