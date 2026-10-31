// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the MPEG audio, FLAC and Ogg media scans, over files built frame by frame, block by block
// and page by page: what each scan reads of its streams and tags, the durations each container's rules give,
// what a scan costs in reads, and files cut short and damaged.

#include "test.h"
#include "builders.h"

#include "libfiles/media.h"
#include "libfiles/write.h"
#include "media_internal.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace test_build;
	using libfiles::media_format;
	using libfiles::media_scan_result;
	using libfiles::media_stream_kind;

	media_scan_result scan(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index)
	{
		libfiles::reader r(file);
		return libfiles::scan_media(r, intent);
	}

	// A scan over a stream, read a block at a time, with the reads it cost.
	media_scan_result scan_stream(const bytes& file, std::uint64_t& reads)
	{
		libfiles::memory_stream s(file);
		libfiles::reader r(s);
		auto result = libfiles::scan_media(r);
		reads = r.source_reads();
		return result;
	}

	bool close_to(const double a, const double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	bool has_tag(const media_scan_result& s, const std::string_view value)
	{
		return std::ranges::any_of(s.tags, [value](const libfiles::media_tag& t) { return t.value == value; });
	}

	void le64(builder& b, const std::uint64_t v)
	{
		b.le32(static_cast<std::uint32_t>(v)).le32(static_cast<std::uint32_t>(v >> 32));
	}

	void be24(builder& b, const std::uint32_t v)
	{
		b.u8(v >> 16).u8(v >> 8).u8(v);
	}

	bytes filler(const std::size_t size, const std::uint8_t value = 0x55)
	{
		return bytes(size, value);
	}

	// What a result holds to, whole or damaged: an error and nothing else, or streams whose durations are
	// numbers no longer than the file's.
	void check_consistent(const media_scan_result& s)
	{
		if (!s.success)
		{
			CHECK(!s.error.empty());
			CHECK(s.streams.empty());
			return;
		}

		CHECK(s.error.empty());
		CHECK(std::isfinite(s.duration) && s.duration >= 0);

		for (const auto& st : s.streams) CHECK(std::isfinite(st.duration) && st.duration >= 0 && st.duration <= s.duration + 1e-9);
		for (const auto& p : s.pictures) CHECK(p.location.size == 0 || p.data.empty());
	}

	// The file cut short at every length near its ends and near each boundary given.
	void check_cut_short(const bytes& file, const std::vector<std::size_t>& boundaries)
	{
		std::vector<std::size_t> lengths;
		for (std::size_t n = 0; n < 64 && n < file.size(); ++n) lengths.push_back(n);
		for (auto n = file.size() > 64 ? file.size() - 64 : 0; n <= file.size(); ++n) lengths.push_back(n);

		for (const auto b : boundaries)
		{
			for (auto n = b > 16 ? b - 16 : 0; n <= b + 16 && n <= file.size(); ++n) lengths.push_back(n);
		}

		for (const auto n : lengths) check_consistent(scan(bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n))));
	}

	// Each byte in [from, to) set in turn to values that break sizes, flags and signatures.
	void check_corrupted(const bytes& file, const std::size_t from, const std::size_t to)
	{
		static constexpr std::uint8_t values[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};

		for (auto i = from; i < to && i < file.size(); ++i)
		{
			for (const auto v : values)
			{
				auto damaged = file;
				damaged[i] = v;
				check_consistent(scan(damaged));
			}
		}
	}

	std::uint32_t crc8(const std::span<const std::uint8_t> data)
	{
		std::uint32_t crc = 0;

		for (const auto b : data)
		{
			crc ^= b;
			for (auto i = 0; i < 8; ++i) crc = (crc & 0x80 ? crc << 1 ^ 0x07 : crc << 1) & 0xFF;
		}

		return crc;
	}

	// ---- MPEG audio --------------------------------------------------------------------------------

	// A frame header without a CRC. version: 3 for MPEG-1, 2 for MPEG-2, 0 for MPEG-2.5; mode 3 is mono.
	std::uint32_t mpa_header(const unsigned version, const unsigned layer, const unsigned bit_rate_index, const unsigned rate_index,
	                         const unsigned mode = 1)
	{
		return 0xFFE00000u | version << 19 | (4 - layer) << 17 | 1u << 16 | bit_rate_index << 12 | rate_index << 10 | mode << 6;
	}

	// MPEG-1 Layer III at 128 kbit/s and 48 kHz, joint stereo: 384 bytes and 1152 samples a frame.
	const auto mp3_header = mpa_header(3, 3, 9, 1);
	constexpr std::size_t mp3_frame = 384;
	// MPEG-2 Layer III at 64 kbit/s and 24 kHz, mono: 192 bytes and 576 samples a frame.
	const auto lsf_header = mpa_header(2, 3, 8, 1, 3);
	constexpr std::size_t lsf_frame = 192;
	// MPEG-1 Layer II at 192 kbit/s and 48 kHz: 576 bytes and 1152 samples a frame.
	const auto mp2_header = mpa_header(3, 2, 10, 1);
	constexpr std::size_t mp2_frame = 576;

	bytes frames(const std::uint32_t header, const std::size_t size, const std::size_t count)
	{
		builder b;
		for (std::size_t i = 0; i < count; ++i) b.be32(header).zeros(size - 4);
		return b.b;
	}

	// The first frame of a stream its encoder counted: an Xing or Info header with every field after the
	// side information, and a LAME tag with the encoder delay and padding.
	bytes xing_frame(const std::uint32_t header, const std::size_t size, const std::size_t side_info, const std::string_view kind,
	                 const std::uint32_t count, const std::uint32_t length, const std::string_view encoder = "LAME3.100",
	                 const unsigned delay = 576, const unsigned padding = 1000)
	{
		builder b;
		b.be32(header).zeros(side_info).raw(kind).be32(0x0F).be32(count).be32(length);
		for (unsigned i = 0; i < 100; ++i) b.u8(i * 255 / 100);
		b.be32(70).raw(encoder).zeros(9 - encoder.size());
		b.u8(0x24).u8(0x6E).be32(0).be16(0).be16(0).u8(0).u8(0);
		b.u8(delay >> 4).u8((delay & 15) << 4 | padding >> 8).u8(padding);
		b.zeros(size - b.size());
		return b.b;
	}

	bytes vbri_frame(const std::uint32_t header, const std::size_t size, const std::uint32_t count, const std::uint32_t length)
	{
		builder b;
		b.be32(header).zeros(32).raw("VBRI").be16(1).be16(0x0240).be16(75).be32(length).be32(count);
		b.zeros(size - b.size());
		return b.b;
	}

	bytes id3v1_tag(const std::string_view title, const std::string_view artist)
	{
		builder b;
		b.raw("TAG").raw(title).zeros(30 - title.size()).raw(artist).zeros(30 - artist.size()).zeros(30).raw("1999").zeros(30).u8(17);
		return b.b;
	}

	bytes ape_tag(const std::vector<std::pair<std::string_view, std::string_view>>& items)
	{
		builder body;
		for (const auto& [key, value] : items) body.le32(static_cast<std::uint32_t>(value.size())).le32(0).raw(key).u8(0).raw(value);

		const auto size = static_cast<std::uint32_t>(body.size() + 32);
		const auto count = static_cast<std::uint32_t>(items.size());
		builder b;
		b.raw("APETAGEX").le32(2000).le32(size).le32(count).le32(0xA0000000).zeros(8);
		b.raw(body.b);
		b.raw("APETAGEX").le32(2000).le32(size).le32(count).le32(0x80000000).zeros(8);
		return b.b;
	}

	bytes lyrics3_tag(const std::string_view lyrics)
	{
		builder b;
		b.raw("LYRICSBEGIN").raw("LYR").raw(std::format("{:05}", lyrics.size())).raw(lyrics);
		const auto size = b.size();
		b.raw(std::format("{:06}", size)).raw("LYRICS200");
		return b.b;
	}

	void syncsafe(builder& b, const std::size_t v)
	{
		b.u8(v >> 21 & 0x7F).u8(v >> 14 & 0x7F).u8(v >> 7 & 0x7F).u8(v & 0x7F);
	}

	bytes id3v2_frame(const unsigned version, const std::string_view id, const bytes& body)
	{
		builder b;
		b.raw(id);
		if (version == 2) be24(b, static_cast<std::uint32_t>(body.size()));
		else if (version == 4) syncsafe(b, body.size());
		else b.be32(static_cast<std::uint32_t>(body.size()));
		if (version != 2) b.be16(0);
		b.raw(body);
		return b.b;
	}

	bytes text_frame(const unsigned version, const std::string_view id, const std::string_view text)
	{
		return id3v2_frame(version, id, builder().u8(0).raw(text).b);
	}

	// Text in an ID3 frame's encoding: 0 Latin-1, 1 UTF-16 with its byte order mark, 3 UTF-8; and the
	// terminator that ends a description in it.
	bytes id3_text(const unsigned encoding, const std::string_view ascii)
	{
		if (encoding != 1) return to_bytes(ascii);

		builder b;
		b.u8(0xFF).u8(0xFE);
		for (const auto c : ascii) b.u8(static_cast<unsigned char>(c)).u8(0);
		return b.b;
	}

	bytes id3_terminator(const unsigned encoding)
	{
		return encoding == 1 ? bytes{0, 0} : bytes{0};
	}

	// A comment: COMM, or COM in ID3v2.2 - its encoding, language and description, then its text.
	bytes comment_frame(const unsigned version, const std::string_view language, const std::string_view description,
	                    const std::string_view text, const unsigned encoding = 0)
	{
		builder b;
		b.u8(encoding).raw(language).raw(id3_text(encoding, description)).raw(id3_terminator(encoding)).raw(id3_text(encoding, text));
		return id3v2_frame(version, version == 2 ? "COM" : "COMM", b.b);
	}

	// A user-defined text frame: TXXX, or TXX in ID3v2.2.
	bytes user_text_frame(const unsigned version, const std::string_view description, const std::string_view text)
	{
		return id3v2_frame(version, version == 2 ? "TXX" : "TXXX", builder().u8(0).raw(description).u8(0).raw(text).b);
	}

	bytes xmp_frame(const unsigned version, const std::string_view packet)
	{
		return id3v2_frame(version, "PRIV", builder().raw("XMP").u8(0).raw(packet).b);
	}

	bytes id3v2_tag(const unsigned version, const std::vector<bytes>& frame_list, const std::size_t padding = 0, const bool footer = false)
	{
		builder body;
		for (const auto& f : frame_list) body.raw(f);
		body.zeros(padding);

		builder b;
		b.raw("ID3").u8(version).u8(0).u8(footer ? 0x10 : 0);
		syncsafe(b, body.size());
		b.raw(body.b);

		if (footer)
		{
			b.raw("3DI").u8(version).u8(0).u8(0x10);
			syncsafe(b, body.size());
		}

		return b.b;
	}

	const libfiles::media_stream& only_stream(const media_scan_result& s)
	{
		static const libfiles::media_stream none;
		CHECK_EQ(s.streams.size(), 1u);
		return s.streams.empty() ? none : s.streams.front();
	}

	// ---- FLAC --------------------------------------------------------------------------------------

	bytes streaminfo(const std::uint32_t rate, const unsigned channels, const unsigned bits, const std::uint64_t total,
	                 const unsigned min_block = 4096, const unsigned max_block = 4096)
	{
		builder b;
		b.be16(min_block).be16(max_block);
		be24(b, 0);
		be24(b, 0);
		const auto packed = std::uint64_t{rate} << 44 | std::uint64_t{channels - 1} << 41 | std::uint64_t{bits - 1} << 36 | total;
		b.be32(static_cast<std::uint32_t>(packed >> 32)).be32(static_cast<std::uint32_t>(packed)).zeros(16);
		return b.b;
	}

	bytes flac_block(const unsigned type, const bytes& data, const bool last = false)
	{
		builder b;
		b.u8((last ? 0x80 : 0) | type);
		be24(b, static_cast<std::uint32_t>(data.size()));
		b.raw(data);
		return b.b;
	}

	bytes vorbis_comment(const std::vector<std::string_view>& fields, const std::string_view vendor = "libfiles test")
	{
		builder b;
		b.le32(static_cast<std::uint32_t>(vendor.size())).raw(vendor).le32(static_cast<std::uint32_t>(fields.size()));
		for (const auto f : fields) b.le32(static_cast<std::uint32_t>(f.size())).raw(f);
		return b.b;
	}

	// A PICTURE block's body: a front cover, its fields, then the image - whose size it may misstate.
	bytes picture(const std::string_view mime, const std::string_view description, const bytes& image, const std::size_t claimed = ~std::size_t{0})
	{
		const auto size = claimed == ~std::size_t{0} ? image.size() : claimed;
		builder b;
		b.be32(3).be32(static_cast<std::uint32_t>(mime.size())).raw(mime).be32(static_cast<std::uint32_t>(description.size())).raw(description);
		b.be32(64).be32(64).be32(24).be32(0).be32(static_cast<std::uint32_t>(size)).raw(image);
		return b.b;
	}

	// The coded number of a frame header: UTF-8's form, extended to 36 bits in seven bytes.
	void coded_number(builder& b, const std::uint64_t v)
	{
		if (v < 0x80)
		{
			b.u8(static_cast<unsigned>(v));
			return;
		}

		const unsigned extra = v < 0x800 ? 1 : v < 0x10000 ? 2 : v < 0x200000 ? 3 : v < 0x4000000 ? 4 : v < 0x80000000 ? 5 : 6;
		b.u8((0xFF00u >> (extra + 1) & 0xFF) | static_cast<unsigned>(v >> 6 * extra));
		for (auto i = static_cast<int>(extra) - 1; i >= 0; --i) b.u8(0x80 | static_cast<unsigned>(v >> 6 * i & 0x3F));
	}

	// A FLAC frame: its header, with a block size coded in 16 bits, the rate and depth from STREAMINFO and
	// two independent channels, then audio that never looks like a header.
	bytes flac_frame(const bool variable, const std::uint64_t number, const unsigned block_size, const std::size_t audio = 200,
	                 const unsigned rate_code = 0)
	{
		builder b;
		b.u8(0xFF).u8(variable ? 0xF9 : 0xF8).u8(0x70 | rate_code).u8(0x10);
		coded_number(b, number);
		b.be16(block_size - 1);
		b.u8(crc8(b.b));
		b.raw(filler(audio, 0x11));
		return b.b;
	}

	// ---- Ogg ---------------------------------------------------------------------------------------

	std::uint32_t ogg_crc(const std::span<const std::uint8_t> page)
	{
		std::uint32_t crc = 0;

		for (const auto byte : page)
		{
			crc ^= static_cast<std::uint32_t>(byte) << 24;
			for (auto i = 0; i < 8; ++i) crc = crc & 0x80000000u ? crc << 1 ^ 0x04C11DB7u : crc << 1;
		}

		return crc;
	}

	constexpr std::uint64_t no_granule = ~std::uint64_t{0};

	bytes ogg_page(const unsigned flags, const std::uint64_t granule, const std::uint32_t serial, const std::uint32_t sequence,
	               const bytes& lacing, const bytes& body)
	{
		builder b;
		b.raw("OggS").u8(0).u8(flags);
		le64(b, granule);
		b.le32(serial).le32(sequence).le32(0).u8(static_cast<unsigned>(lacing.size())).raw(lacing).raw(body);

		const auto crc = ogg_crc(b.b);
		for (auto i = 0; i < 4; ++i) b.b[22 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> 8 * i);
		return b.b;
	}

	// Lays one stream's packets out on pages as a muxer does: at most max_segments segments a page, a packet
	// that does not fit continuing on the next, each page's granule position that of the last packet that
	// ends on it. Each call begins a new page; the stream's first page begins it, and `last` ends it.
	struct ogg_writer
	{
		std::uint32_t serial = 0;
		std::uint32_t sequence = 0;
		bool begun = false;

		bytes pages(const std::vector<std::pair<bytes, std::uint64_t>>& packets, const bool last = false, const std::size_t max_segments = 255)
		{
			bytes out;
			bytes lacing;
			bytes body;
			auto continued = false;
			auto granule = no_granule;

			const auto flush = [&](const bool final_page)
			{
				const auto flags = (continued ? 1u : 0u) | (begun ? 0u : 2u) | (final_page && last ? 4u : 0u);
				const auto page = ogg_page(flags, granule, serial, sequence++, lacing, body);
				out.insert(out.end(), page.begin(), page.end());
				begun = true;
				lacing.clear();
				body.clear();
				granule = no_granule;
			};

			for (const auto& [data, packet_granule] : packets)
			{
				std::size_t at = 0;

				for (;;)
				{
					if (lacing.size() == max_segments)
					{
						flush(false);
						continued = at != 0;
					}

					const auto n = std::min<std::size_t>(255, data.size() - at);
					lacing.push_back(static_cast<std::uint8_t>(n));
					body.insert(body.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
					at += n;

					if (n < 255)
					{
						granule = packet_granule;
						break;
					}
				}
			}

			if (!lacing.empty()) flush(true);
			return out;
		}
	};

	bytes vorbis_id(const unsigned channels, const std::uint32_t rate, const std::uint32_t nominal = 128000)
	{
		return builder().raw("\x01vorbis").le32(0).u8(channels).le32(rate).le32(0).le32(nominal).le32(0).u8(0xB8).u8(1).b;
	}

	bytes vorbis_comment_packet(const std::vector<std::string_view>& fields)
	{
		return builder().raw("\x03vorbis").raw(vorbis_comment(fields)).u8(1).b;
	}

	bytes vorbis_setup()
	{
		return builder().raw("\x05vorbis").raw(filler(300, 0x42)).b;
	}

	bytes opus_head(const unsigned channels, const unsigned pre_skip)
	{
		return builder().raw("OpusHead").u8(1).u8(channels).le16(pre_skip).le32(44100).le16(0).u8(0).b;
	}

	bytes opus_tags(const std::vector<std::string_view>& fields)
	{
		return builder().raw("OpusTags").raw(vorbis_comment(fields)).b;
	}

	// A packet of one 20 ms CELT frame: 960 samples.
	bytes opus_packet()
	{
		return builder().u8(31 << 3).raw(filler(60)).b;
	}

	bytes theora_id(const unsigned width, const unsigned height, const std::uint32_t frn, const std::uint32_t frd, const unsigned shift)
	{
		builder b;
		b.raw("\x80theora").u8(3).u8(2).u8(1).be16((width + 15) / 16).be16((height + 15) / 16);
		be24(b, width);
		be24(b, height);
		b.u8(0).u8(0).be32(frn).be32(frd);
		be24(b, 1);
		be24(b, 1);
		b.u8(0);
		be24(b, 500000);
		// Quality 32, the keyframe shift, 4:2:0, and the reserved bits.
		const auto bits = 32u << 10 | shift << 5;
		b.u8(bits >> 8).u8(bits);
		return b.b;
	}

	bytes speex_header(const std::uint32_t rate, const unsigned channels)
	{
		builder b;
		b.raw("Speex   ").raw("1.2.1").zeros(15).le32(1).le32(80).le32(rate).le32(1).le32(4).le32(channels).le32(24600).le32(320).le32(0).le32(1).le32(0).zeros(8);
		return b.b;
	}

	// A Vorbis stream: its three headers, then pages of data whose granule positions run to `samples`.
	bytes vorbis_stream(ogg_writer& w, const std::uint32_t rate, const std::uint64_t samples, const std::vector<std::string_view>& fields,
	                    const std::size_t pages = 3)
	{
		auto out = w.pages({{vorbis_id(2, rate), 0}});
		const auto headers = w.pages({{vorbis_comment_packet(fields), 0}, {vorbis_setup(), 0}});
		out.insert(out.end(), headers.begin(), headers.end());

		for (std::size_t p = 1; p <= pages; ++p)
		{
			std::vector<std::pair<bytes, std::uint64_t>> packets;
			for (auto i = 0; i < 10; ++i) packets.emplace_back(filler(120, 0x02), samples * p / pages);
			const auto data = w.pages(packets, p == pages);
			out.insert(out.end(), data.begin(), data.end());
		}

		return out;
	}

	void append(bytes& to, const bytes& more)
	{
		to.insert(to.end(), more.begin(), more.end());
	}

	// Two seconds of FLAC in Ogg, 24 bits at 44.1 kHz, with its Vorbis comment.
	bytes ogg_flac_file()
	{
		ogg_writer w{5};
		auto file = w.pages({{builder().u8(0x7F).raw("FLAC").u8(1).u8(0).be16(1).raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 24, 0))).b, 0}});
		append(file, w.pages({{flac_block(4, vorbis_comment({"TITLE=Ogg FLAC"}), true), 0}}));
		for (std::uint64_t p = 1; p <= 2; ++p) append(file, w.pages({{flac_frame(false, p - 1, 4096, 500), p * 44100}}, p == 2));
		return file;
	}

	// Theora at 320 x 240 and 25 frames a second - 75 frames, a keyframe every 25, a page a second - beside
	// 3.5 seconds of Vorbis at 48 kHz.
	bytes theora_vorbis_file()
	{
		ogg_writer video{0xAAAA};
		ogg_writer audio{0xBBBB};

		auto file = video.pages({{theora_id(320, 240, 25, 1, 6), 0}});
		append(file, audio.pages({{vorbis_id(2, 48000), 0}}));
		append(file, video.pages({{builder().raw("\x81theora").raw(vorbis_comment({"TITLE=Film"})).b, 0}, {builder().raw("\x82theora").raw(filler(100)).b, 0}}));
		append(file, audio.pages({{vorbis_comment_packet({}), 0}, {vorbis_setup(), 0}}));

		for (unsigned second = 0; second < 3; ++second)
		{
			std::vector<std::pair<bytes, std::uint64_t>> packets;

			for (unsigned frame = 1; frame <= 25; ++frame)
			{
				const auto keyframe = second * 25 + 1;
				const auto number = second * 25 + frame;
				packets.emplace_back(builder().u8(frame == 1 ? 0x40 : 0x00).raw(filler(40)).b, std::uint64_t{keyframe} << 6 | (number - keyframe));
			}

			append(file, video.pages(packets, second == 2));
			append(file, audio.pages({{filler(200, 0x02), (second + 1) * 48000ull}}));
		}

		append(file, audio.pages({{filler(200, 0x02), 168000}}, true));
		return file;
	}
}

