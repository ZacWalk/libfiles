// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Decoding the image a file holds into memory its caller provides. The caller allocates, so a
// decode lands in the caller's own surface - a texture's staging memory, a bitmap - with nothing copied
// between, and the caller decides what it can afford before the decoder reads the pixels.

#pragma once

#include "libfiles/reader.h"
#include "libfiles/scan.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles
{
	// How a decoded image lies in memory.
	enum class pixel_format : std::uint8_t
	{
		// Four bytes a pixel: blue, green, red, then a byte that carries no colour and that a reader
		// ignores - Direct3D's B8G8R8X8 and a Windows 32-bit DIB.
		bgrx8,
		// Planar 4:2:0 YCbCr, BT.601: height rows of width luma samples, then height / 2 rows of
		// width / 2 interleaved Cb, Cr pairs, all stride bytes apart, the chroma following the last luma
		// row. Full range as JFIF defines it, unless frame_info::limited_range says otherwise. Width
		// and height are even. Direct3D's NV12.
		nv12,
		// Four bytes a pixel: blue, green, red and alpha, the alpha straight rather than premultiplied -
		// Direct3D's B8G8R8A8 as a straight-alpha texture holds it.
		bgra8,
	};

	// Memory for a decoded image: rows of pixels, stride bytes apart. For nv12, the chroma rows follow
	// the luma rows at the same stride.
	struct pixel_buffer
	{
		std::uint8_t* pixels = nullptr;
		std::size_t stride = 0;
	};

	// What a decode will produce, told to the sink before any pixel is read.
	struct frame_info
	{
		// The image as the file stores it. A HEIF's as its item declares it, which a malformed file may
		// make 0 by 0, or other than the size its codec decodes.
		std::uint32_t source_width = 0;
		std::uint32_t source_height = 0;
		// What will be written: a JPEG reduced by scale_denominator, rounded up; a WebP at the size
		// scaled_size_for gives; a HEIF at the size its codec decodes; and nv12 rounded down to even.
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		pixel_format format = pixel_format::bgrx8;
		// nv12 only: the samples use BT.601's limited range, 16 to 235 for luma, as VP8 codes them,
		// rather than JFIF's full range.
		bool limited_range = false;
		// JPEG's reduction: 1, 2, 4 or 8. 1 for the other formats.
		std::uint32_t scale_denominator = 1;
		// Memory the decoder holds besides the caller's while it decodes, where it is large enough to
		// matter: the coefficients of a progressive or non-interleaved JPEG, which it must hold whole,
		// at full size, before it can produce a row.
		std::uint64_t working_bytes = 0;
		// The orientation the file's EXIF gives the image, as summarize_exif reads it - 1 to 8 as stored,
		// unless the file is damaged - or 1 when it gives none. For a PNG, the last EXIF in the file,
		// which may follow the image data; for a WebP, its first EXIF chunk, found as libwebp's mux finds
		// it. A JPEG XL's is its codestream's own, 1 to 8, whatever an Exif box says, and the pixels are
		// left unturned by it.
		std::uint16_t orientation = 1;
		// The TIFF stream of the EXIF block that orientation comes from, for a caller that reads more
		// of it, or reads it its own way; empty when there is none. It lies in the decoder's memory, so
		// it is there only while allocate runs, and always empty in decode_result::frame.
		std::span<const std::uint8_t> exif;
		// HEIF: libheif turns the image by its item's own rotation or mirror ('irot', 'imir') as it
		// decodes, so the orientation is 1, and an EXIF orientation that describes the same turn must
		// not be applied again. A thumbnail item carries its own, or none.
		bool transformed = false;
		// HEIF: the libheif decoder plugin the image is decoded with, by its id, where the caller named
		// one with heif_hevc_decoder; empty where libheif chooses.
		std::string heif_decoder;
	};

	// Where a decoder puts the image. allocate is asked once, as soon as the decoder knows the size it
	// will write and before it writes a pixel - for every format but HEIF and camera raw, as soon as the
	// file has given the size, before the pixels are read. The decoder writes every byte of each row's
	// pixels and nothing past them, so a row's padding is the caller's.
	class image_sink
	{
	public:
		virtual ~image_sink() = default;

		// Asked first, before anything is decoded, by a decoder that learns the size it will write only
		// by decoding - HEIF's, whose codec rather than its container sizes the image, and a raw file's
		// development, which LibRaw does whole in its own memory - with the frame as the file declares it:
		// false refuses the image before the decode. allocate follows with the size decoded. The other
		// decoders know the size before they decode, and ask allocate alone.
		virtual bool accept(const frame_info&) { return true; }

		// Memory for the frame, or no pixels to refuse the image.
		virtual pixel_buffer allocate(const frame_info& frame) = 0;

		// Asked between rows: true stops the decode, which then reports cancelled. A WebP, decoded in
		// one call, does not ask.
		virtual bool cancelled() { return false; }
	};

	struct decode_options
	{
		// The size the caller will show the image at. A decoder that can reduce while it decodes does:
		// JPEG's scaled transform by the largest of 8, 4 and 2 that leaves the source at least this
		// large in both dimensions, counting in whole pixels; WebP's scaler to the size within it that
		// scaled_size_for gives. 0 by 0 decodes at full size.
		std::uint32_t target_width = 0;
		std::uint32_t target_height = 0;
		// The caller takes nv12 where the file holds YCbCr that can be written as nv12 without
		// conversion, and the frame is at least 2 by 2: a JPEG's YCbCr, and an opaque, still, lossy
		// WebP that is not being scaled.
		bool allow_nv12 = false;
		// Interpolates subsampled chroma rather than replicating it: slower, and smoother on saturated
		// edges. nv12 leaves chroma to whoever samples it.
		bool smooth_chroma = true;
		// HEIF: the libheif decoder plugin, by its id, that an HEVC image - an hvc1 item, or a grid,
		// overlay or identity image of them - decodes with, when one of that id is registered. A named
		// decoder takes whatever codec an image holds, so only HEVC images are named to it. Diffractor
		// names FFmpeg's, which it registers. Empty, or not registered, leaves libheif's choice.
		std::string_view heif_hevc_decoder;
		// HEIF: decodes the primary image's first thumbnail item rather than the primary image. It is a
		// coded image of its own, which only a decoder reads.
		bool heif_thumbnail = false;
		// Unknown detects the container signature. A known format also accepts that codec's bare
		// bitstream, such as VP8/VP8L without a WebP RIFF wrapper.
		image_format source_format = image_format::unknown;
		// WebP thumbnails may require rounding down into their bounds rather than the default
		// nearest-pixel rule. Neither policy enlarges an image.
		bool round_webp_size_down = false;
	};

	struct decode_result
	{
		// The image was written. A JPEG cut short still is, as far as its data reached, the rest as
		// its codec fills missing data, and a HEIF cut short within its image data is, as libheif
		// decodes it; a PSD, PNG, WebP or JPEG XL cut short is not.
		bool success = false;
		// The sink refused the image, in accept or allocate. error is empty then.
		bool refused = false;
		// The sink asked to stop. error is empty then.
		bool cancelled = false;
		// The file ended before the image did.
		bool truncated = false;
		// Why the image was not decoded: the file is malformed or cut short, or it holds an image of a
		// kind libfiles does not decode.
		std::string error;
		// What was written, as allocate was told it - or for a HEIF or a raw development that never
		// reached allocate, what accept was told.
		frame_info frame;
		// What the codec warned of while it decoded, in order, whether or not the decode then failed:
		// a chunk whose CRC is wrong and is skipped, say. For a log; libpng's alone, so far.
		std::vector<std::string> warnings;
	};

	// True when decode() reads images of this format: a PSD's composite image, a JPEG's when libfiles
	// is built with libjpeg-turbo, a PNG's when it is built with libpng, a still WebP's when it is
	// built with libwebp, a HEIF's or AVIF's when it is built with libheif, and a JPEG XL's first frame
	// when it is built with libjxl.
	bool can_decode(image_format format) noexcept;

	// Decodes one image after another, keeping what the codecs need between them - libjpeg's
	// decompressor - so a stream of thumbnails does not set one up for each. Not for use by two
	// threads at once; give each thread its own.
	class decoder
	{
	public:
		decoder();
		~decoder();

		decoder(const decoder&) = delete;
		decoder& operator=(const decoder&) = delete;

		// Never throws for a malformed file, nor when it runs out of memory for its own working
		// buffers; a sink that throws is not caught.
		decode_result decode(reader& r, image_sink& sink, const decode_options& options = {});

		struct state;

	private:
		std::unique_ptr<state> _state;
	};

	// One decode with a decoder of its own.
	decode_result decode(reader& r, image_sink& sink, const decode_options& options = {});

	// The reduction decode() applies to a JPEG for a target: the largest of 8, 4 and 2 that leaves the
	// source at least target_width by target_height, else 1.
	std::uint32_t scale_denominator_for(std::uint32_t source_width, std::uint32_t source_height,
	                                    std::uint32_t target_width, std::uint32_t target_height) noexcept;

	struct image_size
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		bool operator==(const image_size&) const = default;
	};

	// The size decode() writes a WebP at for a target: the largest that fits within target_width by
	// target_height, aspect kept, when that is smaller than the source in either dimension; else the
	// source, as it is when the target has a zero edge. Computed as Diffractor's ui::scale_dimensions
	// computes it, so a reduced decode stands in for a decode and a fit: the scale in steps of
	// 1/100000, each step and edge rounded to the nearest, and no edge below 1.
	image_size scaled_size_for(std::uint32_t source_width, std::uint32_t source_height,
	                           std::uint32_t target_width, std::uint32_t target_height) noexcept;
}
