// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the video codec header parsers the media scanners share. Each header is built
// field by field with a bit writer and read back, then cut short at every byte and damaged; the
// configuration records of Diffractor's test files are read as FFmpeg reads them.

#include "test.h"
#include "builders.h"

#include "media_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
	using namespace test_build;
	using libfiles::chroma_format;
	using libfiles::detail::video_header;

	using parser = bool (*)(std::span<const std::uint8_t>, video_header&);

	// ---- Writing headers -----------------------------------------------------------------------------

	// Bits written most significant first, with the Exp-Golomb codes of H.264, HEVC and AV1.
	struct header_bits
	{
		bytes b;
		std::size_t count = 0;

		header_bits& u(const std::uint64_t value, const unsigned n)
		{
			for (unsigned i = n; i-- > 0; ++count)
			{
				if (count % 8 == 0) b.push_back(0);
				if ((value >> i & 1) != 0) b.back() = static_cast<std::uint8_t>(b.back() | 0x80u >> count % 8);
			}

			return *this;
		}

		header_bits& flag(const bool set) { return u(set ? 1 : 0, 1); }

		header_bits& ue(const std::uint64_t value)
		{
			const auto code = value + 1;
			unsigned length = 0;
			while ((code >> (length + 1)) != 0) ++length;
			return u(0, length).u(code, length + 1);
		}

		header_bits& se(const std::int64_t value)
		{
			return ue(value > 0 ? 2 * static_cast<std::uint64_t>(value) - 1 : 2 * static_cast<std::uint64_t>(-value));
		}

		header_bits& zeros()
		{
			while (count % 8 != 0) u(0, 1);
			return *this;
		}

		// rbsp_trailing_bits.
		header_bits& trailing() { return u(1, 1).zeros(); }

		// MPEG-4's next_start_code(): a zero, then ones to the byte.
		header_bits& stuffing()
		{
			u(0, 1);
			while (count % 8 != 0) u(1, 1);
			return *this;
		}
	};

	bytes hex(const std::string_view text)
	{
		const auto digit = [](const char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
		bytes out;

		for (std::size_t i = 0; i + 1 < text.size(); i += 2)
		{
			out.push_back(static_cast<std::uint8_t>(digit(text[i]) << 4 | digit(text[i + 1])));
		}

		return out;
	}

	// A NAL unit's payload as an encoder writes it, with emulation prevention bytes put in.
	bytes escape(const bytes& rbsp)
	{
		bytes out;
		auto zeros = 0;

		for (const auto c : rbsp)
		{
			if (zeros >= 2 && c <= 3)
			{
				out.push_back(3);
				zeros = 0;
			}

			out.push_back(c);
			zeros = c == 0 ? zeros + 1 : 0;
		}

		return out;
	}

	bool contains(const bytes& data, const bytes& run)
	{
		return std::search(data.begin(), data.end(), run.begin(), run.end()) != data.end();
	}

	bytes first(const bytes& data, const std::size_t n)
	{
		return {data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n)};
	}

	// ---- Reading them back ---------------------------------------------------------------------------

	video_header picture(const std::uint32_t width, const std::uint32_t height, const chroma_format chroma, const std::uint8_t depth,
	                     std::string profile = {})
	{
		video_header h;
		h.width = width;
		h.height = height;
		h.chroma = chroma;
		h.bit_depth = depth;
		h.profile = std::move(profile);
		return h;
	}

	video_header& aspect(video_header& h, const std::uint32_t width, const std::uint32_t height)
	{
		h.pixel_aspect_width = width;
		h.pixel_aspect_height = height;
		return h;
	}

	video_header& colour(video_header& h, const std::uint8_t primaries, const std::uint8_t transfer, const std::uint8_t matrix)
	{
		h.color_primaries = primaries;
		h.transfer = transfer;
		h.matrix = matrix;
		return h;
	}

	// What a parser that refuses must leave alone.
	video_header untouched()
	{
		auto h = picture(4321, 1234, chroma_format::yuv410, 3, "untouched");
		aspect(h, 7, 5);
		colour(h, 77, 78, 79);
		h.alpha = true;
		h.interlaced = true;
		h.full_range = true;
		h.frame_rate = 12.5;
		return h;
	}

	std::string differences(const video_header& actual, const video_header& wanted)
	{
		std::string out;

		const auto number = [&out](const char* name, const std::uint64_t a, const std::uint64_t w)
		{
			if (a != w) out += std::string(name) + " " + std::to_string(a) + " not " + std::to_string(w) + "; ";
		};

		number("width", actual.width, wanted.width);
		number("height", actual.height, wanted.height);
		number("pixel aspect width", actual.pixel_aspect_width, wanted.pixel_aspect_width);
		number("pixel aspect height", actual.pixel_aspect_height, wanted.pixel_aspect_height);
		number("chroma", static_cast<std::uint64_t>(actual.chroma), static_cast<std::uint64_t>(wanted.chroma));
		number("bit depth", actual.bit_depth, wanted.bit_depth);
		number("alpha", actual.alpha, wanted.alpha);
		number("interlaced", actual.interlaced, wanted.interlaced);
		number("full range", actual.full_range, wanted.full_range);
		number("primaries", actual.color_primaries, wanted.color_primaries);
		number("transfer", actual.transfer, wanted.transfer);
		number("matrix", actual.matrix, wanted.matrix);

		if (std::abs(actual.frame_rate - wanted.frame_rate) > 1e-9 * std::max(1.0, wanted.frame_rate))
		{
			out += "frame rate " + std::to_string(actual.frame_rate) + " not " + std::to_string(wanted.frame_rate) + "; ";
		}

		if (actual.profile != wanted.profile) out += "profile '" + actual.profile + "' not '" + wanted.profile + "'; ";
		return out;
	}

	// How what parse reads from data differs from what is wanted: empty when it is the same.
	std::string read_as(const parser parse, const bytes& data, const video_header& wanted)
	{
		video_header h;
		if (!parse(data, h)) return "refused";
		return differences(h, wanted);
	}

	video_header read(const parser parse, const bytes& data)
	{
		video_header h;
		CHECK(parse(data, h));
		return h;
	}

	// Refused, with the output left as it was.
	bool refuses(const parser parse, const bytes& data)
	{
		auto h = untouched();
		return !parse(data, h) && differences(h, untouched()).empty();
	}

	// Each field read from a header cut short is the whole header's, or one not read: a stated
	// identity matrix is what makes 4:4:4 RGB, and it may be cut off.
	bool agrees(const video_header& part, const video_header& whole)
	{
		const auto same_or = [](const auto p, const auto w, const auto unread) { return p == w || p == unread; };

		return same_or(part.width, whole.width, 0u) && same_or(part.height, whole.height, 0u) &&
			((part.pixel_aspect_width == whole.pixel_aspect_width && part.pixel_aspect_height == whole.pixel_aspect_height) ||
				(part.pixel_aspect_width == 0 && part.pixel_aspect_height == 0)) &&
			(same_or(part.chroma, whole.chroma, chroma_format::unknown) ||
				(part.chroma == chroma_format::yuv444 && whole.chroma == chroma_format::rgb)) &&
			same_or(part.bit_depth, whole.bit_depth, std::uint8_t{0}) && same_or(part.alpha, whole.alpha, false) &&
			same_or(part.interlaced, whole.interlaced, false) && same_or(part.full_range, whole.full_range, false) &&
			same_or(part.color_primaries, whole.color_primaries, std::uint8_t{2}) &&
			same_or(part.transfer, whole.transfer, std::uint8_t{2}) && same_or(part.matrix, whole.matrix, std::uint8_t{2}) &&
			same_or(part.frame_rate, whole.frame_rate, 0.0) && same_or(part.profile, whole.profile, std::string{});
	}

	using agreement = std::function<bool(const video_header& part, const video_header& whole)>;

	// Every prefix of a header is refused, the output left alone, or read as one that agrees with the
	// whole header. Each prefix is a copy of its own, so a read past its end is a read out of bounds.
	void check_cut_short(const parser parse, const bytes& whole, const agreement& agree = agrees)
	{
		video_header full;
		CHECK(parse(whole, full));

		for (std::size_t n = 0; n < whole.size(); ++n)
		{
			const auto part = first(whole, n);
			auto h = untouched();
			const auto ok = parse(part, h) ? agree(h, full) : differences(h, untouched()).empty();
			const auto what = "cut short at " + std::to_string(n) + " of " + std::to_string(whole.size()) + " bytes: " + differences(h, full);
			test::check(ok, what.c_str(), __FILE__, __LINE__);
		}
	}

	// A damaged header may read as anything sane, but never out of bounds, and a refusal leaves the
	// output alone: each byte flipped in turn, then runs of random bytes written over it.
	void check_damaged(const parser parse, const bytes& whole)
	{
		auto sane = true;

		const auto attempt = [&](const bytes& damaged)
		{
			auto h = untouched();

			if (parse(damaged, h))
			{
				sane = sane && h.width <= 65535 && h.height <= 65535 && h.frame_rate >= 0 && std::isfinite(h.frame_rate);
			}
			else
			{
				sane = sane && differences(h, untouched()).empty();
			}
		};

		for (std::size_t i = 0; i < whole.size(); ++i)
		{
			for (const unsigned mask : {0x01u, 0x10u, 0x80u, 0xFFu})
			{
				auto damaged = whole;
				damaged[i] = static_cast<std::uint8_t>(damaged[i] ^ mask);
				attempt(damaged);
			}
		}

		std::uint32_t seed = 0x2545F491;

		for (auto round = 0; round < 300; ++round)
		{
			auto damaged = whole;

			for (auto j = 0; j < 1 + round % 6; ++j)
			{
				seed = seed * 1664525 + 1013904223;
				damaged[seed % damaged.size()] = static_cast<std::uint8_t>(seed >> 24);
			}

			attempt(damaged);
			attempt(first(damaged, seed % damaged.size()));
		}

		CHECK(sane);
	}

	// ---- H.264 ---------------------------------------------------------------------------------------

	constexpr unsigned h264_chroma_profiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135, 144};

	struct h264_sps
	{
		unsigned profile_idc = 100;
		unsigned constraints = 0;
		unsigned chroma_format_idc = 1;
		bool separate_planes = false;
		unsigned luma_depth = 8;
		unsigned chroma_depth = 8;
		bool scaling_lists = false;
		unsigned poc_type = 0;
		unsigned width_in_mbs = 40;
		unsigned height_in_map_units = 30;
		bool frame_mbs_only = true;
		// Left, right, top and bottom.
		std::array<unsigned, 4> crop{};
		bool vui = false;
		int aspect_idc = -1;
		unsigned sar_width = 0;
		unsigned sar_height = 0;
		bool signal = false;
		bool full_range = false;
		bool colour = false;
		unsigned primaries = 2;
		unsigned transfer = 2;
		unsigned matrix = 2;
		bool timing = false;
		std::uint32_t units = 0;
		std::uint32_t scale = 0;
	};

	// The VUI's start, as H.264 and HEVC share it.
	template <typename Fields>
	void vui_start(header_bits& w, const Fields& f)
	{
		w.flag(f.aspect_idc >= 0);

		if (f.aspect_idc >= 0)
		{
			w.u(static_cast<unsigned>(f.aspect_idc), 8);
			if (f.aspect_idc == 255) w.u(f.sar_width, 16).u(f.sar_height, 16);
		}

		// Overscan information, then the video signal type: PAL, its range and colour.
		w.flag(true).flag(false).flag(f.signal);
		if (f.signal) w.u(1, 3).flag(f.full_range).flag(f.colour);
		if (f.signal && f.colour) w.u(f.primaries, 8).u(f.transfer, 8).u(f.matrix, 8);

		// The chroma location in each field.
		w.flag(true).ue(1).ue(1);
	}

	bytes h264_nal(const h264_sps& s)
	{
		header_bits w;
		w.u(s.profile_idc, 8).u(s.constraints, 8).u(40, 8).ue(0);

		if (std::ranges::find(h264_chroma_profiles, s.profile_idc) != std::end(h264_chroma_profiles))
		{
			w.ue(s.chroma_format_idc);
			if (s.chroma_format_idc == 3) w.flag(s.separate_planes);
			w.ue(s.luma_depth - 8).ue(s.chroma_depth - 8).flag(false).flag(s.scaling_lists);

			// Lists in full, ended early by a zero scale, taken as the default at once, and left out.
			for (unsigned i = 0; s.scaling_lists && i < (s.chroma_format_idc != 3 ? 8u : 12u); ++i)
			{
				const auto size = i < 6 ? 16 : 64;

				switch (i % 4)
				{
				case 0: w.flag(true);
					for (auto j = 0; j < size; ++j) w.se(j % 2 == 0 ? -2 : 3);
					break;
				case 1: w.flag(true).se(-8);
					break;
				case 2: w.flag(true).se(5).se(10).se(-23);
					break;
				default: w.flag(false);
					break;
				}
			}
		}

		w.ue(4).ue(s.poc_type);
		if (s.poc_type == 0) w.ue(4);
		if (s.poc_type == 1) w.flag(false).se(-1).se(2).ue(3).se(1).se(-5).se(100000);

		w.ue(4).flag(false).ue(s.width_in_mbs - 1).ue(s.height_in_map_units - 1).flag(s.frame_mbs_only);
		if (!s.frame_mbs_only) w.flag(true);
		w.flag(true);

		const auto cropped = s.crop != std::array<unsigned, 4>{};
		w.flag(cropped);
		if (cropped) w.ue(s.crop[0]).ue(s.crop[1]).ue(s.crop[2]).ue(s.crop[3]);
		w.flag(s.vui);

		if (s.vui)
		{
			vui_start(w, s);
			w.flag(s.timing);
			if (s.timing) w.u(s.units, 32).u(s.scale, 32).flag(true);

			// NAL HRD parameters for two CPBs, then no VCL HRD, low_delay_hrd_flag, pic_struct_present_flag
			// and the bitstream restriction.
			w.flag(true).ue(1).u(4, 4).u(6, 4);
			for (auto i = 0; i < 2; ++i) w.ue(10000u * (i + 1)).ue(30000).flag(i == 1);
			w.u(23, 5).u(23, 5).u(23, 5).u(24, 5).flag(false).flag(false).flag(false);
			w.flag(true).flag(true).ue(2).ue(1).ue(16).ue(16).ue(2).ue(4);
		}

		w.trailing();
		bytes out{0x67};
		const auto payload = escape(w.b);
		out.insert(out.end(), payload.begin(), payload.end());
		return out;
	}

	bytes avc_config(const std::vector<bytes>& sets)
	{
		builder o;
		o.u8(1).u8(sets.empty() ? 100 : sets[0][1]).u8(0).u8(40).u8(0xFF).u8(0xE0 | static_cast<unsigned>(sets.size()));
		for (const auto& s : sets) o.be16(static_cast<unsigned>(s.size())).raw(s);
		// One PPS, then the high profiles' extension.
		o.u8(1).be16(4).u8(0x68).u8(0xEB).u8(0xE3).u8(0xCB);
		o.u8(0xFD).u8(0xF8).u8(0xF8).u8(0);
		return o.b;
	}

	// ---- HEVC ----------------------------------------------------------------------------------------

	struct hevc_sps
	{
		unsigned nal_type = 33;
		unsigned layer = 0;
		unsigned profile_idc = 1;
		std::uint32_t compatibility = 0x60000000;
		bool progressive_source = true;
		bool interlaced_source = false;
		unsigned sub_layers = 0;
		unsigned chroma_format_idc = 1;
		bool separate_planes = false;
		unsigned width = 1920;
		unsigned height = 1080;
		// Left, right, top and bottom.
		std::array<unsigned, 4> window{};
		unsigned luma_depth = 8;
		unsigned chroma_depth = 8;
		bool every_sub_layer = false;
		bool scaling_lists = false;
		bool pcm = false;
		bool ref_pic_sets = false;
		// When set, the number of short-term sets, and nothing after it.
		int sets_instead = -1;
		bool long_term = false;
		bool vui = false;
		int aspect_idc = -1;
		unsigned sar_width = 0;
		unsigned sar_height = 0;
		bool signal = false;
		bool full_range = false;
		bool colour = false;
		unsigned primaries = 2;
		unsigned transfer = 2;
		unsigned matrix = 2;
		bool field_seq = false;
		bool display_window = false;
		bool timing = false;
		std::uint32_t units = 0;
		std::uint32_t scale = 0;
	};

	bytes hevc_nal(const hevc_sps& s)
	{
		header_bits w;
		w.u(0, 4).u(s.sub_layers, 3).flag(true);

		// profile_tier_level, with each sub-layer's level and every other one's profile.
		w.u(0, 2).u(0, 1).u(s.profile_idc, 5).u(s.compatibility, 32).flag(s.progressive_source).flag(s.interlaced_source);
		w.flag(false).flag(true).u(0, 43).u(0, 1).u(93, 8);
		for (unsigned i = 0; i < s.sub_layers; ++i) w.flag(i % 2 == 0).flag(true);
		for (auto i = s.sub_layers; s.sub_layers > 0 && i < 8; ++i) w.u(0, 2);

		for (unsigned i = 0; i < s.sub_layers; ++i)
		{
			if (i % 2 == 0) w.u(0, 2).u(0, 1).u(s.profile_idc, 5).u(s.compatibility, 32).u(0x800000000000ull, 48);
			w.u(90, 8);
		}

		w.ue(0).ue(s.chroma_format_idc);
		if (s.chroma_format_idc == 3) w.flag(s.separate_planes);
		w.ue(s.width).ue(s.height);

		const auto windowed = s.window != std::array<unsigned, 4>{};
		w.flag(windowed);
		if (windowed) w.ue(s.window[0]).ue(s.window[1]).ue(s.window[2]).ue(s.window[3]);

		// The depths, an 8-bit picture order count, and the buffering of each sub-layer or the last.
		w.ue(s.luma_depth - 8).ue(s.chroma_depth - 8).ue(4).flag(s.every_sub_layer);
		for (auto i = s.every_sub_layer ? 0u : s.sub_layers; i <= s.sub_layers; ++i) w.ue(4).ue(2).ue(0);

		// Block sizes from 8 to 64 and 4 to 32, two transform levels each way.
		w.ue(0).ue(3).ue(0).ue(3).ue(2).ue(2).flag(s.scaling_lists);

		if (s.scaling_lists)
		{
			// Lists copied from the one before or the default, and lists coded in full.
			w.flag(true);

			for (unsigned size = 0; size < 4; ++size)
			{
				for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1)
				{
					if ((size + matrix) % 2 == 0)
					{
						w.flag(false).ue(matrix == 0 ? 0 : 1);
						continue;
					}

					w.flag(true);
					if (size > 1) w.se(4);
					for (unsigned i = 0; i < std::min(64u, 16u << (2 * size)); ++i) w.se(static_cast<int>(i % 3) - 1);
				}
			}
		}

		w.flag(true).flag(true).flag(s.pcm);
		if (s.pcm) w.u(7, 4).u(7, 4).ue(0).ue(1).flag(false);

		if (s.sets_instead >= 0)
		{
			w.ue(static_cast<unsigned>(s.sets_instead));
			for (auto i = 0; i < 40; ++i) w.flag(true);
			w.trailing();
		}
		else
		{
			if (!s.ref_pic_sets)
			{
				w.ue(0);
			}
			else
			{
				// Set 0: pictures 1 and 3 before this one, and 1 after.
				w.ue(5).ue(2).ue(1).ue(0).flag(true).ue(1).flag(true).ue(0).flag(true);
				// Set 1, from set 0 moved one picture on: -1 lands on this picture, and is dropped though
				// its flag is set; -3 and the move itself are kept, as -2 and +1.
				w.flag(true).flag(false).ue(0).flag(true).flag(false).flag(true).flag(false).flag(false).flag(true);
				// Set 2, from set 1's two pictures and its move: none kept. Set 3, from set 2's none and its
				// move of two: that one kept. Set 4, coded empty.
				w.flag(true).flag(true).ue(3);
				for (auto j = 0; j < 3; ++j) w.flag(false).flag(false);
				w.flag(true).flag(false).ue(1).flag(true);
				w.flag(false).ue(0).ue(0);
			}

			w.flag(s.long_term);
			if (s.long_term) w.ue(2).u(5, 8).flag(true).u(200, 8).flag(false);
			w.flag(true).flag(true).flag(s.vui);

			if (s.vui)
			{
				vui_start(w, s);
				w.flag(false).flag(s.field_seq).flag(s.field_seq).flag(s.display_window);
				if (s.display_window) w.ue(8).ue(8).ue(4).ue(4);
				w.flag(s.timing);

				if (s.timing)
				{
					// POC proportional to timing, and HRD parameters: NAL HRD, a CPB for each sub-layer.
					w.u(s.units, 32).u(s.scale, 32).flag(true).ue(0).flag(true);
					w.flag(true).flag(false).flag(false).u(4, 4).u(6, 4).u(23, 5).u(23, 5).u(23, 5);
					for (unsigned i = 0; i <= s.sub_layers; ++i) w.flag(true).ue(0).ue(0).ue(20000).ue(40000).flag(false);
				}

				w.flag(true).flag(false).flag(true).flag(true).ue(0).ue(2).ue(1).ue(15).ue(15);
			}

			w.flag(false).trailing();
		}

		bytes out{static_cast<std::uint8_t>(s.nal_type << 1 | s.layer >> 5), static_cast<std::uint8_t>((s.layer & 31) << 3 | 1)};
		const auto payload = escape(w.b);
		out.insert(out.end(), payload.begin(), payload.end());
		return out;
	}

	struct hevc_record
	{
		unsigned profile_idc = 1;
		std::uint32_t compatibility = 0x60000000;
		bool progressive_source = true;
		bool interlaced_source = false;
		unsigned chroma_format_idc = 1;
		unsigned luma_depth = 8;
	};

	bytes hevc_config(const hevc_record& r, const std::vector<std::pair<unsigned, std::vector<bytes>>>& arrays)
	{
		builder o;
		o.u8(1).u8(r.profile_idc).be32(r.compatibility);
		o.u8((r.progressive_source ? 0x80u : 0) | (r.interlaced_source ? 0x40u : 0)).u8(0).be32(0).u8(93);
		o.be16(0xF000).u8(0xFC).u8(0xFC | r.chroma_format_idc).u8(0xF8 | (r.luma_depth - 8)).u8(0xF8 | (r.luma_depth - 8));
		o.be16(0).u8(0x0F).u8(static_cast<unsigned>(arrays.size()));

		for (const auto& [type, units] : arrays)
		{
			o.u8(0x80 | type).be16(static_cast<unsigned>(units.size()));
			for (const auto& u : units) o.be16(static_cast<unsigned>(u.size())).raw(u);
		}

		return o.b;
	}

	// ---- AV1 -----------------------------------------------------------------------------------------

	struct av1_sequence
	{
		unsigned profile = 0;
		bool reduced = false;
		bool timing = false;
		std::uint32_t units = 0;
		std::uint32_t scale = 0;
		bool equal_interval = false;
		std::uint32_t ticks_minus_1 = 0;
		bool decoder_model = false;
		bool display_delay = false;
		unsigned operating_points = 1;
		unsigned width = 1920;
		unsigned height = 1080;
		bool frame_ids = false;
		bool order_hint = true;
		// 2 when chosen frame by frame, else forced off or on.
		int screen_content = 2;
		int integer_mv = 2;
		unsigned depth = 8;
		bool mono = false;
		bool colour = false;
		unsigned primaries = 2;
		unsigned transfer = 2;
		unsigned matrix = 2;
		bool full_range = false;
		// Profile 2 at 12 bits states its subsampling.
		bool subsampling_x = true;
		bool subsampling_y = true;
	};

	unsigned bits_for(const unsigned value)
	{
		unsigned n = 1;
		while (n < 32 && (value >> n) != 0) ++n;
		return n;
	}

	bytes av1_payload(const av1_sequence& s)
	{
		header_bits w;
		w.u(s.profile, 3).flag(false).flag(s.reduced);

		if (s.reduced)
		{
			w.u(8, 5);
		}
		else
		{
			w.flag(s.timing);

			if (s.timing)
			{
				// uvlc() is ue(v)'s code.
				w.u(s.units, 32).u(s.scale, 32).flag(s.equal_interval);
				if (s.equal_interval) w.ue(s.ticks_minus_1);
				w.flag(s.decoder_model);
				if (s.decoder_model) w.u(9, 5).u(1001, 32).u(15, 5).u(20, 5);
			}

			w.flag(s.display_delay).u(s.operating_points - 1, 5);

			for (unsigned i = 0; i < s.operating_points; ++i)
			{
				const auto level = i % 2 == 0 ? 8u : 5u;
				w.u(i == 0 ? 0x10F : 0x103, 12).u(level, 5);
				if (level > 7) w.flag(i == 2);

				if (s.decoder_model)
				{
					w.flag(i != 1);
					if (i != 1) w.u(500, 10).u(300, 10).flag(false);
				}

				if (s.display_delay)
				{
					w.flag(i != 2);
					if (i != 2) w.u(9, 4);
				}
			}
		}

		const auto width_bits = bits_for(s.width - 1);
		const auto height_bits = bits_for(s.height - 1);
		w.u(width_bits - 1, 4).u(height_bits - 1, 4).u(s.width - 1, width_bits).u(s.height - 1, height_bits);

		if (!s.reduced)
		{
			w.flag(s.frame_ids);
			if (s.frame_ids) w.u(5, 4).u(2, 3);
		}

		w.flag(false).flag(true).flag(true);

		if (!s.reduced)
		{
			w.flag(true).flag(true).flag(true).flag(true).flag(s.order_hint);
			if (s.order_hint) w.flag(true).flag(true);
			w.flag(s.screen_content == 2);
			if (s.screen_content != 2) w.flag(s.screen_content == 1);

			if (s.screen_content != 0)
			{
				w.flag(s.integer_mv == 2);
				if (s.integer_mv != 2) w.flag(s.integer_mv == 1);
			}

			if (s.order_hint) w.u(6, 3);
		}

		w.flag(false).flag(true).flag(true);

		// color_config.
		w.flag(s.depth > 8);
		if (s.profile == 2 && s.depth > 8) w.flag(s.depth == 12);
		if (s.profile != 1) w.flag(s.mono);
		w.flag(s.colour);
		if (s.colour) w.u(s.primaries, 8).u(s.transfer, 8).u(s.matrix, 8);

		if (s.mono)
		{
			w.flag(s.full_range);
		}
		else
		{
			if (!(s.colour && s.primaries == 1 && s.transfer == 13 && s.matrix == 0))
			{
				w.flag(s.full_range);

				const auto stated = s.profile == 2 && s.depth == 12;
				const auto x = s.profile == 0 || (s.profile == 2 && (!stated || s.subsampling_x));
				const auto y = s.profile == 0 || (stated && x && s.subsampling_y);

				if (stated)
				{
					w.flag(x);
					if (x) w.flag(y);
				}

				if (x && y) w.u(1, 2);
			}

			w.flag(false);
		}

		w.flag(false).trailing();
		return w.b;
	}

	// An OBU, its size in size_bytes bytes of leb128 when it has a size field.
	bytes obu(const unsigned type, const bytes& payload, const bool size_field = true, const bool extension = false,
	          const unsigned size_bytes = 1)
	{
		builder o;
		o.u8(type << 3 | (extension ? 4u : 0) | (size_field ? 2u : 0));
		if (extension) o.u8(0x28);

		if (size_field)
		{
			auto v = payload.size();

			for (unsigned i = 0;; ++i)
			{
				const auto low = static_cast<unsigned>(v & 0x7F);
				v >>= 7;

				if (v == 0 && i + 1 >= size_bytes)
				{
					o.u8(low);
					break;
				}

				o.u8(0x80 | low);
			}
		}

		o.raw(payload);
		return o.b;
	}

	bytes av1_config(const unsigned profile, const unsigned flags, const bytes& obus = {})
	{
		return builder().u8(0x81).u8(profile << 5 | 8).u8(flags).u8(0).raw(obus).b;
	}

	// ---- VP9 -----------------------------------------------------------------------------------------

	struct vp9_header
	{
		unsigned marker = 2;
		unsigned profile = 0;
		bool reserved = false;
		bool existing = false;
		bool key = true;
		bool show = true;
		bool error_resilient = false;
		bool intra_only = false;
		std::uint32_t sync = 0x498342;
		unsigned depth = 8;
		unsigned space = 2;
		bool full_range = false;
		bool x = true;
		bool y = true;
		bool colour_reserved = false;
		// An inter frame's reference whose size it takes, or -1 for a size of its own.
		int found_ref = -1;
		unsigned width = 352;
		unsigned height = 288;
	};

	bytes vp9_frame(const vp9_header& f)
	{
		header_bits w;
		w.u(f.marker, 2).u(f.profile & 1, 1).u(f.profile >> 1, 1);
		if (f.profile == 3) w.flag(f.reserved);
		w.flag(f.existing);
		if (f.existing) return w.u(3, 3).zeros().b;
		w.flag(!f.key).flag(f.show).flag(f.error_resilient);

		const auto color_config = [&]
		{
			if (f.profile >= 2) w.flag(f.depth == 12);
			w.u(f.space, 3);

			if (f.space != 7)
			{
				w.flag(f.full_range);
				if (f.profile == 1 || f.profile == 3) w.flag(f.x).flag(f.y).flag(f.colour_reserved);
			}
			else if (f.profile == 1 || f.profile == 3)
			{
				w.flag(f.colour_reserved);
			}
		};

		auto sized = true;

		if (f.key)
		{
			w.u(f.sync, 24);
			color_config();
		}
		else
		{
			if (!f.show) w.flag(f.intra_only);
			if (!f.error_resilient) w.u(0, 2);

			if (f.intra_only)
			{
				w.u(f.sync, 24);
				if (f.profile > 0) color_config();
				w.u(0xFF, 8);
			}
			else
			{
				w.u(1, 8);
				for (unsigned i = 0; i < 3; ++i) w.u(i, 3).flag(false);

				for (auto i = 0; i < 3; ++i)
				{
					w.flag(i == f.found_ref);
					if (i == f.found_ref) break;
				}

				sized = f.found_ref < 0;
			}
		}

		if (sized) w.u(f.width - 1, 16).u(f.height - 1, 16);
		// Render size, then what the header holds after it.
		w.flag(false).u(0x5A5A, 16).zeros();
		return w.b;
	}

	// ---- MPEG-4 Part 2 -------------------------------------------------------------------------------

	struct mpeg4_stream
	{
		bool sequence = true;
		unsigned profile_level = 0xF5;
		bool object = true;
		unsigned object_verid = 0;
		bool signal = false;
		bool full_range = false;
		bool colour = false;
		unsigned primaries = 2;
		unsigned transfer = 2;
		unsigned matrix = 2;
		unsigned type = 17;
		unsigned layer_verid = 0;
		unsigned aspect = 1;
		unsigned par_width = 0;
		unsigned par_height = 0;
		bool control = false;
		bool vbv = false;
		unsigned shape = 0;
		unsigned resolution = 30000;
		bool fixed_rate = false;
		unsigned increment = 1001;
		unsigned width = 720;
		unsigned height = 480;
		bool interlaced = false;
		unsigned sprite = 0;
		bool not_8_bit = false;
		unsigned depth = 8;
	};

	bytes mpeg4_headers(const mpeg4_stream& s)
	{
		builder o;
		if (s.sequence) o.be32(0x1B0).u8(s.profile_level);

		if (s.object)
		{
			header_bits w;
			w.flag(s.object_verid != 0);
			if (s.object_verid != 0) w.u(s.object_verid, 4).u(1, 3);
			w.u(1, 4).flag(s.signal);
			if (s.signal) w.u(5, 3).flag(s.full_range).flag(s.colour);
			if (s.signal && s.colour) w.u(s.primaries, 8).u(s.transfer, 8).u(s.matrix, 8);
			o.be32(0x1B5).raw(w.stuffing().b);
		}

		// A video object, then user data, before the layer.
		o.be32(0x100).be32(0x1B2).raw("encoder");

		const auto verid = s.layer_verid != 0 ? s.layer_verid : s.object_verid != 0 ? s.object_verid : 1;
		header_bits w;
		w.flag(false).u(s.type, 8).flag(s.layer_verid != 0);
		if (s.layer_verid != 0) w.u(s.layer_verid, 4).u(1, 3);
		w.u(s.aspect, 4);
		if (s.aspect == 15) w.u(s.par_width, 8).u(s.par_height, 8);
		w.flag(s.control);

		if (s.control)
		{
			w.u(1, 2).flag(true).flag(s.vbv);
			if (s.vbv) w.u(0x1234, 15).flag(true).u(0x22, 15).flag(true).u(0x111, 15).flag(true).u(3, 3).u(0x400, 11).flag(true).u(0x7FFF, 15).flag(true);
		}

		w.u(s.shape, 2);
		if (s.shape == 3 && verid != 1) w.u(0, 4);
		w.flag(true).u(s.resolution, 16).flag(true).flag(s.fixed_rate);
		if (s.fixed_rate) w.u(s.increment, bits_for(s.resolution - 1));

		if (s.shape != 2)
		{
			if (s.shape == 0) w.flag(true).u(s.width, 13).flag(true).u(s.height, 13).flag(true);
			w.flag(s.interlaced).flag(true).u(s.sprite, verid == 1 ? 1 : 2);

			if (s.sprite == 1 || s.sprite == 2)
			{
				if (s.sprite == 1) w.u(800, 13).flag(true).u(600, 13).flag(true).u(16, 13).flag(true).u(16, 13).flag(true);
				w.u(2, 6).u(1, 2).flag(false);
				if (s.sprite == 1) w.flag(false);
			}

			if (verid != 1 && s.shape != 0) w.flag(true);
			w.flag(s.not_8_bit);
			if (s.not_8_bit) w.u(4, 4).u(s.depth, 4);
			// quant_type, then complexity estimation, resync markers and data partitioning, and no scalability.
			w.flag(false);
			if (verid != 1) w.flag(false);
			w.flag(true).flag(true).flag(false);
			if (verid != 1) w.flag(false).flag(false);
			w.flag(false);
		}

		o.be32(0x120).raw(w.stuffing().b);
		// The first VOP's start.
		o.be32(0x1B6).u8(0x10).u8(0x00).u8(0x31);
		return o.b;
	}

	// ---- MPEG-1 and MPEG-2 ---------------------------------------------------------------------------

	struct mpeg12_stream
	{
		unsigned width = 720;
		unsigned height = 480;
		unsigned aspect = 3;
		unsigned rate = 4;
		bool marker = true;
		bool intra_matrix = false;
		bool non_intra_matrix = false;
		bool extension = true;
		unsigned indication = 0x48;
		bool progressive = false;
		unsigned chroma = 1;
		unsigned rate_n = 0;
		unsigned rate_d = 0;
		bool user_data = false;
		bool display = false;
		bool colour = false;
		unsigned primaries = 2;
		unsigned transfer = 2;
		unsigned matrix = 2;
	};

	bytes mpeg12_headers(const mpeg12_stream& s)
	{
		header_bits w;
		w.u(s.width & 0xFFF, 12).u(s.height & 0xFFF, 12).u(s.aspect, 4).u(s.rate, 4).u(0x3FFFF, 18).flag(s.marker).u(112, 10).flag(false);
		w.flag(s.intra_matrix);
		for (auto i = 0; s.intra_matrix && i < 64; ++i) w.u(8u + i, 8);
		w.flag(s.non_intra_matrix);
		for (auto i = 0; s.non_intra_matrix && i < 64; ++i) w.u(16, 8);

		builder o;
		o.be32(0x1B3).raw(w.b);

		if (s.extension)
		{
			header_bits e;
			e.u(1, 4).u(s.indication, 8).flag(s.progressive).u(s.chroma, 2).u(s.width >> 12, 2).u(s.height >> 12, 2);
			e.u(0, 12).flag(true).u(0, 8).flag(false).u(s.rate_n, 2).u(s.rate_d, 5);
			o.be32(0x1B5).raw(e.b);
		}

		if (s.user_data) o.be32(0x1B2).raw("user data");

		if (s.display)
		{
			header_bits d;
			d.u(2, 4).u(1, 3).flag(s.colour);
			if (s.colour) d.u(s.primaries, 8).u(s.transfer, 8).u(s.matrix, 8);
			d.u(s.width, 14).flag(true).u(s.height, 14).zeros();
			o.be32(0x1B5).raw(d.b);
		}

		// A GOP header follows.
		o.be32(0x1B8).u8(0x00).u8(0x08).u8(0x00).u8(0x40);
		return o.b;
	}

	// A header cut short where its sequence header ends is MPEG-1's: there is no extension to say
	// otherwise.
	agreement mpeg2_cut_short(const bytes& headers, const std::size_t header_end)
	{
		const auto mpeg1 = read(libfiles::detail::parse_mpeg12_video, first(headers, header_end));
		CHECK(mpeg1.profile.empty());

		return [mpeg1](const video_header& part, const video_header& whole)
		{
			return agrees(part, whole) || differences(part, mpeg1).empty();
		};
	}

	// ---- Motion JPEG ---------------------------------------------------------------------------------

	// Each component: its ID and its sampling factors.
	using jpeg_components = std::vector<std::array<std::uint8_t, 2>>;

	const jpeg_components ycbcr420 = {{1, 0x22}, {2, 0x11}, {3, 0x11}};

	bytes jpeg_frame(const unsigned sof, const unsigned precision, const unsigned width, const unsigned height, const jpeg_components& components,
	                 const bytes& markers = {})
	{
		builder o;
		o.u8(0xFF).u8(0xD8).raw(markers);
		o.u8(0xFF).u8(0xDB).be16(67).u8(0);
		for (auto i = 0; i < 64; ++i) o.u8(1 + static_cast<unsigned>(i % 7));

		const auto n = static_cast<unsigned>(components.size());
		o.u8(0xFF).u8(sof).be16(8 + 3 * n).u8(precision).be16(height).be16(width).u8(n);
		for (const auto& c : components) o.u8(c[0]).u8(c[1]).u8(0);

		// A Huffman table, the scan with a stuffed byte, and the end.
		o.u8(0xFF).u8(0xC4).be16(20).u8(0).u8(1).zeros(15).u8(0);
		o.u8(0xFF).u8(0xDA).be16(6 + 2 * n).u8(n);
		for (const auto& c : components) o.u8(c[0]).u8(0);
		o.u8(0).u8(63).u8(0).u8(0x12).u8(0xFF).u8(0x00).u8(0x34).u8(0xFF).u8(0xD9);
		return o.b;
	}

	bytes avi1(const unsigned polarity)
	{
		return builder().u8(0xFF).u8(0xE0).be16(16).raw("AVI1").u8(polarity).u8(0).be32(4000).be32(3990).b;
	}

	bytes adobe(const unsigned transform)
	{
		return builder().u8(0xFF).u8(0xEE).be16(14).raw("Adobe").be16(100).be16(0).be16(0).u8(transform).b;
	}

	bytes jpeg_comment(const std::string_view text)
	{
		return builder().u8(0xFF).u8(0xFE).be16(static_cast<unsigned>(text.size() + 2)).raw(text).b;
	}

	// ---- Theora --------------------------------------------------------------------------------------

	struct theora_info
	{
		unsigned major = 3;
		unsigned minor = 2;
		unsigned frame_width = 1920;
		unsigned frame_height = 1088;
		unsigned width = 1920;
		unsigned height = 1080;
		unsigned x = 0;
		unsigned y = 8;
		std::uint32_t rate_numerator = 30000;
		std::uint32_t rate_denominator = 1001;
		unsigned aspect_numerator = 1;
		unsigned aspect_denominator = 1;
		unsigned colour_space = 2;
		unsigned pixel_format = 0;
	};

	bytes theora_header(const theora_info& t)
	{
		header_bits w;
		w.u(t.frame_width / 16, 16).u(t.frame_height / 16, 16).u(t.width, 24).u(t.height, 24).u(t.x, 8).u(t.y, 8);
		w.u(t.rate_numerator, 32).u(t.rate_denominator, 32).u(t.aspect_numerator, 24).u(t.aspect_denominator, 24);
		w.u(t.colour_space, 8).u(500000, 24).u(48, 6).u(6, 5).u(t.pixel_format, 2).u(0, 3);
		return builder().u8(0x80).raw("theora").u8(t.major).u8(t.minor).u8(1).raw(w.b).b;
	}

	// ---- DV ------------------------------------------------------------------------------------------

	struct dv_info
	{
		bool fifty = false;
		unsigned apt = 0;
		// The VAUX source pack's signal type, or -1 for none.
		int signal_type = -1;
		// The source control pack's DISP, or -1 for none.
		int display = -1;
		bool interlaced = true;
		std::size_t blocks = 6;
	};

	bytes dv_frame(const dv_info& d)
	{
		bytes f(d.blocks * 80, 0xFF);

		const auto id = [&f](const std::size_t block, const std::uint8_t first, const std::uint8_t number)
		{
			if (block * 80 + 3 > f.size()) return;
			f[block * 80] = first;
			f[block * 80 + 1] = 0x07;
			f[block * 80 + 2] = number;
		};

		// The header block, two subcode blocks and three VAUX blocks.
		id(0, 0x1F, 0);
		f[3] = d.fifty ? 0xBF : 0x3F;
		f[4] = static_cast<std::uint8_t>(0xF8 | d.apt);
		id(1, 0x3F, 0);
		id(2, 0x3F, 1);
		for (std::uint8_t k = 0; k < 3; ++k) id(3u + k, 0x5F, k);

		if (d.signal_type >= 0 && f.size() > 457)
		{
			f[448] = 0x60;
			f[451] = static_cast<std::uint8_t>((d.fifty ? 0xE0 : 0xC0) | d.signal_type);
		}

		if (d.display >= 0 && f.size() > 457)
		{
			f[453] = 0x61;
			f[454] = 0x3F;
			f[455] = static_cast<std::uint8_t>(0xC8 | d.display);
			f[456] = d.interlaced ? 0xD0 : 0xC0;
		}

		return f;
	}

	// ---- VC-1 ----------------------------------------------------------------------------------------

	// STRUCT_C, the sequence layer of the Simple, Main and Complex profiles.
	struct vc1_struct_c
	{
		unsigned profile = 1;
		bool y411 = false;
		bool sprite = false;
		bool fast_transform = true;
		bool fast_chroma = true;
		bool extended_mv = false;
		bool transtab = false;
		unsigned sprite_width = 320;
		unsigned sprite_height = 240;
		// The bit after a sprite's RES_X8, which FFmpeg cannot decode when set.
		bool sprite_feature = false;
	};

	bytes struct_c(const vc1_struct_c& c)
	{
		header_bits w;
		w.u(c.profile, 2).flag(c.y411).flag(c.sprite).u(4, 3).u(23, 5);
		// LOOPFILTER, RES_X8, MULTIRES and RES_FASTTX; FASTUVMC, EXTENDED_MV, DQUANT, VSTRANSFORM and
		// RES_TRANSTAB; OVERLAP, RESYNCMARKER, RANGERED, MAXBFRAMES, QUANTIZER and FINTERPFLAG.
		w.flag(c.profile != 0).flag(false).flag(false).flag(c.fast_transform);
		w.flag(c.fast_chroma).flag(c.extended_mv).u(1, 2).flag(true).flag(c.transtab);
		w.flag(true).flag(false).flag(false).u(1, 3).u(2, 2).flag(false);

		// A sprite's size, frame rate, RES_X8, the bit after it and the slice code; else RES_RTM_FLAG.
		if (c.sprite) w.u(c.sprite_width, 11).u(c.sprite_height, 11).u(15, 5).flag(false).flag(c.sprite_feature).u(0, 3);
		else w.flag(true);

		return w.zeros().b;
	}

	// The Advanced profile's sequence header, from PROFILE on.
	struct vc1_header
	{
		unsigned level = 3;
		unsigned chroma = 1;
		unsigned coded_width = 1920;
		unsigned coded_height = 1080;
		bool pulldown = false;
		bool interlace = false;
		bool psf = false;
		bool display = false;
		unsigned display_width = 1920;
		unsigned display_height = 1080;
		// ASPECT_RATIO, -1 for none; at 15, the shape it states.
		int ratio = -1;
		unsigned ratio_width = 1;
		unsigned ratio_height = 1;
		// FRAMERATENR and FRAMERATEDR, or FRAMERATEEXP; -1 for none.
		int rate_nr = -1;
		unsigned rate_dr = 1;
		int rate_exp = -1;
		bool colour = false;
		unsigned primaries = 1;
		unsigned transfer = 1;
		unsigned matrix = 1;
		// HRD_NUM_LEAKY_BUCKETS, -1 for no HRD parameters.
		int buckets = -1;
	};

	bytes vc1_header_bits(const vc1_header& a)
	{
		header_bits w;
		w.u(3, 2).u(a.level, 3).u(a.chroma, 2).u(7, 3).u(31, 5).flag(true);
		w.u(a.coded_width / 2 - 1, 12).u(a.coded_height / 2 - 1, 12);
		// PULLDOWN, INTERLACE, TFCNTRFLAG, FINTERPFLAG, a reserved bit, PSF and DISPLAY_EXT.
		w.flag(a.pulldown).flag(a.interlace).flag(false).flag(false).flag(true).flag(a.psf).flag(a.display);

		if (a.display)
		{
			w.u(a.display_width - 1, 14).u(a.display_height - 1, 14).flag(a.ratio >= 0);
			if (a.ratio >= 0) w.u(static_cast<unsigned>(a.ratio), 4);
			if (a.ratio == 15) w.u(a.ratio_width - 1, 8).u(a.ratio_height - 1, 8);

			w.flag(a.rate_nr >= 0 || a.rate_exp >= 0);
			if (a.rate_exp >= 0) w.flag(true).u(static_cast<unsigned>(a.rate_exp), 16);
			else if (a.rate_nr >= 0) w.flag(false).u(static_cast<unsigned>(a.rate_nr), 8).u(a.rate_dr, 4);

			w.flag(a.colour);
			if (a.colour) w.u(a.primaries, 8).u(a.transfer, 8).u(a.matrix, 8);
		}

		w.flag(a.buckets >= 0);

		if (a.buckets >= 0)
		{
			// BIT_RATE_EXPONENT and BUFFER_SIZE_EXPONENT, then each bucket's HRD_RATE and HRD_BUFFER.
			w.u(static_cast<unsigned>(a.buckets), 5).u(9, 4).u(11, 4);
			for (auto i = 0; i < a.buckets; ++i) w.u(0x1234, 16).u(0x0567, 16);
		}

		return w.trailing().b;
	}

	// An entry point. Its quantiser fields are clear, so that HRD_FULL values can make runs of zero bytes.
	struct vc1_entry
	{
		std::vector<unsigned> fullness;
		unsigned coded_width = 0;
		unsigned coded_height = 0;
		bool extended_mv = false;
	};

	bytes vc1_entry_bits(const vc1_entry& e)
	{
		header_bits w;
		// BROKEN_LINK, CLOSED_ENTRY, PANSCAN_FLAG, REFDIST_FLAG, LOOPFILTER, FASTUVMC, EXTENDED_MV, DQUANT,
		// VSTRANSFORM, OVERLAP and QUANTIZER; then HRD_FULL for each leaky bucket, and CODED_SIZE_FLAG.
		w.flag(false).flag(true).flag(false).flag(true).flag(true).flag(true).flag(e.extended_mv).u(0, 2).flag(false).flag(false).u(0, 2);
		for (const auto f : e.fullness) w.u(f, 8);
		w.flag(e.coded_width != 0);
		if (e.coded_width != 0) w.u(e.coded_width / 2 - 1, 12).u(e.coded_height / 2 - 1, 12);

		// EXTENDED_DMV, RANGE_MAPY_FLAG and RANGE_MAPUV_FLAG.
		if (e.extended_mv) w.flag(true);
		w.flag(false).flag(false);
		return w.trailing().b;
	}

	// A start code, then a unit with emulation prevention put in.
	builder& vc1_unit(builder& b, const std::uint8_t code, const bytes& unit)
	{
		return b.u8(0).u8(0).u8(1).u8(code).raw(escape(unit));
	}

	// The Advanced profile's record: a sequence header and an entry point.
	bytes vc1_units(const vc1_header& a, const vc1_entry& e = {})
	{
		builder b;
		vc1_unit(b, 0x0F, vc1_header_bits(a));
		vc1_unit(b, 0x0E, vc1_entry_bits(e));
		return b.b;
	}

	// As ASF keeps them, after a byte that FFmpeg takes for their size.
	bytes vc1_record(const vc1_header& a, const vc1_entry& e = {})
	{
		const auto units = vc1_units(a, e);
		return builder().u8(static_cast<unsigned>(units.size())).raw(units).b;
	}

	// A BITMAPINFOHEADER whose size counts the codec's data after it, as AVI, ASF and Matroska write one.
	bytes bitmap_info(const std::uint32_t width, const std::uint32_t height, const std::string_view fourcc, const bytes& data)
	{
		builder b;
		b.le32(static_cast<std::uint32_t>(40 + data.size())).le32(width).le32(height).le16(1).le16(24).raw(fourcc);
		b.le32(width * height * 3).le32(0).le32(0).le32(0).le32(0);
		return b.raw(data).b;
	}

	libfiles::media_stream bitmap_stream(const bytes& data)
	{
		libfiles::media_stream s;
		CHECK(libfiles::detail::read_bitmap_info(data, s));
		return s;
	}

	// How a stream's video differs from a header's fields, as read_bitmap_info applies them.
	std::string stream_differences(const libfiles::media_stream& s, const video_header& wanted)
	{
		video_header h;
		h.width = s.video.width;
		h.height = s.video.height;
		h.pixel_aspect_width = s.video.pixel_aspect_width;
		h.pixel_aspect_height = s.video.pixel_aspect_height;
		h.chroma = s.video.chroma;
		h.bit_depth = s.video.bit_depth;
		h.alpha = s.video.alpha;
		h.interlaced = s.video.interlaced;
		h.full_range = s.video.full_range;
		h.color_primaries = s.video.color_primaries;
		h.transfer = s.video.transfer;
		h.matrix = s.video.matrix;
		h.frame_rate = s.video.frame_rate;
		h.profile = s.video.profile;
		return differences(h, wanted);
	}

	std::vector<bytes> visited(const bytes& data, const std::size_t stop_after = 1000)
	{
		std::vector<bytes> units;

		libfiles::detail::for_each_annexb_nal(data, [&](const std::span<const std::uint8_t> nal)
		{
			units.emplace_back(nal.begin(), nal.end());
			return units.size() < stop_after;
		});

		return units;
	}
}

