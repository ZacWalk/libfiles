// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the ASF and FLV media scans. Each file is built object by object and tag by tag, so
// every field a test reads back is one it put there. Each is also cut short at every byte near its ends
// and its boundaries, and has its bytes damaged, to show a scan either refuses it or reads it whole.

#include "test.h"
#include "builders.h"

#include "legacy.h"
#include "libfiles/media.h"
#include "libfiles/write.h"
#include "media_internal.h"
#include "round_trip.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace test_build;

namespace
{
	using libfiles::media_scan_result;
	using libfiles::media_stream_kind;
	using tag_list = std::vector<std::pair<std::string, std::string>>;

	media_scan_result scan(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index)
	{
		libfiles::reader r(file);
		return libfiles::scan_media(r, intent);
	}

	std::string packet_of(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::read_packet(r).packet;
	}

	void check_tags(const std::vector<libfiles::media_tag>& actual, const tag_list& expected)
	{
		CHECK_EQ(actual.size(), expected.size());

		for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i)
		{
			CHECK_STR(actual[i].key, expected[i].first);
			CHECK_STR(actual[i].value, expected[i].second);
		}
	}

	std::string tag_value(const std::vector<libfiles::media_tag>& tags, const std::string_view key)
	{
		for (const auto& t : tags)
		{
			if (t.key == key) return t.value;
		}

		return {};
	}

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	// A scan of a damaged file fails with a reason and nothing else, or reads as a file: what it found
	// lies inside the file, and its packet is the one read_packet finds there.
	void check_consistent(const bytes& file, const media_scan_result& s)
	{
		if (!s.success)
		{
			CHECK(!s.error.empty());
			CHECK(s.streams.empty() && s.tags.empty() && s.pictures.empty() && s.xmp.empty());
			return;
		}

		CHECK(s.error.empty());
		CHECK(std::isfinite(s.duration) && s.duration >= 0);

		for (const auto& st : s.streams)
		{
			CHECK(st.video.width <= 65535 && st.video.height <= 65535);
			CHECK(std::isfinite(st.video.frame_rate) && st.video.frame_rate >= 0);
			for (const auto& t : st.tags) CHECK(!t.key.empty() && !t.value.empty());
		}

		for (const auto& t : s.tags) CHECK(!t.key.empty() && !t.value.empty());

		for (const auto& p : s.pictures)
		{
			CHECK(p.location.size > 0 && p.location.offset <= file.size() && p.location.size <= file.size() - p.location.offset);
		}

		CHECK(to_string(s.xmp) == packet_of(file));
	}

	void check_cuts(const bytes& file, const std::vector<std::size_t>& boundaries)
	{
		std::vector<std::size_t> cuts;
		for (std::size_t n = 0; n < std::min<std::size_t>(file.size(), 160); ++n) cuts.push_back(n);
		for (std::size_t n = file.size() > 160 ? file.size() - 160 : 0; n < file.size(); ++n) cuts.push_back(n);

		for (const auto b : boundaries)
		{
			for (std::size_t n = b > 3 ? b - 3 : 0; n <= b + 3 && n < file.size(); ++n) cuts.push_back(n);
		}

		for (const auto n : cuts)
		{
			const bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
			check_consistent(cut, scan(cut));
		}
	}

	// Every byte of [begin, end) set to 0 and to 0xFF, and every four from it to 0xFF: each length, count
	// and offset there made zero, large or past anything the file holds.
	void check_damage(const bytes& file, const std::size_t begin, const std::size_t end)
	{
		for (auto i = begin; i < end && i < file.size(); ++i)
		{
			for (const auto value : {0x00, 0xFF})
			{
				auto damaged = file;
				damaged[i] = static_cast<std::uint8_t>(value);
				check_consistent(damaged, scan(damaged, libfiles::scan_intent::inspect));
			}

			auto damaged = file;
			for (auto j = i; j < i + 4 && j < damaged.size(); ++j) damaged[j] = 0xFF;
			check_consistent(damaged, scan(damaged));
		}
	}

	// Bits written most significant first, as codec headers hold them.
	struct bit_writer
	{
		bytes b;
		std::size_t count = 0;

		bit_writer& u(const std::uint64_t value, const unsigned bits)
		{
			for (auto i = bits; i-- > 0; ++count)
			{
				if (count % 8 == 0) b.push_back(0);
				b.back() = static_cast<std::uint8_t>(b.back() | (value >> i & 1u) << (7 - count % 8));
			}

			return *this;
		}

		bit_writer& ue(const std::uint32_t value)
		{
			const auto coded = std::uint64_t{value} + 1;
			const auto length = static_cast<unsigned>(std::bit_width(coded));
			return u(0, length - 1).u(coded, length);
		}

		bit_writer& trailing()
		{
			u(1, 1);
			while (count % 8 != 0) u(0, 1);
			return *this;
		}
	};

	// A NAL unit: its header, then its payload with emulation prevention.
	bytes nal_unit(const bytes& header, const bytes& payload)
	{
		auto out = header;
		auto zeros = 0;

		for (const auto c : payload)
		{
			if (zeros >= 2 && c <= 3)
			{
				out.push_back(3);
				zeros = 0;
			}

			out.push_back(c);
			zeros = c == 0 ? zeros + 1 : 0;
		}

		return out;
	}

	// ---- ASF ---------------------------------------------------------------------------------------

	// A GUID from its registry form, as ASF stores it: its first three fields little-endian.
	bytes guid(const std::string_view text)
	{
		bytes v;

		for (std::size_t i = 0; i + 1 < text.size();)
		{
			if (text[i] == '-')
			{
				++i;
				continue;
			}

			v.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
			i += 2;
		}

		return {v[3], v[2], v[1], v[0], v[5], v[4], v[7], v[6], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]};
	}

	const bytes header_guid = guid("75B22630-668E-11CF-A6D9-00AA0062CE6C");
	const bytes data_guid = guid("75B22636-668E-11CF-A6D9-00AA0062CE6C");
	const bytes simple_index_guid = guid("33000890-E5B1-11CF-89F4-00A0C90349CB");
	const bytes xmp_guid = guid("BE7ACFCB-97A9-42E8-9C71-999491E3AFAC");
	const bytes file_properties_guid = guid("8CABDCA1-A947-11CF-8EE4-00C00C205365");
	const bytes stream_properties_guid = guid("B7DC0791-A9B7-11CF-8EE6-00C00C205365");
	const bytes header_extension_guid = guid("5FBF03B5-A92E-11CF-8EE3-00C00C205365");
	const bytes content_description_guid = guid("75B22633-668E-11CF-A6D9-00AA0062CE6C");
	const bytes extended_content_guid = guid("D2D0A440-E307-11D2-97F0-00A0C95EA850");
	const bytes stream_bitrate_guid = guid("7BF875CE-468D-11D1-8D82-006097C9A2B2");
	const bytes padding_guid = guid("1806D474-CADF-4509-A4BA-9AABCB96AAE8");
	const bytes extended_stream_properties_guid = guid("14E6A5CB-C672-4332-8399-A96952065B5A");
	const bytes language_list_guid = guid("7C4346A9-EFE0-4BFC-B229-393EDE415C85");
	const bytes metadata_guid = guid("C5F8CBEA-5BAF-4877-8467-AA8C44FA4CCA");
	const bytes metadata_library_guid = guid("44231C94-9498-49D1-A141-1D134E457054");
	const bytes audio_media_guid = guid("F8699E40-5B4D-11CF-A8FD-00805F5C442B");
	const bytes video_media_guid = guid("BC19EFC0-5B4D-11CF-A8FD-00805F5C442B");
	const bytes command_media_guid = guid("59DACFC0-59E6-11D0-A3AC-00A0C90348F6");
	const bytes jfif_media_guid = guid("B61BE100-5B4E-11CF-A8FD-00805F5C442B");
	const bytes no_error_correction_guid = guid("20FB5700-5B55-11CF-A8FD-00805F5C442B");
	const bytes extension_reserved_guid = guid("ABD3D211-A9BA-11CF-8EE6-00C00C205365");

	constexpr unsigned unicode_type = 0;
	constexpr unsigned bytes_type = 1;
	constexpr unsigned bool_type = 2;
	constexpr unsigned dword_type = 3;
	constexpr unsigned qword_type = 4;
	constexpr unsigned word_type = 5;
	constexpr unsigned guid_type = 6;

	bytes le64(const std::uint64_t v)
	{
		return builder().le32(static_cast<std::uint32_t>(v)).le32(static_cast<std::uint32_t>(v >> 32)).b;
	}

	std::uint64_t load_le64(const bytes& b, const std::size_t at)
	{
		std::uint64_t v = 0;
		for (auto i = 8; i-- > 0;) v = v << 8 | b[at + static_cast<std::size_t>(i)];
		return v;
	}

	bytes asf_object(const bytes& id, const bytes& payload)
	{
		return builder().raw(id).raw(le64(24 + payload.size())).raw(payload).b;
	}

	bytes utf16z(const std::string_view text)
	{
		auto out = libfiles::detail::utf16le(text);
		out.push_back(0);
		out.push_back(0);
		return out;
	}

	// 100 ns intervals since 1601, for a time in UTC given in seconds since 1970.
	std::uint64_t filetime(const std::int64_t unix_seconds)
	{
		return static_cast<std::uint64_t>(unix_seconds + 11'644'473'600) * 10'000'000;
	}

	struct properties_spec
	{
		std::uint64_t created = 0;
		// 100 ns, and milliseconds.
		std::uint64_t play = 0;
		std::uint64_t preroll = 0;
		// Seekable.
		std::uint32_t flags = 2;
		std::uint32_t max_bitrate = 0;
	};

	bytes file_properties(const properties_spec& p)
	{
		builder b;
		b.zeros(16).raw(le64(0)).raw(le64(p.created)).raw(le64(1)).raw(le64(p.play)).raw(le64(p.play)).raw(le64(p.preroll));
		b.le32(p.flags).le32(3200).le32(3200).le32(p.max_bitrate);
		return asf_object(file_properties_guid, b.b);
	}

	bytes stream_properties(const bytes& type, const unsigned number, const bytes& specific, const unsigned flags = 0)
	{
		builder b;
		b.raw(type).raw(no_error_correction_guid).raw(le64(0)).le32(static_cast<std::uint32_t>(specific.size())).le32(0);
		b.le16(number | flags).le32(0).raw(specific);
		return asf_object(stream_properties_guid, b.b);
	}

	bytes wave_format(const unsigned tag, const unsigned channels, const std::uint32_t rate, const std::uint32_t bytes_per_second,
	                  const unsigned bits, const bytes& extra = {})
	{
		return builder().le16(tag).le16(channels).le32(rate).le32(bytes_per_second).le16(channels * 2).le16(bits)
		                .le16(static_cast<unsigned>(extra.size())).raw(extra).b;
	}

	// The encoded size, a reserved byte, and the format data: a BITMAPINFOHEADER and what follows it.
	bytes video_format(const std::uint32_t width, const std::uint32_t height, const std::string_view fourcc, const bytes& extra = {})
	{
		builder bih;
		bih.le32(static_cast<std::uint32_t>(40 + extra.size())).le32(width).le32(height).le16(1).le16(24).raw(fourcc);
		bih.le32(width * height * 3).le32(0).le32(0).le32(0).le32(0).raw(extra);
		return builder().le32(width).le32(height).u8(2).le16(static_cast<unsigned>(bih.size())).raw(bih.b).b;
	}

	bytes content_description(const std::array<std::string_view, 5>& fields)
	{
		builder lengths;
		builder texts;

		for (const auto f : fields)
		{
			const auto text = f.empty() ? bytes{} : utf16z(f);
			lengths.le16(static_cast<unsigned>(text.size()));
			texts.raw(text);
		}

		return asf_object(content_description_guid, builder().raw(lengths.b).raw(texts.b).b);
	}

	bytes descriptor(const std::string_view name, const unsigned type, const bytes& value)
	{
		const auto n = utf16z(name);
		return builder().le16(static_cast<unsigned>(n.size())).raw(n).le16(type).le16(static_cast<unsigned>(value.size())).raw(value).b;
	}

	bytes extended_content(const std::vector<bytes>& descriptors)
	{
		builder b;
		b.le16(static_cast<unsigned>(descriptors.size()));
		for (const auto& d : descriptors) b.raw(d);
		return asf_object(extended_content_guid, b.b);
	}

	bytes record(const unsigned stream, const std::string_view name, const unsigned type, const bytes& value, const unsigned language = 0)
	{
		const auto n = utf16z(name);
		return builder().le16(language).le16(stream).le16(static_cast<unsigned>(n.size())).le16(type)
		                .le32(static_cast<std::uint32_t>(value.size())).raw(n).raw(value).b;
	}

	bytes metadata_object(const bytes& id, const std::vector<bytes>& records)
	{
		builder b;
		b.le16(static_cast<unsigned>(records.size()));
		for (const auto& r : records) b.raw(r);
		return asf_object(id, b.b);
	}

	bytes header_extension(const std::vector<bytes>& children)
	{
		builder data;
		for (const auto& c : children) data.raw(c);
		const auto size = static_cast<std::uint32_t>(data.size());
		return asf_object(header_extension_guid, builder().raw(extension_reserved_guid).le16(6).le32(size).raw(data.b).b);
	}

	struct extended_spec
	{
		unsigned number = 1;
		unsigned language = 0;
		// 100 ns.
		std::uint64_t frame_time = 0;
		std::uint32_t data_bitrate = 0;
		std::vector<std::string> names{};
		unsigned extension_systems = 0;
		bytes embedded{};
	};

	bytes extended_stream_properties(const extended_spec& e)
	{
		builder b;
		b.raw(le64(0)).raw(le64(0)).le32(e.data_bitrate).le32(5000).le32(0).le32(e.data_bitrate).le32(5000).le32(0).le32(0).le32(2);
		b.le16(e.number).le16(e.language).raw(le64(e.frame_time)).le16(static_cast<unsigned>(e.names.size())).le16(e.extension_systems);

		for (const auto& n : e.names)
		{
			const auto text = utf16z(n);
			b.le16(0).le16(static_cast<unsigned>(text.size())).raw(text);
		}

		for (unsigned i = 0; i < e.extension_systems; ++i)
		{
			b.raw(guid("399595EC-8667-4E2D-8FDB-98814CE76C1E")).le16(0xFFFF).le32(4).le32(0x12345678);
		}

		b.raw(e.embedded);
		return asf_object(extended_stream_properties_guid, b.b);
	}

	bytes language_list(const std::vector<std::string_view>& languages)
	{
		builder b;
		b.le16(static_cast<unsigned>(languages.size()));

		for (const auto l : languages)
		{
			const auto text = utf16z(l);
			b.u8(static_cast<unsigned>(text.size())).raw(text);
		}

		return asf_object(language_list_guid, b.b);
	}

	bytes stream_bitrates(const std::vector<std::pair<unsigned, std::uint32_t>>& rates)
	{
		builder b;
		b.le16(static_cast<unsigned>(rates.size()));
		for (const auto& [number, rate] : rates) b.le16(number).le32(rate);
		return asf_object(stream_bitrate_guid, b.b);
	}

	bytes wm_picture(const unsigned type, const std::string_view mime, const std::string_view description, const bytes& image)
	{
		return builder().u8(type).le32(static_cast<std::uint32_t>(image.size())).raw(utf16z(mime)).raw(utf16z(description)).raw(image).b;
	}

	bytes xmp_object(const std::string_view packet)
	{
		return asf_object(xmp_guid, to_bytes(packet));
	}

	bytes simple_index()
	{
		return asf_object(simple_index_guid, bytes(32, 0x22));
	}

	bytes asf_file(const std::vector<bytes>& children, const bytes& media = bytes(64, 0xAB), const std::vector<bytes>& after = {})
	{
		builder header;
		header.le32(static_cast<std::uint32_t>(children.size())).u8(1).u8(2);
		for (const auto& c : children) header.raw(c);

		builder file;
		file.raw(asf_object(header_guid, header.b));
		file.raw(asf_object(data_guid, builder().zeros(16).raw(le64(1)).u8(1).u8(1).raw(media).b));
		for (const auto& a : after) file.raw(a);
		return file.b;
	}

	bool holds(const bytes& file, const libfiles::byte_range& at, const bytes& expected)
	{
		return at.size == expected.size() && at.offset <= file.size() && at.size <= file.size() - at.offset &&
			std::equal(expected.begin(), expected.end(), file.begin() + static_cast<std::ptrdiff_t>(at.offset));
	}

	bool is_guid(const bytes& file, const std::size_t at, const bytes& id)
	{
		return file.size() >= at + 16 && std::equal(id.begin(), id.end(), file.begin() + static_cast<std::ptrdiff_t>(at));
	}

	// Where each object of an ASF file starts and ends: the top-level objects, the header's, and its
	// extension's.
	std::vector<std::size_t> asf_boundaries(const bytes& file)
	{
		std::vector<std::size_t> out;

		const auto walk = [&](const auto& self, std::size_t pos, const std::size_t end, const int depth) -> void
		{
			while (end - pos >= 24)
			{
				const auto size = load_le64(file, pos + 16);
				if (size < 24 || size > end - pos) return;

				out.push_back(pos);
				out.push_back(pos + static_cast<std::size_t>(size));

				if (depth == 0 && is_guid(file, pos, header_guid)) self(self, pos + 30, pos + static_cast<std::size_t>(size), 1);
				if (depth == 1 && is_guid(file, pos, header_extension_guid)) self(self, pos + 46, pos + static_cast<std::size_t>(size), 2);
				pos += static_cast<std::size_t>(size);
			}
		};

		walk(walk, 0, file.size(), 0);
		return out;
	}

	// A WMA: one WMA 9 stream, its tags in the Content Description and the Extended Content Description.
	bytes wma_file(const std::vector<bytes>& descriptors)
	{
		// 2011-03-13T15:13:49Z; ten and a half seconds after a preroll of 1.5.
		const properties_spec p{.created = filetime(1300029229), .play = 120'000'000, .preroll = 1500, .max_bitrate = 128000};
		const auto audio = wave_format(0x0161, 2, 44100, 16000, 16, bytes(10, 0));

		return asf_file({
			file_properties(p),
			header_extension({}),
			stream_properties(audio_media_guid, 1, audio),
			content_description({"Song", "Artist", "(c) 2011", "A description", "PG"}),
			extended_content(descriptors),
		});
	}
}

