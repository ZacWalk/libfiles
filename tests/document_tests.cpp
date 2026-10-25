// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Native storage, row boundaries, refusal and malformed image handling.

#include "test.h"
#include "builders.h"
#include "libfiles/document.h"

#include <algorithm>
#include <cstring>

namespace
{
	struct captured_document final : libfiles::document_sink
	{
		struct frame
		{
			libfiles::document_frame info;
			std::vector<std::uint32_t> palette;
			std::vector<std::uint8_t> pixels;
		};
		std::vector<frame> frames;
		bool refuse = false;
		bool stop = false;
		bool cancel_after_row = false;

		bool begin_frame(const libfiles::document_frame& info) override
		{
			if (refuse) return false;
			frame stored{info, {info.palette.begin(), info.palette.end()}, {}};
			stored.info.palette = {};
			stored.pixels.resize(libfiles::document_row_bytes(info.width, info.storage) * info.height);
			frames.push_back(std::move(stored));
			return true;
		}
		void row(std::uint32_t y, std::span<const std::uint8_t> bytes) override
		{
			auto& frame = frames.back();
			const auto stride = libfiles::document_row_bytes(frame.info.width, frame.info.storage);
			CHECK(y < frame.info.height);
			CHECK(bytes.size() >= stride);
			std::copy_n(bytes.data(), stride, frame.pixels.data() + y * stride);
			if (cancel_after_row) stop = true;
		}
		bool cancelled() override { return stop; }
	};

	std::vector<std::uint8_t> monochrome_pcx()
	{
		std::vector<std::uint8_t> bytes(128, 0);
		bytes[0] = 0x0a;
		bytes[1] = 5;
		bytes[2] = 1;
		bytes[3] = 1;
		bytes[8] = 2;
		bytes[10] = 1;
		bytes[65] = 1;
		bytes[66] = 2;
		bytes.insert(bytes.end(), {0xa0, 0, 0x40, 0});
		return bytes;
	}
}

TEST("native WPG levels preserve RGB rows and reject incomplete rasters")
{
	for (const unsigned level : {1u, 2u})
		for (const unsigned compression : {0u, 1u})
		{
			if (level == 1 && compression == 0) continue;
			std::vector<std::uint8_t> raster;
			for (unsigned y = 0; y < 2; ++y)
			{
				if (compression) raster.push_back(level == 1 ? 9 : 8);
				for (unsigned i = 1; i <= 9; ++i) raster.push_back(static_cast<std::uint8_t>(i + y * 10));
			}
			test_build::builder file;
			file.le32(0x435057ff).le32(18).le16(0x1600).le16(level);
			file.b.insert(file.b.end(), {1, 0, 0, 0, 0, 0});
			if (level == 1)
				file.b.insert(file.b.end(), {0x0b, static_cast<std::uint8_t>(10 + raster.size())});
			else
				file.b.insert(file.b.end(), {0, 0x0e, 0, static_cast<std::uint8_t>(6 + raster.size())});
			file.le16(3).le16(2);
			if (level == 1) file.le16(24).le16(0).le16(0);
			else file.b.insert(file.b.end(), {8, static_cast<std::uint8_t>(compression)});
			file.b.insert(file.b.end(), raster.begin(), raster.end());
			libfiles::reader input(file.b);
			captured_document sink;
			const auto result = libfiles::decode_document(input, libfiles::image_format::wpg, sink);
			CHECK(result.success && result.info.frames == 1);
			CHECK(sink.frames.size() == 1);
			if (sink.frames.size() != 1) continue;
			CHECK(sink.frames[0].pixels == std::vector<std::uint8_t>({
				3, 2, 1, 6, 5, 4, 9, 8, 7, 0, 0, 0,
				13, 12, 11, 16, 15, 14, 19, 18, 17, 0, 0, 0}));
			for (std::size_t length = 0; length < file.b.size(); ++length)
			{
				libfiles::reader cut(std::span<const std::uint8_t>(file.b.data(), length));
				captured_document failed;
				CHECK(!libfiles::decode_document(cut, libfiles::image_format::wpg, failed).success);
			}
		}
}

TEST("native PCX preserves packed rows and the monochrome palette")
{
	libfiles::memory_stream stream(monochrome_pcx());
	libfiles::reader reader(stream);
	captured_document sink;
	const auto result = libfiles::decode_document(reader, libfiles::image_format::pcx, sink);
	CHECK(result.success);
	CHECK(result.info.frames == 1);
	CHECK(sink.frames.size() == 1);
	if (sink.frames.size() != 1) return;
	const auto& frame = sink.frames.front();
	CHECK(frame.info.width == 3);
	CHECK(frame.info.height == 2);
	CHECK(frame.info.storage == libfiles::document_storage::indexed1);
	CHECK(frame.palette == std::vector<std::uint32_t>({0xff000000, 0xffffffff}));
	CHECK(frame.pixels == std::vector<std::uint8_t>({0xa0, 0, 0, 0, 0x40, 0, 0, 0}));
}