// ---- H.264 ---------------------------------------------------------------------------------------------

TEST("media video H.264 SPS reads a Constrained Baseline picture without a VUI")
{
	h264_sps s;
	s.profile_idc = 66;
	s.constraints = 0xC0;
	s.poc_type = 2;

	const auto nal = h264_nal(s);
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, nal, picture(640, 480, chroma_format::yuv420, 8, "Constrained Baseline")), "");
	check_cut_short(libfiles::detail::parse_h264_sps, nal);
	check_damaged(libfiles::detail::parse_h264_sps, nal);
}

TEST("media video H.264 SPS crops a Main 1080p picture and reads its VUI")
{
	h264_sps s;
	s.profile_idc = 77;
	s.constraints = 0x40;
	s.poc_type = 1;
	s.width_in_mbs = 120;
	s.height_in_map_units = 68;
	s.crop = {0, 0, 0, 4};
	s.vui = true;
	s.aspect_idc = 1;
	s.signal = true;
	s.full_range = true;
	s.colour = true;
	s.primaries = 1;
	s.transfer = 1;
	s.matrix = 1;
	s.timing = true;
	s.units = 1001;
	s.scale = 60000;

	auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Main");
	aspect(want, 1, 1);
	colour(want, 1, 1, 1);
	want.full_range = true;
	want.frame_rate = 60000.0 / 2002;

	const auto nal = h264_nal(s);
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, nal, want), "");
	check_cut_short(libfiles::detail::parse_h264_sps, nal);
	check_damaged(libfiles::detail::parse_h264_sps, nal);
}

