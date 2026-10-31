// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning Ogg files: Vorbis, Opus, Speex, FLAC and Theora. The first page of each logical stream
// names its codec; its header packets, put together across pages, give its format and its comments; and the
// granule position of its last page, found from the end of the file, how long it plays. A chained file -
// links of streams one after another - is described by its first link alone.

#include "media_internal.h"

#include <algorithm>
#include <array>
#include <format>

namespace libfiles::detail
{
	namespace
	{
		// RFC 3533: a page is a 27-byte header, a table of up to 255 segment sizes, and the segments.
		constexpr std::size_t page_header_size = 27;
		constexpr std::size_t max_page_size = page_header_size + 255 + 255 * 255;
		// Comments may carry pictures, base64-coded, so a header packet is put together up to this size.
		constexpr std::size_t max_packet = 16 * 1024 * 1024;
		// How far the first pages are walked for the streams' headers.
		constexpr std::uint64_t max_header_walk = 2 * std::uint64_t{max_packet};
		// Pages in a row that bring no stream a header before the walk gives up on what is missing.
		constexpr int max_idle_pages = 64;
		// How far from the end the streams' last pages are looked for.
		constexpr std::uint64_t max_tail = 1024 * 1024;
		// The most a scan checks the CRC of, pages and candidates that fail alike: the header walk and the tail
		// twice over. A capture pattern can claim a whole page wherever it lies, so a file of them would
		// otherwise cost a page's CRC for every few bytes it holds.
		constexpr std::uint64_t max_checked = 2 * (max_header_walk + max_tail);
		// The granule position of a page on which no packet ends.
		constexpr std::uint64_t no_granule = ~std::uint64_t{0};
		constexpr std::uint8_t continued_page = 1;
		constexpr std::uint8_t first_page = 2;
		constexpr std::size_t no_position = ~std::size_t{0};

		// The CRC-32 Ogg checks a page with: polynomial 0x04C11DB7, from zero, most significant bit first.
		constexpr auto crc_table = []
		{
			std::array<std::uint32_t, 256> table{};

			for (std::uint32_t i = 0; i < 256; ++i)
			{
				auto c = i << 24;
				for (auto k = 0; k < 8; ++k) c = c & 0x80000000u ? c << 1 ^ 0x04C11DB7u : c << 1;
				table[i] = c;
			}

			return table;
		}();

		std::uint64_t load_le64(const std::uint8_t* const p)
		{
			return static_cast<std::uint64_t>(load_le32(p + 4)) << 32 | load_le32(p);
		}

		struct page
		{
			std::uint8_t flags = 0;
			std::uint64_t granule = no_granule;
			std::uint32_t serial = 0;
			std::span<const std::uint8_t> lacing;
			std::span<const std::uint8_t> body;
			std::uint64_t body_offset = 0;
		};

		// The page at the start of bytes, when it is whole, of version 0, and its CRC matches: the bytes it
		// takes, else 0. What it checks counts against the scan's max_checked, and none is checked past it.
		std::size_t parse_page(const std::span<const std::uint8_t> bytes, page& p, std::uint64_t& checked)
		{
			if (bytes.size() < page_header_size || !starts_with(bytes, "OggS") || bytes[4] != 0) return 0;

			const std::size_t segments = bytes[26];
			if (bytes.size() < page_header_size + segments) return 0;

			std::size_t body = 0;
			for (std::size_t i = 0; i < segments; ++i) body += bytes[page_header_size + i];

			const auto length = page_header_size + segments + body;
			if (bytes.size() < length || checked >= max_checked) return 0;
			checked += length;

			std::uint32_t crc = 0;

			for (std::size_t i = 0; i < length; ++i)
			{
				const std::uint32_t b = i >= 22 && i < 26 ? 0 : bytes[i];
				crc = crc << 8 ^ crc_table[(crc >> 24 ^ b) & 0xFF];
			}

			if (crc != load_le32(bytes.data() + 22)) return 0;

			p.flags = bytes[5];
			p.granule = load_le64(bytes.data() + 6);
			p.serial = load_le32(bytes.data() + 14);
			p.lacing = bytes.subspan(page_header_size, segments);
			p.body = bytes.subspan(page_header_size + segments, body);
			return length;
		}

		std::size_t find_capture(const std::span<const std::uint8_t> bytes, const std::size_t from)
		{
			for (auto i = from; i + 4 <= bytes.size(); ++i)
			{
				if (bytes[i] == 'O' && bytes[i + 1] == 'g' && bytes[i + 2] == 'g' && bytes[i + 3] == 'S') return i;
			}

			return no_position;
		}

