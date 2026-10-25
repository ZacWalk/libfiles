// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for loading camera raw files: the DNG SDK's own sample DNGs developed into the sink to the
// pixels LibRaw's own dcraw_make_mem_image makes of them, their embedded previews taken as they are and
// turned as the raw data says, and what load_raw reports for a size refused, a file LibRaw cannot open,
// and a file cut short.

#include "test.h"

#ifdef LIBFILES_RAW

#include "builders.h"
#include "libfiles/raw.h"

#define LIBRAW_NODLL 1
#define USE_DNGSDK 1

#include "dng_host.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4005)
#endif
#include <libraw/libraw.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

using namespace test_build;

namespace
{
	std::filesystem::path sample(const char* const name)
	{
		return std::filesystem::path(LIBFILES_DNG_SAMPLES) / name;
	}

	// Memory filled with a byte the developer never writes, with padding after each row.
	struct raw_sink final : libfiles::image_sink
	{
		bytes memory;
		std::size_t stride = 0;
		bool refuse_declared = false;
		int accepts = 0;
		int calls = 0;
		libfiles::frame_info declared;
		libfiles::frame_info told;

		bool accept(const libfiles::frame_info& frame) override
		{
			++accepts;
			declared = frame;
			return !refuse_declared;
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			++calls;
			told = frame;
			stride = std::size_t{4} * frame.width + 8;
			memory.assign(stride * frame.height, 0xAB);
			return {memory.data(), stride};
		}
	};

	// What LibRaw's own developer makes of a file: 8-bit RGB, with the camera's white balance.
	struct reference
	{
		int width = 0;
		int height = 0;
		int colors = 0;
		int flip = 0;
		bytes rgb;
	};

	reference develop_with_libraw(const std::filesystem::path& path)
	{
		reference result;
		dng_host host;
		const auto processor = std::make_unique<LibRaw>();
		processor->set_dng_host(&host);
		processor->imgdata.params.use_camera_wb = 1;

		if (processor->open_file(path.c_str()) != LIBRAW_SUCCESS || processor->unpack() != LIBRAW_SUCCESS ||
			processor->dcraw_process() != LIBRAW_SUCCESS)
			return result;

		result.flip = processor->imgdata.sizes.flip;
		auto error = 0;

		if (auto* const image = processor->dcraw_make_mem_image(&error))
		{
			if (image->bits == 8)
			{
				result.width = image->width;
				result.height = image->height;
				result.colors = image->colors;
				result.rgb.assign(image->data, image->data + image->data_size);
			}

			LibRaw::dcraw_clear_mem(image);
		}

		processor->recycle();
		return result;
	}
}

TEST("Should develop a raw file into the sink to the pixels LibRaw's own developer makes")
{
	// An 8-bit DNG, which LibRaw hands the DNG SDK, and one with colour in it. Most of the SDK's samples
	// develop to white, or grey, in which red and blue could be swapped unseen.
	for (const auto* const name : {"05_PGTM2_unsigned8.dng", "12_ImageStats_WeightedAverage.dng"})
	{
		const auto expected = develop_with_libraw(sample(name));
		CHECK(expected.width > 0 && expected.colors == 3);
		CHECK_EQ(expected.flip, 0);

		if (std::string_view(name).starts_with("12_"))
		{
			auto coloured = false;
			for (std::size_t i = 0; !coloured && i + 2 < expected.rgb.size(); i += 3)
				coloured = expected.rgb[i] != expected.rgb[i + 2];
			CHECK(coloured);
		}

		raw_sink sink;
		const auto result = libfiles::load_raw(sample(name), sink, false);
		CHECK(result.success && !result.preview && result.jpeg.empty());
		CHECK(result.decoded.success);
		CHECK_EQ(sink.accepts, 1);
		CHECK_EQ(sink.calls, 1);
		CHECK(sink.told.format == libfiles::pixel_format::bgrx8);
		CHECK_EQ(static_cast<int>(sink.told.width), expected.width);
		CHECK_EQ(static_cast<int>(sink.told.height), expected.height);
		CHECK_EQ(result.orientation, 1u);

		auto same = expected.rgb.size() == std::size_t{3} * sink.told.width * sink.told.height;

		for (std::size_t y = 0; same && y < sink.told.height; ++y)
		{
			for (std::size_t x = 0; same && x < sink.told.width; ++x)
			{
				const auto* const got = sink.memory.data() + y * sink.stride + x * 4;
				const auto* const want = expected.rgb.data() + (y * sink.told.width + x) * 3;
				same = got[0] == want[2] && got[1] == want[1] && got[2] == want[0] && got[3] == 0;
			}
		}

		CHECK(same);
		// Nothing past each row's pixels is written.
		CHECK(sink.memory[sink.stride - 1] == 0xAB);
	}
}

TEST("Should take a raw file's JPEG preview as it is, rather than develop it")
{
	raw_sink sink;
	const auto result = libfiles::load_raw(sample("04_PGTM2_per_profile.dng"), sink, true);
	CHECK(result.success && result.preview);
	CHECK(result.jpeg.size() > 16 && result.jpeg[0] == 0xFF && result.jpeg[1] == 0xD8);
	CHECK_EQ(sink.accepts, 0);
	CHECK_EQ(sink.calls, 0);
	CHECK_EQ(result.orientation, 1u);
}

