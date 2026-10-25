// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing XMP to a WebP. The packet is the "XMP " chunk, and the VP8X header must declare
// it. A simple file has no VP8X, so one is added, its canvas and alpha read from the image header -
// the toolkit took a lossless image's canvas from bytes that are not its size. The chunk is replaced
// where it stands or appended at the end of the RIFF, and every other chunk keeps its place and its
// bytes.

#include "legacy.h"

#include <optional>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint32_t RIFF = 0x46464952;
		constexpr std::uint32_t WEBP = 0x50424557;
		constexpr std::uint32_t VP8X = 0x58385056;
		constexpr std::uint32_t VP8_ = 0x20385056;
		constexpr std::uint32_t VP8L = 0x4C385056;
		constexpr std::uint32_t ANMF = 0x464D4E41;
		constexpr std::uint32_t XMP_ = 0x20504D58;
		constexpr std::uint32_t EXIF = 0x46495845;

		constexpr std::uint32_t xmp_flag = 0x04;
		constexpr std::uint32_t alpha_flag = 0x10;

		struct chunk
		{
			std::uint64_t start = 0;
			std::uint32_t length = 0;

			std::uint64_t size() const { return 8ull + length + (length & 1u); }
		};

		void append_le24(std::vector<std::uint8_t>& to, const std::uint32_t v)
		{
			to.push_back(static_cast<std::uint8_t>(v));
			to.push_back(static_cast<std::uint8_t>(v >> 8));
			to.push_back(static_cast<std::uint8_t>(v >> 16));
		}
	}

	void plan_webp(reader& r, const metadata_update& update, write_plan& plan)
	{
		if (!update.xmp) return;

		if (r.le32(0) != RIFF || r.le32(8) != WEBP) throw format_error("not a WebP file");

		const std::uint64_t riff_size = r.le32(4);
		if (riff_size < 4 || riff_size > r.size() - 8) throw format_error("the WebP is cut short");

		const auto riff_end = riff_size + 8;
		std::optional<chunk> vp8x;
		std::optional<chunk> xmp;
		std::optional<chunk> exif;
		auto has_image = false;
		std::uint64_t pos = 12;

		while (pos < riff_end)
		{
			if (riff_end - pos < 8) throw format_error("the WebP is cut short");

			const chunk c{pos, r.le32(pos + 4)};
			const auto tag = r.le32(pos);

			if (c.size() > riff_end - pos) throw format_error("a WebP chunk runs past the end of the file");

			if (tag == VP8X && !vp8x) vp8x = c;
			else if (tag == XMP_ && !xmp) xmp = c;
			else if (tag == EXIF && !exif) exif = c;
			else if (tag == VP8_ || tag == VP8L || tag == ANMF) has_image = true;

			pos += c.size();
		}

		if (!has_image) throw format_error("the WebP holds no image");

		const auto packet = serialize_packet(*update.xmp, xmp ? xmp->length : 0);
		if (packet.size() > 0xFFFFFFF0u) throw format_error("the XMP packet is too large for a WebP chunk");

		std::vector<std::uint8_t> xmp_chunk;
		append_le32(xmp_chunk, XMP_);
		append_le32(xmp_chunk, static_cast<std::uint32_t>(packet.size()));
		append(xmp_chunk, packet);
		if (packet.size() & 1u) xmp_chunk.push_back(0);

		std::vector<replacement> changes;

		if (vp8x)
		{
			const auto flags = r.le32(vp8x->start + 8);

			if ((flags & xmp_flag) == 0)
			{
				// After the RIFF size that takes in a chunk appended at the end, so no file announces a
				// packet it does not yet hold.
				std::vector<std::uint8_t> bytes;
				append_le32(bytes, flags | xmp_flag);
				changes.push_back({vp8x->start + 8, 4, std::move(bytes), 1});
			}
		}
		else
		{
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			auto has_alpha = false;

			if (!webp_canvas(r, width, height, has_alpha)) throw format_error("the WebP image header cannot be read");

			std::vector<std::uint8_t> bytes;
			append_le32(bytes, VP8X);
			append_le32(bytes, 10);
			append_le32(bytes, xmp_flag | (has_alpha ? alpha_flag : 0u));
			append_le24(bytes, width - 1);
			append_le24(bytes, height - 1);
			changes.push_back({12, 0, std::move(bytes)});
		}

		if (xmp) changes.emplace_back(xmp->start, xmp->size(), std::move(xmp_chunk));
		else changes.emplace_back(riff_end, 0, std::move(xmp_chunk));

		// The EXIF a WebP already holds takes the edit too, as the toolkit wrote it; a WebP without one
		// does not gain one. Some writers keep the JPEG signature at its start, and it is kept.
		if (exif && touches_exif(update.legacy))
		{
			const auto payload = r.read(exif->start + 8, exif->length);
			const auto prefix = starts_with(payload, {"Exif\0\0", 6}) ? std::size_t{6} : std::size_t{0};
			const bytes block(payload.begin() + static_cast<std::ptrdiff_t>(prefix), payload.end());

			reader block_reader(block);
			tiff_editor editor;

			if (editor.open(block_reader))
			{
				photo_legacy legacy{&editor};
				apply_photo_legacy(update.legacy, {}, false, legacy);

				bytes data(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(prefix));
				const auto edited = apply_changes(block, editor.changes(block.size()));
				data.insert(data.end(), edited.begin(), edited.end());

				std::vector<std::uint8_t> exif_chunk;
				append_le32(exif_chunk, EXIF);
				append_le32(exif_chunk, static_cast<std::uint32_t>(data.size()));
				exif_chunk.insert(exif_chunk.end(), data.begin(), data.end());
				if (data.size() & 1u) exif_chunk.push_back(0);
				changes.push_back({exif->start, exif->size(), std::move(exif_chunk)});
			}
		}

		auto new_riff_size = riff_size;
		for (const auto& c : changes) new_riff_size = new_riff_size - c.length + c.data.size();

		if (new_riff_size > 0xFFFFFFFEu) throw format_error("the WebP would be too large");

		std::vector<std::uint8_t> size_field;
		append_le32(size_field, static_cast<std::uint32_t>(new_riff_size));
		push_changed(r, plan, 4, size_field);

		for (auto& c : changes)
		{
			if (c.length != 0 && c.length == c.data.size()) push_changed(r, plan, c.offset, c.data, c.step);
			else plan.replacements.push_back(std::move(c));
		}
	}
}
