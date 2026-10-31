// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The media scan's front: telling the containers apart, dispatching to each one's scanner,
// and what every scan finishes with - the duration and bit rate, whether the scan is complete, and the
// names its pixel formats take. Also the small rules every scanner shares: text as tags store it, FourCC
// names and dates counted in seconds.

#include "id3_internal.h"
#include "media_internal.h"
#include "text_internal.h"
#include "write_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>

namespace libfiles
{
	namespace
	{
		constexpr std::uint32_t le_tag(const char (&s)[5])
		{
			return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24;
		}

		bool is_transport_stream(const std::span<const std::uint8_t> head, const std::size_t first, const std::size_t packet)
		{
			for (auto i = 0; i < 3; ++i)
			{
				const auto at = first + static_cast<std::size_t>(i) * packet;
				if (at >= head.size() || head[at] != 0x47) return false;
			}

			return true;
		}
	}

	media_format detect_media_format(const std::span<const std::uint8_t> head) noexcept
	{
		if (head.size() < 4) return media_format::unknown;

		// HEIF and AVIF stills are ISO base media too, but photos.
		if (detect_image_format(head) == image_format::heif) return media_format::unknown;

		if (head.size() >= 16 && detail::is_asf(head)) return media_format::asf;

		if (head.size() >= 12)
		{
			const auto id = detail::load_le32(head.data());
			const auto type = detail::load_le32(head.data() + 8);

			if ((id == le_tag("RIFF") || id == le_tag("RF64") || id == le_tag("BW64")) && type == le_tag("WAVE")) return media_format::wav;
			if (id == le_tag("RIFF") && type == le_tag("AVI ")) return media_format::avi;
			if (id == le_tag("FORM") && (type == le_tag("AIFF") || type == le_tag("AIFC"))) return media_format::aiff;
		}

		if (head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3) return media_format::matroska;
		if (detail::starts_with(head, "fLaC")) return media_format::flac;
		if (detail::starts_with(head, "OggS")) return media_format::ogg;
		if (detail::is_flv(head)) return media_format::flv;
		if (detail::is_iso_media(head)) return media_format::mp4;
		if (head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xBA) return media_format::mpeg_ps;
		if (is_transport_stream(head, 0, 188) || is_transport_stream(head, 4, 192)) return media_format::mpeg_ts;
		if (detail::is_mpeg_audio(head)) return media_format::mpeg_audio;
		return media_format::unknown;
	}

	std::string video_format::pixel_format() const
	{
		return picture().name();
	}

	std::string_view video_format::hdr_transfer() const noexcept
	{
		if (transfer == 16) return "PQ";
		if (transfer == 18) return "HLG";
		return {};
	}

	media_scan_result scan_media(reader& r, const scan_intent intent, const scan_limits& limits)
	{
		media_scan_result result;

		if (r.size() < 16)
		{
			result.error = "the file is too short to be audio or video";
			return result;
		}

		auto format = media_format::unknown;
		detail::media_budget budget(limits.media_inflated_bytes);

		try
		{
			// One identification for the scan, read_packet and plan_update: a leading ID3v2 tag says nothing of
			// what follows it, and FLAC is sometimes tagged that way too.
			format = info(identify(r)).media;
			result.format = format;

			std::uint8_t head[detail::id3_header_size];
			r.read(0, head, sizeof(head));
			const auto tag_size = format == media_format::mpeg_audio || format == media_format::flac ? detail::leading_id3_size(head) : 0;

			if (tag_size != 0)
			{
				detail::read_id3v2(r, 0, budget, result);
			}

			switch (format)
			{
			case media_format::mp4: detail::scan_mp4_media(r, intent, result);
				break;
			case media_format::matroska: detail::scan_matroska_media(r, intent, budget, result);
				break;
			case media_format::avi: detail::scan_avi_media(r, intent, result);
				break;
			case media_format::wav: detail::scan_wav_media(r, intent, budget, result);
				break;
			case media_format::aiff: detail::scan_aiff_media(r, intent, budget, result);
				break;
			case media_format::asf: detail::scan_asf_media(r, intent, result);
				break;
			case media_format::mpeg_audio: detail::scan_mpeg_audio_media(r, intent, tag_size, budget, result);
				break;
			case media_format::flac: detail::scan_flac_media(r, intent, tag_size, result);
				break;
			case media_format::ogg: detail::scan_ogg_media(r, intent, result);
				break;
			case media_format::flv: detail::scan_flv_media(r, intent, result);
				break;
			case media_format::mpeg_ts: detail::scan_mpeg_ts_media(r, intent, result);
				break;
			case media_format::mpeg_ps: detail::scan_mpeg_ps_media(r, intent, result);
				break;
			case media_format::unknown:
				result.error = "the file is not audio or video libfiles reads";
				return result;
			}

			detail::finish_media_scan(result, r.size());
			result.success = true;
		}
		catch (const std::exception& e)
		{
			result = {};
			result.format = format;
			result.error = e.what();
		}

		return result;
	}
}

