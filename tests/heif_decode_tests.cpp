// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for decoding HEIF and AVIF: libheif's own HEVC and AV1 examples, as libheif decodes them;
// images libheif writes through its uncompressed codec, whose every pixel is known - premultiplied alpha
// made straight, an EXIF orientation, an item's own rotation, a thumbnail item; steering HEVC to a named
// decoder; files from libheif's fuzzing corpus that declare no size, or the wrong one; and what decode()
// reports for a file cut short or refused.

#include "test.h"

#ifdef LIBFILES_HEIF

#include "builders.h"
#include "libfiles/decode.h"

#include <libheif/heif.h>
#include <libheif/heif_aux_images.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	bytes read_file(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	bytes read_example(const char* const name)
	{
		return read_file(std::string(LIBFILES_HEIF_EXAMPLES) + "/" + name);
	}

	bytes read_corpus(const char* const name)
	{
		return read_file(std::string(LIBFILES_HEIF_CORPUS) + "/" + name);
	}

	// Where the first box of a type starts, or 0.
	std::size_t find_box(const bytes& file, const char* const type)
	{
		for (std::size_t i = 4; i + 4 <= file.size(); ++i)
			if (std::equal(type, type + 4, file.begin() + static_cast<std::ptrdiff_t>(i))) return i - 4;
		return 0;
	}

	// Memory filled with a byte the decoder never writes, with padding after each row.
	struct heif_sink final : libfiles::image_sink
	{
		bytes memory;
		std::size_t stride = 0;
		bool refuse = false;
		bool refuse_declared = false;
		int calls = 0;
		int accepts = 0;
		libfiles::frame_info declared;
		libfiles::frame_info told;
		bytes exif_told;

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
			exif_told.assign(frame.exif.begin(), frame.exif.end());
			if (refuse) return {};
			stride = std::size_t{4} * frame.width + 12;
			memory.assign(stride * frame.height, 0xAB);
			return {memory.data(), stride};
		}

		bytes rows() const
		{
			bytes result;
			const auto row_bytes = std::size_t{4} * told.width;
			for (std::size_t y = 0; y < told.height; ++y)
				result.insert(result.end(), memory.begin() + static_cast<std::ptrdiff_t>(y * stride),
				              memory.begin() + static_cast<std::ptrdiff_t>(y * stride + row_bytes));
			return result;
		}

		bool padding_kept() const
		{
			for (std::size_t y = 0; y < told.height; ++y)
				for (auto x = std::size_t{4} * told.width; x < stride; ++x)
					if (memory[y * stride + x] != 0xAB) return false;
			return true;
		}
	};

	libfiles::decode_result decode_file(const bytes& file, heif_sink& sink, const libfiles::decode_options& options = {})
	{
		libfiles::reader r{std::span<const std::uint8_t>(file)};
		return libfiles::decode(r, sink, options);
	}

	// What libheif's own decode of the primary image is, as BGRA.
	bytes libheif_bgra(const bytes& file)
	{
		bytes result;
		auto* const ctx = heif_context_alloc();
		heif_image_handle* handle = nullptr;
		heif_image* image = nullptr;

		if (heif_context_read_from_memory_without_copy(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok &&
			heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok &&
			heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr).code == heif_error_Ok)
		{
			int stride = 0;
			const auto* const plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
			const auto width = heif_image_get_width(image, heif_channel_interleaved);
			const auto height = heif_image_get_height(image, heif_channel_interleaved);

			for (auto y = 0; y < height; ++y)
			{
				for (auto x = 0; x < width; ++x)
				{
					const auto* const p = plane + static_cast<std::size_t>(y) * stride + x * 4;
					result.insert(result.end(), {p[2], p[1], p[0], p[3]});
				}
			}
		}

		heif_image_release(image);
		heif_image_handle_release(handle);
		heif_context_free(ctx);
		return result;
	}

	heif_error write_to(heif_context*, const void* data, const size_t size, void* userdata)
	{
		auto* const out = static_cast<bytes*>(userdata);
		out->insert(out->end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
		return {heif_error_Ok, heif_suberror_Unspecified, ""};
	}

	struct written_spec
	{
		int width = 7;
		int height = 5;
		bool alpha = false;
		bool premultiplied = false;
		heif_orientation orientation = heif_orientation_normal;
		bytes exif;
		bool thumbnail = false;
	};

	// The pixel at x, y of a written image, as RGBA.
	std::array<std::uint8_t, 4> source_pixel(const int x, const int y, const bool alpha)
	{
		return {static_cast<std::uint8_t>(x * 30), static_cast<std::uint8_t>(y * 40), static_cast<std::uint8_t>(200 - x * y),
		        alpha ? static_cast<std::uint8_t>((x + y) % 3 == 0 ? 0 : 60 + x * 20) : std::uint8_t{255}};
	}

	// An image as libheif writes it through its uncompressed codec.
	bytes write_heif(const written_spec& s)
	{
		heif_image* image = nullptr;
		heif_image_create(s.width, s.height, heif_colorspace_RGB, s.alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, &image);
		heif_image_add_plane(image, heif_channel_interleaved, s.width, s.height, 8);
		int stride = 0;
		auto* const plane = heif_image_get_plane(image, heif_channel_interleaved, &stride);
		const auto channels = s.alpha ? 4 : 3;

		for (auto y = 0; y < s.height; ++y)
		{
			for (auto x = 0; x < s.width; ++x)
			{
				const auto p = source_pixel(x, y, s.alpha);
				std::copy_n(p.begin(), channels, plane + static_cast<std::size_t>(y) * stride + x * channels);
			}
		}

		if (s.premultiplied) heif_image_set_premultiplied_alpha(image, 1);

		auto* const ctx = heif_context_alloc();
		heif_encoder* encoder = nullptr;
		heif_context_get_encoder_for_format(ctx, heif_compression_uncompressed, &encoder);
		auto* const options = heif_encoding_options_alloc();
		options->image_orientation = s.orientation;

		heif_image_handle* handle = nullptr;
		heif_context_encode_image(ctx, image, encoder, options, &handle);

		if (!s.exif.empty()) heif_context_add_exif_metadata(ctx, handle, s.exif.data(), static_cast<int>(s.exif.size()));

		if (s.thumbnail)
		{
			heif_image_handle* thumbnail = nullptr;
			heif_context_encode_thumbnail(ctx, image, handle, encoder, options, 4, &thumbnail);
			heif_image_handle_release(thumbnail);
		}

		bytes file;
		heif_writer writer{};
		writer.writer_api_version = 1;
		writer.write = write_to;
		heif_context_write(ctx, &writer, &file);

		heif_image_handle_release(handle);
		heif_encoding_options_free(options);
		heif_encoder_release(encoder);
		heif_image_release(image);
		heif_context_free(ctx);
		return file;
	}

	bytes orientation_exif(const std::uint16_t value)
	{
		return builder().raw("II").le16(42).le32(8).le16(1).le16(0x0112).le16(3).le32(1).le16(value).le16(0)
		                .le32(0).b;
	}
}

