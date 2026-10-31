// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for scanning camera raw files: what an index reads, as LibRaw parses it, the date the
// camera's clock gave, the listing an inspector shows, the thumbnail a scan hands back or writes, the rule
// a GPS position must meet, and LibRaw's flip read as an orientation.

#include "test.h"

#ifdef LIBFILES_RAW

#include "builders.h"
#include "libfiles/raw.h"
#include "raw_internal.h"

#include <algorithm>
#include <cstring>
#include <ctime>
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

	struct thumbnail_sink final : libfiles::image_sink
	{
		bytes memory;
		std::size_t stride = 0;
		int calls = 0;
		libfiles::frame_info told;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			++calls;
			told = frame;
			stride = std::size_t{4} * frame.width;
			memory.assign(stride * frame.height, 0xAB);
			return {memory.data(), stride};
		}
	};

	// What LibRaw itself parses of a file, sized as a scan sizes it.
	struct parsed
	{
		std::unique_ptr<LibRaw> processor = std::make_unique<LibRaw>();
		bool read = false;

		explicit parsed(const std::filesystem::path& path)
		{
			read = processor->open_file(path.c_str()) == LIBRAW_SUCCESS &&
				processor->adjust_sizes_info_only() == LIBRAW_SUCCESS;
		}

		const libraw_data_t& data() const { return processor->imgdata; }
	};
}

TEST("Should scan a raw file's size, camera and exposure as LibRaw parses them, without developing it")
{
	for (const auto* const name : {"12_ImageStats_WeightedAverage.dng", "01_jxl_linear_raw_integer.dng"})
	{
		const parsed expected(sample(name));
		CHECK(expected.read);

		const auto scanned = libfiles::scan_raw(sample(name), libfiles::scan_intent::index, nullptr);
		CHECK(scanned.success);
		CHECK_EQ(scanned.width, static_cast<std::uint32_t>(expected.data().sizes.width));
		CHECK_EQ(scanned.height, static_cast<std::uint32_t>(expected.data().sizes.height));
		CHECK(scanned.width > 0 && scanned.height > 0);
		CHECK_EQ(scanned.orientation, 1u);
		CHECK_STR(scanned.camera_make, std::string(expected.data().idata.make));
		CHECK_STR(scanned.camera_model, std::string(expected.data().idata.model));
		CHECK_STR(scanned.pixel_format, std::string(expected.data().idata.cdesc));
		CHECK(scanned.f_number == expected.data().other.aperture);
		CHECK(scanned.exposure_time == expected.data().other.shutter);
		CHECK(scanned.focal_length == expected.data().other.focal_len);
		CHECK_EQ(scanned.xmp.size(), static_cast<std::size_t>(expected.data().idata.xmplen));
		// An index asks for no listing, and with no sink, no thumbnail.
		CHECK(scanned.listing.empty());
		CHECK(scanned.thumbnail_jpeg.empty() && !scanned.thumbnail.success);

		// The date is the one LibRaw made its timestamp from: taken back to the camera's clock, it makes
		// the same timestamp again.
		if (expected.data().other.timestamp != 0)
		{
			CHECK(scanned.created.has_value());
			std::tm clock{};
			clock.tm_year = scanned.created->year - 1900;
			clock.tm_mon = scanned.created->month - 1;
			clock.tm_mday = scanned.created->day;
			clock.tm_hour = scanned.created->hour;
			clock.tm_min = scanned.created->minute;
			clock.tm_sec = scanned.created->second;
			clock.tm_isdst = -1;
			CHECK(std::mktime(&clock) == expected.data().other.timestamp);
		}
		else
		{
			CHECK(!scanned.created.has_value());
		}
	}
}