// ---- MPEG audio ----------------------------------------------------------------------------------------

TEST("media mp3 times a constant bit rate stream by its first frame's bit rate")
{
	builder f;
	f.raw(id3v2_tag(3, {text_frame(3, "TIT2", "Title")}, 100));
	// 3 seconds at 128 kbit/s.
	f.raw(frames(mp3_header, mp3_frame, 125)).raw(id3v1_tag("Old title", "Old artist"));

	const auto s = scan(f.b);
	CHECK(s.success);
	CHECK(s.format == media_format::mpeg_audio);
	CHECK_STR(s.brand, "MPEG-1 Layer 3");

	const auto& a = only_stream(s);
	CHECK(a.kind == media_stream_kind::audio);
	CHECK_EQ(a.id, 0u);
	CHECK_STR(a.codec, "mp3");
	CHECK(a.codec_tag.empty());
	CHECK_EQ(a.audio.sample_rate, 48000u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK_EQ(a.audio.bits_per_sample, 0u);
	CHECK_STR(a.audio.profile, "MPEG-1 Layer 3");
	CHECK_EQ(a.bit_rate, 128000u);
	CHECK(close_to(a.duration, 3.0));
	CHECK(close_to(s.duration, 3.0));
	CHECK(s.complete);

	// The ID3v2 tag's title stands, and ID3v1 gives what it lacks.
	CHECK_STR(s.properties.title, "Title");
	CHECK_STR(s.properties.artist, "Old artist");
}

TEST("media mp3 counts an Xing header's frames, less the LAME tag's delay and padding")
{
	const auto build = [](const std::string_view encoder)
	{
		builder f;
		f.raw(xing_frame(mp3_header, mp3_frame, 32, "Xing", 101, 38000, encoder)).raw(frames(mp3_header, mp3_frame, 100));
		return f.b;
	};

	const auto total = 101.0 * 1152;
	const auto lame = scan(build("LAME3.100"));
	const auto& a = only_stream(lame);
	CHECK(close_to(a.duration, (total - 576 - 1000) / 48000));
	CHECK(close_to(lame.duration, a.duration));
	// The header's bytes over its frames' samples.
	CHECK_EQ(a.bit_rate, (38000ull * 8 * 48000 + 116352 / 2) / 116352);
	CHECK(lame.complete);

	// FFmpeg takes the delay and padding from the encoders it knows to write them.
	CHECK(close_to(only_stream(scan(build("Lavf62.1."))).duration, (total - 576 - 1000) / 48000));
	CHECK(close_to(only_stream(scan(build("Lavc62.1."))).duration, (total - 576 - 1000) / 48000));
	CHECK(close_to(only_stream(scan(build("GOGO1.0  "))).duration, total / 48000));
}

TEST("media mp3 takes iTunes' gapless figures where no LAME tag gives a delay and padding")
{
	// 101 frames of 1152 samples: 2112 of delay, 464 of padding and 113776 that play.
	constexpr std::string_view figures = " 00000000 00000840 000001D0 000000000001BC70 00000000 00000000";
	constexpr std::string_view wrong = " 00000000 00000840 000001D0 000000000001BC71";
	const auto played = 113776.0 / 48000;
	const auto counted = 116352.0 / 48000;

	const auto duration = [](const bytes& tag, const std::string_view encoder = "iTunes v7")
	{
		builder f;
		f.raw(tag).raw(xing_frame(mp3_header, mp3_frame, 32, "Xing", 101, 101 * 384, encoder)).raw(frames(mp3_header, mp3_frame, 100));
		return only_stream(scan(f.b)).duration;
	};

	// In a comment, in each version of ID3v2, whatever its language and encoding.
	CHECK(close_to(duration(id3v2_tag(3, {comment_frame(3, "eng", "iTunSMPB", figures)})), played));
	CHECK(close_to(duration(id3v2_tag(3, {comment_frame(3, "fra", "iTunSMPB", figures, 1)})), played));
	CHECK(close_to(duration(id3v2_tag(4, {comment_frame(4, "und", "iTunSMPB", figures, 3)})), played));
	CHECK(close_to(duration(id3v2_tag(2, {comment_frame(2, "xxx", "iTunSMPB", figures)})), played));

	// In a user-defined text frame, as files remuxed by older FFmpeg keep them; the name in any case.
	CHECK(close_to(duration(id3v2_tag(3, {user_text_frame(3, "iTunSMPB", figures)})), played));
	CHECK(close_to(duration(id3v2_tag(4, {user_text_frame(4, "itunsmpb", figures)})), played));
	CHECK(close_to(duration(id3v2_tag(2, {comment_frame(2, "eng", "ITUNSMPB", figures)})), played));

	// A comment is looked for first, wherever each lies.
	CHECK(close_to(duration(id3v2_tag(3, {user_text_frame(3, "iTunSMPB", wrong), comment_frame(3, "eng", "iTunSMPB", figures)})), played));
	CHECK(close_to(duration(id3v2_tag(3, {user_text_frame(3, "iTunSMPB", figures), comment_frame(3, "eng", "iTunSMPB", wrong)})), counted));

	// Other names are not iTunes' figures.
	CHECK(close_to(duration(id3v2_tag(3, {comment_frame(3, "eng", "iTunNORM", figures), user_text_frame(3, "iTunSMPB2", figures),
	                                       text_frame(3, "TIT2", figures)})), counted));

	// A LAME tag's delay and padding come first.
	CHECK(close_to(duration(id3v2_tag(3, {comment_frame(3, "eng", "iTunSMPB", figures)}), "LAME3.100"), (116352.0 - 1576) / 48000));

	// Figures that do not add up to the frame count, or that run past sixteen digits, are not taken.
	CHECK(close_to(duration(id3v2_tag(3, {comment_frame(3, "eng", "iTunSMPB", wrong)})), counted));
	CHECK(close_to(duration(id3v2_tag(3, {comment_frame(3, "eng", "iTunSMPB", " 00000000 00000840 000001D0 00000000000001BC700")})), counted));
}

TEST("media mp3 counts an Info header's frames at the frames' own bit rate")
{
	builder f;
	f.raw(xing_frame(mp3_header, mp3_frame, 32, "Info", 101, 101 * 384)).raw(frames(mp3_header, mp3_frame, 100));

	const auto s = scan(f.b);
	const auto& a = only_stream(s);
	CHECK(close_to(a.duration, (101.0 * 1152 - 1576) / 48000));
	CHECK_EQ(a.bit_rate, 128000u);
}

TEST("media mp3 counts a VBRI header's frames")
{
	builder f;
	f.raw(vbri_frame(mp3_header, mp3_frame, 101, 101 * 384)).raw(frames(mp3_header, mp3_frame, 100));

	const auto s = scan(f.b);
	const auto& a = only_stream(s);
	CHECK(close_to(a.duration, 101.0 * 1152 / 48000));
	CHECK_EQ(a.bit_rate, 128000u);
}

TEST("media mp3 finds the Xing header of an MPEG-2 mono stream after its shorter side information")
{
	builder f;
	f.raw(xing_frame(lsf_header, lsf_frame, 9, "Xing", 51, 51 * 192, "LAME3.100", 576, 0)).raw(frames(lsf_header, lsf_frame, 50));

	const auto s = scan(f.b);
	CHECK_STR(s.brand, "MPEG-2 Layer 3");
	const auto& a = only_stream(s);
	CHECK_EQ(a.audio.sample_rate, 24000u);
	CHECK_EQ(a.audio.channels, 1u);
	CHECK(close_to(a.duration, (51.0 * 576 - 576) / 24000));

	// Where an MPEG-1 stereo frame would hold it, it is not looked for.
	builder misplaced;
	misplaced.raw(xing_frame(lsf_header, lsf_frame, 32, "Xing", 1000, 192000)).raw(frames(lsf_header, lsf_frame, 50));
	CHECK(close_to(only_stream(scan(misplaced.b)).duration, 51.0 * 192 * 8 / 64000));
}

TEST("media mp3 reads Layers I and II, whose first frames hold no Xing header")
{
	builder f;
	f.raw(xing_frame(mp2_header, mp2_frame, 32, "Xing", 1000, 576000)).raw(frames(mp2_header, mp2_frame, 99));

	const auto s = scan(f.b);
	CHECK_STR(s.brand, "MPEG-1 Layer 2");
	const auto& a = only_stream(s);
	CHECK_STR(a.codec, "mp2");
	CHECK_EQ(a.bit_rate, 192000u);
	CHECK(close_to(a.duration, 100.0 * 576 * 8 / 192000));

	// Layer I at 384 kbit/s and 48 kHz: 384 bytes and 384 samples a frame.
	const auto mp1 = scan(frames(mpa_header(3, 1, 12, 1), 384, 125));
	CHECK_STR(mp1.brand, "MPEG-1 Layer 1");
	CHECK_STR(only_stream(mp1).codec, "mp1");
	CHECK(close_to(only_stream(mp1).duration, 1.0));
}

TEST("media mp3 skips junk before the first frame, as far as FFmpeg looks")
{
	builder f;
	f.raw(id3v2_tag(4, {text_frame(4, "TIT2", "Junk")}));
	// A sync word whose frame is followed by no other is junk too.
	f.zeros(300).be32(mp3_header).zeros(700);
	f.raw(frames(mp3_header, mp3_frame, 125));

	const auto s = scan(f.b, libfiles::scan_intent::inspect);
	CHECK(s.success);
	CHECK(close_to(only_stream(s).duration, 3.0));
	CHECK(std::ranges::any_of(s.structure, [](const libfiles::structure_row& row) { return row.key == "Junk" && row.value.starts_with("1004 bytes"); }));

	builder far;
	far.raw(id3v2_tag(3, {})).zeros(64 * 1024 + 1).raw(frames(mp3_header, mp3_frame, 10));
	const auto refused = scan(far.b);
	CHECK(!refused.success);
	CHECK(refused.format == media_format::mpeg_audio);
	CHECK(!refused.error.empty());
}

TEST("media mp3 does not count the tags at the end as audio")
{
	builder f;
	f.raw(frames(mp3_header, mp3_frame, 125));
	f.raw(ape_tag({{"Title", "APE title"}, {"Artist", "APE artist"}}));
	f.raw(lyrics3_tag("[00:01]Words"));
	f.raw(id3v1_tag("V1 title", "V1 artist"));

	const auto s = scan(f.b);
	CHECK(close_to(only_stream(s).duration, 3.0));
	CHECK_STR(s.properties.title, "APE title");
	CHECK_STR(s.properties.artist, "APE artist");

	// An ID3v2.4 tag appended after the audio is found from its footer.
	builder appended;
	appended.raw(frames(mp3_header, mp3_frame, 125));
	appended.raw(id3v2_tag(4, {text_frame(4, "TIT2", "Appended")}, 20, true));
	appended.raw(id3v1_tag("V1 title", ""));

	const auto a = scan(appended.b);
	CHECK(close_to(only_stream(a).duration, 3.0));
	CHECK_STR(a.properties.title, "Appended");

	// Lyrics3 version 1 runs from its start to "LYRICSEND".
	builder v1;
	v1.raw(frames(mp3_header, mp3_frame, 125)).raw("LYRICSBEGINla la laLYRICSEND").raw(id3v1_tag("V1", ""));
	CHECK(close_to(only_stream(scan(v1.b)).duration, 3.0));
}

TEST("media mp3 does not trust the frame count of streams joined together")
{
	builder f;
	// The header counts ten frames; a hundred follow it.
	f.raw(xing_frame(mp3_header, mp3_frame, 32, "Xing", 10, 3840)).raw(frames(mp3_header, mp3_frame, 100));

	const auto s = scan(f.b);
	CHECK(close_to(only_stream(s).duration, 100.0 * 384 * 8 / 128000));
	CHECK_EQ(only_stream(s).bit_rate, 128000u);
}

TEST("media mp3 reads a stream of one frame")
{
	const auto one = frames(mp3_header, mp3_frame, 1);
	CHECK(close_to(only_stream(scan(one)).duration, 0.024));

	builder tagged;
	tagged.raw(one).raw(id3v1_tag("One", ""));
	CHECK(close_to(only_stream(scan(tagged.b)).duration, 0.024));

	// A frame whose length leads nowhere is not one.
	builder lost;
	lost.raw(one).zeros(100);
	CHECK(!scan(lost.b).success);
}

TEST("media mp3 keeps the XMP packet read_packet finds")
{
	const std::string_view first_packet = "<x:xmpmeta>first</x:xmpmeta>";

	builder f;
	f.raw(id3v2_tag(3, {text_frame(3, "TIT2", "Title"), xmp_frame(3, first_packet)}));
	f.raw(id3v2_tag(3, {xmp_frame(3, "<x:xmpmeta>second</x:xmpmeta>")}));
	f.raw(frames(mp3_header, mp3_frame, 50));
	f.raw(id3v2_tag(4, {xmp_frame(4, "<x:xmpmeta>appended</x:xmpmeta>")}, 0, true));

	const auto s = scan(f.b);
	CHECK(s.success);
	CHECK_STR(to_string(s.xmp), first_packet);
	CHECK(close_to(only_stream(s).duration, 50 * 0.024));

	libfiles::reader r(f.b);
	CHECK_STR(to_string(s.xmp), libfiles::read_packet(r).packet);

	// A packet in a tag after the first is not the file's.
	builder later;
	later.raw(id3v2_tag(3, {text_frame(3, "TIT2", "Title")}));
	later.raw(id3v2_tag(3, {xmp_frame(3, "<x:xmpmeta>second</x:xmpmeta>")}));
	later.raw(frames(mp3_header, mp3_frame, 50));
	CHECK(scan(later.b).xmp.empty());
}

TEST("media mp3 lists its structure")
{
	builder f;
	f.raw(id3v2_tag(3, {text_frame(3, "TIT2", "Title")}));
	f.raw(xing_frame(mp3_header, mp3_frame, 32, "Xing", 101, 38784)).raw(frames(mp3_header, mp3_frame, 100));
	f.raw(id3v1_tag("Title", ""));

	const auto s = scan(f.b, libfiles::scan_intent::inspect);
	const auto row = [&s](const std::string_view key)
	{
		const auto found = std::ranges::find(s.structure, key, &libfiles::structure_row::key);
		return found == s.structure.end() ? std::string("(none)") : found->value;
	};

	CHECK_STR(row("ID3v2.3 tag"), std::format("offset 0, {} bytes", id3v2_tag(3, {text_frame(3, "TIT2", "Title")}).size()));
	CHECK_STR(row("Xing header"), "101 frames, 38784 bytes");
	CHECK_STR(row("LAME tag"), "LAME3.100, encoder delay 576, padding 1000");
	CHECK_STR(row("ID3v1 tag"), std::format("offset {}, 128 bytes", f.size() - 128));
	CHECK(scan(f.b).structure.empty());
}

TEST("media mp3 cut short or damaged is read or refused, and what is read holds together")
{
	builder f;
	f.raw(id3v2_tag(3, {text_frame(3, "TIT2", "Title")}, 20));
	const auto audio = f.size();
	f.raw(xing_frame(mp3_header, mp3_frame, 32, "Xing", 21, 21 * 384)).raw(frames(mp3_header, mp3_frame, 20));
	const auto tags = f.size();
	f.raw(ape_tag({{"Title", "APE"}}));
	const auto lyrics = f.size();
	f.raw(lyrics3_tag("words"));
	const auto v1 = f.size();
	f.raw(id3v1_tag("V1", ""));

	check_cut_short(f.b, {audio, audio + mp3_frame, tags, lyrics, v1});
	check_corrupted(f.b, 0, audio + 200);
	check_corrupted(f.b, tags - 4, f.size());

	const auto s = scan(f.b);
	CHECK(s.success);
	CHECK(close_to(only_stream(s).duration, (21.0 * 1152 - 1576) / 48000));
}

TEST("media mp3 reads the ends of a large file and nothing between")
{
	builder f;
	f.raw(id3v2_tag(3, {text_frame(3, "TIT2", "Title")}, 1000));
	// 120 seconds at 128 kbit/s: almost two megabytes.
	f.raw(frames(mp3_header, mp3_frame, 5000)).raw(id3v1_tag("Title", "Artist"));

	std::uint64_t reads = 0;
	const auto s = scan_stream(f.b, reads);
	CHECK(s.success);
	CHECK(close_to(only_stream(s).duration, 120.0));
	CHECK_EQ(reads, 2u);
}

// ---- FLAC ----------------------------------------------------------------------------------------------

TEST("media flac reads STREAMINFO, the Vorbis comment and a picture it locates")
{
	builder f;
	f.raw("fLaC");
	f.raw(flac_block(0, streaminfo(44100, 2, 16, 441000)));
	f.raw(flac_block(4, vorbis_comment({"TITLE=Flac title", "ARTIST=Flac artist"})));
	const auto picture_block = f.size();
	const auto image = filler(1000, 0x89);
	f.raw(flac_block(6, picture("image/png", "Cover", image)));
	f.raw(flac_block(1, filler(100, 0), true));
	f.raw(flac_frame(false, 0, 4096));

	const auto s = scan(f.b);
	CHECK(s.success);
	CHECK(s.format == media_format::flac);
	CHECK_STR(s.brand, "FLAC");

	const auto& a = only_stream(s);
	CHECK(a.kind == media_stream_kind::audio);
	CHECK_EQ(a.id, 0u);
	CHECK_STR(a.codec, "flac");
	CHECK_EQ(a.audio.sample_rate, 44100u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK_EQ(a.audio.bits_per_sample, 16u);
	CHECK_EQ(a.bit_rate, 0u);
	CHECK(close_to(a.duration, 10.0));
	CHECK(s.complete);

	CHECK_STR(s.properties.title, "Flac title");
	CHECK_STR(s.properties.artist, "Flac artist");
	CHECK(s.xmp.empty());

	CHECK_EQ(s.pictures.size(), 1u);
	if (!s.pictures.empty())
	{
		const auto& p = s.pictures[0];
		CHECK_STR(p.mime_type, "image/png");
		CHECK_STR(p.description, "Cover");
		CHECK_EQ(p.type, 3);
		CHECK(p.data.empty());
		CHECK_EQ(p.location.offset, picture_block + 4 + 4 + 4 + 9 + 4 + 5 + 16 + 4);
		CHECK_EQ(p.location.size, image.size());
	}
}

TEST("media flac locates a large picture from the head of its block")
{
	// A picture block between STREAMINFO and a Vorbis comment; the image starts after the block's fields.
	const auto build = [](const bytes& body)
	{
		builder f;
		f.raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 16, 441000))).raw(flac_block(6, body));
		f.raw(flac_block(4, vorbis_comment({"TITLE=After the picture"}), true)).raw(flac_frame(false, 0, 4096));
		return f.b;
	};

	struct reading
	{
		media_scan_result result;
		std::uint64_t reads = 0;
		std::uint64_t bytes = 0;
	};

	const auto read = [](const bytes& file)
	{
		libfiles::memory_stream s(file);
		libfiles::reader r(s);
		reading out{libfiles::scan_media(r)};
		out.reads = r.source_reads();
		out.bytes = r.source_bytes();
		return out;
	};

	constexpr std::uint64_t block = 4 + 38 + 4;
	const auto image = filler(4 * 1024 * 1024, 0xD8);

	// Four megabytes of cover cost the head of its block and the block after it: three reads, not the image.
	const auto large = read(build(picture("image/jpeg", "Front", image)));
	CHECK(large.result.success);
	CHECK_STR(large.result.properties.title, "After the picture");
	CHECK_EQ(large.reads, 3u);
	CHECK(large.bytes < 256 * 1024);
	CHECK_EQ(large.result.pictures.size(), 1u);

	if (!large.result.pictures.empty())
	{
		const auto& p = large.result.pictures[0];
		CHECK_EQ(p.location.offset, block + 4 + 4 + 10 + 4 + 5 + 16 + 4);
		CHECK_EQ(p.location.size, image.size());
		CHECK(p.data.empty());
		CHECK_STR(p.mime_type, "image/jpeg");
		CHECK_STR(p.description, "Front");
	}

	// Fields that run past the head - a description longer than it - are read from the whole block.
	const std::string description(70000, 'd');
	const auto long_fields = read(build(picture("image/png", description, filler(1000, 0x89))));
	CHECK_EQ(long_fields.result.pictures.size(), 1u);
	if (!long_fields.result.pictures.empty()) CHECK_EQ(long_fields.result.pictures[0].location.offset, block + 4 + 4 + 9 + 4 + 70000 + 16 + 4);

	// Fields the head holds are not read again when they are not a picture: a URL, in a large block.
	const auto link = read(build(picture("-->", "", filler(1024 * 1024, 'u'))));
	CHECK(link.result.success);
	CHECK(link.result.pictures.empty());
	CHECK(link.bytes < 256 * 1024);

	// A picture that would run past its block is not one.
	const auto damaged = read(build(picture("image/png", "", filler(1000, 0x89), 5000)));
	CHECK(damaged.result.success);
	CHECK(damaged.result.pictures.empty());
	CHECK_STR(damaged.result.properties.title, "After the picture");
}

