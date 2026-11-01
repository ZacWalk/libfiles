// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Native document and row encoder round trips. These cases intentionally retain packed
// rows, palette alpha/transparency and animation/page attributes rather than testing a display-BGRA
// conversion.

#include "test.h"
#include "builders.h"
#include "legacy.h"
#include "tiff_internal.h"

#include "libfiles/decode.h"
#include "libfiles/document.h"
#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "libfiles/scan.h"
#include "libfiles/xmp.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace
{
	libfiles::decoded_document indexed_document(const libfiles::document_storage storage,
	                                            const std::uint32_t width, const std::uint32_t height,
	                                            const std::vector<std::uint32_t>& palette)
	{
		libfiles::decoded_document document;
		document.info.width = width;
		document.info.height = height;
		document.info.storage = storage;
		document.info.bits_per_pixel = libfiles::storage_bits(storage);
		document.info.frames = 1;
		document.info.has_resolution = true;
		document.info.pixels_per_meter_x = 3780;
		document.info.pixels_per_meter_y = 1890;
		auto& frame = document.frames.emplace_back();
		frame.width = width;
		frame.height = height;
		frame.storage = storage;
		frame.stride = libfiles::document_row_bytes(width, storage);
		frame.palette = palette;
		frame.pixels.assign(frame.stride * height, 0);
		for (std::uint32_t y = 0; y < height; ++y)
		{
			auto* row = frame.pixels.data() + y * frame.stride;
			for (std::uint32_t x = 0; x < width; ++x)
			{
				const auto index = (x + 2 * y) % static_cast<std::uint32_t>(palette.size());
				if (storage == libfiles::document_storage::indexed1)
					row[x >> 3] |= static_cast<std::uint8_t>((index & 1) << (7 - (x & 7)));
				else if (storage == libfiles::document_storage::indexed2)
					row[x >> 2] |= static_cast<std::uint8_t>((index & 3) << (6 - 2 * (x & 3)));
				else if (storage == libfiles::document_storage::indexed4)
					row[x >> 1] |= static_cast<std::uint8_t>((index & 15) << (x & 1 ? 0 : 4));
				else
					row[x] = static_cast<std::uint8_t>(index);
			}
		}
		return document;
	}

	libfiles::decoded_document rgb_document(const std::uint32_t width, const std::uint32_t height,
	                                        const std::size_t channels = 3)
	{
		libfiles::decoded_document document;
		document.info.width = width;
		document.info.height = height;
		document.info.storage = channels == 4 ? libfiles::document_storage::bgra32 :
			libfiles::document_storage::bgr24;
		document.info.bits_per_pixel = static_cast<std::uint32_t>(channels * 8);
		document.info.frames = 1;
		document.info.has_resolution = true;
		document.info.pixels_per_meter_x = 4000;
		document.info.pixels_per_meter_y = 3000;
		auto& frame = document.frames.emplace_back();
		frame.width = width;
		frame.height = height;
		frame.storage = document.info.storage;
		frame.stride = libfiles::document_row_bytes(width, frame.storage);
		frame.pixels.assign(frame.stride * height, 0);
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
			{
				auto* pixel = frame.pixels.data() + y * frame.stride + x * channels;
				pixel[0] = static_cast<std::uint8_t>(x * 17);
				pixel[1] = static_cast<std::uint8_t>(y * 29);
				pixel[2] = static_cast<std::uint8_t>(100 + x + y);
				if (channels == 4) pixel[3] = static_cast<std::uint8_t>(32 + x * 7 + y);
			}
		return document;
	}

	libfiles::decoded_frame copy_frame(const libfiles::decoded_frame& source)
	{
		return source;
	}
}

TEST("native BMP encoder keeps packed indexed rows at odd widths")
{
	const auto source = indexed_document(libfiles::document_storage::indexed4, 5, 3,
	                                     {0xff000000, 0xffff0000, 0xff00ff00, 0xff0000ff,
	                                      0xffffff00, 0xffff00ff, 0xff00ffff, 0xffffffff,
	                                      0xff202020, 0xff404040, 0xff606060, 0xff808080,
	                                      0xffa0a0a0, 0xffc0c0c0, 0xffe0e0e0, 0xffffffff});
	const auto encoded = libfiles::encode_document(source, libfiles::image_format::bmp);
	CHECK(encoded.success);
	libfiles::reader input(encoded.data);
	libfiles::decoded_document decoded;
	CHECK(libfiles::decode_document(input, libfiles::image_format::bmp, decoded).success);
	CHECK_EQ(decoded.frames.size(), std::size_t{1});
	if (decoded.frames.size() != 1) return;
	CHECK(decoded.frames.front().storage == libfiles::document_storage::indexed4);
	CHECK(decoded.frames.front().pixels == source.frames.front().pixels);
}

