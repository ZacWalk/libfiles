// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the tags media files share - ID3, APE and Vorbis comments - the tag mapping, and dates and places.

#include "test.h"
#include "builders.h"

#include "id3_internal.h"
#include "libfiles/media.h"
#include "media_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	using namespace test_build;
	using libfiles::media_date;
	using libfiles::media_properties;
	using libfiles::media_scan_result;
	using libfiles::detail::tag_family;

	bytes join(const std::vector<bytes>& parts)
	{
		bytes out;
		for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
		return out;
	}

	std::size_t find_bytes(const bytes& haystack, const bytes& needle)
	{
		const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
		return it == haystack.end() ? std::string::npos : static_cast<std::size_t>(it - haystack.begin());
	}

	// ---- ID3v2 ---------------------------------------------------------------------------------------

	bytes syncsafe(const std::uint32_t v)
	{
		return {static_cast<std::uint8_t>(v >> 21 & 0x7F), static_cast<std::uint8_t>(v >> 14 & 0x7F),
		        static_cast<std::uint8_t>(v >> 7 & 0x7F), static_cast<std::uint8_t>(v & 0x7F)};
	}

	// A frame: its header as the version writes it, then its body. iTunes wrote ID3v2.4 sizes plainly.
	bytes frame(const int version, const std::string_view id, const bytes& body, const unsigned flags = 0, const bool plain_size = false)
	{
		const auto size = static_cast<std::uint32_t>(body.size());
		builder b;
		b.raw(id);

		if (version == 2) b.u8(size >> 16).u8(size >> 8).u8(size);
		else if (version == 4 && !plain_size) b.raw(syncsafe(size));
		else b.be32(size);

		if (version != 2) b.be16(flags);
		return b.raw(body).b;
	}

	// A text frame's body: the encoding, then the text as given.
	bytes text(const unsigned encoding, const bytes& value)
	{
		return builder().u8(encoding).raw(value).b;
	}

	bytes text(const std::string_view value)
	{
		return text(0, to_bytes(value));
	}

	// ASCII as UTF-16, little-endian after its byte order mark, or big-endian with or without one.
	bytes utf16(const std::string_view value, const bool big_endian = false, const bool mark = true)
	{
		builder b;
		if (mark && big_endian) b.u8(0xFE).u8(0xFF);
		if (mark && !big_endian) b.u8(0xFF).u8(0xFE);

		for (const auto c : value)
		{
			if (big_endian) b.u8(0).u8(static_cast<unsigned char>(c));
			else b.u8(static_cast<unsigned char>(c)).u8(0);
		}

		return b.b;
	}

	bytes picture_body(const int version, const std::string_view format, const unsigned type, const std::string_view description,
	                   const bytes& image)
	{
		builder b;
		b.u8(0).raw(format);
		if (version != 2) b.u8(0);
		return b.u8(type).raw(description).u8(0).raw(image).b;
	}

	// Bytes standing for an image, with the 0xFF pairs unsynchronisation changes.
	bytes image(const std::size_t size, const unsigned seed = 37)
	{
		bytes b(size);
		for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(i % 7 == 0 ? 0xFF : i * seed);

		if (size >= 4)
		{
			b[0] = 0xFF;
			b[1] = 0xD8;
			b[2] = 0xFF;
			b[3] = 0xE0;
		}

		return b;
	}

	bytes id3_tag(const int version, const std::vector<bytes>& frames, const std::size_t padding = 0, const unsigned flags = 0,
	              const bytes& extended = {})
	{
		auto body = join({extended, join(frames)});
		body.resize(body.size() + padding, 0);
		const auto size = static_cast<std::uint32_t>(body.size());

		builder b;
		b.raw("ID3").u8(static_cast<unsigned>(version)).u8(0).u8(flags).raw(syncsafe(size)).raw(body);
		if (flags & 0x10) b.raw("3DI").u8(static_cast<unsigned>(version)).u8(0).u8(flags).raw(syncsafe(size));
		return b.b;
	}

	// Unsynchronisation: a zero after each 0xFF that a zero or a byte of a sync pattern follows, and after
	// a last 0xFF.
	bytes unsynchronise(const bytes& in)
	{
		bytes out;

		for (std::size_t i = 0; i < in.size(); ++i)
		{
			out.push_back(in[i]);
			if (in[i] == 0xFF && (i + 1 == in.size() || in[i + 1] == 0 || in[i + 1] >= 0xE0)) out.push_back(0);
		}

		return out;
	}

	// An ID3v2.2 or 2.3 tag unsynchronised whole after its header.
	bytes unsynchronised_tag(const int version, const std::vector<bytes>& frames, const std::size_t padding = 0, const bytes& extended = {})
	{
		auto body = join({extended, join(frames)});
		body.resize(body.size() + padding, 0);
		body = unsynchronise(body);

		builder b;
		b.raw("ID3").u8(static_cast<unsigned>(version)).u8(0).u8(extended.empty() ? 0x80 : 0xC0);
		return b.raw(syncsafe(static_cast<std::uint32_t>(body.size()))).raw(body).b;
	}

	// A zlib stream of stored blocks, as a compressed frame holds one.
	bytes zlib_stored(const bytes& data)
	{
		builder b;
		b.u8(0x78).u8(0x01);
		std::size_t at = 0;

		do
		{
			const auto n = std::min<std::size_t>(65535, data.size() - at);
			b.u8(at + n == data.size() ? 1 : 0).le16(static_cast<unsigned>(n)).le16(static_cast<unsigned>(~n & 0xFFFF));
			b.raw(bytes(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n)));
			at += n;
		}
		while (at < data.size());

		std::uint32_t a = 1;
		std::uint32_t c = 0;

		for (const auto x : data)
		{
			a = (a + x) % 65521;
			c = (c + a) % 65521;
		}

		return b.be32(c << 16 | a).b;
	}

	// A tag read as a scan reads it, with a whole scan's budget to inflate within.
	std::uint64_t read_tag_at(libfiles::reader& r, const std::uint64_t offset, media_scan_result& out)
	{
		libfiles::detail::media_budget budget(libfiles::scan_limits{}.media_inflated_bytes);
		return libfiles::detail::read_id3v2(r, offset, budget, out);
	}

	media_scan_result read_id3(const bytes& file, std::uint64_t* size = nullptr, const std::uint64_t offset = 0)
	{
		libfiles::reader r(file);
		media_scan_result out;
		const auto taken = read_tag_at(r, offset, out);
		if (size) *size = taken;
		return out;
	}

	// The same, through a windowed reader over a stream with small blocks.
	media_scan_result read_id3_streamed(const bytes& file, const std::size_t block = 4096)
	{
		libfiles::memory_stream s(file);
		libfiles::reader r(s, block);
		media_scan_result out;
		read_tag_at(r, 0, out);
		return out;
	}

	std::string value_of(const media_scan_result& out, const std::string_view key)
	{
		const auto it = std::ranges::find_if(out.tags, [key](const libfiles::media_tag& t) { return t.key == key; });
		return it == out.tags.end() ? std::string("(none)") : it->value;
	}

	std::string keys_of(const media_scan_result& out)
	{
		std::string keys;

		for (const auto& t : out.tags)
		{
			if (!keys.empty()) keys += ' ';
			keys += t.key;
		}

		return keys;
	}

	bool same_date(const media_date& d, const int year, const int month, const int day)
	{
		return d.year == year && d.month == month && d.day == day;
	}

	bool same_time(const media_date& d, const int hour, const int minute, const int second)
	{
		return d.has_time && d.hour == hour && d.minute == minute && d.second == second;
	}

	// Whether text reads as this moment, in this zone.
	bool moment(const std::string_view text, const int y, const int m, const int d, const int hh, const int mm, const int ss,
	            const media_date::time_zone zone = media_date::time_zone::unstated, const int offset = 0)
	{
		const auto date = libfiles::detail::parse_media_date(text);
		return same_date(date, y, m, d) && same_time(date, hh, mm, ss) && date.zone == zone && date.offset_minutes == offset;
	}

	// ---- ID3v1, APE and Vorbis comments --------------------------------------------------------------

	bytes fixed(const std::string_view text, const std::size_t size)
	{
		bytes b(size, 0);
		std::copy_n(text.begin(), std::min(size, text.size()), b.begin());
		return b;
	}

	// An ID3v1 tag; a track makes it ID3v1.1.
	bytes id3v1(const std::string_view title, const std::string_view artist, const std::string_view album, const std::string_view year,
	            const std::string_view comment, const int track, const unsigned genre)
	{
		builder b;
		b.raw("TAG").raw(fixed(title, 30)).raw(fixed(artist, 30)).raw(fixed(album, 30)).raw(fixed(year, 4));
		if (track < 0) b.raw(fixed(comment, 30));
		else b.raw(fixed(comment, 28)).u8(0).u8(static_cast<unsigned>(track));
		return b.u8(genre).b;
	}

	bytes ape_item(const std::string_view key, const bytes& value, const std::uint32_t flags = 0)
	{
		return builder().le32(static_cast<std::uint32_t>(value.size())).le32(flags).raw(key).u8(0).raw(value).b;
	}

	// An APE tag: APEv2 with or without its header, or APEv1, which has none.
	bytes ape_tag(const std::vector<bytes>& items, const std::uint32_t version = 2000, const bool header = true)
	{
		const auto body = join(items);
		const auto size = static_cast<std::uint32_t>(body.size() + 32);
		const auto count = static_cast<std::uint32_t>(items.size());

		builder b;
		if (header) b.raw("APETAGEX").le32(version).le32(size).le32(count).le32(0xA0000000).zeros(8);
		b.raw(body);
		return b.raw("APETAGEX").le32(version).le32(size).le32(count).le32(header ? 0x80000000 : 0).zeros(8).b;
	}

	std::string base64(const bytes& data)
	{
		constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;

		for (std::size_t i = 0; i < data.size(); i += 3)
		{
			const auto n = std::min<std::size_t>(3, data.size() - i);
			std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
			if (n > 1) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
			if (n > 2) v |= data[i + 2];

			out += alphabet[v >> 18 & 63];
			out += alphabet[v >> 12 & 63];
			out += n > 1 ? alphabet[v >> 6 & 63] : '=';
			out += n > 2 ? alphabet[v & 63] : '=';
		}

		return out;
	}

	bytes flac_picture(const unsigned type, const std::string_view mime, const std::string_view description, const bytes& image)
	{
		return builder().be32(type).be32(static_cast<std::uint32_t>(mime.size())).raw(mime)
		                .be32(static_cast<std::uint32_t>(description.size())).raw(description)
		                .be32(640).be32(480).be32(24).be32(0).be32(static_cast<std::uint32_t>(image.size())).raw(image).b;
	}

	bytes vorbis_block(const std::string_view vendor, const std::vector<std::string>& fields)
	{
		builder b;
		b.le32(static_cast<std::uint32_t>(vendor.size())).raw(vendor).le32(static_cast<std::uint32_t>(fields.size()));
		for (const auto& f : fields) b.le32(static_cast<std::uint32_t>(f.size())).raw(f);
		return b.b;
	}

	media_properties apply_all(const tag_family family, const std::vector<std::pair<std::string_view, std::string_view>>& tags,
	                           const std::string_view language = {})
	{
		media_properties p;
		for (const auto& [key, value] : tags) libfiles::detail::apply_tag(p, family, key, value, language);
		return p;
	}

	std::string keywords_of(const media_properties& p)
	{
		std::string joined;

		for (const auto& k : p.keywords)
		{
			if (!joined.empty()) joined += '|';
			joined += k;
		}

		return joined;
	}

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-6;
	}
}