TEST("native PCX rejects every incomplete raster and a zero-length run")
{
	const auto good = monochrome_pcx();
	for (std::size_t length = 0; length < good.size(); ++length)
	{
		libfiles::memory_stream stream({good.begin(), good.begin() + length});
		libfiles::reader reader(stream);
		captured_document sink;
		const auto result = libfiles::decode_document(reader, libfiles::image_format::pcx, sink);
		CHECK(!result.success);
		CHECK(!result.error.empty());
	}
	auto broken = good;
	broken[128] = 0xc0;
	libfiles::memory_stream stream(std::move(broken));
	libfiles::reader reader(stream);
	captured_document sink;
	CHECK(!libfiles::decode_document(reader, libfiles::image_format::pcx, sink).success);
}

TEST("native decoding distinguishes refusal cancellation and working-memory limits")
{
	libfiles::memory_stream stream(monochrome_pcx());
	libfiles::reader reader(stream);
	captured_document sink;
	sink.refuse = true;
	const auto refused = libfiles::decode_document(reader, libfiles::image_format::pcx, sink);
	CHECK(!refused.success && refused.refused);
	CHECK(sink.frames.empty());
	sink.refuse = false;
	sink.cancel_after_row = true;
	const auto cancelled = libfiles::decode_document(reader, libfiles::image_format::pcx, sink);
	CHECK(!cancelled.success && cancelled.cancelled);
	sink.stop = false;
	libfiles::document_options options;
	options.maximum_working_bytes = 1;
	const auto limited = libfiles::decode_document(reader, libfiles::image_format::pcx, sink, options);
	CHECK(!limited.success && limited.resource_limit && !limited.error.empty());
}

TEST("native BMP honors row order zero pixel offsets and zero resolution")
{
	for (const int height : {2, -2})
	{
		test_build::builder file;
		file.raw("BM").le32(1).le32(0).le32(0).le32(40).le32(3).le32(static_cast<std::uint32_t>(height))
			.le16(1).le16(24).le32(0).le32(24).le32(0).le32(0).le32(0).le32(0);
		file.b.insert(file.b.end(), {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0,
			10, 11, 12, 13, 14, 15, 16, 17, 18, 0, 0, 0});
		libfiles::reader input(file.b);
		captured_document sink;
		const auto result = libfiles::decode_document(input, libfiles::image_format::bmp, sink);
		CHECK(result.success && result.info.frames == 1);
		CHECK(result.info.has_resolution && result.info.pixels_per_meter_x == 0 && result.info.pixels_per_meter_y == 0);
		CHECK(sink.frames.size() == 1);
		if (sink.frames.size() != 1) continue;
		const auto& frame = sink.frames.front();
		CHECK(frame.info.storage == libfiles::document_storage::bgr24);
		CHECK(frame.pixels[0] == (height < 0 ? 1 : 10));
		CHECK(frame.pixels[12] == (height < 0 ? 10 : 1));
		for (std::size_t length = 0; length < file.b.size(); ++length)
		{
			libfiles::reader cut(std::span<const std::uint8_t>(file.b.data(), length));
			captured_document rejected;
			CHECK(!libfiles::decode_document(cut, libfiles::image_format::bmp, rejected).success);
		}
	}
}

