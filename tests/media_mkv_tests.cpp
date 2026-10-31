// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the Matroska and WebM media scan. Files are built element by element - tracks of
// every kind, colour and display, tags at every level, attachments, SeekHeads that point past the
// clusters - and read whole, cut short and corrupted, with what their reads cost. The codec headers
// are ones FFmpeg wrote.

#include "test.h"
#include "builders.h"

#include "libfiles/media.h"
#include "media_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using namespace test_build;
	using libfiles::chroma_format;
	using libfiles::media_scan_result;
	using libfiles::media_stream;
	using libfiles::media_stream_kind;

	// Element IDs as RFC 8794 and RFC 9559 give them.
	namespace id
	{
		constexpr std::uint32_t ebml = 0x1A45DFA3;
		constexpr std::uint32_t ebml_version = 0x4286;
		constexpr std::uint32_t ebml_read_version = 0x42F7;
		constexpr std::uint32_t ebml_max_id_length = 0x42F2;
		constexpr std::uint32_t ebml_max_size_length = 0x42F3;
		constexpr std::uint32_t doc_type = 0x4282;
		constexpr std::uint32_t doc_type_version = 0x4287;
		constexpr std::uint32_t doc_type_read_version = 0x4285;
		constexpr std::uint32_t void_element = 0xEC;
		constexpr std::uint32_t crc32 = 0xBF;

		constexpr std::uint32_t segment = 0x18538067;
		constexpr std::uint32_t seek_head = 0x114D9B74;
		constexpr std::uint32_t seek = 0x4DBB;
		constexpr std::uint32_t seek_id = 0x53AB;
		constexpr std::uint32_t seek_position = 0x53AC;
		constexpr std::uint32_t info = 0x1549A966;
		constexpr std::uint32_t timestamp_scale = 0x2AD7B1;
		constexpr std::uint32_t duration = 0x4489;
		constexpr std::uint32_t date_utc = 0x4461;
		constexpr std::uint32_t title = 0x7BA9;
		constexpr std::uint32_t muxing_app = 0x4D80;
		constexpr std::uint32_t writing_app = 0x5741;
		constexpr std::uint32_t tracks = 0x1654AE6B;
		constexpr std::uint32_t cluster = 0x1F43B675;
		constexpr std::uint32_t cues = 0x1C53BB6B;
		constexpr std::uint32_t chapters = 0x1043A770;
		constexpr std::uint32_t tags = 0x1254C367;
		constexpr std::uint32_t attachments = 0x1941A469;

		constexpr std::uint32_t track_entry = 0xAE;
		constexpr std::uint32_t track_number = 0xD7;
		constexpr std::uint32_t track_uid = 0x73C5;
		constexpr std::uint32_t track_type = 0x83;
		constexpr std::uint32_t flag_default = 0x88;
		constexpr std::uint32_t flag_visual_impaired = 0x55AC;
		constexpr std::uint32_t flag_commentary = 0x55AF;
		constexpr std::uint32_t default_duration = 0x23E383;
		constexpr std::uint32_t name = 0x536E;
		constexpr std::uint32_t language = 0x22B59C;
		constexpr std::uint32_t language_bcp47 = 0x22B59D;
		constexpr std::uint32_t codec_id = 0x86;
		constexpr std::uint32_t codec_private = 0x63A2;
		constexpr std::uint32_t video = 0xE0;
		constexpr std::uint32_t audio = 0xE1;
		constexpr std::uint32_t content_encodings = 0x6D80;
		constexpr std::uint32_t content_encoding = 0x6240;
		constexpr std::uint32_t content_encoding_scope = 0x5032;
		constexpr std::uint32_t content_encoding_type = 0x5033;
		constexpr std::uint32_t content_compression = 0x5034;
		constexpr std::uint32_t content_comp_algo = 0x4254;
		constexpr std::uint32_t content_comp_settings = 0x4255;

		constexpr std::uint32_t flag_interlaced = 0x9A;
		constexpr std::uint32_t stereo_mode = 0x53B8;
		constexpr std::uint32_t alpha_mode = 0x53C0;
		constexpr std::uint32_t pixel_width = 0xB0;
		constexpr std::uint32_t pixel_height = 0xBA;
		constexpr std::uint32_t pixel_crop_bottom = 0x54AA;
		constexpr std::uint32_t pixel_crop_top = 0x54BB;
		constexpr std::uint32_t pixel_crop_left = 0x54CC;
		constexpr std::uint32_t pixel_crop_right = 0x54DD;
		constexpr std::uint32_t display_width = 0x54B0;
		constexpr std::uint32_t display_height = 0x54BA;
		constexpr std::uint32_t display_unit = 0x54B2;
		constexpr std::uint32_t uncompressed_fourcc = 0x2EB524;
		constexpr std::uint32_t frame_rate = 0x2383E3;
		constexpr std::uint32_t colour = 0x55B0;
		constexpr std::uint32_t matrix_coefficients = 0x55B1;
		constexpr std::uint32_t bits_per_channel = 0x55B2;
		constexpr std::uint32_t chroma_subsampling_horz = 0x55B3;
		constexpr std::uint32_t chroma_subsampling_vert = 0x55B4;
		constexpr std::uint32_t range = 0x55B9;
		constexpr std::uint32_t transfer_characteristics = 0x55BA;
		constexpr std::uint32_t primaries = 0x55BB;
		constexpr std::uint32_t projection = 0x7670;
		constexpr std::uint32_t projection_type = 0x7671;
		constexpr std::uint32_t projection_pose_yaw = 0x7673;
		constexpr std::uint32_t projection_pose_pitch = 0x7674;
		constexpr std::uint32_t projection_pose_roll = 0x7675;

		constexpr std::uint32_t sampling_frequency = 0xB5;
		constexpr std::uint32_t output_sampling_frequency = 0x78B5;
		constexpr std::uint32_t channels = 0x9F;
		constexpr std::uint32_t bit_depth = 0x6264;

		constexpr std::uint32_t cluster_timestamp = 0xE7;
		constexpr std::uint32_t simple_block = 0xA3;
		constexpr std::uint32_t block_group = 0xA0;
		constexpr std::uint32_t block = 0xA1;
		constexpr std::uint32_t block_duration = 0x9B;

		constexpr std::uint32_t tag = 0x7373;
		constexpr std::uint32_t targets = 0x63C0;
		constexpr std::uint32_t target_type_value = 0x68CA;
		constexpr std::uint32_t target_type = 0x63CA;
		constexpr std::uint32_t tag_track_uid = 0x63C5;
		constexpr std::uint32_t tag_chapter_uid = 0x63C4;
		constexpr std::uint32_t tag_attachment_uid = 0x63C6;
		constexpr std::uint32_t simple_tag = 0x67C8;
		constexpr std::uint32_t tag_name = 0x45A3;
		constexpr std::uint32_t tag_language = 0x447A;
		constexpr std::uint32_t tag_language_bcp47 = 0x447B;
		constexpr std::uint32_t tag_string = 0x4487;
		constexpr std::uint32_t tag_binary = 0x4485;

		constexpr std::uint32_t attached_file = 0x61A7;
		constexpr std::uint32_t file_description = 0x467E;
		constexpr std::uint32_t file_name = 0x466E;
		constexpr std::uint32_t file_mime_type = 0x4660;
		constexpr std::uint32_t file_data = 0x465C;
		constexpr std::uint32_t file_uid = 0x46AE;
	}

	// Codec headers and frame starts FFmpeg wrote, for a 320 x 240 picture and 48 kHz stereo sound but
	// where noted.
	constexpr std::string_view avc_config =
		"0164000dffe100186764000dacd94141fa1000000300100000030320f142996001000668ebe3cb22c0fdf8f800";
	constexpr std::string_view hevc_config =
		"0102200000009000000000003cf000fcfdfafa00000f03200001001840010c01ffff02200000030090000003000003003c959809210001"
		"002942010102200000030090000003000003003ca00a080f1365959a4932b9a0200000030020000003032122000100074401c172b46240";
	constexpr std::string_view av1_config = "81000c000a0b0200000561e7fde017c002";
	constexpr std::string_view mpeg4_config =
		"000001b001000001b58913000001000000012000c48d8800cd0a041e1443000001b24c61766336332e312e313032";
	constexpr std::string_view aac_config = "119056e500";
	constexpr std::string_view opus_head = "4f707573486561640102380180bb0000000000";
	// 44.1 kHz.
	constexpr std::string_view flac_private = "664c614380000022120012000003320005490ac442f00000ac44329626d06ec29f977ca65d95690a7626";
	constexpr std::string_view alac_config = "000010000010280a0e02000000004004001588800000ac44";
	constexpr std::string_view vorbis_identification = "01766f72626973000000000244ac0000000000000000000000000000bb01";
	constexpr std::string_view vp9_frame = "824983420013f00ef60038241c184a000340620c43f62fa3f63c53eb3a8fa8fa35f57ee8eaff9a5a28";
	constexpr std::string_view vp8_key_frame = "d028009d012a4001f00000470885858885848802021be7bb9f01fc07e80798affef407f407f80fc7f8";
	constexpr std::string_view mpeg2_frame = "000001b31400f023ffffe000000001b5148a00010000000001b80008004000000100000ffff8000001b5";
	// 5.1 channels.
	constexpr std::string_view ac3_frame = "0b7761191e40ebf8403eff99c598b31662cc59fffb577cf9477b870df577d0df3e7d0df3e7c28ef70e1beaefa1be7cfa";
	// A JPEG's start of image and 4:2:2 frame header.
	constexpr std::string_view jpeg_frame = "ffd8ffc000110800f0014003012100021101031101";

	bytes hex(const std::string_view text)
	{
		const auto nibble = [](const char c) { return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
		bytes out;
		for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(nibble(text[i]) << 4 | nibble(text[i + 1])));
		return out;
	}

	void append(bytes& to, const bytes& more)
	{
		to.insert(to.end(), more.begin(), more.end());
	}

	bytes cat(const std::vector<bytes>& parts)
	{
		bytes out;
		for (const auto& p : parts) append(out, p);
		return out;
	}

	// ---- An EBML writer ------------------------------------------------------------------------------

	bytes id_bytes(const std::uint32_t element)
	{
		bytes out;
		const auto length = element > 0xFFFFFF ? 4 : element > 0xFFFF ? 3 : element > 0xFF ? 2 : 1;
		for (auto i = length - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(element >> (8 * i)));
		return out;
	}

	// A size in `length` bytes, or in the fewest that hold it.
	bytes size_bytes(const std::uint64_t size, int length = 0)
	{
		if (length == 0)
		{
			length = 1;
			while (length < 8 && size >= (std::uint64_t{1} << (7 * length)) - 1) ++length;
		}

		const auto marked = size | std::uint64_t{1} << (7 * length);
		bytes out;
		for (auto i = length - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(marked >> (8 * i)));
		return out;
	}

	bytes el(const std::uint32_t element, const bytes& payload, const int size_length = 0)
	{
		return cat({id_bytes(element), size_bytes(payload.size(), size_length), payload});
	}

	// An element whose size is unknown: all ones.
	bytes el_unknown(const std::uint32_t element, const bytes& payload)
	{
		return cat({id_bytes(element), bytes{0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, payload});
	}

	bytes el_uint(const std::uint32_t element, const std::uint64_t value, int length = 0)
	{
		if (length == 0)
		{
			length = 1;
			while (length < 8 && value >> (8 * length)) ++length;
		}

		bytes payload;
		for (auto i = length - 1; i >= 0; --i) payload.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
		return el(element, payload);
	}

	bytes el_float(const std::uint32_t element, const double value)
	{
		const auto bits = std::bit_cast<std::uint64_t>(value);
		bytes payload;
		for (auto i = 7; i >= 0; --i) payload.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
		return el(element, payload);
	}

	bytes el_float32(const std::uint32_t element, const float value)
	{
		const auto bits = std::bit_cast<std::uint32_t>(value);
		return el(element, builder().be32(bits).b);
	}

	bytes el_str(const std::uint32_t element, const std::string_view text)
	{
		return el(element, to_bytes(text));
	}

	bytes master(const std::uint32_t element, const std::vector<bytes>& children)
	{
		return el(element, cat(children));
	}

	bytes ebml_header(const std::string_view doc_type = "matroska", const std::uint64_t doc_read_version = 2, const std::uint64_t ebml_read_version = 1)
	{
		return master(id::ebml, {
			              el_uint(id::ebml_version, 1), el_uint(id::ebml_read_version, ebml_read_version), el_uint(id::ebml_max_id_length, 4),
			              el_uint(id::ebml_max_size_length, 8), el_str(id::doc_type, doc_type), el_uint(id::doc_type_version, 4),
			              el_uint(id::doc_type_read_version, doc_read_version)
		              });
	}

	bytes segment(const std::vector<bytes>& children)
	{
		return el(id::segment, cat(children), 8);
	}

	bytes info(const std::vector<bytes>& more = {})
	{
		return master(id::info, {el_uint(id::timestamp_scale, 1000000), cat(more)});
	}

	bytes track(const std::uint64_t number, const std::uint64_t type, const std::string_view codec, const std::vector<bytes>& more = {})
	{
		return master(id::track_entry, {
			              el_uint(id::track_number, number), el_uint(id::track_uid, number + 10), el_uint(id::track_type, type),
			              el_str(id::codec_id, codec), cat(more)
		              });
	}

	bytes picture_size(const std::uint64_t width, const std::uint64_t height, const std::vector<bytes>& more = {})
	{
		return master(id::video, {el_uint(id::pixel_width, width), el_uint(id::pixel_height, height), cat(more)});
	}

	bytes sound(const double rate, const std::uint64_t channels, const std::vector<bytes>& more = {})
	{
		return master(id::audio, {el_float(id::sampling_frequency, rate), el_uint(id::channels, channels), cat(more)});
	}

	bytes simple_block(const unsigned track_number, const bytes& frame, const unsigned flags = 0x80)
	{
		return el(id::simple_block, cat({bytes{static_cast<std::uint8_t>(0x80 | track_number), 0, 0, static_cast<std::uint8_t>(flags)}, frame}));
	}

	bytes cluster(const std::vector<bytes>& blocks)
	{
		return master(id::cluster, {el_uint(id::cluster_timestamp, 0), cat(blocks)});
	}

	bytes simple_tag(const std::string_view name, const std::string_view value, const std::vector<bytes>& more = {})
	{
		return master(id::simple_tag, {el_str(id::tag_name, name), el_str(id::tag_string, value), cat(more)});
	}

	bytes tag(const std::vector<bytes>& targets, const std::vector<bytes>& simple_tags)
	{
		return master(id::tag, {master(id::targets, targets), cat(simple_tags)});
	}

	bytes attached_file(const std::string_view file_name, const std::string_view mime, const bytes& data, const std::string_view description = {})
	{
		return master(id::attached_file, {
			              description.empty() ? bytes{} : el_str(id::file_description, description), el_str(id::file_name, file_name),
			              el_str(id::file_mime_type, mime), el(id::file_data, data), el_uint(id::file_uid, data.size())
		              });
	}

	struct seek_to
	{
		std::uint32_t element = 0;
		std::size_t child = 0;
		// Added to the child's position, to point an entry where it should not.
		std::int64_t adjust = 0;
	};

	// A Segment's children, among them SeekHeads that point at others by index, before or after them.
	// A SeekHead's positions are written in eight bytes, so its size does not depend on them.
	class segment_builder
	{
	public:
		std::size_t add(bytes child)
		{
			_children.push_back(std::move(child));
			_entries.emplace_back();
			_heads.push_back(false);
			return _children.size() - 1;
		}

		std::size_t add_seek_head()
		{
			const auto index = add({});
			_heads[index] = true;
			return index;
		}

		void point(const std::size_t head, std::vector<seek_to> entries)
		{
			_entries[head] = std::move(entries);
		}

		bytes build(const bytes& header, const bool unknown_size = false)
		{
			const std::vector<std::uint64_t> zeros(_children.size(), 0);

			for (std::size_t i = 0; i < _children.size(); ++i)
			{
				if (_heads[i]) _children[i] = seek_head(_entries[i], zeros);
			}

			_positions.assign(_children.size(), 0);
			std::uint64_t at = 0;

			for (std::size_t i = 0; i < _children.size(); ++i)
			{
				_positions[i] = at;
				at += _children[i].size();
			}

			for (std::size_t i = 0; i < _children.size(); ++i)
			{
				if (_heads[i]) _children[i] = seek_head(_entries[i], _positions);
			}

			_data = header.size() + 12;
			return cat({header, unknown_size ? el_unknown(id::segment, cat(_children)) : el(id::segment, cat(_children), 8)});
		}

		// Where a child begins in the file, and where it ends.
		std::uint64_t offset(const std::size_t child) const { return _data + _positions[child]; }
		std::uint64_t end(const std::size_t child) const { return offset(child) + _children[child].size(); }

	private:
		static bytes seek_head(const std::vector<seek_to>& entries, const std::vector<std::uint64_t>& positions)
		{
			bytes payload;

			for (const auto& e : entries)
			{
				const auto position = static_cast<std::uint64_t>(static_cast<std::int64_t>(positions[e.child]) + e.adjust);
				append(payload, master(id::seek, {el(id::seek_id, id_bytes(e.element)), el_uint(id::seek_position, position, 8)}));
			}

			return el(id::seek_head, payload);
		}

		std::vector<bytes> _children;
		std::vector<std::vector<seek_to>> _entries;
		std::vector<bool> _heads;
		std::vector<std::uint64_t> _positions;
		std::uint64_t _data = 0;
	};

	media_scan_result scan(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index)
	{
		libfiles::reader r(file);
		return libfiles::scan_media(r, intent);
	}

	struct counted
	{
		media_scan_result result;
		std::uint64_t reads = 0;
		std::uint64_t bytes_read = 0;
	};

	// A scan through a stream, which counts what it reads.
	counted scan_counted(const bytes& file)
	{
		libfiles::memory_stream source(file);
		libfiles::reader r(source);
		counted c;
		c.result = libfiles::scan_media(r);
		c.reads = r.source_reads();
		c.bytes_read = r.source_bytes();
		return c;
	}

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	bool same_tag(const libfiles::media_tag& t, const std::string_view key, const std::string_view value, const std::string_view language = {})
	{
		return t.key == key && t.value == value && t.language == language;
	}

	// Where bytes first appear in a file.
	std::uint64_t find(const bytes& file, const bytes& what)
	{
		const auto at = std::search(file.begin(), file.end(), what.begin(), what.end());
		return static_cast<std::uint64_t>(at - file.begin());
	}

	// A zlib stream that stores its data uncompressed, as a ContentCompression of algorithm 0 may.
	bytes zlib_stored(const bytes& data)
	{
		std::uint32_t a = 1;
		std::uint32_t b = 0;

		for (const auto byte : data)
		{
			a = (a + byte) % 65521;
			b = (b + a) % 65521;
		}

		const auto length = static_cast<unsigned>(data.size());
		return cat({builder().u8(0x78).u8(0x01).u8(0x01).le16(length).le16(~length & 0xFFFF).b, data, builder().be32(b << 16 | a).b});
	}

	// The file the cut and corruption tests start from. A SeekHead points at Info and Tracks before the
	// Cluster, at Attachments and a second SeekHead after it, and into a Void where nothing begins; the
	// second points at Cues and at Tags.
	struct sample
	{
		bytes file;
		// A file cut before Tracks ends cannot be read.
		std::uint64_t tracks_end = 0;
		libfiles::byte_range cover;
	};

	sample sample_file()
	{
		segment_builder s;
		const auto head = s.add_seek_head();
		const auto void_index = s.add(el(id::void_element, bytes(8, 0)));
		const auto info_index = s.add(info({
			el_float(id::duration, 2000.0), el_str(id::title, "Sample"), el_str(id::muxing_app, "libfiles mux"),
			el_str(id::writing_app, "libfiles writer"), el_uint(id::date_utc, 321722029000000000, 8)
		}));
		const auto tracks_index = s.add(master(id::tracks, {
			track(1, 1, "V_MPEG4/ISO/AVC", {
				      el(id::codec_private, hex(avc_config)), el_uint(id::default_duration, 40000000), el_str(id::language, "ger"),
				      el_str(id::name, "Main"), picture_size(320, 240)
			      }),
			track(2, 2, "A_AAC", {el(id::codec_private, hex(aac_config)), el_uint(id::flag_default, 0), sound(48000, 2)}),
			track(3, 0x11, "S_TEXT/UTF8", {el_str(id::language, "fre"), el_str(id::language_bcp47, "fr-CA")}),
		}));
		s.add(master(id::tags, {
			tag({}, {simple_tag("GENRE", "Test")}),
			tag({el_uint(id::tag_track_uid, 11)}, {
				    simple_tag("DURATION", "00:00:01.960000000"), simple_tag("BPS", "123456"), simple_tag("TITLE", "Track title")
			    }),
		}));
		s.add(cluster({simple_block(1, bytes(8, 0x11)), simple_block(2, bytes(4, 0x22))}));
		const auto cues = s.add(el(id::cues, bytes(4, 0x33)));
		const auto end_tags = s.add(master(id::tags, {
			tag({el_uint(id::target_type_value, 50)}, {
				    simple_tag("ARTIST", "Artist Name", {simple_tag("SORT_WITH", "Name, Artist")}),
				    simple_tag("COMMENT", "Kommentar", {el_str(id::tag_language, "ger")}),
				    simple_tag("TITLE", "Colour", {el_str(id::tag_language, "und"), el_str(id::tag_language_bcp47, "en-GB")}),
				    master(id::simple_tag, {el_str(id::tag_name, "COVER"), el(id::tag_binary, bytes(10, 7))}),
				    master(id::simple_tag, {el_str(id::tag_string, "no name")}),
				    simple_tag("EMPTY", ""),
			    }),
			tag({el_uint(id::target_type_value, 50), el_str(id::target_type, "ALBUM")}, {simple_tag("TITLE", "An Album")}),
			tag({el_uint(id::tag_chapter_uid, 5)}, {simple_tag("TITLE", "Chapter")}),
			tag({el_uint(id::tag_attachment_uid, 7)}, {simple_tag("TITLE", "Attachment")}),
			tag({el_uint(id::tag_track_uid, 999)}, {simple_tag("TITLE", "Nobody")}),
			tag({el_uint(id::tag_track_uid, 11), el_uint(id::tag_track_uid, 12)}, {simple_tag("NOTE", "both")}),
		}));
		const auto attachments = s.add(master(id::attachments, {
			attached_file("font.ttf", "application/x-truetype-font", bytes(20, 0x44)),
			attached_file("cover.jpg", "image/jpeg", hex(jpeg_frame), "The front"),
		}));
		const auto end_head = s.add_seek_head();

		s.point(head, {
			        {id::info, info_index}, {id::tracks, tracks_index}, {id::attachments, attachments}, {id::seek_head, end_head},
			        {id::tags, void_index, 3}
		        });
		s.point(end_head, {{id::cues, cues}, {id::tags, end_tags}});

		sample out;
		out.file = s.build(ebml_header());
		out.tracks_end = s.end(tracks_index);
		out.cover = {find(out.file, hex(jpeg_frame)), hex(jpeg_frame).size()};
		return out;
	}

	// What the sample file's streams are as the container describes them, whatever the codec headers say.
	void check_sample_streams(const media_scan_result& r)
	{
		CHECK_EQ(r.streams.size(), 3u);
		if (r.streams.size() != 3) return;

		const auto& v = r.streams[0];
		CHECK(v.kind == media_stream_kind::video);
		CHECK_EQ(v.id, 1u);
		CHECK_STR(v.codec, "h264");
		CHECK_STR(v.codec_tag, "V_MPEG4/ISO/AVC");
		CHECK_STR(v.language, "ger");
		CHECK_STR(v.title, "Main");
		CHECK(v.is_default);
		CHECK_EQ(v.video.width, 320u);
		CHECK_EQ(v.video.height, 240u);
		CHECK(near(v.video.frame_rate, 25.0));

		const auto& a = r.streams[1];
		CHECK(a.kind == media_stream_kind::audio);
		CHECK_EQ(a.id, 2u);
		CHECK_STR(a.codec, "aac");
		CHECK_STR(a.language, "eng");
		CHECK(!a.is_default);
		CHECK_EQ(a.audio.sample_rate, 48000u);
		CHECK_EQ(a.audio.channels, 2u);
		CHECK_EQ(a.audio.bits_per_sample, 0);

		const auto& t = r.streams[2];
		CHECK(t.kind == media_stream_kind::subtitle);
		CHECK_EQ(t.id, 3u);
		CHECK_STR(t.codec, "subrip");
		CHECK_STR(t.language, "fr-CA");
	}

	// Whether a's items appear in b in the same order, as the tags a file cut short keeps must.
	template <typename T, typename Same>
	bool in_order(const std::vector<T>& a, const std::vector<T>& b, Same same)
	{
		std::size_t j = 0;

		for (const auto& x : a)
		{
			while (j < b.size() && !same(x, b[j])) ++j;
			if (j == b.size()) return false;
			++j;
		}

		return true;
	}

	bool same_tags(const libfiles::media_tag& x, const libfiles::media_tag& y)
	{
		return x.key == y.key && x.value == y.value && x.language == y.language;
	}

	bool same_pictures(const libfiles::media_picture& x, const libfiles::media_picture& y)
	{
		return x.location == y.location && x.mime_type == y.mime_type && x.type == y.type;
	}

	// What any scan of a damaged file keeps to: an error alone, or a result inside the file and the bounds.
	void check_sound(const media_scan_result& r, const std::uint64_t file_size)
	{
		if (!r.success)
		{
			CHECK(!r.error.empty());
			CHECK(r.streams.empty());
			CHECK(r.tags.empty());
			return;
		}

		CHECK(std::isfinite(r.duration) && r.duration >= 0);
		CHECK(r.streams.size() <= 1000);
		CHECK(r.tags.size() <= 4100);

		for (const auto& p : r.pictures)
		{
			CHECK(!p.location.empty() && p.location.offset <= file_size && p.location.size <= file_size - p.location.offset);
			CHECK(p.data.empty());
		}

		for (const auto& s : r.streams)
		{
			CHECK(s.video.width <= 65535 && s.video.height <= 65535);
			CHECK(s.video.rotation % 90 == 0 && s.video.rotation >= 0 && s.video.rotation < 360);
		}
	}
}