TEST("native document encoders reject overflowing row extents before allocation")
{
	libfiles::decoded_document document;
	document.info.width = 1;
	document.info.height = 4;
	document.info.frames = 1;
	auto& frame = document.frames.emplace_back();
	frame.width = 1;
	frame.height = 4;
	frame.storage = libfiles::document_storage::bgr24;
	frame.stride = std::numeric_limits<std::size_t>::max() / 2;
	CHECK(!libfiles::encode_document(document, libfiles::image_format::bmp).success);

	libfiles::image_view rows;
	rows.pixels = reinterpret_cast<const std::uint8_t*>(1);
	rows.width = std::numeric_limits<std::uint32_t>::max();
	rows.height = 2;
	rows.stride = std::numeric_limits<std::size_t>::max();
	CHECK(!libfiles::encode_document(rows, libfiles::image_format::bmp).success);
}

TEST("native BMP encoder keeps legacy 555 and 565 bitfield headers")
{
	for (const auto storage : {libfiles::document_storage::bgr555, libfiles::document_storage::bgr565})
	{
		auto source = rgb_document(5, 3, 2);
		auto& frame = source.frames.front();
		frame.storage = storage;
		source.info.storage = storage;
		source.info.bits_per_pixel = libfiles::storage_bits(storage);
		frame.stride = libfiles::document_row_bytes(frame.width, storage);
		frame.pixels.assign(frame.stride * frame.height, 0);
		for (std::uint32_t y = 0; y < frame.height; ++y)
			for (std::uint32_t x = 0; x < frame.width; ++x)
			{
				const auto value = storage == libfiles::document_storage::bgr565
					? static_cast<std::uint16_t>(((x + 1) & 31) | (((y + 2) & 63) << 5) |
						(((x + y + 3) & 31) << 11))
					: static_cast<std::uint16_t>(((x + 1) & 31) | (((y + 2) & 31) << 5) |
						(((x + y + 3) & 31) << 10));
				std::memcpy(frame.pixels.data() + y * frame.stride + x * 2, &value, sizeof(value));
			}
		const auto encoded = libfiles::encode_document(source, libfiles::image_format::bmp);
		CHECK(encoded.success);
		if (!encoded.success) continue;
		CHECK_EQ(encoded.data[30] | (static_cast<unsigned>(encoded.data[31]) << 8) |
			(static_cast<unsigned>(encoded.data[32]) << 16) |
			(static_cast<unsigned>(encoded.data[33]) << 24), 3u);
		CHECK_EQ(encoded.data[10] | (static_cast<unsigned>(encoded.data[11]) << 8), 66u);
		libfiles::reader input(encoded.data);
		libfiles::decoded_document decoded;
		CHECK(libfiles::decode_document(input, libfiles::image_format::bmp, decoded).success);
		CHECK(!decoded.frames.empty() && decoded.frames.front().storage == storage);
	}
}

#ifdef LIBFILES_PNG
TEST("native PNG encoder keeps indexed palette alpha and metadata")
{
	auto source = indexed_document(libfiles::document_storage::indexed4, 5, 3,
	                               {0x00000000, 0xffff0000, 0xff00ff00, 0xff0000ff,
	                                0xffffffff, 0xff101010, 0xff202020, 0xff303030,
	                                0xff404040, 0xff505050, 0xff606060, 0xff707070,
	                                0xff808080, 0xff909090, 0xffa0a0a0, 0xffb0b0b0});
	source.frames.front().transparent = 0;
	source.metadata.push_back({libfiles::document_metadata::comment, {'c', 'm', 't'}});
	const auto encoded = libfiles::encode_document(source, libfiles::image_format::png);
	CHECK(encoded.success);
	if (!encoded.success) { CHECK_STR(encoded.error, ""); return; }
	libfiles::reader input(encoded.data);
	libfiles::decoded_document decoded;
	const auto result = libfiles::decode_document(input, libfiles::image_format::png, decoded);
	CHECK(result.success);
	if (!result.success) { CHECK_STR(result.error, ""); return; }
	CHECK_EQ(decoded.info.pixels_per_meter_x, 3780u);
	CHECK_EQ(decoded.info.pixels_per_meter_y, 1890u);
	CHECK_EQ(decoded.frames.size(), std::size_t{1});
	if (decoded.frames.size() != 1) return;
	CHECK(decoded.frames.front().palette[0] >> 24 == 0);
	CHECK(decoded.frames.front().transparent == std::optional<std::uint32_t>{0});
}

