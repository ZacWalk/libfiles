// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Decoding a JPEG XL with libjxl into memory the caller provides: its first frame, as B8G8R8A8
// with straight alpha where it has alpha and B8G8R8X8 where it has none - libjxl writes 255 in the fourth
// byte - left unturned, its codestream's orientation reported. libjxl writes RGBA into the caller's rows
// and the red and blue are swapped there. One thread decodes, as Diffractor's loader did. Moved from
// Diffractor's loader unchanged.

#include "decode_internal.h"
#include "scan_internal.h"

#include <jxl/decode.h>

#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		struct decoder_destroy
		{
			void operator()(JxlDecoder* d) const noexcept { JxlDecoderDestroy(d); }
		};

		// RGBA, as libjxl writes it, to BGRA in place: the pixels of each row, and not its padding.
		void swap_red_blue(const pixel_buffer pixels, const std::uint32_t width, const std::uint32_t height)
		{
			for (std::uint32_t y = 0; y < height; ++y)
			{
				auto* p = pixels.pixels + pixels.stride * y;

				for (std::uint32_t x = 0; x < width; ++x, p += 4)
				{
					const auto red = p[0];
					p[0] = p[2];
					p[2] = red;
				}
			}
		}
	}

	void decode_jxl(reader& r, image_sink& sink, decode_result& result)
	{
		// libjxl reads a whole file from memory: a reader over memory lends it, and one over a stream
		// reads it in.
		auto file = r.resident();
		std::vector<std::uint8_t> copy;

		if (file.empty() && r.size() != 0)
		{
			if (r.size() > std::numeric_limits<std::size_t>::max()) throw std::bad_alloc();
			copy = r.read(0, static_cast<std::size_t>(r.size()));
			file = copy;
		}

		const std::unique_ptr<JxlDecoder, decoder_destroy> decoder(JxlDecoderCreate(nullptr));

		if (!decoder)
		{
			result.error = "could not create a libjxl decoder";
			return;
		}

		// The pixels as the codestream stores them, its orientation reported rather than applied, so the
		// size decoded is the size a scan reports; and alpha made straight where it is premultiplied.
		if (JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
			JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
			JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS)
		{
			result.error = "libjxl refused the decoder's options";
			return;
		}

		JxlDecoderSetInput(decoder.get(), file.data(), file.size());
		JxlDecoderCloseInput(decoder.get());

		frame_info frame;
		pixel_buffer pixels;

		for (;;)
		{
			const auto status = JxlDecoderProcessInput(decoder.get());

			if (status == JXL_DEC_BASIC_INFO)
			{
				JxlBasicInfo info = {};

				if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS)
				{
					result.error = "libjxl could not read the image's header";
					return;
				}

				if (info.xsize == 0 || info.ysize == 0 || info.xsize > max_image_dimension ||
					info.ysize > max_image_dimension)
				{
					result.error = "the JPEG XL's image is larger than libfiles decodes, or has no size";
					return;
				}

				frame.source_width = info.xsize;
				frame.source_height = info.ysize;
				frame.width = info.xsize;
				frame.height = info.ysize;
				frame.format = info.alpha_bits > 0 ? pixel_format::bgra8 : pixel_format::bgrx8;
				// JxlOrientation numbers its turns as EXIF does.
				frame.orientation = info.orientation >= JXL_ORIENT_IDENTITY && info.orientation <= JXL_ORIENT_ROTATE_90_CCW
					                    ? static_cast<std::uint16_t>(info.orientation)
					                    : std::uint16_t{1};

				pixels = sink.allocate(frame);
				result.frame = frame;

				if (!pixels.pixels)
				{
					result.refused = true;
					return;
				}
			}
			else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
			{
				if (!pixels.pixels)
				{
					result.error = "libjxl asked for the image before it gave its size";
					return;
				}

				// RGBA rows the sink's stride apart, written where it wants them; the last row needs no
				// padding after it.
				const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, pixels.stride};
				const auto size = pixels.stride * (frame.height - 1) + std::size_t{4} * frame.width;

				if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.pixels, size) != JXL_DEC_SUCCESS)
				{
					result.error = "libjxl refused the image's memory";
					return;
				}
			}
			else if (status == JXL_DEC_FULL_IMAGE)
			{
				// The first frame is the still image.
				swap_red_blue(pixels, frame.width, frame.height);
				result.success = true;
				return;
			}
			else
			{
				// Done without a frame, or an error. Its input closed, libjxl reports a file cut short as an
				// error too, so the two are not told apart.
				result.error = "libjxl could not decode the image, which is malformed or cut short";
				return;
			}
		}
	}
}
