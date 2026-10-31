// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning RIFF media: AVI, with OpenDML's extensions, and WAV, with RF64 and BW64. The forms and
// their chunks are walked by riff_internal.h as the writer walks them, so the packet is the one read_packet
// reads. An AVI is read from what comes before its movie data, as FFmpeg reads it - the main header, each
// stream's header, format, data and name, IDIT and the camera chunks - from every LIST INFO in its first
// form, those before the movie data first, and, for a video codec whose bitstream alone describes the
// picture, from the start of the stream's first chunk. A WAV is read from its fmt, fact and ds64 chunks and
// the tags around its data, which is never read.

#include "media_internal.h"
#include "riff_internal.h"
#include "tiff_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libfiles::detail
{
	using namespace riff;

	namespace
	{
		constexpr auto odml_type = fourcc("odml");
		constexpr auto ncdt_type = fourcc("ncdt");
		constexpr auto rec_type = fourcc("rec ");
		constexpr auto adtl_type = fourcc("adtl");
		constexpr auto avih_id = fourcc("avih");
		constexpr auto strh_id = fourcc("strh");
		constexpr auto strf_id = fourcc("strf");
		constexpr auto strd_id = fourcc("strd");
		constexpr auto strn_id = fourcc("strn");
		constexpr auto vprp_id = fourcc("vprp");
		constexpr auto dmlh_id = fourcc("dmlh");
		constexpr auto idx1_id = fourcc("idx1");
		constexpr auto nctg_id = fourcc("nctg");
		constexpr auto avif_id = fourcc("AVIF");
		constexpr auto vids_type = fourcc("vids");
		constexpr auto auds_type = fourcc("auds");
		constexpr auto txts_type = fourcc("txts");
		constexpr auto pads_type = fourcc("pads");
		constexpr auto iavs_type = fourcc("iavs");
		constexpr auto ivas_type = fourcc("ivas");
		constexpr auto fmt_id = fourcc("fmt ");
		constexpr auto fact_id = fourcc("fact");
		constexpr auto list_lower_id = fourcc("list");
		constexpr auto bext_id = fourcc("bext");
		constexpr auto ixml_id = fourcc("iXML");
		constexpr auto id3_id = fourcc("id3 ");
		constexpr auto id3_upper_id = fourcc("ID3 ");
		constexpr auto cue_id = fourcc("cue ");
		constexpr auto smpl_id = fourcc("smpl");

		// The stream tags FFmpeg reads from a stream's header list besides strn: an older writer's lower-case
		// INFO names.
		constexpr std::uint32_t stream_tag_ids[] = {
			fourcc("isbj"), fourcc("inam"), fourcc("iart"), fourcc("icop"), fourcc("icmt"), fourcc("ignr"), fourcc("ipod"),
			fourcc("isof")
		};

		// The most a scan reads of a text chunk or an INFO item; a longer one is skipped.
		constexpr std::uint64_t max_text = 1u << 20;
		// The most it reads of a stream's format or data, where a codec keeps its configuration.
		constexpr std::uint64_t max_format = 1u << 16;
		// The start of a video stream's first chunk that is read for its codec's headers: first a little, then
		// as much as a frame's headers may take.
		constexpr std::uint64_t first_peek = 4u << 10;
		constexpr std::uint64_t max_peek = 64u << 10;
		// How many chunks into movi, and how far, the first chunks of such streams are looked for, and how
		// many of a stream's chunks are tried before its bitstream is given up on.
		constexpr std::size_t max_peek_chunks = 256;
		constexpr std::uint64_t max_peek_distance = 4u << 20;
		constexpr int max_peeks = 2;
		// AVI names a stream's chunks with two digits.
		constexpr std::size_t max_streams = 100;
		// How deep the lists before movi are read, and how many chunks of them in all.
		constexpr int max_depth = 8;
		constexpr std::size_t max_header_chunks = 10000;
		// The chunks a structure listing shows.
		constexpr std::size_t max_listed_chunks = 512;

		constexpr auto none = static_cast<std::size_t>(-1);

		// How much of a chunk's payload lies before end and inside the file.
		std::uint64_t available(reader& r, const chunk& c, const std::uint64_t end)
		{
			const auto stop = std::min(end, r.size());
			return c.payload() < stop ? std::min(c.size, stop - c.payload()) : 0;
		}

		std::vector<std::uint8_t> read_payload(reader& r, const chunk& c, const std::uint64_t end, const std::uint64_t limit)
		{
			return r.read(c.payload(), static_cast<std::size_t>(std::min(available(r, c, end), limit)));
		}

		// A chunk's text after its first skip bytes: UTF-8 when it is valid, else Windows-1252, to its first
		// NUL. Empty for one longer than max_text.
		std::string read_text(reader& r, const chunk& c, const std::uint64_t end, const std::uint64_t skip = 0)
		{
			if (c.size > max_text + skip) return {};
			const auto bytes = read_payload(r, c, end, max_text + skip);
			if (bytes.size() <= skip) return {};
			return text_from_unknown(std::span(bytes).subspan(static_cast<std::size_t>(skip)));
		}

		// A tag under the container's own name, for one no tag family maps; the caller files what it means.
		void keep_tag(std::vector<media_tag>& tags, const std::string_view key, const std::string_view value)
		{
			const auto text = trim_text(value);
			if (!text.empty()) tags.push_back({std::string(key), std::string(text), {}});
		}

		// Each item of a LIST INFO, as its id and text. A writer that leaves out the pad after an odd-sized item
		// puts the next a byte early, so an item that runs past the list after an odd-sized one is read from a
		// byte earlier, as FFmpeg reads it.
		template <typename Visit>
		void for_each_info_item(reader& r, const std::uint64_t pos, std::uint64_t end, Visit&& visit)
		{
			end = std::min(end, r.size());
			auto at = pos;
			auto odd = false;

			while (end > at && end - at >= 8)
			{
				std::uint64_t size = r.le32(at + 4);

				if (size > end - at - 8)
				{
					if (!odd || r.le32(at + 3) > end - at - 7) break;
					--at;
					size = r.le32(at + 4);
				}

				if (const auto id = r.le32(at); id != 0 && size <= max_text) visit(id, text_from_unknown(r.read(at + 8, static_cast<std::size_t>(size))));

				odd = (size & 1) != 0;
				at += 8 + size + (size & 1);
			}
		}

		// A LIST INFO's items, as tags.
		void read_info(reader& r, const std::uint64_t pos, const std::uint64_t end, media_scan_result& out)
		{
			for_each_info_item(r, pos, end, [&out](const std::uint32_t id, const std::string& text)
			{
				add_tag(out, tag_family::riff_info, fourcc_text(id), text);
			});
		}

		// The ID3v2 tag in an 'id3 ' or 'ID3 ' chunk. A damaged tag keeps what was read of it.
		void read_id3_chunk(reader& r, const chunk& c, media_budget& budget, media_scan_result& out)
		{
			try
			{
				read_id3v2(r, c.payload(), budget, out);
			}
			catch (const read_error&)
			{
			}
			catch (const format_error&)
			{
			}
		}

		// "LIST INFO", "RIFF AVI ", "idx1".
		std::string chunk_name(const chunk& c)
		{
			auto name = fourcc_text(c.id);
			if (c.type != 0) name += " " + fourcc_text(c.type);
			return name;
		}

		std::string_view chunk_description(const chunk& c)
		{
			if (c.id == list_id)
			{
				switch (c.type)
				{
				case hdrl_type: return "header list";
				case movi_type: return "movie data";
				case info_type: return "text tags";
				case odml_type: return "OpenDML header";
				case ncdt_type: return "Nikon camera tags";
				case adtl_type: return "cue labels";
				default: return {};
				}
			}

			switch (c.id)
			{
			case idx1_id: return "index";
			case junk_id: return "padding";
			case pmx_id: return "XMP packet";
			case idit_id: return "original date";
			case fmt_id: return "audio format";
			case data_id: return "audio data";
			case fact_id: return "sample count";
			case ds64_id: return "64-bit sizes";
			case bext_id: return "broadcast extension";
			case disp_id: return "display title";
			case ixml_id: return "iXML metadata";
			case id3_id:
			case id3_upper_id: return "ID3 tag";
			case cue_id: return "cue points";
			case smpl_id: return "sampler";
			default: return {};
			}
		}

		// Every form and the chunks at the top of each, as the walk read them.
		void add_chunk_rows(std::vector<structure_row>& rows, const layout& l)
		{
			add_structure_section(rows, "Chunks", "riff.chunks");
			std::size_t listed = 0;

			for (const auto& f : l.forms)
			{
				auto& form_row = add_row(rows, chunk_name(f.header), std::format("offset {}, {} bytes{}", f.header.start, f.header.size,
				                                                                 f.end != f.data_end() ? ", cut short" : ""));
				form_row.depth = 1;
				form_row.id = std::format("riff.form.{}", f.header.start);

				for (const auto& c : f.children)
				{
					if (++listed > max_listed_chunks)
					{
						add_row(rows, "...", "more chunks not listed").depth = 2;
						return;
					}

					auto value = std::format("offset {}, {} bytes", c.start, c.size);
					if (const auto description = chunk_description(c); !description.empty()) value = std::format("{}, {}", description, value);
					if (!fits(c, f.end)) value += ", cut short";

					auto& row = add_row(rows, chunk_name(c), std::move(value));
					row.depth = 2;
					row.id = std::format("riff.chunk.{}", c.start);
				}
			}
		}

		struct avi_stream
		{
			media_stream stream;
			// The stream's place among the stream headers, by which movi names its chunks.
			std::size_t number = 0;
			std::uint32_t type = 0;
			std::uint32_t handler = 0;
			std::uint32_t flags = 0;
			std::uint32_t scale = 0;
			std::uint32_t rate = 0;
			std::uint32_t start = 0;
			std::uint32_t length = 0;
			std::uint32_t sample_size = 0;
			// rcFrame: left, top, right and bottom.
			std::int16_t frame[4] = {};
			bool has_format = false;
			std::uint32_t avg_bytes_per_second = 0;
			// A video stream's BITMAPINFOHEADER and what its codec keeps after it, and the stream's data.
			std::vector<std::uint8_t> format;
			std::vector<std::uint8_t> data;
			std::optional<video_header> header;
			// How many more of its chunks are read for the codec's headers.
			int peeks = 0;
		};

		struct avi_main_header
		{
			bool present = false;
			std::uint32_t frame_period = 0;
			std::uint32_t flags = 0;
			std::uint32_t total_frames = 0;
			std::uint32_t streams = 0;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
		};

		// The codecs whose bitstream alone describes the picture, so a scan reads their configuration or the
		// start of their first chunk.
		bool described_by_bitstream(const std::string_view codec)
		{
			return codec == "h264" || codec == "hevc" || codec == "mjpeg" || codec == "dv" || codec == "mpeg1" || codec == "mpeg2" ||
				codec == "mpeg4" || codec == "vp9" || codec == "av1";
		}

		// The first sequence parameter set among NAL units with start codes.
		bool parse_annexb_sps(const std::span<const std::uint8_t> data, const bool hevc, video_header& h)
		{
			auto found = false;

			for_each_annexb_nal(data, [&](const std::span<const std::uint8_t> nal)
			{
				if (nal.empty()) return true;
				const auto type = hevc ? nal[0] >> 1 & 0x3F : nal[0] & 0x1F;
				if (type != (hevc ? 33 : 7)) return true;
				found = hevc ? parse_hevc_sps(nal, h) : parse_h264_sps(nal, h);
				return !found;
			});

			return found;
		}

		class avi_scan
		{
		public:
			avi_scan(reader& r, media_scan_result& out) : _r(r), _out(out)
			{
			}

			bool has_movie_data() const { return _movie_data; }

			void continued() { _open_dml = true; }

			// A chunk before movi, at any depth. FFmpeg reads the header flat, so a stream's chunks belong to
			// the last stream header before them, wherever they lie.
			void header_chunk(const chunk& c, const std::uint64_t end, const int depth)
			{
				if (++_header_chunks > max_header_chunks) return;

				if (c.id == list_id)
				{
					if (c.size < 4) return;
					const auto list_end = c.payload() + available(_r, c, end);

					switch (c.type)
					{
					case info_type: early_info(c.payload() + 4, list_end);
						return;
					case ncdt_type: nikon_tags(c.payload() + 4, list_end);
						return;
					case hdrl_type: _header_list = true;
						break;
					case odml_type: _open_dml = true;
						break;
					default: break;
					}

					if (depth >= max_depth) return;

					for_each_chunk(_r, c.payload() + 4, list_end, nullptr, [&](const chunk& inner)
					{
						header_chunk(inner, list_end, depth + 1);
						return _header_chunks <= max_header_chunks;
					});

					return;
				}

				switch (c.id)
				{
				case avih_id: main_header(c, end);
					break;
				case strh_id: stream_header(c, end);
					break;
				case strf_id: stream_format(c, end);
					break;
				case strd_id:
					if (_current != none && _streams[_current].data.empty()) _streams[_current].data = read_payload(_r, c, end, max_format);
					break;
				case strn_id:
					if (_current != none) _streams[_current].stream.title = trim_text(read_text(_r, c, end));
					break;
				case vprp_id: video_properties(c, end);
					break;
				case dmlh_id:
					_open_dml = true;
					if (available(_r, c, end) >= 4) _dml_frames = _r.le32(c.payload());
					break;
				case idit_id: original_date(c, end);
					break;
				default:
					if (_current != none && std::ranges::find(stream_tag_ids, c.id) != std::end(stream_tag_ids))
					{
						keep_tag(_streams[_current].stream.tags, fourcc_text(c.id), read_text(_r, c, end));
					}
					break;
				}
			}

			// Where the movie data lies; and the codec headers of the video streams that need their bitstream,
			// read now, while the walk is at the start of movi.
			void movie_data(const chunk& c, const std::uint64_t end)
			{
				_movie_data = true;
				_movi_start = c.start + 12;
				_movi_end = c.size >= 4 && fits(c, end) ? c.payload() + c.size : end;
				codec_headers();
			}

			// An INFO list after the movie data, at the top of the first form. Its fields wait for the walk to end,
			// since a list before the movie data gives the fields it shares with it.
			void late_info(const chunk& c, const std::uint64_t end)
			{
				if (c.size < 4) return;

				for_each_info_item(_r, c.payload() + 4, c.payload() + available(_r, c, end), [this](const std::uint32_t id, std::string text)
				{
					_late_info.emplace_back(id, std::move(text));
				});
			}

			// The result's streams, duration and tags from what the walk read. stopped is true when the walk
			// ended at movie data that gives no size.
			void finish(const layout& l, const bool stopped, const scan_intent intent)
			{
				if (l.forms.empty()) throw format_error(l.fault ? l.fault : "the file is not a RIFF file");
				if (!_header_list) throw format_error("the AVI has no header list");

				// Where an INFO list before the movie data and one after it give the same field, the one before
				// wins: it is the one the writer edits, and an edit must show.
				for (const auto& [id, text] : _late_info)
				{
					if (std::ranges::find(_early_info, id) == _early_info.end()) add_tag(_out, tag_family::riff_info, fourcc_text(id), text);
				}

				// A file cut short plays for the part of it that is there, as FFmpeg reckons it.
				const auto declared_end = l.first().data_end();
				const auto present = declared_end > _r.size() ? static_cast<double>(_r.size()) / static_cast<double>(declared_end) : 1.0;

				std::uint64_t id = 0;
				auto first_video = true;
				auto longest_video = 0.0;
				auto longest_audio = 0.0;

				for (auto& s : _streams)
				{
					if (s.type == pads_type) continue;

					auto& m = s.stream;
					m.id = id++;
					auto frames = static_cast<double>(s.length);

					if (m.kind == media_stream_kind::video)
					{
						auto& v = m.video;

						// OpenDML counts the frames of every form, where an older writer's stream header counts
						// only the first form's.
						if (first_video && _dml_frames > s.length) frames = _dml_frames;
						first_video = false;

						if (v.width == 0 || v.height == 0)
						{
							if (s.frame[2] > s.frame[0] && s.frame[3] > s.frame[1])
							{
								v.width = static_cast<std::uint32_t>(s.frame[2] - s.frame[0]);
								v.height = static_cast<std::uint32_t>(s.frame[3] - s.frame[1]);
							}
							else if (_main.width != 0 && _main.height != 0)
							{
								v.width = _main.width;
								v.height = _main.height;
							}
						}

						if (s.scale != 0 && s.rate != 0) v.frame_rate = static_cast<double>(s.rate) / s.scale;
						else if (_main.frame_period != 0) v.frame_rate = 1e6 / _main.frame_period;

						if (s.header) apply_video_header(*s.header, v);
						else if (video_header h; fixed_video_format(m.codec, h)) apply_video_header(h, v);
					}
					else if (m.kind == media_stream_kind::audio && m.bit_rate == 0)
					{
						m.bit_rate = std::uint64_t{s.avg_bytes_per_second} * 8;
					}

					if (s.scale != 0 && s.rate != 0) m.duration = frames * s.scale / s.rate * present;
					else if (m.kind == media_stream_kind::video && _main.frame_period != 0) m.duration = frames * _main.frame_period / 1e6 * present;

					if (m.kind == media_stream_kind::video) longest_video = std::max(longest_video, m.duration);
					if (m.kind == media_stream_kind::audio) longest_audio = std::max(longest_audio, m.duration);

					camera_exif(s);
					_out.streams.push_back(std::move(m));
				}

				if (_out.streams.empty()) throw format_error("the AVI's header describes no streams");

				_out.brand = _open_dml ? "AVI OpenDML" : "AVI";
				_out.duration = longest_video > 0 ? longest_video : longest_audio;

				if (_created.valid()) _out.properties.created = _created;
				else if (_exif_created.valid()) _out.properties.created = _exif_created;

				// The packet read_packet reads: none from a file the writer would refuse, nor from one whose
				// movie data gives no size, where read_packet would walk the frames as chunks.
				if (!stopped && !l.fault) _out.xmp = read_current_packet(_r, l);

				if (intent == scan_intent::inspect) structure(l, stopped);
			}

		private:
			void structure(const layout& l, const bool stopped)
			{
				auto& rows = _out.structure;
				add_structure_section(rows, "Summary", "avi.summary", true);
				add_structure_row(rows, "File size", std::format("{} bytes", _r.size()));
				add_structure_row(rows, "Forms", std::to_string(l.forms.size()));

				if (_main.present)
				{
					add_structure_row(rows, "Frame period", std::format("{} microseconds", _main.frame_period));
					add_structure_row(rows, "Total frames", std::to_string(_main.total_frames));
					add_structure_row(rows, "Streams", std::to_string(_main.streams));
					add_structure_row(rows, "Frame size", std::format("{} x {}", _main.width, _main.height));
					add_structure_row(rows, "Flags", std::format("0x{:08X}", _main.flags));
				}

				if (_dml_frames != 0) add_structure_row(rows, "OpenDML total frames", std::to_string(_dml_frames));

				if (_movie_data)
				{
					add_structure_row(rows, "Movie data", std::format("offset {}, {} bytes{}", _movi_start, _movi_end - _movi_start,
					                                                  stopped ? ", its size unstated" : ""));
				}

				add_structure_section(rows, "Streams", "avi.streams");

				for (const auto& s : _streams)
				{
					add_structure_row(rows, std::format("Stream {}", s.number),
					                  std::format("'{}', handler '{}', scale {}, rate {}, start {}, length {}, sample size {}", fourcc_text(s.type),
					                              fourcc_text(s.handler), s.scale, s.rate, s.start, s.length, s.sample_size));
				}

				add_chunk_rows(rows, l);
				finish_structure_sections(rows);
			}

			void main_header(const chunk& c, const std::uint64_t end)
			{
				const auto h = read_payload(_r, c, end, 40);
				if (h.size() < 40) return;

				_main.present = true;
				_main.frame_period = load_le32(h.data());
				_main.flags = load_le32(h.data() + 12);
				_main.total_frames = load_le32(h.data() + 16);
				_main.streams = load_le32(h.data() + 24);
				_main.width = load_le32(h.data() + 32);
				_main.height = load_le32(h.data() + 36);
			}

			// An INFO list before the movie data, at any depth: its items are tags now, and the fields it gives
			// are kept from any list after the movie data.
			void early_info(const std::uint64_t pos, const std::uint64_t end)
			{
				for_each_info_item(_r, pos, end, [this](const std::uint32_t id, const std::string& text)
				{
					if (!trim_text(text).empty()) _early_info.push_back(id);
					add_tag(_out, tag_family::riff_info, fourcc_text(id), text);
				});
			}

			void stream_header(const chunk& c, const std::uint64_t end)
			{
				_current = none;
				if (_streams.size() >= max_streams) return;

				const auto bytes = read_payload(_r, c, end, 56);
				if (bytes.size() < 8) return;

				std::uint8_t h[56] = {};
				std::ranges::copy(bytes, h);

				auto& s = _streams.emplace_back();
				s.number = _streams.size() - 1;
				s.type = load_le32(h);
				s.handler = load_le32(h + 4);
				s.flags = load_le32(h + 8);
				s.scale = load_le32(h + 20);
				s.rate = load_le32(h + 24);
				s.start = load_le32(h + 28);
				s.length = load_le32(h + 32);
				s.sample_size = load_le32(h + 44);

				if (bytes.size() >= 56)
				{
					for (std::size_t i = 0; i < 4; ++i) s.frame[i] = static_cast<std::int16_t>(load_le16(h + 48 + 2 * i));
				}

				switch (s.type)
				{
				case vids_type: s.stream.kind = media_stream_kind::video;
					break;
				case auds_type: s.stream.kind = media_stream_kind::audio;
					break;
				case txts_type: s.stream.kind = media_stream_kind::subtitle;
					break;
				case iavs_type:
				case ivas_type:
					// DV interleaved with its own audio, described by the DV frames alone.
					s.stream.kind = media_stream_kind::video;
					s.stream.codec = "dv";
					s.stream.codec_tag = fourcc_text(s.handler);
					break;
				default: s.stream.kind = media_stream_kind::data;
					break;
				}

				_current = _streams.size() - 1;
			}

			void stream_format(const chunk& c, const std::uint64_t end)
			{
				if (_current == none) return;
				auto& s = _streams[_current];
				if (s.has_format || c.size == 0) return;

				auto bytes = read_payload(_r, c, end, max_format);
				s.has_format = true;

				if (s.type == vids_type)
				{
					read_bitmap_info(bytes, s.stream);

					// DivX subtitles are pictures, in a video stream header.
					if (s.stream.codec_tag == "DXSB" || s.stream.codec_tag == "DXSA") s.stream.kind = media_stream_kind::subtitle;
					s.format = std::move(bytes);
				}
				else if (s.type == auds_type)
				{
					read_wave_format(bytes, s.stream, &s.avg_bytes_per_second);
				}
			}

			// OpenDML's video properties: the frame's shape, from which the pixel's follows, and how many
			// fields make a frame.
			void video_properties(const chunk& c, const std::uint64_t end)
			{
				if (_current == none || _streams[_current].stream.kind != media_stream_kind::video) return;

				const auto p = read_payload(_r, c, end, 36);
				if (p.size() < 36) return;

				auto& v = _streams[_current].stream.video;
				// dwFrameAspectRatio keeps the frame's x in its high word and y in its low.
				const std::uint64_t y = load_le16(p.data() + 20);
				const std::uint64_t x = load_le16(p.data() + 22);
				const std::uint64_t width = load_le32(p.data() + 24);
				const std::uint64_t height = load_le32(p.data() + 28);

				if (x != 0 && y != 0 && width != 0 && height != 0)
				{
					auto n = x * height;
					auto d = y * width;
					const auto g = std::gcd(n, d);
					n /= g;
					d /= g;

					if (n <= 0xFFFFFFFF && d <= 0xFFFFFFFF)
					{
						v.pixel_aspect_width = static_cast<std::uint32_t>(n);
						v.pixel_aspect_height = static_cast<std::uint32_t>(d);
					}
				}

				if (load_le32(p.data() + 32) == 2) v.interlaced = true;
			}

			// IDIT: when the recording was made, as the camera's clock showed it - "Sun Mar 13 15:13:49 2011",
			// or as some write it, "2011/03/13 15:13:49".
			void original_date(const chunk& c, const std::uint64_t end)
			{
				auto text = read_text(_r, c, end);
				keep_tag(_out.tags, "IDIT", text);

				if (text.size() > 7 && text[4] == '/' && text[7] == '/') text[4] = text[7] = '-';
				if (const auto date = parse_media_date(trim_text(text)); date.valid()) _created = date;
			}

			// Nikon's ncdt list: the make, model and date in its nctg chunk, the entries FFmpeg reads.
			void nikon_tags(const std::uint64_t pos, const std::uint64_t end)
			{
				for_each_chunk(_r, pos, end, nullptr, [&](const chunk& c)
				{
					if (c.id != nctg_id) return true;

					const auto stop = c.payload() + available(_r, c, end);
					auto at = c.payload();

					while (stop - at >= 4)
					{
						const auto tag = _r.le16(at);
						const auto size = std::min<std::uint64_t>(_r.le16(at + 2), stop - at - 4);
						at += 4;

						const auto key = tag == 0x03 ? "Make" : tag == 0x04 ? "Model" : tag == 0x13 ? "DateTimeOriginal" : "";

						if (*key)
						{
							const auto text = text_from_unknown(_r.read(at, static_cast<std::size_t>(size)));
							const auto value = trim_text(text);
							keep_tag(_out.tags, key, value);

							if (tag == 0x03 && !value.empty()) _out.properties.camera_make = value;
							if (tag == 0x04 && !value.empty()) _out.properties.camera_model = value;

							if (tag == 0x13)
							{
								if (const auto date = parse_media_date(value); date.valid()) _created = date;
							}
						}

						at += size;
					}

					return true;
				});
			}

			// The EXIF directory some cameras keep in a stream's strd chunk, after "AVIF" and four bytes: a
			// little-endian IFD whose offsets count from its own start. FFmpeg makes its entries the stream's
			// tags.
			void camera_exif(avi_stream& s)
			{
				if (s.data.size() < 10 || load_le32(s.data.data()) != avif_id) return;

				reader ifd(std::span<const std::uint8_t>(s.data).subspan(8));
				const tiff_bytes exif(ifd, true);
				std::uint32_t exif_directory = 0;
				std::string make, model, modified, original;

				const auto text = [](const entry& e)
				{
					const auto raw = e.ascii();
					return std::string(trim_text(text_from_unknown({reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()})));
				};

				for_each_entry(exif, 0, directory_kind::exif, [&](const entry& e)
				{
					if (e.tag == 0x8769 && (e.type == type_long || e.type == type_short)) exif_directory = e.type == type_long ? e.field_u32() : e.field_u16();
					if (e.type != type_ascii) return;
					if (e.tag == 0x010F) make = text(e);
					if (e.tag == 0x0110) model = text(e);
					if (e.tag == 0x0132) modified = text(e);
				});

				if (exif_directory != 0)
				{
					for_each_entry(exif, exif_directory, directory_kind::exif, [&](const entry& e)
					{
						if (e.type == type_ascii && e.tag == 0x9003) original = text(e);
					});
				}

				keep_tag(s.stream.tags, "Make", make);
				keep_tag(s.stream.tags, "Model", model);
				keep_tag(s.stream.tags, "DateTime", modified);
				keep_tag(s.stream.tags, "DateTimeOriginal", original);

				if (_out.properties.camera_make.empty()) _out.properties.camera_make = make;
				if (_out.properties.camera_model.empty()) _out.properties.camera_model = model;
				if (const auto date = parse_media_date(original.empty() ? modified : original); date.valid() && !_exif_created.valid()) _exif_created = date;
			}

			// The codec headers of each video stream whose bitstream alone describes its picture: from the
			// configuration its format or data keeps, else from the start of its first chunks in movi.
			void codec_headers()
			{
				std::size_t wanted = 0;

				for (auto& s : _streams)
				{
					if (s.stream.kind != media_stream_kind::video || !described_by_bitstream(s.stream.codec) || configuration(s)) continue;
					s.peeks = max_peeks;
					++wanted;
				}

				if (wanted == 0) return;

				std::size_t visited = 0;
				const auto limit = _movi_end - _movi_start > max_peek_distance ? _movi_start + max_peek_distance : _movi_end;
				first_chunks(_movi_start, limit, 0, visited, wanted);
			}

			// The codec's configuration, where FFmpeg takes its extradata from: after the BITMAPINFOHEADER,
			// else the stream's data, which some H.264 writers fill with something else.
			bool configuration(avi_stream& s)
			{
				std::span<const std::uint8_t> config;

				if (s.format.size() > 40)
				{
					// What follows the BITMAPINFOHEADER's 40 bytes, to where its size says it ends, when it says.
					const auto stated = std::size_t{load_le32(s.format.data())};
					const auto config_end = stated > 40 && stated < s.format.size() ? stated : s.format.size();
					config = std::span<const std::uint8_t>(s.format).subspan(40, config_end - 40);
				}

				if (config.empty() && s.stream.codec_tag != "H264") config = s.data;
				if (config.empty()) return false;

				const auto& codec = s.stream.codec;
				video_header h;
				auto ok = false;

				if (codec == "h264") ok = config[0] == 1 ? parse_avc_config(config, h) : parse_annexb_sps(config, false, h);
				else if (codec == "hevc") ok = config[0] == 1 ? parse_hevc_config(config, h) : parse_annexb_sps(config, true, h);
				else if (codec == "mpeg4") ok = parse_mpeg4_video(config, h);
				else if (codec == "mpeg1" || codec == "mpeg2") ok = parse_mpeg12_video(config, h);
				else if (codec == "av1") ok = config[0] & 0x80 ? parse_av1_config(config, h) : parse_av1_sequence(config, h);

				if (ok) s.header = h;
				return ok;
			}

			// The stream a movi chunk belongs to, from the two digits its id starts with; none for a chunk of
			// no stream, or a palette change.
			static std::size_t stream_number(const std::uint32_t id)
			{
				const auto d0 = static_cast<int>(id & 0xFF) - '0';
				const auto d1 = static_cast<int>(id >> 8 & 0xFF) - '0';
				if (d0 < 0 || d0 > 9 || d1 < 0 || d1 > 9 || id >> 16 == fourcc("00pc") >> 16) return none;
				return static_cast<std::size_t>(d0 * 10 + d1);
			}

			// Walks movi from its start - into the 'rec ' lists that group chunks - reading the chunks of the
			// streams that want their bitstream. False ends the walk.
			bool first_chunks(const std::uint64_t pos, const std::uint64_t limit, const int depth, std::size_t& visited, std::size_t& wanted)
			{
				auto go_on = true;

				for_each_chunk(_r, pos, limit, nullptr, [&](const chunk& c)
				{
					if (++visited > max_peek_chunks) return go_on = false;

					if (c.id == list_id)
					{
						if (c.type != rec_type || depth >= 2) return true;
						const auto inner = std::min(limit, c.payload() + available(_r, c, _movi_end));
						return go_on = first_chunks(c.payload() + 4, inner, depth + 1, visited, wanted);
					}

					const auto number = stream_number(c.id);
					if (number >= _streams.size() || c.size == 0) return true;

					auto& s = _streams[number];
					if (s.peeks == 0) return true;

					s.peeks = bitstream(s, c) ? 0 : s.peeks - 1;
					if (s.peeks == 0 && --wanted == 0) return go_on = false;
					return true;
				});

				return go_on;
			}

			// The codec's headers at the start of one of the stream's chunks: in its first bytes for most codecs,
			// which the walk has usually read already, else further in.
			bool bitstream(avi_stream& s, const chunk& c)
			{
				const auto whole = std::min(available(_r, c, _movi_end), max_peek);
				const auto first = std::min(whole, first_peek);

				if (first == 0) return false;
				if (headers(s, _r.view(c.payload(), static_cast<std::size_t>(first)))) return true;
				return whole > first && headers(s, _r.view(c.payload(), static_cast<std::size_t>(whole)));
			}

			bool headers(avi_stream& s, const std::span<const std::uint8_t> data)
			{
				const auto& codec = s.stream.codec;
				auto& v = s.stream.video;
				video_header h;
				auto ok = false;

				if (codec == "h264" || codec == "hevc") ok = parse_annexb_sps(data, codec == "hevc", h);
				else if (codec == "mjpeg") ok = parse_jpeg_frame(data, h);
				else if (codec == "dv") ok = parse_dv_frame(data, h);
				else if (codec == "mpeg1" || codec == "mpeg2") ok = parse_mpeg12_video(data, h);
				else if (codec == "mpeg4") ok = parse_mpeg4_video(data, h);
				else if (codec == "vp9") ok = parse_vp9_frame(data, h);
				else if (codec == "av1") ok = parse_av1_sequence(data, h);

				if (!ok) return false;

				// Interlaced Motion JPEG keeps a frame's fields as two pictures, each half the frame's height.
				if (codec == "mjpeg" && h.height != 0 && v.height >= 2 * h.height - 1 && v.height <= 2 * h.height + 1)
				{
					h.width = 0;
					h.height = 0;
					h.interlaced = true;
				}

				s.header = h;
				return true;
			}

			reader& _r;
			media_scan_result& _out;
			avi_main_header _main;
			std::vector<avi_stream> _streams;
			std::size_t _current = none;
			std::size_t _header_chunks = 0;
			bool _header_list = false;
			bool _open_dml = false;
			std::uint32_t _dml_frames = 0;
			bool _movie_data = false;
			std::uint64_t _movi_start = 0;
			std::uint64_t _movi_end = 0;
			// The INFO fields given before the movie data, and those found after it, waiting.
			std::vector<std::uint32_t> _early_info;
			std::vector<std::pair<std::uint32_t, std::string>> _late_info;
			media_date _created;
			media_date _exif_created;
		};

		// The number in text[at, at + count), or -1 when those are not all digits.
		int digits(const std::string_view text, const std::size_t at, const std::size_t count)
		{
			if (at + count > text.size()) return -1;
			auto value = 0;

			for (auto i = at; i < at + count; ++i)
			{
				if (text[i] < '0' || text[i] > '9') return -1;
				value = value * 10 + (text[i] - '0');
			}

			return value;
		}

		// A Broadcast Wave bext chunk (EBU Tech 3285): its text as tags under the specification's names; the
		// description filed as a comment and the originator as who encoded it - where FFmpeg files them, as it
		// files INFO's ICMT and ITCH - and the origination date and time, "yyyy-mm-dd" and "hh:mm:ss" with
		// any separators, as when the recording was made, in a zone it does not name.
		void read_bext(reader& r, const chunk& c, const std::uint64_t end, media_scan_result& out, media_date& created)
		{
			const auto bytes = read_payload(r, c, end, 602 + max_text);
			if (bytes.size() < 338) return;

			const std::span<const std::uint8_t> b(bytes);
			const auto field = [b](const std::size_t at, const std::size_t length) { return text_from_unknown(b.subspan(at, length)); };

			const auto file_as = [&out](const std::string_view key, const std::string& value, const std::string_view info_key)
			{
				keep_tag(out.tags, key, value);
				if (const auto text = trim_text(value); !text.empty()) apply_tag(out.properties, tag_family::riff_info, info_key, text);
			};

			file_as("Description", field(0, 256), "ICMT");
			file_as("Originator", field(256, 32), "ITCH");
			keep_tag(out.tags, "OriginatorReference", field(288, 32));

			const auto date = field(320, 10);
			const auto time = field(330, 8);
			keep_tag(out.tags, "OriginationDate", date);
			keep_tag(out.tags, "OriginationTime", time);

			media_date d;
			d.year = digits(date, 0, 4);
			d.month = digits(date, 5, 2);
			d.day = digits(date, 8, 2);

			if (d.year >= 1 && d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31)
			{
				const auto hour = digits(time, 0, 2);
				const auto minute = digits(time, 3, 2);
				const auto second = digits(time, 6, 2);

				if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59)
				{
					d.has_time = true;
					d.hour = hour;
					d.minute = minute;
					d.second = second;
				}

				created = d;
			}

			if (bytes.size() > 602) keep_tag(out.tags, "CodingHistory", field(602, bytes.size() - 602));
		}
	}

	void scan_avi_media(reader& r, const scan_intent intent, media_scan_result& out)
	{
		avi_scan avi(r, out);
		auto stopped = false;

		const auto l = walk_layout(r, [&](const std::size_t form, const chunk& c, const std::uint64_t end)
		{
			if (form > 0)
			{
				avi.continued();
				return true;
			}

			// After the movie data only the first form's INFO lists are read, and the packet once the walk is done.
			if (avi.has_movie_data())
			{
				if (c.id == list_id && c.type == info_type) avi.late_info(c, end);
				return true;
			}

			if (c.id == list_id && (c.type == movi_type || (c.size < 4 && end - c.start >= 12 && r.le32(c.start + 8) == movi_type)))
			{
				avi.movie_data(c, end);

				// Movie data that gives no size runs to the end, and what follows its header is media, not chunks.
				if (c.size < 4)
				{
					stopped = true;
					return false;
				}

				return true;
			}

			avi.header_chunk(c, end, 0);
			return true;
		});

		avi.finish(l, stopped, intent);
	}

	void scan_wav_media(reader& r, const scan_intent intent, media_budget& budget, media_scan_result& out)
	{
		media_stream stream;
		stream.kind = media_stream_kind::audio;
		auto format = false;
		auto format_read = false;
		auto data = false;
		auto stopped = false;
		std::uint32_t avg_bytes_per_second = 0;
		std::uint16_t block_align = 0;
		std::optional<std::uint32_t> fact;
		std::uint64_t data_bytes = 0;
		std::string display_title;
		media_date created;

		const auto l = walk_layout(r, [&](std::size_t, const chunk& c, const std::uint64_t end)
		{
			switch (c.id)
			{
			case fmt_id:
				// FFmpeg reads the first.
				if (!format)
				{
					format = true;
					format_read = read_wave_format(read_payload(r, c, end, max_format), stream, &avg_bytes_per_second, &block_align);
				}
				break;
			case data_id:
				if (!data)
				{
					data = true;
					data_bytes = available(r, c, end);

					// Data that gives no size runs to the end of the file, as FFmpeg reads it, and what follows its
					// header is audio, not chunks.
					if (c.size == 0)
					{
						data_bytes = std::min(end, r.size()) - c.payload();
						stopped = true;
						return false;
					}
				}
				break;
			case fact_id:
				if (!fact && available(r, c, end) >= 4) fact = r.le32(c.payload());
				break;
			case list_id:
				if (c.type == info_type) read_info(r, c.payload() + 4, c.payload() + available(r, c, end), out);
				break;
			case list_lower_id:
				// Some writers name the list in lower case, which FFmpeg reads too.
				if (available(r, c, end) >= 4 && r.le32(c.payload()) == info_type) read_info(r, c.payload() + 4, c.payload() + available(r, c, end), out);
				break;
			case id3_id:
			case id3_upper_id: read_id3_chunk(r, c, budget, out);
				break;
			case bext_id: read_bext(r, c, end, out, created);
				break;
			case disp_id:
				// The writer's: CF_TEXT, a title, the last of which it edits.
				if (available(r, c, end) >= 4 && r.le32(c.payload()) == 1)
				{
					display_title = trim_text(read_text(r, c, end, 4));
					keep_tag(out.tags, "DISP", display_title);
				}
				break;
			case ixml_id: keep_tag(out.tags, "iXML", read_text(r, c, end));
				break;
			default: break;
			}

			return true;
		});

		if (l.forms.empty()) throw format_error(l.fault ? l.fault : "the file is not a RIFF file");
		if (!format) throw format_error("the WAV has no format chunk");
		if (!format_read) throw format_error("the WAV's format chunk is cut short");

		const auto& a = stream.audio;
		const auto& codec = stream.codec;

		// Samples of one size, whose count the data's size gives: PCM, and G.711's A-law and mu-law. Its rate in
		// bytes is the block's size at the sample rate, when the block holds what the samples say it should.
		const auto constant = !codec.empty() && (codec == "pcm" || codec == codec_from_wave_tag(0x0006) || codec == codec_from_wave_tag(0x0007));
		std::uint64_t bytes_per_second = avg_bytes_per_second;

		if (constant && a.sample_rate != 0 && a.bits_per_sample != 0 && block_align != 0 && block_align == a.channels * ((a.bits_per_sample + 7u) / 8u))
		{
			bytes_per_second = std::uint64_t{block_align} * a.sample_rate;
		}

		// A compressed format's count of samples, from the ds64 chunk, else the fact chunk.
		std::uint64_t samples = 0;
		if (l.sizes && l.sizes->samples != 0) samples = l.sizes->samples;
		else if (fact && *fact != size_in_ds64) samples = *fact;

		if (!constant && samples != 0 && a.sample_rate != 0) stream.duration = static_cast<double>(samples) / a.sample_rate;
		else if (bytes_per_second != 0) stream.duration = static_cast<double>(data_bytes) / static_cast<double>(bytes_per_second);

		if (stream.bit_rate == 0) stream.bit_rate = bytes_per_second * 8;
		stream.id = 0;
		out.duration = stream.duration;
		out.streams.push_back(std::move(stream));

		const auto form_id = l.first().header.id;
		out.brand = form_id == rf64_id ? "RF64" : form_id == bw64_id ? "BW64" : "WAVE";

		// The toolkit reads DISP's title over INFO's.
		if (!display_title.empty()) out.properties.title = display_title;
		if (created.valid()) out.properties.created = created;

		// The packet read_packet reads, which reads a RIFF form only, and none from a file the writer would
		// refuse, nor from one whose data gives no size; a packet in an ID3 tag is not the file's.
		out.xmp.clear();
		if (form_id == riff_id && !stopped && !l.fault) out.xmp = read_current_packet(r, l);

		if (intent == scan_intent::inspect)
		{
			auto& rows = out.structure;
			const auto& f = l.first();
			const auto& s = out.streams.front();

			add_structure_section(rows, "Summary", "wav.summary", true);
			add_structure_row(rows, "File size", std::format("{} bytes", r.size()));
			add_structure_row(rows, "Form", std::format("{}, {} bytes", fourcc_text(f.header.id), f.header.size));

			if (l.sizes)
			{
				add_structure_row(rows, "64-bit sizes", std::format("RIFF {} bytes, data {} bytes, {} samples", l.sizes->riff, l.sizes->data,
				                                                    l.sizes->samples));
			}

			add_structure_row(rows, "Format tag", s.codec_tag);
			add_structure_row(rows, "Channels", std::to_string(s.audio.channels));
			add_structure_row(rows, "Sample rate", std::format("{} Hz", s.audio.sample_rate));
			add_structure_row(rows, "Average bytes per second", std::to_string(avg_bytes_per_second));
			add_structure_row(rows, "Block align", std::to_string(block_align));
			if (s.audio.bits_per_sample != 0) add_structure_row(rows, "Bits per sample", std::to_string(s.audio.bits_per_sample));
			if (fact) add_structure_row(rows, "Samples, as fact counts them", std::to_string(*fact));
			add_structure_row(rows, "Audio data", std::format("{} bytes{}", data_bytes, stopped ? ", its size unstated" : ""));
			add_chunk_rows(rows, l);
			finish_structure_sections(rows);
		}
	}
}
