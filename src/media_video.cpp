// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The video codec headers the media scanners read - H.264 and HEVC parameter sets and the
// records MP4 keeps them in, AV1 sequence headers, VP8 and VP9 frame headers and the VP codec record,
// MPEG-4 Part 2, MPEG-1, MPEG-2 and VC-1 sequence headers, Motion JPEG frames, Theora and DV - and the
// Annex B byte stream H.264 and HEVC travel in. Each is read as its specification lays it out, for
// what it says of the picture: size, sampling, depth, colour, pixel shape, rate and profile. Profile
// names, and the few values a header implies rather than states, are FFmpeg's, which Diffractor read
// these streams with.

#include "media_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		// A parameter set is a few hundred bytes; this bounds only the copy that removes emulation
		// prevention from what a caller hands over.
		constexpr std::size_t max_parameter_set = 64 * 1024;

		constexpr auto max_edge = static_cast<std::uint64_t>(max_image_dimension);

		bool valid_size(const std::uint64_t width, const std::uint64_t height)
		{
			return width > 0 && height > 0 && width <= max_edge && height <= max_edge;
		}

		void set_size(video_header& h, const std::uint64_t width, const std::uint64_t height)
		{
			h.width = static_cast<std::uint32_t>(width);
			h.height = static_cast<std::uint32_t>(height);
		}

		// Kept only when both terms are stated.
		void set_pixel_aspect(video_header& h, const std::uint32_t width, const std::uint32_t height)
		{
			if (width == 0 || height == 0) return;

			const auto divisor = std::gcd(width, height);
			h.pixel_aspect_width = width / divisor;
			h.pixel_aspect_height = height / divisor;
		}

		// H.273 code points as FFmpeg keeps them: a reserved or unassigned value says nothing.
		void set_colour(video_header& h, const std::uint32_t primaries, const std::uint32_t transfer, const std::uint32_t matrix)
		{
			h.color_primaries = static_cast<std::uint8_t>(primaries == 1 || (primaries >= 4 && primaries <= 12) || primaries == 22 ? primaries : 2);
			h.transfer = static_cast<std::uint8_t>(transfer == 1 || (transfer >= 4 && transfer <= 18) ? transfer : 2);
			h.matrix = static_cast<std::uint8_t>(matrix <= 17 && matrix != 3 ? matrix : 2);
		}

		// 4:4:4 coded with the identity matrix is G, B and R planes, and decoders deliver it as RGB.
		void rgb_if_identity(video_header& h)
		{
			if (h.chroma == chroma_format::yuv444 && h.matrix == 0) h.chroma = chroma_format::rgb;
		}

		// chroma_format_idc of H.264 and HEVC. Separate colour planes are 4:4:4 too.
		chroma_format chroma_from_idc(const std::uint32_t idc)
		{
			switch (idc)
			{
			case 0: return chroma_format::gray;
			case 1: return chroma_format::yuv420;
			case 2: return chroma_format::yuv422;
			default: return chroma_format::yuv444;
			}
		}

		std::string numbered_profile(const std::uint32_t profile)
		{
			return "Profile " + std::to_string(profile);
		}

		// Where the next 00 00 01 prefix starts, at or after from; the size when none does.
		std::size_t find_start_code(const std::span<const std::uint8_t> data, std::size_t from)
		{
			for (; from + 3 <= data.size(); ++from)
			{
				if (data[from] == 0 && data[from + 1] == 0 && data[from + 2] == 1) return from;
			}

			return data.size();
		}

		// ---- H.264 and HEVC --------------------------------------------------------------------------

		// The payload of a NAL unit with the emulation prevention bytes taken out.
		std::vector<std::uint8_t> unescape(std::span<const std::uint8_t> payload)
		{
			payload = payload.first(std::min(payload.size(), max_parameter_set));

			std::vector<std::uint8_t> rbsp;
			rbsp.reserve(payload.size());
			auto zeros = 0;

			for (const auto c : payload)
			{
				if (zeros >= 2 && c == 3)
				{
					zeros = 0;
					continue;
				}

				rbsp.push_back(c);
				zeros = c == 0 ? zeros + 1 : 0;
			}

			return rbsp;
		}

		// Table E-1, which both codecs share: the pixel shapes aspect_ratio_idc names.
		constexpr std::uint8_t sample_aspects[17][2] = {
			{0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11},
			{80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1},
		};

		constexpr std::uint32_t extended_sar = 255;

		// The start of the VUI both codecs share: the pixel shape, the video signal type and the chroma
		// location. Each part is kept only when it is read whole.
		void read_vui_start(bit_reader& b, video_header& h)
		{
			if (b.flag())
			{
				const auto idc = b.u(8);
				std::uint32_t width = 0;
				std::uint32_t height = 0;

				if (idc == extended_sar)
				{
					width = b.u(16);
					height = b.u(16);
				}
				else if (idc < std::size(sample_aspects))
				{
					width = sample_aspects[idc][0];
					height = sample_aspects[idc][1];
				}

				if (!b.overrun()) set_pixel_aspect(h, width, height);
			}

			// overscan_info_present_flag, and overscan_appropriate_flag.
			if (b.flag()) b.skip(1);

			// video_signal_type_present_flag: video_format, the range, then the colour description.
			if (b.flag())
			{
				b.skip(3);
				const auto full_range = b.flag();
				std::uint32_t primaries = 2;
				std::uint32_t transfer = 2;
				std::uint32_t matrix = 2;

				if (b.flag())
				{
					primaries = b.u(8);
					transfer = b.u(8);
					matrix = b.u(8);
				}

				if (!b.overrun())
				{
					h.full_range = full_range;
					set_colour(h, primaries, transfer, matrix);
				}
			}

			// chroma_loc_info_present_flag, and the location in each field.
			if (b.flag())
			{
				b.ue();
				b.ue();
			}
		}

		// The profiles whose SPS states its chroma format, depths and scaling matrices; 144 is the High
		// 4:4:4 profile the 2007 edition replaced with 244.
		constexpr std::uint32_t h264_high_profiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135, 144};

		std::string h264_profile(const std::uint32_t idc, const std::uint32_t constraints)
		{
			// constraint_set1_flag makes Baseline Constrained Baseline; constraint_set3_flag makes the
			// high profiles intra-only.
			const auto set1 = (constraints & 0x40) != 0;
			const auto set3 = (constraints & 0x10) != 0;

			switch (idc)
			{
			case 66: return set1 ? "Constrained Baseline" : "Baseline";
			case 77: return "Main";
			case 88: return "Extended";
			case 100: return "High";
			case 110: return set3 ? "High 10 Intra" : "High 10";
			case 122: return set3 ? "High 4:2:2 Intra" : "High 4:2:2";
			case 244: return set3 ? "High 4:4:4 Intra" : "High 4:4:4 Predictive";
			case 44: return "CAVLC 4:4:4";
			case 118: return "Multiview High";
			case 128: return "Stereo High";
			case 144: return "High 4:4:4";
			default: return {};
			}
		}

		// A scaling list's deltas, which end once one makes the next scale zero.
		void skip_scaling_list(bit_reader& b, const unsigned size)
		{
			std::int64_t last = 8;

			for (unsigned j = 0; j < size; ++j)
			{
				const auto next = ((last + b.se()) % 256 + 256) % 256;
				if (next == 0) return;
				last = next;
			}
		}

		struct hevc_profile_tier_level
		{
			std::uint32_t profile_idc = 0;
			std::uint32_t compatibility = 0;
			bool progressive_source = false;
			bool interlaced_source = false;
		};

		hevc_profile_tier_level read_profile_tier_level(bit_reader& b, const unsigned sub_layers)
		{
			hevc_profile_tier_level ptl;

			b.skip(3);
			ptl.profile_idc = b.u(5);
			ptl.compatibility = b.u(32);
			ptl.progressive_source = b.flag();
			ptl.interlaced_source = b.flag();
			// The rest of the 48 constraint bits, and general_level_idc.
			b.skip(46 + 8);

			unsigned profile_present = 0;
			unsigned level_present = 0;

			for (unsigned i = 0; i < sub_layers; ++i)
			{
				profile_present |= b.u(1) << i;
				level_present |= b.u(1) << i;
			}

			// The flags are padded to eight sub-layers.
			if (sub_layers > 0) b.skip(2 * (8 - sub_layers));

			for (unsigned i = 0; i < sub_layers; ++i)
			{
				if (profile_present >> i & 1) b.skip(88);
				if (level_present >> i & 1) b.skip(8);
			}

			return ptl;
		}

		// general_profile_idc; when that is 0, the first profile the compatibility flags claim, as FFmpeg
		// takes it.
		std::string hevc_profile(std::uint32_t idc, const std::uint32_t compatibility)
		{
			for (unsigned j = 1; idc == 0 && j < 32; ++j)
			{
				if (compatibility >> (31 - j) & 1) idc = j;
			}

			switch (idc)
			{
			case 1: return "Main";
			case 2: return "Main 10";
			case 3: return "Main Still Picture";
			case 4: return "Rext";
			case 6: return "Multiview Main";
			case 9: return "Scc";
			default: return {};
			}
		}

		void skip_scaling_list_data(bit_reader& b)
		{
			for (unsigned size = 0; size < 4; ++size)
			{
				for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1)
				{
					// scaling_list_pred_mode_flag clear: a list copied from another, by its distance.
					if (!b.flag())
					{
						b.ue();
						continue;
					}

					if (size > 1) b.se();
					for (unsigned i = 0; i < std::min(64u, 16u << (2 * size)); ++i) b.se();
				}
			}
		}

		// A short-term reference picture set: how far before and after the current picture in output
		// order each of its pictures lies.
		struct short_term_set
		{
			std::uint32_t before = 0;
			std::uint32_t after = 0;
			std::array<std::int32_t, 32> before_deltas{};
			std::array<std::int32_t, 32> after_deltas{};
		};

		// One st_ref_pic_set of an SPS. A set predicted from the one before it codes a flag or two for
		// each of that set's pictures and for the distance between the sets, and how many flags the next
		// set codes is how many pictures this one holds - which 7.4.8 derives from the distances, since a
		// picture predicted onto the current one is dropped, so the distances are kept too.
		bool read_short_term_set(bit_reader& b, const std::size_t index, std::vector<short_term_set>& sets)
		{
			auto& set = sets[index];

			if (index == 0 || !b.flag())
			{
				set.before = b.ue();
				set.after = b.ue();
				if (set.before > 15 || set.after > 15) return false;

				std::int32_t distance = 0;

				for (std::uint32_t i = 0; i < set.before; ++i)
				{
					const auto step = b.ue();
					if (step > 32767) return false;
					distance -= static_cast<std::int32_t>(step) + 1;
					set.before_deltas[i] = distance;
					b.skip(1);
				}

				distance = 0;

				for (std::uint32_t i = 0; i < set.after; ++i)
				{
					const auto step = b.ue();
					if (step > 32767) return false;
					distance += static_cast<std::int32_t>(step) + 1;
					set.after_deltas[i] = distance;
					b.skip(1);
				}

				return !b.overrun();
			}

			const auto& reference = sets[index - 1];
			const auto earlier = b.flag();
			const auto magnitude = b.ue();
			if (magnitude > 32767) return false;
			const auto delta = (earlier ? -1 : 1) * (static_cast<std::int32_t>(magnitude) + 1);

			// used_by_curr_pic_flag, and when it is clear use_delta_flag: the reference's pictures, then
			// the distance itself.
			const auto count = reference.before + reference.after;
			std::array<bool, 32> kept{};
			for (std::uint32_t j = 0; j <= count; ++j) kept[j] = b.flag() || b.flag();

			// Equations 7-61 and 7-62, in their order.
			std::uint32_t n = 0;

			for (auto j = reference.after; j-- > 0;)
			{
				const auto distance = reference.after_deltas[j] + delta;
				if (distance < 0 && kept[reference.before + j]) set.before_deltas[n++] = distance;
			}

			if (delta < 0 && kept[count]) set.before_deltas[n++] = delta;

			for (std::uint32_t j = 0; j < reference.before; ++j)
			{
				const auto distance = reference.before_deltas[j] + delta;
				if (distance < 0 && kept[j]) set.before_deltas[n++] = distance;
			}

			set.before = n;
			n = 0;

			for (auto j = reference.before; j-- > 0;)
			{
				const auto distance = reference.before_deltas[j] + delta;
				if (distance > 0 && kept[j]) set.after_deltas[n++] = distance;
			}

			if (delta > 0 && kept[count]) set.after_deltas[n++] = delta;

			for (std::uint32_t j = 0; j < reference.after; ++j)
			{
				const auto distance = reference.after_deltas[j] + delta;
				if (distance > 0 && kept[reference.before + j]) set.after_deltas[n++] = distance;
			}

			set.after = n;
			return set.before + set.after < 32 && !b.overrun();
		}

		// What lies between an SPS's bit depths and its VUI. False when it is cut short or damaged.
		bool skip_to_hevc_vui(bit_reader& b, const unsigned sub_layers)
		{
			const auto poc_lsb_bits_minus4 = b.ue();
			if (poc_lsb_bits_minus4 > 12) return false;

			// The buffering of every sub-layer, or of the highest alone.
			const auto every_sub_layer = b.flag();

			for (auto i = every_sub_layer ? 0u : sub_layers; i <= sub_layers; ++i)
			{
				b.ue();
				b.ue();
				b.ue();
			}

			// The coding and transform block sizes and the transform hierarchy depths.
			for (auto i = 0; i < 6; ++i) b.ue();

			// scaling_list_enabled_flag, then sps_scaling_list_data_present_flag.
			if (b.flag() && b.flag()) skip_scaling_list_data(b);

			// amp_enabled_flag and sample_adaptive_offset_enabled_flag; then PCM's depths, sizes and flag.
			b.skip(2);

			if (b.flag())
			{
				b.skip(8);
				b.ue();
				b.ue();
				b.skip(1);
			}

			const auto count = b.ue();
			if (count > 64) return false;

			std::vector<short_term_set> sets(count);

			for (std::size_t i = 0; i < sets.size(); ++i)
			{
				if (!read_short_term_set(b, i, sets)) return false;
			}

			if (b.flag())
			{
				const auto long_term = b.ue();
				if (long_term > 32) return false;

				for (std::uint32_t i = 0; i < long_term; ++i) b.skip(poc_lsb_bits_minus4 + 4 + 1);
			}

			// sps_temporal_mvp_enabled_flag and strong_intra_smoothing_enabled_flag.
			b.skip(2);
			return !b.overrun();
		}

		void read_hevc_vui(bit_reader& b, video_header& h)
		{
			read_vui_start(b, h);

			// neutral_chroma_indication_flag, field_seq_flag and frame_field_info_present_flag.
			b.skip(1);
			const auto fields = b.flag();
			b.skip(1);
			if (b.overrun()) return;
			if (fields) h.interlaced = true;

			// The default display window is a suggestion players are free to ignore, and FFmpeg does.
			if (b.flag())
			{
				for (auto i = 0; i < 4; ++i) b.ue();
			}

			if (b.flag())
			{
				const auto units = b.u(32);
				const auto scale = b.u(32);
				if (!b.overrun() && units != 0 && scale != 0) h.frame_rate = static_cast<double>(scale) / units;
			}
		}

		// ---- AV1 -------------------------------------------------------------------------------------

		bool read_leb128(const std::span<const std::uint8_t> data, std::size_t& at, std::uint64_t& value)
		{
			value = 0;

			for (unsigned i = 0; i < 8; ++i)
			{
				if (at >= data.size()) return false;

				const auto byte = data[at++];
				value |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * i);
				if ((byte & 0x80) == 0) break;
			}

			return value <= 0xFFFFFFFFu;
		}

		// uvlc(), in which 32 leading zeros or more stand for the largest value.
		std::uint32_t uvlc(bit_reader& b)
		{
			unsigned zeros = 0;

			while (!b.flag())
			{
				if (b.overrun()) return 0;
				++zeros;
			}

			if (zeros >= 32) return 0xFFFFFFFFu;
			return static_cast<std::uint32_t>((1ull << zeros) - 1 + b.u(zeros));
		}

		std::string av1_profile(const std::uint32_t profile)
		{
			switch (profile)
			{
			case 0: return "Main";
			case 1: return "High";
			default: return "Professional";
			}
		}

		bool read_sequence_header_obu(const std::span<const std::uint8_t> payload, video_header& out)
		{
			bit_reader b(payload);
			video_header h;

			const auto profile = b.u(3);
			if (profile > 2) return false;

			b.skip(1);
			const auto reduced = b.flag();

			if (reduced)
			{
				b.skip(5);
			}
			else
			{
				auto decoder_model = false;
				std::uint32_t buffer_delay_bits = 0;

				if (b.flag())
				{
					const auto units = b.u(32);
					const auto scale = b.u(32);

					// equal_picture_interval, and the ticks each picture lasts.
					if (b.flag())
					{
						const auto ticks = static_cast<double>(uvlc(b)) + 1;
						if (units != 0 && scale != 0) h.frame_rate = scale / (units * ticks);
					}

					decoder_model = b.flag();

					if (decoder_model)
					{
						buffer_delay_bits = b.u(5) + 1;
						b.skip(32 + 5 + 5);
					}
				}

				const auto display_delay = b.flag();
				const auto operating_points = b.u(5) + 1;

				for (std::uint32_t i = 0; i < operating_points; ++i)
				{
					// operating_point_idc, then seq_level_idx with a tier above level 3.3.
					b.skip(12);
					if (b.u(5) > 7) b.skip(1);
					// The decoder model's two buffer delays and low_delay_mode_flag.
					if (decoder_model && b.flag()) b.skip(2 * buffer_delay_bits + 1);
					if (display_delay && b.flag()) b.skip(4);
				}
			}

			const auto width_bits = b.u(4) + 1;
			const auto height_bits = b.u(4) + 1;
			const auto width = std::uint64_t{b.u(width_bits)} + 1;
			const auto height = std::uint64_t{b.u(height_bits)} + 1;

			// The frame ID lengths, then the superblock size and two intra tools.
			if (!reduced && b.flag()) b.skip(4 + 3);
			b.skip(3);

			if (!reduced)
			{
				// Four inter tools, then the order hint and the two tools that need it.
				b.skip(4);
				const auto order_hint = b.flag();
				if (order_hint) b.skip(2);

				// Screen content tools are chosen per frame, or forced on or off. Unless they are off,
				// integer motion vectors are chosen per frame or forced.
				const auto screen_content = b.flag() || b.flag();
				if (screen_content && !b.flag()) b.skip(1);
				if (order_hint) b.skip(3);
			}

			// Superres, CDEF and loop restoration, then the colour configuration.
			b.skip(3);

			const auto high_bitdepth = b.flag();
			auto depth = high_bitdepth ? 10u : 8u;
			if (profile == 2 && high_bitdepth && b.flag()) depth = 12;

			const auto mono = profile != 1 && b.flag();
			std::uint32_t primaries = 2;
			std::uint32_t transfer = 2;
			std::uint32_t matrix = 2;

			if (b.flag())
			{
				primaries = b.u(8);
				transfer = b.u(8);
				matrix = b.u(8);
			}

			if (mono)
			{
				h.full_range = b.flag();
				h.chroma = chroma_format::gray;
			}
			else if (primaries == 1 && transfer == 13 && matrix == 0)
			{
				// sRGB, whose sampling and range go without saying.
				h.full_range = true;
				h.chroma = chroma_format::yuv444;
			}
			else
			{
				h.full_range = b.flag();

				if (profile == 0)
				{
					h.chroma = chroma_format::yuv420;
				}
				else if (profile == 1)
				{
					h.chroma = chroma_format::yuv444;
				}
				else if (depth == 12)
				{
					const auto x = b.flag();
					const auto y = x && b.flag();
					h.chroma = x ? (y ? chroma_format::yuv420 : chroma_format::yuv422) : chroma_format::yuv444;
				}
				else
				{
					h.chroma = chroma_format::yuv422;
				}
			}

			if (b.overrun() || !valid_size(width, height)) return false;

			set_size(h, width, height);
			h.bit_depth = static_cast<std::uint8_t>(depth);
			set_colour(h, primaries, transfer, matrix);
			h.profile = av1_profile(profile);
			rgb_if_identity(h);
			out = std::move(h);
			return true;
		}

		// ---- VP9 -------------------------------------------------------------------------------------

		constexpr std::uint32_t vp9_sync_code = 0x498342;

		bool read_vp9_color_config(bit_reader& b, const std::uint32_t profile, video_header& h)
		{
			h.bit_depth = static_cast<std::uint8_t>(profile >= 2 ? (b.flag() ? 12 : 10) : 8);

			// color_space as matrix coefficients: unknown, BT.601, BT.709, SMPTE 170M, SMPTE 240M,
			// BT.2020, reserved and sRGB.
			constexpr std::uint8_t matrices[8] = {2, 5, 1, 6, 7, 9, 2, 0};
			const auto space = b.u(3);
			h.matrix = matrices[space];

			const auto subsampled_profile = profile == 0 || profile == 2;

			if (space == 7)
			{
				// RGB, which only profiles 1 and 3 carry; then a reserved bit.
				if (subsampled_profile || b.flag()) return false;
				h.full_range = true;
				h.chroma = chroma_format::rgb;
				return true;
			}

			h.full_range = b.flag();

			if (subsampled_profile)
			{
				h.chroma = chroma_format::yuv420;
				return true;
			}

			// Profiles 1 and 3 are the ones without 4:2:0. A reserved bit follows.
			const auto x = b.flag();
			const auto y = b.flag();
			if ((x && y) || b.flag()) return false;

			h.chroma = x ? chroma_format::yuv422 : y ? chroma_format::yuv440 : chroma_format::yuv444;
			return true;
		}

		// ---- MPEG-4 Part 2 ---------------------------------------------------------------------------

		// profile_and_level_indication's profile, by its high four bits, as FFmpeg names them.
		constexpr const char* mpeg4_profiles[16] = {
			"Simple Profile", "Simple Scalable Profile", "Core Profile", "Main Profile", "N-bit Profile",
			"Scalable Texture Profile", "Simple Face Animation Profile", "Basic Animated Texture Profile",
			"Hybrid Profile", "Advanced Real Time Simple Profile", "Core Scalable Profile",
			"Advanced Coding Profile", "Advanced Core Profile", "Advanced Scalable Texture Profile",
			"Simple Studio Profile", "Advanced Simple Profile",
		};

		// aspect_ratio_info's pixel shapes, 1 to 5; 15 is a stated pair.
		constexpr std::uint8_t mpeg4_aspects[6][2] = {{0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}};

		constexpr std::uint32_t rectangular = 0;
		constexpr std::uint32_t binary_only = 2;
		constexpr std::uint32_t grayscale = 3;

		// A visual object's header: its version, and a video object's signal type.
		void read_visual_object(const std::span<const std::uint8_t> body, unsigned& verid, video_header& h)
		{
			bit_reader b(body);
			auto version = 1u;

			if (b.flag())
			{
				version = b.u(4);
				b.skip(3);
			}

			const auto type = b.u(4);
			auto full_range = false;
			std::uint32_t primaries = 2;
			std::uint32_t transfer = 2;
			std::uint32_t matrix = 2;

			// A video or still texture object, and its video_signal_type.
			if ((type == 1 || type == 2) && b.flag())
			{
				b.skip(3);
				full_range = b.flag();

				if (b.flag())
				{
					primaries = b.u(8);
					transfer = b.u(8);
					matrix = b.u(8);
				}
			}

			if (b.overrun()) return;

			verid = version;
			h.full_range = full_range;
			set_colour(h, primaries, transfer, matrix);
		}

		bool read_video_object_layer(const std::span<const std::uint8_t> body, const unsigned object_verid, video_header h, video_header& out)
		{
			bit_reader b(body);
			b.skip(1);

			// Studio objects lay the layer out differently. FFmpeg takes them to be types 14 and 15,
			// the standard 15 and 16; 14 is otherwise face and body animation, which has no picture.
			const auto type = b.u(8);
			if (type >= 14 && type <= 16) return false;

			auto verid = object_verid;

			if (b.flag())
			{
				verid = b.u(4);
				b.skip(3);
			}

			const auto aspect = b.u(4);
			std::uint32_t aspect_width = 0;
			std::uint32_t aspect_height = 0;

			if (aspect == 15)
			{
				aspect_width = b.u(8);
				aspect_height = b.u(8);
			}
			else if (aspect < std::size(mpeg4_aspects))
			{
				aspect_width = mpeg4_aspects[aspect][0];
				aspect_height = mpeg4_aspects[aspect][1];
			}

			// vol_control_parameters: chroma_format, which can only be 4:2:0, low_delay, and the VBV
			// parameters with their markers.
			if (b.flag())
			{
				b.skip(3);
				if (b.flag()) b.skip(79);
			}

			const auto shape = b.u(2);
			if (shape == grayscale && verid != 1) b.skip(4);
			b.skip(1);

			const auto resolution = b.u(16);
			if (resolution == 0) return false;
			b.skip(1);

			std::uint32_t increment = 0;

			if (b.flag())
			{
				// fixed_vop_time_increment takes the bits a count below the resolution needs.
				auto bits = 1u;
				while ((1u << bits) < resolution) ++bits;
				increment = b.u(bits);
			}

			std::uint32_t width = 0;
			std::uint32_t height = 0;
			auto interlaced = false;
			auto depth = 8u;

			if (shape != binary_only)
			{
				if (shape == rectangular)
				{
					b.skip(1);
					width = b.u(13);
					b.skip(1);
					height = b.u(13);
					b.skip(1);
				}

				interlaced = b.flag();
				b.skip(1);

				// sprite_enable: a static sprite states its size and place, then it and global motion
				// compensation their warping points, accuracy and brightness change.
				const auto sprite = b.u(verid == 1 ? 1u : 2u);

				if (sprite == 1 || sprite == 2)
				{
					if (sprite == 1) b.skip(4 * 14);
					b.skip(6 + 2 + 1);
					if (sprite == 1) b.skip(1);
				}

				if (verid != 1 && shape != rectangular) b.skip(1);

				// not_8_bit: the quantiser's precision and the bits a pixel takes.
				if (b.flag())
				{
					b.skip(4);
					depth = b.u(4);
				}
			}

			if (b.overrun() || (shape == rectangular && (width == 0 || height == 0))) return false;

			h.width = width;
			h.height = height;
			set_pixel_aspect(h, aspect_width, aspect_height);
			h.chroma = chroma_format::yuv420;
			h.bit_depth = static_cast<std::uint8_t>(depth);
			h.interlaced = interlaced;
			if (increment != 0) h.frame_rate = static_cast<double>(resolution) / increment;
			out = std::move(h);
			return true;
		}

		// ---- MPEG-1 and MPEG-2 -----------------------------------------------------------------------

		// frame_rate_code's eight rates, then the five FFmpeg also reads for Xing's and libmpeg3's
		// streams.
		constexpr std::uint32_t mpeg_frame_rates[14][2] = {
			{0, 0}, {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001},
			{60, 1}, {15, 1}, {5, 1}, {10, 1}, {12, 1}, {15, 1},
		};

		// MPEG-1 states a pixel's height over its width to four places; these are FFmpeg's fractions
		// of them, width to height.
		constexpr std::uint16_t mpeg1_pixel_aspects[15][2] = {
			{0, 0}, {1, 1}, {49, 33}, {64, 45}, {239, 182}, {36, 29}, {32, 27}, {169, 151}, {178, 163},
			{54, 53}, {196, 201}, {187, 200}, {200, 219}, {127, 147}, {134, 161},
		};

		// MPEG-2 states the display's shape instead: square pixels, 4:3, 16:9 and 2.21:1.
		constexpr std::uint16_t mpeg2_display_aspects[5][2] = {{0, 0}, {0, 0}, {4, 3}, {16, 9}, {221, 100}};

		constexpr std::uint8_t sequence_header_code = 0xB3;
		constexpr std::uint8_t extension_start_code = 0xB5;
		constexpr std::uint8_t user_data_start_code = 0xB2;

		std::string mpeg2_profile(const std::uint32_t indication)
		{
			// The escape bit marks the 4:2:2 and multi-view profiles, each at its levels.
			if ((indication & 0x80) != 0)
			{
				switch (indication)
				{
				case 0x82:
				case 0x85: return "4:2:2";
				case 0x8A:
				case 0x8B:
				case 0x8D:
				case 0x8E: return "Multi-view";
				default: return {};
				}
			}

			switch (indication >> 4 & 7)
			{
			case 1: return "High";
			case 2: return "Spatially Scalable";
			case 3: return "SNR Scalable";
			case 4: return "Main";
			case 5: return "Simple";
			default: return {};
			}
		}

		void set_mpeg_frame_rate(video_header& h, const std::uint32_t code, const std::uint32_t numerator, const std::uint32_t denominator)
		{
			if (code == 0 || code >= std::size(mpeg_frame_rates)) return;
			h.frame_rate = static_cast<double>(mpeg_frame_rates[code][0]) * numerator / (static_cast<double>(mpeg_frame_rates[code][1]) * denominator);
		}

		// The colour a sequence display extension describes, when one follows the sequence extension
		// among its extensions and user data.
		void read_sequence_display_extension(const std::span<const std::uint8_t> data, const std::size_t from, video_header& h)
		{
			for (auto at = find_start_code(data, from); at + 4 <= data.size(); at = find_start_code(data, at + 4))
			{
				const auto code = data[at + 3];
				if (code == user_data_start_code) continue;
				if (code != extension_start_code) return;

				const auto end = find_start_code(data, at + 4);
				const auto body = data.subspan(at + 4, end - (at + 4));
				if (body.empty() || (body[0] >> 4) != 2) continue;

				bit_reader b(body);
				// The identifier, then video_format.
				b.skip(4 + 3);

				if (b.flag())
				{
					const auto primaries = b.u(8);
					const auto transfer = b.u(8);
					const auto matrix = b.u(8);
					if (!b.overrun()) set_colour(h, primaries, transfer, matrix);
				}

				return;
			}
		}

		// ---- Motion JPEG -----------------------------------------------------------------------------

		bool is_frame_header(const std::uint8_t marker)
		{
			// SOF0 to SOF15, which share their codes with DHT, JPG and DAC.
			return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
		}

		// The luma's sampling factors over the chroma's, which must agree.
		chroma_format jpeg_subsampling(const std::uint8_t luma, const std::uint8_t blue, const std::uint8_t red)
		{
			const auto h = luma >> 4;
			const auto v = luma & 15;
			const auto chroma_h = blue >> 4;
			const auto chroma_v = blue & 15;
			if (blue != red || chroma_h == 0 || chroma_v == 0 || h % chroma_h != 0 || v % chroma_v != 0) return chroma_format::unknown;

			switch ((h / chroma_h) << 4 | v / chroma_v)
			{
			case 0x11: return chroma_format::yuv444;
			case 0x21: return chroma_format::yuv422;
			case 0x22: return chroma_format::yuv420;
			case 0x12: return chroma_format::yuv440;
			case 0x41: return chroma_format::yuv411;
			default: return chroma_format::unknown;
			}
		}

		// What the frame's markers said before its frame header.
		struct jpeg_markers
		{
			// AVI1's polarity: 1 or 2 when the image is one field, its other field the next image.
			unsigned polarity = 0;
			// The Adobe marker's transform; 0 is RGB.
			int adobe_transform = -1;
			// FFmpeg's comment for a frame it coded at video levels.
			bool itu601 = false;
		};

		bool read_jpeg_frame_header(const std::uint8_t marker, const std::span<const std::uint8_t> frame, const jpeg_markers& markers, video_header& out)
		{
			if (frame.size() < 6) return false;

			const auto precision = frame[0];
			std::uint64_t height = load_be16(frame.data() + 1);
			const std::uint64_t width = load_be16(frame.data() + 3);
			const auto components = frame[5];
			if (precision == 0 || precision > 16 || components == 0 || frame.size() < 6 + 3u * components) return false;

			video_header h;

			if (markers.polarity == 1 || markers.polarity == 2)
			{
				h.interlaced = true;
				height *= 2;
			}

			if (!valid_size(width, height)) return false;

			set_size(h, width, height);
			h.bit_depth = precision;

			if (components == 1)
			{
				h.chroma = chroma_format::gray;
			}
			else if (components == 3)
			{
				// Each component: its ID, its sampling factors and its quantisation table.
				if ((frame[6] == 'R' && frame[9] == 'G' && frame[12] == 'B') || markers.adobe_transform == 0)
				{
					h.chroma = chroma_format::rgb;
				}
				else
				{
					h.chroma = jpeg_subsampling(frame[7], frame[10], frame[13]);
					// JFIF's YCbCr is BT.601's at full range.
					h.matrix = 5;
					h.full_range = !markers.itu601;
				}
			}

			switch (marker)
			{
			case 0xC0: h.profile = "Baseline";
				break;
			case 0xC1: h.profile = "Sequential";
				break;
			case 0xC2: h.profile = "Progressive";
				break;
			case 0xC3: h.profile = "Lossless";
				break;
			default:
				break;
			}

			out = std::move(h);
			return true;
		}

		// ---- DV --------------------------------------------------------------------------------------

		constexpr std::size_t dif_block = 80;
		// The VAUX source and source control packs, where the third VAUX block of a frame's first DIF
		// sequence keeps them: packs 9 and 10 of its 15, after the block's ID.
		constexpr std::size_t source_pack = 5 * dif_block + 3 + 9 * 5;
		constexpr std::size_t control_pack = source_pack + 5;

		// ---- VC-1 ------------------------------------------------------------------------------------

		constexpr const char* vc1_profiles[4] = {"Simple", "Main", "Complex", "Advanced"};
		constexpr std::uint32_t vc1_simple = 0;
		constexpr std::uint32_t vc1_advanced = 3;
		constexpr std::uint8_t vc1_sequence_header_code = 0x0F;
		constexpr std::uint8_t vc1_entry_point_code = 0x0E;
		// The least of the Advanced profile's record FFmpeg's decoder takes.
		constexpr std::size_t vc1_least_record = 16;

		// FRAMERATENR's rates, and FRAMERATEDR's divisors.
		constexpr std::uint32_t vc1_rates[7] = {24, 25, 30, 50, 60, 48, 72};
		constexpr std::uint32_t vc1_divisors[2] = {1000, 1001};

		// What a record's headers leave for those after them, as FFmpeg's decoder keeps it: a sequence
		// header without a display extension leaves an earlier one's pixel shape and colour standing.
		struct vc1_state
		{
			// The Advanced profile's largest coded size, and the leaky buckets its entry points repeat.
			std::uint32_t coded_width = 0;
			std::uint32_t coded_height = 0;
			std::uint32_t leaky_buckets = 0;
			// The pixel shape the display extension states, or its display size when it leaves the pixel's
			// to follow from the picture's.
			std::uint32_t aspect_width = 0;
			std::uint32_t aspect_height = 0;
			bool aspect_from_display = false;
			std::uint32_t primaries = 0;
			std::uint32_t transfer = 0;
			std::uint32_t matrix = 0;
		};

		// Where the next start code - 00 00 01 and its code - begins, at or after from; the size when
		// none does.
		std::size_t find_vc1_start_code(const std::span<const std::uint8_t> data, std::size_t from)
		{
			for (; from + 4 <= data.size(); ++from)
			{
				if (data[from] == 0 && data[from + 1] == 0 && data[from + 2] == 1) return from;
			}

			return data.size();
		}

		// VC-1's emulation prevention, as FFmpeg removes it: a 03 after two zeros and before a byte below
		// 4, in a unit of four bytes or more.
		std::vector<std::uint8_t> vc1_unescape(std::span<const std::uint8_t> unit)
		{
			unit = unit.first(std::min(unit.size(), max_parameter_set));

			std::vector<std::uint8_t> out;
			out.reserve(unit.size());

			for (std::size_t i = 0; i < unit.size(); ++i)
			{
				if (unit.size() >= 4 && i >= 2 && unit[i] == 3 && unit[i - 1] == 0 && unit[i - 2] == 0 && i + 1 < unit.size() && unit[i + 1] < 4)
				{
					out.push_back(unit[i + 1]);
					++i;
				}
				else
				{
					out.push_back(unit[i]);
				}
			}

			return out;
		}

		// The Advanced profile's sequence header after PROFILE, as FFmpeg's decoder reads it. FFmpeg
		// refuses chroma other than 4:2:0, and progressive segmented frames.
		bool read_vc1_advanced(bit_reader& b, video_header& h, vc1_state& s)
		{
			// LEVEL, which FFmpeg reports and a video_header has no place for, then COLORDIFF_FORMAT.
			b.skip(3);
			if (b.u(2) != 1) return false;

			// FRMRTQ_POSTPROC, BITRTQ_POSTPROC and POSTPROCFLAG, then the largest coded size.
			b.skip(3 + 5 + 1);
			s.coded_width = (b.u(12) + 1) * 2;
			s.coded_height = (b.u(12) + 1) * 2;

			// PULLDOWN, INTERLACE, TFCNTRFLAG, FINTERPFLAG, a reserved bit, and PSF.
			b.skip(1);
			h.interlaced = b.flag();
			b.skip(3);
			if (b.flag() || b.overrun()) return false;

			// DISPLAY_EXT, which decoding ignores. Each part of it is kept when read whole.
			if (b.flag())
			{
				const auto display_width = b.u(14) + 1;
				const auto display_height = b.u(14) + 1;
				const auto ratio = b.flag() ? b.u(4) : 0u;

				// ASPECT_RATIO names a pixel shape, or at 15 states one; otherwise the display's shape
				// gives it.
				auto width = display_width;
				auto height = display_height;

				if (ratio > 0 && ratio < 14)
				{
					width = sample_aspects[ratio][0];
					height = sample_aspects[ratio][1];
				}
				else if (ratio == 15)
				{
					width = b.u(8) + 1;
					height = b.u(8) + 1;
				}

				if (!b.overrun())
				{
					s.aspect_width = width;
					s.aspect_height = height;
					s.aspect_from_display = ratio == 0 || ratio == 14;
				}

				// FRAMERATE_FLAG: then a rate in 32nds, or one of FRAMERATENR's rates over 1000 or 1001.
				if (b.flag())
				{
					auto rate = 0.0;

					if (b.flag())
					{
						rate = (b.u(16) + 1) / 32.0;
					}
					else
					{
						const auto numerator = b.u(8);
						const auto divisor = b.u(4);
						if (numerator > 0 && numerator < 8 && divisor > 0 && divisor < 3) rate = vc1_rates[numerator - 1] * 1000.0 / vc1_divisors[divisor - 1];
					}

					if (!b.overrun() && rate > 0) h.frame_rate = rate;
				}

				// COLOR_FORMAT_FLAG.
				if (b.flag())
				{
					const auto primaries = b.u(8);
					const auto transfer = b.u(8);
					const auto matrix = b.u(8);

					if (!b.overrun())
					{
						s.primaries = primaries;
						s.transfer = transfer;
						s.matrix = matrix;
					}
				}
			}

			// HRD_PARAM_FLAG, then the number of leaky buckets, for each of which an entry point has a byte.
			s.leaky_buckets = b.flag() ? b.u(5) : 0u;
			return true;
		}

		// A sequence header as FFmpeg's decoder reads one: STRUCT_C for the profiles before Advanced, whose
		// own header follows PROFILE in its place. What FFmpeg will not decode is refused.
		bool read_vc1_sequence_header(bit_reader& b, video_header& h, vc1_state& s)
		{
			const auto profile = b.u(2);
			h.profile = vc1_profiles[profile];
			h.chroma = chroma_format::yuv420;
			h.bit_depth = 8;
			if (profile == vc1_advanced) return read_vc1_advanced(b, h, s);

			// RES_Y411, the old interlaced mode, then RES_SPRITE.
			if (b.flag()) return false;
			const auto sprite = b.flag();

			// FRMRTQ_POSTPROC, BITRTQ_POSTPROC, LOOPFILTER, RES_X8, MULTIRES and RES_FASTTX; then
			// FASTUVMC and EXTENDED_MV, which the Simple profile must have on and off.
			b.skip(3 + 5 + 4);
			const auto fast_chroma = b.flag();
			const auto extended_mv = b.flag();
			if (profile == vc1_simple && (!fast_chroma || extended_mv)) return false;

			// DQUANT and VSTRANSFORM, then RES_TRANSTAB, which must be clear.
			b.skip(3);
			if (b.flag()) return false;

			// OVERLAP, RESYNCMARKER, RANGERED, MAXBFRAMES, QUANTIZER and FINTERPFLAG.
			b.skip(1 + 1 + 1 + 3 + 2 + 1);

			if (sprite)
			{
				// The sprite's size, which FFmpeg's WMV3 decoder reports as the picture's; then its frame
				// rate, RES_X8, a bit FFmpeg cannot decode when set, and the slice code.
				const auto width = b.u(11);
				const auto height = b.u(11);
				b.skip(5 + 1);
				if (b.flag() || width == 0 || height == 0) return false;
				b.skip(3);
				set_size(h, width, height);
			}
			else
			{
				// RES_RTM_FLAG.
				b.skip(1);
			}

			return !b.overrun();
		}

		// An entry point as FFmpeg's decoder reads one, for the coded size it states or leaves at the
		// sequence header's largest.
		bool read_vc1_entry_point(bit_reader& b, const vc1_state& s, video_header& h)
		{
			// BROKEN_LINK, CLOSED_ENTRY, PANSCAN_FLAG, REFDIST_FLAG, LOOPFILTER, FASTUVMC, EXTENDED_MV,
			// DQUANT, VSTRANSFORM, OVERLAP and QUANTIZER, then HRD_FULL for each leaky bucket.
			b.skip(13 + 8 * std::uint64_t{s.leaky_buckets});

			auto width = s.coded_width;
			auto height = s.coded_height;

			if (b.flag())
			{
				width = (b.u(12) + 1) * 2;
				height = (b.u(12) + 1) * 2;
			}

			if (b.overrun() || !valid_size(width, height)) return false;

			set_size(h, width, height);
			return true;
		}

		// What FFmpeg's decoder makes of the headers once it has read them all: the pixel shape for the
		// picture it decodes, and only the colour code points it maps, which H.273 shares.
		void finish_vc1(video_header& h, const vc1_state& s)
		{
			if (s.aspect_width != 0)
			{
				// FFmpeg measures the picture by the container's size, which the record gives as the coded
				// size: the entry point's, or without one the largest. Measured larger than the largest,
				// the picture has no shape from the display's.
				const auto width = h.width != 0 ? h.width : s.coded_width;
				const auto height = h.width != 0 ? h.height : s.coded_height;
				auto aspect_width = s.aspect_width;
				auto aspect_height = s.aspect_height;

				if (s.aspect_from_display)
				{
					aspect_width = height * s.aspect_width;
					aspect_height = width * s.aspect_height;
				}

				// FFmpeg also drops a shape that would leave the picture no pixel wide or high.
				const auto scaled = aspect_width < aspect_height ? std::uint64_t{width} * aspect_width / aspect_height : std::uint64_t{height} * aspect_height / aspect_width;
				const auto measured = !s.aspect_from_display || (width <= s.coded_width && height <= s.coded_height);
				if (measured && (aspect_width == aspect_height || scaled > 0)) set_pixel_aspect(h, aspect_width, aspect_height);
			}

			if (s.primaries == 1 || s.primaries == 5 || s.primaries == 6) h.color_primaries = static_cast<std::uint8_t>(s.primaries);
			if (s.transfer == 1 || s.transfer == 7) h.transfer = static_cast<std::uint8_t>(s.transfer);
			if (s.matrix == 1 || s.matrix == 6 || s.matrix == 7) h.matrix = static_cast<std::uint8_t>(s.matrix);
		}
	}

	bool parse_h264_sps(const std::span<const std::uint8_t> nal, video_header& out)
	{
		// forbidden_zero_bit clear, and nal_unit_type 7.
		if (nal.size() < 2 || (nal[0] & 0x9F) != 7) return false;

		const auto rbsp = unescape(nal.subspan(1));
		bit_reader b(rbsp);
		video_header h;

		const auto profile_idc = b.u(8);
		const auto constraints = b.u(8);
		// level_idc, then seq_parameter_set_id.
		b.skip(8);
		if (b.ue() > 31) return false;

		std::uint32_t chroma_format_idc = 1;
		auto separate_planes = false;
		std::uint32_t depth = 8;

		if (std::ranges::find(h264_high_profiles, profile_idc) != std::end(h264_high_profiles))
		{
			chroma_format_idc = b.ue();
			if (chroma_format_idc > 3) return false;
			if (chroma_format_idc == 3) separate_planes = b.flag();

			const auto luma_depth = b.ue();
			const auto chroma_depth = b.ue();
			if (luma_depth > 6 || chroma_depth > 6) return false;
			depth = 8 + luma_depth;

			// qpprime_y_zero_transform_bypass_flag, then seq_scaling_matrix_present_flag.
			b.skip(1);

			if (b.flag())
			{
				const auto lists = chroma_format_idc != 3 ? 8u : 12u;

				for (unsigned i = 0; i < lists; ++i)
				{
					if (b.flag()) skip_scaling_list(b, i < 6 ? 16u : 64u);
				}
			}
		}

		// log2_max_frame_num_minus4, then the picture order count's type and what that type needs.
		if (b.ue() > 12) return false;

		const auto poc_type = b.ue();

		if (poc_type == 0)
		{
			if (b.ue() > 12) return false;
		}
		else if (poc_type == 1)
		{
			// delta_pic_order_always_zero_flag and two offsets, then the offsets of the cycle.
			b.skip(1);
			b.se();
			b.se();

			const auto cycle = b.ue();
			if (cycle > 255) return false;

			for (std::uint32_t i = 0; i < cycle; ++i) b.se();
		}
		else if (poc_type != 2)
		{
			return false;
		}

		// max_num_ref_frames and gaps_in_frame_num_value_allowed_flag.
		if (b.ue() > 16) return false;
		b.skip(1);

		const auto width_in_mbs = std::uint64_t{b.ue()} + 1;
		const auto height_in_map_units = std::uint64_t{b.ue()} + 1;
		const auto frame_mbs_only = b.flag();
		// mb_adaptive_frame_field_flag, then direct_8x8_inference_flag.
		if (!frame_mbs_only) b.skip(1);
		b.skip(1);

		std::uint64_t crop_left = 0;
		std::uint64_t crop_right = 0;
		std::uint64_t crop_top = 0;
		std::uint64_t crop_bottom = 0;

		if (b.flag())
		{
			crop_left = b.ue();
			crop_right = b.ue();
			crop_top = b.ue();
			crop_bottom = b.ue();
		}

		if (b.overrun()) return false;

		// Crop offsets count chroma samples, and lines of a field when frames may be coded as fields.
		const auto array_type = separate_planes ? 0 : chroma_format_idc;
		const std::uint64_t field_lines = frame_mbs_only ? 1 : 2;
		const std::uint64_t unit_x = array_type == 1 || array_type == 2 ? 2 : 1;
		const std::uint64_t unit_y = (array_type == 1 ? 2 : 1) * field_lines;
		const auto coded_width = width_in_mbs * 16;
		const auto coded_height = height_in_map_units * 16 * field_lines;
		const auto crop_x = unit_x * (crop_left + crop_right);
		const auto crop_y = unit_y * (crop_top + crop_bottom);
		if (crop_x >= coded_width || crop_y >= coded_height || !valid_size(coded_width - crop_x, coded_height - crop_y)) return false;

		set_size(h, coded_width - crop_x, coded_height - crop_y);
		h.chroma = chroma_from_idc(chroma_format_idc);
		h.bit_depth = static_cast<std::uint8_t>(depth);
		h.interlaced = !frame_mbs_only;
		h.profile = h264_profile(profile_idc, constraints);

		// The VUI tells a player nothing it needs to decode; what of it is read whole is kept.
		if (b.flag())
		{
			read_vui_start(b, h);

			if (b.flag())
			{
				const auto units = b.u(32);
				const auto scale = b.u(32);
				// A tick is a field.
				if (!b.overrun() && units != 0 && scale != 0) h.frame_rate = scale / (2.0 * units);
			}
		}

		rgb_if_identity(h);
		out = std::move(h);
		return true;
	}

	bool parse_avc_config(const std::span<const std::uint8_t> config, video_header& out)
	{
		if (config.size() < 7 || config[0] != 1) return false;

		const auto sets = config[5] & 0x1Fu;
		std::size_t at = 6;

		for (unsigned i = 0; i < sets; ++i)
		{
			if (config.size() - at < 2) return false;

			const auto length = load_be16(config.data() + at);
			at += 2;
			if (config.size() - at < length) return false;

			if (parse_h264_sps(config.subspan(at, length), out)) return true;
			at += length;
		}

		return false;
	}

	bool parse_hevc_sps(const std::span<const std::uint8_t> nal, video_header& out)
	{
		// forbidden_zero_bit clear, nal_unit_type 33, and a temporal ID.
		if (nal.size() < 3 || (nal[0] & 0x80) != 0 || (nal[0] >> 1) != 33 || (nal[1] & 7) == 0) return false;

		const auto rbsp = unescape(nal.subspan(2));
		bit_reader b(rbsp);
		video_header h;

		// sps_video_parameter_set_id, then sps_max_sub_layers_minus1; 7 marks a layer's SPS that takes
		// its format from the VPS, which is not read here.
		b.skip(4);
		const auto sub_layers = b.u(3);
		if (sub_layers == 7) return false;
		b.skip(1);

		const auto ptl = read_profile_tier_level(b, sub_layers);
		if (b.ue() > 15) return false;

		const auto chroma_format_idc = b.ue();
		if (chroma_format_idc > 3) return false;

		auto separate_planes = false;
		if (chroma_format_idc == 3) separate_planes = b.flag();

		const std::uint64_t coded_width = b.ue();
		const std::uint64_t coded_height = b.ue();
		std::uint64_t crop_x = 0;
		std::uint64_t crop_y = 0;

		if (b.flag())
		{
			// The conformance window counts chroma samples.
			const auto array_type = separate_planes ? 0 : chroma_format_idc;
			const std::uint64_t unit_x = array_type == 1 || array_type == 2 ? 2 : 1;
			const std::uint64_t unit_y = array_type == 1 ? 2 : 1;
			const std::uint64_t left = b.ue();
			const std::uint64_t right = b.ue();
			const std::uint64_t top = b.ue();
			const std::uint64_t bottom = b.ue();
			crop_x = unit_x * (left + right);
			crop_y = unit_y * (top + bottom);
		}

		const auto luma_depth = b.ue();
		const auto chroma_depth = b.ue();
		if (b.overrun() || luma_depth > 8 || chroma_depth > 8) return false;
		if (crop_x >= coded_width || crop_y >= coded_height || !valid_size(coded_width - crop_x, coded_height - crop_y)) return false;

		set_size(h, coded_width - crop_x, coded_height - crop_y);
		h.chroma = chroma_from_idc(chroma_format_idc);
		h.bit_depth = static_cast<std::uint8_t>(8 + luma_depth);
		h.profile = hevc_profile(ptl.profile_idc, ptl.compatibility);
		h.interlaced = ptl.interlaced_source && !ptl.progressive_source;

		// What follows the depths matters only for the VUI after it; damage there keeps the picture.
		if (skip_to_hevc_vui(b, sub_layers) && b.flag()) read_hevc_vui(b, h);

		rgb_if_identity(h);
		out = std::move(h);
		return true;
	}

	bool parse_hevc_config(const std::span<const std::uint8_t> config, video_header& out)
	{
		// Version 1, or 0 from before the record was final. A start code instead is an Annex B stream,
		// which some muxers store in its place.
		if (config.size() < 23 || config[0] > 1 || (config[0] == 0 && config[1] == 0 && config[2] <= 1)) return false;

		video_header h;
		h.profile = hevc_profile(config[1] & 0x1Fu, load_be32(config.data() + 2));
		h.chroma = chroma_from_idc(config[16] & 3u);
		h.bit_depth = static_cast<std::uint8_t>(8 + (config[17] & 7));
		// general_progressive_source_flag and general_interlaced_source_flag lead the constraint flags.
		h.interlaced = (config[6] & 0xC0) == 0x40;

		// The base layer's first SPS describes the same picture in full, as a decoder reads it.
		video_header sps;

		const auto found = [&]
		{
			std::size_t at = 23;

			for (unsigned i = 0; i < config[22]; ++i)
			{
				if (config.size() - at < 3) return false;

				const auto type = config[at] & 0x3Fu;
				const auto units = load_be16(config.data() + at + 1);
				at += 3;

				for (unsigned j = 0; j < units; ++j)
				{
					if (config.size() - at < 2) return false;

					const auto length = load_be16(config.data() + at);
					at += 2;
					if (config.size() - at < length) return false;

					const auto nal = config.subspan(at, length);
					at += length;

					// nuh_layer_id spans the NAL unit header's two bytes.
					const auto base_layer = nal.size() >= 2 && (nal[0] & 1) == 0 && (nal[1] >> 3) == 0;
					if (type == 33 && base_layer && parse_hevc_sps(nal, sps)) return true;
				}
			}

			return false;
		}();

		out = std::move(found ? sps : h);
		return true;
	}

	bool parse_av1_sequence(const std::span<const std::uint8_t> obus, video_header& out)
	{
		std::size_t at = 0;

		while (at < obus.size())
		{
			const auto header = obus[at];
			if ((header & 0x80) != 0) return false;

			const auto type = header >> 3 & 0x0F;
			// The header, and its extension byte when obu_extension_flag is set.
			auto start = at + ((header & 0x04) != 0 ? 2 : 1);
			if (start > obus.size()) return false;

			std::uint64_t size = obus.size() - start;
			if ((header & 0x02) != 0 && !read_leb128(obus, start, size)) return false;

			const std::uint64_t available = obus.size() - start;
			if (type == 1) return read_sequence_header_obu(obus.subspan(start, static_cast<std::size_t>(std::min(size, available))), out);
			if (size > available) return false;

			at = start + static_cast<std::size_t>(size);
		}

		return false;
	}

	bool parse_av1_config(const std::span<const std::uint8_t> config, video_header& out)
	{
		// The marker bit and version 1.
		if (config.size() < 4 || config[0] != 0x81) return false;

		const auto profile = config[1] >> 5u;
		if (profile > 2) return false;

		const auto flags = config[2];
		const auto high_bitdepth = (flags & 0x40) != 0;
		const auto twelve_bit = (flags & 0x20) != 0;

		video_header h;
		h.profile = av1_profile(profile);
		h.bit_depth = static_cast<std::uint8_t>(high_bitdepth ? (profile == 2 && twelve_bit ? 12 : 10) : 8);

		// monochrome, then chroma_subsampling_x and chroma_subsampling_y.
		if ((flags & 0x10) != 0) h.chroma = chroma_format::gray;
		else if ((flags & 0x0C) == 0x0C) h.chroma = chroma_format::yuv420;
		else if ((flags & 0x0C) == 0x08) h.chroma = chroma_format::yuv422;
		else if ((flags & 0x0C) == 0) h.chroma = chroma_format::yuv444;

		// A sequence header among the configOBUs says all of this and more.
		video_header sequence;
		const auto found = parse_av1_sequence(config.subspan(4), sequence);
		out = std::move(found ? sequence : h);
		return true;
	}

	bool parse_vp_config(const std::span<const std::uint8_t> config, video_header& out)
	{
		if (config.size() < 6) return false;

		const auto profile = config[0];
		const auto depth = config[2] >> 4;
		const auto subsampling = config[2] >> 1 & 7;
		if (profile > 3 || (depth != 8 && depth != 10 && depth != 12) || subsampling > 3) return false;

		video_header h;
		h.profile = numbered_profile(profile);
		h.bit_depth = static_cast<std::uint8_t>(depth);
		// 0 and 1 are both 4:2:0, the chroma sited differently.
		h.chroma = subsampling < 2 ? chroma_format::yuv420 : subsampling == 2 ? chroma_format::yuv422 : chroma_format::yuv444;
		h.full_range = (config[2] & 1) != 0;
		set_colour(h, config[3], config[4], config[5]);
		rgb_if_identity(h);
		out = std::move(h);
		return true;
	}

	bool parse_vp9_frame(const std::span<const std::uint8_t> frame, video_header& out)
	{
		bit_reader b(frame);
		video_header h;

		if (b.u(2) != 2) return false;

		const auto low = b.u(1);
		const auto profile = b.u(1) << 1 | low;
		if (profile == 3 && b.flag()) return false;

		// show_existing_frame: a frame decoded before, which says nothing more.
		if (b.flag()) return false;

		const auto key_frame = !b.flag();
		const auto show_frame = b.flag();
		const auto error_resilient = b.flag();
		h.profile = numbered_profile(profile);

		if (key_frame)
		{
			if (b.u(24) != vp9_sync_code || !read_vp9_color_config(b, profile, h)) return false;
		}
		else
		{
			// intra_only, for a frame not shown; then reset_frame_context.
			const auto intra_only = !show_frame && b.flag();
			if (!error_resilient) b.skip(2);

			if (intra_only)
			{
				if (b.u(24) != vp9_sync_code) return false;

				if (profile > 0)
				{
					if (!read_vp9_color_config(b, profile, h)) return false;
				}
				else
				{
					// Profile 0's intra-only frames are BT.601 4:2:0 at 8 bits, without saying so.
					h.bit_depth = 8;
					h.chroma = chroma_format::yuv420;
					h.matrix = 5;
				}

				b.skip(8);
			}
			else
			{
				// refresh_frame_flags, and each reference's index and sign bias.
				b.skip(8 + 3 * 4);

				// found_ref: the size of a reference frame, which is not in this one.
				for (auto i = 0; i < 3; ++i)
				{
					if (b.flag()) return false;
				}
			}
		}

		const auto width = std::uint64_t{b.u(16)} + 1;
		const auto height = std::uint64_t{b.u(16)} + 1;
		if (b.overrun() || !valid_size(width, height)) return false;

		set_size(h, width, height);
		out = std::move(h);
		return true;
	}

	bool parse_vp8_frame(const std::span<const std::uint8_t> frame, video_header& out)
	{
		// The frame tag, whose low bit is clear for a key frame, then a key frame's start code.
		if (frame.size() < 10 || (frame[0] & 1) != 0 || frame[3] != 0x9D || frame[4] != 0x01 || frame[5] != 0x2A) return false;

		// The top two bits of each are an upscaling a player may apply, not the coded size.
		const auto width = load_le16(frame.data() + 6) & 0x3FFFu;
		const auto height = load_le16(frame.data() + 8) & 0x3FFFu;
		if (width == 0 || height == 0) return false;

		video_header h;
		h.width = width;
		h.height = height;
		h.chroma = chroma_format::yuv420;
		h.bit_depth = 8;

		const auto version = frame[0] >> 1 & 7u;
		if (version <= 3) h.profile = numbered_profile(version);

		out = std::move(h);
		return true;
	}

	bool parse_mpeg4_video(const std::span<const std::uint8_t> data, video_header& out)
	{
		video_header h;
		auto verid = 1u;

		for (auto at = find_start_code(data, 0); at + 4 <= data.size();)
		{
			const auto code = data[at + 3];
			const auto next = find_start_code(data, at + 4);
			const auto body = data.subspan(at + 4, next - (at + 4));

			// The visual object sequence's profile and level, a visual object, a video object layer.
			if (code == 0xB0 && !body.empty()) h.profile = mpeg4_profiles[body[0] >> 4];
			else if (code == 0xB5) read_visual_object(body, verid, h);
			else if (code >= 0x20 && code <= 0x2F) return read_video_object_layer(body, verid, h, out);

			at = next;
		}

		return false;
	}

	bool parse_mpeg12_video(const std::span<const std::uint8_t> data, video_header& out)
	{
		auto at = find_start_code(data, 0);
		while (at + 4 <= data.size() && data[at + 3] != sequence_header_code) at = find_start_code(data, at + 3);
		if (at + 4 > data.size()) return false;

		bit_reader b(data.subspan(at + 4));
		auto width = b.u(12);
		auto height = b.u(12);
		const auto aspect = b.u(4);
		const auto rate_code = b.u(4);
		b.skip(18);
		if (!b.flag()) return false;

		// vbv_buffer_size_value and constrained_parameters_flag, then the two quantiser matrices.
		b.skip(10 + 1);
		if (b.flag()) b.skip(64 * 8);
		if (b.flag()) b.skip(64 * 8);
		if (b.overrun()) return false;

		video_header h;
		h.chroma = chroma_format::yuv420;
		h.bit_depth = 8;

		// The header ends on a byte. An MPEG-2 sequence extension starts at the next start code; data
		// that ends with the header is MPEG-1, and anything there but a start code is damage.
		const auto end = at + 4 + static_cast<std::size_t>(b.position() / 8);
		auto next = end;
		while (next < data.size() && data[next] == 0) ++next;

		const auto follows = next < data.size();
		if (next > end && !follows) return false;
		if (follows && (next - end < 2 || data[next] != 1 || next + 1 >= data.size())) return false;

		const auto extension = follows && data[next + 1] == extension_start_code;
		if (extension && next + 2 >= data.size()) return false;

		if (!extension || (data[next + 2] >> 4) != 1)
		{
			if (width == 0 || height == 0) return false;

			set_size(h, width, height);
			if (aspect < std::size(mpeg1_pixel_aspects)) set_pixel_aspect(h, mpeg1_pixel_aspects[aspect][0], mpeg1_pixel_aspects[aspect][1]);
			set_mpeg_frame_rate(h, rate_code, 1, 1);
			out = std::move(h);
			return true;
		}

		bit_reader e(data.subspan(next + 2));
		e.skip(4);
		const auto indication = e.u(8);
		const auto progressive = e.flag();
		const auto chroma = e.u(2);
		width |= e.u(2) << 12;
		height |= e.u(2) << 12;
		// bit_rate_extension, marker_bit, vbv_buffer_size_extension and low_delay.
		e.skip(12 + 1 + 8 + 1);
		const auto rate_numerator = e.u(2) + 1;
		const auto rate_denominator = e.u(5) + 1;
		if (e.overrun() || width == 0 || height == 0) return false;

		set_size(h, width, height);
		h.profile = mpeg2_profile(indication);
		h.interlaced = !progressive;
		// 0 is reserved; FFmpeg takes it for 4:2:0.
		if (chroma == 2) h.chroma = chroma_format::yuv422;
		if (chroma == 3) h.chroma = chroma_format::yuv444;

		if (aspect == 1)
		{
			set_pixel_aspect(h, 1, 1);
		}
		else if (aspect < std::size(mpeg2_display_aspects))
		{
			set_pixel_aspect(h, mpeg2_display_aspects[aspect][0] * height, mpeg2_display_aspects[aspect][1] * width);
		}

		set_mpeg_frame_rate(h, rate_code, rate_numerator, rate_denominator);
		read_sequence_display_extension(data, next + 2 + 6, h);
		out = std::move(h);
		return true;
	}

	bool parse_jpeg_frame(const std::span<const std::uint8_t> data, video_header& out)
	{
		if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xD8) return false;

		jpeg_markers markers;
		std::size_t at = 2;

		for (;;)
		{
			// A marker is FF and a code, after any fill bytes. Bytes before it that are not one are
			// passed over, as decoders pass over them.
			while (at < data.size() && data[at] != 0xFF) ++at;
			while (at < data.size() && data[at] == 0xFF) ++at;
			if (at >= data.size()) return false;

			const auto marker = data[at++];
			// 00 is no marker; TEM, RSTn and SOI stand alone. The image ends, or its scan begins,
			// before any frame header.
			if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;
			if (marker == 0xD9 || marker == 0xDA) return false;

			if (data.size() - at < 2) return false;

			const auto length = load_be16(data.data() + at);
			if (length < 2 || data.size() - at < length) return false;

			const auto segment = data.subspan(at + 2, length - 2u);
			at += length;

			if (is_frame_header(marker)) return read_jpeg_frame_header(marker, segment, markers, out);

			if (marker == 0xE0 && segment.size() >= 5 && starts_with(segment, "AVI1")) markers.polarity = segment[4];
			else if (marker == 0xEE && segment.size() >= 12 && starts_with(segment, "Adobe")) markers.adobe_transform = segment[11];
			else if (marker == 0xFE && starts_with(segment, "CS=ITU601")) markers.itu601 = true;
		}
	}

	bool parse_theora_header(const std::span<const std::uint8_t> data, video_header& out)
	{
		// The identification header of Theora 3.2, the only version the specification reads.
		if (data.size() < 42 || data[0] != 0x80 || !starts_with(data.subspan(1), "theora") || data[7] != 3 || data[8] != 2) return false;

		bit_reader b(data.subspan(10));
		const auto frame_width = b.u(16) * 16;
		const auto frame_height = b.u(16) * 16;
		const auto width = b.u(24);
		const auto height = b.u(24);
		const auto x = b.u(8);
		const auto y = b.u(8);
		const auto rate_numerator = b.u(32);
		const auto rate_denominator = b.u(32);
		const auto aspect_numerator = b.u(24);
		const auto aspect_denominator = b.u(24);
		const auto colour_space = b.u(8);
		// The nominal bit rate, the quality and the keyframe granule shift.
		b.skip(24 + 6 + 5);
		const auto pixel_format = b.u(2);

		// The picture lies within the frame of whole macroblocks; pixel format 1 is reserved.
		if (width > frame_width || height > frame_height || x > frame_width - width || y > frame_height - height) return false;
		if (pixel_format == 1 || !valid_size(width, height)) return false;

		video_header h;
		set_size(h, width, height);
		h.bit_depth = 8;
		h.chroma = pixel_format == 0 ? chroma_format::yuv420 : pixel_format == 2 ? chroma_format::yuv422 : chroma_format::yuv444;
		set_pixel_aspect(h, aspect_numerator, aspect_denominator);
		if (rate_numerator != 0 && rate_denominator != 0) h.frame_rate = static_cast<double>(rate_numerator) / rate_denominator;

		// Rec. 470M and 470BG, as FFmpeg reads them: their primaries, BT.601's matrix and BT.709's
		// transfer.
		if (colour_space == 1 || colour_space == 2)
		{
			h.color_primaries = static_cast<std::uint8_t>(colour_space == 1 ? 4 : 5);
			h.transfer = 1;
			h.matrix = 5;
		}

		out = std::move(h);
		return true;
	}

	bool parse_dv_frame(const std::span<const std::uint8_t> data, video_header& out)
	{
		// The header block: section type 0, the first DIF sequence, block 0; and the bit after DSF clear.
		if (data.size() < 5 || (data[0] & 0xE0) != 0 || (data[1] & 0xF0) != 0 || data[2] != 0 || (data[3] & 0x40) != 0) return false;

		// DSF: the 625/50 system. APT: 0 for IEC 61834, the consumer format; SMPTE 314M and 370M
		// otherwise.
		const auto fifty = (data[3] & 0x80) != 0;
		const auto consumer = (data[4] & 7) == 0;

		// With the first six blocks come the VAUX source pack, whose signal type tells the 50 Mbps and
		// HD formats from the 25 Mbps one, and the source control pack. The header block alone reads as
		// 25 Mbps.
		const auto vaux = data.size() >= 6 * dif_block && (data[5 * dif_block] & 0xE0) == 0x40;
		const auto signal_type = vaux && data[source_pack] == 0x60 ? data[source_pack + 3] & 0x1F : 0;
		const auto hd = signal_type == 0x14 || signal_type == 0x18;

		video_header h;
		h.bit_depth = 8;
		h.chroma = chroma_format::yuv422;
		h.frame_rate = fifty ? 25.0 : 30000.0 / 1001;

		switch (signal_type)
		{
		case 0:
		case 1:
			// Type 1 is IEC 61883-5's, for 625/50 alone. IEC 61834 samples the 625/50 system at 4:2:0,
			// and the rest of 25 Mbps DV is 4:1:1.
			if (signal_type == 1 && !fifty) return false;
			set_size(h, 720, fifty ? 576 : 480);
			h.chroma = fifty && (consumer || signal_type == 1) ? chroma_format::yuv420 : chroma_format::yuv411;
			break;
		case 4: set_size(h, 720, fifty ? 576 : 480);
			break;
		case 0x14: set_size(h, fifty ? 1440 : 1280, 1080);
			h.interlaced = true;
			break;
		case 0x18: set_size(h, 960, 720);
			h.frame_rate = fifty ? 50.0 : 60000.0 / 1001;
			break;
		default: return false;
		}

		if (vaux && data[control_pack] == 0x61)
		{
			// DISP: 2 is 16:9, and so is 7 in the consumer format. Each gives FFmpeg's pixel shape for
			// 525/60, 625/50, 1080i at 60 Hz or the other HD formats, a 4:3 display's and then a 16:9
			// one's. IL marks an interlaced picture, which the HD formats state by their size.
			constexpr std::uint32_t pixel_aspects[4][4] = {{8, 9, 32, 27}, {16, 15, 64, 45}, {1, 1, 3, 2}, {1, 1, 4, 3}};
			const auto display = data[control_pack + 2] & 7;
			const auto wide = display == 2 || (consumer && display == 7);
			const auto& shapes = pixel_aspects[!hd ? (fifty ? 1 : 0) : signal_type == 0x14 && !fifty ? 2 : 3];

			set_pixel_aspect(h, shapes[wide ? 2 : 0], shapes[wide ? 3 : 1]);
			if (!hd) h.interlaced = (data[control_pack + 3] & 0x10) != 0;
		}

		out = std::move(h);
		return true;
	}

	bool parse_vc1_sequence(const std::span<const std::uint8_t> data, video_header& out)
	{
		video_header h;
		vc1_state s;

		// Without the FourCC the bytes tell the two layouts apart. The Advanced profile's record is start
		// codes, which FFmpeg looks for anywhere in a record of 16 bytes or more, and which open one after
		// at most a byte; STRUCT_C is 4 bytes, 8 with a sprite, and perhaps a version byte.
		const auto first = find_vc1_start_code(data, 0);
		const auto opens_with_sequence = first < data.size() && first <= 1 && data[first + 3] == vc1_sequence_header_code;

		if (first < data.size() && (data.size() >= vc1_least_record || opens_with_sequence))
		{
			if (data.size() < vc1_least_record) return false;

			// Each sequence header and entry point in turn, a later one over an earlier; FFmpeg needs one
			// of each.
			auto sequence = false;
			auto entry_point = false;

			for (auto start = first; start < data.size();)
			{
				const auto next = find_vc1_start_code(data, start + 4);
				const auto code = data[start + 3];

				if (next > start + 4 && (code == vc1_sequence_header_code || code == vc1_entry_point_code))
				{
					const auto unit = vc1_unescape(data.subspan(start + 4, next - start - 4));
					bit_reader b(unit);

					if (code == vc1_sequence_header_code)
					{
						if (!read_vc1_sequence_header(b, h, s)) return false;
						sequence = true;
					}
					else
					{
						if (!read_vc1_entry_point(b, s, h)) return false;
						entry_point = true;
					}
				}

				start = next;
			}

			if (!sequence || !entry_point) return false;
		}
		else
		{
			// STRUCT_C, read as it lies. Its picture is the container's size, unless a sprite states one.
			bit_reader b(data);
			if (!read_vc1_sequence_header(b, h, s)) return false;
		}

		finish_vc1(h, s);
		out = std::move(h);
		return true;
	}

	void for_each_annexb_nal(const std::span<const std::uint8_t> data, const std::function<bool(std::span<const std::uint8_t>)>& visit)
	{
		for (auto at = find_start_code(data, 0); at < data.size();)
		{
			// A NAL unit ends where three zeros or the next start code begin, and zeros at its end are
			// padding.
			const auto start = at + 3;
			auto end = start;
			while (end + 3 <= data.size() && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] <= 1)) ++end;
			if (end + 3 > data.size()) end = data.size();

			auto last = end;
			while (last > start && data[last - 1] == 0) --last;
			if (last > start && !visit(data.subspan(start, last - start))) return;

			at = find_start_code(data, end);
		}
	}
}
