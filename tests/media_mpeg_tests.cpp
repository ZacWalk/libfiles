// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the MPEG transport and program stream media scans. Each stream is built here packet
// by packet - the tables with their CRCs, PES packets with their timestamps, the codec headers bit by
// bit - so every field a test reads back is one it put there; each is also cut short and damaged.

#include "test.h"
#include "builders.h"

#include "libfiles/media.h"
#include "media_mpeg_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>
#include <span>

namespace
{
	using namespace test_build;
	using libfiles::media_format;
	using libfiles::media_scan_result;
	using libfiles::media_stream_kind;

	template <typename... Parts>
	bytes cat(const Parts&... parts)
	{
		bytes out;
		(out.insert(out.end(), parts.begin(), parts.end()), ...);
		return out;
	}

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	media_scan_result scan(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index)
	{
		libfiles::reader r(file);
		return libfiles::scan_media(r, intent);
	}

	bool has_row(const media_scan_result& r, const std::string_view key)
	{
		return std::ranges::any_of(r.structure, [key](const libfiles::structure_row& row) { return row.key.starts_with(key); });
	}

	// ---- Bits and codec headers ----------------------------------------------------------------------

	// Bits most significant first, as video headers hold them.
	struct msb_bits
	{
		bytes b;
		std::size_t count = 0;

		msb_bits& put(const std::uint64_t value, const unsigned bits)
		{
			for (auto i = bits; i-- > 0;)
			{
				if (count % 8 == 0) b.push_back(0);
				if ((value >> i) & 1) b.back() = static_cast<std::uint8_t>(b.back() | (0x80 >> (count % 8)));
				++count;
			}

			return *this;
		}

		msb_bits& ue(const std::uint32_t value)
		{
			const auto coded = static_cast<std::uint64_t>(value) + 1;
			const auto width = static_cast<unsigned>(std::bit_width(coded));
			return put(0, width - 1).put(coded, width);
		}

		msb_bits& align()
		{
			while (count % 8) put(0, 1);
			return *this;
		}

		// rbsp_trailing_bits.
		msb_bits& trailing()
		{
			put(1, 1);
			return align();
		}
	};

	// A NAL unit after a four-byte start code, with emulation prevention bytes put in.
	bytes annexb(const bytes& nal)
	{
		bytes out = {0, 0, 0, 1};
		auto zeros = 0;

		for (const auto v : nal)
		{
			if (zeros >= 2 && v <= 3)
			{
				out.push_back(3);
				zeros = 0;
			}

			out.push_back(v);
			zeros = v == 0 ? zeros + 1 : 0;
		}

		return out;
	}

	// An H.264 High profile SPS: 4:2:0 at 8 bits, cropped from whole macroblocks, square pixels, BT.709
	// colour in limited range, and a fixed frame rate.
	bytes h264_sps(const unsigned width, const unsigned height, const unsigned fps)
	{
		const auto columns = (width + 15) / 16;
		const auto rows = (height + 15) / 16;
		const auto crop_right = (columns * 16 - width) / 2;
		const auto crop_bottom = (rows * 16 - height) / 2;

		msb_bits s;
		s.put(0x67, 8).put(100, 8).put(0, 8).put(40, 8);
		s.ue(0).ue(1).ue(0).ue(0).put(0, 1).put(0, 1);
		s.ue(0).ue(2).ue(1).put(0, 1);
		s.ue(columns - 1).ue(rows - 1).put(1, 1).put(1, 1);
		s.put(crop_right != 0 || crop_bottom != 0, 1);
		if (crop_right != 0 || crop_bottom != 0) s.ue(0).ue(crop_right).ue(0).ue(crop_bottom);

		// VUI: aspect, video signal type and colour, timing.
		s.put(1, 1).put(1, 1).put(1, 8).put(0, 1);
		s.put(1, 1).put(5, 3).put(0, 1).put(1, 1).put(1, 8).put(1, 8).put(1, 8);
		s.put(0, 1);
		s.put(1, 1).put(1, 32).put(fps * 2, 32).put(1, 1);
		s.put(0, 1).put(0, 1).put(0, 1).put(0, 1);
		s.trailing();
		return s.b;
	}

	// An AVCHD camcorder's SEI: user data with the MDPM UUID and the packs given, five bytes each.
	bytes mdpm_sei(const bytes& packs)
	{
		builder payload;
		payload.raw(bytes{0x17, 0xEE, 0x8C, 0x60, 0xF8, 0x4D, 0x11, 0xD9, 0x8C, 0xD6, 0x08, 0x00, 0x20, 0x0C, 0x9A, 0x66});
		payload.raw("MDPM").u8(static_cast<unsigned>(packs.size() / 5)).raw(packs);
		return builder().u8(0x06).u8(5).u8(static_cast<unsigned>(payload.size())).raw(payload.b).u8(0x80).b;
	}

	// Packs 0x18 and 0x19, a recording time, and 0xE0, Sony's maker code, with 0xE4 to 0xE6, its model.
	bytes sony_packs(const std::uint8_t zone)
	{
		return builder()
		       .u8(0x18).u8(zone).u8(0x20).u8(0x11).u8(0x07)
		       .u8(0x19).u8(0x04).u8(0x14).u8(0x37).u8(0x28)
		       .u8(0xE0).u8(0x01).u8(0x08).u8(0x30).u8(0x01)
		       .u8(0xE4).raw("HDR-")
		       .u8(0xE5).raw("CX55")
		       .u8(0xE6).raw("0V").u8(0).u8(0).b;
	}

	// The first access unit of an H.264 stream: a delimiter, the parameter sets, any SEI and an IDR slice.
	bytes h264_first_unit(const bytes& sps, const bytes& sei = {})
	{
		auto unit = cat(annexb({0x09, 0x10}), annexb(sps), annexb({0x68, 0xCE, 0x38, 0x80}));
		if (!sei.empty()) unit = cat(unit, annexb(sei));
		return cat(unit, annexb({0x65, 0x88, 0x84, 0x00, 0x33, 0xFF, 0x10, 0x20}));
	}

	bytes h264_unit()
	{
		return cat(annexb({0x09, 0x30}), annexb({0x41, 0x9A, 0x02, 0x04, 0x08}));
	}

	// An HEVC Main profile SPS: 4:2:0 at 8 bits, no VUI.
	bytes hevc_sps(const unsigned width, const unsigned height)
	{
		msb_bits s;
		s.put(0x4201, 16).put(0, 4).put(0, 3).put(1, 1);
		s.put(0, 2).put(0, 1).put(1, 5).put(0x60000000, 32).put(1, 1).put(0, 1).put(0, 1).put(1, 1).put(0, 43).put(0, 1).put(93, 8);
		s.ue(0).ue(1).ue(width).ue(height).put(0, 1);
		s.ue(0).ue(0).ue(4).put(1, 1).ue(4).ue(2).ue(0);
		s.ue(0).ue(3).ue(0).ue(3).ue(0).ue(0);
		s.put(0, 1).put(1, 1).put(1, 1).put(0, 1);
		s.ue(0).put(0, 1).put(1, 1).put(1, 1);
		s.put(0, 1).put(0, 1);
		s.trailing();
		return s.b;
	}

	bytes hevc_first_unit(const bytes& sps)
	{
		return cat(annexb({0x46, 0x01, 0x10}), annexb({0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF}), annexb(sps),
		           annexb({0x44, 0x01, 0xC1, 0x72, 0xB4}), annexb({0x26, 0x01, 0xAF, 0x13, 0x40}));
	}

	// An MPEG-1 or MPEG-2 sequence header for square pixels - MPEG-2 with its sequence extension, Main
	// profile at Main level, progressive 4:2:0 - then a GOP, an I picture and a slice.
	bytes mpeg_video_sequence(const unsigned width, const unsigned height, const unsigned rate_code, const bool mpeg2)
	{
		msb_bits s;
		s.put(0x1B3, 32).put(width, 12).put(height, 12).put(1, 4).put(rate_code, 4).put(0x3FFFF, 18).put(1, 1).put(112, 10);
		s.put(0, 1).put(0, 1).put(0, 1);
		if (mpeg2) s.put(0x1B5, 32).put(1, 4).put(0x48, 8).put(1, 1).put(1, 2).put(0, 4).put(0, 12).put(1, 1).put(0, 8).put(0, 8);
		s.put(0x1B8, 32).put(0, 25).put(1, 1).put(0, 1).align();
		s.put(0x100, 32).put(0, 10).put(1, 3).put(0xFFFF, 16).put(0, 1).align();
		s.put(0x101, 32).put(0x2A, 8).put(0x55, 8);
		return s.b;
	}