TEST("native PNG metadata retains simultaneous short text XMP EXIF profile and arbitrary keywords")
{
	auto source = rgb_document(9, 5);
	libfiles::exif_fields fields;
	fields.orientation = 6;
	source.metadata.push_back({libfiles::document_metadata::exif, libfiles::make_exif(fields)});
	libfiles::xmp::meta packet;
	packet.set_property_int(libfiles::xmp::ns::tiff, "Orientation", 6);
	const auto xmp = packet.serialize();
	source.metadata.push_back({libfiles::document_metadata::xmp, {xmp.begin(), xmp.end()}});
	source.metadata.push_back({libfiles::document_metadata::comment, {'c'}});
	source.metadata.push_back({libfiles::document_metadata::author, {'a'}});
	const auto profile = test_build::rgb_icc_profile_with_copyright();
	source.metadata.push_back({libfiles::document_metadata::icc, profile});
	const auto title = test_build::lit("tEXtTitle\0an arbitrary title");
	const auto international = test_build::lit("iTXtDescription\0\0\0en\0Description\0UTF-8 \xc3\xa9");
	source.metadata.push_back({libfiles::document_metadata::png_text, title});
	source.metadata.push_back({libfiles::document_metadata::png_text, international});
	libfiles::document_encode_options options;
	options.reset_orientation_metadata = true;
	for (int round = 0; round < 2; ++round)
	{
		const auto encoded = libfiles::encode_document(source, libfiles::image_format::png, options);
		CHECK(encoded.success);
		if (!encoded.success) { CHECK_STR(encoded.error, ""); return; }
		for (const auto& warning : encoded.warnings) CHECK_STR(warning, "");
		libfiles::reader scan_input(encoded.data);
		const auto scanned = libfiles::scan(scan_input);
		CHECK(scanned.success);
		CHECK(scanned.orientation == std::optional<std::uint16_t>{1});
		CHECK(scanned.metadata.icc == profile);
		libfiles::reader input(encoded.data);
		libfiles::decoded_document decoded;
		const auto result = libfiles::decode_document(input, libfiles::image_format::png, decoded);
		CHECK(result.success);
		for (const auto& warning : result.warnings) CHECK_STR(warning, "");
		const auto has = [&](const libfiles::document_metadata kind, const std::vector<std::uint8_t>& bytes)
		{
			return std::ranges::any_of(decoded.metadata, [&](const auto& block)
			{
				return block.kind == kind && block.bytes == bytes;
			});
		};
		CHECK(has(libfiles::document_metadata::comment, {'c'}));
		CHECK(has(libfiles::document_metadata::author, {'a'}));
		CHECK(has(libfiles::document_metadata::icc, profile));
		CHECK(has(libfiles::document_metadata::png_text, title));
		CHECK(has(libfiles::document_metadata::png_text, international));
		CHECK(std::ranges::any_of(decoded.metadata, [&](const auto& block)
		{
			if (block.kind != libfiles::document_metadata::xmp) return false;
			libfiles::xmp::meta metadata;
			metadata.parse(std::string_view(reinterpret_cast<const char*>(block.bytes.data()), block.bytes.size()));
			std::int32_t orientation = 0;
			return metadata.get_property_int(libfiles::xmp::ns::tiff, "Orientation", &orientation) && orientation == 1;
		}));
		source = std::move(decoded);
	}
	auto large = rgb_document(2048, 1024);
	large.metadata = source.metadata;
	const auto large_encoded = libfiles::encode_document(large, libfiles::image_format::png);
	CHECK(large_encoded.success);
	libfiles::document_options metadata_only;
	metadata_only.pixels = false;
	metadata_only.maximum_working_bytes = 4096;
	metadata_only.maximum_retained_bytes = 4096;
	libfiles::reader large_input(large_encoded.data);
	libfiles::decoded_document metadata;
	const auto header = libfiles::decode_document(large_input, libfiles::image_format::png,
		metadata, metadata_only);
	CHECK(header.success);
	CHECK_EQ(header.info.width, 2048u);
	CHECK_EQ(header.info.height, 1024u);
	CHECK_EQ(header.info.frames, 1u);
	CHECK_EQ(header.info.pixels_per_meter_x, large.info.pixels_per_meter_x);
	CHECK(metadata.frames.empty());
	CHECK(metadata.metadata.size() == source.metadata.size());
	metadata_only.pixels = true;
	libfiles::reader pixels_input(large_encoded.data);
	const auto refused_pixels = libfiles::decode_document(pixels_input, libfiles::image_format::png,
		metadata, metadata_only);
	CHECK(!refused_pixels.success && refused_pixels.resource_limit);
	source.metadata.push_back({libfiles::document_metadata::png_text, {'b', 'a', 'd'}});
	const auto invalid = libfiles::encode_document(source, libfiles::image_format::png);
	CHECK(!invalid.success);
	CHECK(!invalid.error.empty());
}
#endif

