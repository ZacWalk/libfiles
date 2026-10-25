// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for decoding JPEG. Files are encoded here with libjpeg-turbo, and what libfiles writes
// is held to libjpeg-turbo's own output for the same request, or to values worked from the conversion
// rules: so these test what libfiles adds - scaling, NV12, colour conversion, high precision,
// orientation, cancellation and files cut short. Diffractor holds the decoder to the one it replaced,
// byte for byte, over real files.

#include "test.h"

#ifdef LIBFILES_JPEG

#include "builders.h"
#include "libfiles/decode.h"
#include "libfiles/document.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <jpeglib.h>

using namespace test_build;

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

	struct jpeg_spec
	{
		int width = 37;
		int height = 23;
		// 1 gray, 3 RGB, 4 CMYK.
		int components = 3;
		// Luma sampling factors; chroma is 1 by 1.
		int h_samp = 2;
		int v_samp = 2;
		bool progressive = false;
		bool adobe = true;
		int precision = 8;
		int predictor = 0;
		std::vector<bytes> app1;
	};

	int sample_at(const jpeg_spec& s, const int x, const int y, const int c)
	{
		const auto top = (1 << s.precision) - 1;
		// Smooth gradients with an edge, so chroma subsampling and the scaled transforms do work.
		const auto base = (x * 7 + y * 5 + c * 61 + (x > s.width / 2 ? 90 : 0)) % 256;
		return s.precision == 8 ? base : base * top / 255;
	}

	bytes encode(const jpeg_spec& s)
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

		c.image_width = static_cast<JDIMENSION>(s.width);
		c.image_height = static_cast<JDIMENSION>(s.height);
		c.input_components = s.components;
		c.in_color_space = s.components == 1 ? JCS_GRAYSCALE : s.components == 4 ? JCS_CMYK : JCS_RGB;
		c.data_precision = s.precision;
		jpeg_set_defaults(&c);

		if (s.predictor)
		{
			jpeg_enable_lossless(&c, s.predictor, 0);
		}
		else
		{
			jpeg_set_quality(&c, 85, TRUE);
			if (s.components == 3)
			{
				c.comp_info[0].h_samp_factor = s.h_samp;
				c.comp_info[0].v_samp_factor = s.v_samp;
			}
			if (s.progressive) jpeg_simple_progression(&c);
		}

		c.write_Adobe_marker = s.adobe ? TRUE : FALSE;
		jpeg_start_compress(&c, TRUE);

		for (const auto& block : s.app1) jpeg_write_marker(&c, JPEG_APP0 + 1, block.data(), static_cast<unsigned>(block.size()));

		const auto row_samples = static_cast<std::size_t>(s.width) * s.components;
		std::vector<JSAMPLE> row8(row_samples);
		std::vector<J12SAMPLE> row12(row_samples);
		std::vector<J16SAMPLE> row16(row_samples);

		for (int y = 0; y < s.height; ++y)
		{
			for (int x = 0; x < s.width; ++x)
			{
				for (int i = 0; i < s.components; ++i)
				{
					const auto v = sample_at(s, x, y, i);
					const auto at = static_cast<std::size_t>(x) * s.components + i;
					row8[at] = static_cast<JSAMPLE>(v);
					row12[at] = static_cast<J12SAMPLE>(v);
					row16[at] = static_cast<J16SAMPLE>(v);
				}
			}

			if (s.precision <= 8)
			{
				JSAMPROW rows[1] = {row8.data()};
				jpeg_write_scanlines(&c, rows, 1);
			}
			else if (s.precision <= 12)
			{
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

		bytes result(out, out + out_size);
		std::free(out);
		return result;
	}

	// libjpeg-turbo's own decode of the file: its output rows for the colour space and options given.
	struct reference
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		int components = 0;
		bool adobe = false;
		std::vector<std::vector<std::uint16_t>> rows;
		// Raw YCbCr planes, when raw is asked for, with the chroma planes' size as libjpeg lays them out:
		// it may scale chroma up through the transform rather than leave it subsampled.
		std::vector<std::vector<std::uint8_t>> planes[3];
		std::uint32_t chroma_width = 0;
		std::uint32_t chroma_height = 0;
		// Luma rows each raw read returns.
		std::uint32_t rows_per_read = 0;
	};

	reference decode_reference(const bytes& file, const J_COLOR_SPACE out_space, const unsigned scale, const bool fancy,
	                           const bool raw = false)
	{
		jpeg_decompress_struct d{};
		jpeg_error_mgr err{};
		d.err = jpeg_std_error(&err);
		err.error_exit = throw_error;
		err.output_message = quiet;
		jpeg_create_decompress(&d);
		jpeg_mem_src(&d, file.data(), static_cast<unsigned long>(file.size()));
		jpeg_read_header(&d, TRUE);
		d.out_color_space = out_space;
		d.raw_data_out = raw ? TRUE : FALSE;
		d.do_fancy_upsampling = fancy ? TRUE : FALSE;
		d.do_block_smoothing = FALSE;
		d.dct_method = JDCT_ISLOW;
		d.scale_num = 1;
		d.scale_denom = scale;
		jpeg_start_decompress(&d);

		reference r;
		r.width = d.output_width;
		r.height = d.output_height;
		r.components = d.out_color_components;
		r.adobe = d.saw_Adobe_marker != 0;

		if (raw)
		{
			const auto rows_per_call = static_cast<JDIMENSION>(d.max_v_samp_factor * d.min_DCT_v_scaled_size);
			std::vector<std::vector<std::uint8_t>> storage[3];
			std::vector<JSAMPROW> pointers[3];

			for (int ci = 0; ci < 3; ++ci)
			{
				const auto& comp = d.comp_info[ci];
				const auto row_bytes = static_cast<std::size_t>(comp.width_in_blocks) * comp.DCT_h_scaled_size + 16;
				const auto rows = static_cast<std::size_t>(comp.v_samp_factor) * comp.DCT_v_scaled_size;
				storage[ci].assign(rows, std::vector<std::uint8_t>(row_bytes));
				for (auto& row : storage[ci]) pointers[ci].push_back(row.data());
			}

			JSAMPARRAY image[3] = {pointers[0].data(), pointers[1].data(), pointers[2].data()};
			r.chroma_width = d.comp_info[1].downsampled_width;
			r.chroma_height = d.comp_info[1].downsampled_height;
			r.rows_per_read = rows_per_call;

			while (d.output_scanline < d.output_height)
			{
				jpeg_read_raw_data(&d, image, rows_per_call);
				for (int ci = 0; ci < 3; ++ci) for (const auto& row : storage[ci]) r.planes[ci].push_back(row);
			}
		}
		else
		{
			const auto row_samples = static_cast<std::size_t>(d.output_width) * d.out_color_components;
			std::vector<JSAMPLE> row8(row_samples);
			std::vector<J12SAMPLE> row12(row_samples);
			std::vector<J16SAMPLE> row16(row_samples);

			while (d.output_scanline < d.output_height)
			{
				std::vector<std::uint16_t> out(row_samples);

				if (d.data_precision <= 8)
				{
					JSAMPROW rows[1] = {row8.data()};
					jpeg_read_scanlines(&d, rows, 1);
					std::copy(row8.begin(), row8.end(), out.begin());
				}
				else if (d.data_precision <= 12)
				{
					J12SAMPROW rows[1] = {row12.data()};
					jpeg12_read_scanlines(&d, rows, 1);
					std::copy(row12.begin(), row12.end(), out.begin());
				}
				else
				{
					J16SAMPROW rows[1] = {row16.data()};
					jpeg16_read_scanlines(&d, rows, 1);
					std::copy(row16.begin(), row16.end(), out.begin());
				}

				r.rows.push_back(std::move(out));
			}
		}

		jpeg_finish_decompress(&d);
		jpeg_destroy_decompress(&d);
		return r;
	}

	// Memory for whatever the decoder asks, its padding filled with a byte the decoder never writes.
	struct jpeg_sink final : libfiles::image_sink
	{
		std::vector<std::uint8_t> memory;
		std::size_t stride = 0;
		libfiles::frame_info frame;
		// The EXIF block the sink was shown, copied: the frame's own lies in the decoder's memory.
		std::vector<std::uint8_t> exif;
		int calls = 0;
		bool refuse = false;
		int polls = 0;
		int cancel_after = 1 << 30;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& f) override
		{
			++calls;
			frame = f;
			frame.exif = {};
			exif.assign(f.exif.begin(), f.exif.end());
			if (refuse) return {};
			const auto bytes_per_pixel = f.format == libfiles::pixel_format::nv12 ? 1u : 4u;
			stride = static_cast<std::size_t>(f.width) * bytes_per_pixel + 7;
			const auto rows = f.format == libfiles::pixel_format::nv12 ? f.height + f.height / 2 : f.height;
			memory.assign(stride * rows, 0xAB);
			return {memory.data(), stride};
		}

		bool cancelled() override
		{
			return ++polls > cancel_after;
		}

		const std::uint8_t* row(const std::size_t y) const { return memory.data() + y * stride; }

		bool padding_untouched() const
		{
			const auto bytes_per_pixel = frame.format == libfiles::pixel_format::nv12 ? 1u : 4u;
			const auto rows = frame.format == libfiles::pixel_format::nv12 ? frame.height + frame.height / 2 : frame.height;

			for (std::size_t y = 0; y < rows; ++y)
			{
				for (auto x = static_cast<std::size_t>(frame.width) * bytes_per_pixel; x < stride; ++x)
				{
					if (row(y)[x] != 0xAB) return false;
				}
			}

			return true;
		}
	};

	libfiles::decode_result decode(const bytes& file, jpeg_sink& sink, const libfiles::decode_options& options = {})
	{
		libfiles::reader r(file);
		return libfiles::decode(r, sink, options);
	}

	// The whole of what the sink holds as BGRX equals libjpeg-turbo's EXT_BGRX rows.
	bool same_as_reference(const jpeg_sink& sink, const reference& ref)
	{
		if (sink.frame.width != ref.width || sink.frame.height != ref.height || ref.components != 4) return false;

		for (std::size_t y = 0; y < ref.height; ++y)
		{
			for (std::size_t i = 0; i < std::size_t{4} * ref.width; ++i)
			{
				if (sink.row(y)[i] != ref.rows[y][i]) return false;
			}
		}

		return true;
	}

	bytes exif_block(const int orientation)
	{
		builder b;
		b.raw(std::string_view("Exif\0\0", 6));
		b.raw("II").le16(42).le32(8);
		if (orientation)
		{
			b.le16(1).le16(0x0112).le16(3).le32(1).le16(static_cast<unsigned>(orientation)).le16(0).le32(0);
		}
		else
		{
			b.le16(1).le16(0x010F).le16(2).le32(4).raw(std::string_view("ACME", 4)).le32(0);
		}
		return b.b;
	}
}