	// A VC-1 Advanced profile sequence header with no display extension, its entry point and a frame
	// start code, as a Blu-ray or EVOB stream's first access unit begins.
	bytes vc1_sequence(const unsigned width, const unsigned height, const bool interlace)
	{
		msb_bits s;
		// PROFILE, LEVEL, COLORDIFF_FORMAT, FRMRTQ_POSTPROC, BITRTQ_POSTPROC and POSTPROCFLAG; the coded size;
		// PULLDOWN, INTERLACE, TFCNTRFLAG, FINTERPFLAG, a reserved bit, PSF, DISPLAY_EXT and HRD_PARAM_FLAG.
		s.put(0x10F, 32).put(3, 2).put(3, 3).put(1, 2).put(7, 3).put(31, 5).put(1, 1);
		s.put(width / 2 - 1, 12).put(height / 2 - 1, 12);
		s.put(0, 1).put(interlace ? 1 : 0, 1).put(0, 1).put(0, 1).put(1, 1).put(0, 1).put(0, 1).put(0, 1).trailing();
		// BROKEN_LINK, CLOSED_ENTRY, PANSCAN_FLAG, REFDIST_FLAG, LOOPFILTER, FASTUVMC, EXTENDED_MV, DQUANT,
		// VSTRANSFORM, OVERLAP and QUANTIZER; CODED_SIZE_FLAG, RANGE_MAPY_FLAG and RANGE_MAPUV_FLAG.
		s.put(0x10E, 32).put(0b0101110, 7).put(0, 2).put(0, 1).put(0, 1).put(0, 2).put(0, 3).trailing();
		s.put(0x10D, 32).put(0x40, 8);
		return s.b;
	}

	// An ADTS frame of AAC LC with a payload of zeros.
	bytes adts_frame(const unsigned rate_index, const unsigned channels, const std::size_t payload = 32)
	{
		msb_bits s;
		s.put(0xFFF, 12).put(0, 1).put(0, 2).put(1, 1).put(1, 2).put(rate_index, 4).put(0, 1).put(channels, 3);
		s.put(0, 4).put(7 + payload, 13).put(0x7FF, 11).put(0, 2);
		s.b.resize(7 + payload, 0);
		return s.b;
	}

	// An AC-3 frame: 48 kHz, 192 kb/s, 2/0.
	bytes ac3_frame()
	{
		msb_bits s;
		s.put(0x0B77, 16).put(0, 16).put(0, 2).put(20, 6);
		s.put(8, 5).put(0, 3).put(2, 3).put(0, 2).put(0, 1).put(27, 5).put(0, 1).put(0, 1).put(0, 1).put(0, 1).put(0, 1).put(0, 1).put(0, 1).put(0, 1);
		s.b.resize(768, 0);
		return s.b;
	}

	// An MPEG-1 Layer II frame: 48 kHz, 192 kb/s, stereo; or Layer III at 128 kb/s.
	bytes mpeg_audio_frame(const bool layer3 = false)
	{
		bytes frame(layer3 ? 384 : 576, 0);
		frame[0] = 0xFF;
		frame[1] = layer3 ? 0xFB : 0xFD;
		frame[2] = layer3 ? 0x94 : 0xA4;
		return frame;
	}

	// A LOAS frame: its AudioMuxElement holds a StreamMuxConfig for AAC LC unless config is false.
	bytes latm_frame(const unsigned rate_index, const unsigned channels, const bool config)
	{
		msb_bits e;
		e.put(config ? 0 : 1, 1);

		if (config)
		{
			e.put(0, 1).put(1, 1).put(0, 6).put(0, 4).put(0, 3);
			e.put(2, 5).put(rate_index, 4).put(channels, 4).put(0, 3);
			e.put(0, 3).put(0xFF, 8).put(0, 1).put(0, 1);
		}

		e.put(4, 8).put(0, 32).align();

		msb_bits frame;
		frame.put(0x2B7, 11).put(e.b.size(), 13);
		return cat(frame.b, e.b);
	}

	// ---- PES packets ---------------------------------------------------------------------------------

	bytes timestamp(const std::uint64_t t, const unsigned prefix)
	{
		return {
			static_cast<std::uint8_t>(prefix << 4 | ((t >> 29) & 0x0E) | 1), static_cast<std::uint8_t>(t >> 22),
			static_cast<std::uint8_t>(((t >> 14) & 0xFE) | 1), static_cast<std::uint8_t>(t >> 7),
			static_cast<std::uint8_t>(((t << 1) & 0xFE) | 1)
		};
	}

	struct pes_options
	{
		std::optional<std::uint64_t> pts;
		std::optional<std::uint64_t> dts;
		// A video packet's length left open, as a transport stream may.
		bool open = false;
		std::optional<unsigned> extended_id;

		pes_options& with_dts(const std::uint64_t t)
		{
			dts = t;
			return *this;
		}

		pes_options& open_length()
		{
			open = true;
			return *this;
		}

		pes_options& extended(const unsigned id)
		{
			extended_id = id;
			return *this;
		}
	};

	// A PES packet's options with its PTS.
	pes_options at(const std::uint64_t pts)
	{
		pes_options o;
		o.pts = pts;
		return o;
	}

	// A PES packet in MPEG-2's form.
	bytes pes(const std::uint8_t stream_id, const bytes& payload, const pes_options& o = {})
	{
		bytes optional;
		if (o.pts) optional = timestamp(*o.pts, o.dts ? 3 : 2);
		if (o.dts) optional = cat(optional, timestamp(*o.dts, 1));
		if (o.extended_id) optional = cat(optional, bytes{0x0F, 0x81, static_cast<std::uint8_t>(*o.extended_id)});

		const auto flags = (o.pts ? (o.dts ? 0xC0 : 0x80) : 0) | (o.extended_id ? 0x01 : 0);
		const auto length = 3 + optional.size() + payload.size();
		return builder().be32(0x100u | stream_id).be16(o.open ? 0 : static_cast<unsigned>(length)).u8(0x80).u8(static_cast<unsigned>(flags))
		                .u8(static_cast<unsigned>(optional.size())).raw(optional).raw(payload).b;
	}

	// A PES packet in MPEG-1's form: stuffing, the STD buffer's size, then the timestamps.
	bytes pes_mpeg1(const std::uint8_t stream_id, const bytes& payload, const std::optional<std::uint64_t> pts)
	{
		auto header = bytes{0xFF, 0xFF, 0x60, 0x2E};
		header = cat(header, pts ? timestamp(*pts, 2) : bytes{0x0F});
		return builder().be32(0x100u | stream_id).be16(static_cast<unsigned>(header.size() + payload.size())).raw(header).raw(payload).b;
	}

	// ---- Transport streams ---------------------------------------------------------------------------

	// CRC-32/MPEG-2, written here rather than taken from the scanner it checks.
	std::uint32_t crc_mpeg2(const bytes& data)
	{
		std::uint32_t crc = 0xFFFFFFFF;

		for (const auto v : data)
		{
			for (auto bit = 7; bit >= 0; --bit)
			{
				const auto top = ((crc >> 31) ^ (v >> bit)) & 1;
				crc <<= 1;
				if (top) crc ^= 0x04C11DB7;
			}
		}

		return crc;
	}

	bytes psi_section(const std::uint8_t table, const std::uint16_t id, const bytes& body, const bool damage = false)
	{
		builder b;
		b.u8(table).be16(0xB000u | static_cast<unsigned>(5 + body.size() + 4)).be16(id).u8(0xC1).u8(0).u8(0).raw(body);
		b.be32(crc_mpeg2(b.b) ^ (damage ? 1u : 0u));
		return b.b;
	}

	struct pat_entry
	{
		unsigned program = 0;
		unsigned pid = 0;
	};

	bytes pat(const std::vector<pat_entry>& programs, const bool damage = false)
	{
		builder body;
		for (const auto& [program, pid] : programs) body.be16(program).be16(0xE000u | pid);
		return psi_section(0x00, 1, body.b, damage);
	}

	struct es_entry
	{
		std::uint8_t type = 0;
		std::uint16_t pid = 0;
		bytes descriptors;
	};

	bytes pmt(const std::uint16_t program, const std::uint16_t pcr_pid, const bytes& descriptors, const std::vector<es_entry>& streams,
	          const bool damage = false)
	{
		builder body;
		body.be16(0xE000u | pcr_pid).be16(0xF000u | static_cast<unsigned>(descriptors.size())).raw(descriptors);

		for (const auto& e : streams)
		{
			body.u8(e.type).be16(0xE000u | e.pid).be16(0xF000u | static_cast<unsigned>(e.descriptors.size())).raw(e.descriptors);
		}

		return psi_section(0x02, program, body.b, damage);
	}

	bytes descriptor(const std::uint8_t tag, const bytes& body)
	{
		return builder().u8(tag).u8(static_cast<unsigned>(body.size())).raw(body).b;
	}

	bytes registration(const std::string_view fourcc)
	{
		return descriptor(0x05, to_bytes(fourcc));
	}

	bytes language(const std::string_view code, const std::uint8_t audio_type = 0)
	{
		return descriptor(0x0A, builder().raw(code).u8(audio_type).b);
	}

	// No PCR in a packet's adaptation field.
	constexpr std::uint64_t no_pcr = ~0ull;

	struct ts_writer
	{
		bytes out;
		bool m2ts = false;
		std::array<std::uint8_t, 8192> counters{};
		std::uint32_t arrival = 0;

