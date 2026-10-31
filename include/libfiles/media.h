// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Reading what an audio or video file says about itself without decoding it: its container,
// its streams and what each carries, how long it plays, the tags an organizer files it by, its XMP
// packet and where its cover art lies. This is the indexing path for media, so a scan reads headers
// and indexes - never media it does not need - and locates cover art rather than reading it.

#pragma once

#include "libfiles/identify.h"
#include "libfiles/properties.h"
#include "libfiles/reader.h"
#include "libfiles/types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles
{
	// The bytes detect_media_format needs to tell every format apart: a transport stream is known only
	// by sync bytes repeating every 188 or 192 bytes.
	inline constexpr std::size_t media_detect_bytes = identify_bytes;

	// Names a media container from a file's first bytes. Sixteen tell every format but MPEG-TS, which
	// needs media_detect_bytes. MPEG audio without a leading tag is known by a frame header, which
	// scan_media checks against the frame that follows it.
	media_format detect_media_format(std::span<const std::uint8_t> head) noexcept;

	enum class media_stream_kind : std::uint8_t
	{
		video,
		audio,
		subtitle,
		// Timecode, chapters, GPS tracks and other timed data.
		data,
	};

	struct video_format
	{
		// The picture as decoded and before it is turned: the codec header's size, cropped, where the
		// scan read one, else the container's.
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		// The shape of a pixel, width to height. Zero when the file does not say, which means square.
		std::uint32_t pixel_aspect_width = 0;
		std::uint32_t pixel_aspect_height = 0;
		// The clockwise turn a player applies, from the container: 0, 90, 180 or 270; and whether the
		// picture is mirrored before it is turned.
		int rotation = 0;
		bool mirrored = false;
		// Frames a second, averaged where the container times every frame. Zero when unknown.
		double frame_rate = 0.0;
		chroma_format chroma = chroma_format::unknown;
		// Bits a sample, zero when unknown.
		std::uint8_t bit_depth = 0;
		bool alpha = false;
		bool interlaced = false;
		bool full_range = false;
		// ITU-T H.273 code points; 2, unspecified, when the stream states none.
		std::uint8_t color_primaries = 2;
		std::uint8_t transfer = 2;
		std::uint8_t matrix = 2;
		// The profile as the codec's specification names it: "High", "Main 10", "Profile 0".
		std::string profile;

		// The chroma, depth and alpha as a still image's picture format states them.
		picture_format picture() const noexcept { return {chroma, bit_depth, alpha}; }

		// The chroma as libfiles names a still image's - "yuv420", "yuv422", "gray", "rgb", "rgba" -
		// followed by the bit depth when it is more than 8: "yuv420 10bit". Empty when unknown. The
		// same as picture().name().
		std::string pixel_format() const;

		// The turn and mirror as the EXIF orientation a still image states, 1 to 8.
		std::uint16_t orientation() const noexcept { return orientation_from_turn(rotation, mirrored); }

		// "PQ" or "HLG" when the stream is tagged with an HDR transfer, else empty.
		std::string_view hdr_transfer() const noexcept;
	};

	struct audio_format
	{
		std::uint32_t sample_rate = 0;
		std::uint32_t channels = 0;
		// The samples as coded, for PCM and lossless codecs: bits each, and whether floating point.
		// Zero for a lossy codec, whose samples have no depth until a decoder gives them one.
		std::uint8_t bits_per_sample = 0;
		bool float_samples = false;
		// The profile, where the codec has one worth naming: "LC", "HE-AAC", "HE-AACv2".
		std::string profile;
	};

	// A tag as the file stores it, its value as text.
	struct media_tag
	{
		// The container's own name: "©nam", "TIT2", "INAM", "WM/AlbumTitle", "TITLE".
		std::string key;
		// UTF-8. A number is written in decimal; a binary value is described rather than reproduced.
		std::string value;
		// ISO 639-2, where the tag names a language.
		std::string language;
	};

	struct media_stream
	{
		media_stream_kind kind = media_stream_kind::data;
		// The container's own number for the stream - an MP4 track ID, a Matroska track number, an AVI
		// or ASF stream number, an MPEG-TS PID, an MPEG-PS stream id - by which a player finds it among
		// its demuxer's streams.
		std::uint64_t id = 0;
		// A short name: "h264", "hevc", "av1", "vp9", "mpeg4", "mpeg2", "mjpeg", "prores", "dv", "aac",
		// "mp3", "ac3", "opus", "flac", "pcm". Empty when libfiles does not know the codec.
		std::string codec;
		// The codec as the container identifies it: "avc1", "XVID", "V_MPEG4/ISO/AVC", "0x0161".
		std::string codec_tag;
		// ISO 639-2, or a BCP 47 tag where the container keeps one.
		std::string language;
		std::string title;
		// Seconds, zero when unknown.
		double duration = 0.0;
		// Bits a second as the container states it, zero when it does not.
		std::uint64_t bit_rate = 0;
		bool is_default = false;
		bool commentary = false;
		bool audio_description = false;
		// The one of the two that matches the kind.
		video_format video;
		audio_format audio;
		std::vector<media_tag> tags;
	};

	// A date as a tag states it: the date every area of libfiles shares (types.h).
	using media_date = date;

	// What an organizer files a recording by: the properties every file has (properties.h), as the
	// container's own tags give them.
	using media_properties = properties;

	// Cover art and the other pictures a recording's tags hold.
	using media_picture = embedded_image;

	struct media_scan_result
	{
		// What the file's signature says it is, whether or not the rest could be read.
		media_format format = media_format::unknown;
		bool success = false;
		// True when every audio and video stream is fully described - its codec known, a video stream's
		// size, an audio stream's rate and channels - and so is the duration. A caller that needs all of
		// these can turn to a decoder's probe when it is false.
		bool complete = false;
		// The variant the container names: an ISO file's major brand ("isom", "qt  ", "M4A "), a Matroska
		// DocType ("webm"), the MPEG audio layer ("MPEG-1 Layer 3").
		std::string brand;
		// Seconds: the container's figure, else the longest stream's. Zero when unknown.
		double duration = 0.0;
		// Bits a second over the whole file: its size over its duration. Zero when the duration is unknown.
		std::uint64_t bit_rate = 0;
		std::vector<media_stream> streams;
		// The container's own tags, in file order, under the names it gives them.
		std::vector<media_tag> tags;
		media_properties properties;
		// The XMP packet, found where read_packet (write.h) finds it.
		std::vector<std::uint8_t> xmp;
		std::vector<media_picture> pictures;
		// Only for scan_intent::inspect.
		std::vector<structure_row> structure;
		// Why the scan stopped, when the file was malformed or unreadable. Then nothing but format is set.
		std::string error;
	};

	// Never throws for a malformed file; error says what went wrong. A file shorter than sixteen bytes
	// is not scanned. The file is identified as identify(reader&) identifies it, and read within limits,
	// the scan_limits scan() takes.
	media_scan_result scan_media(reader& r, scan_intent intent = scan_intent::index, const scan_limits& limits = {});
}
