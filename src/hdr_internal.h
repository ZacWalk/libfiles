// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: What the still decoders, the HDR conversions and the gain map arithmetic share for a picture of more than
// a byte a channel: rows of 16-bit RGBA written into the sink's format, rounded once; the mapping that shows a PQ or
// HLG still - an HDR photo from a phone or a camera - as the SDR picture an SDR display can show, by the rules
// Diffractor's HDR video follows; an HDR signal's light, relative to SDR white, and back; and the words the
// scanners' structure listings describe a picture's colour, light levels and gain map in.

#pragma once

#include "libfiles/decode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libfiles::detail
{
	// What an SDR picture shows as white, in the light PQ and HLG describe.
	inline constexpr double sdr_white_nits = 203.0;

	// The HDR transfer an H.273 transfer-characteristics code point names: PQ (16) or HLG (18).
	std::optional<hdr_transfer> hdr_transfer_of(int transfer_characteristics);
	// The H.273 transfer-characteristics code point of an HDR transfer.
	int code_point_of(hdr_transfer transfer);

	// H.273's code points as the structure listings name them - "BT.2020 primaries", "PQ transfer (HDR)",
	// "BT.2020 non-constant luminance matrix" - and a code point they do not name by its number, so a listing says
	// what a file holds even where it is unusual.
	std::string colour_primaries_text(int code);
	std::string transfer_text(int code);
	std::string matrix_text(int code);
	// Light in nits as the listings give it: whole nits from 10, three significant figures below.
	std::string nits_text(double nits);
	// A content light level as the listings give it: the brightest pixel's light and the brightest frame's average,
	// either left out when unknown (0).
	std::string light_level_text(double max_cll, double max_fall);
	// A gain map's reach as the listings give it, from its alternate headroom, log2 of its peak over SDR white: "up
	// to 7.37 times SDR white (2.88 stops)".
	std::string headroom_text(double log2_headroom);

	// Linear RGB in these primaries to linear BT.709, adapted to D65 where the white differs - DCI-P3's - row major.
	std::array<double, 9> to_bt709(const chromaticities& primaries);
	std::array<double, 9> invert(const std::array<double, 9>& m);

	// sRGB's encoding of linear light, 0 to 1, and its decoding.
	double srgb_encode(double linear);
	double srgb_decode(double encoded);

	// An HDR signal's light, relative to SDR white: PQ's absolute light over 203 nits, HLG's scene light, whose peak
	// libfiles shows as white. Read from a table for each 16-bit signal; back by the transfer itself.
	class hdr_light
	{
	public:
		explicit hdr_light(hdr_transfer transfer);

		float light(const std::uint16_t signal) const noexcept { return _light[signal]; }
		// The signal, 0 to 1, of light relative to SDR white, clamped to what the transfer codes: 10000 nits for PQ,
		// the peak for HLG.
		double signal(double light) const noexcept;
		// The most light the transfer codes, relative to SDR white.
		double peak() const noexcept;

	private:
		bool _hlg = false;
		std::vector<float> _light;
	};

	// R'G'B' signal mapped to SDR R'G'B' for an sRGB display. PQ is absolute light: rolled off on luminance
	// by BT.2390's EETF from the picture's peak into SDR white, 203 nits, where BT.2408 puts HDR reference
	// white, leaving midtones untouched. HLG is relative: rendered by BT.2100's HLG EOTF for a display whose
	// peak is SDR white, at a system gamma of 1.0, so the signal's peak lands on white. The wide gamut is taken
	// into BT.709 colorimetrically, and a colour that falls outside it - past white or below black - is
	// desaturated towards its luminance until it fits, which keeps its hue. Evaluated for every pixel, its
	// transfers read from tables: a cube read trilinearly cannot follow the fit's turn towards white, and was
	// tens of levels out on bright saturated colours.
	class hdr_mapping
	{
	public:
		explicit hdr_mapping(const hdr_description& source);

		// One pixel of signal, each channel 0 to 1, to SDR, each channel 0 to 1.
		std::array<float, 3> map(float r, float g, float b) const noexcept;

		// The mapping without its tables, for a test to hold them to.
		static std::array<double, 3> evaluate(const hdr_description& source, double r, double g, double b);

	private:
		bool _hlg = false;
		std::array<float, 9> _to_709{};
		// Light in nits for each 16-bit signal.
		std::vector<float> _light;
		// PQ's luminance gain, the EETF's output over its input, by the fourth root of the luminance's share of
		// 10000 nits.
		std::vector<float> _gain;
		// sRGB's encoding by the square root of linear light.
		std::vector<float> _encode;
	};

	// Rows of 16-bit RGBA - red first, each channel 0 to max_value, rows source_stride bytes apart - written to
	// the sink's pixels in format: straight alpha where premultiplied, mapped to SDR where there is a mapping,
	// and rounded once to the format's precision. The fourth channel of an opaque format is written full.
	void write_wide_rows(const std::uint8_t* source, std::size_t source_stride, std::uint32_t max_value,
	                     std::uint32_t width, std::uint32_t height, bool premultiplied, const hdr_mapping* mapping,
	                     pixel_format format, const pixel_buffer& pixels);

	// The format a decoder of a picture this deep writes for these options: 16 bits a channel where the
	// picture has more than eight and the caller takes them, or where it is HDR and the caller keeps its
	// signal; else a byte. Alpha where the picture has it.
	pixel_format output_format(int bits_per_channel, bool has_alpha, const decode_options& options, bool hdr = false);
}