TEST("media mkv reads a video and an audio track")
{
	const auto file = cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1500.0)}),
			master(id::tracks, {
				track(1, 1, "V_MPEG4/ISO/AVC", {
					      el(id::codec_private, hex(avc_config)), el_uint(id::default_duration, 40000000), el_str(id::language, "ger"),
					      el_str(id::name, "Main"), picture_size(320, 240)
				      }),
				track(2, 2, "A_AAC", {
					      el(id::codec_private, hex(aac_config)), el_uint(id::flag_default, 0), el_uint(id::flag_commentary, 1),
					      el_uint(id::flag_visual_impaired, 1), sound(48000, 2)
				      }),
			}),
			cluster({simple_block(1, bytes(64, 0)), simple_block(2, bytes(16, 0))}),
		})
	});

	const auto r = scan(file);
	CHECK(r.success);
	CHECK(r.format == libfiles::media_format::matroska);
	CHECK_STR(r.brand, "matroska");
	CHECK(near(r.duration, 1.5));
	CHECK(r.complete);
	CHECK_EQ(r.bit_rate, static_cast<std::uint64_t>(static_cast<double>(file.size()) * 8 / 1.5));
	CHECK(r.pictures.empty());
	CHECK(r.xmp.empty());
	CHECK(r.structure.empty());
	CHECK_EQ(r.streams.size(), 2u);
	if (r.streams.size() != 2) return;

	const auto& v = r.streams[0];
	CHECK(v.kind == media_stream_kind::video);
	CHECK_EQ(v.id, 1u);
	CHECK_STR(v.codec, "h264");
	CHECK_STR(v.codec_tag, "V_MPEG4/ISO/AVC");
	CHECK_STR(v.language, "ger");
	CHECK_STR(v.title, "Main");
	CHECK(v.is_default);
	CHECK(!v.commentary);
	CHECK(near(v.duration, 0));
	CHECK_EQ(v.bit_rate, 0u);
	CHECK_EQ(v.video.width, 320u);
	CHECK_EQ(v.video.height, 240u);
	CHECK_EQ(v.video.pixel_aspect_width, 0u);
	CHECK(near(v.video.frame_rate, 25.0));
	CHECK_EQ(v.video.rotation, 0);
	CHECK(!v.video.interlaced);

	const auto& a = r.streams[1];
	CHECK(a.kind == media_stream_kind::audio);
	CHECK_EQ(a.id, 2u);
	CHECK_STR(a.codec, "aac");
	CHECK_STR(a.codec_tag, "A_AAC");
	CHECK_STR(a.language, "eng");
	CHECK(!a.is_default);
	CHECK(a.commentary);
	CHECK(a.audio_description);
	CHECK_EQ(a.audio.sample_rate, 48000u);
	CHECK_EQ(a.audio.channels, 2u);
	CHECK_EQ(a.audio.bits_per_sample, 0);
	CHECK(!a.audio.float_samples);
}

