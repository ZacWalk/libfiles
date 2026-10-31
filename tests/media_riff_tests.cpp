// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the AVI, WAV and AIFF media scans. Each file is built chunk by chunk here, then read
// whole, cut short at every byte near its ends and the edges of its chunks, and with each chunk's size
// corrupted - where a RIFF scan's packet must stay the one read_packet reads.

#include "test.h"
#include "builders.h"

#include "libfiles/media.h"
#include "libfiles/write.h"
#include "media_internal.h"
#include "round_trip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace test_build;
	using libfiles::media_scan_result;
	using libfiles::media_stream_kind;

	bytes cat(const std::initializer_list<bytes> parts)
	{
		bytes out;
		for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
		return out;
	}

	bytes text(const std::string_view s)
	{
		auto b = to_bytes(s);
		b.push_back(0);
		return b;
	}

	// RIFF, little-endian.

	bytes chunk(const std::string_view id, const bytes& payload)
	{
		builder b;
		riff_chunk(b, id, payload);
		return b.b;
	}

	bytes with_type(const std::string_view type, const std::vector<bytes>& children)
	{
		builder b;
		b.raw(type);
		for (const auto& c : children) b.raw(c);
		return b.b;
	}

	bytes list(const std::string_view type, const std::vector<bytes>& children)
	{
		return chunk("LIST", with_type(type, children));
	}

	bytes riff(const std::string_view type, const std::vector<bytes>& children)
	{
		return chunk("RIFF", with_type(type, children));
	}

	bytes avih(const std::uint32_t frame_period, const std::uint32_t frames, const std::uint32_t streams, const std::uint32_t width,
	           const std::uint32_t height)
	{
		return builder().le32(frame_period).le32(0).le32(0).le32(0x910).le32(frames).le32(0).le32(streams).le32(0x100000).le32(width)
		                .le32(height).zeros(16).b;
	}

	bytes strh(const std::string_view type, const std::string_view handler, const std::uint32_t scale, const std::uint32_t rate,
	           const std::uint32_t length, const std::uint32_t sample_size = 0, const std::array<int, 4> frame = {})
	{
		builder b;
		b.raw(type).raw(handler).zeros(4 - handler.size()).le32(0).le16(0).le16(0).le32(0).le32(scale).le32(rate).le32(0).le32(length)
		 .le32(0x100000).le32(0xFFFFFFFF).le32(sample_size);
		for (const auto v : frame) b.le16(static_cast<unsigned>(v));
		return b.b;
	}

	// A BITMAPINFOHEADER, whose size counts what its codec keeps after it, as FFmpeg writes it.
	bytes bitmap_info(const std::uint32_t width, const std::uint32_t height, const std::string_view compression, const bytes& extra = {})
	{
		return builder().le32(static_cast<std::uint32_t>(40 + extra.size())).le32(width).le32(height).le16(1).le16(24).raw(compression)
		                .le32(width * height * 3).zeros(16).raw(extra).b;
	}

	bytes wave_format(const unsigned tag, const unsigned channels, const std::uint32_t rate, const unsigned align, const std::uint32_t average,
	                  const unsigned bits, const bytes& extra = {}, const bool extended = true)
	{
		builder b;
		b.le16(tag).le16(channels).le32(rate).le32(average).le16(align).le16(bits);
		if (extended) b.le16(static_cast<unsigned>(extra.size())).raw(extra);
		return b.b;
	}

	// PCMWAVEFORMAT, as most writers give PCM; tag 3 for floating point.
	bytes pcm_format(const unsigned channels, const std::uint32_t rate, const unsigned bits, const unsigned tag = 1)
	{
		const auto align = channels * ((bits + 7) / 8);
		return wave_format(tag, channels, rate, align, rate * align, bits, {}, tag != 1);
	}

	// WAVEFORMATEXTENSIBLE: samples of container bits holding valid bits, of a KSDATAFORMAT subtype.
	bytes extensible_format(const unsigned channels, const std::uint32_t rate, const unsigned container, const unsigned valid,
	                        const unsigned subtype)
	{
		const auto align = channels * container / 8;
		const auto extra = builder().le16(valid).le32(channels == 2 ? 3u : 4u).le16(subtype).le16(0).le16(0).le16(0x10).u8(0x80).u8(0)
		                            .u8(0).u8(0xAA).u8(0).u8(0x38).u8(0x9B).u8(0x71).b;
		return wave_format(0xFFFE, channels, rate, align, rate * align, container, extra);
	}

	bytes vprp(const unsigned aspect_x, const unsigned aspect_y, const std::uint32_t width, const std::uint32_t height, const std::uint32_t fields)
	{
		return builder().le32(0).le32(0).le32(50).le32(0).le32(0).le16(aspect_y).le16(aspect_x).le32(width).le32(height).le32(fields)
		                .zeros(32 * fields).b;
	}

	// A frame of Motion JPEG: start of image, a baseline SOF with Y sampled hy x vy and both chroma once,
	// and end of image.
	bytes jpeg_frame(const unsigned width, const unsigned height, const unsigned hy, const unsigned vy)
	{
		return builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xE0).be16(16).raw("AVI1").zeros(10).u8(0xFF).u8(0xC0).be16(17).u8(8).be16(height)
		                .be16(width).u8(3).u8(1).u8(hy << 4 | vy).u8(0).u8(2).u8(0x11).u8(1).u8(3).u8(0x11).u8(1).u8(0xFF).u8(0xD9).b;
	}

	// x264's sequence and picture parameter sets for 320 x 240, High profile, 4:2:0, 8 bits.
	const bytes h264_sps = {0x67, 0x64, 0x00, 0x0D, 0xAC, 0xD9, 0x41, 0x41, 0xFA, 0x10, 0x00, 0x00,
	                        0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x20, 0xF1, 0x42, 0x99, 0x60};
	const bytes h264_pps = {0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0};

	bytes annexb(const std::vector<bytes>& nals)
	{
		builder b;
		for (const auto& n : nals) b.be32(1).raw(n);
		return b.b;
	}

	bytes avc_config()
	{
		return builder().u8(1).u8(0x64).u8(0).u8(0x0D).u8(0xFF).u8(0xE1).be16(static_cast<unsigned>(h264_sps.size())).raw(h264_sps).u8(1)
		                .be16(static_cast<unsigned>(h264_pps.size())).raw(h264_pps).b;
	}

	// FFmpeg's MPEG-4 Part 2 headers for 352 x 288, Simple Profile: the visual object sequence, visual
	// object and video object layer.
	const bytes mpeg4_headers = {0x00, 0x00, 0x01, 0xB0, 0x01, 0x00, 0x00, 0x01, 0xB5, 0x89, 0x13, 0x00, 0x00, 0x01,
	                             0x00, 0x00, 0x00, 0x01, 0x20, 0x00, 0xC4, 0x8D, 0x88, 0x00, 0xCD, 0x0B, 0x04, 0x24,
	                             0x14, 0x43, 0x00, 0x00, 0x01, 0xB6};

	// The header DIF block that starts a 625/50 DV frame.
	bytes dv_frame()
	{
		auto b = builder().u8(0x1F).u8(0x07).u8(0x00).u8(0xBF).u8(0xF8).u8(0x78).u8(0x78).u8(0x78).b;
		b.resize(80, 0xFF);
		b.resize(480, 0);
		return b;
	}

	// AIFF, big-endian.

	bytes iff_chunk(const std::string_view id, const bytes& payload)
	{
		builder b;
		b.raw(id).be32(static_cast<std::uint32_t>(payload.size())).raw(payload);
		if (payload.size() & 1) b.u8(0);
		return b.b;
	}

	bytes aiff(const std::string_view type, const std::vector<bytes>& chunks)
	{
		builder body;
		body.raw(type);
		for (const auto& c : chunks) body.raw(c);
		return builder().raw("FORM").be32(static_cast<std::uint32_t>(body.size())).raw(body.b).b;
	}

	// An IEEE 754 80-bit extended number, as COMM keeps a sample rate.
	bytes extended(const double value)
	{
		auto exponent = 0;
		const auto significand = static_cast<std::uint64_t>(std::ldexp(std::frexp(value, &exponent), 64));
		return builder().be16(static_cast<unsigned>(exponent - 1 + 16383)).be32(static_cast<std::uint32_t>(significand >> 32))
		                .be32(static_cast<std::uint32_t>(significand)).b;
	}

	bytes comm(const unsigned channels, const std::uint32_t frames, const unsigned bits, const double rate, const std::string_view compression = {},
	           const std::string_view name = {})
	{
		builder b;
		b.be16(channels).be32(frames).be16(bits).raw(extended(rate));

		if (!compression.empty())
		{
			b.raw(compression).u8(static_cast<unsigned>(name.size())).raw(name);
			if (name.size() % 2 == 0) b.u8(0);
		}

		return b.b;
	}

	// An ID3v2.3 tag holding one text frame.
	bytes id3_tag(const std::string_view frame, const std::string_view value)
	{
		builder f;
		f.raw(frame).be32(static_cast<std::uint32_t>(value.size() + 1)).be16(0).u8(0).raw(value);
		const auto size = static_cast<std::uint32_t>(f.size());
		return builder().raw("ID3").u8(3).u8(0).u8(0).u8(size >> 21 & 0x7F).u8(size >> 14 & 0x7F).u8(size >> 7 & 0x7F).u8(size & 0x7F)
		                .raw(f.b).b;
	}

	const std::string_view packet_a = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF/></x:xmpmeta>";
	const std::string_view packet_b = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" b=\"1\"><rdf:RDF/></x:xmpmeta>";

	// Reading.

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

	// The file as the writer leaves it after an update.
	bytes edited(const bytes& file, const libfiles::metadata_update& u)
	{
		libfiles::reader r(file);
		const auto plan = libfiles::plan_update(r, u);
		libfiles::memory_stream out;
		if (plan.ok()) libfiles::write_updated(r, plan, out);
		return out.bytes();
	}

	// The first stream, or an empty one when the scan found none, so a check fails rather than throws.
	const libfiles::media_stream& first_stream(const media_scan_result& s)
	{
		static const libfiles::media_stream none;
		return s.streams.empty() ? none : s.streams.front();
	}

	std::string xmp_of(const media_scan_result& s)
	{
		return {s.xmp.begin(), s.xmp.end()};
	}

	std::uint64_t reads_of(const bytes& file)
	{
		libfiles::memory_stream source(file);
		libfiles::reader r(source);
		const auto s = libfiles::scan_media(r);
		return s.success ? r.source_reads() : 0;
	}

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	std::string tag_value(const std::vector<libfiles::media_tag>& tags, const std::string_view key)
	{
		for (const auto& t : tags) if (t.key == key) return t.value;
		return {};
	}

	// Where every chunk header starts, at any depth of lists.
	void chunk_offsets(const bytes& file, std::size_t pos, const std::size_t end, const bool big_endian, std::set<std::size_t>& out, const int depth)
	{
		while (end > pos && end - pos >= 8)
		{
			out.insert(pos);
			const auto size = big_endian ? libfiles::detail::load_be32(file.data() + pos + 4) : libfiles::detail::load_le32(file.data() + pos + 4);
			const std::string id(reinterpret_cast<const char*>(file.data() + pos), 4);
			const auto nested = id == "LIST" || id == "RIFF" || id == "RF64" || id == "FORM";
			if (size > end - pos - 8) return;
			if (nested && size >= 4 && depth < 4) chunk_offsets(file, pos + 12, pos + 8 + size, big_endian, out, depth + 1);
			pos += 8 + size + (size & 1);
		}
	}

	// A damaged file reads as an error, or as a result that holds together: streams, finite durations,
	// and for RIFF the packet read_packet reads.
	std::string fault_in(const bytes& file, const bool riff_packet)
	{
		const auto s = scan(file);

		if (!s.success)
		{
			if (s.error.empty()) return "no error given";
			if (!s.streams.empty()) return "streams with an error";
			return {};
		}

		if (s.streams.empty()) return "no streams";
		if (!std::isfinite(s.duration) || s.duration < 0) return "a bad duration";

		for (const auto& st : s.streams)
		{
			if (!std::isfinite(st.duration) || st.duration < 0) return "a bad stream duration";
		}

		if (riff_packet && xmp_of(s) != packet_of(file)) return "a packet read_packet does not read";
		return {};
	}

	// The file cut short at every byte near its ends and the edges of its chunks, and with each chunk's size
	// corrupted. Returns the first fault found, empty when there is none.
	std::string damage(const bytes& file, const bool big_endian, const bool riff_packet = true)
	{
		std::set<std::size_t> offsets;
		chunk_offsets(file, 0, file.size(), big_endian, offsets, 0);

		std::set<std::size_t> cuts;

		for (std::size_t i = 0; i <= 48 && i <= file.size(); ++i)
		{
			cuts.insert(i);
			cuts.insert(file.size() - i);
		}

		for (const auto o : offsets)
		{
			for (std::size_t d = 0; d < 24; ++d)
			{
				if (o + d <= file.size()) cuts.insert(o + d);
				if (o >= d) cuts.insert(o - d);
			}
		}

		for (const auto cut : cuts)
		{
			const bytes part(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut));
			if (const auto fault = fault_in(part, riff_packet); !fault.empty()) return std::format("cut at {}: {}", cut, fault);
		}

		for (const auto o : offsets)
		{
			const auto size = big_endian ? libfiles::detail::load_be32(file.data() + o + 4) : libfiles::detail::load_le32(file.data() + o + 4);
			const std::uint32_t sizes[] = {0, 1, 3, 4, 7, size - 1, size + 1, size + 2, static_cast<std::uint32_t>(file.size()), 0x7FFFFFFF, 0xFFFFFFFE, 0xFFFFFFFF};

			for (const auto v : sizes)
			{
				auto copy = file;
				for (auto i = 0; i < 4; ++i) copy[o + 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(big_endian ? v >> (24 - 8 * i) : v >> (8 * i));

				// Data that gives no size runs to the end of the file, as FFmpeg reads it, where read_packet walks
				// the audio after its header as chunks.
				const auto unsized = v == 0 && std::string_view(reinterpret_cast<const char*>(file.data() + o), 4) == "data";
				if (const auto fault = fault_in(copy, riff_packet && !unsized); !fault.empty()) return std::format("size {} at {}: {}", v, o, fault);
			}
		}

		return {};
	}
}

