// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing metadata to ISO base media (MP4, M4A, M4V, 3GP) and QuickTime files without moving
// their media. The packet lives where the toolkit kept it: a top-level XMP 'uuid' box in an ISO file,
// moov/udta/XMP_ in a QuickTime one. Tags and rating also go to the Xtra box Windows reads, a copyright
// to ISO cprt, a created date to the movie header.
//
// An update is patched into the live file, so wherever it is interrupted the file must still read
// whole. What fits is patched where it lies: the movie's user data keeps free space for its boxes to
// grow into, so the movie box keeps its size. A box that does not fit is written whole at the end of
// the file, then made part of it, and only then does the box it replaces become free space. Chunk
// offsets never change, and no update copies the media.
//
// Readers take the first movie box and the last XMP uuid box, as FFmpeg does; an interrupted update
// can leave more of either, and the next update frees them. The boxes are read through
// mp4_internal.h, which the media scan reads them through too.

#include "legacy.h"
#include "mp4_internal.h"
#include "vocabulary_internal.h"

#include <algorithm>
#include <utility>

namespace libfiles::detail
{
	namespace
	{
		using namespace mp4;

		// The free space a movie's user data is given when the movie box moves, for later edits to grow
		// into: room for the packet to take its padding again, and for tags.
		constexpr std::uint64_t user_data_room = 4096;

		// The order of the patches, after what is appended: first what makes the appended boxes part of
		// the file, then what makes them the ones read and frees the boxes they replace.
		constexpr std::uint8_t reveal = 0;
		constexpr std::uint8_t commit = 1;

		// A box with an eight-byte header, or with the sixteen-byte one when asked for or needed.
		bytes make_box(const std::uint32_t type, const std::span<const std::uint8_t> payload, const std::size_t header = 8)
		{
			bytes out;

			if (header == 8 && 8ull + payload.size() <= 0xFFFFFFFFu)
			{
				append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
				append_be32(out, type);
			}
			else
			{
				const auto size = 16ull + payload.size();
				append_be32(out, 1);
				append_be32(out, type);
				append_be32(out, static_cast<std::uint32_t>(size >> 32));
				append_be32(out, static_cast<std::uint32_t>(size));
			}

			out.insert(out.end(), payload.begin(), payload.end());
			return out;
		}

		std::span<const std::uint8_t> bytes_of(const std::string& text)
		{
			return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
		}

		// A box held whole, header included, so what is not edited is written back byte for byte.
		struct raw_box
		{
			std::uint32_t type = 0;
			bytes raw;

			std::size_t header() const { return load_be32(raw.data()) == 1 ? 16u : 8u; }
		};

		// The children of a box's payload. QuickTime may end a list of them with four zero bytes.
		std::vector<raw_box> parse_children(const std::span<const std::uint8_t> payload, bool& terminator)
		{
			reader r(payload);
			const auto list = list_boxes(r, 0, payload.size());
			if (list.damaged) throw format_error("an MP4 box holds a damaged child");

			std::vector<raw_box> out;
			terminator = list.terminator;

			for (const auto& b : list.boxes)
			{
				out.push_back({b.type, bytes(payload.begin() + static_cast<std::ptrdiff_t>(b.start),
				                             payload.begin() + static_cast<std::ptrdiff_t>(b.end()))});
			}

			return out;
		}

		// The movie box, with its user data open for editing. The user data's free space is held as a
		// size rather than as boxes: written back, the user data's boxes come first and whatever space
		// is left over follows as one free box.
		struct movie
		{
			std::size_t header = 8;
			std::vector<raw_box> children;
			bool terminator = false;

			std::optional<std::size_t> udta_at;
			bool udta_in_file = false;
			std::vector<raw_box> user_data;
			bool user_data_terminator = false;
			// The user data's payload as the file holds it, free space included.
			std::uint64_t user_data_space = 0;

			bool user_data_changed = false;
			// A child other than the user data changed, keeping its size.
			bool children_changed = false;