#ifdef LIBFILES_GIF
TEST("native GIF encoder keeps indexed frames offsets delay disposal and transparency")
{
	auto source = indexed_document(libfiles::document_storage::indexed2, 6, 4,
	                               {0x00000000, 0xffff0000, 0xff00ff00, 0xff0000ff});
	source.frames.front().transparent = 0;
	source.frames.front().delay_ms = 120;
	source.frames.front().disposal = 2;
	auto second = copy_frame(source.frames.front());
	second.left = 1;
	second.top = 1;
	second.width = 3;
	second.height = 3;
	second.delay_ms = 240;
	second.disposal = 3;
	source.frames.push_back(std::move(second));
	source.info.frames = 2;
	const auto encoded = libfiles::encode_document(source, libfiles::image_format::gif);
	CHECK(encoded.success);
	if (!encoded.success) { CHECK_STR(encoded.error, ""); return; }
	libfiles::reader input(encoded.data);
	libfiles::decoded_document decoded;
	const auto result = libfiles::decode_document(input, libfiles::image_format::gif, decoded);
	CHECK(result.success);
	CHECK_EQ(decoded.frames.size(), std::size_t{2});
	if (decoded.frames.size() != 2) return;
	CHECK_EQ(decoded.frames[0].left, 0);
	CHECK_EQ(decoded.frames[1].left, 1);
	CHECK(decoded.frames[0].delay_ms && *decoded.frames[0].delay_ms == 120);
	CHECK(decoded.frames[1].delay_ms && *decoded.frames[1].delay_ms == 240);
	CHECK(decoded.frames[1].disposal && *decoded.frames[1].disposal == 3);
	CHECK(decoded.frames[0].transparent && *decoded.frames[0].transparent == 0);
}
#endif

#ifdef LIBFILES_TIFF
TEST("native TIFF encoder keeps RGB pages and resolution")
{
	auto source = rgb_document(5, 3);
	std::vector<std::uint8_t> photoshop{'P','h','o','t','o','s','h','o','p',' ',
		'3','.', '0', 0, '8','B','I','M', 4, 4, 0, 0, 0, 0, 0, 3, 'a', 'b', 'c', 0};
	source.metadata.push_back({libfiles::document_metadata::photoshop, std::move(photoshop)});
	libfiles::exif_fields exif_fields;
	exif_fields.orientation = 6;
	const auto exif = libfiles::make_exif(exif_fields);
	source.metadata.push_back({libfiles::document_metadata::exif, exif});
	auto second = copy_frame(source.frames.front());
	second.pixels[0] ^= 0x33;
	source.frames.push_back(std::move(second));
	source.info.frames = 2;
	const auto encoded = libfiles::encode_document(source, libfiles::image_format::tiff);
	CHECK(encoded.success);
	if (!encoded.success) { CHECK_STR(encoded.error, ""); return; }
	libfiles::reader input(encoded.data);
	libfiles::decoded_document decoded;
	const auto result = libfiles::decode_document(input, libfiles::image_format::tiff, decoded);
	CHECK(result.success);
	CHECK_EQ(decoded.frames.size(), std::size_t{2});
	CHECK(decoded.info.has_resolution);
	CHECK_EQ(decoded.info.pixels_per_meter_x, 4000u);
	CHECK_EQ(decoded.info.pixels_per_meter_y, 3000u);
	CHECK(decoded.frames[0].pixels == source.frames[0].pixels);
	bool found_iptc = false;
	for (const auto& metadata : decoded.metadata)
		if (metadata.kind == libfiles::document_metadata::iptc)
			found_iptc |= metadata.bytes == std::vector<std::uint8_t>{'a', 'b', 'c'};
	CHECK(found_iptc);
	bool found_exif = false;
	for (const auto& metadata : decoded.metadata)
		if (metadata.kind == libfiles::document_metadata::exif)
			found_exif |= libfiles::read_exif(metadata.bytes).orientation == std::optional<std::uint16_t>{1};
	CHECK(found_exif);
	libfiles::document_encode_options legacy;
	legacy.legacy_compatibility = true;
	const auto historical = libfiles::encode_document(source, libfiles::image_format::tiff, legacy);
	CHECK(historical.success);
	if (historical.success)
	{
		libfiles::reader reader(historical.data);
		libfiles::detail::tiff_editor fields;
		CHECK(fields.open(reader));
		CHECK(!fields.has(libfiles::detail::tiff_editor::directory::ifd0, 255));
		const auto page = fields.value_range(libfiles::detail::tiff_editor::directory::ifd0, 297);
		CHECK(page.has_value() && page->size == 4);
		if (page && page->size == 4)
		{
			const auto bytes = reader.read(page->offset, 4);
			CHECK_EQ(libfiles::detail::load_le16(bytes.data()), 2u);
		}
	}
}

