// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests that the codecs libfiles builds work as their own APIs promise: libjpeg-turbo at 8
// bits through its SIMD paths, and at 12 and 16 bits through the separately compiled libraries; zlib-ng
// through the zlib API, against libfiles' own inflate and CRC-32; libpng and libwebp, against libfiles'
// scanner; and libheif, decoding libheif's own examples through libde265 and dav1d and round-tripping
// an image through its uncompressed codec.

#include "test.h"

#ifdef LIBFILES_JPEG

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include <jpeglib.h>

namespace
{
	[[noreturn]] void throw_error(const j_common_ptr cinfo)
	{
		char message[JMSG_LENGTH_MAX];
		(*cinfo->err->format_message)(cinfo, message);
		throw std::runtime_error(message);
	}

	void quiet(j_common_ptr)
	{
	}

	// A JPEG of width by height at the given precision: lossless when predictor is non-zero, else
	// baseline (8 bits) or extended (12 bits) DCT. Samples come from sample(x, y, component).
	template <typename Sample>
	std::vector<std::uint8_t> encode(const int precision, const int width, const int height, const int components,
	                                 const int predictor, Sample&& sample)
	{
		jpeg_compress_struct c{};
		jpeg_error_mgr err{};
		c.err = jpeg_std_error(&err);
		err.error_exit = throw_error;
		err.output_message = quiet;
		jpeg_create_compress(&c);

		unsigned char* out = nullptr;
		unsigned long out_size = 0;
		jpeg_mem_dest(&c, &out, &out_size);

		c.image_width = static_cast<JDIMENSION>(width);
		c.image_height = static_cast<JDIMENSION>(height);
		c.input_components = components;
		c.in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_RGB;
		c.data_precision = precision;
		jpeg_set_defaults(&c);
		if (predictor) jpeg_enable_lossless(&c, predictor, 0);
		else jpeg_set_quality(&c, 90, TRUE);
		jpeg_start_compress(&c, TRUE);

		const auto row_samples = static_cast<std::size_t>(width) * components;
		std::vector<std::uint16_t> row16(row_samples);
		std::vector<JSAMPLE> row8(row_samples);

		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				for (int i = 0; i < components; ++i)
				{
					const auto v = sample(x, y, i);
					row16[static_cast<std::size_t>(x) * components + i] = static_cast<std::uint16_t>(v);
					row8[static_cast<std::size_t>(x) * components + i] = static_cast<JSAMPLE>(v);
				}
			}

			if (precision <= 8)
			{
				JSAMPROW rows[1] = {row8.data()};
				jpeg_write_scanlines(&c, rows, 1);
			}
			else if (precision <= 12)
			{
				std::vector<J12SAMPLE> row12(row16.begin(), row16.end());
				J12SAMPROW rows[1] = {row12.data()};
				jpeg12_write_scanlines(&c, rows, 1);
			}
			else
			{
				J16SAMPROW rows[1] = {row16.data()};
				jpeg16_write_scanlines(&c, rows, 1);
			}
		}

		jpeg_finish_compress(&c);
		jpeg_destroy_compress(&c);

		std::vector<std::uint8_t> result(out, out + out_size);
		std::free(out);
		return result;
	}

	// Every sample of the image the JPEG holds, row after row, at its own precision.
	std::vector<int> decode(const std::vector<std::uint8_t>& file, int& precision)
	{
		jpeg_decompress_struct d{};
		jpeg_error_mgr err{};
		d.err = jpeg_std_error(&err);
		err.error_exit = throw_error;
		err.output_message = quiet;
		jpeg_create_decompress(&d);
		jpeg_mem_src(&d, file.data(), static_cast<unsigned long>(file.size()));
		jpeg_read_header(&d, TRUE);
		jpeg_start_decompress(&d);

		precision = d.data_precision;
		const auto row_samples = static_cast<std::size_t>(d.output_width) * d.output_components;
		std::vector<int> samples;
		std::vector<JSAMPLE> row8(row_samples);
		std::vector<J12SAMPLE> row12(row_samples);
		std::vector<J16SAMPLE> row16(row_samples);

		while (d.output_scanline < d.output_height)
		{
			if (precision <= 8)
			{
				JSAMPROW rows[1] = {row8.data()};
				jpeg_read_scanlines(&d, rows, 1);
				samples.insert(samples.end(), row8.begin(), row8.end());
			}
			else if (precision <= 12)
			{
				J12SAMPROW rows[1] = {row12.data()};
				jpeg12_read_scanlines(&d, rows, 1);
				samples.insert(samples.end(), row12.begin(), row12.end());
			}
			else
			{
				J16SAMPROW rows[1] = {row16.data()};
				jpeg16_read_scanlines(&d, rows, 1);
				samples.insert(samples.end(), row16.begin(), row16.end());
			}
		}

		jpeg_finish_decompress(&d);
		jpeg_destroy_decompress(&d);
		return samples;
	}
}

