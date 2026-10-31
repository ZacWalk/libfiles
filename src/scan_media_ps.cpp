// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning MPEG program streams: MPG, VOB, MOD and TOD. Packs hold PES packets whose stream
// ids - and in private stream 1 their substream ids - name the streams as FFmpeg's mpeg demuxer
// numbers them; each stream's own header is read from its first packets, and the duration from the
// timestamps at the start and at the end. No table need list the streams, so the start is read until
// those found are known and a second of the stream has passed for any that begin later.

#include "media_mpeg_internal.h"

#include <algorithm>
#include <array>
#include <format>

namespace libfiles::detail
{
	namespace
	{
		// How long the stream's clock runs, once the streams found so far are known, for one that begins
		// later - a second audio track, subtitles - to show itself; and how far the scan reads for it
		// when the packs keep no clock.
		constexpr std::uint64_t discovery_ticks = mpeg_system_ticks_per_second;
		constexpr std::uint64_t discovery_bytes = 2u * 1024u * 1024u;
		// How far a resynchronisation looks past damage for the next start code, and the pieces it reads
		// at a time.
		constexpr std::uint64_t resync_bytes = 64u * 1024u;
		constexpr std::uint64_t resync_piece = 4096;
		// What of a PES packet is read for its header: the longest MPEG-2 header, and private stream 1's
		// substream header after it.
		constexpr std::size_t header_bytes = 9 + 255 + 5;
		constexpr std::size_t max_streams = 128;

		struct ps_stream
		{
			// FFmpeg's AVStream::id for it.
			std::uint32_t id = 0;
			std::uint8_t stream_id = 0;
			std::size_t index = 0;
			// The bytes before the elementary stream in each payload: private stream 1's substream id and
			// the header that follows it.
			std::size_t skip = 0;
			es_probe probe;
			es_clock clock;
			bool timed = false;
			bool ended = false;
		};

		// Where a PES packet's payload goes.
		struct ps_route
		{
			std::uint32_t id = 0;
			es_type type;
			std::size_t skip = 0;
		};

		struct map_entry
		{
			std::uint8_t stream_id = 0;
			std::uint8_t stream_type = 0;
			std::string language;
		};

		class ps_scan
		{
		public:
			ps_scan(reader& r, const scan_intent intent, media_scan_result& out) :
				_r(r),
				_intent(intent),
				_out(out),
				_size(r.size())
			{
			}

			void run()
			{
				const auto head_end = walk(0, std::min(_size, mpeg_head_bytes), true);
				if (_packs == 0) throw format_error("the program stream has no pack header that can be read");
				if (!_any_pes) throw format_error("the program stream holds no PES packet");

				_out.brand = _mpeg1 ? "MPEG-1 system" : "MPEG-PS";

				for (auto& s : _streams) s.probe.finish(_out.streams[s.index], _out.properties);

				// The tail, read backwards in growing steps until every stream's last timestamp is found.
				auto covered = _size;

				for (const auto step : mpeg_tail_steps)
				{
					if (head_end >= covered || tail_complete()) break;

					const auto from = std::max(head_end, _size > step ? _size - step : 0);
					if (from >= covered) continue;

					walk(resync(from, covered), covered, false);
					covered = from;
				}

				finish_timing();
				if (_intent == scan_intent::inspect) structure(head_end, covered);
			}

		private:
			std::uint64_t walk(std::uint64_t at, const std::uint64_t end, const bool head)
			{
				while (at < end && _r.contains(at, 4))
				{
					if (head && settled(at)) break;

					const auto code = _r.be32(at);

					if ((code >> 8) != 1)
					{
						at = resync(at + 1, end);
						continue;
					}

					const auto id = static_cast<std::uint8_t>(code);

					if (id == 0xBA)
					{
						const auto length = read_pack(at, head);
						at = length != 0 ? at + length : resync(at + 1, end);
						continue;
					}

					// The program end code: FFmpeg reads on past it, and so does this.
					if (id == 0xB9)
					{
						at += 4;
						continue;
					}

					if (id < 0xBB)
					{
						at = resync(at + 1, end);
						continue;
					}

					if (!_r.contains(at, 6)) break;

					const auto length = 6u + static_cast<std::uint64_t>(_r.be16(at + 4));

					if (id == 0xBB)
					{
						if (head) read_system_header(at, length);
					}
					else if (id == 0xBC)
					{
						if (head) read_map(at, length);
					}
					else if (id == 0xBD || (id >= 0xC0 && id <= 0xEF) || id == 0xFD)
					{
						read_pes(at, length, head);
					}

					at += length;
				}

				return std::min(at, _size);
			}

