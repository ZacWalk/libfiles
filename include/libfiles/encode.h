// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Encoding an image its caller holds in memory as a JPEG, a PNG or a WebP, with the metadata
// it carries, and turning a JPEG losslessly - what works on its coefficients rather than its pixels.
// The JPEG encoder and the lossless turn need libfiles built with libjpeg-turbo, the PNG encoder with
// libpng and the WebP encoder with libwebp.

#pragma once

#include "libfiles/decode.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles
{
	// Why an image was not encoded: the codec failed, or a metadata block will not fit the format.
	class encode_error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// The pixels an encoder reads: height rows of width pixels, stride bytes apart, as decode() writes
	// them.
	struct image_view
	{
		const std::uint8_t* pixels = nullptr;
		std::size_t stride = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		pixel_format format = pixel_format::bgrx8;
		// For nv12: limited-range BT.601 rather than full range.
		bool limited_range = false;
	};

	// What an encoder embeds beside the pixels. Any can be empty.
	struct embedded_metadata
	{
		// A TIFF stream, with or without the "Exif\0\0" signature a JPEG's APP1 begins with.
		std::span<const std::uint8_t> exif;
		std::span<const std::uint8_t> xmp;
		// IPTC-IIM records, which a JPEG carries among its Photoshop resources.
		std::span<const std::uint8_t> iptc;
		std::span<const std::uint8_t> icc;
		// Photoshop image resources other than the IPTC one, as 8BIM records.
		std::span<const std::uint8_t> photoshop_resources;
	};

	// An image encoded, with what the encoder noted on the way.
	struct encoded
	{
		// Empty when the image could not be encoded.
		std::vector<std::uint8_t> data;
		// For a log, in order: libpng's warnings, why libwebp refused, a chunk that would not go in.
		std::vector<std::string> notes;
	};

	struct jpeg_options
	{
		int quality = 85;
		// The JPEG an edit is being written back over, when there is one. Encoding against its own
		// quantization tables and chroma sampling leaves an untouched block quantizing to itself, where
		// the quality's tables would re-quantize the whole image. Adopted from a plain 8-bit YCbCr JPEG
		// only; anything else, or a source that cannot be read, falls back to the quality.
		std::span<const std::uint8_t> source;
	};

	// Encodes rows of four-byte pixels - blue, green, red and a fourth byte it ignores - as a JPEG. Long
	// lived and reused for every image, because creating libjpeg's compressor is not free; each use
	// resets it first, so one that a throw abandoned does not fail the next.
	class jpeg_encoder
	{
	public:
		jpeg_encoder();
		~jpeg_encoder();
		jpeg_encoder(const jpeg_encoder&) = delete;
		jpeg_encoder& operator=(const jpeg_encoder&) = delete;

		// Begins an image and writes its markers: the EXIF - or, when there is none, an EXIF holding
		// only the orientation, unless that is 0 or 1 - the IPTC and Photoshop resources, the XMP and
		// the ICC profile. Every block is checked against the JPEG's limits first, so a refused image
		// leaves the encoder ready for the next. Throws encode_error.
		void start(std::uint32_t width, std::uint32_t height, std::uint16_t orientation,
		           const embedded_metadata& metadata, const jpeg_options& options);
		// Takes up to count rows; libjpeg may take fewer, and next_row says where to resume.
		void write_rows(std::uint8_t** rows, std::uint32_t count) const;
		std::uint32_t next_row() const noexcept;
		// Ends the image and hands back its bytes. With can_abort, an image not every row of which was
		// written is abandoned rather than finished.
		std::vector<std::uint8_t> complete(bool can_abort = true);
		// start, every row, complete. The orientation is the EXIF value, 1 to 8.
		std::vector<std::uint8_t> encode(std::uint32_t width, std::uint32_t height, const std::uint8_t* pixels,
		                                 std::size_t stride, std::uint16_t orientation,
		                                 const embedded_metadata& metadata, const jpeg_options& options);

		// The size the image started last is written at.
		std::uint32_t width() const noexcept;
		std::uint32_t height() const noexcept;
		// What the last image noted, for a log: a source whose tables could not be adopted, a Photoshop
		// resource too large for any marker. Cleared when the next starts.
		const std::vector<std::string>& notes() const noexcept;

		struct state;

	private:
		std::unique_ptr<state> _state;

		void setup(std::uint32_t width, std::uint32_t height, const jpeg_options& options);

		friend class jpeg_transformer;
	};

	// A JPEG turned losslessly.
	struct lossless_turn
	{
		// Empty when the turn cannot be lossless - it would land on a partial MCU - when the stream
		// cannot be read, or when it is over the budget.
		std::vector<std::uint8_t> data;
		// The turn could have been lossless, but the coefficients it holds - workspace_bytes - would not
		// fit the budget: a refusal a caller must not answer with a re-encode, which holds more still.
		bool over_budget = false;
		// The source's size once its header was read, and what turning it holds, for a caller's log.
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::int64_t workspace_bytes = 0;
	};

	// A JPEG's header, and its lossless turns. Long lived and reused for every image, and reset before
	// and after each use.
	class jpeg_transformer
	{
	public:
		jpeg_transformer();
		~jpeg_transformer();
		jpeg_transformer(const jpeg_transformer&) = delete;
		jpeg_transformer& operator=(const jpeg_transformer&) = delete;

		bool read_header(std::span<const std::uint8_t> jpeg);
		// The size of the stream whose header was read last.
		std::uint32_t width() const noexcept;
		std::uint32_t height() const noexcept;
		// Bytes libjpeg holds in one whole-image DCT coefficient array for the stream whose header was
		// read last - the full resolution always, which a lossless turn holds twice. Zero for a lossless
		// stream, whose data units are single samples that this accounting does not describe.
		std::int64_t coefficient_bytes() const;

		// Turns the JPEG as the EXIF orientation 1 to 8 asks - 6 a quarter turn clockwise - with
		// encoder writing the result, every marker kept and the EXIF's sizes and orientation brought up
		// to date. Anything but 2 to 8 is no turn. The coefficients it holds are judged against budget,
		// in bytes. Throws encode_error when libjpeg fails part way.
		lossless_turn turn(std::span<const std::uint8_t> jpeg, jpeg_encoder& encoder, std::uint16_t orientation,
		                   std::uint64_t budget) const;

		struct state;

	private:
		std::unique_ptr<state> _state;
	};

	// Encodes bgrx8 as an RGB PNG and anything else as RGBA - straight alpha, bgra8 - with the ICC
	// profile, the XMP and the EXIF, or an EXIF holding only the orientation when there is none and the
	// orientation is not 1. Throws encode_error.
	encoded encode_png(const image_view& image, std::uint16_t orientation, const embedded_metadata& metadata);

	struct webp_options
	{
		bool lossless = false;
		int quality = 70;
		// The alpha plane at the colour's quality rather than lossless.
		bool lossy_alpha = false;
		// Trades a few percent of size for a several-times cheaper encode, for output nothing reads back.
		bool fast = false;
	};

	// Encodes bgrx8 or bgra8, or nv12 lossily when its sides are even, as a WebP, with the ICC profile,
	// the EXIF - or one holding only the orientation when there is none and the orientation is not 1 -
	// and the XMP. data is empty when it cannot, and the notes say why.
	encoded encode_webp(const image_view& image, std::uint16_t orientation, const embedded_metadata& metadata,
	                    const webp_options& options);

	namespace test_hooks
	{
		// The next WebP encode that inserts this chunk - "ICCP", "EXIF" or "XMP " - fails to, as a mux
		// out of memory would. Once; then cleared.
		void fail_next_webp_chunk(std::string_view fourcc);
	}
}