namespace libfiles::detail
{
	namespace
	{
		bool printable(const std::uint8_t c)
		{
			return c >= 0x20 && c < 0x7F;
		}

		std::string hex32(const std::uint32_t v)
		{
			char text[16];
			std::snprintf(text, sizeof(text), "0x%08X", v);
			return text;
		}
	}

	std::string fourcc_text(const std::uint32_t little_endian_fourcc)
	{
		const std::uint8_t c[4] = {
			static_cast<std::uint8_t>(little_endian_fourcc), static_cast<std::uint8_t>(little_endian_fourcc >> 8),
			static_cast<std::uint8_t>(little_endian_fourcc >> 16), static_cast<std::uint8_t>(little_endian_fourcc >> 24)
		};

		if (std::ranges::all_of(c, printable)) return {reinterpret_cast<const char*>(c), 4};
		return hex32(little_endian_fourcc);
	}

	std::string box_type_text(const std::uint32_t big_endian_type)
	{
		const std::uint8_t c[4] = {
			static_cast<std::uint8_t>(big_endian_type >> 24), static_cast<std::uint8_t>(big_endian_type >> 16),
			static_cast<std::uint8_t>(big_endian_type >> 8), static_cast<std::uint8_t>(big_endian_type)
		};

		// QuickTime and iTunes name their text atoms with a leading copyright sign, 0xA9 in Mac Roman.
		if (!std::ranges::all_of(c, [](const std::uint8_t b) { return printable(b) || b == 0xA9; })) return hex32(big_endian_type);

		std::string out;
		for (const auto b : c) append_utf8(out, b == 0xA9 ? U'\u00A9' : static_cast<char32_t>(b));
		return out;
	}

	void apply_video_header(const video_header& header, video_format& video)
	{
		if (header.width != 0 && header.height != 0)
		{
			video.width = header.width;
			video.height = header.height;
		}

		if (header.chroma != chroma_format::unknown) video.chroma = header.chroma;
		if (header.bit_depth != 0) video.bit_depth = header.bit_depth;
		video.alpha = video.alpha || header.alpha;
		video.interlaced = video.interlaced || header.interlaced;
		video.full_range = video.full_range || header.full_range;
		if (header.color_primaries != 2) video.color_primaries = header.color_primaries;
		if (header.transfer != 2) video.transfer = header.transfer;
		if (header.matrix != 2) video.matrix = header.matrix;
		if (!header.profile.empty()) video.profile = header.profile;

		if (video.pixel_aspect_width == 0 && header.pixel_aspect_width != 0 && header.pixel_aspect_height != 0)
		{
			video.pixel_aspect_width = header.pixel_aspect_width;
			video.pixel_aspect_height = header.pixel_aspect_height;
		}

		if (!(video.frame_rate > 0) && header.frame_rate > 0) video.frame_rate = header.frame_rate;
	}

	void apply_audio_header(const audio_header& header, audio_format& audio)
	{
		if (header.sample_rate != 0) audio.sample_rate = header.sample_rate;
		if (header.channels != 0) audio.channels = header.channels;

		if (header.bits_per_sample != 0)
		{
			audio.bits_per_sample = header.bits_per_sample;
			audio.float_samples = header.float_samples;
		}

		if (!header.profile.empty()) audio.profile = header.profile;
	}

	void finish_media_scan(media_scan_result& out, const std::uint64_t file_size)
	{
		auto any = false;
		auto complete = true;
		auto longest = 0.0;

		for (auto& s : out.streams)
		{
			if (!(s.duration > 0) || !std::isfinite(s.duration)) s.duration = 0;

			if (s.kind == media_stream_kind::video)
			{
				any = true;
				s.video.rotation = ((s.video.rotation % 360) + 360) % 360 / 90 * 90;
				if (s.codec.empty() || s.video.width == 0 || s.video.height == 0) complete = false;
			}
			else if (s.kind == media_stream_kind::audio)
			{
				any = true;
				if (s.codec.empty() || s.audio.sample_rate == 0 || s.audio.channels == 0) complete = false;
			}
			else
			{
				continue;
			}

			longest = std::max(longest, s.duration);
		}

		if (!(out.duration > 0) || !std::isfinite(out.duration)) out.duration = longest;
		// As FFmpeg's update_stream_timings has it: the file's bits over the duration in whole microseconds.
		const auto microseconds = out.duration > 0 ? std::round(out.duration * 1e6) : 0.0;
		const auto bit_rate = microseconds > 0 ? static_cast<double>(file_size) * 8e6 / microseconds : 0.0;
		out.bit_rate = bit_rate > 0 && bit_rate <= 9.2e18 ? static_cast<std::uint64_t>(bit_rate) : 0;
		out.complete = any && complete && out.duration > 0;
	}
}
