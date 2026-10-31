// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing metadata to RIFF files - WAV and AVI. The packet lives where the toolkit kept it, in a
// `_PMX` chunk at the top of the RIFF form; title, artist, comment, copyright, date and genre go to the
// LIST INFO chunk, a WAV's title to its DISP chunk, and an AVI's original date to IDIT.
//
// Nothing that holds media moves, so an AVI's index offsets stay true. A chunk that fits is patched where
// it lies. One that grows takes the JUNK chunk after it: written inside the JUNK first, then made the
// chunk read by rewriting the eight-byte header in front of it. Otherwise it is appended at the end of
// the form, which the form's size then takes in, and only then does the old chunk become JUNK. Wherever
// the update is interrupted, the file reads whole.
//
// The scan reads an AVI's packet from the first `_PMX` after `movi` in its first form, so that is where it
// is written. It reads every INFO list in the first form and, where two give the same field, takes the one
// before `movi`; so the list edited is the last before `movi`, else the last there is, and one that has to
// move takes room before `movi` where there is some. An AVI whose first form cannot grow - one that
// continues in AVIX forms - takes only what fits. The forms and chunks are read by riff_internal.h, as the
// media scan reads them, and only from a file that stands whole.

#include "legacy.h"
#include "riff_internal.h"
#include "vocabulary_internal.h"

#include <algorithm>
#include <array>
#include <functional>

namespace libfiles::detail
{
	using namespace riff;

	namespace
	{
		// The IDIT chunk's payload: "Www Mmm DD HH:MM:SS YYYY\n" and a NUL.
		constexpr std::uint32_t idit_size = 26;

		// After what is appended: first what makes it part of the file and what is written inside JUNK,
		// then what makes it the chunk read and frees what it replaces.
		constexpr std::uint8_t reveal = 0;
		constexpr std::uint8_t commit = 1;

		bytes make_chunk(const std::uint32_t id, const std::span<const std::uint8_t> payload)
		{
			bytes out;
			append_le32(out, id);
			append_le32(out, static_cast<std::uint32_t>(payload.size()));
			out.insert(out.end(), payload.begin(), payload.end());
			if (payload.size() & 1) out.push_back(0);
			return out;
		}

		bytes text_value(const std::string& text)
		{
			bytes value(text.begin(), text.end());
			value.push_back(0);
			return value;
		}

		// The INFO list with the edit's fields set, the others kept as they are. Empty when nothing is left.
		std::optional<bytes> updated_info(reader& r, const chunk* existing, const legacy_edits& e)
		{
			std::vector<std::pair<std::uint32_t, std::string>> fields;

			if (e.title) fields.emplace_back(fourcc(vocabulary::info_title), *e.title);
			if (e.artist) fields.emplace_back(fourcc(vocabulary::info_artist), *e.artist);
			if (e.comment) fields.emplace_back(fourcc(vocabulary::info_comment), *e.comment);
			if (e.copyright) fields.emplace_back(fourcc(vocabulary::info_copyright), *e.copyright);
			if (e.genre) fields.emplace_back(fourcc(vocabulary::info_genre), *e.genre);
			if (e.date_digitized) fields.emplace_back(fourcc(vocabulary::info_created), *e.date_digitized);
			else if (e.year) fields.emplace_back(fourcc(vocabulary::info_created), std::to_string(*e.year));

			if (fields.empty()) return {};

			std::vector<std::pair<std::uint32_t, bytes>> items;

			if (existing && existing->size >= 4)
			{
				for (const auto& c : parse_children(r, existing->payload() + 4, existing->payload() + existing->size))
				{
					auto item = r.read(c.start, 8 + static_cast<std::size_t>(c.size));
					if (c.size & 1) item.push_back(0);
					items.emplace_back(c.id, std::move(item));
				}
			}

			for (const auto& [id, value] : fields)
			{
				const auto first = std::ranges::find(items, id, &std::pair<std::uint32_t, bytes>::first);
				const auto at = static_cast<std::size_t>(first - items.begin());
				std::erase_if(items, [id](const auto& item) { return item.first == id; });

				if (value.empty()) continue;

				auto item = make_chunk(id, text_value(value));
				items.insert(items.begin() + static_cast<std::ptrdiff_t>(std::min(at, items.size())), {id, std::move(item)});
			}

			if (items.empty()) return bytes{};

			bytes payload;
			append_le32(payload, info_type);
			for (const auto& item : items) payload.insert(payload.end(), item.second.begin(), item.second.end());
			return make_chunk(list_id, payload);
		}