TEST("media flac without a sample count finds where the last frame ends")
{
	const auto build = [](const bool variable, const unsigned rate_code)
	{
		builder f;
		f.raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 16, 0, variable ? 1000 : 4096, 4096), true));

		for (std::uint64_t i = 0; i < 9; ++i) f.raw(flac_frame(variable, variable ? i * 4096 : i, 4096, 300, rate_code));
		f.raw(flac_frame(variable, variable ? 9 * 4096 : 9, 1000, 300, rate_code));
		return f.b;
	};

	const auto fixed = scan(build(false, 0));
	CHECK(close_to(only_stream(fixed).duration, (9.0 * 4096 + 1000) / 44100));
	CHECK(fixed.complete);

	// A header that names its rate must name STREAMINFO's: 44.1 kHz is code 9.
	CHECK(close_to(only_stream(scan(build(true, 9))).duration, (9.0 * 4096 + 1000) / 44100));

	// Frames that do not agree with STREAMINFO are not this stream's: the duration stays unknown.
	const auto other = scan(build(false, 10));
	CHECK(other.success);
	CHECK_EQ(only_stream(other).duration, 0.0);
	CHECK(!other.complete);

	// An ID3v1 tag after the frames is passed over.
	auto tagged = build(false, 0);
	const auto v1 = id3v1_tag("Title", "");
	tagged.insert(tagged.end(), v1.begin(), v1.end());
	CHECK(close_to(only_stream(scan(tagged)).duration, (9.0 * 4096 + 1000) / 44100));
}