		// One packet with as much of the payload as fits after an adaptation field holding the PCR and
		// any stuffing; returns how much fitted.
		std::size_t packet(const std::uint16_t pid, const bool start, const std::span<const std::uint8_t> payload,
		                   const std::uint64_t pcr = no_pcr, const unsigned scrambling = 0)
		{
			if (m2ts)
			{
				arrival += 1000;
				const auto stamp = builder().be32(arrival & 0x3FFFFFFF).b;
				out.insert(out.end(), stamp.begin(), stamp.end());
			}

			const auto has_pcr = pcr != no_pcr;
			const auto take = std::min<std::size_t>(payload.size(), has_pcr ? 176 : 184);
			const auto adaptation = 184 - take;
			auto& counter = counters[pid];

			out.push_back(0x47);
			out.push_back(static_cast<std::uint8_t>((start ? 0x40 : 0) | pid >> 8));
			out.push_back(static_cast<std::uint8_t>(pid));
			out.push_back(static_cast<std::uint8_t>(scrambling << 6 | (adaptation ? 0x20 : 0) | (take ? 0x10 : 0) | counter));
			if (take) counter = static_cast<std::uint8_t>((counter + 1) & 0x0F);

			if (adaptation)
			{
				const auto end = out.size() + adaptation;
				out.push_back(static_cast<std::uint8_t>(adaptation - 1));

				if (adaptation > 1)
				{
					out.push_back(has_pcr ? 0x10 : 0x00);

					if (has_pcr)
					{
						const auto base = pcr / 300;
						const auto extension = pcr % 300;
						const auto field = builder().be32(static_cast<std::uint32_t>(base >> 1))
						                            .u8(static_cast<unsigned>((base & 1) << 7 | 0x7E | extension >> 8))
						                            .u8(static_cast<unsigned>(extension & 0xFF)).b;
						out.insert(out.end(), field.begin(), field.end());
					}

					out.resize(end, 0xFF);
				}
			}

			out.insert(out.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(take));
			return take;
		}

		void section(const std::uint16_t pid, const bytes& s)
		{
			split(pid, cat(bytes{0}, s), no_pcr, 0);
		}

		void pes(const std::uint16_t pid, const bytes& p, const std::uint64_t pcr = no_pcr, const unsigned scrambling = 0)
		{
			split(pid, p, pcr, scrambling);
		}

		void filler(const std::size_t count)
		{
			const bytes stuffing(184, 0xFF);
			for (std::size_t i = 0; i < count; ++i) packet(0x1FFF, false, stuffing);
		}

	private:
		// The PCR goes in the first packet only.
		void split(const std::uint16_t pid, const bytes& data, const std::uint64_t pcr, const unsigned scrambling)
		{
			std::span<const std::uint8_t> rest(data);
			auto first = true;

			while (first || !rest.empty())
			{
				rest = rest.subspan(packet(pid, first, rest, first ? pcr : no_pcr, scrambling));
				first = false;
			}
		}
	};

	// A stream of H.264 at 25 frames a second and AAC at 48 kHz, a second long from 10 s.
	constexpr std::uint64_t ts_start = 900000;

	bytes h264_aac_stream(const bool m2ts = false)
	{
		ts_writer ts;
		ts.m2ts = m2ts;
		ts.section(0, pat({{1, 0x1000}}));
		ts.section(0x1000, pmt(1, 0x100, {}, {{0x1B, 0x100, {}}, {0x0F, 0x101, language("eng")}}));

		std::uint64_t audio = 0;

		for (std::uint64_t frame = 0; frame < 25; ++frame)
		{
			const auto pts = ts_start + frame * 3600;
			const auto unit = frame == 0 ? h264_first_unit(h264_sps(1920, 1080, 25)) : h264_unit();
			ts.pes(0x100, pes(0xE0, unit, at(pts).with_dts(pts).open_length()), (pts - 9000) * 300);

			for (; audio < 47 && ts_start + audio * 1920 < pts + 3600; ++audio)
			{
				ts.pes(0x101, pes(0xC0, adts_frame(3, 2), at(ts_start + audio * 1920)));
			}
		}

		return ts.out;
	}

	// The checks that hold for any cut or damaged copy of a stream: an error, or a result whose streams
	// are the whole stream's, timed no longer than it.
	void check_consistent(const bytes& file, const media_scan_result& whole, const std::size_t at)
	{
		const auto r = scan(file);

		if (!r.success)
		{
			if (r.error.empty()) test::fail(__FILE__, __LINE__, "a failed scan says why: " + std::to_string(at));
			return;
		}

		auto ok = std::isfinite(r.duration) && r.duration >= 0 && r.duration <= whole.duration + 1.0 && r.streams.size() <= whole.streams.size();

		for (const auto& s : r.streams)
		{
			const auto same = std::ranges::find_if(whole.streams, [&s](const libfiles::media_stream& w) { return w.id == s.id && w.kind == s.kind; });
			ok = ok && same != whole.streams.end() && std::isfinite(s.duration) && s.duration >= 0;
		}

		if (!ok) test::fail(__FILE__, __LINE__, "an inconsistent scan of a stream cut or damaged at " + std::to_string(at));
	}

	// Damage that leaves the result anyone's guess: only that it is an error or well formed.
	void check_sane(const bytes& file, const std::size_t at)
	{
		const auto r = scan(file);
		auto ok = r.success ? std::isfinite(r.duration) && r.duration >= 0 && r.streams.size() <= 256 : !r.error.empty();
		for (const auto& s : r.streams) ok = ok && std::isfinite(s.duration) && s.duration >= 0;
		if (!ok) test::fail(__FILE__, __LINE__, "a damaged stream scanned badly at " + std::to_string(at));
	}

	void cut_and_damage(const bytes& file, const std::size_t unit, const std::size_t prefix)
	{
		const auto whole = scan(file);
		CHECK(whole.success);

		// Every length near both ends, and near every packet boundary.
		for (std::size_t n = 16; n < file.size(); ++n)
		{
			const auto boundary = (n - prefix) % unit;
			if (n > 3 * unit && n + 3 * unit < file.size() && boundary > 3 && boundary < unit - 3) continue;
			check_consistent(bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n)), whole, n);
		}

		// Every byte of the tables and the first packets, and a sample beyond, flipped and zeroed.
		for (std::size_t at = 0; at < file.size(); at += at < 8 * unit ? 1 : 97)
		{
			auto damaged = file;
			damaged[at] ^= 0xFF;
			check_sane(damaged, at);
			damaged[at] = 0;
			check_sane(damaged, at);
		}
	}

	// ---- Program streams -----------------------------------------------------------------------------

	bytes pack_header(const std::uint64_t scr, const std::uint32_t mux_rate = 25200, const bool mpeg1 = false)
	{
		const auto base = scr / 300;
		msb_bits s;
		s.put(0x1BA, 32);

		if (mpeg1)
		{
			s.put(2, 4).put(base >> 30, 3).put(1, 1).put((base >> 15) & 0x7FFF, 15).put(1, 1).put(base & 0x7FFF, 15).put(1, 1);
			s.put(1, 1).put(mux_rate, 22).put(1, 1);
			return s.b;
		}

		s.put(1, 2).put(base >> 30, 3).put(1, 1).put((base >> 15) & 0x7FFF, 15).put(1, 1).put(base & 0x7FFF, 15).put(1, 1);
		s.put(scr % 300, 9).put(1, 1).put(mux_rate, 22).put(3, 2).put(0x1F, 5).put(2, 3);
		return cat(s.b, bytes{0xFF, 0xFF});
	}

	struct buffer_bound
	{
		unsigned id = 0;
		unsigned size = 0;
	};

	bytes system_header(const std::vector<buffer_bound>& streams, const unsigned audio, const unsigned video)
	{
		msb_bits s;
		s.put(1, 1).put(25200, 22).put(1, 1).put(audio, 6).put(0, 1).put(0, 1).put(1, 1).put(1, 1).put(1, 1).put(video, 5).put(0, 1).put(0x7F, 7);
		for (const auto& [id, size] : streams) s.put(id, 8).put(3, 2).put(1, 1).put(size, 13);
		return builder().be32(0x1BB).be16(static_cast<unsigned>(s.b.size())).raw(s.b).b;
	}

	struct map_line
	{
		unsigned type = 0;
		unsigned id = 0;
		bytes descriptors;
	};

	bytes stream_map(const std::vector<map_line>& entries)
	{
		builder map;
		for (const auto& [type, id, descriptors] : entries) map.u8(type).u8(id).be16(static_cast<unsigned>(descriptors.size())).raw(descriptors);

		builder body;
		body.u8(0x80).u8(0xFF).be16(0).be16(static_cast<unsigned>(map.size())).raw(map.b);

		auto packet = builder().be32(0x1BC).be16(static_cast<unsigned>(body.size() + 4)).raw(body.b).b;
		return builder().raw(packet).be32(crc_mpeg2(packet)).b;
	}

	bytes padding(const std::size_t size)
	{
		return builder().be32(0x1BE).be16(static_cast<unsigned>(size)).raw(bytes(size, 0xFF)).b;
	}

	// Private stream 1's substream headers: AC-3's frame count and pointer, and DVD LPCM's own after them.
	bytes ac3_substream(const std::uint8_t id, const bytes& frames)
	{
		return cat(bytes{id, 0x01, 0x00, 0x01}, frames);
	}

	bytes lpcm_substream(const std::uint8_t id, const std::size_t samples, const std::uint8_t format = 0x01)
	{
		return cat(bytes{id, 0x01, 0x00, 0x04, 0x00, format, 0x80}, bytes(samples * 4, 0));
	}

	// A DVD-like program stream: MPEG-2 video at 25 frames a second, AC-3 and LPCM in private stream 1,
	// a subpicture stream, padding and navigation packets, a second long from 1 s.
	constexpr std::uint64_t ps_start = 90000;

	bytes dvd_stream()
	{
		builder ps;
		std::uint64_t ac3 = 0;
		std::uint64_t lpcm = 0;

		for (std::uint64_t frame = 0; frame < 25; ++frame)
		{
			const auto pts = ps_start + frame * 3600;
			ps.raw(pack_header((pts - 18000) * 300));
			if (frame == 0) ps.raw(system_header({{0xE0, 232}, {0xBD, 58}, {0xBF, 2}}, 1, 1));
			if (frame == 0) ps.raw(builder().be32(0x1BF).be16(4).be32(0).b);

			const auto video = frame == 0 ? mpeg_video_sequence(720, 576, 3, true) : bytes{0, 0, 1, 0, 0x00, 0x58, 0xFF, 0xF8, 0, 0, 1, 1, 0x2A};
			ps.raw(pes(0xE0, video, at(pts + 3600).with_dts(pts)));

			for (; ac3 < 32 && ps_start + ac3 * 2880 < pts + 3600; ++ac3)
			{
				ps.raw(pes(0xBD, ac3_substream(0x80, ac3_frame()), at(ps_start + ac3 * 2880)));
			}

			for (; lpcm < 50 && ps_start + lpcm * 1800 < pts + 3600; ++lpcm)
			{
				ps.raw(pes(0xBD, lpcm_substream(0xA0, 960), at(ps_start + lpcm * 1800)));
			}

			if (frame == 5) ps.raw(pes(0xBD, bytes{0x20, 0x00, 0x08, 0x00, 0x04, 0xFF, 0xFF, 0xFF}, at(pts)));
			ps.raw(padding(30));
		}

		ps.raw(bytes{0, 0, 1, 0xB9});
		return ps.b;
	}
}

