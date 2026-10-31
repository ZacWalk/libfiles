// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests that libfiles' operations agree about a file. identify() names what scan_media, read_packet
// and plan_update dispatch on, and the format table says what each operation does with a format; the packet
// a scan reports is the one read_packet reads and an update replaces; an edit keeps what it does not own;
// and a picture's format, turn and thumbnails read the one way whichever format or reader they come from.

#include "test.h"

#include "libfiles/decode.h"
#include "libfiles/document.h"
#include "libfiles/identify.h"
#include "libfiles/media.h"
#include "libfiles/scan.h"
#include "libfiles/types.h"
#include "libfiles/write.h"
#include "media_builders.h"
#include "media_internal.h"
#include "round_trip.h"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace test_build;

namespace
{
	using libfiles::file_type;

	libfiles::media_scan_result scan_av(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::scan_media(r);
	}

	libfiles::packet_reading packet_of(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::read_packet(r);
	}

	file_type identified(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::identify(r);
	}

	struct written
	{
		bytes file;
		libfiles::write_plan plan;
	};

	// Patched where the file lies when the plan allows it, otherwise rewritten in one pass.
	written update(const bytes& file, const libfiles::metadata_update& u)
	{
		libfiles::reader r(file);
		auto plan = libfiles::plan_update(r, u);

		if (plan.patchable())
		{
			libfiles::memory_stream live(file);
			libfiles::write_in_place(live, plan);
			return {live.bytes(), std::move(plan)};
		}

		libfiles::memory_stream out;
		if (plan.ok()) libfiles::write_updated(r, plan, out);
		return {out.bytes(), std::move(plan)};
	}

	std::string packet_titled(const std::string_view title)
	{
		libfiles::xmp::meta meta;
		meta.set_localized_text(libfiles::xmp::ns::dc, "title", "", "x-default", title);
		return meta.serialize();
	}

	std::string title_in(const std::string_view packet)
	{
		libfiles::xmp::meta meta;
		meta.parse(packet);
		std::string title;
		meta.get_localized_text(libfiles::xmp::ns::dc, "title", "", "x-default", nullptr, &title);
		return title;
	}

	std::string text(const bytes& b)
	{
		return {b.begin(), b.end()};
	}

	bool contains(const bytes& file, const bytes& part)
	{
		return std::search(file.begin(), file.end(), part.begin(), part.end()) != file.end();
	}

	std::size_t tags_named(const libfiles::media_scan_result& s, const std::string_view key)
	{
		return static_cast<std::size_t>(std::ranges::count(s.tags, key, &libfiles::media_tag::key));
	}

	bytes padded(bytes head, const std::size_t size = libfiles::identify_bytes)
	{
		head.resize(std::max(head.size(), size), 0);
		return head;
	}

	bytes flac_after_id3(const std::vector<bytes>& id3_frames)
	{
		builder f;
		f.raw(id3_tag(3, id3_frames));
		f.raw("fLaC").raw(flac_block(0, flac_streaminfo(44100, 2, 16, 441000)));
		f.raw(flac_block(4, vorbis_comment_block({"TITLE=Vorbis title"}), true));
		return f.b;
	}
}

TEST("identify names every format by its signature")
{
	const auto check_head = [](const bytes& head, const file_type expected)
	{
		CHECK(libfiles::identify(head) == expected);
		CHECK(identified(padded(head)) == expected);
	};

	check_head(builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xE0).zeros(12).b, file_type::jpeg);
	check_head(builder().u8(0x89).raw("PNG\r\n\x1A\n").zeros(8).b, file_type::png);
	check_head(builder().raw("GIF89a").zeros(10).b, file_type::gif);
	check_head(builder().raw("RIFF").le32(100).raw("WEBP").raw("VP8 ").b, file_type::webp);
	check_head(builder().raw("8BPS").zeros(12).b, file_type::psd);
	check_head(builder().raw("II").u8(42).u8(0).zeros(12).b, file_type::tiff);
	check_head(builder().be32(24).raw("ftypheic").be32(0).raw("mif1heic").b, file_type::heif);
	check_head(builder().u8(0xFF).u8(0x0A).zeros(14).b, file_type::jxl);
	check_head(builder().be32(24).raw("ftypisom").be32(0).raw("isommp41").b, file_type::mp4);
	check_head(builder().u8(0x1A).u8(0x45).u8(0xDF).u8(0xA3).zeros(12).b, file_type::matroska);
	check_head(builder().raw("RIFF").le32(100).raw("AVI LIST").b, file_type::avi);
	check_head(builder().raw("RIFF").le32(100).raw("WAVEfmt ").b, file_type::wav);
	check_head(builder().raw("FORM").be32(100).raw("AIFFCOMM").b, file_type::aiff);
	check_head(builder().u8(0x30).u8(0x26).u8(0xB2).u8(0x75).u8(0x8E).u8(0x66).u8(0xCF).u8(0x11).u8(0xA6).u8(0xD9).u8(0x00).u8(0xAA)
	                    .u8(0x00).u8(0x62).u8(0xCE).u8(0x6C).b, file_type::asf);
	check_head(builder().be32(mp3_frame_header).zeros(12).b, file_type::mpeg_audio);
	check_head(builder().raw("fLaC").zeros(12).b, file_type::flac);
	check_head(builder().raw("OggS").zeros(12).b, file_type::ogg);
	check_head(builder().raw("FLV").u8(1).u8(5).be32(9).zeros(7).b, file_type::flv);
	check_head(builder().u8(0).u8(0).u8(1).u8(0xBA).zeros(12).b, file_type::mpeg_ps);
	check_head(builder().raw("BM").zeros(14).b, file_type::bmp);
	check_head(builder().u8(0xFF).raw("WPC").le32(16).u8(1).u8(0x16).u8(1).u8(0).u8(0).u8(0).u8(0).u8(0).b, file_type::wpg);

	// A transport stream is known only by sync bytes 188 bytes apart.
	auto ts = padded({});
	ts[0] = ts[188] = ts[376] = 0x47;
	CHECK(libfiles::identify(ts) == file_type::mpeg_ts);
	CHECK(libfiles::identify(std::span(ts).first(16)) == file_type::unknown);

	// PCX's header names its layout at byte 65, so sixteen bytes cannot tell one.
	auto pcx = padded(builder().u8(0x0A).u8(5).u8(1).u8(8).b, 128);
	pcx[65] = 1;
	CHECK(libfiles::identify(pcx) == file_type::pcx);
	CHECK(libfiles::identify(std::span(pcx).first(16)) == file_type::unknown);
	pcx[2] = 0;
	CHECK(libfiles::identify(pcx) == file_type::unknown);

	CHECK(libfiles::identify(bytes{}) == file_type::unknown);
	CHECK(libfiles::identify(builder().raw("not a file we know").b) == file_type::unknown);
}