TEST("media flac after an ID3v2 tag")
{
	builder f;
	f.raw(id3v2_tag(3, {text_frame(3, "TIT2", "ID3 title")}));
	f.raw("fLaC").raw(flac_block(0, streaminfo(48000, 1, 24, 96000), true)).raw(flac_frame(false, 0, 4096));

	const auto s = scan(f.b);
	CHECK(s.success);
	CHECK(s.format == media_format::flac);
	CHECK(close_to(only_stream(s).duration, 2.0));
	CHECK_EQ(only_stream(s).audio.bits_per_sample, 24u);
	CHECK_STR(s.properties.title, "ID3 title");
}

TEST("media flac refuses a stream that does not begin with STREAMINFO")
{
	builder comment_first;
	comment_first.raw("fLaC").raw(flac_block(4, vorbis_comment({"TITLE=x"}))).raw(flac_block(0, streaminfo(44100, 2, 16, 44100), true));
	CHECK(!scan(comment_first.b).success);

	builder short_info;
	auto info = streaminfo(44100, 2, 16, 44100);
	info.pop_back();
	short_info.raw("fLaC").raw(flac_block(0, info, true)).zeros(100);
	CHECK(!scan(short_info.b).success);

	builder cut;
	cut.raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 16, 44100), true));
	cut.b.resize(cut.b.size() - 1);
	const auto s = scan(cut.b);
	CHECK(!s.success);
	CHECK(s.format == media_format::flac);
}