TEST("Should list what LibRaw parsed by section, each named with what it holds, the empty ones left out")
{
	const auto scanned = libfiles::scan_raw(sample("12_ImageStats_WeightedAverage.dng"), libfiles::scan_intent::inspect,
	                                        nullptr);
	CHECK(scanned.success);
	CHECK(scanned.listing.size() > 10);
	CHECK(!scanned.listing.empty() && scanned.listing.front().container);
	CHECK(!scanned.listing.empty() && scanned.listing.front().key.starts_with("Identification ("));

	auto sections = 0;

	for (std::size_t i = 0; i < scanned.listing.size(); ++i)
	{
		const auto& row = scanned.listing[i];

		if (row.container)
		{
			++sections;
			std::size_t count = 0;
			while (i + 1 + count < scanned.listing.size() && !scanned.listing[i + 1 + count].container) ++count;

			CHECK_EQ(row.depth, 0);
			CHECK(row.id.starts_with("raw."));
			CHECK(row.value.empty());
			CHECK(count > 0 && row.key.ends_with(" (" + std::to_string(count) + ")"));
		}
		else
		{
			CHECK_EQ(row.depth, 1);
			CHECK(!row.key.empty() && !row.value.empty());
		}
	}

	CHECK(sections > 3);
}

TEST("Should hand back a raw file's JPEG thumbnail, and write a bitmap one to the sink turned")
{
	thumbnail_sink jpeg_sink;
	const auto jpeg = libfiles::scan_raw(sample("04_PGTM2_per_profile.dng"), libfiles::scan_intent::index, &jpeg_sink);
	CHECK(jpeg.thumbnail_jpeg.size() > 16 && jpeg.thumbnail_jpeg[0] == 0xFF && jpeg.thumbnail_jpeg[1] == 0xD8);
	CHECK_EQ(jpeg_sink.calls, 0);
	CHECK_EQ(jpeg.image_width, jpeg.width);
	CHECK_EQ(jpeg.image_height, jpeg.height);
	CHECK_EQ(jpeg.image_orientation, 1u);

	const parsed bitmap_file(sample("01_jxl_linear_raw_integer.dng"));
	CHECK(bitmap_file.processor->unpack_thumb() == LIBRAW_SUCCESS);
	const auto& expected = bitmap_file.data().thumbnail;
	CHECK(expected.tformat == LIBRAW_THUMBNAIL_BITMAP && expected.tcolors == 3);

	thumbnail_sink bitmap_sink;
	const auto bitmap = libfiles::scan_raw(sample("01_jxl_linear_raw_integer.dng"), libfiles::scan_intent::index,
	                                       &bitmap_sink);
	CHECK(bitmap.thumbnail_jpeg.empty() && bitmap.thumbnail.success);
	CHECK_EQ(bitmap_sink.calls, 1);
	CHECK_EQ(static_cast<int>(bitmap_sink.told.width), static_cast<int>(expected.twidth));
	CHECK_EQ(static_cast<int>(bitmap_sink.told.height), static_cast<int>(expected.theight));
	CHECK_EQ(bitmap_sink.told.orientation,
	         libfiles::raw_preview_orientation(expected.twidth, expected.theight, 1, bitmap.width, bitmap.height,
	                                           bitmap.orientation));

	auto same = expected.thumb != nullptr;
	for (std::size_t i = 0; same && i < std::size_t{expected.twidth} * expected.theight; ++i)
	{
		const auto* const got = bitmap_sink.memory.data() + i * 4;
		const auto* const want = reinterpret_cast<const std::uint8_t*>(expected.thumb) + i * 3;
		same = got[0] == want[2] && got[1] == want[1] && got[2] == want[0] && got[3] == 0;
	}
	CHECK(same);
}

TEST("Should not scan what LibRaw cannot open")
{
	const auto folder = std::filesystem::temp_directory_path() / "libfiles-raw-scan-tests";
	std::filesystem::create_directories(folder);
	const auto text = folder / "not-raw.dng";
	std::ofstream(text, std::ios::binary) << "This is not a raw file, whatever its name says.";

	thumbnail_sink sink;
	const auto scanned = libfiles::scan_raw(text, libfiles::scan_intent::inspect, &sink);
	CHECK(!scanned.success && scanned.listing.empty() && scanned.width == 0);
	CHECK(scanned.thumbnail_jpeg.empty() && !scanned.thumbnail.success);
	CHECK_EQ(sink.calls, 0);

	std::filesystem::remove_all(folder);
}