TEST("media video H.264 SPS reads High 10 past its scaling lists and removes emulation prevention")
{
	h264_sps s;
	s.profile_idc = 110;
	s.luma_depth = 10;
	s.chroma_depth = 10;
	s.scaling_lists = true;
	s.vui = true;
	s.aspect_idc = 255;
	s.sar_width = 32;
	s.sar_height = 24;
	s.signal = true;
	s.colour = true;
	s.primaries = 9;
	s.transfer = 16;
	s.matrix = 9;
	s.timing = true;
	s.units = 1;
	s.scale = 50;

	auto want = picture(640, 480, chroma_format::yuv420, 10, "High 10");
	aspect(want, 4, 3);
	colour(want, 9, 16, 9);
	want.frame_rate = 25;

	// A tick of 1 is 31 zero bits, so the NAL unit must carry an emulation prevention byte.
	const auto nal = h264_nal(s);
	CHECK(contains(nal, {0, 0, 3}));
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, nal, want), "");
	check_cut_short(libfiles::detail::parse_h264_sps, nal);
	check_damaged(libfiles::detail::parse_h264_sps, nal);
}

TEST("media video H.264 SPS counts crop units in chroma samples and field lines")
{
	using libfiles::detail::parse_h264_sps;

	// 4:2:2 coded as fields: two samples across, two lines down.
	h264_sps fields;
	fields.profile_idc = 122;
	fields.constraints = 0x10;
	fields.chroma_format_idc = 2;
	fields.luma_depth = 10;
	fields.chroma_depth = 10;
	fields.width_in_mbs = 120;
	fields.height_in_map_units = 34;
	fields.frame_mbs_only = false;
	fields.crop = {1, 1, 0, 4};

	auto want = picture(1916, 1080, chroma_format::yuv422, 10, "High 4:2:2 Intra");
	want.interlaced = true;
	CHECK_STR(read_as(parse_h264_sps, h264_nal(fields), want), "");

	// 4:2:0 coded as fields: two across, four down.
	h264_sps field420;
	field420.height_in_map_units = 17;
	field420.frame_mbs_only = false;
	field420.crop = {3, 0, 0, 2};
	want = picture(634, 536, chroma_format::yuv420, 8, "High");
	want.interlaced = true;
	CHECK_STR(read_as(parse_h264_sps, h264_nal(field420), want), "");

	// 4:4:4 with its twelve scaling lists: one each way.
	h264_sps full;
	full.profile_idc = 244;
	full.chroma_format_idc = 3;
	full.scaling_lists = true;
	full.crop = {0, 3, 1, 0};
	CHECK_STR(read_as(parse_h264_sps, h264_nal(full), picture(637, 479, chroma_format::yuv444, 8, "High 4:4:4 Predictive")), "");
	check_cut_short(parse_h264_sps, h264_nal(full));

	// Separate colour planes crop as monochrome does, and are still 4:4:4.
	h264_sps planes;
	planes.profile_idc = 244;
	planes.constraints = 0x10;
	planes.chroma_format_idc = 3;
	planes.separate_planes = true;
	planes.frame_mbs_only = false;
	planes.height_in_map_units = 15;
	planes.crop = {1, 0, 0, 1};
	want = picture(639, 478, chroma_format::yuv444, 8, "High 4:4:4 Intra");
	want.interlaced = true;
	CHECK_STR(read_as(parse_h264_sps, h264_nal(planes), want), "");

	h264_sps mono;
	mono.chroma_format_idc = 0;
	mono.luma_depth = 12;
	mono.chroma_depth = 12;
	mono.crop = {1, 0, 0, 1};
	CHECK_STR(read_as(parse_h264_sps, h264_nal(mono), picture(639, 479, chroma_format::gray, 12, "High")), "");
}

