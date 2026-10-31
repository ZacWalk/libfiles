// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the MP4 and QuickTime media scan. Every file is built here box by box, so every field
// a scan reports is one the test put there. Each file a test sweeps is also cut short near its ends and
// at its boxes' edges, damaged after each box's header and given wrong box sizes - a few at every byte -
// which must never crash, read past the end of the file or report more than the scan read, and whose
// packet must be the one read_packet reads.

#include "test.h"

#include "builders.h"
#include "libfiles/media.h"
#include "libfiles/write.h"
#include "media_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace test_build;

namespace
{
	using libfiles::media_stream_kind;

	bytes join(const std::initializer_list<bytes> parts)
	{
		bytes out;
		for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
		return out;
	}

	bytes join(const std::vector<bytes>& parts)
	{
		bytes out;
		for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
		return out;
	}

	bytes box(const std::string_view type, const bytes& payload)
	{
		return iso_box(type, payload);
	}

	bytes full(const std::string_view type, const unsigned version, const std::uint32_t flags, const bytes& payload)
	{
		return iso_full_box(type, version, flags, payload);
	}

	// A box in the 64-bit size form.
	bytes large_box(const std::string_view type, const bytes& payload)
	{
		return builder().be32(1).raw(type).be32(0).be32(static_cast<std::uint32_t>(payload.size() + 16)).raw(payload).b;
	}

	builder& be64(builder& w, const std::uint64_t v)
	{
		return w.be32(static_cast<std::uint32_t>(v >> 32)).be32(static_cast<std::uint32_t>(v));
	}

	// A language as mdhd and QuickTime's text pack it.
	std::uint16_t packed(const std::string_view language)
	{
		return static_cast<std::uint16_t>((language[0] - 0x60) << 10 | (language[1] - 0x60) << 5 | (language[2] - 0x60));
	}

	const std::uint16_t und = packed("und");

	bytes ftyp(const std::string_view major, const std::vector<std::string_view>& compatible = {}, const std::uint32_t minor = 0)
	{
		builder w;
		w.raw(major).be32(minor);
		for (const auto brand : compatible) w.raw(brand);
		return box("ftyp", w.b);
	}

	// A transform's a, b, c and d in 16.16 fixed point.
	struct transform
	{
		std::int32_t a = 0x10000;
		std::int32_t b = 0;
		std::int32_t c = 0;
		std::int32_t d = 0x10000;
	};

	void put_matrix(builder& w, const transform& m)
	{
		w.be32(static_cast<std::uint32_t>(m.a)).be32(static_cast<std::uint32_t>(m.b)).be32(0);
		w.be32(static_cast<std::uint32_t>(m.c)).be32(static_cast<std::uint32_t>(m.d)).be32(0);
		w.be32(0).be32(0).be32(0x40000000);
	}

	bytes mvhd(const std::uint32_t timescale, const std::uint64_t duration, const std::uint64_t created = 0, const unsigned version = 0,
	           const transform& m = {})
	{
		builder w;

		if (version == 1)
		{
			be64(w, created).zeros(8).be32(timescale);
			be64(w, duration);
		}
		else
		{
			w.be32(static_cast<std::uint32_t>(created)).be32(0).be32(timescale).be32(static_cast<std::uint32_t>(duration));
		}

		w.be32(0x10000).be16(0x100).zeros(10);
		put_matrix(w, m);
		w.zeros(24).be32(10);
		return full("mvhd", version, 0, w.b);
	}

	bytes tkhd(const std::uint32_t id, const std::uint32_t flags, const std::uint32_t width, const std::uint32_t height,
	           const transform& m = {}, const unsigned version = 0)
	{
		builder w;
		if (version == 1) w.zeros(16).be32(id).zeros(4).zeros(8);
		else w.zeros(8).be32(id).zeros(4).zeros(4);
		w.zeros(8).be16(0).be16(0).be16(0x100).be16(0);
		put_matrix(w, m);
		w.be32(width << 16).be32(height << 16);
		return full("tkhd", version, flags, w.b);
	}

	bytes mdhd(const std::uint32_t timescale, const std::uint64_t duration, const std::uint16_t language, const unsigned version = 0)
	{
		builder w;

		if (version == 1)
		{
			w.zeros(16).be32(timescale);
			be64(w, duration);
		}
		else
		{
			w.zeros(8).be32(timescale).be32(static_cast<std::uint32_t>(duration));
		}

		w.be16(language).be16(0);
		return full("mdhd", version, 0, w.b);
	}

	// ISO ends a handler's name with a NUL; QuickTime counts it, after its component type.
	bytes hdlr(const std::string_view handler, const std::string_view name = {}, const bool quicktime = false)
	{
		builder w;
		if (quicktime) w.raw("mhlr");
		else w.zeros(4);
		w.raw(handler).zeros(12);
		if (quicktime) w.u8(static_cast<unsigned>(name.size())).raw(name);
		else w.raw(name).u8(0);
		return full("hdlr", 0, 0, w.b);
	}

	// A visual sample description, its 78 bytes of fields and then its boxes.
	bytes visual(const std::string_view type, const unsigned width, const unsigned height, const bytes& boxes = {}, const unsigned depth = 24,
	             const bytes& colour_table = {})
	{
		builder w;
		w.zeros(6).be16(1).zeros(16).be16(width).be16(height).be32(0x480000).be32(0x480000).be32(0).be16(1);
		w.u8(4).raw("test").zeros(27).be16(depth).be16(colour_table.empty() ? 0xFFFF : 0);
		w.raw(colour_table).raw(boxes);
		return box(type, w.b);
	}

	// A sound sample description: ISO's, which is QuickTime's version 0, or QuickTime's version 1 with
	// its four more fields.
	bytes sound(const std::string_view type, const unsigned channels, const unsigned sample_size, const std::uint32_t rate,
	            const bytes& boxes = {}, const unsigned version = 0)
	{
		builder w;
		w.zeros(6).be16(1).be16(version).be16(0).be32(0).be16(channels).be16(sample_size).be16(0).be16(0).be32(rate << 16);
		if (version == 1) w.be32(1024).be32(1).be32(2).be32(2);
		w.raw(boxes);
		return box(type, w.b);
	}

	// QuickTime's version 2 sound description, with its rate as a double and LPCM's flags.
	bytes sound_v2(const std::string_view type, const std::uint32_t channels, const std::uint32_t bits, const double rate,
	               const std::uint32_t flags, const bytes& boxes = {})
	{
		builder w;
		w.zeros(6).be16(1).be16(2).be16(0).be32(0).be16(3).be16(16).be16(0xFFFE).be16(0).be32(0x10000);
		w.be32(72);
		be64(w, std::bit_cast<std::uint64_t>(rate));
		w.be32(channels).be32(0x7F000000).be32(bits).be32(flags).be32(channels * bits / 8).be32(1);
		w.raw(boxes);
		return box(type, w.b);
	}

	// An esds box: an ES_Descriptor holding the decoder configuration, its lengths written in one byte
	// or, as Apple writes them, in four.
	bytes esds(const std::uint8_t object_type, const std::uint32_t average, const bytes& specific = {}, const bool long_lengths = false)
	{
		const auto length = [long_lengths](builder& w, const std::size_t n)
		{
			if (long_lengths) w.u8(0x80).u8(0x80).u8(0x80);
			w.u8(static_cast<unsigned>(n));
		};

		builder config;
		config.u8(object_type).u8(0x15).u8(0).be16(0x1800).be32(average * 2).be32(average);

		if (!specific.empty())
		{
			config.u8(5);
			length(config, specific.size());
			config.raw(specific);
		}

		builder es;
		es.be16(1).u8(0).u8(4);
		length(es, config.size());
		es.raw(config.b).u8(6).u8(1).u8(2);

		builder w;
		w.u8(3);
		length(w, es.size());
		w.raw(es.b);
		return full("esds", 0, 0, w.b);
	}

	bytes btrt(const std::uint32_t average)
	{
		return box("btrt", builder().be32(0x10000).be32(average * 2).be32(average).b);
	}

	bytes pasp(const std::uint32_t h, const std::uint32_t v)
	{
		return box("pasp", builder().be32(h).be32(v).b);
	}

	bytes colr_nclx(const unsigned primaries, const unsigned transfer, const unsigned matrix, const bool full_range)
	{
		return box("colr", builder().raw("nclx").be16(primaries).be16(transfer).be16(matrix).u8(full_range ? 0x80 : 0).b);
	}

	// The avcC of a 640 x 480 High profile x264 stream, 4:2:0 at 8 bits, whose SPS says its pixels are
	// 4:3, as anamorphic.mp4 holds it.
	const bytes avc_config = {
		0x01, 0x64, 0x00, 0x16, 0xFF, 0xE1, 0x00, 0x19, 0x67, 0x64, 0x00, 0x16, 0xAC, 0xD9, 0x40, 0xA0, 0x3D, 0xB0, 0xE1, 0x00,
		0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x1E, 0x0F, 0x16, 0x2D, 0x96, 0x01, 0x00, 0x06, 0x68, 0xEB, 0xE3, 0xCB,
		0x22, 0xC0, 0xFD, 0xF8, 0xF8, 0x00
	};

	// AudioSpecificConfigs: AAC LC at 48 kHz in mono, and at 24 kHz in stereo, the core of an HE-AAC
	// stream whose SBR only the stream itself signals.
	const bytes aac_48k_mono = {0x11, 0x88};
	const bytes aac_24k_stereo = {0x13, 0x10};

	bytes stsd(const std::vector<bytes>& entries)
	{
		builder w;
		w.be32(static_cast<std::uint32_t>(entries.size()));
		for (const auto& e : entries) w.raw(e);
		return full("stsd", 0, 0, w.b);
	}

