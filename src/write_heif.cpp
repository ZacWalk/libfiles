// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Writing XMP, and the EXIF a file already holds, to a HEIF - HEIC, AVIF and the other ISO base media
// still images. The packet is the XMP item the scan reads from the primary image - the last 'mime' item of type
// application/rdf+xml, or 'XMP', that a 'cdsc' reference attaches to it - and the EXIF is its first Exif item. A
// file without a packet gains one, made as libheif makes one: a hidden 'mime' item with a 'cdsc' reference to the
// primary image. A file without EXIF gains none.
//
// Items whose data keeps its length, each in one piece, are patched where they lie. Anything else is rewritten in
// one pass: an item's data in an mdat is replaced where it lies, and data that lies anywhere else, or is new, goes
// at the start of the first mdat; the meta box is rebuilt with the item and its reference; an mdat that changes
// length is given its new size; and every item location moves with the bytes it locates, as an image sequence's
// chunk offsets do. Every other box, every other item's data, and every byte of the meta box but its item
// information, references and locations are kept. A fragmented image sequence, whose offsets lie in boxes an update
// does not read, is refused a rewrite.
//
// An orientation turns the picture libheif decodes: the primary image's rotation and mirror, and those of its
// thumbnails, its auxiliary images - Apple's HDR gain map among them - and the gain map an ISO 21496-1 tone map item
// applies to it, are set to what they come to with the turn - the values of the boxes they have patched where those
// can show it, else the boxes libheif writes for it put in their place - and the EXIF's orientation is made the
// primary image's, as Apple writes it.
//
// The same rewrite finishes an AVIF libheif wrote with a gain map, as libavif writes one: the gain map's item hidden
// and an 'altr' group added, which libheif has no way to write.

#include "bmff_internal.h"
#include "heif_internal.h"
#include "legacy.h"
#include "write_internal.h"