			bool is_start_code(const std::uint64_t at)
			{
				if (!_r.contains(at, 4)) return false;
				const auto code = _r.be32(at);
				return (code >> 8) == 1 && (code & 0xFF) >= 0xB9;
			}

			// The next place at or after from where a pack header, or a packet whose length leads to another
			// start code, begins; else where the search's reach ends, for the scan to go on from. Searched
			// in pieces smaller than the reader's block, which its window keeps.
			std::uint64_t resync(const std::uint64_t from, const std::uint64_t end)
			{
				const auto limit = std::min(end, from + resync_bytes);

				for (auto at = from; at < limit && at + 4 <= _size;)
				{
					const auto piece = _r.read(at, static_cast<std::size_t>(std::min<std::uint64_t>(resync_piece, _size - at)));

					// The last three bytes are looked at again with the next piece, so a start code across
					// the boundary is not missed.
					for (std::size_t i = 0; i + 4 <= piece.size() && at + i < limit; ++i)
					{
						if (piece[i] == 0 && piece[i + 1] == 0 && piece[i + 2] == 1 && piece[i + 3] >= 0xBA && synced(at + i)) return at + i;
					}

					if (piece.size() < 4) break;
					at += piece.size() - 3;
				}

				return limit;
			}

			// Whether a start code at begins a pack header, or a packet, that another start code follows.
			bool synced(const std::uint64_t at)
			{
				if (!_r.contains(at, 6)) return false;

				const auto id = _r.u8(at + 3);
				std::uint64_t length = 0;

				if (id != 0xBA)
				{
					length = 6u + _r.be16(at + 4);
				}
				else if ((_r.u8(at + 4) & 0xC0) == 0x40 && _r.contains(at, 14))
				{
					length = 14u + (_r.u8(at + 13) & 7u);
				}
				else if ((_r.u8(at + 4) & 0xF0) == 0x20)
				{
					length = 12;
				}
				else
				{
					return false;
				}

				return !_r.contains(at + length, 4) || is_start_code(at + length);
			}

			// A pack header's size, noting its clock reference; 0 when it is not one.
			std::uint64_t read_pack(const std::uint64_t at, const bool head)
			{
				std::array<std::uint8_t, 14> b{};
				const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(b.size(), _size - at));
				if (available < 12) return 0;
				_r.read(at, b.data(), available);

				std::uint64_t scr = 0;
				std::uint64_t length = 12;
				std::uint32_t rate = 0;
				auto mpeg1 = false;

				if ((b[4] & 0xC0) == 0x40)
				{
					if (available < 14) return 0;

					// MPEG-2's: a 33-bit base and a 9-bit extension at 27 MHz, split by marker bits.
					const auto base = static_cast<std::uint64_t>(b[4] >> 3 & 7) << 30 | static_cast<std::uint64_t>(b[4] & 3) << 28 |
						static_cast<std::uint64_t>(b[5]) << 20 | static_cast<std::uint64_t>(b[6] >> 3) << 15 |
						static_cast<std::uint64_t>(b[6] & 3) << 13 | static_cast<std::uint64_t>(b[7]) << 5 |
						static_cast<std::uint64_t>(b[8] >> 3);
					const auto extension = static_cast<std::uint64_t>(b[8] & 3) << 7 | static_cast<std::uint64_t>(b[9] >> 1);
					scr = base * 300 + extension;
					rate = static_cast<std::uint32_t>(b[10]) << 14 | static_cast<std::uint32_t>(b[11]) << 6 |
						static_cast<std::uint32_t>(b[12] >> 2);
					length = 14u + (b[13] & 7u);
				}
				else if ((b[4] & 0xF0) == 0x20)
				{
					// MPEG-1's: 33 bits at 90 kHz, laid out as a PES timestamp is.
					scr = read_mpeg_timestamp(b.data() + 4) * 300;
					rate = static_cast<std::uint32_t>(b[9] & 0x7F) << 15 | static_cast<std::uint32_t>(b[10]) << 7 |
						static_cast<std::uint32_t>(b[11] >> 1);
					mpeg1 = true;
				}
				else
				{
					return 0;
				}

				if (_packs++ == 0)
				{
					_mpeg1 = mpeg1;
					_mux_rate = rate;
				}

				_scr.see(_scr_line.place(scr), 0);
				if (!head) _scr_ended = true;
				return length;
			}