TEST("identify puts unmistakable signatures before weak ones")
{
	// "BM" begins a QuickTime file whose first box is about 1.1 GB of media.
	const auto quicktime = padded(builder().raw("BM").be16(0x0010).raw("mdat").b);
	CHECK(libfiles::detect_image_format(quicktime) == libfiles::image_format::bmp);
	CHECK(libfiles::identify(quicktime) == file_type::mp4);

	// A GIF's 'G' is a transport stream's sync byte, which other bytes of a GIF can repeat.
	auto gif = padded(builder().raw("GIF89a").b);
	gif[188] = gif[376] = 0x47;
	CHECK(libfiles::detect_media_format(gif) == libfiles::media_format::mpeg_ts);
	CHECK(libfiles::identify(gif) == file_type::gif);

	// An ID3v2 tag reads as MPEG audio from its bytes; the reader looks past it.
	const auto flac = flac_after_id3({id3_text_frame(3, "TIT2", "Title")});
	CHECK(libfiles::identify(std::span(flac).first(16)) == file_type::mpeg_audio);
	CHECK(identified(flac) == file_type::flac);
	CHECK(identified(builder().raw(id3_tag(3, {id3_text_frame(3, "TIT2", "Title")})).raw(mp3_frames(4)).b) == file_type::mpeg_audio);
}

TEST("identify agrees with each area's detect function")
{
	std::vector<bytes> heads;
	heads.push_back(padded(builder().u8(0xFF).u8(0xD8).u8(0xFF).b));
	heads.push_back(padded(builder().raw("RIFF").le32(100).raw("WEBP").b));
	heads.push_back(padded(builder().raw("RIFF").le32(100).raw("WAVE").b));
	heads.push_back(padded(builder().raw("RIFF").le32(100).raw("AVI ").b));
	heads.push_back(padded(builder().be32(24).raw("ftypheic").be32(0).raw("mif1heic").b));
	heads.push_back(padded(builder().be32(24).raw("ftypqt  ").be32(0).raw("qt  ").b));
	heads.push_back(padded(builder().be32(24).raw("ftypcrx ").be32(0).raw("crx isom").b));
	heads.push_back(padded(builder().raw(id3_tag(4, {})).b));
	heads.push_back(padded(builder().be32(mp3_frame_header).b));
	heads.push_back(padded(builder().raw("FLV").u8(1).u8(5).be32(9).b));
	heads.push_back(padded(builder().raw("BM").b));
	heads.push_back(padded(builder().raw("II").u8(42).u8(0).b));
	heads.push_back(padded(builder().raw("OggS").b));
	heads.push_back(padded({}));

	for (const auto& h : heads)
	{
		const auto type = libfiles::identify(h);
		const auto& i = libfiles::info(type);
		const auto image = libfiles::detect_image_format(h);
		const auto media = libfiles::detect_media_format(h);

		if (image != libfiles::image_format::unknown && image != libfiles::image_format::bmp) CHECK(i.image == image);
		if (i.family == libfiles::file_family::media) CHECK(i.media == media);
		CHECK(i.update_format == libfiles::detect_file_format(std::span(h).first(16)));
	}
}