// ---- Transport streams -------------------------------------------------------------------------------

TEST("media ts reads H.264 and AAC in 188-byte packets")
{
	const auto r = scan(h264_aac_stream());
	CHECK(r.success);
	CHECK(r.format == media_format::mpeg_ts);
	CHECK_STR(r.brand, "MPEG-TS");
	CHECK_EQ(r.streams.size(), std::size_t{2});
	if (r.streams.size() != 2) return;

	const auto& v = r.streams[0];
	CHECK(v.kind == media_stream_kind::video);
	CHECK_EQ(v.id, std::uint64_t{0x100});
	CHECK_STR(v.codec, "h264");
	CHECK_STR(v.codec_tag, "0x1B");
	CHECK_EQ(v.video.width, 1920u);
	CHECK_EQ(v.video.height, 1080u);
	CHECK(near(v.video.frame_rate, 25.0));
	CHECK(v.video.chroma == libfiles::chroma_format::yuv420);
	CHECK_EQ(v.video.bit_depth, 8);
	CHECK_STR(v.video.profile, "High");
	CHECK_EQ(v.video.color_primaries, 1);
	CHECK_EQ(v.video.transfer, 1);
	CHECK_EQ(v.video.matrix, 1);
	CHECK(!v.video.full_range);

	const auto& a = r.streams[1];
	CHECK(a.kind == media_stream_kind::audio);
	CHECK_EQ(a.id, std::uint64_t{0x101});
	CHECK_STR(a.codec, "aac");
	CHECK_STR(a.codec_tag, "0x0F");
	CHECK_STR(a.language, "eng");
	CHECK_EQ(a.audio.sample_rate, 48000u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK_STR(a.audio.profile, "LC");
	CHECK_EQ(a.audio.bits_per_sample, 0);

	// An ADTS frame's length gives its own rate, not the stream's, which nothing states.
	CHECK_EQ(a.bit_rate, std::uint64_t{0});

	// Each stream from its first timestamp to its last plus a frame: 25 frames of 1/25 s, and 47 of
	// 1024 samples; the file from the earliest start to the latest end.
	CHECK(near(v.duration, 1.0));
	CHECK(near(a.duration, 47 * 1024 / 48000.0));
	CHECK(near(r.duration, 47 * 1024 / 48000.0));
	CHECK(r.complete);
	CHECK(r.properties.camera_make.empty());
	CHECK(!r.properties.created.valid());
}

TEST("media ts reads 192-byte M2TS packets with Blu-ray's stream types and the AVCHD camera metadata")
{
	ts_writer ts;
	ts.m2ts = true;
	ts.section(0, pat({{0, 0x1F}, {1, 0x100}}));

	const auto hdmv = cat(registration("HDMV"), descriptor(0x88, {0x0F, 0xFF, 0xFC, 0xFC}));
	ts.section(0x100, pmt(1, 0x1001, hdmv, {
		                      {0x1B, 0x1011, cat(registration("HDMV"), bytes{0xFF, 0x1B, 0x43, 0x3F})},
		                      {0x80, 0x1100, {}},
		                      {0x81, 0x1101, registration("AC-3")},
		                      {0x90, 0x1200, {}},
	                      }));

	constexpr std::uint64_t start = 900000;

	for (std::uint64_t frame = 0; frame < 25; ++frame)
	{
		const auto pts = start + frame * 3600;
		const auto unit = frame == 0 ? h264_first_unit(h264_sps(1440, 1080, 25), mdpm_sei(sony_packs(0x42))) : h264_unit();
		ts.pes(0x1011, pes(0xE0, unit, at(pts).with_dts(pts).open_length()));
		ts.pes(0x1001, {}, (pts - 9000) * 300);

		// Blu-ray LPCM: 48 kHz stereo at 16 bits, 1/25 s a packet.
		const auto lpcm = cat(bytes{0x0F, 0x00, 0x31, 0x40}, bytes(1920 * 4, 0));
		ts.pes(0x1100, pes(0xBD, lpcm, at(pts)));
	}

	for (std::uint64_t frame = 0; frame < 32; ++frame) ts.pes(0x1101, pes(0xBD, ac3_frame(), at(start + frame * 2880)));

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_STR(r.brand, "M2TS");
	CHECK_EQ(r.streams.size(), std::size_t{4});
	if (r.streams.size() != 4) return;

	CHECK_STR(r.streams[0].codec, "h264");
	CHECK_STR(r.streams[0].codec_tag, "HDMV");
	CHECK_EQ(r.streams[0].id, std::uint64_t{0x1011});
	CHECK_EQ(r.streams[0].video.width, 1440u);

	const auto& lpcm = r.streams[1];
	CHECK_STR(lpcm.codec, "pcm");
	CHECK_STR(lpcm.codec_tag, "0x80");
	CHECK_EQ(lpcm.audio.sample_rate, 48000u);
	CHECK_EQ(lpcm.audio.channels, 2u);
	CHECK_EQ(lpcm.audio.bits_per_sample, 16);
	CHECK(!lpcm.audio.float_samples);

	// FFmpeg counts the last LPCM packet's samples from its size: 1920 frames of two 16-bit channels.
	CHECK(near(lpcm.duration, 1.0));

	const auto& ac3 = r.streams[2];
	CHECK_STR(ac3.codec, "ac3");
	CHECK_STR(ac3.codec_tag, "AC-3");
	CHECK_EQ(ac3.audio.sample_rate, 48000u);
	CHECK_EQ(ac3.audio.channels, 2u);
	CHECK_EQ(ac3.bit_rate, std::uint64_t{192000});
	CHECK(near(ac3.duration, 32 * 1536 / 48000.0));

	const auto& pgs = r.streams[3];
	CHECK(pgs.kind == media_stream_kind::subtitle);
	CHECK_STR(pgs.codec, "hdmv_pgs_subtitle");
	CHECK_EQ(pgs.id, std::uint64_t{0x1200});

	// Recorded 2011-07-04 14:37:28 at UTC+1 with daylight saving in effect.
	const auto& created = r.properties.created;
	CHECK_EQ(created.year, 2011);
	CHECK_EQ(created.month, 7);
	CHECK_EQ(created.day, 4);
	CHECK_EQ(created.hour, 14);
	CHECK_EQ(created.minute, 37);
	CHECK_EQ(created.second, 28);
	CHECK(created.zone == libfiles::media_date::time_zone::offset);
	CHECK_EQ(created.offset_minutes, 120);
	CHECK_STR(r.properties.camera_make, "Sony");
	CHECK_STR(r.properties.camera_model, "HDR-CX550V");
	CHECK(near(r.duration, 32 * 1536 / 48000.0));
}

TEST("media ts reads the AVCHD date's zone only when the camera states one")
{
	const auto read = [](const std::uint8_t zone, const std::uint16_t maker)
	{
		auto packs = sony_packs(zone);
		packs[11] = static_cast<std::uint8_t>(maker >> 8);
		packs[12] = static_cast<std::uint8_t>(maker);
		libfiles::media_properties p;
		CHECK(libfiles::detail::read_avchd_camera(mdpm_sei(packs), p));
		return p;
	};

	// West of UTC by 5 hours and 30 minutes.
	const auto west = read(0x2B, 0x0108);
	CHECK(west.created.zone == libfiles::media_date::time_zone::offset);
	CHECK_EQ(west.created.offset_minutes, -330);

	// All zeros is what Panasonic's cameras write, a zone they do not keep; bit 7 set is no zone at all.
	const auto panasonic = read(0x00, 0x0103);
	CHECK(panasonic.created.valid());
	CHECK(panasonic.created.zone == libfiles::media_date::time_zone::unstated);
	CHECK_STR(panasonic.camera_make, "Panasonic");
	CHECK(panasonic.camera_model.empty());
	CHECK(read(0xFF, 0x1011).created.zone == libfiles::media_date::time_zone::unstated);
	CHECK_STR(read(0xFF, 0x1011).camera_make, "Canon");
	CHECK(read(0x40, 0x9999).camera_make.empty());

	// A date that is not BCD, and an SEI without the UUID.
	auto packs = sony_packs(0x02);
	packs[3] = 0x1A;
	libfiles::media_properties bad;
	CHECK(libfiles::detail::read_avchd_camera(mdpm_sei(packs), bad));
	CHECK(!bad.created.valid());

	auto other = mdpm_sei(sony_packs(0x02));
	other[3] ^= 1;
	libfiles::media_properties none;
	CHECK(!libfiles::detail::read_avchd_camera(other, none));
	CHECK(!libfiles::detail::read_avchd_camera(bytes{0x06}, none));
}

TEST("media ts reads MPEG-2 video and private data named by its descriptors")
{
	ts_writer ts;
	ts.section(0, pat({{3, 0x20}}));
	ts.section(0x20, pmt(3, 0x200, {}, {
		                     {0x02, 0x200, {}},
		                     {0x06, 0x201, cat(descriptor(0x6A, {0x00}), language("deu", 3))},
		                     {0x06, 0x202, descriptor(0x56, cat(to_bytes("eng"), bytes{0x09, 0x88}))},
		                     {0x06, 0x203, descriptor(0x59, cat(to_bytes("fra"), bytes{0x10, 0x00, 0x01, 0x00, 0x01}))},
		                     {0x06, 0x204, cat(registration("Opus"), descriptor(0x7F, {0x80, 0x06}), language("und"))},
		                     {0x05, 0x205, {}},
		                     {0x06, 0x206, cat(descriptor(0x7A, {0x00}), descriptor(0x0A, cat(to_bytes("und"), bytes{0}, to_bytes("spa"), bytes{0})))},
	                     }));

	for (std::uint64_t frame = 0; frame < 25; ++frame)
	{
		const auto pts = ts_start + frame * 3600;
		const auto video = frame == 0 ? mpeg_video_sequence(720, 576, 3, true) : bytes{0, 0, 1, 0, 0x00, 0x58, 0xFF, 0xF8};
		ts.pes(0x200, pes(0xE0, video, at(pts + 3600).with_dts(pts).open_length()), pts * 300);
		ts.pes(0x204, pes(0xBD, bytes{0x7F, 0xE0, 0x10, 0xFC, 0x00}, at(pts)));
	}

	for (std::uint64_t frame = 0; frame < 31; ++frame) ts.pes(0x201, pes(0xBD, ac3_frame(), at(ts_start + frame * 2880)));

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{7});
	if (r.streams.size() != 7) return;

	const auto& video = r.streams[0];
	CHECK_STR(video.codec, "mpeg2");
	CHECK_EQ(video.video.width, 720u);
	CHECK_EQ(video.video.height, 576u);
	CHECK(near(video.video.frame_rate, 25.0));
	CHECK_STR(video.video.profile, "Main");
	CHECK(video.video.chroma == libfiles::chroma_format::yuv420);

	const auto& ac3 = r.streams[1];
	CHECK_STR(ac3.codec, "ac3");
	CHECK_STR(ac3.codec_tag, "0x06");
	CHECK_STR(ac3.language, "deu");
	CHECK(ac3.audio_description);
	CHECK_EQ(ac3.audio.channels, 2u);

	CHECK(r.streams[2].kind == media_stream_kind::subtitle);
	CHECK_STR(r.streams[2].codec, "dvb_teletext");
	CHECK_STR(r.streams[2].language, "eng");
	CHECK_STR(r.streams[3].codec, "dvb_subtitle");
	CHECK_STR(r.streams[3].language, "fra");

	const auto& opus = r.streams[4];
	CHECK_STR(opus.codec, "opus");
	CHECK_STR(opus.codec_tag, "Opus");
	CHECK_EQ(opus.audio.sample_rate, 48000u);
	CHECK_EQ(opus.audio.channels, 6u);

	// "und" names no language, so none is reported; a descriptor's next code stands instead.
	CHECK(opus.language.empty());

	CHECK(r.streams[5].kind == media_stream_kind::data);
	CHECK(r.streams[5].codec.empty());
	CHECK_STR(r.streams[5].codec_tag, "0x05");
	CHECK_STR(r.streams[6].codec, "eac3");
	CHECK_STR(r.streams[6].language, "spa");
	CHECK(!r.streams[6].audio_description);

	// The video's 25 frames from its first PTS, 1/25 s each; the AC-3's 31 frames of 1536 samples.
	CHECK(near(video.duration, 1.0));
	CHECK(near(ac3.duration, 31 * 1536 / 48000.0));
	CHECK(near(r.duration, 1.04));
}