	using time_runs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

	bytes stts(const time_runs& runs)
	{
		builder w;
		w.be32(static_cast<std::uint32_t>(runs.size()));
		for (const auto& [count, delta] : runs) w.be32(count).be32(delta);
		return full("stts", 0, 0, w.b);
	}

	// The sample sizes, each written out, and the chunk offsets, which a scan never reads.
	bytes stsz(const std::uint32_t count)
	{
		builder w;
		w.be32(0).be32(count);
		for (std::uint32_t i = 0; i < count; ++i) w.be32(100 + i);
		return full("stsz", 0, 0, w.b);
	}

	bytes stco(const std::uint32_t count)
	{
		builder w;
		w.be32(count);
		for (std::uint32_t i = 0; i < count; ++i) w.be32(1000 * i);
		return full("stco", 0, 0, w.b);
	}

	struct track_parts
	{
		std::uint32_t id = 1;
		std::uint32_t flags = 3;
		std::string_view handler = "vide";
		std::string_view handler_name = "VideoHandler";
		std::uint32_t timescale = 1000;
		std::uint64_t duration = 2000;
		std::uint16_t language = und;
		bytes entry;
		time_runs times;
		std::uint32_t samples = 0;
		transform matrix;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		// Boxes the track holds before its media - tref, edts - and after it - udta, meta.
		bytes before;
		bytes after;
		bool quicktime = false;
	};

	bytes trak(const track_parts& p)
	{
		const auto table = box("stbl", join({stsd({p.entry}), stts(p.times), stsz(p.samples), stco(p.samples)}));
		const auto header = p.handler == "soun" ? full("smhd", 0, 0, bytes(4, 0)) : full("vmhd", 0, 1, bytes(8, 0));
		const auto information = box("dinf", full("dref", 0, 0, join({builder().be32(1).b, full("url ", 0, 1, {})})));
		const auto minf = box("minf", join({header, information, table}));
		const auto mdia = box("mdia", join({mdhd(p.timescale, p.duration, p.language), hdlr(p.handler, p.handler_name, p.quicktime), minf}));
		return box("trak", join({tkhd(p.id, p.flags, p.width, p.height, p.matrix), p.before, mdia, p.after}));
	}

	// An H.264 video track of 640 x 480 at 25 frames a second, two seconds long.
	track_parts video_track(const bytes& entry_boxes = {})
	{
		track_parts p;
		p.entry = visual("avc1", 640, 480, join({box("avcC", avc_config), entry_boxes}));
		p.timescale = 12800;
		p.duration = 25600;
		p.times = {{50, 512}};
		p.samples = 50;
		p.width = 640;
		p.height = 480;
		return p;
	}

	// An AAC track of 48 kHz mono, two seconds long.
	track_parts audio_track(const std::uint32_t id = 2)
	{
		track_parts p;
		p.id = id;
		p.handler = "soun";
		p.handler_name = "SoundHandler";
		p.entry = sound("mp4a", 1, 16, 48000, esds(0x40, 96000, aac_48k_mono));
		p.timescale = 48000;
		p.duration = 96000;
		p.language = packed("eng");
		p.times = {{94, 1024}};
		p.samples = 94;
		return p;
	}

	// The ilst items of iTunes metadata, and the data boxes they hold.
	bytes data(const std::uint32_t type, const bytes& value, const std::uint32_t locale = 0)
	{
		return box("data", join({builder().be32(type).be32(locale).b, value}));
	}

	bytes item(const std::string_view key, const std::vector<bytes>& values)
	{
		return box(key, join(values));
	}

	bytes text_item(const std::string_view key, const std::string_view text)
	{
		return item(key, {data(1, to_bytes(text))});
	}

	// iTunes metadata as udta holds it: a full meta box with an 'mdir' handler.
	bytes itunes(const std::vector<bytes>& items)
	{
		return full("meta", 0, 0, join({hdlr("mdir"), box("ilst", join(items))}));
	}

	// QuickTime metadata as the movie holds it: a meta box that is not a full box, an 'mdta' handler,
	// the keys, and items named by their keys' places.
	bytes keyed(const std::vector<std::string_view>& keys, const std::vector<bytes>& values)
	{
		builder k;
		k.be32(static_cast<std::uint32_t>(keys.size()));
		for (const auto key : keys) k.be32(static_cast<std::uint32_t>(key.size() + 8)).raw("mdta").raw(key);

		builder items;
		for (std::size_t i = 0; i < values.size(); ++i) items.be32(static_cast<std::uint32_t>(values[i].size() + 8)).be32(static_cast<std::uint32_t>(i + 1)).raw(values[i]);

		return box("meta", join({hdlr("mdta"), full("keys", 0, 0, k.b), box("ilst", items.b)}));
	}

	// A QuickTime user data text atom: the text's length and language, then the text.
	bytes text_atom(const std::string_view type, const bytes& text, const std::uint16_t language)
	{
		return box(type, join({builder().be16(static_cast<unsigned>(text.size())).be16(language).b, text}));
	}

	// A 3GPP asset box: a full box, a packed language and a NUL-ended string.
	bytes asset(const std::string_view type, const bytes& text, const std::uint16_t language = packed("eng"))
	{
		return full(type, 0, 0, join({builder().be16(language).b, text, bytes{0}}));
	}

	// The Xtra box Windows writes: entries of a name and values, each value its size, a VARTYPE and data.
	struct xtra_value
	{
		std::uint16_t type;
		bytes data;
	};

	bytes xtra(const std::vector<std::pair<std::string_view, std::vector<xtra_value>>>& entries)
	{
		builder w;

		for (const auto& [name, values] : entries)
		{
			builder v;
			for (const auto& value : values) v.be32(static_cast<std::uint32_t>(value.data.size() + 6)).be16(value.type).raw(value.data);
			w.be32(static_cast<std::uint32_t>(12 + name.size() + v.size())).be32(static_cast<std::uint32_t>(name.size())).raw(name);
			w.be32(static_cast<std::uint32_t>(values.size())).raw(v.b);
		}

		return box("Xtra", w.b);
	}

	bytes utf16le(const std::string_view ascii)
	{
		builder w;
		for (const auto c : ascii) w.le16(static_cast<unsigned char>(c));
		w.le16(0);
		return w.b;
	}

