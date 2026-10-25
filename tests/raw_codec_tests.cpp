// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for the camera raw codecs libfiles builds for its consumers: LibRaw opening and developing
// the DNG SDK's own sample DNGs through the SDK - a JPEG XL compressed one among them, which LibRaw cannot
// read alone - and the SDK's XMP read and written through libfiles' XMP data model, its backend.

#include "test.h"

#ifdef LIBFILES_RAW

#include "builders.h"

#define LIBRAW_NODLL 1
#define USE_DNGSDK 1

#include "dng_host.h"
#include "dng_memory.h"
#include "dng_string.h"
#include "dng_xmp.h"

// LibRaw redefines the DNG SDK's feature flags for its own headers; its values are the ones the SDK above
// was built with, so the redefinition is only silenced.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4005)
#endif
#include <libraw/libraw.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "libfiles/xmp.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace test_build;

namespace
{
	bytes read_sample(const char* const name)
	{
		std::ifstream in(std::string(LIBFILES_DNG_SAMPLES) + "/" + name, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	struct developed
	{
		bool opened = false;
		bool dng = false;
		bool unpacked = false;
		bool processed = false;
		int width = 0;
		int height = 0;
		int colors = 0;
	};

	// Opened, unpacked and developed at half size by LibRaw, with a DNG host so that it can hand a DNG to
	// the SDK.
	developed develop(const bytes& file)
	{
		developed d;
		dng_host host;
		const auto processor = std::make_unique<LibRaw>();
		processor->set_dng_host(&host);

		d.opened = processor->open_buffer(file.data(), file.size()) == LIBRAW_SUCCESS;
		if (!d.opened) return d;

		d.dng = processor->imgdata.idata.dng_version != 0;
		d.unpacked = processor->unpack() == LIBRAW_SUCCESS;
		if (!d.unpacked) return d;

		processor->imgdata.params.half_size = 1;
		d.processed = processor->dcraw_process() == LIBRAW_SUCCESS;
		if (!d.processed) return d;

		auto error = 0;
		if (auto* const image = processor->dcraw_make_mem_image(&error))
		{
			d.width = image->width;
			d.height = image->height;
			d.colors = image->colors;
			LibRaw::dcraw_clear_mem(image);
		}

		processor->recycle();
		return d;
	}
}

TEST("Should develop the DNG SDK's sample DNGs with LibRaw, through the DNG SDK")
{
	for (const auto* const name : {"05_PGTM2_unsigned8.dng", "06_PGTM2_unsigned16.dng", "08_PGTM2_float32.dng"})
	{
		const auto d = develop(read_sample(name));
		CHECK(d.opened && d.dng && d.unpacked && d.processed);
		CHECK(d.width > 0 && d.height > 0);
		CHECK_EQ(d.colors, 3);
	}

	// Cut short, it does not develop.
	auto cut = read_sample("05_PGTM2_unsigned8.dng");
	cut.resize(cut.size() / 3);
	const auto d = develop(cut);
	CHECK(!(d.opened && d.unpacked && d.processed && d.width > 0));
}

// Only where libjxl is optimised: unoptimised, decoding the sample's 4667 x 7000 image takes it a minute
// and a half, and the route from LibRaw through the SDK to libjxl is the same in either build.
#ifdef NDEBUG
TEST("Should develop a JPEG XL compressed DNG, which LibRaw hands to the DNG SDK and libjxl")
{
	const auto d = develop(read_sample("02_jxl_linear_raw_float.dng"));
	CHECK(d.opened && d.dng && d.unpacked && d.processed);
	CHECK(d.width > 0 && d.height > 0);
}
#endif

TEST("Should read and write the DNG SDK's XMP through libfiles' XMP data model")
{
	dng_host host;
	dng_xmp xmp(host.Allocator());
	const std::string packet =
		R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about="" xmlns:tiff="http://ns.adobe.com/tiff/1.0/" tiff:Make="Diffractor"/>)"
		R"(</rdf:RDF></x:xmpmeta>)";
	xmp.Parse(host, packet.data(), static_cast<uint32>(packet.size()));

	dng_string make;
	CHECK(xmp.GetString(XMP_NS_TIFF, "Make", make));
	CHECK(std::string(make.Get()) == "Diffractor");

	dng_string model;
	model.Set("libfiles");
	xmp.SetString(XMP_NS_TIFF, "Model", model);

	const std::unique_ptr<dng_memory_block> block(xmp.Serialize());
	CHECK(block != nullptr);
	if (!block) return;

	libfiles::xmp::meta written;
	written.parse({block->Buffer_char(), block->LogicalSize()});
	std::string value;
	CHECK(written.get_property(libfiles::xmp::ns::tiff, "Make", &value) && value == "Diffractor");
	CHECK(written.get_property(libfiles::xmp::ns::tiff, "Model", &value) && value == "libfiles");
}

#endif