TEST("the format table describes every type once")
{
	const auto types = libfiles::file_types();
	CHECK_EQ(types.size(), static_cast<std::size_t>(file_type::mpeg_ps));

	std::set<std::string_view> extensions;

	for (std::size_t n = 0; n < types.size(); ++n)
	{
		const auto& i = types[n];
		CHECK(static_cast<std::size_t>(i.type) == n + 1);
		CHECK(&libfiles::info(i.type) == &i);
		CHECK(!i.name.empty());
		CHECK(i.mime_type.find('/') != std::string_view::npos);
		CHECK(!i.extensions.empty());
		CHECK(i.family != libfiles::file_family::unknown);

		for (const auto e : i.extensions)
		{
			CHECK(e.size() > 1 && e.front() == '.');
			CHECK(std::ranges::none_of(e, [](const char c) { return c >= 'A' && c <= 'Z'; }));
			CHECK(extensions.insert(e).second);
			CHECK(libfiles::file_type_from_extension(e) == i.type);
		}

		// Each area's own query says what the table says.
		CHECK(i.can(libfiles::capability::update) == (i.update_format != libfiles::file_format::unknown));
		CHECK(i.can(libfiles::capability::update) == libfiles::can_update(i.update_format));
		if (i.can(libfiles::capability::update)) CHECK(i.can(libfiles::capability::read_packet));

		if (i.family == libfiles::file_family::image)
		{
			CHECK(i.image != libfiles::image_format::unknown && i.media == libfiles::media_format::unknown);
			CHECK(i.can(libfiles::capability::scan) == libfiles::can_scan(i.image));
			CHECK(i.can(libfiles::capability::decode) == libfiles::can_decode(i.image));
			CHECK(i.can(libfiles::capability::decode_document) == libfiles::can_decode_document(i.image));
			CHECK(!i.can(libfiles::capability::scan_media));
		}
		else
		{
			CHECK(i.media != libfiles::media_format::unknown && i.image == libfiles::image_format::unknown);
			CHECK(i.can(libfiles::capability::scan_media));
			CHECK(!i.can(libfiles::capability::scan) && !i.can(libfiles::capability::decode));
		}
	}

	CHECK(libfiles::info(file_type::unknown).type == file_type::unknown);
	CHECK(libfiles::info(static_cast<file_type>(200)).type == file_type::unknown);
	CHECK(libfiles::file_type_from_extension(".JPG") == file_type::jpeg);
	CHECK(libfiles::file_type_from_extension("Mp3") == file_type::mpeg_audio);
	CHECK(libfiles::file_type_from_extension(".avif") == file_type::heif);
	CHECK(libfiles::file_type_from_extension(".exe") == file_type::unknown);
	CHECK(libfiles::file_type_from_extension("") == file_type::unknown);
	CHECK(libfiles::file_type_from_extension(".") == file_type::unknown);
}

TEST("An ID3-tagged FLAC is FLAC to every operation")
{
	const auto packet = packet_titled("Packet title");
	const auto file = flac_after_id3({id3_text_frame(3, "TIT2", "ID3 title"), id3_private_frame(3, "XMP", to_bytes(packet))});

	CHECK(identified(file) == file_type::flac);

	const auto scanned = scan_av(file);
	CHECK(scanned.success);
	CHECK(scanned.format == libfiles::media_format::flac);
	CHECK_STR(scanned.properties.title, "Vorbis title");
	CHECK_STR(text(scanned.xmp), packet);

	// The packet reads where the scan finds it; FLAC is not one libfiles updates, so nothing is written to a
	// tag a FLAC player would not read.
	const auto read = packet_of(file);
	CHECK(read.readable);
	CHECK_STR(read.packet, packet);

	libfiles::metadata_update edit;
	edit.legacy.title = "New title";
	libfiles::reader r(file);
	const auto plan = libfiles::plan_update(r, edit);
	CHECK(!plan.ok());
	CHECK_STR(plan.error, "this file format cannot be updated");

	// A FLAC without a tag has a packet that is known to be absent.
	builder plain;
	plain.raw("fLaC").raw(flac_block(0, flac_streaminfo(44100, 2, 16, 441000), true));
	const auto none = packet_of(plain.b);
	CHECK(none.readable && none.packet.empty());
	CHECK(scan_av(plain.b).xmp.empty());
}

TEST("read_packet reads an AIFF's packet where its scan finds it")
{
	const auto first = packet_titled("First");
	const auto last = packet_titled("Last");
	const auto in_tag = packet_titled("In the ID3 chunk");
	const auto file = aiff_form("AIFF", {aiff_comm(2, 44100, 16, 44100), aiff_xmp_chunk(first), aiff_xmp_chunk(last),
	                                     aiff_chunk("ID3 ", id3_tag(3, {id3_private_frame(3, "XMP", to_bytes(in_tag))}))});

	CHECK(identified(file) == file_type::aiff);

	const auto scanned = scan_av(file);
	CHECK(scanned.success);
	CHECK_STR(text(scanned.xmp), last);

	const auto read = packet_of(file);
	CHECK(read.readable);
	CHECK_STR(read.packet, last);

	libfiles::reader r(file);
	CHECK(!libfiles::plan_update(r, libfiles::metadata_update{}).ok());

	// Without a packet chunk the packet is known to be absent; the scan finds none either.
	const auto bare = aiff_form("AIFF", {aiff_comm(2, 44100, 16, 44100)});
	const auto none = packet_of(bare);
	CHECK(none.readable && none.packet.empty());
	CHECK(scan_av(bare).xmp.empty());
}

TEST("An edit to an MP3 whose ID3v2.4 tag has iTunes' plain sizes keeps every frame")
{
	// A user text frame of 300 bytes: its size as a plain number, 00 00 01 2C, reads as 172 when syncsafe,
	// which ends the walk in the middle of its text.
	const auto long_text = builder().u8(0).raw("Notes").u8(0).raw(std::string(292, 'x')).b;
	const auto notes = id3_frame(4, "TXXX", long_text, 0, true);
	const auto album = id3_frame(4, "TALB", builder().u8(0).raw("Album").b, 0, true);
	const auto packet = packet_titled("Packet title");
	const auto xmp = id3_frame(4, "PRIV", builder().raw("XMP").u8(0).raw(packet).b, 0, true);
	const auto file = builder().raw(id3_tag(4, {notes, album, xmp}, 64)).raw(mp3_frames(8)).b;

	const auto before = scan_av(file);
	CHECK(before.success);
	CHECK_STR(before.properties.album, "Album");
	CHECK_STR(text(before.xmp), packet);
	CHECK_STR(packet_of(file).packet, packet);

	libfiles::metadata_update edit;
	edit.legacy.title = "New title";
	const auto out = update(file, edit);
	CHECK(out.plan.ok());

	// Every frame the edit does not own is kept byte for byte.
	CHECK(contains(out.file, notes));
	CHECK(contains(out.file, album));
	CHECK(contains(out.file, xmp));

	const auto after = scan_av(out.file);
	CHECK(after.success);
	CHECK_STR(after.properties.title, "New title");
	CHECK_STR(after.properties.album, "Album");
	CHECK_STR(text(after.xmp), packet);
	CHECK_EQ(tags_named(after, "TXXX:Notes"), 1u);
	CHECK_STR(packet_of(out.file).packet, packet);
}

