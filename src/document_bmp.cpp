// Copyright (C) 1998-2026 Zac Walker.
// SPDX-License-Identifier: MIT

// Purpose: Uncompressed native BMP rows; compressed DIB rendering remains a host SDK capability.

#include "document_internal.h"

#include <array>

namespace libfiles::detail
{
	void document_bmp(document_decode& decode)
	{
		auto& input = decode.input;
		if (input.size() < 54 || input.le16(0) != 0x4d42 || input.le32(14) != 40 || input.le16(26) != 1)
			decode.fail("invalid BMP header");
		const auto width = static_cast<std::int32_t>(input.le32(18));
		const auto signed_height = static_cast<std::int32_t>(input.le32(22));
		if (width <= 0 || signed_height == 0 || signed_height == std::numeric_limits<std::int32_t>::min())
			decode.fail("invalid BMP dimensions");
		if (input.le32(30) != 0) decode.fail("this BMP compression requires a different decoder");
		const auto height = static_cast<std::uint32_t>(signed_height < 0 ? -signed_height : signed_height);
		const auto storage = storage_from_bits(input.le16(28));
		const auto entries = palette_entries(storage);
		const auto declared_colors = input.le32(46);
		const auto colors = declared_colors == 0 || declared_colors > entries ? entries : declared_colors;
		const auto minimum_offset = 54 + colors * 4;
		const auto declared_offset = input.le32(10);
		const std::uint64_t offset = declared_offset == 0 ? minimum_offset : declared_offset;
		const auto stride = document_row_bytes(width, storage);
		if (offset < minimum_offset || !input.contains(offset, static_cast<std::uint64_t>(stride) * height))
			decode.fail("the BMP pixel data is truncated or its offset is invalid");
		std::array<std::uint32_t, 256> palette{};
		for (std::size_t i = 0; i < entries; ++i)
		{
			const auto gray = entries == 2 ? static_cast<std::uint32_t>(i * 255) : static_cast<std::uint32_t>(i);
			palette[i] = 0xff000000u | gray * 0x010101;
		}
		for (std::size_t i = 0; i < colors; ++i) palette[i] = input.le32(54 + i * 4) | 0xff000000;
		document_frame frame;
		frame.width = width;
		frame.height = height;
		frame.storage = storage;
		frame.palette = {palette.data(), entries};
		decode.working_bytes(stride);
		decode.begin(frame);
		std::vector<std::uint8_t> row(stride);
		for (std::uint32_t y = 0; y < height; ++y)
		{
			const auto source_y = signed_height < 0 ? y : height - 1 - y;
			input.read(offset + static_cast<std::uint64_t>(source_y) * stride, row.data(), row.size());
			decode.row(y, row);
		}
		decode.end();
		auto& info = decode.result.info;
		info.width = width;
		info.height = height;
		info.storage = storage;
		info.bits_per_pixel = storage == document_storage::bgr555 ? 15 : storage_bits(storage);
		info.has_resolution = true;
		info.pixels_per_meter_x = input.le32(38);
		info.pixels_per_meter_y = input.le32(42);
	}
}
