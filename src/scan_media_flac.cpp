// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning FLAC files: the metadata blocks after "fLaC" - STREAMINFO for the stream, the Vorbis
// comment for its tags and PICTURE blocks for its cover art, located rather than read. When STREAMINFO
// does not count the samples, as an encoder writing to a pipe leaves it, the header of the last frame says
// where the stream ends.

#include "media_internal.h"

#include <array>
#include <format>
#include <optional>

namespace libfiles::detail
{
	namespace
	{
		constexpr unsigned streaminfo_type = 0;
		constexpr unsigned vorbis_comment_type = 4;
		constexpr unsigned picture_type = 6;
		constexpr std::size_t streaminfo_size = 34;
		// Files hold a handful; one made of empty blocks is not walked to its end.
		constexpr int max_blocks = 1024;
		// How much of the end is read for the last frame's header, unless STREAMINFO allows larger frames.
		constexpr std::uint64_t min_tail = 64 * 1024;
		// A frame header is at most 16 bytes.
		constexpr std::uint64_t max_frame_header = 16;
		// How much of a PICTURE block is read: its fields come first, and the image after them need only be
		// located.
		constexpr std::uint64_t picture_head = 64 * 1024;

		std::string block_name(const unsigned type)
		{
			static constexpr std::string_view names[] = {"STREAMINFO", "PADDING", "APPLICATION", "SEEKTABLE", "VORBIS_COMMENT", "CUESHEET", "PICTURE"};
			return type < std::size(names) ? std::string(names[type]) : std::format("Reserved ({})", type);
		}

		std::uint32_t load_be24(const std::uint8_t* const p)
		{
			return static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
		}

		// Where the fields at the start of a PICTURE block end - its type, its MIME type and description, each
		// counted, then the image's dimensions and size - as far as the head of it that was read can tell: at
		// least there, when the head ends before the description is counted.
		std::uint64_t picture_fields_end(const std::span<const std::uint8_t> head)
		{
			if (head.size() < 8) return 32;

			const std::uint64_t mime = load_be32(head.data() + 4);
			if (head.size() < 12 + mime) return 32 + mime;
			return 32 + mime + load_be32(head.data() + 8 + static_cast<std::size_t>(mime));
		}

		// The picture a PICTURE block locates, from the head of the block, and from the whole of it only when
		// its fields run past the head: a scan does not read the image to find it. A picture that would run
		// past its block is not one.
		std::optional<media_picture> read_picture_block(reader& r, const std::uint64_t data, const std::uint32_t length)
		{
			const auto head = r.view(data, static_cast<std::size_t>(std::min<std::uint64_t>(length, picture_head)));
			media_picture picture;
			auto found = read_flac_picture(head, data, picture);

			if (!found && head.size() < length)
			{
				const auto fields = picture_fields_end(head);
				if (fields > head.size() && fields <= length) found = read_flac_picture(r.view(data, length), data, picture);
			}

			const auto& place = picture.location;
			if (!found || place.offset < data || place.offset - data > length || place.size > length - (place.offset - data)) return {};
			return picture;
		}

		// What every frame of the stream shares with STREAMINFO.
		struct stream_shape
		{
			std::uint32_t sample_rate = 0;
			std::uint32_t channels = 0;
			std::uint32_t bits = 0;
			// The block size of every frame but the last in a stream of fixed block size.
			std::uint32_t block_size = 0;
		};

		// The CRC-8 that ends a frame header: x^8 + x^2 + x + 1, from zero.
		std::uint8_t crc8(const std::span<const std::uint8_t> bytes)
		{
			std::uint8_t crc = 0;

			for (const auto b : bytes)
			{
				crc ^= b;
				for (auto i = 0; i < 8; ++i) crc = static_cast<std::uint8_t>(crc & 0x80 ? crc << 1 ^ 0x07 : crc << 1);
			}

			return crc;
		}

		// The sample after the frame whose header starts h. The header must agree with STREAMINFO and end in
		// its CRC, as a run of coded audio that happens to begin with the sync code does not.
		std::optional<std::uint64_t> frame_end(const std::span<const std::uint8_t> h, const stream_shape& shape)
		{
			if (h.size() < 6 || h[0] != 0xFF || (h[1] & 0xFE) != 0xF8) return {};

			const auto variable = (h[1] & 1) != 0;
			const unsigned size_code = h[2] >> 4;
			const unsigned rate_code = h[2] & 15;
			const unsigned channel_code = h[3] >> 4;
			const unsigned depth_code = h[3] >> 1 & 7;
			if (size_code == 0 || rate_code == 15 || channel_code > 10 || depth_code == 3 || (h[3] & 1)) return {};

			// The frame's number in a stream of fixed block size, else its first sample: coded as UTF-8 is,
			// extended to seven bytes.
			std::size_t at = 4;
			const unsigned lead = h[at++];
			std::uint64_t number = lead;

			if (lead >= 0x80)
			{
				unsigned ones = 0;
				while (ones < 8 && (lead & 0x80u >> ones)) ++ones;
				if (ones < 2 || ones > 7 || (!variable && ones > 6) || h.size() < at + ones - 1) return {};

				number = lead & 0x7Fu >> ones;

				for (unsigned i = 1; i < ones; ++i)
				{
					const unsigned c = h[at++];
					if ((c & 0xC0) != 0x80) return {};
					number = number << 6 | (c & 0x3F);
				}
			}

			std::uint32_t block = 0;
			const std::size_t size_bytes = size_code == 6 ? 1 : size_code == 7 ? 2 : 0;
			const std::size_t rate_bytes = rate_code == 12 ? 1 : rate_code >= 13 ? 2 : 0;
			if (h.size() < at + size_bytes + rate_bytes + 1) return {};

			if (size_code == 1) block = 192;
			else if (size_code <= 5) block = 576u << (size_code - 2);
			else if (size_code == 6) block = h[at] + 1u;
			else if (size_code == 7) block = load_be16(h.data() + at) + 1u;
			else block = 1u << size_code;

			at += size_bytes;

			static constexpr std::uint32_t rates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
			std::uint32_t rate = 0;

			if (rate_code < 12) rate = rates[rate_code];
			else if (rate_code == 12) rate = h[at] * 1000u;
			else if (rate_code == 13) rate = load_be16(h.data() + at);
			else rate = load_be16(h.data() + at) * 10u;

			at += rate_bytes;
			if (crc8(h.first(at)) != h[at]) return {};

			static constexpr std::uint32_t depths[8] = {0, 8, 12, 0, 16, 20, 24, 32};
			const auto channels = channel_code < 8 ? channel_code + 1 : 2;
			if ((rate_code != 0 && rate != shape.sample_rate) || channels != shape.channels) return {};
			if (depth_code != 0 && depths[depth_code] != shape.bits) return {};
			if (!variable && shape.block_size == 0) return {};

			return (variable ? number : number * shape.block_size) + block;
		}