		// The page at offset, read in steps - its header, its segment table, then the page - so that no more is
		// read than it takes; 0 when it is not a whole page that checks.
		std::size_t read_page(reader& r, const std::uint64_t offset, page& p, std::uint64_t& checked)
		{
			if (!r.contains(offset, page_header_size)) return 0;

			const std::size_t segments = r.u8(offset + 26);
			if (!r.contains(offset, page_header_size + segments)) return 0;

			std::size_t length = page_header_size + segments;
			for (const auto s : r.view(offset + page_header_size, segments)) length += s;
			if (!r.contains(offset, length)) return 0;

			const auto read = parse_page(r.view(offset, length), p, checked);
			p.body_offset = offset + page_header_size + segments;
			return read;
		}

		enum class codec : std::uint8_t
		{
			unknown,
			skeleton,
			vorbis,
			opus,
			flac,
			theora,
			speex,
		};

		struct logical_stream
		{
			std::uint32_t serial = 0;
			// FFmpeg's number for it: its place among the streams that begin the file, Skeleton included.
			std::uint64_t id = 0;
			codec kind = codec::unknown;
			// Where it is in the result's streams; a Skeleton stream is not listed there.
			std::size_t index = no_position;

			// The packet being put together, while its headers are read: its bytes, where it begins, whether it
			// runs onto another page, and its first two bytes.
			std::vector<std::uint8_t> packet;
			bool in_packet = false;
			bool oversized = false;
			bool split = false;
			std::uint64_t packet_offset = 0;
			std::array<std::uint8_t, 2> head{};
			std::size_t head_size = 0;

			std::size_t headers = 0;
			bool headers_done = false;
			// Whether the first page of data has said where the stream starts, as Opus and Theora need.
			bool start_known = true;
			// In granule units: samples, or Theora frames.
			std::uint64_t start = 0;

			std::uint32_t sample_rate = 0;
			std::uint32_t pre_skip = 0;
			double frame_rate = 0;
			unsigned granule_shift = 0;
			// Theora before 3.2.1 numbered its frames from 0.
			bool zero_based = false;
			// The header packets the Ogg FLAC mapping announces after its first, 0 when it does not say.
			std::uint32_t flac_headers = 0;

			std::uint64_t last_granule = no_granule;

			bool done() const { return headers_done && start_known; }
			bool timed() const { return kind >= codec::vorbis; }
		};

		// The samples an Opus packet decodes to, from its TOC byte and the frame count that may follow it
		// (RFC 6716, 3.1).
		std::uint64_t opus_samples(const logical_stream& s)
		{
			if (s.head_size == 0) return 0;

			const unsigned config = s.head[0] >> 3u;
			const unsigned frame = config < 12 ? std::max(480u, 960u * (config & 3)) : config < 16 ? 480u << (config & 1) : 120u << (config & 3);
			const unsigned code = s.head[0] & 3u;
			const unsigned frames = code == 0 ? 1 : code < 3 ? 2 : s.head_size > 1 ? s.head[1] & 0x3Fu : 0;
			return std::uint64_t{frame} * frames;
		}

		// A Theora granule position as a count of frames: the last keyframe's number in its high bits, and
		// the frames since in the low ones.
		std::uint64_t theora_frames(const logical_stream& s, const std::uint64_t granule)
		{
			return (granule >> s.granule_shift) + (granule & ((std::uint64_t{1} << s.granule_shift) - 1)) + (s.zero_based ? 1 : 0);
		}

		// A stream begins: its first packet names its codec.
		logical_stream begin_stream(const page& p, const std::uint64_t id, media_scan_result& out)
		{
			logical_stream s;
			s.serial = p.serial;
			s.id = id;

			const auto b = p.body;
			if (starts_with(b, "\x01vorbis")) s.kind = codec::vorbis;
			else if (starts_with(b, "OpusHead")) s.kind = codec::opus;
			else if (starts_with(b, "\x7F" "FLAC")) s.kind = codec::flac;
			else if (starts_with(b, "\x80theora")) s.kind = codec::theora;
			else if (starts_with(b, "Speex   ")) s.kind = codec::speex;
			else if (starts_with(b, std::string_view("fishead\0", 8))) s.kind = codec::skeleton;

			s.headers_done = !s.timed();
			s.start_known = s.kind != codec::opus && s.kind != codec::theora;
			if (s.kind == codec::skeleton) return s;

			static constexpr std::string_view names[] = {"", "", "vorbis", "opus", "flac", "theora", "speex"};
			media_stream m;
			m.id = id;
			m.codec = names[static_cast<std::size_t>(s.kind)];
			m.kind = s.kind == codec::theora ? media_stream_kind::video : s.timed() ? media_stream_kind::audio : media_stream_kind::data;
			s.index = out.streams.size();
			out.streams.push_back(std::move(m));
			return s;
		}