TEST("media video H.264 SPS reads 4:4:4 with the identity matrix as RGB")
{
	h264_sps s;
	s.profile_idc = 244;
	s.chroma_format_idc = 3;
	s.vui = true;
	s.signal = true;
	s.full_range = true;
	s.colour = true;
	s.primaries = 1;
	s.transfer = 13;
	s.matrix = 0;

	auto want = picture(640, 480, chroma_format::rgb, 8, "High 4:4:4 Predictive");
	colour(want, 1, 13, 0);
	want.full_range = true;
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");
	check_cut_short(libfiles::detail::parse_h264_sps, h264_nal(s));

	s.matrix = 1;
	want.chroma = chroma_format::yuv444;
	want.matrix = 1;
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");
}

TEST("media video H.264 SPS keeps only the colour code points H.273 assigns")
{
	h264_sps s;
	s.vui = true;
	s.signal = true;
	s.colour = true;
	s.primaries = 3;
	s.transfer = 0;
	s.matrix = 3;

	auto want = picture(640, 480, chroma_format::yuv420, 8, "High");
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");

	s.primaries = 22;
	s.transfer = 18;
	s.matrix = 14;
	colour(want, 22, 18, 14);
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");

	s.primaries = 13;
	s.transfer = 19;
	s.matrix = 18;
	colour(want, 2, 2, 2);
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");

	// An aspect_ratio_idc past the table, and an extended one with a zero term, state no shape.
	s.aspect_idc = 17;
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");
	s.aspect_idc = 255;
	s.sar_width = 0;
	s.sar_height = 11;
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");
	s.aspect_idc = 13;
	aspect(want, 160, 99);
	CHECK_STR(read_as(libfiles::detail::parse_h264_sps, h264_nal(s), want), "");
}

TEST("media video H.264 profiles are named as FFmpeg names them")
{
	struct case_
	{
		unsigned idc;
		unsigned constraints;
		const char* name;
	};

	constexpr case_ cases[] = {
		{66, 0x00, "Baseline"}, {66, 0x40, "Constrained Baseline"}, {77, 0x00, "Main"}, {77, 0x40, "Main"},
		{88, 0x00, "Extended"}, {100, 0x00, "High"}, {100, 0x0C, "High"}, {110, 0x00, "High 10"},
		{110, 0x10, "High 10 Intra"}, {122, 0x00, "High 4:2:2"}, {122, 0x10, "High 4:2:2 Intra"},
		{244, 0x00, "High 4:4:4 Predictive"}, {244, 0x10, "High 4:4:4 Intra"}, {44, 0x00, "CAVLC 4:4:4"},
		{118, 0x00, "Multiview High"}, {128, 0x00, "Stereo High"}, {144, 0x00, "High 4:4:4"}, {83, 0x00, ""}, {139, 0x00, ""},
	};

	for (const auto& c : cases)
	{
		h264_sps s;
		s.profile_idc = c.idc;
		s.constraints = c.constraints;
		const auto h = read(libfiles::detail::parse_h264_sps, h264_nal(s));
		CHECK_STR(h.profile, c.name);
		CHECK_EQ(h.width, 640u);
	}
}

TEST("media video H.264 SPS refuses what is not one")
{
	using libfiles::detail::parse_h264_sps;

	const auto good = h264_nal(h264_sps{});

	auto pps = good;
	pps[0] = 0x68;
	CHECK(refuses(parse_h264_sps, pps));

	auto forbidden = good;
	forbidden[0] = 0xE7;
	CHECK(refuses(parse_h264_sps, forbidden));
	CHECK(refuses(parse_h264_sps, {}));
	CHECK(refuses(parse_h264_sps, {0x67}));

	h264_sps s;
	s.chroma_format_idc = 4;
	CHECK(refuses(parse_h264_sps, h264_nal(s)));

	s = {};
	s.luma_depth = 15;
	CHECK(refuses(parse_h264_sps, h264_nal(s)));

	s = {};
	s.poc_type = 3;
	CHECK(refuses(parse_h264_sps, h264_nal(s)));

	// Cropping that leaves nothing, and a picture wider than a scan reports.
	s = {};
	s.crop = {160, 160, 0, 0};
	CHECK(refuses(parse_h264_sps, h264_nal(s)));
	s.crop = {159, 160, 0, 0};
	CHECK_STR(read_as(parse_h264_sps, h264_nal(s), picture(2, 480, chroma_format::yuv420, 8, "High")), "");

	s = {};
	s.width_in_mbs = 4097;
	CHECK(refuses(parse_h264_sps, h264_nal(s)));
	s.width_in_mbs = 4096;
	s.crop = {0, 1, 0, 0};
	CHECK_STR(read_as(parse_h264_sps, h264_nal(s), picture(65534, 480, chroma_format::yuv420, 8, "High")), "");
}

TEST("media video avcC reads its first sequence parameter set that parses")
{
	using libfiles::detail::parse_avc_config;

	h264_sps main;
	main.profile_idc = 77;
	main.width_in_mbs = 80;
	main.height_in_map_units = 45;
	main.vui = true;
	main.timing = true;
	main.units = 1;
	main.scale = 48;

	auto want = picture(1280, 720, chroma_format::yuv420, 8, "Main");
	want.frame_rate = 24;

	const auto record = avc_config({h264_nal(main)});
	CHECK_STR(read_as(parse_avc_config, record, want), "");
	check_cut_short(parse_avc_config, record);
	check_damaged(parse_avc_config, record);

	// A first set that does not parse is passed over.
	h264_sps bad;
	bad.chroma_format_idc = 4;
	const auto broken = h264_nal(bad);
	CHECK(refuses(libfiles::detail::parse_h264_sps, broken));
	CHECK_STR(read_as(parse_avc_config, avc_config({broken, h264_nal(main)}), want), "");

	CHECK(refuses(parse_avc_config, avc_config({})));
	CHECK(refuses(parse_avc_config, avc_config({broken})));

	auto version = record;
	version[0] = 0;
	CHECK(refuses(parse_avc_config, version));
	CHECK(refuses(parse_avc_config, first(record, 6)));
}

TEST("media video avcC records from real files read as FFmpeg reads them")
{
	using libfiles::detail::parse_avc_config;

	// gizmo.mp4: HandBrake's Constrained Baseline, BT.709 at video levels, 30 fps.
	auto gizmo = picture(560, 320, chroma_format::yuv420, 8, "Constrained Baseline");
	colour(gizmo, 1, 1, 1);
	gizmo.frame_rate = 30;
	CHECK_STR(read_as(parse_avc_config, hex("0142c01effe1001b6742c01e9e218118534d40404050000003001000000303c8f162ee01000568ce06cb20"), gizmo), "");

	// StPauls.MOV: an iPhone's Baseline, SMPTE 170M, no timing.
	auto phone = picture(640, 480, chroma_format::yuv420, 8, "Baseline");
	colour(phone, 6, 1, 6);
	CHECK_STR(read_as(parse_avc_config, hex("0142001effe1000e6742001e8d680a03da6a0c020c0401000468ce09c8"), phone), "");

	// tvp.mp4: High at FFmpeg's 50058/1001.
	auto tvp = picture(528, 560, chroma_format::yuv420, 8, "High");
	tvp.frame_rate = 50058.0 / 1001;
	CHECK_STR(read_as(parse_avc_config, hex("0164001fffe100176764001facd9808411e8400000fa400061c523c60c668001000568e97b3c8f"), tvp), "");

	// anamorphic.mp4 states 4:3 pixels in its SPS; anamorphic-pasp.mp4 square ones, and 4:3 in its pasp box.
	auto anamorphic = picture(640, 480, chroma_format::yuv420, 8, "High");
	aspect(anamorphic, 4, 3);
	anamorphic.frame_rate = 15;
	const auto anamorphic_record = hex("01640016ffe1001967640016acd940a03db0e10000030001000003001e0f162d9601000668ebe3cb22c0fdf8f800");
	CHECK_STR(read_as(parse_avc_config, anamorphic_record, anamorphic), "");
	aspect(anamorphic, 1, 1);
	CHECK_STR(read_as(parse_avc_config, hex("01640016ffe1001967640016acd940a03db0110000030001000003001e0f162d9601000668ebe3cb22c0fdf8f800"), anamorphic), "");
	check_cut_short(parse_avc_config, anamorphic_record);

	auto ten = picture(128, 72, chroma_format::yuv420, 10, "High 10");
	aspect(ten, 1, 1);
	ten.frame_rate = 25;
	const auto ten_record = hex("016e000affe10018676e000aa6cb4105fcb8088000000300800000190789135001000468ce0fc8fdfafa00");
	CHECK_STR(read_as(parse_avc_config, ten_record, ten), "");
	check_damaged(parse_avc_config, ten_record);

	auto single = picture(128, 72, chroma_format::yuv420, 8, "High");
	aspect(single, 1, 1);
	single.frame_rate = 25;
	CHECK_STR(read_as(parse_avc_config, hex("0164000affe100196764000aacd9420bf97011000003000100000300320f12259601000668ebe3cb22c0fdf8f800"), single), "");

	// indy.mp4: High at level 4, which FFmpeg's probe leaves unnamed until it decodes a frame.
	auto indy = picture(1920, 816, chroma_format::yuv420, 8, "High");
	aspect(indy, 1, 1);
	indy.frame_rate = 24000.0 / 1001;
	CHECK_STR(read_as(parse_avc_config, hex("01640028ffe1001867640028acc8601e019ec04400000fa40002ee023c60c66801000568e97b2c8b"), indy), "");
}

// ---- HEVC ----------------------------------------------------------------------------------------------

TEST("media video HEVC SPS reads a Main picture through every structure before its VUI")
{
	hevc_sps s;
	s.sub_layers = 2;
	s.height = 1088;
	s.window = {0, 0, 0, 4};
	s.every_sub_layer = true;
	s.scaling_lists = true;
	s.pcm = true;
	s.ref_pic_sets = true;
	s.long_term = true;
	s.vui = true;
	s.aspect_idc = 2;
	s.signal = true;
	s.full_range = true;
	s.colour = true;
	s.primaries = 9;
	s.transfer = 16;
	s.matrix = 9;
	s.field_seq = true;
	s.display_window = true;
	s.timing = true;
	s.units = 1001;
	s.scale = 24000;

	auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Main");
	aspect(want, 12, 11);
	colour(want, 9, 16, 9);
	want.full_range = true;
	want.interlaced = true;
	want.frame_rate = 24000.0 / 1001;

	const auto nal = hevc_nal(s);
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, nal, want), "");
	check_cut_short(libfiles::detail::parse_hevc_sps, nal);
	check_damaged(libfiles::detail::parse_hevc_sps, nal);

	// The sub-layer ordering of the highest alone, and one sub-layer.
	s.every_sub_layer = false;
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, hevc_nal(s), want), "");
	s.sub_layers = 1;
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, hevc_nal(s), want), "");
	s.sub_layers = 6;
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, hevc_nal(s), want), "");
}

TEST("media video HEVC SPS reads Main 10 without a VUI")
{
	hevc_sps s;
	s.profile_idc = 2;
	s.compatibility = 0x20000000;
	s.width = 3840;
	s.height = 2160;
	s.luma_depth = 10;
	s.chroma_depth = 10;

	const auto nal = hevc_nal(s);
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, nal, picture(3840, 2160, chroma_format::yuv420, 10, "Main 10")), "");
	check_cut_short(libfiles::detail::parse_hevc_sps, nal);
}

TEST("media video HEVC conformance windows count chroma samples")
{
	using libfiles::detail::parse_hevc_sps;

	hevc_sps s;
	s.profile_idc = 4;
	s.compatibility = 0x08000000;
	s.chroma_format_idc = 2;
	s.luma_depth = 10;
	s.chroma_depth = 10;
	s.window = {1, 1, 1, 1};
	CHECK_STR(read_as(parse_hevc_sps, hevc_nal(s), picture(1916, 1078, chroma_format::yuv422, 10, "Rext")), "");

	// 4:4:4 with the identity matrix is RGB.
	s.chroma_format_idc = 3;
	s.luma_depth = 12;
	s.chroma_depth = 12;
	s.window = {1, 0, 0, 1};
	s.vui = true;
	s.signal = true;
	s.full_range = true;
	s.colour = true;
	s.primaries = 1;
	s.transfer = 13;
	s.matrix = 0;
	auto want = picture(1919, 1079, chroma_format::rgb, 12, "Rext");
	colour(want, 1, 13, 0);
	want.full_range = true;
	CHECK_STR(read_as(parse_hevc_sps, hevc_nal(s), want), "");
	check_cut_short(parse_hevc_sps, hevc_nal(s));

	s.vui = false;
	s.chroma_format_idc = 0;
	s.window = {2, 0, 0, 2};
	CHECK_STR(read_as(parse_hevc_sps, hevc_nal(s), picture(1918, 1078, chroma_format::gray, 12, "Rext")), "");

	s.chroma_format_idc = 3;
	s.separate_planes = true;
	s.window = {0, 1, 1, 0};
	CHECK_STR(read_as(parse_hevc_sps, hevc_nal(s), picture(1919, 1079, chroma_format::yuv444, 12, "Rext")), "");
}

TEST("media video HEVC profiles are named as FFmpeg names them")
{
	struct case_
	{
		unsigned idc;
		std::uint32_t compatibility;
		const char* name;
	};

	constexpr case_ cases[] = {
		{1, 0, "Main"}, {2, 0, "Main 10"}, {3, 0, "Main Still Picture"}, {4, 0, "Rext"}, {6, 0, "Multiview Main"},
		{9, 0, "Scc"}, {5, 0, ""}, {0, 0x20000000, "Main 10"}, {0, 0x40000000, "Main"}, {0, 0x60000000, "Main"},
		{0, 0x08000000, "Rext"}, {0, 0x80000000, ""}, {0, 0, ""},
	};

	for (const auto& c : cases)
	{
		hevc_sps s;
		s.profile_idc = c.idc;
		s.compatibility = c.compatibility;
		const auto h = read(libfiles::detail::parse_hevc_sps, hevc_nal(s));
		CHECK_STR(h.profile, c.name);
		CHECK_EQ(h.width, 1920u);
	}
}

TEST("media video HEVC SPS takes an interlaced source from its profile")
{
	hevc_sps s;
	s.progressive_source = false;
	s.interlaced_source = true;

	auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Main");
	want.interlaced = true;
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, hevc_nal(s), want), "");

	// Both flags say each picture states its own scan.
	s.progressive_source = true;
	want.interlaced = false;
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, hevc_nal(s), want), "");
}

TEST("media video HEVC SPS keeps its picture when what follows it is damaged")
{
	hevc_sps s;
	s.sets_instead = 65;
	CHECK_STR(read_as(libfiles::detail::parse_hevc_sps, hevc_nal(s), picture(1920, 1080, chroma_format::yuv420, 8, "Main")), "");

	// A VUI cut short keeps what it had read whole: the shape and colour, not the timing.
	hevc_sps v;
	v.vui = true;
	v.aspect_idc = 14;
	v.signal = true;
	v.colour = true;
	v.primaries = 1;
	v.transfer = 1;
	v.matrix = 1;
	v.timing = true;
	v.units = 1;
	v.scale = 60;

	auto nal = hevc_nal(v);
	const auto whole = read(libfiles::detail::parse_hevc_sps, nal);
	CHECK(std::abs(whole.frame_rate - 60) < 1e-9);

	// Cut where the timing begins: everything before it is kept.
	for (auto n = nal.size(); n > 0; --n)
	{
		const auto h = read(libfiles::detail::parse_hevc_sps, first(nal, n));
		if (h.frame_rate > 0) continue;

		auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Main");
		aspect(want, 4, 3);
		colour(want, 1, 1, 1);
		CHECK_STR(differences(h, want), "");
		break;
	}
}

TEST("media video HEVC SPS refuses what is not one")
{
	using libfiles::detail::parse_hevc_sps;

	hevc_sps s;
	s.nal_type = 32;
	CHECK(refuses(parse_hevc_sps, hevc_nal(s)));

	auto nal = hevc_nal(hevc_sps{});
	auto damaged = nal;
	damaged[0] = static_cast<std::uint8_t>(damaged[0] | 0x80);
	CHECK(refuses(parse_hevc_sps, damaged));
	damaged = nal;
	damaged[1] = static_cast<std::uint8_t>(damaged[1] & 0xF8);
	CHECK(refuses(parse_hevc_sps, damaged));
	CHECK(refuses(parse_hevc_sps, first(nal, 2)));

	s = {};
	s.sub_layers = 7;
	CHECK(refuses(parse_hevc_sps, hevc_nal(s)));

	s = {};
	s.chroma_format_idc = 4;
	CHECK(refuses(parse_hevc_sps, hevc_nal(s)));

	s = {};
	s.width = 0;
	CHECK(refuses(parse_hevc_sps, hevc_nal(s)));
	s.width = 70000;
	CHECK(refuses(parse_hevc_sps, hevc_nal(s)));

	s = {};
	s.window = {0, 0, 0, 540};
	CHECK(refuses(parse_hevc_sps, hevc_nal(s)));
	s.window = {0, 0, 0, 539};
	CHECK_STR(read_as(parse_hevc_sps, hevc_nal(s), picture(1920, 2, chroma_format::yuv420, 8, "Main")), "");

	s = {};
	s.luma_depth = 17;
	CHECK(refuses(parse_hevc_sps, hevc_nal(s)));

	// Another layer's SPS in the ordinary syntax reads as one.
	s = {};
	s.layer = 1;
	CHECK_STR(read_as(parse_hevc_sps, hevc_nal(s), picture(1920, 1080, chroma_format::yuv420, 8, "Main")), "");
}

TEST("media video hvcC reads its base layer's first SPS, else its own fields")
{
	using libfiles::detail::parse_hevc_config;

	hevc_sps s;
	s.profile_idc = 2;
	s.compatibility = 0x20000000;
	s.chroma_format_idc = 2;
	s.luma_depth = 10;
	s.chroma_depth = 10;
	s.vui = true;
	s.aspect_idc = 1;
	s.timing = true;
	s.units = 1;
	s.scale = 50;

	hevc_record r;
	r.profile_idc = 2;
	r.compatibility = 0x20000000;
	r.chroma_format_idc = 2;
	r.luma_depth = 10;

	const bytes vps = {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF};
	const bytes pps = {0x44, 0x01, 0xC1, 0x72, 0xB4};

	auto want = picture(1920, 1080, chroma_format::yuv422, 10, "Main 10");
	aspect(want, 1, 1);
	want.frame_rate = 50;

	const auto record = hevc_config(r, {{32, {vps}}, {33, {hevc_nal(s)}}, {34, {pps}}});
	CHECK_STR(read_as(parse_hevc_config, record, want), "");
	check_cut_short(parse_hevc_config, record);
	check_damaged(parse_hevc_config, record);

	// Without a usable SPS: the record's own profile, sampling, depth and source scan.
	r.progressive_source = false;
	r.interlaced_source = true;
	auto own = picture(0, 0, chroma_format::yuv422, 10, "Main 10");
	own.interlaced = true;
	CHECK_STR(read_as(parse_hevc_config, hevc_config(r, {}), own), "");
	CHECK_STR(read_as(parse_hevc_config, hevc_config(r, {{33, {bytes{0x42, 0x01, 0xFF}}}}), own), "");
	CHECK_STR(read_as(parse_hevc_config, first(record, 30), picture(0, 0, chroma_format::yuv422, 10, "Main 10")), "");

	// Another layer's SPS is passed over for the base layer's.
	hevc_sps other = s;
	other.layer = 1;
	other.width = 960;
	CHECK_STR(read_as(parse_hevc_config, hevc_config(r, {{33, {hevc_nal(other), hevc_nal(s)}}}), want), "");

	// An Annex B stream in the record's place, a version past 1 and a record cut short.
	CHECK(refuses(parse_hevc_config, bytes{0, 0, 0, 1, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x02, 0x20, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 0, 0, 0x5D}));
	auto version = record;
	version[0] = 2;
	CHECK(refuses(parse_hevc_config, version));
	version[0] = 0;
	CHECK_STR(read_as(parse_hevc_config, version, want), "");
	CHECK(refuses(parse_hevc_config, first(record, 22)));
}