		// The IDIT text for a date, as the toolkit wrote it: "Www Mmm DD HH:MM:SS YYYY\n" and a NUL.
		std::optional<bytes> idit_text(const std::string& xmp_date)
		{
			auto d = parse_xmp_date(xmp_date);
			if (!d || d->year < 1800 || d->year > 3999 || d->month < 1 || d->month > 12 || d->day < 1 || d->day > 31) return {};

			static constexpr const char* months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
			static constexpr const char* days[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};

			auto day_only = *d;
			day_only.hour = day_only.minute = day_only.second = 0;
			day_only.zone = libfiles::date::time_zone::unstated;
			const auto since_1970 = seconds_since(1970, day_only) / 86400;
			const auto weekday = static_cast<std::size_t>(((since_1970 % 7) + 7) % 7);

			const auto two = [](const int v)
			{
				return std::string(1, static_cast<char>('0' + v / 10 % 10)) + static_cast<char>('0' + v % 10);
			};

			std::string text = days[weekday];
			text += ' ';
			text += months[d->month - 1];
			text += ' ' + two(d->day) + ' ' + two(d->hour) + ':' + two(d->minute) + ':' + two(d->second) + ' ' + std::to_string(d->year) + '\n';
			if (text.size() != idit_size - 1) return {};

			return text_value(text);
		}
	}

	bool is_riff_media(const std::span<const std::uint8_t> head) noexcept
	{
		if (head.size() < 12 || load_le32(head.data()) != riff_id) return false;
		const auto type = load_le32(head.data() + 8);
		return type == wave_type || type == avi_type;
	}

	void read_riff_packet(reader& r, packet_reading& out)
	{
		const auto packet = read_current_packet(r, read_layout(r));
		out.packet.assign(packet.begin(), packet.end());
		out.readable = true;
	}