TEST("Should decode libheif's HEVC and AV1 examples as libheif decodes them, as BGRX")
{
	CHECK(libfiles::can_decode(libfiles::image_format::heif));

	for (const auto* const name : {"example.heic", "example.avif"})
	{
		const auto file = read_example(name);
		heif_sink sink;
		const auto result = decode_file(file, sink);

		CHECK(result.success);
		CHECK(result.error.empty());
		CHECK_EQ(sink.calls, 1);
		CHECK(result.frame.format == libfiles::pixel_format::bgrx8);
		CHECK(result.frame.width > 100 && result.frame.height > 100);
		CHECK(result.frame.heif_decoder.empty());
		CHECK(sink.rows() == libheif_bgra(file));
		CHECK(sink.padding_kept());
	}
}

TEST("Should decode a HEIF's exact pixels, its alpha made straight")
{
	written_spec opaque;
	heif_sink opaque_sink;
	const auto opaque_result = decode_file(write_heif(opaque), opaque_sink);
	CHECK(opaque_result.success);
	CHECK(opaque_result.frame.format == libfiles::pixel_format::bgrx8);
	CHECK_EQ(opaque_result.frame.width, 7u);
	CHECK_EQ(opaque_result.frame.height, 5u);

	bytes expected;
	for (auto y = 0; y < 5; ++y)
		for (auto x = 0; x < 7; ++x)
		{
			const auto p = source_pixel(x, y, false);
			expected.insert(expected.end(), {p[2], p[1], p[0], 255});
		}
	CHECK(opaque_sink.rows() == expected);

	// Straight alpha stays as written.
	written_spec straight;
	straight.alpha = true;
	heif_sink straight_sink;
	const auto straight_result = decode_file(write_heif(straight), straight_sink);
	CHECK(straight_result.success);
	CHECK(straight_result.frame.format == libfiles::pixel_format::bgra8);

	bytes straight_expected;
	for (auto y = 0; y < 5; ++y)
		for (auto x = 0; x < 7; ++x)
		{
			const auto p = source_pixel(x, y, true);
			straight_expected.insert(straight_expected.end(), {p[2], p[1], p[0], p[3]});
		}
	CHECK(straight_sink.rows() == straight_expected);

	// libheif's uncompressed writer keeps alpha in the image and does not record it as premultiplied,
	// which only an alpha item of its own can be: Diffractor's associated-alpha fixture holds the rest.
	written_spec premultiplied = straight;
	premultiplied.premultiplied = true;
	heif_sink premultiplied_sink;
	CHECK(decode_file(write_heif(premultiplied), premultiplied_sink).success);
	CHECK(premultiplied_sink.rows() == straight_expected);
}