TEST("native TIFF metadata does not invent identity-only EXIF")
{
	const auto encoded = libfiles::encode_document(rgb_document(5, 3), libfiles::image_format::tiff);
	CHECK(encoded.success);
	if (!encoded.success) return;
	for (const int variant : {0, 1, 2})
	{
		libfiles::reader original(encoded.data);
		libfiles::detail::tiff_editor fields;
		CHECK(fields.open(original));
		if (variant == 1)
			fields.set_short(libfiles::detail::tiff_editor::directory::ifd0,
				libfiles::detail::tag_orientation, 6);
		if (variant == 2)
			fields.set_ascii(libfiles::detail::tiff_editor::directory::ifd0,
				libfiles::detail::tag_make, "recorded camera");
		const auto data = libfiles::detail::apply_changes(encoded.data, fields.changes(encoded.data.size()));
		libfiles::reader input(data);
		libfiles::document_options options;
		options.pixels = false;
		libfiles::decoded_document decoded;
		CHECK(libfiles::decode_document(input, libfiles::image_format::tiff, decoded, options).success);
		const auto exif = std::find_if(decoded.metadata.begin(), decoded.metadata.end(), [](const auto& value)
		{
			return value.kind == libfiles::document_metadata::exif;
		});
		CHECK((exif != decoded.metadata.end()) == (variant != 0));
		if (exif != decoded.metadata.end())
		{
			const auto info = libfiles::read_exif(exif->bytes);
			CHECK(info.orientation == std::optional<std::uint16_t>{variant == 1 ? 6u : 1u});
			if (variant == 2) CHECK_STR(info.camera_make, "recorded camera");
		}
	}
}

TEST("native TIFF unassociated alpha and historical opaque storage are explicit reader policies")
{
	const auto source = rgb_document(5, 3, 4);
	const auto encoded = libfiles::encode_document(source, libfiles::image_format::tiff);
	CHECK(encoded.success);
	libfiles::reader input(encoded.data);
	libfiles::decoded_document alpha;
	CHECK(libfiles::decode_document(input, libfiles::image_format::tiff, alpha).success);
	CHECK(alpha.frames.size() == 1 && alpha.frames.front().storage == libfiles::document_storage::bgra32);
	libfiles::document_options options;
	options.legacy_tiff_storage = true;
	libfiles::decoded_document opaque;
	CHECK(libfiles::decode_document(input, libfiles::image_format::tiff, opaque, options).success);
	CHECK(opaque.frames.size() == 1 && opaque.frames.front().storage == libfiles::document_storage::bgrx32);
	if (alpha.frames.size() == 1 && opaque.frames.size() == 1)
	{
		CHECK(alpha.frames.front().pixels == opaque.frames.front().pixels);
		std::vector<std::uint32_t> pixels(15);
		libfiles::copy_bgra(opaque.frames.front().rows(), pixels, 5, false);
		CHECK(std::ranges::all_of(pixels, [](const auto value) { return value >> 24 == 255; }));
	}
}