TEST("libjpeg-turbo round-trips 8-bit colour close to its input")
{
	const auto sample = [](const int x, const int y, const int c) { return c == 0 ? x * 4 : c == 1 ? y * 5 : 128; };
	const auto file = encode(8, 64, 48, 3, 0, sample);
	CHECK(file.size() > 100);

	int precision = 0;
	const auto samples = decode(file, precision);
	CHECK_EQ(precision, 8);
	CHECK_EQ(samples.size(), std::size_t{64} * 48 * 3);

	auto worst = 0;
	for (std::size_t i = 0; i < samples.size(); ++i)
	{
		const auto pixel = i / 3;
		const auto expected = sample(static_cast<int>(pixel % 64), static_cast<int>(pixel / 64), static_cast<int>(i % 3));
		worst = std::max(worst, std::abs(samples[i] - expected));
	}
	CHECK(worst <= 8);
}

TEST("libjpeg-turbo's 12- and 16-bit libraries round-trip lossless jpeg exactly")
{
	for (const auto precision : {12, 16})
	{
		const auto top = (1 << precision) - 1;
		const auto sample = [top](const int x, const int y, const int c) { return (x * 977 + y * 131 + c * 4099) % (top + 1); };
		const auto file = encode(precision, 33, 17, 3, 1, sample);

		int decoded_precision = 0;
		const auto samples = decode(file, decoded_precision);
		CHECK_EQ(decoded_precision, precision);
		CHECK_EQ(samples.size(), std::size_t{33} * 17 * 3);

		auto exact = true;
		for (std::size_t i = 0; i < samples.size(); ++i)
		{
			const auto pixel = i / 3;
			exact = exact && samples[i] == sample(static_cast<int>(pixel % 33), static_cast<int>(pixel / 33), static_cast<int>(i % 3));
		}
		CHECK(exact);
	}

	// 12-bit DCT, which the 12-bit library's IDCT decodes close to its input.
	const auto smooth = [](const int x, const int y, const int) { return 1000 + x * 20 + y * 10; };
	int precision = 0;
	const auto samples = decode(encode(12, 32, 32, 1, 0, smooth), precision);
	CHECK_EQ(precision, 12);
	auto worst = 0;
	for (std::size_t i = 0; i < samples.size(); ++i) worst = std::max(worst, std::abs(samples[i] - smooth(static_cast<int>(i % 32), static_cast<int>(i / 32), 0)));
	CHECK(worst <= 16);
}

#endif

#ifdef LIBFILES_PNG

#include "builders.h"
#include "libfiles/scan.h"

#include <png.h>
#include <zlib.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{
	// Bytes that compress, but not to nothing: runs, a ramp and noise.
	std::vector<std::uint8_t> compressible(const std::size_t size)
	{
		std::vector<std::uint8_t> data(size);
		std::uint32_t seed = 1;

		for (std::size_t i = 0; i < size; ++i)
		{
			seed = seed * 1664525u + 1013904223u;
			data[i] = (i / 64) % 3 == 0 ? static_cast<std::uint8_t>(i) : (i / 64) % 3 == 1 ? std::uint8_t{0x55} : static_cast<std::uint8_t>(seed >> 24);
		}

		return data;
	}
}