	const bytes xmp_uuid_id = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};

	bytes xmp_box(const std::string_view packet)
	{
		return box("uuid", join({xmp_uuid_id, to_bytes(packet)}));
	}

	const bytes media = bytes(64, 0xAB);

	// An ISO file: its file type, the movie and its media, the movie first unless asked otherwise.
	bytes iso_file(const bytes& moov_payload, const bool moov_last = false, const std::string_view brand = "isom",
	               const bytes& trailing = {})
	{
		const auto file_type = ftyp(brand, {brand, "iso2", "mp41"}, 512);
		const auto movie = box("moov", moov_payload);
		const auto data = box("mdat", media);
		return moov_last ? join({file_type, box("free", {}), data, movie, trailing}) : join({file_type, movie, data, trailing});
	}

	// A file built from a head, a run of zeros a read cannot tell from media, and a tail: a large file
	// without the memory, counting the reads a scan makes of it.
	class sparse_stream final : public libfiles::stream
	{
	public:
		sparse_stream(bytes head, const std::uint64_t gap, bytes tail) : _head(std::move(head)), _gap(gap), _tail(std::move(tail))
		{
		}

		std::uint64_t size() override { return _head.size() + _gap + _tail.size(); }

		std::size_t read(const std::uint64_t offset, void* data, const std::size_t size) override
		{
			++reads;
			auto* const out = static_cast<std::uint8_t*>(data);
			const auto total = this->size();
			if (offset >= total) return 0;

			const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(size, total - offset));

			for (std::size_t i = 0; i < n; ++i)
			{
				const auto at = offset + i;
				if (at < _head.size()) out[i] = _head[static_cast<std::size_t>(at)];
				else if (at < _head.size() + _gap) out[i] = 0;
				else out[i] = _tail[static_cast<std::size_t>(at - _head.size() - _gap)];
			}

			return n;
		}

		int reads = 0;

	private:
		bytes _head;
		std::uint64_t _gap;
		bytes _tail;
	};

	libfiles::media_scan_result scan_of(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index)
	{
		libfiles::reader r(file);
		return libfiles::scan_media(r, intent);
	}

	// What a scan that failed may report, what one that succeeded may not, and that its packet is the one
	// read_packet reads.
	void check_consistent(const bytes& file, const libfiles::media_scan_result& s)
	{
		if (!s.success)
		{
			if (s.error.empty() || !s.streams.empty() || !s.tags.empty() || !s.xmp.empty() || !s.pictures.empty() || !s.brand.empty() ||
			    s.duration != 0)
			{
				CHECK(!"a scan that failed reported more than its format");
			}

			if (s.error == "read past the end of the file") CHECK(!"a scan read past the end of the file");
			return;
		}

		for (const auto& p : s.pictures)
		{
			if (p.location.empty() || p.location.offset > file.size() || p.location.size > file.size() - p.location.offset)
			{
				CHECK(!"a picture lies outside the file");
			}
		}

		for (const auto& stream : s.streams)
		{
			if (!std::isfinite(stream.duration) || stream.duration < 0 || !std::isfinite(stream.video.frame_rate)) CHECK(!"a stream's time is not a number");
		}

		if (!std::isfinite(s.duration) || s.duration < 0) CHECK(!"the duration is not a number");

		libfiles::reader r(file);
		const auto packet = libfiles::read_packet(r);
		const auto expected = packet.readable ? packet.packet : std::string();
		if (std::string(s.xmp.begin(), s.xmp.end()) != expected) CHECK(!"the scan's packet is not read_packet's");
	}

	void check_scan(const bytes& file, const bool inspect = true)
	{
		const auto index = scan_of(file);
		check_consistent(file, index);
		if (!inspect) return;

		const auto inspected = scan_of(file, libfiles::scan_intent::inspect);

		if (index.success != inspected.success || index.streams.size() != inspected.streams.size() || index.tags.size() != inspected.tags.size() ||
		    index.xmp != inspected.xmp)
		{
			CHECK(!"an inspection read the file otherwise");
		}
	}

	// Where box headers seem to lie: a printable type after a size that fits what follows.
	std::vector<std::size_t> box_headers(const bytes& file)
	{
		std::vector<std::size_t> found;

		for (std::size_t i = 0; i + 8 <= file.size(); ++i)
		{
			const auto size = libfiles::detail::load_be32(file.data() + i);
			const auto printable = std::all_of(file.begin() + static_cast<std::ptrdiff_t>(i + 4), file.begin() + static_cast<std::ptrdiff_t>(i + 8),
			                                   [](const std::uint8_t c) { return (c >= 0x20 && c < 0x7F) || c == 0xA9; });
			if (printable && size >= 8 && size <= file.size() - i) found.push_back(i);
		}

		return found;
	}

	// A file is cut short near its ends and near every box's start and end, its boxes' sizes are made
	// wrong, and the fields after each box's header - versions, flags and counts - are damaged. A
	// thorough sweep also cuts it at every byte and damages every byte. Each must scan without throwing,
	// and consistently; every eighth cut is inspected too.
	void sweep(const bytes& file, const bool thorough)
	{
		std::set<std::size_t> cuts;
		std::set<std::size_t> damage;
		const auto headers = box_headers(file);

		const auto around = [&file](std::set<std::size_t>& to, const std::size_t at, const std::size_t before, const std::size_t after)
		{
			for (auto i = at > before ? at - before : 0; i <= at + after && i < file.size(); ++i) to.insert(i);
		};

		around(cuts, 0, 0, 64);
		around(cuts, file.size(), 64, 0);

		for (const auto at : headers)
		{
			around(cuts, at, 1, 8);
			around(cuts, at + libfiles::detail::load_be32(file.data() + at), 1, 1);
			around(damage, at + 8, 0, 7);
		}

		if (thorough)
		{
			around(cuts, 0, 0, file.size());
			around(damage, 0, 0, file.size());
		}

		std::size_t n = 0;
		for (const auto cut : cuts) check_scan(bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut)), n++ % 8 == 0);

		for (const auto i : damage)
		{
			if (file[i] == 0xFF) continue;
			auto damaged = file;
			damaged[i] = 0xFF;
			check_scan(damaged, false);
		}

		for (const auto at : headers)
		{
			const auto size = libfiles::detail::load_be32(file.data() + at);

			for (const std::uint32_t wrong : {0u, 7u, 8u, size - 1, size + 1, 0xFFFFFFFFu})
			{
				auto damaged = file;
				builder w;
				w.be32(wrong);
				std::copy(w.b.begin(), w.b.end(), damaged.begin() + static_cast<std::ptrdiff_t>(at));
				check_scan(damaged, false);
			}
		}

		check_scan(file);
	}

	// Every file a test scans is swept once.
	libfiles::media_scan_result scan_bytes(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index,
	                                       const bool thorough = false)
	{
		static std::set<std::string> swept;
		if (swept.emplace(file.begin(), file.end()).second) sweep(file, thorough);
		return scan_of(file, intent);
	}

	const libfiles::media_tag* find_tag(const libfiles::media_scan_result& s, const std::string_view key)
	{
		const auto it = std::ranges::find(s.tags, key, &libfiles::media_tag::key);
		return it == s.tags.end() ? nullptr : &*it;
	}

	std::string tag_value(const libfiles::media_scan_result& s, const std::string_view key)
	{
		const auto* const tag = find_tag(s, key);
		return tag ? tag->value : std::string("(none)");
	}

	std::string tag_language(const libfiles::media_scan_result& s, const std::string_view key)
	{
		const auto* const tag = find_tag(s, key);
		return tag ? tag->language : std::string("(none)");
	}

	std::vector<std::string> tag_values(const libfiles::media_scan_result& s, const std::string_view key)
	{
		std::vector<std::string> values;
		for (const auto& t : s.tags) if (t.key == key) values.push_back(t.value);
		return values;
	}

	bool near(const double a, const double b)
	{
		return std::abs(a - b) < 1e-6;
	}

	// A missing stream fails the test without ending the run.
	const libfiles::media_stream& stream_of(const libfiles::media_scan_result& s, const std::size_t i)
	{
		static const libfiles::media_stream none;
		CHECK(i < s.streams.size());
		return i < s.streams.size() ? s.streams[i] : none;
	}
}

TEST("media mp4 reads an ISO movie's tracks")
{
	auto video = video_track(join({pasp(4, 3), btrt(800000), colr_nclx(1, 1, 1, false)}));
	auto audio = audio_track();
	const auto created = std::uint64_t{3376728829}; // 2011-01-01T12:13:49Z, from 1904
	const auto file = iso_file(join({mvhd(1000, 2040, created), trak(video), trak(audio)}));

	const auto s = scan_bytes(file);
	CHECK(s.success);
	CHECK(s.format == libfiles::media_format::mp4);
	CHECK_STR(s.brand, "isom");
	CHECK(near(s.duration, 2.04));
	CHECK(s.complete);
	CHECK_EQ(s.streams.size(), std::size_t{2});

	const auto& v = stream_of(s, 0);
	CHECK(v.kind == media_stream_kind::video);
	CHECK_EQ(v.id, std::uint64_t{1});
	CHECK_STR(v.codec, "h264");
	CHECK_STR(v.codec_tag, "avc1");
	CHECK_STR(v.language, "");
	CHECK(near(v.duration, 2.0));
	CHECK_EQ(v.bit_rate, std::uint64_t{800000});
	CHECK(v.is_default);
	CHECK_EQ(v.video.width, 640u);
	CHECK_EQ(v.video.height, 480u);
	CHECK_EQ(v.video.pixel_aspect_width, 4u);
	CHECK_EQ(v.video.pixel_aspect_height, 3u);
	CHECK(near(v.video.frame_rate, 25.0));
	CHECK_EQ(v.video.rotation, 0);
	CHECK(!v.video.mirrored);
	CHECK_EQ(v.video.color_primaries, 1);
	CHECK_EQ(v.video.transfer, 1);
	CHECK_EQ(v.video.matrix, 1);
	CHECK(!v.video.full_range);

	const auto& a = stream_of(s, 1);
	CHECK(a.kind == media_stream_kind::audio);
	CHECK_EQ(a.id, std::uint64_t{2});
	CHECK_STR(a.codec, "aac");
	CHECK_STR(a.codec_tag, "mp4a");
	CHECK_STR(a.language, "eng");
	CHECK(near(a.duration, 2.0));
	CHECK_EQ(a.bit_rate, std::uint64_t{96000});
	CHECK_EQ(a.audio.sample_rate, 48000u);
	CHECK_EQ(a.audio.channels, 1u);
	CHECK_EQ(a.audio.bits_per_sample, 0);

	const auto& date = s.properties.created;
	CHECK_EQ(date.year, 2011);
	CHECK_EQ(date.month, 1);
	CHECK_EQ(date.day, 1);
	CHECK_EQ(date.hour, 12);
	CHECK_EQ(date.minute, 13);
	CHECK_EQ(date.second, 49);
	CHECK(date.zone == libfiles::media_date::time_zone::utc);

	// What the codec configurations say, read by the shared codec headers.
	CHECK_STR(v.video.profile, "High");
	CHECK(v.video.chroma == libfiles::chroma_format::yuv420);
	CHECK_EQ(v.video.bit_depth, 8);
	CHECK_STR(a.audio.profile, "LC");
}

TEST("media mp4 reads what a codec's configuration says over what the container says")
{
	// The sample description's size and rate are the container's; the configuration's replace them.
	auto video = video_track();
	video.entry = visual("avc1", 320, 240, box("avcC", avc_config));

	auto audio = audio_track();
	audio.entry = sound("mp4a", 2, 16, 32000, esds(0x40, 0, aac_48k_mono, true));

	const auto s = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(video), trak(audio)})));
	CHECK(s.success);
	CHECK_EQ(stream_of(s, 0).video.width, 640u);
	CHECK_EQ(stream_of(s, 0).video.height, 480u);
	CHECK_EQ(stream_of(s, 0).video.pixel_aspect_width, 4u);
	CHECK_EQ(stream_of(s, 0).video.pixel_aspect_height, 3u);
	CHECK_EQ(stream_of(s, 1).audio.sample_rate, 48000u);
	CHECK_EQ(stream_of(s, 1).audio.channels, 1u);
	CHECK_EQ(stream_of(s, 1).bit_rate, std::uint64_t{0});
}

TEST("media mp4 keeps the sample description's rate for AAC whose SBR only its stream signals")
{
	auto audio = audio_track(1);
	audio.entry = sound("mp4a", 2, 16, 48000, esds(0x40, 64000, aac_24k_stereo));

	const auto s = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(audio)})));
	CHECK(s.success);
	CHECK_EQ(stream_of(s, 0).audio.sample_rate, 48000u);
	CHECK_EQ(stream_of(s, 0).audio.channels, 2u);
	CHECK_EQ(stream_of(s, 0).bit_rate, std::uint64_t{64000});
}