TEST("media video hvcC records from real files read as FFmpeg reads them")
{
	using libfiles::detail::parse_hevc_config;

	// x265's records, trimmed to their VPS, SPS and PPS.
	auto hlg = picture(128, 72, chroma_format::yuv420, 10, "Main 10");
	aspect(hlg, 1, 1);
	colour(hlg, 9, 18, 9);
	hlg.frame_rate = 25;
	const auto hlg_record = hex(
		"0102200000009000000000001ef000fcfdfafa00000f03200001001840010c01ffff02200000030090000003000003001e9594092100010"
		"02e42010102200000030090000003000003001ea0102051f2b6595952930bc05a84890482000003000200000300321022000100064401c073c189");
	CHECK_STR(read_as(parse_hevc_config, hlg_record, hlg), "");
	check_cut_short(parse_hevc_config, hlg_record);
	check_damaged(parse_hevc_config, hlg_record);

	auto pq = hlg;
	pq.transfer = 16;
	CHECK_STR(read_as(parse_hevc_config, hex(
		"0102200000009000000000001ef000fcfdfafa00000f03200001001840010c01ffff02200000030090000003000003001e9594092100010"
		"02e42010102200000030090000003000003001ea0102051f2b6595952930bc05a84880482000003000200000300321022000100064401c073c189"), pq), "");

	auto rext = picture(128, 72, chroma_format::yuv422, 10, "Rext");
	aspect(rext, 1, 1);
	rext.frame_rate = 25;
	CHECK_STR(read_as(parse_hevc_config, hex(
		"0104080000009d08000000001ef000fcfefafa00000f03200001001740010c01ffff0408000003009d0800000300001e959409210001002"
		"94201010408000003009d0800000300001eb0102051f12d965654a4c2f0168080000003008000000c8422000100064401c073c189"), rext), "");
}

// ---- AV1 -----------------------------------------------------------------------------------------------

TEST("media video AV1 sequence header reads through its timing and operating points")
{
	using libfiles::detail::parse_av1_sequence;

	av1_sequence s;
	s.timing = true;
	s.units = 1001;
	s.scale = 60000;
	s.equal_interval = true;
	s.ticks_minus_1 = 1;
	s.decoder_model = true;
	s.display_delay = true;
	s.operating_points = 3;
	s.frame_ids = true;
	s.colour = true;
	s.primaries = 1;
	s.transfer = 1;
	s.matrix = 1;

	auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Main");
	colour(want, 1, 1, 1);
	want.frame_rate = 60000.0 / 2002;

	// After a temporal delimiter, with an extension byte and a padded size.
	const auto stream = builder().raw(obu(2, {})).raw(obu(1, av1_payload(s), true, true, 3)).raw(obu(6, bytes(40, 0x55))).b;
	CHECK_STR(read_as(parse_av1_sequence, stream, want), "");
	check_cut_short(parse_av1_sequence, stream);
	check_damaged(parse_av1_sequence, stream);

	// Timing without an equal interval states no rate.
	s.equal_interval = false;
	want.frame_rate = 0;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(s)), want), "");

	// Screen content tools forced off leave the integer motion vector bits out; forced on keep them.
	for (const auto& [screen, integer] : {std::pair{0, 2}, std::pair{1, 0}, std::pair{1, 2}, std::pair{2, 1}})
	{
		s.screen_content = screen;
		s.integer_mv = integer;
		s.order_hint = screen != 1;
		CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(s)), want), "");
	}
}

TEST("media video AV1 colour configurations by profile and depth")
{
	using libfiles::detail::parse_av1_sequence;

	av1_sequence high;
	high.profile = 1;
	high.depth = 10;
	high.full_range = true;
	high.colour = true;
	high.primaries = 9;
	high.transfer = 16;
	high.matrix = 9;
	auto want = picture(1920, 1080, chroma_format::yuv444, 10, "High");
	colour(want, 9, 16, 9);
	want.full_range = true;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(high)), want), "");

	// sRGB says nothing of its range and sampling: full, and 4:4:4 as RGB.
	av1_sequence srgb;
	srgb.profile = 1;
	srgb.colour = true;
	srgb.primaries = 1;
	srgb.transfer = 13;
	srgb.matrix = 0;
	want = picture(1920, 1080, chroma_format::rgb, 8, "High");
	colour(want, 1, 13, 0);
	want.full_range = true;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(srgb)), want), "");
	check_cut_short(parse_av1_sequence, obu(1, av1_payload(srgb)));

	av1_sequence pro;
	pro.profile = 2;
	pro.depth = 12;
	pro.subsampling_y = false;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(pro)), picture(1920, 1080, chroma_format::yuv422, 12, "Professional")), "");
	pro.subsampling_y = true;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(pro)), picture(1920, 1080, chroma_format::yuv420, 12, "Professional")), "");
	pro.subsampling_x = false;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(pro)), picture(1920, 1080, chroma_format::yuv444, 12, "Professional")), "");
	pro.depth = 10;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(pro)), picture(1920, 1080, chroma_format::yuv422, 10, "Professional")), "");
	pro.depth = 8;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(pro)), picture(1920, 1080, chroma_format::yuv422, 8, "Professional")), "");

	av1_sequence mono;
	mono.depth = 10;
	mono.mono = true;
	mono.full_range = true;
	want = picture(1920, 1080, chroma_format::gray, 10, "Main");
	want.full_range = true;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(mono)), want), "");

	// A reduced still picture header, and the largest size a scan reports.
	av1_sequence still;
	still.reduced = true;
	still.width = 4000;
	still.height = 3000;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(still)), picture(4000, 3000, chroma_format::yuv420, 8, "Main")), "");
	still.width = 65535;
	CHECK_STR(read_as(parse_av1_sequence, obu(1, av1_payload(still)), picture(65535, 3000, chroma_format::yuv420, 8, "Main")), "");
	still.width = 65536;
	CHECK(refuses(parse_av1_sequence, obu(1, av1_payload(still))));
}

TEST("media video AV1 OBUs are walked by their sizes")
{
	using libfiles::detail::parse_av1_sequence;

	const auto payload = av1_payload(av1_sequence{});
	const auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Main");

	// The last OBU may go without a size, as a sample's does.
	CHECK_STR(read_as(parse_av1_sequence, builder().raw(obu(2, {})).raw(obu(1, payload, false)).b, want), "");
	CHECK_STR(read_as(parse_av1_sequence, builder().raw(obu(5, bytes(200, 0))).raw(obu(1, payload)).b, want), "");

	CHECK(refuses(parse_av1_sequence, obu(2, {})));
	CHECK(refuses(parse_av1_sequence, {}));

	auto forbidden = obu(1, payload);
	forbidden[0] = static_cast<std::uint8_t>(forbidden[0] | 0x80);
	CHECK(refuses(parse_av1_sequence, forbidden));

	// A size past the data before the sequence header, and a leb128 longer than 32 bits.
	auto overlong = builder().raw(obu(5, bytes(10, 0))).raw(obu(1, payload)).b;
	overlong[1] = 100;
	CHECK(refuses(parse_av1_sequence, overlong));
	CHECK(refuses(parse_av1_sequence, builder().u8(0x2A).u8(0xFF).u8(0xFF).u8(0xFF).u8(0xFF).u8(0x7F).raw(obu(1, payload)).b));

	auto reserved = payload;
	reserved[0] = static_cast<std::uint8_t>(reserved[0] | 0x60);
	CHECK(refuses(parse_av1_sequence, obu(1, reserved)));
}

TEST("media video av1C reads its own fields, then a sequence header among its OBUs")
{
	using libfiles::detail::parse_av1_config;

	// high_bitdepth, twelve_bit, then chroma_subsampling_x without y: Professional 12-bit 4:2:2.
	CHECK_STR(read_as(parse_av1_config, av1_config(2, 0x68), picture(0, 0, chroma_format::yuv422, 12, "Professional")), "");
	CHECK_STR(read_as(parse_av1_config, av1_config(0, 0x0C), picture(0, 0, chroma_format::yuv420, 8, "Main")), "");
	CHECK_STR(read_as(parse_av1_config, av1_config(1, 0x40), picture(0, 0, chroma_format::yuv444, 10, "High")), "");
	CHECK_STR(read_as(parse_av1_config, av1_config(0, 0x1C), picture(0, 0, chroma_format::gray, 8, "Main")), "");
	// twelve_bit means 12 bits only in the professional profile.
	CHECK_STR(read_as(parse_av1_config, av1_config(0, 0x6C), picture(0, 0, chroma_format::yuv420, 10, "Main")), "");

	av1_sequence s;
	s.depth = 10;
	s.colour = true;
	s.primaries = 9;
	s.transfer = 18;
	s.matrix = 9;
	auto want = picture(1920, 1080, chroma_format::yuv420, 10, "Main");
	colour(want, 9, 18, 9);

	const auto record = av1_config(0, 0x4C, obu(1, av1_payload(s)));
	CHECK_STR(read_as(parse_av1_config, record, want), "");
	check_cut_short(parse_av1_config, record);
	check_damaged(parse_av1_config, record);

	// OBUs that hold no sequence header leave the record's fields.
	CHECK_STR(read_as(parse_av1_config, av1_config(0, 0x4C, obu(5, bytes(8, 1))), picture(0, 0, chroma_format::yuv420, 10, "Main")), "");

	CHECK(refuses(parse_av1_config, first(record, 3)));
	auto marker = record;
	marker[0] = 0x01;
	CHECK(refuses(parse_av1_config, marker));
	marker[0] = 0x82;
	CHECK(refuses(parse_av1_config, marker));
	CHECK(refuses(parse_av1_config, av1_config(3, 0x0C)));
}

TEST("media video av1C from a real file reads as FFmpeg reads it")
{
	// av1.mp4: libaom's Main 4:2:0 at video levels, with no timing.
	const auto record = hex("81000c000a0a0000000337f8e6d7c802");
	CHECK_STR(read_as(libfiles::detail::parse_av1_config, record, picture(128, 72, chroma_format::yuv420, 8, "Main")), "");
	check_cut_short(libfiles::detail::parse_av1_config, record);
}

// ---- VP9, VP8 and the VP codec record ------------------------------------------------------------------

TEST("media video VP9 key frames give their profile, depth, colour and size")
{
	using libfiles::detail::parse_vp9_frame;

	vp9_header f;
	auto want = picture(352, 288, chroma_format::yuv420, 8, "Profile 0");
	want.matrix = 1;
	const auto frame = vp9_frame(f);
	CHECK_STR(read_as(parse_vp9_frame, frame, want), "");
	check_cut_short(parse_vp9_frame, frame);
	check_damaged(parse_vp9_frame, frame);

	// Profile 1: 8 bits at 4:4:4, 4:2:2 or 4:4:0, and sRGB as RGB at full range.
	f.profile = 1;
	f.space = 1;
	f.full_range = true;
	f.x = false;
	f.y = false;
	want = picture(352, 288, chroma_format::yuv444, 8, "Profile 1");
	want.matrix = 5;
	want.full_range = true;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");
	f.x = true;
	want.chroma = chroma_format::yuv422;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");
	f.x = false;
	f.y = true;
	want.chroma = chroma_format::yuv440;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");
	f.space = 7;
	want.chroma = chroma_format::rgb;
	want.matrix = 0;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");

	// Profiles 2 and 3: 10 or 12 bits.
	f = {};
	f.profile = 2;
	f.depth = 10;
	f.space = 5;
	f.width = 3840;
	f.height = 2160;
	want = picture(3840, 2160, chroma_format::yuv420, 10, "Profile 2");
	want.matrix = 9;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");
	f.depth = 12;
	want.bit_depth = 12;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");

	f.profile = 3;
	f.space = 3;
	f.y = false;
	want = picture(3840, 2160, chroma_format::yuv422, 12, "Profile 3");
	want.matrix = 6;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");
	check_cut_short(parse_vp9_frame, vp9_frame(f));

	// color_space 6 is reserved.
	f.space = 6;
	want.matrix = 2;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");
}

TEST("media video VP9 frames that are not key frames")
{
	using libfiles::detail::parse_vp9_frame;

	// Intra-only frames: profile 0 implies BT.601 4:2:0 at 8 bits, the others state theirs.
	vp9_header f;
	f.key = false;
	f.show = false;
	f.intra_only = true;
	auto want = picture(352, 288, chroma_format::yuv420, 8, "Profile 0");
	want.matrix = 5;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");
	check_cut_short(parse_vp9_frame, vp9_frame(f));

	f.profile = 2;
	f.depth = 10;
	f.error_resilient = true;
	want = picture(352, 288, chroma_format::yuv420, 10, "Profile 2");
	want.matrix = 1;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(f), want), "");

	// An inter frame with a size of its own gives that and its profile.
	vp9_header inter;
	inter.key = false;
	inter.width = 640;
	inter.height = 360;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(inter), picture(640, 360, chroma_format::unknown, 0, "Profile 0")), "");
	inter.show = false;
	inter.error_resilient = true;
	CHECK_STR(read_as(parse_vp9_frame, vp9_frame(inter), picture(640, 360, chroma_format::unknown, 0, "Profile 0")), "");

	// One that takes a reference's size, and one that shows a frame decoded before, give none.
	inter.found_ref = 1;
	CHECK(refuses(parse_vp9_frame, vp9_frame(inter)));
	vp9_header existing;
	existing.existing = true;
	CHECK(refuses(parse_vp9_frame, vp9_frame(existing)));
}

TEST("media video VP9 frames refused for their marker, sync code or reserved bits")
{
	using libfiles::detail::parse_vp9_frame;

	vp9_header f;
	f.marker = 3;
	CHECK(refuses(parse_vp9_frame, vp9_frame(f)));

	f = {};
	f.sync = 0x498343;
	CHECK(refuses(parse_vp9_frame, vp9_frame(f)));

	f = {};
	f.profile = 3;
	f.depth = 10;
	f.y = false;
	f.reserved = true;
	CHECK(refuses(parse_vp9_frame, vp9_frame(f)));

	// 4:2:0 in profile 1, RGB in profile 0, and the colour configuration's reserved bit.
	f = {};
	f.profile = 1;
	CHECK(refuses(parse_vp9_frame, vp9_frame(f)));
	f = {};
	f.space = 7;
	CHECK(refuses(parse_vp9_frame, vp9_frame(f)));
	f = {};
	f.profile = 1;
	f.y = false;
	f.colour_reserved = true;
	CHECK(refuses(parse_vp9_frame, vp9_frame(f)));

	f = {};
	f.width = 65536;
	CHECK(refuses(parse_vp9_frame, vp9_frame(f)));
	CHECK(refuses(parse_vp9_frame, {}));
}

TEST("media video VP8 key frames give their size")
{
	using libfiles::detail::parse_vp8_frame;

	const auto frame = vp8_frame(640, 480);
	CHECK_STR(read_as(parse_vp8_frame, frame, picture(640, 480, chroma_format::yuv420, 8, "Profile 0")), "");
	check_cut_short(parse_vp8_frame, first(frame, 16));
	check_damaged(parse_vp8_frame, first(frame, 16));

	// The version names the profile; the size's top two bits are an upscaling.
	auto scaled = frame;
	scaled[0] = static_cast<std::uint8_t>(scaled[0] | 3 << 1);
	scaled[7] = static_cast<std::uint8_t>(scaled[7] | 0xC0);
	scaled[9] = static_cast<std::uint8_t>(scaled[9] | 0x40);
	CHECK_STR(read_as(parse_vp8_frame, scaled, picture(640, 480, chroma_format::yuv420, 8, "Profile 3")), "");
	scaled[0] = static_cast<std::uint8_t>(scaled[0] | 4 << 1);
	CHECK_STR(read_as(parse_vp8_frame, scaled, picture(640, 480, chroma_format::yuv420, 8)), "");

	auto inter = frame;
	inter[0] = static_cast<std::uint8_t>(inter[0] | 1);
	CHECK(refuses(parse_vp8_frame, inter));
	auto code = frame;
	code[4] = 0x02;
	CHECK(refuses(parse_vp8_frame, code));
	CHECK(refuses(parse_vp8_frame, vp8_frame(0, 480)));

	// tagged.webm's frame.
	CHECK_STR(read_as(parse_vp8_frame, hex("3000009d012a100010000000000000000000"), picture(16, 16, chroma_format::yuv420, 8, "Profile 0")), "");
}

TEST("media video vpcC gives the profile, depth, sampling, range and colour")
{
	using libfiles::detail::parse_vp_config;

	const auto record = [](const unsigned profile, const unsigned depth, const unsigned subsampling, const bool full,
	                       const unsigned primaries, const unsigned transfer, const unsigned matrix)
	{
		return builder().u8(profile).u8(31).u8(depth << 4 | subsampling << 1 | (full ? 1u : 0)).u8(primaries).u8(transfer).u8(matrix).be16(0).b;
	};

	auto want = picture(0, 0, chroma_format::yuv420, 10, "Profile 2");
	colour(want, 9, 16, 9);
	want.full_range = true;
	const auto hdr = record(2, 10, 1, true, 9, 16, 9);
	CHECK_STR(read_as(parse_vp_config, hdr, want), "");
	check_cut_short(parse_vp_config, hdr);
	check_damaged(parse_vp_config, hdr);

	want = picture(0, 0, chroma_format::yuv420, 8, "Profile 0");
	colour(want, 1, 1, 1);
	CHECK_STR(read_as(parse_vp_config, record(0, 8, 0, false, 1, 1, 1), want), "");
	want = picture(0, 0, chroma_format::yuv422, 8, "Profile 1");
	colour(want, 1, 1, 1);
	CHECK_STR(read_as(parse_vp_config, record(1, 8, 2, false, 1, 1, 1), want), "");
	want.chroma = chroma_format::yuv444;
	CHECK_STR(read_as(parse_vp_config, record(1, 8, 3, false, 1, 1, 1), want), "");

	// The identity matrix at 4:4:4 is RGB; reserved code points say nothing.
	want = picture(0, 0, chroma_format::rgb, 12, "Profile 3");
	colour(want, 1, 13, 0);
	want.full_range = true;
	CHECK_STR(read_as(parse_vp_config, record(3, 12, 3, true, 1, 13, 0), want), "");
	CHECK_STR(read_as(parse_vp_config, record(0, 8, 1, false, 3, 0, 3), picture(0, 0, chroma_format::yuv420, 8, "Profile 0")), "");

	CHECK(refuses(parse_vp_config, record(0, 9, 1, false, 1, 1, 1)));
	CHECK(refuses(parse_vp_config, record(0, 8, 4, false, 1, 1, 1)));
	CHECK(refuses(parse_vp_config, record(4, 8, 1, false, 1, 1, 1)));
}