TEST("native JPEG preserves grayscale samples and the native thumbnail rule")
{
	struct native_sink final : libfiles::document_sink
	{
		libfiles::document_frame frame;
		bytes pixels;
		std::size_t stride = 0;
		bool begin_frame(const libfiles::document_frame& value) override
		{
			frame = value;
			stride = libfiles::document_row_bytes(frame.width, frame.storage);
			pixels.resize(stride * frame.height);
			return true;
		}
		void row(std::uint32_t y, std::span<const std::uint8_t> bytes) override
		{
			std::copy_n(bytes.data(), stride, pixels.data() + y * stride);
		}
	};
	jpeg_spec spec;
	spec.components = 1;
	spec.predictor = 1;
	spec.width = 7;
	spec.height = 5;
	libfiles::memory_stream file(encode(spec));
	libfiles::reader input(file);
	native_sink sink;
	const auto result = libfiles::decode_document(input, libfiles::image_format::jpeg, sink);
	CHECK(result.success);
	CHECK(sink.frame.storage == libfiles::document_storage::gray8);
	CHECK(sink.stride == 8);
	for (int y = 0; y < spec.height; ++y)
		for (int x = 0; x < spec.width; ++x)
			CHECK(sink.pixels[y * sink.stride + x] == sample_at(spec, x, y, 0));

	spec = {};
	libfiles::memory_stream thumbnail_file(encode(spec));
	libfiles::reader thumbnail_input(thumbnail_file);
	libfiles::document_options options;
	options.thumbnail_width = options.thumbnail_height = 8;
	native_sink thumbnail;
	const auto reduced = libfiles::decode_document(thumbnail_input, libfiles::image_format::jpeg, thumbnail, options);
	CHECK(reduced.success);
	CHECK(reduced.info.width == 37 && reduced.info.height == 23);
	CHECK(thumbnail.frame.width == 19 && thumbnail.frame.height == 12);

	const std::uint8_t not_jpeg[] = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'};
	libfiles::reader misnamed(not_jpeg);
	native_sink invalid;
	const auto rejected = libfiles::decode_document(misnamed, libfiles::image_format::jpeg, invalid);
	CHECK(!rejected.success && !rejected.error.empty());
	CHECK(invalid.pixels.empty());
}