TEST("media mkv reads a video and an audio track's codec headers")
{
	const auto file = cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0)}),
			master(id::tracks, {
				track(1, 1, "V_MPEG4/ISO/AVC", {el(id::codec_private, hex(avc_config)), picture_size(320, 240)}),
				track(2, 2, "A_AAC", {el(id::codec_private, hex(aac_config)), sound(48000, 2)}),
			}),
		})
	});

	const auto r = scan(file);
	CHECK(r.success);
	if (r.streams.size() != 2) return;

	const auto& v = r.streams[0].video;
	CHECK_STR(v.profile, "High");
	CHECK(v.chroma == chroma_format::yuv420);
	CHECK_EQ(v.bit_depth, 8);
	CHECK_STR(v.pixel_format(), "yuv420");
	CHECK_STR(r.streams[1].audio.profile, "LC");
}

TEST("media mkv reads the sample file whole")
{
	const auto s = sample_file();
	const auto r = scan(s.file);
	CHECK(r.success);
	CHECK(near(r.duration, 2.0));
	check_sample_streams(r);
	if (r.streams.size() != 3) return;

	// Info's title and applications come first, then the Tags in file order, flattened as FFmpeg
	// flattens them; a TargetType prefixes its keys, and tags for chapters, attachments and tracks
	// that do not exist are left out.
	const std::vector<libfiles::media_tag> expected = {
		{"TITLE", "Sample", ""},
		{"ENCODER", "libfiles writer", ""},
		{"ENCODER", "libfiles mux", ""},
		{"GENRE", "Test", ""},
		{"ARTIST", "Artist Name", ""},
		{"ARTIST/SORT_WITH", "Name, Artist", ""},
		{"COMMENT", "Kommentar", "ger"},
		{"TITLE", "Colour", "en-GB"},
		{"COVER", "10 bytes of binary data", ""},
		{"ALBUM/TITLE", "An Album", ""},
	};

	CHECK_EQ(r.tags.size(), expected.size());
	for (std::size_t i = 0; i < std::min(r.tags.size(), expected.size()); ++i)
	{
		CHECK_STR(r.tags[i].key, expected[i].key);
		CHECK_STR(r.tags[i].value, expected[i].value);
		CHECK_STR(r.tags[i].language, expected[i].language);
	}

	// Tags for a track go to its stream, and its statistics state its duration and bit rate.
	const auto& video_tags = r.streams[0].tags;
	CHECK_EQ(video_tags.size(), 4u);
	if (video_tags.size() == 4)
	{
		CHECK(same_tag(video_tags[0], "DURATION", "00:00:01.960000000"));
		CHECK(same_tag(video_tags[1], "BPS", "123456"));
		CHECK(same_tag(video_tags[2], "TITLE", "Track title"));
		CHECK(same_tag(video_tags[3], "NOTE", "both"));
	}

	CHECK(near(r.streams[0].duration, 1.96));
	CHECK_EQ(r.streams[0].bit_rate, 123456u);
	CHECK_STR(r.streams[0].title, "Main");
	CHECK_EQ(r.streams[1].tags.size(), 1u);
	if (!r.streams[1].tags.empty()) CHECK(same_tag(r.streams[1].tags[0], "NOTE", "both"));
	CHECK(near(r.streams[1].duration, 0));
	CHECK(r.streams[2].tags.empty());

	// DateUTC, 2011-03-13 15:13:49 in UTC.
	const auto& created = r.properties.created;
	CHECK_EQ(created.year, 2011);
	CHECK_EQ(created.month, 3);
	CHECK_EQ(created.day, 13);
	CHECK(created.has_time);
	CHECK_EQ(created.hour, 15);
	CHECK_EQ(created.minute, 13);
	CHECK_EQ(created.second, 49);
	CHECK(created.zone == libfiles::media_date::time_zone::utc);

	CHECK_EQ(r.pictures.size(), 1u);
	if (r.pictures.size() == 1)
	{
		const auto& p = r.pictures[0];
		CHECK(p.location == s.cover);
		CHECK(p.data.empty());
		CHECK_STR(p.mime_type, "image/jpeg");
		CHECK_EQ(p.type, 3);
		CHECK_STR(p.description, "The front");
	}
}