TEST("media tags read an ID3v2.3 tag's frames as tags, properties, a rating, a picture and a packet")
{
	const auto cover = image(300);
	const std::vector<bytes> frames = {
		frame(3, "TIT2", text("Colorblind")),
		frame(3, "TPE1", text(1, utf16("Counting Crows"))),
		frame(3, "TALB", text(1, utf16("This Desert Life", true))),
		frame(3, "TPE2", text(2, utf16("Band", true, false))),
		frame(3, "TCON", text("(17)Rock")),
		frame(3, "TRCK", text("7/12")),
		frame(3, "TPOS", text("1/2")),
		frame(3, "TYER", text("1999")),
		frame(3, "TDAT", text("0405")),
		frame(3, "TIME", text("1011")),
		frame(3, "TXXX", text(0, lit("Rip date\0" "2001-02-03"))),
		frame(3, "TXXX", text(0, lit("originalyear\0" "1969"))),
		frame(3, "COMM", builder().u8(0).raw("eng").u8(0).raw("Comments").b),
		frame(3, "COMM", builder().u8(0).raw("eng").raw("iTunNORM").u8(0).raw(" 0000").b),
		frame(3, "POPM", builder().raw("someone@example.com").u8(0).u8(64).be32(3).b),
		frame(3, "POPM", builder().raw("Windows Media Player 9 Series").u8(0).u8(196).be32(0).b),
		frame(3, "PRIV", builder().raw("WM/Provider").u8(0).raw("AMG").u8(0).b),
		frame(3, "PRIV", builder().raw("XMP").u8(0).raw("<x:xmpmeta/>").u8(0).u8(0).b),
		frame(3, "PRIV", builder().raw("XMP").u8(0).raw("<second/>").b),
		frame(3, "APIC", picture_body(3, "image/jpeg", 3, "Cover", cover)),
		frame(3, "MCDI", bytes(29, 1)),
		frame(3, "WOAR", to_bytes("http://example.com/artist")),
	};

	const auto tag = id3_tag(3, frames, 100);
	const auto file = join({tag, bytes(64, 0xEE)});
	std::uint64_t size = 0;
	const auto out = read_id3(file, &size);

	CHECK_EQ(size, std::uint64_t{tag.size()});
	CHECK_STR(keys_of(out), "TIT2 TPE1 TALB TPE2 TCON TRCK TPOS TYER TDAT TIME TXXX:Rip date TXXX:originalyear COMM COMM:iTunNORM "
	          "POPM:someone@example.com POPM:Windows Media Player 9 Series PRIV:WM/Provider PRIV:XMP PRIV:XMP APIC MCDI WOAR");
	CHECK_STR(value_of(out, "TPE1"), "Counting Crows");
	CHECK_STR(value_of(out, "TCON"), "(17)Rock");
	CHECK_STR(value_of(out, "TXXX:Rip date"), "2001-02-03");
	CHECK_STR(value_of(out, "COMM:iTunNORM"), "0000");
	CHECK_STR(value_of(out, "POPM:Windows Media Player 9 Series"), "196");
	CHECK_STR(value_of(out, "PRIV:WM/Provider"), "4 bytes");
	CHECK_STR(value_of(out, "APIC"), "image/jpeg, front cover, 300 bytes");
	CHECK_STR(value_of(out, "MCDI"), "29 bytes");
	CHECK_STR(value_of(out, "WOAR"), "http://example.com/artist");
	CHECK(out.tags.size() > 12 && out.tags[12].language == "eng");

	const auto& p = out.properties;
	CHECK_STR(p.title, "Colorblind");
	CHECK_STR(p.artist, "Counting Crows");
	CHECK_STR(p.album, "This Desert Life");
	CHECK_STR(p.album_artist, "Band");
	CHECK_STR(p.genre, "Rock");
	CHECK_EQ(p.track, 7);
	CHECK_EQ(p.track_count, 12);
	CHECK_EQ(p.disc, 1);
	CHECK_EQ(p.disc_count, 2);
	CHECK_EQ(p.year, 1999);
	CHECK(same_date(p.created, 1999, 5, 4) && same_time(p.created, 10, 11, 0));
	CHECK(p.created.zone == media_date::time_zone::unstated);
	CHECK(same_date(p.original_release, 1969, 0, 0));
	CHECK(p.comments.size() == 1 && p.comments[0].text == "Comments" && p.comments[0].language == "eng");
	CHECK(p.rating == 4);

	// Located where the image lies in the file, not read.
	CHECK_EQ(out.pictures.size(), std::size_t{1});

	if (!out.pictures.empty())
	{
		const auto& picture = out.pictures[0];
		CHECK_EQ(picture.location.offset, std::uint64_t{find_bytes(file, cover)});
		CHECK_EQ(picture.location.size, std::uint64_t{300});
		CHECK(picture.data.empty());
		CHECK_STR(picture.mime_type, "image/jpeg");
		CHECK_EQ(picture.type, 3);
		CHECK_STR(picture.description, "Cover");
	}

	// The first packet, its terminators dropped, as read_mp3_packet takes it.
	CHECK_STR(to_string(out.xmp), "<x:xmpmeta/>");

	// The same through a stream read in small windows.
	const auto streamed = read_id3_streamed(file, 64);
	CHECK_STR(keys_of(streamed), keys_of(out));
	CHECK(streamed.pictures.size() == 1 && !out.pictures.empty() && streamed.pictures[0].location == out.pictures[0].location);
}

TEST("media tags read ID3v2.4's encodings, several values, footer and extended header")
{
	const auto long_comment = std::string(300, 'c');
	const std::vector<bytes> frames = {
		frame(4, "TIT2", text(3, to_bytes("Zo\xC3\xAB"))),
		frame(4, "TPE1", text(3, lit("A\0B\0"))),
		frame(4, "TPE2", text(1, join({utf16("X"), bytes{0, 0}, utf16("Y", true), bytes{0, 0}, utf16("Z", true, false)}))),
		frame(4, "TCON", text(0, lit("17\0Pop\0RX\0(13)"))),
		frame(4, "TDRC", text("2011-03-13T15:13:49")),
		frame(4, "TDOR", text("1969-07-01")),
		frame(4, "TDRL", text("2012")),
		frame(4, "COMM", builder().u8(0).raw("XXX").u8(0).raw(long_comment).b),
		frame(4, "TXXX", text(3, lit("CATALOGNUMBER\0" "ABC\0" "DEF"))),
		frame(4, "WXXX", builder().u8(1).raw(utf16("Home")).u8(0).u8(0).raw("http://example.com/").b),
		frame(4, "TSSE", text("LAME 3.100")),
	};

	// An extended header of six bytes, its size syncsafe and counting itself.
	const auto extended = join({syncsafe(6), bytes{1, 0}});
	const auto tag = id3_tag(4, frames, 20, 0x50, extended);
	std::uint64_t size = 0;
	const auto out = read_id3(join({tag, bytes(16, 0xEE)}), &size);

	CHECK_EQ(size, std::uint64_t{tag.size()});
	CHECK_STR(keys_of(out), "TIT2 TPE1 TPE2 TCON TDRC TDOR TDRL COMM TXXX:CATALOGNUMBER WXXX:Home TSSE");
	CHECK_STR(value_of(out, "TPE1"), "A; B");
	CHECK_STR(value_of(out, "TPE2"), "X; Y; Z");
	CHECK_STR(value_of(out, "TXXX:CATALOGNUMBER"), "ABC; DEF");
	CHECK_STR(value_of(out, "WXXX:Home"), "http://example.com/");
	CHECK_STR(value_of(out, "COMM"), long_comment);

	const auto& p = out.properties;
	CHECK_STR(p.title, "Zo\xC3\xAB");
	CHECK_STR(p.artist, "A; B");
	CHECK_STR(p.genre, "Rock; Pop; Remix");
	CHECK_STR(p.encoder, "LAME 3.100");
	CHECK(same_date(p.created, 2011, 3, 13) && same_time(p.created, 15, 13, 49));
	CHECK_EQ(p.year, 2011);
	CHECK(same_date(p.original_release, 1969, 7, 1));
	CHECK(p.comments.size() == 1 && p.comments[0].language.empty());
	CHECK(!p.rating);
}

TEST("media tags read ID3v2.2's three-letter frames and PIC")
{
	const auto cover = image(64, 11);
	const std::vector<bytes> frames = {
		frame(2, "TT2", text("Song")),
		frame(2, "TP1", text("Artist")),
		frame(2, "TAL", text("Album")),
		frame(2, "TCO", text("(13)")),
		frame(2, "TYE", text("1987")),
		frame(2, "TDA", text("3112")),
		frame(2, "TXX", text(0, lit("Rip date\0" "x"))),
		frame(2, "COM", builder().u8(0).raw("eng").u8(0).raw("Hello").b),
		frame(2, "POP", builder().raw("Windows Media Player 9 Series").u8(0).u8(255).b),
		frame(2, "PRV", builder().raw("XMP").u8(0).raw("<x/>").b),
		frame(2, "PIC", picture_body(2, "PNG", 4, "Back", cover)),
	};

	const auto tag = id3_tag(2, frames, 10);
	const auto file = join({bytes(50, 0xAA), tag});
	std::uint64_t size = 0;
	const auto out = read_id3(file, &size, 50);

	CHECK_EQ(size, std::uint64_t{tag.size()});
	CHECK_STR(keys_of(out), "TT2 TP1 TAL TCO TYE TDA TXX:Rip date COM POP:Windows Media Player 9 Series PRV:XMP PIC");
	CHECK_STR(value_of(out, "PIC"), "image/png, back cover, 64 bytes");

	const auto& p = out.properties;
	CHECK_STR(p.title, "Song");
	CHECK_STR(p.artist, "Artist");
	CHECK_STR(p.album, "Album");
	CHECK_STR(p.genre, "Pop");
	CHECK(same_date(p.created, 1987, 12, 31) && !p.created.has_time);
	CHECK(p.comments.size() == 1 && p.comments[0].text == "Hello");
	CHECK(p.rating == 5);
	CHECK_STR(to_string(out.xmp), "<x/>");
	CHECK_EQ(out.pictures.size(), std::size_t{1});

	if (!out.pictures.empty())
	{
		CHECK_STR(out.pictures[0].mime_type, "image/png");
		CHECK_EQ(out.pictures[0].type, 4);
		CHECK_EQ(out.pictures[0].location.offset, std::uint64_t{find_bytes(file, cover)});
		CHECK_EQ(out.pictures[0].location.size, std::uint64_t{64});
	}
}

TEST("media tags undo a whole ID3v2.2 or 2.3 tag's unsynchronisation, copying its picture")
{
	// Large enough to span several of the blocks the tag is resynchronised in.
	const auto cover = image(150000);
	auto last = image(40, 5);
	last.back() = 0xFF;

	const std::vector<bytes> frames = {
		frame(3, "TIT2", text(0, bytes{0xFF, 0xFF, 'x'})),
		frame(3, "APIC", picture_body(3, "image/jpeg", 3, "Cover", cover)),
		frame(3, "TALB", text("After the picture")),
		// The tag's last byte is an 0xFF, which takes a zero after it.
		frame(3, "PRIV", join({to_bytes("X"), bytes{0}, last})),
	};

	// ID3v2.3's extended header is unsynchronised with the frames; its size does not count itself.
	const auto extended = builder().be32(6).be16(0).be32(0xFFE0FF00).b;
	const auto tag = unsynchronised_tag(3, frames, 0, extended);
	const auto file = join({tag, bytes(8, 0xEE)});
	CHECK(tag.size() > 10 + extended.size() + join(frames).size());

	std::uint64_t size = 0;
	const auto out = read_id3(file, &size);
	CHECK_EQ(size, std::uint64_t{tag.size()});
	CHECK_STR(keys_of(out), "TIT2 APIC TALB PRIV:X");
	CHECK_STR(out.properties.title, "\xC3\xBF\xC3\xBFx");
	CHECK_STR(out.properties.album, "After the picture");
	CHECK_STR(value_of(out, "APIC"), "image/jpeg, front cover, 150000 bytes");
	CHECK_STR(value_of(out, "PRIV:X"), "40 bytes");

	// Its bytes as stored are not the image's, so the picture is copied.
	CHECK_EQ(out.pictures.size(), std::size_t{1});
	CHECK(!out.pictures.empty() && out.pictures[0].location.empty() && out.pictures[0].data == cover);

	for (const std::size_t block : {std::size_t{4096}, std::size_t{100000}})
	{
		const auto streamed = read_id3_streamed(file, block);
		CHECK_STR(keys_of(streamed), keys_of(out));
		CHECK(streamed.pictures.size() == 1 && streamed.pictures[0].data == cover);
	}

	const auto small = image(90, 13);
	const auto old = unsynchronised_tag(2, {frame(2, "TT2", text(0, bytes{'a', 0xFF, 0xFE})), frame(2, "PIC", picture_body(2, "JPG", 0, "", small))});
	const auto v22 = read_id3(old);
	CHECK_STR(v22.properties.title, "a\xC3\xBF\xC3\xBE");
	CHECK(v22.pictures.size() == 1 && v22.pictures[0].data == small && v22.pictures[0].mime_type == "image/jpeg" && v22.pictures[0].type == 0);
}

