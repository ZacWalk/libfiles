// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Decoding a PSD's composite image - the flattened copy Photoshop keeps after the layers - in
// bitmap, grayscale, indexed, RGB, CMYK, multichannel, duotone and Lab modes, at 8 bits a channel or 1
// for bitmap mode, raw or PackBits. Moved from Diffractor's loader unchanged, byte for byte: what each
// mode puts in the fourth byte included.

#include "decode_internal.h"
#include "document_internal.h"

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		enum psd_mode : std::uint16_t
		{
			bitmap_mode = 0,
			grayscale_mode = 1,
			indexed_mode = 2,
			rgb_mode = 3,
			cmyk_mode = 4,
			multichannel_mode = 7,
			duotone_mode = 8,
			lab_mode = 9
		};

		// Shared with the scanner's sense of a sane header, so an image that scans is one that decodes.
		constexpr std::uint32_t max_psd_dimension = 30000;
		constexpr std::uint64_t max_psd_pixels = 256ull * 1024ull * 1024ull;

		bool is_valid_header(const std::uint32_t columns, const std::uint32_t rows, const std::uint16_t channels,
		                     const std::uint16_t depth, const std::uint16_t mode)
		{
			// Bitmap mode is the only depth besides 8 decoded: a single 1-bit plane.
			const auto depth_ok = depth == 8 || (depth == 1 && mode == bitmap_mode && channels == 1);

			return columns != 0 && rows != 0 && columns <= max_psd_dimension && rows <= max_psd_dimension &&
				static_cast<std::uint64_t>(columns) * rows <= max_psd_pixels && channels != 0 && channels <= 56 &&
				depth_ok;
		}

		// Which byte of a pixel a channel lands in: the first three are red, green and blue (or cyan,
		// magenta and yellow, or L, a and b), and every later one shares the fourth byte.
		int channel_byte(const int channel)
		{
			switch (channel)
			{
			case 0: return 2;
			case 1: return 1;
			case 2: return 0;
			default: return 3;
			}
		}

		// Rounds as Diffractor's df::round did: anything not a normal number is 0.
		std::int32_t round_to_int(const double d)
		{
			if (!std::isnormal(d)) return 0;
			return static_cast<std::int32_t>(std::round(d));
		}

		std::uint8_t byte_clamp(const int n)
		{
			return static_cast<std::uint8_t>(n > 255 ? 255 : n < 0 ? 0 : n);
		}

		// PackBits into exactly len bytes; the run must hold that and nothing else.
		bool decode_packbits(std::uint8_t* const dst, const int len, const std::span<const std::uint8_t> run)
		{
			std::size_t at = 0;
			int x = 0;

			while (x < len && at < run.size())
			{
				const auto control = static_cast<std::int8_t>(run[at++]);

				if (control >= 0)
				{
					const auto count = static_cast<int>(control) + 1;
					if (count > len - x || static_cast<std::size_t>(count) > run.size() - at) return false;
					for (auto i = 0; i < count; ++i) dst[x++] = run[at++];
				}
				else if (control != -128)
				{
					const auto count = 1 - static_cast<int>(control);
					if (count > len - x || at >= run.size()) return false;
					const auto value = run[at++];
					for (auto i = 0; i < count; ++i) dst[x++] = value;
				}
			}

			return x == len && at == run.size();
		}

		// Merges one decoded plane into a row, its values into one byte of each pixel. A bitmap-mode
		// row packs eight pixels to the byte, most significant bit first, and a set bit is black.
		void scatter_plane(std::uint8_t* const row, const std::uint8_t* const plane, const int cx, const bool is_bitmap,
		                   const int byte, const int pixel_stride = 4, const bool replace = false)
		{
			if (byte >= pixel_stride) return;
			if (is_bitmap)
			{
				for (int x = 0; x < cx; x++)
				{
					const auto is_black = (plane[x / 8] >> (7 - (x % 8))) & 1;
					auto& value = row[x * pixel_stride + byte];
					if (replace) value = is_black ? 0 : 0xff;
					else value |= is_black ? 0 : 0xff;
				}
			}
			else
			{
				for (int x = 0; x < cx; x++)
				{
					auto& value = row[x * pixel_stride + byte];
					if (replace) value = plane[x];
					else value |= plane[x];
				}
			}
		}

		const std::array<int, 4097>& linear_to_srgb_table()
		{
			static const auto table = []
			{
				std::array<int, 4097> result{};
				for (std::size_t i = 0; i < result.size(); ++i)
				{
					const auto v = static_cast<double>(i) / 4096.0;
					const auto srgb = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
					result[i] = byte_clamp(round_to_int(srgb * 255.0));
				}
				return result;
			}();
			return table;
		}

		int linear_to_srgb_byte(const double v)
		{
			const auto clamped = std::clamp(v, 0.0, 1.0);
			return linear_to_srgb_table()[static_cast<std::size_t>(round_to_int(clamped * 4096.0))];
		}

		struct lab_l_lookup
		{
			double y = 0.0;
			double fy = 0.0;
		};

		const std::array<lab_l_lookup, 256>& lab_l_table()
		{
			static const auto table = []
			{
				std::array<lab_l_lookup, 256> result{};
				for (std::size_t i = 0; i < result.size(); ++i)
				{
					const auto l = static_cast<double>(i) * 100.0 / 255.0;
					const auto fy = (l + 16.0) / 116.0;
					auto y = fy * fy * fy;
					if (y < 0.008856) y = l / 903.3;
					result[i].y = y;
					result[i].fy = y > 0.008856 ? fy : 7.787 * y + 16.0 / 116.0;
				}
				return result;
			}();
			return table;
		}

		double cube(const double v)
		{
			return v * v * v;
		}

		// CIE Lab to sRGB through XYZ, with BT.709 primaries and a D65 white, after Mark A. Ruzon.
		void lab_to_rgb(const int stored_l, const int a, const int b, int& red, int& green, int& blue)
		{
			const auto& lookup = lab_l_table()[static_cast<std::size_t>(stored_l)];
			const auto y = lookup.y;
			const auto fy = lookup.fy;

			const double fx = a / 500.0 + fy;
			auto x = fx > 0.206893 ? cube(fx) : (fx - 16.0 / 116.0) / 7.787;

			const double fz = fy - b / 200.0;
			auto z = fz > 0.206893 ? cube(fz) : (fz - 16.0 / 116.0) / 7.787;

			x *= 0.950456;
			z *= 1.088754;

			red = linear_to_srgb_byte(3.240479 * x - 1.537150 * y - 0.498535 * z);
			green = linear_to_srgb_byte(-0.969256 * x + 1.875992 * y + 0.041556 * z);
			blue = linear_to_srgb_byte(0.055648 * x - 0.204043 * y + 1.057311 * z);
		}

		template <typename Pixel>
		void each_pixel(const pixel_buffer& out, const int cx, const int cy, Pixel&& pixel)
		{
			for (int y = 0; y < cy; ++y)
			{
				auto* const row = out.pixels + static_cast<std::size_t>(y) * out.stride;
				for (int x = 0; x < cx; ++x) pixel(row + x * 4);
			}
		}

		void put(std::uint8_t* const p, const int b, const int g, const int r, const int fourth)
		{
			p[0] = static_cast<std::uint8_t>(b);
			p[1] = static_cast<std::uint8_t>(g);
			p[2] = static_cast<std::uint8_t>(r);
			p[3] = static_cast<std::uint8_t>(fourth);
		}
	}

	static void read_psd(reader& r, image_sink* sink, decode_result& result, document_decode* document)
	{
		cursor in{r};

		const auto signature = in.be32();
		const auto version = in.be16();

		if (signature != 0x38425053 || version != 1)
		{
			result.error = "not a PSD file of a version that is decoded";
			return;
		}

		in.skip(6);

		const auto channels = in.be16();
		const auto rows = in.be32();
		const auto columns = in.be32();
		const auto depth = in.be16();
		const auto mode = in.be16();

		if (!is_valid_header(columns, rows, channels, depth, mode))
		{
			result.error = "the PSD's header describes no image that is decoded";
			return;
		}

		const auto cx = static_cast<int>(columns);
		const auto cy = static_cast<int>(rows);

		frame_info frame;
		frame.source_width = columns;
		frame.source_height = rows;
		frame.width = columns;
		frame.height = rows;
		pixel_buffer out;
		if (sink)
		{
			out = sink->allocate(frame);
			if (!out.pixels)
			{
				result.refused = true;
				return;
			}
			for (int y = 0; y < cy; ++y) std::memset(out.pixels + static_cast<std::size_t>(y) * out.stride, 0, std::size_t{4} * columns);
		}
		document_frame native_frame;
		native_frame.width = columns;
		native_frame.height = rows;
		std::array<std::uint32_t, 256> native_palette{};
		int pixel_stride = 4;
		if (document)
		{
			switch (mode)
			{
			case bitmap_mode:
				if (depth != 1 || channels != 1) document->fail("invalid bitmap PSD sample layout");
				native_frame.storage = document_storage::indexed1;
				break;
			case grayscale_mode: native_frame.storage = document_storage::gray8; break;
			case indexed_mode:
			case multichannel_mode:
			case duotone_mode: native_frame.storage = document_storage::indexed8; break;
			case rgb_mode:
			case lab_mode:
				if (channels < 3) document->fail("the PSD has too few colour channels");
				native_frame.storage = channels >= 4 ? document_storage::bgra32 : document_storage::bgr24;
				break;
			case cmyk_mode:
				if (channels < 4) document->fail("the PSD has too few CMYK channels");
				native_frame.storage = channels >= 5 ? document_storage::bgra32 : document_storage::bgr24;
				break;
			default: document->fail("unsupported PSD colour mode");
			}
			pixel_stride = std::max(1u, storage_bits(native_frame.storage) / 8);
			for (unsigned i = 0; i < native_palette.size(); ++i) native_palette[i] = i * 0x010101;
			if (mode == bitmap_mode)
			{
				native_palette[0] = 0xffffffff;
				native_palette[1] = 0xff000000;
			}
		}

		const auto is_bitmap = mode == bitmap_mode;
		auto is_single_channel = is_bitmap;

		switch (mode)
		{
		case grayscale_mode:
		case indexed_mode:
		case multichannel_mode:
		case duotone_mode:
			is_single_channel = true;
			break;
		default:
			break;
		}

		// The colour table, which only an indexed image's is read.
		auto table_length = in.be32();
		unsigned num_colors = 0;
		std::array<std::array<std::uint8_t, 3>, 256> palette{};
		if (document && table_length > 3072) document->fail("the PSD colour table is too large");

		if (table_length != 0)
		{
			if ((mode == indexed_mode && table_length == 768) || (document && mode != duotone_mode))
			{
				num_colors = table_length / 3;
				std::array<std::uint8_t, 3072> table{};
				in.read(table.data(), table_length);

				// Every red, then every green, then every blue.
				for (unsigned i = 0; i < std::min(num_colors, 256u); ++i)
				{
					palette[i] = {table[i + num_colors * 2], table[i + num_colors], table[i]};
					native_palette[i] = 0xff000000u | (table[i] << 16) |
						(table[i + num_colors] << 8) | table[i + num_colors * 2];
				}
			}
			else
			{
				in.skip(table_length);
			}
		}

		if (!document && mode == indexed_mode && num_colors != 256)
		{
			result.error = "the indexed PSD has no full colour table";
			return;
		}

		const auto resources_length = in.be32();

		if (resources_length > in.remaining())
		{
			result.error = "the PSD's image resources run past the end of the file";
			return;
		}

		if (document && resources_length)
		{
			document->working_bytes(resources_length);
			const auto resources = r.read(in.pos, resources_length);
			metadata_blocks metadata;
			parse_photoshop_resources(metadata, resources, false);
			if (!metadata.iptc.empty()) document->output.metadata(document_metadata::iptc, metadata.iptc);
		}
		in.skip(resources_length);

		// The layer and mask information, which the composite image follows.
		table_length = in.be32();

		if (table_length == 8)
		{
			table_length = in.be32();
			table_length = in.be32();
		}

		if (table_length != 0) in.skip(table_length);

		const auto compression = in.be16();

		if (compression > 1)
		{
			result.error = "the PSD's composite image is compressed in a way that is not decoded";
			return;
		}
		std::vector<std::uint8_t> native_pixels;
		if (document)
		{
			native_frame.palette = {native_palette.data(), palette_entries(native_frame.storage)};
			document->begin(native_frame);
			const auto intermediate_storage = mode == bitmap_mode ? document_storage::gray8 : native_frame.storage;
			const auto stride = document_row_bytes(columns, intermediate_storage);
			const auto bytes = static_cast<std::uint64_t>(stride) * rows;
			document->working_bytes(bytes);
			native_pixels.resize(static_cast<std::size_t>(bytes), 0);
			out = {native_pixels.data(), stride};
			auto& info = document->result.info;
			info.width = columns;
			info.height = rows;
			info.storage = native_frame.storage;
			info.bits_per_pixel = storage_bits(native_frame.storage);
			info.encoding = compression;
			info.color_model = mode;
		}

		// Grayscale, duotone, indexed and bitmap images are mapped from the first byte afterwards, so
		// their single plane is decoded there.
		const auto plane_byte = [is_single_channel](const int channel)
		{
			return is_single_channel ? 0 : channel_byte(channel);
		};

		const auto plane_bytes = is_bitmap ? (cx + 7) / 8 : cx;
		std::vector<std::uint8_t> plane(static_cast<std::size_t>(plane_bytes));
		const auto decoded_channels = is_single_channel ? 1u : static_cast<unsigned>(channels);

		const auto row = [&out](const int y)
		{
			return out.pixels + static_cast<std::size_t>(y) * out.stride;
		};
		const auto cancelled = [&] { return document ? document->output.cancelled() : sink->cancelled(); };

		if (compression == 1)
		{
			const auto table_bytes = static_cast<std::uint64_t>(cy) * channels * sizeof(std::uint16_t);
			if (table_bytes > std::numeric_limits<std::size_t>::max()) throw std::bad_alloc();
			if (document) document->working_bytes(native_pixels.size() + table_bytes);
			std::vector<std::uint16_t> run_lengths(static_cast<std::size_t>(cy) * channels);
			for (auto& length : run_lengths) length = in.be16();

			std::vector<std::uint8_t> run;

			for (auto channel = 0u; channel < decoded_channels; ++channel)
			{
				for (auto y = 0; y < cy; ++y)
				{
					if (cancelled())
					{
						result.cancelled = true;
						return;
					}

					const auto length = run_lengths[static_cast<std::size_t>(channel) * cy + y];

					if (length > in.remaining())
					{
						result.error = "a PSD row runs past the end of the file";
						return;
					}

					run.resize(length);
					in.read(run.data(), run.size());

					if (!decode_packbits(plane.data(), plane_bytes, run))
					{
						result.error = "a PSD row's PackBits run does not fill the row";
						return;
					}

					scatter_plane(row(y), plane.data(), cx, is_bitmap, plane_byte(static_cast<int>(channel)),
						pixel_stride, document != nullptr);
				}
			}

			for (auto channel = decoded_channels; channel < channels; ++channel)
			{
				for (auto y = 0; y < cy; ++y) in.skip(run_lengths[static_cast<std::size_t>(channel) * cy + y]);
			}
		}
		else
		{
			for (auto channel = 0u; channel < decoded_channels; channel++)
			{
				for (auto y = 0; y < cy; ++y)
				{
					if (cancelled())
					{
						result.cancelled = true;
						return;
					}

					in.read(plane.data(), plane.size());
					scatter_plane(row(y), plane.data(), cx, is_bitmap, plane_byte(static_cast<int>(channel)),
						pixel_stride, document != nullptr);
				}
			}
		}

		if (document)
		{
			const auto stride = document_row_bytes(columns, native_frame.storage);
			std::vector<std::uint8_t> bitmap(mode == bitmap_mode ? stride : 0);
			for (int y = 0; y < cy; ++y)
			{
				auto* pixels = row(y);
				if (mode == bitmap_mode)
				{
					std::fill(bitmap.begin(), bitmap.end(), 0);
					for (int x = 0; x < cx; ++x)
						if (pixels[x] == 0) bitmap[x / 8] |= static_cast<std::uint8_t>(1u << (7 - x % 8));
					document->row(y, bitmap);
				}
				else
				{
					for (int x = 0; x < cx; ++x)
					{
						auto* p = pixels + x * pixel_stride;
						if (mode == cmyk_mode)
						{
							const auto black = pixel_stride == 3 ? 255 : p[3];
							for (int c = 0; c < 3; ++c) p[c] = byte_clamp(255 - (p[c] + black));
							if (pixel_stride == 4) p[3] = 255;
						}
						else if (mode == lab_mode)
						{
							native_lab_to_bgr(p[2], p[1], p[0], p);
							if (pixel_stride == 4) p[3] = 255;
						}
					}
					document->row(y, {pixels, stride});
				}
			}
			document->end();
			document->output.metadata_complete();
			result.success = true;
			return;
		}

		switch (mode)
		{
		case cmyk_mode:
			each_pixel(out, cx, cy, [](std::uint8_t* const p)
			{
				// Ink plus black routinely exceeds 255, so this is signed.
				const int black = p[3];
				put(p, byte_clamp(255 - (p[0] + black)), byte_clamp(255 - (p[1] + black)), byte_clamp(255 - (p[2] + black)), 255);
			});
			break;
		case lab_mode:
			each_pixel(out, cx, cy, [](std::uint8_t* const p)
			{
				int red = 0, green = 0, blue = 0;
				lab_to_rgb(p[2], p[1] - 128, p[0] - 128, red, green, blue);
				put(p, blue, green, red, 255);
			});
			break;
		case grayscale_mode:
		case duotone_mode:
		case bitmap_mode:
			each_pixel(out, cx, cy, [](std::uint8_t* const p)
			{
				put(p, p[0], p[0], p[0], 0);
			});
			break;
		case indexed_mode:
			each_pixel(out, cx, cy, [&palette](std::uint8_t* const p)
			{
				const auto& c = palette[p[0]];
				put(p, c[0], c[1], c[2], 0);
			});
			break;
		default:
			break;
		}

		result.success = true;
		result.frame = frame;
	}

	void decode_psd(reader& input, image_sink& output, decode_result& result)
	{
		read_psd(input, &output, result, nullptr);
	}

	void document_psd(document_decode& document)
	{
		decode_result result;
		read_psd(document.input, nullptr, result, &document);
		if (result.cancelled)
		{
			document.result.cancelled = true;
			throw document_stop{};
		}
		if (!result.success) document.fail(result.error);
	}
}
