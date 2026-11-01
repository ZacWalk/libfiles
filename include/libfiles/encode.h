// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Encoding an image its caller holds in memory as a JPEG, a PNG, a WebP or a JPEG XL, with the
// metadata it carries, and turning a JPEG losslessly - what works on its coefficients rather than its
// pixels. The JPEG encoder and the lossless turn need libfiles built with libjpeg-turbo, the PNG encoder
// with libpng, the WebP encoder with libwebp and the JPEG XL encoder with libjxl.

#pragma once

#include "libfiles/decode.h"
#include "libfiles/document.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

	// What an encoder embeds beside the pixels: views of the blocks, which must outlive the encode. Any can
	// be empty.
	struct embedded_metadata
	{
		// A TIFF stream, with or without the "Exif\0\0" signature a JPEG's APP1 begins with.
		std::span<const std::uint8_t> exif;
		std::span<const std::uint8_t> xmp;
		// IPTC-IIM records, which a JPEG carries among its Photoshop resources.
		std::span<const std::uint8_t> iptc;
		std::span<const std::uint8_t> icc;
		// Photoshop image resources other than the IPTC one, as 8BIM records.
		std::span<const std::uint8_t> photoshop;
		std::uint32_t pixels_per_meter_x = 0;
		std::uint32_t pixels_per_meter_y = 0;

		embedded_metadata() = default;

		// The blocks a scan read, or a caller assembled, embedded as they are.
		embedded_metadata(const metadata_blocks& blocks) noexcept :
			exif(blocks.exif), xmp(blocks.xmp), iptc(blocks.iptc), icc(blocks.icc), photoshop(blocks.photoshop)
		{
		}
	};

	// The native document encoders report these separately so an application can choose a different
	// format for an unsupported storage layout, retry after a larger budget, or keep a cancelled
	// operation out of its publication path.
	struct document_encode_result
	{
		bool success = false;
		bool unsupported = false;
		bool cancelled = false;
		bool resource_limit = false;
		std::string error;
		std::vector<std::string> warnings;
		std::vector<std::uint8_t> data;

		explicit operator bool() const noexcept { return success; }
	};

	struct document_encode_options
	{
		int quality = 85;
		bool progressive = false;
		bool optimize = false;
		// Selects historical native-writer conventions, including TIFF inch resolution and metadata
		// layout. reset_orientation_metadata opts TIFF into standard EXIF carrying for upright saves.
		// It never enables a lossy pixel conversion.
		bool legacy_compatibility = false;
		// The caller has physically made pixels upright; correct existing EXIF/XMP orientation and
		// dimensions and unlink stale thumbnails instead of carrying the old turn into the saved file.
		bool reset_orientation_metadata = false;
		// Used by JPEG when no document_info resolution is present, and by the row overload.
		std::uint32_t pixels_per_meter_x = 0;
		std::uint32_t pixels_per_meter_y = 0;
		std::uint64_t maximum_working_bytes = 256ull * 1024 * 1024;
		std::function<bool()> cancelled;
		std::function<void(std::uint32_t, std::uint32_t)> progress;
	};

	// The native formats have no lossy colour conversion fallback: indexed and packed storage stays
	// indexed or packed where the target format can represent it, and an unsupported layout is refused.
	bool can_encode_document(image_format format) noexcept;

	// Encodes a complete native document. The document owns its rows and metadata for the duration of
	// the call. BMP, GIF, TIFF and PNG retain native indexed/packed rows where their file format permits
	// them; JPEG accepts one bgr24/bgrx32/bgra32/gray8 frame and is inherently lossy.
	document_encode_result encode_document(const decoded_document& document, image_format format,
	                                        const document_encode_options& options = {});

	// Row-oriented convenience form for one image_view. It is intentionally separate from the document
	// form so callers cannot accidentally flatten a multi-page or indexed document into BGRA pixels.
	document_encode_result encode_document(const image_view& image, image_format format,
	                                        const document_info& info = {},
	                                        const embedded_metadata& metadata = {},
	                                        const document_encode_options& options = {});

	// Writes only after a complete in-memory encode succeeds. Encoding or cancellation therefore never
	// touches the destination; a destination write failure follows the writable_stream's own staging
	// and rollback policy.
	document_encode_result encode_document(writable_stream& destination, const decoded_document& document,
	                                        image_format format, const document_encode_options& options = {});

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
		// A grayscale row passed to jpeg_encoder::write_rows has one byte per pixel when this is true.
		bool grayscale = false;
		// A BGR24 row passed to write_rows has three bytes per pixel. This keeps the native JPEG
		// path's source colour conversion distinct from the four-byte display-row encoder.
		bool bgr24 = false;
		bool progressive = false;
		bool optimize = false;
		// Native legacy writers explicitly emit an inch-based JFIF density. The generic encoder
		// leaves libjpeg's default density fields untouched unless this is requested.
		bool legacy_density = false;
		std::uint32_t pixels_per_meter_x = 0;
		std::uint32_t pixels_per_meter_y = 0;
		// The JPEG an edit is being written back over, when there is one. Encoding against its own
		// quantization tables and chroma sampling leaves an untouched block quantizing to itself, where
		// the quality's tables would re-quantize the whole image. Adopted from a plain 8-bit YCbCr JPEG
		// only; anything else, or a source that cannot be read, falls back to the quality.
		std::span<const std::uint8_t> source;
	};

	// Encodes rows of four-byte pixels - blue, green, red and a fourth byte it ignores - as a JPEG,
	// or rows of three-byte BGR/one-byte grayscale when the corresponding option is set. Long lived
	// and reused for every image, because creating libjpeg's compressor is not free; each use resets
	// it first, so one that a throw abandoned does not fail the next.
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
		// A result with no data is otherwise distinguishable by these flags: the source was not a
		// transformable JPEG, the request was cancelled, or libjpeg/transupp rejected it.
		bool unsupported = false;
		bool cancelled = false;
		bool resource_limit = false;
		bool invalid = false;
		std::string error;
	};

	enum class jpeg_transform : std::uint8_t
	{
		none,
		rotate90,
		rotate180,
		rotate270,
		flip_horizontal,
		flip_vertical,
		transpose,
		transverse,
	};

	enum class jpeg_transform_policy : std::uint8_t
	{
		// Refuse a turn when an edge is a partial iMCU. This is the exact transform used by Diffractor.
		perfect,
		// Keep transupp's partial edge blocks, matching ImageWalker's historical native path. No trim
		// is enabled; pixels are never silently discarded.
		partial_blocks,
	};

	enum class jpeg_density_unit : std::uint8_t
	{
		aspect_ratio,
		inches,
		centimetres
	};

	struct jpeg_density
	{
		jpeg_density_unit unit = jpeg_density_unit::aspect_ratio;
		std::uint16_t x = 1;
		std::uint16_t y = 1;
	};

	struct jpeg_transform_options
	{
		jpeg_transform transform = jpeg_transform::none;
		jpeg_transform_policy policy = jpeg_transform_policy::perfect;
		// Explicitly permits transupp to discard partial edge iMCUs. False never trims; a
		// perfect request remains distinguishable as unsupported instead.
		bool trim = false;
		bool force_grayscale = false;
		bool optimize_coding = false;
		bool progressive = false;
		// Unset preserves the source JFIF density. An override is in the output's axis order.
		std::optional<jpeg_density> density;
		std::uint64_t maximum_bytes = 256ull * 1024 * 1024;
		std::function<bool()> cancelled;
		std::function<void(std::uint32_t, std::uint32_t)> progress;
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

		// An explicit coefficient-only transform. It never recompresses; trimming requires trim=true.
		// A failed transform
		// reports error; an unsupported partial-MCU request, an over-budget workspace and cancellation
		// have their own flags.
		lossless_turn transform(std::span<const std::uint8_t> jpeg, jpeg_encoder& encoder,
		                       const jpeg_transform_options& options = {}) const;

		struct state;

	private:
		std::unique_ptr<state> _state;
	};

	// A re-encoded JPEG with the descriptive segments of the JPEG it was made from in place of its own:
	// EXIF, XMP, IPTC, comments, vendor segments and an RGB ICC profile, copied byte for byte but for what
	// re-encoding invalidated - the EXIF and XMP orientation becomes 1, for the new pixels are upright, the
	// EXIF pixel dimensions become width by height, and the EXIF and JFIF thumbnails are dropped. Segments
	// describing the old encoding - Adobe's colour transform, the tables - or pointing into the old file - a
	// multi-picture index - are dropped, and so is an EXIF block too malformed to correct. source need hold
	// only its header, as read_jpeg_header gives it. Empty when either is not a well-formed JPEG. Needs no
	// codec.
	std::vector<std::uint8_t> carry_jpeg_metadata(std::span<const std::uint8_t> source,
	                                              std::span<const std::uint8_t> encoded, int width, int height);

	// SOI and the marker segments before a JPEG's first scan, read no further. Empty when the stream is not
	// a JPEG, or its header is malformed or larger than a photo's (32 MiB).
	std::vector<std::uint8_t> read_jpeg_header(reader& input);

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
		// Keeps the colour under fully transparent pixels, which libwebp otherwise may change to compress
		// better.
		bool exact = false;
		// libwebp's effort, 0 to 6, with lossless at the quality given; or -1 for libfiles' own: 6, or 2
		// when fast, and lossless from libwebp's preset.
		int method = -1;
	};

	// Encodes bgrx8 or bgra8, or nv12 lossily when its sides are even, as a WebP, with the ICC profile,
	// the EXIF - or one holding only the orientation when there is none and the orientation is not 1 -
	// and the XMP. data is empty when it cannot, and the notes say why.
	encoded encode_webp(const image_view& image, std::uint16_t orientation, const embedded_metadata& metadata,
	                    const webp_options& options);

	struct jxl_options
	{
		// Bit for bit, alpha and all. Otherwise only the colour is lossy; alpha stays lossless.
		bool lossless = false;
		// 1 to 100, read as libjpeg reads its quality and mapped to libjxl's distance: 90 is libjxl's
		// default, visually lossless.
		int quality = 90;
		// libjxl's effort, 1 the fastest to 9 the smallest; 7 is its default.
		int effort = 7;
	};

	// Encodes bgrx8 as RGB and bgra8 as RGBA - straight alpha - as a JPEG XL. The orientation goes in the
	// codestream, which a JPEG XL reader takes it from, and an EXIF's orientation is brought into line
	// with it. The ICC profile describes the pixels; the EXIF and the XMP go in boxes of their own,
	// uncompressed, so a reader without Brotli finds them. IPTC and Photoshop resources have no place in
	// the format and are left out. data is empty when it cannot, and the notes say why.
	encoded encode_jxl(const image_view& image, std::uint16_t orientation, const embedded_metadata& metadata,
	                   const jxl_options& options);

	namespace test_hooks
	{
		// The next WebP encode that inserts this chunk - "ICCP", "EXIF" or "XMP " - fails to, as a mux
		// out of memory would. Once; then cleared.
		void fail_next_webp_chunk(std::string_view fourcc);
	}
}