			void read_system_header(const std::uint64_t at, const std::uint64_t length)
			{
				if (_intent != scan_intent::inspect || _system_header || length < 12 || !_r.contains(at, length)) return;

				const auto b = _r.read(at, static_cast<std::size_t>(length));
				_system_header = true;
				_rate_bound = static_cast<std::uint32_t>(b[6] & 0x7F) << 15 | static_cast<std::uint32_t>(b[7]) << 7 |
					static_cast<std::uint32_t>(b[8] >> 1);
				_audio_bound = b[9] >> 2;
				_video_bound = b[10] & 0x1F;

				// Each stream it bounds: its id, then the scale and size of its decoder's buffer.
				for (std::size_t i = 12; i + 3 <= b.size() && (b[i] & 0x80); i += 3)
				{
					const auto scale = (b[i + 1] & 0x20) != 0 ? 1024u : 128u;
					_system_streams.emplace_back(b[i], (load_be16(b.data() + i + 1) & 0x1FFFu) * scale);
				}
			}

			void read_map(const std::uint64_t at, const std::uint64_t length)
			{
				// Its CRC is not checked: FFmpeg reads the map whatever it says.
				if (_map_read || length < 16 || !_r.contains(at, length)) return;

				const auto b = _r.read(at, static_cast<std::size_t>(length));
				const std::size_t info = load_be16(b.data() + 8);
				if (12 + info > b.size() - 4) return;

				const std::size_t map_length = load_be16(b.data() + 10 + info);
				const auto end = std::min(12 + info + map_length, b.size() - 4);

				for (auto i = 12 + info; i + 4 <= end;)
				{
					const std::size_t descriptors = load_be16(b.data() + i + 2);
					if (i + 4 + descriptors > end) break;

					map_entry entry;
					entry.stream_type = b[i];
					entry.stream_id = b[i + 1];
					entry.language = read_es_descriptors(std::span<const std::uint8_t>(b).subspan(i + 4, descriptors)).language;
					_map.push_back(std::move(entry));
					i += 4 + descriptors;
				}

				_map_read = true;
			}

			const map_entry* mapped(const std::uint8_t stream_id) const
			{
				const auto entry = std::ranges::find(_map, stream_id, &map_entry::stream_id);
				return entry == _map.end() ? nullptr : &*entry;
			}

			// The codec the program stream map names for a stream id, when it names one of this kind.
			es_type mapped_type(const std::uint8_t stream_id, const media_stream_kind kind) const
			{
				const auto* const entry = mapped(stream_id);
				if (!entry) return {};

				const auto type = es_type_from_stream_type(entry->stream_type, 0);
				return type.known() && type.kind == kind ? type : es_type{};
			}

			// The stream a PES packet belongs to, numbered as FFmpeg's mpeg demuxer numbers it: MPEG video
			// and audio by their start code, private stream 1 by its substream id, and EVOB's VC-1 by the
			// extended stream id after 0xFD. False for a packet FFmpeg passes over.
			bool route(const pes_header& h, const std::span<const std::uint8_t> payload, ps_route& out) const
			{
				const auto id = h.stream_id;

				if (id >= 0xE0 && id <= 0xEF)
				{
					out.id = 0x100u | id;
					out.type = mapped_type(id, media_stream_kind::video);
					if (!out.type.known()) out.type = {media_stream_kind::video, {}, es_syntax::video};
					return true;
				}

				if (id >= 0xC0 && id <= 0xDF)
				{
					out.id = 0x100u | id;
					out.type = mapped_type(id, media_stream_kind::audio);
					if (!out.type.known()) out.type = {media_stream_kind::audio, "mp2", es_syntax::mpeg_audio};
					return true;
				}

				if (id == 0xFD)
				{
					const auto extended = h.extended_stream_id.value_or(0);
					if (extended < 0x55 || extended > 0x5F) return false;

					out.id = 0xFD00u | extended;
					out.type = {media_stream_kind::video, "vc1", es_syntax::vc1};
					return true;
				}

				if (id != 0xBD || payload.empty()) return false;

				const auto sub = payload[0];
				const auto audio = [&out, sub](const std::string_view codec, const es_syntax syntax, const std::size_t skip)
				{
					out = {sub, {media_stream_kind::audio, codec, syntax}, skip};
					return true;
				};

				// AC-3 without a substream header, which FFmpeg takes for substream 0x80.
				if (sub == 0x0B && payload.size() >= 2 && payload[1] == 0x77)
				{
					out = {0x80, {media_stream_kind::audio, "ac3", es_syntax::ac3}, 0};
					return true;
				}

				// After the substream id, audio has a count of frames and a pointer to the first; LPCM's own
				// header follows them, and TrueHD has one byte more.
				if (sub >= 0x80 && sub <= 0x87) return audio("ac3", es_syntax::ac3, 4);
				if ((sub >= 0x88 && sub <= 0x8F) || (sub >= 0x98 && sub <= 0x9F)) return audio("dts", es_syntax::dts, 4);
				if (sub >= 0xA0 && sub <= 0xAF) return audio("pcm", es_syntax::lpcm_dvd, 4);
				if (sub >= 0xB0 && sub <= 0xBF) return audio("truehd", es_syntax::none, 5);

				// EVOB's E-AC-3; as FFmpeg does, the frames have the last word.
				if (sub >= 0xC0 && sub <= 0xCF) return audio("eac3", es_syntax::ac3, 4);

				if (sub >= 0x20 && sub <= 0x3F)
				{
					out = {sub, {media_stream_kind::subtitle, "dvd_subtitle", es_syntax::none}, 1};
					return true;
				}

				return false;
			}