		void take_audio_header(logical_stream& s, const audio_header& a, media_stream& m)
		{
			apply_audio_header(a, m.audio);
			m.bit_rate = a.bit_rate;
			s.sample_rate = a.sample_rate;
		}

		// A header packet - or what was taken for one - of a stream whose headers are being read, at offset
		// in the file when it lies there whole. True when it turns out to be the first packet of data.
		bool take_header(logical_stream& s, const std::span<const std::uint8_t> packet, const std::uint64_t offset, media_scan_result& out)
		{
			const auto number = s.headers++;
			auto& m = out.streams[s.index];

			switch (s.kind)
			{
			case codec::vorbis:
				if (number == 0)
				{
					audio_header a;
					if (parse_vorbis_header(packet, a)) take_audio_header(s, a, m);
					return false;
				}

				if (starts_with(packet, "\x03vorbis")) read_vorbis_comments(packet.subspan(7), out);
				s.headers_done = true;
				return false;

			case codec::opus:
				if (number == 0)
				{
					audio_header a;
					std::uint32_t pre_skip = 0;

					if (parse_opus_head(packet, a, pre_skip))
					{
						take_audio_header(s, a, m);
						s.pre_skip = pre_skip;
					}

					return false;
				}

				s.headers_done = true;
				if (!starts_with(packet, "OpusTags")) return !packet.empty();
				read_vorbis_comments(packet.subspan(8), out);
				return false;

			case codec::flac:
				if (number == 0)
				{
					// The mapping's own header - 0x7F "FLAC", its version, the count of header packets after
					// it - then "fLaC" and the STREAMINFO block with its block header.
					if (packet.size() >= 51 && packet[5] == 1 && starts_with(packet.subspan(9), "fLaC"))
					{
						audio_header a;
						std::uint64_t total_samples = 0;
						s.flac_headers = load_be16(packet.data() + 7);
						if (parse_flac_streaminfo(packet.subspan(17, 34), a, total_samples)) take_audio_header(s, a, m);
					}

					return false;
				}

				// A frame's sync code: the metadata blocks have ended.
				if (packet.size() < 4 || packet[0] == 0xFF)
				{
					s.headers_done = true;
					return !packet.empty();
				}

				if ((packet[0] & 0x7F) == 4)
				{
					read_vorbis_comments(packet.subspan(4), out);
				}
				else if ((packet[0] & 0x7F) == 6)
				{
					// Located when the packet lies whole in the file, as long as the picture lies inside it.
					const auto block = packet.subspan(4);
					const auto located = offset != 0 ? offset + 4 : 0;
					media_picture picture;

					if (read_flac_picture(block, located, picture))
					{
						const auto& place = picture.location;
						const auto inside = located == 0 || (place.offset >= located && place.offset - located <= block.size() &&
							place.size <= block.size() - (place.offset - located));
						if (inside) out.pictures.push_back(std::move(picture));
					}
				}

				s.headers_done = (packet[0] & 0x80) != 0 || (s.flac_headers != 0 && number >= s.flac_headers);
				return false;

			case codec::theora:
				if (number == 0)
				{
					video_header v;

					if (parse_theora_header(packet, v))
					{
						apply_video_header(v, m.video);
						s.frame_rate = v.frame_rate;
					}

					// The version, and the keyframe shift that splits a granule position (Theora 6.2).
					if (packet.size() >= 42)
					{
						s.zero_based = (packet[7] << 16 | packet[8] << 8 | packet[9]) < 0x030201;
						s.granule_shift = (packet[40] & 3u) << 3 | packet[41] >> 5;
					}

					return false;
				}

				// A packet without the header bit is the first frame; an empty one repeats a frame.
				if (packet.empty() || !(packet[0] & 0x80))
				{
					s.headers_done = true;
					return true;
				}

				if (starts_with(packet, "\x81theora")) read_vorbis_comments(packet.subspan(7), out);
				s.headers_done = number >= 2;
				return false;

			case codec::speex:
				if (number == 0)
				{
					audio_header a;
					if (parse_speex_header(packet, a)) take_audio_header(s, a, m);
					return false;
				}

				read_vorbis_comments(packet, out);
				s.headers_done = true;
				return false;

			default:
				s.headers_done = true;
				return false;
			}
		}