TEST("media mp4 reads QuickTime's sound descriptions")
{
	// Version 1, its configuration in a wave box, as QuickTime writes AAC.
	const auto wave = box("wave", join({box("frma", to_bytes("mp4a")), box("mp4a", bytes(4, 0)), esds(0x40, 48000, aac_48k_mono, true), bytes(8, 0)}));
	auto v1 = audio_track(1);
	v1.entry = sound("mp4a", 1, 16, 48000, wave, 1);
	v1.quicktime = true;
	v1.handler_name = "Apple Sound Media Handler";

	const auto s = scan_bytes(join({ftyp("qt  ", {"qt  "}), box("wide", {}), box("mdat", media), box("moov", join({mvhd(600, 1200), trak(v1)}))}), libfiles::scan_intent::index, true);
	CHECK(s.success);
	CHECK_STR(s.brand, "qt  ");
	CHECK_STR(stream_of(s, 0).codec, "aac");
	CHECK_EQ(stream_of(s, 0).audio.sample_rate, 48000u);
	CHECK_EQ(stream_of(s, 0).audio.channels, 1u);
	CHECK_EQ(stream_of(s, 0).bit_rate, std::uint64_t{48000});
	CHECK_STR(stream_of(s, 0).audio.profile, "LC");

	// Version 2, whose rate is a double and whose channels follow it.
	auto v2 = audio_track(1);
	v2.entry = sound_v2("lpcm", 6, 24, 96000.0, 4 | 8);
	const auto s2 = scan_bytes(join({ftyp("qt  "), box("moov", join({mvhd(600, 1200), trak(v2)})), box("mdat", media)}));
	CHECK(s2.success);
	CHECK_STR(stream_of(s2, 0).codec, "pcm");
	CHECK_STR(stream_of(s2, 0).codec_tag, "lpcm");
	CHECK_EQ(stream_of(s2, 0).audio.sample_rate, 96000u);
	CHECK_EQ(stream_of(s2, 0).audio.channels, 6u);
	CHECK_EQ(stream_of(s2, 0).audio.bits_per_sample, 24);
	CHECK(!stream_of(s2, 0).audio.float_samples);
}

TEST("media mp4 reads PCM in each of its forms")
{
	struct pcm_case
	{
		bytes entry;
		unsigned channels;
		std::uint8_t bits;
		bool floating;
		// Whether the file is swept: those whose descriptions are laid out differently.
		bool sweep = false;
	};

	const auto little = box("wave", join({box("frma", to_bytes("in24")), box("enda", builder().be16(1).b), bytes(4, 0)}));
	const pcm_case cases[] = {
		{sound("twos", 2, 16, 44100), 2, 16, false},
		{sound("twos", 1, 8, 22050), 1, 8, false},
		{sound("sowt", 2, 16, 44100), 2, 16, false},
		{sound("NONE", 2, 16, 44100), 2, 16, false},
		{sound("raw ", 1, 8, 11025), 1, 8, false},
		{sound("in24", 2, 16, 48000, little, 1), 2, 24, false, true},
		{sound("in32", 2, 16, 48000), 2, 32, false},
		{sound("fl32", 2, 16, 48000), 2, 32, true},
		{sound("fl64", 2, 16, 48000), 2, 64, true},
		{sound_v2("lpcm", 2, 32, 48000.0, 1 | 8), 2, 32, true, true},
		{sound("ipcm", 2, 16, 48000, full("pcmC", 0, 0, builder().u8(1).u8(24).b)), 2, 24, false, true},
		{sound("fpcm", 2, 16, 48000, full("pcmC", 0, 0, builder().u8(0).u8(64).b)), 2, 64, true},
	};

	for (const auto& c : cases)
	{
		auto t = audio_track(1);
		t.entry = c.entry;
		const auto file = iso_file(join({mvhd(1000, 2000), trak(t)}));
		const auto s = c.sweep ? scan_bytes(file) : scan_of(file);
		CHECK(s.success);
		if (s.streams.size() != 1) continue;

		const auto& a = stream_of(s, 0);
		CHECK_STR(a.codec, "pcm");
		CHECK_EQ(a.audio.channels, c.channels);
		CHECK_EQ(a.audio.bits_per_sample, c.bits);
		CHECK_EQ(a.audio.float_samples, c.floating);
	}
}

TEST("media mp4 names sound QuickTime keeps by its WAVE format tag as WAVE does")
{
	struct wave_case
	{
		std::string_view type;
		std::uint16_t tag;
	};

	for (const auto& [type, tag] : {wave_case{"ulaw", 7}, wave_case{"alaw", 6}, wave_case{std::string_view("ms\0\x11", 4), 0x11}})
	{
		auto t = audio_track(1);
		t.entry = sound(type, 1, 16, 8000);
		const auto s = scan_of(iso_file(join({mvhd(1000, 2000), trak(t)})));
		CHECK(s.success);
		if (s.streams.size() != 1) continue;

		CHECK(stream_of(s, 0).kind == media_stream_kind::audio);
		CHECK_STR(stream_of(s, 0).codec, std::string(libfiles::detail::codec_from_wave_tag(tag)));
		CHECK_EQ(stream_of(s, 0).audio.sample_rate, 8000u);
	}

	// A track whose handler names no kind is known by its description.
	auto t = audio_track(1);
	t.handler = "????";
	t.entry = sound(std::string_view("ms\0\x55", 4), 2, 16, 44100);
	const auto s = scan_of(iso_file(join({mvhd(1000, 2000), trak(t)})));
	CHECK(s.streams.size() == 1 && stream_of(s, 0).kind == media_stream_kind::audio);
	CHECK_STR(stream_of(s, 0).codec_tag, "0x6D730055");
}

TEST("media mp4 reads the other audio configurations")
{
	// AC-3: 48 kHz, 3/2 with LFE, 640 kbit/s.
	auto ac3 = audio_track(1);
	ac3.entry = sound("ac-3", 2, 16, 48000, box("dac3", {0x10, 0x3E, 0x40}));

	// Opus: two channels, a pre-skip of 312, recorded at 44.1 kHz, played at 48.
	auto opus = audio_track(1);
	opus.entry = sound("Opus", 2, 16, 48000, box("dOps", builder().u8(0).u8(2).be16(312).be32(44100).be16(0).u8(0).b));

	// FLAC: STREAMINFO for 44.1 kHz stereo at 16 bits.
	builder info;
	info.be16(4096).be16(4096).u8(0).be16(0).u8(0).be16(0);
	info.u8(0x0A).u8(0xC4).u8(0x42).u8(0xF0).zeros(4).zeros(16);
	auto flac = audio_track(1);
	flac.entry = sound("fLaC", 2, 16, 44100, full("dfLa", 0, 0, join({builder().u8(0x80).u8(0).u8(0).u8(34).b, info.b})));

	// ALAC: 24 bits, stereo, 96 kHz.
	builder alac_config;
	alac_config.be32(4096).u8(0).u8(24).u8(40).u8(10).u8(14).u8(2).be16(255).be32(0).be32(0).be32(96000);
	auto alac = audio_track(1);
	alac.entry = sound("alac", 2, 24, 96000, full("alac", 0, 0, alac_config.b));

	const auto a = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(ac3)})));
	CHECK_STR(stream_of(a, 0).codec, "ac3");
	CHECK_STR(stream_of(a, 0).codec_tag, "ac-3");
	CHECK_EQ(stream_of(a, 0).audio.sample_rate, 48000u);
	CHECK_EQ(stream_of(a, 0).audio.channels, 6u);
	CHECK_EQ(stream_of(a, 0).bit_rate, std::uint64_t{640000});

	const auto o = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(opus)})));
	CHECK_STR(stream_of(o, 0).codec, "opus");
	CHECK_EQ(stream_of(o, 0).audio.sample_rate, 48000u);
	CHECK_EQ(stream_of(o, 0).audio.channels, 2u);

	const auto f = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(flac)})));
	CHECK_STR(stream_of(f, 0).codec, "flac");
	CHECK_EQ(stream_of(f, 0).audio.sample_rate, 44100u);
	CHECK_EQ(stream_of(f, 0).audio.bits_per_sample, 16);

	const auto l = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(alac)})));
	CHECK_STR(stream_of(l, 0).codec, "alac");
	CHECK_EQ(stream_of(l, 0).audio.sample_rate, 96000u);
	CHECK_EQ(stream_of(l, 0).audio.bits_per_sample, 24);

	// A rate too high for the sample description's 16 bits is in an srat box.
	auto high = audio_track(1);
	high.entry = sound("fLaC", 2, 16, 0, full("srat", 0, 0, builder().be32(192000).b));
	const auto h = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(high)})));
	CHECK_EQ(stream_of(h, 0).audio.sample_rate, 192000u);
}

TEST("media mp4 turns and mirrors a picture as its matrices say")
{
	struct turn
	{
		transform matrix;
		int rotation;
		bool mirrored;
	};

	constexpr std::int32_t one = 0x10000;
	const turn turns[] = {
		{{one, 0, 0, one}, 0, false}, {{0, one, -one, 0}, 90, false}, {{-one, 0, 0, -one}, 180, false}, {{0, -one, one, 0}, 270, false},
		{{-one, 0, 0, one}, 0, true}, {{0, -one, -one, 0}, 90, true}, {{one, 0, 0, -one}, 180, true}, {{0, one, one, 0}, 270, true},
	};

	for (const auto& t : turns)
	{
		auto video = video_track();
		video.matrix = t.matrix;
		const auto s = scan_of(iso_file(join({mvhd(1000, 2000), trak(video)})));
		CHECK(s.success && s.streams.size() == 1);
		if (s.streams.size() != 1) continue;

		CHECK_EQ(stream_of(s, 0).video.rotation, t.rotation);
		CHECK_EQ(stream_of(s, 0).video.mirrored, t.mirrored);
		// The picture is reported before it is turned.
		CHECK_EQ(stream_of(s, 0).video.width, 640u);
	}

	// The movie's matrix applies after the track's: a quarter turn twice is a half turn. A matrix that
	// scales as it turns turns all the same.
	auto video = video_track();
	video.matrix = {0, one, -one, 0};
	const auto twice = scan_bytes(iso_file(join({mvhd(1000, 2000, 0, 0, {0, 2 * one, -2 * one, 0}), trak(video)})));
	CHECK_EQ(stream_of(twice, 0).video.rotation, 180);
	CHECK(!stream_of(twice, 0).video.mirrored);
}