TEST("Should write a raw file's bitmap preview to the sink as BGRX")
{
	const auto path = sample("01_jxl_linear_raw_integer.dng");

	// The thumbnail as LibRaw unpacks it: RGB.
	dng_host host;
	const auto processor = std::make_unique<LibRaw>();
	processor->set_dng_host(&host);
	CHECK(processor->open_file(path.c_str()) == LIBRAW_SUCCESS && processor->unpack_thumb() == LIBRAW_SUCCESS);
	const auto& thumbnail = processor->imgdata.thumbnail;
	CHECK(thumbnail.tformat == LIBRAW_THUMBNAIL_BITMAP && thumbnail.tcolors == 3);

	raw_sink sink;
	const auto result = libfiles::load_raw(path, sink, true);
	CHECK(result.success && result.preview && result.jpeg.empty());
	CHECK_EQ(sink.accepts, 0);
	CHECK_EQ(sink.calls, 1);
	CHECK_EQ(static_cast<int>(sink.told.width), static_cast<int>(thumbnail.twidth));
	CHECK_EQ(static_cast<int>(sink.told.height), static_cast<int>(thumbnail.theight));
	CHECK(sink.told.format == libfiles::pixel_format::bgrx8);

	auto same = thumbnail.thumb != nullptr;
	for (std::size_t y = 0; same && y < sink.told.height; ++y)
	{
		for (std::size_t x = 0; same && x < sink.told.width; ++x)
		{
			const auto* const got = sink.memory.data() + y * sink.stride + x * 4;
			const auto* const want = reinterpret_cast<const std::uint8_t*>(thumbnail.thumb) + (y * sink.told.width + x) * 3;
			same = got[0] == want[2] && got[1] == want[1] && got[2] == want[0] && got[3] == 0;
		}
	}
	CHECK(same);
}

TEST("Should refuse a raw file the sink refuses at its declared size, before developing it")
{
	raw_sink sink;
	sink.refuse_declared = true;
	const auto result = libfiles::load_raw(sample("05_PGTM2_unsigned8.dng"), sink, false);
	CHECK(!result.success && result.decoded.refused);
	CHECK_EQ(sink.accepts, 1);
	CHECK_EQ(sink.calls, 0);
	CHECK(sink.declared.source_width > 0 && sink.declared.source_height > 0);
}

TEST("Should not load what LibRaw cannot open, nor a raw file cut short")
{
	const auto folder = std::filesystem::temp_directory_path() / "libfiles-raw-tests";
	std::filesystem::create_directories(folder);

	const auto text = folder / "not-raw.dng";
	std::ofstream(text, std::ios::binary) << "This is not a raw file, whatever its name says.";
	raw_sink text_sink;
	const auto not_raw = libfiles::load_raw(text, text_sink, true);
	CHECK(!not_raw.success && !not_raw.decoded.error.empty());
	CHECK_EQ(text_sink.calls, 0);

	std::ifstream in(sample("05_PGTM2_unsigned8.dng"), std::ios::binary);
	const bytes whole{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	const auto cut = folder / "cut.dng";
	std::ofstream(cut, std::ios::binary).write(reinterpret_cast<const char*>(whole.data()),
	                                           static_cast<std::streamsize>(whole.size() / 3));
	raw_sink cut_sink;
	const auto cut_result = libfiles::load_raw(cut, cut_sink, false);
	CHECK(!cut_result.success);

	std::filesystem::remove_all(folder);
}

TEST("Should show a raw file's preview turned as the raw data is, unless it is the raw image's shape as shown")
{
	// A square raw image says nothing of the preview's shape.
	CHECK_EQ(libfiles::raw_preview_orientation(160, 120, 3, 4000, 4000, 6), 3);
	// A preview stored upright keeps its own orientation, as does one that states the raw data's turn.
	CHECK_EQ(libfiles::raw_preview_orientation(120, 160, 1, 4000, 3000, 6), 1);
	CHECK_EQ(libfiles::raw_preview_orientation(160, 120, 6, 4000, 3000, 6), 6);
	// One stored as the sensor saw it, stating no turn, takes the raw data's; and one stating a turn it
	// does not need takes the raw data's none.
	CHECK_EQ(libfiles::raw_preview_orientation(160, 120, 1, 4000, 3000, 6), 6);
	CHECK_EQ(libfiles::raw_preview_orientation(160, 120, 8, 4000, 3000, 1), 1);
	// Shapes are compared in tenths: 1.45 is not 3 to 2, but 1.55 is.
	CHECK_EQ(libfiles::raw_preview_orientation(145, 100, 2, 3000, 2000, 1), 1);
	CHECK_EQ(libfiles::raw_preview_orientation(155, 100, 2, 3000, 2000, 1), 2);
	// A preview of no size takes the raw data's.
	CHECK_EQ(libfiles::raw_preview_orientation(0, 100, 3, 3000, 2000, 1), 1);
}

#endif