TEST("media tags undo ID3v2.4's frame unsynchronisation, data lengths, groups and compression")
{
	const auto cover = image(5000);
	const auto located = image(700, 3);
	const auto title = text(1, utf16("Unsynchronised"));
	const auto album = text("Compressed album");
	const auto artist = text("Grouped!");
	const auto picture = picture_body(4, "image/png", 3, "", cover);
	const auto plain = picture_body(4, "image/gif", 4, "Plain", located);

	const std::vector<bytes> frames = {
		frame(4, "TIT2", unsynchronise(join({syncsafe(static_cast<std::uint32_t>(title.size())), title})), 0x03),
		frame(4, "TALB", join({syncsafe(static_cast<std::uint32_t>(album.size())), zlib_stored(album)}), 0x09),
		frame(4, "TPE1", join({bytes{7}, syncsafe(static_cast<std::uint32_t>(artist.size())), artist}), 0x41),
		// Encrypted: skipped, as there is no key to read it with.
		frame(4, "TCOM", join({bytes{0x80}, bytes(12, 0x55)}), 0x04),
		frame(4, "APIC", join({syncsafe(static_cast<std::uint32_t>(picture.size())), zlib_stored(picture)}), 0x09),
		frame(4, "APIC", unsynchronise(picture), 0x02),
		frame(4, "APIC", join({syncsafe(static_cast<std::uint32_t>(plain.size())), plain}), 0x01),
	};

	const auto file = id3_tag(4, frames, 10);
	const auto out = read_id3(file);
	CHECK_STR(keys_of(out), "TIT2 TALB TPE1 APIC APIC APIC");
	CHECK_STR(out.properties.title, "Unsynchronised");
	CHECK_STR(out.properties.album, "Compressed album");
	CHECK_STR(out.properties.artist, "Grouped!");
	CHECK(out.properties.composer.empty());

	CHECK_EQ(out.pictures.size(), std::size_t{3});

	if (out.pictures.size() == 3)
	{
		CHECK(out.pictures[0].data == cover && out.pictures[0].location.empty() && out.pictures[0].mime_type == "image/png");
		CHECK(out.pictures[1].data == cover && out.pictures[1].location.empty());
		CHECK(out.pictures[2].data.empty() && out.pictures[2].description == "Plain" && out.pictures[2].type == 4);
		CHECK_EQ(out.pictures[2].location.offset, std::uint64_t{find_bytes(file, located)});
		CHECK_EQ(out.pictures[2].location.size, std::uint64_t{700});
	}

	// The tag's own flag unsynchronises every frame.
	const auto whole = read_id3(id3_tag(4, {frame(4, "TIT2", unsynchronise(text(1, utf16("Whole"))))}, 0, 0x80));
	CHECK_STR(whole.properties.title, "Whole");

	// ID3v2.3 orders a frame's additions as its flags do: decompressed size, encryption method, group.
	const auto old_title = text("Old compressed");
	const auto both = text("Both");
	const std::vector<bytes> old_frames = {
		frame(3, "TIT2", join({builder().be32(static_cast<std::uint32_t>(old_title.size())).b, zlib_stored(old_title)}), 0x0080),
		frame(3, "TPE1", join({bytes{9}, text("Grouped")}), 0x0020),
		frame(3, "TALB", join({bytes{1}, bytes(10, 0x33)}), 0x0040),
		frame(3, "TCOM", join({builder().be32(static_cast<std::uint32_t>(both.size())).b, bytes{9}, zlib_stored(both)}), 0x00A0),
		// Damaged compressed data is described, not read.
		frame(3, "TPE2", join({builder().be32(10).b, bytes{0x78, 0x01, 0x05}}), 0x0080),
	};

	const auto v23 = read_id3(id3_tag(3, old_frames));
	CHECK_STR(keys_of(v23), "TIT2 TPE1 TCOM TPE2");
	CHECK_STR(v23.properties.title, "Old compressed");
	CHECK_STR(v23.properties.artist, "Grouped");
	CHECK_STR(v23.properties.composer, "Both");
	CHECK(v23.properties.album.empty() && v23.properties.album_artist.empty());
	CHECK_STR(value_of(v23, "TPE2"), "7 bytes");
}

TEST("media tags read the ID3v2.4 tags iTunes wrote with plain sizes")
{
	// 295 is 00 00 01 27: no byte has its high bit, so read as syncsafe it is 167 and the walk goes astray.
	const std::vector<bytes> frames = {
		frame(4, "TIT2", text("Before"), 0, true),
		frame(4, "COMM", builder().u8(0).raw("eng").u8(0).raw(std::string(290, 'n')).b, 0, true),
		frame(4, "TALB", text(std::string(199, 'a')), 0, true),
		frame(4, "TPE1", text("After"), 0, true),
	};

	const auto out = read_id3(id3_tag(4, frames, 30));
	CHECK_STR(keys_of(out), "TIT2 COMM TALB TPE1");
	CHECK_STR(out.properties.title, "Before");
	CHECK_STR(out.properties.artist, "After");
	CHECK(out.properties.comments.size() == 1 && out.properties.comments[0].text.size() == 290);

	// A standard tag damaged partway is not taken for one: it keeps what came before the damage.
	const auto damaged = join({frame(4, "TIT2", text("Kept")), frame(4, "COMM", builder().u8(0).raw("eng").u8(0).raw(std::string(290, 'n')).b),
	                           to_bytes("not a frame at all")});
	const auto kept = read_id3(id3_tag(4, {damaged}, 30));
	CHECK_STR(keys_of(kept), "TIT2 COMM");
}

TEST("media tags find no ID3v2 tag where none can be read")
{
	const auto good = id3_tag(3, {frame(3, "TIT2", text("Title"))}, 10);
	std::uint64_t size = 0;
	CHECK_STR(read_id3(good, &size).properties.title, "Title");
	CHECK_EQ(size, std::uint64_t{good.size()});

	const auto refused = [](const bytes& b)
	{
		std::uint64_t taken = 1;
		const auto out = read_id3(b, &taken);
		return taken == 0 && out.tags.empty();
	};

	auto not_id3 = good;
	not_id3[2] = '2';
	CHECK(refused(not_id3));

	for (const auto version : {0, 1, 5, 0xFF})
	{
		auto unknown = good;
		unknown[3] = static_cast<std::uint8_t>(version);
		CHECK(refused(unknown));
	}

	auto revision = good;
	revision[4] = 0xFF;
	CHECK(refused(revision));

	auto size_bit = good;
	size_bit[9] |= 0x80;
	CHECK(refused(size_bit));

	CHECK(refused(bytes(good.begin(), good.begin() + 9)));

	libfiles::reader r(good);
	media_scan_result out;
	CHECK_EQ(read_tag_at(r, good.size(), out), std::uint64_t{0});
	CHECK_EQ(read_tag_at(r, good.size() + 100, out), std::uint64_t{0});
	CHECK_EQ(read_tag_at(r, std::numeric_limits<std::uint64_t>::max(), out), std::uint64_t{0});

	// ID3v2.2's compression was never defined: the tag's size is known, its frames are not.
	const auto compressed = id3_tag(2, {frame(2, "TT2", text("Title"))}, 0, 0x40);
	CHECK(read_id3(compressed, &size).tags.empty());
	CHECK_EQ(size, std::uint64_t{compressed.size()});

	// Nor are a tag's frames after an extended header whose size is damaged.
	for (const auto length : {0u, 1000u})
	{
		const auto extended = id3_tag(3, {frame(3, "TIT2", text("Title"))}, 0, 0x40, builder().be32(length).be16(0).be32(0).b);
		CHECK(read_id3(extended, &size).tags.empty());
		CHECK_EQ(size, std::uint64_t{extended.size()});
	}
}

TEST("media tags keep what an ID3v2 tag held before it was cut short or damaged")
{
	const auto cover = image(200);

	for (const auto version : {2, 3, 4})
	{
		const auto v2 = version == 2;
		const std::vector<bytes> frames = {
			frame(version, v2 ? "TT2" : "TIT2", text("Title")),
			frame(version, v2 ? "TP1" : "TPE1", text(1, utf16("Artist"))),
			frame(version, v2 ? "PIC" : "APIC", picture_body(version, v2 ? "JPG" : "image/jpeg", 3, "", cover)),
			frame(version, v2 ? "TAL" : "TALB", text("Album")),
		};

		const auto tag = id3_tag(version, frames, 16);
		std::vector<std::size_t> ends;
		auto end = std::size_t{10};

		for (const auto& f : frames)
		{
			end += f.size();
			ends.push_back(end);
		}

		for (std::size_t n = 0; n <= tag.size(); ++n)
		{
			const bytes cut(tag.begin(), tag.begin() + static_cast<std::ptrdiff_t>(n));
			const auto out = read_id3(cut);
			const auto whole = static_cast<std::size_t>(std::ranges::count_if(ends, [n](const std::size_t e) { return e <= n; }));
			CHECK_EQ(out.tags.size(), whole);
			CHECK(std::ranges::all_of(out.pictures, [n](const libfiles::media_picture& p) { return p.location.offset + p.location.size <= n; }));
		}

		// A frame whose size claims more than the tag holds ends the frames before it.
		auto at = std::size_t{10};

		for (std::size_t i = 0; i < frames.size(); ++i)
		{
			for (const std::uint8_t damage : {std::uint8_t{0x7F}, std::uint8_t{0xFF}})
			{
				auto damaged = tag;
				for (std::size_t b = v2 ? 3 : 4; b < (v2 ? 6u : 8u); ++b) damaged[at + b] = damage;
				CHECK_EQ(read_id3(damaged).tags.size(), i);
			}

			at += frames[i].size();
		}
	}

	// Cut short, an unsynchronised tag keeps the frames before the cut too.
	const auto unsynchronised = unsynchronised_tag(3, {frame(3, "TIT2", text("Title")), frame(3, "APIC", picture_body(3, "image/jpeg", 3, "", cover)),
	                                                   frame(3, "TALB", text("Album"))});
	std::size_t previous = 0;

	for (std::size_t n = 0; n <= unsynchronised.size(); ++n)
	{
		const auto out = read_id3(bytes(unsynchronised.begin(), unsynchronised.begin() + static_cast<std::ptrdiff_t>(n)));
		CHECK(out.tags.size() >= previous);
		previous = out.tags.size();
		CHECK(std::ranges::all_of(out.pictures, [&cover](const libfiles::media_picture& p) { return p.data == cover; }));
	}

	CHECK_EQ(previous, std::size_t{3});
}

TEST("media tags keep what an ID3v2 tag held before its file failed to read")
{
	// A stream that fails every read past a point, as a file on a share may.
	class failing final : public libfiles::stream
	{
	public:
		failing(const bytes& b, const std::uint64_t limit) : _b(b), _limit(limit)
		{
		}

		std::uint64_t size() override { return _b.size(); }

		std::size_t read(const std::uint64_t offset, void* const data, const std::size_t size) override
		{
			if (offset + size > _limit) return 0;
			std::memcpy(data, _b.data() + offset, size);
			return size;
		}

	private:
		const bytes& _b;
		std::uint64_t _limit;
	};

	const auto cover = image(3000);
	const std::vector<bytes> frames = {
		frame(3, "TIT2", text("Title")),
		frame(3, "APIC", picture_body(3, "image/jpeg", 3, "", cover)),
		frame(3, "TALB", text("Album")),
		frame(3, "TPE1", text("Artist")),
	};

	for (const auto unsynchronised : {false, true})
	{
		const auto file = join({unsynchronised ? unsynchronised_tag(3, frames) : id3_tag(3, frames, 20), bytes(100, 0xEE)});
		const auto all = keys_of(read_id3(file));
		CHECK_STR(all, "TIT2 APIC TALB TPE1");

		for (std::uint64_t limit = 0; limit <= file.size(); limit += 97)
		{
			failing s(file, limit);
			libfiles::reader r(s, 256);
			media_scan_result out;
			read_tag_at(r, 0, out);
			CHECK(all.starts_with(keys_of(out)));
		}
	}
}