TEST("native TIFF metadata uses standard EXIF GPS and interoperability directories in either byte order")
{
	for (const bool big_endian : {false, true})
	{
		auto exif = big_endian
			? std::vector<std::uint8_t>{'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1,
				1, 18, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0, 0, 0, 0, 0}
			: libfiles::detail::tiff_editor::empty_stream();
		using directory = libfiles::detail::tiff_editor::directory;
		libfiles::reader original(exif);
		libfiles::detail::tiff_editor metadata;
		CHECK(metadata.open(original));
		metadata.set_short(directory::ifd0, libfiles::detail::tag_orientation, 6);
		metadata.set_ascii(directory::ifd0, libfiles::detail::tag_make, "TestCam");
		metadata.set_ascii(directory::ifd0, libfiles::detail::tag_model, "QA-9000");
		metadata.set_ascii(directory::exif, libfiles::detail::tag_date_time_original, "2025:07:14 10:20:30");
		metadata.set_short(directory::exif, libfiles::detail::tag_pixel_x_dimension, 7);
		metadata.set_short(directory::exif, libfiles::detail::tag_pixel_y_dimension, 11);
		const std::vector<std::uint8_t> note{'o', 'p', 'a', 'q', 'u', 'e', 0, 1, 255};
		metadata.set(directory::exif, libfiles::detail::tag_maker_note,
			libfiles::detail::tiff_editor::type_undefined, static_cast<std::uint32_t>(note.size()), note);
		metadata.set_ascii(directory::gps, libfiles::detail::tag_gps_latitude_ref, "N");
		metadata.set_rationals(directory::gps, libfiles::detail::tag_gps_latitude, {{37, 1}, {30, 1}, {0, 1}});
		metadata.set_ascii(directory::gps, libfiles::detail::tag_gps_longitude_ref, "W");
		metadata.set_rationals(directory::gps, libfiles::detail::tag_gps_longitude, {{122, 1}, {15, 1}, {0, 1}});
		metadata.set_ascii(directory::interoperability, 1, "R98");
		exif = libfiles::detail::apply_changes(exif, metadata.changes(exif.size()));
		auto source = rgb_document(9, 5);
		source.metadata.push_back({libfiles::document_metadata::exif, exif});
		libfiles::document_encode_options options;
		options.reset_orientation_metadata = true;
		const auto encoded = libfiles::encode_document(source, libfiles::image_format::tiff, options);
		CHECK(encoded.success);
		if (!encoded.success) { CHECK_STR(encoded.error, ""); continue; }
		libfiles::reader input(encoded.data);
		const auto scanned = libfiles::scan(input);
		CHECK(scanned.success);
		CHECK_EQ(scanned.width, 9u);
		CHECK_EQ(scanned.height, 5u);
		CHECK(scanned.orientation == std::optional<std::uint16_t>{1});
		CHECK(scanned.tiff_metadata.has_value());
		if (scanned.tiff_metadata)
		{
			CHECK_STR(scanned.tiff_metadata->camera_make, "TestCam");
			CHECK_STR(scanned.tiff_metadata->original.text, "2025:07:14 10:20:30");
			CHECK(scanned.tiff_metadata->latitude == std::optional<double>{37.5});
			CHECK(scanned.tiff_metadata->longitude == std::optional<double>{-122.25});
		}
		libfiles::detail::tiff_editor encoded_metadata;
		CHECK(encoded_metadata.open(input));
		CHECK(!encoded_metadata.has(directory::ifd0, 65000));
		const auto maker = encoded_metadata.value_range(directory::exif, libfiles::detail::tag_maker_note);
		CHECK(maker.has_value());
		if (maker) CHECK(input.read(maker->offset, static_cast<std::size_t>(maker->size)) == note);
		for (const auto& [tag, expected] : {
			std::pair{libfiles::detail::tag_pixel_x_dimension, 9u},
			std::pair{libfiles::detail::tag_pixel_y_dimension, 5u}})
		{
			const auto dimension = encoded_metadata.value_range(directory::exif, tag);
			CHECK(dimension.has_value() && dimension->size == 4);
			if (dimension && dimension->size == 4)
			{
				const auto bytes = input.read(dimension->offset, 4);
				CHECK_EQ(encoded_metadata.little_endian() ? libfiles::detail::load_le32(bytes.data()) :
					libfiles::detail::load_be32(bytes.data()), expected);
			}
		}
		const auto interoperability = encoded_metadata.value_range(directory::interoperability, 1);
		CHECK(interoperability.has_value());
		if (interoperability)
			CHECK(input.read(interoperability->offset, static_cast<std::size_t>(interoperability->size)) ==
				test_build::lit("R98\0"));
		libfiles::decoded_document decoded;
		CHECK(libfiles::decode_document(input, libfiles::image_format::tiff, decoded).success);
		CHECK(!decoded.frames.empty() && decoded.frames.front().pixels == source.frames.front().pixels);
		CHECK(std::ranges::any_of(decoded.metadata, [&](const auto& block)
		{
			if (block.kind != libfiles::document_metadata::exif) return false;
			const auto info = libfiles::read_exif(block.bytes);
			return info.camera_make == "TestCam" && info.original.text == "2025:07:14 10:20:30" &&
				info.orientation == std::optional<std::uint16_t>{1};
		}));
		libfiles::document_options metadata_only;
		metadata_only.pixels = false;
		metadata_only.maximum_working_bytes = 4096;
		metadata_only.maximum_retained_bytes = 4096;
		const auto metadata_result = libfiles::decode_document(input, libfiles::image_format::tiff,
			decoded, metadata_only);
		CHECK(metadata_result.success);
		if (!metadata_result.success)
		{
			CHECK_STR(metadata_result.error, "");
			for (const auto& warning : metadata_result.warnings) CHECK_STR(warning, "");
		}
		CHECK(decoded.frames.empty());
		CHECK(std::ranges::any_of(decoded.metadata, [](const auto& block)
		{
			return block.kind == libfiles::document_metadata::exif &&
				libfiles::read_exif(block.bytes).camera_make == "TestCam";
		}));
	}
}