	void plan_riff(reader& r, const metadata_update& update, write_plan& plan)
	{
		const auto l = read_layout(r);
		const auto& first = l.first();
		const auto& children = first.children;
		bytes appended;

		const auto patch = [&plan](const std::uint64_t offset, bytes data, const std::uint8_t step)
		{
			const auto length = data.size();
			plan.replacements.push_back({offset, length, std::move(data), step});
		};

		// A chunk becomes JUNK by its id alone, so it keeps its size and what follows it stays.
		const auto to_junk = [&patch](const chunk& c)
		{
			bytes id;
			append_le32(id, junk_id);
			patch(c.start, std::move(id), commit);
		};

		const auto next_of = [&children](const chunk& c) -> const chunk*
		{
			const auto it = std::ranges::find(children, c.start, &chunk::start);
			return it != children.end() && it + 1 != children.end() ? &*(it + 1) : nullptr;
		};

		// A JUNK chunk becomes the chunk read after what is written inside it.
		std::vector<std::uint64_t> taken;

		// True when the file holds exactly these bytes as the chunk, its pad included.
		const auto same_bytes = [&r](const chunk& c, const bytes& whole)
		{
			return c.padded_size() == whole.size() && c.end() <= r.size() && r.read(c.start, whole.size()) == whole;
		};

		// Writes the new chunk inside the JUNK chunk `room`, then makes it the chunk read with one header
		// write: at `room` itself, or at `before`, the chunk in front of it, which becomes JUNK too.
		const auto place_in_junk = [&](const chunk& room, const chunk* before, const bytes& whole) -> bool
		{
			if (room.id != junk_id || std::ranges::find(taken, room.start) != taken.end()) return false;

			const auto at = room.payload();
			if (whole.size() > room.end() - at) return false;

			const auto tail = room.end() - at - whole.size();
			if (tail != 0 && tail < 8) return false;

			auto body = whole;
			if (tail != 0)
			{
				append_le32(body, junk_id);
				append_le32(body, static_cast<std::uint32_t>(tail - 8));
			}

			patch(at, std::move(body), reveal);

			const auto head_at = before ? before->start : room.start;
			bytes head;
			append_le32(head, junk_id);
			append_le32(head, static_cast<std::uint32_t>(at - head_at - 8));
			patch(head_at, std::move(head), commit);

			taken.push_back(room.start);
			return true;
		};

		// Puts a whole chunk - header, payload and pad - in place of `old` when `keep` accepts where it lies,
		// otherwise in a JUNK chunk `room` accepts, otherwise at the end of the form.
		const auto put = [&](const chunk* old, const bytes& whole, const std::function<bool(const chunk&)>& keep,
		                     const std::function<bool(const chunk&)>& room)
		{
			if (old && keep(*old))
			{
				if (old->padded_size() == whole.size() && old->end() <= r.size())
				{
					const auto original = r.read(old->start, whole.size());
					if (auto p = changed_span(old->start, original, whole, commit)) plan.replacements.push_back(std::move(*p));
					return;
				}

				if (const auto* const next = next_of(*old); next && room(*next) && place_in_junk(*next, old, whole)) return;
			}

			auto placed = false;

			for (const auto& c : children)
			{
				if (c.id == junk_id && room(c) && place_in_junk(c, nullptr, whole))
				{
					placed = true;
					break;
				}
			}

			if (!placed) appended.insert(appended.end(), whole.begin(), whole.end());
			if (old) to_junk(*old);
		};

		const auto anywhere = [](const chunk&) { return true; };
		// Where the scan reads an AVI's packet, and so where it is written: after `movi`, in the first form.
		const auto after_movi = [&l, &first](const chunk& c) { return !l.avi || (c.start >= l.movi_end && c.start < first.data_end()); };
		const auto before_movi = [&l](const chunk& c) { return !l.avi || c.start < l.movi_end; };

		// The packet, and any stale copy beside it.
		if (update.xmp)
		{
			const auto* const current = current_packet(l);
			const auto old_length = current && after_movi(*current) ? current->size : 0;
			auto packet = serialize_packet(*update.xmp, old_length);

			if (current && after_movi(*current) && packet.size() == old_length)
			{
				patch(current->payload(), to_bytes(packet), commit);
			}
			else
			{
				if (packet.size() & 1) packet.push_back(' ');
				put(current, make_chunk(pmx_id, {reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size()}), after_movi, after_movi);
			}

			for (const auto& f : l.forms)
			{
				for (const auto& c : f.children)
				{
					if (c.id == pmx_id && &c != current) to_junk(c);
				}
			}
		}

		// LIST INFO: the list whose fields the scan reads over any other's - an AVI's last before `movi`, else the
		// last in the form.
		{
			const chunk* info = nullptr;

			for (const auto& c : children)
			{
				if (c.id != list_id || c.type != info_type) continue;
				if (!info || !l.avi || before_movi(c) || !before_movi(*info)) info = &c;
			}

			if (const auto updated = updated_info(r, info, update.legacy))
			{
				if (updated->empty())
				{
					if (info) to_junk(*info);
				}
				else if (!info || !same_bytes(*info, *updated))
				{
					// An INFO list that has to move takes a JUNK chunk before an AVI's `movi` where one has room,
					// where its fields go on winning over a list after `movi`; else it goes to the end of the form.
					put(info, *updated, anywhere, before_movi);
				}
			}
		}

		// A WAV's DISP title, where the file has one.
		if (!l.avi && update.legacy.title)
		{
			const chunk* disp = nullptr;
			for (const auto& c : children) if (c.id == disp_id && c.size >= 4 && r.le32(c.payload()) == 1) disp = &c;

			if (disp)
			{
				if (update.legacy.title->empty())
				{
					to_junk(*disp);
				}
				else
				{
					bytes payload;
					append_le32(payload, 1);
					const auto text = text_value(*update.legacy.title);
					payload.insert(payload.end(), text.begin(), text.end());
					const auto whole = make_chunk(disp_id, payload);
					if (!same_bytes(*disp, whole)) put(disp, whole, anywhere, anywhere);
				}
			}
		}

		// An AVI's IDIT original date, where the file has one.
		if (l.avi && update.legacy.date_original)
		{
			for (const auto& c : children)
			{
				if (c.id != list_id || c.type != hdrl_type) continue;

				for (const auto& h : parse_children(r, c.payload() + 4, c.payload() + c.size))
				{
					if (h.id != idit_id || h.size != idit_size) continue;
					if (const auto text = idit_text(*update.legacy.date_original); text && r.read(h.payload(), idit_size) != *text) patch(h.payload(), *text, commit);
				}
			}
		}

		if (appended.empty()) return;

		// Appended at the end of the first form, which takes it in once it is written.
		const auto data_end = first.data_end();
		const auto aligned = data_end + (data_end & 1);
		const auto insert_at = aligned <= r.size() ? aligned : data_end;

		if (l.forms.size() > 1) throw format_error("the AVI continues in further forms, so its first cannot grow to take the metadata");
		if (insert_at & 1) appended.insert(appended.begin(), 0);

		const auto form_size = insert_at + appended.size() - first.header.start - 8;
		if (form_size > 0xFFFFFFFFu) throw format_error("the RIFF form would grow past 4 GB");

		bytes size;
		append_le32(size, static_cast<std::uint32_t>(form_size));
		patch(first.header.start + 4, std::move(size), reveal);
		plan.replacements.push_back({insert_at, 0, std::move(appended)});
	}
}