TEST("a jpeg decodes to bgrx as libjpeg-turbo writes it, at every reduction and both chroma filters")
{
	for (const auto& [h, v] : {std::pair{2, 2}, std::pair{2, 1}, std::pair{1, 1}})
	{
		jpeg_spec s;
		s.width = 301;
		s.height = 157;
		s.h_samp = h;
		s.v_samp = v;
		const auto file = encode(s);

		for (const auto& [target_w, target_h, scale] : {std::tuple{0u, 0u, 1u}, std::tuple{150u, 78u, 2u}, std::tuple{75u, 39u, 4u},
		                                               std::tuple{37u, 19u, 8u}, std::tuple{151u, 1u, 1u}})
		{
			for (const auto smooth : {true, false})
			{
				jpeg_sink sink;
				libfiles::decode_options o;
				o.target_width = target_w;
				o.target_height = target_h;
				o.smooth_chroma = smooth;
				const auto result = decode(file, sink, o);

				CHECK(result.success);
				CHECK(!result.truncated);
				CHECK(result.frame.format == libfiles::pixel_format::bgrx8);
				CHECK_EQ(result.frame.scale_denominator, scale);
				CHECK_EQ(result.frame.source_width, 301u);
				CHECK_EQ(result.frame.width, (301u + scale - 1) / scale);
				CHECK_EQ(result.frame.height, (157u + scale - 1) / scale);
				CHECK_EQ(result.frame.working_bytes, std::uint64_t{0});
				CHECK_EQ(result.frame.orientation, 1u);
				CHECK(same_as_reference(sink, decode_reference(file, JCS_EXT_BGRX, scale, smooth)));
				CHECK(sink.padding_untouched());
			}
		}
	}
}