TEST("media ts reads HEVC, MPEG audio's layer and LATM")
{
	ts_writer ts;
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x200, {}, {{0x24, 0x200, {}}, {0x04, 0x201, {}}, {0x11, 0x202, {}}}));

	for (std::uint64_t frame = 0; frame < 10; ++frame)
	{
		const auto pts = ts_start + frame * 3000;
		const auto unit = frame == 0 ? hevc_first_unit(hevc_sps(1280, 720)) : annexb({0x02, 0x01, 0xD0, 0x10});
		ts.pes(0x200, pes(0xE0, unit, at(pts).open_length()), pts * 300);
		ts.pes(0x201, pes(0xC0, cat(mpeg_audio_frame(true), mpeg_audio_frame(true)), at(pts)));
		ts.pes(0x202, pes(0xC0, cat(latm_frame(3, 2, frame == 1), latm_frame(3, 2, false)), at(pts)));
	}

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{3});
	if (r.streams.size() != 3) return;

	const auto& hevc = r.streams[0];
	CHECK_STR(hevc.codec, "hevc");
	CHECK_EQ(hevc.video.width, 1280u);
	CHECK_EQ(hevc.video.height, 720u);
	CHECK_STR(hevc.video.profile, "Main");
	CHECK(hevc.video.chroma == libfiles::chroma_format::yuv420);

	// The stream type says MPEG-2 audio; the frame says MPEG-1 Layer III.
	const auto& mp3 = r.streams[1];
	CHECK_STR(mp3.codec, "mp3");
	CHECK_EQ(mp3.audio.sample_rate, 48000u);
	CHECK_EQ(mp3.audio.channels, 2u);
	CHECK_EQ(mp3.bit_rate, std::uint64_t{128000});

	// LATM's configuration comes in its second frame here; the first reuses an earlier one.
	const auto& latm = r.streams[2];
	CHECK_STR(latm.codec, "aac");
	CHECK_EQ(latm.audio.sample_rate, 48000u);
	CHECK_EQ(latm.audio.channels, 2u);
}

TEST("media ts reads a VC-1 stream's sequence header and entry point")
{
	ts_writer ts;
	ts.m2ts = true;
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x1011, registration("HDMV"), {{0xEA, 0x1011, {}}}));

	for (std::uint64_t frame = 0; frame < 10; ++frame)
	{
		const auto pts = ts_start + frame * 3600;
		const auto unit = frame == 0 ? vc1_sequence(1440, 1080, true) : bytes{0, 0, 1, 0x0D, 0x40};
		ts.pes(0x1011, pes(0xFD, unit, at(pts).open_length().extended(0x55)), pts * 300);
	}

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{1});
	if (r.streams.size() != 1) return;

	const auto& v = r.streams[0];
	CHECK_STR(v.codec, "vc1");
	CHECK_EQ(v.video.width, 1440u);
	CHECK_EQ(v.video.height, 1080u);
	CHECK_STR(v.video.profile, "Advanced");
	CHECK(v.video.chroma == libfiles::chroma_format::yuv420);
	CHECK_EQ(v.video.bit_depth, 8);
	CHECK(r.complete);

	// Cut before the entry point, which FFmpeg's decoder needs too, the stream is named but not sized.
	const bytes entry_code = {0, 0, 1, 0x0E};
	const auto entry = std::search(ts.out.begin(), ts.out.end(), entry_code.begin(), entry_code.end());
	CHECK(entry != ts.out.end());
	if (entry == ts.out.end()) return;

	const auto cut = scan(bytes(ts.out.begin(), entry));
	CHECK(!cut.complete);
	CHECK_EQ(cut.streams.size(), std::size_t{1});
	if (cut.streams.size() != 1) return;
	CHECK_STR(cut.streams[0].codec, "vc1");
	CHECK_EQ(cut.streams[0].video.width, 0u);
}