			void open(const bytes& moov_bytes, const std::size_t moov_header)
			{
				header = moov_header;
				children = parse_children(std::span(moov_bytes).subspan(header), terminator);

				for (std::size_t i = 0; i < children.size(); ++i)
				{
					if (children[i].type != udta) continue;

					const auto& box = children[i];
					udta_at = i;
					udta_in_file = true;
					user_data_space = box.raw.size() - box.header();
					user_data = parse_children(std::span(box.raw).subspan(box.header()), user_data_terminator);
					std::erase_if(user_data, [](const raw_box& b) { return b.type == free_ || b.type == skip; });
					break;
				}
			}

			const raw_box* find_udta(const std::uint32_t type) const
			{
				const auto it = std::ranges::find(user_data, type, &raw_box::type);
				return it != user_data.end() ? &*it : nullptr;
			}

			// Puts the box where the first of its type was, or at the end, and drops the others.
			void set_udta(const std::uint32_t type, bytes raw)
			{
				const auto first = std::ranges::find(user_data, type, &raw_box::type);

				if (first == user_data.end())
				{
					user_data.push_back({type, std::move(raw)});
				}
				else
				{
					auto first_kept = false;
					std::erase_if(user_data, [type, &first_kept](const raw_box& b)
					{
						if (b.type != type) return false;
						if (first_kept) return true;
						first_kept = true;
						return false;
					});
					const auto kept = std::ranges::find(user_data, type, &raw_box::type);
					kept->raw = std::move(raw);
				}

				if (!udta_at)
				{
					udta_at = children.size();
					children.push_back({udta, {}});
				}

				user_data_changed = true;
			}

			void remove_udta(const std::uint32_t type)
			{
				if (std::erase_if(user_data, [type](const raw_box& b) { return b.type == type; }) != 0) user_data_changed = true;
			}

			// The bytes the user data's boxes take, free space aside.
			std::uint64_t user_data_need() const
			{
				std::uint64_t need = user_data_terminator ? 4 : 0;
				for (const auto& b : user_data) need += b.raw.size();
				return need;
			}

			// True when the edited user data fits the space it has, with nothing left over or with
			// enough for a free box.
			bool user_data_fits() const
			{
				const auto need = user_data_need();
				return udta_in_file && (need == user_data_space || need + 8 <= user_data_space);
			}

			// The movie box, its user data given space bytes when it changed.
			bytes serialize(const std::uint64_t space) const
			{
				bytes payload;

				for (std::size_t i = 0; i < children.size(); ++i)
				{
					const auto& child = children[i];

					if (!udta_at || i != *udta_at || !user_data_changed)
					{
						payload.insert(payload.end(), child.raw.begin(), child.raw.end());
						continue;
					}

					bytes user;

					for (const auto& b : user_data)
					{
						const auto at = user.size();
						user.insert(user.end(), b.raw.begin(), b.raw.end());

						// A box that ran to the end of the user data stops where it ends now.
						if (load_be32(b.raw.data()) == 0)
						{
							bytes size;
							append_be32(size, static_cast<std::uint32_t>(b.raw.size()));
							std::ranges::copy(size, user.begin() + static_cast<std::ptrdiff_t>(at));
						}
					}

					if (const auto left = space - user_data_need(); left != 0)
					{
						const auto room = make_box(free_, bytes(static_cast<std::size_t>(left - 8), 0));
						user.insert(user.end(), room.begin(), room.end());
					}

					if (user_data_terminator) user.insert(user.end(), 4, 0);

					const auto box = make_box(udta, user, child.raw.empty() ? 8 : child.header());
					payload.insert(payload.end(), box.begin(), box.end());
				}

				if (terminator) payload.insert(payload.end(), 4, 0);
				return make_box(moov, payload, header);
			}

			// Where the user data the file holds lies in the movie box, its header included.
			std::pair<std::size_t, std::size_t> user_data_range() const
			{
				std::size_t at = header;
				for (std::size_t i = 0; i < *udta_at; ++i) at += children[i].raw.size();
				return {at, at + children[*udta_at].raw.size()};
			}
		};

		void append_be16(bytes& to, const std::uint16_t v)
		{
			to.push_back(static_cast<std::uint8_t>(v >> 8));
			to.push_back(static_cast<std::uint8_t>(v));
		}

		void append_le64(bytes& to, const std::uint64_t v)
		{
			for (auto i = 0; i < 8; ++i) to.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
		}

