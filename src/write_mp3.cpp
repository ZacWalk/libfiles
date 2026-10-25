// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing metadata to MPEG audio files - MP3, MP2 and MPA - through their ID3v2 tag. The packet
// lives where the toolkit kept it, in a PRIV frame owned by "XMP"; the edit's other fields go to the text
// frames players read, a rating to the POPM frame Windows Media Player reads and writes.
//
// The tag keeps its version, and every frame the edit does not own is kept byte for byte. When the new
// frames fit the tag's space, the tag is patched where it lies at the size it has; otherwise the file is
// rewritten in one pass with a larger tag, given padding for the next edit. ID3v1, which readers consult
// only when there is no ID3v2 tag, is left as it is.

#include "legacy.h"

#include <algorithm>
#include <array>
#include <functional>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint64_t header_size = 10;
		// The padding a rewritten tag is given for later edits, as the toolkit gave it.
		constexpr std::uint64_t new_padding = 2048;
		// The largest size the tag header's 28 bits hold.
		constexpr std::uint64_t max_tag = (1ull << 28) - 1;

		constexpr std::string_view wmp_owner = "Windows Media Player 9 Series";

		std::uint32_t syncsafe(const std::uint8_t* const p)
		{
			return static_cast<std::uint32_t>(p[0]) << 21 | static_cast<std::uint32_t>(p[1]) << 14 |
				static_cast<std::uint32_t>(p[2]) << 7 | p[3];
		}

		void append_syncsafe(bytes& to, const std::uint32_t v)
		{
			for (auto shift = 21; shift >= 0; shift -= 7) to.push_back(static_cast<std::uint8_t>(v >> shift & 0x7F));
		}

		struct frame
		{
			std::string id;
			bytes raw;
			std::size_t header = 10;

			std::span<const std::uint8_t> body() const { return std::span(raw).subspan(header); }
		};

		struct tag
		{
			bool present = false;
			int version = 3;
			// What the tag takes at the start of the file: header, frames, padding and footer.
			std::uint64_t total = 0;
			std::vector<frame> frames;
		};

		bool valid_id(const std::span<const std::uint8_t> id)
		{
			return std::ranges::all_of(id, [](const std::uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
		}

		tag read_tag(reader& r)
		{
			tag t;
			if (r.size() < header_size) return t;

			const auto h = r.read(0, header_size);
			if (!starts_with(h, "ID3")) return t;

			t.present = true;
			t.version = h[3];
			const auto flags = h[5];

			if (t.version < 2 || t.version > 4 || h[4] == 0xFF) throw format_error("the ID3 tag's version is not one libfiles writes");
			if ((h[6] | h[7] | h[8] | h[9]) & 0x80) throw format_error("the ID3 tag's size is damaged");
			if (flags & 0x80) throw format_error("an ID3 tag with unsynchronisation cannot be updated");
			if (t.version == 2 && (flags & 0x40)) throw format_error("a compressed ID3v2.2 tag cannot be updated");

			const auto end = header_size + syncsafe(h.data() + 6);
			t.total = end + (t.version == 4 && (flags & 0x10) ? 10 : 0);
			if (t.total > r.size()) throw format_error("the ID3 tag runs past the end of the file");

			auto pos = header_size;

			// An extended header is skipped, and not written back: what it checks is rewritten.
			if (t.version >= 3 && (flags & 0x40))
			{
				if (end - pos < 4) throw format_error("the ID3 tag's extended header is damaged");
				const auto e = r.read(pos, 4);
				const std::uint64_t length = t.version == 3 ? load_be32(e.data()) + 4ull : syncsafe(e.data());
				if (length < 4 || length > end - pos) throw format_error("the ID3 tag's extended header is damaged");
				pos += length;
			}

			const std::size_t frame_header = t.version == 2 ? 6 : 10;
			const std::size_t id_length = t.version == 2 ? 3 : 4;

			while (end - pos >= frame_header)
			{
				const auto fh = r.read(pos, frame_header);
				if (fh[0] == 0 || !valid_id(std::span(fh).first(id_length))) break;

				std::uint64_t size;
				if (t.version == 2) size = static_cast<std::uint64_t>(fh[3]) << 16 | static_cast<std::uint64_t>(fh[4]) << 8 | fh[5];
				else if (t.version == 3 || ((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80)) size = load_be32(fh.data() + 4);
				else size = syncsafe(fh.data() + 4);

				if (size > end - pos - frame_header) throw format_error("an ID3 frame runs past the end of its tag");

				frame f;
				f.id.assign(fh.begin(), fh.begin() + static_cast<std::ptrdiff_t>(id_length));
				f.header = frame_header;
				f.raw = r.read(pos, frame_header + static_cast<std::size_t>(size));
				t.frames.push_back(std::move(f));
				pos += frame_header + size;
			}

			return t;
		}

		bool owned_by(const frame& f, const std::string_view owner)
		{
			const auto body = f.body();
			return body.size() > owner.size() && std::equal(owner.begin(), owner.end(), body.begin()) && body[owner.size()] == 0;
		}

		// A comment without a descriptor: the one players show.
		bool plain_comment(const frame& f)
		{
			const auto b = f.body();
			if (b.size() < 5) return false;

			const auto d = b.subspan(4);
			if (b[0] == 0 || b[0] == 3) return d[0] == 0;
			if (d.size() >= 2 && d[0] == 0 && d[1] == 0) return true;
			return d.size() >= 4 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF)) && d[2] == 0 && d[3] == 0;
		}

		bool is_ascii(const std::string_view text)
		{
			return std::ranges::all_of(text, [](const char c) { return static_cast<unsigned char>(c) < 0x80; });
		}

		struct writer
		{
			int version;

			std::string id(const char* v22, const char* v23) const { return version == 2 ? v22 : v23; }

			bytes frame_of(const std::string& frame_id, const bytes& body) const
			{
				bytes out(frame_id.begin(), frame_id.end());
				const auto size = static_cast<std::uint32_t>(body.size());

				if (version == 2)
				{
					out.push_back(static_cast<std::uint8_t>(size >> 16));
					out.push_back(static_cast<std::uint8_t>(size >> 8));
					out.push_back(static_cast<std::uint8_t>(size));
				}
				else
				{
					if (version == 3) append_be32(out, size);
					else append_syncsafe(out, size);
					out.push_back(0);
					out.push_back(0);
				}

				out.insert(out.end(), body.begin(), body.end());
				return out;
			}

			// Plain text when it is ASCII, otherwise UTF-16 with a byte order mark, as the toolkit chose.
			static void append_text(bytes& to, const std::string_view text)
			{
				if (is_ascii(text))
				{
					append(to, text);
				}
				else
				{
					to.push_back(0xFF);
					to.push_back(0xFE);
					const auto u = utf16le(text);
					to.insert(to.end(), u.begin(), u.end());
				}
			}

			static std::uint8_t encoding(const std::string_view text) { return is_ascii(text) ? 0 : 1; }

			bytes text(const std::string& frame_id, const std::string_view value) const
			{
				bytes body = {encoding(value)};
				append_text(body, value);
				return frame_of(frame_id, body);
			}

			bytes comment(const std::string_view value) const
			{
				bytes body = {encoding(value)};
				append(body, "eng");
				if (body[0] == 0) body.push_back(0);
				else body.insert(body.end(), {0xFF, 0xFE, 0, 0});
				append_text(body, value);
				return frame_of(id("COM", "COMM"), body);
			}
		};

		// The fields an edit owns, each with the frames it replaces and those it writes.
		struct field
		{
			std::function<bool(const frame&)> owns;
			std::vector<bytes> frames;
		};

		std::string two(const int v)
		{
			return std::string(1, static_cast<char>('0' + v / 10 % 10)) + static_cast<char>('0' + v % 10);
		}

		std::vector<field> fields_of(const writer& w, const tag& t, const metadata_update& update)
		{
			std::vector<field> fields;
			const auto& e = update.legacy;

			const auto by_id = [](std::vector<std::string> ids)
			{
				return [ids = std::move(ids)](const frame& f) { return std::ranges::find(ids, f.id) != ids.end(); };
			};

			const auto text = [&](const std::optional<std::string>& value, const char* v22, const char* v23)
			{
				if (!value) return;
				field f{by_id({w.id(v22, v23)}), {}};
				if (!value->empty()) f.frames.push_back(w.text(w.id(v22, v23), *value));
				fields.push_back(std::move(f));
			};

			if (update.xmp)
			{
				const auto frame_id = w.id("PRV", "PRIV");
				const auto owns = [frame_id](const frame& f) { return f.id == frame_id && owned_by(f, "XMP"); };

				// Sized to what the old packet took, so an edit that fits leaves every other frame where it is.
				std::uint64_t old_length = 0;
				auto terminated = true;

				if (const auto old = std::ranges::find_if(t.frames, owns); old != t.frames.end())
				{
					const auto packet = old->body().subspan(4);
					terminated = !packet.empty() && packet.back() == 0;
					old_length = packet.size() - (terminated ? 1 : 0);
				}

				bytes body = {'X', 'M', 'P', 0};
				append(body, serialize_packet(*update.xmp, old_length));
				if (terminated) body.push_back(0);
				fields.push_back({owns, {w.frame_of(frame_id, body)}});
			}

			text(e.title, "TT2", "TIT2");
			text(e.artist, "TP1", "TPE1");
			text(e.album, "TAL", "TALB");
			text(e.album_artist, "TP2", "TPE2");
			text(e.genre, "TCO", "TCON");
			text(e.track, "TRK", "TRCK");
			text(e.disc, "TPA", "TPOS");
			text(e.copyright, "TCR", "TCOP");

			if (e.comment)
			{
				const auto frame_id = w.id("COM", "COMM");
				field f{[frame_id](const frame& fr) { return fr.id == frame_id && plain_comment(fr); }, {}};
				if (!e.comment->empty()) f.frames.push_back(w.comment(*e.comment));
				fields.push_back(std::move(f));
			}

			if (e.copyright_url)
			{
				const auto frame_id = w.id("WCP", "WCOP");
				field f{by_id({frame_id}), {}};
				if (!e.copyright_url->empty()) f.frames.push_back(w.frame_of(frame_id, to_bytes(*e.copyright_url)));
				fields.push_back(std::move(f));
			}

			if (e.rating)
			{
				// Windows Media Player's scale, as Diffractor's fork of the toolkit wrote it.
				static constexpr std::uint8_t star_to_byte[6] = {0, 1, 64, 128, 196, 255};
				const auto frame_id = w.id("POP", "POPM");
				field f{[frame_id](const frame& fr) { return fr.id == frame_id && owned_by(fr, wmp_owner); }, {}};

				if (*e.rating > 0)
				{
					bytes body;
					append(body, wmp_owner);
					body.push_back(0);
					body.push_back(star_to_byte[std::min(*e.rating, 5)]);
					body.insert(body.end(), 4, 0);
					f.frames.push_back(w.frame_of(frame_id, body));
				}

				fields.push_back(std::move(f));
			}

			// The date: TDRC in ID3v2.4, the year, day and time frames before it.
			std::optional<date_parts> date;
			if (e.date_digitized) date = parse_xmp_date(*e.date_digitized);
			if (!date && e.year)
			{
				date_parts year_only;
				year_only.year = *e.year;
				date = year_only;
			}

			if (date && date->year > 0 && date->year <= 9999)
			{
				const auto year = std::to_string(date->year);
				field f{by_id(w.version == 2 ? std::vector<std::string>{"TYE", "TDA", "TIM"}
				                             : std::vector<std::string>{"TYER", "TDAT", "TIME", "TDRC"}), {}};

				if (w.version == 4)
				{
					auto value = std::string(4 - std::min<std::size_t>(4, year.size()), '0') + year;
					if (date->month > 0) value += '-' + two(date->month) + '-' + two(date->day);
					if (date->month > 0 && date->has_time) value += 'T' + two(date->hour) + ':' + two(date->minute) + ':' + two(date->second);
					f.frames.push_back(w.text("TDRC", value));
				}
				else
				{
					f.frames.push_back(w.text(w.id("TYE", "TYER"), year));
					if (date->month > 0) f.frames.push_back(w.text(w.id("TDA", "TDAT"), two(date->day) + two(date->month)));
					if (date->month > 0 && date->has_time) f.frames.push_back(w.text(w.id("TIM", "TIME"), two(date->hour) + two(date->minute)));
				}

				fields.push_back(std::move(f));
			}

			return fields;
		}
	}

	bool is_mpeg_audio(const std::span<const std::uint8_t> head) noexcept
	{
		if (head.size() >= 4 && head[0] == 'I' && head[1] == 'D' && head[2] == '3') return head[3] >= 2 && head[3] <= 4;
		if (head.size() < 4 || head[0] != 0xFF || (head[1] & 0xE0) != 0xE0) return false;

		// A frame header: a known version and layer, and a bit rate and sample rate that exist.
		return ((head[1] >> 3) & 3) != 1 && ((head[1] >> 1) & 3) != 0 && (head[2] >> 4) != 15 && ((head[2] >> 2) & 3) != 3;
	}

	void read_mp3_packet(reader& r, packet_reading& out)
	{
		const auto t = read_tag(r);
		const auto frame_id = t.version == 2 ? "PRV" : "PRIV";

		for (const auto& f : t.frames)
		{
			if (f.id != frame_id || !owned_by(f, "XMP")) continue;

			const auto body = f.body().subspan(4);
			out.packet.assign(body.begin(), body.end());
			while (!out.packet.empty() && out.packet.back() == '\0') out.packet.pop_back();
			break;
		}

		out.readable = true;
	}

	void plan_mp3(reader& r, const metadata_update& update, write_plan& plan)
	{
		const auto t = read_tag(r);
		const writer w{t.present ? t.version : 3};
		const auto fields = fields_of(w, t, update);
		if (fields.empty()) return;

		bytes frames;
		std::vector<bool> written(fields.size(), false);

		for (const auto& f : t.frames)
		{
			const auto owner = std::ranges::find_if(fields, [&f](const field& d) { return d.owns(f); });

			if (owner == fields.end())
			{
				frames.insert(frames.end(), f.raw.begin(), f.raw.end());
				continue;
			}

			const auto index = static_cast<std::size_t>(owner - fields.begin());
			if (written[index]) continue;

			for (const auto& n : owner->frames) frames.insert(frames.end(), n.begin(), n.end());
			written[index] = true;
		}

		for (std::size_t i = 0; i < fields.size(); ++i)
		{
			if (written[i]) continue;
			for (const auto& n : fields[i].frames) frames.insert(frames.end(), n.begin(), n.end());
		}

		const auto fits = t.present && header_size + frames.size() <= t.total;
		const auto total = fits ? t.total : header_size + frames.size() + new_padding;
		if (total - header_size > max_tag) throw format_error("the ID3 tag would grow past what its header can hold");

		bytes tag_bytes = {'I', 'D', '3', static_cast<std::uint8_t>(w.version), 0, 0};
		append_syncsafe(tag_bytes, static_cast<std::uint32_t>(total - header_size));
		tag_bytes.insert(tag_bytes.end(), frames.begin(), frames.end());
		tag_bytes.resize(static_cast<std::size_t>(total), 0);

		if (fits)
		{
			const auto original = r.read(0, static_cast<std::size_t>(t.total));
			if (auto p = changed_span(0, original, tag_bytes, 0)) plan.replacements.push_back(std::move(*p));
		}
		else
		{
			plan.replacements.push_back({0, t.total, std::move(tag_bytes)});
		}
	}
}
