// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What the format writers share and do not export: each format's planner and the packet
// serialization they all use.

#pragma once

#include "libfiles/write.h"
#include "scan_internal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	// The packet as the toolkit's PutXMP wrote it: padded to the old packet's exact length when it
	// fits there, so the file can be patched where it lies; else compact with the default padding.
	std::string serialize_packet(const xmp::meta& meta, std::uint64_t old_length);

	inline void append(std::vector<std::uint8_t>& to, const std::string_view bytes)
	{
		to.insert(to.end(), bytes.begin(), bytes.end());
	}

	inline void append_be32(std::vector<std::uint8_t>& to, const std::uint32_t v)
	{
		to.push_back(static_cast<std::uint8_t>(v >> 24));
		to.push_back(static_cast<std::uint8_t>(v >> 16));
		to.push_back(static_cast<std::uint8_t>(v >> 8));
		to.push_back(static_cast<std::uint8_t>(v));
	}

	inline void append_le32(std::vector<std::uint8_t>& to, const std::uint32_t v)
	{
		to.push_back(static_cast<std::uint8_t>(v));
		to.push_back(static_cast<std::uint8_t>(v >> 8));
		to.push_back(static_cast<std::uint8_t>(v >> 16));
		to.push_back(static_cast<std::uint8_t>(v >> 24));
	}

	inline std::vector<std::uint8_t> to_bytes(const std::string_view text)
	{
		return {text.begin(), text.end()};
	}

	void plan_png(reader& r, const metadata_update& update, write_plan& plan);
	void plan_gif(reader& r, const metadata_update& update, write_plan& plan);
	void plan_webp(reader& r, const metadata_update& update, write_plan& plan);
	void plan_jpeg(reader& r, const metadata_update& update, write_plan& plan);
	void plan_tiff(reader& r, const metadata_update& update, write_plan& plan);
	void plan_psd(reader& r, const metadata_update& update, write_plan& plan);
	void plan_mp4(reader& r, const metadata_update& update, write_plan& plan);
	void plan_asf(reader& r, const metadata_update& update, write_plan& plan);
	void plan_riff(reader& r, const metadata_update& update, write_plan& plan);
	void plan_mp3(reader& r, const metadata_update& update, write_plan& plan);
	void plan_flv(reader& r, const metadata_update& update, write_plan& plan);

	// True when the first bytes are an ISO base media or QuickTime file's first box.
	bool is_iso_media(std::span<const std::uint8_t> head) noexcept;
	void read_mp4_packet(reader& r, packet_reading& out);

	// True when the first bytes are an ASF header object.
	bool is_asf(std::span<const std::uint8_t> head) noexcept;
	void read_asf_packet(reader& r, packet_reading& out);

	// True when the first bytes begin a RIFF WAVE or AVI form.
	bool is_riff_media(std::span<const std::uint8_t> head) noexcept;
	void read_riff_packet(reader& r, packet_reading& out);

	// True when the first bytes are an ID3v2 tag or an MPEG audio frame.
	bool is_mpeg_audio(std::span<const std::uint8_t> head) noexcept;
	void read_mp3_packet(reader& r, packet_reading& out);

	bool is_flv(std::span<const std::uint8_t> head) noexcept;
	void read_flv_packet(reader& r, packet_reading& out);

	// Same-length replacements for the bytes of updated that differ from original, which starts at
	// offset in the file. Runs closer than a few bytes are joined.
	std::vector<replacement> diff_patches(std::uint64_t offset, std::span<const std::uint8_t> original,
	                                      std::span<const std::uint8_t> updated);

	// One replacement from the first byte that differs to the last, for a structure whose parts may
	// have moved, so it is written at once rather than as patches that each depend on the others.
	std::optional<replacement> changed_span(std::uint64_t offset, std::span<const std::uint8_t> original,
	                                        std::span<const std::uint8_t> updated, std::uint8_t step);

	// A same-length rewrite of what lies at offset, as the one span of it that changes, or nothing when
	// nothing does. Patched where the file lies, an edit then writes only what it changed: a rating in a
	// padded packet is a byte or two, not the packet.
	void push_changed(reader& r, write_plan& plan, std::uint64_t offset, std::span<const std::uint8_t> updated,
	                  std::uint8_t step = 0);
}
