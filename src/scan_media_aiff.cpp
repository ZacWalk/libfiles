// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning AIFF and AIFF-C files: the COMM chunk's channels, frames, sample size and rate, and an
// AIFF-C's compression; the text chunks and an ID3 chunk as tags; and the XMP packet Adobe's toolkit keeps
// in an APPL chunk signed "XMP " (XMP Specification Part 3). The sound data is never read.

#include "libfiles/write.h"
#include "media_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		// A chunk id as AIFF stores it, read big-endian.
		constexpr std::uint32_t id_of(const char (&s)[5])
		{
			return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) << 24 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 16 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 8 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3]));
		}

		constexpr auto aifc_type = id_of("AIFC");
		constexpr auto comm_id = id_of("COMM");
		constexpr auto ssnd_id = id_of("SSND");
		constexpr auto name_id = id_of("NAME");
		constexpr auto auth_id = id_of("AUTH");
		constexpr auto copyright_id = id_of("(c) ");
		constexpr auto anno_id = id_of("ANNO");
		constexpr auto id3_id = id_of("ID3 ");
		constexpr auto id3_lower_id = id_of("id3 ");
		constexpr auto appl_id = id_of("APPL");
		constexpr auto xmp_signature = id_of("XMP ");

		// The most a scan reads of a text chunk; a longer one is skipped.
		constexpr std::uint64_t max_text = 1u << 20;
		// The chunks a structure listing shows.
		constexpr std::size_t max_listed_chunks = 512;

		// What an AIFF-C compression type names, as FFmpeg reads it: the codec; for PCM its depth, set by the
		// type or else by COMM, and whether float; and for a codec that packs frames into packets, how many
		// frames and bytes a packet holds - numSampleFrames counts packets then. Zero frames a packet for a
		// codec whose packets COMM does not describe.
		struct compression
		{
			std::string codec;
			bool pcm = false;
			std::uint8_t bits = 0;
			bool float_samples = false;
			std::uint32_t frames_per_packet = 1;
			// Bytes a packet, and the same again for each channel.
			std::uint32_t packet_bytes = 0;
			std::uint32_t packet_bytes_per_channel = 0;
		};

		compression compression_of(const std::uint32_t type)
		{
			// FFmpeg matches a type whatever its case, and Apple writes "fl32" and "FL32" alike.
			auto upper = type;

			for (auto shift = 0; shift < 32; shift += 8)
			{
				const auto c = upper >> shift & 0xFF;
				if (c >= 'a' && c <= 'z') upper -= 0x20u << shift;
			}

			compression c;

			const auto pcm = [&c](const std::uint8_t bits, const bool float_samples)
			{
				c.codec = "pcm";
				c.pcm = true;
				c.bits = bits;
				c.float_samples = float_samples;
			};

			const auto packed = [&c](const std::string_view codec, const std::uint32_t frames, const std::uint32_t bytes,
			                         const std::uint32_t bytes_per_channel)
			{
				c.codec = codec;
				c.frames_per_packet = frames;
				c.packet_bytes = bytes;
				c.packet_bytes_per_channel = bytes_per_channel;
			};

			switch (upper)
			{
			case id_of("NONE"):
			case id_of("TWOS"):
			case id_of("SOWT"):
			case id_of("RAW "): pcm(0, false);
				break;
			case id_of("IN24"): pcm(24, false);
				break;
			case id_of("IN32"): pcm(32, false);
				break;
			case id_of("FL32"): pcm(32, true);
				break;
			case id_of("FL64"): pcm(64, true);
				break;
			// G.711 is named as WAVE names it, so a WAV and an AIFF of it name the same codec.
			case id_of("ULAW"): packed(codec_from_wave_tag(0x0007), 1, 0, 1);
				break;
			case id_of("ALAW"): packed(codec_from_wave_tag(0x0006), 1, 0, 1);
				break;
			case id_of("IMA4"): packed("adpcm_ima_qt", 64, 0, 34);
				break;
			case id_of("MAC3"): packed("mace3", 6, 0, 2);
				break;
			case id_of("MAC6"): packed("mace6", 6, 0, 1);
				break;
			case id_of("GSM "): packed("gsm", 160, 33, 0);
				break;
			case id_of("QCLP"): packed("qcelp", 160, 35, 0);
				break;
			case id_of("QDM2"): packed("qdm2", 0, 0, 0);
				break;
			case id_of("QDMC"): packed("qdmc", 0, 0, 0);
				break;
			case id_of("SDX2"): packed("sdx2_dpcm", 1, 0, 1);
				break;
			case id_of("CBD2"): packed("cbd2_dpcm", 1, 0, 1);
				break;
			case id_of("G722"): packed("adpcm_g722", 2, 0, 1);
				break;
			case id_of("ADP4"): packed("adpcm_ima_ws", 2, 0, 1);
				break;
			case id_of("VAPC"): packed("adpcm_n64", 16, 9, 0);
				break;
			default: c.frames_per_packet = 0;
				break;
			}

			return c;
		}

		// COMM's sample rate, an IEEE 754 80-bit extended number - a sign, a 15-bit exponent and a 64-bit
		// significand with its integer bit - rounded to a whole rate; 0 when it is not one.
		std::uint32_t extended_rate(const std::uint8_t* const p)
		{
			const auto sign_exponent = load_be16(p);
			const auto significand = std::uint64_t{load_be32(p + 2)} << 32 | load_be32(p + 6);
			if (sign_exponent & 0x8000) return 0;

			const auto rate = std::ldexp(static_cast<double>(significand), static_cast<int>(sign_exponent & 0x7FFF) - 16383 - 63);
			if (!(rate >= 0.5) || rate >= 4294967295.5) return 0;
			return static_cast<std::uint32_t>(std::floor(rate + 0.5));
		}

		struct aiff_chunk
		{
			std::uint64_t start = 0;
			std::uint32_t id = 0;
			std::uint32_t size = 0;
			bool cut = false;
		};

		// Where the form's chunks end: where its size says, or the end of the file for a form that runs past it.
		std::uint64_t aiff_form_end(reader& r, bool& cut)
		{
			const auto form_size = r.be32(4);
			cut = form_size < 4 || form_size > r.size() - 8;
			return cut ? r.size() : 8 + std::uint64_t{form_size};
		}

		// The chunk at pos of a form that ends at end, as every reader of the form walks them.
		aiff_chunk aiff_chunk_at(reader& r, const std::uint64_t pos, const std::uint64_t end)
		{
			aiff_chunk c;
			c.start = pos;
			c.id = r.be32(pos);
			c.size = r.be32(pos + 4);
			c.cut = c.size > end - pos - 8;
			return c;
		}

		std::uint64_t next_aiff_chunk(const aiff_chunk& c)
		{
			return c.start + 8 + c.size + (c.size & 1);
		}

		// The toolkit's packet: an APPL chunk signed "XMP ", whole. Of several, the toolkit reads the last.
		bool is_aiff_packet(reader& r, const aiff_chunk& c)
		{
			return c.id == appl_id && !c.cut && c.size >= 4 && r.be32(c.start + 8) == xmp_signature;
		}

		std::vector<std::uint8_t> aiff_packet(reader& r, const aiff_chunk& c)
		{
			auto packet = r.read(c.start + 12, c.size - 4);
			while (!packet.empty() && packet.back() == 0) packet.pop_back();
			return packet;
		}
	}

	void read_aiff_packet(reader& r, packet_reading& out)
	{
		bool cut = false;
		const auto end = aiff_form_end(r, cut);
		std::optional<aiff_chunk> packet;

		for (std::uint64_t pos = 12; end > pos && end - pos >= 8;)
		{
			const auto c = aiff_chunk_at(r, pos, end);
			if (is_aiff_packet(r, c)) packet = c;
			if (c.cut) break;
			pos = next_aiff_chunk(c);
		}

		if (packet)
		{
			const auto bytes = aiff_packet(r, *packet);
			out.packet.assign(bytes.begin(), bytes.end());
		}

		out.readable = true;
	}

	void scan_aiff_media(reader& r, const scan_intent intent, media_budget& budget, media_scan_result& out)
	{
		const auto size = r.size();
		const auto form_size = r.be32(4);
		const auto form_type = r.be32(8);
		bool form_cut = false;
		const auto end = aiff_form_end(r, form_cut);

		std::vector<aiff_chunk> chunks;
		std::vector<std::uint8_t> comm;
		std::optional<aiff_chunk> packet;
		std::uint64_t pos = 12;

		while (end > pos && end - pos >= 8)
		{
			const auto c = aiff_chunk_at(r, pos, end);

			const auto payload = pos + 8;
			const auto available = std::min<std::uint64_t>(c.size, end - payload);
			const auto text = [&]
			{
				return c.size > max_text ? std::string() : text_from_unknown(r.read(payload, static_cast<std::size_t>(available)));
			};

			switch (c.id)
			{
			case comm_id:
				// Its fields, an AIFF-C's compression type and the name after it, a Pascal string.
				if (comm.empty()) comm = r.read(payload, static_cast<std::size_t>(std::min<std::uint64_t>(available, 18 + 4 + 256)));
				break;
			case name_id:
			case auth_id:
			case copyright_id:
			case anno_id: add_tag(out, tag_family::aiff, box_type_text(c.id), text());
				break;
			case id3_id:
			case id3_lower_id:
				// A damaged tag keeps what was read of it.
				try
				{
					read_id3v2(r, payload, budget, out);
				}
				catch (const read_error&)
				{
				}
				catch (const format_error&)
				{
				}
				break;
			case appl_id:
				if (is_aiff_packet(r, c)) packet = c;
				break;
			default: break;
			}

			chunks.push_back(c);
			if (c.cut) break;
			pos = next_aiff_chunk(c);
		}

		const auto comm_at = std::ranges::find(chunks, comm_id, &aiff_chunk::id);
		if (comm_at == chunks.end()) throw format_error("the AIFF has no COMM chunk");
		if (comm.size() < 18) throw format_error("the AIFF's COMM chunk is cut short");

		media_stream stream;
		stream.kind = media_stream_kind::audio;
		auto& a = stream.audio;
		a.channels = load_be16(comm.data());
		const auto frames = load_be32(comm.data() + 2);
		const auto sample_size = load_be16(comm.data() + 6);
		a.sample_rate = extended_rate(comm.data() + 8);

		// AIFF-C names its compression after the rate; plain AIFF, and an AIFF-C whose COMM is too short to
		// name one, is big-endian PCM.
		const auto aifc = form_type == aifc_type && comm.size() >= 22;
		const auto type = aifc ? load_be32(comm.data() + 18) : id_of("NONE");
		auto c = compression_of(type);
		if (aifc) stream.codec_tag = box_type_text(type);

		if (c.pcm)
		{
			if (c.bits == 0) c.bits = sample_size >= 1 && sample_size <= 32 ? static_cast<std::uint8_t>(sample_size) : 0;
			if (c.bits == 0) c.codec.clear();
			a.bits_per_sample = c.bits;
			a.float_samples = c.float_samples;
			c.packet_bytes_per_channel = (c.bits + 7u) / 8u;
		}

		stream.codec = c.codec;

		if (c.frames_per_packet != 0 && a.sample_rate != 0)
		{
			stream.duration = static_cast<double>(frames) * c.frames_per_packet / a.sample_rate;
			const auto packet_bytes = std::uint64_t{c.packet_bytes} + std::uint64_t{c.packet_bytes_per_channel} * a.channels;
			stream.bit_rate = packet_bytes * 8 * a.sample_rate / c.frames_per_packet;
		}

		out.brand = form_type == aifc_type ? "AIFC" : "AIFF";
		out.duration = stream.duration;
		out.streams.push_back(std::move(stream));

		// Only the APPL packet is the file's, as read_packet reads it: one an ID3 tag carries is not.
		out.xmp.clear();
		if (packet) out.xmp = aiff_packet(r, *packet);

		if (intent != scan_intent::inspect) return;

		auto& rows = out.structure;
		const auto& s = out.streams.front();
		add_structure_section(rows, "Summary", "aiff.summary", true);
		add_structure_row(rows, "File size", std::format("{} bytes", size));
		add_structure_row(rows, "Form", std::format("{}, {} bytes{}", box_type_text(form_type), form_size, form_cut ? ", cut short" : ""));
		add_structure_row(rows, "Channels", std::to_string(s.audio.channels));
		add_structure_row(rows, "Sample frames", std::to_string(frames));
		add_structure_row(rows, "Sample size", std::format("{} bits", sample_size));
		add_structure_row(rows, "Sample rate", std::format("{} Hz", s.audio.sample_rate));

		if (aifc)
		{
			// The compression's name follows its type, as a Pascal string.
			std::string name;
			if (comm.size() > 22) name = text_from_unknown(std::span(comm).subspan(23, std::min<std::size_t>(comm[22], comm.size() - 23)));
			add_structure_row(rows, "Compression", name.empty() ? box_type_text(type) : std::format("{}, {}", box_type_text(type), name));
		}

		add_structure_section(rows, "Chunks", "aiff.chunks");
		std::size_t listed = 0;

		for (const auto& chunk : chunks)
		{
			if (++listed > max_listed_chunks)
			{
				add_structure_row(rows, "...", "more chunks not listed");
				break;
			}

			auto value = std::format("offset {}, {} bytes", chunk.start, chunk.size);

			if (chunk.id == ssnd_id && !chunk.cut && chunk.size >= 8)
			{
				value = std::format("sound data, {}, data offset {}, block size {}", value, r.be32(chunk.start + 8), r.be32(chunk.start + 12));
			}

			if (chunk.cut) value += ", cut short";
			auto& row = add_row(rows, box_type_text(chunk.id), std::move(value));
			row.depth = 1;
			row.id = std::format("aiff.chunk.{}", chunk.start);
		}

		finish_structure_sections(rows);
	}
}