		// One Xtra entry: its size, its name, its value count, then the values.
		void append_xtra_entry(bytes& to, const std::string_view name, const std::uint32_t count, const bytes& values)
		{
			append_be32(to, static_cast<std::uint32_t>(12 + name.size() + values.size()));
			append_be32(to, static_cast<std::uint32_t>(name.size()));
			append(to, name);
			append_be32(to, count);
			to.insert(to.end(), values.begin(), values.end());
		}

		// The Xtra box Windows Explorer and Media Player read tags and rating from, as Diffractor's
		// fork of the toolkit wrote it. Entries the edit does not own are kept as they are.
		std::optional<bytes> updated_xtra(const raw_box* existing, const legacy_edits& e)
		{
			const auto owns_category = e.keywords.has_value();
			const auto owns_rating = e.rating.has_value();
			if (!owns_category && !owns_rating) return {};

			bytes content;
			bytes old_content;

			if (existing)
			{
				old_content.assign(existing->raw.begin() + static_cast<std::ptrdiff_t>(existing->header()), existing->raw.end());

				for_each_xtra_entry(old_content, [&](const xtra_entry& entry)
				{
					if (entry.name.empty()) return;

					const auto owned = (owns_category && entry.name == vocabulary::wm_category) || (owns_rating && entry.name == vocabulary::wm_shared_user_rating);
					if (!owned) content.insert(content.end(), entry.bytes.begin(), entry.bytes.end());
				});
			}

			if (owns_category)
			{
				bytes values;
				std::uint32_t count = 0;

				for (const auto& keyword : *e.keywords)
				{
					if (keyword.empty()) continue;
					auto text = utf16le(keyword);
					text.push_back(0);
					text.push_back(0);
					append_be32(values, static_cast<std::uint32_t>(6 + text.size()));
					append_be16(values, 8); // VT_LPWSTR
					values.insert(values.end(), text.begin(), text.end());
					++count;
				}

				if (count > 0) append_xtra_entry(content, vocabulary::wm_category, count, values);
			}

			if (owns_rating && *e.rating > 0)
			{
				bytes value;
				append_be32(value, 14);
				append_be16(value, 19); // VT_UI8
				append_le64(value, static_cast<std::uint64_t>(percent_from_stars(*e.rating)));
				append_xtra_entry(content, vocabulary::wm_shared_user_rating, 1, value);
			}

			return content;
		}

		// Seconds since 1904, the epoch of the movie header, for an XMP date read as UTC.
		std::optional<std::uint64_t> movie_seconds(const std::string& xmp_date)
		{
			const auto d = parse_xmp_date(xmp_date);
			if (!d) return {};

			const auto seconds = seconds_since(1904, *d);
			if (seconds < 0) return {};
			return static_cast<std::uint64_t>(seconds);
		}

		struct layout
		{
			std::vector<box> boxes;
			bool quicktime = false;
			std::size_t moov_at = 0;
			// Movie boxes after the first, which no reader takes.
			std::vector<std::size_t> later_moovs;
			std::vector<std::size_t> xmp_uuids;

			const box& moov_box() const { return boxes[moov_at]; }
			const box* xmp_box() const { return xmp_uuids.empty() ? nullptr : &boxes[xmp_uuids.back()]; }
		};

		layout read_layout(reader& r)
		{
			layout l;
			l.boxes = top_level_boxes(r);
			l.quicktime = is_quicktime(r, l.boxes);
			auto found_moov = false;

			for (std::size_t i = 0; i < l.boxes.size(); ++i)
			{
				if (l.boxes[i].type == moov)
				{
					if (found_moov) l.later_moovs.push_back(i);
					else l.moov_at = i;
					found_moov = true;
				}
				else if (is_xmp_uuid(r, l.boxes[i]))
				{
					l.xmp_uuids.push_back(i);
				}
			}

			if (!found_moov) throw format_error("the MP4 has no movie box");
			if (l.moov_box().size > max_moov) throw format_error("the MP4's movie box is too large to update");
			return l;
		}
	}