// ---- ASF ---------------------------------------------------------------------------------------------

TEST("media asf reads an audio file's stream, duration, date and tags")
{
	const auto file = wma_file({
		descriptor("WM/AlbumTitle", unicode_type, utf16z("The Album")),
		descriptor("WM/TrackNumber", dword_type, builder().le32(5).b),
		descriptor("WM/Category", unicode_type, utf16z("Live")),
		descriptor("WM/SharedUserRating", dword_type, builder().le32(75).b),
		descriptor("WM/EncodingSettings", unicode_type, utf16z("Lavf62.12.102")),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK(s.format == libfiles::media_format::asf);
	CHECK_STR(s.brand, "ASF");
	CHECK(near(s.duration, 10.5));
	CHECK_EQ(s.bit_rate, static_cast<std::uint64_t>(static_cast<double>(file.size()) * 8 / 10.5));

	CHECK_EQ(s.streams.size(), 1u);
	if (!s.streams.empty())
	{
		const auto& a = s.streams[0];
		CHECK(a.kind == media_stream_kind::audio);
		CHECK_EQ(a.id, 1u);
		CHECK(near(a.duration, 10.5));
		// The WAVEFORMATEX's bytes a second, as FFmpeg takes them.
		CHECK_EQ(a.bit_rate, 128000u);
		CHECK_STR(a.codec, "wmav2");
		CHECK_STR(a.codec_tag, "0x0161");
		CHECK_EQ(a.audio.sample_rate, 44100u);
		CHECK_EQ(a.audio.channels, 2u);
		// A lossy codec's samples have no depth.
		CHECK_EQ(a.audio.bits_per_sample, 0u);
	}

	CHECK(s.complete);

	check_tags(s.tags, {
		{"Title", "Song"},
		{"Author", "Artist"},
		{"Copyright", "(c) 2011"},
		{"Description", "A description"},
		{"Rating", "PG"},
		{"WM/AlbumTitle", "The Album"},
		{"WM/TrackNumber", "5"},
		{"WM/Category", "Live"},
		{"WM/SharedUserRating", "75"},
		{"WM/EncodingSettings", "Lavf62.12.102"},
	});

	// The File Properties' creation date, in UTC.
	const auto& created = s.properties.created;
	CHECK_EQ(created.year, 2011);
	CHECK_EQ(created.month, 3);
	CHECK_EQ(created.day, 13);
	CHECK(created.has_time);
	CHECK_EQ(created.hour, 15);
	CHECK_EQ(created.minute, 13);
	CHECK_EQ(created.second, 49);
	CHECK(created.zone == libfiles::media_date::time_zone::utc);

	// What the tags map to, as FFmpeg names them.
	const auto& p = s.properties;
	CHECK_STR(p.title, "Song");
	CHECK_STR(p.artist, "Artist");
	CHECK_STR(p.copyright, "(c) 2011");
	CHECK_STR(p.album, "The Album");
	CHECK_EQ(p.track, 5);
	CHECK_STR(p.encoder, "Lavf62.12.102");
	CHECK(p.keywords == std::vector<std::string>{"Live"});
	CHECK(p.rating == std::optional<int>(4));
	CHECK(s.xmp.empty());
	CHECK(s.pictures.empty());
}

TEST("media asf reads a value of every descriptor type")
{
	const auto file = asf_file({
		file_properties({.play = 20'000'000}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
		extended_content({
			descriptor("Text", unicode_type, utf16z(" Zürich ")),
			descriptor("Bytes", bytes_type, bytes(12, 0x5A)),
			descriptor("One byte", bytes_type, bytes(1, 0x5A)),
			// Described bytes give no property their value.
			descriptor("WM/AlbumTitle", bytes_type, bytes(3, 0x41)),
			descriptor("Flag", bool_type, builder().le32(1).b),
			descriptor("Double word", dword_type, builder().le32(4000000000u).b),
			descriptor("Quad word", qword_type, le64(0x0000010000000005ull)),
			descriptor("Word", word_type, builder().le16(65535).b),
			descriptor("Empty text", unicode_type, utf16z("")),
			descriptor("No bytes", bytes_type, {}),
			descriptor("Undefined type", 9, bytes(4, 1)),
		}),
		header_extension({
			metadata_object(metadata_library_guid, {
				record(0, "WM/MediaClassPrimaryID", guid_type, guid("D1607DBC-E323-4BE2-86A1-48A42A28441E")),
				// A Metadata Library BOOL is 16 bits.
				record(0, "IsProtected", bool_type, builder().le16(0).b),
				record(0, "Library text", unicode_type, utf16z("from the library")),
			}),
		}),
	});

	const auto s = scan(file);
	CHECK(s.success);

	// The header extension comes after the descriptors, so its records do too; text is trimmed.
	check_tags(s.tags, {
		{"Text", "Zürich"},
		{"Bytes", "12 bytes"},
		{"One byte", "1 byte"},
		{"WM/AlbumTitle", "3 bytes"},
		{"Flag", "1"},
		{"Double word", "4000000000"},
		{"Quad word", "1099511627781"},
		{"Word", "65535"},
		{"WM/MediaClassPrimaryID", "{D1607DBC-E323-4BE2-86A1-48A42A28441E}"},
		{"IsProtected", "0"},
		{"Library text", "from the library"},
	});

	CHECK(s.properties.album.empty());
}

TEST("media asf reads a video file's streams, frame rate, names, languages and bit rates")
{
	const auto video = video_format(640, 480, "WMV3", bytes{0x4E, 0xF9, 0x2A, 0x01});
	const auto audio = wave_format(0x0161, 2, 48000, 24000, 16, bytes(10, 0));

	const auto file = asf_file({
		file_properties({.created = filetime(1300029229), .play = 61'000'000, .preroll = 3100, .max_bitrate = 1400000}),
		header_extension({
			language_list({"en-us", "fr-fr"}),
			// 333667 hundreds of nanoseconds a frame: 29.97 frames a second.
			extended_stream_properties({.number = 1, .language = 0, .frame_time = 333667, .data_bitrate = 1000000, .names = {"Main video"}}),
			extended_stream_properties({.number = 2, .language = 1, .data_bitrate = 192000, .names = {"Commentaire", "Second name"},
			                            .extension_systems = 2}),
			metadata_object(metadata_guid, {
				record(1, "AspectRatioX", dword_type, builder().le32(16).b),
				record(1, "AspectRatioY", dword_type, builder().le32(12).b),
				record(1, "DeviceConformanceTemplate", unicode_type, utf16z("MP@ML")),
				record(9, "Of no stream", unicode_type, utf16z("dropped")),
			}),
		}),
		stream_properties(video_media_guid, 1, video),
		stream_properties(audio_media_guid, 2, audio),
		stream_bitrates({{1, 1200000}, {2, 130000}}),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK(near(s.duration, 3.0));
	CHECK(s.tags.empty());
	CHECK_EQ(s.streams.size(), 2u);
	if (s.streams.size() != 2) return;

	const auto& v = s.streams[0];
	CHECK(v.kind == media_stream_kind::video);
	CHECK_EQ(v.id, 1u);
	CHECK_EQ(v.video.width, 640u);
	CHECK_EQ(v.video.height, 480u);
	CHECK(std::abs(v.video.frame_rate - 29.97) < 0.001);
	CHECK_STR(v.title, "Main video");
	CHECK_STR(v.language, "en-us");
	// The Stream Bitrate Properties, ahead of the extended stream properties' leaky bucket.
	CHECK_EQ(v.bit_rate, 1200000u);
	CHECK_EQ(v.video.pixel_aspect_width, 4u);
	CHECK_EQ(v.video.pixel_aspect_height, 3u);
	check_tags(v.tags, {{"AspectRatioX", "16"}, {"AspectRatioY", "12"}, {"DeviceConformanceTemplate", "MP@ML"}});
	CHECK(near(v.duration, 3.0));
	CHECK_STR(v.codec, "wmv3");
	CHECK_STR(v.codec_tag, "WMV3");
	// WMV 9 is 4:2:0 at 8 bits whatever the stream.
	CHECK_STR(v.video.pixel_format(), "yuv420");

	const auto& a = s.streams[1];
	CHECK(a.kind == media_stream_kind::audio);
	CHECK_EQ(a.id, 2u);
	CHECK_STR(a.title, "Commentaire");
	CHECK_STR(a.language, "fr-fr");
	CHECK_EQ(a.bit_rate, 192000u);
	CHECK(a.tags.empty());
	CHECK_EQ(a.audio.sample_rate, 48000u);

	CHECK(s.complete);
}

TEST("media asf collects WM/Category from the extended content description and the metadata library")
{
	// As Windows writes them: one descriptor a keyword, some in each object.
	const auto file = asf_file({
		file_properties({.play = 20'000'000}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
		extended_content({
			descriptor("WM/Category", unicode_type, utf16z("AsfTagA")),
			descriptor("WM/Category", unicode_type, utf16z("AsfTagB")),
		}),
		header_extension({
			metadata_object(metadata_library_guid, {record(0, "WM/Category", unicode_type, utf16z("AsfTagC"))}),
		}),
	});

	const auto s = scan(file);
	CHECK(s.success);
	check_tags(s.tags, {{"WM/Category", "AsfTagA"}, {"WM/Category", "AsfTagB"}, {"WM/Category", "AsfTagC"}});
	CHECK(s.properties.keywords == std::vector<std::string>({"AsfTagA", "AsfTagB", "AsfTagC"}));
}

TEST("media asf locates WM/Picture rather than reading it")
{
	bytes front(3000);
	for (std::size_t i = 0; i < front.size(); ++i) front[i] = static_cast<std::uint8_t>(i * 7);
	bytes back(150000, 0x42);
	back[0] = 0x89;

	const auto file = asf_file({
		file_properties({.play = 20'000'000}),
		header_extension({
			metadata_object(metadata_library_guid, {record(0, "WM/Picture", bytes_type, wm_picture(4, "image/png", "", back))}),
		}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
		extended_content({
			descriptor("WM/Picture", bytes_type, wm_picture(3, "image/jpeg", "Front", front)),
			// An image larger than the value holds is no picture, and an unknown type is "Other".
			descriptor("WM/Picture", bytes_type, builder().u8(3).le32(5000).raw(utf16z("image/jpeg")).raw(utf16z("")).raw(bytes(10, 1)).b),
			descriptor("WM/Picture", bytes_type, wm_picture(99, "image/gif", "Odd", bytes(5, 7))),
		}),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK_EQ(s.pictures.size(), 3u);
	if (s.pictures.size() != 3) return;

	CHECK_EQ(s.pictures[0].type, 4);
	CHECK_STR(s.pictures[0].mime_type, "image/png");
	CHECK_STR(s.pictures[0].description, "");
	CHECK(holds(file, s.pictures[0].location, back));
	CHECK(s.pictures[0].data.empty());

	CHECK_EQ(s.pictures[1].type, 3);
	CHECK_STR(s.pictures[1].mime_type, "image/jpeg");
	CHECK_STR(s.pictures[1].description, "Front");
	CHECK(holds(file, s.pictures[1].location, front));

	CHECK_EQ(s.pictures[2].type, 0);
	CHECK_STR(s.pictures[2].mime_type, "image/gif");
	CHECK(holds(file, s.pictures[2].location, bytes(5, 7)));

	// Each is a tag too, described: a picture by its type and size, bytes that are none by their count.
	check_tags(s.tags, {
		{"WM/Picture", "image/png, back cover, 150000 bytes"},
		{"WM/Picture", "image/jpeg, front cover, 3000 bytes"},
		{"WM/Picture", "39 bytes"},
		{"WM/Picture", "image/gif, other, 5 bytes"},
	});

	// Over a stream, the large image is stepped over, not read.
	libfiles::memory_stream stream(file);
	libfiles::reader r(stream);
	const auto streamed = libfiles::scan_media(r);
	CHECK_EQ(streamed.pictures.size(), 3u);
	CHECK(r.source_bytes() < 3 * libfiles::reader::default_block_size);
	CHECK(r.source_reads() <= 4);
}

TEST("media asf keeps a hidden stream declared in its extended stream properties")
{
	const auto hidden = stream_properties(video_media_guid, 3, video_format(320, 240, "WMV2"));

	const auto file = asf_file({
		file_properties({.play = 20'000'000}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
		header_extension({
			language_list({"UND", "de-de"}),
			extended_stream_properties({.number = 3, .frame_time = 400000, .names = {"Low rate"}, .extension_systems = 1, .embedded = hidden}),
			extended_stream_properties({.number = 1, .language = 1}),
		}),
		stream_properties(command_media_guid, 4, {}),
		stream_properties(jfif_media_guid, 5, builder().le32(640).le32(480).le32(0).b),
	});

	const auto s = scan(file, libfiles::scan_intent::inspect);
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), 4u);
	if (s.streams.size() != 4) return;

	CHECK(s.streams[0].kind == media_stream_kind::audio);
	CHECK_STR(s.streams[0].language, "de-de");
	CHECK(s.streams[1].kind == media_stream_kind::video);
	CHECK_EQ(s.streams[1].id, 3u);
	CHECK_EQ(s.streams[1].video.width, 320u);
	CHECK_EQ(s.streams[1].video.height, 240u);
	CHECK(near(s.streams[1].video.frame_rate, 25.0));
	CHECK_STR(s.streams[1].title, "Low rate");
	CHECK_STR(s.streams[1].codec, "wmv2");
	// "und" names no language.
	CHECK_STR(s.streams[1].language, "");

	// Script commands and slides are data.
	CHECK(s.streams[2].kind == media_stream_kind::data);
	CHECK_EQ(s.streams[2].id, 4u);
	CHECK(s.streams[3].kind == media_stream_kind::data);
	CHECK_STR(s.streams[3].codec, "mjpeg");

	const auto hidden_row = std::ranges::find_if(s.structure, [](const libfiles::structure_row& row) { return row.key == "Stream 3"; });
	CHECK(hidden_row != s.structure.end() && hidden_row->value.find("hidden") != std::string::npos);
}

TEST("media asf takes a stream's language from its WM/Language record when the Language List gives none")
{
	// As FFmpeg reads them: a stream's own records, then the Language List over them.
	const auto file = asf_file({
		file_properties({.play = 20'000'000}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
		stream_properties(audio_media_guid, 2, wave_format(0x0161, 2, 44100, 16000, 16)),
		stream_properties(audio_media_guid, 3, wave_format(0x0161, 2, 44100, 16000, 16)),
		header_extension({
			language_list({"en-us"}),
			extended_stream_properties({.number = 1, .language = 0}),
			metadata_object(metadata_guid, {
				record(1, "WM/Language", unicode_type, utf16z("de-de")),
				record(2, "WM/Language", unicode_type, utf16z("fr-fr")),
				record(3, "WM/Language", unicode_type, utf16z("und")),
			}),
			metadata_object(metadata_library_guid, {
				// The last record wins, and FFmpeg matches the name whatever its case.
				record(2, "wm/language", unicode_type, utf16z("es-es")),
			}),
		}),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), 3u);
	if (s.streams.size() != 3) return;

	CHECK_STR(s.streams[0].language, "en-us");
	CHECK_STR(s.streams[1].language, "es-es");
	// "und" names no language.
	CHECK_STR(s.streams[2].language, "");
	// The records are still the streams' tags.
	check_tags(s.streams[0].tags, {{"WM/Language", "de-de"}});
}

TEST("media asf takes its packet from the first XMP object after the data")
{
	const auto children = std::vector<bytes>{
		file_properties({.play = 20'000'000}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
	};

	const std::string first = "<x:xmpmeta xmlns:x='adobe:ns:meta/'>first</x:xmpmeta>";
	const std::string second = "<x:xmpmeta xmlns:x='adobe:ns:meta/'>second</x:xmpmeta>";

	const auto file = asf_file(children, bytes(64, 0xAB), {simple_index(), xmp_object(first), xmp_object(second)});
	const auto s = scan(file);
	CHECK(s.success);
	CHECK_STR(to_string(s.xmp), first);
	CHECK_STR(to_string(s.xmp), packet_of(file));

	// One before the data is not the packet, as the writer reads it.
	builder before;
	const auto plain = asf_file(children);
	const auto header_size = static_cast<std::size_t>(load_le64(plain, 16));
	before.raw(bytes(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(header_size))).raw(xmp_object(first));
	before.raw(bytes(plain.begin() + static_cast<std::ptrdiff_t>(header_size), plain.end()));
	const auto early = scan(before.b);
	CHECK(early.success);
	CHECK(early.xmp.empty());

	// A file cut short in its data still scans, and has no packet, as read_packet reads it.
	const bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(header_size + 40));
	const auto partial = scan(cut);
	CHECK(partial.success);
	CHECK(partial.xmp.empty());
	CHECK_EQ(partial.streams.size(), 1u);
}

TEST("media asf takes no duration or date from a broadcast file")
{
	const auto file = asf_file({
		file_properties({.created = filetime(1300029229), .play = 120'000'000, .preroll = 1500, .flags = 1}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK_EQ(s.duration, 0.0);
	CHECK(!s.properties.created.valid());
	CHECK(!s.complete);
	CHECK(s.streams.size() == 1 && s.streams[0].duration == 0.0);

	// A FILETIME of zero is no date.
	const auto undated = scan(asf_file({
		file_properties({.created = 0, .play = 20'000'000}),
		stream_properties(audio_media_guid, 1, wave_format(0x0161, 2, 44100, 16000, 16)),
	}));
	CHECK(!undated.properties.created.valid());
	CHECK(near(undated.duration, 2.0));
}

TEST("media asf reports its structure")
{
	const auto file = asf_file({
		file_properties({.created = filetime(0), .play = 51'000'000, .preroll = 3100, .max_bitrate = 1000}),
		header_extension({language_list({"en-us"}), asf_object(padding_guid, bytes(10, 0))}),
		stream_properties(video_media_guid, 1, video_format(160, 120, "MP42"), 0x8000),
	}, bytes(100, 0), {simple_index(), xmp_object("<x/>")});

	const auto s = scan(file, libfiles::scan_intent::inspect);
	CHECK(s.success);

	std::vector<std::string> keys;
	for (const auto& row : s.structure) keys.push_back(row.key);

	const std::vector<std::string> expected = {
		"Summary (7)", "File size", "Play duration", "Preroll", "Flags", "Data packets", "Packet size", "Maximum bit rate",
		"Objects (4)", "Header", "Data", "Simple Index", "XMP",
		"Header (5)", "File Properties", "Header Extension", "Language List", "Padding", "Stream Properties",
		"Streams (1)", "Stream 1",
	};

	CHECK(keys == expected);

	const auto row = [&](const std::string_view key) -> const libfiles::structure_row*
	{
		const auto it = std::ranges::find(s.structure, key, &libfiles::structure_row::key);
		return it != s.structure.end() ? &*it : nullptr;
	};

	CHECK(row("Play duration") && row("Play duration")->value == "5.100 s");
	CHECK(row("Preroll") && row("Preroll")->value == "3100 ms");
	CHECK(row("Flags") && row("Flags")->value == "seekable");
	CHECK(row("Language List") && row("Language List")->depth == 2);
	CHECK(row("Stream 1") && row("Stream 1")->value.find("encrypted") != std::string::npos);
	CHECK(row("Data") && row("Data")->value == std::format("offset {}, 150 bytes", load_le64(file, 16)));

	// Indexing reports none of it.
	CHECK(scan(file).structure.empty());
}

TEST("media asf refuses a header it cannot read")
{
	const auto file = wma_file({});

	// The header object cut short, or its size not that of a header.
	const bytes cut(file.begin(), file.begin() + 200);
	const auto short_header = scan(cut);
	CHECK(!short_header.success);
	CHECK(short_header.format == libfiles::media_format::asf);
	CHECK(!short_header.error.empty());

	auto tiny = file;
	std::ranges::copy(le64(29), tiny.begin() + 16);
	CHECK(!scan(tiny).success);

	// A header without a stream describes nothing to play.
	const auto empty = scan(asf_file({file_properties({.play = 20'000'000})}));
	CHECK(!empty.success);
	CHECK(!empty.error.empty());
}

TEST("media asf survives files cut short")
{
	bytes cover(200);
	for (std::size_t i = 0; i < cover.size(); ++i) cover[i] = static_cast<std::uint8_t>(i);

	const auto file = asf_file({
		file_properties({.created = filetime(1300029229), .play = 61'000'000, .preroll = 3100}),
		header_extension({
			language_list({"en-us", "fr-fr"}),
			extended_stream_properties({.number = 1, .frame_time = 333667, .data_bitrate = 1000000, .names = {"Video"}, .extension_systems = 1,
			                            .embedded = stream_properties(video_media_guid, 3, video_format(320, 240, "WMV2"))}),
			metadata_object(metadata_guid, {record(1, "AspectRatioX", dword_type, builder().le32(4).b)}),
			metadata_object(metadata_library_guid, {record(0, "WM/Category", unicode_type, utf16z("C")),
			                                         record(0, "WM/Picture", bytes_type, wm_picture(3, "image/jpeg", "", cover))}),
		}),
		stream_properties(video_media_guid, 1, video_format(640, 480, "WMV3", bytes(4, 1))),
		stream_properties(audio_media_guid, 2, wave_format(0x0161, 2, 44100, 16000, 16, bytes(10, 0))),
		content_description({"Title", "Author", "", "", ""}),
		extended_content({descriptor("WM/Category", unicode_type, utf16z("A")), descriptor("WM/Year", unicode_type, utf16z("2011"))}),
		stream_bitrates({{1, 1200000}, {2, 128000}}),
	}, bytes(300, 0x11), {simple_index(), xmp_object("<x:xmpmeta xmlns:x='adobe:ns:meta/'/>")});

	const auto whole = scan(file);
	CHECK(whole.success);
	CHECK_EQ(whole.streams.size(), 3u);
	CHECK_EQ(whole.pictures.size(), 1u);
	check_consistent(file, whole);

	check_cuts(file, asf_boundaries(file));

	// Cut anywhere after its header, a file reads as it did, but for its packet.
	const auto header_size = static_cast<std::size_t>(load_le64(file, 16));

	for (auto n = header_size; n < file.size(); n += 37)
	{
		const bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
		const auto s = scan(cut);
		CHECK(s.success);
		CHECK_EQ(s.streams.size(), 3u);
		CHECK_EQ(s.tags.size(), whole.tags.size());
		CHECK_EQ(s.pictures.size(), 1u);
		CHECK(near(s.duration, whole.duration));
	}
}

TEST("media asf survives damaged sizes and counts")
{
	const auto file = asf_file({
		file_properties({.created = filetime(1300029229), .play = 61'000'000, .preroll = 3100}),
		header_extension({
			language_list({"en-us"}),
			extended_stream_properties({.number = 1, .frame_time = 333667, .names = {"V"}, .extension_systems = 1,
			                            .embedded = stream_properties(video_media_guid, 3, video_format(32, 24, "WMV2"))}),
			metadata_object(metadata_library_guid, {record(2, "Lang", unicode_type, utf16z("x")),
			                                         record(0, "WM/Picture", bytes_type, wm_picture(3, "image/png", "d", bytes(16, 9)))}),
		}),
		stream_properties(video_media_guid, 1, video_format(64, 48, "WMV3", bytes(4, 1))),
		stream_properties(audio_media_guid, 2, wave_format(0x0161, 2, 44100, 16000, 16, bytes(4, 0))),
		content_description({"T", "A", "C", "D", "R"}),
		extended_content({descriptor("WM/Category", unicode_type, utf16z("A")), descriptor("N", dword_type, builder().le32(7).b)}),
		stream_bitrates({{1, 1200000}}),
	}, bytes(40, 0x11), {simple_index(), xmp_object("<x/>")});

	CHECK(scan(file).success);
	check_damage(file, 0, static_cast<std::size_t>(load_le64(file, 16)) + 80);
}

TEST("media asf reads a large file in a few blocks")
{
	const auto file = asf_file({
		file_properties({.play = 600'000'000}),
		stream_properties(video_media_guid, 1, video_format(640, 480, "WVC1")),
		stream_properties(audio_media_guid, 2, wave_format(0x0161, 2, 44100, 16000, 16)),
		extended_content({descriptor("WM/Category", unicode_type, utf16z("A"))}),
	}, bytes(4 * 1024 * 1024, 0x5A), {simple_index(), xmp_object("<x/>")});

	libfiles::memory_stream stream(file);
	libfiles::reader r(stream);
	const auto s = libfiles::scan_media(r);
	CHECK(s.success);
	CHECK_STR(to_string(s.xmp), "<x/>");
	CHECK_EQ(s.streams.size(), 2u);

	// The header's block, and the block at the end that holds the index and the packet.
	CHECK_EQ(r.source_reads(), 2u);
	CHECK(r.source_bytes() <= 2 * libfiles::reader::default_block_size);
}

TEST("media asf reads the categories, rating, title and date its writer wrote")
{
	const auto original = asf_file({
		file_properties({.created = filetime(0), .play = 20'000'000}),
		header_extension({}),
		stream_properties(video_media_guid, 1, video_format(160, 120, "WMV2")),
	});

	libfiles::xmp::meta meta;
	meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", 4);
	libfiles::metadata_update u{&meta};
	u.legacy.title = "A title";
	u.legacy.keywords = std::vector<std::string>{"AsfTagA", "AsfTagB", "AsfTagC"};
	u.legacy.rating = 4;
	u.legacy.date_digitized = "2011-03-13T15:13:49Z";

	libfiles::reader r(original);
	const auto plan = libfiles::plan_update(r, u);
	CHECK(plan.ok());
	libfiles::memory_stream written;
	libfiles::write_updated(r, plan, written);

	const auto s = scan(written.bytes());
	CHECK(s.success);
	CHECK_STR(tag_value(s.tags, "Title"), "A title");
	CHECK_EQ(std::ranges::count(s.tags, std::string("WM/Category"), &libfiles::media_tag::key), 3);
	CHECK_STR(tag_value(s.tags, "WM/SharedUserRating"), "75");
	CHECK(s.properties.keywords == std::vector<std::string>({"AsfTagA", "AsfTagB", "AsfTagC"}));
	CHECK(s.properties.rating == std::optional<int>(4));
	CHECK_EQ(s.properties.created.year, 2011);
	CHECK_EQ(s.properties.created.hour, 15);
	CHECK(!s.xmp.empty());
	CHECK_STR(to_string(s.xmp), packet_of(written.bytes()));
}

// ---- FLV ---------------------------------------------------------------------------------------------

namespace
{
	bytes flv_header(const unsigned flags)
	{
		return builder().raw("FLV").u8(1).u8(flags).be32(9).be32(0).b;
	}

	bytes flv_tag(const unsigned type, const std::uint32_t timestamp, const bytes& data)
	{
		builder b;
		b.u8(type).u8(data.size() >> 16 & 0xFF).u8(data.size() >> 8 & 0xFF).u8(data.size() & 0xFF);
		b.u8(timestamp >> 16 & 0xFF).u8(timestamp >> 8 & 0xFF).u8(timestamp & 0xFF).u8(timestamp >> 24).u8(0).u8(0).u8(0);
		b.raw(data).be32(static_cast<std::uint32_t>(11 + data.size()));
		return b.b;
	}

	bytes amf_name(const std::string_view s)
	{
		return builder().be16(static_cast<unsigned>(s.size())).raw(s).b;
	}

	bytes amf_string(const std::string_view s)
	{
		return builder().u8(2).raw(amf_name(s)).b;
	}

	bytes amf_long_string(const std::string_view s)
	{
		return builder().u8(12).be32(static_cast<std::uint32_t>(s.size())).raw(s).b;
	}

	bytes amf_number(const double v)
	{
		const auto bits = std::bit_cast<std::uint64_t>(v);
		return builder().u8(0).be32(static_cast<std::uint32_t>(bits >> 32)).be32(static_cast<std::uint32_t>(bits)).b;
	}

	bytes amf_boolean(const bool v)
	{
		return builder().u8(1).u8(v ? 1 : 0).b;
	}

	bytes amf_date(const double milliseconds, const unsigned zone = 0)
	{
		const auto bits = std::bit_cast<std::uint64_t>(milliseconds);
		return builder().u8(11).be32(static_cast<std::uint32_t>(bits >> 32)).be32(static_cast<std::uint32_t>(bits)).be16(zone).b;
	}

	using amf_properties = std::vector<std::pair<std::string, bytes>>;

	bytes amf_members(const amf_properties& properties)
	{
		builder b;
		for (const auto& [name, value] : properties) b.raw(amf_name(name)).raw(value);
		return b.be16(0).u8(9).b;
	}

	bytes amf_ecma_array(const amf_properties& properties)
	{
		return builder().u8(8).be32(static_cast<std::uint32_t>(properties.size())).raw(amf_members(properties)).b;
	}

	bytes amf_object(const amf_properties& properties)
	{
		return builder().u8(3).raw(amf_members(properties)).b;
	}

	bytes amf_strict_array(const std::vector<bytes>& values)
	{
		builder b;
		b.u8(10).be32(static_cast<std::uint32_t>(values.size()));
		for (const auto& v : values) b.raw(v);
		return b.b;
	}

	bytes script_tag(const std::string_view name, const bytes& value, const std::uint32_t timestamp = 0)
	{
		return flv_tag(18, timestamp, builder().raw(amf_string(name)).raw(value).b);
	}

	// High profile, 1920 x 1088 cropped to 1080, 4:2:0 at 8 bits.
	bytes h264_sps()
	{
		bit_writer s;
		s.u(100, 8).u(0, 8).u(40, 8).ue(0);
		s.ue(1).ue(0).ue(0).u(0, 1).u(0, 1);
		s.ue(0).ue(2).ue(1).u(0, 1);
		s.ue(119).ue(67).u(1, 1).u(1, 1);
		s.u(1, 1).ue(0).ue(0).ue(0).ue(4);
		s.u(0, 1).trailing();
		return nal_unit({0x67}, s.b);
	}

	bytes avc_config()
	{
		const auto sps = h264_sps();
		const bytes pps = {0x68, 0xEE, 0x3C, 0x80};
		return builder().u8(1).u8(sps[1]).u8(sps[2]).u8(sps[3]).u8(0xFF).u8(0xE1).be16(static_cast<unsigned>(sps.size())).raw(sps)
		                .u8(1).be16(static_cast<unsigned>(pps.size())).raw(pps).b;
	}

	// Main 10's profile, tier and level: progressive frames at level 3.1.
	void hevc_profile_tier_level(bit_writer& s)
	{
		s.u(0, 2).u(0, 1).u(2, 5).u(0x20000000, 32);
		s.u(1, 1).u(0, 1).u(0, 1).u(1, 1).u(0, 43).u(0, 1).u(93, 8);
	}

	// Main 10, 1280 x 720, 4:2:0: the record's own fields, then its VPS, SPS and PPS.
	bytes hevc_config()
	{
		bit_writer v;
		v.u(0, 4).u(1, 1).u(1, 1).u(0, 6).u(0, 3).u(1, 1).u(0xFFFF, 16);
		hevc_profile_tier_level(v);
		v.u(1, 1).ue(4).ue(2).ue(0);
		v.u(0, 6).ue(0).u(0, 1).u(0, 1).trailing();

		bit_writer s;
		s.u(0, 4).u(0, 3).u(1, 1);
		hevc_profile_tier_level(s);
		s.ue(0).ue(1).ue(1280).ue(720).u(0, 1);
		s.ue(2).ue(2).ue(4);
		s.u(1, 1).ue(4).ue(2).ue(0);
		s.ue(0).ue(3).ue(0).ue(3).ue(0).ue(0);
		s.u(0, 1).u(0, 1).u(1, 1).u(0, 1);
		s.ue(0).u(0, 1).u(1, 1).u(1, 1);
		s.u(0, 1).u(0, 1).trailing();

		bit_writer p;
		p.ue(0).ue(0).u(0, 1).u(0, 1).u(0, 3).u(0, 1).u(0, 1).ue(0).ue(0).ue(0);
		p.u(0, 1).u(0, 1).u(0, 1).ue(0).ue(0).u(0, 10).ue(0).u(0, 1).u(0, 1).trailing();

		const std::pair<unsigned, bytes> arrays[] = {
			{32, nal_unit({0x40, 0x01}, v.b)},
			{33, nal_unit({0x42, 0x01}, s.b)},
			{34, nal_unit({0x44, 0x01}, p.b)},
		};

		builder b;
		b.u8(1).u8(0x02).be32(0x20000000).u8(0x90).zeros(5).u8(93);
		b.be16(0xF000).u8(0xFC).u8(0xFD).u8(0xFA).u8(0xFA).be16(0).u8(0x0F);
		b.u8(3);
		for (const auto& [type, unit] : arrays) b.u8(0x80 | type).be16(1).be16(static_cast<unsigned>(unit.size())).raw(unit);
		return b.b;
	}

	// Main profile, 1280 x 720, 4:2:0 at 8 bits, BT.709: the av1C fields, then the sequence header OBU.
	bytes av1_config()
	{
		bit_writer s;
		s.u(0, 3).u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 5).u(0, 12).u(8, 5).u(0, 1);
		s.u(10, 4).u(10, 4).u(1279, 11).u(719, 11);
		s.u(0, 1).u(0, 1).u(0, 1).u(0, 1);
		s.u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(1, 1).u(0, 1).u(0, 1);
		s.u(1, 1).u(1, 1).u(6, 3);
		s.u(0, 1).u(1, 1).u(1, 1);
		s.u(0, 1).u(0, 1).u(1, 1).u(1, 8).u(1, 8).u(1, 8).u(0, 1).u(0, 2).u(0, 1);
		s.u(0, 1).trailing();
		return builder().u8(0x81).u8(0x08).u8(0x0C).u8(0x00).u8(0x0A).u8(static_cast<unsigned>(s.b.size())).raw(s.b).b;
	}

	// An AudioSpecificConfig: AAC LC at 48 kHz, one channel.
	const bytes aac_config = {0x11, 0x88};

	bytes avc_sequence_header()
	{
		return flv_tag(9, 0, builder().u8(0x17).u8(0).u8(0).u8(0).u8(0).raw(avc_config()).b);
	}

	bytes avc_frame(const std::uint32_t timestamp)
	{
		return flv_tag(9, timestamp, builder().u8(0x27).u8(1).u8(0).u8(0).u8(0).be32(4).raw(bytes(4, 0x41)).b);
	}

	bytes aac_sequence_header()
	{
		return flv_tag(8, 0, builder().u8(0xAF).u8(0).raw(aac_config).b);
	}

	bytes aac_frame(const std::uint32_t timestamp)
	{
		return flv_tag(8, timestamp, builder().u8(0xAF).u8(1).raw(bytes(20, 0x21)).b);
	}

	// An enhanced RTMP video tag: a key frame of the packet type given, its FourCC, and its body.
	bytes enhanced_video(const unsigned packet_type, const std::string_view fourcc, const bytes& body, const std::uint32_t timestamp = 0)
	{
		return flv_tag(9, timestamp, builder().u8(0x80 | 0x10 | packet_type).raw(fourcc).raw(body).b);
	}

	// A Sorenson Spark picture header, its size a standard one or stated in 8 or 16 bits a side.
	bytes sorenson_frame(const unsigned size_code, const unsigned width = 0, const unsigned height = 0)
	{
		bit_writer s;
		s.u(1, 17).u(0, 5).u(0, 8).u(size_code, 3);
		if (size_code == 0) s.u(width, 8).u(height, 8);
		if (size_code == 1) s.u(width, 16).u(height, 16);
		s.u(0, 16);
		return builder().u8(0x12).raw(s.b).b;
	}

	// A VP6 key frame's first bytes: 40 x 23 macroblocks.
	const bytes vp6_frame = {0x28, 0x36, 23, 40, 23, 40, 0, 0};

	struct flv_parts
	{
		bool metadata = true;
		bool duration = true;
		bool xmp = false;
		std::uint32_t frames = 10;
	};

	const std::string flv_packet = "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF/></x:xmpmeta>";

	// AVC and AAC with onMetaData, as encoders write them: the sequence headers at time zero, then
	// frames 40 ms apart.
	bytes avc_aac_file(const flv_parts& p = {})
	{
		builder b;
		b.raw(flv_header(5));

		if (p.metadata)
		{
			amf_properties meta;
			if (p.duration) meta.emplace_back("duration", amf_number(12.5));
			meta.emplace_back("width", amf_number(1920));
			meta.emplace_back("height", amf_number(1088));
			meta.emplace_back("framerate", amf_number(29.97));
			meta.emplace_back("videocodecid", amf_number(7));
			meta.emplace_back("videodatarate", amf_number(2500));
			meta.emplace_back("audiocodecid", amf_number(10));
			meta.emplace_back("audiodatarate", amf_number(128));
			meta.emplace_back("audiosamplerate", amf_number(44100));
			meta.emplace_back("stereo", amf_boolean(true));
			meta.emplace_back("encoder", amf_string("Lavf62.12.102"));
			meta.emplace_back("filesize", amf_number(123456));
			b.raw(script_tag("onMetaData", amf_ecma_array(meta)));
		}

		if (p.xmp) b.raw(script_tag("onXMPData", amf_ecma_array({{"liveXML", amf_string(flv_packet)}})));

		b.raw(avc_sequence_header()).raw(aac_sequence_header());

		for (std::uint32_t i = 0; i < p.frames; ++i) b.raw(avc_frame(40 * i)).raw(aac_frame(40 * i + 20));
		return b.b;
	}

	std::vector<std::size_t> flv_boundaries(const bytes& file)
	{
		std::vector<std::size_t> out = {9, 13};
		std::size_t pos = 13;

		while (file.size() - pos >= 15)
		{
			const auto size = static_cast<std::size_t>(file[pos + 1] << 16 | file[pos + 2] << 8 | file[pos + 3]);
			if (file.size() - pos < 15 + size) break;
			pos += 15 + size;
			out.push_back(pos - 4);
			out.push_back(pos);
		}

		return out;
	}

	const libfiles::media_stream* stream_of(const media_scan_result& s, const media_stream_kind kind)
	{
		const auto it = std::ranges::find(s.streams, kind, &libfiles::media_stream::kind);
		return it != s.streams.end() ? &*it : nullptr;
	}
}

TEST("media flv reads AVC and AAC with onMetaData")
{
	const auto file = avc_aac_file();
	const auto s = scan(file);
	CHECK(s.success);
	CHECK(s.format == libfiles::media_format::flv);
	CHECK_STR(s.brand, "FLV");
	CHECK(near(s.duration, 12.5));
	CHECK_EQ(s.streams.size(), 2u);

	// Both have FFmpeg's id: the track, which a file without enhanced RTMP's tracks has as 0.
	const auto* const v = stream_of(s, media_stream_kind::video);
	CHECK(v != nullptr);
	if (v)
	{
		CHECK_EQ(v->id, 0u);
		CHECK_STR(v->codec, "h264");
		CHECK_STR(v->codec_tag, "7");
		// onMetaData's frame rate is the container's; its rate is in kilobits of 1024, as FFmpeg reads it.
		CHECK(near(v->video.frame_rate, 29.97));
		CHECK_EQ(v->bit_rate, 2500u * 1024);
		// The sequence parameter set's size, cropped, before onMetaData's.
		CHECK_EQ(v->video.width, 1920u);
		CHECK_EQ(v->video.height, 1080u);
		CHECK_STR(v->video.profile, "High");
		CHECK_STR(v->video.pixel_format(), "yuv420");
	}

	const auto* const a = stream_of(s, media_stream_kind::audio);
	CHECK(a != nullptr);
	if (a)
	{
		CHECK_EQ(a->id, 0u);
		CHECK_STR(a->codec, "aac");
		CHECK_STR(a->codec_tag, "10");
		CHECK_EQ(a->bit_rate, 128u * 1024);
		// The AudioSpecificConfig's, not the flags' 44.1 kHz stereo that AAC always states.
		CHECK_EQ(a->audio.sample_rate, 48000u);
		CHECK_EQ(a->audio.channels, 1u);
		CHECK_STR(a->audio.profile, "LC");
	}

	CHECK(s.complete);

	check_tags(s.tags, {
		{"duration", "12.5"},
		{"width", "1920"},
		{"height", "1088"},
		{"framerate", "29.97"},
		{"videocodecid", "7"},
		{"videodatarate", "2500"},
		{"audiocodecid", "10"},
		{"audiodatarate", "128"},
		{"audiosamplerate", "44100"},
		{"stereo", "true"},
		{"encoder", "Lavf62.12.102"},
		{"filesize", "123456"},
	});

	CHECK_STR(s.properties.encoder, "Lavf62.12.102");
	CHECK(s.xmp.empty());
}

TEST("media flv reads onMetaData values of every AMF type")
{
	builder b;
	b.raw(flv_header(1));
	b.raw(script_tag("onMetaData", amf_ecma_array({
		{"integer", amf_number(42)},
		{"fraction", amf_number(-0.25)},
		{"large", amf_number(1e21)},
		{"flag", amf_boolean(false)},
		{"title", amf_string(" A title ")},
		{"comment", amf_long_string("A long string")},
		{"latin", amf_string("caf\xE9")},
		{"creationdate", amf_string("Mon Mar 14 10:00:00 2011")},
		{"lastmodified", amf_date(1300029229000.0 + 500, 60)},
		{"nothing", builder().u8(5).b},
		{"undefined", builder().u8(6).b},
		{"keyframes", amf_object({{"times", amf_strict_array({amf_number(0), amf_number(2)})},
		                          {"filepositions", amf_strict_array({amf_number(13), amf_number(9000)})}})},
		{"trackinfo", amf_strict_array({amf_object({{"language", amf_string("eng")}})})},
		{"tags", amf_strict_array({amf_string("one"), amf_string("two")})},
		{"after", amf_string("still read")},
	})));
	b.raw(flv_tag(9, 0, sorenson_frame(6)));
	b.raw(flv_tag(9, 40, sorenson_frame(6)));
	const auto file = b.b;

	const auto s = scan(file);
	CHECK(s.success);

	// Only onMetaData's own values are tags: what nests in its objects and arrays describes the media.
	check_tags(s.tags, {
		{"integer", "42"},
		{"fraction", "-0.25"},
		{"large", "1e+21"},
		{"flag", "false"},
		{"title", "A title"},
		{"comment", "A long string"},
		{"latin", "caf\xC3\xA9"},
		{"creationdate", "Mon Mar 14 10:00:00 2011"},
		{"lastmodified", "2011-03-13T15:13:49Z"},
		{"after", "still read"},
	});

	CHECK_STR(s.properties.title, "A title");
}

TEST("media flv reads MP3 audio, and the duration from the last tag when onMetaData has none")
{
	// MPEG-1 Layer III at 48 kHz, which FLV's flags cannot say: they say 44 kHz.
	const bytes frame = {0xFF, 0xFB, 0x94, 0x64};

	builder b;
	b.raw(flv_header(4));
	b.raw(script_tag("onMetaData", amf_ecma_array({{"encoder", amf_string("x")}})));
	for (std::uint32_t i = 0; i < 40; ++i) b.raw(flv_tag(8, i * 24 + (i == 39 ? 1 : 0), builder().u8(0x2F).raw(frame).raw(bytes(100, 0)).b));
	// onMetaData written again at the end, at time zero, is stepped back over.
	b.raw(script_tag("onMetaData", amf_ecma_array({{"duration", amf_number(0)}})));
	const auto file = b.b;

	const auto s = scan(file);
	CHECK(s.success);
	CHECK(near(s.duration, 0.937));
	CHECK_EQ(s.streams.size(), 1u);

	if (!s.streams.empty())
	{
		const auto& a = s.streams[0];
		CHECK(a.kind == media_stream_kind::audio);
		CHECK_STR(a.codec, "mp3");
		CHECK_STR(a.codec_tag, "2");
		CHECK_EQ(a.audio.channels, 2u);
		CHECK_EQ(a.audio.sample_rate, 48000u);
		CHECK_EQ(a.audio.bits_per_sample, 0u);
	}

	CHECK(s.complete);

	// A timestamp past 24 bits takes its extension byte.
	builder late;
	late.raw(flv_header(4)).raw(flv_tag(8, 0, builder().u8(0x2F).raw(frame).b)).raw(flv_tag(8, 0x01000010, builder().u8(0x2F).raw(frame).b));
	CHECK(near(scan(late.b).duration, 16777.232));

	// No duration anywhere: a last tag at time zero, or a size field at the end that does not match.
	builder timeless;
	timeless.raw(flv_header(4)).raw(flv_tag(8, 0, builder().u8(0x2F).raw(frame).b));
	CHECK_EQ(scan(timeless.b).duration, 0.0);

	auto broken = late.b;
	broken.back() ^= 1;
	CHECK_EQ(scan(broken).duration, 0.0);
}

TEST("media flv reads PCM and Flash's other audio formats from their flags")
{
	struct expected
	{
		std::uint8_t flags;
		std::string_view codec;
		std::uint32_t rate;
		std::uint32_t channels;
		std::uint8_t bits;
	};

	const expected cases[] = {
		{0x36, "pcm", 11025, 1, 16},     // little-endian, 11 kHz, 16 bits, mono
		{0x01, "pcm", 5512, 2, 8},       // in the writer's byte order, 5.5 kHz, 8 bits, stereo
		{0x1E, "adpcm_swf", 44100, 1, 0},
		{0x5E, "nellymoser", 8000, 1, 0},
		{0x4F, "nellymoser", 16000, 1, 0},
		{0x6B, "nellymoser", 22050, 2, 0},
		{0x7E, "alaw", 8000, 1, 0},
		{0x8F, "mulaw", 8000, 2, 0},
		{0xB2, "speex", 16000, 1, 0},
		{0xF2, "", 5512, 1, 0},
	};

	for (const auto& c : cases)
	{
		const auto frame = builder().u8(c.flags).raw(bytes(32, 0)).b;
		builder b;
		b.raw(flv_header(4)).raw(flv_tag(8, 0, frame)).raw(flv_tag(8, 1000, frame));
		const auto s = scan(b.b);
		CHECK(s.success);
		CHECK_EQ(s.streams.size(), 1u);
		if (s.streams.empty()) continue;

		const auto& a = s.streams[0].audio;
		CHECK_STR(s.streams[0].codec, c.codec);
		CHECK_STR(s.streams[0].codec_tag, std::to_string(c.flags >> 4));
		CHECK_EQ(a.sample_rate, c.rate);
		CHECK_EQ(a.channels, c.channels);
		CHECK_EQ(a.bits_per_sample, c.bits);
		CHECK(near(s.duration, 1.0));
		CHECK_EQ(s.complete, !c.codec.empty());
	}
}

TEST("media flv reads enhanced RTMP's HEVC, AV1 and VP9")
{
	const auto with = [](const bytes& sequence_start)
	{
		builder b;
		b.raw(flv_header(1));
		b.raw(script_tag("onMetaData", amf_ecma_array({
			{"duration", amf_number(2)},
			{"width", amf_number(640)},
			{"height", amf_number(360)},
		})));
		b.raw(sequence_start);
		b.raw(enhanced_video(1, "hvc1", bytes(8, 0), 40));
		return scan(b.b);
	};

	const auto hevc = with(enhanced_video(0, "hvc1", hevc_config()));
	CHECK(hevc.success && hevc.streams.size() == 1);
	if (!hevc.streams.empty())
	{
		const auto& v = hevc.streams[0];
		CHECK_STR(v.codec, "hevc");
		CHECK_STR(v.codec_tag, "hvc1");
		CHECK_EQ(v.id, 0u);
		CHECK_EQ(v.video.width, 1280u);
		CHECK_EQ(v.video.height, 720u);
		CHECK_STR(v.video.pixel_format(), "yuv420 10bit");
		CHECK_STR(v.video.profile, "Main 10");
	}

	const auto av1 = with(enhanced_video(0, "av01", av1_config()));
	CHECK(av1.success && av1.streams.size() == 1);
	if (!av1.streams.empty())
	{
		const auto& v = av1.streams[0];
		CHECK_STR(v.codec, "av1");
		CHECK_STR(v.codec_tag, "av01");
		CHECK_EQ(v.video.width, 1280u);
		CHECK_EQ(v.video.height, 720u);
		CHECK_EQ(v.video.color_primaries, 1u);
		CHECK_STR(v.video.pixel_format(), "yuv420");
	}

	// The vpcC box's payload, its version and flags first; the size is onMetaData's.
	const bytes vpcc = {1, 0, 0, 0, 2, 31, 0xA2, 9, 16, 9, 0, 0};
	const auto vp9 = with(enhanced_video(0, "vp09", vpcc));
	CHECK(vp9.success && vp9.streams.size() == 1);
	if (!vp9.streams.empty())
	{
		const auto& v = vp9.streams[0];
		CHECK_STR(v.codec, "vp9");
		CHECK_EQ(v.video.width, 640u);
		CHECK_EQ(v.video.height, 360u);
		CHECK_STR(v.video.pixel_format(), "yuv420 10bit");
		CHECK_STR(std::string(v.video.hdr_transfer()), "PQ");
	}

	// AVC through enhanced RTMP, in a multitrack tag naming track 2, with ModEx data before it.
	builder avc;
	avc.raw(flv_header(1));
	avc.raw(flv_tag(9, 0, builder().u8(0x80 | 0x10 | 7).u8(2).raw(bytes{0, 0, 0}).u8(0x06).u8(0x00).raw("avc1").u8(2).raw(avc_config()).b));
	const auto tracked = scan(avc.b);
	CHECK(tracked.success && tracked.streams.size() == 1);
	if (!tracked.streams.empty())
	{
		CHECK_STR(tracked.streams[0].codec, "h264");
		CHECK_EQ(tracked.streams[0].id, 2u);
		CHECK_EQ(tracked.streams[0].video.height, 1080u);
	}
}

TEST("media flv reads enhanced RTMP's audio")
{
	const bytes opus_head = builder().raw("OpusHead").u8(1).u8(2).le16(312).le32(44100).le16(0).u8(0).b;

	builder b;
	b.raw(flv_header(4));
	b.raw(flv_tag(8, 0, builder().u8(0x90).raw("Opus").raw(opus_head).b));
	b.raw(flv_tag(8, 20, builder().u8(0x91).raw("Opus").raw(bytes(10, 0)).b));
	const auto opus = scan(b.b);
	CHECK(opus.success && opus.streams.size() == 1);
	if (!opus.streams.empty())
	{
		CHECK_STR(opus.streams[0].codec, "opus");
		CHECK_STR(opus.streams[0].codec_tag, "Opus");
		CHECK_EQ(opus.streams[0].audio.sample_rate, 48000u);
		CHECK_EQ(opus.streams[0].audio.channels, 2u);
	}

	// AAC in a multitrack tag naming track 1.
	builder m;
	m.raw(flv_header(4));
	m.raw(flv_tag(8, 0, builder().u8(0x95).u8(0x00).raw("mp4a").u8(1).raw(aac_config).b));
	const auto aac = scan(m.b);
	CHECK(aac.success && aac.streams.size() == 1);
	if (!aac.streams.empty())
	{
		CHECK_STR(aac.streams[0].codec, "aac");
		CHECK_EQ(aac.streams[0].id, 1u);
		CHECK_EQ(aac.streams[0].audio.sample_rate, 48000u);
	}
}

TEST("media flv reads the picture size of Flash's own video codecs")
{
	struct expected
	{
		bytes data;
		std::string_view codec;
		std::uint32_t width;
		std::uint32_t height;
		bool alpha;
	};

	const expected cases[] = {
		{sorenson_frame(6), "flv1", 160, 120, false},
		{sorenson_frame(0, 200, 150), "flv1", 200, 150, false},
		{sorenson_frame(1, 1000, 600), "flv1", 1000, 600, false},
		{builder().u8(0x13).u8(0x33).u8(0x20).u8(0x32).u8(0x58).raw(bytes(8, 0)).b, "flashsv", 800, 600, false},
		{builder().u8(0x16).u8(0x33).u8(0x20).u8(0x32).u8(0x58).raw(bytes(8, 0)).b, "flashsv2", 800, 600, false},
		// The adjustment byte takes 8 rows off the bottom of 23 macroblocks.
		{builder().u8(0x14).u8(0x08).raw(vp6_frame).b, "vp6f", 640, 360, false},
		{builder().u8(0x15).u8(0x08).u8(0).u8(0).u8(16).raw(vp6_frame).b, "vp6a", 640, 360, true},
	};

	for (const auto& c : cases)
	{
		builder b;
		b.raw(flv_header(1)).raw(flv_tag(9, 0, c.data)).raw(flv_tag(9, 500, c.data));
		const auto s = scan(b.b);
		CHECK(s.success);
		CHECK_EQ(s.streams.size(), 1u);
		if (s.streams.empty()) continue;

		const auto& v = s.streams[0];
		CHECK_STR(v.codec, c.codec);
		CHECK_EQ(v.video.width, c.width);
		CHECK_EQ(v.video.height, c.height);
		CHECK_EQ(v.video.alpha, c.alpha);
		CHECK(near(s.duration, 0.5));
		CHECK(s.complete);
	}

	// A command frame holds no picture, and is not the stream's first tag.
	builder command;
	command.raw(flv_header(1)).raw(flv_tag(9, 0, builder().u8(0x52).u8(0).b)).raw(flv_tag(9, 40, sorenson_frame(5)));
	const auto s = scan(command.b);
	CHECK(s.streams.size() == 1 && s.streams[0].video.width == 320);
}

TEST("media flv trusts the tags it sees over the header's flags")
{
	// The header says audio and video; only video follows.
	builder video_only;
	video_only.raw(flv_header(5)).raw(flv_tag(9, 0, sorenson_frame(6))).raw(flv_tag(9, 40, sorenson_frame(6)));
	const auto v = scan(video_only.b);
	CHECK(v.success);
	CHECK(v.streams.size() == 1 && v.streams[0].kind == media_stream_kind::video);

	// The header says video only; audio comes first.
	builder both;
	both.raw(flv_header(1)).raw(flv_tag(8, 0, builder().u8(0x36).raw(bytes(8, 0)).b)).raw(flv_tag(9, 0, sorenson_frame(6)));
	const auto b = scan(both.b);
	CHECK(b.success);
	CHECK(b.streams.size() == 2 && b.streams[0].kind == media_stream_kind::audio && b.streams[1].kind == media_stream_kind::video);

	// No tags at all is a file with nothing to play.
	const auto empty = scan(builder().raw(flv_header(5)).zeros(4).b);
	CHECK(empty.success);
	CHECK(empty.streams.empty());
	CHECK(!empty.complete);
}

TEST("media flv takes its packet from onXMPData")
{
	const auto file = avc_aac_file({.xmp = true});
	const auto s = scan(file);
	CHECK(s.success);
	CHECK_STR(to_string(s.xmp), flv_packet);
	CHECK_STR(to_string(s.xmp), packet_of(file));
	// Its packet is not a tag.
	CHECK(tag_value(s.tags, "liveXML").empty());

	// One past time zero is not where the writer looks, and so not the packet.
	builder late;
	late.raw(flv_header(1)).raw(flv_tag(9, 0, sorenson_frame(6)));
	late.raw(script_tag("onXMPData", amf_ecma_array({{"liveXML", amf_string(flv_packet)}}), 40));
	const auto l = scan(late.b);
	CHECK(l.success);
	CHECK(l.xmp.empty());
	CHECK_STR(packet_of(late.b), "");
}

TEST("media flv reports its structure")
{
	const auto s = scan(avc_aac_file({.xmp = true}), libfiles::scan_intent::inspect);
	CHECK(s.success);

	std::vector<std::string> keys;
	for (const auto& row : s.structure) keys.push_back(row.key);
	const std::vector<std::string> expected = {
		"Header (3)", "Version", "Streams declared", "Header size", "Tags (4)", "onMetaData", "onXMPData", "Video", "Audio",
	};

	CHECK(keys == expected);

	const auto it = std::ranges::find(s.structure, std::string("Streams declared"), &libfiles::structure_row::key);
	CHECK(it != s.structure.end() && it->value == "audio, video");
	CHECK(scan(avc_aac_file()).structure.empty());
}

TEST("media flv survives files cut short")
{
	const auto file = avc_aac_file({.xmp = true});
	CHECK(scan(file).success);
	check_cuts(file, flv_boundaries(file));
}

TEST("media flv survives damaged sizes and counts")
{
	const auto file = avc_aac_file({.xmp = true, .frames = 2});
	check_damage(file, 0, file.size());

	// Without onMetaData's duration the scan reads the end of the file too.
	const auto timeless = avc_aac_file({.duration = false, .frames = 2});
	check_damage(timeless, timeless.size() - 40, timeless.size());
}

TEST("media flv reads a large file in a few blocks")
{
	// Both streams are known from the first tags: the rest is not read.
	const auto file = avc_aac_file({.frames = 20000});
	libfiles::memory_stream stream(file);
	libfiles::reader r(stream);
	const auto s = libfiles::scan_media(r);
	CHECK(s.success && s.streams.size() == 2);
	CHECK_EQ(r.source_reads(), 1u);

	// A file with one stream is read as far as the bound, and then at its end for the duration.
	builder video_only;
	video_only.raw(flv_header(1));
	for (std::uint32_t i = 0; i < 20000; ++i) video_only.raw(flv_tag(9, i * 40, sorenson_frame(6)));
	libfiles::memory_stream one(video_only.b);
	libfiles::reader rv(one);
	const auto v = libfiles::scan_media(rv);
	CHECK(v.success && v.streams.size() == 1);
	CHECK(near(v.duration, 19999 * 0.04));
	CHECK(rv.source_reads() <= 6u);
	CHECK(rv.source_bytes() < video_only.b.size() / 2);
}

TEST("media flv reads the packet its writer wrote")
{
	const auto original = avc_aac_file();
	libfiles::xmp::meta meta;
	meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", 2);

	libfiles::reader r(original);
	const auto plan = libfiles::plan_update(r, {&meta});
	CHECK(plan.ok());
	libfiles::memory_stream written;
	libfiles::write_updated(r, plan, written);

	const auto s = scan(written.bytes());
	CHECK(s.success);
	CHECK(!s.xmp.empty());
	CHECK_STR(to_string(s.xmp), packet_of(written.bytes()));
	CHECK_EQ(s.streams.size(), 2u);
	CHECK(near(s.duration, 12.5));
	CHECK_EQ(s.tags.size(), scan(original).tags.size());
}

TEST("asf reads back the edits it keeps outside the packet")
{
	// The Content Description keeps the title, the artist as its author, the copyright and the comment as its
	// description, where Windows, FFmpeg and the taggers that write WMA read them; Windows' descriptors the
	// keywords and rating; the File Properties the creation date. The description stays in the packet.
	check_round_trip("asf", wma_file({}), {"artist", "comment", "copyright", "keywords", "rating", "recorded", "title"});
}

TEST("asf writes the creators as its author when an edit names no artist, and only a comment as its description")
{
	const auto original = wma_file({});

	libfiles::xmp::meta meta;
	meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", 2);
	libfiles::metadata_update u{&meta};
	u.legacy.creators = std::vector<std::string>{"First", "", "Second"};
	u.legacy.description = "A new description";

	libfiles::reader r(original);
	const auto plan = libfiles::plan_update(r, u);
	CHECK(plan.ok());
	libfiles::memory_stream written;
	libfiles::write_updated(r, plan, written);

	const auto s = scan(written.bytes());
	CHECK(s.success);
	CHECK_STR(tag_value(s.tags, "Author"), "First; Second");
	CHECK_STR(s.properties.artist, "First; Second");
	// What the file held as its comment is not the description's to replace.
	CHECK_STR(tag_value(s.tags, "Description"), "A description");
	CHECK_STR(tag_value(s.tags, "Title"), "Song");
}