#ifdef LIBFILES_TIFF
TEST("native TIFF encoder keeps every packed and alpha storage's displayed pixels")
{
	const auto palette = std::vector<std::uint32_t>{
		0xff102030, 0xff405060, 0xff708090, 0xffa0b0c0,
		0xffd0e0f0, 0xff203040, 0xff506070, 0xff8090a0,
		0xffb0c0d0, 0xffe0f000, 0xff304050, 0xff607080,
		0xff90a0b0, 0xffc0d0e0, 0xfff01020, 0xff506070};
	for (const auto storage : {libfiles::document_storage::indexed1, libfiles::document_storage::indexed2,
		libfiles::document_storage::indexed4, libfiles::document_storage::indexed8,
		libfiles::document_storage::indexed8_alpha})
	{
		auto colors = palette;
		if (storage == libfiles::document_storage::indexed8 ||
			storage == libfiles::document_storage::indexed8_alpha)
			colors.resize(256, palette.front());
		auto source = indexed_document(storage, 9, 5, colors);
		if (storage == libfiles::document_storage::indexed8_alpha)
			for (std::size_t i = 0; i < source.frames.front().palette.size(); ++i)
				source.frames.front().palette[i] =
					(source.frames.front().palette[i] & 0x00ffffffu) | (i & 1 ? 0x80000000u : 0xff000000u);
		const auto encoded = libfiles::encode_document(source, libfiles::image_format::tiff);
		CHECK(encoded.success);
		if (!encoded.success) continue;
		libfiles::reader input(encoded.data);
		libfiles::decoded_document decoded;
		CHECK(libfiles::decode_document(input, libfiles::image_format::tiff, decoded).success);
		if (decoded.frames.empty()) continue;
		std::vector<std::uint32_t> expected(45), actual(45);
		libfiles::copy_bgra(source.frames.front().rows(), expected, 9, false);
		libfiles::copy_bgra(decoded.frames.front().rows(), actual, 9, false);
		CHECK(expected == actual);
	}
}
#endif
#endif

#ifdef LIBFILES_JPEG
TEST("native JPEG row encoder and explicit coefficient transform report distinct outcomes")
{
	const auto source = rgb_document(64, 32);
	libfiles::image_view view{source.frames.front().pixels.data(), source.frames.front().stride,
	                          source.info.width, source.info.height, libfiles::pixel_format::bgrx8};
	// The row view is four-byte storage, so make a compact copy only for this test's input.
	std::vector<std::uint8_t> bgrx(static_cast<std::size_t>(view.width) * view.height * 4);
	for (std::uint32_t y = 0; y < view.height; ++y)
		for (std::uint32_t x = 0; x < view.width; ++x)
			std::memcpy(bgrx.data() + (static_cast<std::size_t>(y) * view.width + x) * 4,
			            source.frames.front().pixels.data() + y * source.frames.front().stride + x * 3, 3);
	view.pixels = bgrx.data();
	view.stride = static_cast<std::size_t>(view.width) * 4;
	const auto encoded = libfiles::encode_document(view, libfiles::image_format::jpeg);
	CHECK(encoded.success);
	libfiles::jpeg_transformer density_transformer;
	libfiles::jpeg_encoder density_encoder;
	libfiles::jpeg_transform_options density_options;
	const auto unchanged = density_transformer.transform(encoded.data, density_encoder, density_options);
	CHECK(unchanged.data.size() >= 18 && unchanged.data[13] == encoded.data[13]);
	density_options.density = {libfiles::jpeg_density_unit::centimetres, 65535, 1};
	const auto overridden = density_transformer.transform(encoded.data, density_encoder, density_options);
	CHECK(overridden.data.size() >= 18);
	if (overridden.data.size() >= 18)
	{
		CHECK_EQ(overridden.data[13], 2u);
		CHECK_EQ(overridden.data[14] * 256u + overridden.data[15], 65535u);
		CHECK_EQ(overridden.data[16] * 256u + overridden.data[17], 1u);
	}
	density_options.density->x = 0;
	CHECK(density_transformer.transform(encoded.data, density_encoder, density_options).invalid);
	auto density_source = rgb_document(9, 5);
	density_source.info.has_resolution = true;
	density_source.info.pixels_per_meter_x = 0xffffffffu;
	density_source.info.pixels_per_meter_y = 0;
	const auto density_encoded = libfiles::encode_document(density_source, libfiles::image_format::jpeg);
	CHECK(density_encoded.success && density_encoded.data.size() >= 18);
	if (density_encoded.success && density_encoded.data.size() >= 18)
	{
		CHECK(density_encoded.data[13] == 0);
		CHECK_EQ(static_cast<unsigned>((density_encoded.data[14] << 8) | density_encoded.data[15]), 1u);
		CHECK_EQ(static_cast<unsigned>((density_encoded.data[16] << 8) | density_encoded.data[17]), 1u);
	}
	libfiles::document_encode_options legacy_options;
	legacy_options.legacy_compatibility = true;
	const auto legacy_density = libfiles::encode_document(
		density_source, libfiles::image_format::jpeg, legacy_options);
	CHECK(legacy_density.success && legacy_density.data.size() >= 18);
	if (legacy_density.success && legacy_density.data.size() >= 18)
	{
		CHECK(legacy_density.data[13] == 1);
		CHECK_EQ(static_cast<unsigned>((legacy_density.data[14] << 8) | legacy_density.data[15]), 65535u);
		CHECK_EQ(static_cast<unsigned>((legacy_density.data[16] << 8) | legacy_density.data[17]), 1u);
	}
	libfiles::jpeg_transformer transformer;
	libfiles::jpeg_encoder encoder;
	libfiles::jpeg_transform_options options;
	options.transform = libfiles::jpeg_transform::rotate90;
	CHECK(!transformer.transform(encoded.data, encoder, options).data.empty());
	options.maximum_bytes = 1;
	const auto limited = transformer.transform(encoded.data, encoder, options);
	CHECK(limited.data.empty());
	CHECK(limited.over_budget);
	options.maximum_bytes = 256ull * 1024 * 1024;
	options.cancelled = [] { return true; };
	CHECK(transformer.transform(encoded.data, encoder, options).cancelled);
	const auto partial_source = rgb_document(60, 30);
	const auto partial_encoded = libfiles::encode_document(partial_source, libfiles::image_format::jpeg);
	options.cancelled = {};
	options.policy = libfiles::jpeg_transform_policy::partial_blocks;
	CHECK(!transformer.transform(partial_encoded.data, encoder, options).data.empty());
}