TEST("a jpeg decodes to nv12 from libjpeg-turbo's own planes, averaging chroma it holds at full size")
{
	for (const auto& [h, v] : {std::pair{2, 2}, std::pair{2, 1}, std::pair{1, 1}})
	{
		jpeg_spec s;
		s.width = 41;
		s.height = 27;
		s.h_samp = h;
		s.v_samp = v;
		const auto file = encode(s);

		for (const auto scale : {1u, 2u, 8u})
		{
			jpeg_sink sink;
			libfiles::decode_options o;
			o.allow_nv12 = true;
			o.target_width = scale == 1 ? 0 : 41 / scale;
			o.target_height = scale == 1 ? 0 : 27 / scale;
			const auto result = decode(file, sink, o);

			CHECK(result.success);
			CHECK(result.frame.format == libfiles::pixel_format::nv12);
			CHECK_EQ(result.frame.scale_denominator, scale);

			const auto ref = decode_reference(file, JCS_YCbCr, scale, false, true);
			const auto width = ref.width & ~1u;
			const auto height = ref.height & ~1u;
			CHECK_EQ(result.frame.width, width);
			CHECK_EQ(result.frame.height, height);

			auto luma_same = true;
			for (std::size_t y = 0; y < height; ++y)
			{
				luma_same = luma_same && std::memcmp(sink.row(y), ref.planes[0][y].data(), width) == 0;
			}
			CHECK(luma_same);

			// What a pair should hold: the chroma samples under its 2 by 2 luma block, averaged - as far as
			// the raw read holding the block's first luma row reaches, since each read returns one iMCU
			// row and the next may not exist yet.
			const auto chroma_w = ref.chroma_width;
			const auto chroma_h = ref.chroma_height;
			const auto tx = std::max(1u, 2 * chroma_w / ref.width);
			const auto ty = std::max(1u, 2 * chroma_h / ref.height);
			const auto chroma_rows_per_read = ref.rows_per_read * ty / 2;

			auto chroma_same = true;
			for (std::size_t y = 0; y < height / 2; ++y)
			{
				const auto* const pair = sink.row(height + y);

				for (std::size_t x = 0; x < width / 2; ++x)
				{
					for (int ci = 1; ci <= 2; ++ci)
					{
						const auto first = y * ty;
						const auto read_end = (first / chroma_rows_per_read + 1) * chroma_rows_per_read;
						const auto rows = std::min<std::size_t>(ty, read_end - first);
						auto sum = 0u;
						for (std::size_t sy = 0; sy < rows; ++sy)
						{
							for (std::size_t sx = 0; sx < tx; ++sx) sum += ref.planes[ci][first + sy][x * tx + sx];
						}
						const auto n = static_cast<unsigned>(tx * rows);
						chroma_same = chroma_same && pair[x * 2 + (ci - 1)] == (sum + n / 2) / n;
					}
				}
			}
			CHECK(chroma_same);
			CHECK(sink.padding_untouched());
		}
	}

	// Gray has no chroma to hand over: it decodes as BGRX.
	jpeg_spec gray;
	gray.components = 1;
	jpeg_sink sink;
	libfiles::decode_options o;
	o.allow_nv12 = true;
	CHECK(decode(encode(gray), sink, o).frame.format == libfiles::pixel_format::bgrx8);
}