TEST("media mp4 reads ProRes, DV and uncompressed pictures from their sample descriptions")
{
	struct picture_case
	{
		bytes entry;
		std::string_view codec;
		std::string_view pixel_format;
		std::string_view profile;
	};

	const auto palette = builder().be32(0).be16(0x8000).be16(1).zeros(16).b;
	const picture_case cases[] = {
		{visual("apch", 1920, 1080), "prores", "yuv422 10bit", "422 HQ"},
		{visual("apco", 1920, 1080), "prores", "yuv422 10bit", "422 Proxy"},
		{visual("ap4h", 1920, 1080, {}, 32), "prores", "yuva444 12bit", "4444"},
		{visual("ap4x", 1920, 1080, {}, 24), "prores", "yuv444 12bit", "4444 XQ"},
		{visual("2vuy", 720, 486), "rawvideo", "yuv422", ""},
		{visual("v210", 1920, 1080), "v210", "yuv422 10bit", ""},
		{visual("raw ", 320, 240, {}, 24), "rawvideo", "rgb", ""},
		{visual("raw ", 320, 240, {}, 32), "rawvideo", "rgba", ""},
		{visual("raw ", 320, 240, {}, 40), "rawvideo", "gray", ""},
		{visual("raw ", 320, 240, pasp(10, 11), 8, palette), "rawvideo", "", ""},
		{visual("dvc ", 720, 480), "dv", "yuv411", ""},
		{visual("dvcp", 720, 576), "dv", "yuv420", ""},
		{visual("dvh1", 1920, 1080), "dv", "yuv422", ""},
		{visual("hdv2", 1440, 1080), "mpeg2", "yuv420", ""},
		{visual("xd5c", 1920, 1080), "mpeg2", "yuv422", ""},
		{visual("b64a", 1920, 1080), "rawvideo", "rgba 16bit", ""},
	};

	for (const auto& c : cases)
	{
		auto video = video_track();
		video.entry = c.entry;
		const auto file = iso_file(join({mvhd(1000, 2000), trak(video)}));
		// The one with a colour table is laid out differently, and swept.
		const auto s = &c == &cases[9] ? scan_bytes(file) : scan_of(file);
		CHECK(s.success);
		if (s.streams.size() != 1) continue;

		const auto& v = stream_of(s, 0);
		CHECK_STR(v.codec, c.codec);
		CHECK_STR(v.video.pixel_format(), c.pixel_format);
		CHECK_STR(v.video.profile, c.profile);
	}

	// The boxes after a QuickTime colour table are read.
	auto palettized = video_track();
	palettized.entry = cases[9].entry;
	const auto s = scan_of(iso_file(join({mvhd(1000, 2000), trak(palettized)})));
	CHECK_EQ(stream_of(s, 0).video.pixel_aspect_width, 10u);

	// Dolby Vision's 'dvh1' is HEVC, told from DVCPRO HD by its HEVC configuration.
	auto dolby = video_track();
	dolby.entry = visual("dvh1", 1920, 1080, box("hvcC", bytes(23, 0)));
	CHECK_STR(stream_of(scan_of(iso_file(join({mvhd(1000, 2000), trak(dolby)}))), 0).codec, "hevc");
}

TEST("media mp4 reads iTunes metadata")
{
	const auto jpeg = join({bytes{0xFF, 0xD8, 0xFF, 0xE0}, bytes(40, 0x4A)});
	const auto png = join({bytes{0x89, 'P', 'N', 'G'}, bytes(30, 0x50)});

	builder free_form;
	free_form.raw(full("mean", 0, 0, to_bytes("com.apple.iTunes"))).raw(full("name", 0, 0, to_bytes("ISRC"))).raw(data(1, to_bytes("USABC0901234")));

	const std::vector<bytes> items = {
		text_item("\xA9nam", "Song"),
		item("\xA9" "ART", {data(2, builder().be16(0xC4).be16('r').be16('t').be16('i').be16('s').be16('t').b)}),
		text_item("aART", "Band"),
		item("trkn", {data(0, {0, 0, 0, 3, 0, 12, 0, 0})}),
		item("disk", {data(0, {0, 0, 0, 1, 0, 2})}),
		item("gnre", {data(0, {0, 18})}),
		item("tmpo", {data(21, {0, 120})}),
		item("cpil", {data(21, {1})}),
		box("stik", {9}),
		text_item("\xA9" "day", "2009-05-04T10:20:30Z"),
		item("\xA9" "cmt", {data(1, to_bytes("Bonjour"), packed("fra"))}),
		box("----", free_form.b),
		item("covr", {data(13, jpeg), data(14, png)}),
		item("plID", {data(21, {0, 0, 0, 0, 0, 0, 1, 2})}),
		item("zzzz", {data(0, {1, 2, 3})}),
		item("\xA9too", {data(0, to_bytes("Caf\x8E"))}),
		item("\xA9" "alb", {data(1, to_bytes("Album")), data(1, to_bytes("Second"))}),
	};

	const auto file = iso_file(join({mvhd(1000, 2000), trak(audio_track(1)), box("udta", itunes(items))}), false, "M4A ");
	const auto s = scan_bytes(file, libfiles::scan_intent::index, true);
	CHECK(s.success);
	CHECK_STR(s.brand, "M4A ");

	CHECK_STR(tag_value(s, "\xC2\xA9nam"), "Song");
	CHECK_STR(tag_value(s, "\xC2\xA9" "ART"), "\xC3\x84rtist");
	CHECK_STR(tag_value(s, "aART"), "Band");
	CHECK_STR(tag_value(s, "trkn"), "3/12");
	CHECK_STR(tag_value(s, "disk"), "1/2");
	CHECK_STR(tag_value(s, "gnre"), "18");
	CHECK_STR(tag_value(s, "tmpo"), "120");
	CHECK_STR(tag_value(s, "cpil"), "1");
	CHECK_STR(tag_value(s, "stik"), "9");
	CHECK_STR(tag_value(s, "\xC2\xA9" "day"), "2009-05-04T10:20:30Z");
	CHECK_STR(tag_value(s, "\xC2\xA9" "cmt"), "Bonjour");
	CHECK_STR(tag_language(s, "\xC2\xA9" "cmt"), "fra");
	CHECK_STR(tag_language(s, "\xC2\xA9nam"), "");
	CHECK_STR(tag_value(s, "----:com.apple.iTunes:ISRC"), "USABC0901234");
	CHECK_STR(tag_value(s, "plID"), "258");
	CHECK_STR(tag_value(s, "zzzz"), "binary, 3 bytes");
	CHECK_STR(tag_value(s, "\xC2\xA9too"), "Caf\xC3\xA9");
	CHECK(tag_values(s, "\xC2\xA9" "alb") == std::vector<std::string>({"Album", "Second"}));
	CHECK(find_tag(s, "covr") == nullptr);

	// Cover art is located, not read.
	CHECK_EQ(s.pictures.size(), std::size_t{2});

	if (s.pictures.size() == 2)
	{
		const auto at = std::search(file.begin(), file.end(), jpeg.begin(), jpeg.end()) - file.begin();
		CHECK_EQ(s.pictures[0].location.offset, static_cast<std::uint64_t>(at));
		CHECK_EQ(s.pictures[0].location.size, jpeg.size());
		CHECK_STR(s.pictures[0].mime_type, "image/jpeg");
		CHECK(s.pictures[0].data.empty());
		CHECK_EQ(s.pictures[0].type, 3);
		CHECK_STR(s.pictures[1].mime_type, "image/png");
		CHECK_EQ(s.pictures[1].location.size, png.size());
	}

	// What the shared tag mapping makes of them.
	CHECK_STR(s.properties.title, "Song");
	CHECK_STR(s.properties.artist, "\xC3\x84rtist");
	CHECK_STR(s.properties.album_artist, "Band");
	CHECK_EQ(s.properties.track, 3);
	CHECK_EQ(s.properties.track_count, 12);
	CHECK_EQ(s.properties.disc, 1);
	CHECK_EQ(s.properties.disc_count, 2);
	CHECK_EQ(s.properties.created.year, 2009);
}

