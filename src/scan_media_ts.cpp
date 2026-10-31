// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning MPEG transport streams: TS, and the 192-byte packets of M2TS and MTS that Blu-ray
// discs and AVCHD camcorders write. The program association table leads to the first program's map,
// whose streams are named by their types and descriptors; each stream's own header is read from its
// first PES packets, and the duration from the timestamps at the start and at the end - never the
// media between. Also what the program stream scanner shares, declared in media_mpeg_internal.h: PES
// headers, stream types and descriptors, the elementary stream probe with the AVCHD camera metadata,
// and the duration as FFmpeg estimates it.

#include "media_mpeg_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::size_t npos = static_cast<std::size_t>(-1);

		// The most of a stream a probe holds at once, and what it keeps of a full window to look on from,
		// so that a header the window's end cut through is still found whole.
		constexpr std::size_t probe_window = 64u * 1024u;
		constexpr std::size_t probe_keep = 4u * 1024u;

		// The next 00 00 01 at or after from with its code byte; npos when there is none.
		std::size_t next_start_code(const std::span<const std::uint8_t> data, std::size_t from)
		{
			while (from + 4 <= data.size())
			{
				// A start code ends in a 1 after two zeros, so none begins in the next three bytes when the
				// third is neither.
				if (data[from + 2] > 1)
				{
					from += 3;
					continue;
				}

				if (data[from] == 0 && data[from + 1] == 0 && data[from + 2] == 1) return from;
				++from;
			}

			return npos;
		}

		template <typename Match>
		std::size_t find_start_code(const std::span<const std::uint8_t> data, const std::size_t from, Match match)
		{
			for (auto at = next_start_code(data, from); at != npos; at = next_start_code(data, at + 3))
			{
				if (match(data[at + 3])) return at;
			}

			return npos;
		}

		bool is_letter(const std::uint8_t c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		}

		// An ISO 639-2 code as a descriptor holds it; empty when the bytes are not three letters, or name
		// no language: "und", which every container's scan reports as no language at all.
		std::string language_code(const std::span<const std::uint8_t> bytes)
		{
			if (bytes.size() < 3 || !is_letter(bytes[0]) || !is_letter(bytes[1]) || !is_letter(bytes[2])) return {};
			if ((bytes[0] | 0x20) == 'u' && (bytes[1] | 0x20) == 'n' && (bytes[2] | 0x20) == 'd') return {};
			return {reinterpret_cast<const char*>(bytes.data()), 3};
		}

		constexpr es_type video_type(const std::string_view codec, const es_syntax syntax = es_syntax::none)
		{
			return {media_stream_kind::video, codec, syntax};
		}

		constexpr es_type audio_type(const std::string_view codec, const es_syntax syntax = es_syntax::none)
		{
			return {media_stream_kind::audio, codec, syntax};
		}

		constexpr es_type subtitle_type(const std::string_view codec)
		{
			return {media_stream_kind::subtitle, codec, es_syntax::none};
		}

		constexpr es_type data_type(const std::string_view codec)
		{
			return {media_stream_kind::data, codec, es_syntax::none};
		}

		// A registration descriptor's format identifier, for a stream whose type names no codec, as
		// FFmpeg's mpegts demuxer reads it.
		es_type es_type_from_registration(const std::uint32_t id)
		{
			if (id == mpeg_registration("AC-3")) return audio_type("ac3", es_syntax::ac3);
			if (id == mpeg_registration("EAC3")) return audio_type("eac3", es_syntax::ac3);
			if (id == mpeg_registration("AC-4")) return audio_type("ac4");
			if (id == mpeg_registration("DTS1") || id == mpeg_registration("DTS2") || id == mpeg_registration("DTS3"))
			{
				return audio_type("dts", es_syntax::dts);
			}
			if (id == mpeg_registration("Opus")) return audio_type("opus");
			if (id == mpeg_registration("BSSD")) return audio_type("s302m");
			if (id == mpeg_registration("HEVC")) return video_type("hevc", es_syntax::hevc);
			if (id == mpeg_registration("VC-1")) return video_type("vc1", es_syntax::vc1);
			if (id == mpeg_registration("VVC ")) return video_type("vvc");
			if (id == mpeg_registration("drac")) return video_type("dirac");
			if (id == mpeg_registration("KLVA")) return data_type("klv");
			if (id == mpeg_registration("ID3 ")) return data_type("timed_id3");
			return {};
		}

		// The DVB descriptors that name what a stream of type 0x06, private data, carries (ETSI EN 300 468).
		es_type es_type_from_descriptor(const std::uint8_t tag)
		{
			switch (tag)
			{
			case 0x56: return subtitle_type("dvb_teletext");
			case 0x59: return subtitle_type("dvb_subtitle");
			case 0x6A: return audio_type("ac3", es_syntax::ac3);
			case 0x7A: return audio_type("eac3", es_syntax::ac3);
			case 0x7B: return audio_type("dts", es_syntax::dts);
			default: return {};
			}
		}

		// ---- Audio frames ----------------------------------------------------------------------------

		struct audio_frame
		{
			audio_header header;
			std::string codec;
		};

		// The bytes each framed syntax's parser needs from its sync word on.
		std::size_t frame_header_bytes(const es_syntax syntax)
		{
			switch (syntax)
			{
			case es_syntax::mpeg_audio: return 4;
			case es_syntax::adts: return 9;
			case es_syntax::ac3:
			case es_syntax::dts: return 16;
			default: return 0;
			}
		}

		// Four bytes at p.
		bool frame_sync(const es_syntax syntax, const std::uint8_t* const p)
		{
			switch (syntax)
			{
			case es_syntax::mpeg_audio: return p[0] == 0xFF && (p[1] & 0xE0) == 0xE0;
			case es_syntax::adts: return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
			case es_syntax::ac3: return p[0] == 0x0B && p[1] == 0x77;
			case es_syntax::dts:
				{
					// A DTS core in 16-bit or 14-bit words, either byte order.
					const auto word = load_be32(p);
					return word == 0x7FFE8001 || word == 0xFE7F0180 || word == 0x1FFFE800 || word == 0xFF1F00E8;
				}
			default: return false;
			}
		}

		bool parse_frame(const es_syntax syntax, const std::span<const std::uint8_t> data, audio_frame& out)
		{
			switch (syntax)
			{
			case es_syntax::mpeg_audio: return parse_mpeg_audio_header(load_be32(data.data()), out.header, out.codec);
			case es_syntax::adts: return parse_adts_header(data, out.header);
			case es_syntax::ac3: return parse_ac3_frame(data, out.header, out.codec);
			case es_syntax::dts: return parse_dts_frame(data, out.header);
			default: return false;
			}
		}

		// LatmGetValue (ISO/IEC 14496-3, 1.7.3).
		std::uint32_t latm_value(bit_reader& bits)
		{
			const auto count = bits.u(2);
			std::uint32_t value = 0;
			for (std::uint32_t i = 0; i <= count; ++i) value = value << 8 | bits.u(8);
			return value;
		}

		// The first AudioSpecificConfig of an AudioMuxElement's StreamMuxConfig. False when the element
		// reuses an earlier frame's configuration, as most do, or is not one.
		bool read_stream_mux_config(const std::span<const std::uint8_t> element, audio_header& out)
		{
			bit_reader bits(element);

			// useSameStreamMux.
			if (bits.flag()) return false;

			const auto version = bits.u(1);

			// audioMuxVersionA, whose 1 is reserved; then taraBufferFullness.
			if (version != 0 && bits.u(1) != 0) return false;
			if (version != 0) latm_value(bits);

			// allStreamsSameTimeFraming, numSubFrames, numProgram and the first program's numLayer: the
			// first layer's configuration follows, after its length in bits when the version is 1.
			bits.skip(1 + 6 + 4 + 3);
			if (version != 0) latm_value(bits);
			if (bits.overrun()) return false;

			// The configuration starts at any bit; parse_aac_config reads it from a byte.
			std::array<std::uint8_t, 32> config{};
			const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(bits.remaining() / 8, config.size()));
			for (std::size_t i = 0; i < size; ++i) config[i] = static_cast<std::uint8_t>(bits.u(8));
			return parse_aac_config(std::span<const std::uint8_t>(config).first(size), out);
		}

		// ---- AVCHD camera metadata -------------------------------------------------------------------

		// "Modified DV pack metadata": an AVCHD camcorder's SEI user data, after this UUID, holds DV packs
		// of a tag and four bytes each (IEC 61834-4 as AVCHD adapts it; ExifTool's H264.pm and MediaInfo
		// read them the same way).
		constexpr std::array<std::uint8_t, 16> mdpm_uuid = {
			0x17, 0xEE, 0x8C, 0x60, 0xF8, 0x4D, 0x11, 0xD9, 0x8C, 0xD6, 0x08, 0x00, 0x20, 0x0C, 0x9A, 0x66
		};

		int from_bcd(const std::uint8_t b)
		{
			if ((b >> 4) > 9 || (b & 0x0F) > 9) return -1;
			return (b >> 4) * 10 + (b & 0x0F);
		}

		std::string_view mdpm_maker(const std::uint16_t code)
		{
			switch (code)
			{
			case 0x0103: return "Panasonic";
			case 0x0108: return "Sony";
			case 0x1011: return "Canon";
			case 0x1104: return "JVC";
			default: return {};
			}
		}

		// The recording date: pack 0x18 holds a time zone byte, the year in four BCD digits and the month;
		// pack 0x19 the day, hour, minute and second.
		media_date mdpm_date(const std::uint8_t* const date, const std::uint8_t* const time)
		{
			const auto century = from_bcd(date[1]);
			const auto year = from_bcd(date[2]);
			const auto month = from_bcd(date[3]);
			const auto day = from_bcd(time[0]);
			const auto hour = from_bcd(time[1]);
			const auto minute = from_bcd(time[2]);
			const auto second = from_bcd(time[3]);

			media_date d;
			if (century < 0 || year < 0 || century * 100 + year == 0) return d;
			if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
				second < 0 || second > 59)
			{
				return d;
			}

			d.year = century * 100 + year;
			d.month = month;
			d.day = day;
			d.has_time = true;
			d.hour = hour;
			d.minute = minute;
			d.second = second;

			// The zone byte: bit 7 clear, then daylight saving, the sign, the hours and a half hour. Daylight
			// saving is in effect on top of the zone's standard time. All zeros is what cameras that keep no
			// zone write - Panasonic's do - so it states nothing.
			const auto zone = date[0];
			const auto hours = zone >> 1 & 0x0F;
			const auto west = (zone & 0x20) != 0;

			if ((zone & 0x80) == 0 && zone != 0 && hours <= (west ? 12 : 14))
			{
				const auto minutes = hours * 60 + (zone & 1 ? 30 : 0);
				d.zone = media_date::time_zone::offset;
				d.offset_minutes = (west ? -minutes : minutes) + (zone & 0x40 ? 60 : 0);
			}

			return d;
		}

		bool read_mdpm(const std::span<const std::uint8_t> payload, media_properties& properties)
		{
			if (payload.size() < 21 || !std::equal(mdpm_uuid.begin(), mdpm_uuid.end(), payload.begin()) ||
				!starts_with(payload.subspan(16), "MDPM"))
			{
				return false;
			}

			const std::uint8_t* date = nullptr;
			const std::uint8_t* time = nullptr;
			std::uint16_t maker = 0;
			std::array<std::uint8_t, 12> model{};
			auto has_model = false;

			const std::size_t count = payload[20];

			for (std::size_t i = 0; i < count && 21 + i * 5 + 5 <= payload.size(); ++i)
			{
				const auto* const pack = payload.data() + 21 + i * 5;

				switch (pack[0])
				{
				case 0x18: date = pack + 1;
					break;
				case 0x19: time = pack + 1;
					break;
				case 0xE0: maker = load_be16(pack + 1);
					break;
				case 0xE4:
				case 0xE5:
				case 0xE6:
					std::copy_n(pack + 1, 4, model.begin() + (pack[0] - 0xE4) * 4);
					has_model = true;
					break;
				default:
					break;
				}
			}

			if (date && time && !properties.created.valid()) properties.created = mdpm_date(date, time);

			const auto maker_name = mdpm_maker(maker);
			if (!maker_name.empty() && properties.camera_make.empty()) properties.camera_make = maker_name;

			// Packs 0xE4 to 0xE6 hold the model's name in Sony's cameras; others use them otherwise.
			if (has_model && maker == 0x0108 && properties.camera_model.empty())
			{
				properties.camera_model = trim_text(text_from_windows_1252(model));
			}

			return true;
		}
	}

	std::uint32_t mpeg_crc32(const std::span<const std::uint8_t> bytes) noexcept
	{
		std::uint32_t crc = 0xFFFFFFFF;

		for (const auto b : bytes)
		{
			crc ^= static_cast<std::uint32_t>(b) << 24;
			for (auto i = 0; i < 8; ++i) crc = (crc & 0x80000000) != 0 ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
		}

		return crc;
	}

	pes_read read_pes_header(const std::span<const std::uint8_t> data, const bool mpeg1, pes_header& out)
	{
		if (data.size() < 6) return pes_read::short_data;
		if (data[0] != 0 || data[1] != 0 || data[2] != 1 || data[3] < 0xBC) return pes_read::invalid;

		pes_header h;
		h.stream_id = data[3];
		h.packet_length = load_be16(data.data() + 4);

		// The streams whose packets have no optional header: the program stream map, padding, private
		// stream 2, ECM, EMM, DSM-CC, H.222.1 type E and the program stream directory.
		switch (h.stream_id)
		{
		case 0xBC:
		case 0xBE:
		case 0xBF:
		case 0xF0:
		case 0xF1:
		case 0xF2:
		case 0xF8:
		case 0xFF:
			h.size = 6;
			out = h;
			return pes_read::ok;
		default:
			break;
		}

		// A stated length bounds the header; an open one does not.
		const auto fits = [&h](const std::size_t size) { return h.packet_length == 0 || size <= 6u + h.packet_length; };

		if (data.size() < 7) return pes_read::short_data;

		if ((data[6] & 0xC0) == 0x80)
		{
			if (data.size() < 9) return pes_read::short_data;

			const auto flags = data[7];
			h.size = 9u + data[8];
			if (!fits(h.size)) return pes_read::invalid;
			if (data.size() < h.size) return pes_read::short_data;

			std::size_t at = 9;

			// '01' is forbidden, and FFmpeg reads no timestamp from it either.
			if (flags >> 6 >= 2)
			{
				if (at + 5 > h.size) return pes_read::invalid;
				h.pts = read_mpeg_timestamp(data.data() + at);
				at += 5;
			}

			if (flags >> 6 == 3)
			{
				if (at + 5 > h.size) return pes_read::invalid;
				h.dts = read_mpeg_timestamp(data.data() + at);
				at += 5;
			}

			// ESCR, ES rate, DSM trick mode, additional copy info and the previous packet's CRC.
			if (flags & 0x20) at += 6;
			if (flags & 0x10) at += 3;
			if (flags & 0x08) at += 1;
			if (flags & 0x04) at += 1;
			if (flags & 0x02) at += 2;

			if ((flags & 0x01) && at < h.size)
			{
				const auto extension = data[at++];

				// PES private data, a pack header, the program packet sequence counter and the P-STD buffer.
				if (extension & 0x80) at += 16;
				if ((extension & 0x40) && at < h.size) at += 1u + data[at];
				if (extension & 0x20) at += 2;
				if (extension & 0x10) at += 2;

				if ((extension & 0x01) && at + 2 <= h.size && (data[at] & 0x7F) != 0 && (data[at + 1] & 0x80) == 0)
				{
					h.extended_stream_id = data[at + 1];
				}
			}

			out = h;
			return pes_read::ok;
		}

		if (!mpeg1) return pes_read::invalid;

		// MPEG-1's form: up to 16 stuffing bytes, the STD buffer's size, then the timestamps or 0x0F.
		std::size_t at = 6;

		for (auto stuffing = 0;; ++stuffing)
		{
			if (at >= data.size()) return pes_read::short_data;
			if (data[at] != 0xFF) break;
			if (stuffing == 16) return pes_read::invalid;
			++at;
		}

		if ((data[at] & 0xC0) == 0x40)
		{
			at += 2;
			if (at >= data.size()) return pes_read::short_data;
		}

		const auto marker = data[at] & 0xF0;

		if (marker == 0x20 || marker == 0x30)
		{
			const auto end = at + (marker == 0x30 ? 10u : 5u);
			if (data.size() < end) return pes_read::short_data;
			h.pts = read_mpeg_timestamp(data.data() + at);
			if (marker == 0x30) h.dts = read_mpeg_timestamp(data.data() + at + 5);
			at = end;
		}
		else if (data[at] == 0x0F)
		{
			++at;
		}
		else
		{
			return pes_read::invalid;
		}

		h.size = at;
		if (!fits(h.size)) return pes_read::invalid;
		out = h;
		return pes_read::ok;
	}

	es_type es_type_from_stream_type(const std::uint8_t stream_type, const std::uint32_t program_registration)
	{
		if (program_registration == mpeg_registration("HDMV") || program_registration == mpeg_registration("HDPR"))
		{
			switch (stream_type)
			{
			case 0x80: return audio_type("pcm", es_syntax::lpcm_bluray);
			case 0x81: return audio_type("ac3", es_syntax::ac3);
			case 0x82:
			case 0x85:
			case 0x86:
			case 0xA2: return audio_type("dts", es_syntax::dts);
			case 0x83: return audio_type("truehd");
			case 0x84:
			case 0xA1: return audio_type("eac3", es_syntax::ac3);
			case 0x90: return subtitle_type("hdmv_pgs_subtitle");
			case 0x92: return subtitle_type("hdmv_text_subtitle");
			default: break;
			}
		}

		switch (stream_type)
		{
		case 0x01: return video_type("mpeg1", es_syntax::mpeg_video);
		case 0x02: return video_type("mpeg2", es_syntax::mpeg_video);
		// The layer is the frame header's to say; until one is read, MP2 is what broadcasts and discs carry.
		case 0x03:
		case 0x04: return audio_type("mp2", es_syntax::mpeg_audio);
		case 0x0F: return audio_type("aac", es_syntax::adts);
		case 0x10: return video_type("mpeg4", es_syntax::mpeg4_video);
		case 0x11: return audio_type("aac", es_syntax::latm);
		case 0x1B:
		case 0x20: return video_type("h264", es_syntax::h264);
		case 0x1C: return audio_type("aac");
		case 0x21: return video_type("jpeg2000");
		case 0x24: return video_type("hevc", es_syntax::hevc);
		case 0x33: return video_type("vvc");
		case 0x42: return video_type("cavs");
		case 0xD1: return video_type("dirac");
		case 0xD2: return video_type("avs2");
		case 0xD4: return video_type("avs3");
		case 0xEA: return video_type("vc1", es_syntax::vc1);
		// ATSC's AC-3 and E-AC-3, and the DTS FFmpeg finds beside them.
		case 0x81: return audio_type("ac3", es_syntax::ac3);
		case 0x87: return audio_type("eac3", es_syntax::ac3);
		case 0x8A: return audio_type("dts", es_syntax::dts);
		case 0x86:
			if (program_registration == mpeg_registration("CUEI")) return data_type("scte_35");
			return {};
		default: return {};
		}
	}

	void apply_es_type(const es_type& type, media_stream& stream)
	{
		stream.kind = type.kind;
		stream.codec = type.codec;

		video_header fixed;
		if (type.kind == media_stream_kind::video && fixed_video_format(type.codec, fixed)) apply_video_header(fixed, stream.video);
	}

	es_descriptors read_es_descriptors(const std::span<const std::uint8_t> loop)
	{
		es_descriptors d;
		std::string subtitle_language;
		std::string supplementary_language;

		for (std::size_t at = 0; at + 2 <= loop.size();)
		{
			const auto tag = loop[at];
			const std::size_t length = loop[at + 1];
			if (length > loop.size() - at - 2) break;

			const auto body = loop.subspan(at + 2, length);
			at += 2 + length;

			switch (tag)
			{
			case 0x05:
				if (length >= 4 && d.registration == 0) d.registration = load_be32(body.data());
				break;
			case 0x0A:
				// ISO 639: a code and an audio type each; type 3 is a commentary for the visually impaired.
				for (std::size_t i = 0; i + 4 <= length; i += 4)
				{
					if (d.language.empty()) d.language = language_code(body.subspan(i, 3));
					if (body[i + 3] == 3) d.audio_description = true;
				}
				break;
			case 0x56:
			case 0x59:
				if (!d.described.known()) d.described = es_type_from_descriptor(tag);
				if (subtitle_language.empty()) subtitle_language = language_code(body);
				break;
			case 0x6A:
			case 0x7A:
				if (!d.described.known()) d.described = es_type_from_descriptor(tag);

				// A component type whose service type is 2 serves the visually impaired.
				if (length >= 2 && (body[0] & 0x80) && (body[1] >> 3 & 7) == 2) d.audio_description = true;
				break;
			case 0x7B:
				if (!d.described.known()) d.described = es_type_from_descriptor(tag);
				break;
			case 0x7F:
				// DVB extensions: Opus's channel configuration, and the supplementary audio descriptor.
				if (length >= 2 && body[0] == 0x80 && body[1] <= 8) d.opus_channels = body[1] != 0 ? body[1] : 2u;

				if (length >= 2 && body[0] == 0x06)
				{
					if ((body[1] >> 2 & 0x1F) == 1) d.audio_description = true;
					if ((body[1] & 1) && length >= 5) supplementary_language = language_code(body.subspan(2, 3));
				}
				break;
			default:
				break;
			}
		}

		// The more particular descriptors' languages stand over ISO 639's, as FFmpeg sets them.
		if (!subtitle_language.empty()) d.language = subtitle_language;
		if (!supplementary_language.empty()) d.language = supplementary_language;
		if (d.registration != 0) d.registered = es_type_from_registration(d.registration);
		return d;
	}

	std::uint32_t find_registration(const std::span<const std::uint8_t> loop)
	{
		for (std::size_t at = 0; at + 2 <= loop.size();)
		{
			const std::size_t length = loop[at + 1];
			if (length > loop.size() - at - 2) break;
			if (loop[at] == 0x05 && length >= 4) return load_be32(loop.data() + at + 2);
			at += 2 + length;
		}

		return 0;
	}

	std::string describe_descriptors(const std::span<const std::uint8_t> loop)
	{
		std::string text;

		const auto add = [&text](const std::string_view part)
		{
			if (!text.empty()) text += ", ";
			text += part;
		};

		for (std::size_t at = 0; at + 2 <= loop.size();)
		{
			const auto tag = loop[at];
			const std::size_t length = loop[at + 1];

			if (length > loop.size() - at - 2)
			{
				add("damaged descriptor");
				break;
			}

			const auto body = loop.subspan(at + 2, length);
			at += 2 + length;

			switch (tag)
			{
			case 0x05: add(length >= 4 ? "registration " + box_type_text(load_be32(body.data())) : "registration");
				break;
			case 0x0A:
				if (length >= 3 && is_letter(body[0]) && is_letter(body[1]) && is_letter(body[2]))
				{
					add("language " + std::string(reinterpret_cast<const char*>(body.data()), 3));
				}
				else
				{
					add("language");
				}
				break;
			case 0x56: add("teletext");
				break;
			case 0x59: add("DVB subtitles");
				break;
			case 0x6A: add("AC-3");
				break;
			case 0x7A: add("E-AC-3");
				break;
			case 0x7B: add("DTS");
				break;
			default: add(std::format("descriptor 0x{:02X}", tag));
				break;
			}
		}

		return text;
	}

	es_probe::es_probe(const es_syntax syntax, const bool camera) :
		_syntax(syntax),
		_camera(camera && (syntax == es_syntax::h264 || syntax == es_syntax::video)),
		_done(syntax == es_syntax::none)
	{
	}

	void es_probe::add(std::span<const std::uint8_t> bytes, media_stream& stream, media_properties& properties)
	{
		while (!_done && !bytes.empty())
		{
			const auto take = std::min(bytes.size(), probe_window - _data.size());
			_data.insert(_data.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(take));
			bytes = bytes.subspan(take);

			// Each look doubles what it reads, so the window is searched a few times rather than at every
			// packet.
			if (_data.size() >= _next_attempt || _data.size() >= probe_window)
			{
				attempt(stream, properties, false);
				_next_attempt = std::max<std::size_t>(_data.size() * 2, 256);
			}

			if (!_done && _data.size() >= probe_window)
			{
				_data.erase(_data.begin(), _data.end() - static_cast<std::ptrdiff_t>(probe_keep));
				_aligned = false;
				_next_attempt = probe_window;
			}
		}
	}

	void es_probe::finish(media_stream& stream, media_properties& properties)
	{
		if (!_done && !_data.empty()) attempt(stream, properties, true);
		_done = true;
		_data = {};
	}

	std::uint64_t es_probe::end_ticks(const media_stream& stream, const std::uint64_t last_payload) const noexcept
	{
		if (stream.kind == media_stream_kind::video)
		{
			if (!(stream.video.frame_rate > 0)) return 0;
			return static_cast<std::uint64_t>(std::floor(static_cast<double>(mpeg_ticks_per_second) / stream.video.frame_rate + 1e-6));
		}

		const auto& audio = stream.audio;
		if (stream.kind != media_stream_kind::audio || audio.sample_rate == 0) return 0;

		std::uint64_t samples = _samples_per_frame;

		// LPCM packets hold what their size says, as FFmpeg counts them: Blu-ray's in pairs of channels
		// after a 4-byte header, DVD's in pairs of samples after a 3-byte one.
		if (_syntax == es_syntax::lpcm_bluray && audio.bits_per_sample >= 8 && audio.channels != 0 && last_payload >= 4)
		{
			samples = (last_payload - 4) / ((audio.channels + (audio.channels & 1)) * audio.bits_per_sample / 8);
		}
		else if (_syntax == es_syntax::lpcm_dvd && audio.bits_per_sample >= 8 && audio.channels != 0 && last_payload >= 3)
		{
			samples = 2 * ((last_payload - 3) / (audio.bits_per_sample * 2u / 8u * audio.channels));
		}

		return samples * mpeg_ticks_per_second / audio.sample_rate;
	}

	void es_probe::attempt(media_stream& stream, media_properties& properties, const bool final)
	{
		if (_syntax == es_syntax::video && !identify_video(stream, final)) return;

		switch (_syntax)
		{
		case es_syntax::h264: read_annexb(stream, properties, final, false);
			break;
		case es_syntax::hevc: read_annexb(stream, properties, final, true);
			break;
		case es_syntax::mpeg_video: read_mpeg_video(stream, final);
			break;
		case es_syntax::mpeg4_video: read_mpeg4_video(stream, final);
			break;
		case es_syntax::vc1: read_vc1(stream, final);
			break;
		case es_syntax::mpeg_audio:
		case es_syntax::adts:
		case es_syntax::ac3:
		case es_syntax::dts: read_frames(stream, final);
			break;
		case es_syntax::latm: read_latm(stream);
			break;
		case es_syntax::lpcm_bluray:
		case es_syntax::lpcm_dvd: read_lpcm(stream);
			break;
		default: _done = true;
			break;
		}

		if (_done) _data = {};
	}

	bool es_probe::identify_video(media_stream& stream, const bool final)
	{
		// Program stream video the map does not name, told apart as FFmpeg's probes tell it: a sequence
		// header is MPEG-1 or MPEG-2 video, a visual object sequence MPEG-4 Part 2, a parameter set H.264
		// or HEVC; and a video object layer after a video object MPEG-4 Part 2 again. An H.264 parameter
		// set is believed only with a profile H.264 has, since MPEG-2's slice start codes look like one.
		const std::span<const std::uint8_t> data(_data);
		auto type = es_type{};

		static constexpr std::uint8_t profiles[] = {44, 66, 77, 83, 86, 88, 100, 110, 118, 122, 128, 134, 135, 138, 139, 144, 244};

		if (find_start_code(data, 0, [](const std::uint8_t c) { return c == 0xB3; }) != npos)
		{
			type = video_type("mpeg2", es_syntax::mpeg_video);
		}
		else if (find_start_code(data, 0, [](const std::uint8_t c) { return c == 0xB0; }) != npos)
		{
			type = video_type("mpeg4", es_syntax::mpeg4_video);
		}
		else
		{
			std::uint8_t previous = 0xFF;

			for (auto at = next_start_code(data, 0); at != npos && !type.known(); at = next_start_code(data, at + 3))
			{
				const auto code = data[at + 3];
				const auto next = at + 4 < data.size() ? data[at + 4] : 0;
				const auto after = at + 5 < data.size() ? data[at + 5] : 0;

				if ((code & 0x9F) == 0x07 && std::ranges::find(profiles, next) != std::end(profiles) && (after & 0x03) == 0)
				{
					type = video_type("h264", es_syntax::h264);
				}
				else if ((code == 0x40 || code == 0x42 || code == 0x46) && next == 0x01)
				{
					type = video_type("hevc", es_syntax::hevc);
				}
				else if (code >= 0x20 && code <= 0x2F && previous <= 0x1F)
				{
					type = video_type("mpeg4", es_syntax::mpeg4_video);
				}

				previous = code;
			}
		}

		if (!type.known())
		{
			if (final) _done = true;
			return false;
		}

		apply_es_type(type, stream);
		_syntax = type.syntax;
		return true;
	}

	void es_probe::read_annexb(media_stream& stream, media_properties& properties, const bool final, const bool hevc)
	{
		const std::span<const std::uint8_t> data(_data);
		const auto* const end = data.data() + data.size();

		for_each_annexb_nal(data, [&](const std::span<const std::uint8_t> nal)
		{
			if (nal.empty() || (nal[0] & 0x80)) return true;

			// A unit is whole when a start code follows it; the last in the window may still be arriving.
			const auto whole = final || end - (nal.data() + nal.size()) >= 3;

			if (hevc)
			{
				if (nal.size() >= 2 && (nal[0] >> 1 & 0x3F) == 33 && !_parameters && whole)
				{
					video_header header;

					if (parse_hevc_sps(nal, header))
					{
						apply_video_header(header, stream.video);
						_parameters = true;
					}
				}

				return !_parameters;
			}

			const auto type = nal[0] & 0x1F;

			if (type == 7 && !_parameters && whole)
			{
				video_header header;

				if (parse_h264_sps(nal, header))
				{
					apply_video_header(header, stream.video);
					_parameters = true;
				}
			}
			else if (type == 6 && _camera && !_first_unit && !_camera_read && whole)
			{
				_camera_read = read_avchd_camera(nal, properties);
			}
			else if (type >= 1 && type <= 5)
			{
				// A slice: the first access unit's SEI, where AVCHD keeps the camera's metadata, is behind.
				_first_unit = true;
			}

			return !(_parameters && (_first_unit || !_camera));
		});

		_done = _parameters && (hevc || _first_unit || !_camera);
	}

	void es_probe::read_mpeg_video(media_stream& stream, const bool final)
	{
		const std::span<const std::uint8_t> data(_data);
		const auto sequence_header = [](const std::uint8_t c) { return c == 0xB3; };

		for (auto at = find_start_code(data, 0, sequence_header); at != npos; at = find_start_code(data, at + 4, sequence_header))
		{
			// The header's fixed eight bytes, then up to two quantiser matrices; the start code after them
			// says whether a sequence extension follows, which MPEG-2 has and MPEG-1 has not.
			const auto next = next_start_code(data, at + 12);
			const auto extension = next != npos && data[next + 3] == 0xB5;
			if (!final && (next == npos || (extension && next + 10 > data.size()))) return;

			video_header header;
			if (!parse_mpeg12_video(data.subspan(at), header)) continue;

			apply_video_header(header, stream.video);
			stream.codec = extension && next + 4 < data.size() && (data[next + 4] >> 4) == 1 ? "mpeg2" : "mpeg1";
			_done = true;
			return;
		}
	}

	void es_probe::read_mpeg4_video(media_stream& stream, const bool final)
	{
		// From the first visual object sequence, visual object or video object start code, through a
		// whole video object layer.
		const std::span<const std::uint8_t> data(_data);
		const auto first = find_start_code(data, 0, [](const std::uint8_t c) { return c <= 0x2F || c == 0xB0 || c == 0xB5; });
		if (first == npos) return;

		const auto layer = find_start_code(data, first, [](const std::uint8_t c) { return c >= 0x20 && c <= 0x2F; });
		if (layer == npos || (!final && next_start_code(data, layer + 4) == npos)) return;

		video_header header;
		if (!parse_mpeg4_video(data.subspan(first), header)) return;

		apply_video_header(header, stream.video);
		_done = true;
	}

	void es_probe::read_vc1(media_stream& stream, const bool final)
	{
		// An Advanced profile sequence header and the entry point after it, which FFmpeg's decoder needs
		// both of, as a VfW record holds them.
		const std::span<const std::uint8_t> data(_data);
		const auto sequence = find_start_code(data, 0, [](const std::uint8_t c) { return c == 0x0F; });
		if (sequence == npos) return;

		const auto entry = find_start_code(data, sequence + 4, [](const std::uint8_t c) { return c == 0x0E; });
		if (!final && (entry == npos || next_start_code(data, entry + 4) == npos)) return;

		video_header header;
		if (!parse_vc1_sequence(data.subspan(sequence), header)) return;

		apply_video_header(header, stream.video);
		_done = true;
	}

	void es_probe::read_frames(media_stream& stream, const bool final)
	{
		const std::span<const std::uint8_t> data(_data);
		const auto need = frame_header_bytes(_syntax);

		for (std::size_t at = 0; at + 4 <= data.size(); ++at)
		{
			if (!frame_sync(_syntax, data.data() + at)) continue;

			if (at + need > data.size())
			{
				if (!final) return;
				break;
			}

			audio_frame frame;
			if (!parse_frame(_syntax, data.subspan(at), frame)) continue;

			// A header is believed when the next frame's follows it, or when it opens the stream's first
			// payload, where a stream's first frame begins; otherwise the next frame is waited for.
			const std::size_t length = frame.header.frame_bytes;

			if (length != 0 && length < data.size() - at && data.size() - at - length >= need)
			{
				audio_frame following;
				const auto next = at + length;
				if (!frame_sync(_syntax, data.data() + next) || !parse_frame(_syntax, data.subspan(next), following)) continue;
			}
			else if (!final && !(at == 0 && _aligned))
			{
				return;
			}

			apply_audio_header(frame.header, stream.audio);
			if (!frame.codec.empty()) stream.codec = frame.codec;

			// MPEG audio, AC-3 and DTS frames state the stream's bit rate; an ADTS frame's length says only
			// its own, which in a variable rate is not the stream's.
			if (frame.header.bit_rate != 0 && _syntax != es_syntax::adts) stream.bit_rate = frame.header.bit_rate;
			_samples_per_frame = frame.header.samples_per_frame;
			_done = true;
			return;
		}
	}

	void es_probe::read_latm(media_stream& stream)
	{
		// LOAS frames: the sync word 0x2B7 in 11 bits and a 13-bit length, then an AudioMuxElement.
		const std::span<const std::uint8_t> data(_data);

		for (std::size_t at = 0; at + 3 <= data.size();)
		{
			if (data[at] != 0x56 || (data[at + 1] & 0xE0) != 0xE0)
			{
				++at;
				continue;
			}

			const std::size_t length = static_cast<std::size_t>(data[at + 1] & 0x1F) << 8 | data[at + 2];
			if (at + 3 + length > data.size()) return;

			audio_header header;

			if (read_stream_mux_config(data.subspan(at + 3, length), header))
			{
				apply_audio_header(header, stream.audio);
				_samples_per_frame = header.samples_per_frame;
				_done = true;
				return;
			}

			at += 3 + length;
		}
	}

	void es_probe::read_lpcm(media_stream& stream)
	{
		auto& audio = stream.audio;

		if (_syntax == es_syntax::lpcm_bluray)
		{
			// Blu-ray's header: the payload's size, a channel layout and a rate, then the sample size.
			if (_data.size() < 4) return;

			static constexpr std::uint8_t channels[16] = {0, 1, 0, 2, 3, 3, 4, 4, 5, 6, 7, 8, 0, 0, 0, 0};
			const auto rate = _data[2] & 0x0F;
			const auto bits = _data[3] >> 6;

			audio.sample_rate = rate == 1 ? 48000 : rate == 4 ? 96000 : rate == 5 ? 192000 : 0;
			audio.channels = channels[_data[2] >> 4];
			audio.bits_per_sample = static_cast<std::uint8_t>(bits == 0 ? 0 : 12 + bits * 4);
		}
		else
		{
			// DVD's: emphasis, mute and the frame number; then the sample size, the rate and the channels.
			if (_data.size() < 3) return;

			static constexpr std::uint32_t rates[4] = {48000, 96000, 44100, 32000};
			const auto quantization = _data[1] >> 6;

			audio.sample_rate = rates[_data[1] >> 4 & 3];
			audio.channels = (_data[1] & 7) + 1u;
			audio.bits_per_sample = static_cast<std::uint8_t>(quantization == 3 ? 0 : 16 + quantization * 4);
		}

		audio.float_samples = false;
		_done = true;
	}

	bool read_avchd_camera(const std::span<const std::uint8_t> sei, media_properties& properties)
	{
		if (sei.size() < 2) return false;

		// The payload as the SEI syntax reads it, without the emulation prevention bytes.
		std::vector<std::uint8_t> rbsp;
		rbsp.reserve(sei.size());
		auto zeros = 0;

		for (std::size_t i = 1; i < sei.size(); ++i)
		{
			const auto b = sei[i];

			if (zeros >= 2 && b == 3)
			{
				zeros = 0;
				continue;
			}

			rbsp.push_back(b);
			zeros = b == 0 ? zeros + 1 : 0;
		}

		// SEI messages: a type and a size, each in bytes of 255 and a last, then the payload.
		for (std::size_t at = 0; at + 2 <= rbsp.size();)
		{
			std::size_t type = 0;
			std::size_t size = 0;

			while (at < rbsp.size() && rbsp[at] == 0xFF) type += rbsp[at++];
			if (at >= rbsp.size()) return false;
			type += rbsp[at++];

			while (at < rbsp.size() && rbsp[at] == 0xFF) size += rbsp[at++];
			if (at >= rbsp.size()) return false;
			size += rbsp[at++];

			if (size > rbsp.size() - at) return false;

			// Type 5 is user data registered by a UUID.
			if (type == 5 && read_mdpm(std::span<const std::uint8_t>(rbsp).subspan(at, size), properties)) return true;
			at += size;
		}

		return false;
	}

	std::int64_t timestamp_line::place(const std::uint64_t timestamp)
	{
		const auto t = timestamp % _wrap;
		if (!_reference) _reference = t;

		const auto ahead = (t + _wrap - *_reference) % _wrap;
		const auto offset = ahead >= _wrap - _before
			                    ? static_cast<std::int64_t>(ahead) - static_cast<std::int64_t>(_wrap)
			                    : static_cast<std::int64_t>(ahead);
		return static_cast<std::int64_t>(*_reference) + offset;
	}

	double finish_pts_durations(const std::span<timed_stream> streams)
	{
		std::optional<std::int64_t> start;
		std::optional<std::int64_t> end;

		for (auto& t : streams)
		{
			if (!t.stream || !t.clock.first || !t.clock.last) continue;
			if (t.stream->kind != media_stream_kind::video && t.stream->kind != media_stream_kind::audio) continue;

			const auto first = *t.clock.first;
			const auto stream_end = *t.clock.last + static_cast<std::int64_t>(t.end_ticks);
			if (stream_end <= first) continue;

			t.stream->duration = static_cast<double>(stream_end - first) / static_cast<double>(mpeg_ticks_per_second);
			start = std::min(start.value_or(first), first);
			end = std::max(end.value_or(stream_end), stream_end);
		}

		if (!start || !end || *end <= *start) return 0;
		return static_cast<double>(*end - *start) / static_cast<double>(mpeg_ticks_per_second);
	}

	namespace
	{
		constexpr std::uint8_t sync_byte = 0x47;
		constexpr std::size_t packet_size = 188;
		constexpr std::uint16_t null_pid = 0x1FFF;
		// A program-specific section: three header bytes and at most 1021 more.
		constexpr std::size_t max_section = 1024;
		constexpr std::size_t max_pes_header = 9 + 255;
		// How far a resynchronisation looks past damage for the next run of sync bytes, as FFmpeg's, and
		// the pieces it reads at a time.
		constexpr std::uint64_t resync_bytes = 64u * 1024u;
		constexpr std::uint64_t resync_piece = 4096;
		// FFmpeg's MAX_STREAMS_PER_PROGRAM, and the programs whose maps are read while one is chosen.
		constexpr std::size_t max_streams = 128;
		constexpr std::size_t max_programs = 64;
		// The stream_id_extension of the AC-3 core Blu-ray interleaves with a TrueHD stream.
		constexpr std::uint8_t truehd_ac3_core = 0x76;

		std::uint64_t read_pcr(const std::uint8_t* const p)
		{
			const auto base = static_cast<std::uint64_t>(p[0]) << 25 | static_cast<std::uint64_t>(p[1]) << 17 |
				static_cast<std::uint64_t>(p[2]) << 9 | static_cast<std::uint64_t>(p[3]) << 1 | static_cast<std::uint64_t>(p[4] >> 7);
			return base * 300 + (static_cast<std::uint64_t>(p[4] & 1) << 8 | p[5]);
		}

		enum class continuity : std::uint8_t
		{
			next,
			// The standard lets a packet be sent twice in a row.
			duplicate,
			// Packets were lost.
			broken,
		};

		continuity check_continuity(int& last, const int counter, const bool discontinuity)
		{
			const auto previous = last;
			last = counter;
			if (previous < 0 || discontinuity) return continuity::next;
			if (counter == previous) return continuity::duplicate;
			return counter == ((previous + 1) & 0x0F) ? continuity::next : continuity::broken;
		}

		// One PID's program-specific sections, put back together from its packets' payloads.
		class section_reader
		{
		public:
			template <typename Take>
			void add(std::span<const std::uint8_t> payload, const bool start, Take take)
			{
				if (start)
				{
					if (payload.empty()) return;

					// The pointer field: the bytes before the next section end the one in progress.
					const std::size_t pointer = payload[0];
					payload = payload.subspan(1);

					if (pointer > payload.size())
					{
						reset();
						return;
					}

					if (_active) append(payload.first(pointer), take);
					_bytes.clear();
					_active = true;
					payload = payload.subspan(pointer);
				}

				if (_active) append(payload, take);
			}

			void reset()
			{
				_bytes.clear();
				_active = false;
			}

		private:
			template <typename Take>
			void append(std::span<const std::uint8_t> bytes, Take take)
			{
				while (_active && !bytes.empty())
				{
					if (_bytes.size() < 3)
					{
						const auto n = std::min(3 - _bytes.size(), bytes.size());
						_bytes.insert(_bytes.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
						bytes = bytes.subspan(n);
						if (_bytes.size() < 3) return;

						// A table id of 0xFF is stuffing: no section follows in this packet.
						if (_bytes[0] == 0xFF)
						{
							reset();
							return;
						}
					}

					const auto total = 3u + (load_be16(_bytes.data() + 1) & 0x0FFFu);

					if (total > max_section)
					{
						reset();
						return;
					}

					const auto n = std::min(total - _bytes.size(), bytes.size());
					_bytes.insert(_bytes.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
					bytes = bytes.subspan(n);
					if (_bytes.size() < total) return;

					take(std::span<const std::uint8_t>(_bytes));
					_bytes.clear();
				}
			}

			std::vector<std::uint8_t> _bytes;
			bool _active = false;
		};

		// A PID that carries program maps, and its sections in progress.
		struct map_source
		{
			std::uint16_t pid = 0;
			section_reader sections;
			int counter = -1;
		};

		// A program the association table lists, and its map once one is read.
		struct ts_program
		{
			std::uint16_t number = 0;
			std::uint16_t pid = 0;
			std::vector<std::uint8_t> map;
			int copies = 0;
		};

		struct ts_stream
		{
			std::uint16_t pid = 0;
			std::uint8_t stream_type = 0;
			std::size_t index = 0;
			// Blu-ray's TrueHD carries an AC-3 core in packets of its own, which FFmpeg lists as a second
			// stream with the same PID.
			std::optional<std::size_t> core;
			es_probe probe;
			es_probe core_probe;
			es_clock clock;
			es_clock core_clock;
			bool timed = false;
			bool ended = false;
			bool core_ended = false;
			int counter = -1;
			// The current PES packet's first bytes, gathered until its header is whole.
			std::vector<std::uint8_t> head;
			bool in_header = false;
			bool in_payload = false;
			bool to_core = false;
			std::string descriptors;
		};

		class ts_scan
		{
		public:
			ts_scan(reader& r, const scan_intent intent, media_scan_result& out) :
				_r(r),
				_intent(intent),
				_out(out),
				_size(r.size())
			{
			}

			void run()
			{
				find_packets();
				_out.brand = _prefix != 0 ? "M2TS" : "MPEG-TS";

				const auto head_end = walk(_first, std::min(_size, _first + mpeg_head_bytes),
				                           [this](const std::uint8_t* const p) { return head_packet(p); });

				choose(true);
				if (!_pat_read) throw format_error("the transport stream has no program association table");
				if (!_pmt_read) throw format_error("the transport stream has no program map table");

				for (auto& s : _streams)
				{
					s.probe.finish(_out.streams[s.index], _out.properties);
					if (s.core) s.core_probe.finish(_out.streams[*s.core], _out.properties);
				}

				// The tail, read backwards in growing steps until every stream's last timestamp is found.
				auto covered = _size;

				for (const auto step : mpeg_tail_steps)
				{
					if (head_end >= covered || tail_complete()) break;

					const auto from = std::max(head_end, _size > step ? _size - step : 0);
					if (from >= covered) continue;

					const auto at = _first + (from - _first + _unit - 1) / _unit * _unit;
					walk(at, covered, [this](const std::uint8_t* const p)
					{
						tail_packet(p);
						return true;
					});
					covered = from;
				}

				finish_timing();
				if (_intent == scan_intent::inspect) structure(head_end, covered);
			}

		private:
			void find_packets()
			{
				// The sync byte repeating every 188 bytes from the start, or every 192 after the 4-byte
				// arrival time M2TS puts before each packet.
				std::array<std::uint8_t, 8 * 192 + 4> head{};
				const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(_size, head.size()));
				_r.read(0, head.data(), size);

				const auto run = [&head, size](const std::size_t first, const std::size_t unit)
				{
					std::size_t count = 0;
					for (auto at = first; at < size && head[at] == sync_byte; at += unit) ++count;
					return count;
				};

				const auto plain = run(0, packet_size);
				const auto prefixed = run(4, 192);
				if (plain < 2 && prefixed < 2) throw format_error("no transport stream packets begin the file");

				if (prefixed > plain)
				{
					_unit = 192;
					_prefix = 4;
				}
			}

			// The next unit at or after from whose sync byte two more follow a packet apart - fewer at the
			// file's end - as FFmpeg resynchronises; else where the search's reach ends, for the scan to go
			// on from. Searched in pieces smaller than the reader's block, which its window keeps.
			std::uint64_t resync(const std::uint64_t from, const std::uint64_t end)
			{
				const auto limit = std::min(end, from + resync_bytes);

				for (auto at = from; at < limit && at + _prefix < _size;)
				{
					const auto piece = _r.read(at + _prefix, static_cast<std::size_t>(std::min<std::uint64_t>(resync_piece, _size - at - _prefix)));

					for (std::size_t i = 0; i < piece.size() && at + i < limit; ++i)
					{
						if (piece[i] == sync_byte && synced(at + i)) return at + i;
					}

					at += piece.size();
				}

				return limit;
			}

			bool synced(const std::uint64_t unit)
			{
				if (!_r.contains(unit, _unit)) return false;

				for (std::uint64_t k = 1; k <= 2; ++k)
				{
					const auto next = unit + k * _unit + _prefix;
					if (next >= _size) break;
					if (_r.u8(next) != sync_byte) return false;
				}

				return true;
			}

			template <typename Visit>
			std::uint64_t walk(std::uint64_t at, const std::uint64_t end, Visit visit)
			{
				std::array<std::uint8_t, packet_size> packet{};

				while (at < end && _r.contains(at, _unit))
				{
					_r.read(at + _prefix, packet.data(), packet.size());

					if (packet[0] != sync_byte)
					{
						at = resync(at + 1, end);
						continue;
					}

					at += _unit;
					if (!visit(packet.data())) break;
				}

				return at;
			}

			// The packet's payload, after any adaptation field; reads a PCR on the program's PCR PID.
			// Empty when the packet has none, or is damaged.
			std::span<const std::uint8_t> payload(const std::uint8_t* const p, bool& discontinuity, const bool tail)
			{
				const auto pid = static_cast<std::uint16_t>((p[1] & 0x1F) << 8 | p[2]);
				const auto control = p[3] >> 4 & 3;
				std::size_t at = 4;
				discontinuity = false;

				if (control & 2)
				{
					const std::size_t length = p[4];
					if (length > packet_size - 5) return {};

					if (length > 0)
					{
						discontinuity = (p[5] & 0x80) != 0;

						if ((p[5] & 0x10) && length >= 7 && _pcr_pid == pid)
						{
							_pcr.see(_pcr_line.place(read_pcr(p + 6)), 0);
							if (tail) _pcr_ended = true;
						}
					}

					at = 5 + length;
				}

				if (!(control & 1) || at >= packet_size) return {};
				return {p + at, packet_size - at};
			}

			bool head_packet(const std::uint8_t* const p)
			{
				// A packet the transport marks as damaged is not read.
				if (p[1] & 0x80) return true;

				const auto pid = static_cast<std::uint16_t>((p[1] & 0x1F) << 8 | p[2]);
				const auto start = (p[1] & 0x40) != 0;
				auto discontinuity = false;
				const auto bytes = payload(p, discontinuity, false);
				if (bytes.empty()) return !settled();

				if (pid == 0)
				{
					take_sections(_pat_sections, _pat_counter, p, bytes, discontinuity, [this](const std::span<const std::uint8_t> s) { read_pat(s); });
					return !settled();
				}

				// Until a program is chosen, the maps of every program the association table lists are read.
				if (!_pmt_read)
				{
					const auto map = std::ranges::find(_maps, pid, &map_source::pid);

					if (map != _maps.end())
					{
						take_sections(map->sections, map->counter, p, bytes, discontinuity,
						              [this, pid](const std::span<const std::uint8_t> s) { read_map(s, pid); });
					}

					return !settled();
				}

				if (_by_pid[pid] < 0) return true;

				auto& s = _streams[static_cast<std::size_t>(_by_pid[pid])];
				const auto c = check_continuity(s.counter, p[3] & 0x0F, discontinuity);
				if (c == continuity::duplicate) return true;

				if (c == continuity::broken || (p[3] & 0xC0) != 0)
				{
					// Lost packets leave the PES packet in progress incomplete; a scrambled payload cannot
					// be read at all.
					s.in_header = false;
					s.in_payload = false;
					if ((p[3] & 0xC0) != 0) return true;
				}

				pes_bytes(s, bytes, start);
				return !settled();
			}

			void tail_packet(const std::uint8_t* const p)
			{
				if (p[1] & 0x80) return;

				const auto pid = static_cast<std::uint16_t>((p[1] & 0x1F) << 8 | p[2]);
				auto discontinuity = false;
				const auto bytes = payload(p, discontinuity, true);
				if (bytes.empty() || !(p[1] & 0x40) || (p[3] & 0xC0) != 0 || _by_pid[pid] < 0) return;

				auto& s = _streams[static_cast<std::size_t>(_by_pid[pid])];
				if (!s.timed) return;

				// Only a PES header that fits in its first packet is read here.
				pes_header h;
				if (read_pes_header(bytes, false, h) != pes_read::ok || !h.pts) return;

				const auto core = s.core && h.extended_stream_id == truehd_ac3_core;
				(core ? s.core_clock : s.clock).see(_pts.place(*h.pts), payload_bytes(h));
				(core ? s.core_ended : s.ended) = true;
			}

			static std::uint64_t payload_bytes(const pes_header& h)
			{
				return h.packet_length != 0 && 6u + h.packet_length > h.size ? 6u + h.packet_length - h.size : 0u;
			}

			bool settled() const
			{
				if (!_pmt_read) return false;

				// Every audio and video stream's header read and first timestamp seen; subtitles and data,
				// which may not appear for minutes, are not waited for.
				return std::ranges::all_of(_streams, [](const ts_stream& s)
				{
					if (!s.timed) return true;
					if (!s.probe.done() || !s.clock.first) return false;
					return !s.core || (s.core_probe.done() && s.core_clock.first);
				});
			}

			bool tail_complete() const
			{
				auto timed = false;

				for (const auto& s : _streams)
				{
					if (!s.timed) continue;

					if (s.clock.first)
					{
						timed = true;
						if (!s.ended) return false;
					}

					if (s.core && s.core_clock.first)
					{
						timed = true;
						if (!s.core_ended) return false;
					}
				}

				// Without timestamps, as when every payload is scrambled, the PCR alone times the stream.
				return timed || !_pcr.first || _pcr_ended;
			}

			template <typename Take>
			static void take_sections(section_reader& sections, int& counter, const std::uint8_t* const p, const std::span<const std::uint8_t> bytes,
			                          const bool discontinuity, Take take)
			{
				const auto c = check_continuity(counter, p[3] & 0x0F, discontinuity);
				if (c == continuity::duplicate) return;
				if (c == continuity::broken) sections.reset();
				sections.add(bytes, (p[1] & 0x40) != 0, take);
			}

			void read_pat(const std::span<const std::uint8_t> s)
			{
				if (_pmt_read || s.size() < 12 || s[0] != 0x00 || !(s[1] & 0x80) || !(s[5] & 1) || mpeg_crc32(s) != 0) return;

				const auto first = !_pat_read;
				_pat_read = true;
				if (first) _transport_id = load_be16(s.data() + 3);

				for (std::size_t at = 8; at + 4 <= s.size() - 4; at += 4)
				{
					const auto number = load_be16(s.data() + at);
					const auto pid = static_cast<std::uint16_t>(load_be16(s.data() + at + 2) & 0x1FFF);
					if (first && _intent == scan_intent::inspect) _pat_entries.emplace_back(number, pid);

					// Program 0 names the network information table's PID; FFmpeg skips a map PID that is
					// reserved or null too.
					if (number == 0 || pid <= 0x000F || pid == null_pid || _programs.size() >= max_programs) continue;
					if (std::ranges::find(_programs, number, &ts_program::number) != _programs.end()) continue;

					_programs.push_back({number, pid, {}, 0});
					if (std::ranges::find(_maps, pid, &map_source::pid) == _maps.end()) _maps.push_back({pid, {}, -1});
				}
			}

			// A program map section, kept for its program until one is chosen.
			void read_map(const std::span<const std::uint8_t> s, const std::uint16_t pid)
			{
				if (_pmt_read || s.size() < 16 || s[0] != 0x02 || !(s[1] & 0x80) || !(s[5] & 1) || mpeg_crc32(s) != 0) return;
				if (12u + (load_be16(s.data() + 10) & 0x0FFFu) > s.size() - 4) return;

				const auto number = load_be16(s.data() + 3);
				const auto program = std::ranges::find_if(_programs, [number, pid](const ts_program& p) { return p.number == number && p.pid == pid; });
				if (program == _programs.end()) return;

				++program->copies;
				if (program->map.empty()) program->map.assign(s.begin(), s.end());
				choose(false);
			}

			// The program read is the association table's first, once its map is read; or, when another
			// program's map has come round again without the first's, the first whose map was read - as in
			// a recording of one program that kept the whole table. At the end of the scan's reach, any.
			void choose(const bool last)
			{
				if (_pmt_read || _programs.empty()) return;

				const auto again = std::ranges::any_of(_programs, [](const ts_program& p) { return p.copies >= 2; });
				if (_programs.front().map.empty() && !again && !last) return;

				const auto chosen = std::ranges::find_if(_programs, [](const ts_program& p) { return !p.map.empty(); });
				if (chosen == _programs.end()) return;

				_program = chosen->number;
				_pmt_pid = chosen->pid;
				read_pmt(chosen->map);
			}

			void read_pmt(const std::span<const std::uint8_t> s)
			{
				const auto end = s.size() - 4;
				const std::size_t info = load_be16(s.data() + 10) & 0x0FFF;

				_pcr_pid = static_cast<std::uint16_t>(load_be16(s.data() + 8) & 0x1FFF);
				_pmt_version = static_cast<std::uint8_t>(s[5] >> 1 & 0x1F);
				_by_pid.assign(8192, -1);

				const auto program_info = s.subspan(12, info);
				const auto registration = find_registration(program_info);
				if (_intent == scan_intent::inspect) _program_descriptors = describe_descriptors(program_info);

				for (auto at = 12 + info; at + 5 <= end;)
				{
					const auto stream_type = s[at];
					const auto pid = static_cast<std::uint16_t>(load_be16(s.data() + at + 1) & 0x1FFF);
					const std::size_t length = load_be16(s.data() + at + 3) & 0x0FFF;
					if (at + 5 + length > end) break;

					add_stream(stream_type, pid, s.subspan(at + 5, length), registration);
					at += 5 + length;
				}

				_pmt_read = true;
			}

			void add_stream(const std::uint8_t stream_type, const std::uint16_t pid, const std::span<const std::uint8_t> descriptors,
			                const std::uint32_t registration)
			{
				if (_streams.size() >= max_streams || pid == *_pmt_pid || pid == 0 || pid == null_pid || _by_pid[pid] >= 0) return;

				// The stream type names the codec; for private data, a DVB descriptor; else a registration.
				const auto d = read_es_descriptors(descriptors);
				auto type = es_type_from_stream_type(stream_type, registration);
				if (!type.known() && stream_type == 0x06) type = d.described;
				if (!type.known()) type = d.registered;

				media_stream stream;
				apply_es_type(type, stream);
				stream.id = pid;
				stream.codec_tag = d.registration != 0 ? box_type_text(d.registration) : std::format("0x{:02X}", stream_type);
				stream.language = d.language;
				stream.audio_description = d.audio_description;

				// Opus in a transport stream plays at 48 kHz, its channels in the descriptor that names it.
				if (type.codec == "opus")
				{
					stream.audio.sample_rate = 48000;
					stream.audio.channels = d.opus_channels;
				}

				ts_stream s;
				s.pid = pid;
				s.stream_type = stream_type;
				s.index = _out.streams.size();
				s.timed = stream.kind == media_stream_kind::video || stream.kind == media_stream_kind::audio;

				const auto camera = type.syntax == es_syntax::h264 && !_camera_taken;
				_camera_taken = _camera_taken || camera;
				s.probe = es_probe(type.syntax, camera);
				if (_intent == scan_intent::inspect) s.descriptors = describe_descriptors(descriptors);

				_out.streams.push_back(std::move(stream));

				if (type.codec == "truehd")
				{
					media_stream core;
					apply_es_type(audio_type("ac3", es_syntax::ac3), core);
					core.id = pid;
					core.codec_tag = _out.streams.back().codec_tag;
					core.language = d.language;
					s.core = _out.streams.size();
					s.core_probe = es_probe(es_syntax::ac3, false);
					_out.streams.push_back(std::move(core));
				}

				_by_pid[pid] = static_cast<std::int16_t>(_streams.size());
				_streams.push_back(std::move(s));
			}

			void pes_bytes(ts_stream& s, const std::span<const std::uint8_t> bytes, const bool start)
			{
				if (start)
				{
					s.head.clear();
					s.in_header = true;
					s.in_payload = false;
				}

				if (!s.in_header)
				{
					if (s.in_payload) feed(s, bytes);
					return;
				}

				const auto take = std::min(bytes.size(), max_pes_header - s.head.size());
				s.head.insert(s.head.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(take));

				pes_header h;
				const auto read = read_pes_header(s.head, false, h);
				if (read == pes_read::short_data && s.head.size() < max_pes_header) return;

				s.in_header = false;
				if (read != pes_read::ok || h.stream_id == 0xBE) return;

				s.to_core = s.core && h.extended_stream_id == truehd_ac3_core;
				if (h.pts && s.timed) (s.to_core ? s.core_clock : s.clock).see(_pts.place(*h.pts), payload_bytes(h));

				s.in_payload = !(s.to_core ? s.core_probe : s.probe).done();
				if (!s.in_payload) return;

				feed(s, std::span<const std::uint8_t>(s.head).subspan(h.size));
				feed(s, bytes.subspan(take));
			}

			void feed(ts_stream& s, const std::span<const std::uint8_t> bytes)
			{
				if (!s.in_payload || bytes.empty()) return;

				auto& probe = s.to_core ? s.core_probe : s.probe;
				probe.add(bytes, _out.streams[s.to_core ? *s.core : s.index], _out.properties);
				s.in_payload = !probe.done();
			}

			void finish_timing()
			{
				std::vector<timed_stream> timed;

				for (auto& s : _streams)
				{
					auto& stream = _out.streams[s.index];
					timed.push_back({&stream, s.clock, s.probe.end_ticks(stream, s.clock.last_payload)});

					if (s.core)
					{
						auto& core = _out.streams[*s.core];
						timed.push_back({&core, s.core_clock, s.core_probe.end_ticks(core, s.core_clock.last_payload)});
					}
				}

				auto duration = finish_pts_durations(timed);

				// Scrambled payloads keep their PES headers from view, but never the PCRs.
				if (!(duration > 0) && _pcr.first && _pcr.last && *_pcr.last > *_pcr.first)
				{
					duration = static_cast<double>(*_pcr.last - *_pcr.first) / static_cast<double>(mpeg_system_ticks_per_second);
				}

				_out.duration = duration;
			}

			void structure(const std::uint64_t head_end, const std::uint64_t tail_start)
			{
				auto& rows = _out.structure;

				add_structure_section(rows, "Transport stream", "ts.summary", true);
				add_structure_row(rows, "Packets",
				                  _prefix != 0 ? "192 bytes: a 4-byte arrival time, then a 188-byte packet" : "188 bytes",
				                  std::format("{} bytes", _unit));
				add_structure_row(rows, "First packet", std::format("offset {}", _first));
				add_structure_row(rows, "Read", tail_start < _size
					                                ? std::format("{} bytes from the start, and the last {}", head_end - _first, _size - tail_start)
					                                : std::format("{} bytes from the start", head_end - _first));

				add_structure_section(rows, "Program association table", "ts.pat");
				add_structure_row(rows, "Transport stream id", std::to_string(_transport_id));

				for (const auto& [program, pid] : _pat_entries)
				{
					if (program == 0) add_structure_row(rows, "Network", std::format("PID 0x{:04X}", pid));
					else add_structure_row(rows, std::format("Program {}", program), std::format("map on PID 0x{:04X}", pid));
				}

				add_structure_section(rows, std::format("Program {}", _program), "ts.program");
				add_structure_row(rows, "Program map", std::format("PID 0x{:04X}, version {}", *_pmt_pid, _pmt_version));
				if (_pcr_pid) add_structure_row(rows, "PCR", std::format("PID 0x{:04X}", *_pcr_pid));
				if (!_program_descriptors.empty()) add_structure_row(rows, "Descriptors", _program_descriptors);

				for (const auto& s : _streams)
				{
					const auto& stream = _out.streams[s.index];
					auto value = std::format("stream type 0x{:02X}, {}", s.stream_type, stream.codec.empty() ? "unknown" : stream.codec);
					if (!s.descriptors.empty()) value += "; " + s.descriptors;
					add_structure_row(rows, std::format("PID 0x{:04X}", s.pid), std::move(value), std::format("0x{:02X}", s.stream_type));
				}

				add_structure_section(rows, "Timing", "ts.timing");

				if (_pcr.first && _pcr.last)
				{
					add_structure_row(rows, "PCR", std::format("{:.3f} s to {:.3f} s",
					                                           static_cast<double>(*_pcr.first) / mpeg_system_ticks_per_second,
					                                           static_cast<double>(*_pcr.last) / mpeg_system_ticks_per_second));
				}

				for (const auto& s : _streams)
				{
					if (!s.clock.first || !s.clock.last) continue;
					add_structure_row(rows, std::format("PID 0x{:04X} PTS", s.pid),
					                  std::format("{:.3f} s to {:.3f} s", static_cast<double>(*s.clock.first) / mpeg_ticks_per_second,
					                              static_cast<double>(*s.clock.last) / mpeg_ticks_per_second));
				}

				finish_structure_sections(rows);
			}

			reader& _r;
			scan_intent _intent;
			media_scan_result& _out;
			std::uint64_t _size;
			// A packet's unit in the file - 188 bytes, or 192 with M2TS's prefix - and where the first begins.
			std::uint64_t _unit = packet_size;
			std::uint64_t _prefix = 0;
			std::uint64_t _first = 0;

			section_reader _pat_sections;
			int _pat_counter = -1;
			bool _pat_read = false;
			bool _pmt_read = false;
			std::vector<ts_program> _programs;
			std::vector<map_source> _maps;
			std::uint16_t _program = 0;
			std::optional<std::uint16_t> _pmt_pid;
			std::optional<std::uint16_t> _pcr_pid;

			std::vector<ts_stream> _streams;
			std::vector<std::int16_t> _by_pid;
			bool _camera_taken = false;

			timestamp_line _pts{mpeg_timestamp_wrap, mpeg_ticks_per_second};
			timestamp_line _pcr_line{mpeg_system_clock_wrap, mpeg_system_ticks_per_second};
			es_clock _pcr;
			bool _pcr_ended = false;

			std::vector<std::pair<std::uint16_t, std::uint16_t>> _pat_entries;
			std::uint16_t _transport_id = 0;
			std::uint8_t _pmt_version = 0;
			std::string _program_descriptors;
		};
	}

	void scan_mpeg_ts_media(reader& r, const scan_intent intent, media_scan_result& out)
	{
		ts_scan(r, intent, out).run();
	}
}