#include "libfiles/stream.h"
#include "libfiles/write.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		using bmff::fourcc;

		constexpr auto box_meta = fourcc("meta");
		constexpr auto box_mdat = fourcc("mdat");
		constexpr auto box_iinf = fourcc("iinf");
		constexpr auto box_iref = fourcc("iref");
		constexpr auto box_iloc = fourcc("iloc");
		constexpr auto box_infe = fourcc("infe");
		constexpr auto item_mime = fourcc("mime");
		constexpr auto ref_cdsc = fourcc("cdsc");

		// Top-level boxes of a fragmented movie, whose offsets lie where an update does not move them.
		constexpr std::uint32_t fragment_boxes[] = {fourcc("moof"), fourcc("mfra"), fourcc("sidx")};

		// A movie's chunk offsets lie in each track's sample table, which these boxes lead down to from the movie box.
		constexpr auto box_moov = fourcc("moov");
		constexpr std::uint32_t sample_table_path[] = {fourcc("trak"), fourcc("mdia"), fourcc("minf"), fourcc("stbl")};
		constexpr auto box_stco = fourcc("stco");
		constexpr auto box_co64 = fourcc("co64");
		// The largest movie box an update reads into memory, as an MP4's update reads one.
		constexpr std::uint64_t max_movie = 256ull * 1024 * 1024;

		constexpr std::uint64_t max_u16 = std::numeric_limits<std::uint16_t>::max();
		constexpr std::uint64_t max_u32 = std::numeric_limits<std::uint32_t>::max();

		constexpr std::string_view xmp_content_type = "application/rdf+xml";

		void append_be16(bytes& to, const std::uint64_t v)
		{
			to.push_back(static_cast<std::uint8_t>(v >> 8));
			to.push_back(static_cast<std::uint8_t>(v));
		}

		void append_be64(bytes& to, const std::uint64_t v)
		{
			append_be32(to, static_cast<std::uint32_t>(v >> 32));
			append_be32(to, static_cast<std::uint32_t>(v));
		}

		// An item location field: none, four bytes or eight.
		void append_sized(bytes& to, const std::uint64_t v, const unsigned size)
		{
			if (size == 4) append_be32(to, static_cast<std::uint32_t>(v));
			else if (size == 8) append_be64(to, v);
		}

		bool fits(const std::uint64_t v, const unsigned size)
		{
			return size == 8 || (size == 4 && v <= max_u32) || v == 0;
		}

		// A box with an eight-byte header, or the sixteen-byte one when the box had it or its size needs it.
		bytes make_box(const std::uint32_t type, const std::span<const std::uint8_t> payload, const bool large)
		{
			bytes out;
			const auto size = std::uint64_t{payload.size()};

			if (large || size > max_u32 - 8)
			{
				append_be32(out, 1);
				append_be32(out, type);
				append_be64(out, size + 16);
			}
			else
			{
				append_be32(out, static_cast<std::uint32_t>(size + 8));
				append_be32(out, type);
			}

			out.insert(out.end(), payload.begin(), payload.end());
			return out;
		}

		void append_full_box_header(bytes& to, const std::uint8_t version, const std::uint32_t flags)
		{
			append_be32(to, static_cast<std::uint32_t>(version) << 24 | (flags & 0xffffffu));
		}

		// The Exif item's data with the edit's EXIF fields written. The block is what the scan reads: after the
		// offset to its TIFF header, and after "Exif\0\0" where the item keeps that before it. Nothing when the block
		// cannot be read, or nothing in it changes.
		std::optional<bytes> edit_exif(const bytes& data, const legacy_edits& edits)
		{
			if (data.size() < 4) return {};

			auto start = std::uint64_t{4} + load_be32(data.data());
			if (start > data.size()) return {};
			if (starts_with(std::span(data).subspan(static_cast<std::size_t>(start)), {"Exif\0\0", 6})) start += 6;

			const auto block = std::span(data).subspan(static_cast<std::size_t>(start));
			reader block_reader(block);
			tiff_editor editor;
			if (!editor.open(block_reader)) return {};

			photo_legacy legacy{&editor};
			apply_photo_legacy(edits, {}, false, legacy);
			const auto edited = apply_changes(block, editor.changes(block.size()));
			if (std::ranges::equal(edited, block)) return {};

			bytes out(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(start));
			out.insert(out.end(), edited.begin(), edited.end());
			return out;
		}

		constexpr auto box_iprp = fourcc("iprp");
		constexpr auto box_ipco = fourcc("ipco");
		constexpr auto box_ipma = fourcc("ipma");
		constexpr auto prop_irot = fourcc("irot");
		constexpr auto prop_imir = fourcc("imir");
		constexpr auto ref_thmb = fourcc("thmb");
		constexpr auto ref_auxl = fourcc("auxl");
		constexpr auto ref_dimg = fourcc("dimg");
		constexpr auto item_tmap = fourcc("tmap");
		constexpr auto box_grpl = fourcc("grpl");
		constexpr auto box_altr = fourcc("altr");

		// The eight ways a picture can be turned and mirrored, as where its top left, top right, bottom right and
		// bottom left corners go, in the order of the EXIF orientations 1 to 8.
		using turn = std::array<std::uint8_t, 4>;
		constexpr turn turns[8] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1}, {3, 2, 1, 0},
		                           {0, 3, 2, 1}, {1, 2, 3, 0}, {2, 1, 0, 3}, {3, 0, 1, 2}};

		bool contains(const std::vector<std::uint32_t>& values, const std::uint32_t value)
		{
			return std::ranges::find(values, value) != values.end();
		}

		turn then(const turn& first, const turn& second)
		{
			return {second[first[0]], second[first[1]], second[first[2]], second[first[3]]};
		}

		int orientation_of(const turn& t)
		{
			const auto found = std::ranges::find(turns, t);
			return found == std::end(turns) ? 1 : static_cast<int>(found - std::begin(turns)) + 1;
		}

		// What an irot does with its angle - quarter turns anticlockwise - and an imir with its axis, as libheif
		// reads them: axis 1 mirrors left and right, 0 top and bottom.
		turn property_turn(const std::uint32_t type, const std::uint8_t value)
		{
			if (type == prop_imir) return turns[(value & 1u) ? 1 : 3];
			constexpr int by_angle[4] = {0, 7, 2, 5};
			return turns[by_angle[value & 3u]];
		}

		// The boxes 'ipco' holds, numbered from one, and the associations 'ipma' makes of them with items: where
		// each lies in the meta box's payload.
		struct property_box
		{
			std::uint32_t type = 0;
			std::size_t at = 0;
			std::size_t size = 0;
			std::uint32_t header = 8;
		};

		struct association
		{
			std::uint32_t index = 0;
			bool essential = false;
		};

		struct item_associations
		{
			std::uint32_t item = 0;
			std::vector<association> list;
		};

		struct item_properties
		{
			std::size_t iprp_child = 0;
			property_box iprp;
			property_box ipco;
			property_box ipma;
			std::uint8_t ipma_version = 0;
			std::uint32_t ipma_flags = 0;
			std::vector<property_box> boxes;
			std::vector<item_associations> items;

			// The value byte of an irot or imir, which follows its header.
			std::uint8_t value(const heif::layout& l, const std::uint32_t index) const
			{
				const auto& b = boxes[index - 1];
				return l.meta_payload[b.at + b.header];
			}
		};

		// A box at pos of the meta box's payload, ending no later than end.
		property_box payload_box(const heif::layout& l, const std::size_t pos, const std::size_t end)
		{
			bmff::box_header h;
			const auto bytes = std::span(l.meta_payload).subspan(pos, end - pos);
			if (bmff::read_header(bytes, h) != bmff::header_status::box)
				throw format_error("a HEIF item property box is cut short");

			const auto size = h.size == 0 ? end - pos : h.size;
			if (size < h.header || size > end - pos) throw format_error("a HEIF item property box is cut short");
			return {h.type, pos, static_cast<std::size_t>(size), h.header};
		}

		item_properties read_item_properties(const heif::layout& l)
		{
			const auto payload_at = l.meta.offset + l.meta.header;
			const auto child = std::ranges::find(l.children, box_iprp, &heif::box_place::type);
			if (child == l.children.end()) throw format_error("the HEIF file has no item properties");

			item_properties p;
			p.iprp_child = static_cast<std::size_t>(child - l.children.begin());
			p.iprp = {box_iprp, static_cast<std::size_t>(child->offset - payload_at),
			          static_cast<std::size_t>(child->size), child->header};

			auto have_ipco = false;
			auto have_ipma = false;
			const auto end = p.iprp.at + p.iprp.size;

			for (auto pos = p.iprp.at + p.iprp.header; pos < end;)
			{
				const auto box = payload_box(l, pos, end);

				if (box.type == box_ipco && !have_ipco)
				{
					have_ipco = true;
					p.ipco = box;

					for (auto at = box.at + box.header; at < box.at + box.size;)
					{
						const auto property = payload_box(l, at, box.at + box.size);
						p.boxes.push_back(property);
						at += property.size;
					}
				}
				else if (box.type == box_ipma)
				{
					if (have_ipma)
						throw format_error("the HEIF has property associations in more than one box, which a turn is "
						                   "not written to");
					have_ipma = true;
					p.ipma = box;

					reader r(std::span(l.meta_payload).subspan(box.at + box.header, box.size - box.header));
					const auto head = r.be32(0);
					p.ipma_version = static_cast<std::uint8_t>(head >> 24);
					p.ipma_flags = head & 0xffffffu;
					if (p.ipma_version > 1)
						throw format_error("the HEIF property associations have an unknown version");

					const auto count = r.be32(4);
					std::uint64_t at = 8;

					for (std::uint32_t i = 0; i < count; ++i)
					{
						item_associations entry;
						entry.item = p.ipma_version < 1 ? r.be16(at) : r.be32(at);
						at += p.ipma_version < 1 ? 2 : 4;
						const auto associations = r.u8(at++);

						for (unsigned k = 0; k < associations; ++k)
						{
							const auto value = (p.ipma_flags & 1u) ? r.be16(at) : r.u8(at);
							at += (p.ipma_flags & 1u) ? 2 : 1;
							const auto wide = (p.ipma_flags & 1u) != 0;
							entry.list.push_back(
								{value & (wide ? 0x7fffu : 0x7fu), (value & (wide ? 0x8000u : 0x80u)) != 0});
						}

						p.items.push_back(std::move(entry));
					}
				}

				pos += box.size;
			}

			if (!have_ipco || !have_ipma) throw format_error("the HEIF file has no item properties");

			for (const auto& entry : p.items)
				for (const auto& a : entry.list)
					if (a.index > p.boxes.size()) throw format_error("a HEIF item names a property there is none of");

			return p;
		}

		// A turn of the picture libheif decodes - its rotations and mirrors applied - written where libheif reads it:
		// as the rotation and mirror of the primary image, and of the thumbnails and auxiliary images that belong to
		// it - Apple's HDR gain map among them - and of the gain map an ISO 21496-1 tone map item applies to it, which
		// libavif turns as it turns the primary, leaving the tone map item unturned. orientation is what the EXIF says
		// once the primary image is turned, as Apple writes it beside them.
		struct turn_plan
		{
			int orientation = 1;
			// Whether the turn is the value bytes of rotations and mirrors rewritten where they lie in the meta box's
			// payload: these.
			bool in_place = false;
			std::vector<std::pair<std::size_t, std::uint8_t>> patches;
			// The item property box with the turn in it, for a meta box written again.
			bytes iprp;
		};

		turn_plan plan_turn(const heif::layout& l, const item_properties& p, const int orientation)
		{
			const auto by = turns[std::clamp(orientation, 1, 8) - 1];

			std::vector<std::uint32_t> turned = {l.primary};
			for (const auto& ref : l.references)
			{
				if ((ref.type == ref_thmb || ref.type == ref_auxl) && contains(ref.to, l.primary) &&
				    !contains(turned, ref.from))
				{
					turned.push_back(ref.from);
				}

				// A tone map item's inputs are the base image, the primary, then its gain map.
				const auto from = l.items.find(ref.from);
				if (ref.type == ref_dimg && from != l.items.end() && from->second.type == item_tmap &&
				    ref.to.size() == 2 && ref.to[0] == l.primary && !contains(turned, ref.to[1]))
				{
					turned.push_back(ref.to[1]);
				}
			}

			const auto entry_of = [&p](const std::uint32_t item)
			{
				const auto found = std::ranges::find(p.items, item, &item_associations::item);
				return found == p.items.end() ? nullptr : &*found;
			};

			// The rotations and mirrors an item has, in the order libheif applies them.
			const auto transforms_of = [&](const std::uint32_t item)
			{
				std::vector<std::uint32_t> indices;

				if (const auto* const e = entry_of(item))
				{
					for (const auto& a : e->list)
					{
						if (a.index == 0) continue;
						const auto& b = p.boxes[a.index - 1];
						if (b.type != prop_irot && b.type != prop_imir) continue;
						if (b.size < b.header + 1) throw format_error("a HEIF rotation or mirror is cut short");
						indices.push_back(a.index);
					}
				}

				return indices;
			};

			turn_plan out;
			std::vector<std::pair<std::uint32_t, turn>> wanted;

			for (const auto item : turned)
			{
				auto now = turns[0];
				for (const auto index : transforms_of(item))
					now = then(now, property_turn(p.boxes[index - 1].type, p.value(l, index)));
				wanted.emplace_back(item, then(now, by));
				if (item == l.primary) out.orientation = orientation_of(wanted.back().second);
			}

			// In place: each turned item keeps the rotations and mirrors it has, with values that make its turn -
			// fewest changed first - agreeing with every other turned item that shares one, and changing none an item
			// left alone shares.
			std::map<std::uint32_t, std::uint8_t> values;
			auto in_place = true;

			for (const auto& [item, want] : wanted)
			{
				const auto own = transforms_of(item);
				if (own.size() > 4)
				{
					in_place = false;
					break;
				}

				std::optional<std::vector<std::uint8_t>> best;
				auto best_changes = 0;

				for (std::uint32_t choice = 0; choice < 1u << (2 * own.size()); ++choice)
				{
					auto t = turns[0];
					std::vector<std::uint8_t> chosen;
					auto changes = 0;
					auto possible = true;

					for (std::size_t k = 0; k < own.size() && possible; ++k)
					{
						const auto type = p.boxes[own[k] - 1].type;
						const auto v = static_cast<std::uint8_t>(choice >> (2 * k) & 3u);
						const auto mask = type == prop_irot ? 3u : 1u;
						const auto old = p.value(l, own[k]);
						const auto value = static_cast<std::uint8_t>((old & ~mask) | v);
						possible = v <= mask && (!values.contains(own[k]) || values[own[k]] == value);
						chosen.push_back(value);
						t = then(t, property_turn(type, value));
						if (value != old) ++changes;
					}

					if (possible && t == want && (!best || changes < best_changes))
					{
						best = std::move(chosen);
						best_changes = changes;
					}
				}

				if (!best)
				{
					in_place = false;
					break;
				}

				for (std::size_t k = 0; k < own.size(); ++k) values[own[k]] = (*best)[k];
			}

			for (const auto& e : p.items)
			{
				if (!in_place || contains(turned, e.item)) continue;
				for (const auto& a : e.list)
					if (values.contains(a.index) && values[a.index] != p.value(l, a.index)) in_place = false;
			}

			const auto iprp_bytes = std::span(l.meta_payload).subspan(p.iprp.at, p.iprp.size);

			if (in_place)
			{
				out.in_place = true;
				out.iprp.assign(iprp_bytes.begin(), iprp_bytes.end());

				for (const auto& [index, value] : values)
				{
					const auto at = p.boxes[index - 1].at + p.boxes[index - 1].header;
					if (value == l.meta_payload[at]) continue;
					out.patches.emplace_back(at, value);
					out.iprp[at - p.iprp.at] = value;
				}

				return out;
			}

			// Written again: each turned item's rotations and mirrors replaced, after its other properties, by those
			// libheif writes for its turn - a rotation, then a mirror - shared with a box ipco holds where one has the
			// value, and added where none has.
			constexpr int canonical_angle[8] = {-1, -1, 2, -1, 3, 3, 3, 1};
			constexpr int canonical_axis[8] = {-1, 1, -1, 0, 1, -1, 0, -1};
			std::vector<bytes> added;
			auto items = p.items;

			const auto property = [&](const std::uint32_t type, const std::uint8_t value)
			{
				for (std::size_t i = 0; i < p.boxes.size(); ++i)
				{
					const auto& b = p.boxes[i];
					if (b.type == type && b.size == b.header + 1 && l.meta_payload[b.at + b.header] == value)
						return static_cast<std::uint32_t>(i + 1);
				}

				const auto box = make_box(type, std::vector<std::uint8_t>{value}, false);
				const auto found = static_cast<std::size_t>(std::ranges::find(added, box) - added.begin());
				if (found == added.size()) added.push_back(box);
				return static_cast<std::uint32_t>(p.boxes.size() + found + 1);
			};

			for (const auto& [item, want] : wanted)
			{
				auto entry = std::ranges::find(items, item, &item_associations::item);
				if (entry == items.end())
				{
					entry = std::ranges::upper_bound(items, item, {}, &item_associations::item);
					entry = items.insert(entry, {item, {}});
				}

				std::erase_if(entry->list, [&p](const association& a)
				{
					const auto type = a.index == 0 ? 0 : p.boxes[a.index - 1].type;
					return type == prop_irot || type == prop_imir;
				});

				const auto canonical = orientation_of(want) - 1;
				if (canonical_angle[canonical] >= 0)
					entry->list.push_back(
						{property(prop_irot, static_cast<std::uint8_t>(canonical_angle[canonical])), true});
				if (canonical_axis[canonical] >= 0)
					entry->list.push_back(
						{property(prop_imir, static_cast<std::uint8_t>(canonical_axis[canonical])), true});
			}

			bytes ipco(l.meta_payload.begin() + static_cast<std::ptrdiff_t>(p.ipco.at + p.ipco.header),
			           l.meta_payload.begin() + static_cast<std::ptrdiff_t>(p.ipco.at + p.ipco.size));
			for (const auto& b : added) ipco.insert(ipco.end(), b.begin(), b.end());

			const auto wide = (p.ipma_flags & 1u) != 0 || p.boxes.size() + added.size() > 0x7f;
			if (p.boxes.size() + added.size() > 0x7fff) throw format_error("the HEIF has too many item properties");

			bytes ipma;
			append_full_box_header(ipma, p.ipma_version, p.ipma_flags | (wide ? 1u : 0u));
			append_be32(ipma, static_cast<std::uint32_t>(items.size()));

			for (const auto& e : items)
			{
				if (e.list.size() > 0xff) throw format_error("a HEIF item has too many properties");
				if (p.ipma_version < 1) append_be16(ipma, e.item);
				else append_be32(ipma, e.item);
				ipma.push_back(static_cast<std::uint8_t>(e.list.size()));

				for (const auto& a : e.list)
				{
					if (wide) append_be16(ipma, (a.essential ? 0x8000u : 0u) | a.index);
					else ipma.push_back(static_cast<std::uint8_t>((a.essential ? 0x80u : 0u) | a.index));
				}
			}

			bytes children;
			for (auto pos = p.iprp.at + p.iprp.header; pos < p.iprp.at + p.iprp.size;)
			{
				const auto box = payload_box(l, pos, p.iprp.at + p.iprp.size);
				bytes written;
				if (box.at == p.ipco.at) written = make_box(box_ipco, ipco, box.header == 16);
				else if (box.at == p.ipma.at) written = make_box(box_ipma, ipma, box.header == 16);
				else
					written.assign(l.meta_payload.begin() + static_cast<std::ptrdiff_t>(box.at),
					               l.meta_payload.begin() + static_cast<std::ptrdiff_t>(box.at + box.size));
				children.insert(children.end(), written.begin(), written.end());
				pos += box.size;
			}

			out.iprp = make_box(box_iprp, children, p.iprp.header == 16);
			return out;
		}

		// New data for an item: one the file holds, or the packet item it gains, which has no id yet.
		struct item_change
		{
			std::optional<std::uint32_t> id;
			bytes data;
		};

		// Where an item's data lies in the file when it lies there in one piece.
		std::optional<std::uint64_t> single_place(const heif::layout& l, const heif::location* loc)
		{
			if (!loc || loc->data_reference != 0 || loc->extents.size() != 1) return {};

			const auto& e = loc->extents.front();
			if (e.length == 0) return {};
			if (loc->method == 0) return loc->base + e.offset;
			if (loc->method == 1 && l.idat_data) return *l.idat_data + loc->base + e.offset;
			return {};
		}

		// Whether another item's data in the file shares a byte of the range, so rewriting it would change that item.
		bool shares_data(const heif::layout& l, const std::uint32_t id, const std::uint64_t at,
		                 const std::uint64_t length)
		{
			for (const auto& other : l.locations)
			{
				if (other.item == id || other.method != 0 || other.data_reference != 0) continue;

				for (const auto& e : other.extents)
				{
					const auto start = other.base + e.offset;
					if (start < at + length && at < start + std::max<std::uint64_t>(e.length, 1)) return true;
				}
			}

			return false;
		}

		// Every item that keeps its length, in one piece that no other item shares, is patched where it lies; false,
		// and nothing planned, when one does not.
		bool patch_in_place(reader& r, const heif::layout& l, const std::vector<item_change>& changes, write_plan& plan)
		{
			for (const auto& c : changes)
			{
				const auto* const loc = c.id ? l.location_of(*c.id) : nullptr;
				const auto at = single_place(l, loc);
				if (!at || loc->extents.front().length != c.data.size()) return false;
				if (loc->method == 0 && shares_data(l, *c.id, *at, c.data.size())) return false;
			}

			for (const auto& c : changes) push_changed(r, plan, *single_place(l, l.location_of(*c.id)), c.data);
			return true;
		}

		// A span of the source and the length it takes in the result. A length of zero inserts.
		struct span_edit
		{
			std::uint64_t offset = 0;
			std::uint64_t length = 0;
			std::uint64_t new_length = 0;
		};

		// Where a position of the source lies in the result: after every edit that ends at or before it, and after
		// what is inserted where it is. Lengths only add up to a position, so the sum is taken modulo 2^64.
		std::uint64_t moved(const std::vector<span_edit>& edits, const std::uint64_t at)
		{
			auto result = at;

			for (const auto& e : edits)
			{
				if (e.length == 0 ? e.offset <= at : e.offset + e.length <= at)
					result = result + e.new_length - e.length;
			}

			return result;
		}

		class rewrite
		{
		public:
			rewrite(const heif::layout& l, const std::vector<item_change>& changes) : _l(l)
			{
				refuse_what_cannot_move();

				for (const auto& c : changes)
				{
					if (const auto range = replaceable(c)) _replaced.push_back({*c.id, *range, &c.data});
					else _placed.push_back({c.id.value_or(0), 0, &c.data});
				}

				_format = l.locations_format;
				choose_widths(changes);
				if (std::ranges::any_of(_placed, [](const placed_item& p) { return p.id == 0; })) choose_new_id();
			}

			void write_into(reader& r, write_plan& out)
			{
				// What is placed goes at the start of the first mdat, or in an mdat of its own after the meta box.
				bytes block;
				for (auto& p : _placed)
				{
					p.offset = block.size();
					block.insert(block.end(), p.data->begin(), p.data->end());
				}

				const auto first_mdat = std::ranges::find(_l.boxes, box_mdat, &heif::box_place::type);
				const auto own_mdat = !_placed.empty() && first_mdat == _l.boxes.end();
				if (own_mdat) block = make_box(box_mdat, block, false);
				const auto insert_at = own_mdat || first_mdat == _l.boxes.end()
				                           ? _l.meta.end()
				                           : first_mdat->offset + first_mdat->header;
				const auto block_header = own_mdat ? block.size() - total_placed() : 0;

				_edits.push_back({_l.meta.offset, _l.meta.size, _l.meta.size});
				for (const auto& rep : _replaced) _edits.push_back({rep.offset, rep.length, rep.data->size()});
				if (!block.empty()) _edits.push_back({insert_at, 0, block.size()});

				refuse_overlaps(insert_at, block.empty());

				// The meta box's length does not depend on the positions it holds, so it is built twice.
				auto meta = build_meta(insert_at, block_header);
				_edits.front().new_length = meta.size();
				meta = build_meta(insert_at, block_header);
				if (meta.size() != _edits.front().new_length)
					throw std::logic_error("the HEIF meta box changed length when its positions did");

				out.replacements.emplace_back(_l.meta.offset, _l.meta.size, std::move(meta));
				for (const auto& rep : _replaced) out.replacements.emplace_back(rep.offset, rep.length, *rep.data);
				if (!block.empty()) out.replacements.emplace_back(insert_at, 0, std::move(block));
				if (!own_mdat) resize_mdats(first_mdat == _l.boxes.end() ? nullptr : &*first_mdat, out);
				move_chunk_offsets(r, out);
			}

			// One of the meta box's children written as these bytes in place of its own.
			void replace_child(const std::size_t index, bytes child)
			{
				_children.emplace_back(index, std::move(child));
			}
			// A box added after the meta box's last child.
			void append_child(bytes child) { _appended.push_back(std::move(child)); }

		private:
			struct replaced_item
			{
				std::uint32_t id = 0;
				std::uint64_t offset = 0;
				const bytes* data = nullptr;
				std::uint64_t length = 0;

				replaced_item(const std::uint32_t item, const std::pair<std::uint64_t, std::uint64_t> range,
				              const bytes* with) : id(item), offset(range.first), data(with), length(range.second)
				{
				}
			};

			struct placed_item
			{
				// Zero for the packet item the file gains, until it has an id.
				std::uint32_t id = 0;
				// Where its data starts in what is placed.
				std::uint64_t offset = 0;
				const bytes* data = nullptr;
			};

			const heif::layout& _l;
			std::vector<replaced_item> _replaced;
			std::vector<placed_item> _placed;
			std::vector<span_edit> _edits;
			heif::location_format _format;
			std::uint32_t _new_id = 0;
			std::vector<std::pair<std::size_t, bytes>> _children;
			std::vector<bytes> _appended;

			std::uint64_t total_placed() const
			{
				std::uint64_t total = 0;
				for (const auto& p : _placed) total += p.data->size();
				return total;
			}

			void refuse_what_cannot_move() const
			{
				if (_l.cut) throw format_error("the HEIF file is cut short");

				if (std::ranges::any_of(_l.boxes, [](const heif::box_place& b)
				{ return std::ranges::find(fragment_boxes, b.type) != std::end(fragment_boxes); }))
				{
					throw format_error(
						"the HEIF holds a fragmented image sequence, whose media an update does not move");
				}

				if (_l.movie &&
				    std::ranges::none_of(_l.boxes, [](const heif::box_place& b) { return b.type == box_moov; }))
					throw format_error("the HEIF's movie box lies where an update does not read it");

				if (std::ranges::any_of(_l.boxes, [](const heif::box_place& b) { return b.size < b.header; }))
					throw format_error("a HEIF top-level box is smaller than its header");

				for (const auto size : {_l.locations_format.offset_size, _l.locations_format.length_size,
				                        _l.locations_format.base_size, _l.locations_format.index_size})
				{
					if (size != 0 && size != 4 && size != 8)
						throw format_error("the HEIF item locations have a field width libfiles does not write");
				}

				if (!_l.iloc_child) throw format_error("the HEIF file has no item locations");
			}

			// The bytes of the item's data when it can be replaced where it lies: one piece of this file, inside an
			// mdat's payload, that no other item's data shares.
			std::optional<std::pair<std::uint64_t, std::uint64_t>> replaceable(const item_change& c) const
			{
				if (!c.id) return {};

				const auto* const loc = _l.location_of(*c.id);
				if (!loc || loc->method != 0 || loc->data_reference != 0) return {};

				const auto at = single_place(_l, loc);
				if (!at) return {};

				const auto length = loc->extents.front().length;
				const auto end = *at + length;
				if (end < *at) return {};

				const auto in_mdat = std::ranges::any_of(_l.boxes, [&](const heif::box_place& b)
				{ return b.type == box_mdat && b.offset + b.header <= *at && end <= b.end(); });
				if (!in_mdat || shares_data(_l, *c.id, *at, length)) return {};

				return std::pair{*at, length};
			}

			// Wide enough fields for every position the result can hold. The meta box can at most double, with
			// room for the entries the update adds.
			void choose_widths(const std::vector<item_change>& changes)
			{
				std::uint64_t growth = _l.meta.size + 65536;
				std::uint64_t longest = 0;

				for (const auto& c : changes)
				{
					growth += c.data.size();
					longest = std::max<std::uint64_t>(longest, c.data.size());
				}

				if (!_placed.empty() && _format.offset_size == 0 && _format.base_size == 0) _format.offset_size = 4;
				if (_format.length_size == 0 && (!_placed.empty() || !_replaced.empty())) _format.length_size = 4;

				if (_l.file_size + growth > max_u32)
				{
					if (_format.offset_size == 4) _format.offset_size = 8;
					if (_format.base_size == 4) _format.base_size = 8;
				}

				if (longest > max_u32 && _format.length_size == 4) _format.length_size = 8;
			}

			// The next id after every one the file gives, in the widths the item boxes hold it in.
			void choose_new_id()
			{
				_new_id = _l.largest_id + 1;
				const auto narrow = _format.version < 2 || (_l.iref_child && _l.iref_version == 0);
				if (_new_id == 0 || (narrow && _new_id > max_u16))
					throw format_error("the HEIF has no item id left for its packet");
				if (_l.iref_child && _l.iref_version == 0 && _l.primary > max_u16)
					throw format_error("the HEIF's references cannot name its primary image");

				for (auto& p : _placed)
					if (p.id == 0) p.id = _new_id;
			}

			// No other item's data may lie in what is rewritten - the meta box, the data replaced, an mdat's size -
			// or across where the placed data goes in.
			void refuse_overlaps(const std::uint64_t insert_at, const bool nothing_inserted) const
			{
				std::vector<std::pair<std::uint64_t, std::uint64_t>> rewritten = {{_l.meta.offset, _l.meta.end()}};
				for (const auto& b : _l.boxes)
					if (b.type == box_mdat) rewritten.emplace_back(b.offset, b.offset + b.header);

				for (const auto& loc : _l.locations)
				{
					if (loc.method != 0 || loc.data_reference != 0) continue;
					if (std::ranges::any_of(_replaced, [&loc](const replaced_item& r) { return r.id == loc.item; }) ||
					    std::ranges::any_of(_placed, [&loc](const placed_item& p) { return p.id == loc.item; }))
					{
						continue;
					}

					for (const auto& e : loc.extents)
					{
						const auto start = loc.base + e.offset;
						const auto end = start + std::max<std::uint64_t>(e.length, 1);
						if (start < loc.base || end < start)
							throw format_error("a HEIF item lies past the end of what a file can hold");

						for (const auto& [from, to] : rewritten)
							if (start < to && from < end)
								throw format_error("a HEIF item's data lies in a box the update rewrites");

						for (const auto& rep : _replaced)
							if (start < rep.offset + rep.length && rep.offset < end)
								throw format_error("a HEIF item's data lies in data the update replaces");

						if (!nothing_inserted && start < insert_at && insert_at < start + e.length)
							throw format_error("a HEIF item's data lies across where the update puts its own");
					}
				}
			}

			// An item location moved with the bytes it locates: as offsets from no base where extents have offsets,
			// else as its base.
			heif::location rebased(const heif::location& loc) const
			{
				auto out = loc;

				if (_format.offset_size != 0)
				{
					out.base = 0;
					for (auto& e : out.extents) e.offset = moved(_edits, loc.base + e.offset);
				}
				else
				{
					out.base = moved(_edits, loc.base);
				}

				return out;
			}

			heif::location placed_at(const std::uint32_t id, const std::uint64_t at, const std::uint64_t length) const
			{
				heif::location out;
				out.item = id;
				heif::extent e;
				e.length = length;

				if (_format.offset_size != 0) e.offset = at;
				else out.base = at;

				out.extents = {e};
				return out;
			}

			void append_location(bytes& out, const heif::location& l) const
			{
				const auto& f = _format;
				const auto writable = fits(l.base, f.base_size) &&
				                      std::ranges::all_of(l.extents, [&f](const heif::extent& e)
				{
					return fits(e.offset, f.offset_size) && fits(e.length, f.length_size) &&
					       fits(e.index, f.version >= 1 ? f.index_size : 0);
				});

				if (!writable) throw format_error("the HEIF item locations cannot hold where the update puts data");

				if (f.version < 2) append_be16(out, l.item);
				else append_be32(out, l.item);
				if (f.version >= 1) append_be16(out, l.method);
				append_be16(out, l.data_reference);
				append_sized(out, l.base, f.base_size);
				append_be16(out, l.extents.size());

				for (const auto& e : l.extents)
				{
					if (f.version >= 1) append_sized(out, e.index, f.index_size);
					append_sized(out, e.offset, f.offset_size);
					append_sized(out, e.length, f.length_size);
				}
			}

			// The bytes of the meta box's payload that lie from one file position to another.
			std::span<const std::uint8_t> meta_bytes(const std::uint64_t from, const std::uint64_t to) const
			{
				const auto payload_at = _l.meta.offset + _l.meta.header;
				return std::span(_l.meta_payload)
				    .subspan(static_cast<std::size_t>(from - payload_at), static_cast<std::size_t>(to - from));
			}

			bytes build_iloc(const std::uint64_t placed_at_start) const
			{
				const auto& child = _l.children[*_l.iloc_child];
				const auto head = meta_bytes(child.offset + child.header, child.offset + child.header + 4);

				bytes payload;
				append_full_box_header(payload, _format.version, load_be32(head.data()) & 0xffffffu);
				payload.push_back(static_cast<std::uint8_t>(_format.offset_size << 4 | _format.length_size));
				payload.push_back(static_cast<std::uint8_t>(_format.base_size << 4 |
				                                            (_format.version >= 1 ? _format.index_size : 0)));

				const auto added = std::ranges::count_if(_placed, [this](const placed_item& p)
				{ return _l.location_of(p.id) == nullptr; });
				const auto count = _l.locations.size() + static_cast<std::size_t>(added);
				if (_format.version < 2 && count > max_u16) throw format_error("the HEIF has too many items to locate");
				if (_format.version < 2) append_be16(payload, count);
				else append_be32(payload, static_cast<std::uint32_t>(count));

				const auto placed_of = [this](const std::uint32_t id)
				{
					return std::ranges::find(_placed, id, &placed_item::id);
				};

				for (const auto& loc : _l.locations)
				{
					if (const auto p = placed_of(loc.item); p != _placed.end())
					{
						append_location(payload, placed_at(loc.item, placed_at_start + p->offset, p->data->size()));
					}
					else if (const auto rep = std::ranges::find(_replaced, loc.item, &replaced_item::id);
					         rep != _replaced.end())
					{
						auto l = rebased(loc);
						l.extents.front().length = rep->data->size();
						append_location(payload, l);
					}
					else if (loc.method == 0 && loc.data_reference == 0)
					{
						append_location(payload, rebased(loc));
					}
					else
					{
						append_location(payload, loc);
					}
				}

				for (const auto& p : _placed)
				{
					if (!_l.location_of(p.id))
						append_location(payload, placed_at(p.id, placed_at_start + p.offset, p.data->size()));
				}

				return make_box(box_iloc, payload, child.header == 16);
			}

			// The packet item's entry, as libheif's add_XMP_metadata writes one.
			bytes packet_entry() const
			{
				bytes payload;
				const auto version = _new_id > max_u16 ? 3 : 2;
				append_full_box_header(payload, static_cast<std::uint8_t>(version), 1);
				if (version == 2) append_be16(payload, _new_id);
				else append_be32(payload, _new_id);
				append_be16(payload, 0);
				append_be32(payload, item_mime);
				payload.push_back(0);
				append(payload, xmp_content_type);
				payload.push_back(0);
				payload.push_back(0);
				return make_box(box_infe, payload, false);
			}

			bytes build_iinf() const
			{
				const auto& child = _l.children[*_l.iinf_child];
				const auto start = child.offset + child.header;
				const auto head = meta_bytes(start, start + 4);
				const auto version = head[0];
				const auto count_size = version == 0 ? 2u : 4u;
				const auto count_bytes = meta_bytes(start + 4, start + 4 + count_size);
				const std::uint64_t count = version == 0 ? std::uint64_t{count_bytes[0]} << 8 | count_bytes[1]
				                                         : load_be32(count_bytes.data());

				bytes payload(head.begin(), head.end());
				if (version == 0 && count + 1 > max_u16) payload[0] = 1;

				if (payload[0] == 0) append_be16(payload, count + 1);
				else append_be32(payload, static_cast<std::uint32_t>(count + 1));

				const auto entries = meta_bytes(start + 4 + count_size, _l.iinf_entries_end);
				payload.insert(payload.end(), entries.begin(), entries.end());
				const auto entry = packet_entry();
				payload.insert(payload.end(), entry.begin(), entry.end());
				return make_box(box_iinf, payload, child.header == 16);
			}

			// The item references with the packet item's 'cdsc' to the primary image after them.
			bytes build_iref() const
			{
				bytes payload;
				auto version = static_cast<std::uint8_t>(_new_id > max_u16 || _l.primary > max_u16 ? 1 : 0);
				auto large = false;

				if (_l.iref_child)
				{
					const auto& child = _l.children[*_l.iref_child];
					const auto existing = meta_bytes(child.offset + child.header, child.end());
					payload.assign(existing.begin(), existing.end());
					version = _l.iref_version;
					large = child.header == 16;
				}
				else
				{
					append_full_box_header(payload, version, 0);
				}

				append_be32(payload, version == 0 ? 14u : 18u);
				append_be32(payload, ref_cdsc);
				if (version == 0) append_be16(payload, _new_id);
				else append_be32(payload, _new_id);
				append_be16(payload, 1);
				if (version == 0) append_be16(payload, _l.primary);
				else append_be32(payload, _l.primary);
				return make_box(box_iref, payload, large);
			}

			bytes build_meta(const std::uint64_t insert_at, const std::uint64_t block_header) const
			{
				const auto adding = _new_id != 0;
				const auto placed_start = moved(_edits, insert_at) -
				                          (_edits.back().length == 0 ? _edits.back().new_length : 0) + block_header;

				bytes payload(_l.meta_payload.begin(), _l.meta_payload.begin() + 4);

				const auto append_bytes = [&payload](const std::span<const std::uint8_t> b)
				{
					payload.insert(payload.end(), b.begin(), b.end());
				};

				for (std::size_t i = 0; i < _l.children.size(); ++i)
				{
					const auto& child = _l.children[i];
					const auto replaced = std::ranges::find(_children, i, &std::pair<std::size_t, bytes>::first);

					if (i == *_l.iloc_child)
					{
						append_bytes(build_iloc(placed_start));
					}
					else if (replaced != _children.end())
					{
						append_bytes(replaced->second);
					}
					else if (adding && _l.iinf_child && i == *_l.iinf_child)
					{
						append_bytes(build_iinf());
						if (!_l.iref_child) append_bytes(build_iref());
					}
					else if (adding && _l.iref_child && i == *_l.iref_child)
					{
						append_bytes(build_iref());
					}
					else
					{
						auto raw = meta_bytes(child.offset, child.end());
						const auto at = payload.size();
						append_bytes(raw);

						// A box that ran to the end of the meta box may not now: it is given its size.
						if (child.declared == 0)
						{
							if (child.header == 16 || child.size > max_u32)
								throw format_error("a HEIF box runs to the end of the meta box");
							const auto size = static_cast<std::uint32_t>(child.size);
							payload[at] = static_cast<std::uint8_t>(size >> 24);
							payload[at + 1] = static_cast<std::uint8_t>(size >> 16);
							payload[at + 2] = static_cast<std::uint8_t>(size >> 8);
							payload[at + 3] = static_cast<std::uint8_t>(size);
						}
					}
				}

				for (const auto& box : _appended) append_bytes(box);

				if (adding && !_l.iinf_child) throw format_error("the HEIF file has no item information");
				return make_box(box_meta, payload, _l.meta.header == 16);
			}

			// The size of each mdat whose payload changes length, where it declares one.
			void resize_mdats(const heif::box_place* first_mdat, write_plan& plan) const
			{
				for (const auto& b : _l.boxes)
				{
					if (b.type != box_mdat) continue;

					std::uint64_t delta = 0;
					for (const auto& rep : _replaced)
						if (b.offset + b.header <= rep.offset && rep.offset + rep.length <= b.end())
							delta += rep.data->size() - rep.length;
					if (&b == first_mdat) delta += total_placed();

					if (delta == 0 || b.declared == 0) continue;

					const auto size = b.declared + delta;
					bytes field;

					if (b.header == 16)
					{
						append_be64(field, size);
						plan.replacements.emplace_back(b.offset + 8, 8, std::move(field));
					}
					else
					{
						if (size > max_u32) throw format_error("a HEIF media data box would outgrow its 32-bit size");
						append_be32(field, static_cast<std::uint32_t>(size));
						plan.replacements.emplace_back(b.offset, 4, std::move(field));
					}
				}
			}

			// An image sequence's chunk offsets, moved with the media they locate: each table rewritten where it lies,
			// at its size.
			void move_chunk_offsets(reader& r, write_plan& out) const
			{
				for (const auto& b : _l.boxes)
				{
					if (b.type != box_moov) continue;
					if (b.size > max_movie) throw format_error("the HEIF's movie box is too large to update");

					const auto movie = r.read(b.offset, static_cast<std::size_t>(b.size));
					if (movie.size() != b.size) throw format_error("the HEIF's movie box could not be read");
					move_offsets_in(movie, b.offset, b.header, movie.size(), 0, out);
				}
			}

			// The boxes between from and to of a movie box read whole, which lies at movie_at: the way down a track to
			// its sample table, depth boxes along, and the chunk offsets there.
			void move_offsets_in(const bytes& movie, const std::uint64_t movie_at, std::uint64_t from,
			                     const std::uint64_t to, const std::size_t depth, write_plan& out) const
			{
				while (from < to)
				{
					bmff::box_header h;
					const auto status = bmff::read_header(std::span(movie).subspan(static_cast<std::size_t>(from),
					                                                               static_cast<std::size_t>(to - from)),
					                                      h);
					if (status == bmff::header_status::none) return;

					const auto size = h.size == 0 ? to - from : h.size;
					if (status != bmff::header_status::box || size < h.header || size > to - from)
						throw format_error("the HEIF's movie box is damaged");

					if (depth < std::size(sample_table_path) && h.type == sample_table_path[depth])
						move_offsets_in(movie, movie_at, from + h.header, from + size, depth + 1, out);
					else if (depth == std::size(sample_table_path) && (h.type == box_stco || h.type == box_co64))
						move_table(movie, movie_at, from + h.header, from + size, h.type == box_co64, out);

					from += size;
				}
			}

			// A chunk offset table - its version and flags, its count, then the offsets - written anew where any
			// offset moves. A chunk may not begin in what the update rewrites, nor a four-byte offset pass them.
			void move_table(const bytes& movie, const std::uint64_t movie_at, const std::uint64_t from,
			                const std::uint64_t to, const bool wide, write_plan& out) const
			{
				const auto width = wide ? 8u : 4u;
				if (to - from < 8) throw format_error("a HEIF track's chunk offsets are damaged");
				const auto count = load_be32(movie.data() + static_cast<std::size_t>(from) + 4);
				if (std::uint64_t{count} * width > to - from - 8)
					throw format_error("a HEIF track's chunk offsets are damaged");

				bytes table;
				auto changed = false;

				for (std::uint64_t i = 0; i < count; ++i)
				{
					const auto* const p = movie.data() + static_cast<std::size_t>(from + 8 + i * width);
					const auto offset = wide ? std::uint64_t{load_be32(p)} << 32 | load_be32(p + 4) : load_be32(p);

					const auto rewritten = (_l.meta.offset <= offset && offset < _l.meta.end()) ||
					                       std::ranges::any_of(_replaced, [offset](const replaced_item& rep)
					{ return rep.offset <= offset && offset < rep.offset + rep.length; });
					if (rewritten) throw format_error("a HEIF track's media lies in what the update rewrites");

					const auto at = moved(_edits, offset);
					if (!wide && at > max_u32) throw format_error("a HEIF track's chunk offset would outgrow 32 bits");
					changed |= at != offset;

					if (wide) append_be64(table, at);
					else append_be32(table, static_cast<std::uint32_t>(at));
				}

				if (changed) out.replacements.emplace_back(movie_at + from + 8, table.size(), std::move(table));
			}
		};
	}

	void plan_heif(reader& r, const metadata_update& update, write_plan& plan)
	{
		const auto layout = heif::read_layout(r);

		// A HEIF's orientation is its images' rotations and mirrors, which libheif turns the picture by: a turn of the
		// picture as libheif decodes it is written there, and the EXIF orientation made the one they come to.
		auto edits = update.legacy;
		std::optional<item_properties> properties;
		std::optional<turn_plan> turned;

		if (edits.orientation)
		{
			properties = read_item_properties(layout);
			turned = plan_turn(layout, *properties, *edits.orientation);
			edits.orientation = turned->orientation;
		}

		std::vector<item_change> changes;

		if (update.xmp)
		{
			// A packet that states an orientation states the one the EXIF is made.
			auto packet = *update.xmp;
			if (turned && packet.exists(xmp::ns::tiff, "Orientation"))
				packet.set_property_int(xmp::ns::tiff, "Orientation", turned->orientation);

			const auto old_size = layout.xmp ? layout.xmp->size : 0;
			changes.push_back({layout.xmp ? std::optional(layout.xmp->id) : std::nullopt,
			                   to_bytes(serialize_packet(packet, old_size))});
		}

		if (layout.exif && touches_exif(edits))
		{
			if (!layout.exif->data) throw format_error("the HEIF's EXIF is too large to edit");
			if (auto edited = edit_exif(*layout.exif->data, edits))
				changes.push_back({layout.exif->id, std::move(*edited)});
		}

		if ((!turned || turned->in_place) && (changes.empty() || patch_in_place(r, layout, changes, plan)))
		{
			const auto payload_at = layout.meta.offset + layout.meta.header;
			if (turned)
				for (const auto& [at, value] : turned->patches)
					push_changed(r, plan, payload_at + at, std::vector<std::uint8_t>{value});
			return;
		}

		rewrite written(layout, changes);
		if (turned) written.replace_child(properties->iprp_child, std::move(turned->iprp));
		written.write_into(r, plan);
	}

	namespace heif
	{
		std::vector<std::uint8_t> finish_gain_map(const std::span<const std::uint8_t> file,
		                                          const std::uint32_t tone_map, const std::uint32_t gain_map)
		{
			reader r(file);
			const auto layout = read_layout(r);
			if (!layout.iinf_child) throw format_error("the HEIF has no item information");

			// The gain map's entry marked hidden, by the lowest bit of its flags, as libavif writes it: it is an
			// input of the tone map item, not a picture of its own.
			const auto& iinf = layout.children[*layout.iinf_child];
			const auto payload_at = layout.meta.offset + layout.meta.header;
			const auto first = static_cast<std::ptrdiff_t>(iinf.offset - payload_at);
			bytes entries(layout.meta_payload.begin() + first,
			              layout.meta_payload.begin() + first + static_cast<std::ptrdiff_t>(iinf.size));

			const auto count_size = entries.size() > iinf.header && entries[iinf.header] == 0 ? 2u : 4u;
			auto hidden = false;

			for (std::size_t pos = iinf.header + 4 + count_size; pos + 8 <= entries.size();)
			{
				bmff::box_header h;
				const auto status = bmff::read_header(std::span(entries).subspan(pos), h);
				const auto size = h.size == 0 ? entries.size() - pos : h.size;
				if (status != bmff::header_status::box || size < h.header || size > entries.size() - pos) break;

				const auto at = pos + h.header;
				if (h.type == box_infe && size >= h.header + 6)
				{
					const auto version = entries[at];
					const auto id = version >= 3 && size >= h.header + 8
					                    ? load_be32(entries.data() + at + 4)
					                    : std::uint32_t{entries[at + 4]} << 8 | entries[at + 5];
					if (id == gain_map)
					{
						entries[at + 3] |= 1u;
						hidden = true;
					}
				}

				pos += static_cast<std::size_t>(size);
			}

			if (!hidden) throw format_error("the HEIF has no entry for its gain map");

			// The tone map item and the primary image as alternatives, the tone map item preferred: a reader that does
			// not apply a gain map shows the primary.
			bytes altr;
			append_full_box_header(altr, 0, 0);
			append_be32(altr, layout.largest_id + 1);
			append_be32(altr, 2);
			append_be32(altr, tone_map);
			append_be32(altr, layout.primary);

			rewrite written(layout, {});
			written.replace_child(*layout.iinf_child, std::move(entries));
			written.append_child(make_box(box_grpl, make_box(box_altr, altr, false), false));

			write_plan plan;
			plan.source_size = file.size();
			written.write_into(r, plan);
			std::ranges::stable_sort(plan.replacements, {}, &replacement::offset);

			memory_stream out;
			write_updated(r, plan, out);
			return std::move(out.bytes());
		}
	}
}
