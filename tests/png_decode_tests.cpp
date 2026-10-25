// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for decoding PNGs: each colour type and bit depth brought to straight-alpha BGRA, tRNS,
// 16-bit scaling, gamma, interlacing, the orientation from an eXIf before or after the image data, and
// what decode_file() reports for a file cut short, damaged or refused. The files are written by libpng, with
// the chunks it will not place spliced in.

#include "test.h"

#ifdef LIBFILES_PNG

#include "builders.h"
#include "libfiles/decode.h"
#include "libfiles/document.h"

#include <png.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	[[noreturn]] void PNGCBAPI write_failed(png_structp, const png_const_charp message)
	{
		throw std::runtime_error(message);
	}

	void PNGCBAPI write_quietly(png_structp, png_const_charp)
	{
	}

	void PNGCBAPI append(const png_structp png, const png_bytep data, const size_t length)
	{
		auto* const out = static_cast<bytes*>(png_get_io_ptr(png));
		out->insert(out->end(), data, data + length);
	}

	void PNGCBAPI flush_nothing(png_structp)
	{
	}

	struct png_spec
	{
		std::uint32_t width = 3;
		std::uint32_t height = 2;
		int bit_depth = 8;
		int color_type = PNG_COLOR_TYPE_RGB;
		bool interlaced = false;
		std::vector<png_color> palette;
		std::vector<std::uint8_t> palette_alpha;
		std::optional<png_color_16> transparent;
		double gamma = 0;
		bool srgb = false;
	};

	// A PNG of rows, as libpng writes one.
	bytes write_png(const png_spec& s, const std::vector<bytes>& rows)
	{
		bytes out;
		auto* png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, write_failed, write_quietly);
		auto* info = png_create_info_struct(png);
		png_set_write_fn(png, &out, append, flush_nothing);
		png_set_IHDR(png, info, s.width, s.height, s.bit_depth, s.color_type,
		             s.interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		             PNG_FILTER_TYPE_DEFAULT);

		if (!s.palette.empty()) png_set_PLTE(png, info, s.palette.data(), static_cast<int>(s.palette.size()));
		if (!s.palette_alpha.empty())
			png_set_tRNS(png, info, s.palette_alpha.data(), static_cast<int>(s.palette_alpha.size()), nullptr);

		if (s.transparent)
		{
			auto colour = *s.transparent;
			png_set_tRNS(png, info, nullptr, 0, &colour);
		}

		if (s.srgb) png_set_sRGB_gAMA_and_cHRM(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
		else if (s.gamma > 0) png_set_gAMA(png, info, s.gamma);

		std::vector<png_bytep> pointers;
		for (const auto& row : rows) pointers.push_back(const_cast<png_bytep>(row.data()));

		png_write_info(png, info);
		png_write_image(png, pointers.data());
		png_write_end(png, info);
		png_destroy_write_struct(&png, &info);
		return out;
	}

	// Where the first chunk of a type begins, or 0.
	std::size_t find_chunk(const bytes& file, const std::string_view type)
	{
		for (std::size_t at = 8; at + 12 <= file.size();)
		{
			if (std::string_view(reinterpret_cast<const char*>(file.data() + at + 4), 4) == type) return at;
			at += 12 + libfiles::detail::load_be32(file.data() + at);
		}

		return 0;
	}

	// The file with a chunk put in at a byte offset.
	bytes splice(const bytes& file, const std::size_t at, const std::string_view type, const bytes& data,
	             const bool damage = false)
	{
		builder chunk;
		png_chunk(chunk, type, data, damage);
		bytes result(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(at));
		result.insert(result.end(), chunk.b.begin(), chunk.b.end());
		result.insert(result.end(), file.begin() + static_cast<std::ptrdiff_t>(at), file.end());
		return result;
	}

	// A little-endian TIFF stream with one entry: the orientation.
	bytes orientation_exif(const std::uint16_t value)
	{
		return builder().raw("II").le16(42).le32(8).le16(1).le16(0x0112).le16(3).le32(1).le16(value).le16(0)
		                .le32(0).b;
	}

	using pixel = std::array<std::uint8_t, 4>;

	// A pixel as BGRA, spelled so a test macro does not take its commas for its own.
	pixel px(const std::uint8_t b, const std::uint8_t g, const std::uint8_t r, const std::uint8_t a)
	{
		return {b, g, r, a};
	}

	// Memory filled with a byte the decoder never writes, with padding after each row, so what the
	// decoder touched shows.
	struct png_sink final : libfiles::image_sink
	{
		bytes memory;
		std::size_t stride = 0;
		bool refuse = false;
		int calls = 0;
		libfiles::frame_info told;
		std::size_t exif_told = 0;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			++calls;
			told = frame;
			exif_told = frame.exif.size();
			if (refuse) return {};
			stride = std::size_t{4} * frame.width + 12;
			memory.assign(stride * frame.height, 0xAB);
			return {memory.data(), stride};
		}

		pixel at(const std::size_t x, const std::size_t y) const
		{
			const auto* const p = memory.data() + y * stride + x * 4;
			return {p[0], p[1], p[2], p[3]};
		}

		bool padding_kept() const
		{
			for (std::size_t y = 0; y < told.height; ++y)
				for (auto x = std::size_t{4} * told.width; x < stride; ++x)
					if (memory[y * stride + x] != 0xAB) return false;
			return true;
		}
	};

	libfiles::decode_result decode_file(const bytes& file, png_sink& sink)
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::decode(r, sink);
	}

	bytes rgb_row(const std::uint32_t width, const std::uint32_t y)
	{
		bytes row;
		for (std::uint32_t x = 0; x < width; ++x)
		{
			row.push_back(static_cast<std::uint8_t>(x * 80));
			row.push_back(static_cast<std::uint8_t>(y * 100));
			row.push_back(7);
		}
		return row;
	}

	std::vector<bytes> rgb_rows(const std::uint32_t width, const std::uint32_t height)
	{
		std::vector<bytes> rows;
		for (std::uint32_t y = 0; y < height; ++y) rows.push_back(rgb_row(width, y));
		return rows;
	}

	bool contains(const std::vector<std::string>& warnings, const std::string_view text)
	{
		for (const auto& w : warnings)
			if (w.find(text) != std::string::npos) return true;
		return false;
	}
}