TEST("media ts lists Blu-ray TrueHD's AC-3 core as FFmpeg does")
{
	ts_writer ts;
	ts.m2ts = true;
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x1011, registration("HDMV"), {{0xEA, 0x1011, {}}, {0x83, 0x1100, language("jpn")}}));

	for (std::uint64_t frame = 0; frame < 10; ++frame)
	{
		const auto pts = ts_start + frame * 3600;
		ts.pes(0x1011, pes(0xFD, bytes{0, 0, 1, 0x0F, 0x10}, at(pts).open_length().extended(0x55)), pts * 300);
		ts.pes(0x1100, pes(0xFD, bytes{0xF8, 0x72, 0x6F, 0xBA}, at(pts).extended(0x72)));
		ts.pes(0x1100, pes(0xFD, ac3_frame(), at(pts).extended(0x76)));
	}

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{3});
	if (r.streams.size() != 3) return;

	CHECK_STR(r.streams[0].codec, "vc1");
	CHECK(r.streams[0].video.chroma == libfiles::chroma_format::yuv420);
	CHECK_STR(r.streams[1].codec, "truehd");
	CHECK_EQ(r.streams[1].id, std::uint64_t{0x1100});
	CHECK_STR(r.streams[1].language, "jpn");
	CHECK_STR(r.streams[2].codec, "ac3");
	CHECK_EQ(r.streams[2].id, std::uint64_t{0x1100});
	CHECK_EQ(r.streams[2].audio.sample_rate, 48000u);

	// VC-1 and TrueHD have no frame this scan times, so their last timestamps end them.
	CHECK(near(r.streams[0].duration, 0.36));
	CHECK(near(r.streams[1].duration, 0.36));
	CHECK(near(r.streams[2].duration, 0.36 + 1536 / 48000.0));
}

TEST("media ts times a stream across the 33-bit wrap")
{
	// VC-1 and Opus: neither has a frame duration this scan adds, so the timestamps alone time them.
	constexpr std::uint64_t first = libfiles::detail::mpeg_timestamp_wrap - 45000;

	ts_writer ts;
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x200, {}, {{0xEA, 0x200, {}}, {0x06, 0x201, registration("Opus")}}));

	for (std::uint64_t frame = 0; frame < 50; ++frame)
	{
		const auto pts = (first + frame * 3600) % libfiles::detail::mpeg_timestamp_wrap;
		ts.pes(0x200, pes(0xE0, bytes{0, 0, 1, 0x0D, 0x40}, at(pts).open_length()), pts * 300);
		ts.pes(0x201, pes(0xBD, bytes{0x7F, 0xE0, 0x10}, at((pts + 1800) % libfiles::detail::mpeg_timestamp_wrap)));
	}

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK(near(r.streams[0].duration, 49 * 3600 / 90000.0));
	CHECK(near(r.streams[1].duration, 49 * 3600 / 90000.0));
	CHECK(near(r.duration, (49 * 3600 + 1800) / 90000.0));
}

TEST("media ts times scrambled payloads by the PCR, across its wrap")
{
	constexpr auto pcr_wrap = libfiles::detail::mpeg_system_clock_wrap;
	constexpr std::uint64_t first = pcr_wrap - 27000000;

	ts_writer ts;
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x200, {}, {{0x1B, 0x200, {}}, {0x0F, 0x201, {}}}));

	for (std::uint64_t i = 0; i < 30; ++i)
	{
		const auto pcr = (first + i * 2700000) % pcr_wrap;
		ts.pes(0x200, pes(0xE0, h264_unit(), at(i * 9000).open_length()), pcr, 2);
		ts.pes(0x201, pes(0xC0, adts_frame(3, 2), at(i * 9000)), no_pcr, 3);
	}

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{2});
	CHECK(near(r.duration, 2.9));
	CHECK(!r.complete);
}

TEST("media ts passes over damaged tables to their next copy")
{
	ts_writer ts;
	ts.section(0, pat({{1, 0x300}}, true));
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x200, {}, {{0x02, 0x300, {}}}, true));
	ts.filler(3);
	ts.section(0x100, pmt(1, 0x200, {}, {{0xEA, 0x200, {}}}));
	ts.pes(0x200, pes(0xE0, bytes{0, 0, 1, 0x0D}, at(9000).open_length()), 9000 * 300);

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{1});
	if (!r.streams.empty()) CHECK_EQ(r.streams[0].id, std::uint64_t{0x200});

	// Only the damaged copies: no table to read.
	ts_writer broken;
	broken.section(0, pat({{1, 0x100}}, true));
	broken.filler(4);
	const auto b = scan(broken.out);
	CHECK(!b.success);
	CHECK(b.format == media_format::mpeg_ts);
	CHECK(!b.error.empty());

	ts_writer unmapped;
	unmapped.section(0, pat({{1, 0x100}}));
	unmapped.section(0x100, pmt(1, 0x200, {}, {{0xEA, 0x200, {}}}, true));
	unmapped.filler(4);
	CHECK(!scan(unmapped.out).success);
}

TEST("media ts reads the first program, or the one a recording kept")
{
	const auto vc1 = bytes{0, 0, 1, 0x0D, 0x40};

	// Both maps: the table's first program, though the other's map comes first.
	ts_writer both;
	both.section(0, pat({{1, 0x100}, {2, 0x200}}));
	both.section(0x200, pmt(2, 0x210, {}, {{0xEA, 0x210, {}}}));
	both.section(0x100, pmt(1, 0x110, {}, {{0xEA, 0x110, {}}}));
	both.pes(0x210, pes(0xE0, vc1, at(9000).open_length()), 9000 * 300);
	both.pes(0x110, pes(0xE0, vc1, at(9000).open_length()), 9000 * 300);

	const auto b = scan(both.out);
	CHECK(b.success);
	CHECK_EQ(b.streams.size(), std::size_t{1});
	if (b.streams.size() == 1) CHECK_EQ(b.streams[0].id, std::uint64_t{0x110});

	// A recording of the second program that kept the whole table: once its map comes round again
	// without the first's, it is read.
	ts_writer kept;
	kept.section(0, pat({{1, 0x100}, {2, 0x200}}));
	kept.section(0x200, pmt(2, 0x210, {}, {{0xEA, 0x210, {}}}));
	kept.pes(0x210, pes(0xE0, vc1, at(9000).open_length()), 9000 * 300);
	kept.section(0x200, pmt(2, 0x210, {}, {{0xEA, 0x210, {}}}));

	for (std::uint64_t frame = 1; frame <= 10; ++frame)
	{
		kept.pes(0x210, pes(0xE0, vc1, at(9000 + frame * 3600).open_length()), (9000 + frame * 3600) * 300);
	}

	const auto k = scan(kept.out);
	CHECK(k.success);
	CHECK_EQ(k.streams.size(), std::size_t{1});
	if (k.streams.size() == 1) CHECK_EQ(k.streams[0].id, std::uint64_t{0x210});
	CHECK(near(k.duration, 9 * 3600 / 90000.0));
}

TEST("media ts resynchronises after lost and damaged packets")
{
	ts_writer ts;
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x200, registration("HDMV"), {{0xEA, 0x200, {}}, {0x80, 0x201, {}}}));

	for (std::uint64_t frame = 0; frame < 20; ++frame)
	{
		const auto pts = ts_start + frame * 3600;
		ts.pes(0x200, pes(0xE0, bytes(400, 0x11), at(pts).open_length()), pts * 300);

		// Junk between packets, and a packet sent twice.
		if (frame == 5) ts.out.insert(ts.out.end(), 77, 0x47);

		if (frame == 9)
		{
			const bytes again(ts.out.end() - 188, ts.out.end());
			ts.out.insert(ts.out.end(), again.begin(), again.end());
		}
	}

	// A continuity break inside a PES packet; the stream's next packet starts afresh.
	ts.pes(0x201, cat(pes(0xBD, cat(bytes{0x01, 0x00, 0x31, 0x40}, bytes(800, 0)), at(ts_start))));
	ts.counters[0x201] = 9;
	ts.pes(0x201, pes(0xBD, cat(bytes{0x01, 0x00, 0x31, 0x40}, bytes(8, 0)), at(ts_start + 7200)));

	const auto r = scan(ts.out);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{2});
	if (r.streams.size() != 2) return;

	CHECK(near(r.streams[0].duration, 19 * 3600 / 90000.0));
	CHECK_EQ(r.streams[1].audio.sample_rate, 48000u);
	CHECK(r.streams[1].duration > 0.08);
}

TEST("media ts survives every cut and damage")
{
	cut_and_damage(h264_aac_stream(), 188, 0);
	cut_and_damage(h264_aac_stream(true), 192, 4);
}