		// One page of a stream whose headers are being read. Its segments make packets: header packets are
		// read as they end, and for Opus and Theora the first page on which data ends says where the stream
		// starts, as FFmpeg reckons it - the page's granule position less what its packets hold.
		void take_page(logical_stream& s, const page& p, media_scan_result& out)
		{
			std::size_t segment = 0;
			std::size_t at = 0;

			if (p.flags & continued_page)
			{
				// The rest of a packet that began on a page not read is passed over.
				while (!s.in_packet && segment < p.lacing.size())
				{
					const auto length = p.lacing[segment++];
					at += length;
					if (length < 255) break;
				}
			}
			else if (s.in_packet)
			{
				// The packet in progress never ended.
				s.packet.clear();
				s.in_packet = false;
			}

			std::uint64_t held = 0;
			auto data_packets = 0;

			for (; segment < p.lacing.size(); ++segment)
			{
				const std::size_t length = p.lacing[segment];
				const auto bytes = p.body.subspan(at, length);
				at += length;

				if (!s.in_packet)
				{
					s.in_packet = true;
					s.oversized = false;
					s.split = false;
					s.packet_offset = p.body_offset + (at - length);
					s.head_size = 0;
				}

				for (std::size_t i = 0; i < bytes.size() && s.head_size < s.head.size(); ++i) s.head[s.head_size++] = bytes[i];

				if (!s.headers_done && !s.oversized)
				{
					if (s.packet.size() + length > max_packet)
					{
						s.oversized = true;
						s.packet = {};
					}
					else
					{
						s.packet.insert(s.packet.end(), bytes.begin(), bytes.end());
					}
				}

				if (length == 255) continue;

				auto data = s.headers_done;
				if (!data) data = take_header(s, s.oversized ? std::span<const std::uint8_t>() : s.packet, s.split ? 0 : s.packet_offset, out);

				if (data)
				{
					++data_packets;
					held += s.kind == codec::opus ? opus_samples(s) : 1;
				}

				s.packet.clear();
				s.in_packet = false;
			}

			if (s.in_packet) s.split = true;

			if (s.headers_done && !s.start_known && data_packets != 0)
			{
				s.start_known = true;

				if (p.granule < std::uint64_t{1} << 63)
				{
					const auto end = s.kind == codec::theora ? theora_frames(s, p.granule) : p.granule;
					s.start = end > held ? end - held : 0;
				}
			}
		}

		// The granule positions of the pages that begin in [from, stop), read from bytes that run to `to` so
		// that the last of them is whole: each stream that has none yet takes the last of its own. A page of
		// a stream that did not begin the file belongs to a later link, and so does a stream's first page
		// after the first link's: links joined end to end may reuse a serial number.
		void read_last_pages(reader& r, const std::uint64_t from, const std::uint64_t to, const std::uint64_t stop,
		                     const std::uint64_t first_link_begun, std::vector<logical_stream>& streams, bool& chained, std::uint64_t& checked)
		{
			const auto bytes = r.view(from, static_cast<std::size_t>(to - from));
			std::vector<std::uint64_t> found(streams.size(), no_granule);

			for (std::size_t at = 0; at < bytes.size() && from + at < stop && checked < max_checked;)
			{
				page p;
				const auto length = bytes[at] == 'O' ? parse_page(bytes.subspan(at), p, checked) : 0;

				if (length == 0)
				{
					++at;
					continue;
				}

				const auto s = std::ranges::find(streams, p.serial, &logical_stream::serial);

				if (s == streams.end() || ((p.flags & first_page) && from + at >= first_link_begun))
				{
					chained = true;
				}
				else if (p.granule != no_granule && p.granule != 0 && p.granule < std::uint64_t{1} << 63)
				{
					found[static_cast<std::size_t>(s - streams.begin())] = p.granule;
				}

				at += length;
			}

			for (std::size_t i = 0; i < streams.size(); ++i)
			{
				if (streams[i].last_granule == no_granule) streams[i].last_granule = found[i];
			}
		}