namespace
{
	bytes video_strl(const std::string_view fourcc, const std::uint32_t width, const std::uint32_t height, const std::uint32_t frames,
	                 const std::vector<bytes>& more = {}, const bytes& extra = {})
	{
		std::vector<bytes> c = {chunk("strh", strh("vids", fourcc, 1, 25, frames)), chunk("strf", bitmap_info(width, height, fourcc, extra))};
		c.insert(c.end(), more.begin(), more.end());
		return list("strl", c);
	}

	// PCM in AVI, timed as a writer times it: a block a sample, the bytes' rate the stream's.
	bytes pcm_strl(const std::uint32_t seconds, const std::vector<bytes>& more = {})
	{
		std::vector<bytes> c = {chunk("strh", strh("auds", "", 4, 192000, 48000 * seconds, 4)), chunk("strf", wave_format(1, 2, 48000, 4, 192000, 16))};
		c.insert(c.end(), more.begin(), more.end());
		return list("strl", c);
	}

	// An AVI of an MPEG-4 video and PCM audio, ten seconds of each, with what the header and tags hold.
	bytes full_avi()
	{
		const auto hdrl = list("hdrl", {
			chunk("avih", avih(40000, 250, 2, 352, 288)),
			video_strl("XVID", 352, 288, 250, {chunk("strn", text("Main video")), chunk("vprp", vprp(16, 9, 352, 288, 2))}),
			pcm_strl(10, {chunk("strn", text("Commentary"))}),
			chunk("IDIT", text("Sun Mar 13 15:13:49 2011\n")),
		});

		const auto info = list("INFO", {chunk("INAM", text("A title")), chunk("IART", text("An artist")), chunk("ICMT", text("Caf\xE9"))});
		const auto late = list("INFO", {chunk("INAM", text("A later title")), chunk("IGNR", text("Documentary"))});
		const auto movi = list("movi", {chunk("00dc", mpeg4_headers), chunk("01wb", bytes(1920, 0))});
		return riff("AVI ", {hdrl, info, chunk("JUNK", bytes(64, 0)), movi, chunk("idx1", bytes(32, 0)), chunk("_PMX", to_bytes(packet_a)), late});
	}
}