TEST("gray, adobe cmyk and plain cmyk jpegs convert to bgrx")
{
	jpeg_spec gray;
	gray.components = 1;
	const auto gray_file = encode(gray);
	jpeg_sink g;
	CHECK(decode(gray_file, g).success);
	const auto gray_ref = decode_reference(gray_file, JCS_GRAYSCALE, 1, true);
	auto gray_same = true;
	for (std::size_t y = 0; y < gray_ref.height; ++y)
	{
		for (std::size_t x = 0; x < gray_ref.width; ++x)
		{
			const auto v = gray_ref.rows[y][x];
			const auto* const p = g.row(y) + x * 4;
			gray_same = gray_same && p[0] == v && p[1] == v && p[2] == v && p[3] == 0xFF;
		}
	}
	CHECK(gray_same);

	for (const auto adobe : {true, false})
	{
		jpeg_spec cmyk;
		cmyk.components = 4;
		cmyk.adobe = adobe;
		const auto file = encode(cmyk);
		jpeg_sink sink;
		CHECK(decode(file, sink).success);

		const auto ref = decode_reference(file, JCS_CMYK, 1, true);
		CHECK_EQ(ref.adobe, adobe);
		auto same = true;
		for (std::size_t y = 0; y < ref.height; ++y)
		{
			for (std::size_t x = 0; x < ref.width; ++x)
			{
				const auto* const in = ref.rows[y].data() + x * 4;
				const auto ink = [adobe](const int value) { return adobe ? value : 255 - value; };
				const int c = ink(in[0]), m = ink(in[1]), ye = ink(in[2]), k = ink(in[3]);
				const auto* const p = sink.row(y) + x * 4;
				same = same && p[2] == (c * k + 127) / 255 && p[1] == (m * k + 127) / 255 && p[0] == (ye * k + 127) / 255 && p[3] == 0;
			}
		}
		CHECK(same);
	}
}

