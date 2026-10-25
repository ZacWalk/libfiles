// Copyright (C) 1998-2026 Zac Walker.
// SPDX-License-Identifier: MIT

// Purpose: PCX packed indices and planar RGB, moved from ImageWalker's native reader.

#include "document_internal.h"

#include <array>
#include <cstring>

namespace libfiles::detail
{
	void document_pcx(document_decode& decode)
	{
		auto& input = decode.input;
		if (input.size() < 128 || input.u8(0) != 0x0a || input.u8(2) != 1)
			decode.fail("invalid PCX header");

		const auto width = 1 + static_cast<std::int16_t>(input.le16(8)) -
			static_cast<std::int16_t>(input.le16(4));
		const auto height = 1 + static_cast<std::int16_t>(input.le16(10)) -
			static_cast<std::int16_t>(input.le16(6));
		const auto bits = input.u8(3);
		const auto planes = input.u8(65);
		const auto bytes_per_line = static_cast<std::int16_t>(input.le16(66));
		const bool true_color = planes == 3 && bits == 8;
		const bool indexed = planes == 1 && (bits == 1 || bits == 2 || bits == 4 || bits == 8);
		if (width <= 0 || height <= 0 || bytes_per_line <= 0 || (!true_color && !indexed) ||
			bytes_per_line < (width * bits + 7) / 8)
			decode.fail("invalid PCX dimensions or sample layout");

		const auto format = true_color ? document_storage::bgr24 : storage_from_bits(bits);
		const auto stride = document_row_bytes(width, format);
		const auto row_bytes = static_cast<std::size_t>(bytes_per_line) * planes;
		decode.working_bytes(row_bytes + std::max(stride, row_bytes));
		std::array<std::uint32_t, 256> palette{};
		auto end = input.size();
		bool has_vga_palette = false;

		if (indexed)
		{
			const auto entries = palette_entries(format);
			auto colors = std::min<std::size_t>(entries, 16);
			std::uint64_t offset = 16;
			if (bits == 8 && input.size() > 768 && input.u8(input.size() - 769) == 0x0c)
			{
				offset = input.size() - 768;
				end = offset - 1;
				colors = entries;
				has_vga_palette = true;
			}
			for (std::size_t i = 0; i < colors; ++i)
			{
				const auto red = input.u8(offset++);
				const auto green = input.u8(offset++);
				const auto blue = input.u8(offset++);
				palette[i] = 0xff000000u | (red << 16) | (green << 8) | blue;
			}
			for (auto i = colors; i < entries; ++i) palette[i] = 0xff000000;
			if (bits == 1 && palette[0] == palette[1]) palette[1] = 0xffffffff;
		}

		document_frame frame;
		frame.width = width;
		frame.height = height;
		frame.storage = format;
		frame.palette = {palette.data(), palette_entries(format)};
		decode.begin(frame);
		std::vector<std::uint8_t> plane(row_bytes);
		std::vector<std::uint8_t> row(std::max(stride, row_bytes), 0);
		std::uint64_t position = 128;
		const std::uint8_t* next = nullptr;
		std::size_t available = 0;
		// There are no reader calls between refills, so the borrowed window stays valid.
		const auto read_byte = [&]()
		{
			if (available == 0)
			{
				if (position >= end) decode.fail("the PCX pixels are incomplete");
				const auto bytes = input.view(position, static_cast<std::size_t>(
					std::min<std::uint64_t>(end - position, reader::default_block_size)));
				next = bytes.data();
				available = bytes.size();
			}
			++position;
			--available;
			return *next++;
		};
		unsigned repetitions = 0;
		std::uint8_t repeated = 0;
		for (int y = 0; y < height; ++y)
		{
			for (auto& pixel : plane)
			{
				if (repetitions == 0)
				{
					if (position >= end) decode.fail("the PCX pixels are incomplete");
					const auto value = read_byte();
					repetitions = (value & 0xc0) == 0xc0 ? value & 0x3f : 1;
					if (repetitions == 0 || ((value & 0xc0) == 0xc0 && position >= end))
						decode.fail("invalid PCX run");
					repeated = (value & 0xc0) == 0xc0 ? read_byte() : value;
				}
				pixel = repeated;
				--repetitions;
			}
			if (true_color)
			{
				for (int x = 0; x < width; ++x)
				{
					row[x * 3] = plane[2 * bytes_per_line + x];
					row[x * 3 + 1] = plane[bytes_per_line + x];
					row[x * 3 + 2] = plane[x];
				}
			}
			else
			{
				std::memcpy(row.data(), plane.data(), std::min<std::size_t>(bytes_per_line, stride));
			}
			decode.row(y, row);
			decode.output.progress(y, height);
		}
		if (indexed && bits == 8 && !has_vga_palette && position != end)
			decode.fail("the PCX trailing palette is incomplete");
		decode.end();
		decode.result.info.width = width;
		decode.result.info.height = height;
		decode.result.info.storage = format;
		decode.result.info.bits_per_pixel = bits * planes;
	}
}