		// The samples the stream holds by the header of its last frame, found from the end of the file; 0
		// when no frame header there agrees with STREAMINFO.
		std::uint64_t samples_by_last_frame(reader& r, const std::uint64_t audio_start, const stream_shape& shape,
		                                    const std::uint32_t max_frame_size)
		{
			const auto size = r.size();
			if (audio_start >= size) return 0;

			const auto length = std::min(size - audio_start, std::max(min_tail, max_frame_size + max_frame_header));
			const auto bytes = r.view(size - length, static_cast<std::size_t>(length));

			for (auto i = bytes.size(); i-- > 0;)
			{
				if (bytes[i] != 0xFF) continue;
				if (const auto end = frame_end(bytes.subspan(i), shape)) return *end;
			}

			return 0;
		}
	}

	void scan_flac_media(reader& r, const scan_intent intent, const std::uint64_t offset, media_scan_result& out)
	{
		const auto inspect = intent == scan_intent::inspect;
		if (!r.contains(offset, 4) || !starts_with(r.view(offset, 4), "fLaC")) throw format_error("the file has no FLAC signature");

		audio_header audio;
		std::uint64_t total_samples = 0;
		stream_shape shape;
		std::uint32_t max_frame_size = 0;
		auto found = false;
		auto last = false;
		auto at = offset + 4;

		if (inspect) add_structure_section(out.structure, "Metadata blocks", "flac.blocks", true);

		for (auto count = 0; !last && count < max_blocks && r.contains(at, 4); ++count)
		{
			const auto h = r.view(at, 4);
			const unsigned type = h[0] & 0x7F;
			const auto length = load_be24(h.data() + 1);
			const auto data = at + 4;
			last = (h[0] & 0x80) != 0;

			if (!r.contains(data, length)) break;

			if (count == 0)
			{
				if (type != streaminfo_type || length != streaminfo_size) throw format_error("the FLAC stream does not begin with its STREAMINFO block");

				std::array<std::uint8_t, streaminfo_size> info;
				r.read(data, info.data(), info.size());
				if (!parse_flac_streaminfo(info, audio, total_samples)) throw format_error("the FLAC STREAMINFO block is damaged");

				shape = {audio.sample_rate, audio.channels, audio.bits_per_sample, load_be16(info.data() + 2)};
				max_frame_size = load_be24(info.data() + 7);
				found = true;
			}
			else if (type == vorbis_comment_type)
			{
				read_vorbis_comments(r.view(data, length), out);
			}
			else if (type == picture_type)
			{
				if (auto picture = read_picture_block(r, data, length)) out.pictures.push_back(std::move(*picture));
			}

			if (inspect) add_structure_row(out.structure, block_name(type), std::format("offset {}, {} bytes{}", at, length, last ? ", last" : ""));

			at = data + length;
		}

		if (!found) throw format_error("the FLAC STREAMINFO block is cut short");

		media_stream stream;
		stream.kind = media_stream_kind::audio;
		stream.codec = "flac";
		apply_audio_header(audio, stream.audio);

		auto samples_source = "STREAMINFO";

		if (total_samples == 0)
		{
			total_samples = samples_by_last_frame(r, at, shape, max_frame_size);
			samples_source = "the last frame's header";
		}

		if (total_samples != 0 && audio.sample_rate != 0) stream.duration = static_cast<double>(total_samples) / audio.sample_rate;

		out.brand = "FLAC";
		out.streams.push_back(std::move(stream));

		if (inspect)
		{
			add_structure_section(out.structure, "Audio", "flac.audio", true);
			add_structure_row(out.structure, "Frames", std::format("offset {}, {} bytes", at, r.size() - std::min(at, r.size())));
			add_structure_row(out.structure, "Samples", total_samples != 0 ? std::format("{}, from {}", total_samples, samples_source) : "unknown");
			finish_structure_sections(out.structure);
		}
	}
}