// Diffractor's SRC-022: a coordinate of zero is a position when the other has a fix.
TEST("Should take a GPS position LibRaw parsed only in range, on the equator and the meridian too")
{
	CHECK(libfiles::detail::raw_gps_fix(true, 0.0, 37.0667));
	CHECK(libfiles::detail::raw_gps_fix(true, 51.4778, 0.0));
	CHECK(libfiles::detail::raw_gps_fix(true, 0.0, -37.0667));
	CHECK(libfiles::detail::raw_gps_fix(true, -51.4778, 0.0));

	CHECK(!libfiles::detail::raw_gps_fix(false, 0.0, 37.0667));
	CHECK(!libfiles::detail::raw_gps_fix(true, 0.0, 0.0));
	CHECK(!libfiles::detail::raw_gps_fix(true, 91.0, 10.0));
	CHECK(!libfiles::detail::raw_gps_fix(true, 10.0, 180.0));

	const float dms[3] = {51.0f, 28.0f, 40.0f};
	CHECK(libfiles::detail::raw_gps_degrees(dms) == 51.0 + 28.0 / 60.0 + 40.0 / 3600.0);
}

TEST("Should take LibRaw's GPS position with south and west negative, where it has a fix")
{
	libraw_gps_info_t gps{};
	gps.gpsparsed = 1;
	gps.latitude[0] = 51.0f;
	gps.latitude[1] = 28.0f;
	gps.latitude[2] = 40.0f;
	gps.longitude[2] = 5.0f;
	gps.latref = 'N';
	gps.longref = 'W';

	auto position = libfiles::detail::raw_gps_position(gps);
	CHECK(position.has_value());
	CHECK(position && position->latitude == 51.0 + 28.0 / 60.0 + 40.0 / 3600.0);
	CHECK(position && position->longitude == -(5.0 / 3600.0));

	gps.latref = 'S';
	gps.longref = 'E';
	position = libfiles::detail::raw_gps_position(gps);
	CHECK(position && position->latitude == -(51.0 + 28.0 / 60.0 + 40.0 / 3600.0));
	CHECK(position && position->longitude == 5.0 / 3600.0);

	gps.gpsparsed = 0;
	CHECK(!libfiles::detail::raw_gps_position(gps).has_value());
}

TEST("Should take LibRaw's flip as the orientation it describes, numbered as EXIF numbers them")
{
	constexpr std::uint16_t expected[] = {1, 2, 4, 3, 5, 8, 6, 7};
	for (auto flip = 0; flip < 8; ++flip) CHECK_EQ(libfiles::detail::orientation_of_flip(flip), expected[flip]);
	CHECK_EQ(libfiles::detail::orientation_of_flip(8), std::uint16_t{1});
	CHECK_EQ(libfiles::detail::orientation_of_flip(-1), std::uint16_t{1});
}

namespace
{
	bytes file_bytes(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	// A stream over memory that counts the reads it is asked for, as a network share charges for them.
	class counting_stream final : public libfiles::stream
	{
	public:
		explicit counting_stream(bytes data) : _data(std::move(data))
		{
		}

		std::uint64_t size() override { return _data.size(); }

		std::size_t read(const std::uint64_t offset, void* data, const std::size_t size) override
		{
			++reads;
			if (offset >= _data.size()) return 0;
			const auto n = std::min<std::size_t>(size, _data.size() - static_cast<std::size_t>(offset));
			std::memcpy(data, _data.data() + offset, n);
			return n;
		}

		int reads = 0;

	private:
		bytes _data;
	};

	// A stream that says it holds the whole file but supplies only its start, as a share that drops does.
	class failing_stream final : public libfiles::stream
	{
	public:
		failing_stream(const bytes& data, const std::uint64_t fails_from) : _data(data), _fails_from(fails_from)
		{
		}

		std::uint64_t size() override { return _data.size(); }

		std::size_t read(const std::uint64_t offset, void* data, const std::size_t size) override
		{
			if (offset >= _fails_from) return 0;
			const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(size, _fails_from - offset));
			std::memcpy(data, _data.data() + offset, n);
			return n;
		}

	private:
		const bytes& _data;
		std::uint64_t _fails_from = 0;
	};
}

