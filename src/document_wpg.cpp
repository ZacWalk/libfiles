// Copyright (C) 1998-2026 Zac Walker.
// SPDX-License-Identifier: MIT

// Purpose: WPG level 1 and 2 native rasters, palettes and byte-run decoding.

#include "document_internal.h"

#include <array>
#include <cstring>

namespace libfiles::detail
{
	namespace
	{
		struct palette_color { std::uint8_t red, blue, green; };
		constexpr palette_color default_palette[256] = {
			{0, 0, 0}, {0, 0, 168}, {0, 168, 0}, {0, 168, 168},
			{168, 0, 0}, {168, 0, 168}, {168, 84, 0}, {168, 168, 168},
			{84, 84, 84}, {84, 84, 252}, {84, 252, 84}, {84, 252, 252},
			{252, 84, 84}, {252, 84, 252}, {252, 252, 84}, {252, 252, 252},
			{0, 0, 0}, {20, 20, 20}, {32, 32, 32}, {44, 44, 44},
			{56, 56, 56}, {68, 68, 68}, {80, 80, 80}, {96, 96, 96},
			{112, 112, 112}, {128, 128, 128}, {144, 144, 144}, {160, 160, 160},
			{180, 180, 180}, {200, 200, 200}, {224, 224, 224}, {252, 252, 252},
			{0, 0, 252}, {64, 0, 252}, {124, 0, 252}, {188, 0, 252},
			{252, 0, 252}, {252, 0, 188}, {252, 0, 124}, {252, 0, 64},
			{252, 0, 0}, {252, 64, 0}, {252, 124, 0}, {252, 188, 0},
			{252, 252, 0}, {188, 252, 0}, {124, 252, 0}, {64, 252, 0},
			{0, 252, 0}, {0, 252, 64}, {0, 252, 124}, {0, 252, 188},
			{0, 252, 252}, {0, 188, 252}, {0, 124, 252}, {0, 64, 252},
			{124, 124, 252}, {156, 124, 252}, {188, 124, 252}, {220, 124, 252},
			{252, 124, 252}, {252, 124, 220}, {252, 124, 188}, {252, 124, 156},
			{252, 124, 124}, {252, 156, 124}, {252, 188, 124}, {252, 220, 124},
			{252, 252, 124}, {220, 252, 124}, {188, 252, 124}, {156, 252, 124},
			{124, 252, 124}, {124, 252, 156}, {124, 252, 188}, {124, 252, 220},
			{124, 252, 252}, {124, 220, 252}, {124, 188, 252}, {124, 156, 252},
			{180, 180, 252}, {196, 180, 252}, {216, 180, 252}, {232, 180, 252},
			{252, 180, 252}, {252, 180, 232}, {252, 180, 216}, {252, 180, 196},
			{252, 180, 180}, {252, 196, 180}, {252, 216, 180}, {252, 232, 180},
			{252, 252, 180}, {232, 252, 180}, {216, 252, 180}, {196, 252, 180},
			{180, 220, 180}, {180, 252, 196}, {180, 252, 216}, {180, 252, 232},
			{180, 252, 252}, {180, 232, 252}, {180, 216, 252}, {180, 196, 252},
			{0, 0, 112}, {28, 0, 112}, {56, 0, 112}, {84, 0, 112},
			{112, 0, 112}, {112, 0, 84}, {112, 0, 56}, {112, 0, 28},
			{112, 0, 0}, {112, 28, 0}, {112, 56, 0}, {112, 84, 0},
			{112, 112, 0}, {84, 112, 0}, {56, 112, 0}, {28, 112, 0},
			{0, 112, 0}, {0, 112, 28}, {0, 112, 56}, {0, 112, 84},
			{0, 112, 112}, {0, 84, 112}, {0, 56, 112}, {0, 28, 112},
			{56, 56, 112}, {68, 56, 112}, {84, 56, 112}, {96, 56, 112},
			{112, 56, 112}, {112, 56, 96}, {112, 56, 84}, {112, 56, 68},
			{112, 56, 56}, {112, 68, 56}, {112, 84, 56}, {112, 96, 56},
			{112, 112, 56}, {96, 112, 56}, {84, 112, 56}, {68, 112, 56},
			{56, 112, 56}, {56, 112, 69}, {56, 112, 84}, {56, 112, 96},
			{56, 112, 112}, {56, 96, 112}, {56, 84, 112}, {56, 68, 112},
			{80, 80, 112}, {88, 80, 112}, {96, 80, 112}, {104, 80, 112},
			{112, 80, 112}, {112, 80, 104}, {112, 80, 96}, {112, 80, 88},
			{112, 80, 80}, {112, 88, 80}, {112, 96, 80}, {112, 104, 80},
			{112, 112, 80}, {104, 112, 80}, {96, 112, 80}, {88, 112, 80},
			{80, 112, 80}, {80, 112, 88}, {80, 112, 96}, {80, 112, 104},
			{80, 112, 112}, {80, 114, 112}, {80, 96, 112}, {80, 88, 112},
			{0, 0, 64}, {16, 0, 64}, {32, 0, 64}, {48, 0, 64},
			{64, 0, 64}, {64, 0, 48}, {64, 0, 32}, {64, 0, 16},
			{64, 0, 0}, {64, 16, 0}, {64, 32, 0}, {64, 48, 0},
			{64, 64, 0}, {48, 64, 0}, {32, 64, 0}, {16, 64, 0},
			{0, 64, 0}, {0, 64, 16}, {0, 64, 32}, {0, 64, 48},
			{0, 64, 64}, {0, 48, 64}, {0, 32, 64}, {0, 16, 64},
			{32, 32, 64}, {40, 32, 64}, {48, 32, 64}, {56, 32, 64},
			{64, 32, 64}, {64, 32, 56}, {64, 32, 48}, {64, 32, 40},
			{64, 32, 32}, {64, 40, 32}, {64, 48, 32}, {64, 56, 32},
			{64, 64, 32}, {56, 64, 32}, {48, 64, 32}, {40, 64, 32},
			{32, 64, 32}, {32, 64, 40}, {32, 64, 48}, {32, 64, 56},
			{32, 64, 64}, {32, 56, 64}, {32, 48, 64}, {32, 40, 64},
			{44, 44, 64}, {48, 44, 64}, {52, 44, 64}, {60, 44, 64},
			{64, 44, 64}, {64, 44, 60}, {64, 44, 52}, {64, 44, 48},
			{64, 44, 44}, {64, 48, 44}, {64, 52, 44}, {64, 60, 44},
			{64, 64, 44}, {60, 64, 44}, {52, 64, 44}, {48, 64, 44},
			{44, 64, 44}, {44, 64, 48}, {44, 64, 52}, {44, 64, 60},
			{44, 64, 64}, {44, 60, 64}, {44, 55, 64}, {44, 48, 64},
			{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
			{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}
		};

		std::uint32_t record_size(cursor& input)
		{
			const auto first = input.u8();
			if (first != 255) return first;
			const auto value = input.le16();
			return value < 0x8000 ? value : ((value & 0x7fffu) << 16) | input.le16();
		}

		struct wpg_rows
		{
			document_decode& decode;
			document_frame frame;
			std::size_t stride, packed, x = 0;
			std::uint32_t y = 0;
			bool first;
			std::vector<std::uint8_t> line, saved;

			wpg_rows(document_decode& decode, document_frame frame) :
				decode(decode), frame(frame), stride(document_row_bytes(frame.width, frame.storage)),
				packed((static_cast<std::size_t>(frame.width) * storage_bits(frame.storage) + 7) / 8),
				first(decode.result.info.frames == 0)
			{
				decode.working_bytes(stride + (first ? 0 : static_cast<std::uint64_t>(stride) * frame.height));
				line.resize(stride, 0);
				if (first) decode.begin(frame);
				else saved.resize(stride * frame.height, 0);
			}

			void store()
			{
				if (y >= frame.height) decode.fail("a WPG run exceeds its raster");
				if (frame.storage == document_storage::bgr24)
					for (std::size_t i = 0; i < packed; i += 3) std::swap(line[i], line[i + 2]);
				if (first) decode.row(y, line);
				else std::memcpy(saved.data() + y * stride, line.data(), stride);
				++y;
			}

			void put(const std::uint8_t value)
			{
				if (y >= frame.height) return;
				line[x++] = value;
				if (x == packed) { store(); x = 0; }
			}

			void repeat(const unsigned count)
			{
				if (count && (x || y == 0)) decode.fail("a WPG row repeat has no complete preceding row");
				for (unsigned i = 0; i < count; ++i) store();
			}

			void finish()
			{
				if (y != frame.height || x) decode.fail("the WPG raster is incomplete");
				if (!first)
				{
					decode.begin(frame);
					for (unsigned row = 0; row < frame.height; ++row)
						decode.row(row, {saved.data() + row * stride, stride});
				}
				decode.end();
			}
		};

		void unpack_wpg1(cursor& input, wpg_rows& rows)
		{
			while (rows.y < rows.frame.height)
			{
				rows.decode.check_cancel();
				const auto control = input.u8();
				const auto count = control & 0x7f;
				if (control & 0x80)
				{
					const auto value = count ? input.u8() : std::uint8_t{255};
					const auto repeats = count ? count : input.u8();
					for (int i = 0; i < repeats; ++i) rows.put(value);
				}
				else if (count)
					for (int i = 0; i < count; ++i) rows.put(input.u8());
				else rows.repeat(input.u8());
			}
		}

		void unpack_wpg2(cursor& input, wpg_rows& rows)
		{
			unsigned sample_size = 1;
			std::array<std::uint8_t, 8> sample{};
			while (rows.y < rows.frame.height)
			{
				rows.decode.check_cancel();
				const auto control = input.u8();
				if (control == 0x7d)
				{
					sample_size = input.u8();
					if (!sample_size || sample_size > sample.size()) rows.decode.fail("invalid WPG sample size");
				}
				else if (control == 0x7e)
					rows.decode.result.warnings.emplace_back("the WPG XOR token is not supported");
				else if (control == 0x7f || control == 0xff)
				{
					const auto count = (static_cast<unsigned>(input.u8()) + 1) * sample_size;
					for (unsigned i = 0; i < count; ++i) rows.put(control == 0xff ? 255 : 0);
				}
				else if (control == 0xfe)
					rows.repeat(static_cast<unsigned>(input.u8()) + 1);
				else if (control == 0xfd || (control & 0x80))
				{
					const auto count = control == 0xfd ? static_cast<unsigned>(input.u8()) + 1 :
						static_cast<unsigned>(control & 0x7f) + 1;
					if (control != 0xfd) input.read(sample.data(), sample_size);
					for (unsigned i = 0; i < count; ++i)
						for (unsigned j = 0; j < sample_size; ++j) rows.put(sample[j]);
				}
				else
					for (unsigned i = 0; i < sample_size * (static_cast<unsigned>(control) + 1); ++i)
						rows.put(input.u8());
			}
		}
	}

