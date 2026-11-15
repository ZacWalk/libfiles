// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Reading an ICC colour profile, for a person and for pixels. Listing it for what it says: a summary a
// person reads - the profile's name, its colour space and primaries, its tone response, white point and rendering
// intent - then its header whole, and every tag in its directory with its type and size, a tag it cannot read
// named as such with the reason. And mapping the pixels an RGB or grey display profile describes to sRGB, and an
// edit made to that sRGB rendition back into them.

#pragma once

#include "libfiles/decode.h"
#include "libfiles/types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libfiles
{
	// Reads only the data-colour-space signature in the header, not profile validity.
	bool icc_declares_rgb(std::span<const std::uint8_t> profile) noexcept;

	// What a profile says it is, as a person asks: the summary list_icc opens with, field by field, in the
	// listing's own words. Text is UTF-8; what the profile does not state is empty.
	struct icc_summary
	{
		// The profile's name, its 'desc' tag: "sRGB IEC61966-2.1", "Display P3".
		std::string name;
		std::string device_model;
		std::string device_manufacturer;
		std::string copyright;
		// The data colour space, by its signature and what it means.
		std::string colour_space;
		// The gamut the colorants match - "approximately sRGB", "approximately Display P3" - or "custom";
		// empty for a profile without colorants.
		std::string primaries;
		std::string tone_response;
		std::string white_point;
		std::string rendering_intent;
	};

	// Nothing for a block that is not a profile, as list_icc lists nothing for one. Never throws for a
	// malformed profile.
	std::optional<icc_summary> summarize_icc(std::span<const std::uint8_t> profile);
	// A profile as an inspector lists it: the containers "Summary" (id "icc.summary"), "Header"
	// ("icc.header"), "Tags (n)" ("icc.tags") and, when any tag could not be read, "Unread tags (n)"
	// ("icc.unread"), each followed by its rows at depth 1. A tag's row is keyed by its signature, has
	// the id "icc.tag." and the signature, and holds the tag's first 4,096 bytes as its detail. Nothing
	// for a block that is not a profile - one shorter than its 128-byte header, without the 'acsp'
	// signature, declaring a size it does not have, or more than 1,000 tags. Never throws for a
	// malformed profile.
	std::vector<structure_row> list_icc(std::span<const std::uint8_t> profile);

	// What map_icc_to_srgb makes of the colours a profile describes.
	enum class srgb_mapping : std::uint8_t
	{
		// Nothing: a profile of lookup tables alone, a CMYK, Lab or named-colour one, or a block that is no profile.
		unmapped,
		// Nothing, as there is nothing to do: sRGB's own colours, or so near them that mapping would move none as
		// much as half a level.
		srgb,
		// sRGB's: the colours of an RGB profile of colorants and tone curves - Display P3's, Adobe RGB (1998)'s,
		// ProPhoto RGB's, as display, camera and working-space profiles are - or of a grey one of another tone curve.
		mapped,
	};

	srgb_mapping icc_srgb_mapping(std::span<const std::uint8_t> profile);

	// True when pixels this profile describes show other than sRGB's would, and libfiles can map them to sRGB:
	// icc_srgb_mapping is srgb_mapping::mapped.
	bool icc_maps_to_srgb(std::span<const std::uint8_t> profile);

	// Pixels in the colours a profile icc_maps_to_srgb takes describes, mapped to sRGB, white to white - the
	// colours decode_options::map_to_srgb shows: each made linear by its own tone curve, taken into sRGB's
	// primaries, and one sRGB cannot show clipped channel by channel, as colour-managed viewers show such a profile's
	// colours (relative colorimetric), before sRGB's curve encodes it. pixels and out are bgrx8, bgra8, rgbx16 or
	// rgba16, the same format and size, and may be the same memory; the fourth channel is left as it was. False, with
	// nothing written, where icc_maps_to_srgb is false or the format is another.
	bool map_icc_to_srgb(const image_view& pixels, std::span<const std::uint8_t> profile, const pixel_buffer& out);

	// An edit made to pixels' sRGB rendition - what map_icc_to_srgb gave - carried back into pixels, in the
	// colours the profile describes, as carry_sdr_edit carries one into an HDR signal: each pixel's light scaled,
	// channel by channel in the profile's own primaries, by how far the edit moved the rendition's, with ISO
	// 21496-1's offset of 1/64 of white on each side, so near black the edit is added rather than multiplied. A
	// pixel the edit left as it was comes back exactly as it was, and a colour outside sRGB keeps what of its gamut
	// the edit does not take. pixels and out are bgrx8, bgra8, rgbx16 or rgba16, the same format and size, and may
	// be the same memory; before and after are any of those four, of pixels' size. False, with nothing written,
	// where icc_maps_to_srgb is false or the images do not fit.
	bool carry_srgb_edit(const image_view& pixels, std::span<const std::uint8_t> profile, const image_view& before,
	                     const image_view& after, const pixel_buffer& out);
}