TEST("media flac lists its metadata blocks")
{
	builder f;
	f.raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 16, 441000))).raw(flac_block(3, filler(18, 0))).raw(flac_block(1, filler(10, 0), true));
	f.raw(flac_frame(false, 0, 4096));

	const auto s = scan(f.b, libfiles::scan_intent::inspect);
	const auto row = [&s](const std::string_view key)
	{
		const auto found = std::ranges::find(s.structure, key, &libfiles::structure_row::key);
		return found == s.structure.end() ? std::string("(none)") : found->value;
	};

	CHECK_STR(row("STREAMINFO"), "offset 4, 34 bytes");
	CHECK_STR(row("SEEKTABLE"), "offset 42, 18 bytes");
	CHECK_STR(row("PADDING"), "offset 64, 10 bytes, last");
	CHECK_STR(row("Samples"), "441000, from STREAMINFO");
}

TEST("media flac cut short or damaged is read or refused, and what is read holds together")
{
	builder f;
	f.raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 16, 0)));
	const auto comment = f.size();
	f.raw(flac_block(4, vorbis_comment({"TITLE=Title", "ARTIST=Artist"})));
	const auto pic = f.size();
	f.raw(flac_block(6, picture("image/jpeg", "", filler(50, 0xD8))));
	const auto pad = f.size();
	f.raw(flac_block(1, filler(20, 0), true));
	const auto audio = f.size();
	for (std::uint64_t i = 0; i < 4; ++i) f.raw(flac_frame(false, i, i == 3 ? 100 : 4096, 100));

	check_cut_short(f.b, {comment, pic, pad, audio});
	check_corrupted(f.b, 0, audio + 20);
	check_corrupted(f.b, f.size() - 120, f.size());
}