TEST("media riff avi reads its header, streams and tags")
{
	const auto file = full_avi();
	const auto s = scan(file);
	CHECK(s.success);
	CHECK(s.format == libfiles::media_format::avi);
	CHECK_STR(s.brand, "AVI");
	CHECK(near(s.duration, 10.0));
	CHECK_EQ(s.streams.size(), std::size_t{2});
	if (s.streams.size() != 2) return;

	const auto& v = s.streams[0];
	CHECK(v.kind == media_stream_kind::video);
	CHECK_EQ(v.id, std::uint64_t{0});
	CHECK_STR(v.codec, "mpeg4");
	CHECK_STR(v.codec_tag, "XVID");
	CHECK_STR(v.title, "Main video");
	CHECK_EQ(v.video.width, 352u);
	CHECK_EQ(v.video.height, 288u);
	CHECK(near(v.video.frame_rate, 25.0));
	CHECK(near(v.duration, 10.0));
	// vprp's 16:9 frame at 352 x 288, which the VOL's square pixels do not override.
	CHECK_EQ(v.video.pixel_aspect_width, 16u);
	CHECK_EQ(v.video.pixel_aspect_height, 11u);
	CHECK(v.video.interlaced);
	CHECK(v.video.chroma == libfiles::chroma_format::yuv420);
	CHECK_EQ(v.video.bit_depth, 8);

	const auto& a = s.streams[1];
	CHECK(a.kind == media_stream_kind::audio);
	CHECK_EQ(a.id, std::uint64_t{1});
	CHECK_STR(a.codec, "pcm");
	CHECK_STR(a.title, "Commentary");
	CHECK_EQ(a.audio.sample_rate, 48000u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK_EQ(a.audio.bits_per_sample, 16);
	CHECK_EQ(a.bit_rate, std::uint64_t{1536000});
	CHECK(near(a.duration, 10.0));

	CHECK_STR(tag_value(s.tags, "INAM"), "A title");
	CHECK_STR(tag_value(s.tags, "IART"), "An artist");
	CHECK_STR(tag_value(s.tags, "ICMT"), "Caf\xC3\xA9");
	CHECK_STR(tag_value(s.tags, "IGNR"), "Documentary");
	CHECK_STR(tag_value(s.tags, "IDIT"), "Sun Mar 13 15:13:49 2011");
	CHECK_STR(s.properties.title, "A title");
	CHECK_STR(s.properties.genre, "Documentary");
	CHECK_EQ(s.properties.created.year, 2011);
	CHECK_EQ(s.properties.created.month, 3);
	CHECK_EQ(s.properties.created.day, 13);
	CHECK_EQ(s.properties.created.hour, 15);
	CHECK_EQ(s.properties.created.second, 49);
	CHECK(s.properties.created.zone == libfiles::media_date::time_zone::unstated);

	CHECK_STR(xmp_of(s), packet_a);
	CHECK_STR(xmp_of(s), packet_of(file));
	CHECK(s.complete);
}

TEST("media riff avi reads the first chunk of a stream whose bitstream describes it")
{
	// H.264 with its parameter sets in the stream, after an audio chunk and a dropped frame.
	const auto h264 = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 2, 320, 240)), video_strl("H264", 320, 240, 25), pcm_strl(1)}),
		list("movi", {chunk("01wb", bytes(400, 0)), chunk("00dc", {}), chunk("00dc", cat({annexb({h264_sps, h264_pps}), bytes(64, 0x55)}))}),
	});

	auto s = scan(h264);
	CHECK(s.success);
	CHECK_STR(first_stream(s).codec, "h264");
	CHECK_STR(first_stream(s).video.profile, "High");
	CHECK_EQ(first_stream(s).video.width, 320u);
	CHECK_EQ(first_stream(s).video.height, 240u);
	CHECK(first_stream(s).video.chroma == libfiles::chroma_format::yuv420);
	CHECK_EQ(first_stream(s).video.bit_depth, 8);

	// avc1 keeps its configuration after the BITMAPINFOHEADER, so its chunks need not be read.
	const auto avc1 = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 1, 320, 240)), video_strl("avc1", 320, 240, 25, {}, avc_config())}),
		list("movi", {chunk("00dc", bytes(64, 0))}),
	});

	s = scan(avc1);
	CHECK(s.success);
	CHECK_STR(first_stream(s).video.profile, "High");
	CHECK(first_stream(s).video.chroma == libfiles::chroma_format::yuv420);

	// Motion JPEG, inside a 'rec ' list: the frame's sampling, 4:2:2.
	const auto mjpeg = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 1, 640, 480)), video_strl("MJPG", 640, 480, 25)}),
		list("movi", {list("rec ", {chunk("00dc", jpeg_frame(640, 480, 2, 1))})}),
	});

	s = scan(mjpeg);
	CHECK(s.success);
	CHECK_STR(first_stream(s).codec, "mjpeg");
	CHECK(first_stream(s).video.chroma == libfiles::chroma_format::yuv422);
	CHECK_EQ(first_stream(s).video.width, 640u);
	CHECK(!first_stream(s).video.interlaced);

	// Interlaced Motion JPEG: each field a picture half the frame's height.
	const auto fields = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 1, 720, 576)), video_strl("MJPG", 720, 576, 25)}),
		list("movi", {chunk("00dc", cat({jpeg_frame(720, 288, 2, 1), jpeg_frame(720, 288, 2, 1)}))}),
	});

	s = scan(fields);
	CHECK(s.success);
	CHECK_EQ(first_stream(s).video.width, 720u);
	CHECK_EQ(first_stream(s).video.height, 576u);
	CHECK(first_stream(s).video.interlaced);

	// Type-1 DV: one stream of interleaved DV, its picture from the frame's header.
	const auto dv = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 1, 720, 576)),
		              list("strl", {chunk("strh", strh("iavs", "dvsd", 1, 25, 25)), chunk("strf", bytes(32, 0))})}),
		list("movi", {chunk("00dc", dv_frame())}),
	});

	s = scan(dv);
	CHECK(s.success);
	CHECK_STR(first_stream(s).codec, "dv");
	CHECK(first_stream(s).kind == media_stream_kind::video);
	CHECK_EQ(first_stream(s).video.width, 720u);
	CHECK_EQ(first_stream(s).video.height, 576u);
	CHECK(near(s.duration, 1.0));
}