// ---- MPEG-4 Part 2 -------------------------------------------------------------------------------------

TEST("media video MPEG-4 Part 2 reads its visual object sequence down to the layer")
{
	using libfiles::detail::parse_mpeg4_video;

	mpeg4_stream s;
	s.object_verid = 2;
	s.signal = true;
	s.full_range = true;
	s.colour = true;
	s.primaries = 1;
	s.transfer = 1;
	s.matrix = 1;
	s.aspect = 3;
	s.control = true;
	s.vbv = true;
	s.fixed_rate = true;
	s.interlaced = true;

	auto want = picture(720, 480, chroma_format::yuv420, 8, "Advanced Simple Profile");
	aspect(want, 10, 11);
	colour(want, 1, 1, 1);
	want.full_range = true;
	want.interlaced = true;
	want.frame_rate = 30000.0 / 1001;

	const auto headers = mpeg4_headers(s);
	CHECK_STR(read_as(parse_mpeg4_video, headers, want), "");
	check_cut_short(parse_mpeg4_video, headers);
	check_damaged(parse_mpeg4_video, headers);
}

TEST("media video MPEG-4 Part 2 reads a layer alone")
{
	using libfiles::detail::parse_mpeg4_video;

	mpeg4_stream s;
	s.sequence = false;
	s.object = false;
	s.type = 1;
	s.aspect = 15;
	s.par_width = 64;
	s.par_height = 45;
	s.resolution = 25;
	s.width = 352;
	s.height = 288;

	auto want = picture(352, 288, chroma_format::yuv420, 8);
	aspect(want, 64, 45);
	CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");

	// A fixed rate of one tick in 25.
	s.fixed_rate = true;
	s.increment = 1;
	want.frame_rate = 25;
	CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");

	// The shapes of a pixel by aspect_ratio_info, which reserves 6 to 14.
	for (const auto& [code, width, height] : {std::tuple{1u, 1u, 1u}, std::tuple{2u, 12u, 11u}, std::tuple{4u, 16u, 11u}, std::tuple{5u, 40u, 33u}, std::tuple{9u, 0u, 0u}})
	{
		s.aspect = code;
		aspect(want, width, height);
		CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");
	}
}

TEST("media video MPEG-4 Part 2 layers with sprites, depth and shapes")
{
	using libfiles::detail::parse_mpeg4_video;

	// Version 2: two bits of sprite_enable, here global motion compensation, and a 10-bit picture.
	mpeg4_stream s;
	s.layer_verid = 2;
	s.sprite = 2;
	s.not_8_bit = true;
	s.depth = 10;
	s.width = 640;
	s.height = 360;
	auto want = picture(640, 360, chroma_format::yuv420, 10, "Advanced Simple Profile");
	aspect(want, 1, 1);
	CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");
	check_cut_short(parse_mpeg4_video, mpeg4_headers(s));

	// Version 1: a static sprite's size and place, and 12 bits.
	s.layer_verid = 1;
	s.sprite = 1;
	s.depth = 12;
	want.bit_depth = 12;
	CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");

	// Shapes other than a rectangle have no size; a grayscale one in version 2 has an extension.
	s = {};
	s.shape = 2;
	want = picture(0, 0, chroma_format::yuv420, 8, "Advanced Simple Profile");
	aspect(want, 1, 1);
	CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");
	s.shape = 3;
	s.layer_verid = 2;
	s.interlaced = true;
	want.interlaced = true;
	CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");
	s.shape = 1;
	s.layer_verid = 1;
	CHECK_STR(read_as(parse_mpeg4_video, mpeg4_headers(s), want), "");
}

TEST("media video MPEG-4 Part 2 profiles are named by their high bits, as FFmpeg names them")
{
	struct case_
	{
		unsigned indication;
		const char* name;
	};

	constexpr case_ cases[] = {
		{0x01, "Simple Profile"}, {0x08, "Simple Profile"}, {0x11, "Simple Scalable Profile"}, {0x21, "Core Profile"},
		{0x32, "Main Profile"}, {0x42, "N-bit Profile"}, {0x51, "Scalable Texture Profile"},
		{0x61, "Simple Face Animation Profile"}, {0x71, "Basic Animated Texture Profile"}, {0x81, "Hybrid Profile"},
		{0x91, "Advanced Real Time Simple Profile"}, {0xA1, "Core Scalable Profile"}, {0xB1, "Advanced Coding Profile"},
		{0xC1, "Advanced Core Profile"}, {0xD1, "Advanced Scalable Texture Profile"}, {0xE1, "Simple Studio Profile"},
		{0xF3, "Advanced Simple Profile"},
	};

	for (const auto& c : cases)
	{
		mpeg4_stream s;
		s.profile_level = c.indication;
		CHECK_STR(read(libfiles::detail::parse_mpeg4_video, mpeg4_headers(s)).profile, c.name);
	}
}

TEST("media video MPEG-4 Part 2 refuses streams it cannot read")
{
	using libfiles::detail::parse_mpeg4_video;

	mpeg4_stream s;
	s.resolution = 0;
	CHECK(refuses(parse_mpeg4_video, mpeg4_headers(s)));

	s = {};
	s.type = 15;
	CHECK(refuses(parse_mpeg4_video, mpeg4_headers(s)));
	s.type = 14;
	CHECK(refuses(parse_mpeg4_video, mpeg4_headers(s)));

	s = {};
	s.width = 0;
	CHECK(refuses(parse_mpeg4_video, mpeg4_headers(s)));

	// Start codes without a layer.
	CHECK(refuses(parse_mpeg4_video, builder().be32(0x1B0).u8(0xF5).be32(0x100).be32(0x1B6).u8(0x12).b));
	CHECK(refuses(parse_mpeg4_video, {}));
}

TEST("media video MPEG-4 Part 2 from a real file reads as FFmpeg reads it")
{
	// ipod.mov's esds: 3ivx's Simple Profile, which fixes 25 frames a second though the file plays 10.
	const auto info = hex("000001b001000001b58913000001000000012000c488800ce18a021e0a31000001b23369767840a7");
	auto want = picture(640, 480, chroma_format::yuv420, 8, "Simple Profile");
	aspect(want, 1, 1);
	want.frame_rate = 25;
	CHECK_STR(read_as(libfiles::detail::parse_mpeg4_video, info, want), "");
	check_cut_short(libfiles::detail::parse_mpeg4_video, info);
}

// ---- MPEG-1 and MPEG-2 ---------------------------------------------------------------------------------

TEST("media video MPEG-1 sequence headers give pixel shapes from their own table")
{
	using libfiles::detail::parse_mpeg12_video;

	mpeg12_stream s;
	s.width = 352;
	s.height = 240;
	s.aspect = 12;
	s.extension = false;

	auto want = picture(352, 240, chroma_format::yuv420, 8);
	aspect(want, 200, 219);
	want.frame_rate = 30000.0 / 1001;
	const auto headers = mpeg12_headers(s);
	CHECK_STR(read_as(parse_mpeg12_video, headers, want), "");
	check_cut_short(parse_mpeg12_video, headers);
	check_damaged(parse_mpeg12_video, headers);

	// A header that ends the data is MPEG-1's, quantiser matrices and all.
	s.intra_matrix = true;
	s.non_intra_matrix = true;
	s.aspect = 8;
	s.rate = 3;
	aspect(want, 178, 163);
	want.frame_rate = 25;
	const auto alone = first(mpeg12_headers(s), 4 + 8 + 128);
	CHECK_STR(read_as(parse_mpeg12_video, alone, want), "");

	// Xing's 15 frames a second, and the reserved codes.
	s.rate = 9;
	want.frame_rate = 15;
	CHECK_STR(read_as(parse_mpeg12_video, mpeg12_headers(s), want), "");
	s.rate = 14;
	s.aspect = 15;
	want.frame_rate = 0;
	aspect(want, 0, 0);
	CHECK_STR(read_as(parse_mpeg12_video, mpeg12_headers(s), want), "");
}

TEST("media video MPEG-2 sequence extensions give the profile, sampling, scan and rate")
{
	using libfiles::detail::parse_mpeg12_video;

	// Main at Main level, 16:9 on 720 by 480 pixels, interlaced.
	mpeg12_stream s;
	auto want = picture(720, 480, chroma_format::yuv420, 8, "Main");
	aspect(want, 32, 27);
	want.interlaced = true;
	want.frame_rate = 30000.0 / 1001;
	const auto headers = mpeg12_headers(s);
	CHECK_STR(read_as(parse_mpeg12_video, headers, want), "");

	// Cut where the sequence header ends, it is MPEG-1's: code 3 is a 0.7031 pixel there.
	auto mpeg1 = picture(720, 480, chroma_format::yuv420, 8);
	aspect(mpeg1, 64, 45);
	mpeg1.frame_rate = 30000.0 / 1001;
	CHECK_STR(read_as(parse_mpeg12_video, first(headers, 12), mpeg1), "");
	check_cut_short(parse_mpeg12_video, headers, mpeg2_cut_short(headers, 12));
	check_damaged(parse_mpeg12_video, headers);

	// The 4:2:2 profile past both matrices, 4:3 pixels, and twice the rate.
	s.intra_matrix = true;
	s.non_intra_matrix = true;
	s.indication = 0x85;
	s.chroma = 2;
	s.aspect = 2;
	s.rate = 3;
	s.rate_n = 1;
	s.progressive = true;
	want = picture(720, 480, chroma_format::yuv422, 8, "4:2:2");
	aspect(want, 8, 9);
	want.frame_rate = 50;
	CHECK_STR(read_as(parse_mpeg12_video, mpeg12_headers(s), want), "");

	// Sizes past 12 bits, 4:4:4, a 2.21:1 display and High profile.
	s = {};
	s.width = 5000;
	s.height = 3000;
	s.aspect = 4;
	s.indication = 0x14;
	s.chroma = 3;
	s.progressive = true;
	s.rate = 8;
	want = picture(5000, 3000, chroma_format::yuv444, 8, "High");
	aspect(want, 663, 500);
	want.frame_rate = 60;
	CHECK_STR(read_as(parse_mpeg12_video, mpeg12_headers(s), want), "");
}

TEST("media video MPEG-2 sequence display extensions give the colour")
{
	using libfiles::detail::parse_mpeg12_video;

	mpeg12_stream s;
	s.aspect = 1;
	s.rate = 3;
	s.rate_n = 2;
	s.rate_d = 4;
	s.progressive = true;
	s.user_data = true;
	s.display = true;
	s.colour = true;
	s.primaries = 5;
	s.transfer = 6;
	s.matrix = 5;

	auto want = picture(720, 480, chroma_format::yuv420, 8, "Main");
	aspect(want, 1, 1);
	colour(want, 5, 6, 5);
	want.frame_rate = 15;
	const auto headers = mpeg12_headers(s);
	CHECK_STR(read_as(parse_mpeg12_video, headers, want), "");
	check_cut_short(parse_mpeg12_video, headers, mpeg2_cut_short(headers, 12));

	s.colour = false;
	colour(want, 2, 2, 2);
	CHECK_STR(read_as(parse_mpeg12_video, mpeg12_headers(s), want), "");
}

TEST("media video MPEG-2 profiles are named as FFmpeg names them")
{
	struct case_
	{
		unsigned indication;
		const char* name;
	};

	constexpr case_ cases[] = {
		{0x14, "High"}, {0x24, "Spatially Scalable"}, {0x34, "SNR Scalable"}, {0x48, "Main"}, {0x58, "Simple"},
		{0x82, "4:2:2"}, {0x85, "4:2:2"}, {0x8A, "Multi-view"}, {0x8E, "Multi-view"}, {0x64, ""}, {0x04, ""}, {0x80, ""},
	};

	for (const auto& c : cases)
	{
		mpeg12_stream s;
		s.indication = c.indication;
		CHECK_STR(read(libfiles::detail::parse_mpeg12_video, mpeg12_headers(s)).profile, c.name);
	}
}

TEST("media video MPEG-1 and MPEG-2 refuse damaged sequence headers")
{
	using libfiles::detail::parse_mpeg12_video;

	mpeg12_stream s;
	s.marker = false;
	CHECK(refuses(parse_mpeg12_video, mpeg12_headers(s)));

	s = {};
	s.width = 0;
	CHECK(refuses(parse_mpeg12_video, mpeg12_headers(s)));

	// After the header: a byte that starts no start code, and zeros that end the data.
	const auto headers = mpeg12_headers(mpeg12_stream{});
	auto junk = headers;
	junk[12] = 0x47;
	CHECK(refuses(parse_mpeg12_video, junk));
	CHECK(refuses(parse_mpeg12_video, first(headers, 14)));

	// Other start codes before the header are passed over; none at all is refused.
	auto want = picture(720, 480, chroma_format::yuv420, 8, "Main");
	aspect(want, 32, 27);
	want.interlaced = true;
	want.frame_rate = 30000.0 / 1001;
	CHECK_STR(read_as(parse_mpeg12_video, builder().u8(0).be32(0x1B8).u8(0x10).be32(0x100).raw(headers).b, want), "");
	CHECK(refuses(parse_mpeg12_video, builder().be32(0x1B8).be32(0x100).be32(0x1B7).b));
	CHECK(refuses(parse_mpeg12_video, {}));

	// An extension other than the sequence extension makes it MPEG-1.
	auto display_first = headers;
	display_first[16] = static_cast<std::uint8_t>((display_first[16] & 0x0F) | 0x20);
	CHECK(read(parse_mpeg12_video, display_first).profile.empty());
}

TEST("media video MPEG-2 from a real file reads as FFmpeg reads it")
{
	// gen.mpg: FFmpeg's encoder writes 15 frames a second as 25 times 3/5, and 4:3 on 160 by 120 pixels.
	const auto headers = hex("000001b30a007823ffffe000000001b5148a00010044000001b80008004000000100000ffff8000001b58ffff3418000");
	auto want = picture(160, 120, chroma_format::yuv420, 8, "Main");
	aspect(want, 1, 1);
	want.frame_rate = 15;
	CHECK_STR(read_as(libfiles::detail::parse_mpeg12_video, headers, want), "");
}

// ---- Motion JPEG ---------------------------------------------------------------------------------------

TEST("media video JPEG frames give their size, depth and sampling")
{
	using libfiles::detail::parse_jpeg_frame;

	auto want = picture(640, 480, chroma_format::yuv420, 8, "Baseline");
	want.matrix = 5;
	want.full_range = true;
	const auto frame = jpeg_frame(0xC0, 8, 640, 480, ycbcr420);
	CHECK_STR(read_as(parse_jpeg_frame, frame, want), "");
	check_cut_short(parse_jpeg_frame, frame);
	check_damaged(parse_jpeg_frame, frame);

	for (const auto& [luma, chroma] : {std::pair{0x21, chroma_format::yuv422}, std::pair{0x11, chroma_format::yuv444},
	                                   std::pair{0x41, chroma_format::yuv411}, std::pair{0x12, chroma_format::yuv440},
	                                   std::pair{0x42, chroma_format::unknown}})
	{
		want.chroma = chroma;
		const jpeg_components components = {{1, static_cast<std::uint8_t>(luma)}, {2, 0x11}, {3, 0x11}};
		CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 480, components), want), "");
	}

	// Chroma sampled finer than luma, or its two components sampled apart, names nothing.
	want.chroma = chroma_format::unknown;
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 480, jpeg_components{{1, 0x11}, {2, 0x22}, {3, 0x22}}), want), "");
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 480, jpeg_components{{1, 0x22}, {2, 0x11}, {3, 0x21}}), want), "");

	// Gray and CMYK state no colour.
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 480, jpeg_components{{1, 0x11}}), picture(640, 480, chroma_format::gray, 8, "Baseline")), "");
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 480, jpeg_components{{1, 0x11}, {2, 0x11}, {3, 0x11}, {4, 0x11}}),
	                  picture(640, 480, chroma_format::unknown, 8, "Baseline")), "");
}

TEST("media video JPEG frame headers name their process")
{
	using libfiles::detail::parse_jpeg_frame;

	struct case_
	{
		unsigned marker;
		unsigned precision;
		const char* name;
	};

	constexpr case_ cases[] = {
		{0xC1, 12, "Sequential"}, {0xC2, 8, "Progressive"}, {0xC3, 16, "Lossless"}, {0xC5, 8, ""}, {0xC7, 8, ""},
		{0xC9, 8, ""}, {0xCB, 8, ""}, {0xCD, 12, ""}, {0xCF, 8, ""},
	};

	for (const auto& c : cases)
	{
		auto want = picture(320, 240, chroma_format::yuv420, static_cast<std::uint8_t>(c.precision), c.name);
		want.matrix = 5;
		want.full_range = true;
		CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(c.marker, c.precision, 320, 240, ycbcr420), want), "");
	}
}

TEST("media video JPEG markers before the frame header: AVI1 fields, Adobe RGB and video levels")
{
	using libfiles::detail::parse_jpeg_frame;

	// Each image of the frame is one field of two.
	auto want = picture(720, 480, chroma_format::yuv420, 8, "Baseline");
	want.matrix = 5;
	want.full_range = true;
	want.interlaced = true;
	const auto fields = jpeg_frame(0xC0, 8, 720, 240, ycbcr420, avi1(1));
	CHECK_STR(read_as(parse_jpeg_frame, fields, want), "");
	check_cut_short(parse_jpeg_frame, fields);
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 720, 240, ycbcr420, avi1(2)), want), "");
	CHECK(refuses(parse_jpeg_frame, jpeg_frame(0xC0, 8, 720, 40000, ycbcr420, avi1(2))));

	want.height = 240;
	want.interlaced = false;
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 720, 240, ycbcr420, avi1(0)), want), "");

	// FFmpeg's comment for a frame it coded at video levels.
	want.full_range = false;
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 720, 240, ycbcr420, jpeg_comment("CS=ITU601")), want), "");

	// RGB by Adobe's transform 0, or by component IDs R, G and B.
	const jpeg_components full = {{1, 0x11}, {2, 0x11}, {3, 0x11}};
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 720, 240, full, adobe(0)), picture(720, 240, chroma_format::rgb, 8, "Baseline")), "");
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 720, 240, jpeg_components{{'R', 0x11}, {'G', 0x11}, {'B', 0x11}}),
	                  picture(720, 240, chroma_format::rgb, 8, "Baseline")), "");

	want = picture(720, 240, chroma_format::yuv444, 8, "Baseline");
	want.matrix = 5;
	want.full_range = true;
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 720, 240, full, adobe(1)), want), "");
}