TEST("native PNG keeps packed palettes alpha and interlaced rows")
{
	struct native_sink final : libfiles::document_sink
	{
		libfiles::document_storage storage{};
		bytes pixels;
		std::vector<std::uint32_t> palette;
		std::size_t stride = 0;
		bool begin_frame(const libfiles::document_frame& frame) override
		{
			storage = frame.storage;
			stride = libfiles::document_row_bytes(frame.width, frame.storage);
			pixels.resize(stride * frame.height);
			palette.assign(frame.palette.begin(), frame.palette.end());
			return true;
		}
		void row(std::uint32_t y, std::span<const std::uint8_t> bytes) override
		{
			std::copy_n(bytes.data(), stride, pixels.data() + y * stride);
		}
	};
	for (const bool interlaced : {false, true})
	{
		png_spec spec;
		spec.bit_depth = 2;
		spec.color_type = PNG_COLOR_TYPE_PALETTE;
		spec.interlaced = interlaced;
		spec.palette = {{17, 34, 51}, {68, 85, 102}, {119, 136, 153}, {170, 187, 204}};
		spec.palette_alpha = {0, 128};
		spec.gamma = 0.2;
		libfiles::memory_stream file(write_png(spec, {{0x18}, {0x90}}));
		libfiles::reader input(file);
		native_sink sink;
		const auto result = libfiles::decode_document(input, libfiles::image_format::png, sink);
		CHECK(result.success);
		CHECK(result.info.interlace_passes == (interlaced ? 7 : 1));
		CHECK(sink.storage == libfiles::document_storage::indexed2);
		CHECK(sink.pixels == bytes({0x18, 0, 0, 0, 0x90, 0, 0, 0}));
		CHECK(sink.palette == std::vector<std::uint32_t>({0x00112233, 0x80445566, 0xff778899, 0xffaabbcc}));
	}
}