	bool is_iso_media(const std::span<const std::uint8_t> head) noexcept
	{
		if (head.size() < 8) return false;

		// Canon's CR3 raw is ISO base media too, but a photo, and libfiles does not write raw files.
		const auto type = load_be32(head.data() + 4);
		if (type == ftyp && head.size() >= 12 && load_be32(head.data() + 8) == fourcc("crx ")) return false;

		return type == ftyp || type == moov || type == mdat || type == free_ || type == skip || type == wide || type == pnot;
	}

	void read_mp4_packet(reader& r, packet_reading& out)
	{
		const auto l = read_layout(r);
		const auto& mb = l.moov_box();
		const auto in_movie = movie_xmp(r, list_boxes(r, mb.payload(), mb.end()));

		if (const auto place = packet_place(l.quicktime, in_movie, l.xmp_box()))
		{
			const auto packet = r.read(place->offset, static_cast<std::size_t>(place->size));
			out.packet.assign(packet.begin(), packet.end());
		}

		out.readable = true;
	}

	void plan_mp4(reader& r, const metadata_update& update, write_plan& plan)
	{
		const auto l = read_layout(r);
		const auto& mb = l.moov_box();
		const auto moov_bytes = r.read(mb.start, static_cast<std::size_t>(mb.size));

		movie m;
		m.open(moov_bytes, mb.header);

		bytes appended;

		const auto patch = [&plan](const std::uint64_t offset, bytes data, const std::uint8_t step)
		{
			const auto length = data.size();
			plan.replacements.push_back({offset, length, std::move(data), step});
		};

		// A box becomes free space by its type alone, so it keeps its size and what follows it stays.
		const auto free_box_at = [&patch](const std::uint64_t start)
		{
			bytes type;
			append_be32(type, free_);
			patch(start + 4, std::move(type), commit);
		};

		for (const auto i : l.later_moovs) free_box_at(l.boxes[i].start);

		// A QuickTime packet is part of the movie; an ISO one, in its uuid box, is placed once the movie
		// box's place is known.
		if (update.xmp)
		{
			if (l.quicktime)
			{
				const auto* const old = m.find_udta(xmp_);
				const auto packet = serialize_packet(*update.xmp, old ? old->raw.size() - old->header() : 0);
				m.set_udta(xmp_, make_box(xmp_, bytes_of(packet)));

				// A packet in a uuid box would be read as well.
				for (const auto i : l.xmp_uuids) free_box_at(l.boxes[i].start);
			}
			else
			{
				m.remove_udta(xmp_);
			}
		}

		// The Xtra box.
		if (auto xtra_content = updated_xtra(m.find_udta(xtra), update.legacy))
		{
			const auto* const old = m.find_udta(xtra);
			const auto same = old && std::equal(old->raw.begin() + static_cast<std::ptrdiff_t>(old->header()), old->raw.end(),
			                                    xtra_content->begin(), xtra_content->end());

			if (!same)
			{
				if (xtra_content->empty()) m.remove_udta(xtra);
				else m.set_udta(xtra, make_box(xtra, *xtra_content));
			}
		}

		// The ISO copyright, where the file has user data for it to join.
		if (update.legacy.copyright && m.udta_at)
		{
			m.remove_udta(cprt);

			if (!update.legacy.copyright->empty())
			{
				bytes payload = {0, 0, 0, 0, 0x55, 0xC4}; // version, flags, and 'und'
				append(payload, *update.legacy.copyright);
				payload.push_back(0);
				m.set_udta(cprt, make_box(cprt, payload));
			}
		}

		// The created date, in the movie header.
		if (update.legacy.date_digitized)
		{
			const auto seconds = movie_seconds(*update.legacy.date_digitized);
			const auto header = std::ranges::find(m.children, mvhd, &raw_box::type);

			if (seconds && header != m.children.end())
			{
				auto& raw = header->raw;
				const auto at = header->header();

				if (raw.size() >= at + 12 && raw[at] == 0 && *seconds <= 0xFFFFFFFFu)
				{
					bytes value;
					append_be32(value, static_cast<std::uint32_t>(*seconds));
					std::ranges::copy(value, raw.begin() + static_cast<std::ptrdiff_t>(at + 4));
					m.children_changed = true;
				}
				else if (raw.size() >= at + 20 && raw[at] == 1)
				{
					bytes value;
					append_be32(value, static_cast<std::uint32_t>(*seconds >> 32));
					append_be32(value, static_cast<std::uint32_t>(*seconds));
					std::ranges::copy(value, raw.begin() + static_cast<std::ptrdiff_t>(at + 4));
					m.children_changed = true;
				}
			}
		}

		const auto moov_moves = m.user_data_changed && !m.user_data_fits();

		if (moov_moves)
		{
			// Written whole at the end of the file, with room for later edits to grow into.
			const auto updated = m.serialize(m.user_data_need() + user_data_room);
			appended.insert(appended.end(), updated.begin(), updated.end());
			free_box_at(mb.start);
		}
		else if (m.user_data_changed || m.children_changed)
		{
			// The same size, so nothing in the movie box moves but the user data's boxes. A size field
			// of zero stays zero, so what is appended is revealed by one patch alone.
			auto updated = m.serialize(m.user_data_space);
			std::copy_n(moov_bytes.begin(), mb.header, updated.begin());

			const std::span<const std::uint8_t> before(moov_bytes);
			const std::span<const std::uint8_t> after(updated);
			const auto [u0, u1] = m.user_data_changed ? m.user_data_range() : std::pair<std::size_t, std::size_t>{before.size(), before.size()};

			for (auto& p : diff_patches(mb.start, before.first(u0), after.first(u0))) patch(p.offset, std::move(p.data), commit);
			if (auto p = changed_span(mb.start + u0, before.subspan(u0, u1 - u0), after.subspan(u0, u1 - u0), commit)) plan.replacements.push_back(std::move(*p));
			for (auto& p : diff_patches(mb.start + u1, before.subspan(u1), after.subspan(u1))) patch(p.offset, std::move(p.data), commit);
		}

		// An ISO packet's uuid box. FFmpeg reads none that comes before the movie box, so the packet is
		// written where it lies only when it fits there and follows a movie box that stays; otherwise
		// it is appended after everything, the movie box included.
		if (!l.quicktime)
		{
			const auto* const current = l.xmp_box();
			const auto follows_movie = current && current->start > mb.start;
			const auto append_box = [&appended](const std::span<const std::uint8_t> box)
			{
				appended.insert(appended.end(), box.begin(), box.end());
			};

			if (update.xmp)
			{
				const auto old_length = current ? current->size - current->header - 16 : 0;
				const auto stays = follows_movie && !moov_moves;
				const auto packet = serialize_packet(*update.xmp, stays ? old_length : 0);

				if (stays && packet.size() == old_length)
				{
					patch(current->start + current->header + 16, to_bytes(packet), commit);
				}
				else
				{
					bytes payload(xmp_uuid.begin(), xmp_uuid.end());
					append(payload, packet);
					append_box(make_box(uuid, payload));
					if (current) free_box_at(current->start);
				}

				// Older packets - an interrupted update's, or another tool's - are stale, and a reader
				// that takes the first would show one.
				for (const auto i : l.xmp_uuids)
				{
					if (&l.boxes[i] != current) free_box_at(l.boxes[i].start);
				}
			}
			else if (follows_movie && moov_moves)
			{
				// The packet the file holds follows the movie box to the end, as it is.
				auto box = r.read(current->start, static_cast<std::size_t>(current->size));

				if (current->to_end)
				{
					bytes size;
					append_be32(size, static_cast<std::uint32_t>(box.size()));
					std::ranges::copy(size, box.begin());
				}

				append_box(box);
				free_box_at(current->start);
			}
		}

		if (!appended.empty())
		{
			if (l.boxes.back().end() != r.size()) throw format_error("the MP4 ends with bytes that are not a box");

			// A last box that runs to the end of the file would take in what follows it, so once that is
			// written it is closed where it ends now.
			const auto& last = l.boxes.back();

			if (last.to_end)
			{
				if (last.size > 0xFFFFFFFFu) throw format_error("the MP4's last box is too large to follow");
				bytes size;
				append_be32(size, static_cast<std::uint32_t>(last.size));
				patch(last.start, std::move(size), reveal);
			}

			plan.replacements.push_back({r.size(), 0, std::move(appended)});
		}
	}
}
