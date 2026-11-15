// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: What a gain map is found, signalled and described by, shared by the functions that find and attach a
// JPEG's, the metadata carry that leaves one behind, the metadata writer, the HEIF decoder that reads Apple's and the
// gain map arithmetic: the multi-picture index that locates a JPEG's, the metadata that marks a JPEG as one, the
// primary's XMP that says one follows, and Apple's map said in ISO 21496-1's terms.

#pragma once

#include "libfiles/decode.h"
#include "libfiles/xmp.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	inline constexpr std::string_view mpf_identifier{"MPF\0", 4};
	// ISO 21496-1's APP2: a gain map's metadata in the gain map, and in the primary only the two versions
	// that say one follows.
	inline constexpr std::string_view iso_gain_map_identifier{"urn:iso:std:iso:ts:21496:-1\0", 28};
	inline constexpr std::size_t iso_version_bytes = 4;

	// One picture of a multi-picture index: its MP entry, and where the entry lies in the index - the bytes
	// after "MPF\0", from which the offsets of the pictures after the first are measured.
	struct mpf_entry
	{
		std::uint32_t attribute = 0;
		std::uint32_t size = 0;
		std::uint32_t offset = 0;
		std::size_t at = 0;
	};

	struct mpf_index
	{
		bool little_endian = false;
		std::vector<mpf_entry> entries;
	};

	// The index an APP2 MPF segment holds, from the bytes after its identifier. Empty when malformed.
	std::optional<mpf_index> read_mpf_index(std::span<const std::uint8_t> index);
	// Writes the entry's size and offset where it lies in the index.
	void write_mpf_entry(std::span<std::uint8_t> index, const mpf_index& mpf, const mpf_entry& entry);
	// An APP2 payload, identifier and all, indexing primary_size bytes of primary - this segment among them
	// - and the gain_map_size bytes that follow it. index_at is where the payload's index lies in the file:
	// four bytes past the payload's start.
	std::vector<std::uint8_t> make_mpf_payload(std::uint64_t primary_size, std::uint64_t gain_map_size,
	                                           std::uint64_t index_at);

	// Which schemes a JPEG's own metadata follows as a gain map: Adobe's hdrgm XMP, which Ultra HDR shares;
	// ISO 21496-1's APP2; Apple's auxiliary-image XMP. Read from the segments before its first scan, as many
	// of them as header holds.
	struct gain_map_schemes
	{
		bool adobe = false;
		bool iso = false;
		bool apple = false;

		bool any() const noexcept { return adobe || iso || apple; }
	};

	gain_map_schemes read_gain_map_schemes(std::span<const std::uint8_t> header);

	// True for the APP2 payload of a primary's ISO 21496-1 versions, which say a gain map follows.
	bool is_iso_version_segment(std::span<const std::uint8_t> payload) noexcept;

	// Just past the EOI of the JPEG data starts with: where a picture after it would begin. Zero when data
	// does not start with a JPEG, or the JPEG is cut short.
	std::size_t jpeg_end(std::span<const std::uint8_t> data);

	// True when packet may hold gain map signalling, without parsing it.
	bool may_signal_gain_map(std::string_view packet) noexcept;

	// True when a primary's own XMP says an Adobe gain map follows it - the hdrgm namespace Ultra HDR declares the
	// gain map's version in - without parsing it. A container directory alone says no such thing: motion photos
	// list theirs there too.
	bool signals_adobe_gain_map(std::string_view packet) noexcept;

	// A primary's XMP made to say what follows it. With a gain map: its length in every GainMap item of the
	// container directory, and hdrgm:Version when the gain map's metadata is Adobe's. With none: neither,
	// the GainMap items removed, and the directory with them when nothing but the primary is left in it. A
	// gain map's own XMP - which states hdrgm:GainMapMax - is left as it is. True when meta changed.
	struct gain_map_signal
	{
		std::uint64_t length = 0;
		bool adobe = false;
	};

	bool signal_gain_map(xmp::meta& primary, const std::optional<gain_map_signal>& gain_map);

	// An APP1 XMP payload, identifier and all, whose packet signal_gain_map has made say what follows the
	// primary, as compact as it must be to fit the segment. Empty when the packet already says it, or cannot
	// be read or made to fit. An empty payload is a primary with no XMP: one is made only when an Adobe gain
	// map follows.
	std::vector<std::uint8_t> signalled_xmp(std::span<const std::uint8_t> payload,
	                                        const std::optional<gain_map_signal>& gain_map);

	// What Apple's HDR gain map is said as in ISO 21496-1's terms, once convert_apple_gain_map has rewritten its
	// values: an SDR base, gains from none to the headroom's, coded linearly, with no offsets.
	gain_map_metadata apple_gain_map_metadata(double headroom);
}