TEST("media mkv names the codecs of every CodecID")
{
	struct expectation
	{
		std::string_view codec_id;
		std::uint64_t type;
		std::string_view codec;
		media_stream_kind kind;
	};

	constexpr auto video = media_stream_kind::video;
	constexpr auto audio = media_stream_kind::audio;
	constexpr auto subtitle = media_stream_kind::subtitle;
	constexpr auto data = media_stream_kind::data;

	const expectation cases[] = {
		{"V_MPEG4/ISO/AVC", 1, "h264", video}, {"V_MPEGH/ISO/HEVC", 1, "hevc", video}, {"V_AV1", 1, "av1", video},
		{"V_VP9", 1, "vp9", video}, {"V_VP8", 1, "vp8", video}, {"V_MPEG4/ISO/SP", 1, "mpeg4", video},
		{"V_MPEG4/ISO/ASP", 1, "mpeg4", video}, {"V_MPEG4/ISO/AP", 1, "mpeg4", video}, {"V_MPEG1", 1, "mpeg1", video},
		{"V_MPEG2", 1, "mpeg2", video}, {"V_MJPEG", 1, "mjpeg", video}, {"V_THEORA", 1, "theora", video},
		{"V_PRORES", 1, "prores", video}, {"V_FFV1", 1, "ffv1", video}, {"V_UNCOMPRESSED", 1, "rawvideo", video},
		{"A_AAC", 2, "aac", audio}, {"A_AAC/MPEG4/LC/SBR", 2, "aac", audio}, {"A_MPEG/L3", 2, "mp3", audio},
		{"A_MPEG/L2", 2, "mp2", audio}, {"A_MPEG/L1", 2, "mp1", audio}, {"A_AC3", 2, "ac3", audio}, {"A_EAC3", 2, "eac3", audio},
		{"A_DTS", 2, "dts", audio}, {"A_DTS/LOSSLESS", 2, "dts", audio}, {"A_OPUS", 2, "opus", audio},
		{"A_VORBIS", 2, "vorbis", audio}, {"A_FLAC", 2, "flac", audio}, {"A_ALAC", 2, "alac", audio},
		{"A_PCM/INT/LIT", 2, "pcm", audio}, {"A_PCM/INT/BIG", 2, "pcm", audio}, {"A_PCM/FLOAT/IEEE", 2, "pcm", audio},
		{"A_TRUEHD", 2, "truehd", audio}, {"A_WAVPACK4", 2, "wavpack", audio}, {"A_TTA1", 2, "tta", audio},
		{"S_TEXT/UTF8", 0x11, "subrip", subtitle}, {"S_TEXT/ASS", 0x11, "ass", subtitle}, {"S_TEXT/SSA", 0x11, "ass", subtitle},
		{"S_TEXT/WEBVTT", 0x11, "webvtt", subtitle}, {"S_VOBSUB", 0x11, "dvd_subtitle", subtitle},
		{"S_HDMV/PGS", 0x11, "hdmv_pgs_subtitle", subtitle},
		// FFmpeg reads a WebVTT metadata track as subtitles.
		{"D_WEBVTT/METADATA", 0x21, "webvtt", subtitle},
		// A CodecID that does not fit its track's type, as FFmpeg checks, names nothing; nor does one
		// nobody knows, nor none at all.
		{"A_AAC", 1, "", video}, {"V_VP8", 2, "", audio}, {"V_FUTURE", 1, "", video}, {"B_VOBBTN", 0x12, "", data},
		{"", 0x20, "", data}, {"V_VP8", 3, "", data},
	};

	std::vector<bytes> entries;
	std::uint64_t number = 1;
	for (const auto& c : cases) entries.push_back(track(number++, c.type, c.codec_id));

	const auto r = scan(cat({ebml_header(), segment({info(), master(id::tracks, entries)})}));
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), std::size(cases));
	if (r.streams.size() != std::size(cases)) return;

	for (std::size_t i = 0; i < std::size(cases); ++i)
	{
		CHECK_STR(r.streams[i].codec, cases[i].codec);
		CHECK_STR(r.streams[i].codec_tag, cases[i].codec_id);
		CHECK(r.streams[i].kind == cases[i].kind);
		CHECK_EQ(r.streams[i].id, i + 1);
	}

	// Nothing plays at a known length, so the scan is not complete.
	CHECK(!r.complete);
}

TEST("media mkv reads PCM and lossless sample formats")
{
	struct expectation
	{
		std::string_view codec_id;
		std::uint64_t bit_depth;
		std::uint8_t bits;
		bool floating;
	};

	// FFmpeg reads PCM without a BitDepth as 16-bit integers or 32-bit floats; a lossy codec's BitDepth
	// is not its samples'.
	const expectation cases[] = {
		{"A_PCM/INT/LIT", 16, 16, false}, {"A_PCM/INT/LIT", 24, 24, false}, {"A_PCM/INT/LIT", 8, 8, false},
		{"A_PCM/INT/LIT", 0, 16, false}, {"A_PCM/INT/BIG", 16, 16, false}, {"A_PCM/INT/BIG", 32, 32, false},
		{"A_PCM/FLOAT/IEEE", 32, 32, true}, {"A_PCM/FLOAT/IEEE", 64, 64, true}, {"A_PCM/FLOAT/IEEE", 0, 32, true},
		{"A_WAVPACK4", 24, 24, false}, {"A_TTA1", 16, 16, false}, {"A_TRUEHD", 24, 24, false}, {"A_FLAC", 20, 20, false},
		{"A_MPEG/L3", 16, 0, false}, {"A_AC3", 16, 0, false}, {"A_WAVPACK4", 0, 0, false},
	};

	std::vector<bytes> entries;
	std::uint64_t number = 1;

	for (const auto& c : cases)
	{
		entries.push_back(track(number++, 2, c.codec_id, {sound(44100, 2, {c.bit_depth ? el_uint(id::bit_depth, c.bit_depth) : bytes{}})}));
	}

	const auto r = scan(cat({ebml_header(), segment({info({el_float(id::duration, 10.0)}), master(id::tracks, entries)})}));
	CHECK(r.success);
	CHECK(r.complete);
	CHECK_EQ(r.streams.size(), std::size(cases));
	if (r.streams.size() != std::size(cases)) return;

	for (std::size_t i = 0; i < std::size(cases); ++i)
	{
		CHECK_EQ(r.streams[i].audio.bits_per_sample, cases[i].bits);
		CHECK(r.streams[i].audio.float_samples == cases[i].floating);
		CHECK_EQ(r.streams[i].audio.sample_rate, 44100u);
		CHECK_EQ(r.streams[i].audio.channels, 2u);
	}
}