			ps_stream* find(const std::uint32_t id)
			{
				const auto s = std::ranges::find(_streams, id, &ps_stream::id);
				return s == _streams.end() ? nullptr : &*s;
			}

			ps_stream* add(const ps_route& route, const std::uint8_t stream_id)
			{
				if (_streams.size() >= max_streams) return nullptr;

				media_stream stream;
				apply_es_type(route.type, stream);
				stream.id = route.id;

				// A map's entry for private stream 1 stands for all its substreams, so says nothing of one.
				if (const auto* const entry = mapped(stream_id); entry && stream_id != 0xBD)
				{
					stream.codec_tag = std::format("0x{:02X}", entry->stream_type);
					stream.language = entry->language;
				}

				ps_stream s;
				s.id = route.id;
				s.stream_id = stream_id;
				s.index = _out.streams.size();
				s.skip = route.skip;
				s.timed = stream.kind == media_stream_kind::video || stream.kind == media_stream_kind::audio;

				const auto camera = (route.type.syntax == es_syntax::h264 || route.type.syntax == es_syntax::video) && !_camera_taken;
				_camera_taken = _camera_taken || camera;
				s.probe = es_probe(route.type.syntax, camera);

				_out.streams.push_back(std::move(stream));
				_streams.push_back(std::move(s));
				return &_streams.back();
			}

			void read_pes(const std::uint64_t at, const std::uint64_t length, const bool head)
			{
				const auto available = std::min(length, _size - at);
				pes_header h;
				ps_route where;

				{
					const auto bytes = _r.view(at, static_cast<std::size_t>(std::min<std::uint64_t>(available, header_bytes)));
					if (read_pes_header(bytes, true, h) != pes_read::ok) return;

					_any_pes = true;
					_seen[h.stream_id] = true;
					if (!route(h, bytes.subspan(std::min(h.size, bytes.size())), where)) return;
				}

				auto* s = find(where.id);

				if (!s)
				{
					if (!head) return;
					s = add(where, h.stream_id);
					if (!s) return;
				}

				if (h.pts && s->timed)
				{
					const auto payload = 6u + h.packet_length > h.size ? 6u + h.packet_length - h.size : 0u;
					s->clock.see(_pts.place(*h.pts), payload > s->skip ? payload - s->skip : 0);
					if (!head) s->ended = true;
				}

				if (head && !s->probe.done() && h.size + s->skip < available)
				{
					const auto bytes = _r.view(at, static_cast<std::size_t>(available));
					s->probe.add(bytes.subspan(h.size + s->skip), _out.streams[s->index], _out.properties);
				}
			}

			bool settled(const std::uint64_t at) const
			{
				if (_streams.empty()) return false;

				const auto known = std::ranges::all_of(_streams, [](const ps_stream& s)
				{
					return !s.timed || (s.probe.done() && s.clock.first);
				});

				if (!known) return false;

				// A map lists the streams, so once each has been seen there are no more to wait for. A system
				// header does not: a VCD has one for each stream, the first naming the video alone.
				if (_map_read && std::ranges::all_of(_map, [this](const map_entry& e) { return _seen[e.stream_id]; })) return true;

				return (_scr.first && _scr.last && *_scr.last - *_scr.first >= static_cast<std::int64_t>(discovery_ticks)) ||
					at >= discovery_bytes;
			}

			bool tail_complete() const
			{
				auto timed = false;

				for (const auto& s : _streams)
				{
					if (!s.timed || !s.clock.first) continue;
					timed = true;
					if (!s.ended) return false;
				}

				// Without timestamps the packs' clock alone times the stream.
				return timed || !_scr.first || _scr_ended;
			}