TEST("media tags read ID3v1 and ID3v1.1, taking only what the file's other tags lack")
{
	const auto tag = id3v1("Caf\xE9 song   ", "Artist", "Album", "1999", "A comment", 7, 17);
	const auto file = join({bytes(72, 0xEE), tag});
	libfiles::reader r(file);
	media_scan_result out;
	CHECK(libfiles::detail::read_id3v1(r, 72, out));

	CHECK_STR(keys_of(out), "ID3v1:title ID3v1:artist ID3v1:album ID3v1:year ID3v1:comment ID3v1:track ID3v1:genre");
	CHECK_STR(value_of(out, "ID3v1:title"), "Caf\xC3\xA9 song");
	CHECK_STR(value_of(out, "ID3v1:genre"), "Rock");
	CHECK_STR(out.properties.title, "Caf\xC3\xA9 song");
	CHECK_STR(out.properties.genre, "Rock");
	CHECK_EQ(out.properties.track, 7);
	CHECK_EQ(out.properties.year, 1999);
	CHECK(!out.properties.created.valid());
	CHECK(out.properties.comments.size() == 1 && out.properties.comments[0].text == "A comment");

	// ID3v1.0's comment takes all thirty bytes, and 255 names no genre.
	const auto v10 = id3v1("T", "", "", "", std::string(30, 'c'), -1, 255);
	libfiles::reader r10(v10);
	media_scan_result plain;
	CHECK(libfiles::detail::read_id3v1(r10, 0, plain));
	CHECK_STR(keys_of(plain), "ID3v1:title ID3v1:comment");
	CHECK_STR(value_of(plain, "ID3v1:comment"), std::string(30, 'c'));
	CHECK_EQ(plain.properties.track, 0);

	// What another tag already set stands.
	media_scan_result tagged;
	tagged.properties.title = "From ID3v2";
	tagged.properties.year = 2001;
	tagged.properties.created = libfiles::detail::parse_media_date("2001-02-03");
	tagged.properties.comments.push_back({"Kept", ""});
	tagged.properties.track = 2;
	CHECK(libfiles::detail::read_id3v1(r, 72, tagged));
	CHECK_STR(tagged.properties.title, "From ID3v2");
	CHECK_STR(tagged.properties.artist, "Artist");
	CHECK_EQ(tagged.properties.year, 2001);
	CHECK(same_date(tagged.properties.created, 2001, 2, 3));
	CHECK(tagged.properties.comments.size() == 1 && tagged.properties.comments[0].text == "Kept");
	CHECK_EQ(tagged.properties.track, 2);

	media_scan_result none;
	CHECK(!libfiles::detail::read_id3v1(r, 0, none));
	CHECK(!libfiles::detail::read_id3v1(r, 73, none));
	CHECK(!libfiles::detail::read_id3v1(r, std::numeric_limits<std::uint64_t>::max(), none));
	CHECK(none.tags.empty());
}

TEST("media tags read APEv2 and APEv1 tags from their footers, locating cover art")
{
	const auto front = image(500);
	const auto back = image(64, 5);
	const auto tag = ape_tag({
		ape_item("Title", to_bytes("Song")),
		ape_item("Artist", lit("One\0Two")),
		ape_item("Album Artist", to_bytes("Band")),
		ape_item("Year", to_bytes("2004-05-03")),
		ape_item("Track", to_bytes("3/10")),
		ape_item("DISC", to_bytes("1/2")),
		ape_item("genre", to_bytes("Jazz")),
		ape_item("Comment", to_bytes("Nice")),
		ape_item("Rating", to_bytes("4")),
		ape_item("Cover Art (Front)", join({to_bytes("cover.jpg"), bytes{0}, front}), 2),
		ape_item("Cover Art (Back)", join({to_bytes("back"), bytes{0}, back}), 2),
		ape_item("Data", bytes(10, 1), 2),
		ape_item("Related", to_bytes("http://example.com/"), 4),
	});

	const auto audio = bytes(100, 0xEE);
	const auto v1 = id3v1("Title", "", "", "", "", -1, 255);
	const auto file = join({audio, tag, v1});
	libfiles::reader r(file);
	media_scan_result out;
	CHECK_EQ(libfiles::detail::read_ape_tag(r, file.size() - 128, out), std::uint64_t{tag.size()});

	CHECK_STR(keys_of(out), "Title Artist Album Artist Year Track DISC genre Comment Rating Cover Art (Front) Cover Art (Back) Data Related");
	CHECK_STR(value_of(out, "Artist"), "One; Two");
	CHECK_STR(value_of(out, "Cover Art (Front)"), "image/jpeg, front cover, 500 bytes");
	CHECK_STR(value_of(out, "Data"), "10 bytes");
	CHECK_STR(value_of(out, "Related"), "http://example.com/");

	const auto& p = out.properties;
	CHECK_STR(p.title, "Song");
	CHECK_STR(p.artist, "One; Two");
	CHECK_STR(p.album_artist, "Band");
	CHECK_STR(p.genre, "Jazz");
	CHECK(same_date(p.created, 2004, 5, 3));
	CHECK_EQ(p.year, 2004);
	CHECK(p.track == 3 && p.track_count == 10 && p.disc == 1 && p.disc_count == 2);
	CHECK(p.comments.size() == 1 && p.comments[0].text == "Nice");
	CHECK(p.rating == 4);

	CHECK_EQ(out.pictures.size(), std::size_t{2});

	if (out.pictures.size() == 2)
	{
		CHECK_EQ(out.pictures[0].location.offset, std::uint64_t{find_bytes(file, front)});
		CHECK_EQ(out.pictures[0].location.size, std::uint64_t{500});
		CHECK(out.pictures[0].mime_type == "image/jpeg" && out.pictures[0].type == 3 && out.pictures[0].description == "cover.jpg");
		CHECK_EQ(out.pictures[1].location.offset, std::uint64_t{find_bytes(file, back)});
		CHECK(out.pictures[1].mime_type.empty() && out.pictures[1].type == 4);
	}

	// Without its header the tag is the footer's size; APEv1 has none, and Latin-1 text.
	const auto bare = ape_tag({ape_item("Title", to_bytes("Bare"))}, 2000, false);
	libfiles::reader rb(bare);
	media_scan_result b;
	CHECK_EQ(libfiles::detail::read_ape_tag(rb, bare.size(), b), std::uint64_t{bare.size()});
	CHECK_STR(b.properties.title, "Bare");

	const auto old = ape_tag({ape_item("Title", to_bytes("Caf\xE9"), 2)}, 1000, false);
	libfiles::reader ro(old);
	media_scan_result o;
	CHECK_EQ(libfiles::detail::read_ape_tag(ro, old.size(), o), std::uint64_t{old.size()});
	CHECK_STR(o.properties.title, "Caf\xC3\xA9");
	CHECK(o.pictures.empty());

	// A binary item is described, and the description is no property's value.
	const auto binary = ape_tag({ape_item("Title", to_bytes("Binary"), 2)});
	libfiles::reader rbin(binary);
	media_scan_result bin;
	libfiles::detail::read_ape_tag(rbin, binary.size(), bin);
	CHECK_STR(value_of(bin, "Title"), "6 bytes");
	CHECK(bin.properties.title.empty());

	media_scan_result none;
	CHECK_EQ(libfiles::detail::read_ape_tag(r, file.size(), none), std::uint64_t{0});
	CHECK_EQ(libfiles::detail::read_ape_tag(r, 10, none), std::uint64_t{0});
	CHECK_EQ(libfiles::detail::read_ape_tag(r, file.size() + 1, none), std::uint64_t{0});
	CHECK(none.tags.empty());
}

