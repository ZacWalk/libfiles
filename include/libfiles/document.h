// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Native, indexed and multi-page image decoding into the caller's row stream, or whole into
// memory; and any document's rows as BGRA.

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

		friend bool operator==(const document_info&, const document_info&) = default;
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

	// ---- Pixels as BGRA, and a document held whole -----------------------------------------------------

	// Rows of pixels in a document's storage, borrowed: top-down, as a decoder gives them, or bottom-up, as a
	// Windows bitmap holds them. Each row is four-byte aligned.
	struct pixel_rows
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		document_storage storage = document_storage::bgr24;
		std::size_t stride = 0;
		bool bottom_up = false;
		std::span<const std::uint8_t> pixels;
		// BGRA, which indexed storage indexes.
		std::span<const std::uint32_t> palette;
		// The index indexed storage draws as transparent.
		std::optional<std::uint32_t> transparent;
	};

	// The rows as BGRA pixels, stride pixels apart, top-down unless bottom_up: indexed storage through its
	// palette, the transparent index's alpha cleared; and storage without alpha opaque. Throws
	// std::invalid_argument for rows that do not hold what they describe, an index past the palette, or a
	// destination too small for them or overlapping them.
	void copy_bgra(const pixel_rows& source, std::span<std::uint32_t> destination, std::size_t stride, bool bottom_up);

	// A frame decoded whole: as document_frame describes it, with its rows - top-down and four-byte aligned -
	// and its palette, owned.
	struct decoded_frame
	{
		std::int32_t left = 0;
		std::int32_t top = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		document_storage storage = document_storage::bgr24;
		std::size_t stride = 0;
		std::vector<std::uint8_t> pixels;
		// BGRA.
		std::vector<std::uint32_t> palette;
		std::optional<std::uint32_t> transparent;
		std::optional<std::uint32_t> background;
		std::optional<std::uint32_t> delay_ms;
		std::optional<std::uint32_t> disposal;

		pixel_rows rows() const noexcept;

		friend bool operator==(const decoded_frame&, const decoded_frame&) = default;
	};

	struct decoded_metadata
	{
		document_metadata kind = document_metadata::exif;
		std::vector<std::uint8_t> bytes;

		friend bool operator==(const decoded_metadata&, const decoded_metadata&) = default;
	};

	// A document decoded whole - its frames and its metadata, owned - so that a copy stands apart from it.
	struct decoded_document
	{
		document_info info;
		std::vector<decoded_frame> frames;
		std::vector<decoded_metadata> metadata;

		friend bool operator==(const decoded_document&, const decoded_document&) = default;
	};

	// A sink that keeps every frame and every block of metadata it is given, whole.
	class document_keeper final : public document_sink
	{
	public:
		decoded_document document;

		bool begin_frame(const document_frame& frame) override;
		void row(std::uint32_t y, std::span<const std::uint8_t> bytes) override;
		void metadata(document_metadata kind, std::span<const std::uint8_t> bytes) override;
	};

	// The document decoded whole into memory, as a document_keeper keeps it, its info with it.
	document_result decode_document(reader& input, image_format format, decoded_document& output,
	                                const document_options& options = {});
}