TEST("zlib-ng compresses and inflates through the zlib API, and libfiles' inflate agrees")
{
	CHECK(std::string(zlibVersion()).starts_with("1.3.1.zlib-ng"));

	const auto data = compressible(100000);

	for (const auto level : {0, 1, 6, 9})
	{
		auto bound = compressBound(static_cast<uLong>(data.size()));
		std::vector<std::uint8_t> packed(bound);
		CHECK_EQ(compress2(packed.data(), &bound, data.data(), static_cast<uLong>(data.size()), level), Z_OK);
		packed.resize(bound);
		if (level > 0) CHECK(packed.size() < data.size());

		std::vector<std::uint8_t> unpacked(data.size());
		auto unpacked_size = static_cast<uLongf>(unpacked.size());
		CHECK_EQ(uncompress(unpacked.data(), &unpacked_size, packed.data(), static_cast<uLong>(packed.size())), Z_OK);
		CHECK(unpacked_size == data.size() && unpacked == data);

		// The scanners' own inflate reads what zlib-ng deflates.
		std::vector<std::uint8_t> inflated;
		CHECK(libfiles::detail::zlib_inflate(packed, inflated, data.size()));
		CHECK(inflated == data);
	}

	// The scanners' CRC-32 is zlib's, and so is the Adler-32 at the end of a zlib stream.
	for (const std::size_t size : {0u, 1u, 7u, 64u, 4096u, 100000u})
	{
		const std::span<const std::uint8_t> part(data.data(), size);
		const auto ours = libfiles::detail::crc32(0, part);
		const auto theirs = crc32(0, data.data(), static_cast<uInt>(size));
		CHECK_EQ(ours, static_cast<std::uint32_t>(theirs));
	}
}

TEST("libpng writes and reads a PNG, and libfiles' scanner reads what it wrote")
{
	constexpr std::uint32_t width = 37;
	constexpr std::uint32_t height = 23;
	std::vector<std::uint8_t> rgba(std::size_t{width} * height * 4);

	for (std::size_t i = 0; i < rgba.size(); ++i)
	{
		rgba[i] = static_cast<std::uint8_t>(i * 7 + (i / 4) * 3);
	}

	png_image write{};
	write.version = PNG_IMAGE_VERSION;
	write.width = width;
	write.height = height;
	write.format = PNG_FORMAT_RGBA;

	png_alloc_size_t size = 0;
	CHECK(png_image_write_to_memory(&write, nullptr, &size, 0, rgba.data(), 0, nullptr) != 0);
	std::vector<std::uint8_t> file(size);
	CHECK(png_image_write_to_memory(&write, file.data(), &size, 0, rgba.data(), 0, nullptr) != 0);
	file.resize(size);
	CHECK(file.size() > 8 && png_sig_cmp(file.data(), 0, 8) == 0);

	png_image read{};
	read.version = PNG_IMAGE_VERSION;
	CHECK(png_image_begin_read_from_memory(&read, file.data(), file.size()) != 0);
	CHECK_EQ(read.width, width);
	CHECK_EQ(read.height, height);
	read.format = PNG_FORMAT_RGBA;

	std::vector<std::uint8_t> decoded(PNG_IMAGE_SIZE(read));
	CHECK(png_image_finish_read(&read, nullptr, decoded.data(), 0, nullptr) != 0);
	CHECK(decoded == rgba);

	libfiles::reader r{std::span<const std::uint8_t>(file)};
	const auto scanned = libfiles::scan(r);
	CHECK(scanned.format == libfiles::image_format::png);
	CHECK_EQ(scanned.width, width);
	CHECK_EQ(scanned.height, height);
}

#endif

#ifdef LIBFILES_WEBP

#include "libfiles/scan.h"

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <cstdlib>
#include <cstring>
#include <vector>

