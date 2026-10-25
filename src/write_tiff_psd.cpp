// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing metadata to a TIFF file, by appending. The packet is tag 700 of IFD0; EXIF lives in
// the file's own directories, and IPTC in tag 33723 with its digest in the Photoshop resources of tag
// 34377. A packet that fits the old one is written over it. Anything else is appended at the end of
// the file and the entries that point to it are repointed, so the image data and every offset into
// it stay where they are. A DNG carries no IPTC, as the DNG specification asks.

#include "legacy.h"

#include <algorithm>

namespace libfiles::detail
{
	namespace
	{
		using directory = tiff_editor::directory;

		constexpr std::uint16_t tag_xmp = 700;
		constexpr std::uint16_t tag_iptc = 33723;
		constexpr std::uint16_t tag_photoshop = 34377;
		constexpr std::uint16_t tag_dng_version = 0xC612;
	}

	void plan_tiff(reader& r, const metadata_update& update, write_plan& plan)
	{
		tiff_editor editor;
		if (!editor.open(r)) throw format_error("only a classic TIFF file can be updated");

		if (update.xmp)
		{
			const auto old = editor.value_range(directory::ifd0, tag_xmp);
			const auto packet = serialize_packet(*update.xmp, old && old->size > 4 ? old->size : 0);

			if (old && old->size > 4 && packet.size() == old->size)
			{
				push_changed(r, plan, old->offset, to_bytes(packet));
			}
			else
			{
				editor.set(directory::ifd0, tag_xmp, tiff_editor::type_undefined, static_cast<std::uint32_t>(packet.size()),
				           to_bytes(packet));
			}
		}

		if (touches_exif(update.legacy) || touches_iptc(update.legacy))
		{
			const auto is_dng = editor.has(directory::ifd0, tag_dng_version);

			bytes existing_iptc;
			if (const auto range = editor.value_range(directory::ifd0, tag_iptc))
			{
				existing_iptc = r.read(range->offset, static_cast<std::size_t>(range->size));
			}

			photo_legacy legacy{&editor};
			apply_photo_legacy(update.legacy, existing_iptc, !is_dng, legacy);

			if (legacy.iptc)
			{
				editor.set(directory::ifd0, tag_iptc, tiff_editor::type_undefined, static_cast<std::uint32_t>(legacy.iptc->size()),
				           *legacy.iptc);

				// The digest beside the IPTC tells an Adobe reader the two still agree.
				if (const auto range = editor.value_range(directory::ifd0, tag_photoshop))
				{
					const auto block = r.read(range->offset, static_cast<std::size_t>(range->size));
					std::vector<photoshop_resource> resources;

					if (parse_photoshop_resources(block, resources))
					{
						set_iptc_resources(resources, *legacy.iptc);
						auto updated = serialize_photoshop_resources(resources);
						const auto count = static_cast<std::uint32_t>(updated.size());
						editor.set(directory::ifd0, tag_photoshop, tiff_editor::type_undefined, count, std::move(updated));
					}
				}
			}
		}

		for (auto& c : editor.changes(r.size())) plan.replacements.push_back(std::move(c));
	}

	void plan_psd(reader& r, const metadata_update& update, write_plan& plan)
	{
		if (r.be32(0) != 0x38425053) throw format_error("not a PSD file");

		const auto version = r.be16(4);
		if (version != 1 && version != 2) throw format_error("the PSD version is not one that can be updated");

		std::uint64_t pos = 26;
		pos += 4 + static_cast<std::uint64_t>(r.be32(pos));

		const auto section_start = pos;
		const std::uint64_t section_length = r.be32(pos);
		if (!r.contains(pos + 4, section_length)) throw format_error("the PSD is cut short");

		const auto block = r.read(pos + 4, static_cast<std::size_t>(section_length));
		std::vector<photoshop_resource> resources;
		if (!parse_photoshop_resources(block, resources)) throw format_error("the PSD's image resources cannot be read");

		const auto find = [&resources](const std::uint16_t id) { return find_photoshop_resource(resources, id); };

		std::optional<std::string> packet;

		if (update.xmp)
		{
			const auto* const old = find(0x0424);
			packet = serialize_packet(*update.xmp, old ? old->data.size() : 0);

			// Only the packet changes and it fits: written where it lies.
			if (old && packet->size() == old->data.size() && !touches_exif(update.legacy) && !touches_iptc(update.legacy))
			{
				push_changed(r, plan, section_start + 4 + old->data_offset, to_bytes(*packet));
				return;
			}
		}

		const auto set = [&resources, &find](const std::uint16_t id, bytes data)
		{
			if (auto* const existing = find(id)) existing->data = std::move(data);
			else resources.push_back({id, {}, std::move(data)});
		};

		if (packet) set(0x0424, to_bytes(*packet));

		if (touches_exif(update.legacy))
		{
			const auto* const old = find(0x0422);
			const auto exif = old ? old->data : tiff_editor::empty_stream();

			reader exif_reader(exif);
			tiff_editor editor;
			if (!editor.open(exif_reader)) throw format_error("the PSD's EXIF block cannot be read");

			photo_legacy legacy{&editor};
			apply_photo_legacy(update.legacy, {}, false, legacy);
			set(0x0422, apply_changes(exif, editor.changes(exif.size())));
		}

		if (touches_iptc(update.legacy))
		{
			const auto* const old = find(0x0404);
			const auto existing = old ? old->data : bytes{};

			photo_legacy legacy;
			apply_photo_legacy(update.legacy, existing, true, legacy);
			if (legacy.iptc) set_iptc_resources(resources, *legacy.iptc);
		}

		const auto updated = serialize_photoshop_resources(resources);
		if (updated.size() > 0xFFFFFFFFu) throw format_error("the PSD's image resources would be too large");

		bytes section;
		append_be32(section, static_cast<std::uint32_t>(updated.size()));
		section.insert(section.end(), updated.begin(), updated.end());
		if (section.size() == 4 + section_length) push_changed(r, plan, section_start, section);
		else plan.replacements.push_back({section_start, 4 + section_length, std::move(section)});
	}
}
