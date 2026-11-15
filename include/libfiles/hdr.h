// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: What an application working on an HDR photo needs beside decode() and the encoders: an HDR picture's
// signal shown as SDR, as decode() shows it; an edit made to that SDR rendition carried into the signal; a gain map
// made from the two; ISO 21496-1's gain map metadata read and written; and Apple's HDR gain map said in ISO 21496-1's
// terms.

#pragma once

#include "libfiles/decode.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libfiles
{
	// HDR signal - rgbx16 or rgba16, as decode() gives it with keep_hdr - mapped to SDR as decode() maps an HDR
	// still, into out in format: bgrx8 or bgra8, or rgbx16 or rgba16 for sixteen bits. Alpha goes where both have it,
	// and an opaque format's fourth channel is full. False, with nothing written, for other formats.
	bool map_hdr_to_sdr(const image_view& signal, const hdr_description& hdr, pixel_format format,
	                    const pixel_buffer& out);

	// An edit made to an HDR picture's SDR rendition, carried into its signal: each pixel's light scaled, channel by
	// channel in the signal's own primaries, by how far the edit moved the rendition's light - with ISO 21496-1's
	// offset of 1/64 of SDR white on each side, so near black the edit is added rather than multiplied - as a gain map
	// scales whatever colour an edit gives the picture under it. A pixel the edit left as it was comes back as it
	// was, and the picture keeps its light above SDR white in proportion. signal is rgbx16 or rgba16, and out takes
	// the same, its alpha the signal's; before - what map_hdr_to_sdr gave - and after are bgrx8, bgra8, rgbx16 or
	// rgba16, both of the signal's size. False, with nothing written, when they do not fit.
	bool carry_sdr_edit(const image_view& signal, const hdr_description& hdr, const image_view& before,
	                    const image_view& after, const pixel_buffer& out);

	// ISO 21496-1's GainMapMetadata: what follows a JPEG APP2's identifier, and a HEIF tone map item's version. Each
	// value is written as a fraction over 10^6 - a headroom or an offset over 10^7 where it fits - and the channels
	// as one where all three agree. Empty when a value cannot be written: a headroom below 0, a gamma not above 0, or
	// a magnitude past what 32 bits hold.
	std::vector<std::uint8_t> write_gain_map_metadata(const gain_map_metadata& metadata);
	// Nothing when the data is cut short, its minimum version is not 0, which is all there is, or a denominator is 0.
	std::optional<gain_map_metadata> read_gain_map_metadata(std::span<const std::uint8_t> data);

	// A gain map made for an HDR picture and the SDR rendition it is to be the gain map of: one grey value a block of
	// scale by scale pixels, the block's light ratio in log2 - its luminance in BT.709, offset by 1/64 as ISO 21496-1
	// offsets it - spread over the map's range, which runs from the smallest gain to the largest. bgrx8, its value in
	// every channel. The metadata applies it to the SDR rendition, whose headroom is 0, to make the HDR picture, whose
	// headroom is its brightest pixel's.
	struct made_gain_map
	{
		std::vector<std::uint8_t> pixels;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		gain_map_metadata metadata;

		image_view view() const noexcept
		{
			return {pixels.data(), std::size_t{4} * width, width, height, pixel_format::bgrx8, false};
		}
	};

	// signal is rgbx16 or rgba16 and base bgrx8, bgra8, rgbx16 or rgba16, of one size. Empty pixels when they do not
	// fit, or scale is 0.
	made_gain_map make_gain_map(const image_view& signal, const hdr_description& hdr, const image_view& base,
	                            std::uint32_t scale = 2);

	// Apple's HDR gain map - grey values coded by BT.709's transfer, each multiplying the photo's linear light by
	// 1 + (headroom - 1) * value - rewritten in place, bgrx8 rows stride apart, as the values ISO 21496-1's metadata,
	// returned, applies to the same effect: log2 of that factor over log2 of the headroom. A headroom of 1 or less
	// brightens nothing, and leaves the map 0.
	gain_map_metadata convert_apple_gain_map(const pixel_buffer& pixels, std::uint32_t width, std::uint32_t height,
	                                         double headroom);

	// The headroom - peak over SDR white - Apple's metadata gives its HDR gain map: the gain map's XMP's
	// HDRGainMap:HDRGainMapHeadroom, else the photo's EXIF maker note's tags 33 and 48 by Apple's formula. Nothing
	// when neither says, or what they say is not a number above 1.
	std::optional<double> apple_gain_map_headroom(std::span<const std::uint8_t> gain_map_xmp,
	                                              std::span<const std::uint8_t> exif);
}