TEST("media riff avi without a format keeps the stream header's frame or the main header's size")
{
	const auto file = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 50, 2, 640, 360)),
		              list("strl", {chunk("strh", strh("vids", "XVID", 1, 25, 50, 0, {0, 0, 320, 180}))}),
		              list("strl", {chunk("strh", strh("vids", "XVID", 0, 0, 50))})}),
		list("movi", {}),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), std::size_t{2});
	if (s.streams.size() != 2) return;
	CHECK_EQ(s.streams[0].video.width, 320u);
	CHECK_EQ(s.streams[0].video.height, 180u);
	CHECK_EQ(s.streams[1].video.width, 640u);
	CHECK_EQ(s.streams[1].video.height, 360u);
	// Without a rate the main header's frame period times the stream, as FFmpeg times it.
	CHECK(near(s.streams[1].video.frame_rate, 25.0));
	CHECK(near(s.streams[1].duration, 2.0));
}

TEST("media riff avi reads OpenDML's frame count and further forms")
{
	const auto hdrl = list("hdrl", {chunk("avih", avih(40000, 300, 1, 352, 288)), video_strl("XVID", 352, 288, 300),
	                                list("odml", {chunk("dmlh", builder().le32(500).zeros(244).b)})});
	const auto avix = riff("AVIX", {list("movi", {chunk("00dc", bytes(16, 1))})});

	auto file = cat({riff("AVI ", {hdrl, list("movi", {chunk("00dc", mpeg4_headers)}), chunk("idx1", bytes(16, 0)), chunk("_PMX", to_bytes(packet_a))}), avix});
	auto s = scan(file);
	CHECK(s.success);
	CHECK_STR(s.brand, "AVI OpenDML");
	// The stream header counts the first form's frames; dmlh counts all of them.
	CHECK(near(s.duration, 20.0));
	CHECK(near(first_stream(s).duration, 20.0));
	CHECK_STR(xmp_of(s), packet_a);
	CHECK_STR(xmp_of(s), packet_of(file));

	// A packet in a further form is read only when the first has none after its movie data.
	file = cat({riff("AVI ", {hdrl, list("movi", {chunk("00dc", mpeg4_headers)})}), riff("AVIX", {list("movi", {chunk("00dc", bytes(16, 1))}), chunk("_PMX", to_bytes(packet_b))})});
	s = scan(file);
	CHECK(s.success);
	CHECK_STR(xmp_of(s), packet_b);
	CHECK_STR(xmp_of(s), packet_of(file));

	// A further form cut short leaves the packet unread, as read_packet leaves it.
	file = cat({riff("AVI ", {hdrl, list("movi", {chunk("00dc", mpeg4_headers)}), chunk("_PMX", to_bytes(packet_a))}), avix});
	file.resize(file.size() - 6);
	s = scan(file);
	CHECK(s.success);
	CHECK_STR(s.brand, "AVI OpenDML");
	CHECK(s.xmp.empty());
	CHECK_STR(packet_of(file), "");
}

TEST("media riff avi reads Nikon's tags and a camera's EXIF")
{
	builder nikon;
	const auto entry = [&nikon](const unsigned tag, const std::string_view value)
	{
		nikon.le16(tag).le16(static_cast<unsigned>(value.size() + 1)).raw(value).u8(0);
	};
	entry(0x03, "NIKON");
	entry(0x04, "COOLPIX P7000");
	entry(0x05, "Ver.1.0");
	entry(0x13, "2011:03:13 15:13:49");

	// A little-endian IFD whose offsets count from its start: Make, Model and the Exif IFD with the original date.
	builder ifd;
	ifd.le16(3);
	ifd.le16(0x010F).le16(2).le32(7).le32(50);
	ifd.le16(0x0110).le16(2).le32(6).le32(58);
	ifd.le16(0x8769).le16(4).le32(1).le32(64);
	ifd.le32(0);
	ifd.zeros(50 - ifd.size()).raw(text("PENTAX")).u8(0).raw(text("Optio"));
	ifd.zeros(64 - ifd.size()).le16(1).le16(0x9003).le16(2).le32(20).le32(82).le32(0).raw(text("2010:01:02 03:04:05"));
	const auto avif = cat({to_bytes("AVIF"), bytes(4, 0), ifd.b});

	const auto file = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 1, 352, 288)), video_strl("XVID", 352, 288, 25, {chunk("strd", avif)})}),
		list("ncdt", {chunk("ncvr", bytes(10, 0)), chunk("nctg", nikon.b), chunk("ncth", bytes(33, 0xFF))}),
		list("movi", {chunk("00dc", mpeg4_headers)}),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK_STR(tag_value(s.tags, "Make"), "NIKON");
	CHECK_STR(tag_value(s.tags, "Model"), "COOLPIX P7000");
	CHECK_STR(tag_value(s.tags, "DateTimeOriginal"), "2011:03:13 15:13:49");
	CHECK_STR(tag_value(s.tags, "Software"), "");
	CHECK_STR(s.properties.camera_make, "NIKON");
	CHECK_STR(s.properties.camera_model, "COOLPIX P7000");
	CHECK_EQ(s.properties.created.year, 2011);
	CHECK_EQ(s.properties.created.minute, 13);

	const auto& tags = first_stream(s).tags;
	CHECK_STR(tag_value(tags, "Make"), "PENTAX");
	CHECK_STR(tag_value(tags, "Model"), "Optio");
	CHECK_STR(tag_value(tags, "DateTimeOriginal"), "2010:01:02 03:04:05");
}

TEST("media riff avi whose movie data gives no size reads to the end without walking it")
{
	// A capture that stopped before its sizes were written: RIFF and movi both zero.
	builder b;
	b.raw("RIFF").le32(0).raw("AVI ");
	b.raw(list("hdrl", {chunk("avih", avih(40000, 100, 1, 320, 240)), video_strl("MJPG", 320, 240, 100)}));
	b.raw("LIST").le32(0).raw("movi");
	for (auto i = 0; i < 100; ++i) b.raw(chunk("00dc", cat({jpeg_frame(320, 240, 2, 2), bytes(40000, 0x11)})));
	b.raw(chunk("_PMX", to_bytes(packet_a)));

	const auto s = scan(b.b);
	CHECK(s.success);
	CHECK(near(s.duration, 4.0));
	CHECK(first_stream(s).video.chroma == libfiles::chroma_format::yuv420);
	CHECK(s.xmp.empty());
	CHECK(reads_of(b.b) <= 1);
}

TEST("media riff avi costs a read for its header and one for its end")
{
	builder movi;
	movi.raw(chunk("00dc", cat({mpeg4_headers, bytes(20000, 0x22)})));
	for (auto i = 1; i < 400; ++i) movi.raw(chunk(i % 2 ? "01wb" : "00dc", bytes(i % 2 ? 7680 : 20000, 0x22)));

	const auto file = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 200, 2, 352, 288)), video_strl("XVID", 352, 288, 200), pcm_strl(8)}),
		list("INFO", {chunk("ISFT", text("Lavf"))}),
		chunk("LIST", cat({to_bytes("movi"), movi.b})),
		chunk("idx1", bytes(6400, 0)),
		chunk("_PMX", to_bytes(packet_a)),
		list("INFO", {chunk("IART", text("Read on the way to the end"))}),
	});

	CHECK(file.size() > 5u << 20);
	CHECK(reads_of(file) <= 2);
	const auto s = scan(file);
	CHECK_STR(xmp_of(s), packet_a);
	CHECK_STR(tag_value(s.tags, "IART"), "Read on the way to the end");
}