TEST("media mkv reads the rate that plays and Matroska's defaults")
{
	const auto r = scan(cat({
		ebml_header(), segment({
			info(),
			master(id::tracks, {
				// AAC with SBR states the rate it plays at apart.
				track(1, 2, "A_AAC/MPEG4/LC/SBR", {sound(24000, 2, {el_float(id::output_sampling_frequency, 48000.0)})}),
				// No Audio element: 8 kHz and one channel, as FFmpeg reads it.
				track(2, 2, "A_MPEG/L3"),
				// A rate FFmpeg cannot use falls back to 8 kHz; a single-precision rate reads too.
				track(3, 2, "A_MPEG/L3", {sound(-5.0, 2)}),
				track(4, 2, "A_MPEG/L2", {master(id::audio, {el_float32(id::sampling_frequency, 32000.0f), el_uint(id::channels, 1)})}),
				track(5, 2, "A_AAC/MPEG2/MAIN", {sound(22050, 1)}),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 5u);
	if (r.streams.size() != 5) return;

	CHECK_EQ(r.streams[0].audio.sample_rate, 48000u);
	CHECK_STR(r.streams[0].audio.profile, "HE-AAC");
	CHECK_EQ(r.streams[1].audio.sample_rate, 8000u);
	CHECK_EQ(r.streams[1].audio.channels, 1u);
	CHECK_EQ(r.streams[2].audio.sample_rate, 8000u);
	CHECK_EQ(r.streams[3].audio.sample_rate, 32000u);
	CHECK_EQ(r.streams[3].audio.channels, 1u);
	CHECK_STR(r.streams[4].audio.profile, "Main");
	CHECK_EQ(r.streams[4].audio.sample_rate, 22050u);
}

TEST("media mkv reads colour, display shape, flags and turn")
{
	const auto colour = master(id::colour, {
		el_uint(id::matrix_coefficients, 9), el_uint(id::bits_per_channel, 10), el_uint(id::chroma_subsampling_horz, 1),
		el_uint(id::chroma_subsampling_vert, 1), el_uint(id::range, 2), el_uint(id::transfer_characteristics, 16),
		el_uint(id::primaries, 9)
	});

	const auto roll = [](const double degrees, const double yaw = 0)
	{
		return master(id::projection, {
			              el_uint(id::projection_type, 0), el_float(id::projection_pose_yaw, yaw), el_float(id::projection_pose_pitch, 0),
			              el_float(id::projection_pose_roll, degrees)
		              });
	};

	const auto r = scan(cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0)}),
			master(id::tracks, {
				// HDR10 colour, 16:9 shown from 1440 x 1080, interlaced, with alpha.
				track(1, 1, "V_FFV1", {
					      picture_size(1440, 1080, {
						                   el_uint(id::display_width, 1920), el_uint(id::display_height, 1080), el_uint(id::flag_interlaced, 1),
						                   el_uint(id::alpha_mode, 1), colour
					                   })
				      }),
				// A display aspect ratio, and a crop that the size loses.
				track(2, 1, "V_FFV1", {
					      picture_size(720, 576, {
						                   el_uint(id::pixel_crop_left, 8), el_uint(id::pixel_crop_right, 8), el_uint(id::pixel_crop_top, 2),
						                   el_uint(id::pixel_crop_bottom, 2), el_uint(id::display_width, 16), el_uint(id::display_height, 9),
						                   el_uint(id::display_unit, 3), el_uint(id::flag_interlaced, 2)
					                   })
				      }),
				// Turned a quarter counter-clockwise, then mirrored and turned back; a unit that is unknown
				// gives no shape, and a display size that is the picture's is square.
				track(3, 1, "V_FFV1", {picture_size(640, 480, {roll(90), el_uint(id::display_width, 640), el_uint(id::display_unit, 4)})}),
				track(4, 1, "V_FFV1", {picture_size(640, 480, {roll(-90, 180), el_uint(id::display_width, 640)})}),
				// A pose that is not a flat right-angle turn is left alone, and so is the shape of a picture
				// whose display size is missing.
				track(5, 1, "V_FFV1", {picture_size(640, 480, {roll(45)})}),
				track(6, 1, "V_FFV1", {
					      picture_size(640, 480, {
						                   master(id::projection, {el_float(id::projection_pose_pitch, 90), el_float(id::projection_pose_roll, 90)})
					                   })
				      }),
				// Side by side, each eye shown at the display size.
				track(7, 1, "V_FFV1", {
					      picture_size(3840, 1080, {el_uint(id::stereo_mode, 1), el_uint(id::display_width, 1920), el_uint(id::display_height, 1080)})
				      }),
				// FrameRate stands only where DefaultDuration does not.
				track(8, 1, "V_FFV1", {picture_size(64, 64, {el_float(id::frame_rate, 29.97)})}),
				track(9, 1, "V_FFV1", {el_uint(id::default_duration, 20000000), picture_size(64, 64, {el_float(id::frame_rate, 29.97)})}),
				// V_UNCOMPRESSED is tagged by its FourCC.
				track(10, 1, "V_UNCOMPRESSED", {picture_size(64, 48, {el(id::uncompressed_fourcc, to_bytes("I420"))})}),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 10u);
	if (r.streams.size() != 10) return;

	const auto& hdr = r.streams[0].video;
	CHECK_EQ(hdr.width, 1440u);
	CHECK_EQ(hdr.height, 1080u);
	CHECK_EQ(hdr.pixel_aspect_width, 4u);
	CHECK_EQ(hdr.pixel_aspect_height, 3u);
	CHECK(hdr.interlaced);
	CHECK(hdr.alpha);
	CHECK(hdr.full_range);
	CHECK_EQ(hdr.matrix, 9);
	CHECK_EQ(hdr.transfer, 16);
	CHECK_EQ(hdr.color_primaries, 9);
	CHECK_EQ(hdr.bit_depth, 10);
	CHECK(hdr.chroma == chroma_format::yuv420);
	CHECK_STR(hdr.pixel_format(), "yuva420 10bit");
	CHECK_STR(std::string(hdr.hdr_transfer()), "PQ");

	const auto& pal = r.streams[1].video;
	CHECK_EQ(pal.width, 704u);
	CHECK_EQ(pal.height, 572u);
	CHECK_EQ(pal.pixel_aspect_width, 13u);
	CHECK_EQ(pal.pixel_aspect_height, 9u);
	CHECK(!pal.interlaced);
	CHECK(pal.chroma == chroma_format::unknown);
	CHECK_EQ(pal.matrix, 2);

	CHECK_EQ(r.streams[2].video.rotation, 270);
	CHECK(!r.streams[2].video.mirrored);
	CHECK_EQ(r.streams[2].video.pixel_aspect_width, 0u);
	CHECK_EQ(r.streams[3].video.rotation, 90);
	CHECK(r.streams[3].video.mirrored);
	CHECK_EQ(r.streams[3].video.pixel_aspect_width, 1u);
	CHECK_EQ(r.streams[3].video.pixel_aspect_height, 1u);
	CHECK_EQ(r.streams[4].video.rotation, 0);
	CHECK_EQ(r.streams[5].video.rotation, 0);
	CHECK_EQ(r.streams[5].video.pixel_aspect_width, 0u);

	CHECK_EQ(r.streams[6].video.pixel_aspect_width, 1u);
	CHECK_EQ(r.streams[6].video.pixel_aspect_height, 1u);

	CHECK(near(r.streams[7].video.frame_rate, 29.97));
	CHECK(near(r.streams[8].video.frame_rate, 50.0));

	CHECK_STR(r.streams[9].codec, "rawvideo");
	CHECK_STR(r.streams[9].codec_tag, "I420");
}

TEST("media mkv reads VfW and ACM tracks through their Windows structures")
{
	// A BITMAPINFOHEADER for XviD at 640 x 480, and a WAVEFORMATEX for WMA 2 at 44.1 kHz in stereo.
	const auto bitmap = builder().le32(40).le32(640).le32(480).le16(1).le16(12).raw("XVID").le32(0).zeros(16).b;
	const auto wave = builder().le16(0x0161).le16(2).le32(44100).le32(16000).le16(2973).le16(16).le16(0).b;

	const auto r = scan(cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0)}),
			master(id::tracks, {
				// The container's size stands over the structure's, as FFmpeg reads them.
				track(1, 1, "V_MS/VFW/FOURCC", {el(id::codec_private, bitmap), picture_size(640, 360)}),
				track(2, 2, "A_MS/ACM", {el(id::codec_private, wave), sound(44100, 2)}),
				// Too short to be a structure: the track names no codec.
				track(3, 1, "V_MS/VFW/FOURCC", {el(id::codec_private, bytes(20, 0)), picture_size(320, 240)}),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 3u);
	if (r.streams.size() != 3) return;

	CHECK_STR(r.streams[0].codec, "mpeg4");
	CHECK_STR(r.streams[0].codec_tag, "XVID");
	CHECK_EQ(r.streams[0].video.width, 640u);
	CHECK_EQ(r.streams[0].video.height, 360u);
	CHECK_STR(r.streams[1].codec, "wmav2");
	CHECK_STR(r.streams[1].codec_tag, "0x0161");
	CHECK_EQ(r.streams[1].audio.sample_rate, 44100u);
	CHECK_EQ(r.streams[1].audio.channels, 2u);
	CHECK(r.streams[2].codec.empty());
	CHECK_STR(r.streams[2].codec_tag, "V_MS/VFW/FOURCC");
	CHECK_EQ(r.streams[2].video.width, 320u);
}

TEST("media mkv reads codec headers from private data")
{
	// Theora's identification header for 320 x 240 at 25 fps, 4:2:0.
	const auto theora = builder().u8(0x80).raw("theora").u8(3).u8(2).u8(1).be16(20).be16(15).u8(0).be16(320).u8(0).be16(240)
	                             .u8(0).u8(0).be32(25).be32(1).u8(0).be16(1).u8(0).be16(1).u8(0).u8(0).be16(0).be16(0x00C0).b;
	const auto xiph = [](const bytes& first) { return cat({bytes{2, static_cast<std::uint8_t>(first.size()), 1}, first, bytes{3, 5}}); };

	const auto r = scan(cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0)}),
			master(id::tracks, {
				track(1, 1, "V_MPEGH/ISO/HEVC", {el(id::codec_private, hex(hevc_config)), picture_size(320, 240)}),
				track(2, 1, "V_AV1", {el(id::codec_private, hex(av1_config)), picture_size(320, 240)}),
				track(3, 1, "V_MPEG4/ISO/ASP", {el(id::codec_private, hex(mpeg4_config)), picture_size(320, 240)}),
				track(4, 1, "V_THEORA", {el(id::codec_private, xiph(theora)), picture_size(320, 240)}),
				track(5, 2, "A_OPUS", {el(id::codec_private, hex(opus_head)), sound(48000, 2)}),
				track(6, 2, "A_VORBIS", {el(id::codec_private, xiph(hex(vorbis_identification))), sound(44100, 2)}),
				track(7, 2, "A_FLAC", {el(id::codec_private, hex(flac_private)), sound(44100, 2)}),
				track(8, 2, "A_ALAC", {el(id::codec_private, hex(alac_config)), sound(44100, 2)}),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 8u);
	if (r.streams.size() != 8) return;

	CHECK_STR(r.streams[0].video.profile, "Main 10");
	CHECK_EQ(r.streams[0].video.bit_depth, 10);
	CHECK(r.streams[0].video.chroma == chroma_format::yuv420);
	CHECK(r.streams[1].video.chroma == chroma_format::yuv420);
	CHECK_EQ(r.streams[1].video.bit_depth, 8);
	CHECK(r.streams[2].video.chroma == chroma_format::yuv420);
	CHECK(!r.streams[2].video.profile.empty());
	CHECK(r.streams[3].video.chroma == chroma_format::yuv420);
	CHECK(near(r.streams[3].video.frame_rate, 25.0));

	CHECK_EQ(r.streams[4].audio.sample_rate, 48000u);
	CHECK_EQ(r.streams[4].audio.channels, 2u);
	CHECK_EQ(r.streams[4].audio.bits_per_sample, 0);
	CHECK_EQ(r.streams[5].audio.sample_rate, 44100u);
	CHECK_EQ(r.streams[5].audio.channels, 2u);
	CHECK_EQ(r.streams[6].audio.bits_per_sample, 16);
	CHECK_EQ(r.streams[7].audio.bits_per_sample, 16);
}

TEST("media mkv reads VP9's codec features and private data a muxer encoded")
{
	// Profile 1, 10 bits, 4:4:4.
	const auto features = bytes{1, 1, 1, 2, 1, 31, 3, 1, 10, 4, 1, 3};
	const auto compressed = master(id::content_encodings, {
		master(id::content_encoding, {
			       el_uint(id::content_encoding_scope, 2), el_uint(id::content_encoding_type, 0),
			       master(id::content_compression, {el_uint(id::content_comp_algo, 0)})
		       })
	});
	const auto stripped = master(id::content_encodings, {
		master(id::content_encoding, {
			       el_uint(id::content_encoding_scope, 3),
			       master(id::content_compression, {el_uint(id::content_comp_algo, 3), el(id::content_comp_settings, bytes{1, 1, 1, 2})})
		       })
	});

	const auto r = scan(cat({
		ebml_header("webm"), segment({
			info(),
			master(id::tracks, {
				track(1, 1, "V_VP9", {el(id::codec_private, features), picture_size(320, 240)}),
				track(2, 1, "V_VP9", {el(id::codec_private, zlib_stored(features)), compressed, picture_size(320, 240)}),
				track(3, 1, "V_VP9", {el(id::codec_private, bytes(features.begin() + 4, features.end())), stripped, picture_size(320, 240)}),
				// A private the scan cannot inflate is dropped, as FFmpeg drops it.
				track(4, 1, "V_VP9", {el(id::codec_private, bytes{9, 9, 9}), compressed, picture_size(320, 240)}),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_STR(r.brand, "webm");
	CHECK_EQ(r.streams.size(), 4u);
	if (r.streams.size() != 4) return;

	for (std::size_t i = 0; i < 3; ++i)
	{
		const auto& v = r.streams[i].video;
		CHECK_STR(v.profile, "Profile 1");
		CHECK_EQ(v.bit_depth, 10);
		CHECK(v.chroma == chroma_format::yuv444);
	}

	CHECK(r.streams[3].video.profile.empty());
}

TEST("media mkv inflates codec headers within the scan's budget")
{
	// Eight tracks whose 12-byte features are compressed, read with room for three.
	const auto features = bytes{1, 1, 1, 2, 1, 31, 3, 1, 10, 4, 1, 3};
	const auto compressed = master(id::content_encodings, {
		master(id::content_encoding, {
			       el_uint(id::content_encoding_scope, 2), el_uint(id::content_encoding_type, 0),
			       master(id::content_compression, {el_uint(id::content_comp_algo, 0)})
		       })
	});

	std::vector<bytes> tracks;
	for (std::uint64_t n = 1; n <= 8; ++n) tracks.push_back(track(n, 1, "V_VP9", {el(id::codec_private, zlib_stored(features)), compressed, picture_size(320, 240)}));
	const auto file = cat({ebml_header("webm"), segment({info(), master(id::tracks, tracks)})});

	libfiles::scan_limits limits;
	limits.media_inflated_bytes = 40;
	libfiles::reader reader(file);
	const auto r = libfiles::scan_media(reader, libfiles::scan_intent::index, limits);
	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 8u);
	if (r.streams.size() != 8) return;

	for (std::size_t i = 0; i < 8; ++i) CHECK(r.streams[i].video.profile.empty() == (i >= 3));
}

TEST("media mkv reads a first block only for a codec that needs it")
{
	// The Cluster begins inside the first 64 KiB, but a large block of another track comes first, so
	// reaching the block after it costs a read of its own.
	const auto file_with = [](std::vector<bytes> tracks, const bytes& block)
	{
		tracks.push_back(track(9, 2, "A_FLAC", {sound(44100, 2)}));
		return cat({
			ebml_header(), segment({
				info({el_float(id::duration, 1000.0)}), master(id::tracks, tracks), cluster({simple_block(9, bytes(100000, 0)), block})
			})
		});
	};

	// Every track described by its private data, or one a block would not help, and an AC-3 frame that
	// lies past what the scan has read: the Cluster is not read.
	const auto described = scan_counted(file_with({
		                                              track(1, 1, "V_VP9", {el(id::codec_private, bytes{4, 1, 0, 3, 1, 8}), picture_size(320, 240)}),
		                                              track(2, 1, "V_VP8", {picture_size(320, 240)}),
		                                              track(3, 2, "A_AC3", {sound(48000, 2)}),
	                                              }, simple_block(3, hex(ac3_frame))));
	CHECK(described.result.success);
	CHECK_EQ(described.reads, 1u);
	if (described.result.streams.size() == 4) CHECK_EQ(described.result.streams[2].audio.channels, 2u);

	// Motion JPEG keeps its sampling in its frames, so its first block is read: a read more, and no
	// more of the frame than its headers need.
	const auto needed = scan_counted(file_with({track(1, 1, "V_MJPEG", {picture_size(320, 240)})},
	                                           simple_block(1, cat({hex(jpeg_frame), bytes(1000000, 0)}))));
	CHECK(needed.result.success);
	CHECK(needed.reads > described.reads);
	CHECK(needed.reads <= 3);
	CHECK(needed.bytes_read <= 3u * 64 * 1024);

	// Encrypted frames cannot be read, so they are not.
	const auto encrypted = scan_counted(file_with({
		                                              track(1, 1, "V_MJPEG", {
			                                                    picture_size(320, 240), master(id::content_encodings, {
				                                                    master(id::content_encoding, {el_uint(id::content_encoding_type, 1)})
			                                                    })
		                                                    })
	                                              }, simple_block(1, hex(jpeg_frame))));
	CHECK(encrypted.result.success);
	CHECK_EQ(encrypted.reads, 1u);
}

TEST("media mkv reads what only a first block says")
{
	// The sequence parameter set of the avcC, as a block holds it after a four-byte length.
	const auto config = hex(avc_config);
	const auto sps = cat({bytes{0, 0, 0, 0x18}, bytes(config.begin() + 8, config.begin() + 32)});
	const auto ac3 = hex(ac3_frame);

	const auto r = scan(cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0)}),
			master(id::tracks, {
				track(1, 1, "V_MJPEG", {picture_size(320, 240)}),
				track(2, 1, "V_VP9", {picture_size(320, 240)}),
				track(3, 1, "V_MPEG4/ISO/AVC", {picture_size(320, 240)}),
				track(4, 1, "V_MPEG2", {picture_size(320, 240)}),
				track(5, 1, "V_VP8"),
				// A track whose muxer stripped the sync word from every frame.
				track(6, 2, "A_AC3", {
					      sound(48000, 2), master(id::content_encodings, {
						      master(id::content_encoding, {
							             master(id::content_compression, {
								                    el_uint(id::content_comp_algo, 3), el(id::content_comp_settings, bytes{0x0B, 0x77})
							                    })
						             })
					      })
				      }),
			}),
			cluster({
				master(id::block_group, {el_uint(id::block_duration, 40), el(id::block, cat({bytes{0x81, 0, 0, 0}, hex(jpeg_frame)}))}),
				simple_block(2, hex(vp9_frame)),
				simple_block(3, sps),
				simple_block(4, hex(mpeg2_frame)),
				simple_block(5, hex(vp8_key_frame)),
				simple_block(6, bytes(ac3.begin() + 2, ac3.end())),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 6u);
	if (r.streams.size() != 6) return;

	CHECK(r.streams[0].video.chroma == chroma_format::yuv422);
	CHECK(r.streams[1].video.chroma == chroma_format::yuv420);
	CHECK_STR(r.streams[1].video.profile, "Profile 0");
	CHECK_STR(r.streams[2].video.profile, "High");
	CHECK(r.streams[3].video.chroma == chroma_format::yuv420);
	CHECK_EQ(r.streams[4].video.width, 320u);
	CHECK_EQ(r.streams[4].video.height, 240u);
	CHECK(r.streams[4].video.chroma == chroma_format::yuv420);
	CHECK_EQ(r.streams[5].audio.channels, 6u);
	CHECK_STR(r.streams[5].codec, "ac3");
}

TEST("media mkv reads the first frame of a laced block")
{
	const auto ac3 = hex(ac3_frame);
	const auto two = cat({ac3, ac3});

	// Xiph lacing gives every size but the last in runs of 255; EBML lacing gives the first as a number
	// and the rest as differences; fixed lacing divides the block evenly.
	const auto xiph = cat({bytes{1, static_cast<std::uint8_t>(ac3.size())}, two});
	const auto ebml = cat({bytes{2, static_cast<std::uint8_t>(0x80 | ac3.size()), 0xBF}, two, ac3});
	const auto fixed = cat({bytes{1}, two});

	const auto r = scan(cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0)}),
			master(id::tracks, {track(1, 2, "A_AC3", {sound(48000, 2)}), track(2, 2, "A_AC3", {sound(48000, 2)}), track(3, 2, "A_AC3", {sound(48000, 2)})}),
			cluster({simple_block(1, xiph, 0x82), simple_block(2, ebml, 0x86), simple_block(3, fixed, 0x84)}),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 3u);
	for (const auto& s : r.streams) CHECK_EQ(s.audio.channels, 6u);
}

TEST("media mkv locates attached pictures without reading them")
{
	const auto big = cat({hex(jpeg_frame), bytes(1000000, 0x55)});
	const auto file = cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0)}),
			master(id::tracks, {track(1, 2, "A_FLAC", {sound(44100, 2)})}),
			master(id::attachments, {
				attached_file("cover.png", "image/png", big),
				attached_file("Small_Cover.JPG", "IMAGE/JPEG", bytes(30, 1)),
				attached_file("cover_land.webp", "image/webp", bytes(30, 2)),
				attached_file("photo.jpg", "image/jpeg", bytes(30, 3), "On the beach"),
				attached_file("font.otf", "font/otf", bytes(30, 4)),
				attached_file("empty.png", "image/png", bytes{}),
			}),
			cluster({simple_block(1, bytes(10, 0))}),
		})
	});

	const auto c = scan_counted(file);
	const auto& r = c.result;
	CHECK(r.success);
	CHECK(c.bytes_read < 300000);
	CHECK_EQ(c.reads, 2u);
	CHECK_EQ(r.pictures.size(), 4u);
	if (r.pictures.size() != 4) return;

	CHECK(r.pictures[0].location == (libfiles::byte_range{find(file, hex(jpeg_frame)), big.size()}));
	CHECK_STR(r.pictures[0].mime_type, "image/png");
	CHECK_EQ(r.pictures[0].type, 3);
	CHECK_EQ(r.pictures[1].type, 3);
	CHECK_STR(r.pictures[1].mime_type, "IMAGE/JPEG");
	CHECK_EQ(r.pictures[2].type, 3);
	CHECK_EQ(r.pictures[3].type, 0);

	// The FileDescription, else the attachment's name.
	CHECK_STR(r.pictures[0].description, "cover.png");
	CHECK_STR(r.pictures[1].description, "Small_Cover.JPG");
	CHECK_STR(r.pictures[2].description, "cover_land.webp");
	CHECK_STR(r.pictures[3].description, "On the beach");

	for (const auto& p : r.pictures)
	{
		CHECK(p.data.empty());
		CHECK(p.location.offset + p.location.size <= file.size());
	}
}

