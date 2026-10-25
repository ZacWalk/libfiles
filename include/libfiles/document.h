// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Native, indexed and multi-page image decoding into the caller's row stream.

#pragma once

#include "libfiles/scan.h"

#include <optional>

namespace libfiles
{
	enum class document_storage : std::uint8_t
	{
		indexed1, indexed2, indexed4, indexed8, indexed8_alpha, gray8,
		bgr555, bgr565, bgr24, bgrx32, bgra32
	};

	std::uint32_t storage_bits(document_storage format) noexcept;
	std::size_t palette_entries(document_storage format) noexcept;
	std::size_t document_row_bytes(std::uint32_t width, document_storage format);

	struct document_frame
	{
		std::int32_t left = 0;
		std::int32_t top = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		document_storage storage = document_storage::bgr24;
		bool sequential = true;
		std::span<const std::uint32_t> palette;
		std::optional<std::uint32_t> transparent;
		std::optional<std::uint32_t> background;
		std::optional<std::uint32_t> delay_ms;
		std::optional<std::uint32_t> disposal;
	};

	enum class document_metadata : std::uint8_t
	{
		exif, iptc, xmp, icc, source_icc, comment
	};

	struct document_info
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		document_storage storage = document_storage::bgr24;
		std::uint32_t bits_per_pixel = 0;
		bool has_resolution = false;
		std::uint32_t pixels_per_meter_x = 0;
		std::uint32_t pixels_per_meter_y = 0;
		std::uint32_t frames = 0;
		std::uint16_t encoding = 0;
		std::uint16_t color_model = 0;
		std::uint16_t version = 0;
		std::uint8_t interlace_passes = 1;
	};

	class document_sink
	{
	public:
		virtual ~document_sink() = default;
		// Called before pixel allocation. False refuses the frame without decoding it.
		virtual bool begin_frame(const document_frame& frame) = 0;
		// Rows are top-down and four-byte aligned; borrowed data lives only through the callback.
		virtual void row(std::uint32_t y, std::span<const std::uint8_t> bytes) = 0;
		virtual void end_frame(const document_frame&) {}
		virtual void metadata(document_metadata, std::span<const std::uint8_t>) {}
		virtual void metadata_complete() {}
		virtual bool cancelled() { return false; }
		virtual void progress(std::uint32_t, std::uint32_t) {}
	};

	struct document_options
	{
		std::uint32_t thumbnail_width = 0;
		std::uint32_t thumbnail_height = 0;
		bool pixels = true;
		// Scratch rasters/strips are separate from the sink's retained frames and codec bookkeeping.
		std::uint64_t maximum_working_bytes = 256ull * 1024 * 1024;
	};

	struct document_result
	{
		bool success = false;
		bool refused = false;
		bool cancelled = false;
		bool truncated = false;
		bool resource_limit = false;
		std::string error;
		std::vector<std::string> warnings;
		document_info info;
	};

	bool can_decode_document(image_format format) noexcept;
	document_result decode_document(reader& input, image_format format, document_sink& output,
	                                const document_options& options = {});
}
