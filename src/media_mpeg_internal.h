// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What the MPEG transport and program stream scanners share: PES packet headers and their
// 33-bit timestamps, the stream types and descriptors both name codecs by, reading each elementary
// stream's own header from its first bytes - with the camera metadata an AVCHD camcorder keeps in its
// H.264 stream - and the duration as FFmpeg estimates it from the first and last timestamps. Defined
// in scan_media_ts.cpp.

#pragma once

#include "media_internal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	// Timestamps count a 90 kHz clock in 33 bits; a PCR or an MPEG-2 SCR adds a 27 MHz extension.
	inline constexpr std::uint64_t mpeg_ticks_per_second = 90000;
	inline constexpr std::uint64_t mpeg_timestamp_wrap = 1ull << 33;

	// What a scan reads from the start of a stream to find its streams' headers, and from its end to
	// find their last timestamps.
	inline constexpr std::uint64_t mpeg_head_bytes = 8u * 1024u * 1024u;
	inline constexpr std::uint64_t mpeg_tail_bytes = 8u * 1024u * 1024u;
	// The tail is read backwards in growing steps, ending at these distances from the end, until every
	// audio and video stream has a last timestamp.
	inline constexpr std::uint64_t mpeg_tail_steps[] = {256u * 1024u, 1024u * 1024u, 4u * 1024u * 1024u, mpeg_tail_bytes};

	// A 33-bit timestamp from the five bytes a PES header or an MPEG-1 pack header keeps it in. The
	// marker bits are not checked: FFmpeg reads a timestamp whatever they hold.
	inline std::uint64_t read_mpeg_timestamp(const std::uint8_t* const p) noexcept
	{
		return static_cast<std::uint64_t>(p[0] >> 1 & 7) << 30 | static_cast<std::uint64_t>(p[1]) << 22 |
			static_cast<std::uint64_t>(p[2] >> 1) << 15 | static_cast<std::uint64_t>(p[3]) << 7 |
			static_cast<std::uint64_t>(p[4] >> 1);
	}

	// The CRC-32 of MPEG-2 systems' sections: 0 over a whole section, its own CRC included, when the
	// section is intact.
	std::uint32_t mpeg_crc32(std::span<const std::uint8_t> bytes) noexcept;

	// ---- PES packets ---------------------------------------------------------------------------------

	struct pes_header
	{
		std::uint8_t stream_id = 0;
		// The bytes after the length field; 0 when a transport stream leaves a video packet's open.
		std::uint16_t packet_length = 0;
		// The bytes from the start code to the payload.
		std::size_t size = 0;
		std::optional<std::uint64_t> pts;
		std::optional<std::uint64_t> dts;
		// PES extension 2's stream_id_extension, which EVOB's VC-1 and Blu-ray's TrueHD streams use.
		std::optional<std::uint8_t> extended_stream_id;
	};

	enum class pes_read : std::uint8_t
	{
		ok,
		// More bytes are needed to read the header.
		short_data,
		// Not a PES header.
		invalid,
	};

	// Reads a PES header from its start code. MPEG-1's form, without the 0x80 marker, is read too when
	// mpeg1 allows it, as an MPEG-1 system stream writes it.
	pes_read read_pes_header(std::span<const std::uint8_t> data, bool mpeg1, pes_header& out);

	// ---- Codecs as stream types and descriptors name them --------------------------------------------

	// How an elementary stream's own header is read.
	enum class es_syntax : std::uint8_t
	{
		// Nothing to read: subtitles, data, or a codec libfiles reads no header of.
		none,
		// MPEG-1 or MPEG-2 video: a sequence extension after the sequence header makes it MPEG-2.
		mpeg_video,
		h264,
		hevc,
		mpeg4_video,
		vc1,
		// Program stream video the stream map does not name: its first start codes decide.
		video,
		mpeg_audio,
		adts,
		latm,
		// AC-3 or E-AC-3: the frame decides.
		ac3,
		dts,
		// Blu-ray LPCM: a four-byte header begins every PES payload.
		lpcm_bluray,
		// DVD LPCM: a three-byte header after private stream 1's own.
		lpcm_dvd,
	};

	struct es_type
	{
		media_stream_kind kind = media_stream_kind::data;
		// Empty when libfiles does not know the codec.
		std::string_view codec;
		es_syntax syntax = es_syntax::none;

		bool known() const noexcept { return !codec.empty(); }
	};

	inline constexpr std::uint32_t mpeg_registration(const char (&s)[5]) noexcept
	{
		return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) << 24 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 16 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 8 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3]));
	}

	// A PMT's or program stream map's stream_type. A program registered as Blu-ray's ("HDMV" or "HDPR")
	// gives 0x80 to 0x86 and a few others their Blu-ray meaning.
	es_type es_type_from_stream_type(std::uint8_t stream_type, std::uint32_t program_registration);

	// A stream as its type names it: its kind and codec, and for a codec whose bitstream fixes them its
	// chroma and depth.
	void apply_es_type(const es_type& type, media_stream& stream);

	// An elementary stream's descriptors, as a PMT or a program stream map lists them.
	struct es_descriptors
	{
		// The language a supplementary audio descriptor names, else a subtitling or teletext
		// descriptor, else the first ISO 639 code other than "und".
		std::string language;
		// The format identifier of a registration descriptor, 0 when there is none.
		std::uint32_t registration = 0;
		// The codec a DVB descriptor names, for stream type 0x06.
		es_type described;
		// The codec the registration names, for a stream type that names none.
		es_type registered;
		bool audio_description = false;
		// An Opus stream's channels, from its DVB extension descriptor; 0 when not stated.
		std::uint32_t opus_channels = 0;
	};

	es_descriptors read_es_descriptors(std::span<const std::uint8_t> loop);
	// The format identifier of the first registration descriptor in a descriptor loop, 0 when none.
	std::uint32_t find_registration(std::span<const std::uint8_t> loop);
	// "registration HDMV, language eng" for the structure rows.
	std::string describe_descriptors(std::span<const std::uint8_t> loop);

	// ---- Elementary stream headers -------------------------------------------------------------------

	// Reads an elementary stream's own header - an SPS, a sequence header, a frame header - from the
	// stream's first bytes, gathered across PES packets from the start of one. At most a bounded window
	// of the stream is kept: a header not found in it is looked for in what follows.
	class es_probe
	{
	public:
		es_probe() = default;

		// camera: also read the AVCHD camera metadata, from the SEI of the stream's first access unit.
		es_probe(es_syntax syntax, bool camera);

		// The stream's next bytes, in order. Fills stream, and properties for the camera metadata.
		void add(std::span<const std::uint8_t> bytes, media_stream& stream, media_properties& properties);
		// No more bytes will come: what was gathered is read as it stands.
		void finish(media_stream& stream, media_properties& properties);

		// True once the header has been read, or there is none to read.
		bool done() const noexcept { return _done; }
		// The duration FFmpeg adds to a stream's last timestamp, in 90 kHz ticks: one frame's, or for
		// LPCM what the last packet's payload holds. 0 when unknown.
		std::uint64_t end_ticks(const media_stream& stream, std::uint64_t last_payload) const noexcept;

	private:
		void attempt(media_stream& stream, media_properties& properties, bool final);
		void read_annexb(media_stream& stream, media_properties& properties, bool final, bool hevc);
		void read_mpeg_video(media_stream& stream, bool final);
		void read_mpeg4_video(media_stream& stream, bool final);
		void read_vc1(media_stream& stream, bool final);
		void read_frames(media_stream& stream, bool final);
		void read_latm(media_stream& stream);
		void read_lpcm(media_stream& stream);
		bool identify_video(media_stream& stream, bool final);

		es_syntax _syntax = es_syntax::none;
		bool _camera = false;
		bool _done = true;
		// Whether the window still begins where a PES payload began, as a stream's first frame does.
		bool _aligned = true;
		std::vector<std::uint8_t> _data;
		std::size_t _next_attempt = 0;
		// H.264 and HEVC: the parameter set is read, and the first access unit has ended.
		bool _parameters = false;
		bool _first_unit = false;
		bool _camera_read = false;
		std::uint32_t _samples_per_frame = 0;
	};

	// The AVCHD "MDPM" metadata in an H.264 SEI NAL unit, its header byte included and emulation
	// prevention still in place: the recording date and the camera's maker and model. False when the
	// unit carries none.
	bool read_avchd_camera(std::span<const std::uint8_t> sei, media_properties& properties);

	// ---- Timestamps and duration ---------------------------------------------------------------------

	// Timestamps on one line across their wrap: a PTS's 33 bits at 90 kHz, or a PCR's or SCR's count at
	// 27 MHz of the same 33-bit base. The first one given is the reference; as FFmpeg does, a later one
	// up to 60 seconds before it counts as earlier, and any other as later.
	class timestamp_line
	{
	public:
		timestamp_line(const std::uint64_t wrap, const std::uint64_t ticks_per_second) :
			_wrap(wrap),
			_before(60 * ticks_per_second)
		{
		}

		std::int64_t place(std::uint64_t timestamp);

	private:
		std::uint64_t _wrap;
		std::uint64_t _before;
		std::optional<std::uint64_t> _reference;
	};

	// A PCR or an MPEG-2 SCR counts 27 MHz: its 33-bit base at 90 kHz times 300, plus its extension.
	inline constexpr std::uint64_t mpeg_system_clock_wrap = mpeg_timestamp_wrap * 300;
	inline constexpr std::uint64_t mpeg_system_ticks_per_second = 27000000;

	// One stream's first and last timestamps, placed on a shared line, and the payload of the packet
	// with the last.
	struct es_clock
	{
		std::optional<std::int64_t> first;
		std::optional<std::int64_t> last;
		std::uint64_t last_payload = 0;

		void see(const std::int64_t placed, const std::uint64_t payload) noexcept
		{
			if (!first) first = placed;

			if (!last || placed > *last)
			{
				last = placed;
				last_payload = payload;
			}
		}
	};

	struct timed_stream
	{
		media_stream* stream = nullptr;
		es_clock clock;
		// What FFmpeg adds to the last timestamp: es_probe::end_ticks.
		std::uint64_t end_ticks = 0;
	};

	// Each audio and video stream's duration, from its first timestamp to its last plus one frame, and
	// the file's, from the earliest first to the latest end: FFmpeg's estimate_timings_from_pts. Zero
	// when no stream has both.
	double finish_pts_durations(std::span<timed_stream> streams);
}