TEST("12- and 16-bit jpegs scale to 8 bits, and a progressive one reports what it buffers")
{
	for (const auto& [precision, predictor, components] : {std::tuple{12, 0, 3}, std::tuple{12, 1, 1}, std::tuple{16, 1, 3}, std::tuple{16, 1, 1}})
	{
		jpeg_spec s;
		s.precision = precision;
		s.predictor = predictor;
		s.components = components;
		const auto file = encode(s);
		jpeg_sink sink;
		CHECK(decode(file, sink).success);

		const auto ref = decode_reference(file, components == 1 ? JCS_GRAYSCALE : JCS_EXT_BGRX, 1, true);
		const auto top = (1u << precision) - 1u;
		auto same = true;
		for (std::size_t y = 0; y < ref.height; ++y)
		{
			for (std::size_t x = 0; x < ref.width; ++x)
			{
				const auto* const p = sink.row(y) + x * 4;
				if (components == 1)
				{
					const auto v = static_cast<std::uint8_t>((ref.rows[y][x] * 255u + top / 2u) / top);
					same = same && p[0] == v && p[1] == v && p[2] == v && p[3] == 0xFF;
				}
				else
				{
					for (std::size_t i = 0; i < 4; ++i) same = same && p[i] == static_cast<std::uint8_t>((ref.rows[y][x * 4 + i] * 255u + top / 2u) / top);
				}
			}
		}
		CHECK(same);
	}

	jpeg_spec progressive;
	progressive.progressive = true;
	const auto file = encode(progressive);
	jpeg_sink sink;
	const auto result = decode(file, sink);
	CHECK(result.success);
	CHECK(result.frame.working_bytes > 0u);
	CHECK(same_as_reference(sink, decode_reference(file, JCS_EXT_BGRX, 1, true)));
}

TEST("a jpeg's orientation is its last exif block's, as stored")
{
	jpeg_spec s;
	s.app1 = {exif_block(6)};
	jpeg_sink one;
	CHECK_EQ(decode(encode(s), one).frame.orientation, 6u);

	s.app1 = {exif_block(6), exif_block(3)};
	jpeg_sink two;
	CHECK_EQ(decode(encode(s), two).frame.orientation, 3u);

	// A later block without the tag sets the orientation back to 1.
	s.app1 = {exif_block(8), exif_block(0)};
	jpeg_sink none;
	CHECK_EQ(decode(encode(s), none).frame.orientation, 1u);
}

TEST("a jpeg's sink is shown the exif block its orientation comes from, and only the sink")
{
	jpeg_spec s;
	s.app1 = {exif_block(6), exif_block(3), to_bytes(std::string_view("http://ns.adobe.com/xap/1.0/\0<x:xmpmeta/>", 42))};
	jpeg_sink sink;
	const auto result = decode(encode(s), sink);
	const auto last = exif_block(3);

	CHECK(result.success);
	CHECK(sink.exif == std::vector<std::uint8_t>(last.begin() + 6, last.end()));
	CHECK(result.frame.exif.empty());

	s.app1 = {};
	jpeg_sink none;
	CHECK(decode(encode(s), none).success);
	CHECK(none.exif.empty());
}