TEST("An unsynchronised ID3 tag's packet reads the same in the scan and read_packet")
{
	// 0xFF in the packet's text is stored with a zero after it.
	const auto packet = std::string("<x:xmpmeta>\xFF\xFE</x:xmpmeta>");
	const auto frames = builder().raw(id3_text_frame(3, "TIT2", "Title")).raw(id3_private_frame(3, "XMP", to_bytes(packet))).b;
	const auto body = unsynchronised(frames);

	builder tag;
	tag.raw("ID3").u8(3).u8(0).u8(0x80);
	put_syncsafe(tag, body.size());
	tag.raw(body);
	const auto file = builder().raw(tag.b).raw(mp3_frames(8)).b;

	const auto scanned = scan_av(file);
	CHECK(scanned.success);
	CHECK_STR(scanned.properties.title, "Title");
	CHECK_STR(text(scanned.xmp), packet);

	const auto read = packet_of(file);
	CHECK(read.readable);
	CHECK_STR(read.packet, packet);

	// The writer still takes no unsynchronised tag.
	libfiles::metadata_update edit;
	edit.legacy.title = "New title";
	libfiles::reader r(file);
	const auto plan = libfiles::plan_update(r, edit);
	CHECK(!plan.ok());
	CHECK_STR(plan.error, "an ID3 tag with unsynchronisation cannot be updated");
}

TEST("A damaged ID3 tag has no packet in the scan or read_packet")
{
	const auto packet = packet_titled("Packet title");

	// After the packet, a frame that claims more than the tag holds.
	const auto overrun = builder().raw("TALB").be32(4000).be16(0).u8(0).raw("Album").b;
	const auto damaged = builder().raw(id3_tag(3, {id3_private_frame(3, "XMP", to_bytes(packet)), overrun})).raw(mp3_frames(8)).b;

	const auto scanned = scan_av(damaged);
	CHECK(scanned.success);
	CHECK(scanned.xmp.empty());
	CHECK(!packet_of(damaged).readable);

	libfiles::reader r(damaged);
	CHECK(!libfiles::plan_update(r, libfiles::metadata_update{}).ok());

	// A tag that runs past the end of the file.
	const auto whole = builder().raw(id3_tag(3, {id3_private_frame(3, "XMP", to_bytes(packet))}, 32)).b;
	const auto cut = bytes(whole.begin(), whole.end() - 8);
	CHECK(scan_av(builder().raw(cut).b).xmp.empty());
	CHECK(!packet_of(cut).readable);
}

TEST("An ID3v2.4 packet frame with a data length indicator is read and replaced")
{
	const auto packet = packet_titled("Old title");
	const auto data = builder().raw("XMP").u8(0).raw(packet).b;
	builder body;
	put_syncsafe(body, data.size());
	body.raw(data);
	const auto file = builder().raw(id3_tag(4, {id3_text_frame(4, "TIT2", "Title"), id3_frame(4, "PRIV", body.b, 0x0001)}, 256)).raw(mp3_frames(8)).b;

	const auto scanned = scan_av(file);
	CHECK_STR(text(scanned.xmp), packet);
	CHECK_STR(packet_of(file).packet, packet);

	// The edit replaces that frame rather than adding a second packet behind it.
	libfiles::xmp::meta meta;
	meta.parse(packet);
	meta.set_localized_text(libfiles::xmp::ns::dc, "title", "", "x-default", "New title");
	const auto out = update(file, libfiles::metadata_update(&meta));
	CHECK(out.plan.ok());

	const auto after = scan_av(out.file);
	CHECK_STR(title_in(text(after.xmp)), "New title");
	CHECK_STR(title_in(packet_of(out.file).packet), "New title");
	CHECK_EQ(tags_named(after, "PRIV:XMP"), 1u);
}

TEST("A rating reads the same whichever container holds it")
{
	// EXIF's RatingPercent, and its Rating holding a percent, read as a FLAC's RATING comment does.
	const auto exif_with = [](const std::uint16_t tag, const unsigned value)
	{
		return builder().raw("II").le16(42).le32(8).le16(1).le16(tag).le16(3).le32(1).le16(value).le16(0).le32(0).b;
	};

	for (unsigned percent = 6; percent <= 100; ++percent)
	{
		const auto stars = libfiles::stars_from_percent(static_cast<int>(percent));
		const auto from_percent = libfiles::read_exif(exif_with(0x4749, percent)).rating;
		const auto from_rating = libfiles::read_exif(exif_with(0x4746, percent)).rating;
		CHECK(from_percent && *from_percent == stars);
		CHECK(from_rating && *from_rating == stars);

		const auto value = "RATING=" + std::to_string(percent);
		builder flac;
		flac.raw("fLaC").raw(flac_block(0, flac_streaminfo(44100, 2, 16, 441000))).raw(flac_block(4, vorbis_comment_block({value}), true));
		const auto scanned = scan_av(flac.b);
		CHECK(scanned.properties.rating && *scanned.properties.rating == stars);
	}

	CHECK(libfiles::read_exif(exif_with(0x4749, 0)).rating == std::optional<std::int16_t>(0));
	CHECK(libfiles::read_exif(exif_with(0x4746, 0xFFFF)).rating == std::optional<std::int16_t>(-1));
}

