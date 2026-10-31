// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What a file is. One table of every format libfiles knows - its family, name, MIME type and
// extensions, and what libfiles can do with it in this build - and the one identification that scan_media,
// read_packet and plan_update dispatch through, so a scan, a packet read and an update never disagree
// about what a file is. The older per-area enums - image_format, media_format and file_format - are
// defined here too, each a view of the table.

#pragma once

#include "libfiles/reader.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace libfiles
{
	// Every format libfiles reads, writes or decodes, by what its bytes say it is.
	enum class file_type : std::uint8_t
	{
		unknown,
		// Still images.
		jpeg,
		png,
		webp,
		gif,
		psd,
		bmp,
		tiff,
		// HEIC, AVIF and the other ISO base media still images.
		heif,
		jxl,
		pcx,
		wpg,
		// Audio and video.
		// ISO base media and QuickTime: MP4, MOV, M4A, M4V, 3GP and 3G2.
		mp4,
		// Matroska and WebM.
		matroska,
		avi,
		// RIFF WAVE, and RF64 and BW64 for files past 4 GiB.
		wav,
		// AIFF and AIFF-C.
		aiff,
		// Advanced Systems Format: WMV, WMA and ASF.
		asf,
		// MPEG audio - MP3, MP2 and MP1 - with its ID3v2, ID3v1 and APE tags.
		mpeg_audio,
		flac,
		// Ogg: Vorbis, Opus, Speex, FLAC and Theora.
		ogg,
		flv,
		// MPEG transport streams: TS, and the 192-byte packets of M2TS and MTS.
		mpeg_ts,
		// MPEG program streams: MPG, VOB, MOD and TOD.
		mpeg_ps,
	};

	enum class file_family : std::uint8_t
	{
		unknown,
		// A picture: scan() and the decoders read it.
		image,
		// Audio or video: scan_media() reads it.
		media,
	};

	// What libfiles can do with a format, in this build. Flags.
	enum class capability : std::uint16_t
	{
		none = 0,
		// scan(): a still image's size, pixel format and metadata.
		scan = 1u << 0,
		// scan_media(): streams, duration, tags and the XMP packet of audio and video.
		scan_media = 1u << 1,
		// decode(): the image into the caller's memory.
		decode = 1u << 2,
		// decode_document(): native storage, palettes, frames and pages.
		decode_document = 1u << 3,
		// read_packet(): the XMP packet, found where scan and plan_update find it.
		read_packet = 1u << 4,
		// plan_update(): metadata written back into the file.
		update = 1u << 5,
		// encode_jpeg, encode_png or encode_webp.
		encode = 1u << 6,
	};

	constexpr capability operator|(const capability a, const capability b) noexcept
	{
		return static_cast<capability>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
	}

	constexpr capability operator&(const capability a, const capability b) noexcept
	{
		return static_cast<capability>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
	}

	// The still image formats scan(), decode() and decode_document() name.
	enum class image_format : std::uint8_t
	{
		unknown,
		jpeg,
		png,
		webp,
		gif,
		psd,
		bmp,
		tiff,
		heif,
		jxl,
		pcx,
		wpg,
	};

	// The containers scan_media() names.
	enum class media_format : std::uint8_t
	{
		unknown,
		// ISO base media and QuickTime: MP4, MOV, M4A, M4V, 3GP and 3G2.
		mp4,
		// Matroska and WebM.
		matroska,
		avi,
		// RIFF WAVE, and RF64 and BW64 for files past 4 GiB.
		wav,
		// AIFF and AIFF-C.
		aiff,
		// Advanced Systems Format: WMV, WMA and ASF.
		asf,
		// MPEG audio - MP3, MP2 and MP1 - with its ID3v2, ID3v1 and APE tags.
		mpeg_audio,
		flac,
		// Ogg: Vorbis, Opus, Speex, FLAC and Theora.
		ogg,
		flv,
		// MPEG transport streams: TS, and the 192-byte packets of M2TS and MTS.
		mpeg_ts,
		// MPEG program streams: MPG, VOB, MOD and TOD.
		mpeg_ps,
	};

	// The formats plan_update() writes, as its writers group them.
	enum class file_format : std::uint8_t
	{
		unknown,
		jpeg,
		png,
		gif,
		webp,
		tiff,
		psd,
		// ISO base media - MP4, M4A, M4V, 3GP - and QuickTime.
		mp4,
		// ASF: WMV, WMA and ASF.
		asf,
		// RIFF: WAV and AVI.
		riff,
		// MPEG audio, MP3 and MP2, with or without an ID3v2 tag.
		mp3,
		flv,
	};

	struct file_type_info
	{
		file_type type = file_type::unknown;
		file_family family = file_family::unknown;
		// For a person: "JPEG", "MP4 and QuickTime".
		std::string_view name;
		// The registered type, "image/jpeg", or the one in common use where none is registered.
		std::string_view mime_type;
		// Lower case, with the dot - ".jpg", ".jpeg" - the usual one first. Empty for unknown.
		std::span<const std::string_view> extensions;
		// The format as each area's enum names it; unknown where that area does not read it.
		image_format image = image_format::unknown;
		media_format media = media_format::unknown;
		file_format update_format = file_format::unknown;
		capability capabilities = capability::none;

		constexpr bool can(const capability c) const noexcept
		{
			return c != capability::none && (capabilities & c) == c;
		}
	};

	// Every format libfiles knows, in file_type order, unknown excluded.
	std::span<const file_type_info> file_types() noexcept;

	// The table's row for a type; unknown's for unknown.
	const file_type_info& info(file_type type) noexcept;

	// The type an extension names, with or without its dot, whatever its case; unknown for one no format
	// claims. For a caller that has a name but no bytes - a file's bytes say what it is.
	file_type file_type_from_extension(std::string_view extension) noexcept;

	// The bytes identify needs to tell every format apart: an MPEG transport stream is known only by sync
	// bytes repeating every 188 or 192 bytes. Sixteen tell every other format.
	inline constexpr std::size_t identify_bytes = 576;

	// What a file's first bytes say it is. Signatures that cannot be mistaken come first, then audio and
	// video, then BMP's two bytes, WordPerfect graphics and PCX. A leading ID3v2 tag reads as MPEG audio:
	// the reader overload looks past it.
	file_type identify(std::span<const std::uint8_t> head) noexcept;

	// What a file is, from its first identify_bytes and, after a leading ID3v2 tag, what follows it: a FLAC
	// stream that some taggers put behind one is FLAC. Never throws for a malformed file; unknown when the
	// stream cannot be read.
	file_type identify(reader& r);
}
