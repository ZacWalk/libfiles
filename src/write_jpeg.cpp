// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Writing metadata to a JPEG: the XMP packet in its APP1, EXIF in its APP1 and IPTC in the
// Photoshop resources of APP13. EXIF is edited where it lies, so an edit that fits is a patch and one
// that does not grows the segment at its end; nothing in the EXIF block moves. A packet that fits the
// old one is a patch. Segments the file lacks are added after its leading APP0 and APP1 segments, and
// every other segment keeps its place and its bytes, but for a multi-picture index, whose sizes and
// offsets follow the pictures as the header grows or shrinks. Extended XMP segments are left as they are:
// the main packet keeps the GUID that names them. The packet's gain map signalling is kept true to the
// file: the length of the gain map the index locates, or, when nothing follows the primary, nothing said
// of one - and then an index pointing past the end, and ISO 21496-1's version segment, go too.

#include "legacy.h"

#include "gain_map_internal.h"

#include <algorithm>
#include <optional>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::string_view exif_signature{"Exif\0\0", 6};
		constexpr std::string_view xmp_signature{"http://ns.adobe.com/xap/1.0/\0", 29};
		constexpr std::string_view photoshop_signature{"Photoshop 3.0\0", 14};

		// A segment's length field counts itself, and holds at most 65535.
		constexpr std::uint64_t max_segment_payload = 65533;
		constexpr std::uint64_t max_packet = max_segment_payload - xmp_signature.size();

		struct segment
		{
			std::uint64_t start = 0;
			std::uint8_t marker = 0;
			std::uint32_t length = 0; // the length field: the payload and the field itself

			std::uint64_t payload() const { return start + 4; }
			std::uint64_t payload_size() const { return length - 2u; }
			std::uint64_t end() const { return start + 2 + length; }
		};

		bytes make_segment(const std::uint8_t marker, const std::string_view signature,
		                   const std::span<const std::uint8_t> data)
		{
			const auto length = 2 + signature.size() + data.size();
			bytes s = {0xFF, marker, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)};
			append(s, signature);
			s.insert(s.end(), data.begin(), data.end());
			return s;
		}

		bytes be16(const std::uint64_t v)
		{
			return {static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
		}

		// The packet as it fits a segment: as the toolkit sized it when that fits, else compact with no
		// formatting and modest padding.
		std::string jpeg_packet(const xmp::meta& meta, const std::uint64_t old_size)
		{
			auto packet = serialize_packet(meta, old_size);
			if (packet.size() <= max_packet) return packet;

			xmp::serialize_options tight;
			tight.use_compact_format = true;
			tight.omit_all_formatting = true;
			tight.read_only_packet = true;
			const auto minimum = meta.serialize(tight).size();

			if (minimum > max_packet) throw format_error("the XMP packet is too large for a JPEG segment");

			xmp::serialize_options padded;
			padded.use_compact_format = true;
			padded.omit_all_formatting = true;
			padded.exact_packet_length = true;
			padded.padding = static_cast<std::uint32_t>(std::min<std::uint64_t>(minimum + 2048, max_packet));
			return meta.serialize(padded);
		}

		void set_resource(std::vector<photoshop_resource>& resources, const std::uint16_t id, bytes data)
		{
			if (auto* const existing = find_photoshop_resource(resources, id)) existing->data = std::move(data);
			else resources.push_back({id, {}, std::move(data)});
		}

		// The gain map a multi-picture index locates: the first picture after the primary whose own
		// metadata marks it as one. dangling when the index points at a picture the file does not hold.
		struct located_gain_map
		{
			std::optional<gain_map_signal> gain_map;
			bool dangling = false;
		};

		located_gain_map locate_gain_map(reader& r, const std::uint64_t index_at, const mpf_index& mpf)
		{
			// A gain map's metadata comes long before its first scan.
			constexpr std::uint64_t header_bytes = 256 * 1024;
			located_gain_map located;

			for (std::size_t e = 1; e < mpf.entries.size(); ++e)
			{
				const auto& entry = mpf.entries[e];
				if (entry.offset == 0) continue;

				const auto start = index_at + entry.offset;

				if (!r.contains(start, std::max<std::uint64_t>(entry.size, 2)) || r.be16(start) != 0xFFD8)
				{
					located.dangling = true;
					continue;
				}

				if (located.gain_map) continue;
				const auto head = r.read(start,
				                         static_cast<std::size_t>(std::min<std::uint64_t>(entry.size, header_bytes)));
				const auto schemes = read_gain_map_schemes(head);
				if (schemes.any()) located.gain_map = gain_map_signal{entry.size, schemes.adobe};
			}

			return located;
		}

		// True when the primary is all the file holds, its EOI the file's end, so no picture follows it.
		bool primary_is_whole_file(reader& r)
		{
			// Read whole only for a file whose signalling is at odds with its index, which a photo this
			// large is not worth.
			constexpr std::uint64_t most = 512ull * 1024 * 1024;
			if (r.size() > most) return false;

			const auto resident = r.resident();
			if (!resident.empty()) return jpeg_end(resident) == resident.size();
			const auto whole = r.read(0, static_cast<std::size_t>(r.size()));
			return jpeg_end(whole) == whole.size();
		}
	}

	void set_iptc_resources(std::vector<photoshop_resource>& resources, const bytes& iptc)
	{
		set_resource(resources, 0x0404, iptc);
		const auto digest = md5(iptc);
		set_resource(resources, 0x0425, bytes(digest.begin(), digest.end()));
	}

	void plan_jpeg(reader& r, const metadata_update& update, write_plan& plan)
	{
		const auto size = r.size();
		if (r.be16(0) != 0xFFD8) throw format_error("not a JPEG file");

		std::vector<segment> segments;
		auto reached_scan = false;
		std::uint64_t pos = 2;

		while (size - pos >= 2)
		{
			if (r.u8(pos) != 0xFF) throw format_error("the JPEG's markers are damaged");

			auto marker = r.u8(pos + 1);

			while (marker == 0xFF && size - pos >= 3)
			{
				++pos;
				marker = r.u8(pos + 1);
			}

			if (marker == 0xDA || marker == 0xD9)
			{
				reached_scan = true;
				break;
			}

			if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
			{
				pos += 2;
				continue;
			}

			if (size - pos < 4) break;

			const std::uint32_t length = r.be16(pos + 2);
			if (length < 2 || length > size - pos - 2)
				throw format_error("a JPEG segment runs past the end of the file");

			segments.push_back({pos, marker, length});
			pos += 2 + length;
		}

		// An edit is not written into a file whose structure is already broken.
		if (!reached_scan) throw format_error("the JPEG is cut short");

		const segment* exif = nullptr;
		const segment* xmp = nullptr;
		const segment* mpf_segment = nullptr;
		std::vector<const segment*> photoshop;
		std::vector<const segment*> iso_versions;
		std::uint64_t insert_at = 2;
		auto leading = true;

		for (const auto& s : segments)
		{
			if (leading && (s.marker == 0xE0 || s.marker == 0xE1)) insert_at = s.end();
			else leading = false;

			if (s.marker == 0xE1)
			{
				const auto head = r.view(s.payload(),
				                         static_cast<std::size_t>(
											 std::min<std::uint64_t>(s.payload_size(), xmp_signature.size())));
				if (!exif && starts_with(head, exif_signature)) exif = &s;
				else if (!xmp && starts_with(head, xmp_signature)) xmp = &s;
			}
			else if (s.marker == 0xE2)
			{
				constexpr auto iso_version_size = iso_gain_map_identifier.size() + iso_version_bytes;
				const auto head = r.view(s.payload(), static_cast<std::size_t>(
														  std::min<std::uint64_t>(s.payload_size(), iso_version_size)));
				if (!mpf_segment && starts_with(head, mpf_identifier)) mpf_segment = &s;
				else if (s.payload_size() == iso_version_size && is_iso_version_segment(head))
					iso_versions.push_back(&s);
			}
			else if (s.marker == 0xED &&
			         starts_with(r.view(s.payload(),
			                            static_cast<std::size_t>(
											std::min<std::uint64_t>(s.payload_size(), photoshop_signature.size()))),
			                     photoshop_signature))
			{
				photoshop.push_back(&s);
			}
		}

		// What the multi-picture index says follows the primary, and the gain map among it.
		std::uint64_t index_at = 0;
		bytes index;
		std::optional<mpf_index> mpf;
		located_gain_map located;

		if (mpf_segment && mpf_segment->payload_size() > mpf_identifier.size())
		{
			index_at = mpf_segment->payload() + mpf_identifier.size();
			index = r.read(index_at, static_cast<std::size_t>(mpf_segment->payload_size() - mpf_identifier.size()));
			mpf = read_mpf_index(index);
			if (mpf) located = locate_gain_map(r, index_at, *mpf);
		}

		std::optional<bool> whole_file;
		const auto nothing_follows = [&r, &whole_file]
		{
			if (!whole_file) whole_file = primary_is_whole_file(r);
			return *whole_file;
		};

		// An index pointing past the end and a version segment saying a gain map follows go when nothing does.
		auto drop_mpf = false;

		if (!located.gain_map && ((mpf && located.dangling) || !iso_versions.empty()) && nothing_follows())
		{
			drop_mpf = mpf && located.dangling;
			if (drop_mpf)
				plan.replacements.push_back({mpf_segment->start, mpf_segment->end() - mpf_segment->start, {}});
			for (const auto* s : iso_versions) plan.replacements.push_back({s->start, s->end() - s->start, {}});
		}

		bytes new_exif;
		bytes new_xmp;
		bytes new_photoshop;

		if (touches_exif(update.legacy))
		{
			if (exif)
			{
				const auto base = exif->payload() + exif_signature.size();
				const auto block = r.read(base, static_cast<std::size_t>(exif->payload_size() - exif_signature.size()));
				reader block_reader(block);
				tiff_editor editor;
				if (!editor.open(block_reader)) throw format_error("the JPEG's EXIF block cannot be read");

				photo_legacy legacy{&editor};
				apply_photo_legacy(update.legacy, {}, false, legacy);

				std::uint64_t grown = 0;

				for (auto& c : editor.changes(block.size()))
				{
					grown += c.data.size() - c.length;
					c.offset += base;
					plan.replacements.push_back(std::move(c));
				}

				if (grown != 0)
				{
					const auto length = exif->length + grown;
					if (length > 0xFFFF) throw format_error("the EXIF block would not fit its JPEG segment");
					plan.replacements.push_back({exif->start + 2, 2, be16(length)});
				}
			}
			else
			{
				const auto empty = tiff_editor::empty_stream();
				reader block_reader(empty);
				tiff_editor editor;
				editor.open(block_reader);

				photo_legacy legacy{&editor};
				apply_photo_legacy(update.legacy, {}, false, legacy);

				const auto block = apply_changes(empty, editor.changes(empty.size()));
				if (block.size() + exif_signature.size() > max_segment_payload)
					throw format_error("the EXIF block is too large for a JPEG segment");
				new_exif = make_segment(0xE1, exif_signature, block);
			}
		}

		if (update.xmp)
		{
			// The packet says a gain map follows exactly when one does: with the length of the one the
			// index locates, or nothing said of one when nothing follows the primary.
			auto signalled = *update.xmp;
			const auto resignalled = signal_gain_map(signalled, located.gain_map) &&
			                         (located.gain_map || nothing_follows());
			const auto& packet_meta = resignalled ? signalled : *update.xmp;

			const auto old_size = xmp ? xmp->payload_size() - xmp_signature.size() : 0;
			const auto packet = jpeg_packet(packet_meta, old_size);

			if (xmp && packet.size() == old_size)
			{
				push_changed(r, plan, xmp->payload() + xmp_signature.size(), to_bytes(packet));
			}
			else if (xmp)
			{
				plan.replacements.push_back(
					{xmp->start, xmp->end() - xmp->start,
				     make_segment(0xE1, xmp_signature,
				                  {reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size()})});
			}
			else
			{
				new_xmp = make_segment(0xE1, xmp_signature,
				                       {reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size()});
			}
		}

		if (touches_iptc(update.legacy))
		{
			bytes block;

			for (const auto* s : photoshop)
			{
				const auto body = r.read(s->payload() + photoshop_signature.size(),
				                         static_cast<std::size_t>(s->payload_size() - photoshop_signature.size()));
				block.insert(block.end(), body.begin(), body.end());
			}

			std::vector<photoshop_resource> resources;
			if (!parse_photoshop_resources(block, resources))
				throw format_error("the JPEG's Photoshop resources cannot be read");

			const auto* const existing = find_photoshop_resource(resources, 0x0404);
			const auto existing_iptc = existing ? std::span<const std::uint8_t>(existing->data)
			                                    : std::span<const std::uint8_t>{};

			photo_legacy legacy;
			apply_photo_legacy(update.legacy, existing_iptc, true, legacy);

			if (legacy.iptc)
			{
				set_iptc_resources(resources, *legacy.iptc);
				const auto updated = serialize_photoshop_resources(resources);

				// A block larger than a segment continues in the next, each with the signature.
				constexpr auto chunk = max_segment_payload - photoshop_signature.size();
				bytes segments_bytes;

				for (std::size_t at = 0; at < updated.size(); at += chunk)
				{
					const auto n = std::min<std::size_t>(chunk, updated.size() - at);
					const auto seg = make_segment(0xED, photoshop_signature, {updated.data() + at, n});
					segments_bytes.insert(segments_bytes.end(), seg.begin(), seg.end());
				}

				if (photoshop.empty())
				{
					new_photoshop = std::move(segments_bytes);
				}
				else if (photoshop.size() == 1 &&
				         segments_bytes.size() == photoshop.front()->end() - photoshop.front()->start)
				{
					push_changed(r, plan, photoshop.front()->start, segments_bytes);
				}
				else
				{
					plan.replacements.push_back({photoshop.front()->start,
					                             photoshop.front()->end() - photoshop.front()->start,
					                             std::move(segments_bytes)});

					for (std::size_t i = 1; i < photoshop.size(); ++i)
					{
						plan.replacements.push_back(
							{photoshop[i]->start, photoshop[i]->end() - photoshop[i]->start, {}});
					}
				}
			}
		}

		// New segments in the order a JPEG normally holds them: EXIF, XMP, then the Photoshop resources.
		bytes added;
		added.insert(added.end(), new_exif.begin(), new_exif.end());
		added.insert(added.end(), new_xmp.begin(), new_xmp.end());
		added.insert(added.end(), new_photoshop.begin(), new_photoshop.end());
		if (!added.empty()) plan.replacements.push_back({insert_at, 0, std::move(added)});

		// The pictures after the primary move with what changes before them, and the primary's size with
		// all of it; offsets are measured from the index, which moves with what changes before it.
		if (mpf && !drop_mpf)
		{
			std::int64_t before = 0;
			std::int64_t after = 0;

			for (const auto& c : plan.replacements)
			{
				const auto delta = static_cast<std::int64_t>(c.data.size()) - static_cast<std::int64_t>(c.length);
				if (c.offset + c.length <= mpf_segment->start) before += delta;
				else if (c.offset >= mpf_segment->end()) after += delta;
			}

			if (before != 0 || after != 0)
			{
				auto updated = index;

				for (std::size_t e = 0; e < mpf->entries.size(); ++e)
				{
					auto entry = mpf->entries[e];
					if (e == 0) entry.size = static_cast<std::uint32_t>(entry.size + before + after);
					else if (entry.offset != 0) entry.offset = static_cast<std::uint32_t>(entry.offset + after);
					write_mpf_entry(updated, *mpf, entry);
				}

				push_changed(r, plan, index_at, updated);
			}
		}
	}
}