TEST("An MP4's packet is found however many boxes come before its user data")
{
	const auto packet = packet_titled("Packet title");
	builder movie;
	movie.raw(iso_full_box("mvhd", 0, 0, builder().be32(0).be32(0).be32(1000).be32(5000).be32(0x00010000).be16(0x0100).zeros(10)
	                                               .be32(0x00010000).zeros(12).be32(0x00010000).zeros(12).be32(0x40000000).zeros(24).be32(1).b));
	for (auto i = 0; i < 4100; ++i) movie.raw(iso_box("free", {}));
	movie.raw(iso_box("udta", iso_box("XMP_", to_bytes(packet))));

	const auto file = builder().raw(iso_box("ftyp", builder().raw("qt  ").be32(0).raw("qt  ").b)).raw(iso_box("moov", movie.b))
	                           .raw(iso_box("mdat", bytes(16, 0))).b;

	const auto read = packet_of(file);
	CHECK(read.readable);
	CHECK_STR(read.packet, packet);
	CHECK_STR(text(scan_av(file).xmp), packet);
}

TEST("A media scan inflates compressed tags within its budget")
{
	// Ten compressed text frames of 100 bytes each, read with room for three.
	std::vector<bytes> frames;
	for (auto i = 0; i < 10; ++i) frames.push_back(id3_compressed_frame("TXXX", builder().u8(0).raw("Note " + std::to_string(i)).u8(0).raw(std::string(91, 'a')).b));
	const auto file = builder().raw(id3_tag(3, frames)).raw(mp3_frames(8)).b;

	libfiles::scan_limits limits;
	limits.media_inflated_bytes = 320;
	libfiles::reader r(file);
	const auto scanned = libfiles::scan_media(r, libfiles::scan_intent::index, limits);
	CHECK(scanned.success);

	std::size_t read = 0;
	std::size_t described = 0;

	for (const auto& t : scanned.tags)
	{
		if (t.key.starts_with("TXXX:Note")) ++read;
		else if (t.key == "TXXX") ++described;
	}

	CHECK_EQ(read, 3u);
	CHECK_EQ(described, 7u);

	// The default budget reads them all.
	libfiles::reader whole(file);
	CHECK_EQ(static_cast<std::size_t>(std::ranges::count_if(libfiles::scan_media(whole).tags, [](const libfiles::media_tag& t) { return t.key.starts_with("TXXX:Note"); })), 10u);
}

TEST("An Ogg scan bounds what it checks of capture patterns that are no pages")
{
	// Each "OggS" claims a page of every segment it could hold, so checking each in turn would cost a
	// page's CRC for every seven bytes of the file.
	builder f;
	for (auto i = 0; i < 300000; ++i) f.raw(std::string_view("OggS\0\xFF\xFF", 7));

	const auto scanned = scan_av(f.b);
	CHECK(!scanned.success);
	CHECK(scanned.format == libfiles::media_format::ogg);
}

namespace
{
	using libfiles::chroma_format;
	using libfiles::picture_format;

	libfiles::scan_result scan_still(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::scan(r);
	}

	// A 64 x 48 JPEG: a frame header of the given marker and precision, a component for each sampling
	// given - the luma's first - JFIF's marker when there are three, and a scan of zeros.
	bytes jpeg_still(const unsigned sof, const unsigned precision, const std::vector<unsigned>& sampling)
	{
		const auto count = static_cast<unsigned>(sampling.size());

		builder j;
		j.u8(0xFF).u8(0xD8);
		if (count == 3) j.u8(0xFF).u8(0xE0).be16(16).raw(std::string_view("JFIF\0", 5)).u8(1).u8(1).u8(0).be16(1).be16(1).u8(0).u8(0);

		j.u8(0xFF).u8(sof).be16(8 + 3 * count).u8(precision).be16(48).be16(64).u8(count);
		for (unsigned c = 0; c < count; ++c) j.u8(c + 1).u8(sampling[c]).u8(0);

		j.u8(0xFF).u8(0xDA).be16(6 + 2 * count).u8(count);
		for (unsigned c = 0; c < count; ++c) j.u8(c + 1).u8(0);
		j.u8(0).u8(63).u8(0);

		j.zeros(16).u8(0xFF).u8(0xD9);
		return j.b;
	}

	bytes png_still(const unsigned depth, const unsigned color_type, const unsigned palette_entries = 0)
	{
		builder p;
		p.raw(std::string_view("\x89PNG\r\n\x1a\n", 8));
		png_chunk(p, "IHDR", builder().be32(64).be32(48).u8(depth).u8(color_type).u8(0).u8(0).u8(0).b);
		if (palette_entries) png_chunk(p, "PLTE", bytes(3 * palette_entries, 0));
		png_chunk(p, "IDAT", bytes(8, 0));
		png_chunk(p, "IEND", {});
		return p.b;
	}

	// A Photoshop document's header, its colour data - an indexed image's table - and no resources.
	bytes psd_still(const unsigned channels, const unsigned depth, const unsigned mode)
	{
		builder p;
		p.raw("8BPS").be16(1).zeros(6).be16(channels).be32(48).be32(64).be16(depth).be16(mode);
		const auto colours = mode == 2 ? 768u : 0u;
		p.be32(colours).zeros(colours);
		p.be32(0);
		p.zeros(16);
		return p.b;
	}