TEST("media flac reads a large file's metadata and its last frame, and nothing between")
{
	builder f;
	f.raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 16, 0)));
	f.raw(flac_block(1, filler(256 * 1024, 0)));
	f.raw(flac_block(4, vorbis_comment({"TITLE=Title"}), true));
	for (std::uint64_t i = 0; i < 200; ++i) f.raw(flac_frame(false, i, 4096, 8000));

	std::uint64_t reads = 0;
	const auto s = scan_stream(f.b, reads);
	CHECK(close_to(only_stream(s).duration, 200.0 * 4096 / 44100));
	CHECK_EQ(reads, 3u);
}

// ---- Ogg -----------------------------------------------------------------------------------------------

TEST("media ogg reads a Vorbis stream: its headers, comments and last granule position")
{
	ogg_writer w{0x12345678};
	const auto file = vorbis_stream(w, 44100, 132300, {"TITLE=Ogg title", "ARTIST=Ogg artist"});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK(s.format == media_format::ogg);
	CHECK_STR(s.brand, "Ogg");

	const auto& a = only_stream(s);
	CHECK(a.kind == media_stream_kind::audio);
	// FFmpeg numbers Ogg streams in the order they begin, whatever their serial numbers.
	CHECK_EQ(a.id, 0u);
	CHECK_STR(a.codec, "vorbis");
	CHECK_EQ(a.audio.sample_rate, 44100u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK_EQ(a.audio.bits_per_sample, 0u);
	CHECK_EQ(a.bit_rate, 128000u);
	CHECK(close_to(a.duration, 3.0));
	CHECK(close_to(s.duration, 3.0));
	CHECK(s.complete);
	CHECK_STR(s.properties.title, "Ogg title");
	CHECK_STR(s.properties.artist, "Ogg artist");
}

TEST("media ogg times Opus at 48 kHz without its pre-skip, from where its first page starts it")
{
	// As libopus lays a stream out: granule positions count the samples decoded, the pre-skip among them, so
	// three seconds and a pre-skip of 312 end at 144312, on a page of their own.
	const auto build = [](const std::uint64_t start)
	{
		ogg_writer w{77};
		auto file = w.pages({{opus_head(2, 312), 0}});
		const auto tags = w.pages({{opus_tags({"TITLE=Opus title"}), 0}});
		file.insert(file.end(), tags.begin(), tags.end());

		for (std::uint64_t p = 1; p <= 3; ++p)
		{
			std::vector<std::pair<bytes, std::uint64_t>> packets;
			for (auto i = 0; i < 50; ++i) packets.emplace_back(opus_packet(), start + p * 48000);
			const auto data = w.pages(packets);
			file.insert(file.end(), data.begin(), data.end());
		}

		const auto last = w.pages({{opus_packet(), start + 144312}}, true);
		file.insert(file.end(), last.begin(), last.end());
		return file;
	};

	const auto s = scan(build(0));
	const auto& a = only_stream(s);
	CHECK_STR(a.codec, "opus");
	CHECK_EQ(a.audio.sample_rate, 48000u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK(close_to(a.duration, 3.0));
	CHECK_STR(s.properties.title, "Opus title");

	// A stream whose first page begins a second in is timed from there.
	CHECK(close_to(only_stream(scan(build(48000))).duration, 3.0));
}

TEST("media ogg reads FLAC in Ogg")
{
	const auto s = scan(ogg_flac_file());
	const auto& a = only_stream(s);
	CHECK_STR(a.codec, "flac");
	CHECK_EQ(a.audio.sample_rate, 44100u);
	CHECK_EQ(a.audio.bits_per_sample, 24u);
	CHECK(close_to(a.duration, 2.0));
	CHECK_STR(s.properties.title, "Ogg FLAC");
}

TEST("media ogg locates a FLAC picture that lies whole on its page")
{
	const auto build = [](const bytes& image, const std::size_t claimed)
	{
		ogg_writer w{8};
		auto file = w.pages({{builder().u8(0x7F).raw("FLAC").u8(1).u8(0).be16(2).raw("fLaC").raw(flac_block(0, streaminfo(44100, 2, 16, 0))).b, 0}});
		append(file, w.pages({{flac_block(4, vorbis_comment({"TITLE=Pictured"})), 0}, {flac_block(6, picture("image/png", "Cover", image, claimed), true), 0}}));
		append(file, w.pages({{flac_frame(false, 0, 4096, 500), 44100}}, true));
		return file;
	};

	const auto image = filler(500, 0xA5);
	const auto file = build(image, image.size());
	const auto s = scan(file);
	CHECK_STR(s.properties.title, "Pictured");
	CHECK_EQ(s.pictures.size(), 1u);

	if (!s.pictures.empty())
	{
		const auto at = std::search(file.begin(), file.end(), image.begin(), image.end());
		CHECK_EQ(s.pictures[0].location.offset, static_cast<std::uint64_t>(at - file.begin()));
		CHECK_EQ(s.pictures[0].location.size, image.size());
		CHECK(s.pictures[0].data.empty());
		CHECK_STR(s.pictures[0].description, "Cover");
	}

	// One that would run past its packet is not a picture.
	CHECK(scan(build(image, 5000)).pictures.empty());

	// One whose packet runs across pages is not one run of bytes in the file: its image is copied.
	const auto large = filler(100000, 0xA5);
	const auto split = scan(build(large, large.size()));
	CHECK_EQ(split.pictures.size(), 1u);

	if (!split.pictures.empty())
	{
		CHECK_EQ(split.pictures[0].location.size, 0u);
		CHECK(split.pictures[0].data == large);
	}
}

TEST("media ogg reads Theora and Vorbis together, timed by the longer")
{
	const auto s = scan(theora_vorbis_file());
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), 2u);

	if (s.streams.size() == 2)
	{
		const auto& v = s.streams[0];
		CHECK(v.kind == media_stream_kind::video);
		CHECK_EQ(v.id, 0u);
		CHECK_STR(v.codec, "theora");
		CHECK_EQ(v.video.width, 320u);
		CHECK_EQ(v.video.height, 240u);
		CHECK(close_to(v.video.frame_rate, 25.0));
		CHECK(v.video.chroma == libfiles::chroma_format::yuv420);
		CHECK_EQ(v.video.bit_depth, 8u);
		CHECK(close_to(v.duration, 3.0));

		const auto& a = s.streams[1];
		CHECK(a.kind == media_stream_kind::audio);
		CHECK_EQ(a.id, 1u);
		CHECK_STR(a.codec, "vorbis");
		CHECK(close_to(a.duration, 3.5));
	}

	CHECK(close_to(s.duration, 3.5));
	CHECK(s.complete);
	CHECK_STR(s.properties.title, "Film");
}