TEST("native PNG reports a damaged IHDR through its result instead of unwinding C frames")
{
	png_spec spec;
	auto file = write_png(spec, {{10, 20, 30, 40, 50, 60, 70, 80, 90},
		{90, 80, 70, 60, 50, 40, 30, 20, 10}});
	CHECK(file.size() > 32);
	if (file.size() <= 32) return;
	file[29] ^= 1;
	struct no_pixels final : libfiles::document_sink
	{
		bool begin_frame(const libfiles::document_frame&) override { CHECK(false); return false; }
		void row(std::uint32_t, std::span<const std::uint8_t>) override { CHECK(false); }
	} sink;
	libfiles::reader input(file);
	const auto result = libfiles::decode_document(input, libfiles::image_format::png, sink);
	CHECK(!result.success && !result.error.empty());
}

TEST("Should decode an RGB PNG to opaque BGRA")
{
	CHECK(libfiles::can_decode(libfiles::image_format::png));

	png_sink sink;
	const auto result = decode_file(write_png({}, rgb_rows(3, 2)), sink);

	CHECK(result.success);
	CHECK(!result.refused && !result.truncated && result.error.empty());
	CHECK(result.warnings.empty());
	CHECK_EQ(sink.calls, 1);
	CHECK(result.frame.format == libfiles::pixel_format::bgra8);
	CHECK_EQ(result.frame.width, 3u);
	CHECK_EQ(result.frame.height, 2u);
	CHECK_EQ(result.frame.source_width, 3u);
	CHECK_EQ(result.frame.source_height, 2u);
	CHECK_EQ(result.frame.orientation, 1u);

	auto exact = true;
	for (std::uint32_t y = 0; y < 2; ++y)
		for (std::uint32_t x = 0; x < 3; ++x)
			exact &= sink.at(x, y) == pixel{7, static_cast<std::uint8_t>(y * 100), static_cast<std::uint8_t>(x * 80), 0xFF};
	CHECK(exact);
	CHECK(sink.padding_kept());
}

TEST("Should keep a PNG's alpha straight")
{
	png_spec spec;
	spec.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
	spec.width = 3;
	spec.height = 1;

	png_sink sink;
	const auto result = decode_file(write_png(spec, {{200, 100, 50, 0, 200, 100, 50, 128, 200, 100, 50, 255}}), sink);

	CHECK(result.success);
	CHECK(sink.at(0, 0) == px(50, 100, 200, 0));
	CHECK(sink.at(1, 0) == px(50, 100, 200, 128));
	CHECK(sink.at(2, 0) == px(50, 100, 200, 255));
}

TEST("Should expand gray, gray with alpha and low bit depths to BGRA")
{
	{
		png_spec spec;
		spec.color_type = PNG_COLOR_TYPE_GRAY;
		spec.bit_depth = 1;
		spec.width = 8;
		spec.height = 1;

		png_sink sink;
		CHECK(decode_file(write_png(spec, {{0xA0}}), sink).success);
		CHECK(sink.at(0, 0) == px(255, 255, 255, 255));
		CHECK(sink.at(1, 0) == px(0, 0, 0, 255));
		CHECK(sink.at(2, 0) == px(255, 255, 255, 255));
		CHECK(sink.at(7, 0) == px(0, 0, 0, 255));
	}

	{
		png_spec spec;
		spec.color_type = PNG_COLOR_TYPE_GRAY;
		spec.bit_depth = 4;
		spec.width = 4;
		spec.height = 1;

		png_sink sink;
		CHECK(decode_file(write_png(spec, {{0x05, 0xAF}}), sink).success);
		CHECK(sink.at(0, 0) == px(0, 0, 0, 255));
		CHECK(sink.at(1, 0) == px(85, 85, 85, 255));
		CHECK(sink.at(2, 0) == px(170, 170, 170, 255));
		CHECK(sink.at(3, 0) == px(255, 255, 255, 255));
	}

	{
		png_spec spec;
		spec.color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
		spec.width = 2;
		spec.height = 1;

		png_sink sink;
		CHECK(decode_file(write_png(spec, {{90, 0, 60, 200}}), sink).success);
		CHECK(sink.at(0, 0) == px(90, 90, 90, 0));
		CHECK(sink.at(1, 0) == px(60, 60, 60, 200));
	}
}

