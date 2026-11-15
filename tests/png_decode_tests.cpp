// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Tests for decoding PNGs: each colour type and bit depth brought to straight-alpha BGRA, tRNS,
// 16-bit scaling, gamma, an iCCP profile's colours mapped to sRGB, a cICP chunk making the picture HDR to the
// scan as to the decode, interlacing, the orientation from an eXIf before or after the image data, and what
// decode_file() reports for a file cut short, damaged or refused. The files are written by libpng, with the
// chunks it will not place spliced in.

#include "test.h"

#ifdef LIBFILES_PNG

#include "builders.h"
#include "icc_corpus.h"
#include "libfiles/decode.h"
#include "libfiles/document.h"
#include "libfiles/hdr.h"
#include "libfiles/scan.h"

#include <png.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
		std::vector<std::uint8_t> icc;
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

		if (!s.icc.empty())
			png_set_iCCP(png, info, "icc", PNG_COMPRESSION_TYPE_BASE, s.icc.data(),
			             static_cast<png_uint_32>(s.icc.size()));

		std::vector<png_bytep> pointers;
		pointers.reserve(rows.size());
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
		return builder().raw("II").le16(42).le32(8).le16(1).le16(0x0112).le16(3).le32(1).le16(value).le16(0).le32(0).b;
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

		std::size_t pixel_bytes() const
		{
			return told.format == libfiles::pixel_format::rgba16 || told.format == libfiles::pixel_format::rgbx16 ? 8
			                                                                                                      : 4;
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			++calls;
			told = frame;
			exif_told = frame.exif.size();
			if (refuse) return {};
			stride = pixel_bytes() * frame.width + 12;
			memory.assign(stride * frame.height, 0xAB);
			return {memory.data(), stride};
		}

		pixel at(const std::size_t x, const std::size_t y) const
		{
			const auto* const p = memory.data() + y * stride + x * 4;
			return {p[0], p[1], p[2], p[3]};
		}

		std::array<std::uint16_t, 4> wide_at(const std::size_t x, const std::size_t y) const
		{
			std::array<std::uint16_t, 4> channels{};
			std::memcpy(channels.data(), memory.data() + y * stride + x * 8, 8);
			return channels;
		}

		bool padding_kept() const
		{
			for (std::size_t y = 0; y < told.height; ++y)
				for (auto x = pixel_bytes() * told.width; x < stride; ++x)
					if (memory[y * stride + x] != 0xAB) return false;
			return true;
		}
	};

	libfiles::decode_result decode_file(const bytes& file, png_sink& sink, const libfiles::decode_options& options = {})
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::decode(r, sink, options);
	}

	// Sixteen-bit channels, red first, spelled so a test macro does not take their commas for its own.
	std::array<std::uint16_t, 4> wide(const std::uint16_t r, const std::uint16_t g, const std::uint16_t b,
	                                  const std::uint16_t a)
	{
		return {r, g, b, a};
	}

	// A row of 16-bit samples as a PNG stores them, big-endian.
	bytes big_endian_row(const std::vector<std::uint16_t>& samples)
	{
		bytes row;
		for (const auto v : samples)
		{
			row.push_back(static_cast<std::uint8_t>(v >> 8));
			row.push_back(static_cast<std::uint8_t>(v));
		}
		return row;
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
		rows.reserve(height);
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
	auto file = write_png(spec, {{10, 20, 30, 40, 50, 60, 70, 80, 90}, {90, 80, 70, 60, 50, 40, 30, 20, 10}});
	CHECK(file.size() > 32);
	if (file.size() <= 32) return;
	file[29] ^= 1;
	struct no_pixels final : libfiles::document_sink
	{
		bool begin_frame(const libfiles::document_frame&) override
		{
			CHECK(false);
			return false;
		}
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
			exact &= sink.at(x, y) ==
			         pixel{7, static_cast<std::uint8_t>(y * 100), static_cast<std::uint8_t>(x * 80), 0xFF};
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

TEST("Should keep sixteen bits a channel for a caller that takes them")
{
	png_spec spec;
	spec.bit_depth = 16;
	spec.width = 2;
	spec.height = 1;

	libfiles::decode_options options;
	options.allow_16bit = true;

	png_sink sink;
	CHECK(
		decode_file(write_png(spec, {big_endian_row({0x1234, 0xABCD, 0x0102, 0xFFFF, 0x0000, 0x8000})}), sink, options)
			.success);
	CHECK(sink.told.format == libfiles::pixel_format::rgbx16);
	CHECK(sink.wide_at(0, 0) == wide(0x1234, 0xABCD, 0x0102, 0xFFFF));
	CHECK(sink.wide_at(1, 0) == wide(0xFFFF, 0x0000, 0x8000, 0xFFFF));
	CHECK(sink.padding_kept());

	png_spec alpha = spec;
	alpha.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
	alpha.width = 1;
	png_sink alpha_sink;
	CHECK(
		decode_file(write_png(alpha, {big_endian_row({0x1234, 0xABCD, 0x0102, 0x4001})}), alpha_sink, options).success);
	CHECK(alpha_sink.told.format == libfiles::pixel_format::rgba16);
	CHECK(alpha_sink.wide_at(0, 0) == wide(0x1234, 0xABCD, 0x0102, 0x4001));

	// Eight bits stay eight, whatever the caller takes.
	png_spec eight;
	eight.width = 1;
	eight.height = 1;
	png_sink byte_sink;
	CHECK(decode_file(write_png(eight, {{10, 20, 30}}), byte_sink, options).success);
	CHECK(byte_sink.told.format == libfiles::pixel_format::bgra8);
	CHECK(byte_sink.at(0, 0) == px(30, 20, 10, 255));
}

// An HDR still shown as an SDR display shows it: a neutral at 50 nits of PQ - below where BT.2390 rolls
// highlights off - comes out at 50 / 203 of SDR white, sRGB-encoded, and HLG's reference white at a quarter
// of white's light. Drawn as it is, the signal would be read as sRGB and show dim and washed out.
TEST("Should map a PNG its cICP chunk marks PQ or HLG to SDR")
{
	png_spec spec;
	spec.bit_depth = 16;
	spec.width = 2;
	spec.height = 1;

	const auto with_cicp = [](const bytes& file, const std::uint8_t transfer)
	{
		return splice(file, find_chunk(file, "IDAT"), "cICP", {9, transfer, 0, 1});
	};

	// The neutral, and BT.2020's red, which lies outside BT.709 and is fitted rather than clipped.
	const auto pq = with_cicp(write_png(spec, {big_endian_row({28854, 28854, 28854, 32768, 0, 0})}), 16);
	png_sink sink;
	const auto mapped = decode_file(pq, sink);
	CHECK(mapped.success);
	CHECK(mapped.frame.tone_mapped);

	const auto grey = sink.at(0, 0);
	CHECK(std::abs(grey[2] - 136) <= 1 && std::abs(grey[1] - 136) <= 1 && std::abs(grey[0] - 136) <= 1);
	const auto red = sink.at(1, 0);
	CHECK(red[2] > red[1] + 100 && red[2] > red[0] + 100);

	libfiles::decode_options options;
	options.allow_16bit = true;
	png_sink wide_sink;
	CHECK(decode_file(pq, wide_sink, options).success);
	CHECK(wide_sink.told.format == libfiles::pixel_format::rgbx16);
	CHECK(std::abs(wide_sink.wide_at(0, 0)[1] - 34959) <= 200);

	const auto hlg = with_cicp(write_png(spec, {big_endian_row({49151, 49151, 49151, 65535, 65535, 65535})}), 18);
	png_sink hlg_sink;
	CHECK(decode_file(hlg, hlg_sink).frame.tone_mapped);
	CHECK(std::abs(hlg_sink.at(0, 0)[1] - 141) <= 1);
	CHECK(hlg_sink.at(1, 0) == px(255, 255, 255, 255));

	// A cICP chunk naming an SDR transfer changes nothing.
	png_sink sdr_sink;
	const auto sdr = decode_file(with_cicp(write_png(spec, {big_endian_row({28854, 28854, 28854, 0, 0, 0})}), 13),
	                             sdr_sink);
	CHECK(!sdr.frame.tone_mapped);
	CHECK(sdr_sink.at(0, 0) == px(112, 112, 112, 255));
}

// Kept for a caller that keeps HDR, the picture is its signal as the file stores it, at sixteen bits whatever else the
// caller takes, described by its cICP, cLLI and mDCV chunks; and map_hdr_to_sdr maps it as the decode itself would.
TEST("Should keep a PNG's HDR signal and what its chunks say of it, for a caller that keeps HDR")
{
	png_spec spec;
	spec.bit_depth = 16;
	spec.width = 2;
	spec.height = 1;

	auto pq = write_png(spec, {big_endian_row({28854, 28854, 28854, 32768, 0, 0})});
	pq = splice(pq, find_chunk(pq, "IDAT"), "cICP", {9, 16, 0, 1});
	pq = splice(pq, find_chunk(pq, "IDAT"), "cLLI", builder().be32(10000000).be32(4000000).b);
	pq = splice(pq, find_chunk(pq, "IDAT"), "mDCV",
	            builder()
	                .be16(34000)
	                .be16(16000)
	                .be16(13250)
	                .be16(34500)
	                .be16(7500)
	                .be16(3000)
	                .be16(15635)
	                .be16(16450)
	                .be32(10000000)
	                .be32(50)
	                .b);

	libfiles::decode_options keep;
	keep.keep_hdr = true;
	png_sink kept;
	const auto signal = decode_file(pq, kept, keep);
	CHECK(signal.success);
	CHECK(!signal.frame.tone_mapped);
	CHECK(kept.told.format == libfiles::pixel_format::rgbx16);
	CHECK(kept.wide_at(0, 0) == wide(28854, 28854, 28854, 0xFFFF));
	CHECK(kept.wide_at(1, 0) == wide(32768, 0, 0, 0xFFFF));

	CHECK(signal.frame.hdr.has_value());
	const auto& hdr = *signal.frame.hdr;
	CHECK(hdr.transfer == libfiles::hdr_transfer::pq);
	CHECK_EQ(hdr.colour_primaries, 9);
	CHECK(hdr.primaries == *libfiles::chromaticities_of(9));
	CHECK(std::abs(hdr.max_cll - 1000.0) < 1e-6 && std::abs(hdr.max_fall - 400.0) < 1e-6);
	CHECK(hdr.mastering.has_value());
	CHECK(std::abs(hdr.mastering->primaries.red_x - 0.68) < 1e-6 &&
	      std::abs(hdr.mastering->primaries.white_y - 0.329) < 1e-6);
	CHECK(std::abs(hdr.mastering->max_luminance - 1000.0) < 1e-6 &&
	      std::abs(hdr.mastering->min_luminance - 0.005) < 1e-6);
	CHECK(std::abs(hdr.peak_nits() - 1000.0) < 1e-6);

	// Mapped, it says the same of itself.
	png_sink mapped;
	const auto shown = decode_file(pq, mapped);
	CHECK(shown.frame.tone_mapped && shown.frame.hdr == signal.frame.hdr);

	std::vector<std::uint8_t> sdr(8, 0);
	const libfiles::image_view view{kept.memory.data(), kept.stride, 2, 1, libfiles::pixel_format::rgbx16};
	CHECK(libfiles::map_hdr_to_sdr(view, hdr, libfiles::pixel_format::bgrx8, {sdr.data(), 8}));
	for (std::size_t x = 0; x < 2; ++x)
		for (std::size_t c = 0; c < 3; ++c) CHECK_EQ(sdr[x * 4 + c], mapped.at(x, 0)[c]);

	// An eight-bit HDR PNG is widened to the signal its bytes stand for.
	png_spec eight;
	eight.width = 1;
	eight.height = 1;
	const auto bytes_png = write_png(eight, {{128, 64, 255}});
	const auto narrow = splice(bytes_png, find_chunk(bytes_png, "IDAT"), "cICP", {9, 18, 0, 1});
	png_sink widened;
	const auto hlg = decode_file(narrow, widened, keep);
	CHECK(hlg.success && hlg.frame.hdr && hlg.frame.hdr->transfer == libfiles::hdr_transfer::hlg);
	CHECK(widened.wide_at(0, 0) == wide(128 * 257, 64 * 257, 255 * 257, 0xFFFF));
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

TEST("Should map a PNG's colours to sRGB by its iCCP profile when asked, in place of its gamma")
{
	png_spec spec;
	spec.width = 2;
	spec.height = 1;
	spec.icc = icc_corpus::display_p3();
	// Linear, so the gamma alone would lighten the picture: the profile's own curve takes its place.
	spec.gamma = 1.0;
	// Display P3's (187, 105, 62) is sRGB's (200, 100, 50); P3's red is past sRGB's, and clips to it.
	const auto file = write_png(spec, {{187, 105, 62, 255, 0, 0}});

	libfiles::decode_options options;
	options.map_to_srgb = true;
	png_sink mapped;
	const auto result = decode_file(file, mapped, options);
	CHECK(result.success && result.frame.profile_mapped);
	const auto first = mapped.at(0, 0);
	CHECK(std::abs(first[0] - 50) <= 2 && std::abs(first[1] - 100) <= 2 && std::abs(first[2] - 200) <= 2);
	CHECK(mapped.at(1, 0) == px(0, 0, 255, 255));
	CHECK(mapped.padding_kept());

	// Without the option the profile changes nothing: the gamma corrects the picture as it would without it.
	png_sink plain;
	CHECK(!decode_file(file, plain).frame.profile_mapped);
	spec.icc.clear();
	png_sink gamma_alone;
	CHECK(decode_file(write_png(spec, {{187, 105, 62, 255, 0, 0}}), gamma_alone).success);
	CHECK(plain.memory == gamma_alone.memory);
	CHECK(plain.at(0, 0) != mapped.at(0, 0));
}

TEST("Should say a PNG is HDR by the cICP chunk libpng keeps, as its decode finds it")
{
	const auto plain = write_png({}, rgb_rows(3, 2));
	const auto at_idat = find_chunk(plain, "IDAT");

	png_spec palette_spec;
	palette_spec.color_type = PNG_COLOR_TYPE_PALETTE;
	palette_spec.width = 3;
	palette_spec.height = 1;
	palette_spec.palette = {{10, 20, 30}, {40, 50, 60}};
	const auto palette = write_png(palette_spec, {{1, 0, 1}});

	// BT.2020 primaries, full range, and the transfer and matrix given.
	const auto cicp = [](const std::uint8_t transfer, const std::uint8_t matrix) -> bytes
	{
		return {9, transfer, matrix, 1};
	};
	const auto whole_then = splice(plain, at_idat, "cICP", cicp(16, 0));

	struct variant
	{
		const char* name;
		bytes file;
		bool hdr;
	};

	const variant variants[] = {
		{"no cICP", plain, false},
		{"PQ", splice(plain, at_idat, "cICP", cicp(16, 0)), true},
		{"HLG", splice(plain, at_idat, "cICP", cicp(18, 0)), true},
		{"sRGB's transfer", splice(plain, at_idat, "cICP", cicp(13, 0)), false},
		{"a matrix other than RGB's", splice(plain, at_idat, "cICP", cicp(16, 1)), false},
		{"a damaged CRC", splice(plain, at_idat, "cICP", cicp(16, 0), true), false},
		{"a damaged one, then a whole one", splice(whole_then, at_idat, "cICP", cicp(16, 0), true), true},
		{"after PLTE", splice(palette, find_chunk(palette, "IDAT"), "cICP", cicp(16, 0)), false},
		{"before PLTE", splice(palette, find_chunk(palette, "PLTE"), "cICP", cicp(16, 0)), true},
	};

	for (const auto& v : variants)
	{
		libfiles::reader r{std::span<const std::uint8_t>(v.file)};
		const auto scanned = libfiles::scan(r);
		png_sink sink;
		const auto decoded = decode_file(v.file, sink);

		if (!scanned.success || !decoded.success || scanned.hdr != v.hdr || decoded.frame.hdr.has_value() != v.hdr)
			std::printf("  %s: scanned %d, decoded %d, expected %d\n", v.name, scanned.hdr ? 1 : 0,
			            decoded.frame.hdr.has_value() ? 1 : 0, v.hdr ? 1 : 0);

		CHECK(scanned.success && decoded.success);
		CHECK(scanned.hdr == v.hdr);
		CHECK(decoded.frame.hdr.has_value() == v.hdr);
	}
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