TEST("media video JPEG frames refused for what comes before their frame header")
{
	using libfiles::detail::parse_jpeg_frame;

	const auto frame = jpeg_frame(0xC0, 8, 640, 480, ycbcr420);

	auto start = frame;
	start[1] = 0xD9;
	CHECK(refuses(parse_jpeg_frame, start));
	CHECK(refuses(parse_jpeg_frame, {0xFF}));

	// A scan or the end before any frame header.
	CHECK(refuses(parse_jpeg_frame, builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xDA).be16(8).zeros(6).u8(0xFF).u8(0xD9).b));
	CHECK(refuses(parse_jpeg_frame, builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xD9).b));

	// A segment whose length runs past the data, or is too short to be one.
	CHECK(refuses(parse_jpeg_frame, builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xE1).be16(1000).zeros(20).b));
	CHECK(refuses(parse_jpeg_frame, builder().u8(0xFF).u8(0xD8).u8(0xFF).u8(0xE1).be16(1).raw(first(frame, 50)).b));

	CHECK(refuses(parse_jpeg_frame, jpeg_frame(0xC0, 8, 0, 480, ycbcr420)));
	CHECK(refuses(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 0, ycbcr420)));
	CHECK(refuses(parse_jpeg_frame, jpeg_frame(0xC0, 0, 640, 480, ycbcr420)));
	CHECK(refuses(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 480, jpeg_components{})));

	// Fill bytes before a marker, and bytes that are no marker, are passed over.
	auto want = picture(640, 480, chroma_format::yuv420, 8, "Baseline");
	want.matrix = 5;
	want.full_range = true;
	CHECK_STR(read_as(parse_jpeg_frame, jpeg_frame(0xC0, 8, 640, 480, ycbcr420, {0xFF, 0xFF, 0xFF, 0x01, 0x12, 0x34, 0xFF, 0xD0}), want), "");
}

TEST("media video JPEG frames from real files read as FFmpeg reads them")
{
	using libfiles::detail::parse_jpeg_frame;

	// deep16.jpg: lossless 16-bit RGB, by Adobe's marker and its components' IDs.
	CHECK_STR(read_as(parse_jpeg_frame, hex("ffd8ffee000e41646f626500640000000000ffc30011100040004003521100471100421100"),
	                  picture(64, 64, chroma_format::rgb, 16, "Lossless")), "");

	// deep12gray.jpg: 12-bit extended sequential gray.
	CHECK_STR(read_as(parse_jpeg_frame, hex(
		"ffd8ffe000104a46494600010100000100010000ffdb00430001010101010101010101010101010101010101010101010101010101010101"
		"010101010101010101010101010101010101010101010101010101010101010101ffc1000b0c0040004001011100"),
	                  picture(64, 64, chroma_format::gray, 12, "Sequential")), "");

	// Progressive.jpg: JFIF 4:2:0.
	auto want = picture(256, 256, chroma_format::yuv420, 8, "Progressive");
	want.matrix = 5;
	want.full_range = true;
	const auto progressive = hex(
		"ffd8ffe000104a46494600010100000100010000ffdb00430006040506050406060506070706080a100a0a09090a140e0f0c101714181817"
		"1416161a1d251f1a1b231c1616202c20232627292a29191f2d302d283025282928ffdb0043010707070a080a130a0a13281a161a28282828"
		"28282828282828282828282828282828282828282828282828282828282828282828282828282828282828282828ffc20011080100010003"
		"012200021101031101");
	CHECK_STR(read_as(parse_jpeg_frame, progressive, want), "");
	check_cut_short(parse_jpeg_frame, progressive);
}

// ---- Theora --------------------------------------------------------------------------------------------

TEST("media video Theora identification headers give the picture within the frame")
{
	using libfiles::detail::parse_theora_header;

	theora_info t;
	auto want = picture(1920, 1080, chroma_format::yuv420, 8);
	aspect(want, 1, 1);
	colour(want, 5, 1, 5);
	want.frame_rate = 30000.0 / 1001;
	const auto header = theora_header(t);
	CHECK_STR(read_as(parse_theora_header, header, want), "");
	check_cut_short(parse_theora_header, header);
	check_damaged(parse_theora_header, header);

	// Rec. 470M, 4:2:2 and 4:4:4; no shape or rate stated.
	t.colour_space = 1;
	t.pixel_format = 2;
	t.aspect_numerator = 0;
	t.rate_denominator = 0;
	want.chroma = chroma_format::yuv422;
	aspect(want, 0, 0);
	colour(want, 4, 1, 5);
	want.frame_rate = 0;
	CHECK_STR(read_as(parse_theora_header, theora_header(t), want), "");
	t.pixel_format = 3;
	t.colour_space = 0;
	want.chroma = chroma_format::yuv444;
	colour(want, 2, 2, 2);
	CHECK_STR(read_as(parse_theora_header, theora_header(t), want), "");

	// A picture offset within its frame of macroblocks, and anamorphic pixels.
	t = {};
	t.frame_width = 720;
	t.frame_height = 576;
	t.width = 704;
	t.height = 570;
	t.x = 16;
	t.y = 6;
	t.aspect_numerator = 128;
	t.aspect_denominator = 117;
	t.rate_numerator = 25;
	t.rate_denominator = 1;
	want = picture(704, 570, chroma_format::yuv420, 8);
	aspect(want, 128, 117);
	colour(want, 5, 1, 5);
	want.frame_rate = 25;
	CHECK_STR(read_as(parse_theora_header, theora_header(t), want), "");
}

TEST("media video Theora identification headers refused")
{
	using libfiles::detail::parse_theora_header;

	theora_info t;
	t.pixel_format = 1;
	CHECK(refuses(parse_theora_header, theora_header(t)));

	t = {};
	t.major = 4;
	CHECK(refuses(parse_theora_header, theora_header(t)));
	t = {};
	t.minor = 1;
	CHECK(refuses(parse_theora_header, theora_header(t)));

	// A picture larger than its frame, or offset out of it.
	t = {};
	t.width = 1936;
	CHECK(refuses(parse_theora_header, theora_header(t)));
	t = {};
	t.x = 1;
	CHECK(refuses(parse_theora_header, theora_header(t)));
	t.x = 0;
	t.y = 9;
	CHECK(refuses(parse_theora_header, theora_header(t)));
	t = {};
	t.width = 0;
	CHECK(refuses(parse_theora_header, theora_header(t)));

	auto vorbis = theora_header(theora_info{});
	vorbis[0] = 0x01;
	CHECK(refuses(parse_theora_header, vorbis));
}

// ---- DV ------------------------------------------------------------------------------------------------

TEST("media video DV header blocks give the system and its sampling")
{
	using libfiles::detail::parse_dv_frame;

	dv_info d;
	d.blocks = 1;
	auto want = picture(720, 480, chroma_format::yuv411, 8);
	want.frame_rate = 30000.0 / 1001;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	check_cut_short(parse_dv_frame, dv_frame(d));
	check_damaged(parse_dv_frame, dv_frame(d));

	// 625/50: IEC 61834 at 4:2:0, SMPTE 314M at 4:1:1.
	d.fifty = true;
	want = picture(720, 576, chroma_format::yuv420, 8);
	want.frame_rate = 25;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	d.apt = 1;
	want.chroma = chroma_format::yuv411;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
}

TEST("media video DV source packs give the format, the display's shape and the scan")
{
	using libfiles::detail::parse_dv_frame;

	// 25 Mbps 525/60, 4:3 and interlaced.
	dv_info d;
	d.signal_type = 0;
	d.display = 0;
	auto want = picture(720, 480, chroma_format::yuv411, 8);
	aspect(want, 8, 9);
	want.interlaced = true;
	want.frame_rate = 30000.0 / 1001;
	const auto frame = dv_frame(d);
	CHECK_STR(read_as(parse_dv_frame, frame, want), "");
	check_damaged(parse_dv_frame, frame);

	// Cut before the VAUX blocks, a frame reads as its header block alone.
	auto header_only = picture(720, 480, chroma_format::yuv411, 8);
	header_only.frame_rate = 30000.0 / 1001;
	check_cut_short(parse_dv_frame, frame, [&](const video_header& part, const video_header& whole)
	{
		return differences(part, whole).empty() || differences(part, header_only).empty();
	});

	// 625/50, 16:9 by DISP 2, and by 7 in the consumer format alone; progressive by IL.
	d.fifty = true;
	d.display = 2;
	d.interlaced = false;
	want = picture(720, 576, chroma_format::yuv420, 8);
	aspect(want, 64, 45);
	want.frame_rate = 25;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	d.display = 7;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	d.apt = 1;
	want.chroma = chroma_format::yuv411;
	aspect(want, 16, 15);
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");

	// DVCPRO50.
	d = {};
	d.apt = 1;
	d.signal_type = 4;
	want = picture(720, 480, chroma_format::yuv422, 8);
	want.frame_rate = 30000.0 / 1001;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	d.fifty = true;
	want.height = 576;
	want.frame_rate = 25;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");

	// IEC 61883-5's type, for 625/50 alone.
	d = {};
	d.fifty = true;
	d.apt = 1;
	d.signal_type = 1;
	want.chroma = chroma_format::yuv420;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	d.fifty = false;
	CHECK(refuses(parse_dv_frame, dv_frame(d)));
}

TEST("media video DV source packs give DVCPRO HD's sizes")
{
	using libfiles::detail::parse_dv_frame;

	dv_info d;
	d.apt = 1;
	d.signal_type = 0x14;
	d.display = 2;
	d.interlaced = false;

	// 1080i: interlaced by its size, whatever IL says.
	auto want = picture(1280, 1080, chroma_format::yuv422, 8);
	aspect(want, 3, 2);
	want.interlaced = true;
	want.frame_rate = 30000.0 / 1001;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	d.fifty = true;
	want.width = 1440;
	aspect(want, 4, 3);
	want.frame_rate = 25;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");

	// 720p, progressive whatever IL says.
	d.signal_type = 0x18;
	d.interlaced = true;
	want = picture(960, 720, chroma_format::yuv422, 8);
	aspect(want, 4, 3);
	want.frame_rate = 50;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");
	d.fifty = false;
	d.display = 0;
	aspect(want, 1, 1);
	want.frame_rate = 60000.0 / 1001;
	CHECK_STR(read_as(parse_dv_frame, dv_frame(d), want), "");

	d.signal_type = 0x02;
	CHECK(refuses(parse_dv_frame, dv_frame(d)));
}

TEST("media video DV refuses what is not a frame's first block")
{
	using libfiles::detail::parse_dv_frame;

	const auto frame = dv_frame(dv_info{});
	CHECK(refuses(parse_dv_frame, first(frame, 4)));

	// A subcode block, the second DIF sequence, the second block, and the bit after DSF set.
	for (const auto& [at, value] : {std::pair{0, 0x3F}, std::pair{1, 0x17}, std::pair{2, 0x01}, std::pair{3, 0x7F}})
	{
		auto damaged = frame;
		damaged[static_cast<std::size_t>(at)] = static_cast<std::uint8_t>(value);
		CHECK(refuses(parse_dv_frame, damaged));
	}

	// Packs where no VAUX block is are not read.
	auto moved = dv_frame(dv_info{false, 0, 4, 2});
	moved[400] = 0x9F;
	auto want = picture(720, 480, chroma_format::yuv411, 8);
	want.frame_rate = 30000.0 / 1001;
	CHECK_STR(read_as(parse_dv_frame, moved, want), "");
}

// ---- VC-1 ----------------------------------------------------------------------------------------------

TEST("media video VC-1 STRUCT_C names the Simple, Main and Complex profiles")
{
	using libfiles::detail::parse_vc1_sequence;

	// The picture's size is the container's.
	vc1_struct_c c;
	c.profile = 0;
	CHECK_STR(read_as(parse_vc1_sequence, struct_c(c), picture(0, 0, chroma_format::yuv420, 8, "Simple")), "");
	c.profile = 1;
	CHECK_STR(read_as(parse_vc1_sequence, struct_c(c), picture(0, 0, chroma_format::yuv420, 8, "Main")), "");
	check_cut_short(parse_vc1_sequence, struct_c(c));
	check_damaged(parse_vc1_sequence, struct_c(c));
	c.profile = 2;
	CHECK_STR(read_as(parse_vc1_sequence, struct_c(c), picture(0, 0, chroma_format::yuv420, 8, "Complex")), "");

	// Past the Simple profile, FASTUVMC may be clear and EXTENDED_MV set. A clear RES_FASTTX has FFmpeg
	// pass over 16 bits more, which need not be there, and a version byte may follow.
	c.fast_chroma = false;
	c.extended_mv = true;
	c.fast_transform = false;
	auto versioned = struct_c(c);
	versioned.push_back(1);
	CHECK_STR(read_as(parse_vc1_sequence, versioned, picture(0, 0, chroma_format::yuv420, 8, "Complex")), "");
	check_cut_short(parse_vc1_sequence, versioned);
}

TEST("media video VC-1 STRUCT_C refuses what FFmpeg will not decode")
{
	using libfiles::detail::parse_vc1_sequence;

	const auto refused = [](const std::function<void(vc1_struct_c&)>& change)
	{
		vc1_struct_c c;
		change(c);
		return refuses(parse_vc1_sequence, struct_c(c));
	};

	// The old interlaced mode, RES_TRANSTAB, and the Simple profile without FASTUVMC or with EXTENDED_MV.
	CHECK(refused([](vc1_struct_c& c) { c.y411 = true; }));
	CHECK(refused([](vc1_struct_c& c) { c.transtab = true; }));
	CHECK(refused([](vc1_struct_c& c) { c.profile = 0; c.fast_chroma = false; }));
	CHECK(refused([](vc1_struct_c& c) { c.profile = 0; c.extended_mv = true; }));

	// A sprite with the feature FFmpeg lacks, or without a size.
	CHECK(refused([](vc1_struct_c& c) { c.sprite = true; c.sprite_feature = true; }));
	CHECK(refused([](vc1_struct_c& c) { c.sprite = true; c.sprite_width = 0; }));
	CHECK(refused([](vc1_struct_c& c) { c.sprite = true; c.sprite_height = 0; }));

	CHECK(refuses(parse_vc1_sequence, {}));
	CHECK(refuses(parse_vc1_sequence, first(struct_c({}), 3)));
}

TEST("media video VC-1 STRUCT_C with a sprite gives the sprite's size")
{
	using libfiles::detail::parse_vc1_sequence;

	// As FFmpeg's WMV3 decoder reports it, odd or even.
	vc1_struct_c c;
	c.sprite = true;
	CHECK_EQ(struct_c(c).size(), std::size_t{8});
	CHECK_STR(read_as(parse_vc1_sequence, struct_c(c), picture(320, 240, chroma_format::yuv420, 8, "Main")), "");
	check_cut_short(parse_vc1_sequence, struct_c(c));
	check_damaged(parse_vc1_sequence, struct_c(c));

	c.sprite_width = 2047;
	c.sprite_height = 1;
	CHECK_STR(read_as(parse_vc1_sequence, struct_c(c), picture(2047, 1, chroma_format::yuv420, 8, "Main")), "");
}

TEST("media video VC-1 STRUCT_C may hold the Advanced profile's sequence header")
{
	using libfiles::detail::parse_vc1_sequence;

	// FFmpeg's WMV3 decoder reads it without a start code or an entry point: the size is the
	// container's, and the display's shape is measured against the largest coded size.
	vc1_header a;
	a.coded_width = 1440;
	a.interlace = true;
	a.display = true;
	a.rate_nr = 3;
	a.rate_dr = 2;
	auto want = picture(0, 0, chroma_format::yuv420, 8, "Advanced");
	want.interlaced = true;
	aspect(want, 4, 3);
	want.frame_rate = 30000.0 / 1001;
	const auto header = vc1_header_bits(a);
	CHECK_STR(read_as(parse_vc1_sequence, header, want), "");
	check_cut_short(parse_vc1_sequence, header);
	check_damaged(parse_vc1_sequence, header);

	// FFmpeg decodes 4:2:0 alone, and no progressive segmented frames.
	a.chroma = 2;
	CHECK(refuses(parse_vc1_sequence, vc1_header_bits(a)));
	a.chroma = 1;
	a.psf = true;
	CHECK(refuses(parse_vc1_sequence, vc1_header_bits(a)));
}

TEST("media video VC-1 Advanced records give the coded size, scan and profile")
{
	using libfiles::detail::parse_vc1_sequence;

	// After ASF's leading byte, Matroska's zero, or none: the entry point leaves the size at the largest.
	vc1_header a;
	auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Advanced");
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	CHECK_STR(read_as(parse_vc1_sequence, builder().u8(0).raw(vc1_units(a)).b, want), "");
	CHECK_STR(read_as(parse_vc1_sequence, vc1_units(a), want), "");
	check_cut_short(parse_vc1_sequence, vc1_record(a));
	check_cut_short(parse_vc1_sequence, vc1_units(a));

	// Interlaced, and coded at a size of the entry point's own.
	a.pulldown = true;
	a.interlace = true;
	vc1_entry e;
	e.coded_width = 1440;
	e.coded_height = 1080;
	e.extended_mv = true;
	want = picture(1440, 1080, chroma_format::yuv420, 8, "Advanced");
	want.interlaced = true;
	const auto record = vc1_record(a, e);
	CHECK_STR(read_as(parse_vc1_sequence, record, want), "");
	check_cut_short(parse_vc1_sequence, record);
	check_damaged(parse_vc1_sequence, record);

	// FFmpeg finds the first start code wherever it is, passes over units of other kinds, and reads each
	// sequence header and entry point in turn: a later one's fields replace an earlier one's, and those
	// it lacks - here the display extension's pixel shape - stand.
	vc1_header sd;
	sd.coded_width = 720;
	sd.coded_height = 576;
	sd.display = true;
	sd.ratio = 2;
	builder b;
	b.raw("WVC1");
	vc1_unit(b, 0x1F, bytes{0x12, 0x34});
	vc1_unit(b, 0x0F, vc1_header_bits(sd));
	vc1_unit(b, 0x0E, vc1_entry_bits({}));
	vc1_unit(b, 0x0F, vc1_header_bits(a));
	vc1_unit(b, 0x0E, vc1_entry_bits(e));
	b.u8(0).u8(0).u8(1).u8(0x0A);
	aspect(want, 12, 11);
	CHECK_STR(read_as(parse_vc1_sequence, b.b, want), "");
}