TEST("Should scan a raw file read through a reader as it scans the file by its path")
{
	for (const auto* const name : {"12_ImageStats_WeightedAverage.dng", "01_jxl_linear_raw_integer.dng", "04_PGTM2_per_profile.dng"})
	{
		const auto path = sample(name);
		const auto file = file_bytes(path);
		CHECK(file.size() > 1024);

		thumbnail_sink by_path_sink;
		thumbnail_sink by_reader_sink;
		const auto by_path = libfiles::scan_raw(path, libfiles::scan_intent::inspect, &by_path_sink);
		libfiles::reader memory(file);
		const auto by_reader = libfiles::scan_raw(memory, libfiles::scan_intent::inspect, &by_reader_sink, path);

		CHECK(by_path.success && by_reader.success);
		CHECK_EQ(by_reader.width, by_path.width);
		CHECK_EQ(by_reader.height, by_path.height);
		CHECK_EQ(by_reader.orientation, by_path.orientation);
		CHECK(by_reader.xmp == by_path.xmp);
		CHECK(by_reader.created.has_value() == by_path.created.has_value());
		CHECK_STR(by_reader.pixel_format, by_path.pixel_format);
		CHECK_STR(by_reader.camera_make, by_path.camera_make);
		CHECK_STR(by_reader.camera_model, by_path.camera_model);
		CHECK_EQ(by_reader.iso_speed, by_path.iso_speed);
		CHECK(by_reader.exposure_time == by_path.exposure_time && by_reader.f_number == by_path.f_number);
		CHECK_EQ(by_reader.listing.size(), by_path.listing.size());
		CHECK(by_reader.thumbnail_jpeg == by_path.thumbnail_jpeg);
		CHECK(by_reader_sink.memory == by_path_sink.memory);

		for (std::size_t i = 0; i < std::min(by_reader.listing.size(), by_path.listing.size()); ++i)
		{
			CHECK_STR(by_reader.listing[i].key + "=" + by_reader.listing[i].value, by_path.listing[i].key + "=" + by_path.listing[i].value);
		}

		// Over a stream, LibRaw's reads go through the reader's window: a read of the stream for each block
		// they touch, not for each of LibRaw's own.
		counting_stream counted(file);
		libfiles::reader streamed(counted);
		const auto through_stream = libfiles::scan_raw(streamed, libfiles::scan_intent::index, nullptr);
		CHECK(through_stream.success);
		CHECK_EQ(through_stream.width, by_path.width);
		CHECK(counted.reads <= static_cast<int>(file.size() / libfiles::reader::default_block_size) + 8);
	}

	// A file cut short scans as nothing, and so does one whose stream fails: neither throws.
	const auto file = file_bytes(sample("12_ImageStats_WeightedAverage.dng"));
	const bytes cut(file.begin(), file.begin() + 512);
	libfiles::reader short_file(cut);
	CHECK(!libfiles::scan_raw(short_file, libfiles::scan_intent::index, nullptr).success);

	failing_stream failing(file, 512);
	libfiles::reader dropped(failing);
	const auto through_failure = libfiles::scan_raw(dropped, libfiles::scan_intent::inspect, nullptr);
	CHECK(!through_failure.success && through_failure.width == 0 && through_failure.listing.empty());
}

#endif