TEST("media ts reads a large stream's ends and nothing between")
{
	// VC-1 and Blu-ray LPCM, whose headers this scanner reads itself; then 30 MB of null packets.
	ts_writer ts;
	ts.section(0, pat({{1, 0x100}}));
	ts.section(0x100, pmt(1, 0x200, registration("HDMV"), {{0xEA, 0x200, {}}, {0x80, 0x201, {}}}));

	const auto media = [&ts](const std::uint64_t pts, const bool first)
	{
		const auto video = first ? vc1_sequence(1920, 1080, false) : bytes{0, 0, 1, 0x0D, 0x40};
		ts.pes(0x200, pes(0xE0, video, at(pts).open_length()), pts * 300);
		ts.pes(0x201, pes(0xBD, cat(bytes{0x07, 0x84, 0x31, 0x40}, bytes(1920, 0)), at(pts)));
	};

	media(ts_start, true);
	ts.filler(170000);
	media(ts_start + 90000 * 600, false);

	libfiles::memory_stream file(ts.out);
	libfiles::reader r(file);
	const auto s = libfiles::scan_media(r);
	CHECK(s.success);
	CHECK(near(s.duration, 600 + 480 / 48000.0));
	CHECK(r.source_reads() <= 6);
	CHECK(r.source_bytes() <= 6 * libfiles::reader::default_block_size);

	// H.264 and AAC need their headers read before the scan stops.
	ts_writer av;
	av.section(0, pat({{1, 0x1000}}));
	av.section(0x1000, pmt(1, 0x100, {}, {{0x1B, 0x100, {}}, {0x0F, 0x101, {}}}));
	av.pes(0x100, pes(0xE0, h264_first_unit(h264_sps(1920, 1080, 25)), at(ts_start).with_dts(ts_start).open_length()), ts_start * 300);
	av.pes(0x101, pes(0xC0, cat(adts_frame(3, 2), adts_frame(3, 2)), at(ts_start)));
	av.filler(170000);
	av.pes(0x100, pes(0xE0, h264_unit(), at(ts_start + 90000 * 600).open_length()), (ts_start + 90000 * 600) * 300);
	av.pes(0x101, pes(0xC0, adts_frame(3, 2), at(ts_start + 90000 * 600)));

	libfiles::memory_stream big(av.out);
	libfiles::reader rb(big);
	const auto sb = libfiles::scan_media(rb);
	CHECK(sb.success);
	CHECK(sb.complete);
	CHECK(near(sb.duration, 600.04));
	CHECK(rb.source_reads() <= 6);
}

TEST("media ts lists its packets, tables and streams for inspection")
{
	const auto r = scan(h264_aac_stream(true), libfiles::scan_intent::inspect);
	CHECK(r.success);
	CHECK(has_row(r, "Transport stream"));
	CHECK(has_row(r, "Packets"));
	CHECK(has_row(r, "Program 1"));
	CHECK(has_row(r, "PID 0x0100"));
	CHECK(has_row(r, "PID 0x0101"));
	CHECK(scan(h264_aac_stream()).structure.empty());
}

TEST("media ts reads PES headers in both forms and refuses others")
{
	using libfiles::detail::pes_read;
	libfiles::detail::pes_header h;

	const auto two = pes(0xE0, bytes{1, 2, 3}, at(0x1FFFFFFFF).with_dts(5).extended(0x55));
	CHECK(libfiles::detail::read_pes_header(two, false, h) == pes_read::ok);
	CHECK_EQ(h.stream_id, 0xE0);
	CHECK(h.pts == 0x1FFFFFFFFull);
	CHECK(h.dts == 5ull);
	CHECK(h.extended_stream_id == std::uint8_t{0x55});
	CHECK_EQ(h.size, two.size() - 3);

	const auto one = pes_mpeg1(0xC0, bytes{1, 2}, 123456789);
	CHECK(libfiles::detail::read_pes_header(one, false, h) == pes_read::invalid);
	CHECK(libfiles::detail::read_pes_header(one, true, h) == pes_read::ok);
	CHECK(h.pts == 123456789ull);
	CHECK_EQ(h.size, one.size() - 2);

	for (std::size_t n = 0; n < two.size() - 3; ++n)
	{
		CHECK(libfiles::detail::read_pes_header(std::span(two).first(n), false, h) == pes_read::short_data);
	}

	CHECK(libfiles::detail::read_pes_header(bytes{0, 0, 1, 0xBA, 0, 0}, true, h) == pes_read::invalid);
	CHECK(libfiles::detail::read_pes_header(bytes{0, 0, 2, 0xE0, 0, 0}, true, h) == pes_read::invalid);

	// A header longer than the packet's stated length.
	auto overlong = two;
	overlong[5] = 4;
	CHECK(libfiles::detail::read_pes_header(overlong, false, h) == pes_read::invalid);
}

TEST("media ts places timestamps across the wrap as FFmpeg does")
{
	libfiles::detail::timestamp_line line(libfiles::detail::mpeg_timestamp_wrap, 90000);
	const auto first = line.place(libfiles::detail::mpeg_timestamp_wrap - 100);
	CHECK_EQ(line.place(50) - first, std::int64_t{150});
	CHECK_EQ(line.place(libfiles::detail::mpeg_timestamp_wrap - 200) - first, std::int64_t{-100});

	// Up to 60 seconds before the first counts as earlier; anything else as later.
	CHECK_EQ(line.place(libfiles::detail::mpeg_timestamp_wrap - 100 - 59 * 90000) - first, std::int64_t{-59 * 90000});
	CHECK(line.place(libfiles::detail::mpeg_timestamp_wrap - 100 - 61 * 90000) > first);
}

// ---- Program streams ---------------------------------------------------------------------------------

TEST("media ps reads a DVD's MPEG-2 video, AC-3, LPCM and subpictures")
{
	const auto r = scan(dvd_stream());
	CHECK(r.success);
	CHECK(r.format == media_format::mpeg_ps);
	CHECK_STR(r.brand, "MPEG-PS");
	CHECK_EQ(r.streams.size(), std::size_t{4});
	if (r.streams.size() != 4) return;

	// In order of appearance, numbered as FFmpeg numbers them.
	const auto& video = r.streams[0];
	CHECK_EQ(video.id, std::uint64_t{0x1E0});
	CHECK_STR(video.codec, "mpeg2");
	CHECK(video.codec_tag.empty());
	CHECK_EQ(video.video.width, 720u);
	CHECK_EQ(video.video.height, 576u);
	CHECK(near(video.video.frame_rate, 25.0));

	const auto& ac3 = r.streams[1];
	CHECK_EQ(ac3.id, std::uint64_t{0x80});
	CHECK_STR(ac3.codec, "ac3");
	CHECK_EQ(ac3.audio.sample_rate, 48000u);
	CHECK_EQ(ac3.audio.channels, 2u);

	const auto& lpcm = r.streams[2];
	CHECK_EQ(lpcm.id, std::uint64_t{0xA0});
	CHECK_STR(lpcm.codec, "pcm");
	CHECK_EQ(lpcm.audio.sample_rate, 48000u);
	CHECK_EQ(lpcm.audio.channels, 2u);
	CHECK_EQ(lpcm.audio.bits_per_sample, 16);

	const auto& sub = r.streams[3];
	CHECK_EQ(sub.id, std::uint64_t{0x20});
	CHECK(sub.kind == media_stream_kind::subtitle);
	CHECK_STR(sub.codec, "dvd_subtitle");

	// Video from its first PTS, 1/25 s on; AC-3's 32 frames; LPCM's 50 packets of 960 samples.
	CHECK(near(video.duration, 1.0));
	CHECK(near(ac3.duration, 32 * 1536 / 48000.0));
	CHECK(near(lpcm.duration, 1.0));
	CHECK(near(r.duration, 1.04));
	CHECK(r.complete);
}

TEST("media ps reads an MPEG-1 system stream")
{
	builder ps;

	for (std::uint64_t frame = 0; frame < 25; ++frame)
	{
		const auto pts = ps_start + frame * 3600;
		ps.raw(pack_header(pts * 300, 3000, true));
		if (frame == 0) ps.raw(system_header({{0xE0, 46}, {0xC0, 32}}, 1, 1));
		const auto video = frame == 0 ? mpeg_video_sequence(352, 288, 3, false) : bytes{0, 0, 1, 0, 0x00, 0x58, 0xFF, 0xF8};
		ps.raw(pes_mpeg1(0xE0, video, pts));
		ps.raw(pes_mpeg1(0xC0, mpeg_audio_frame(), pts));
	}

	const auto r = scan(ps.b);
	CHECK(r.success);
	CHECK_STR(r.brand, "MPEG-1 system");
	CHECK_EQ(r.streams.size(), std::size_t{2});
	if (r.streams.size() != 2) return;

	CHECK_EQ(r.streams[0].id, std::uint64_t{0x1E0});
	CHECK_STR(r.streams[0].codec, "mpeg1");
	CHECK_EQ(r.streams[0].video.width, 352u);
	CHECK_EQ(r.streams[1].id, std::uint64_t{0x1C0});
	CHECK_STR(r.streams[1].codec, "mp2");
	CHECK_EQ(r.streams[1].audio.sample_rate, 48000u);
	CHECK_EQ(r.streams[1].bit_rate, std::uint64_t{192000});
	CHECK(near(r.streams[1].duration, (24 * 3600 + 2160) / 90000.0));
}

TEST("media ps knows H.264 by its content or by the stream map")
{
	// FFmpeg's muxer puts H.264 in stream 0xE2 and names it nowhere.
	builder plain;
	plain.raw(pack_header(0));
	plain.raw(pes(0xE2, h264_first_unit(h264_sps(640, 360, 30)), at(3003)));
	plain.raw(pes(0xE2, h264_unit(), at(6006)));

	const auto r = scan(plain.b);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{1});
	if (r.streams.size() == 1)
	{
		CHECK_EQ(r.streams[0].id, std::uint64_t{0x1E2});
		CHECK_STR(r.streams[0].codec, "h264");
		CHECK_EQ(r.streams[0].video.width, 640u);
		CHECK_EQ(r.streams[0].video.height, 360u);
	}

	// A map naming HEVC for 0xE0 and AAC for 0xC0, with a language.
	builder mapped;
	mapped.raw(pack_header(0));
	mapped.raw(stream_map({{0x24, 0xE0, {}}, {0x0F, 0xC0, language("ita")}}));
	mapped.raw(pes(0xE0, hevc_first_unit(hevc_sps(1920, 1080)), at(3600)));
	mapped.raw(pes(0xC0, cat(adts_frame(4, 1), adts_frame(4, 1)), at(3600)));

	const auto m = scan(mapped.b);
	CHECK(m.success);
	CHECK_EQ(m.streams.size(), std::size_t{2});
	if (m.streams.size() != 2) return;

	CHECK_STR(m.streams[0].codec, "hevc");
	CHECK_STR(m.streams[0].codec_tag, "0x24");
	CHECK_EQ(m.streams[0].video.width, 1920u);
	CHECK_STR(m.streams[1].codec, "aac");
	CHECK_STR(m.streams[1].language, "ita");
	CHECK_EQ(m.streams[1].audio.sample_rate, 44100u);
	CHECK_EQ(m.streams[1].audio.channels, 1u);
}

