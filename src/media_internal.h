// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What the media scanners share and do not export: a bit reader for codec headers; the video
// and audio codec headers several containers carry; the Windows structures AVI, ASF, WAV and Matroska
// hold; the tags several containers share - ID3, APE and Vorbis comments - and the mapping of every
// container's tag names onto media_properties; dates and positions as tags write them; and each
// container's scanner.

#pragma once

#include "libfiles/media.h"
#include "libfiles/types.h"
#include "date_internal.h"
#include "scan_internal.h"
#include "text_internal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	// ---- Budget --------------------------------------------------------------------------------------

	// What one media scan may hold beyond the bytes it reads from the file, shared by every reader the scan
	// calls: what it inflates of compressed tags and codec headers, scan_limits::media_inflated_bytes in all.
	class media_budget
	{
	public:
		explicit media_budget(const std::size_t inflated) noexcept : _left(inflated)
		{
		}

		// The most the next inflation may produce: what is left, and never more than one element may hold.
		std::size_t inflate_limit(const std::size_t element_limit) const noexcept { return std::min(_left, element_limit); }

		// Counts what an inflation produced against what is left.
		void inflated(const std::size_t bytes) noexcept { _left -= std::min(_left, bytes); }

	private:
		std::size_t _left;
	};

	// ---- Bits ----------------------------------------------------------------------------------------

	// Bits read most significant first, as video codec headers hold them. Reading past the end yields
	// zeros and sets overrun, so a parser reads a whole header and checks once at the end.
	class bit_reader
	{
	public:
		explicit bit_reader(const std::span<const std::uint8_t> data) noexcept : _data(data)
		{
		}

		// Up to 32 bits.
		std::uint32_t u(const unsigned count) noexcept
		{
			std::uint32_t v = 0;

			for (unsigned i = 0; i < count; ++i)
			{
				v = v << 1 | bit();
			}

			return v;
		}

		bool flag() noexcept { return bit() != 0; }

		// Exp-Golomb, as H.264 and HEVC write it. A code longer than 32 bits is damage: it reads as
		// overrun.
		std::uint32_t ue() noexcept
		{
			unsigned zeros = 0;

			while (bit() == 0)
			{
				if (_overrun || ++zeros > 31)
				{
					_overrun = true;
					return 0;
				}
			}

			if (zeros == 0) return 0;
			return static_cast<std::uint32_t>((1ull << zeros) - 1 + u(zeros));
		}

		std::int32_t se() noexcept
		{
			const auto k = ue();
			return k & 1 ? static_cast<std::int32_t>((k + 1) / 2) : -static_cast<std::int32_t>(k / 2);
		}

		void skip(const std::uint64_t count) noexcept
		{
			if (count > remaining())
			{
				_position = _data.size() * 8;
				_overrun = true;
				return;
			}

			_position += count;
		}

		void align() noexcept
		{
			_position = (_position + 7) & ~std::uint64_t{7};
			if (_position > _data.size() * 8) _overrun = true;
		}

		std::uint64_t position() const noexcept { return _position; }
		std::uint64_t remaining() const noexcept { return _data.size() * 8 - std::min<std::uint64_t>(_position, _data.size() * 8); }
		bool overrun() const noexcept { return _overrun; }

	private:
		std::uint32_t bit() noexcept
		{
			if (_position >= _data.size() * 8)
			{
				_overrun = true;
				return 0;
			}

			const auto v = _data[static_cast<std::size_t>(_position / 8)] >> (7 - _position % 8) & 1;
			++_position;
			return static_cast<std::uint32_t>(v);
		}

		std::span<const std::uint8_t> _data;
		std::uint64_t _position = 0;
		bool _overrun = false;
	};

	// ---- Text ----------------------------------------------------------------------------------------

	// Text as tags store it is decoded by text_internal.h, which every reader shares.

	// "XVID" for a printable FourCC as it lies in the file, else "0x" and eight hex digits.
	std::string fourcc_text(std::uint32_t little_endian_fourcc);
	// "avc1" for a printable big-endian box or sample entry type, else as above.
	std::string box_type_text(std::uint32_t big_endian_type);

	// ---- Video codec headers -------------------------------------------------------------------------

	// What a video codec's own header says about the picture. Zero or unknown where it says nothing.
	struct video_header
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t pixel_aspect_width = 0;
		std::uint32_t pixel_aspect_height = 0;
		chroma_format chroma = chroma_format::unknown;
		std::uint8_t bit_depth = 0;
		bool alpha = false;
		bool interlaced = false;
		bool full_range = false;
		std::uint8_t color_primaries = 2;
		std::uint8_t transfer = 2;
		std::uint8_t matrix = 2;
		double frame_rate = 0.0;
		std::string profile;
	};

	// Copies what the codec header knows into a stream: its size, chroma, depth, range, colour and
	// profile replace the container's, which describe the same picture less exactly; its pixel aspect
	// and frame rate only fill what the container left unknown, since the container's timing and
	// display shape are what a player follows.
	void apply_video_header(const video_header& header, video_format& video);

	// Each parser returns false, leaving out as it was, when the bytes are not that header or are cut
	// short. None throws.

	// H.264. A NAL unit, its one-byte header included, without emulation prevention removed.
	bool parse_h264_sps(std::span<const std::uint8_t> nal, video_header& out);
	// An AVCDecoderConfigurationRecord (avcC): its first sequence parameter set.
	bool parse_avc_config(std::span<const std::uint8_t> config, video_header& out);

	// HEVC. A NAL unit, its two-byte header included, without emulation prevention removed.
	bool parse_hevc_sps(std::span<const std::uint8_t> nal, video_header& out);
	// An HEVCDecoderConfigurationRecord (hvcC): its own chroma and depth fields, then its first SPS.
	bool parse_hevc_config(std::span<const std::uint8_t> config, video_header& out);

	// AV1. OBUs in the low-overhead format, holding a sequence header.
	bool parse_av1_sequence(std::span<const std::uint8_t> obus, video_header& out);
	// An AV1CodecConfigurationRecord (av1C): its own fields, then a sequence header among its configOBUs.
	bool parse_av1_config(std::span<const std::uint8_t> config, video_header& out);

	// VP8 and VP9. A VPCodecConfigurationRecord (vpcC) after its version and flags.
	bool parse_vp_config(std::span<const std::uint8_t> config, video_header& out);
	// The start of a frame: a VP9 uncompressed header, or a VP8 key frame's.
	bool parse_vp9_frame(std::span<const std::uint8_t> frame, video_header& out);
	bool parse_vp8_frame(std::span<const std::uint8_t> frame, video_header& out);

	// MPEG-4 Part 2. Start codes from a visual object sequence up to and including a video object layer:
	// an esds DecoderSpecificInfo, or the start of the elementary stream.
	bool parse_mpeg4_video(std::span<const std::uint8_t> data, video_header& out);
	// MPEG-1 and MPEG-2 video: a sequence header, with the sequence extension when one follows.
	bool parse_mpeg12_video(std::span<const std::uint8_t> data, video_header& out);
	// A JPEG frame, as Motion JPEG stores one: its SOF's size and sampling.
	bool parse_jpeg_frame(std::span<const std::uint8_t> data, video_header& out);
	// A Theora identification header, from its 0x80 "theora".
	bool parse_theora_header(std::span<const std::uint8_t> data, video_header& out);
	// The first DIF block of a DV frame: the system (525/60 or 625/50), and from it size and sampling.
	bool parse_dv_frame(std::span<const std::uint8_t> data, video_header& out);
	// VC-1, as AVI, ASF and Matroska's VfW mode store it after the 40 bytes of a BITMAPINFOHEADER, read as
	// FFmpeg's decoders read it: for WMV3 - the Simple, Main and Complex profiles - the STRUCT_C sequence
	// layer; for WVC1 and WMVA - the Advanced profile - a sequence header and an entry point, each after
	// its start code. What FFmpeg refuses at the decoder's start is refused here too.
	bool parse_vc1_sequence(std::span<const std::uint8_t> data, video_header& out);

	// Calls visit with each NAL unit of an Annex B byte stream - start codes of two or three zeros and
	// a one - without its start code, until visit returns false or the data ends.
	void for_each_annexb_nal(std::span<const std::uint8_t> data,
	                         const std::function<bool(std::span<const std::uint8_t>)>& visit);

	// ---- Audio codec headers -------------------------------------------------------------------------

	struct audio_header
	{
		std::uint32_t sample_rate = 0;
		std::uint32_t channels = 0;
		std::uint8_t bits_per_sample = 0;
		bool float_samples = false;
		std::string profile;
		// What one frame of a framed codec carries, where its header says.
		std::uint32_t bit_rate = 0;
		std::uint32_t samples_per_frame = 0;
		std::uint32_t frame_bytes = 0;
	};

	// Copies what the codec header knows into a stream; it replaces the container's figures, which
	// for AAC with SBR or PS, and for Opus, are not what plays.
	void apply_audio_header(const audio_header& header, audio_format& audio);

	// An AudioSpecificConfig: the object type, rate and channels, with SBR's doubled rate and PS's
	// two channels. The profile is "LC", "HE-AAC", "HE-AACv2", "Main", "LTP", "LD", "ELD" or "xHE-AAC".
	bool parse_aac_config(std::span<const std::uint8_t> config, audio_header& out);
	// An ADTS frame header.
	bool parse_adts_header(std::span<const std::uint8_t> data, audio_header& out);
	// An MPEG audio frame header. codec becomes "mp1", "mp2" or "mp3"; the profile names the version
	// and layer, "MPEG-1 Layer 3".
	bool parse_mpeg_audio_header(std::uint32_t header, audio_header& out, std::string& codec);
	// An AC-3 or E-AC-3 sync frame. codec becomes "ac3" or "eac3". E-AC-3's dependent substreams that
	// follow in data count towards the channels (7.1), and frame_bytes and bit_rate then cover them all.
	bool parse_ac3_frame(std::span<const std::uint8_t> data, audio_header& out, std::string& codec);
	// The dac3 and dec3 boxes' payloads, as MP4 stores AC-3 and E-AC-3.
	bool parse_ac3_config(std::span<const std::uint8_t> config, audio_header& out);
	bool parse_eac3_config(std::span<const std::uint8_t> config, audio_header& out);
	// A DTS core frame header.
	bool parse_dts_frame(std::span<const std::uint8_t> data, audio_header& out);
	// An OpusHead packet. Opus always plays at 48 kHz, whatever input rate it records.
	bool parse_opus_head(std::span<const std::uint8_t> data, audio_header& out, std::uint32_t& pre_skip);
	// A Vorbis identification header, from its 0x01 "vorbis".
	bool parse_vorbis_header(std::span<const std::uint8_t> data, audio_header& out);
	// A FLAC STREAMINFO block's 34 bytes. total_samples is 0 when the encoder did not know it.
	bool parse_flac_streaminfo(std::span<const std::uint8_t> data, audio_header& out, std::uint64_t& total_samples);
	// An ALACSpecificConfig, with or without the 12-byte 'alac' full box header before it.
	bool parse_alac_config(std::span<const std::uint8_t> data, audio_header& out);
	// A Speex header packet, from its "Speex   ".
	bool parse_speex_header(std::span<const std::uint8_t> data, audio_header& out);

	// ---- Windows structures and codec names ----------------------------------------------------------

	// Fills an audio stream from a WAVEFORMATEX or WAVEFORMATEXTENSIBLE: codec, codec_tag ("0x0161"),
	// rate, channels and, for PCM and lossless tags, the coded sample format; for the AAC tags, what the
	// AudioSpecificConfig after the structure says. avg_bytes_per_second and block_align are given back
	// for a container that times its data by them; the stream's bit_rate is left to the caller. False
	// when too short, or when the rate is 0.
	bool read_wave_format(std::span<const std::uint8_t> data, media_stream& stream,
	                      std::uint32_t* avg_bytes_per_second = nullptr, std::uint16_t* block_align = nullptr);
	// Fills a video stream from a BITMAPINFOHEADER: codec and codec_tag from its compression FourCC,
	// width and height, and the chroma and depth the format fixes - for uncompressed pictures by their
	// bit count, for raw FourCCs (YUY2, I420...) by their layout, and for the codecs fixed_video_format
	// knows. A WMV3, WVC1 or WMVA stream's VC-1 sequence header, after the structure's 40 bytes, gives
	// its profile and what else parse_vc1_sequence reads.
	bool read_bitmap_info(std::span<const std::uint8_t> data, media_stream& stream);

	// The short codec name for a video FourCC as AVI, ASF and Matroska's VfW mode store it, in the
	// file's byte order; empty when unknown.
	std::string_view codec_from_fourcc(std::uint32_t little_endian_fourcc) noexcept;
	// The short codec name for a WAVE format tag; empty when unknown.
	std::string_view codec_from_wave_tag(std::uint16_t tag) noexcept;
	// The short codec name for an MPEG-4 systems object type indication, as esds and MPEG-TS give it.
	std::string_view codec_from_object_type(std::uint8_t object_type) noexcept;

	// Chroma and depth for the codecs whose bitstream fixes them, so a stream needs no header read:
	// VP8, H.263, MPEG-4 Part 2 Simple and Advanced Simple, VC-1 and WMV, Cinepak and the like. False for
	// a codec whose streams differ, Theora's among them.
	bool fixed_video_format(std::string_view codec, video_header& out);

	// ---- Tags ----------------------------------------------------------------------------------------

	// The vocabularies containers name their tags in.
	enum class tag_family : std::uint8_t
	{
		// MP4 ilst atoms: "©nam", "aART", "trkn".
		itunes,
		// QuickTime: udta's "©xyz" and friends, and the keys of a 'mdta' handler, "com.apple.quicktime.make".
		quicktime,
		// ID3v2.2, 2.3 and 2.4 frame IDs: "TIT2", "TP1", "TXXX:Rip date".
		id3v2,
		id3v1,
		ape,
		// RIFF INFO: "INAM", "IART", "ICRD".
		riff_info,
		// ASF: the Content Description's "Title", "Author", ...; and descriptors, "WM/AlbumTitle".
		asf,
		// Matroska SimpleTag names: "TITLE", "ARTIST", "DATE_RELEASED".
		matroska,
		// Vorbis comment field names: "TITLE", "ARTIST", "DATE".
		vorbis,
		// FLV onMetaData names: "title", "creationdate", "encoder".
		flv,
		// AIFF text chunks: "NAME", "AUTH", "(c) ", "ANNO".
		aiff,
	};

	// Applies one tag to the properties its family's name maps to, as FFmpeg's demuxers name them so
	// that a recording files where it did. A tag no property takes is left alone.
	void apply_tag(media_properties& properties, tag_family family, std::string_view key, std::string_view value,
	               std::string_view language = {});

	// Adds a tag, trimmed, to the container's list and applies it. An empty value is not added.
	void add_tag(media_scan_result& out, tag_family family, std::string_view key, std::string_view value,
	             std::string_view language = {});

	// Ratings are read by the one rule each scale has, in libfiles/types.h: stars_from_percent for
	// Windows' percent and stars_from_popm for the POPM byte.

	// The ID3v2 tag at offset: its frames as tags, the properties they map to, APIC pictures, POPM
	// ratings and the PRIV "XMP" packet, by id3_packet's rule. Unsynchronised tags and frames are read too,
	// and compressed frames inflated within the scan's budget. Returns the size the tag takes, header and
	// footer included, or 0 when no tag starts there; a tag whose frames are damaged keeps the frames before
	// the damage.
	std::uint64_t read_id3v2(reader& r, std::uint64_t offset, media_budget& budget, media_scan_result& out);
	// The ID3v1 tag in the 128 bytes at offset, when it is one. Only fields the result lacks are taken.
	bool read_id3v1(reader& r, std::uint64_t offset, media_scan_result& out);
	// The APE tag that ends at end, header and footer both found from its footer. Returns the bytes it
	// takes, or 0 when none ends there.
	std::uint64_t read_ape_tag(reader& r, std::uint64_t end, media_scan_result& out);

	// A Vorbis comment block - the vendor string, then counted fields - as FLAC, Ogg Vorbis, Opus,
	// Speex and Theora hold it after their own signatures. METADATA_BLOCK_PICTURE fields become pictures.
	bool read_vorbis_comments(std::span<const std::uint8_t> block, media_scan_result& out);
	// A FLAC PICTURE block, in FLAC itself or base64-decoded from a comment. When block_offset is not
	// zero the block lies in the file there and the picture is located rather than copied.
	bool read_flac_picture(std::span<const std::uint8_t> block, std::uint64_t block_offset, media_picture& out);

	// ---- Dates and places ----------------------------------------------------------------------------

	// Dates are read by the rules date_internal.h keeps for readers and writers alike.

	// An ISO 6709 position, "+51.5142-000.0985+012.000/". Absent when it is not one, or is 0, 0.
	std::optional<gps_position> parse_iso6709(std::string_view text, double* altitude = nullptr) noexcept;

	// ---- Scanners ------------------------------------------------------------------------------------

	// Each fills out from a file detect_media_format named, and throws format_error or read_error for
	// one it cannot read. scan_media catches them, then finish_media_scan completes the result.
	void scan_mp4_media(reader& r, scan_intent intent, media_scan_result& out);
	void scan_matroska_media(reader& r, scan_intent intent, media_budget& budget, media_scan_result& out);
	void scan_avi_media(reader& r, scan_intent intent, media_scan_result& out);
	void scan_wav_media(reader& r, scan_intent intent, media_budget& budget, media_scan_result& out);
	void scan_aiff_media(reader& r, scan_intent intent, media_budget& budget, media_scan_result& out);
	void scan_asf_media(reader& r, scan_intent intent, media_scan_result& out);
	// From the first frame after any leading ID3v2 tag, whose size is given.
	void scan_mpeg_audio_media(reader& r, scan_intent intent, std::uint64_t tag_size, media_budget& budget, media_scan_result& out);
	// From the "fLaC" signature at offset, after any leading ID3v2 tag.
	void scan_flac_media(reader& r, scan_intent intent, std::uint64_t offset, media_scan_result& out);
	void scan_ogg_media(reader& r, scan_intent intent, media_scan_result& out);
	void scan_flv_media(reader& r, scan_intent intent, media_scan_result& out);
	void scan_mpeg_ts_media(reader& r, scan_intent intent, media_scan_result& out);
	void scan_mpeg_ps_media(reader& r, scan_intent intent, media_scan_result& out);

	// The duration and bit rate where the streams know them better than the container, and whether
	// the scan is complete.
	void finish_media_scan(media_scan_result& out, std::uint64_t file_size);
}