#ifdef LIBFILES_GIF
namespace
{
	std::vector<std::uint8_t> native_gif()
	{
		std::vector<std::uint8_t> bytes{'G', 'I', 'F', '8', '9', 'a', 3, 0, 2, 0, 0x81, 0, 0,
			0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
		for (const unsigned index : {1u, 2u})
		{
			bytes.insert(bytes.end(), {0x2c, 0, 0, 0, 0, 3, 0, 2, 0, 0, 2});
			std::vector<std::uint8_t> raster;
			unsigned bits = 0;
			const auto emit = [&](const unsigned code)
			{
				for (unsigned bit = 0; bit < 3; ++bit, ++bits)
				{
					if (bits % 8 == 0) raster.push_back(0);
					raster.back() |= static_cast<std::uint8_t>(((code >> bit) & 1) << (bits % 8));
				}
			};
			for (int i = 0; i < 6; ++i) { emit(4); emit(index); }
			emit(5);
			bytes.push_back(static_cast<std::uint8_t>(raster.size()));
			bytes.insert(bytes.end(), raster.begin(), raster.end());
			bytes.push_back(0);
		}
		bytes.push_back(0x3b);
		return bytes;
	}
}

TEST("native GIF preserves complete frames palettes and control extensions")
{
	auto file = native_gif();
	file.insert(file.begin() + 25, {0x21, 0xf9, 4, 9, 5, 0, 1, 0});
	libfiles::reader input(file);
	captured_document sink;
	const auto result = libfiles::decode_document(input, libfiles::image_format::gif, sink);
	CHECK(result.success && result.info.frames == 2);
	CHECK(sink.frames.size() == 2);
	if (sink.frames.size() != 2) return;
	CHECK(sink.frames[0].palette[1] == 0xffff0000);
	CHECK(sink.frames[1].palette[2] == 0xff00ff00);
	CHECK(sink.frames[0].info.transparent == 1);
	CHECK(sink.frames[0].info.delay_ms == 50);
	CHECK(sink.frames[0].info.disposal == 2);
	CHECK(!sink.frames[1].info.transparent);

	const auto good = native_gif();
	for (const std::size_t length : {good.size() - 1, std::size_t{25 + 18 + 14}})
	{
		libfiles::reader cut(std::span<const std::uint8_t>(good.data(), length));
		captured_document partial;
		const auto kept = libfiles::decode_document(cut, libfiles::image_format::gif, partial);
		CHECK(kept.success);
		CHECK(kept.info.frames == (length == good.size() - 1 ? 2u : 1u));
		CHECK(partial.frames.size() == kept.info.frames);
		CHECK(kept.warnings.empty() == (length == good.size() - 1));
	}
	libfiles::reader cut(std::span<const std::uint8_t>(good.data(), 25 + 14));
	captured_document failed;
	CHECK(!libfiles::decode_document(cut, libfiles::image_format::gif, failed).success);
}
#endif

#ifdef LIBFILES_TIFF
TEST("native TIFF decodes strips and keeps complete pages before a damaged directory")
{
	test_build::builder file;
	file.raw("II").le16(42).le32(8).le16(13);
	const auto tag = [&](unsigned id, unsigned type, unsigned count, unsigned value)
	{
		file.le16(id).le16(type).le32(count).le32(value);
	};
	tag(256, 4, 1, 3);
	tag(257, 4, 1, 2);
	tag(258, 3, 3, 170);
	tag(259, 3, 1, 1);
	tag(262, 3, 1, 2);
	tag(273, 4, 1, 192);
	tag(277, 3, 1, 3);
	tag(278, 4, 1, 2);
	tag(279, 4, 1, 18);
	tag(282, 5, 1, 176);
	tag(283, 5, 1, 184);
	tag(284, 3, 1, 1);
	tag(296, 3, 1, 2);
	file.le32(0).le16(8).le16(8).le16(8).le32(72).le32(1).le32(72).le32(1);
	for (int i = 0; i < 6; ++i) file.b.insert(file.b.end(), {31, 61, 101});
	for (const bool damaged : {false, true})
	{
		auto bytes = file.b;
		if (damaged) std::fill_n(bytes.begin() + 166, 4, 0xff);
		libfiles::reader input(bytes);
		captured_document sink;
		const auto result = libfiles::decode_document(input, libfiles::image_format::tiff, sink);
		CHECK(result.success && result.info.frames == 1);
		CHECK(result.info.has_resolution && result.info.pixels_per_meter_x == 2834);
		CHECK(result.warnings.empty() == !damaged);
		CHECK(sink.frames.size() == 1);
		if (sink.frames.size() != 1) continue;
		CHECK(sink.frames[0].info.storage == libfiles::document_storage::bgr24);
		CHECK(sink.frames[0].pixels[0] == 101);
		CHECK(sink.frames[0].pixels[1] == 61);
		CHECK(sink.frames[0].pixels[2] == 31);
	}
	auto huge_resolution = file.b;
	std::fill_n(huge_resolution.begin() + 176, 4, 0xff);
	libfiles::reader huge_input(huge_resolution);
	captured_document huge_sink;
	const auto huge = libfiles::decode_document(huge_input, libfiles::image_format::tiff, huge_sink);
	CHECK(huge.success && huge.info.pixels_per_meter_x == 0x80000000u);
	CHECK(!huge.warnings.empty());
	libfiles::reader cut(std::span<const std::uint8_t>(file.b.data(), file.b.size() - 1));
	captured_document failed;
	const auto result = libfiles::decode_document(cut, libfiles::image_format::tiff, failed);
	CHECK(!result.success && !result.error.empty());
}
#endif