TEST("Should take a HEIF's orientation from its EXIF, unless its item turns itself")
{
	written_spec with_exif;
	with_exif.exif = orientation_exif(6);
	heif_sink exif_sink;
	const auto exif_result = decode_file(write_heif(with_exif), exif_sink);
	CHECK(exif_result.success);
	CHECK_EQ(exif_result.frame.orientation, 6u);
	CHECK(!exif_result.frame.transformed);
	CHECK(exif_sink.exif_told == orientation_exif(6));
	CHECK(exif_result.frame.exif.empty());

	// The item's own rotation is applied by libheif, which swaps the edges, and the EXIF is not.
	written_spec rotated = with_exif;
	rotated.orientation = heif_orientation_rotate_90_cw;
	heif_sink rotated_sink;
	const auto rotated_result = decode_file(write_heif(rotated), rotated_sink);
	CHECK(rotated_result.success);
	CHECK(rotated_result.frame.transformed);
	CHECK_EQ(rotated_result.frame.orientation, 1u);
	CHECK_EQ(rotated_result.frame.width, 5u);
	CHECK_EQ(rotated_result.frame.height, 7u);
}

TEST("Should decode a HEIF's thumbnail item when asked")
{
	written_spec spec;
	spec.width = 16;
	spec.height = 8;
	spec.thumbnail = true;
	spec.exif = orientation_exif(3);
	const auto file = write_heif(spec);

	libfiles::decode_options options;
	options.heif_thumbnail = true;
	heif_sink sink;
	const auto result = decode_file(file, sink, options);
	CHECK(result.success);
	CHECK_EQ(result.frame.width, 4u);
	CHECK_EQ(result.frame.height, 2u);
	CHECK(!result.frame.transformed);
	CHECK_EQ(result.frame.orientation, 1u);

	// None to decode.
	heif_sink none_sink;
	const auto none = decode_file(write_heif({}), none_sink, options);
	CHECK(!none.success && !none.error.empty());
	CHECK_EQ(none_sink.calls, 0);
}

TEST("Should steer HEVC, and only HEVC, to a decoder the caller names when it is registered")
{
	libfiles::decode_options options;
	options.heif_hevc_decoder = "libde265";

	heif_sink hevc_sink;
	const auto hevc = decode_file(read_example("example.heic"), hevc_sink, options);
	CHECK(hevc.success);
	CHECK(hevc.frame.heif_decoder == "libde265");
	CHECK(hevc_sink.told.heif_decoder == "libde265");

	heif_sink av1_sink;
	const auto av1 = decode_file(read_example("example.avif"), av1_sink, options);
	CHECK(av1.success);
	CHECK(av1.frame.heif_decoder.empty());

	options.heif_hevc_decoder = "no-such-decoder";
	heif_sink unknown_sink;
	const auto unknown = decode_file(read_example("example.heic"), unknown_sink, options);
	CHECK(unknown.success);
	CHECK(unknown.frame.heif_decoder.empty());
}