TEST("Should take a palette's colours, and transparency from tRNS")
{
	png_spec spec;
	spec.color_type = PNG_COLOR_TYPE_PALETTE;
	spec.width = 3;
	spec.height = 1;
	spec.palette = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}};
	spec.palette_alpha = {0, 128};

	png_sink sink;
	CHECK(decode_file(write_png(spec, {{2, 1, 0}}), sink).success);
	CHECK(sink.at(0, 0) == px(90, 80, 70, 255));
	CHECK(sink.at(1, 0) == px(60, 50, 40, 128));
	CHECK(sink.at(2, 0) == px(30, 20, 10, 0));

	// An RGB tRNS names one colour transparent.
	png_spec rgb;
	rgb.width = 2;
	rgb.height = 1;
	rgb.transparent = png_color_16{0, 10, 20, 30, 0};

	png_sink rgb_sink;
	CHECK(decode_file(write_png(rgb, {{10, 20, 30, 11, 20, 30}}), rgb_sink).success);
	CHECK(rgb_sink.at(0, 0) == px(30, 20, 10, 0));
	CHECK(rgb_sink.at(1, 0) == px(30, 20, 11, 255));
}

TEST("Should scale 16 bits a channel to 8")
{
	png_spec spec;
	spec.bit_depth = 16;
	spec.width = 2;
	spec.height = 1;

	// Each sample v * 257, which scales to v exactly.
	png_sink sink;
	CHECK(decode_file(write_png(spec, {{1, 1, 128, 128, 255, 255, 0, 0, 77, 77, 200, 200}}), sink).success);
	CHECK(sink.at(0, 0) == px(255, 128, 1, 255));
	CHECK(sink.at(1, 0) == px(200, 77, 0, 255));
}

TEST("Should correct a PNG's own gamma for an sRGB display, unless it says it is sRGB")
{
	png_spec spec;
	spec.color_type = PNG_COLOR_TYPE_GRAY;
	spec.width = 1;
	spec.height = 1;

	// Linear, so mid-grey shows lighter: 255 * (128 / 255) ^ (1 / 2.2), rounded.
	spec.gamma = 1.0;
	png_sink linear;
	CHECK(decode_file(write_png(spec, {{128}}), linear).success);
	CHECK(linear.at(0, 0) == px(186, 186, 186, 255));

	spec.srgb = true;
	png_sink srgb;
	CHECK(decode_file(write_png(spec, {{128}}), srgb).success);
	CHECK(srgb.at(0, 0) == px(128, 128, 128, 255));
}

TEST("Should decode an interlaced PNG whole, saying libpng warned")
{
	png_spec spec;
	spec.width = 9;
	spec.height = 9;

	png_sink plain;
	CHECK(decode_file(write_png(spec, rgb_rows(9, 9)), plain).success);

	spec.interlaced = true;
	png_sink interlaced;
	const auto result = decode_file(write_png(spec, rgb_rows(9, 9)), interlaced);

	CHECK(result.success);
	CHECK(interlaced.memory == plain.memory);
	CHECK(contains(result.warnings, "Interlace handling should be turned on"));
}