TEST("media video VC-1 Advanced display extensions give the pixel shape")
{
	using libfiles::detail::parse_vc1_sequence;

	// The shapes ASPECT_RATIO names, H.264's first thirteen, and one it states at 15.
	constexpr std::uint32_t shapes[14][2] = {
		{0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99},
	};

	vc1_header a;
	a.coded_width = 720;
	a.coded_height = 480;
	a.display = true;
	auto want = picture(720, 480, chroma_format::yuv420, 8, "Advanced");

	for (auto ratio = 1; ratio < 14; ++ratio)
	{
		a.ratio = ratio;
		aspect(want, shapes[ratio][0], shapes[ratio][1]);
		CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	}

	a.ratio = 15;
	a.ratio_width = 200;
	a.ratio_height = 100;
	aspect(want, 2, 1);
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");

	// Without one, or at 0 or 14, the display's shape over the picture's: 1920x1080 shown from 1440x1080.
	a.coded_width = 1440;
	a.coded_height = 1080;
	want = picture(1440, 1080, chroma_format::yuv420, 8, "Advanced");
	aspect(want, 4, 3);

	for (const auto ratio : {-1, 0, 14})
	{
		a.ratio = ratio;
		CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	}

	// The picture is measured as FFmpeg measures the container's: by the entry point's coded size.
	vc1_entry e;
	e.coded_width = 1280;
	e.coded_height = 1080;
	want.width = 1280;
	aspect(want, 3, 2);
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a, e), want), "");

	// One larger than the largest coded size has no shape from the display's.
	a.coded_width = 1280;
	e.coded_width = 1440;
	want.width = 1440;
	aspect(want, 0, 0);
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a, e), want), "");

	// A shape that would leave the picture no pixel wide or high is dropped.
	a = {};
	a.coded_width = 2;
	a.coded_height = 2;
	a.display = true;
	a.ratio = 15;
	a.ratio_width = 1;
	a.ratio_height = 256;
	want = picture(2, 2, chroma_format::yuv420, 8, "Advanced");
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	a.ratio_width = 256;
	a.ratio_height = 1;
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	a.ratio_height = 128;
	aspect(want, 2, 1);
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
}

TEST("media video VC-1 Advanced display extensions give the frame rate and colour")
{
	using libfiles::detail::parse_vc1_sequence;

	// A display as large as the picture: its pixels are square.
	vc1_header a;
	a.display = true;
	auto want = picture(1920, 1080, chroma_format::yuv420, 8, "Advanced");
	aspect(want, 1, 1);

	// FRAMERATENR's rates over FRAMERATEDR's 1000 or 1001, or FRAMERATEEXP's 32nds.
	for (const auto& [nr, dr, rate] : {std::tuple{1, 1u, 24.0}, std::tuple{2, 1u, 25.0}, std::tuple{3, 2u, 30000.0 / 1001},
	                                   std::tuple{4, 1u, 50.0}, std::tuple{5, 2u, 60000.0 / 1001}, std::tuple{6, 1u, 48.0},
	                                   std::tuple{7, 2u, 72000.0 / 1001}})
	{
		a.rate_nr = nr;
		a.rate_dr = dr;
		want.frame_rate = rate;
		CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	}

	a.rate_nr = -1;
	a.rate_exp = 959;
	want.frame_rate = 30;
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	a.rate_exp = 0;
	want.frame_rate = 1.0 / 32;
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");

	// Codes outside FFmpeg's tables state no rate.
	a.rate_exp = -1;
	want.frame_rate = 0;

	for (const auto& [nr, dr] : {std::pair{0, 1u}, std::pair{8, 1u}, std::pair{255, 2u}, std::pair{2, 0u}, std::pair{2, 3u}, std::pair{2, 15u}})
	{
		a.rate_nr = nr;
		a.rate_dr = dr;
		CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	}

	// Colour: only the code points FFmpeg maps, BT.709, BT.470 System B and G, SMPTE 170M and 240M.
	a.rate_nr = -1;
	a.colour = true;

	for (const auto& [stated, kept] : {std::pair{std::array{1u, 1u, 1u}, std::array{1, 1, 1}}, std::pair{std::array{5u, 7u, 6u}, std::array{5, 7, 6}},
	                                   std::pair{std::array{6u, 1u, 7u}, std::array{6, 1, 7}}, std::pair{std::array{4u, 4u, 4u}, std::array{2, 2, 2}},
	                                   std::pair{std::array{9u, 16u, 9u}, std::array{2, 2, 2}}, std::pair{std::array{7u, 6u, 5u}, std::array{2, 2, 2}},
	                                   std::pair{std::array{0u, 0u, 0u}, std::array{2, 2, 2}}, std::pair{std::array{1u, 255u, 6u}, std::array{1, 2, 6}}})
	{
		a.primaries = stated[0];
		a.transfer = stated[1];
		a.matrix = stated[2];
		colour(want, static_cast<std::uint8_t>(kept[0]), static_cast<std::uint8_t>(kept[1]), static_cast<std::uint8_t>(kept[2]));
		CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a), want), "");
	}

	// A later sequence header without a display extension leaves the earlier one's shape and colour.
	vc1_header plain;
	builder b;
	vc1_unit(b, 0x0F, vc1_header_bits(a));
	vc1_unit(b, 0x0F, vc1_header_bits(plain));
	vc1_unit(b, 0x0E, vc1_entry_bits({}));
	CHECK_STR(read_as(parse_vc1_sequence, b.b, want), "");
}

TEST("media video VC-1 Advanced records with leaky buckets and emulation prevention")
{
	using libfiles::detail::parse_vc1_sequence;

	// Each leaky bucket of the sequence header puts a byte in the entry point before its coded size.
	// Fullness of 0, 0 and 0x2A makes 00 00 01 there, which the encoder escapes.
	vc1_header a;
	a.buckets = 3;
	vc1_entry e;
	e.fullness = {0, 0, 0x2A};
	e.coded_width = 1280;
	e.coded_height = 720;
	const auto record = vc1_record(a, e);
	CHECK(contains(record, bytes{0, 0, 3, 1}));
	CHECK_STR(read_as(parse_vc1_sequence, record, picture(1280, 720, chroma_format::yuv420, 8, "Advanced")), "");
	check_cut_short(parse_vc1_sequence, record);
	check_damaged(parse_vc1_sequence, record);

	// As many buckets as HRD_NUM_LEAKY_BUCKETS counts, and none.
	a.buckets = 31;
	e.fullness.assign(31, 0x80);
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a, e), picture(1280, 720, chroma_format::yuv420, 8, "Advanced")), "");
	a.buckets = 0;
	e.fullness.clear();
	CHECK_STR(read_as(parse_vc1_sequence, vc1_record(a, e), picture(1280, 720, chroma_format::yuv420, 8, "Advanced")), "");

	// A display of one pixel puts runs of zeros before the colour: 1x1 shown from 1440x1080 is 3:4.
	vc1_header tiny;
	tiny.coded_width = 1440;
	tiny.display = true;
	tiny.display_width = 1;
	tiny.display_height = 1;
	tiny.colour = true;
	tiny.primaries = 6;
	tiny.transfer = 1;
	tiny.matrix = 6;
	const auto escaped = vc1_record(tiny);
	CHECK(contains(escaped, bytes{0, 0, 3}));
	auto want = picture(1440, 1080, chroma_format::yuv420, 8, "Advanced");
	aspect(want, 3, 4);
	colour(want, 6, 1, 6);
	CHECK_STR(read_as(parse_vc1_sequence, escaped, want), "");
	check_cut_short(parse_vc1_sequence, escaped);
}

TEST("media video VC-1 Advanced records refused as FFmpeg's decoder refuses them")
{
	using libfiles::detail::parse_vc1_sequence;

	// Without an entry point, or without a sequence header.
	vc1_header a;
	builder no_entry;
	no_entry.u8(0);
	vc1_unit(no_entry, 0x0F, vc1_header_bits(a));
	vc1_unit(no_entry, 0x1F, bytes(8, 0x55));
	CHECK(refuses(parse_vc1_sequence, no_entry.b));

	builder no_sequence;
	no_sequence.u8(0);
	vc1_unit(no_sequence, 0x0E, vc1_entry_bits({}));
	vc1_unit(no_sequence, 0x1F, bytes(16, 0x55));
	CHECK(refuses(parse_vc1_sequence, no_sequence.b));

	// Chroma other than 4:2:0, and progressive segmented frames.
	a.chroma = 0;
	CHECK(refuses(parse_vc1_sequence, vc1_record(a)));
	a.chroma = 1;
	a.psf = true;
	CHECK(refuses(parse_vc1_sequence, vc1_record(a)));
	a.psf = false;

	// An entry point cut before its coded size, or within it.
	builder cut;
	vc1_unit(cut, 0x0F, vc1_header_bits(a));
	vc1_unit(cut, 0x0E, bytes{0x5C});
	vc1_unit(cut, 0x1F, bytes(8, 0x55));
	CHECK(refuses(parse_vc1_sequence, cut.b));

	vc1_entry e;
	e.coded_width = 1280;
	e.coded_height = 720;
	builder within;
	vc1_unit(within, 0x0F, vc1_header_bits(a));
	vc1_unit(within, 0x0E, first(vc1_entry_bits(e), 3));
	vc1_unit(within, 0x1F, bytes(8, 0x55));
	CHECK(refuses(parse_vc1_sequence, within.b));

	// FFmpeg reads a sequence header of whichever profile it names. STRUCT_C's bits give no largest
	// size, so the entry point must state one.
	builder older;
	older.u8(0);
	vc1_unit(older, 0x0F, struct_c({}));
	vc1_unit(older, 0x0E, vc1_entry_bits({}));
	CHECK_EQ(older.b.size(), std::size_t{16});
	CHECK(refuses(parse_vc1_sequence, older.b));

	builder sized;
	sized.u8(0);
	vc1_unit(sized, 0x0F, struct_c({}));
	vc1_unit(sized, 0x0E, vc1_entry_bits(e));
	CHECK_STR(read_as(parse_vc1_sequence, sized.b, picture(1280, 720, chroma_format::yuv420, 8, "Main")), "");

	// Fewer than FFmpeg's 16 bytes, opening with a sequence header's start code: not a STRUCT_C.
	CHECK(refuses(parse_vc1_sequence, first(vc1_units(a), 15)));
	CHECK(refuses(parse_vc1_sequence, builder().u8(0x40).u8(0).u8(0).u8(1).u8(0x0F).u8(0xDB).b));
}

TEST("media video VC-1 from a real file reads as FFmpeg reads it")
{
	using libfiles::detail::parse_vc1_sequence;

	// Byzantium.avi's strf: a BITMAPINFOHEADER of 45 bytes for WMV3 at 854x480, padded to 46. FFmpeg
	// takes the 5 after the first 40 as the codec's, and reports Main, 854x480, yuv420p.
	const auto strf = hex("2d00000056030000e001000001001800574d563300000000000000000000000000000000000000004ff100010000");
	CHECK_STR(read_as(parse_vc1_sequence, bytes(strf.begin() + 40, strf.end() - 1), picture(0, 0, chroma_format::yuv420, 8, "Main")), "");

	const auto s = bitmap_stream(strf);
	CHECK_STR(s.codec, "wmv3");
	CHECK_STR(stream_differences(s, picture(854, 480, chroma_format::yuv420, 8, "Main")), "");
}

TEST("media video VC-1 through a BITMAPINFOHEADER: WMV3, WVC1 and WMVA")
{
	// WVC1 as ASF keeps it. Its display's shape, rate and colour fill what the container leaves unknown.
	vc1_header a;
	a.coded_width = 1440;
	a.interlace = true;
	a.display = true;
	a.rate_nr = 3;
	a.rate_dr = 2;
	a.colour = true;
	auto want = picture(1440, 1080, chroma_format::yuv420, 8, "Advanced");
	want.interlaced = true;
	aspect(want, 4, 3);
	want.frame_rate = 30000.0 / 1001;
	colour(want, 1, 1, 1);
	auto s = bitmap_stream(bitmap_info(1440, 1080, "WVC1", vc1_record(a)));
	CHECK_STR(s.codec, "vc1");
	CHECK_STR(stream_differences(s, want), "");

	// WMVA, without the leading byte; the record's size replaces the container's.
	a.display = false;
	want = picture(1440, 1080, chroma_format::yuv420, 8, "Advanced");
	want.interlaced = true;
	s = bitmap_stream(bitmap_info(1440, 1088, "WMVA", vc1_units(a)));
	CHECK_STR(s.codec, "vc1");
	CHECK_STR(stream_differences(s, want), "");

	// WMVP's sprites are not read: FFmpeg names no profile for WMV9 Image, and takes the container's size.
	vc1_struct_c c;
	c.sprite = true;
	s = bitmap_stream(bitmap_info(640, 480, "WMVP", struct_c(c)));
	CHECK_STR(s.codec, "wmv3image");
	CHECK_STR(stream_differences(s, picture(640, 480, chroma_format::yuv420, 8)), "");

	// A record refused leaves the container's description, as does a WMV3 without one; another codec's
	// data is not read as VC-1.
	a.psf = true;
	s = bitmap_stream(bitmap_info(1440, 1080, "WVC1", vc1_record(a)));
	CHECK_STR(stream_differences(s, picture(1440, 1080, chroma_format::yuv420, 8)), "");
	s = bitmap_stream(bitmap_info(854, 480, "WMV3", {}));
	CHECK_STR(stream_differences(s, picture(854, 480, chroma_format::yuv420, 8)), "");
	s = bitmap_stream(bitmap_info(854, 480, "WMV2", struct_c({})));
	CHECK_STR(s.codec, "wmv2");
	CHECK_STR(stream_differences(s, picture(854, 480, chroma_format::yuv420, 8)), "");
}

// ---- Annex B -------------------------------------------------------------------------------------------

TEST("media video Annex B byte streams are split into NAL units")
{
	// Bytes before the first start code, three- and four-byte start codes, zeros at the ends of units,
	// an empty unit, and 00 00 02 inside one.
	const auto stream = builder()
	                    .u8(0xAB).u8(0xCD)
	                    .u8(0).u8(0).u8(0).u8(1).u8(0x67).u8(0x42).u8(0x00).u8(0x1E)
	                    .u8(0).u8(0).u8(1).u8(0x68).u8(0xCE).u8(0x3C).u8(0x80).u8(0).u8(0)
	                    .u8(0).u8(0).u8(1)
	                    .u8(0).u8(0).u8(1).u8(0x65).u8(0x88).u8(0x80).u8(0x00).u8(0x03).u8(0).u8(0).u8(0).u8(0)
	                    .u8(0).u8(0).u8(1).u8(0x41).u8(0x00).u8(0x00).u8(0x02).u8(0x55)
	                    .u8(0).u8(0).u8(1).b;

	const auto units = visited(stream);
	CHECK_EQ(units.size(), std::size_t{4});

	if (units.size() == 4)
	{
		CHECK(units[0] == (bytes{0x67, 0x42, 0x00, 0x1E}));
		CHECK(units[1] == (bytes{0x68, 0xCE, 0x3C, 0x80}));
		CHECK(units[2] == (bytes{0x65, 0x88, 0x80, 0x00, 0x03}));
		CHECK(units[3] == (bytes{0x41, 0x00, 0x00, 0x02, 0x55}));
	}

	// Stopping when the visitor says so.
	CHECK_EQ(visited(stream, 2).size(), std::size_t{2});

	CHECK(visited({}).empty());
	CHECK(visited(bytes(1000, 0)).empty());
	CHECK(visited(bytes{0x67, 0x42, 0x00, 0x1E}).empty());
	CHECK(visited(bytes{0, 0, 1}).empty());
	CHECK(visited(bytes{0, 0, 1, 0x09}).size() == 1);

	// Every prefix splits without reading past its end.
	for (std::size_t n = 0; n <= stream.size(); ++n)
	{
		const auto part = visited(first(stream, n));
		CHECK(part.size() <= 4);
	}
}

TEST("media video Annex B units read as the parameter sets they carry")
{
	h264_sps s;
	s.vui = true;
	s.timing = true;
	s.units = 1;
	s.scale = 50;

	builder stream;
	stream.u8(0).u8(0).u8(0).u8(1).u8(0x09).u8(0xF0);
	stream.u8(0).u8(0).u8(0).u8(1).raw(h264_nal(s));
	stream.u8(0).u8(0).u8(1).u8(0x68).u8(0xEB).u8(0xE3).u8(0xCB);

	video_header h;
	auto found = false;

	libfiles::detail::for_each_annexb_nal(stream.b, [&](const std::span<const std::uint8_t> nal)
	{
		found = libfiles::detail::parse_h264_sps(nal, h);
		return !found;
	});

	CHECK(found);
	auto want = picture(640, 480, chroma_format::yuv420, 8, "High");
	want.frame_rate = 25;
	CHECK_STR(differences(h, want), "");
}

// ---- Every parser -----------------------------------------------------------------------------------

TEST("media video parsers stop within whatever they are handed")
{
	using namespace libfiles::detail;

	constexpr parser parsers[] = {
		parse_h264_sps, parse_avc_config, parse_hevc_sps, parse_hevc_config, parse_av1_sequence, parse_av1_config,
		parse_vp_config, parse_vp9_frame, parse_vp8_frame, parse_mpeg4_video, parse_mpeg12_video, parse_jpeg_frame,
		parse_theora_header, parse_dv_frame, parse_vc1_sequence,
	};

	bytes pattern(1 << 20);
	std::uint32_t seed = 7;

	for (auto& b : pattern)
	{
		seed = seed * 1103515245 + 12345;
		b = static_cast<std::uint8_t>(seed >> 16);
	}

	auto sane = true;

	for (const auto parse : parsers)
	{
		for (const auto& data : {bytes(1 << 20, 0x00), bytes(1 << 20, 0xFF), pattern, bytes{0x67}, bytes{0x42, 0x01}})
		{
			auto h = untouched();
			const auto ok = parse(data, h);
			sane = sane && (ok ? h.width <= 65535 && h.height <= 65535 : differences(h, untouched()).empty());
		}
	}

	CHECK(sane);

	// Start codes and nothing else.
	bytes codes;
	for (auto i = 0; i < 100000; ++i) codes.insert(codes.end(), {0, 0, 1});
	CHECK(visited(codes).empty());
	video_header h;
	CHECK(!parse_mpeg4_video(codes, h));
}