TEST("media ps reads private stream 1's other substreams")
{
	builder ps;
	ps.raw(pack_header(0));

	// AC-3 without a substream header, which FFmpeg takes for substream 0x80; EVOB's E-AC-3; DTS; TrueHD;
	// 24-bit 96 kHz LPCM; and a substream FFmpeg passes over.
	ps.raw(pes(0xBD, cat(ac3_frame(), ac3_frame()), at(9000)));
	ps.raw(pes(0xBD, ac3_substream(0xC0, ac3_frame()), at(9000)));
	ps.raw(pes(0xBD, cat(bytes{0x89, 0x01, 0x00, 0x01}, bytes{0x7F, 0xFE, 0x80, 0x01}, bytes(64, 0)), at(9000)));
	ps.raw(pes(0xBD, cat(bytes{0xB0, 0x01, 0x00, 0x01, 0x00}, bytes(16, 0)), at(9000)));
	ps.raw(pes(0xBD, lpcm_substream(0xA1, 64, 0x95), at(9000)));
	ps.raw(pes(0xBD, bytes{0x60, 0x00, 0x00}, at(9000)));

	const auto r = scan(ps.b);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size_t{5});
	if (r.streams.size() != 5) return;

	CHECK_EQ(r.streams[0].id, std::uint64_t{0x80});
	CHECK_STR(r.streams[0].codec, "ac3");
	CHECK_EQ(r.streams[1].id, std::uint64_t{0xC0});
	CHECK_STR(r.streams[1].codec, "ac3");
	CHECK_EQ(r.streams[2].id, std::uint64_t{0x89});
	CHECK_STR(r.streams[2].codec, "dts");
	CHECK_EQ(r.streams[3].id, std::uint64_t{0xB0});
	CHECK_STR(r.streams[3].codec, "truehd");
	CHECK_EQ(r.streams[4].id, std::uint64_t{0xA1});
	CHECK_EQ(r.streams[4].audio.sample_rate, 96000u);
	CHECK_EQ(r.streams[4].audio.channels, 6u);
	CHECK_EQ(r.streams[4].audio.bits_per_sample, 24);
}

TEST("media ps times streams without timestamps by the packs' clock")
{
	builder ps;

	for (std::uint64_t i = 0; i < 20; ++i)
	{
		ps.raw(pack_header((ps_start + i * 4500) * 300 + 150));
		ps.raw(pes(0xBD, lpcm_substream(0xA0, 16)));
	}

	const auto r = scan(ps.b);
	CHECK(r.success);
	CHECK(near(r.duration, 19 * 4500 / 90000.0));
	CHECK(near(r.streams[0].duration, 0));
}

TEST("media ps survives every cut and damage")
{
	const auto file = dvd_stream();
	const auto whole = scan(file);
	CHECK(whole.success);

	for (std::size_t n = 16; n < file.size(); ++n)
	{
		if (n > 600 && n + 600 < file.size() && n % 97 != 0) continue;
		check_consistent(bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n)), whole, n);
	}

	for (std::size_t at = 0; at < file.size(); at += at < 1200 ? 1 : 89)
	{
		auto damaged = file;
		damaged[at] ^= 0xFF;
		check_sane(damaged, at);
		damaged[at] = 0x01;
		check_sane(damaged, at);
	}

	// A file that only begins as a program stream.
	const auto junk = cat(bytes{0, 0, 1, 0xBA, 0x00}, bytes(4000, 0x5A));
	const auto j = scan(junk);
	CHECK(!j.success);
	CHECK(j.format == media_format::mpeg_ps);
	CHECK(!j.error.empty());

	const auto packs_only = cat(pack_header(0), pack_header(9000), padding(100));
	CHECK(!scan(packs_only).success);
}

TEST("media ps reads a large stream's ends and nothing between")
{
	// VC-1 behind EVOB's extended stream id, and DVD LPCM: neither needs a shared codec header.
	builder ps;

	const auto media = [&ps](const std::uint64_t pts, const bool first)
	{
		ps.raw(pack_header((pts - 9000) * 300));
		ps.raw(pes(0xFD, first ? vc1_sequence(720, 480, true) : bytes{0, 0, 1, 0x0D, 0x40}, at(pts).extended(0x55)));
		ps.raw(pes(0xBD, lpcm_substream(0xA0, 480), at(pts)));
	};

	media(ps_start, true);
	media(ps_start + 120000, false);

	for (auto i = 0; i < 12000; ++i)
	{
		ps.raw(pack_header((ps_start + 120000 + static_cast<std::uint64_t>(i) * 4500) * 300));
		ps.raw(padding(2034));
	}

	media(ps_start + 90000 * 600, false);

	libfiles::memory_stream file(ps.b);
	libfiles::reader r(file);
	const auto s = libfiles::scan_media(r);
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), std::size_t{2});
	if (s.streams.size() == 2)
	{
		CHECK_EQ(s.streams[0].id, std::uint64_t{0xFD55});
		CHECK_EQ(s.streams[0].video.width, 720u);
		CHECK_STR(s.streams[0].video.profile, "Advanced");
	}
	CHECK(near(s.duration, 600 + 480 / 48000.0));
	CHECK(r.source_reads() <= 6);
}

TEST("media ps finds a VCD's audio, which has a system header of its own")
{
	// The VCD standard gives each stream its system header, in its first pack: the first names the
	// video alone and bounds no audio.
	builder ps;
	ps.raw(pack_header(ps_start * 300, 3000, true));
	ps.raw(system_header({{0xE0, 46}}, 0, 1));
	ps.raw(pes_mpeg1(0xE0, mpeg_video_sequence(352, 288, 3, false), ps_start));

	for (std::uint64_t i = 1; i < 10; ++i)
	{
		ps.raw(pack_header((ps_start + i * 3600) * 300, 3000, true));
		if (i == 3) ps.raw(system_header({{0xC0, 32}}, 1, 0));
		if (i >= 3) ps.raw(pes_mpeg1(0xC0, mpeg_audio_frame(), ps_start + i * 3600));
		ps.raw(pes_mpeg1(0xE0, bytes{0, 0, 1, 0, 0x00, 0x58, 0xFF, 0xF8}, ps_start + i * 3600));
	}

	const auto r = scan(ps.b);
	CHECK(r.success);
	CHECK(r.complete);
	CHECK_EQ(r.streams.size(), std::size_t{2});
	if (r.streams.size() == 2) CHECK_STR(r.streams[1].codec, "mp2");
}

TEST("media ps stops once it has the streams its map lists")
{
	// The packs' clock barely moves, so only the map ends the search before 2 MiB.
	builder ps;
	ps.raw(pack_header(ps_start * 300));
	ps.raw(stream_map({{0x01, 0xE0, {}}, {0x03, 0xC0, {}}}));
	ps.raw(pes(0xE0, mpeg_video_sequence(352, 288, 3, false), at(ps_start)));
	ps.raw(pes(0xC0, cat(mpeg_audio_frame(), mpeg_audio_frame()), at(ps_start)));

	for (std::uint64_t i = 0; i < 12000; ++i)
	{
		ps.raw(pack_header((ps_start + i) * 300));
		ps.raw(padding(2026));
	}

	ps.raw(pack_header((ps_start + 90000 * 300) * 300));
	ps.raw(pes(0xE0, bytes{0, 0, 1, 0, 0x00, 0x58, 0xFF, 0xF8}, at(ps_start + 90000 * 300)));
	ps.raw(pes(0xC0, mpeg_audio_frame(), at(ps_start + 90000 * 300)));

	libfiles::memory_stream file(ps.b);
	libfiles::reader r(file);
	const auto s = libfiles::scan_media(r);
	CHECK(s.success);
	CHECK(s.complete);
	CHECK_EQ(s.streams.size(), std::size_t{2});
	if (s.streams.size() == 2) CHECK_STR(s.streams[0].codec_tag, "0x01");
	CHECK(near(s.duration, 300 + 0.04));
	CHECK(r.source_reads() <= 6);
}

TEST("media ps lists its packs and streams for inspection")
{
	const auto r = scan(dvd_stream(), libfiles::scan_intent::inspect);
	CHECK(r.success);
	CHECK(has_row(r, "Program stream"));
	CHECK(has_row(r, "Pack header"));
	CHECK(has_row(r, "System header"));
	CHECK(has_row(r, "Stream 0x1E0"));
	CHECK(has_row(r, "Private stream 1, substream 0x80"));
	CHECK(scan(dvd_stream()).structure.empty());
}