TEST("media ogg counts a Skeleton stream but does not list it")
{
	ogg_writer skeleton{1};
	ogg_writer w{2};

	// The Skeleton's first page, then the Vorbis stream's; the Skeleton ends after the headers.
	auto file = skeleton.pages({{builder().raw(std::string_view("fishead\0", 8)).le16(3).le16(0).zeros(52).b, 0}});
	const auto audio = vorbis_stream(w, 44100, 44100, {});
	const auto audio_headers = 27 + 1 + 30;
	file.insert(file.end(), audio.begin(), audio.begin() + audio_headers);
	const auto end = skeleton.pages({{bytes{}, 0}}, true);
	file.insert(file.end(), end.begin(), end.end());
	file.insert(file.end(), audio.begin() + audio_headers, audio.end());

	const auto s = scan(file);
	const auto& a = only_stream(s);
	CHECK_STR(a.codec, "vorbis");
	CHECK_EQ(a.id, 1u);
	CHECK(close_to(a.duration, 1.0));
}

TEST("media ogg puts together a comment packet that spans pages")
{
	const std::string description(100000, 'd');
	const auto field = "DESCRIPTION=" + description;

	ogg_writer w{9};
	const auto file = vorbis_stream(w, 44100, 88200, {"TITLE=Long", field});

	const auto s = scan(file);
	CHECK_STR(s.properties.title, "Long");
	CHECK(has_tag(s, description));
	CHECK(close_to(only_stream(s).duration, 2.0));
}