TEST("libwebp round-trips lossless exactly and lossy closely, and libfiles' scanner reads what it wrote")
{
	constexpr int width = 37;
	constexpr int height = 23;
	std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);

	for (std::size_t i = 0; i < bgra.size(); ++i)
	{
		const auto pixel = i / 4;
		const auto x = static_cast<int>(pixel % width);
		const auto y = static_cast<int>(pixel / width);
		bgra[i] = static_cast<std::uint8_t>(i % 4 == 3 ? 255 : i % 4 == 0 ? x * 6 : i % 4 == 1 ? y * 10 : 90);
	}

	std::uint8_t* lossless = nullptr;
	const auto lossless_size = WebPEncodeLosslessBGRA(bgra.data(), width, height, width * 4, &lossless);
	CHECK(lossless_size > 0);

	int w = 0;
	int h = 0;
	auto* const exact = WebPDecodeBGRA(lossless, lossless_size, &w, &h);
	CHECK(exact != nullptr && w == width && h == height);
	CHECK(exact && std::memcmp(exact, bgra.data(), bgra.size()) == 0);
	WebPFree(exact);

	const std::vector<std::uint8_t> file(lossless, lossless + lossless_size);
	WebPFree(lossless);

	libfiles::reader r{std::span<const std::uint8_t>(file)};
	const auto scanned = libfiles::scan(r);
	CHECK(scanned.format == libfiles::image_format::webp);
	CHECK_EQ(scanned.width, static_cast<std::uint32_t>(width));
	CHECK_EQ(scanned.height, static_cast<std::uint32_t>(height));

	std::uint8_t* lossy = nullptr;
	const auto lossy_size = WebPEncodeBGRA(bgra.data(), width, height, width * 4, 90.f, &lossy);
	CHECK(lossy_size > 0);

	WebPBitstreamFeatures features{};
	CHECK(WebPGetFeatures(lossy, lossy_size, &features) == VP8_STATUS_OK);
	CHECK_EQ(features.format, 1);
	CHECK_EQ(features.width, width);

	auto* const close = WebPDecodeBGRA(lossy, lossy_size, &w, &h);
	CHECK(close != nullptr);
	auto worst = 0;
	for (std::size_t i = 0; close && i < bgra.size(); ++i) worst = std::max(worst, std::abs(close[i] - bgra[i]));
	CHECK(worst <= 24);
	WebPFree(close);
	WebPFree(lossy);
}

TEST("libwebp's mux adds a chunk its demux finds")
{
	std::vector<std::uint8_t> bgra(4 * 4 * 4, 0x80);
	std::uint8_t* encoded = nullptr;
	const auto size = WebPEncodeLosslessBGRA(bgra.data(), 4, 4, 16, &encoded);
	CHECK(size > 0);

	WebPData image{encoded, size};
	auto* const mux = WebPMuxCreate(&image, 1);
	CHECK(mux != nullptr);

	const std::uint8_t exif[] = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	const WebPData chunk{exif, sizeof(exif)};
	CHECK_EQ(WebPMuxSetChunk(mux, "EXIF", &chunk, 1), WEBP_MUX_OK);

	WebPData assembled{};
	CHECK_EQ(WebPMuxAssemble(mux, &assembled), WEBP_MUX_OK);
	WebPMuxDelete(mux);
	WebPFree(encoded);

	auto* const demux = WebPDemux(&assembled);
	CHECK(demux != nullptr);
	CHECK((WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS) & EXIF_FLAG) != 0);

	WebPChunkIterator it{};
	CHECK(WebPDemuxGetChunk(demux, "EXIF", 1, &it) != 0);
	CHECK(it.chunk.size == sizeof(exif) && std::memcmp(it.chunk.bytes, exif, sizeof(exif)) == 0);
	WebPDemuxReleaseChunkIterator(&it);
	WebPDemuxDelete(demux);
	WebPDataClear(&assembled);
}

#endif

#ifdef LIBFILES_HEIF