	struct tiff_entry
	{
		std::uint16_t tag = 0;
		// SHORT or LONG.
		std::uint16_t type = 3;
		std::vector<std::uint32_t> values;
	};

	// A little-endian TIFF of a 64 x 48 picture: IFD0 with these entries besides its size, the values too
	// long for an entry after it, and when asked an IFD1 whose thumbnail is a four-byte JPEG.
	bytes tiff_still(std::vector<tiff_entry> entries, const bool thumbnail = false)
	{
		entries.push_back({0x0100, 3, {64}});
		entries.push_back({0x0101, 3, {48}});
		std::ranges::sort(entries, {}, &tiff_entry::tag);

		const auto count = static_cast<std::uint32_t>(entries.size());
		auto outside = 8u + 2u + 12u * count + 4u;
		builder values;

		builder t;
		t.raw("II").le16(42).le32(8).le16(count);

		for (const auto& e : entries)
		{
			builder field;
			for (const auto v : e.values) e.type == 3 ? field.le16(v) : field.le32(v);

			t.le16(e.tag).le16(e.type).le32(static_cast<std::uint32_t>(e.values.size()));

			if (field.size() <= 4)
			{
				t.raw(field.zeros(4 - field.size()).b);
			}
			else
			{
				t.le32(outside);
				outside += static_cast<std::uint32_t>(field.size());
				values.raw(field.b);
			}
		}

		t.le32(thumbnail ? outside : 0u).raw(values.b);

		if (thumbnail)
		{
			t.le16(2);
			t.le16(0x0201).le16(4).le32(1).le32(outside + 30u);
			t.le16(0x0202).le16(4).le32(1).le32(4);
			t.le32(0).u8(0xFF).u8(0xD8).u8(0xFF).u8(0xD9);
		}

		t.zeros(16);
		return t.b;
	}

	libfiles::video_format as_video(const libfiles::detail::video_header& h)
	{
		libfiles::video_format v;
		libfiles::detail::apply_video_header(h, v);
		return v;
	}

	struct point
	{
		int x = 0;
		int y = 0;

		friend bool operator==(const point&, const point&) = default;
	};
}

TEST("A picture format has one name, a still's or a video's")
{
	CHECK_STR(picture_format{}.name(), "");
	CHECK_STR((picture_format{chroma_format::gray, 8}).name(), "gray");
	CHECK_STR((picture_format{chroma_format::gray, 16, true}).name(), "graya 16bit");
	CHECK_STR((picture_format{chroma_format::yuv420, 10}).name(), "yuv420 10bit");
	CHECK_STR((picture_format{chroma_format::yuv422, 8, true}).name(), "yuva422");
	CHECK_STR((picture_format{chroma_format::yuv411, 8}).name(), "yuv411");
	CHECK_STR((picture_format{chroma_format::rgb, 8, true}).name(), "rgba");
	CHECK_STR((picture_format{chroma_format::cmyk, 16}).name(), "cmyk 16bit");
	CHECK_STR((picture_format{chroma_format::ycck, 8}).name(), "ycck");
	CHECK_STR((picture_format{chroma_format::lab, 8}).name(), "lab");
	CHECK_STR((picture_format{chroma_format::rgb, 4, false, true}).name(), "palette");

	libfiles::video_format v;
	v.chroma = chroma_format::yuv420;
	v.bit_depth = 10;
	CHECK(v.picture() == (picture_format{chroma_format::yuv420, 10}));
	CHECK_STR(v.pixel_format(), v.picture().name());
	CHECK_STR(v.pixel_format(), "yuv420 10bit");
}

TEST("Gray is gray to every scanner, whatever each has always called it")
{
	const auto png = scan_still(png_still(8, 0));
	const auto jpeg = scan_still(jpeg_still(0xC0, 8, {0x11}));
	const auto psd = scan_still(psd_still(1, 8, 1));
	const auto tiff = scan_still(tiff_still({{0x0102, 3, {8}}, {0x0106, 3, {1}}}));

	CHECK_STR(png.pixel_format, "gray");
	CHECK_STR(jpeg.pixel_format, "gray8");
	CHECK_STR(psd.pixel_format, "gray8");
	CHECK_STR(tiff.pixel_format, "");

	for (const auto* const s : {&png, &jpeg, &psd, &tiff})
	{
		CHECK(s->success);
		CHECK(s->picture == (picture_format{chroma_format::gray, 8}));
	}
}