TEST("native encoders can reset EXIF and XMP orientation after an upright edit")
{
	auto source = rgb_document(9, 5);
	libfiles::exif_fields fields;
	fields.orientation = 6;
	const auto exif = libfiles::make_exif(fields);
	source.metadata.push_back({libfiles::document_metadata::exif, exif});
	libfiles::xmp::meta xmp;
	xmp.set_property_int(libfiles::xmp::ns::tiff, "Orientation", 6);
	const auto packet = xmp.serialize();
	source.metadata.push_back({libfiles::document_metadata::xmp,
		std::vector<std::uint8_t>(packet.begin(), packet.end())});
	libfiles::document_encode_options options;
	options.reset_orientation_metadata = true;
	const auto encoded = libfiles::encode_document(source, libfiles::image_format::jpeg, options);
	CHECK(encoded.success);
	if (!encoded.success) return;
	libfiles::reader input(encoded.data);
	const auto scanned = libfiles::scan(input);
	CHECK(scanned.success);
	CHECK(scanned.orientation == std::optional<std::uint16_t>{1});
	libfiles::xmp::meta round_trip;
	round_trip.parse(std::string_view(reinterpret_cast<const char*>(scanned.metadata.xmp.data()),
	                                  scanned.metadata.xmp.size()));
	std::int32_t orientation = 0;
	CHECK(round_trip.get_property_int(libfiles::xmp::ns::tiff, "Orientation", &orientation));
	CHECK_EQ(orientation, 1);
	auto invalid = source;
	invalid.metadata.front().bytes = {'b', 'a', 'd'};
	const auto refused = libfiles::encode_document(invalid, libfiles::image_format::jpeg, options);
	CHECK(!refused.success && !refused.error.empty());
	CHECK(source.metadata.front().bytes == exif);
	libfiles::image_view rows{source.frames.front().pixels.data(), source.frames.front().stride,
		source.info.width, source.info.height, libfiles::pixel_format::bgrx8};
	auto rgba = rgb_document(9, 5, 4);
	rows.pixels = rgba.frames.front().pixels.data();
	rows.stride = rgba.frames.front().stride;
	const std::vector<std::uint8_t> damaged{'b', 'a', 'd'};
	libfiles::embedded_metadata bad;
	bad.exif = damaged;
	const auto refused_rows = libfiles::encode_document(rows, libfiles::image_format::jpeg, {}, bad, options);
	CHECK(!refused_rows.success && !refused_rows.error.empty());
}

TEST("native JPEG metadata-only reads do not decode progressive coefficient rasters")
{
	const auto source = rgb_document(128, 128);
	libfiles::document_encode_options encode_options;
	encode_options.progressive = true;
	const auto encoded = libfiles::encode_document(source, libfiles::image_format::jpeg, encode_options);
	CHECK(encoded.success);
	libfiles::document_options options;
	options.pixels = false;
	options.maximum_working_bytes = 4096;
	options.maximum_retained_bytes = 4096;
	libfiles::reader input(encoded.data);
	libfiles::decoded_document document;
	const auto metadata = libfiles::decode_document(input, libfiles::image_format::jpeg, document, options);
	CHECK(metadata.success);
	CHECK_EQ(metadata.info.width, 128u);
	CHECK_EQ(metadata.info.height, 128u);
	CHECK_EQ(metadata.info.frames, 1u);
	CHECK(document.frames.empty());
	options.pixels = true;
	libfiles::reader full(encoded.data);
	const auto pixels = libfiles::decode_document(full, libfiles::image_format::jpeg, document, options);
	CHECK(!pixels.success && pixels.resource_limit);
}
#endif