TEST("media mp4 reads QuickTime user data, its languages and its metadata keys")
{
	const auto udta = box("udta", join({
		text_atom("\xA9nam", to_bytes("Caf\x8E"), 1),
		text_atom("\xA9xyz", to_bytes("+51.5142-000.0985+027.885/"), packed("eng")),
		text_atom("\xA9mak", to_bytes("Apple"), 0),
		text_atom("\xA9" "day", to_bytes("2011-03-13T15:13:49+0000"), 0x7FFF),
		// A text atom too short for its length is its text alone, as 'name' often is.
		box("name", to_bytes("Movie")),
		// A camera's binary atom is not text.
		box("\xA9TSC", {0, 2, 0, 0, 1, 0}),
		box("free", bytes(16, 0)),
		bytes(4, 0),
	}));

	const auto artwork = join({bytes{0xFF, 0xD8, 0xFF}, bytes(20, 0x11)});
	const auto keys = keyed({"com.apple.quicktime.make", "com.apple.quicktime.model", "com.apple.quicktime.creationdate",
	                         "com.apple.quicktime.location.ISO6709", "com.apple.quicktime.rating.user", "com.apple.quicktime.artwork"},
	                        {
		                        data(1, to_bytes("Apple"), packed("eng")),
		                        data(1, to_bytes("iPhone 3GS")),
		                        data(1, to_bytes("2011-03-13T15:13:49+0100")),
		                        data(1, to_bytes("+51.5142-000.0985/")),
		                        data(23, builder().be32(std::bit_cast<std::uint32_t>(0.75f)).b),
		                        data(13, artwork),
	                        });

	auto video = video_track();
	video.quicktime = true;
	const auto file = join({ftyp("qt  ", {"qt  "}), box("wide", {}), box("mdat", media), box("moov", join({mvhd(600, 1200, 3382866829u), trak(video), udta, keys}))});
	const auto s = scan_bytes(file, libfiles::scan_intent::index, true);
	CHECK(s.success);

	CHECK_STR(tag_value(s, "\xC2\xA9nam"), "Caf\xC3\xA9");
	CHECK_STR(tag_language(s, "\xC2\xA9nam"), "fra");
	CHECK_STR(tag_value(s, "\xC2\xA9xyz"), "+51.5142-000.0985+027.885/");
	CHECK_STR(tag_language(s, "\xC2\xA9xyz"), "eng");
	CHECK_STR(tag_value(s, "\xC2\xA9mak"), "Apple");
	CHECK_STR(tag_language(s, "\xC2\xA9mak"), "eng");
	CHECK_STR(tag_language(s, "\xC2\xA9" "day"), "");
	CHECK_STR(tag_value(s, "name"), "Movie");
	CHECK(find_tag(s, "\xC2\xA9TSC") == nullptr);

	CHECK_STR(tag_value(s, "com.apple.quicktime.make"), "Apple");
	CHECK_STR(tag_language(s, "com.apple.quicktime.make"), "eng");
	CHECK_STR(tag_value(s, "com.apple.quicktime.model"), "iPhone 3GS");
	CHECK_STR(tag_value(s, "com.apple.quicktime.creationdate"), "2011-03-13T15:13:49+0100");
	CHECK_STR(tag_value(s, "com.apple.quicktime.rating.user"), "0.75");
	CHECK(find_tag(s, "com.apple.quicktime.artwork") == nullptr);
	CHECK_EQ(s.pictures.size(), std::size_t{1});
	if (!s.pictures.empty()) CHECK_STR(s.pictures[0].mime_type, "image/jpeg");

	// The tags are in file order: the user data's, then the metadata's.
	CHECK(!s.tags.empty() && s.tags.front().key == "\xC2\xA9nam");
	CHECK(!s.tags.empty() && s.tags.back().key == "com.apple.quicktime.rating.user");

	// What the shared tag mapping makes of them: a date in its own zone replaces the movie's.
	CHECK_STR(s.properties.camera_make, "Apple");
	CHECK_STR(s.properties.camera_model, "iPhone 3GS");
	CHECK(s.properties.location.has_value());
	CHECK(s.properties.created.zone == libfiles::media_date::time_zone::offset);
	CHECK_EQ(s.properties.created.offset_minutes, 60);
}

TEST("media mp4 reads 3GPP's asset boxes")
{
	builder place;
	place.be16(packed("eng")).raw("London").u8(0).u8(0);
	place.be32(static_cast<std::uint32_t>(-6455)).be32(3376035).be32(1827471).raw("earth").u8(0).u8(0);

	builder words;
	words.be16(packed("eng")).u8(2).u8(5).raw("beach").u8(4).raw("dogs");

	const auto udta = box("udta", join({
		asset("titl", to_bytes("Title")),
		asset("auth", join({bytes{0xFE, 0xFF}, builder().be16('A').be16('u').be16('t').be16('h').be16(0x00F6).be16('r').b, bytes{0}})),
		asset("perf", to_bytes("Performer")),
		asset("gnre", to_bytes("Jazz")),
		asset("dscp", to_bytes("A description"), packed("fra")),
		asset("cprt", to_bytes("(c) 2009")),
		asset("albm", join({to_bytes("Album"), bytes{0, 7}})),
		full("yrrc", 0, 0, builder().be16(2009).b),
		full("loci", 0, 0, place.b),
		full("kywd", 0, 0, words.b),
	}));

	const auto s = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(audio_track(1)), udta}), false, "3gp6"));
	CHECK(s.success);
	CHECK_STR(s.brand, "3gp6");
	CHECK_STR(tag_value(s, "titl"), "Title");
	CHECK_STR(tag_language(s, "titl"), "eng");
	CHECK_STR(tag_value(s, "auth"), "Auth\xC3\xB6r");
	CHECK_STR(tag_value(s, "perf"), "Performer");
	CHECK_STR(tag_value(s, "gnre"), "Jazz");
	CHECK_STR(tag_value(s, "dscp"), "A description");
	CHECK_STR(tag_language(s, "dscp"), "fra");
	CHECK_STR(tag_value(s, "cprt"), "(c) 2009");
	CHECK_STR(tag_value(s, "albm"), "Album");
	CHECK_STR(tag_value(s, "yrrc"), "2009");
	CHECK_STR(tag_language(s, "yrrc"), "");
	CHECK_STR(tag_value(s, "loci"), "+51.5142-000.0985+027.885/London");
	CHECK(tag_values(s, "kywd") == std::vector<std::string>({"beach", "dogs"}));

	CHECK_STR(s.properties.title, "Title");
	CHECK_STR(s.properties.copyright, "(c) 2009");
	// 'loci' is the place FFmpeg files as "location".
	CHECK(s.properties.location.has_value());
	if (s.properties.location)
	{
		CHECK(std::abs(s.properties.location->latitude - 51.5142) < 0.0001);
		CHECK(std::abs(s.properties.location->longitude + 0.0985) < 0.0001);
	}
}

TEST("media mp4 reads the tags and rating Windows keeps in the Xtra box")
{
	const auto entries = xtra({
		{"WM/SharedUserRating", {{19, builder().le32(75).le32(0).b}}},
		{"WM/Category", {{8, utf16le("beach")}, {8, utf16le("Zurich")}}},
		{"WM/SubTitle", {{31, utf16le("Subtitle")}}},
		{"WM/Signed", {{3, builder().le32(static_cast<std::uint32_t>(-5)).b}}},
		{"WM/MediaClassPrimaryID", {{72, bytes(16, 0x42)}}},
		{"WM/Flag", {{11, {0xFF, 0xFF}}}},
	});

	// An entry that runs past the box ends the entries, keeping those before it.
	const auto damaged = box("Xtra", join({bytes(entries.begin() + 8, entries.end()), builder().be32(100).be32(4).raw("WM/X").be32(1).b}));

	const auto s = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(video_track()), box("udta", damaged)})), libfiles::scan_intent::index, true);
	CHECK(s.success);
	CHECK_STR(tag_value(s, "WM/SharedUserRating"), "75");
	CHECK(tag_values(s, "WM/Category") == std::vector<std::string>({"beach", "Zurich"}));
	CHECK_STR(tag_value(s, "WM/SubTitle"), "Subtitle");
	CHECK_STR(tag_value(s, "WM/Signed"), "-5");
	CHECK_STR(tag_value(s, "WM/MediaClassPrimaryID"), "binary, 16 bytes");
	CHECK_STR(tag_value(s, "WM/Flag"), "1");
	CHECK(find_tag(s, "WM/X") == nullptr);

	CHECK(s.properties.rating.has_value() && *s.properties.rating == 4);
	CHECK(s.properties.keywords == std::vector<std::string>({"beach", "Zurich"}));
}

TEST("media mp4 dates a movie by its header until a tag dates it")
{
	const auto date_of = [](const bytes& header)
	{
		return scan_of(iso_file(join({header, trak(video_track())}))).properties.created;
	};

	// 2011-01-01T12:13:49Z, from 1904, in both versions of the header.
	for (const unsigned version : {0u, 1u})
	{
		const auto d = date_of(mvhd(1000, 2000, 3376728829u, version));
		CHECK_EQ(d.year, 2011);
		CHECK_EQ(d.month, 1);
		CHECK_EQ(d.day, 1);
		CHECK(d.has_time);
		CHECK_EQ(d.hour, 12);
		CHECK_EQ(d.minute, 13);
		CHECK_EQ(d.second, 49);
		CHECK(d.zone == libfiles::media_date::time_zone::utc);
	}

	CHECK(!date_of(mvhd(1000, 2000, 0)).valid());
	CHECK(!date_of(mvhd(1000, 2000, 0x8000000000000000ull, 1)).valid());

	// A time before 1970 counts from 1970, as some writers count and FFmpeg reads it.
	const auto from_1970 = date_of(mvhd(1000, 2000, 1300029229));
	CHECK_EQ(from_1970.year, 2011);
	CHECK_EQ(from_1970.month, 3);
	CHECK_EQ(from_1970.day, 13);
	CHECK_EQ(from_1970.hour, 15);

	// The header's date gives the year, as a tag's date does.
	CHECK_EQ(scan_of(iso_file(join({mvhd(1000, 2000, 3376728829u), trak(video_track())}))).properties.year, 2011);

	// A tag's date replaces the header's, wherever the tag lies.
	const auto after = scan_bytes(iso_file(join({mvhd(1000, 2000, 3376728829u), trak(video_track()), box("udta", itunes({text_item("\xA9" "day", "1999-12-31")}))})));
	CHECK_EQ(after.properties.created.year, 1999);
	CHECK_EQ(after.properties.created.month, 12);
	CHECK(!after.properties.created.has_time);
	CHECK_EQ(after.properties.year, 1999);

	const auto before = scan_of(iso_file(join({box("udta", itunes({text_item("\xA9" "day", "1999-12-31")})), mvhd(1000, 2000, 3376728829u), trak(video_track())})));
	CHECK_EQ(before.properties.created.year, 1999);
	CHECK_EQ(before.properties.year, 1999);
}