TEST("media mkv follows SeekHeads past a large cluster at few reads")
{
	std::vector<bytes> blocks;
	for (auto i = 0; i < 64; ++i) blocks.push_back(simple_block(1, bytes(256 * 1024, static_cast<std::uint8_t>(i))));

	segment_builder s;
	const auto head = s.add_seek_head();
	const auto info_index = s.add(info({el_float(id::duration, 60000.0), el_str(id::muxing_app, "mux")}));
	const auto tracks_index = s.add(master(id::tracks, {
		track(1, 1, "V_MPEG4/ISO/AVC", {el(id::codec_private, hex(avc_config)), picture_size(320, 240)})
	}));
	s.add(cluster(blocks));
	const auto cues = s.add(el(id::cues, bytes(100, 0)));
	const auto tags = s.add(master(id::tags, {tag({}, {simple_tag("ARTIST", "At the end")})}));
	const auto attachments = s.add(master(id::attachments, {attached_file("cover.jpg", "image/jpeg", hex(jpeg_frame))}));
	const auto second = s.add_seek_head();

	// The first SeekHead knows only Info, Tracks and the second, which knows the rest.
	s.point(head, {{id::info, info_index}, {id::tracks, tracks_index}, {id::seek_head, second}});
	s.point(second, {{id::cues, cues}, {id::tags, tags}, {id::attachments, attachments}});
	const auto file = s.build(ebml_header());
	CHECK(file.size() > 16u * 1024 * 1024);

	const auto c = scan_counted(file);
	const auto& r = c.result;
	CHECK(r.success);
	CHECK(c.reads <= 3);
	CHECK(c.bytes_read <= 3u * 64 * 1024);
	CHECK(near(r.duration, 60.0));
	CHECK_EQ(r.streams.size(), 1u);
	CHECK_EQ(r.tags.size(), 2u);
	if (r.tags.size() == 2) CHECK(same_tag(r.tags[1], "ARTIST", "At the end"));
	CHECK_EQ(r.pictures.size(), 1u);
	if (!r.pictures.empty()) CHECK_EQ(r.pictures[0].location.offset, find(file, hex(jpeg_frame)));
}

