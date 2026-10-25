// Copyright (C) 1998-2026 Zac Walker.
// SPDX-License-Identifier: MIT

// Purpose: Native GIF frames, local palettes, disposal and comments, through giflib.

#include "document_internal.h"

#include <gif_lib.h>

#include <array>
#include <cstring>

namespace libfiles::detail
{
	namespace
	{
		struct gif_input
		{
			document_decode& decode;
			std::uint64_t position = 0;
			bool failed = false;
		};

		int gif_read(GifFileType* file, GifByteType* bytes, const int requested)
		{
			auto& input = *static_cast<gif_input*>(file->UserData);
			if (requested <= 0) return 0;
			const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
				requested, input.decode.input.size() - input.position));
			try
			{
				input.decode.input.read(input.position, bytes, count);
				input.position += count;
				return static_cast<int>(count);
			}
			catch (const read_error&)
			{
				input.failed = true;
			}
			catch (const std::bad_alloc&)
			{
				input.failed = true;
				input.decode.result.resource_limit = true;
			}
			return 0;
		}
	}

	void document_gif(document_decode& decode)
	{
		std::array<std::uint8_t, 6> signature{};
		decode.input.read(0, signature.data(), signature.size());
		if (std::memcmp(signature.data(), "GIF87a", 6) && std::memcmp(signature.data(), "GIF89a", 6))
			decode.fail("invalid GIF signature");
		gif_input source{decode};
		int error = 0;
		std::unique_ptr<GifFileType, void(*)(GifFileType*)> owner(DGifOpen(&source, gif_read, &error),
			[](GifFileType* file) { int error = 0; DGifCloseFile(file, &error); });
		if (!owner) decode.fail("the GIF header could not be read");
		auto* file = owner.get();
		auto& info = decode.result.info;
		info.width = file->SWidth;
		info.height = file->SHeight;
		info.storage = document_storage::indexed8;
		std::vector<std::uint8_t> row, saved, comment;
		int transparent = -1;
		unsigned delay = 0, disposal = 2;
		std::string damage;
		bool done = false;
		while (!done)
		{
			decode.check_cancel();
			GifRecordType type{};
			if (DGifGetRecordType(file, &type) == GIF_ERROR)
			{
				if (source.failed || file->Error != D_GIF_ERR_READ_FAILED)
					damage = "the GIF record stream is damaged";
				break;
			}
			if (type == TERMINATE_RECORD_TYPE) break;
			if (type == IMAGE_DESC_RECORD_TYPE)
			{
				if (DGifGetImageDesc(file) == GIF_ERROR)
				{
					damage = "a GIF image descriptor is incomplete";
					break;
				}
				const auto& image = file->Image;
				if (image.Left + image.Width > file->SWidth || image.Top + image.Height > file->SHeight ||
					image.Width <= 0 || image.Height <= 0)
				{
					damage = "a GIF frame exceeds its canvas";
					break;
				}
				std::array<std::uint32_t, 256> palette{};
				const auto* colors = image.ColorMap ? image.ColorMap : file->SColorMap;
				if (colors)
				{
					info.storage = storage_from_bits(colors->BitsPerPixel);
					for (int i = 0; i < colors->ColorCount && i < 256; ++i)
						palette[i] = 0xff000000u | (colors->Colors[i].Red << 16) |
							(colors->Colors[i].Green << 8) | colors->Colors[i].Blue;
				}
				else
					for (unsigned i = 0; i < palette.size(); ++i) palette[i] = i * 0x010101;
				document_frame frame;
				frame.left = image.Left;
				frame.top = image.Top;
				frame.width = image.Width;
				frame.height = image.Height;
				frame.storage = document_storage::indexed8;
				frame.sequential = image.Interlace == 0;
				frame.palette = palette;
				frame.delay_ms = delay;
				frame.disposal = disposal;
				if (transparent >= 0)
				{
					frame.transparent = transparent;
					frame.background = transparent;
				}
				info.interlace_passes = image.Interlace ? 4 : 1;
				const auto stride = document_row_bytes(frame.width, frame.storage);
				const bool first = info.frames == 0;
				decode.working_bytes(stride + (first ? 0 : static_cast<std::uint64_t>(stride) * frame.height));
				row.resize(stride);
				if (first) decode.begin(frame);
				else saved.assign(stride * frame.height, 0);
				const auto read_row = [&](const unsigned y)
				{
					decode.check_cancel();
					if (DGifGetLine(file, row.data(), image.Width) == GIF_ERROR) return false;
					if (first) decode.row(y, row);
					else std::memcpy(saved.data() + y * stride, row.data(), frame.width);
					return true;
				};
				bool complete = true;
				if (image.Interlace)
				{
					constexpr unsigned offsets[] = {0, 4, 2, 1};
					constexpr unsigned jumps[] = {8, 8, 4, 2};
					for (unsigned pass = 0; complete && pass < 4; ++pass)
						for (unsigned y = offsets[pass]; complete && y < frame.height; y += jumps[pass])
							complete = read_row(y);
				}
				else
					for (unsigned y = 0; complete && y < frame.height; ++y) complete = read_row(y);
				if (!complete)
				{
					damage = "a GIF frame is incomplete";
					break;
				}
				if (!first)
				{
					frame.sequential = true;
					decode.begin(frame);
					for (unsigned y = 0; y < frame.height; ++y)
						decode.row(y, {saved.data() + y * stride, stride});
				}
				decode.end();
				transparent = -1;
				disposal = 2;
			}
			else if (type == EXTENSION_RECORD_TYPE)
			{
				int code = 0;
				GifByteType* extension = nullptr;
				if (DGifGetExtension(file, &code, &extension) == GIF_ERROR)
				{
					damage = "a GIF extension block is incomplete";
					break;
				}
				while (extension)
				{
					if (code == COMMENT_EXT_FUNC_CODE)
					{
						decode.working_bytes(comment.size() + extension[0]);
						comment.insert(comment.end(), extension + 1, extension + 1 + extension[0]);
					}
					else if (code == GRAPHICS_EXT_FUNC_CODE)
					{
						if (extension[0] < 4)
						{
							damage = "a GIF control extension is incomplete";
							done = true;
							break;
						}
						transparent = extension[1] & 1 ? extension[4] : -1;
						disposal = (extension[1] >> 2) & 7;
						delay = static_cast<unsigned>(static_cast<std::int16_t>(extension[2] | (extension[3] << 8))) * 10;
					}
					if (DGifGetExtensionNext(file, &extension) == GIF_ERROR)
					{
						damage = "a GIF extension block is incomplete";
						done = true;
						break;
					}
				}
			}
		}
		if (decode.result.resource_limit) decode.fail("not enough memory to read the GIF");
		if (info.frames == 0) decode.fail(damage.empty() ? "the GIF image data is incomplete" : damage);
		if (!damage.empty()) decode.result.warnings.push_back(damage + "; complete frames before it were kept");
		if (!comment.empty()) decode.output.metadata(document_metadata::comment, comment);
		info.bits_per_pixel = storage_bits(info.storage);
		if (DGifCloseFile(owner.release(), &error) == GIF_ERROR) decode.fail("the GIF stream could not be closed");
	}
}