	void document_wpg(document_decode& decode)
	{
		auto& file = decode.input;
		if (file.size() < 18 || file.le32(0) != 0x435057ff || (file.le16(8) >> 8) != 0x16 || file.le16(14))
			decode.fail("invalid WPG header");
		const auto level = file.le16(10);
		if (level != 1 && level != 2) decode.fail("unsupported WPG level");
		cursor input{file, 18};
		std::uint64_t next = file.le32(4);
		std::array<std::uint32_t, 256> palette{};
		unsigned colors = 0;
		unsigned precision = 0;
		try
		{
			while (input.pos < file.size())
			{
				decode.check_cancel();
				if (next <= file.size()) input.seek(next);
				else decode.result.warnings.emplace_back("an invalid WPG record offset was read sequentially");
				if (input.pos == file.size()) break;
				if (level == 2) input.u8();
				const auto type = input.u8();
				if (level == 2) record_size(input);
				const auto length = record_size(input);
				next = input.pos + length;
				if ((level == 1 && type == 0x0e) || (level == 2 && type == 0x0c))
				{
					const auto start = input.le16();
					colors = std::min<unsigned>(input.le16(), 256);
					for (unsigned i = std::min<unsigned>(start, colors); i < colors; ++i)
					{
						const auto red = input.u8(), green = input.u8(), blue = input.u8();
						palette[i] = 0xff000000u | (red << 16) | (green << 8) | blue;
						if (level == 2) input.u8();
					}
				}
				else if ((level == 1 && (type == 0x0b || type == 0x14)) || (level == 2 && type == 0x0e))
				{
					if (level == 1 && type == 0x14) input.skip(10);
					const auto width = input.le16(), height = input.le16();
					unsigned bits = 0, compression = 1;
					if (level == 1)
					{
						bits = input.le16();
						input.skip(4);
					}
					else
					{
						const auto depth = input.u8();
						compression = input.u8();
						if (compression > 1) continue;
						bits = depth == 1 ? 1 : depth == 2 ? 2 : depth == 3 ? 4 : depth == 4 ? 8 : depth == 8 ? 24 : 0;
						if (!bits) continue;
					}
					if (!width || !height || (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 24))
						decode.fail("invalid WPG raster layout");
					document_frame frame;
					frame.width = width;
					frame.height = height;
					frame.storage = storage_from_bits(bits);
					frame.sequential = false;
					const auto entries = palette_entries(frame.storage);
					if (!colors && entries)
					{
						colors = static_cast<unsigned>(entries);
						for (unsigned i = 0; i < colors; ++i)
						{
							const auto c = default_palette[i];
							palette[i] = 0xff000000u | (c.red << 16) | (c.green << 8) | c.blue;
						}
					}
					if (bits == 1 && (palette[0] & 0xffffff) == 0 && (palette[1] & 0xffffff) == 0)
						palette[1] = 0xffffffff;
					for (auto& color : palette) color |= 0xff000000;
					frame.palette = {palette.data(), entries};
					wpg_rows rows(decode, frame);
					if (level == 1) unpack_wpg1(input, rows);
					else if (compression) unpack_wpg2(input, rows);
					else
						while (rows.y < height)
						{
							input.read(rows.line.data(), rows.packed);
							rows.store();
						}
					rows.finish();
					decode.result.info.width = width;
					decode.result.info.height = height;
					decode.result.info.storage = frame.storage;
					decode.result.info.bits_per_pixel = bits;
					if (level == 1 && type == 0x14) colors = 0;
				}
				else if (level == 2 && type == 1)
				{
					input.skip(4);
					precision = input.u8();
				}
				else if (level == 2 && type == 0x1b)
				{
					const auto flags = input.le16();
					if (flags & 0x80) input.skip(4);
					if (flags & 0x20) input.skip(precision ? 4 : 2);
					if (flags & 0x10) input.skip(4);
					if (flags & 0x18) input.skip(8);
					if (flags & 0x14) input.skip(8);
					if (flags & 2) input.skip(16);
					if (flags & 1) input.skip(8);
				}
			}
			if (!decode.result.info.frames) decode.fail("the WPG contains no complete raster");
		}
		catch (const read_error& error)
		{
			if (!decode.result.info.frames) throw;
			decode.result.warnings.emplace_back(std::string(error.what()) + "; complete WPG rasters were kept");
		}
		catch (const document_stop&)
		{
			if (!decode.result.info.frames || decode.result.refused || decode.result.cancelled || decode.result.resource_limit) throw;
			decode.result.warnings.push_back(decode.result.error + "; complete WPG rasters were kept");
			decode.result.error.clear();
		}
	}
}