TEST("media mp4 reads the packet read_packet reads")
{
	const auto movie = join({mvhd(1000, 2000), trak(video_track())});
	const auto quicktime_movie = [&movie](const std::string_view packet)
	{
		return box("moov", join({movie, box("udta", join({text_atom("\xA9nam", to_bytes("x"), 0), box("XMP_", to_bytes(packet))}))}));
	};

	struct packet_case
	{
		bytes file;
		std::string_view packet;
	};

	const packet_case cases[] = {
		// An ISO file's last XMP uuid box, after the movie or before it.
		{join({ftyp("isom"), box("moov", movie), box("mdat", media), xmp_box("<first/>"), box("free", {}), xmp_box("<last/>")}), "<last/>"},
		{join({ftyp("isom"), xmp_box("<before/>"), box("moov", movie), box("mdat", media)}), "<before/>"},
		// A QuickTime file prefers its movie's, an ISO file its uuid box; each takes the other's alone.
		{join({ftyp("qt  "), quicktime_movie("<movie/>"), box("mdat", media), xmp_box("<uuid/>")}), "<movie/>"},
		{join({ftyp("isom"), quicktime_movie("<movie/>"), box("mdat", media), xmp_box("<uuid/>")}), "<uuid/>"},
		{join({ftyp("isom"), quicktime_movie("<movie/>"), box("mdat", media)}), "<movie/>"},
		{join({ftyp("qt  "), box("moov", movie), xmp_box("<uuid/>")}), "<uuid/>"},
		// A file with no file type box is QuickTime.
		{join({quicktime_movie("<old/>"), box("mdat", media), xmp_box("<uuid/>")}), "<old/>"},
		// A walk that stops at damage after the movie reads no packet, as an update would read none.
		{join({ftyp("isom"), box("moov", movie), xmp_box("<lost/>"), builder().be32(1000).raw("mdat").b}), ""},
	};

	for (std::size_t i = 0; i < std::size(cases); ++i)
	{
		// The layouts that differ most are swept.
		const auto& c = cases[i];
		const auto s = i == 0 || i == 2 || i == 7 ? scan_bytes(c.file) : scan_of(c.file);
		CHECK(s.success);
		CHECK_STR(to_string(s.xmp), c.packet);

		libfiles::reader r(c.file);
		CHECK_STR(libfiles::read_packet(r).packet, c.packet);
	}

	CHECK_STR(scan_of(cases[6].file).brand, "");
}

TEST("media mp4 reads a movie wherever it lies and however its boxes are sized")
{
	const auto payload = join({mvhd(1000, 2000), trak(video_track()), trak(audio_track())});
	const auto moov = box("moov", payload);
	const auto to_end = builder().be32(0).raw("mdat").raw(media).b;

	const bytes files[] = {
		// After the media, with QuickTime's wide box and free space before it.
		join({ftyp("isom"), box("wide", {}), box("free", bytes(10, 0)), box("skip", {}), box("mdat", media), moov}),
		// Before media that runs to the end of the file.
		join({ftyp("isom"), moov, to_end}),
		// After media in a 64-bit box, and in a 64-bit box itself.
		join({ftyp("isom"), large_box("mdat", media), moov}),
		join({ftyp("isom"), large_box("moov", payload), box("mdat", media)}),
		// With fewer bytes after the last box than make a box.
		join({ftyp("isom"), moov, box("mdat", media), bytes(5, 0)}),
		// With no file type, as the oldest QuickTime movies.
		join({box("mdat", media), moov}),
	};

	for (std::size_t i = 0; i < std::size(files); ++i)
	{
		// The layouts that differ most are swept.
		const auto s = i == 0 || i == 1 || i == 3 ? scan_bytes(files[i]) : scan_of(files[i]);
		CHECK(s.success);
		CHECK(near(s.duration, 2.0));
		CHECK_EQ(s.streams.size(), std::size_t{2});
		CHECK(s.complete);
	}

	// A second movie box is not read, as the writer and FFmpeg read none.
	auto other = video_track();
	other.id = 9;
	const auto second = scan_of(join({ftyp("isom"), moov, box("moov", join({mvhd(1000, 5000), trak(other)}))}));
	CHECK(near(second.duration, 2.0));
	CHECK_EQ(second.streams.size(), std::size_t{2});
}

namespace
{
	bytes sidx(const std::uint32_t id, const std::uint32_t timescale, const std::uint32_t earliest, const std::vector<std::uint32_t>& durations)
	{
		builder w;
		w.be32(id).be32(timescale).be32(earliest).be32(0).be16(0).be16(static_cast<unsigned>(durations.size()));
		for (const auto d : durations) w.be32(1000).be32(d).be32(0x90000000);
		return full("sidx", 0, 0, w.b);
	}

	bytes mvex(const bool with_duration)
	{
		const auto trex = full("trex", 0, 0, builder().be32(1).be32(1).be32(512).be32(0).be32(0).b);
		return box("mvex", join({with_duration ? full("mehd", 0, 0, builder().be32(90000).b) : bytes(), trex}));
	}

	const bytes fragment = join({box("moof", full("mfhd", 0, 0, builder().be32(1).b)), box("mdat", media)});
}

TEST("media mp4 times a fragmented movie by mehd, else its segment indexes, else its tracks")
{
	// The movie box holds no samples; its fragments do.
	auto empty = video_track();
	empty.duration = 0;
	empty.times = {};
	empty.samples = 0;

	const auto stated = scan_bytes(join({ftyp("iso6"), box("moov", join({mvhd(1000, 0), trak(empty), mvex(true)})), fragment, fragment}));
	CHECK(stated.success);
	CHECK(near(stated.duration, 90.0));
	CHECK(near(stream_of(stated, 0).duration, 0.0));
	// Without samples to count, the frame rate is the default duration's.
	CHECK(near(stream_of(stated, 0).video.frame_rate, 25.0));

	const auto indexed = scan_bytes(join({ftyp("iso6"), box("moov", join({mvhd(1000, 0), trak(empty), mvex(false)})),
	                                      sidx(1, 12800, 0, {25600, 25600}), fragment, sidx(1, 12800, 51200, {76800}), fragment}));
	CHECK(indexed.success);
	CHECK(near(indexed.duration, 10.0));
	CHECK(near(stream_of(indexed, 0).duration, 10.0));

	// Neither: the movie lasts as long as its longest track says.
	auto stated_track = empty;
	stated_track.duration = 38400;
	const auto tracks = scan_of(join({ftyp("iso6"), box("moov", join({mvhd(1000, 1000), trak(stated_track), mvex(false)})), fragment}));
	CHECK(near(tracks.duration, 3.0));

	// A movie that may have fragments but has none is timed by its header.
	const auto whole = scan_of(join({ftyp("isom"), box("moov", join({mvhd(1000, 3000), trak(video_track()), mvex(true)})), box("mdat", media)}));
	CHECK(near(whole.duration, 3.0));
}

TEST("media mp4 knows chapter tracks, track names and roles")
{
	auto video = video_track();
	video.before = box("tref", box("chap", builder().be32(3).b));

	auto audio = audio_track();
	audio.flags = 2;
	audio.after = box("udta", join({box("name", to_bytes("Stereo")), full("kind", 0, 0, join({lit("urn:mpeg:dash:role:2011\0commentary"), bytes{0}}))}));

	track_parts chapters;
	chapters.id = 3;
	chapters.flags = 0;
	chapters.handler = "text";
	chapters.handler_name = "SubtitleHandler";
	chapters.entry = box("text", bytes(60, 0));
	chapters.language = packed("eng");

	auto subtitles = chapters;
	subtitles.id = 4;
	subtitles.flags = 1;
	subtitles.entry = box("tx3g", bytes(38, 0));
	subtitles.after = box("udta", text_atom("\xA9nam", to_bytes("English"), packed("eng")));

	const auto s = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(video), trak(audio), trak(chapters), trak(subtitles)})));
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), std::size_t{4});
	if (s.streams.size() != 4) return;

	CHECK_STR(stream_of(s, 1).title, "Stereo");
	CHECK(stream_of(s, 1).commentary);
	CHECK(!stream_of(s, 1).is_default);
	CHECK(stream_of(s, 1).tags.size() == 1 && stream_of(s, 1).tags[0].key == "name" && stream_of(s, 1).tags[0].value == "Stereo");

	CHECK(stream_of(s, 2).kind == media_stream_kind::data);
	CHECK_STR(stream_of(s, 2).codec, "mov_text");
	CHECK_STR(stream_of(s, 2).codec_tag, "text");
	CHECK_STR(stream_of(s, 2).language, "eng");
	CHECK(!stream_of(s, 2).is_default);

	CHECK(stream_of(s, 3).kind == media_stream_kind::subtitle);
	CHECK_STR(stream_of(s, 3).codec, "mov_text");
	CHECK_STR(stream_of(s, 3).title, "English");
	CHECK(stream_of(s, 3).is_default);

	// The tracks' tags are their own, not the file's.
	CHECK(s.tags.empty());
	CHECK(s.properties.title.empty());
}