#include <libheif/heif.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	std::vector<std::uint8_t> read_example(const char* const name)
	{
		std::ifstream in(std::string(LIBFILES_HEIF_EXAMPLES) + "/" + name, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	struct decoded_image
	{
		bool ok = false;
		int width = 0;
		int height = 0;
		std::vector<std::uint8_t> rgba;
	};

	// The primary image as interleaved RGBA, through whichever decoder libheif picks.
	decoded_image decode_primary(const std::vector<std::uint8_t>& file)
	{
		decoded_image result;
		auto* const ctx = heif_context_alloc();
		heif_image_handle* handle = nullptr;
		heif_image* image = nullptr;

		if (heif_context_read_from_memory_without_copy(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok &&
			heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok &&
			heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr).code == heif_error_Ok)
		{
			int stride = 0;
			const auto* const plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
			result.width = heif_image_get_width(image, heif_channel_interleaved);
			result.height = heif_image_get_height(image, heif_channel_interleaved);

			for (auto y = 0; plane && y < result.height; ++y)
			{
				const auto* const row = plane + static_cast<std::size_t>(y) * stride;
				result.rgba.insert(result.rgba.end(), row, row + static_cast<std::size_t>(result.width) * 4);
			}

			result.ok = plane != nullptr;
		}

		heif_image_release(image);
		heif_image_handle_release(handle);
		heif_context_free(ctx);
		return result;
	}

	bool has_decoder(const heif_compression_format format, const char* const id)
	{
		const heif_decoder_descriptor* descriptors[16] = {};
		const auto count = heif_get_decoder_descriptors(format, descriptors, 16);

		for (auto i = 0; i < count; ++i)
		{
			if (std::string(heif_decoder_descriptor_get_id_name(descriptors[i])) == id) return true;
		}

		return false;
	}
}

TEST("libheif decodes HEVC with libde265 and AV1 with dav1d, and has no FFmpeg decoder of its own")
{
	CHECK(has_decoder(heif_compression_HEVC, "libde265"));
	CHECK(has_decoder(heif_compression_AV1, "dav1d"));
	CHECK(!has_decoder(heif_compression_HEVC, "ffmpeg"));

	for (const auto* const name : {"example.heic", "example.avif"})
	{
		const auto file = read_example(name);
		CHECK(file.size() > 10000);

		const auto image = decode_primary(file);
		CHECK(image.ok);
		CHECK(image.width > 100 && image.height > 100);

		// A photograph: not one colour.
		auto distinct = false;
		for (std::size_t i = 4; i < image.rgba.size() && !distinct; i += 4) distinct = image.rgba[i] != image.rgba[0];
		CHECK(distinct);
	}
}

TEST("libheif round-trips an image through its uncompressed codec exactly")
{
	constexpr int width = 19;
	constexpr int height = 11;
	heif_image* image = nullptr;
	CHECK(heif_image_create(width, height, heif_colorspace_RGB, heif_chroma_interleaved_RGB, &image).code == heif_error_Ok);
	CHECK(heif_image_add_plane(image, heif_channel_interleaved, width, height, 8).code == heif_error_Ok);

	int stride = 0;
	auto* const plane = heif_image_get_plane(image, heif_channel_interleaved, &stride);
	std::vector<std::uint8_t> rgba;

	for (auto y = 0; y < height; ++y)
	{
		for (auto x = 0; x < width; ++x)
		{
			auto* const p = plane + static_cast<std::size_t>(y) * stride + x * 3;
			p[0] = static_cast<std::uint8_t>(x * 13);
			p[1] = static_cast<std::uint8_t>(y * 23);
			p[2] = static_cast<std::uint8_t>(x * y);
			rgba.insert(rgba.end(), {p[0], p[1], p[2], 255});
		}
	}

	auto* const ctx = heif_context_alloc();
	heif_encoder* encoder = nullptr;
	CHECK(heif_context_get_encoder_for_format(ctx, heif_compression_uncompressed, &encoder).code == heif_error_Ok);
	CHECK(heif_context_encode_image(ctx, image, encoder, nullptr, nullptr).code == heif_error_Ok);
	heif_encoder_release(encoder);
	heif_image_release(image);

	std::vector<std::uint8_t> file;
	heif_writer writer{};
	writer.writer_api_version = 1;
	writer.write = [](heif_context*, const void* data, const size_t size, void* userdata)
	{
		auto* const out = static_cast<std::vector<std::uint8_t>*>(userdata);
		out->insert(out->end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
		return heif_error{heif_error_Ok, heif_suberror_Unspecified, ""};
	};
	CHECK(heif_context_write(ctx, &writer, &file).code == heif_error_Ok);
	heif_context_free(ctx);

	const auto decoded = decode_primary(file);
	CHECK(decoded.ok && decoded.width == width && decoded.height == height);
	CHECK(decoded.rgba == rgba);
}

#endif