TEST("A still's picture states the depth, alpha and palette its header does")
{
	// PNG: gray with alpha, which the legacy names never covered, and a palette's index depth.
	CHECK_STR(scan_still(png_still(16, 4)).picture.name(), "graya 16bit");
	CHECK_STR(scan_still(png_still(16, 4)).pixel_format, "");
	CHECK(scan_still(png_still(4, 3, 16)).picture == (picture_format{chroma_format::rgb, 4, false, true}));
	CHECK_STR(scan_still(png_still(8, 6)).picture.name(), "rgba");

	// JPEG: the frame's precision, which the legacy name leaves at 8, and the ink models.
	CHECK_STR(scan_still(jpeg_still(0xC1, 12, {0x11})).pixel_format, "gray8");
	CHECK_STR(scan_still(jpeg_still(0xC1, 12, {0x11})).picture.name(), "gray 12bit");
	CHECK_STR(scan_still(jpeg_still(0xC0, 8, {0x21, 0x11, 0x11})).picture.name(), "yuv422");
	CHECK_STR(scan_still(jpeg_still(0xC0, 8, {0x11, 0x11, 0x11, 0x11})).picture.name(), "cmyk");

	// GIF and BMP: palettes of the depth stated, and BMP's direct colour.
	builder gif;
	gif.raw("GIF89a").le16(64).le16(48).u8(0x82).u8(0).u8(0).zeros(24);
	gif.u8(0x2C).zeros(8).u8(0).u8(2).u8(1).u8(0).u8(0).u8(0x3B);
	CHECK(scan_still(gif.b).picture == (picture_format{chroma_format::rgb, 3, false, true}));

	const auto bmp = [](const unsigned bit_count)
	{
		builder b;
		b.raw("BM").zeros(12);
		b.le32(40).le32(64).le32(48).le16(1).le16(bit_count).zeros(24);
		return scan_still(b.b).picture;
	};

	CHECK(bmp(8) == (picture_format{chroma_format::rgb, 8, false, true}));
	CHECK(bmp(24) == (picture_format{chroma_format::rgb, 8}));
	CHECK(bmp(16) == (picture_format{chroma_format::rgb, 0}));

	// PSD: its colour modes, at the depth of a channel.
	CHECK_STR(scan_still(psd_still(4, 8, 3)).picture.name(), "rgba");
	CHECK_STR(scan_still(psd_still(4, 8, 4)).picture.name(), "cmyk");
	CHECK_STR(scan_still(psd_still(3, 8, 9)).picture.name(), "lab");
	CHECK(scan_still(psd_still(1, 1, 0)).picture == (picture_format{chroma_format::gray, 1}));
	CHECK(scan_still(psd_still(1, 8, 2)).picture == (picture_format{chroma_format::rgb, 8, false, true}));

	// TIFF, which names no legacy format: a depth stored past its entry, an alpha among the extra
	// samples, YCbCr's sampling and a palette.
	const auto rgba = scan_still(tiff_still({{0x0102, 3, {16, 16, 16, 16}}, {0x0106, 3, {2}}, {0x0115, 3, {4}}, {0x0152, 3, {2}}}));
	CHECK_STR(rgba.picture.name(), "rgba 16bit");
	CHECK_STR(scan_still(tiff_still({{0x0102, 3, {8, 8, 8}}, {0x0106, 3, {6}}, {0x0212, 3, {2, 1}}})).picture.name(), "yuv422");
	CHECK_STR(scan_still(tiff_still({{0x0102, 3, {8, 8, 8}}, {0x0106, 3, {6}}})).picture.name(), "yuv420");
	// 4:1:0 is a quarter of the chroma both ways, as FFmpeg reads it; a quarter by a half has no name.
	CHECK_STR(scan_still(tiff_still({{0x0102, 3, {8, 8, 8}}, {0x0106, 3, {6}}, {0x0212, 3, {4, 4}}})).picture.name(), "yuv410");
	CHECK_STR(scan_still(tiff_still({{0x0102, 3, {8, 8, 8}}, {0x0106, 3, {6}}, {0x0212, 3, {4, 2}}})).picture.name(), "");
	CHECK(scan_still(tiff_still({{0x0102, 3, {4}}, {0x0106, 3, {3}}})).picture == (picture_format{chroma_format::rgb, 4, false, true}));

	// A bilevel TIFF need not state its depth, which is then one bit.
	CHECK(scan_still(tiff_still({{0x0106, 3, {0}}})).picture == (picture_format{chroma_format::gray, 1}));
}

TEST("A frame a still and a video both hold reads as the same picture in each")
{
	// The image and video readers follow different references, which part ways only over records that
	// are malformed; a well-formed frame must read the same in each. A JPEG still and a Motion JPEG frame:
	for (const auto& [sof, precision, sampling] : std::vector<std::tuple<unsigned, unsigned, std::vector<unsigned>>>{
		     {0xC0, 8, {0x11}}, {0xC1, 12, {0x11}}, {0xC0, 8, {0x22, 0x11, 0x11}}, {0xC0, 8, {0x21, 0x11, 0x11}},
		     {0xC2, 8, {0x11, 0x11, 0x11}},
	     })
	{
		const auto file = jpeg_still(sof, precision, sampling);
		libfiles::detail::video_header frame;
		CHECK(libfiles::detail::parse_jpeg_frame(file, frame));
		CHECK(scan_still(file).picture.chroma != chroma_format::unknown);
		CHECK(as_video(frame).picture() == scan_still(file).picture);
	}

	// A lossy WebP and a VP8 key frame in a video.
	for (const auto& [width, height] : std::vector<std::pair<unsigned, unsigned>>{{300, 200}, {1, 1}, {16383, 3}})
	{
		builder webp;
		riff_chunk(webp, "VP8 ", vp8_frame(width, height));
		const auto still = scan_still(finish_riff(webp));

		libfiles::detail::video_header frame;
		CHECK(libfiles::detail::parse_vp8_frame(vp8_frame(width, height), frame));
		const auto video = as_video(frame);
		CHECK(still.picture.chroma != chroma_format::unknown);
		CHECK(video.picture() == still.picture);
		CHECK_EQ(video.width, still.width);
		CHECK_EQ(video.height, still.height);
	}
}