		// How long a stream plays, by its last granule position. Vorbis, FLAC and Speex are timed from
		// position 0, as Diffractor's FFmpeg times them; Opus and Theora from where their first page of data
		// starts them, the start FFmpeg reckons for those two.
		double stream_duration(const logical_stream& s)
		{
			const auto g = s.last_granule;
			if (g == no_granule) return 0;

			switch (s.kind)
			{
			case codec::vorbis:
			case codec::flac:
			case codec::speex:
				return s.sample_rate != 0 ? static_cast<double>(g) / s.sample_rate : 0;
			case codec::opus:
				// RFC 7845, 4: the samples the pre-skip discards are not played.
				return g > s.start && g - s.start > s.pre_skip ? static_cast<double>(g - s.start - s.pre_skip) / 48000 : 0;
			case codec::theora:
			{
				const auto frames = theora_frames(s, g);
				return frames > s.start && s.frame_rate > 0 ? static_cast<double>(frames - s.start) / s.frame_rate : 0;
			}
			default:
				return 0;
			}
		}
	}

	void scan_ogg_media(reader& r, const scan_intent intent, media_scan_result& out)
	{
		const auto size = r.size();
		std::vector<logical_stream> streams;
		auto chained = false;
		auto group_closed = false;
		auto idle = 0;
		std::uint64_t offset = 0;
		std::uint64_t header_pages = 0;
		// Where the first page that begins no stream lies: past it, a page that begins one begins a later link.
		auto first_link_begun = size;
		std::uint64_t checked = 0;

		// The first pages: each stream's beginning, then its headers.
		while (offset < size && offset < max_header_walk)
		{
			page p;
			const auto length = read_page(r, offset, p, checked);

			if (length == 0)
			{
				if (checked >= max_checked) break;

				// Not a whole page that checks: on to the next capture pattern, as FFmpeg resynchronises.
				const auto next = find_capture(r.view(offset, static_cast<std::size_t>(std::min<std::uint64_t>(size - offset, max_page_size))), 1);
				if (next == no_position) break;
				offset += next;
				continue;
			}

			const auto page_offset = offset;
			offset += length;
			++header_pages;

			if (p.flags & first_page)
			{
				// Every stream of a link begins before any page of data, so one that begins later starts the
				// next link.
				if (group_closed)
				{
					chained = true;
					break;
				}

				if (std::ranges::find(streams, p.serial, &logical_stream::serial) == streams.end())
				{
					streams.push_back(begin_stream(p, streams.size(), out));
				}
			}
			else if (!group_closed)
			{
				group_closed = true;
				first_link_begun = page_offset;
			}

			const auto s = std::ranges::find(streams, p.serial, &logical_stream::serial);

			if (s != streams.end() && !s->done())
			{
				take_page(*s, p, out);
				idle = 0;
			}
			else if (group_closed && ++idle > max_idle_pages)
			{
				break;
			}

			if (group_closed && std::ranges::all_of(streams, &logical_stream::done)) break;
		}

		if (streams.empty()) throw format_error("no Ogg stream begins in the file");

		// The last pages, from the end: the last page's worth of bytes, then twice as much each time a stream
		// that plays for a time has yet to be found.
		const auto missing = [&] { return std::ranges::any_of(streams, [](const logical_stream& s) { return s.timed() && s.last_granule == no_granule; }); };
		auto scanned = size;
		auto tail = std::min<std::uint64_t>(size, max_page_size);

		while (!chained && missing())
		{
			const auto from = size - tail;
			read_last_pages(r, from, std::min(size, scanned + max_page_size), scanned, first_link_begun, streams, chained, checked);
			scanned = from;

			const auto wider = std::min({size, tail * 2, max_tail});
			if (wider <= tail) break;
			tail = wider;
		}

		if (!chained)
		{
			for (const auto& s : streams)
			{
				if (s.index != no_position) out.streams[s.index].duration = stream_duration(s);
			}
		}

		out.brand = "Ogg";

		if (intent == scan_intent::inspect)
		{
			static constexpr std::string_view names[] = {"unknown", "Skeleton", "Vorbis", "Opus", "FLAC", "Theora", "Speex"};
			add_structure_section(out.structure, "Streams", "ogg.streams", true);

			for (const auto& s : streams)
			{
				auto value = std::format("serial {:#010x}, {}", s.serial, names[static_cast<std::size_t>(s.kind)]);
				if (s.last_granule != no_granule) value += std::format(", last granule position {}", s.last_granule);
				add_structure_row(out.structure, std::format("Stream {}", s.id), std::move(value));
			}

			add_structure_section(out.structure, "Pages", "ogg.pages");
			add_structure_row(out.structure, "Headers", std::format("{} pages, offset 0 to {}", header_pages, offset));
			if (scanned < size) add_structure_row(out.structure, "Last pages", std::format("offset {} to {}", scanned, size));
			if (chained) add_structure_row(out.structure, "Chained", "a later link follows; only the first is described");
			finish_structure_sections(out.structure);
		}
	}
}