TEST("a jpeg cut short decodes as far as its data goes, or says why not")
{
	jpeg_spec s;
	s.width = 64;
	s.height = 64;
	const auto file = encode(s);
	const auto whole = decode_reference(file, JCS_EXT_BGRX, 1, true);

	// Cut in the scan: the rows before the cut are as they were.
	const bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() * 2 / 3));
	jpeg_sink sink;
	const auto result = decode(cut, sink);
	CHECK(result.success);
	CHECK(result.truncated);
	CHECK(std::equal(whole.rows[0].begin(), whole.rows[0].end(), sink.row(0)));

	// Cut in the header.
	const bytes header(file.begin(), file.begin() + 30);
	jpeg_sink header_sink;
	const auto header_result = decode(header, header_sink);
	CHECK(!header_result.success);
	CHECK(header_result.truncated);
	CHECK(!header_result.error.empty());
	CHECK_EQ(header_sink.calls, 0);

	// A progressive file buffers every scan before its first row, so cut short it has none.
	jpeg_spec p;
	p.progressive = true;
	const auto progressive = encode(p);
	const bytes progressive_cut(progressive.begin(), progressive.begin() + static_cast<std::ptrdiff_t>(progressive.size() / 2));
	jpeg_sink progressive_sink;
	const auto progressive_result = decode(progressive_cut, progressive_sink);
	CHECK(!progressive_result.success);
	CHECK(progressive_result.truncated);

	// Corrupt data is an error, not an exception.
	auto corrupt = file;
	for (std::size_t i = 2; i < 40 && i < corrupt.size(); ++i) corrupt[i] = 0xFF;
	jpeg_sink corrupt_sink;
	CHECK(!decode(corrupt, corrupt_sink).success);
}

TEST("a jpeg decode is refused or cancelled when the sink says so, and a decoder carries on after")
{
	jpeg_spec s;
	s.width = 64;
	s.height = 64;
	s.app1 = {exif_block(6)};
	const auto file = encode(s);

	jpeg_sink refusing;
	refusing.refuse = true;
	const auto refused = decode(file, refusing);
	CHECK(!refused.success);
	CHECK(refused.refused);
	CHECK(refused.error.empty());

	jpeg_sink cancelling;
	cancelling.cancel_after = 1;
	const auto cancelled = decode(file, cancelling);
	CHECK(!cancelled.success);
	CHECK(cancelled.cancelled);

	// One decoder through a refusal, a corrupt file and a file without EXIF: each as if alone.
	libfiles::decoder d;
	jpeg_sink a;
	a.refuse = true;
	libfiles::reader ra(file);
	CHECK(d.decode(ra, a).refused);

	const bytes broken(file.begin(), file.begin() + 25);
	jpeg_sink b;
	libfiles::reader rb(broken);
	CHECK(!d.decode(rb, b).success);

	jpeg_spec plain;
	const auto plain_file = encode(plain);
	jpeg_sink c;
	libfiles::reader rc(plain_file);
	const auto result = d.decode(rc, c);
	CHECK(result.success);
	CHECK_EQ(result.frame.orientation, 1u);
	CHECK(same_as_reference(c, decode_reference(plain_file, JCS_EXT_BGRX, 1, true)));
}

TEST("a jpeg read through a stream decodes as one read from memory")
{
	jpeg_spec s;
	s.width = 333;
	s.height = 211;
	s.progressive = true;
	const auto file = encode(s);

	for (const auto size : {file.size(), file.size() * 3 / 4})
	{
		const bytes part(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(size));

		jpeg_sink from_memory;
		const auto memory_result = decode(part, from_memory);

		libfiles::memory_stream stream(part);
		libfiles::reader r(stream, 4096);
		jpeg_sink from_stream;
		const auto stream_result = libfiles::decode(r, from_stream);

		CHECK_EQ(memory_result.success, stream_result.success);
		CHECK_EQ(memory_result.truncated, stream_result.truncated);
		CHECK(from_memory.memory == from_stream.memory);
	}
}

#endif