			void finish_timing()
			{
				std::vector<timed_stream> timed;

				for (auto& s : _streams)
				{
					auto& stream = _out.streams[s.index];
					timed.push_back({&stream, s.clock, s.probe.end_ticks(stream, s.clock.last_payload)});
				}

				auto duration = finish_pts_durations(timed);

				if (!(duration > 0) && _scr.first && _scr.last && *_scr.last > *_scr.first)
				{
					duration = static_cast<double>(*_scr.last - *_scr.first) / static_cast<double>(mpeg_system_ticks_per_second);
				}

				_out.duration = duration;
			}

			void structure(const std::uint64_t head_end, const std::uint64_t tail_start)
			{
				auto& rows = _out.structure;

				add_structure_section(rows, "Program stream", "ps.summary", true);
				add_structure_row(rows, "Pack header",
				                  std::format("{}, mux rate {} bit/s", _mpeg1 ? "MPEG-1" : "MPEG-2", static_cast<std::uint64_t>(_mux_rate) * 400),
				                  "0xBA");
				add_structure_row(rows, "Packs read", std::to_string(_packs));

				if (_scr.first && _scr.last)
				{
					add_structure_row(rows, "SCR", std::format("{:.3f} s to {:.3f} s",
					                                           static_cast<double>(*_scr.first) / mpeg_system_ticks_per_second,
					                                           static_cast<double>(*_scr.last) / mpeg_system_ticks_per_second));
				}

				if (_system_header)
				{
					add_structure_row(rows, "System header",
					                  std::format("rate bound {} bit/s, {} audio and {} video streams at most",
					                              static_cast<std::uint64_t>(_rate_bound) * 400, _audio_bound, _video_bound), "0xBB");

					for (const auto& [id, bound] : _system_streams)
					{
						add_structure_row(rows, std::format("Bound 0x{:02X}", id), std::format("buffer {} bytes", bound));
					}
				}

				if (_map_read)
				{
					add_structure_row(rows, "Program stream map", std::format("{} streams", _map.size()), "0xBC");

					for (const auto& e : _map)
					{
						add_structure_row(rows, std::format("Map 0x{:02X}", e.stream_id), std::format("stream type 0x{:02X}", e.stream_type));
					}
				}

				add_structure_row(rows, "Read", tail_start < _size
					                                ? std::format("{} bytes from the start, and the last {}", head_end, _size - tail_start)
					                                : std::format("{} bytes from the start", head_end));

				add_structure_section(rows, "Streams", "ps.streams");

				for (const auto& s : _streams)
				{
					const auto& stream = _out.streams[s.index];
					auto value = stream.codec.empty() ? std::string("unknown") : stream.codec;

					if (s.clock.first && s.clock.last)
					{
						value += std::format(", PTS {:.3f} s to {:.3f} s", static_cast<double>(*s.clock.first) / mpeg_ticks_per_second,
						                     static_cast<double>(*s.clock.last) / mpeg_ticks_per_second);
					}

					const auto key = s.stream_id == 0xBD
						                 ? std::format("Private stream 1, substream 0x{:02X}", s.id)
						                 : std::format("Stream 0x{:X}", s.id);
					add_structure_row(rows, key, std::move(value), std::format("0x{:02X}", s.stream_id));
				}

				finish_structure_sections(rows);
			}

			reader& _r;
			scan_intent _intent;
			media_scan_result& _out;
			std::uint64_t _size;

			std::uint64_t _packs = 0;
			bool _mpeg1 = false;
			std::uint32_t _mux_rate = 0;
			bool _any_pes = false;
			std::array<bool, 256> _seen{};

			bool _system_header = false;
			std::uint32_t _rate_bound = 0;
			int _audio_bound = 0;
			int _video_bound = 0;
			std::vector<std::pair<std::uint8_t, std::uint32_t>> _system_streams;

			bool _map_read = false;
			std::vector<map_entry> _map;

			std::vector<ps_stream> _streams;
			bool _camera_taken = false;

			timestamp_line _pts{mpeg_timestamp_wrap, mpeg_ticks_per_second};
			timestamp_line _scr_line{mpeg_system_clock_wrap, mpeg_system_ticks_per_second};
			es_clock _scr;
			bool _scr_ended = false;
		};
	}

	void scan_mpeg_ps_media(reader& r, const scan_intent intent, media_scan_result& out)
	{
		ps_scan(r, intent, out).run();
	}
}