TEST("Should decode a HEIF cut short as libheif does, and refuse what the sink refuses")
{
	const auto file = read_example("example.heic");

	// libheif decodes what is there of a file cut inside its image data, and so does this.
	auto cut = file;
	cut.resize(cut.size() / 2);
	heif_sink cut_sink;
	const auto cut_result = decode_file(cut, cut_sink);
	CHECK(cut_result.success);
	CHECK(cut_sink.rows() == libheif_bgra(cut));

	bytes header(file.begin(), file.begin() + 40);
	heif_sink header_sink;
	const auto header_result = decode_file(header, header_sink);
	CHECK(!header_result.success && !header_result.error.empty());
	CHECK_EQ(header_sink.accepts, 0);
	CHECK_EQ(header_sink.calls, 0);

	heif_sink refusing;
	refusing.refuse = true;
	const auto refused = decode_file(file, refusing);
	CHECK(!refused.success && refused.refused && refused.error.empty());
	CHECK_EQ(refusing.accepts, 1);
	CHECK_EQ(refusing.calls, 1);

	// Refused at its declared size, it is never decoded, so allocate is never asked.
	heif_sink refusing_declared;
	refusing_declared.refuse_declared = true;
	const auto refused_declared = decode_file(file, refusing_declared);
	CHECK(!refused_declared.success && refused_declared.refused && refused_declared.error.empty());
	CHECK_EQ(refusing_declared.accepts, 1);
	CHECK_EQ(refusing_declared.calls, 0);
	CHECK(refusing.told.width > 0);
	CHECK_EQ(refused_declared.frame.source_width, refusing.told.source_width);
	CHECK_EQ(refused_declared.frame.width, refusing.told.width);
	CHECK_EQ(refused_declared.frame.height, refusing.told.height);
}

TEST("Should accept the size a HEIF declares before decoding it, and allocate the size it decodes")
{
	// A well-formed file declares what it decodes to.
	heif_sink example;
	const auto example_result = decode_file(read_example("example.avif"), example);
	CHECK(example_result.success);
	CHECK_EQ(example.accepts, 1);
	CHECK(example.declared.width > 0 && example.declared.height > 0);
	CHECK_EQ(example.declared.width, example.told.width);
	CHECK_EQ(example.declared.height, example.told.height);

	// Coded 64 by 64, its 'clap' cropping that to 32 by 32 about the centre. With the crop's vertical
	// offset made absurd, libheif crops the width alone: accepted as declared, written as decoded.
	auto uncropped = read_corpus("hevc32.heif");
	const auto clap = find_box(uncropped, "clap");
	CHECK(clap > 0);
	uncropped[clap + 33] = 0;
	heif_sink uncropped_sink;
	const auto uncropped_result = decode_file(uncropped, uncropped_sink);
	CHECK(uncropped_result.success);
	CHECK_EQ(uncropped_sink.declared.width, 32u);
	CHECK_EQ(uncropped_sink.declared.height, 32u);
	CHECK_EQ(uncropped_result.frame.source_width, 32u);
	CHECK_EQ(uncropped_result.frame.source_height, 32u);
	CHECK_EQ(uncropped_result.frame.width, 32u);
	CHECK_EQ(uncropped_result.frame.height, 64u);
	CHECK(uncropped_sink.rows() == libheif_bgra(uncropped));
	CHECK(uncropped_sink.padding_kept());

	// With its 'ispe' renamed, the item declares no size, and libheif decodes it without checking one:
	// the sink is told 0 by 0, and decides.
	auto undeclared = read_corpus("avif32.heif");
	const auto ispe = find_box(undeclared, "ispe");
	CHECK(ispe > 0);
	undeclared[ispe + 5] = 0xFF;
	heif_sink undeclared_sink;
	const auto undeclared_result = decode_file(undeclared, undeclared_sink);
	CHECK(undeclared_result.success);
	CHECK_EQ(undeclared_sink.declared.width, 0u);
	CHECK_EQ(undeclared_sink.declared.height, 0u);
	CHECK_EQ(undeclared_result.frame.width, 32u);
	CHECK_EQ(undeclared_result.frame.height, 32u);
	CHECK(undeclared_sink.rows() == libheif_bgra(undeclared));

	heif_sink refusing;
	refusing.refuse_declared = true;
	const auto refused = decode_file(undeclared, refusing);
	CHECK(refused.refused && refused.error.empty());
	CHECK_EQ(refusing.calls, 0);
}

TEST("Should decode a HEIF from a stream as from memory")
{
	const auto file = read_example("example.avif");

	heif_sink from_memory;
	const auto memory_result = decode_file(file, from_memory);

	libfiles::memory_stream source(file);
	libfiles::reader r(source, 4096);
	heif_sink from_stream;
	const auto stream_result = libfiles::decode(r, from_stream);

	CHECK(memory_result.success && stream_result.success);
	CHECK(from_stream.memory == from_memory.memory);
}

#endif
