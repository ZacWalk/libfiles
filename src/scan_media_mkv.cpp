// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning Matroska and WebM files. The EBML header names the variant, and the Segment's
// top-level elements are walked by their headers as far as the first Cluster; its SeekHeads lead to
// the rest - Info, Tracks, Tags and Attachments - wherever they lie, so Cues and Clusters are never
// read. A track whose codec keeps its parameters only in the bitstream is completed from its first
// block in the first Cluster, and an attached picture is located rather than read. Tags reach the
// result flattened as FFmpeg's matroskadec flattens them, so a recording files where it did.

#include "media_internal.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		// Element IDs as RFC 8794 and RFC 9559 give them, their length markers included.
		constexpr std::uint32_t id_ebml = 0x1A45DFA3;
		constexpr std::uint32_t id_ebml_read_version = 0x42F7;
		constexpr std::uint32_t id_ebml_max_id_length = 0x42F2;
		constexpr std::uint32_t id_ebml_max_size_length = 0x42F3;
		constexpr std::uint32_t id_doc_type = 0x4282;
		constexpr std::uint32_t id_doc_type_version = 0x4287;
		constexpr std::uint32_t id_doc_type_read_version = 0x4285;
		constexpr std::uint32_t id_void = 0xEC;
		constexpr std::uint32_t id_crc32 = 0xBF;

		constexpr std::uint32_t id_segment = 0x18538067;
		constexpr std::uint32_t id_seek_head = 0x114D9B74;
		constexpr std::uint32_t id_info = 0x1549A966;
		constexpr std::uint32_t id_tracks = 0x1654AE6B;
		constexpr std::uint32_t id_cluster = 0x1F43B675;
		constexpr std::uint32_t id_cues = 0x1C53BB6B;
		constexpr std::uint32_t id_chapters = 0x1043A770;
		constexpr std::uint32_t id_tags = 0x1254C367;
		constexpr std::uint32_t id_attachments = 0x1941A469;

		constexpr std::uint32_t id_seek = 0x4DBB;
		constexpr std::uint32_t id_seek_id = 0x53AB;
		constexpr std::uint32_t id_seek_position = 0x53AC;

		constexpr std::uint32_t id_timestamp_scale = 0x2AD7B1;
		constexpr std::uint32_t id_duration = 0x4489;
		constexpr std::uint32_t id_date_utc = 0x4461;
		constexpr std::uint32_t id_title = 0x7BA9;
		constexpr std::uint32_t id_muxing_app = 0x4D80;
		constexpr std::uint32_t id_writing_app = 0x5741;

		constexpr std::uint32_t id_track_entry = 0xAE;
		constexpr std::uint32_t id_track_number = 0xD7;
		constexpr std::uint32_t id_track_uid = 0x73C5;
		constexpr std::uint32_t id_track_type = 0x83;
		constexpr std::uint32_t id_flag_default = 0x88;
		constexpr std::uint32_t id_flag_visual_impaired = 0x55AC;
		constexpr std::uint32_t id_flag_commentary = 0x55AF;
		constexpr std::uint32_t id_default_duration = 0x23E383;
		constexpr std::uint32_t id_name = 0x536E;
		constexpr std::uint32_t id_language = 0x22B59C;
		constexpr std::uint32_t id_language_bcp47 = 0x22B59D;
		constexpr std::uint32_t id_codec_id = 0x86;
		constexpr std::uint32_t id_codec_private = 0x63A2;
		constexpr std::uint32_t id_video = 0xE0;
		constexpr std::uint32_t id_audio = 0xE1;
		constexpr std::uint32_t id_content_encodings = 0x6D80;
		constexpr std::uint32_t id_content_encoding = 0x6240;
		constexpr std::uint32_t id_content_encoding_scope = 0x5032;
		constexpr std::uint32_t id_content_encoding_type = 0x5033;
		constexpr std::uint32_t id_content_compression = 0x5034;
		constexpr std::uint32_t id_content_comp_algo = 0x4254;
		constexpr std::uint32_t id_content_comp_settings = 0x4255;

		constexpr std::uint32_t id_flag_interlaced = 0x9A;
		constexpr std::uint32_t id_stereo_mode = 0x53B8;
		constexpr std::uint32_t id_alpha_mode = 0x53C0;
		constexpr std::uint32_t id_pixel_width = 0xB0;
		constexpr std::uint32_t id_pixel_height = 0xBA;
		constexpr std::uint32_t id_pixel_crop_bottom = 0x54AA;
		constexpr std::uint32_t id_pixel_crop_top = 0x54BB;
		constexpr std::uint32_t id_pixel_crop_left = 0x54CC;
		constexpr std::uint32_t id_pixel_crop_right = 0x54DD;
		constexpr std::uint32_t id_display_width = 0x54B0;
		constexpr std::uint32_t id_display_height = 0x54BA;
		constexpr std::uint32_t id_display_unit = 0x54B2;
		constexpr std::uint32_t id_uncompressed_fourcc = 0x2EB524;
		constexpr std::uint32_t id_frame_rate = 0x2383E3;
		constexpr std::uint32_t id_colour = 0x55B0;
		constexpr std::uint32_t id_matrix_coefficients = 0x55B1;
		constexpr std::uint32_t id_bits_per_channel = 0x55B2;
		constexpr std::uint32_t id_chroma_subsampling_horz = 0x55B3;
		constexpr std::uint32_t id_chroma_subsampling_vert = 0x55B4;
		constexpr std::uint32_t id_range = 0x55B9;
		constexpr std::uint32_t id_transfer_characteristics = 0x55BA;
		constexpr std::uint32_t id_primaries = 0x55BB;
		constexpr std::uint32_t id_projection = 0x7670;
		constexpr std::uint32_t id_projection_type = 0x7671;
		constexpr std::uint32_t id_projection_private = 0x7672;
		constexpr std::uint32_t id_projection_pose_yaw = 0x7673;
		constexpr std::uint32_t id_projection_pose_pitch = 0x7674;
		constexpr std::uint32_t id_projection_pose_roll = 0x7675;

		constexpr std::uint32_t id_sampling_frequency = 0xB5;
		constexpr std::uint32_t id_output_sampling_frequency = 0x78B5;
		constexpr std::uint32_t id_channels = 0x9F;
		constexpr std::uint32_t id_bit_depth = 0x6264;

		constexpr std::uint32_t id_tag = 0x7373;
		constexpr std::uint32_t id_targets = 0x63C0;
		constexpr std::uint32_t id_target_type = 0x63CA;
		constexpr std::uint32_t id_tag_track_uid = 0x63C5;
		constexpr std::uint32_t id_tag_chapter_uid = 0x63C4;
		constexpr std::uint32_t id_tag_attachment_uid = 0x63C6;
		constexpr std::uint32_t id_simple_tag = 0x67C8;
		constexpr std::uint32_t id_tag_name = 0x45A3;
		constexpr std::uint32_t id_tag_language = 0x447A;
		constexpr std::uint32_t id_tag_language_bcp47 = 0x447B;
		constexpr std::uint32_t id_tag_string = 0x4487;
		constexpr std::uint32_t id_tag_binary = 0x4485;

		constexpr std::uint32_t id_attached_file = 0x61A7;
		constexpr std::uint32_t id_file_description = 0x467E;
		constexpr std::uint32_t id_file_name = 0x466E;
		constexpr std::uint32_t id_file_mime_type = 0x4660;
		constexpr std::uint32_t id_file_data = 0x465C;

		constexpr std::uint32_t id_cluster_timestamp = 0xE7;
		constexpr std::uint32_t id_cluster_position = 0xA7;
		constexpr std::uint32_t id_cluster_prev_size = 0xAB;
		constexpr std::uint32_t id_silent_tracks = 0x5854;
		constexpr std::uint32_t id_simple_block = 0xA3;
		constexpr std::uint32_t id_block_group = 0xA0;
		constexpr std::uint32_t id_block = 0xA1;
		constexpr std::uint32_t id_encrypted_block = 0xAF;

		constexpr std::uint64_t unknown_size = ~std::uint64_t{0};

		// libfiles' own bounds, which no well-formed file comes near.
		constexpr std::size_t max_ebml_header = 64u * 1024u;
		constexpr std::size_t max_small_element = 1024u * 1024u;
		constexpr std::size_t max_held_element = 16u * 1024u * 1024u;
		constexpr std::size_t max_top_level = 4096;
		constexpr std::size_t max_seek_entries = 1024;
		constexpr std::size_t max_element_reads = 64;
		constexpr std::size_t max_tracks = 1000;
		constexpr std::size_t max_tags = 4096;
		constexpr int max_tag_depth = 16;
		constexpr std::size_t max_tag_key = 1024;
		constexpr std::size_t max_attached_files = 128;
		constexpr std::size_t max_attached_file_children = 32;
		constexpr std::size_t max_attachment_text = 64u * 1024u;
		constexpr std::uint64_t max_channels = 65535;
		constexpr std::uint64_t max_display_size = 1u << 24;

		// The first Cluster is read only for a track that needs its first block, and then no further
		// than this; a frame is read a little at first, where codec headers lie, and more only when that
		// was not enough.
		constexpr std::size_t max_cluster_children = 32;
		constexpr std::size_t max_block_group_children = 8;
		constexpr std::uint64_t needed_block_span = 1024u * 1024u;
		constexpr std::size_t max_block_header = 4096;
		constexpr std::size_t video_frame_head = 4096;
		constexpr std::size_t video_frame_most = 64u * 1024u;
		constexpr std::size_t audio_frame_head = 256;

		// A block a track would merely like, an AC-3 or DTS frame, is read only where it costs nothing:
		// within the first block a scan's first read brings in.
		constexpr std::uint64_t cheap_span = reader::default_block_size;

		struct element_header
		{
			std::uint32_t id = 0;
			// The payload's size, or unknown_size.
			std::uint64_t size = 0;
			// The ID's and the size's bytes.
			std::size_t length = 0;
		};

		// The element header at the start of bytes: an ID of one to four bytes, neither all zeros nor
		// all ones after its marker, then a size of one to eight bytes, all ones meaning unknown.
		std::optional<element_header> parse_header(const std::span<const std::uint8_t> bytes)
		{
			if (bytes.empty() || bytes[0] < 0x10) return {};

			const auto id_length = static_cast<std::size_t>(std::countl_zero(bytes[0])) + 1;
			if (bytes.size() <= id_length) return {};

			std::uint32_t id = 0;
			for (std::size_t i = 0; i < id_length; ++i) id = id << 8 | bytes[i];

			const auto id_mask = (std::uint32_t{1} << (7 * id_length)) - 1;
			if ((id & id_mask) == 0 || (id & id_mask) == id_mask) return {};

			const auto first = bytes[id_length];
			if (first == 0) return {};

			const auto size_length = static_cast<std::size_t>(std::countl_zero(first)) + 1;
			if (bytes.size() < id_length + size_length) return {};

			std::uint64_t size = first & (0xFFu >> size_length);
			for (std::size_t i = 1; i < size_length; ++i) size = size << 8 | bytes[id_length + i];
			if (size == (std::uint64_t{1} << (7 * size_length)) - 1) size = unknown_size;

			return element_header{id, size, id_length + size_length};
		}

		struct child
		{
			std::uint32_t id = 0;
			std::span<const std::uint8_t> data;
		};

		// The children of a master element held in memory, in order, up to the first that is damaged,
		// of unknown size or larger than what is left.
		class children
		{
		public:
			explicit children(const std::span<const std::uint8_t> payload) noexcept : _rest(payload)
			{
			}

			std::optional<child> next()
			{
				if (_rest.empty()) return {};

				const auto h = parse_header(_rest);

				if (!h || h->size == unknown_size || h->size > _rest.size() - h->length)
				{
					_damaged = true;
					_rest = {};
					return {};
				}

				const auto size = static_cast<std::size_t>(h->size);
				const child c{h->id, _rest.subspan(h->length, size)};
				_rest = _rest.subspan(h->length + size);
				return c;
			}

			bool damaged() const noexcept { return _damaged; }

		private:
			std::span<const std::uint8_t> _rest;
			bool _damaged = false;
		};

		// An unsigned integer of one to eight bytes. Absent for none, which means the default, and for
		// more than eight, which is damage.
		std::optional<std::uint64_t> to_uint(const std::span<const std::uint8_t> data)
		{
			if (data.empty() || data.size() > 8) return {};

			std::uint64_t v = 0;
			for (const auto b : data) v = v << 8 | b;
			return v;
		}

		std::uint64_t uint_or(const std::span<const std::uint8_t> data, const std::uint64_t fallback)
		{
			return to_uint(data).value_or(fallback);
		}

		std::optional<double> to_float(const std::span<const std::uint8_t> data)
		{
			if (data.size() == 4) return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(*to_uint(data))));
			if (data.size() == 8) return std::bit_cast<double>(*to_uint(data));
			return {};
		}

		std::string to_text(const std::span<const std::uint8_t> data)
		{
			return text_from_utf8(data);
		}

		bool equal_ignoring_case(const std::string_view a, const std::string_view b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y)
			{
				const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
				return lower(x) == lower(y);
			});
		}

		struct codec_mapping
		{
			std::string_view codec_id;
			std::string_view codec;
		};

		// The Matroska codec mappings' CodecIDs, matched as prefixes as FFmpeg matches them, so that
		// "A_AAC/MPEG4/LC/SBR" is AAC and "A_DTS/EXPRESS" DTS. V_MS/VFW/FOURCC and A_MS/ACM name their
		// codec in the Windows structure they carry.
		constexpr codec_mapping codec_mappings[] = {
			{"V_MPEG4/ISO/AVC", "h264"},
			{"V_MPEGH/ISO/HEVC", "hevc"},
			{"V_MPEGI/ISO/VVC", "vvc"},
			{"V_AV1", "av1"},
			{"V_VP9", "vp9"},
			{"V_VP8", "vp8"},
			{"V_MPEG4/ISO/SP", "mpeg4"},
			{"V_MPEG4/ISO/ASP", "mpeg4"},
			{"V_MPEG4/ISO/AP", "mpeg4"},
			{"V_MPEG4/MS/V3", "msmpeg4v3"},
			{"V_MPEG1", "mpeg1"},
			{"V_MPEG2", "mpeg2"},
			{"V_MJPEG", "mjpeg"},
			{"V_THEORA", "theora"},
			{"V_PRORES", "prores"},
			{"V_FFV1", "ffv1"},
			{"V_DIRAC", "dirac"},
			{"V_JPEG2000", "jpeg2000"},
			{"V_UNCOMPRESSED", "rawvideo"},
			{"V_REAL/RV10", "rv10"},
			{"V_REAL/RV20", "rv20"},
			{"V_REAL/RV30", "rv30"},
			{"V_REAL/RV40", "rv40"},
			{"A_AAC", "aac"},
			{"A_MPEG/L3", "mp3"},
			{"A_MPEG/L2", "mp2"},
			{"A_MPEG/L1", "mp1"},
			{"A_AC3", "ac3"},
			{"A_EAC3", "eac3"},
			{"A_DTS", "dts"},
			{"A_OPUS", "opus"},
			{"A_VORBIS", "vorbis"},
			{"A_FLAC", "flac"},
			{"A_ALAC", "alac"},
			{"A_PCM/INT/LIT", "pcm"},
			{"A_PCM/INT/BIG", "pcm"},
			{"A_PCM/FLOAT/IEEE", "pcm"},
			{"A_TRUEHD", "truehd"},
			{"A_MLP", "mlp"},
			{"A_WAVPACK4", "wavpack"},
			{"A_TTA1", "tta"},
			{"A_ATRAC/AT1", "atrac1"},
			{"A_REAL/14_4", "ra_144"},
			{"A_REAL/28_8", "ra_288"},
			{"A_REAL/ATRC", "atrac3"},
			{"A_REAL/COOK", "cook"},
			{"A_REAL/SIPR", "sipr"},
			{"A_QUICKTIME/QDMC", "qdmc"},
			{"A_QUICKTIME/QDM2", "qdm2"},
			{"S_TEXT/UTF8", "subrip"},
			{"S_TEXT/ASCII", "text"},
			{"S_TEXT/ASS", "ass"},
			{"S_TEXT/SSA", "ass"},
			{"S_ASS", "ass"},
			{"S_SSA", "ass"},
			{"S_TEXT/WEBVTT", "webvtt"},
			{"D_WEBVTT/", "webvtt"},
			{"S_VOBSUB", "dvd_subtitle"},
			{"S_DVBSUB", "dvb_subtitle"},
			{"S_HDMV/PGS", "hdmv_pgs_subtitle"},
			{"S_HDMV/TEXTST", "hdmv_text_subtitle"},
			{"S_ARIBSUB", "arib_caption"},
		};

		std::string_view codec_from_id(const std::string_view codec_id)
		{
			for (const auto& m : codec_mappings)
			{
				if (codec_id.starts_with(m.codec_id)) return m.codec;
			}

			return {};
		}

		// Whether a CodecID belongs to a track of this kind, as FFmpeg checks before it trusts one: a
		// video track's starts with V, an audio track's with A, a subtitle's with S or D.
		bool codec_fits_kind(const std::string_view codec_id, const media_stream_kind kind)
		{
			if (codec_id.empty()) return false;

			switch (kind)
			{
			case media_stream_kind::video: return codec_id[0] == 'V';
			case media_stream_kind::audio: return codec_id[0] == 'A';
			case media_stream_kind::subtitle:
			case media_stream_kind::data: return codec_id[0] == 'S' || codec_id[0] == 'D';
			}

			return false;
		}

		bool is_lossless(const std::string_view codec)
		{
			return codec == "pcm" || codec == "flac" || codec == "alac" || codec == "wavpack" || codec == "truehd" ||
				codec == "mlp" || codec == "tta";
		}

		// The profile an old-style AAC CodecID names, for a track without an AudioSpecificConfig.
		std::string_view aac_profile_from_id(const std::string_view codec_id)
		{
			if (codec_id.find("SBR") != std::string_view::npos) return "HE-AAC";
			if (codec_id.find("MAIN") != std::string_view::npos) return "Main";
			if (codec_id.find("SSR") != std::string_view::npos) return "SSR";
			if (codec_id.find("LTP") != std::string_view::npos) return "LTP";
			if (codec_id.find("LC") != std::string_view::npos) return "LC";
			return {};
		}

		// The first packet of a Xiph-laced CodecPrivate, as Matroska keeps Vorbis's and Theora's three
		// headers: a count less one, the sizes of all but the last in runs of 255, then the packets.
		std::optional<std::span<const std::uint8_t>> first_xiph_packet(const std::span<const std::uint8_t> data)
		{
			if (data.empty()) return {};

			const std::size_t sizes = data[0];
			std::size_t pos = 1;
			std::size_t first = 0;
			std::size_t total = 0;

			for (std::size_t i = 0; i < sizes; ++i)
			{
				std::size_t size = 0;

				for (;;)
				{
					if (pos >= data.size()) return {};
					const auto b = data[pos++];
					size += b;
					if (b < 255) break;
				}

				if (i == 0) first = size;
				total += size;
			}

			if (total > data.size() - pos) return {};
			return sizes == 0 ? data.subspan(pos) : data.subspan(pos, first);
		}

		// The VP9 CodecPrivate WebM defines: features of an ID byte, a length byte and a value. Its
		// profile, bit depth and chroma subsampling are what a first frame would otherwise be read for.
		bool read_vp9_features(const std::span<const std::uint8_t> data, video_header& out)
		{
			video_header h;
			auto any = false;
			std::size_t pos = 0;

			while (data.size() - pos >= 2)
			{
				const auto id = data[pos];
				const std::size_t length = data[pos + 1];
				pos += 2;
				if (length > data.size() - pos) break;

				if (length == 1)
				{
					const auto v = data[pos];

					if (id == 1 && v <= 3)
					{
						h.profile = std::format("Profile {}", v);
						any = true;
					}
					else if (id == 3 && (v == 8 || v == 10 || v == 12))
					{
						h.bit_depth = v;
						any = true;
					}
					else if (id == 4 && v <= 3)
					{
						h.chroma = v <= 1 ? chroma_format::yuv420 : v == 2 ? chroma_format::yuv422 : chroma_format::yuv444;
						any = true;
					}
				}

				pos += length;
			}

			if (any) out = h;
			return any;
		}

		// Whether an H.264 or HEVC CodecPrivate is an Annex B byte stream rather than a configuration
		// record, as some muxers write it.
		bool starts_with_start_code(const std::span<const std::uint8_t> data)
		{
			return data.size() >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1));
		}

		// The first sequence parameter set among NAL units: an Annex B stream, else units each after a
		// four-byte length, as a Matroska block holds them.
		bool find_parameter_set(const std::span<const std::uint8_t> data, const bool hevc, video_header& out)
		{
			auto found = false;

			const auto visit = [&](const std::span<const std::uint8_t> nal)
			{
				if (nal.empty()) return true;
				const auto type = hevc ? (nal[0] >> 1) & 0x3F : nal[0] & 0x1F;
				if (type != (hevc ? 33 : 7)) return true;

				found = hevc ? parse_hevc_sps(nal, out) : parse_h264_sps(nal, out);
				return !found;
			};

			if (starts_with_start_code(data))
			{
				for_each_annexb_nal(data, visit);
				return found;
			}

			std::size_t pos = 0;

			while (data.size() - pos > 4)
			{
				const auto length = load_be32(data.data() + pos);
				pos += 4;
				const auto nal = data.subspan(pos, std::min<std::size_t>(length, data.size() - pos));
				if (!visit(nal) || nal.size() < length) break;
				pos += nal.size();
			}

			return found;
		}

		chroma_format chroma_from_subsampling(const std::uint64_t horizontal, const std::uint64_t vertical)
		{
			if (horizontal == 0 && vertical == 0) return chroma_format::yuv444;
			if (horizontal == 1 && vertical == 0) return chroma_format::yuv422;
			if (horizontal == 1 && vertical == 1) return chroma_format::yuv420;
			if (horizontal == 2 && vertical == 0) return chroma_format::yuv411;
			if (horizontal == 2 && vertical == 2) return chroma_format::yuv410;
			if (horizontal == 0 && vertical == 1) return chroma_format::yuv440;
			return chroma_format::unknown;
		}

		// The Colour element: H.273 code points as FFmpeg takes them - the reserved ones left out - with
		// the range, the bit depth and the chroma subsampling.
		void read_colour(const std::span<const std::uint8_t> data, video_format& video)
		{
			std::optional<std::uint64_t> horizontal;
			std::optional<std::uint64_t> vertical;
			children c(data);

			while (const auto e = c.next())
			{
				const auto v = to_uint(e->data);
				if (!v) continue;

				switch (e->id)
				{
				case id_matrix_coefficients:
					if (*v <= 255 && *v != 3) video.matrix = static_cast<std::uint8_t>(*v);
					break;
				case id_primaries:
					if (*v <= 255 && *v != 0 && *v != 3) video.color_primaries = static_cast<std::uint8_t>(*v);
					break;
				case id_transfer_characteristics:
					if (*v <= 255 && *v != 0 && *v != 3) video.transfer = static_cast<std::uint8_t>(*v);
					break;
				case id_range:
					if (*v == 1 || *v == 2) video.full_range = *v == 2;
					break;
				case id_bits_per_channel:
					if (*v > 0 && *v <= 16) video.bit_depth = static_cast<std::uint8_t>(*v);
					break;
				case id_chroma_subsampling_horz: horizontal = v;
					break;
				case id_chroma_subsampling_vert: vertical = v;
					break;
				default: break;
				}
			}

			if (horizontal && vertical) video.chroma = chroma_from_subsampling(*horizontal, *vertical);
		}

		// The turn a rectangular Projection states, as FFmpeg turns it into a display matrix: its roll
		// turns the picture counter-clockwise, and a yaw of 180 degrees, applied first, mirrors it. Any
		// other pose is not a flat turn, and a turn that is not a right angle is left out.
		void read_projection(const std::span<const std::uint8_t> data, video_format& video)
		{
			std::uint64_t type = 0;
			std::span<const std::uint8_t> projection_private;
			double yaw = 0;
			double pitch = 0;
			double roll = 0;
			children c(data);

			while (const auto e = c.next())
			{
				switch (e->id)
				{
				case id_projection_type: type = uint_or(e->data, 0);
					break;
				case id_projection_private: projection_private = e->data;
					break;
				case id_projection_pose_yaw: yaw = to_float(e->data).value_or(0);
					break;
				case id_projection_pose_pitch: pitch = to_float(e->data).value_or(0);
					break;
				case id_projection_pose_roll: roll = to_float(e->data).value_or(0);
					break;
				default: break;
				}
			}

			if (type != 0 || (!projection_private.empty() && projection_private[0] != 0)) return;
			if (pitch != 0 || (yaw != 0 && yaw != 180 && yaw != -180)) return;
			if (!std::isfinite(roll) || std::abs(roll) > 360 || (yaw == 0 && roll == 0)) return;

			const auto turn = std::round(-roll / 90) * 90;
			if (std::abs(turn + roll) > 1) return;

			video.rotation = (static_cast<int>(turn) % 360 + 360) % 360;
			video.mirrored = yaw != 0;
		}

		struct pixel_crop
		{
			std::uint64_t left = 0;
			std::uint64_t right = 0;
			std::uint64_t top = 0;
			std::uint64_t bottom = 0;
		};

		struct video_fields
		{
			pixel_crop crop;
			// V_UNCOMPRESSED's FourCC, which names the picture's layout.
			std::optional<std::uint32_t> fourcc;
		};

		// The Video element: the picture's size before cropping, the pixel shape its display size gives
		// - for every display unit but unknown, as FFmpeg computes it, and only when the file states a
		// display size - and the flags, colour and turn it states. The crop it returns applies to the
		// size a codec header gives too.
		video_fields read_video(const std::span<const std::uint8_t> data, video_format& video)
		{
			std::uint64_t width = 0;
			std::uint64_t height = 0;
			video_fields fields;
			auto& crop = fields.crop;
			std::optional<std::uint64_t> display_width;
			std::optional<std::uint64_t> display_height;
			std::uint64_t display_unit = 0;
			std::uint64_t stereo_mode = 0;
			auto colour_read = false;
			children c(data);

			while (const auto e = c.next())
			{
				switch (e->id)
				{
				case id_pixel_width: width = uint_or(e->data, 0);
					break;
				case id_pixel_height: height = uint_or(e->data, 0);
					break;
				case id_pixel_crop_left: crop.left = uint_or(e->data, 0);
					break;
				case id_pixel_crop_right: crop.right = uint_or(e->data, 0);
					break;
				case id_pixel_crop_top: crop.top = uint_or(e->data, 0);
					break;
				case id_pixel_crop_bottom: crop.bottom = uint_or(e->data, 0);
					break;
				case id_display_width: display_width = to_uint(e->data);
					break;
				case id_display_height: display_height = to_uint(e->data);
					break;
				case id_display_unit: display_unit = uint_or(e->data, 0);
					break;
				case id_stereo_mode: stereo_mode = uint_or(e->data, 0);
					break;
				case id_flag_interlaced: video.interlaced = uint_or(e->data, 0) == 1;
					break;
				case id_alpha_mode: video.alpha = uint_or(e->data, 0) == 1;
					break;
				case id_frame_rate:
					if (const auto f = to_float(e->data); f && std::isfinite(*f) && *f > 0) video.frame_rate = *f;
					break;
				case id_colour:
					if (!colour_read) read_colour(e->data, video);
					colour_read = true;
					break;
				case id_projection: read_projection(e->data, video);
					break;
				case id_uncompressed_fourcc:
					if (e->data.size() == 4) fields.fourcc = load_le32(e->data.data());
					break;
				default: break;
				}
			}

			const auto max_edge = static_cast<std::uint64_t>(max_image_dimension);

			if (width == 0 || height == 0 || width > max_edge || height > max_edge)
			{
				crop = {};
				return fields;
			}

			if (crop.left >= width || crop.right >= width - crop.left || crop.top >= height || crop.bottom >= height - crop.top) crop = {};

			video.width = static_cast<std::uint32_t>(width);
			video.height = static_cast<std::uint32_t>(height);

			if (display_unit >= 4 || (!display_width && !display_height)) return fields;

			const auto shown_width = width - crop.left - crop.right;
			const auto shown_height = height - crop.top - crop.bottom;
			const auto across = display_width.value_or(display_unit == 0 ? shown_width : 0);
			const auto down = display_height.value_or(display_unit == 0 ? shown_height : 0);
			if (across == 0 || down == 0 || across > max_display_size || down > max_display_size) return fields;

			// A stereo picture packed side by side, or one above the other, shows each eye at its display size.
			const auto side_by_side = stereo_mode == 1 || stereo_mode == 8 || stereo_mode == 9 || stereo_mode == 11;
			const auto stacked = stereo_mode == 2 || stereo_mode == 3 || stereo_mode == 6 || stereo_mode == 7;
			auto shape_width = shown_height * across * (side_by_side ? 2u : 1u);
			auto shape_height = shown_width * down * (stacked ? 2u : 1u);
			const auto divisor = std::gcd(shape_width, shape_height);
			shape_width /= divisor;
			shape_height /= divisor;

			if (shape_width <= 0xFFFFFFFFu && shape_height <= 0xFFFFFFFFu)
			{
				video.pixel_aspect_width = static_cast<std::uint32_t>(shape_width);
				video.pixel_aspect_height = static_cast<std::uint32_t>(shape_height);
			}

			return fields;
		}

		// The Audio element, with Matroska's defaults of 8 kHz and one channel as FFmpeg applies them,
		// and its fallback to 8 kHz for a rate it cannot use. The rate that plays is the output rate,
		// which AAC with SBR states apart. Returns the coded bit depth, 0 when not stated.
		std::uint64_t read_audio(const std::span<const std::uint8_t> data, audio_format& audio)
		{
			double rate = 8000;
			double output = 0;
			std::uint64_t channels = 1;
			std::uint64_t bit_depth = 0;
			children c(data);

			while (const auto e = c.next())
			{
				switch (e->id)
				{
				case id_sampling_frequency: rate = to_float(e->data).value_or(rate);
					break;
				case id_output_sampling_frequency: output = to_float(e->data).value_or(0);
					break;
				case id_channels: channels = uint_or(e->data, 1);
					break;
				case id_bit_depth: bit_depth = uint_or(e->data, 0);
					break;
				default: break;
				}
			}

			constexpr double max_rate = 2147483647.0;
			if (std::isnan(rate) || rate < 0 || rate > max_rate) rate = 8000;
			const auto plays = output > 0 && output <= max_rate ? output : rate;

			audio.sample_rate = static_cast<std::uint32_t>(plays);
			audio.channels = channels <= max_channels ? static_cast<std::uint32_t>(channels) : 0;
			return bit_depth <= 64 ? bit_depth : 0;
		}

		// DateUTC counts nanoseconds from the start of 2001, in UTC.
		media_date date_from_nanoseconds(const std::int64_t nanoseconds)
		{
			constexpr std::int64_t per_second = 1000000000;
			constexpr std::int64_t seconds_before_2001 = 978307200;

			auto seconds = nanoseconds / per_second;
			if (nanoseconds % per_second < 0) --seconds;
			return utc_date_from_seconds(seconds + seconds_before_2001);
		}

		bool parse_digits(const std::string_view text, std::uint64_t& value)
		{
			if (text.empty() || text.size() > 9) return false;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			return error == std::errc() && end == text.data() + text.size();
		}

		// A track's statistics tag DURATION, as mkvmerge and FFmpeg write it: "01:23:45.678901234".
		std::optional<double> parse_duration_tag(const std::string_view text)
		{
			const auto first = text.find(':');
			if (first == std::string_view::npos) return {};
			const auto second = text.find(':', first + 1);
			if (second == std::string_view::npos) return {};

			std::uint64_t hours = 0;
			std::uint64_t minutes = 0;
			std::uint64_t seconds = 0;
			const auto rest = text.substr(second + 1);
			const auto dot = rest.find('.');

			if (!parse_digits(text.substr(0, first), hours) || !parse_digits(text.substr(first + 1, second - first - 1), minutes) ||
				!parse_digits(rest.substr(0, dot), seconds) || minutes >= 60 || seconds >= 60)
			{
				return {};
			}

			auto total = static_cast<double>(hours * 3600 + minutes * 60 + seconds);

			if (dot != std::string_view::npos)
			{
				auto scale = 0.1;

				for (const auto c : rest.substr(dot + 1))
				{
					if (c < '0' || c > '9') return {};
					total += (c - '0') * scale;
					scale /= 10;
				}
			}

			if (!(total > 0)) return {};
			return total;
		}

		// A cover in the names Matroska's attachment conventions give one: cover.jpg, small_cover.png,
		// cover_land.jpg and small_cover_land.png.
		bool is_cover_name(std::string_view name)
		{
			if (const auto dot = name.rfind('.'); dot != std::string_view::npos) name = name.substr(0, dot);
			return equal_ignoring_case(name, "cover") || equal_ignoring_case(name, "small_cover") ||
				equal_ignoring_case(name, "cover_land") || equal_ignoring_case(name, "small_cover_land");
		}

		std::string element_name(const std::uint32_t id)
		{
			switch (id)
			{
			case id_seek_head: return "SeekHead";
			case id_info: return "Info";
			case id_tracks: return "Tracks";
			case id_cluster: return "Cluster";
			case id_cues: return "Cues";
			case id_chapters: return "Chapters";
			case id_tags: return "Tags";
			case id_attachments: return "Attachments";
			case id_void: return "Void";
			case id_crc32: return "CRC-32";
			default: return std::format("0x{:X}", id);
			}
		}

		std::string_view kind_name(const media_stream_kind kind)
		{
			switch (kind)
			{
			case media_stream_kind::video: return "video";
			case media_stream_kind::audio: return "audio";
			case media_stream_kind::subtitle: return "subtitle";
			case media_stream_kind::data: return "data";
			}

			return {};
		}

		// An element found in the file rather than held in memory.
		struct located
		{
			std::uint32_t id = 0;
			std::uint64_t offset = 0;
			std::uint64_t data = 0;
			// The payload's size, or unknown_size.
			std::uint64_t size = 0;
		};

		enum class block_use : std::uint8_t
		{
			none,
			// Read only where it costs nothing.
			cheap,
			needed,
		};

		struct track
		{
			media_stream stream;
			std::uint64_t uid = 0;
			std::uint64_t type = 0;
			std::string codec_id;
			std::vector<std::uint8_t> codec_private;
			pixel_crop crop;
			// What a muxer stripped from the front of every frame, and whether a frame can be read at all.
			std::vector<std::uint8_t> stripped;
			bool frames_readable = true;
			block_use block = block_use::none;
		};

		struct simple_tag
		{
			// The names of the SimpleTags it nests in and its own, joined by "/", as FFmpeg keys it.
			std::string key;
			std::string value;
			std::string language;
			bool binary = false;
		};

		struct tag_group
		{
			// The Tags element it came from, which puts the groups in file order.
			std::uint64_t offset = 0;
			std::string target_type;
			std::vector<std::uint64_t> track_uids;
			// Chapters and attachments, which the result does not describe.
			bool other_target = false;
			std::vector<simple_tag> tags;
		};

		struct info_fields
		{
			double duration = 0;
			std::uint64_t timestamp_scale = 1000000;
			std::string title;
			std::string muxing_app;
			std::string writing_app;
			std::optional<std::int64_t> date_utc;
		};

		struct seek_entry
		{
			std::uint32_t id = 0;
			std::uint64_t position = 0;
			bool done = false;
		};

		class matroska_scan
		{
		public:
			matroska_scan(reader& r, const scan_intent intent, media_budget& budget, media_scan_result& out) :
				_r(r), _intent(intent), _budget(budget), _out(out), _file_size(r.size())
			{
			}

			void run()
			{
				read_ebml_header();
				find_segment();
				walk_segment();

				// What lies before the first Cluster is read while the file's start is at hand, then its
				// first blocks, and only then what the SeekHeads place at the end.
				follow_seeks(true);
				read_first_blocks();
				follow_seeks(false);
				read_first_blocks();

				if (!_info) throw format_error("the Matroska segment has no readable Info");
				if (!_have_tracks) throw format_error("the Matroska segment has no readable Tracks");

				fill_result();
			}

		private:
			std::optional<located> header_at(const std::uint64_t offset, const std::uint64_t end)
			{
				if (offset >= end) return {};

				const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(end - offset, 12));
				const auto h = parse_header(_r.view(offset, available));
				if (!h) return {};
				return located{h->id, offset, offset + h->length, h->size};
			}

			// As much of an element's payload as lies in the segment, up to most.
			std::size_t held_size(const located& e, const std::size_t most) const
			{
				const auto available = std::min(e.size, _segment_end - e.data);
				return static_cast<std::size_t>(std::min<std::uint64_t>(available, most));
			}

			bool whole(const located& e) const
			{
				return e.size != unknown_size && e.size <= _segment_end - e.data;
			}

			void note(const located& e)
			{
				if (_intent == scan_intent::inspect && _elements.size() < max_top_level + max_seek_entries) _elements.push_back(e);
			}

			void read_ebml_header()
			{
				const auto h = header_at(0, _file_size);
				if (!h || h->id != id_ebml) throw format_error("the file has no EBML header");
				if (h->size == unknown_size || h->size > max_ebml_header || h->size > _file_size - h->data)
				{
					throw format_error("the EBML header cannot be read");
				}

				const auto payload = _r.read(h->data, static_cast<std::size_t>(h->size));
				std::uint64_t read_version = 1;
				std::uint64_t max_id_length = 4;
				std::uint64_t max_size_length = 8;
				children c(payload);

				while (const auto e = c.next())
				{
					switch (e->id)
					{
					case id_ebml_read_version: read_version = uint_or(e->data, 1);
						break;
					case id_ebml_max_id_length: max_id_length = uint_or(e->data, 4);
						break;
					case id_ebml_max_size_length: max_size_length = uint_or(e->data, 8);
						break;
					case id_doc_type: _doc_type = to_text(e->data);
						break;
					case id_doc_type_version: _doc_type_version = uint_or(e->data, 1);
						break;
					case id_doc_type_read_version: _doc_type_read_version = uint_or(e->data, 1);
						break;
					default: break;
					}
				}

				// What FFmpeg refuses: an EBML or a DocType it cannot read, and IDs or sizes longer than it parses.
				if (read_version > 1 || max_id_length > 4 || max_size_length > 8)
				{
					throw format_error("the file needs an EBML reader newer than version 1");
				}

				if (_doc_type_read_version > 3)
				{
					throw format_error(std::format("Matroska read version {} is not read", _doc_type_read_version));
				}

				_ebml_end = h->data + h->size;
			}

			void find_segment()
			{
				auto offset = _ebml_end;

				for (auto skipped = 0;; ++skipped)
				{
					const auto e = header_at(offset, _file_size);
					if (!e) break;

					if (e->id == id_segment)
					{
						_segment = *e;
						_segment_end = e->size == unknown_size || e->size > _file_size - e->data ? _file_size : e->data + e->size;
						return;
					}

					if ((e->id != id_void && e->id != id_crc32) || skipped >= 16 || e->size == unknown_size ||
						e->size > _file_size - e->data)
					{
						break;
					}

					offset = e->data + e->size;
				}

				throw format_error("the file has no Matroska segment");
			}

			// The Segment's top-level elements in order, by their headers alone, as far as the first Cluster.
			void walk_segment()
			{
				auto offset = _segment.data;

				for (std::size_t count = 0; count < max_top_level && offset < _segment_end; ++count)
				{
					const auto e = header_at(offset, _segment_end);
					if (!e) break;

					note(*e);

					if (e->id == id_cluster)
					{
						_first_cluster = e;
						break;
					}

					if (e->size == unknown_size) break;

					read_top_level(*e);
					if (e->size > _segment_end - e->data) break;
					offset = e->data + e->size;
				}
			}

			// The elements the SeekHeads point at. A SeekHead read on the way adds its entries, as the one
			// many muxers leave at the end of the file does. As FFmpeg does, an element is read by the ID
			// found where an entry points, not the ID the entry names.
			void follow_seeks(const bool before_clusters)
			{
				for (std::size_t i = 0; i < _seeks.size(); ++i)
				{
					const auto entry = _seeks[i];
					if (entry.done) continue;
					if (before_clusters && _first_cluster && entry.position > _first_cluster->offset) continue;

					_seeks[i].done = true;

					const auto wanted = entry.id == id_info || entry.id == id_tracks || entry.id == id_tags ||
						entry.id == id_attachments || entry.id == id_seek_head;
					if (!wanted && _intent != scan_intent::inspect) continue;

					const auto e = header_at(entry.position, _segment_end);
					if (!e) continue;

					if (std::ranges::none_of(_elements, [&](const located& seen) { return seen.offset == e->offset; })) note(*e);
					if (wanted) read_top_level(*e);
				}
			}

			void read_top_level(const located& e)
			{
				if (e.id != id_seek_head && e.id != id_info && e.id != id_tracks && e.id != id_tags && e.id != id_attachments) return;
				if (e.size == unknown_size || _read.size() >= max_element_reads || std::ranges::find(_read, e.offset) != _read.end()) return;

				_read.push_back(e.offset);

				switch (e.id)
				{
				case id_seek_head: read_seek_head(e);
					break;
				case id_info: read_info(e);
					break;
				case id_tracks: read_tracks(e);
					break;
				case id_tags: read_tags(e);
					break;
				case id_attachments: read_attachments(e);
					break;
				default: break;
				}
			}

			void read_seek_head(const located& e)
			{
				const auto payload = _r.read(e.data, held_size(e, max_small_element));
				children c(payload);

				while (const auto seek = c.next())
				{
					if (seek->id != id_seek) continue;

					std::optional<std::uint32_t> id;
					std::optional<std::uint64_t> position;
					children fields(seek->data);

					while (const auto f = fields.next())
					{
						if (f->id == id_seek_id && f->data.size() <= 4) id = static_cast<std::uint32_t>(uint_or(f->data, 0));
						else if (f->id == id_seek_position) position = to_uint(f->data);
					}

					if (!id || !position || *position >= _segment_end - _segment.data) continue;
					if (_seeks.size() >= max_seek_entries) break;
					_seeks.push_back({*id, _segment.data + *position});
				}
			}

			void read_info(const located& e)
			{
				if (_info || !whole(e) || e.size > max_small_element) return;

				const auto payload = _r.read(e.data, static_cast<std::size_t>(e.size));
				info_fields info;
				std::optional<double> duration;
				children c(payload);

				while (const auto x = c.next())
				{
					switch (x->id)
					{
					case id_timestamp_scale:
						if (const auto v = to_uint(x->data); v && *v != 0) info.timestamp_scale = *v;
						break;
					case id_duration: duration = to_float(x->data);
						break;
					case id_title: info.title = to_text(x->data);
						break;
					case id_muxing_app: info.muxing_app = to_text(x->data);
						break;
					case id_writing_app: info.writing_app = to_text(x->data);
						break;
					case id_date_utc:
						if (x->data.size() == 8) info.date_utc = static_cast<std::int64_t>(*to_uint(x->data));
						break;
					default: break;
					}
				}

				if (duration && std::isfinite(*duration) && *duration > 0)
				{
					info.duration = *duration * static_cast<double>(info.timestamp_scale) / 1e9;
				}

				_info = std::move(info);
			}

			void read_tracks(const located& e)
			{
				if (_have_tracks || !whole(e) || e.size > max_held_element) return;

				const auto payload = _r.read(e.data, static_cast<std::size_t>(e.size));
				std::vector<track> tracks;
				children c(payload);

				while (const auto entry = c.next())
				{
					if (entry->id != id_track_entry) continue;
					if (tracks.size() >= max_tracks) break;
					tracks.push_back(read_track_entry(entry->data));
				}

				// Damage after a track keeps the tracks before it; damage before any leaves none to keep.
				if (c.damaged() && tracks.empty()) return;

				_tracks = std::move(tracks);
				_have_tracks = true;
			}

			track read_track_entry(const std::span<const std::uint8_t> entry)
			{
				track t;
				auto& s = t.stream;
				s.is_default = true;
				std::string language = "eng";
				std::string language_bcp47;
				std::uint64_t default_duration = 0;
				std::span<const std::uint8_t> video;
				std::span<const std::uint8_t> audio;
				std::span<const std::uint8_t> encodings;
				children c(entry);

				while (const auto e = c.next())
				{
					switch (e->id)
					{
					case id_track_number: s.id = uint_or(e->data, 0);
						break;
					case id_track_uid: t.uid = uint_or(e->data, 0);
						break;
					case id_track_type: t.type = uint_or(e->data, 0);
						break;
					case id_flag_default: s.is_default = uint_or(e->data, 1) != 0;
						break;
					case id_flag_commentary: s.commentary = uint_or(e->data, 0) != 0;
						break;
					case id_flag_visual_impaired: s.audio_description = uint_or(e->data, 0) != 0;
						break;
					case id_default_duration: default_duration = uint_or(e->data, 0);
						break;
					case id_name: s.title = to_text(e->data);
						break;
					case id_language: language = to_text(e->data);
						break;
					case id_language_bcp47: language_bcp47 = to_text(e->data);
						break;
					case id_codec_id: t.codec_id = to_text(e->data);
						break;
					case id_codec_private: t.codec_private.assign(e->data.begin(), e->data.end());
						break;
					case id_video: video = e->data;
						break;
					case id_audio: audio = e->data;
						break;
					case id_content_encodings: encodings = e->data;
						break;
					default: break;
					}
				}

				// LanguageBCP47, where there is one, replaces Language; "und" names no language at all.
				s.language = !language_bcp47.empty() ? language_bcp47 : !language.empty() ? language : "eng";
				if (s.language == "und") s.language.clear();
				s.kind = t.type == 1 ? media_stream_kind::video : t.type == 2 ? media_stream_kind::audio
					         : t.type == 0x11 ? media_stream_kind::subtitle : media_stream_kind::data;
				s.codec_tag = t.codec_id;

				if (codec_fits_kind(t.codec_id, s.kind)) s.codec = codec_from_id(t.codec_id);
				if (s.codec == "webvtt") s.kind = media_stream_kind::subtitle;

				read_encodings(encodings, t);

				if (s.kind == media_stream_kind::video)
				{
					std::span<const std::uint8_t> extra = t.codec_private;

					// A Video for Windows track names its codec in a BITMAPINFOHEADER, whose own data follows it.
					if (t.codec_id == "V_MS/VFW/FOURCC")
					{
						if (t.codec_private.size() >= 40) read_bitmap_info(t.codec_private, s);
						extra = extra.size() >= 40 ? extra.subspan(40) : std::span<const std::uint8_t>();
					}

					const auto fields = read_video(video, s.video);
					t.crop = fields.crop;
					if (default_duration > 0) s.video.frame_rate = 1e9 / static_cast<double>(default_duration);

					// An uncompressed track names its layout with a FourCC, as FFmpeg tags it.
					if (t.codec_id == "V_UNCOMPRESSED" && fields.fourcc) s.codec_tag = fourcc_text(*fields.fourcc);
					describe_video(t, extra);
				}
				else if (s.kind == media_stream_kind::audio)
				{
					const auto bit_depth = read_audio(audio, s.audio);

					// An ACM track names its codec in a WAVEFORMATEX; FFmpeg keeps the container's rate.
					if (t.codec_id == "A_MS/ACM")
					{
						const auto rate = s.audio.sample_rate;
						const auto channels = s.audio.channels;

						if (read_wave_format(t.codec_private, s))
						{
							s.audio.sample_rate = rate;
							if (s.audio.channels == 0) s.audio.channels = channels;
						}
					}
					else
					{
						describe_audio(t, bit_depth);
					}
				}

				return t;
			}

			// A ContentEncoding undoes what a muxer did to the frames or the CodecPrivate: header stripping
			// is undone here, a compressed CodecPrivate inflated, and frames otherwise compressed or
			// encrypted are left unread. More than one encoding, which FFmpeg does not combine, leaves
			// the frames unread too.
			void read_encodings(const std::span<const std::uint8_t> data, track& t)
			{
				std::size_t count = 0;
				std::uint64_t scope = 1;
				std::uint64_t type = 0;
				std::uint64_t algorithm = 0;
				std::span<const std::uint8_t> settings;
				children c(data);

				while (const auto e = c.next())
				{
					if (e->id != id_content_encoding || ++count > 1) continue;

					children fields(e->data);

					while (const auto f = fields.next())
					{
						if (f->id == id_content_encoding_scope) scope = uint_or(f->data, 1);
						else if (f->id == id_content_encoding_type) type = uint_or(f->data, 0);
						else if (f->id == id_content_compression)
						{
							children compression(f->data);

							while (const auto x = compression.next())
							{
								if (x->id == id_content_comp_algo) algorithm = uint_or(x->data, 0);
								else if (x->id == id_content_comp_settings) settings = x->data;
							}
						}
					}
				}

				if (count == 0) return;

				if (count > 1 || type != 0)
				{
					if (count > 1 || (scope & 1)) t.frames_readable = false;
					return;
				}

				if (algorithm == 3)
				{
					if (scope & 1) t.stripped.assign(settings.begin(), settings.end());
					if (scope & 2) t.codec_private.insert(t.codec_private.begin(), settings.begin(), settings.end());
					return;
				}

				if (scope & 1) t.frames_readable = false;

				if ((scope & 2) && !t.codec_private.empty())
				{
					// Within what the scan may inflate in all; what fails or would pass it holds nothing.
					std::vector<std::uint8_t> inflated;
					const auto limit = _budget.inflate_limit(max_held_element);
					if (algorithm != 0 || limit == 0 || !zlib_inflate(t.codec_private, inflated, limit)) inflated = std::vector<std::uint8_t>();
					_budget.inflated(inflated.size());
					t.codec_private = std::move(inflated);
				}
			}

			// The codec header in a video track's private data, and whether its first block is needed for
			// what only the bitstream says.
			void describe_video(track& t, const std::span<const std::uint8_t> extra)
			{
				auto& s = t.stream;
				const std::string_view codec = s.codec;
				video_header h;
				auto parsed = false;

				if (codec == "h264") parsed = !extra.empty() && extra[0] == 1 ? parse_avc_config(extra, h) : find_parameter_set(extra, false, h);
				else if (codec == "hevc")
				{
					// FFmpeg's rule for telling an hvcC from an Annex B stream.
					const auto config = extra.size() > 3 && (extra[0] != 0 || extra[1] != 0 || extra[2] > 1);
					parsed = config ? parse_hevc_config(extra, h) : find_parameter_set(extra, true, h);
				}
				else if (codec == "av1") parsed = parse_av1_config(extra, h);
				else if (codec == "vp9") parsed = read_vp9_features(extra, h);
				else if (codec == "mpeg4") parsed = parse_mpeg4_video(extra, h);
				else if (codec == "mpeg1" || codec == "mpeg2") parsed = parse_mpeg12_video(extra, h);
				else if (codec == "theora")
				{
					const auto packet = first_xiph_packet(extra);
					parsed = packet && parse_theora_header(*packet, h);
				}

				if (parsed) apply_video_header(h, s.video);
				if (!t.frames_readable) return;

				const auto& v = s.video;

				if (codec == "h264" || codec == "hevc" || codec == "av1" || codec == "mpeg4" || codec == "mpeg1" || codec == "mpeg2")
				{
					if (!parsed) t.block = block_use::needed;
				}
				else if (codec == "vp9" || codec == "mjpeg")
				{
					if (v.chroma == chroma_format::unknown || v.bit_depth == 0) t.block = block_use::needed;
				}
				else if (codec == "vp8")
				{
					if (v.width == 0 || v.height == 0) t.block = block_use::needed;
				}
			}

			// The coded sample format for PCM and lossless codecs, and the codec header in the private data.
			void describe_audio(track& t, const std::uint64_t bit_depth)
			{
				auto& s = t.stream;
				const std::string_view codec = s.codec;
				const std::span<const std::uint8_t> data = t.codec_private;

				if (codec == "pcm")
				{
					// FFmpeg reads PCM without a BitDepth as 16-bit integers or 32-bit floats.
					s.audio.float_samples = t.codec_id.starts_with("A_PCM/FLOAT");
					s.audio.bits_per_sample = static_cast<std::uint8_t>(bit_depth != 0 ? bit_depth : s.audio.float_samples ? 32 : 16);
				}
				else if (is_lossless(codec) && bit_depth != 0)
				{
					s.audio.bits_per_sample = static_cast<std::uint8_t>(bit_depth);
				}

				audio_header h;
				auto parsed = false;

				if (codec == "aac") parsed = parse_aac_config(data, h);
				else if (codec == "opus")
				{
					std::uint32_t pre_skip = 0;
					parsed = parse_opus_head(data, h, pre_skip);
				}
				else if (codec == "vorbis")
				{
					const auto packet = first_xiph_packet(data);
					parsed = packet && parse_vorbis_header(*packet, h);
				}
				else if (codec == "flac")
				{
					// "fLaC", then the STREAMINFO block's header and its 34 bytes.
					std::uint64_t total_samples = 0;
					parsed = data.size() >= 42 && starts_with(data, "fLaC") && (data[4] & 0x7F) == 0 &&
						parse_flac_streaminfo(data.subspan(8, 34), h, total_samples);
				}
				else if (codec == "alac") parsed = parse_alac_config(data, h);

				if (parsed) apply_audio_header(h, s.audio);
				else if (codec == "aac") s.audio.profile = aac_profile_from_id(t.codec_id);

				if (data.empty() && t.frames_readable && (codec == "ac3" || codec == "eac3" || codec == "dts")) t.block = block_use::cheap;
			}

			void read_tags(const located& e)
			{
				const auto payload = _r.read(e.data, held_size(e, max_held_element));
				children c(payload);

				while (const auto tag = c.next())
				{
					if (_tag_count >= max_tags) break;
					if (tag->id != id_tag) continue;

					tag_group group;
					group.offset = e.offset;
					std::vector<std::span<const std::uint8_t>> simple;
					children parts(tag->data);

					while (const auto p = parts.next())
					{
						if (p->id == id_targets) read_targets(p->data, group);
						else if (p->id == id_simple_tag) simple.push_back(p->data);
					}

					for (const auto s : simple) flatten(s, {}, 0, group.tags);
					if (!group.tags.empty()) _tag_groups.push_back(std::move(group));
				}
			}

			static void read_targets(const std::span<const std::uint8_t> data, tag_group& group)
			{
				children c(data);

				while (const auto e = c.next())
				{
					const auto v = uint_or(e->data, 0);

					switch (e->id)
					{
					case id_target_type: group.target_type = to_text(e->data);
						break;
					case id_tag_track_uid:
						if (v != 0 && group.track_uids.size() < max_tracks) group.track_uids.push_back(v);
						break;
					case id_tag_chapter_uid:
					case id_tag_attachment_uid:
						if (v != 0) group.other_target = true;
						break;
					default: break;
					}
				}
			}

			// A SimpleTag and those nested in it, each keyed by the names it nests in as FFmpeg keys them,
			// "ARTIST/SORT_WITH". A tag without a name is skipped with what it holds, as FFmpeg skips it.
			void flatten(const std::span<const std::uint8_t> data, const std::string& parent, const int depth, std::vector<simple_tag>& out)
			{
				std::string name;
				std::string value;
				std::string language;
				std::string language_bcp47;
				auto has_value = false;
				std::optional<std::size_t> binary;
				std::vector<std::span<const std::uint8_t>> nested;
				children c(data);

				while (const auto e = c.next())
				{
					switch (e->id)
					{
					case id_tag_name: name = to_text(e->data);
						break;
					case id_tag_string:
						value = to_text(e->data);
						has_value = true;
						break;
					case id_tag_binary: binary = e->data.size();
						break;
					case id_tag_language: language = to_text(e->data);
						break;
					case id_tag_language_bcp47: language_bcp47 = to_text(e->data);
						break;
					case id_simple_tag: nested.push_back(e->data);
						break;
					default: break;
					}
				}

				if (name.empty() || parent.size() + 1 + name.size() > max_tag_key) return;

				const auto key = parent.empty() ? name : parent + "/" + name;
				auto tag_language = !language_bcp47.empty() ? language_bcp47 : language;
				if (tag_language == "und") tag_language.clear();

				if (_tag_count >= max_tags) return;

				if (has_value || binary)
				{
					++_tag_count;
					if (has_value) out.push_back({key, std::move(value), tag_language, false});
					else out.push_back({key, std::format("{} bytes of binary data", *binary), tag_language, true});
				}

				if (depth + 1 >= max_tag_depth) return;
				for (const auto n : nested) flatten(n, key, depth + 1, out);
			}

			// The attached files by their headers. FileData is located, never read.
			void read_attachments(const located& e)
			{
				const auto end = e.data + std::min(e.size, _segment_end - e.data);
				auto offset = e.data;

				while (_attached_files < max_attached_files && offset < end)
				{
					const auto file = header_at(offset, end);
					if (!file || file->size == unknown_size) break;

					++_attached_files;
					if (file->id == id_attached_file) read_attached_file(*file, file->data + std::min(file->size, end - file->data));
					if (file->size > end - file->data) break;
					offset = file->data + file->size;
				}
			}

			void read_attached_file(const located& file, const std::uint64_t end)
			{
				std::string name;
				std::string mime;
				std::string description;
				std::optional<byte_range> data;
				auto offset = file.data;

				for (std::size_t n = 0; n < max_attached_file_children && offset < end; ++n)
				{
					const auto e = header_at(offset, end);
					if (!e || e->size == unknown_size || e->size > end - e->data) break;

					const auto text = [&]
					{
						return e->size <= max_attachment_text ? to_text(_r.read(e->data, static_cast<std::size_t>(e->size))) : std::string();
					};

					switch (e->id)
					{
					case id_file_name: name = text();
						break;
					case id_file_mime_type: mime = text();
						break;
					case id_file_description: description = text();
						break;
					case id_file_data: data = byte_range{e->data, e->size};
						break;
					default: break;
					}

					offset = e->data + e->size;
				}

				if (!data || data->empty() || mime.size() < 6 || !equal_ignoring_case(std::string_view(mime).substr(0, 6), "image/")) return;

				media_picture picture;
				picture.location = *data;
				picture.mime_type = std::move(mime);
				picture.type = is_cover_name(name) ? 3 : 0;
				picture.description = description.empty() ? std::move(name) : std::move(description);
				_pictures.push_back(std::move(picture));
			}

			// The first block of each track that needs one, from the start of the first Cluster.
			void read_first_blocks()
			{
				if (_blocks_read || !_have_tracks || !_first_cluster) return;
				_blocks_read = true;

				for (const auto& t : _tracks)
				{
					if (t.block == block_use::needed) ++_needed;
					else if (t.block == block_use::cheap) ++_cheap;
				}

				const auto& cluster = *_first_cluster;
				if (_needed == 0 && (_cheap == 0 || cluster.data >= cheap_span)) return;

				const auto unknown = cluster.size == unknown_size;
				const auto end = unknown || cluster.size > _segment_end - cluster.data ? _segment_end : cluster.data + cluster.size;
				auto offset = cluster.data;

				for (std::size_t n = 0; n < max_cluster_children && offset < end && (_needed > 0 || _cheap > 0); ++n)
				{
					if (offset >= (_needed > 0 ? cluster.data + needed_block_span : cheap_span)) break;

					const auto e = header_at(offset, end);
					if (!e || e->size == unknown_size) break;

					// A Cluster of unknown size ends where an element that cannot be its child begins.
					const auto in_cluster = e->id == id_simple_block || e->id == id_block_group || e->id == id_cluster_timestamp ||
						e->id == id_cluster_position || e->id == id_cluster_prev_size || e->id == id_silent_tracks ||
						e->id == id_encrypted_block || e->id == id_void || e->id == id_crc32;
					if (unknown && !in_cluster) break;

					const auto size = std::min(e->size, end - e->data);
					if (e->id == id_simple_block) take_block(e->data, size);
					else if (e->id == id_block_group) take_block_group(e->data, e->data + size);

					if (e->size > end - e->data) break;
					offset = e->data + e->size;
				}
			}

			void take_block_group(std::uint64_t offset, const std::uint64_t end)
			{
				for (std::size_t n = 0; n < max_block_group_children && offset < end; ++n)
				{
					const auto e = header_at(offset, end);
					if (!e || e->size == unknown_size) return;

					if (e->id == id_block)
					{
						take_block(e->data, std::min(e->size, end - e->data));
						return;
					}

					if (e->size > end - e->data) return;
					offset = e->data + e->size;
				}
			}

			// A block's track, then its first frame - past the lace sizes when it is laced - with what the
			// muxer stripped put back in front.
			void take_block(const std::uint64_t offset, const std::uint64_t size)
			{
				const auto head = _r.read(offset, static_cast<std::size_t>(std::min<std::uint64_t>(size, max_block_header)));
				if (head.empty() || head[0] == 0) return;

				const auto number_length = static_cast<std::size_t>(std::countl_zero(head[0])) + 1;
				if (head.size() < number_length + 3) return;

				std::uint64_t number = head[0] & (0xFFu >> number_length);
				for (std::size_t i = 1; i < number_length; ++i) number = number << 8 | head[i];

				const auto found = std::ranges::find_if(_tracks, [&](const track& t) { return t.stream.id == number && t.block != block_use::none; });
				if (found == _tracks.end()) return;

				// Only a track's first block is read, whatever it holds.
				auto& t = *found;
				if (t.block == block_use::needed) --_needed;
				else --_cheap;
				const auto use = t.block;
				t.block = block_use::none;

				const auto lacing = (head[number_length + 2] >> 1) & 3;
				auto pos = number_length + 3;
				std::uint64_t frame_size = size - pos;

				if (lacing != 0)
				{
					if (pos >= head.size()) return;
					const std::size_t laces = head[pos++] + 1u;
					std::uint64_t first = 0;

					// Fixed lacing divides the block evenly. Xiph and EBML lacing give every size but the
					// last, which are walked to find where the frames begin.
					for (std::size_t i = 0; lacing != 2 && i + 1 < laces; ++i)
					{
						std::uint64_t lace = 0;

						if (lacing == 1)
						{
							for (;;)
							{
								if (pos >= head.size()) return;
								const auto b = head[pos++];
								lace += b;
								if (b < 255) break;
							}
						}
						else
						{
							// The sizes after the first are signed differences, only skipped here.
							if (pos >= head.size() || head[pos] == 0) return;
							const auto length = static_cast<std::size_t>(std::countl_zero(head[pos])) + 1;
							if (head.size() - pos < length) return;
							lace = head[pos] & (0xFFu >> length);
							for (std::size_t j = 1; j < length; ++j) lace = lace << 8 | head[pos + j];
							pos += length;
						}

						if (i == 0) first = lace;
					}

					if (pos > size) return;
					if (lacing == 2) frame_size = (size - pos) / laces;
					else if (laces > 1) frame_size = first;
					else frame_size = size - pos;
				}

				if (pos > size || frame_size > size - pos) return;

				const auto video = t.stream.kind == media_stream_kind::video;
				auto wanted = std::min<std::uint64_t>(frame_size, video ? video_frame_head : audio_frame_head);
				if (use == block_use::cheap && _needed == 0 && offset + pos + wanted > cheap_span) return;

				for (;;)
				{
					auto frame = t.stripped;
					const auto bytes = _r.read(offset + pos, static_cast<std::size_t>(wanted));
					frame.insert(frame.end(), bytes.begin(), bytes.end());

					if (apply_frame(t, frame) || !video || wanted == frame_size || wanted >= video_frame_most) return;
					wanted = std::min<std::uint64_t>(frame_size, video_frame_most);
				}
			}

			bool apply_frame(track& t, const std::span<const std::uint8_t> frame)
			{
				auto& s = t.stream;
				const std::string_view codec = s.codec;

				if (s.kind == media_stream_kind::video)
				{
					video_header h;
					auto parsed = false;

					if (codec == "h264" || codec == "hevc") parsed = find_parameter_set(frame, codec == "hevc", h);
					else if (codec == "av1") parsed = parse_av1_sequence(frame, h);
					else if (codec == "vp9") parsed = parse_vp9_frame(frame, h);
					else if (codec == "vp8") parsed = parse_vp8_frame(frame, h);
					else if (codec == "mpeg4") parsed = parse_mpeg4_video(frame, h);
					else if (codec == "mpeg1" || codec == "mpeg2") parsed = parse_mpeg12_video(frame, h);
					else if (codec == "mjpeg") parsed = parse_jpeg_frame(frame, h);

					if (parsed) apply_video_header(h, s.video);
					return parsed;
				}

				audio_header h;
				std::string name;
				auto parsed = false;

				if (codec == "ac3" || codec == "eac3") parsed = parse_ac3_frame(frame, h, name);
				else if (codec == "dts") parsed = parse_dts_frame(frame, h);
				if (!parsed) return false;

				// An A_AC3 track can hold E-AC-3, which only its frames tell.
				if (!name.empty()) s.codec = name;
				apply_audio_header(h, s.audio);
				return true;
			}

			// A video track's last word: the container's crop applied to whatever size stands, and the
			// chroma and depth that codecs like VP8 fix, where nothing has stated them.
			static void finish_video(track& t)
			{
				auto& v = t.stream.video;
				const auto& crop = t.crop;

				if (v.width > crop.left + crop.right && v.height > crop.top + crop.bottom)
				{
					v.width -= static_cast<std::uint32_t>(crop.left + crop.right);
					v.height -= static_cast<std::uint32_t>(crop.top + crop.bottom);
				}

				if (v.chroma == chroma_format::unknown && !t.stream.codec.empty())
				{
					video_header h;
					if (fixed_video_format(t.stream.codec, h)) apply_video_header(h, v);
				}
			}

			void add_stream_tag(media_stream& s, const simple_tag& tag)
			{
				const auto value = trim_text(tag.value);
				if (value.empty() || _stream_tag_count >= max_tags) return;

				++_stream_tag_count;
				s.tags.push_back({tag.key, std::string(value), tag.language});
				if (tag.binary) return;

				// mkvmerge's and FFmpeg's statistics tags state what the container has no element for.
				if (tag.key == "DURATION")
				{
					if (const auto seconds = parse_duration_tag(value)) s.duration = *seconds;
				}
				else if (tag.key == "BPS")
				{
					std::uint64_t bits = 0;
					const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), bits);
					if (error == std::errc() && end == value.data() + value.size()) s.bit_rate = bits;
				}
			}

			// As FFmpeg routes a Tag: by its attachment or chapter, which the result does not describe; by
			// its track, to that stream; else to the file, keyed under its TargetType when it names one.
			void route(const tag_group& group)
			{
				if (group.other_target) return;

				if (!group.track_uids.empty())
				{
					for (auto& t : _tracks)
					{
						if (t.uid == 0 || std::ranges::find(group.track_uids, t.uid) == group.track_uids.end()) continue;
						for (const auto& tag : group.tags) add_stream_tag(t.stream, tag);
					}

					return;
				}

				for (const auto& tag : group.tags)
				{
					const auto key = group.target_type.empty() ? tag.key : group.target_type + "/" + tag.key;
					if (key.size() > max_tag_key) continue;

					if (!tag.binary)
					{
						add_tag(_out, tag_family::matroska, key, tag.value, tag.language);
					}
					else if (const auto value = trim_text(tag.value); !value.empty())
					{
						// A binary value is listed, described, but stands for no property.
						_out.tags.push_back({key, std::string(value), tag.language});
					}
				}
			}

			void fill_result()
			{
				const auto& info = *_info;
				_out.brand = _doc_type;
				_out.duration = info.duration;

				// FFmpeg reports MuxingApp as the encoder and ignores WritingApp. apply_tag lets a later tag
				// replace an earlier one, as FFmpeg's dictionary does, so MuxingApp comes after WritingApp -
				// and a global ENCODER SimpleTag, which FFmpeg's muxer writes, after both, as in FFmpeg.
				add_tag(_out, tag_family::matroska, "TITLE", info.title);
				if (info.writing_app != info.muxing_app) add_tag(_out, tag_family::matroska, "ENCODER", info.writing_app);
				add_tag(_out, tag_family::matroska, "ENCODER", info.muxing_app);
				if (info.date_utc) _out.properties.created = date_from_nanoseconds(*info.date_utc);

				std::ranges::stable_sort(_tag_groups, {}, &tag_group::offset);
				for (const auto& group : _tag_groups) route(group);

				for (auto& t : _tracks)
				{
					if (t.stream.kind == media_stream_kind::video) finish_video(t);
				}

				if (_intent == scan_intent::inspect) _out.structure = structure();

				_out.streams.reserve(_tracks.size());
				for (auto& t : _tracks) _out.streams.push_back(std::move(t.stream));
				_out.pictures = std::move(_pictures);
			}

			std::vector<structure_row> structure()
			{
				std::vector<structure_row> rows;
				const auto& info = *_info;

				add_structure_section(rows, "Summary", "mkv.summary", true);
				add_structure_row(rows, "File size", std::format("{} bytes", _file_size));
				add_structure_row(rows, "DocType", std::format("{}, version {}, read version {}", _doc_type, _doc_type_version, _doc_type_read_version));
				add_structure_row(rows, "Segment", _segment.size == unknown_size
					                                   ? std::format("offset {}, size unknown", _segment.offset)
					                                   : std::format("offset {}, {} bytes", _segment.offset, _segment.data - _segment.offset + _segment.size));
				add_structure_row(rows, "Timestamp scale", std::format("{} ns", info.timestamp_scale));
				if (info.duration > 0) add_structure_row(rows, "Duration", std::format("{:.3f} s", info.duration));
				if (!info.muxing_app.empty()) add_structure_row(rows, "Muxing application", info.muxing_app);
				if (!info.writing_app.empty()) add_structure_row(rows, "Writing application", info.writing_app);

				add_structure_section(rows, "Segment elements", "mkv.elements");
				std::ranges::sort(_elements, {}, &located::offset);

				for (std::size_t i = 0; i < _elements.size(); ++i)
				{
					const auto& e = _elements[i];
					if (i > 0 && _elements[i - 1].offset == e.offset) continue;

					auto value = e.size == unknown_size
						             ? std::format("offset {}, size unknown", e.offset)
						             : std::format("offset {}, {} bytes", e.offset, e.data - e.offset + e.size);
					if (_first_cluster && e.offset == _first_cluster->offset) value += ", the first cluster";

					auto& row = add_row(rows, element_name(e.id), std::move(value));
					row.depth = 1;
					row.id = std::format("mkv.element.{}", e.offset);
				}

				add_structure_section(rows, "Tracks", "mkv.tracks");

				for (const auto& t : _tracks)
				{
					const auto& s = t.stream;
					auto value = std::format("{}, {}", kind_name(s.kind), t.codec_id.empty() ? std::string("no codec") : t.codec_id);

					if (s.kind == media_stream_kind::video && s.video.width != 0) value += std::format(", {} x {}", s.video.width, s.video.height);
					if (s.kind == media_stream_kind::audio) value += std::format(", {} Hz, {} channels", s.audio.sample_rate, s.audio.channels);
					if (!s.language.empty()) value += ", " + s.language;

					auto& row = add_row(rows, std::format("Track {}", s.id), std::move(value));
					row.depth = 1;
					row.id = std::format("mkv.track.{}", s.id);
				}

				finish_structure_sections(rows);
				return rows;
			}

			reader& _r;
			scan_intent _intent;
			media_budget& _budget;
			media_scan_result& _out;
			std::uint64_t _file_size = 0;

			std::string _doc_type;
			std::uint64_t _doc_type_version = 1;
			std::uint64_t _doc_type_read_version = 1;
			std::uint64_t _ebml_end = 0;
			located _segment;
			std::uint64_t _segment_end = 0;

			std::optional<located> _first_cluster;
			std::vector<seek_entry> _seeks;
			// The top-level elements read, by offset, and those seen, for the structure listing.
			std::vector<std::uint64_t> _read;
			std::vector<located> _elements;

			std::optional<info_fields> _info;
			std::vector<track> _tracks;
			bool _have_tracks = false;
			std::vector<tag_group> _tag_groups;
			std::size_t _tag_count = 0;
			std::size_t _stream_tag_count = 0;
			std::size_t _attached_files = 0;
			std::vector<media_picture> _pictures;

			bool _blocks_read = false;
			std::size_t _needed = 0;
			std::size_t _cheap = 0;
		};
	}

	void scan_matroska_media(reader& r, const scan_intent intent, media_budget& budget, media_scan_result& out)
	{
		matroska_scan(r, intent, budget, out).run();
	}
}