TEST("media riff avi numbers its streams as FFmpeg does, past a padding stream")
{
	// A 'pads' stream is no stream to FFmpeg, but movi still names the streams after it by their place.
	const auto file = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 3, 640, 480)),
		              video_strl("MJPG", 640, 480, 25, {chunk("inam", text("Lower-case name"))}),
		              list("strl", {chunk("strh", strh("pads", "", 1, 25, 0)), chunk("strf", bytes(8, 0))}),
		              pcm_strl(1, {chunk("isft", text("A writer"))})}),
		list("movi", {chunk("02wb", bytes(16, 0)), chunk("00dc", jpeg_frame(640, 480, 2, 2))}),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), std::size_t{2});
	if (s.streams.size() != 2) return;
	CHECK_EQ(s.streams[0].id, std::uint64_t{0});
	CHECK_EQ(s.streams[1].id, std::uint64_t{1});
	CHECK(s.streams[1].kind == media_stream_kind::audio);
	CHECK(s.streams[0].video.chroma == libfiles::chroma_format::yuv420);
	CHECK_STR(tag_value(s.streams[0].tags, "inam"), "Lower-case name");
	CHECK(s.streams[1].tags.empty());
}

TEST("media riff avi reads INFO on both sides of its movie data, the side before it first")
{
	// An INFO item of odd size without its pad puts the next a byte early.
	builder info;
	info.raw("INFO").raw("INAM").le32(5).raw("Title").raw("IART").le32(6).raw("Artist");
	const auto file = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 1, 352, 288)), video_strl("XVID", 352, 288, 25)}),
		chunk("LIST", info.b),
		list("movi", {chunk("00dc", mpeg4_headers)}),
		chunk("idx1", bytes(16, 0)),
		chunk("_PMX", to_bytes(packet_a)),
		// A field of its own, and one the list before the movie data gives too, which wins.
		list("INFO", {chunk("ICMT", text("After the movie")), chunk("INAM", text("Late title"))}),
	});

	auto s = scan(file);
	CHECK(s.success);
	CHECK_STR(tag_value(s.tags, "INAM"), "Title");
	CHECK_STR(tag_value(s.tags, "IART"), "Artist");
	CHECK_STR(tag_value(s.tags, "ICMT"), "After the movie");
	CHECK_EQ(std::ranges::count(s.tags, std::string("INAM"), &libfiles::media_tag::key), std::ptrdiff_t{1});
	CHECK_STR(s.properties.title, "Title");
	CHECK_STR(s.properties.artist, "Artist");
	CHECK(!s.properties.comments.empty() && s.properties.comments.front().text == "After the movie");
	CHECK_STR(xmp_of(s), packet_a);

	// Without a list before the movie data, the one after it gives every field, as in Byzantium.avi.
	const auto late = riff("AVI ", {
		list("hdrl", {chunk("avih", avih(40000, 25, 1, 352, 288)), video_strl("XVID", 352, 288, 25)}),
		list("movi", {chunk("00dc", mpeg4_headers)}),
		chunk("idx1", bytes(16, 0)),
		chunk("_PMX", to_bytes(packet_a)),
		list("INFO", {chunk("INAM", text("Byzantium"))}),
	});

	s = scan(late);
	CHECK(s.success);
	CHECK_STR(tag_value(s.tags, "INAM"), "Byzantium");
	CHECK_STR(s.properties.title, "Byzantium");
}

TEST("media riff avi shows an edit to its INFO wherever its lists lie")
{
	const auto hdrl = list("hdrl", {chunk("avih", avih(40000, 25, 1, 352, 288)), video_strl("XVID", 352, 288, 25)});
	const auto movi = list("movi", {chunk("00dc", mpeg4_headers)});
	const auto late = list("INFO", {chunk("INAM", text("Late")), chunk("IART", text("Someone"))});

	libfiles::metadata_update u;
	u.legacy.title = "A new title, longer than the old one";

	// The list before the movie data is edited, in the JUNK after it, and is read over the one after.
	auto s = scan(edited(riff("AVI ", {hdrl, list("INFO", {chunk("INAM", text("Old"))}), chunk("JUNK", bytes(200, 0)), movi, chunk("idx1", bytes(16, 0)), late}), u));
	CHECK(s.success);
	CHECK_STR(s.properties.title, "A new title, longer than the old one");
	CHECK_STR(tag_value(s.tags, "INAM"), "A new title, longer than the old one");
	CHECK_STR(s.properties.artist, "Someone");

	// With no room before the movie data the edited list goes to the end of the form, after the other.
	s = scan(edited(riff("AVI ", {hdrl, list("INFO", {chunk("INAM", text("Old"))}), movi, chunk("idx1", bytes(16, 0)), late}), u));
	CHECK(s.success);
	CHECK_STR(s.properties.title, "A new title, longer than the old one");
	CHECK_STR(s.properties.artist, "Someone");

	// The only list, after the movie data, is the one edited.
	s = scan(edited(riff("AVI ", {hdrl, movi, chunk("idx1", bytes(16, 0)), late}), u));
	CHECK(s.success);
	CHECK_STR(s.properties.title, "A new title, longer than the old one");
	CHECK_STR(s.properties.artist, "Someone");
}

TEST("media riff avi walks further forms by their headers alone")
{
	const auto hdrl = list("hdrl", {chunk("avih", avih(40000, 100, 1, 352, 288)), video_strl("XVID", 352, 288, 100),
	                                list("odml", {chunk("dmlh", builder().le32(800).zeros(244).b)})});

	auto file = riff("AVI ", {hdrl, list("movi", {chunk("00dc", cat({mpeg4_headers, bytes(300000, 1)}))}), chunk("idx1", bytes(16, 0))});
	for (auto i = 0; i < 7; ++i)
	{
		const auto avix = riff("AVIX", {list("movi", {chunk("00dc", bytes(300000, 2)), chunk("ix00", bytes(32, 0))})});
		file.insert(file.end(), avix.begin(), avix.end());
	}

	const auto s = scan(file);
	CHECK(s.success);
	CHECK_STR(s.brand, "AVI OpenDML");
	CHECK(near(s.duration, 32.0));
	// A read for the header and the first form's end, and one for each further form, as read_packet costs:
	// the packet is read only from a file whose every form is whole.
	CHECK(reads_of(file) <= 1 + 7);
}
TEST("media riff avi refuses a file without a header or streams")
{
	auto s = scan(riff("AVI ", {list("movi", {chunk("00dc", bytes(8, 0))})}));
	CHECK(!s.success);
	CHECK(!s.error.empty());

	s = scan(riff("AVI ", {list("hdrl", {chunk("avih", avih(40000, 1, 0, 1, 1))}), list("movi", {})}));
	CHECK(!s.success);
	CHECK(!s.error.empty());
}

TEST("media riff avi lists its forms, chunks and streams to inspect")
{
	const auto s = scan(full_avi(), libfiles::scan_intent::inspect);
	CHECK(s.success);

	std::set<std::string> keys;
	for (const auto& row : s.structure) keys.insert(row.key);
	CHECK(keys.contains("Summary (8)"));
	CHECK(keys.contains("Streams (2)"));
	CHECK(keys.contains("LIST movi"));
	CHECK(keys.contains("_PMX"));
	CHECK(keys.contains("Stream 1"));
	CHECK(scan(full_avi()).structure.empty());
}