TEST("media mkv reads an unknown-size segment and cluster among Void and CRC-32 elements")
{
	const auto crc = el(id::crc32, bytes{1, 2, 3, 4});

	segment_builder s;
	s.add(el(id::void_element, bytes(7, 0)));
	s.add(master(id::info, {crc, el_uint(id::timestamp_scale, 1000000), el(id::void_element, bytes(3, 0)), el_float(id::duration, 3000.0)}));
	s.add(master(id::tracks, {crc, track(1, 1, "V_MJPEG", {picture_size(320, 240), el(id::void_element, bytes{})})}));
	s.add(master(id::tags, {crc, tag({}, {simple_tag("TITLE", "Unknown sizes")})}));
	s.add(el_unknown(id::cluster, cat({el_uint(id::cluster_timestamp, 0), crc, el(id::void_element, bytes(2, 0)), simple_block(1, hex(jpeg_frame))})));
	s.add(el_unknown(id::cluster, cat({el_uint(id::cluster_timestamp, 40), simple_block(1, bytes(30, 0))})));
	const auto file = s.build(ebml_header(), true);

	const auto r = scan(file);
	CHECK(r.success);
	CHECK(near(r.duration, 3.0));
	CHECK_EQ(r.streams.size(), 1u);
	CHECK_EQ(r.tags.size(), 1u);
	if (!r.tags.empty()) CHECK(same_tag(r.tags[0], "TITLE", "Unknown sizes"));
	if (r.streams.size() == 1) CHECK_EQ(r.streams[0].video.width, 320u);

	// The Void before the Segment, too.
	const auto padded = cat({ebml_header(), el(id::void_element, bytes(5, 0)), segment({info(), master(id::tracks, {track(1, 2, "A_OPUS")})})});
	CHECK(scan(padded).success);
}

TEST("media mkv reads the first block of a Cluster of unknown size")
{
	segment_builder s;
	s.add(info({el_float(id::duration, 3000.0)}));
	s.add(master(id::tracks, {track(1, 1, "V_MJPEG", {picture_size(320, 240)})}));
	s.add(el_unknown(id::cluster, cat({el_uint(id::cluster_timestamp, 0), el(id::crc32, bytes{1, 2, 3, 4}), simple_block(1, hex(jpeg_frame))})));
	const auto r = scan(s.build(ebml_header(), true));
	CHECK(r.success);
	if (r.streams.size() == 1) CHECK(r.streams[0].video.chroma == chroma_format::yuv422);
}

TEST("media mkv files its tags under the properties FFmpeg gave them")
{
	// tagged.mkv's tags, as Diffractor reads them, and an encoder named twice: MuxingApp is the one
	// FFmpeg reports.
	const auto r = scan(cat({
		ebml_header(), segment({
			info({el_float(id::duration, 1000.0), el_str(id::muxing_app, "the muxer"), el_str(id::writing_app, "the writer")}),
			master(id::tracks, {track(1, 1, "V_VP8", {picture_size(16, 16)})}),
			master(id::tags, {
				tag({}, {
					    simple_tag("TITLE", "Matroska Test Title"), simple_tag("ARTIST", "Matroska Test Artist"), simple_tag("GENRE", "TestGenre"),
					    simple_tag("COMMENT", "matroska test comment"), simple_tag("RATING", "4"), simple_tag("KEYWORDS", "alpha,beta")
				    }),
				// Album-level under its TargetType, and a track's own title, are not the recording's title.
				tag({el_str(id::target_type, "ALBUM")}, {simple_tag("TITLE", "The Album")}),
				tag({el_uint(id::tag_track_uid, 11)}, {simple_tag("TITLE", "The Track")}),
			}),
		})
	}));

	CHECK(r.success);
	const auto& p = r.properties;
	CHECK_STR(p.title, "Matroska Test Title");
	CHECK_STR(p.artist, "Matroska Test Artist");
	CHECK_STR(p.genre, "TestGenre");
	CHECK_STR(p.encoder, "the muxer");
	CHECK(p.rating == 4);
	CHECK_EQ(p.comments.size(), 1u);
	if (!p.comments.empty()) CHECK_STR(p.comments[0].text, "matroska test comment");
	CHECK_EQ(p.keywords.size(), 2u);

	// A title in a stated language is the title still, where FFmpeg keys it apart as "TITLE-ger".
	const auto german = scan(cat({
		ebml_header(), segment({
			info(), master(id::tracks, {track(1, 2, "A_OPUS")}),
			master(id::tags, {tag({}, {simple_tag("TITLE", "Der Titel", {el_str(id::tag_language, "ger")})})}),
		})
	}));

	CHECK(german.success);
	CHECK_STR(german.properties.title, "Der Titel");
	CHECK_EQ(german.tags.size(), 1u);
	if (!german.tags.empty()) CHECK(same_tag(german.tags[0], "TITLE", "Der Titel", "ger"));
}

