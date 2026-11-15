// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Writing XMP, and the EXIF a file already holds, to a JPEG XL. The packet is an 'xml ' box and
// the EXIF an 'Exif' box, found as the scan finds them: each is replaced where it stands, a first packet
// goes before the codestream, where libjxl's encoder puts one, and a bare codestream is put in a
// container to hold it. A compressed box is replaced by a plain one, and every other box keeps its place
// and its bytes. A file made from a JPEG - one with a 'jbrd' box - keeps the boxes libjxl gives that JPEG
// back from at their size, or it could no longer give it back. An orientation is written in the codestream's
// header, which libjxl turns the picture by, and the EXIF's is made to agree; the header grows by whole bytes
// where it had no orientation, so the codestream's box grows with it. A file made from a JPEG is turned only
// where its EXIF takes the turn, or the JPEG it gives back would not be.

#include "bmff_internal.h"
#include "legacy.h"
#include "scan_internal.h"
#include "write_internal.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace libfiles::detail
{
	namespace
	{
		using bmff::fourcc;

		constexpr auto box_xml = fourcc("xml ");
		constexpr auto box_exif = fourcc("Exif");
		constexpr auto box_jxlc = fourcc("jxlc");

		// The 32-bit size, or the 64-bit one after it when contents need it or the box replaced had one, so
		// a box that keeps its length can still be patched.
		bytes make_box(const std::uint32_t type, const std::span<const std::uint8_t> contents, const bool large)
		{
			bytes out;
			const auto size = std::uint64_t{contents.size()};

			if (large || size > std::numeric_limits<std::uint32_t>::max() - 8u)
			{
				append_be32(out, 1);
				append_be32(out, type);
				append_be32(out, static_cast<std::uint32_t>((size + 16) >> 32));
				append_be32(out, static_cast<std::uint32_t>(size + 16));
			}
			else
			{
				append_be32(out, static_cast<std::uint32_t>(size + 8));
				append_be32(out, type);
			}

			out.insert(out.end(), contents.begin(), contents.end());
			return out;
		}

		// A box replaced where it stands: patched when it keeps its length, else rewritten.
		void replace_box(reader& r, const jxl_box_place& box, const bytes& replaced, write_plan& plan)
		{
			if (!box.compressed && replaced.size() == box.size) push_changed(r, plan, box.offset, replaced);
			else plan.replacements.emplace_back(box.offset, box.size, replaced);
		}

		// The block with the edit's EXIF fields written, as every photo's EXIF takes them. A block that
		// cannot be read is left as it is.
		bytes edit_block(const std::span<const std::uint8_t> block, const legacy_edits& edits)
		{
			reader block_reader(block);
			tiff_editor editor;
			if (!editor.open(block_reader)) return bytes(block.begin(), block.end());

			photo_legacy legacy{&editor};
			apply_photo_legacy(edits, {}, false, legacy);
			return apply_changes(block, editor.changes(block.size()));
		}

		// The fields EXIF takes, each with what it is written with: a rating's percent follows its stars,
		// and a latitude and longitude share the GPS directory.
		using field_copy = void (*)(legacy_edits& to, const legacy_edits& from);

		constexpr field_copy exif_fields[] = {
			[](legacy_edits& to, const legacy_edits& from) { to.description = from.description; },
			[](legacy_edits& to, const legacy_edits& from) { to.copyright = from.copyright; },
			[](legacy_edits& to, const legacy_edits& from) { to.creators = from.creators; },
			[](legacy_edits& to, const legacy_edits& from) { to.keywords = from.keywords; },
			[](legacy_edits& to, const legacy_edits& from)
		{
			to.rating = from.rating;
			to.rating_percent = from.rating_percent;
		},
			[](legacy_edits& to, const legacy_edits& from) { to.date_original = from.date_original; },
			[](legacy_edits& to, const legacy_edits& from) { to.date_digitized = from.date_digitized; },
			[](legacy_edits& to, const legacy_edits& from)
		{
			to.gps_latitude = from.gps_latitude;
			to.gps_longitude = from.gps_longitude;
		},
			[](legacy_edits& to, const legacy_edits& from) { to.orientation = from.orientation; },
		};

		// As edit_block, but the block keeps its size: the fields that fit where they lie are written, and
		// one that would grow the block is left as it is, for the packet to hold. Should the fields that fit
		// alone not fit together, none is written.
		bytes edit_block_in_place(const std::span<const std::uint8_t> block, const legacy_edits& edits)
		{
			auto all = edit_block(block, edits);
			if (all.size() == block.size()) return all;

			legacy_edits fitting;

			for (const auto copy : exif_fields)
			{
				legacy_edits one;
				copy(one, edits);
				if (touches_exif(one) && edit_block(block, one).size() == block.size()) copy(fitting, edits);
			}

			auto some = edit_block(block, fitting);
			return some.size() == block.size() ? some : bytes(block.begin(), block.end());
		}

		void plan_packet(reader& r, const jxl_layout& layout, const xmp::meta& meta, write_plan& plan)
		{
			if (!layout.xmp_box)
			{
				const auto packet = serialize_packet(meta, 0);
				plan.replacements.emplace_back(layout.codestream_box, 0, make_box(box_xml, to_bytes(packet), false));
				return;
			}

			const auto& box = *layout.xmp_box;
			if (!layout.xmp)
				throw format_error("the JPEG XL's packet is compressed, and its Brotli stream could not be read");

			const auto old_size = layout.xmp->size();
			const auto packet = serialize_packet(meta, old_size);

			// libjxl gives a JPEG back with the first packet box as its XMP segment, which has to be the
			// size the segment was.
			if (layout.jpeg_reconstruction && layout.first_xmp_box == box.offset && packet.size() != old_size)
			{
				throw format_error("the JPEG XL keeps the JPEG it was made from, whose XMP has no room for the edit");
			}

			replace_box(r, box, make_box(box_xml, to_bytes(packet), !box.compressed && box.header == 16), plan);
		}

		// The block an Exif box holds: after the offset to its TIFF header, past anything kept before it. Nothing
		// when the box is too short to say where it starts.
		std::optional<std::span<const std::uint8_t>> exif_block(const std::vector<std::uint8_t>& contents)
		{
			if (contents.size() < 4) return {};
			const auto start = std::uint64_t{4} + load_be32(contents.data());
			if (start > contents.size()) return {};
			return std::span<const std::uint8_t>(contents).subspan(static_cast<std::size_t>(start));
		}

		// Whether the EXIF libjxl gives a JPEG back with - the first Exif box, kept at its size - takes the
		// orientation where it lies.
		bool exif_takes_orientation(const jxl_layout& layout, const std::uint32_t orientation)
		{
			if (!layout.exif_box || layout.first_exif_box != layout.exif_box->offset) return false;
			const auto block = exif_block(layout.exif);
			if (!block) return false;

			legacy_edits turn;
			turn.orientation = static_cast<int>(orientation);
			return summarize_exif(edit_block_in_place(*block, turn)).orientation == orientation;
		}

		void plan_exif(reader& r, const jxl_layout& layout, const legacy_edits& edits, write_plan& plan)
		{
			if (layout.exif_unread)
				throw format_error("the JPEG XL's EXIF is compressed, and its Brotli stream could not be read");

			// A JPEG XL without EXIF does not gain one, as a WebP does not.
			if (!layout.exif_box) return;

			const auto& contents = layout.exif;
			const auto block = exif_block(contents);
			if (!block) return;
			const auto& box = *layout.exif_box;

			// libjxl gives a JPEG back with the first Exif box as its EXIF segment, which keeps its size.
			const auto keep_size = layout.jpeg_reconstruction && layout.first_exif_box == box.offset;
			const auto edited = keep_size ? edit_block_in_place(*block, edits) : edit_block(*block, edits);
			if (std::ranges::equal(edited, *block)) return;

			bytes updated(contents.begin(), contents.end() - static_cast<std::ptrdiff_t>(block->size()));
			updated.insert(updated.end(), edited.begin(), edited.end());
			replace_box(r, box, make_box(box_exif, updated, !box.compressed && box.header == 16), plan);
		}

		// A bare codestream has nowhere to keep a packet, so it goes into a container: the signature and file
		// type boxes, the packet, then the codestream in a 'jxlc' box, every byte of it as it was.
		bytes container_head(const xmp::meta& meta, const std::uint64_t codestream_size)
		{
			bytes out(std::begin(jxl_container_signature), std::end(jxl_container_signature));
			const bytes file_type = {0,   0,   0, 20, 'f', 't', 'y', 'p', 'j', 'x',
			                         'l', ' ', 0, 0,  0,   0,   'j', 'x', 'l', ' '};
			out.insert(out.end(), file_type.begin(), file_type.end());

			const auto packet = make_box(box_xml, to_bytes(serialize_packet(meta, 0)), false);
			out.insert(out.end(), packet.begin(), packet.end());

			if (codestream_size > std::numeric_limits<std::uint32_t>::max() - 8u)
			{
				append_be32(out, 1);
				append_be32(out, box_jxlc);
				append_be32(out, static_cast<std::uint32_t>((codestream_size + 16) >> 32));
				append_be32(out, static_cast<std::uint32_t>(codestream_size + 16));
			}
			else
			{
				append_be32(out, static_cast<std::uint32_t>(codestream_size + 8));
				append_be32(out, box_jxlc);
			}

			return out;
		}

		// A box whose contents grow: its size made larger to hold them, unless it runs to the end of the file.
		void plan_box_growth(reader& r, const std::uint64_t box, const std::uint64_t grown, write_plan& plan)
		{
			const auto size = r.be32(box);
			if (size == 0) return;

			bytes field;

			if (size == 1)
			{
				const auto large = r.be64(box + 8) + grown;
				append_be32(field, static_cast<std::uint32_t>(large >> 32));
				append_be32(field, static_cast<std::uint32_t>(large));
				plan.replacements.emplace_back(box + 8, 8, std::move(field));
				return;
			}

			if (std::uint64_t{size} + grown > std::numeric_limits<std::uint32_t>::max())
				throw format_error("the JPEG XL's codestream box would outgrow its 32-bit size");

			append_be32(field, static_cast<std::uint32_t>(size + grown));
			plan.replacements.emplace_back(box, 4, std::move(field));
		}
	}

	void plan_jxl(reader& r, const metadata_update& update, write_plan& plan)
	{
		const auto layout = read_jxl_layout(r, update.brotli ? &update.brotli : default_brotli_decompressor());
		if (!layout.whole) throw format_error("the JPEG XL cannot be read whole");

		// A JPEG XL's orientation is its codestream's, which libjxl turns the picture by: it is written there, and
		// the EXIF's is made to agree.
		std::optional<jxl_header_edit> header;

		if (update.legacy.orientation)
		{
			if (!layout.codestream_first)
				throw format_error("the JPEG XL's codestream begins in a part read out of order");

			const auto head = r.read(layout.codestream_data,
			                         static_cast<std::size_t>(
										 std::min<std::uint64_t>(layout.codestream_part, 1u << 20)));
			header = jxl_orientation_edit(head, static_cast<std::uint32_t>(*update.legacy.orientation));
			if (!header) throw format_error("the JPEG XL's codestream header cannot be read");

			// The orientation the codestream has already is left as it is.
			if (std::ranges::equal(header->bytes, std::span(head).first(header->replaced))) header.reset();
		}

		if (header)
		{
			if (header->bytes.size() != header->replaced && layout.frame_index)
				throw format_error("the JPEG XL's frame index counts the codestream bytes its orientation would move");

			// A file made from a JPEG gives that JPEG back with its first Exif box as the JPEG's EXIF: a turn that
			// box cannot take where it lies would leave the JPEG given back as it was.
			if (layout.jpeg_reconstruction &&
			    !exif_takes_orientation(layout, static_cast<std::uint32_t>(*update.legacy.orientation)))
				throw format_error(
					"the JPEG XL keeps the JPEG it was made from, whose EXIF has no orientation to turn");
		}

		const auto grown = header ? header->bytes.size() - header->replaced : 0;

		if (!layout.container)
		{
			if (header)
			{
				auto head = update.xmp ? container_head(*update.xmp, r.size() + grown) : bytes{};
				head.insert(head.end(), header->bytes.begin(), header->bytes.end());
				plan.replacements.emplace_back(0, header->replaced, std::move(head));
			}
			else if (update.xmp)
			{
				plan.replacements.emplace_back(0, 0, container_head(*update.xmp, r.size()));
			}

			return;
		}

		if (header)
		{
			plan.replacements.emplace_back(layout.codestream_data, header->replaced, std::move(header->bytes));
			if (grown != 0) plan_box_growth(r, layout.codestream_box, grown, plan);
		}

		if (update.xmp) plan_packet(r, layout, *update.xmp, plan);
		if (touches_exif(update.legacy)) plan_exif(r, layout, update.legacy, plan);
	}
}