TEST("media riff avi survives being cut short and having its sizes corrupted")
{
	CHECK_STR(damage(full_avi(), false), "");

	const auto hdrl = list("hdrl", {chunk("avih", avih(40000, 300, 1, 352, 288)), video_strl("MJPG", 352, 288, 300),
	                                list("odml", {chunk("dmlh", builder().le32(500).zeros(4).b)})});
	const auto open_dml = cat({riff("AVI ", {hdrl, list("movi", {chunk("00dc", jpeg_frame(352, 288, 2, 2))}), chunk("_PMX", to_bytes(packet_a))}),
	                           riff("AVIX", {list("movi", {chunk("00dc", bytes(16, 1))}), chunk("_PMX", to_bytes(packet_b))})});
	CHECK_STR(damage(open_dml, false), "");
}

namespace
{
	bytes wav(const std::vector<bytes>& chunks)
	{
		return riff("WAVE", chunks);
	}

	void put_le64(bytes& b, const std::size_t at, const std::uint64_t v)
	{
		for (std::size_t i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
	}

	// An RF64 or BW64 file that leaves the form's and the data's sizes to its ds64 chunk.
	bytes wav64(const std::string_view id, const bytes& format, const bytes& data, const std::uint64_t samples, const std::vector<bytes>& after = {})
	{
		builder body;
		body.raw("WAVE").raw("ds64").le32(28).zeros(28).raw(chunk("fmt ", format)).raw("data").le32(0xFFFFFFFF).raw(data);
		if (data.size() & 1) body.u8(0);
		for (const auto& a : after) body.raw(a);

		put_le64(body.b, 12, body.size());
		put_le64(body.b, 20, data.size());
		put_le64(body.b, 28, samples);
		return builder().raw(id).le32(0xFFFFFFFF).raw(body.b).b;
	}

	// MPEG-1 Layer 3 in WAVE, its MPEGLAYER3WAVEFORMAT's twelve bytes after WAVEFORMATEX.
	bytes mp3_format(const std::uint32_t average)
	{
		return wave_format(0x55, 2, 44100, 1, average, 0, builder().le16(1).le32(2).le16(1).le16(1).le16(0).b);
	}

	bytes bext()
	{
		return builder().raw("A description").zeros(256 - 13).raw("Originator").zeros(32 - 10).raw("REF-1").zeros(32 - 5).raw("2011-03-13")
		                .raw("15:13:49").le32(0).le32(0).le16(1).zeros(64 + 10 + 180).raw("A=PCM,F=48000,W=16\r\n").b;
	}

	bytes tagged_wav()
	{
		return wav({
			chunk("fmt ", pcm_format(2, 48000, 16)),
			chunk("bext", bext()),
			list("INFO", {chunk("INAM", text("Info title")), chunk("ICMT", text("A comment"))}),
			chunk("DISP", cat({builder().le32(1).b, text("Display title")})),
			chunk("DISP", cat({builder().le32(8).b, bytes(40, 0)})),
			chunk("iXML", text("<BWFXML><PROJECT>P</PROJECT></BWFXML>")),
			chunk("data", bytes(19200, 0)),
			chunk("id3 ", id3_tag("TIT2", "ID3 title")),
			chunk("_PMX", to_bytes(packet_a)),
		});
	}
}

TEST("media riff wav reads pcm at every depth")
{
	struct depth
	{
		unsigned bits;
		unsigned tag;
		bool float_samples;
	};

	for (const auto d : {depth{8, 1, false}, depth{16, 1, false}, depth{24, 1, false}, depth{32, 1, false}, depth{32, 3, true}, depth{64, 3, true}})
	{
		const auto align = 2 * ((d.bits + 7) / 8);
		// Half a second at 8 kHz. Samples of one size are timed by the data, so a wrong fact count is no matter.
		const auto file = wav({chunk("fmt ", pcm_format(2, 8000, d.bits, d.tag)), chunk("fact", builder().le32(1).b), chunk("data", bytes(4000 * align, 0))});
		const auto s = scan(file);
		CHECK(s.success);
		CHECK_STR(s.brand, "WAVE");
		CHECK_EQ(s.streams.size(), std::size_t{1});
		if (s.streams.size() != 1) continue;

		const auto& a = s.streams[0];
		CHECK_EQ(a.id, std::uint64_t{0});
		CHECK(a.kind == media_stream_kind::audio);
		CHECK_STR(a.codec, "pcm");
		CHECK_EQ(a.audio.bits_per_sample, static_cast<int>(d.bits));
		CHECK(a.audio.float_samples == d.float_samples);
		CHECK_EQ(a.audio.sample_rate, 8000u);
		CHECK_EQ(a.audio.channels, 2u);
		CHECK(near(a.duration, 0.5));
		CHECK(near(s.duration, 0.5));
		CHECK_EQ(a.bit_rate, std::uint64_t{8000} * align * 8);
		CHECK(s.complete);
	}
}

TEST("media riff wav reads an extensible format by its subtype")
{
	// 24-bit samples in 32-bit containers, a quarter of a second.
	auto s = scan(wav({chunk("fmt ", extensible_format(2, 48000, 32, 24, 1)), chunk("data", bytes(48000 * 8 / 4, 0))}));
	CHECK(s.success);
	CHECK_STR(first_stream(s).codec, "pcm");
	CHECK_EQ(first_stream(s).audio.bits_per_sample, 24);
	CHECK(!first_stream(s).audio.float_samples);
	CHECK(near(s.duration, 0.25));
	CHECK_EQ(first_stream(s).bit_rate, std::uint64_t{48000} * 8 * 8);

	s = scan(wav({chunk("fmt ", extensible_format(2, 48000, 32, 32, 3)), chunk("data", bytes(48000 * 8, 0))}));
	CHECK(s.success);
	CHECK_STR(first_stream(s).codec, "pcm");
	CHECK(first_stream(s).audio.float_samples);
	CHECK(near(s.duration, 1.0));
}

TEST("media riff wav times a compressed format by its fact chunk")
{
	// IMA ADPCM, four bits a sample in blocks of 1017: a second of mono at 22050 Hz.
	auto s = scan(wav({chunk("fmt ", wave_format(0x11, 1, 22050, 512, 11100, 4, builder().le16(1017).b)), chunk("fact", builder().le32(22050).b),
	                   chunk("data", bytes(11264, 0))}));
	CHECK(s.success);
	CHECK(!first_stream(s).codec.empty());
	CHECK(near(s.duration, 1.0));
	CHECK_EQ(first_stream(s).bit_rate, std::uint64_t{88800});

	// MP3 whose fact counts two seconds, where its average rate would make its data 1.6.
	s = scan(wav({chunk("fmt ", mp3_format(20000)), chunk("fact", builder().le32(88200).b), chunk("data", bytes(32000, 0))}));
	CHECK(s.success);
	CHECK_STR(first_stream(s).codec, "mp3");
	CHECK_EQ(first_stream(s).audio.bits_per_sample, 0);
	CHECK(near(s.duration, 2.0));

	s = scan(wav({chunk("fmt ", mp3_format(20000)), chunk("data", bytes(32000, 0))}));
	CHECK(s.success);
	CHECK(near(s.duration, 1.6));
}

TEST("media riff wav reads RF64 and BW64 sizes from ds64")
{
	auto file = wav64("RF64", pcm_format(2, 44100, 16), bytes(44100 * 2, 0), 22050, {list("INFO", {chunk("INAM", text("Long"))}), chunk("_PMX", to_bytes(packet_a))});
	auto s = scan(file);
	CHECK(s.success);
	CHECK_STR(s.brand, "RF64");
	CHECK(near(s.duration, 0.5));
	CHECK_STR(tag_value(s.tags, "INAM"), "Long");
	// read_packet reads RIFF forms only.
	CHECK(s.xmp.empty());
	CHECK_STR(packet_of(file), "");

	// Compressed, it is timed by ds64's count of samples.
	file = wav64("BW64", mp3_format(16000), bytes(16000, 0), 88200);
	s = scan(file);
	CHECK(s.success);
	CHECK_STR(s.brand, "BW64");
	CHECK(near(s.duration, 2.0));
}

TEST("media riff wav reads bext, DISP, iXML and an ID3 chunk")
{
	const auto file = tagged_wav();
	const auto s = scan(file);
	CHECK(s.success);
	CHECK(near(s.duration, 0.1));
	CHECK_STR(tag_value(s.tags, "Description"), "A description");
	CHECK_STR(tag_value(s.tags, "Originator"), "Originator");
	CHECK_STR(tag_value(s.tags, "OriginatorReference"), "REF-1");
	CHECK_STR(tag_value(s.tags, "OriginationDate"), "2011-03-13");
	CHECK_STR(tag_value(s.tags, "OriginationTime"), "15:13:49");
	CHECK_STR(tag_value(s.tags, "CodingHistory"), "A=PCM,F=48000,W=16");
	CHECK_STR(tag_value(s.tags, "INAM"), "Info title");
	CHECK_STR(tag_value(s.tags, "DISP"), "Display title");
	CHECK_STR(tag_value(s.tags, "iXML"), "<BWFXML><PROJECT>P</PROJECT></BWFXML>");
	CHECK_STR(tag_value(s.tags, "TIT2"), "ID3 title");

	// The toolkit reads DISP's title over the others.
	CHECK_STR(s.properties.title, "Display title");
	CHECK_EQ(s.properties.created.year, 2011);
	CHECK_EQ(s.properties.created.month, 3);
	CHECK_EQ(s.properties.created.day, 13);
	CHECK(s.properties.created.has_time);
	CHECK_EQ(s.properties.created.hour, 15);
	CHECK_EQ(s.properties.created.minute, 13);
	CHECK_EQ(s.properties.created.second, 49);
	CHECK(s.properties.created.zone == libfiles::media_date::time_zone::unstated);

	CHECK_STR(xmp_of(s), packet_a);
	CHECK_STR(xmp_of(s), packet_of(file));
}

TEST("media riff wav takes its last packet")
{
	const auto file = wav({chunk("fmt ", pcm_format(1, 8000, 8)), chunk("_PMX", to_bytes(packet_a)), chunk("data", bytes(800, 0)), chunk("_PMX", to_bytes(packet_b))});
	const auto s = scan(file);
	CHECK_STR(xmp_of(s), packet_b);
	CHECK_STR(xmp_of(s), packet_of(file));
}

TEST("media riff wav reads an INFO list named in lower case, as FFmpeg does")
{
	const auto s = scan(wav({chunk("fmt ", pcm_format(1, 8000, 8)), chunk("data", bytes(800, 0)), chunk("list", with_type("INFO", {chunk("IART", text("Someone"))}))}));
	CHECK(s.success);
	CHECK_STR(tag_value(s.tags, "IART"), "Someone");
}

TEST("media riff wav reads data that gives no size, or more than the file holds, to the end of the file")
{
	// A stream that was never finished: RIFF and data sizes unwritten, a second of audio after.
	builder streamed;
	streamed.raw("RIFF").le32(0xFFFFFFFF).raw("WAVE").raw(chunk("fmt ", pcm_format(1, 8000, 16))).raw("data").le32(0).raw(bytes(16000, 0x41));
	auto s = scan(streamed.b);
	CHECK(s.success);
	CHECK(near(s.duration, 1.0));
	CHECK(s.xmp.empty());

	// A capture cut short: its data says two seconds, the file holds one.
	auto cut = wav({chunk("fmt ", pcm_format(1, 8000, 16)), chunk("data", bytes(32000, 0)), chunk("_PMX", to_bytes(packet_a))});
	cut.resize(44 + 16000);
	s = scan(cut);
	CHECK(s.success);
	CHECK(near(s.duration, 1.0));
	CHECK(s.xmp.empty());
}

TEST("media riff wav costs a read for its header and one for what follows its data")
{
	const auto file = wav({chunk("fmt ", pcm_format(2, 48000, 16)), chunk("data", bytes(8u << 20, 0)), list("INFO", {chunk("INAM", text("Title"))}),
	                       chunk("_PMX", to_bytes(packet_a))});
	CHECK(reads_of(file) <= 2);
	CHECK_STR(xmp_of(scan(file)), packet_a);
}

TEST("media riff wav refuses a file without a whole format")
{
	auto s = scan(wav({chunk("data", bytes(100, 0))}));
	CHECK(!s.success);
	CHECK(!s.error.empty());

	s = scan(wav({chunk("fmt ", bytes(6, 1)), chunk("data", bytes(100, 0))}));
	CHECK(!s.success);
}

TEST("media riff wav lists its chunks to inspect")
{
	const auto s = scan(tagged_wav(), libfiles::scan_intent::inspect);
	CHECK(s.success);

	std::set<std::string> keys;
	for (const auto& row : s.structure) keys.insert(row.key);
	CHECK(keys.contains("RIFF WAVE"));
	CHECK(keys.contains("fmt "));
	CHECK(keys.contains("bext"));
	CHECK(keys.contains("Sample rate"));
	CHECK(keys.contains("Chunks (10)"));
}

TEST("media riff wav survives being cut short and having its sizes corrupted")
{
	CHECK_STR(damage(tagged_wav(), false), "");
	CHECK_STR(damage(wav({chunk("fmt ", mp3_format(20000)), chunk("fact", builder().le32(88200).b), chunk("data", bytes(3001, 0)), chunk("_PMX", to_bytes(packet_b))}), false), "");
	CHECK_STR(damage(wav64("RF64", pcm_format(2, 44100, 16), bytes(4410, 0), 1102, {list("INFO", {chunk("INAM", text("Long"))}), chunk("_PMX", to_bytes(packet_a))}), false), "");
}

// AIFF

TEST("media riff aiff reads its COMM chunk, text and packet")
{
	const auto file = aiff("AIFF", {
		iff_chunk("COMM", comm(2, 22050, 16, 44100)),
		iff_chunk("NAME", to_bytes("A name")),
		iff_chunk("AUTH", to_bytes("An author")),
		iff_chunk("(c) ", to_bytes("2011 Someone")),
		iff_chunk("ANNO", to_bytes("A note")),
		iff_chunk("APPL", cat({to_bytes("XMP "), to_bytes(packet_a)})),
		iff_chunk("APPL", cat({to_bytes("stoc"), bytes(12, 0)})),
		iff_chunk("SSND", cat({builder().be32(0).be32(0).b, bytes(22050 * 4, 0)})),
		iff_chunk("ID3 ", id3_tag("TIT2", "ID3 title")),
		iff_chunk("APPL", cat({to_bytes("XMP "), to_bytes(packet_b), bytes(3, 0)})),
	});

	const auto s = scan(file);
	CHECK(s.success);
	CHECK(s.format == libfiles::media_format::aiff);
	CHECK_STR(s.brand, "AIFF");
	CHECK(near(s.duration, 0.5));
	CHECK_EQ(s.streams.size(), std::size_t{1});
	if (s.streams.size() != 1) return;

	const auto& a = s.streams[0];
	CHECK_EQ(a.id, std::uint64_t{0});
	CHECK_STR(a.codec, "pcm");
	CHECK_STR(a.codec_tag, "");
	CHECK_EQ(a.audio.bits_per_sample, 16);
	CHECK(!a.audio.float_samples);
	CHECK_EQ(a.audio.sample_rate, 44100u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK_EQ(a.bit_rate, std::uint64_t{1411200});
	CHECK(near(a.duration, 0.5));
	CHECK(s.complete);

	CHECK_STR(tag_value(s.tags, "NAME"), "A name");
	CHECK_STR(tag_value(s.tags, "AUTH"), "An author");
	CHECK_STR(tag_value(s.tags, "(c) "), "2011 Someone");
	CHECK_STR(tag_value(s.tags, "ANNO"), "A note");
	CHECK_STR(tag_value(s.tags, "TIT2"), "ID3 title");
	// The ID3 chunk follows NAME, and its title is the one FFmpeg leaves.
	CHECK_STR(s.properties.title, "ID3 title");

	// The toolkit reads the last packet.
	CHECK_STR(xmp_of(s), packet_b);
}

TEST("media riff aifc names its compression")
{
	struct variant
	{
		std::string_view type;
		unsigned sample_size;
		std::string codec;
		int bits;
		bool float_samples;
		double duration;
	};

	const variant variants[] = {
		{"NONE", 16, "pcm", 16, false, 1.0},
		{"twos", 16, "pcm", 16, false, 1.0},
		{"sowt", 24, "pcm", 24, false, 1.0},
		{"raw ", 8, "pcm", 8, false, 1.0},
		{"in24", 24, "pcm", 24, false, 1.0},
		{"in32", 32, "pcm", 32, false, 1.0},
		{"fl32", 32, "pcm", 32, true, 1.0},
		{"FL32", 32, "pcm", 32, true, 1.0},
		{"fl64", 64, "pcm", 64, true, 1.0},
		{"FL64", 64, "pcm", 64, true, 1.0},
		{"ulaw", 16, std::string(libfiles::detail::codec_from_wave_tag(0x0007)), 0, false, 1.0},
		{"alaw", 16, std::string(libfiles::detail::codec_from_wave_tag(0x0006)), 0, false, 1.0},
		// numSampleFrames counts packets, of 64 frames for IMA4, 6 for MACE and 160 for GSM.
		{"ima4", 16, "adpcm_ima_qt", 0, false, 64.0},
		{"MAC3", 8, "mace3", 0, false, 6.0},
		{"MAC6", 8, "mace6", 0, false, 6.0},
		{"GSM ", 16, "gsm", 0, false, 160.0},
		// QDesign's packets are described in a chunk this does not read.
		{"QDM2", 16, "qdm2", 0, false, 0.0},
	};

	for (const auto& v : variants)
	{
		const auto file = aiff("AIFC", {
			iff_chunk("FVER", builder().be32(0xA2805140).b),
			iff_chunk("COMM", comm(1, 8000, v.sample_size, 8000, v.type, "a codec")),
			iff_chunk("SSND", cat({builder().be32(0).be32(0).b, bytes(64, 0)})),
		});

		const auto s = scan(file);
		CHECK(s.success);
		CHECK_STR(s.brand, "AIFC");
		if (s.streams.size() != 1) continue;

		const auto& a = s.streams[0];
		CHECK_STR(a.codec, v.codec);
		CHECK_STR(a.codec_tag, v.type);
		CHECK_EQ(a.audio.bits_per_sample, v.bits);
		CHECK(a.audio.float_samples == v.float_samples);
		CHECK_EQ(a.audio.sample_rate, 8000u);
		CHECK_EQ(a.audio.channels, 1u);
		CHECK(near(a.duration, v.duration));
	}
}

TEST("media riff aiff reads every common sample rate")
{
	for (const auto rate : {8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0})
	{
		const auto s = scan(aiff("AIFF", {iff_chunk("COMM", comm(2, 1000, 16, rate)), iff_chunk("SSND", bytes(8, 0))}));
		CHECK(s.success);
		CHECK_EQ(first_stream(s).audio.sample_rate, static_cast<std::uint32_t>(rate));
	}

	// The old Macintosh rate, 22254.5454... Hz, rounds as FFmpeg rounds it.
	auto s = scan(aiff("AIFF", {iff_chunk("COMM", comm(1, 1000, 8, 22254.545454545454))}));
	CHECK(s.success);
	CHECK_EQ(first_stream(s).audio.sample_rate, 22255u);

	// A negative rate is none.
	auto negative = aiff("AIFF", {iff_chunk("COMM", comm(1, 1000, 8, 44100))});
	negative[12 + 8 + 8] |= 0x80;
	s = scan(negative);
	CHECK(s.success);
	CHECK_EQ(first_stream(s).audio.sample_rate, 0u);
	CHECK(!s.complete);
}

TEST("media riff aiff refuses a file without a whole COMM chunk")
{
	auto s = scan(aiff("AIFF", {iff_chunk("SSND", bytes(64, 0))}));
	CHECK(!s.success);
	CHECK(!s.error.empty());

	s = scan(aiff("AIFF", {iff_chunk("COMM", bytes(10, 0)), iff_chunk("SSND", bytes(64, 0))}));
	CHECK(!s.success);
}

TEST("media riff aiff lists its chunks to inspect")
{
	const auto s = scan(aiff("AIFC", {iff_chunk("COMM", comm(2, 100, 16, 48000, "sowt", "little endian")), iff_chunk("SSND", cat({builder().be32(0).be32(0).b, bytes(400, 0)}))}),
	                    libfiles::scan_intent::inspect);
	CHECK(s.success);

	std::string compression;
	std::set<std::string> keys;

	for (const auto& row : s.structure)
	{
		keys.insert(row.key);
		if (row.key == "Compression") compression = row.value;
	}

	CHECK_STR(compression, "sowt, little endian");
	CHECK(keys.contains("COMM"));
	CHECK(keys.contains("SSND"));
	CHECK(keys.contains("Chunks (2)"));
}

TEST("media riff aiff survives being cut short and having its sizes corrupted")
{
	const auto file = aiff("AIFC", {
		iff_chunk("FVER", builder().be32(0xA2805140).b),
		iff_chunk("COMM", comm(2, 100, 16, 44100, "NONE", "not compressed")),
		iff_chunk("NAME", to_bytes("odd")),
		iff_chunk("ID3 ", id3_tag("TIT2", "ID3 title")),
		iff_chunk("SSND", cat({builder().be32(0).be32(0).b, bytes(400, 0)})),
		iff_chunk("APPL", cat({to_bytes("XMP "), to_bytes(packet_a)})),
	});

	CHECK_STR(damage(file, true, false), "");
}

TEST("media riff files read back the edits they keep outside the packet")
{
	// An AVI's IDIT keeps the date taken, which its scan reads over INFO's creation date, as a packet's Date
	// Created stands over its CreateDate. A Broadcast WAV's bext keeps when it was recorded, which its scan
	// reads over INFO's and an edit leaves as it is: the edited date reads from the packet.
	check_round_trip("avi", full_avi(), {"artist", "comment", "copyright", "genre", "taken", "title", "year"});
	check_round_trip("tagged wav", tagged_wav(), {"artist", "comment", "copyright", "genre", "title", "year"});
}