namespace
{
	// A zlib stream of one stored block, as deflate writes what it cannot shrink.
	bytes zlib_stored(const bytes& data)
	{
		std::uint32_t a = 1;
		std::uint32_t b = 0;

		for (const auto c : data)
		{
			a = (a + c) % 65521;
			b = (b + a) % 65521;
		}

		builder w;
		w.u8(0x78).u8(0x01).u8(1).le16(static_cast<unsigned>(data.size())).le16(static_cast<unsigned>(~data.size() & 0xFFFF)).raw(data);
		w.be32(b << 16 | a);
		return w.b;
	}
}

TEST("media mp4 reads a compressed movie")
{
	const auto cover = join({bytes{0xFF, 0xD8, 0xFF}, bytes(20, 0x11)});
	const auto inner = box("moov", join({mvhd(1000, 2000, 3376728829u), trak(video_track()), box("udta", itunes({text_item("\xA9nam", "Packed"), item("covr", {data(13, cover)})}))}));
	const auto compressed = box("cmov", join({box("dcom", to_bytes("zlib")), box("cmvd", join({builder().be32(static_cast<std::uint32_t>(inner.size())).b, zlib_stored(inner)}))}));

	const auto s = scan_bytes(join({ftyp("qt  "), box("moov", compressed), box("mdat", media)}));
	CHECK(s.success);
	CHECK(near(s.duration, 2.0));
	CHECK_EQ(s.streams.size(), std::size_t{1});
	if (!s.streams.empty()) CHECK_STR(stream_of(s, 0).codec, "h264");
	CHECK_STR(tag_value(s, "\xC2\xA9nam"), "Packed");
	CHECK_EQ(s.properties.created.year, 2011);
	// Its cover art lies in no place in the file.
	CHECK(s.pictures.empty());
}

TEST("media mp4 reads the format an encrypted sample description stands for")
{
	const auto scheme = box("sinf", join({box("frma", to_bytes("avc1")), full("schm", 0, 0, builder().raw("cenc").be32(0x10000).b)}));
	auto video = video_track();
	video.entry = visual("encv", 640, 480, join({box("avcC", avc_config), scheme}));

	const auto sound_scheme = box("sinf", box("frma", to_bytes("mp4a")));
	auto audio = audio_track();
	audio.entry = sound("enca", 1, 16, 48000, join({esds(0x40, 96000, aac_48k_mono), sound_scheme}));

	const auto s = scan_bytes(iso_file(join({mvhd(1000, 2000), trak(video), trak(audio)})));
	CHECK(s.success);
	CHECK_STR(stream_of(s, 0).codec, "h264");
	CHECK_STR(stream_of(s, 0).codec_tag, "encv");
	CHECK_STR(stream_of(s, 1).codec, "aac");
	CHECK_STR(stream_of(s, 1).codec_tag, "enca");
}

TEST("media mp4 reads a few blocks however large its media")
{
	const auto moov = box("moov", join({mvhd(1000, 2000), trak(video_track()), trak(audio_track())}));
	constexpr std::uint64_t gap = 6ull * 1024 * 1024 * 1024;
	const auto media_header = [](const std::uint64_t size)
	{
		builder w;
		w.be32(1).raw("mdat");
		return be64(w, size).b;
	};

	// Six gigabytes of media before the movie, and the packet after it.
	sparse_stream after(join({ftyp("isom"), media_header(16 + gap)}), gap, join({moov, xmp_box("<after/>")}));
	libfiles::reader r1(after);
	const auto s1 = libfiles::scan_media(r1);
	CHECK(s1.success);
	CHECK_EQ(s1.streams.size(), std::size_t{2});
	CHECK_STR(to_string(s1.xmp), "<after/>");
	CHECK(after.reads <= 2);

	// The movie first, then the media, then a packet appended at the end.
	sparse_stream before(join({ftyp("isom"), moov, media_header(16 + gap)}), gap, xmp_box("<appended/>"));
	libfiles::reader r2(before);
	const auto s2 = libfiles::scan_media(r2);
	CHECK(s2.success);
	CHECK_EQ(s2.streams.size(), std::size_t{2});
	CHECK_STR(to_string(s2.xmp), "<appended/>");
	CHECK(before.reads <= 2);
}

TEST("media mp4 reads the counts of sample tables but never their entries")
{
	// A long track's sample sizes and chunk offsets: 400 KB each.
	auto video = video_track();
	video.samples = 100000;
	video.times = {{100000, 512}};
	video.duration = 100000ull * 512;

	const auto file = iso_file(join({mvhd(1000, 4000000), trak(video), trak(audio_track()), box("udta", itunes({text_item("\xA9nam", "Long")}))}));
	libfiles::memory_stream source(file);
	libfiles::reader r(source);
	const auto s = libfiles::scan_media(r);
	CHECK(s.success);
	CHECK_EQ(s.streams.size(), std::size_t{2});
	CHECK(near(stream_of(s, 0).video.frame_rate, 25.0));
	CHECK_STR(tag_value(s, "\xC2\xA9nam"), "Long");
	CHECK(r.source_reads() <= 6);
	CHECK(r.source_bytes() < file.size() / 2);
}

TEST("media mp4 describes its boxes and tracks when asked")
{
	const auto file = iso_file(join({mvhd(1000, 2000), trak(video_track()), trak(audio_track())}), true, "isom", xmp_box("<x/>"));
	const auto s = scan_bytes(file, libfiles::scan_intent::inspect);
	CHECK(s.success);

	const auto row = [&s](const std::string_view key) -> const libfiles::structure_row*
	{
		const auto it = std::ranges::find(s.structure, key, &libfiles::structure_row::key);
		return it == s.structure.end() ? nullptr : &*it;
	};

	CHECK(row("Summary (7)") != nullptr);
	CHECK(row("Boxes (5)") != nullptr);
	CHECK(row("Tracks (2)") != nullptr);
	CHECK(row("Major brand") && row("Major brand")->value == "isom");
	CHECK(row("Compatible brands") && row("Compatible brands")->value == "isom, iso2, mp41");
	CHECK(row("moov") && row("moov")->value.starts_with("movie, offset "));
	CHECK(row("uuid") && row("uuid")->value.starts_with("XMP packet, "));
	CHECK(row("Track 1") && row("Track 1")->value.starts_with("vide handler \"VideoHandler\", sample description avc1"));
	CHECK(row("Track 2") && row("Track 2")->value.starts_with("soun handler \"SoundHandler\", sample description mp4a"));

	CHECK(scan_of(file).structure.empty());
}

TEST("media mp4 refuses a file without a movie it can read")
{
	const auto movie = box("moov", join({mvhd(1000, 2000), trak(video_track())}));

	const auto none = scan_of(join({ftyp("isom"), box("mdat", media), box("free", bytes(8, 0))}));
	CHECK(!none.success);
	CHECK(none.format == libfiles::media_format::mp4);
	CHECK_STR(none.error, "the MP4 has no movie box");

	// Media that runs past the end of the file before the movie is met.
	const auto cut = scan_of(join({ftyp("isom"), builder().be32(5000).raw("mdat").b, movie}));
	CHECK(!cut.success);
	CHECK(!cut.error.empty());

	// A movie that runs past the end of the file.
	const auto whole = join({ftyp("isom"), box("mdat", media), movie});
	const auto short_movie = scan_of(bytes(whole.begin(), whole.end() - 1));
	CHECK(!short_movie.success);

	// A movie whose boxes are damaged has no tracks to report, but the file is still a movie.
	const auto damaged = scan_bytes(join({ftyp("isom"), box("moov", bytes(24, 0xFF)), box("mdat", media)}));
	CHECK(damaged.success);
	CHECK(damaged.streams.empty());
	CHECK(!damaged.complete);
}

TEST("media mp4 names languages in ISO 639-2 whatever code the file uses")
{
	struct language_case
	{
		std::uint16_t code;
		std::string_view name;
	};

	const language_case cases[] = {
		{packed("deu"), "deu"}, {2, "deu"}, {0, "eng"}, {11, "jpn"}, {0x7FFF, ""}, {und, ""}, {0x7C00, ""}, {200, ""},
	};

	std::vector<bytes> tracks;

	for (std::size_t i = 0; i < std::size(cases); ++i)
	{
		auto t = audio_track(static_cast<std::uint32_t>(i + 1));
		t.language = cases[i].code;
		tracks.push_back(trak(t));
	}

	const auto s = scan_of(iso_file(join({mvhd(1000, 2000), join(tracks)})));
	CHECK_EQ(s.streams.size(), std::size(cases));

	for (std::size_t i = 0; i < std::min(s.streams.size(), std::size(cases)); ++i)
	{
		CHECK_STR(s.streams[i].language, cases[i].name);
	}
}

TEST("media mp4 reads boxes as the writer does, which refuses one smaller than its 64-bit header")
{
	// The movie's user data holds a box whose 64-bit size, 8, is less than its own sixteen-byte header.
	const auto small = builder().be32(1).raw("XMP_").be32(0).be32(8).b;
	const auto file = join({ftyp("qt  "), box("moov", join({mvhd(1000, 2000), trak(video_track()), box("udta", small)})), box("mdat", media)});

	libfiles::reader r(file);
	CHECK(!libfiles::read_packet(r).readable);

	libfiles::metadata_update update;
	update.legacy.rating = 3;
	CHECK(!libfiles::plan_update(r, update).ok());

	const auto s = scan_bytes(file);
	CHECK(s.success);
	CHECK(s.xmp.empty());
	CHECK_EQ(s.streams.size(), std::size_t{1});
}