TEST("A turn and a mirror are EXIF's orientation, for a still and a video alike")
{
	using libfiles::orientation_from_turn;
	using libfiles::turn_from_orientation;

	// Where a pixel of a w x h picture shows once it is mirrored and turned as the turn says.
	const auto shown = [](point p, int w, int h, const libfiles::picture_turn& turn)
	{
		if (turn.mirrored) p.x = w - 1 - p.x;

		for (auto quarter = 0; quarter < turn.clockwise_degrees / 90; ++quarter)
		{
			p = {h - 1 - p.y, p.x};
			std::swap(w, h);
		}

		return p;
	};

	// EXIF's definition of each orientation, for a 4 x 3 picture: where its first pixel shows, and the
	// second of its first row, which runs along the side the orientation names.
	const std::pair<point, point> exif[] = {
		{{0, 0}, {1, 0}}, {{3, 0}, {2, 0}}, {{3, 2}, {2, 2}}, {{0, 2}, {1, 2}},
		{{0, 0}, {0, 1}}, {{2, 0}, {2, 1}}, {{2, 3}, {2, 2}}, {{0, 3}, {0, 2}},
	};

	for (std::uint16_t orientation = 1; orientation <= 8; ++orientation)
	{
		const auto turn = turn_from_orientation(orientation);
		const auto& [first, second] = exif[orientation - 1];
		CHECK(shown({0, 0}, 4, 3, turn) == first);
		CHECK(shown({1, 0}, 4, 3, turn) == second);
		CHECK_EQ(orientation_from_turn(turn.clockwise_degrees, turn.mirrored), orientation);
	}

	CHECK(turn_from_orientation(0) == libfiles::picture_turn{});
	CHECK(turn_from_orientation(9) == libfiles::picture_turn{});
	CHECK_EQ(orientation_from_turn(-90, false), 8);
	CHECK_EQ(orientation_from_turn(450, false), 6);

	libfiles::video_format v;
	CHECK_EQ(v.orientation(), 1);
	v.rotation = 90;
	CHECK_EQ(v.orientation(), 6);
	v.mirrored = true;
	CHECK_EQ(v.orientation(), 7);
}

TEST("A still's thumbnails are embedded images, as a recording's cover art is")
{
	static_assert(std::is_same_v<libfiles::media_picture, libfiles::embedded_image>);

	// IFD0 with an orientation, and IFD1 pointing at a four-byte JPEG.
	builder exif;
	exif.raw("II").le16(42).le32(8);
	exif.le16(1).le16(0x0112).le16(3).le32(1).le16(1).le16(0).le32(26);
	exif.le16(2).le16(0x0201).le16(4).le32(1).le32(56).le16(0x0202).le16(4).le32(1).le32(4).le32(0);
	exif.u8(0xFF).u8(0xD8).u8(0xFF).u8(0xD9);

	builder jpeg;
	jpeg.u8(0xFF).u8(0xD8);
	jpeg.u8(0xFF).u8(0xE0).be16(16).raw(std::string_view("JFIF\0", 5)).zeros(9);
	jpeg.u8(0xFF).u8(0xE0).be16(24).raw(std::string_view("JFXX\0", 5)).u8(0x10).u8(0xFF).u8(0xD8).zeros(14);
	jpeg.u8(0xFF).u8(0xE1).be16(static_cast<unsigned>(8 + exif.size())).raw(std::string_view("Exif\0\0", 6)).raw(exif.b);
	const auto frame = jpeg_still(0xC0, 8, {0x11});
	jpeg.raw(bytes(frame.begin() + 2, frame.end()));

	const auto scanned = scan_still(jpeg.b);
	const auto images = libfiles::thumbnails(scanned);
	CHECK_EQ(images.size(), 2u);

	if (images.size() == 2)
	{
		// The EXIF block's first, its bytes copied out, and then the JFIF extension's, in place.
		CHECK(images[0].location.empty());
		CHECK(images[0].data == (bytes{0xFF, 0xD8, 0xFF, 0xD9}));
		CHECK(images[1].location == scanned.jfif_thumbnail);
		CHECK(images[1].data.empty());

		for (const auto& image : images)
		{
			CHECK_STR(image.mime_type, "image/jpeg");
			CHECK_EQ(image.type, 0);
		}
	}

	const auto tiff = tiff_still({{0x0106, 3, {2}}}, true);
	const auto tiff_images = libfiles::thumbnails(scan_still(tiff));
	CHECK_EQ(tiff_images.size(), 1u);

	if (tiff_images.size() == 1)
	{
		const auto at = tiff.begin() + static_cast<std::ptrdiff_t>(tiff_images[0].location.offset);
		CHECK(bytes(at, at + 4) == (bytes{0xFF, 0xD8, 0xFF, 0xD9}));
	}

	CHECK(libfiles::thumbnails(scan_still(png_still(8, 2))).empty());
}

TEST("MPEG audio reads back the edits it keeps outside the packet")
{
	// ID3 keeps a recording's own fields. ID3v2.3 keeps the time of day to the minute, so a time with seconds
	// does not read back whole; a file without a tag gains an ID3v2.3 one.
	const std::set<std::string_view> id3 = {
		"album", "album artist", "artist", "comment", "copyright", "disc", "genre", "rating", "title", "track", "year",
	};

	auto id3v24 = id3;
	id3v24.insert("recorded");

	check_round_trip("mp3 id3v2.3", builder().raw(id3_tag(3, {}, 64)).raw(mp3_frames(8)).b, id3);
	check_round_trip("mp3 id3v2.4", builder().raw(id3_tag(4, {}, 64)).raw(mp3_frames(8)).b, id3v24);
	check_round_trip("mp3 without a tag", mp3_frames(8), id3);
}