TEST("media mkv refuses what it cannot read")
{
	const auto body = segment({info(), master(id::tracks, {track(1, 2, "A_OPUS")})});
	const auto refused = [](const bytes& file)
	{
		const auto r = scan(file);
		CHECK(!r.success);
		CHECK(!r.error.empty());
		CHECK(r.format == libfiles::media_format::matroska);
		CHECK(r.streams.empty());
	};

	CHECK(scan(cat({ebml_header(), body})).success);

	// An EBML or a Matroska version FFmpeg does not read, an EBML header of unknown size, and none at all.
	refused(cat({ebml_header("matroska", 2, 2), body}));
	refused(cat({ebml_header("matroska", 4), body}));
	refused(cat({el_unknown(id::ebml, bytes{}), body}));
	refused(cat({id_bytes(id::ebml), bytes(20, 0)}));

	// No Segment, and Segments without Info or Tracks.
	refused(cat({ebml_header(), el(id::cues, bytes(10, 0)), body}));
	refused(cat({ebml_header(), segment({master(id::tracks, {track(1, 2, "A_OPUS")})})}));
	refused(cat({ebml_header(), segment({info()})}));

	// Tracks whose only entry is damaged, and Tracks that runs past the end of the file.
	refused(cat({ebml_header(), segment({info(), el(id::tracks, bytes{0xAE, 0x90, 1, 2})})}));
	auto cut = cat({ebml_header(), body});
	cut.resize(cut.size() - 2);
	refused(cut);
}

TEST("media mkv survives files cut short")
{
	const auto s = sample_file();
	const auto whole = scan(s.file);
	CHECK(whole.success);
	if (!whole.success) return;

	for (std::size_t size = 16; size < s.file.size(); ++size)
	{
		const bytes cut(s.file.begin(), s.file.begin() + static_cast<std::ptrdiff_t>(size));
		const auto r = scan(cut);
		check_sound(r, size);

		// A file is read once Tracks is whole, keeping what lies before the cut.
		CHECK(r.success == (size >= s.tracks_end));
		if (!r.success) continue;

		CHECK(near(r.duration, whole.duration));
		check_sample_streams(r);
		CHECK(in_order(r.tags, whole.tags, same_tags));
		CHECK(in_order(r.pictures, whole.pictures, same_pictures));
	}
}

TEST("media mkv survives corrupted sizes and IDs")
{
	const auto s = sample_file();

	for (std::size_t i = 0; i < s.file.size(); ++i)
	{
		for (const int change : {0x00, 0xFF, 0x80, 0x01, 0x40, 0x7F})
		{
			auto damaged = s.file;
			damaged[i] = static_cast<std::uint8_t>(change == 0x00 || change == 0xFF ? change : damaged[i] ^ change);
			check_sound(scan(damaged), damaged.size());
		}
	}

	// Sizes that claim the whole of a 64-bit range.
	for (std::size_t i = 0; i + 8 <= s.file.size(); i += 7)
	{
		auto damaged = s.file;
		damaged[i] = 0x01;
		for (std::size_t j = 1; j < 8; ++j) damaged[i + j] = 0xFE;
		check_sound(scan(damaged), damaged.size());
	}
}

TEST("media mkv reports its structure for inspection")
{
	const auto s = sample_file();
	const auto r = scan(s.file, libfiles::scan_intent::inspect);
	CHECK(r.success);

	const auto has = [&](const std::string_view key, const std::string_view value_start)
	{
		return std::ranges::any_of(r.structure, [&](const libfiles::structure_row& row)
		{
			return row.key == key && row.value.starts_with(value_start);
		});
	};

	CHECK(has("Summary (7)", ""));
	CHECK(has("DocType", "matroska, version 4, read version 2"));
	CHECK(has("Segment elements (10)", ""));
	CHECK(has("SeekHead", "offset 52, "));
	CHECK(has("Void", ""));
	CHECK(has("Info", ""));
	CHECK(has("Cluster", ""));
	CHECK(has("Cues", ""));
	CHECK(has("Attachments", ""));
	CHECK(has("Tracks (3)", ""));
	CHECK(has("Track 1", "video, V_MPEG4/ISO/AVC, 320 x 240, ger"));
	CHECK(has("Track 2", "audio, A_AAC, 48000 Hz, 2 channels, eng"));
	CHECK(has("Track 3", "subtitle, S_TEXT/UTF8, fr-CA"));

	// Index scans list nothing.
	CHECK(scan(s.file).structure.empty());
}

TEST("media mkv reads WebM")
{
	const auto r = scan(cat({
		ebml_header("webm"), segment({
			info({el_float(id::duration, 2500.0), el_uint(id::timestamp_scale, 1000000)}),
			master(id::tracks, {
				track(1, 1, "V_VP8", {picture_size(640, 360, {el_uint(id::alpha_mode, 1)})}),
				track(2, 2, "A_OPUS", {el(id::codec_private, hex(opus_head)), sound(48000, 2)}),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_STR(r.brand, "webm");
	CHECK(near(r.duration, 2.5));
	CHECK(r.complete);
	CHECK_EQ(r.streams.size(), 2u);
	if (r.streams.size() != 2) return;

	CHECK_STR(r.streams[0].codec, "vp8");
	CHECK(r.streams[0].video.alpha);
	CHECK_STR(r.streams[1].codec, "opus");
	CHECK_EQ(r.streams[1].audio.sample_rate, 48000u);
}

TEST("media mkv reads a stream's language")
{
	// Matroska's default is English; LanguageBCP47 replaces Language; "und", either way, names none.
	const auto r = scan(cat({
		ebml_header(), segment({
			info(),
			master(id::tracks, {
				track(1, 2, "A_OPUS"),
				track(2, 2, "A_OPUS", {el_str(id::language, "ger")}),
				track(3, 2, "A_OPUS", {el_str(id::language, "und")}),
				track(4, 0x11, "S_TEXT/UTF8", {el_str(id::language, "fre"), el_str(id::language_bcp47, "fr-CA")}),
				track(5, 0x11, "S_TEXT/UTF8", {el_str(id::language, "fre"), el_str(id::language_bcp47, "und")}),
				track(6, 2, "A_OPUS", {el_str(id::language, "")}),
			}),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 6u);
	if (r.streams.size() != 6) return;

	CHECK_STR(r.streams[0].language, "eng");
	CHECK_STR(r.streams[1].language, "ger");
	CHECK_STR(r.streams[2].language, "");
	CHECK_STR(r.streams[3].language, "fr-CA");
	CHECK_STR(r.streams[4].language, "");
	CHECK_STR(r.streams[5].language, "eng");
}

TEST("media mkv reads a TimestampScale other than a millisecond")
{
	const auto r = scan(cat({
		ebml_header(), segment({
			master(id::info, {el_float(id::duration, 90000.0), el_uint(id::timestamp_scale, 100000)}),
			master(id::tracks, {track(1, 2, "A_FLAC", {sound(96000, 2)})}),
		})
	}));

	CHECK(r.success);
	CHECK(near(r.duration, 9.0));
}

TEST("media mkv bounds what a crafted file makes it hold")
{
	// Tracks past the thousand FFmpeg would open, all sharing one UID that a Tag of many names targets.
	std::vector<bytes> entries;

	for (std::uint64_t n = 1; n <= 1500; ++n)
	{
		entries.push_back(master(id::track_entry, {el_uint(id::track_number, n), el_uint(id::track_uid, 7), el_uint(id::track_type, 2), el_str(id::codec_id, "A_OPUS")}));
	}

	std::vector<bytes> names;
	for (auto i = 0; i < 5000; ++i) names.push_back(simple_tag("NAME" + std::to_string(i), "value"));

	// SimpleTags nested forty deep, and twenty thousand Tags that hold nothing.
	auto nested = simple_tag("DEEP", "bottom");
	for (auto i = 0; i < 40; ++i) nested = simple_tag("DEEP", "level", {nested});

	bytes empty_tags;
	for (auto i = 0; i < 20000; ++i) append(empty_tags, bytes{0x73, 0x73, 0x80});

	const auto r = scan(cat({
		ebml_header(), segment({
			info(), master(id::tracks, entries),
			master(id::tags, {tag({}, {nested}), tag({el_uint(id::tag_track_uid, 7)}, names), tag({}, names)}),
			el(id::tags, empty_tags),
		})
	}));

	CHECK(r.success);
	CHECK_EQ(r.streams.size(), 1000u);

	std::size_t stream_tags = 0;
	for (const auto& s : r.streams) stream_tags += s.tags.size();
	CHECK(stream_tags <= 4096);
	CHECK(r.tags.size() <= 4096);

	// The nesting is followed sixteen deep.
	std::size_t deepest = 0;
	for (const auto& t : r.tags) deepest = std::max(deepest, static_cast<std::size_t>(std::ranges::count(t.key, '/')));
	CHECK_EQ(deepest, 15u);
}