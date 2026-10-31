// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning MPEG audio files - MP3, MP2 and MP1 - with their tags: ID3v2 at the start, and at the
// end an APE tag, Lyrics3 lyrics, an appended ID3v2 tag and ID3v1. The stream is known by its first frame,
// confirmed by the frame its length leads to. How long it plays is the frame count an Xing, Info or VBRI
// header in that frame states, less the encoder delay and padding a LAME tag states - or the samples iTunes'
// gapless figures state - as Diffractor's FFmpeg counts them; without either, the audio's bytes over the
// first frame's bit rate.

#include "id3_internal.h"
#include "media_internal.h"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace libfiles::detail
{
	namespace
	{
		// FFmpeg's demuxer looks this far past the tags for the first frame.
		constexpr std::uint64_t max_junk = 64 * 1024;
		// The sync word, version, layer and sample rate, which every frame of a stream repeats.
		constexpr std::uint32_t stream_bits = 0xFFFE0C00;
		// How many tags are looked for at either end, in whatever order taggers left them.
		constexpr int max_tags = 8;
		// An Xing header with all its fields, and the LAME tag after it up to its delay and padding.
		constexpr std::uint64_t max_xing = 8 + 4 + 4 + 100 + 4 + 24;

		struct frame
		{
			std::uint64_t offset = 0;
			std::uint32_t header = 0;
			audio_header audio;
			std::string codec;
		};

		bool parse_frame(const std::uint32_t header, frame& f)
		{
			f.header = header;
			f.audio = {};
			f.codec.clear();
			return parse_mpeg_audio_header(header, f.audio, f.codec) && f.audio.frame_bytes >= 4 && f.audio.sample_rate != 0;
		}

		bool same_stream(const std::uint32_t a, const std::uint32_t b)
		{
			return (a & stream_bits) == (b & stream_bits);
		}

		// The first frame at most max_junk past start whose length leads to the header of another frame of
		// the stream, to the end of the file, or to a tag after the audio - a stream of one frame. Read through
		// the reader's window a byte at a time, so a stream that starts where it should costs no read past
		// its first frames.
		std::optional<frame> find_first_frame(reader& r, const std::uint64_t start)
		{
			const auto size = r.size();
			const auto last = std::min(size, start + max_junk);

			for (auto at = start; at < last && size - at >= 4; ++at)
			{
				if (r.u8(at) != 0xFF) continue;

				frame f;
				if (!parse_frame(r.be32(at), f)) continue;

				const auto next = at + f.audio.frame_bytes;
				auto confirmed = next == size;

				if (!confirmed && r.contains(next, 4))
				{
					// The frame and what follows it in one view, which keeps a frame that straddles two of the
					// reader's blocks from costing a read each way.
					const auto through = r.view(at, static_cast<std::size_t>(std::min<std::uint64_t>(size - at, f.audio.frame_bytes + 11)));
					const auto after = through.subspan(f.audio.frame_bytes);
					frame following;
					confirmed = starts_with(after, "TAG") || starts_with(after, "APETAGEX") || starts_with(after, "LYRICSBEGIN") ||
						starts_with(after, "ID3") || (same_stream(load_be32(after.data()), f.header) && parse_frame(load_be32(after.data()), following));
				}

				if (confirmed)
				{
					f.offset = at;
					return f;
				}
			}

			return {};
		}

		// What an Xing or Info header - or a VBRI header, as Fraunhofer's encoder writes - in the first frame
		// says of the stream, and the LAME tag after an Xing or Info header.
		struct vbr_header
		{
			std::string_view kind;
			std::uint32_t frames = 0;
			std::uint32_t bytes = 0;
			// An Info header marks a constant bit rate.
			bool constant = false;
			std::string encoder;
			std::uint32_t delay = 0;
			std::uint32_t padding = 0;
			// Whether the delay and padding are taken off the duration: FFmpeg takes them only from the
			// encoders it knows to write them.
			bool trims = false;
		};

		// Read where FFmpeg reads them: an Xing header after the side information, whose size depends on
		// the version and on whether the frame is mono, and a VBRI header 32 bytes after the frame header.
		vbr_header read_vbr_header(reader& r, const frame& first)
		{
			vbr_header v;
			const auto lsf = first.audio.samples_per_frame < 1152;
			const auto mono = first.audio.channels == 1;
			const std::uint64_t side_info = lsf ? (mono ? 9 : 17) : (mono ? 17 : 32);
			const auto xing = first.offset + 4 + side_info;

			if (r.contains(xing, 8))
			{
				const auto b = r.view(xing, static_cast<std::size_t>(std::min(r.size() - xing, max_xing)));

				if (starts_with(b, "Xing") || starts_with(b, "Info"))
				{
					v.kind = starts_with(b, "Info") ? "Info" : "Xing";
					v.constant = v.kind == "Info";

					const auto flags = load_be32(b.data() + 4);
					std::size_t at = 8;

					if ((flags & 1) && at + 4 <= b.size())
					{
						v.frames = load_be32(b.data() + at);
						at += 4;
					}

					if ((flags & 2) && at + 4 <= b.size())
					{
						v.bytes = load_be32(b.data() + at);
						at += 4;
					}

					if (flags & 4) at += 100;
					if (flags & 8) at += 4;

					// The LAME tag: a nine-character encoder version, and twelve bytes on the delay and the
					// padding, twelve bits each.
					if (at + 24 <= b.size())
					{
						const auto lame = b.subspan(at);
						v.encoder = trim_text(text_from_windows_1252(lame.first(9)));
						const auto both = static_cast<std::uint32_t>(lame[21]) << 16 | static_cast<std::uint32_t>(lame[22]) << 8 | lame[23];
						v.delay = both >> 12;
						v.padding = both & 0xFFF;
						v.trims = starts_with(lame, "LAME") || starts_with(lame, "Lavf") || starts_with(lame, "Lavc");
					}

					// As FFmpeg: a file more than a sixteenth larger than its header says is streams joined
					// together, and the first one's frame count is not the file's.
					const auto rest = r.size() - std::min(r.size(), first.offset + 4);
					if (v.bytes != 0 && rest > v.bytes && rest - v.bytes > v.bytes / 16) v.frames = 0;
				}
			}

			const auto vbri = first.offset + 4 + 32;

			if (r.contains(vbri, 18))
			{
				const auto b = r.view(vbri, 18);

				if (starts_with(b, "VBRI") && load_be16(b.data() + 4) == 1)
				{
					v.kind = "VBRI";
					v.bytes = load_be32(b.data() + 10);
					v.frames = load_be32(b.data() + 14);
				}
			}

			return v;
		}

		// Lyrics3 lyrics that end at end, before an ID3v1 tag: version 2's sized by the six digits before
		// "LYRICS200", version 1's from "LYRICSBEGIN" to "LYRICSEND", with at most 5100 bytes between.
		// Returns the bytes they take, or 0.
		std::uint64_t lyrics3_size(reader& r, const std::uint64_t start, const std::uint64_t end)
		{
			constexpr std::string_view begin = "LYRICSBEGIN";
			if (end - start < 15 + begin.size()) return 0;

			const auto footer = r.view(end - 15, 15);

			if (starts_with(footer.subspan(6), "LYRICS200"))
			{
				std::uint64_t length = 0;

				for (std::size_t i = 0; i < 6; ++i)
				{
					if (footer[i] < '0' || footer[i] > '9') return 0;
					length = length * 10 + (footer[i] - '0');
				}

				if (length < begin.size() || length > end - start - 15) return 0;
				return starts_with(r.view(end - 15 - length, begin.size()), begin) ? length + 15 : 0;
			}

			if (starts_with(footer.subspan(6), "LYRICSEND"))
			{
				const auto length = std::min<std::uint64_t>(end - start, 5100 + begin.size() + 9);
				const auto bytes = r.view(end - length, static_cast<std::size_t>(length));
				const auto found = std::search(bytes.begin(), bytes.end(), begin.begin(), begin.end(),
				                               [](const std::uint8_t a, const char b) { return a == static_cast<std::uint8_t>(b); });
				if (found != bytes.end()) return length - static_cast<std::uint64_t>(found - bytes.begin());
			}

			return 0;
		}

		// An ID3v2.4 tag appended after the audio, found from its footer, "3DI", which repeats its header.
		// Returns the bytes it takes, or 0.
		std::uint64_t appended_id3v2_size(reader& r, const std::uint64_t start, const std::uint64_t end)
		{
			if (end - start < 20) return 0;

			const auto f = r.view(end - 10, 10);
			if (!starts_with(f, "3DI") || f[3] != 4 || f[4] == 0xFF || ((f[6] | f[7] | f[8] | f[9]) & 0x80)) return 0;

			const auto length = 20 + std::uint64_t{id3_syncsafe(f.data() + 6)};
			if (length > end - start) return 0;
			return starts_with(r.view(end - length, 3), "ID3") ? length : 0;
		}

		// An ID3v2 tag other than the one at the start of the file adds its frames, but its XMP packet is not
		// the one read_packet finds, so it does not replace the first tag's.
		std::uint64_t read_other_id3v2(reader& r, const std::uint64_t offset, media_budget& budget, media_scan_result& out)
		{
			auto xmp = std::move(out.xmp);
			out.xmp.clear();
			const auto length = read_id3v2(r, offset, budget, out);
			out.xmp = std::move(xmp);
			return length;
		}

		// The gapless playback figures iTunes writes to a comment named iTunSMPB, in hexadecimal: a field
		// nothing reads, the encoder delay, the padding, and the samples that play.
		struct itunes_gapless
		{
			std::uint64_t delay = 0;
			std::uint64_t padding = 0;
			std::uint64_t samples = 0;
		};

		// As Diffractor's FFmpeg reads them: four fields of at most sixteen digits, each a count it can add.
		std::optional<itunes_gapless> parse_itunes_gapless(std::string_view text)
		{
			const auto digit = [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
			const auto space = [](const char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
			std::uint64_t fields[4] = {};

			for (auto& field : fields)
			{
				while (!text.empty() && space(text.front())) text.remove_prefix(1);

				std::size_t length = 0;

				for (; length < text.size() && digit(text[length]); ++length)
				{
					if (length == 16) return {};
					const auto c = text[length];
					field = field << 4 | static_cast<std::uint64_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
				}

				if (length == 0) return {};
				text.remove_prefix(length);
			}

			constexpr std::uint64_t most = std::uint64_t{1} << 40;
			if (fields[1] > most || fields[2] > most || fields[3] > most) return {};
			return itunes_gapless{fields[1], fields[2], fields[3]};
		}

		// The figures where Diffractor's FFmpeg looks for them among the tags read so far: in the first comment
		// whose description is iTunSMPB, in whatever language, and failing that in the first user-defined text
		// frame of that name, as files remuxed by older FFmpeg keep them - ID3v2.2's frames as well as 2.3's
		// and 2.4's, the name matched without regard to case, as FFmpeg's metadata keys are.
		std::optional<itunes_gapless> find_itunes_gapless(const media_scan_result& out)
		{
			const auto named = [&out](const std::string_view id, const std::string_view short_id) -> const media_tag*
			{
				constexpr std::string_view name = "iTunSMPB";
				const auto same = [](const char a, const char b) { return (a | 0x20) == (b | 0x20); };

				for (const auto& tag : out.tags)
				{
					const std::string_view key = tag.key;
					const auto colon = key.find(':');
					if (colon == std::string_view::npos || (key.substr(0, colon) != id && key.substr(0, colon) != short_id)) continue;
					if (std::ranges::equal(key.substr(colon + 1), name, same)) return &tag;
				}

				return nullptr;
			};

			auto tag = named("COMM", "COM");
			if (!tag) tag = named("TXXX", "TXX");
			if (!tag) return {};
			return parse_itunes_gapless(tag->value);
		}

		std::string_view channel_text(const std::uint32_t channels)
		{
			return channels == 1 ? "mono" : "stereo";
		}
	}

	void scan_mpeg_audio_media(reader& r, const scan_intent intent, const std::uint64_t tag_size, media_budget& budget, media_scan_result& out)
	{
		const auto size = r.size();
		const auto inspect = intent == scan_intent::inspect;
		std::vector<std::pair<std::uint64_t, structure_row>> tag_rows;
		std::vector<structure_row> audio_rows;

		const auto tag_row = [&](const std::uint64_t offset, std::string key, const std::uint64_t length)
		{
			if (!inspect) return;
			structure_row row;
			row.key = std::move(key);
			row.value = std::format("offset {}, {} bytes", offset, length);
			row.depth = 1;
			tag_rows.emplace_back(offset, std::move(row));
		};

		const auto audio_row = [&](std::string key, std::string value)
		{
			if (inspect) add_structure_row(audio_rows, key, std::move(value));
		};

		if (tag_size != 0) tag_row(0, std::format("ID3v2.{} tag", r.u8(3)), tag_size);

		// FFmpeg reads a second ID3v2 tag that follows the first, as some taggers leave.
		auto start = std::min(tag_size, size);

		for (auto i = 0; i < max_tags && r.contains(start, 10) && starts_with(r.view(start, 3), "ID3"); ++i)
		{
			const auto version = r.u8(start + 3);
			const auto length = std::min(read_other_id3v2(r, start, budget, out), size - start);
			if (length == 0) break;

			tag_row(start, std::format("ID3v2.{} tag", version), length);
			start += length;
		}

		if (start >= size) throw format_error("the file holds no MPEG audio after its tags");

		const auto first = find_first_frame(r, start);
		if (!first) throw format_error("no MPEG audio frame was found where the audio should begin");

		const auto& audio = first->audio;
		if (first->offset > start) audio_row("Junk", std::format("{} bytes before the first frame", first->offset - start));

		audio_row("First frame", std::format("offset {}, {}, {} kbit/s, {} Hz, {}, {} bytes", first->offset, audio.profile,
		                                     audio.bit_rate / 1000, audio.sample_rate, channel_text(audio.channels), audio.frame_bytes));

		// FFmpeg reads these headers only in Layer III streams.
		const auto vbr = first->codec == "mp3" ? read_vbr_header(r, *first) : vbr_header{};
		const auto has_vbr = vbr.frames != 0 || vbr.bytes != 0;

		if (!vbr.kind.empty())
		{
			audio_row(std::format("{} header", vbr.kind), std::format("{} frames, {} bytes", vbr.frames, vbr.bytes));

			if (!vbr.encoder.empty()) audio_row("LAME tag", std::format("{}, encoder delay {}, padding {}", vbr.encoder, vbr.delay, vbr.padding));
		}

		// Diffractor's FFmpeg takes iTunes' gapless figures from the tags at the start when no LAME tag gave a
		// delay and padding, and when a frame count, if there is one, agrees with them.
		const std::uint64_t samples_per_frame = audio.samples_per_frame;
		const auto total = std::uint64_t{vbr.frames} * samples_per_frame;
		std::optional<std::uint64_t> itunes_samples;

		if (first->codec == "mp3" && !vbr.trims)
		{
			if (const auto g = find_itunes_gapless(out); g && g->delay <= 16384 && g->samples != 0 && (total == 0 || g->delay + g->samples + g->padding == total))
			{
				itunes_samples = g->samples;
			}
		}

		// A frame that holds an Xing, Info or VBRI header plays as silence, and so is not audio; the bit rate
		// of the frame after it is the stream's.
		const auto audio_start = has_vbr ? first->offset + audio.frame_bytes : first->offset;
		auto frame_bit_rate = audio.bit_rate;

		if (has_vbr && r.contains(audio_start, 4))
		{
			frame next;
			if (parse_frame(r.be32(audio_start), next) && same_stream(next.header, first->header)) frame_bit_rate = next.audio.bit_rate;
		}

		// The tags after the audio, found from the end: ID3v1 in the last 128 bytes, an extended ID3v1 tag
		// before it, and before those any of the others.
		auto end = size;
		std::optional<std::uint64_t> id3v1;

		if (end - first->offset >= 128 && starts_with(r.view(end - 128, 3), "TAG"))
		{
			id3v1 = end - 128;
			end -= 128;
			tag_row(end, "ID3v1 tag", 128);

			if (end - first->offset >= 227 && starts_with(r.view(end - 227, 4), "TAG+"))
			{
				end -= 227;
				tag_row(end, "Extended ID3v1 tag", 227);
			}
		}

		for (auto i = 0; i < max_tags && end > first->offset; ++i)
		{
			if (const auto ape = std::min(read_ape_tag(r, end, out), end - first->offset); ape != 0)
			{
				end -= ape;
				tag_row(end, "APE tag", ape);
			}
			else if (const auto lyrics = lyrics3_size(r, first->offset, end); lyrics != 0)
			{
				end -= lyrics;
				tag_row(end, "Lyrics3 tag", lyrics);
			}
			else if (const auto appended = appended_id3v2_size(r, first->offset, end); appended != 0)
			{
				end -= appended;
				read_other_id3v2(r, end, budget, out);
				tag_row(end, "ID3v2.4 tag after the audio", appended);
			}
			else
			{
				break;
			}
		}

		const auto audio_end = std::max(end, audio_start);
		audio_row("Audio", std::format("offset {}, {} bytes", audio_start, audio_end - audio_start));

		media_stream stream;
		stream.kind = media_stream_kind::audio;
		stream.codec = first->codec;
		stream.bit_rate = frame_bit_rate;
		apply_audio_header(audio, stream.audio);

		// A counted stream's bit rate is its bytes over its frames' samples, unless Info marks it constant.
		if (total != 0 && vbr.bytes != 0 && !vbr.constant) stream.bit_rate = (std::uint64_t{vbr.bytes} * 8 * audio.sample_rate + total / 2) / total;

		const auto trimmed = vbr.trims ? std::uint64_t{vbr.delay} + vbr.padding : 0;

		if (itunes_samples)
		{
			stream.duration = static_cast<double>(*itunes_samples) / audio.sample_rate;
			audio_row("Duration", std::format("{} samples from iTunSMPB", *itunes_samples));
		}
		else if (total > trimmed)
		{
			stream.duration = static_cast<double>(total - trimmed) / audio.sample_rate;
			audio_row("Duration", std::format("{} samples from the frame count", total - trimmed));
		}
		else if (frame_bit_rate != 0)
		{
			stream.duration = static_cast<double>(audio_end - audio_start) * 8 / static_cast<double>(frame_bit_rate);
			audio_row("Duration", "from the bit rate");
		}

		out.brand = audio.profile;
		out.streams.push_back(std::move(stream));

		// Last, as it takes only what no other tag gave.
		if (id3v1) read_id3v1(r, *id3v1, out);

		if (inspect)
		{
			std::ranges::stable_sort(tag_rows, {}, &std::pair<std::uint64_t, structure_row>::first);
			add_structure_section(out.structure, "Tags", "mpa.tags");
			for (auto& row : tag_rows) out.structure.push_back(std::move(row.second));
			add_structure_section(out.structure, "Audio", "mpa.audio", true);
			for (auto& row : audio_rows) out.structure.push_back(std::move(row));
			finish_structure_sections(out.structure);
		}
	}
}