TEST("media tags keep what an APE tag held before its damage")
{
	const std::vector<bytes> items = {
		ape_item("Title", to_bytes("Song")),
		ape_item("Artist", to_bytes("Artist")),
		ape_item("Cover Art (Front)", join({to_bytes("a.png"), bytes{0}, image(300)}), 2),
		ape_item("Album", to_bytes("Album")),
	};

	const auto tag = ape_tag(items);
	const auto items_at = std::size_t{32};

	// Only the tag that ends where it is looked for is found.
	for (std::size_t n = 0; n <= tag.size(); ++n)
	{
		const bytes cut(tag.begin(), tag.begin() + static_cast<std::ptrdiff_t>(n));
		libfiles::reader r(cut);
		media_scan_result out;
		CHECK_EQ(libfiles::detail::read_ape_tag(r, n, out), n == tag.size() ? std::uint64_t{tag.size()} : std::uint64_t{0});
	}

	// A footer whose size is wrong reads no further than its bytes allow.
	const auto size_at = tag.size() - 32 + 12;

	for (auto d = -40; d <= 40; ++d)
	{
		auto damaged = tag;
		const auto size = static_cast<std::uint32_t>(static_cast<int>(tag.size() - 32) + d);
		for (auto i = 0; i < 4; ++i) damaged[size_at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
		libfiles::reader r(damaged);
		media_scan_result out;
		libfiles::detail::read_ape_tag(r, damaged.size(), out);
		if (d == 0) CHECK_EQ(out.tags.size(), std::size_t{4});
		CHECK(out.tags.size() <= 4);
	}

	// An item whose size runs past the tag ends the items before it.
	auto at = items_at;

	for (std::size_t i = 0; i < items.size(); ++i)
	{
		auto damaged = tag;
		damaged[at + 3] = 0x7F;
		libfiles::reader r(damaged);
		media_scan_result out;
		CHECK_EQ(libfiles::detail::read_ape_tag(r, damaged.size(), out), std::uint64_t{tag.size()});
		CHECK_EQ(out.tags.size(), i);
		at += items[i].size();
	}

	// A count past the items stops at the footer; a key without its NUL stops the items.
	auto counted = tag;
	counted[tag.size() - 32 + 16] = 200;
	libfiles::reader rc(counted);
	media_scan_result c;
	libfiles::detail::read_ape_tag(rc, counted.size(), c);
	CHECK_EQ(c.tags.size(), std::size_t{4});

	auto unended = ape_tag({ape_item("Title", to_bytes("Song")), ape_item(std::string(300, 'K'), to_bytes("x"))});
	libfiles::reader ru(unended);
	media_scan_result u;
	libfiles::detail::read_ape_tag(ru, unended.size(), u);
	CHECK_STR(keys_of(u), "Title");
}

TEST("media tags read Vorbis comments, their pictures and their vendor")
{
	const auto front = image(400);
	const auto legacy = image(120, 9);
	const auto block = vorbis_block("Xiph.Org libVorbis I 20200704", {
		"TITLE=Song",
		"ARTIST=One",
		"artist=Two",
		"ALBUMARTIST=Band",
		"DATE=2004-05-03",
		"TRACKNUMBER=3",
		"TRACKTOTAL=12",
		"DISCNUMBER=1/2",
		"COMMENT=First",
		"DESCRIPTION=Second",
		"ORGANIZATION=Label",
		"RATING=80",
		"ORIGINALYEAR=1969",
		"GENRE=",
		"NOEQUALS",
		"=no name",
		"METADATA_BLOCK_PICTURE=" + base64(flac_picture(3, "image/jpeg", "Front", front)),
		"COVERART=" + base64(legacy),
		"COVERARTMIME=image/png",
		"METADATA_BLOCK_PICTURE=not base64!",
	});

	media_scan_result out;
	CHECK(libfiles::detail::read_vorbis_comments(block, out));
	CHECK_STR(keys_of(out), "vendor TITLE ARTIST ALBUMARTIST DATE TRACKNUMBER TRACKTOTAL DISCNUMBER COMMENT DESCRIPTION ORGANIZATION "
	          "RATING ORIGINALYEAR METADATA_BLOCK_PICTURE COVERART COVERARTMIME METADATA_BLOCK_PICTURE");
	CHECK_STR(value_of(out, "vendor"), "Xiph.Org libVorbis I 20200704");
	CHECK_STR(value_of(out, "ARTIST"), "One; Two");
	CHECK_STR(value_of(out, "METADATA_BLOCK_PICTURE"), "image/jpeg, front cover, 400 bytes");
	CHECK_STR(value_of(out, "COVERART"), "front cover, 120 bytes");

	const auto& p = out.properties;
	CHECK_STR(p.title, "Song");
	CHECK_STR(p.artist, "One; Two");
	CHECK_STR(p.album_artist, "Band");
	CHECK(same_date(p.created, 2004, 5, 3));
	CHECK(p.track == 3 && p.track_count == 12 && p.disc == 1 && p.disc_count == 2);
	CHECK(p.comments.size() == 2 && p.comments[0].text == "First" && p.comments[1].text == "Second");
	CHECK_STR(p.publisher, "Label");
	CHECK(p.rating == 4);
	CHECK(same_date(p.original_release, 1969, 0, 0));
	CHECK(p.genre.empty());
	CHECK_STR(p.encoder, "Xiph.Org libVorbis I 20200704");

	CHECK_EQ(out.pictures.size(), std::size_t{2});

	if (out.pictures.size() == 2)
	{
		CHECK(out.pictures[0].data == front && out.pictures[0].location.empty());
		CHECK(out.pictures[0].mime_type == "image/jpeg" && out.pictures[0].type == 3 && out.pictures[0].description == "Front");
		CHECK(out.pictures[1].data == legacy && out.pictures[1].mime_type == "image/png");
	}

	// An encoder field is the encoder; so is what another tag already named.
	media_scan_result named;
	CHECK(libfiles::detail::read_vorbis_comments(vorbis_block("libFLAC 1.4.3", {"ENCODER=Lavf61"}), named));
	CHECK_STR(named.properties.encoder, "Lavf61");

	media_scan_result earlier;
	earlier.properties.encoder = "From ID3v2";
	CHECK(libfiles::detail::read_vorbis_comments(vorbis_block("libFLAC 1.4.3", {}), earlier));
	CHECK_STR(earlier.properties.encoder, "From ID3v2");
}

TEST("media tags join a Vorbis field name that repeats into one tag, but not its pictures")
{
	const auto front = image(64);
	const auto back = image(48, 5);
	const auto block = vorbis_block("vendor", {
		"ARTIST=A",
		"TITLE=Song",
		"artist=B",
		"Artist=",
		"ARTIST=C",
		"GENRE=",
		"KEYWORDS=one,two",
		"COMMENT=First",
		"keywords=Two; three",
		"COMMENT=Second",
		"DATE=2004-05-03",
		"DATE=2011",
		"TRACKNUMBER=3",
		"TRACKNUMBER=4/9",
		"RATING=4",
		"RATING=1",
		"METADATA_BLOCK_PICTURE=" + base64(flac_picture(3, "image/jpeg", "", front)),
		"COVERART=" + base64(front),
		"COVERARTMIME=image/jpeg",
		"genre=Rock",
		"METADATA_BLOCK_PICTURE=" + base64(flac_picture(4, "image/png", "", back)),
		"COVERART=" + base64(back),
		"COVERARTMIME=image/png",
	});

	media_scan_result out;
	CHECK(libfiles::detail::read_vorbis_comments(block, out));

	// One tag a name, where it first appears, its values in file order; each picture its own.
	CHECK_STR(keys_of(out), "vendor ARTIST TITLE GENRE KEYWORDS COMMENT DATE TRACKNUMBER RATING METADATA_BLOCK_PICTURE COVERART COVERARTMIME "
	          "METADATA_BLOCK_PICTURE COVERART");
	CHECK_STR(value_of(out, "ARTIST"), "A; B; C");
	CHECK_STR(value_of(out, "GENRE"), "Rock");
	CHECK_STR(value_of(out, "KEYWORDS"), "one,two; Two; three");
	CHECK_STR(value_of(out, "COMMENT"), "First; Second");
	CHECK_STR(value_of(out, "DATE"), "2004-05-03; 2011");
	CHECK_STR(value_of(out, "COVERARTMIME"), "image/jpeg; image/png");

	const auto& p = out.properties;
	CHECK_STR(p.artist, "A; B; C");
	CHECK_STR(p.genre, "Rock");
	CHECK_STR(keywords_of(p), "one|two|three");
	CHECK(p.comments.size() == 1 && p.comments[0].text == "First; Second");

	// A property that holds one value takes the first.
	CHECK(same_date(p.created, 2004, 5, 3) && p.year == 2004);
	CHECK(p.track == 3 && p.track_count == 0);
	CHECK(p.rating == 4);

	CHECK_EQ(out.pictures.size(), std::size_t{4});

	if (out.pictures.size() == 4)
	{
		CHECK(out.pictures[0].data == front && out.pictures[0].mime_type == "image/jpeg");
		CHECK(out.pictures[1].data == front && out.pictures[1].mime_type == "image/jpeg");
		CHECK(out.pictures[2].data == back && out.pictures[2].mime_type == "image/png" && out.pictures[2].type == 4);
		CHECK(out.pictures[3].data == back && out.pictures[3].mime_type == "image/png");
	}

	// Each block is joined on its own: a second stream's comments are tags of their own.
	CHECK(libfiles::detail::read_vorbis_comments(vorbis_block("vendor", {"ARTIST=D"}), out));
	CHECK_EQ(std::ranges::count_if(out.tags, [](const libfiles::media_tag& t) { return t.key == "ARTIST"; }), 2);
	CHECK_STR(out.properties.artist, "D");

	// Other containers' values joined from several take the same rule.
	const auto several = apply_all(tag_family::id3v2, {{"TDRC", "2004; 2005"}, {"TRCK", "3/12; 4"}, {"TPOS", " ; 2"}});
	CHECK(same_date(several.created, 2004, 0, 0) && several.track == 3 && several.track_count == 12 && several.disc == 0);
	CHECK(apply_all(tag_family::ape, {{"Rating", "5; 1"}}).rating == 5);
	CHECK(apply_all(tag_family::quicktime, {{"\xC2\xA9" "xyz", "+10.0+020.0/; +11.0+021.0/"}}).location.has_value());
}

TEST("media tags keep what a Vorbis comment block held before its damage")
{
	const std::vector<std::string> fields = {"TITLE=Song", "ARTIST=Artist", "METADATA_BLOCK_PICTURE=" + base64(flac_picture(3, "image/png", "", image(50))),
	                                         "ALBUM=Album"};
	const auto block = vorbis_block("vendor", fields);
	std::vector<std::size_t> ends;
	auto end = std::size_t{4 + 6 + 4};

	for (const auto& f : fields)
	{
		end += 4 + f.size();
		ends.push_back(end);
	}

	for (std::size_t n = 0; n <= block.size(); ++n)
	{
		media_scan_result out;
		const auto read = libfiles::detail::read_vorbis_comments(std::span(block).first(n), out);
		CHECK_EQ(read, n >= 14);
		const auto whole = static_cast<std::size_t>(std::ranges::count_if(ends, [n](const std::size_t e) { return e <= n; }));
		CHECK_EQ(out.tags.size(), read ? 1 + whole : 0);
	}

	// A vendor or field longer than the block ends it.
	auto vendor = block;
	vendor[3] = 0x10;
	media_scan_result v;
	CHECK(!libfiles::detail::read_vorbis_comments(vendor, v));

	auto field = block;
	field[14 + 3] = 0x10;
	media_scan_result f;
	CHECK(libfiles::detail::read_vorbis_comments(field, f));
	CHECK_STR(keys_of(f), "vendor");
}

TEST("media tags read FLAC pictures, located in the file or copied")
{
	const auto picture = image(256);
	const auto block = flac_picture(4, "image/png", "Back", picture);
	const auto fields = block.size() - picture.size();

	libfiles::media_picture copied;
	CHECK(libfiles::detail::read_flac_picture(block, 0, copied));
	CHECK(copied.data == picture && copied.location.empty());
	CHECK(copied.mime_type == "image/png" && copied.type == 4 && copied.description == "Back");

	libfiles::media_picture located;
	CHECK(libfiles::detail::read_flac_picture(block, 1000, located));
	CHECK(located.data.empty());
	CHECK_EQ(located.location.offset, std::uint64_t{1000 + fields});
	CHECK_EQ(located.location.size, std::uint64_t{256});

	// Located, the block need hold only the fields before the image.
	for (std::size_t n = 0; n <= block.size(); ++n)
	{
		const auto cut = std::span(block).first(n);
		libfiles::media_picture a;
		CHECK_EQ(libfiles::detail::read_flac_picture(cut, 0, a), n == block.size());
		libfiles::media_picture b;
		CHECK_EQ(libfiles::detail::read_flac_picture(cut, 1000, b), n >= fields);
	}

	libfiles::media_picture refused;
	CHECK(!libfiles::detail::read_flac_picture(flac_picture(3, "-->", "", to_bytes("http://example.com/a.jpg")), 0, refused));
	CHECK(!libfiles::detail::read_flac_picture(flac_picture(3, "image/png", "", {}), 0, refused));

	auto long_mime = block;
	long_mime[4] = 0x7F;
	CHECK(!libfiles::detail::read_flac_picture(long_mime, 1000, refused));

	auto long_image = block;
	long_image[fields - 4] = 0x7F;
	CHECK(!libfiles::detail::read_flac_picture(long_image, 0, refused));
}

TEST("media tags map iTunes and QuickTime names")
{
	const auto itunes = apply_all(tag_family::itunes, {
		{"\xC2\xA9" "nam", "Title"}, {"\xC2\xA9" "ART", "Artist"}, {"aART", "Album Artist"}, {"\xC2\xA9" "alb", "Album"},
		{"\xC2\xA9" "cmt", "Comment"}, {"\xC2\xA9" "day", "2007-11-09T08:00:00Z"}, {"\xC2\xA9" "gen", "Pop"}, {"trkn", "7/12"},
		{"disk", "1/2"}, {"\xC2\xA9" "wrt", "Composer"}, {"\xC2\xA9" "too", "Lavf54.63.100"}, {"cprt", "(c) 2007"},
		{"desc", "Description"}, {"ldes", "Synopsis"}, {"tvsh", "Show"}, {"tvsn", "2"}, {"tves", "5"},
		{"keyw", "gadget, test; Gadget"}, {"\xC2\xA9" "pub", "Publisher"}, {"rtng", "1"},
	});

	CHECK_STR(itunes.title, "Title");
	CHECK_STR(itunes.artist, "Artist");
	CHECK_STR(itunes.album_artist, "Album Artist");
	CHECK_STR(itunes.album, "Album");
	CHECK(itunes.comments.size() == 1 && itunes.comments[0].text == "Comment");
	CHECK(same_date(itunes.created, 2007, 11, 9) && same_time(itunes.created, 8, 0, 0));
	CHECK(itunes.created.zone == media_date::time_zone::utc);
	CHECK_EQ(itunes.year, 2007);
	CHECK_STR(itunes.genre, "Pop");
	CHECK(itunes.track == 7 && itunes.track_count == 12 && itunes.disc == 1 && itunes.disc_count == 2);
	CHECK_STR(itunes.composer, "Composer");
	CHECK_STR(itunes.encoder, "Lavf54.63.100");
	CHECK_STR(itunes.copyright, "(c) 2007");
	CHECK_STR(itunes.description, "Description");
	CHECK_STR(itunes.synopsis, "Synopsis");
	CHECK_STR(itunes.show, "Show");
	CHECK(itunes.season == 2 && itunes.episode == 5);
	CHECK_STR(keywords_of(itunes), "gadget|test");
	CHECK_STR(itunes.publisher, "Publisher");
	// rtng is iTunes' content advisory.
	CHECK(!itunes.rating);

	// gnre is an ID3v1 genre's number plus one.
	CHECK_STR(apply_all(tag_family::itunes, {{"gnre", "18"}}).genre, "Rock");
	CHECK(apply_all(tag_family::itunes, {{"gnre", "0"}}).genre.empty());
	CHECK(apply_all(tag_family::itunes, {{"gnre", "500"}}).genre.empty());
	CHECK_STR(apply_all(tag_family::itunes, {{"\xC2\xA9" "enc", "First"}, {"\xC2\xA9" "too", "Second"}}).encoder, "Second");

	// Atom names are exact; FFmpeg's own names are not.
	CHECK(apply_all(tag_family::itunes, {{"\xC2\xA9" "NAM", "x"}, {"AART", "x"}}).title.empty());
	CHECK_STR(apply_all(tag_family::itunes, {{"Title", "Generic"}}).title, "Generic");

	const auto quicktime = apply_all(tag_family::quicktime, {
		{"\xC2\xA9" "xyz", "+51.5142-000.0985+012.000/"}, {"\xC2\xA9" "mak", "Apple"}, {"\xC2\xA9" "mod", "iPhone 4"},
		{"titl", "3GPP title"}, {"auth", "Author"}, {"albm", "3GPP album"}, {"dscp", "3GPP description"}, {"yrrc", "2005"},
		{"perf", "Performer"}, {"gnre", "Speech"}, {"\xC2\xA9" "swr", "QuickTime 7"}, {"\xC2\xA9" "key", "apple"},
		{"com.apple.quicktime.displayname", "Not the title"},
	});

	CHECK(quicktime.location && near(quicktime.location->latitude, 51.5142) && near(quicktime.location->longitude, -0.0985));
	CHECK(quicktime.altitude && near(*quicktime.altitude, 12.0));
	CHECK_STR(quicktime.camera_make, "Apple");
	CHECK_STR(quicktime.camera_model, "iPhone 4");
	CHECK_STR(quicktime.title, "3GPP title");
	CHECK_STR(quicktime.artist, "Author");
	CHECK_STR(quicktime.album, "3GPP album");
	CHECK_STR(quicktime.description, "3GPP description");
	CHECK(same_date(quicktime.created, 2005, 0, 0) && quicktime.year == 2005);
	CHECK_STR(quicktime.performer, "Performer");
	CHECK_STR(quicktime.genre, "Speech");
	CHECK_STR(quicktime.encoder, "QuickTime 7");
	CHECK_STR(keywords_of(quicktime), "apple");

	const auto keys = apply_all(tag_family::quicktime, {
		{"com.apple.quicktime.make", "Apple"}, {"com.apple.quicktime.model", "iPhone 12"}, {"com.apple.quicktime.software", "16.1"},
		{"com.apple.quicktime.creationdate", "2011-03-13T15:13:49+0100"},
		{"com.apple.quicktime.location.ISO6709", "+51.5142-000.0985/"}, {"com.apple.quicktime.displayname", "Display name"},
		{"com.apple.quicktime.artist", "Artist"}, {"com.apple.quicktime.author", "Author"}, {"com.apple.quicktime.album", "Album"},
		{"com.apple.quicktime.comment", "Comment"}, {"com.apple.quicktime.description", "Description"},
		{"com.apple.quicktime.keywords", "one,two"}, {"com.apple.quicktime.copyright", "Copyright"},
		{"com.apple.quicktime.genre", "Genre"}, {"com.apple.quicktime.year", "2010"},
	});

	CHECK_STR(keys.camera_make, "Apple");
	CHECK_STR(keys.camera_model, "iPhone 12");
	CHECK_STR(keys.encoder, "16.1");
	CHECK(same_date(keys.created, 2011, 3, 13) && same_time(keys.created, 15, 13, 49));
	CHECK(keys.created.zone == media_date::time_zone::offset && keys.created.offset_minutes == 60);
	CHECK(keys.location && !keys.altitude);
	CHECK_STR(keys.title, "Display name");
	CHECK_STR(keys.artist, "Author");
	CHECK_STR(keys.album, "Album");
	CHECK(keys.comments.size() == 1 && keys.comments[0].text == "Comment");
	CHECK_STR(keys.description, "Description");
	CHECK_STR(keywords_of(keys), "one|two");
	CHECK_STR(keys.copyright, "Copyright");
	CHECK_STR(keys.genre, "Genre");
	CHECK_EQ(keys.year, 2010);

	// The display name stands in only for a missing title.
	CHECK_STR(apply_all(tag_family::quicktime, {{"com.apple.quicktime.title", "Title"}, {"com.apple.quicktime.displayname", "Name"}}).title, "Title");
}

TEST("media tags map ID3v2 frames, resolving genres")
{
	const auto genre = [](const std::string_view value) { return apply_all(tag_family::id3v2, {{"TCON", value}}).genre; };
	CHECK_STR(genre("(17)"), "Rock");
	CHECK_STR(genre("17"), "Rock");
	CHECK_STR(genre("(17)Rock"), "Rock");
	CHECK_STR(genre("(RX)"), "Remix");
	CHECK_STR(genre("CR"), "Cover");
	CHECK_STR(genre("(4)Eurodisco"), "Eurodisco");
	CHECK_STR(genre("(17)(13)"), "Rock; Pop");
	CHECK_STR(genre("((Weird)"), "(Weird)");
	CHECK_STR(genre("Rock; 17; 13"), "Rock; Pop");
	CHECK_STR(genre("Synthwave"), "Synthwave");
	CHECK_STR(genre("80s"), "80s");
	CHECK_STR(genre("(999)"), "(999)");
	CHECK_STR(genre("(191)"), "Psybient");
	CHECK_STR(apply_all(tag_family::id3v2, {{"TCO", "(0)"}}).genre, "Blues");

	const auto p = apply_all(tag_family::id3v2, {
		{"TIT2", "Title"}, {"TPE1", "Artist"}, {"TPE2", "Album Artist"}, {"TALB", "Album"}, {"TCOM", "Composer"},
		{"TPE3", "Performer"}, {"TPUB", "Publisher"}, {"TCOP", "Copyright"}, {"TENC", "Encoded by"}, {"TSSE", "LAME"},
		{"TRCK", "5"}, {"TPOS", "2/3"}, {"TDEN", "2012-01-01"}, {"TDRC", "2006-06-20"}, {"TDRL", "2007"}, {"TDOR", "1969"},
		{"TXXX:Rip date", "2010-01-01"}, {"TXXX:originalyear", "1970"}, {"COMM:iTunNORM", " 0000"}, {"tit2", "lower"},
	});

	CHECK_STR(p.title, "Title");
	CHECK_STR(p.artist, "Artist");
	CHECK_STR(p.album_artist, "Album Artist");
	CHECK_STR(p.album, "Album");
	CHECK_STR(p.composer, "Composer");
	CHECK_STR(p.performer, "Performer");
	CHECK_STR(p.publisher, "Publisher");
	CHECK_STR(p.copyright, "Copyright");
	CHECK_STR(p.encoder, "LAME");
	CHECK(p.track == 5 && p.track_count == 0 && p.disc == 2 && p.disc_count == 3);
	// TDRC is the recording's date; TDEN and TDRL stand in only when there is none.
	CHECK(same_date(p.created, 2006, 6, 20) && p.year == 2006);
	CHECK(same_date(p.original_release, 1970, 0, 0));
	CHECK(p.comments.empty());

	CHECK(same_date(apply_all(tag_family::id3v2, {{"TDEN", "2012-01-01"}}).created, 2012, 1, 1));
	CHECK(same_date(apply_all(tag_family::id3v2, {{"TDRL", "2007"}, {"TDEN", "2012-01-01"}}).created, 2007, 0, 0));

	// The year alone is a year; with TDAT, and TIME, read_id3v2 makes it a date.
	const auto year = apply_all(tag_family::id3v2, {{"TYER", "1999"}, {"TDAT", "0405"}, {"TIME", "1011"}});
	CHECK(year.year == 1999 && !year.created.valid());

	media_properties comments;
	libfiles::detail::apply_tag(comments, tag_family::id3v2, "COMM", "English", "ENG");
	libfiles::detail::apply_tag(comments, tag_family::id3v2, "COMM", "Unknown", "XXX");
	libfiles::detail::apply_tag(comments, tag_family::id3v2, "COM", "Undefined", "und");
	libfiles::detail::apply_tag(comments, tag_family::id3v2, "COMM", "English", "eng");
	CHECK_EQ(comments.comments.size(), std::size_t{3});

	if (comments.comments.size() == 3)
	{
		CHECK(comments.comments[0].language == "eng" && comments.comments[1].language.empty() && comments.comments[2].language.empty());
	}

	// A POPM frame's rating is chosen by read_id3v2, among them all.
	CHECK(!apply_all(tag_family::id3v2, {{"POPM:Windows Media Player 9 Series", "255"}}).rating);
}

TEST("media tags map RIFF INFO, AIFF and FLV names")
{
	const auto riff = apply_all(tag_family::riff_info, {
		{"INAM", "Title"}, {"IART", "Artist"}, {"IPRD", "Album"}, {"ICMT", "Comment"}, {"ICRD", "2011-03-13"}, {"IGNR", "Genre"},
		{"ICOP", "Copyright"}, {"ISFT", "Lavf"}, {"IENG", "Engineer"}, {"ISBJ", "Subject"}, {"IKEY", "one; two"}, {"ITRK", "4"},
		{"inam", "lower"},
	});

	CHECK_STR(riff.title, "Title");
	CHECK_STR(riff.artist, "Artist");
	CHECK_STR(riff.album, "Album");
	CHECK(riff.comments.size() == 1 && riff.comments[0].text == "Comment");
	CHECK(same_date(riff.created, 2011, 3, 13) && riff.year == 2011);
	CHECK_STR(riff.genre, "Genre");
	CHECK_STR(riff.copyright, "Copyright");
	CHECK_STR(riff.encoder, "Lavf");
	CHECK(riff.composer.empty());
	CHECK_STR(riff.description, "Subject");
	CHECK_STR(keywords_of(riff), "one|two");
	CHECK_EQ(riff.track, 4);

	// Diffractor's FFmpeg took COMM and TITL as the comment and title, and IPRT as the track.
	const auto extra = apply_all(tag_family::riff_info, {{"COMM", "Comment"}, {"TITL", "Title"}, {"IPRT", "9"}});
	CHECK(extra.title == "Title" && extra.comments.size() == 1 && extra.track == 9);
	CHECK(same_date(apply_all(tag_family::riff_info, {{"IDIT", "Sun Mar 13 15:13:49 2011\n"}}).created, 2011, 3, 13));

	const auto aiff = apply_all(tag_family::aiff, {{"NAME", "Title"}, {"AUTH", "Author"}, {"(c) ", "Copyright"}, {"ANNO", "Note"}});
	CHECK_STR(aiff.title, "Title");
	CHECK_STR(aiff.artist, "Author");
	CHECK_STR(aiff.copyright, "Copyright");
	CHECK(aiff.comments.size() == 1 && aiff.comments[0].text == "Note");

	const auto flv = apply_all(tag_family::flv, {
		{"Title", "Title"}, {"author", "Author"}, {"comment", "Comment"}, {"description", "Description"}, {"keywords", "a,b"},
		{"creationdate", "Sun Mar 13 15:13:49 2011\n"}, {"encoder", "Lavf"}, {"metadatacreator", "Yet Another Metadata Injector"},
		{"album", "Album"},
	});

	CHECK_STR(flv.title, "Title");
	CHECK_STR(flv.artist, "Author");
	CHECK(flv.comments.size() == 1);
	CHECK_STR(flv.description, "Description");
	CHECK_STR(keywords_of(flv), "a|b");
	CHECK(same_date(flv.created, 2011, 3, 13) && same_time(flv.created, 15, 13, 49));
	CHECK_STR(flv.encoder, "Yet Another Metadata Injector");
	CHECK_STR(flv.album, "Album");
}

TEST("media tags map ASF names")
{
	const auto asf = apply_all(tag_family::asf, {
		{"Title", "Title"}, {"Author", "Author"}, {"Copyright", "Copyright"}, {"Description", "Description"}, {"Rating", "4"},
		{"WM/AlbumTitle", "Album"}, {"wm/albumartist", "Album Artist"}, {"WM/Composer", "Composer"}, {"WM/Genre", "Genre"},
		{"WM/Year", "2011"}, {"WM/Track", "4"}, {"WM/PartOfSet", "1/2"}, {"WM/Publisher", "Publisher"},
		{"WM/EncodedBy", "Encoded by"}, {"WM/ToolName", "Tool"}, {"WM/Conductor", "Conductor"},
		{"WM/OriginalReleaseYear", "1969"}, {"WM/Category", "New York, NY; London"}, {"WM/Category", "london"},
		{"WM/SharedUserRating", "75"}, {"WM/Lyrics", "La la"}, {"WM/Mood", "Happy"},
	});

	CHECK_STR(asf.title, "Title");
	CHECK_STR(asf.artist, "Author");
	CHECK_STR(asf.copyright, "Copyright");
	CHECK(asf.comments.size() == 1 && asf.comments[0].text == "Description");
	CHECK(asf.description.empty());
	CHECK_STR(asf.album, "Album");
	CHECK_STR(asf.album_artist, "Album Artist");
	CHECK_STR(asf.composer, "Composer");
	CHECK_STR(asf.genre, "Genre");
	CHECK_EQ(asf.year, 2011);
	CHECK_EQ(asf.track, 5);
	CHECK(asf.disc == 1 && asf.disc_count == 2);
	CHECK_STR(asf.publisher, "Publisher");
	CHECK_STR(asf.encoder, "Tool");
	CHECK_STR(asf.performer, "Conductor");
	CHECK(same_date(asf.original_release, 1969, 0, 0));
	CHECK_STR(keywords_of(asf), "New York, NY|London");
	CHECK(asf.rating == 4);

	// WM/Track counts from zero, and gives way to WM/TrackNumber either way round.
	CHECK_EQ(apply_all(tag_family::asf, {{"WM/TrackNumber", "7"}, {"WM/Track", "3"}}).track, 7);
	CHECK_EQ(apply_all(tag_family::asf, {{"WM/Track", "3"}, {"WM/TrackNumber", "7"}}).track, 7);
	CHECK_EQ(apply_all(tag_family::asf, {{"WM/Track", "0"}}).track, 1);

	// Windows' percent: 0 means unrated.
	CHECK(apply_all(tag_family::asf, {{"WM/SharedUserRating", "99"}}).rating == 5);
	CHECK(apply_all(tag_family::asf, {{"WM/SharedUserRating", "1"}}).rating == 1);
	CHECK(!apply_all(tag_family::asf, {{"WM/SharedUserRating", "0"}}).rating);
}

TEST("media tags map Matroska, Vorbis and APE names")
{
	// What Diffractor's should_scan_matroska expects of tagged.mkv.
	const auto mkv = apply_all(tag_family::matroska, {
		{"ENCODER", "diffractor-test"}, {"TITLE", "Matroska Test Title"}, {"ARTIST", "Matroska Test Artist"}, {"GENRE", "TestGenre"},
		{"COMMENT", "matroska test comment"}, {"RATING", "4"}, {"KEYWORDS", "alpha,beta"},
	}, "und");

	CHECK_STR(mkv.title, "Matroska Test Title");
	CHECK_STR(mkv.artist, "Matroska Test Artist");
	CHECK_STR(mkv.genre, "TestGenre");
	CHECK(mkv.comments.size() == 1 && mkv.comments[0].text == "matroska test comment" && mkv.comments[0].language.empty());
	CHECK(mkv.rating == 4);
	CHECK_STR(keywords_of(mkv), "alpha|beta");
	CHECK_STR(mkv.encoder, "diffractor-test");

	const auto more = apply_all(tag_family::matroska, {
		{"lead_performer", "Lead"}, {"SUMMARY", "Summary"}, {"SYNOPSIS", "Synopsis"}, {"LABEL", "Label"}, {"COMPOSER", "Composer"},
		{"DATE_RELEASED", "2001"}, {"DATE_RECORDED", "1999-05-04"}, {"DATE_RELEASED", "2002"}, {"ORIGINAL_DATE", "1969"},
		{"PART_NUMBER", "3"}, {"TOTAL_PARTS", "9"}, {"ALBUM", "Album"}, {"ALBUM_ARTIST", "Band"}, {"COPYRIGHT", "Copyright"},
	});

	CHECK_STR(more.performer, "Lead");
	CHECK_STR(more.description, "Summary");
	CHECK_STR(more.synopsis, "Synopsis");
	CHECK_STR(more.publisher, "Label");
	CHECK_STR(more.composer, "Composer");
	CHECK(same_date(more.created, 1999, 5, 4) && more.year == 1999);
	CHECK(same_date(more.original_release, 1969, 0, 0));
	CHECK(more.track == 3 && more.track_count == 9);
	CHECK_STR(more.album, "Album");
	CHECK_STR(more.album_artist, "Band");
	CHECK_STR(more.copyright, "Copyright");

	// Stars from 0 to 5, a fraction rounded; above 5, a percent.
	const auto stars = [](const std::string_view value) { return apply_all(tag_family::matroska, {{"RATING", value}}).rating; };
	CHECK(stars("3.6") == 4);
	CHECK(stars("80") == 4);
	CHECK(stars("100") == 5);
	CHECK(stars("5") == 5);
	CHECK(!stars("0"));
	CHECK(!stars("0.2"));
	CHECK(!stars("-1"));
	CHECK(!stars("four"));

	const auto vorbis = apply_all(tag_family::vorbis, {
		{"Album Artist", "Band"}, {"TOTALTRACKS", "11"}, {"TRACKNUMBER", "4"}, {"TOTALDISCS", "2"}, {"DISCNUMBER", "1"},
		{"ENCODED-BY", "Encoder"}, {"ORIGINALDATE", "1969-07-01"}, {"YEAR", "1999"}, {"Performer", "Performer"},
		{"label", "Label"}, {"copyright", "Copyright"},
	});

	CHECK_STR(vorbis.album_artist, "Band");
	CHECK(vorbis.track == 4 && vorbis.track_count == 11 && vorbis.disc == 1 && vorbis.disc_count == 2);
	CHECK_STR(vorbis.encoder, "Encoder");
	CHECK(same_date(vorbis.original_release, 1969, 7, 1));
	CHECK(vorbis.year == 1999 && !vorbis.created.valid());
	CHECK_STR(vorbis.performer, "Performer");
	CHECK_STR(vorbis.publisher, "Label");
	CHECK_STR(vorbis.copyright, "Copyright");

	const auto ape = apply_all(tag_family::ape, {
		{"TITLE", "Title"}, {"albumartist", "Band"}, {"Conductor", "Conductor"}, {"Publisher", "Publisher"}, {"Year", "1999"},
		{"encoder", "Generic encoder"},
	});

	CHECK_STR(ape.title, "Title");
	CHECK_STR(ape.album_artist, "Band");
	CHECK_STR(ape.performer, "Conductor");
	CHECK_STR(ape.publisher, "Publisher");
	CHECK(same_date(ape.created, 1999, 0, 0) && ape.year == 1999);
	CHECK_STR(ape.encoder, "Generic encoder");
}

TEST("media tags share the common rules: trimmed values, later values, comments and keywords")
{
	media_scan_result out;
	libfiles::detail::add_tag(out, tag_family::vorbis, "TITLE", "  Song \r\n");
	libfiles::detail::add_tag(out, tag_family::vorbis, "ARTIST", " \t ");
	libfiles::detail::add_tag(out, tag_family::matroska, "COMMENT", "Hello", "eng");
	libfiles::detail::add_tag(out, tag_family::matroska, "COMMENT", "Hello", "eng");
	libfiles::detail::add_tag(out, tag_family::matroska, "COMMENT", "Hello", "fre");
	libfiles::detail::add_tag(out, tag_family::matroska, "TITLE", "Later", "und");

	CHECK_STR(keys_of(out), "TITLE COMMENT COMMENT COMMENT TITLE");
	CHECK_STR(value_of(out, "TITLE"), "Song");
	CHECK(out.tags.size() > 1 && out.tags[1].language == "eng");
	// "und" names no language, so none is reported.
	CHECK(out.tags.size() == 5 && out.tags[4].language.empty());
	CHECK_STR(out.properties.title, "Later");
	CHECK(out.properties.artist.empty());
	CHECK_EQ(out.properties.comments.size(), std::size_t{2});

	const auto keywords = apply_all(tag_family::matroska, {{"KEYWORDS", " one ,two;; Three,"}, {"KEYWORDS", "TWO, four"}});
	CHECK_STR(keywords_of(keywords), "one|two|Three|four");

	// A value that is not one keeps the one before it.
	const auto kept = apply_all(tag_family::vorbis, {{"DATE", "2004"}, {"DATE", "soon"}, {"TRACKNUMBER", "3"}, {"TRACKNUMBER", "three"}});
	CHECK(same_date(kept.created, 2004, 0, 0) && kept.track == 3);

	// A later place replaces an earlier one with its altitude.
	const auto place = apply_all(tag_family::quicktime, {{"\xC2\xA9" "xyz", "+10.0+020.0+5.0/"}, {"\xC2\xA9" "xyz", "+11.0+021.0/"}});
	CHECK(place.location && near(place.location->latitude, 11.0) && !place.altitude);
}

TEST("media tags file the fixtures' MP4 and QuickTime tags as Diffractor filed FFmpeg's")
{
	// StPauls.MOV: udta atoms, some with a language, and the keys of its 'mdta' handler.
	media_scan_result st_pauls;
	using libfiles::detail::add_tag;
	add_tag(st_pauls, tag_family::quicktime, "\xC2\xA9" "mod", "iPhone 3GS", "eng");
	add_tag(st_pauls, tag_family::quicktime, "\xC2\xA9" "swr", "4.3", "eng");
	add_tag(st_pauls, tag_family::quicktime, "\xC2\xA9" "day", "2011-03-13T15:13:49+0000", "eng");
	add_tag(st_pauls, tag_family::quicktime, "\xC2\xA9" "xyz", "+51.5142-000.0985+027.885/", "eng");
	add_tag(st_pauls, tag_family::quicktime, "\xC2\xA9" "mak", "Apple", "eng");
	add_tag(st_pauls, tag_family::quicktime, "com.apple.quicktime.make", "Apple");
	add_tag(st_pauls, tag_family::quicktime, "com.apple.quicktime.creationdate", "2011-03-13T15:13:49+0000");
	add_tag(st_pauls, tag_family::quicktime, "com.apple.quicktime.location.ISO6709", "+51.5142-000.0985+027.885/");
	add_tag(st_pauls, tag_family::quicktime, "com.apple.quicktime.software", "4.3");
	add_tag(st_pauls, tag_family::quicktime, "com.apple.quicktime.model", "iPhone 3GS");

	const auto& p = st_pauls.properties;
	CHECK_STR(p.camera_make, "Apple");
	CHECK_STR(p.camera_model, "iPhone 3GS");
	CHECK_STR(p.encoder, "4.3");
	CHECK(same_date(p.created, 2011, 3, 13) && same_time(p.created, 15, 13, 49));
	CHECK(p.created.zone == media_date::time_zone::offset && p.created.offset_minutes == 0);
	CHECK_EQ(p.year, 2011);
	CHECK(p.location && near(p.location->latitude, 51.5142) && near(p.location->longitude, -0.0985));
	CHECK(p.altitude && near(*p.altitude, 27.885));

	// indy.mp4 and This Year's Love.m4a: iTunes atoms.
	const auto indy = apply_all(tag_family::itunes, {
		{"\xC2\xA9" "nam", "Raiders of the Lost Ark"}, {"\xC2\xA9" "ART", "Harrison Ford"},
		{"\xC2\xA9" "wrt", "Lawrence Kasdan|George Lucas|Philip Kaufman"}, {"\xC2\xA9" "day", "1981-06-12"},
		{"\xC2\xA9" "too", "Lavf61.3.104"}, {"\xC2\xA9" "cmt", "Re-Encoded"}, {"\xC2\xA9" "gen", "Action &Adventure"},
		{"desc", "Indiana Jones"}, {"ldes", "When Dr. Indiana Jones"}, {"tves", "1"}, {"tvsn", "1"}, {"stik", "9"}, {"hdvd", "49"},
	});

	CHECK_STR(indy.title, "Raiders of the Lost Ark");
	CHECK_STR(indy.artist, "Harrison Ford");
	CHECK_STR(indy.composer, "Lawrence Kasdan|George Lucas|Philip Kaufman");
	CHECK(same_date(indy.created, 1981, 6, 12) && indy.year == 1981);
	CHECK_STR(indy.encoder, "Lavf61.3.104");
	CHECK(indy.comments.size() == 1 && indy.comments[0].text == "Re-Encoded");
	CHECK_STR(indy.genre, "Action &Adventure");
	CHECK_STR(indy.description, "Indiana Jones");
	CHECK_STR(indy.synopsis, "When Dr. Indiana Jones");
	CHECK(indy.episode == 1 && indy.season == 1);

	const auto love = apply_all(tag_family::itunes, {
		{"\xC2\xA9" "nam", "This Year's Love"}, {"aART", "David Gray"}, {"\xC2\xA9" "day", "2007-11-09T08:00:00Z"},
		{"cprt", "\xE2\x84\x97 2007 Iht Records Ltd"}, {"trkn", "7"}, {"pgap", "0"},
	});

	CHECK(same_date(love.created, 2007, 11, 9) && same_time(love.created, 8, 0, 0) && love.created.zone == media_date::time_zone::utc);
	CHECK_STR(love.copyright, "\xE2\x84\x97 2007 Iht Records Ltd");
	CHECK(love.track == 7 && love.track_count == 0);
}

TEST("media tags bound what a crafted tag makes them hold or compare")
{
	// More frames than a tag is read for.
	const std::vector<bytes> frames(70000, frame(3, "TXXX", text(0, lit("k\0v"))));
	CHECK_EQ(read_id3(id3_tag(3, frames)).tags.size(), std::size_t{65536});

	// More keywords and genres than are kept, each compared with those before it.
	std::string words;
	for (auto i = 0; i < 10000; ++i) words += std::to_string(i) + ",";
	CHECK_EQ(apply_all(tag_family::matroska, {{"KEYWORDS", words}}).keywords.size(), std::size_t{4096});

	std::string genres;
	for (auto i = 0; i < 100000; ++i) genres += std::to_string(i % 192) + ";";
	const auto genre = apply_all(tag_family::id3v2, {{"TCON", genres}}).genre;
	CHECK(genre.starts_with("Blues; Classic Rock; ") && genre.ends_with("; Psybient"));

	media_properties comments;
	for (auto i = 0; i < 5000; ++i) libfiles::detail::apply_tag(comments, tag_family::vorbis, "COMMENT", std::to_string(i));
	CHECK_EQ(comments.comments.size(), std::size_t{4096});
}

TEST("media tags read dates in the forms tags write them")
{
	using libfiles::detail::parse_media_date;
	using zone = media_date::time_zone;

	const auto day = [](const std::string_view text, const int y, const int m, const int d)
	{
		const auto date = parse_media_date(text);
		return same_date(date, y, m, d) && !date.has_time;
	};


	CHECK(day("2011", 2011, 0, 0));
	CHECK(day("2011-03", 2011, 3, 0));
	CHECK(day("2011-03-13", 2011, 3, 13));
	CHECK(day(" 2011-3-5 ", 2011, 3, 5));
	CHECK(day("1999-00-00", 1999, 0, 0));
	CHECK(day("1999-05-00", 1999, 5, 0));
	CHECK(day("2011:03:13", 2011, 3, 13));
	CHECK(day("2011/03/13", 2011, 3, 13));
	CHECK(day("20110313", 2011, 3, 13));
	CHECK(day("2000-02-29", 2000, 2, 29));
	CHECK(moment("2011-03-13T15", 2011, 3, 13, 15, 0, 0));
	CHECK(moment("2011-03-13T15:13", 2011, 3, 13, 15, 13, 0));
	CHECK(moment("2011-03-13T15:13:49", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("2011-03-13 15:13:49", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("2011-03-13  15:13:49", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("2011:03:13 15:13:49", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("2011-03-13t15:13:49.123456", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("2010-03-20T21:29:11.000000Z", 2010, 3, 20, 21, 29, 11, zone::utc));
	CHECK(moment("2011-03-13T15:13:49z", 2011, 3, 13, 15, 13, 49, zone::utc));
	CHECK(moment("2011-03-13 15:13:49 UTC", 2011, 3, 13, 15, 13, 49, zone::utc));
	CHECK(moment("2011-03-13T15:13:49+01:00", 2011, 3, 13, 15, 13, 49, zone::offset, 60));
	CHECK(moment("2011-03-13T15:13:49+0130", 2011, 3, 13, 15, 13, 49, zone::offset, 90));
	CHECK(moment("2011-03-13T15:13:49-05", 2011, 3, 13, 15, 13, 49, zone::offset, -300));
	CHECK(moment("2011-03-13T15:13:49-03:30", 2011, 3, 13, 15, 13, 49, zone::offset, -210));
	CHECK(moment("2011-03-13 15:13:49 +0000", 2011, 3, 13, 15, 13, 49, zone::offset, 0));
	CHECK(moment("20110313T151349", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("20110313T1513", 2011, 3, 13, 15, 13, 0));
	CHECK(moment("20110313151349", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("201103131513", 2011, 3, 13, 15, 13, 0));
	CHECK(moment("20110313T151349Z", 2011, 3, 13, 15, 13, 49, zone::utc));
	CHECK(moment("Sun Mar 13 15:13:49 2011", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("SUN MAR 13 15:13:49 2011\n", 2011, 3, 13, 15, 13, 49));
	CHECK(moment(std::string_view("Sun Mar 13 15:13:49 2011\n\0", 26), 2011, 3, 13, 15, 13, 49));
	CHECK(moment("Mar 13 15:13:49 2011", 2011, 3, 13, 15, 13, 49));
	CHECK(moment("Sunday March  3 05:13:49 2011", 2011, 3, 3, 5, 13, 49));
	CHECK(moment("Sun Mar 13 15:13:49 GMT 2011", 2011, 3, 13, 15, 13, 49, zone::utc));

	for (const auto invalid : {"", "   ", "0000", "99", "20111", "2011-", "2011-13-01", "2011-00-05", "2011-04-31", "2001-02-29",
	                           "1900-02-29", "2011-03-13T", "2011-03-13T24:00", "2011-03-13T15:60", "2011-03-13T15:13:60",
	                           "2011-03-13T15:13:49+15:00", "2011-03-13T15:13:49+01:60", "2011-03-13T15:13:49+1", "2011-03-13Z",
	                           "2011 15:13", "2011-03-13 garbage", "2011-03-13T15:13:49+01:00 extra", "2011-03-1315:13",
	                           "Sun Foo 13 15:13:49 2011", "Sun Mar 32 15:13:49 2011", "Sun Mar 13 25:13:49 2011", "Sun Mar 13 15:13:49",
	                           "Sun Mar 13 15:13:49 PST 2011", "Sun Mar 13 15:13 49 2011", "Sun Mar 13 15:13:49 11", "Feb 29 00:00:00 2001",
	                           "soon"})
	{
		const auto date = parse_media_date(invalid);
		if (date.valid()) test::fail(__FILE__, __LINE__, std::string("read as a date: ") + invalid);
		CHECK(!date.valid() && date.year == 0);
	}
}

TEST("media tags read ISO 6709 places")
{
	using libfiles::detail::parse_iso6709;

	const auto place = [](const std::string_view text, const double latitude, const double longitude)
	{
		const auto p = parse_iso6709(text);
		return p && near(p->latitude, latitude) && near(p->longitude, longitude);
	};

	CHECK(place("+51.5142-000.0985/", 51.5142, -0.0985));
	CHECK(place("+51.5142-000.0985", 51.5142, -0.0985));
	CHECK(place("-33.8688+151.2093/", -33.8688, 151.2093));
	CHECK(place("+5130.852-00005.910/", 51.5142, -0.0985));
	CHECK(place("+513051.12-0000554.6/", 51.5142, -0.0985));
	CHECK(place("+40.20361-075.00417CRSWGS_84/", 40.20361, -75.00417));
	CHECK(place("+9-9/", 9, -9));
	CHECK(place("+90.0000+180.0000/", 90, 180));

	auto altitude = std::numeric_limits<double>::quiet_NaN();
	CHECK(place("+51.5142-000.0985/", 51.5142, -0.0985));
	CHECK(parse_iso6709("+51.5142-000.0985/", &altitude) && std::isnan(altitude));
	CHECK(parse_iso6709("+51.5142-000.0985+012.000/", &altitude) && near(altitude, 12.0));
	CHECK(parse_iso6709("+51.5142-000.0985-5.5", &altitude) && near(altitude, -5.5));
	CHECK(parse_iso6709("+40.20361-075.00417+350.517CRSWGS_84/", &altitude) && near(altitude, 350.517));
	// The place a 3GPP 'loci' box names follows the slash, as FFmpeg writes it.
	CHECK(place("+51.5142-000.0985+027.885/London", 51.5142, -0.0985));
	CHECK(parse_iso6709("+51.5142-000.0985+027.885/London", &altitude) && near(altitude, 27.885));

	for (const auto invalid : {"", "+00.0000+000.0000/", "+00-000/", "+91.0000+000.0000/", "-90.5+000.0/", "+45.0000+181.0000/",
	                           "+5160.0+00000.0/", "+513060.0-0000554.6/", "51.5142-000.0985/", "+51.5142/", "+515-000/",
	                           "+51.5142-0000.0985/", "+51.5142-000.0985garbage", "+51.5142-000.0985+abc/", "+51.5142-000.0985 /",
	                           "London"})
	{
		auto untouched = std::numeric_limits<double>::quiet_NaN();
		if (parse_iso6709(invalid, &untouched)) test::fail(__FILE__, __LINE__, std::string("read as a place: ") + invalid);
		CHECK(std::isnan(untouched));
	}
}

TEST("media tags walk ID3v2 frames by the one parser the MP3 writer shares")
{
	using namespace libfiles::detail;

	const auto tag = id3_tag(4, {frame(4, "TIT2", text("Title")), frame(4, "COMM", bytes(200, 'c'), 0x0001)}, 5, 0x10);
	const auto header = parse_id3_header(tag);
	CHECK(header && header->version == 4 && header->known_version() && header->size_syncsafe && header->has_footer());
	CHECK(header && header->end() == tag.size() - 10 && header->total() == tag.size());
	CHECK(!parse_id3_header(lit("ID2\x03\0\0\0\0\0\0")));
	CHECK(!parse_id3_header(lit("ID3\x03\0\0\0\0\0")));

	libfiles::reader r(tag);

	if (header)
	{
		id3_frame_walker frames(r, 4, 10, header->end());
		const auto first = frames.next();
		const auto second = frames.next();
		CHECK(first && first->name() == "TIT2" && first->offset == 10 && first->size == 6 && first->body() == 20);
		CHECK(second && second->name() == "COMM" && second->size == 200 && second->flags == 0x0001);
		CHECK(!frames.next() && !frames.overran() && !frames.stray());
	}

	// Bytes that are not a frame end the walk as stray; a size past the end, as overrun.
	const auto stray = join({frame(3, "TIT2", text("Title")), to_bytes("tit2 not a frame")});
	libfiles::reader rs(stray);
	id3_frame_walker s(rs, 3, 0, stray.size());
	CHECK(s.next() && !s.next() && s.stray() && !s.overran());

	const auto overrun = frame(3, "TIT2", text("Title"));
	libfiles::reader ro(overrun);
	id3_frame_walker o(ro, 3, 0, overrun.size() - 1);
	CHECK(!o.next() && o.overran());

	// An ID3v2.4 size with a high bit set can only be a plain number; a whole tag of them is iTunes'.
	const auto high = frame(4, "TIT2", text(std::string(199, 't')), 0, true);
	libfiles::reader rh(high);
	id3_frame_walker h(rh, 4, 0, high.size());
	const auto f = h.next();
	CHECK(f && f->size == 200);

	const auto plain = frame(4, "TIT2", text(std::string(299, 't')), 0, true);
	libfiles::reader rp(plain);
	id3_frame_walker standard(rp, 4, 0, plain.size());
	const auto misread = standard.next();
	CHECK(misread && misread->size == 172);
	id3_frame_walker itunes(rp, 4, 0, plain.size(), id3_sizes::plain);
	const auto right = itunes.next();
	CHECK(right && right->size == 300);

	// The extended header: ID3v2.3 counts its size without the size itself, ID3v2.4 with it.
	const auto v3 = id3_tag(3, {frame(3, "TIT2", text("Title"))}, 0, 0x40, builder().be32(6).be16(0).be32(0).b);
	libfiles::reader r3(v3);
	const auto h3 = parse_id3_header(v3);
	CHECK(h3 && id3_frames_begin(r3, *h3, 10, h3->end()) == std::optional<std::uint64_t>(20));

	const auto v4 = id3_tag(4, {frame(4, "TIT2", text("Title"))}, 0, 0x40, join({syncsafe(6), bytes{1, 0}}));
	libfiles::reader r4(v4);
	const auto h4 = parse_id3_header(v4);
	CHECK(h4 && id3_frames_begin(r4, *h4, 10, h4->end()) == std::optional<std::uint64_t>(16));
	CHECK(h4 && !id3_frames_begin(r4, *h4, 10, 13));
}