TEST("Should take the orientation from the last EXIF, after the image data too")
{
	const auto file = write_png({}, rgb_rows(3, 2));
	const auto before = splice(file, find_chunk(file, "IDAT"), "eXIf", orientation_exif(6));
	const auto after = splice(file, find_chunk(file, "IEND"), "eXIf", orientation_exif(3));
	const auto both = splice(before, find_chunk(before, "IEND"), "eXIf", orientation_exif(8));

	png_sink sink_before;
	const auto result_before = decode_file(before, sink_before);
	CHECK(result_before.success);
	CHECK_EQ(result_before.frame.orientation, 6u);
	CHECK(sink_before.told.orientation == 6 && sink_before.exif_told > 0);
	CHECK(result_before.frame.exif.empty());

	png_sink sink_after;
	CHECK_EQ(decode_file(after, sink_after).frame.orientation, 3u);

	png_sink sink_both;
	CHECK_EQ(decode_file(both, sink_both).frame.orientation, 8u);
}

TEST("Should report a PNG cut short, keeping the warnings given before")
{
	png_spec spec;
	spec.width = 64;
	spec.height = 64;
	const auto file = write_png(spec, rgb_rows(64, 64));
	const auto idat = find_chunk(file, "IDAT");

	// A damaged tEXt before the image data gives a warning, then the file ends inside it.
	auto damaged = splice(file, idat, "tEXt", to_bytes(std::string_view("Comment\0x", 9)), true);
	damaged.resize(find_chunk(damaged, "IDAT") + 8 + 20);

	png_sink sink;
	const auto result = decode_file(damaged, sink);
	CHECK(!result.success);
	CHECK(result.truncated);
	CHECK(!result.refused);
	CHECK(!result.error.empty());
	CHECK(contains(result.warnings, "CRC"));
	CHECK_EQ(sink.calls, 1);

	// Cut before the header ends.
	bytes short_header(file.begin(), file.begin() + 20);
	png_sink header_sink;
	const auto header_result = decode_file(short_header, header_sink);
	CHECK(!header_result.success && header_result.truncated);
	CHECK_EQ(header_sink.calls, 0);
}

TEST("Should report a damaged PNG, and skip a damaged ancillary chunk")
{
	const auto file = write_png({}, rgb_rows(3, 2));

	// Starts as a PNG does, but the rest of the signature is wrong.
	auto signature = file;
	signature[4] = 'X';
	png_sink signature_sink;
	const auto signature_result = decode_file(signature, signature_sink);
	CHECK(!signature_result.success && !signature_result.truncated && !signature_result.error.empty());
	CHECK_EQ(signature_sink.calls, 0);

	// The header's CRC is wrong: libpng will not go on.
	auto header = file;
	header[29] ^= 0xFF;
	png_sink header_sink;
	const auto header_result = decode_file(header, header_sink);
	CHECK(!header_result.success && !header_result.truncated && !header_result.error.empty());

	// A tEXt whose CRC is wrong is skipped with a warning, and the image decodes.
	const auto text = splice(file, find_chunk(file, "IDAT"), "tEXt", to_bytes(std::string_view("Comment\0x", 9)), true);
	png_sink text_sink;
	const auto text_result = decode_file(text, text_sink);
	CHECK(text_result.success);
	CHECK(contains(text_result.warnings, "CRC"));
	CHECK(text_sink.at(2, 1) == px(7, 100, 160, 255));
}

TEST("Should ask the sink once, from the header alone, and stop if it refuses")
{
	png_spec spec;
	spec.width = 5;
	spec.height = 4;

	png_sink sink;
	sink.refuse = true;
	const auto result = decode_file(write_png(spec, rgb_rows(5, 4)), sink);

	CHECK(!result.success);
	CHECK(result.refused);
	CHECK(result.error.empty());
	CHECK_EQ(sink.calls, 1);
	CHECK_EQ(sink.told.width, 5u);
	CHECK_EQ(sink.told.height, 4u);
	CHECK(sink.told.format == libfiles::pixel_format::bgra8);
}

#endif