TEST("media ogg passes over a page whose CRC does not match")
{
	ogg_writer w{3};
	auto file = vorbis_stream(w, 44100, 132300, {"TITLE=Lost"});

	// The comment page, the second, damaged: its stream keeps its format and its duration, not its tags.
	constexpr std::size_t second_page = 27 + 1 + 30;
	auto damaged = file;
	damaged[second_page + 40] ^= 0xFF;

	const auto s = scan(damaged);
	CHECK(s.success);
	CHECK(s.properties.title.empty());
	CHECK(close_to(only_stream(s).duration, 3.0));

	// The last page damaged: the page before it says how far the stream got.
	auto last = file;
	last[last.size() - 10] ^= 0xFF;
	CHECK(close_to(only_stream(scan(last)).duration, 2.0));
}

TEST("media ogg cut short keeps what its whole pages say")
{
	ogg_writer w{4};
	const auto file = vorbis_stream(w, 44100, 132300, {"TITLE=Cut"});

	// Cut inside the last page: the page before it ends at two seconds.
	const auto cut = scan(bytes(file.begin(), file.end() - 50));
	CHECK(cut.success);
	CHECK(close_to(only_stream(cut).duration, 2.0));

	// Cut after the headers: the stream is described, but not for how long it plays.
	ogg_writer h{4};
	const auto headers = vorbis_stream(h, 44100, 0, {"TITLE=Cut"}, 0);
	const auto bare = scan(headers);
	CHECK(bare.success);
	CHECK_EQ(only_stream(bare).duration, 0.0);
	CHECK(!bare.complete);
	CHECK_STR(bare.properties.title, "Cut");
}

TEST("media ogg describes the first link of a chained file")
{
	ogg_writer first{10};
	ogg_writer second{11};
	auto file = vorbis_stream(first, 44100, 88200, {"TITLE=First"});
	const auto next = vorbis_stream(second, 22050, 66150, {"TITLE=Second"});
	file.insert(file.end(), next.begin(), next.end());

	const auto s = scan(file, libfiles::scan_intent::inspect);
	CHECK(s.success);
	const auto& a = only_stream(s);
	CHECK_EQ(a.audio.sample_rate, 44100u);
	CHECK_STR(s.properties.title, "First");
	// How long the links play together is not in their last pages.
	CHECK_EQ(a.duration, 0.0);
	CHECK(!s.complete);
	CHECK(std::ranges::any_of(s.structure, [](const libfiles::structure_row& row) { return row.key == "Chained"; }));

	// Links joined end to end may reuse a serial number: the later link's first page gives it away.
	ogg_writer once{12};
	ogg_writer again{12};
	auto twice = vorbis_stream(once, 44100, 88200, {});
	const auto repeat = vorbis_stream(again, 44100, 88200, {});
	twice.insert(twice.end(), repeat.begin(), repeat.end());
	CHECK_EQ(only_stream(scan(twice)).duration, 0.0);
}

TEST("media ogg reads Speex, and a stream it does not know as data")
{
	ogg_writer speex{20};
	ogg_writer other{21};

	auto file = speex.pages({{speex_header(16000, 1), 0}});
	const auto other_id = other.pages({{builder().raw("\x01video").zeros(50).b, 0}});
	file.insert(file.end(), other_id.begin(), other_id.end());
	const auto comment = speex.pages({{vorbis_comment({"TITLE=Speech"}), 0}});
	file.insert(file.end(), comment.begin(), comment.end());
	const auto data = speex.pages({{filler(40, 0x10), 16000}, {filler(40, 0x10), 32000}}, true);
	file.insert(file.end(), data.begin(), data.end());
	const auto other_data = other.pages({{filler(40, 0x10), 100}}, true);
	file.insert(file.end(), other_data.begin(), other_data.end());

	const auto s = scan(file);
	CHECK_EQ(s.streams.size(), 2u);

	if (s.streams.size() == 2)
	{
		CHECK_STR(s.streams[0].codec, "speex");
		CHECK_EQ(s.streams[0].audio.sample_rate, 16000u);
		CHECK_EQ(s.streams[0].audio.channels, 1u);
		CHECK(close_to(s.streams[0].duration, 2.0));
		CHECK(s.streams[1].kind == media_stream_kind::data);
		CHECK(s.streams[1].codec.empty());
		CHECK_EQ(s.streams[1].id, 1u);
	}

	CHECK_STR(s.properties.title, "Speech");
}

TEST("media ogg refuses a file whose first pages begin no stream")
{
	ogg_writer w{6};
	w.begun = true;
	const auto file = w.pages({{filler(100), 1000}}, true);
	const auto s = scan(file);
	CHECK(!s.success);
	CHECK(s.format == media_format::ogg);
}

TEST("media ogg cut short or damaged is read or refused, and what is read holds together")
{
	ogg_writer opus{30};
	auto file = opus.pages({{opus_head(2, 312), 0}});
	const auto tags = file.size();
	const auto tag_pages = opus.pages({{opus_tags({"TITLE=T", "ARTIST=A"}), 0}});
	file.insert(file.end(), tag_pages.begin(), tag_pages.end());
	const auto data = file.size();

	for (std::uint64_t p = 1; p <= 2; ++p)
	{
		std::vector<std::pair<bytes, std::uint64_t>> packets;
		for (auto i = 0; i < 5; ++i) packets.emplace_back(opus_packet(), p * 4800 + 312);
		const auto pages = opus.pages(packets, p == 2);
		file.insert(file.end(), pages.begin(), pages.end());
	}

	check_cut_short(file, {tags, data});
	check_corrupted(file, 0, data + 40);
	check_corrupted(file, file.size() - 60, file.size());

	// Two streams, and FLAC in Ogg, near their ends and their headers.
	const auto both = theora_vorbis_file();
	check_cut_short(both, {42 + 28, 2 * 28 + 42 + 30, 600});
	check_corrupted(both, 0, 160);

	const auto flac = ogg_flac_file();
	check_cut_short(flac, {79, 160});
	check_corrupted(flac, 0, 140);
}

TEST("media ogg reads a large file's first and last pages, and nothing between")
{
	ogg_writer w{40};
	const auto file = vorbis_stream(w, 44100, 44100 * 60, {"TITLE=Long"}, 1000);
	CHECK(file.size() > 1000000);

	std::uint64_t reads = 0;
	const auto s = scan_stream(file, reads);
	CHECK(close_to(only_stream(s).duration, 60.0));
	CHECK_EQ(reads, 2u);
}
