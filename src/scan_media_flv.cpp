// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning FLV files: onMetaData's values as tags, and what they say of the file and its
// streams; the first audio and video tags' codecs, with the sequence headers of AVC and AAC and of the
// codecs enhanced RTMP adds, and the picture headers of Flash's own video codecs; and the duration, from
// onMetaData or else the last tag's timestamp. The streams are the ones whose tags are seen, since the
// header's flags can be wrong, and tags are read from the start only until both are known or a bound is
// reached. The packet is onXMPData's, as the writer reads it.

#include "bmff_internal.h"
#include "flv_internal.h"
#include "media_internal.h"
#include "write_internal.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		// Enhanced RTMP names its codecs by their ISO base media sample entries.
		using bmff::fourcc;

		// How far the tags are read for the streams' codecs, onMetaData aside.
		constexpr std::uint64_t max_scan_bytes = 256 * 1024;
		constexpr int max_scan_tags = 512;
		// How many tags are stepped back over from the end for a timestamp, past script tags at time zero.
		constexpr int max_tail_tags = 64;
		// The most of a tag read for a codec configuration, and what is read first of an enhanced tag: its
		// header, and enough of a frame for the codecs whose frames state the picture.
		constexpr std::uint64_t max_config = 64 * 1024;
		constexpr std::uint64_t enhanced_head = 1024;
		// The longest script string taken for a tag.
		constexpr std::uint64_t max_text = 64 * 1024;

		// Enhanced RTMP's packet types, of video and of audio.
		constexpr unsigned sequence_start = 0;
		constexpr unsigned coded_frames = 1;
		constexpr unsigned coded_frames_x = 3;
		constexpr unsigned video_metadata = 4;
		constexpr unsigned video_multitrack = 6;
		constexpr unsigned audio_multichannel = 4;
		constexpr unsigned audio_multitrack = 5;
		constexpr unsigned mod_ex = 7;
		// A multitrack tag's first nibble: one track, or several each with its size.
		constexpr unsigned one_track = 0;
		// A video tag's frame type for a command or information frame, which holds no picture.
		constexpr unsigned command_frame = 5;

		std::string_view video_codec(const unsigned id)
		{
			switch (id)
			{
			case 2: return "flv1";
			case 3: return "flashsv";
			case 4: return "vp6f";
			case 5: return "vp6a";
			case 6: return "flashsv2";
			case 7: return "h264";
			// What FFmpeg reads beyond the specification: H.263 itself, MPEG-4 Part 2, and HEVC as Chinese
			// streaming services carried it before enhanced RTMP.
			case 8: return "h263";
			case 9: return "mpeg4";
			case 12: return "hevc";
			default: return {};
			}
		}

		std::string_view video_codec_fourcc(const std::uint32_t id)
		{
			switch (id)
			{
			case fourcc("avc1"): return "h264";
			case fourcc("hvc1"): return "hevc";
			case fourcc("av01"): return "av1";
			case fourcc("vp09"): return "vp9";
			case fourcc("vp08"): return "vp8";
			case fourcc("vvc1"): return "vvc";
			default: return {};
			}
		}

		std::string_view audio_codec_fourcc(const std::uint32_t id)
		{
			switch (id)
			{
			case fourcc("mp4a"): return "aac";
			case fourcc("Opus"): return "opus";
			case fourcc(".mp3"): return "mp3";
			case fourcc("fLaC"): return "flac";
			case fourcc("ac-3"): return "ac3";
			case fourcc("ec-3"): return "eac3";
			default: return {};
			}
		}

		// Sorenson Spark's picture header, which every frame starts with: a 17-bit start code, a version, a
		// temporal reference, and the picture size - one of five fixed sizes, or one stated in 8 or 16 bits
		// a side.
		bool sorenson_size(const std::span<const std::uint8_t> frame, video_header& out)
		{
			bit_reader b(frame);
			if (b.u(17) != 1 || b.u(5) > 1) return false;
			b.skip(8);

			std::uint32_t width = 0;
			std::uint32_t height = 0;

			switch (b.u(3))
			{
			case 0: width = b.u(8);
				height = b.u(8);
				break;
			case 1: width = b.u(16);
				height = b.u(16);
				break;
			case 2: width = 352;
				height = 288;
				break;
			case 3: width = 176;
				height = 144;
				break;
			case 4: width = 128;
				height = 96;
				break;
			case 5: width = 320;
				height = 240;
				break;
			case 6: width = 160;
				height = 120;
				break;
			default: return false;
			}

			if (b.overrun() || width == 0 || height == 0) return false;

			out.width = width;
			out.height = height;
			return true;
		}

		// A Screen Video packet starts with its block and image sizes, 4 and 12 bits each.
		bool screen_size(const std::span<const std::uint8_t> packet, video_header& out)
		{
			if (packet.size() < 4) return false;

			const auto width = static_cast<std::uint32_t>((packet[0] & 0x0F) << 8 | packet[1]);
			const auto height = static_cast<std::uint32_t>((packet[2] & 0x0F) << 8 | packet[3]);
			if (width == 0 || height == 0) return false;

			out.width = width;
			out.height = height;
			return true;
		}

		// A VP6 key frame's header gives the picture in macroblocks; FLV's adjustment byte, before the
		// frame, takes what is not shown off the right and the bottom.
		bool vp6_size(const std::span<const std::uint8_t> frame, const std::uint8_t adjustment, video_header& out)
		{
			if (frame.size() < 2 || (frame[0] & 0x80) != 0 || frame[1] >> 3 > 8) return false;

			// Two bytes of coefficient offset come first when the frame separates its coefficients, or
			// carries no filter header.
			const std::size_t at = (frame[0] & 1) != 0 || (frame[1] & 0x06) == 0 ? 4 : 2;
			if (frame.size() < at + 2) return false;

			const auto rows = frame[at];
			const auto columns = frame[at + 1];
			if (rows == 0 || columns == 0) return false;

			const auto width = 16u * columns;
			const auto height = 16u * rows;
			const auto crop_width = static_cast<unsigned>(adjustment >> 4);
			const auto crop_height = adjustment & 0x0Fu;
			if (crop_width >= width || crop_height >= height) return false;

			out.width = width - crop_width;
			out.height = height - crop_height;
			return true;
		}

		std::string number_text(const double v)
		{
			char text[32];
			const auto result = std::to_chars(text, text + sizeof(text), v);
			return {text, result.ptr};
		}

		struct track
		{
			std::optional<std::size_t> index;
			bool done = false;
		};

		// What onMetaData says of the file and its streams.
		struct metadata_values
		{
			double duration = 0.0;
			double width = 0.0;
			double height = 0.0;
			double frame_rate = 0.0;
			double video_data_rate = 0.0;
			double audio_data_rate = 0.0;
			double sample_rate = 0.0;
			std::optional<bool> stereo;
		};

		class tag_reader
		{
		public:
			tag_reader(reader& r, const scan_intent intent, media_scan_result& out) :
				_r(r),
				_inspect(intent == scan_intent::inspect),
				_out(out)
			{
			}

			void read()
			{
				_first = flv::first_tag(_r);
				header_rows();

				auto pos = _first;
				std::uint64_t scanned = 0;

				for (auto n = 0; n < max_scan_tags && scanned <= max_scan_bytes; ++n)
				{
					const auto t = flv::read_tag(_r, pos);
					if (!t) break;

					auto counted = true;

					if (t->type == flv::script_tag) counted = !script(*t);
					else if (t->type == flv::audio_tag) audio(*t);
					else if (t->type == flv::video_tag) video(*t);

					if (counted) scanned += t->end() - t->start;
					if (_audio.done && _video.done) break;
					pos = t->end();
				}

				finish();
				if (_inspect) finish_structure_sections(_rows);
			}

			std::vector<structure_row> rows() { return std::move(_rows); }

		private:
			void header_rows()
			{
				if (!_inspect) return;

				const auto flags = _r.u8(4);
				std::string declared;
				if (flags & flv::has_audio) declared = "audio";
				if (flags & flv::has_video) declared += declared.empty() ? "video" : ", video";

				add_structure_section(_rows, "Header", "flv.header", true);
				add_structure_row(_rows, "Version", std::to_string(_r.u8(3)));
				add_structure_row(_rows, "Streams declared", declared.empty() ? std::string("none") : declared);
				add_structure_row(_rows, "Header size", std::format("{} bytes", _first - 4));
				add_structure_section(_rows, "Tags", "flv.tags", true);
			}

			void tag_row(const flv::tag& t, std::string key, const std::string_view what)
			{
				if (!_inspect) return;

				auto value = std::format("offset {}, {} bytes, at {} ms", t.start, t.data_size, t.timestamp);
				if (!what.empty()) value = std::format("{}, {}", what, value);

				auto& row = add_row(_rows, std::move(key), std::move(value));
				row.depth = 1;
				row.id = std::format("flv.tag.{}", t.start);
			}

			// Returns true for the onMetaData that is read, which the scan's bound does not count.
			bool script(const flv::tag& t)
			{
				std::uint64_t after = 0;
				const auto name = flv::script_name(_r, t, after);

				if (_inspect)
				{
					const auto shown = text_from_unknown(as_bytes(name));
					tag_row(t, shown.empty() ? std::string("Script data") : shown, "script data");
				}

				if (name != "onMetaData" || _metadata_read) return false;

				_metadata_read = true;
				flv::amf a(_r, t.data() + t.data_size);
				a.on_value = [this](const flv::amf_value& v) { metadata_value(v); };
				a.value(after);
				return true;
			}

			static std::span<const std::uint8_t> as_bytes(const std::string_view text)
			{
				return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
			}

			// One of onMetaData's own values, as a tag. What nests in its objects and arrays - the keyframe
			// index, track descriptions - describes the media rather than the recording.
			void metadata_value(const flv::amf_value& v)
			{
				if (v.depth != 1 || v.in_array) return;

				const auto key = text_from_unknown(as_bytes(v.key));
				if (key.empty()) return;

				std::string text;

				switch (v.type)
				{
				case flv::amf_number:
					{
						const auto n = std::bit_cast<double>(_r.be64(v.at));
						if (!std::isfinite(n)) return;

						remember(key, n);
						text = number_text(n);
						break;
					}
				case flv::amf_boolean:
					{
						const auto b = _r.u8(v.at) != 0;
						if (key == "stereo") _metadata.stereo = b;
						text = b ? "true" : "false";
						break;
					}
				case flv::amf_string:
				case flv::amf_long_string:
					if (v.length > max_text) return;
					text = text_from_unknown(_r.view(v.at, static_cast<std::size_t>(v.length)));
					break;
				case flv::amf_date:
					text = date_text(v.at);
					break;
				default:
					return;
				}

				add_tag(_out, tag_family::flv, key, text);
			}

			void remember(const std::string_view key, const double n)
			{
				if (key == "duration") _metadata.duration = n;
				else if (key == "width") _metadata.width = n;
				else if (key == "height") _metadata.height = n;
				else if (key == "framerate") _metadata.frame_rate = n;
				else if (key == "videodatarate") _metadata.video_data_rate = n;
				else if (key == "audiodatarate") _metadata.audio_data_rate = n;
				else if (key == "audiosamplerate") _metadata.sample_rate = n;
				else if (key == "stereo") _metadata.stereo = n != 0;
			}

			// An AMF date: milliseconds since 1970 in UTC, and a time zone the specification reserves and
			// FFmpeg ignores, as this does.
			std::string date_text(const std::uint64_t at)
			{
				const auto milliseconds = std::bit_cast<double>(_r.be64(at));
				if (!std::isfinite(milliseconds) || std::abs(milliseconds) > 3e17) return {};

				const auto d = utc_date_from_seconds(static_cast<std::int64_t>(std::floor(milliseconds / 1000)));
				if (!d.valid()) return {};

				return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", d.year, d.month, d.day, d.hour, d.minute, d.second);
			}

			media_stream& stream(track& t, const media_stream_kind kind, const std::uint64_t id, bool& created)
			{
				created = !t.index;

				if (created)
				{
					t.index = _out.streams.size();
					auto& s = _out.streams.emplace_back();
					s.kind = kind;
					s.id = id;
				}

				return _out.streams[*t.index];
			}

			// What comes before an enhanced tag's body: the packet type, past any ModEx data; the FourCC;
			// and for a multitrack tag the first track's id, and its size when the tag holds several.
			struct enhanced_header
			{
				unsigned packet_type = 0;
				std::uint32_t codec = 0;
				std::uint64_t track = 0;
				std::span<const std::uint8_t> body;
			};

			static bool read_enhanced(const std::span<const std::uint8_t> data, const unsigned multitrack, enhanced_header& out)
			{
				std::size_t p = 1;
				auto type = data[0] & 0x0Fu;

				for (auto i = 0; type == mod_ex; ++i)
				{
					if (i == 8 || p >= data.size()) return false;

					std::size_t size = data[p++] + 1u;

					if (size == 256)
					{
						if (data.size() - p < 2) return false;
						size = load_be16(data.data() + p) + 1u;
						p += 2;
					}

					if (data.size() - p < size + 1) return false;
					p += size;
					type = data[p++] & 0x0Fu;
				}

				auto tracks = one_track;
				const auto many = type == multitrack;

				if (many)
				{
					if (p >= data.size()) return false;
					tracks = data[p] & 0xF0u;
					type = data[p++] & 0x0Fu;
				}

				if (data.size() - p < 4) return false;
				out.codec = load_be32(data.data() + p);
				p += 4;

				auto size = data.size() - p;

				if (many)
				{
					if (p >= data.size()) return false;
					out.track = data[p++];

					if (tracks != one_track)
					{
						if (data.size() - p < 3) return false;
						const auto track_size = static_cast<std::size_t>(data[p]) << 16 | static_cast<std::size_t>(data[p + 1]) << 8 | data[p + 2];
						p += 3;
						size = std::min(data.size() - p, track_size);
					}
					else
					{
						size = data.size() - p;
					}
				}

				out.packet_type = type;
				out.body = data.subspan(p, size);
				return true;
			}

			void audio(const flv::tag& t)
			{
				if (_audio.done || t.data_size == 0) return;

				// The flags, and enough after them for an MPEG audio frame header or AAC's packet type.
				const auto data = _r.read(t.data(), static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size, 16)));
				const auto flags = data[0];
				const auto format = flags >> 4u;

				if (format == 9)
				{
					enhanced_audio(t);
					return;
				}

				auto created = false;
				auto& s = stream(_audio, media_stream_kind::audio, 0, created);

				if (created)
				{
					static constexpr std::uint32_t rates[] = {5512, 11025, 22050, 44100};

					s.codec_tag = std::to_string(format);
					s.audio.sample_rate = rates[flags >> 2 & 3];
					s.audio.channels = (flags & 1) != 0 ? 2 : 1;

					switch (format)
					{
					case 0: // in the byte order of the machine that wrote it, where 3 is little-endian
					case 3:
						s.codec = "pcm";
						s.audio.bits_per_sample = (flags & 2) != 0 ? 16 : 8;
						break;
					case 1: s.codec = "adpcm_swf";
						break;
					case 2: s.codec = "mp3";
						break;
					case 14: s.codec = "mp3";
						s.audio.sample_rate = 8000;
						break;
					case 4: s.codec = "nellymoser";
						s.audio.sample_rate = 16000;
						s.audio.channels = 1;
						break;
					case 5: s.codec = "nellymoser";
						s.audio.sample_rate = 8000;
						s.audio.channels = 1;
						break;
					case 6: s.codec = "nellymoser";
						break;
					case 7: s.codec = "alaw";
						s.audio.sample_rate = 8000;
						break;
					case 8: s.codec = "mulaw";
						s.audio.sample_rate = 8000;
						break;
					case 10: s.codec = "aac";
						break;
					case 11: s.codec = "speex";
						s.audio.sample_rate = 16000;
						s.audio.channels = 1;
						break;
					default:
						break;
					}

					tag_row(t, "Audio", s.codec.empty() ? std::format("format {}", format) : s.codec);
				}

				if (t.filtered)
				{
					_audio.done = true;
					return;
				}

				audio_header h;

				if (format == 10)
				{
					// Raw frames until the sequence header, AACPacketType 0, gives the AudioSpecificConfig.
					if (data.size() < 2 || data[1] != 0) return;

					const auto config = _r.read(t.data() + 2, static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size - 2, max_config)));
					if (parse_aac_config(config, h)) apply_audio_header(h, s.audio);
				}
				else if ((format == 2 || format == 14) && data.size() >= 5)
				{
					std::string codec;
					if (parse_mpeg_audio_header(load_be32(data.data() + 1), h, codec)) apply_audio_header(h, s.audio);
				}

				_audio.done = true;
			}

			void enhanced_audio(const flv::tag& t)
			{
				enhanced_header e;
				auto data = _r.read(t.data(), static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size, enhanced_head)));
				if (!read_enhanced(data, audio_multitrack, e)) return;

				if (e.packet_type == sequence_start && t.data_size > data.size())
				{
					data = _r.read(t.data(), static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size, max_config)));
					if (!read_enhanced(data, audio_multitrack, e)) return;
				}

				auto created = false;
				auto& s = stream(_audio, media_stream_kind::audio, e.track, created);

				if (created)
				{
					s.codec = audio_codec_fourcc(e.codec);
					s.codec_tag = box_type_text(e.codec);
					tag_row(t, "Audio", s.codec.empty() ? s.codec_tag : s.codec);
				}

				if (t.filtered || s.codec.empty())
				{
					_audio.done = true;
					return;
				}

				// A count of channels, ahead of the configuration.
				if (e.packet_type == audio_multichannel)
				{
					if (e.body.size() >= 2 && e.body[1] != 0) s.audio.channels = e.body[1];
					return;
				}

				// MP3, AC-3 and E-AC-3 are described by their frames; the others by their sequence start.
				const auto framed = s.codec == "mp3" || s.codec == "ac3" || s.codec == "eac3";
				audio_header h;
				std::string codec;
				auto found = false;

				if (e.packet_type == sequence_start && !framed)
				{
					if (s.codec == "aac")
					{
						found = parse_aac_config(e.body, h);
					}
					else if (s.codec == "opus")
					{
						std::uint32_t pre_skip = 0;
						found = parse_opus_head(e.body, h, pre_skip);
					}
					else if (s.codec == "flac")
					{
						// STREAMINFO, after the "fLaC" marker and its block header when the tag keeps them.
						auto info = e.body;
						if (starts_with(info, "fLaC") && info.size() >= 8) info = info.subspan(8);
						std::uint64_t samples = 0;
						found = info.size() >= 34 && parse_flac_streaminfo(info.first(34), h, samples);
					}

					_audio.done = true;
				}
				else if (e.packet_type == coded_frames && framed && !e.body.empty())
				{
					if (s.codec == "mp3") found = e.body.size() >= 4 && parse_mpeg_audio_header(load_be32(e.body.data()), h, codec);
					else found = parse_ac3_frame(e.body, h, codec);

					_audio.done = found;
				}

				if (found) apply_audio_header(h, s.audio);
			}

			void video(const flv::tag& t)
			{
				if (_video.done || t.data_size == 0) return;

				const auto first = _r.u8(t.data());
				const auto enhanced = (first & 0x80) != 0;
				if ((first >> 4 & 7u) == command_frame && enhanced) return;
				if (first >> 4 == command_frame && !enhanced) return;

				if (enhanced)
				{
					enhanced_video(t);
					return;
				}

				const auto codec = first & 0x0Fu;
				auto created = false;
				auto& s = stream(_video, media_stream_kind::video, 0, created);

				if (created)
				{
					s.codec = video_codec(codec);
					s.codec_tag = std::to_string(codec);
					// The codec id says so: VP6 with an alpha channel.
					if (codec == 5) s.video.alpha = true;
					tag_row(t, "Video", s.codec.empty() ? std::format("codec {}", codec) : s.codec);
				}

				if (t.filtered || s.codec.empty())
				{
					_video.done = true;
					return;
				}

				video_header h;
				auto found = false;

				if (codec == 7 || codec == 9 || codec == 12)
				{
					// The packet type, 0 for the sequence header, and a composition time come before the configuration.
					const auto head = _r.read(t.data(), static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size, 5)));
					if (head.size() < 5 || head[1] != 0) return;

					const auto config = _r.read(t.data() + 5, static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size - 5, max_config)));
					if (codec == 7) found = parse_avc_config(config, h);
					else if (codec == 12) found = parse_hevc_config(config, h);
					else found = parse_mpeg4_video(config, h);

					// A sequence header that cannot be read is not followed by a better one.
					_video.done = true;
				}
				else
				{
					const auto head = _r.read(t.data(), static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size, 32)));
					const auto packet = std::span(head).subspan(1);

					if (codec == 2) found = sorenson_size(packet, h);
					else if (codec == 3 || codec == 6) found = screen_size(packet, h);
					else if (codec == 4 && packet.size() >= 2) found = vp6_size(packet.subspan(1), packet[0], h);
					else if (codec == 5 && packet.size() >= 5) found = vp6_size(packet.subspan(4), packet[0], h);
					else found = true;

					_video.done = found;
				}

				if (found) apply_video_header(h, s.video);
				fixed_format(s);
			}

			void enhanced_video(const flv::tag& t)
			{
				enhanced_header e;
				auto data = _r.read(t.data(), static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size, enhanced_head)));
				if (!read_enhanced(data, video_multitrack, e)) return;

				if (e.packet_type == sequence_start && t.data_size > data.size())
				{
					data = _r.read(t.data(), static_cast<std::size_t>(std::min<std::uint64_t>(t.data_size, max_config)));
					if (!read_enhanced(data, video_multitrack, e)) return;
				}

				if (e.packet_type == video_metadata) return;

				auto created = false;
				auto& s = stream(_video, media_stream_kind::video, e.track, created);

				if (created)
				{
					s.codec = video_codec_fourcc(e.codec);
					s.codec_tag = box_type_text(e.codec);
					tag_row(t, "Video", s.codec.empty() ? s.codec_tag : s.codec);
				}

				if (t.filtered || s.codec.empty())
				{
					_video.done = true;
					return;
				}

				video_header h;
				auto found = false;

				if (e.packet_type == sequence_start)
				{
					if (s.codec == "h264") found = parse_avc_config(e.body, h);
					else if (s.codec == "hevc") found = parse_hevc_config(e.body, h);
					else if (s.codec == "av1") found = parse_av1_config(e.body, h);
					else if (s.codec == "vp9" || s.codec == "vp8")
					{
						// A vpcC box's payload: its version and flags, then the record.
						if (e.body.size() >= 4 && e.body[0] == 1) found = parse_vp_config(e.body.subspan(4), h);
					}

					_video.done = true;
				}
				else if ((e.packet_type == coded_frames || e.packet_type == coded_frames_x) && !e.body.empty())
				{
					// A key frame states the picture of the codecs that need no sequence header, and an AV1
					// one carries its sequence header among its OBUs.
					if (s.codec == "vp9") found = parse_vp9_frame(e.body, h);
					else if (s.codec == "vp8") found = parse_vp8_frame(e.body, h);
					else if (s.codec == "av1") found = parse_av1_sequence(e.body, h);
					if (found) _video.done = true;
				}

				if (found) apply_video_header(h, s.video);
				fixed_format(s);
			}

			static void fixed_format(media_stream& s)
			{
				video_header fixed;
				if (s.video.chroma == chroma_format::unknown && fixed_video_format(s.codec, fixed)) apply_video_header(fixed, s.video);
			}

			// The timestamp of the last tag, found from the size field that ends the file; a tag at time zero
			// - onMetaData written again at the end - is stepped back over.
			std::uint32_t last_timestamp()
			{
				auto end = _r.size();

				for (auto i = 0; i < max_tail_tags; ++i)
				{
					if (end - _first < flv::tag_header + 4) return 0;

					const std::uint64_t previous = _r.be32(end - 4);
					if (previous < flv::tag_header || previous > end - 4 - _first) return 0;

					const auto start = end - 4 - previous;
					std::uint32_t data_size = 0;
					std::uint32_t timestamp = 0;

					{
						const auto h = _r.view(start, flv::tag_header);
						data_size = static_cast<std::uint32_t>(h[1]) << 16 | static_cast<std::uint32_t>(h[2]) << 8 | h[3];
						timestamp = static_cast<std::uint32_t>(h[7]) << 24 | static_cast<std::uint32_t>(h[4]) << 16 |
							static_cast<std::uint32_t>(h[5]) << 8 | h[6];
					}

					if (previous != data_size + flv::tag_header) return 0;
					if (timestamp != 0) return timestamp;
					end = start;
				}

				return 0;
			}

			// onMetaData's data rates are in kilobits of 1024 bits, as FFmpeg reads and writes them.
			static std::uint64_t data_rate(const double kilobits)
			{
				return kilobits > 0 && kilobits < 1e9 ? static_cast<std::uint64_t>(std::llround(kilobits * 1024)) : 0;
			}

			// onMetaData fills what the tags did not say. Its frame rate is the container's, which a player
			// follows ahead of the codec's; its size is the encoder's word, behind the codec header's.
			void finish()
			{
				const auto& m = _metadata;

				if (_video.index)
				{
					auto& v = _out.streams[*_video.index];

					if ((v.video.width == 0 || v.video.height == 0) && m.width >= 1 && m.height >= 1 &&
						m.width <= max_image_dimension && m.height <= max_image_dimension)
					{
						v.video.width = static_cast<std::uint32_t>(m.width);
						v.video.height = static_cast<std::uint32_t>(m.height);
					}

					if (m.frame_rate > 0 && m.frame_rate < 10000) v.video.frame_rate = m.frame_rate;
					if (const auto rate = data_rate(m.video_data_rate); rate != 0) v.bit_rate = rate;
				}

				if (_audio.index)
				{
					auto& a = _out.streams[*_audio.index];

					if (a.audio.sample_rate == 0 && m.sample_rate >= 1 && m.sample_rate <= 1e6)
					{
						a.audio.sample_rate = static_cast<std::uint32_t>(m.sample_rate);
					}

					if (a.audio.channels == 0 && m.stereo) a.audio.channels = *m.stereo ? 2 : 1;
					if (const auto rate = data_rate(m.audio_data_rate); rate != 0) a.bit_rate = rate;
				}

				if (m.duration > 0) _out.duration = m.duration;
				else _out.duration = last_timestamp() / 1000.0;
			}

			reader& _r;
			bool _inspect = false;
			media_scan_result& _out;
			std::uint64_t _first = 0;
			track _audio;
			track _video;
			bool _metadata_read = false;
			metadata_values _metadata;
			std::vector<structure_row> _rows;
		};
	}

	void scan_flv_media(reader& r, const scan_intent intent, media_scan_result& out)
	{
		out.brand = "FLV";

		tag_reader t(r, intent, out);

		// The packet the writer reads and would replace, read first while the start of the file is at hand.
		try
		{
			packet_reading packet;
			read_flv_packet(r, packet);
			out.xmp.assign(packet.packet.begin(), packet.packet.end());
		}
		catch (const std::exception&)
		{
			out.xmp.clear();
		}

		t.read();
		if (intent == scan_intent::inspect) out.structure = t.rows();
	}
}
