// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the audio codec header parsers, the Windows stream structures and the codec name tables.
// Every header is built field by field, then read whole, cut short at every byte and damaged bit by bit.

#include "test.h"
#include "builders.h"

#include "media_internal.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace
{
	using namespace test_build;
	using libfiles::chroma_format;
	using libfiles::media_stream;
	using libfiles::media_stream_kind;
	using libfiles::detail::audio_header;
	using libfiles::detail::video_header;

	// Bits written most significant first, as codec headers hold them.
	struct msb_bits
	{
		bytes b;
		std::size_t count = 0;

		msb_bits& put(const std::uint64_t value, const unsigned n)
		{
			for (auto i = n; i > 0; --i, ++count)
			{
				if (count % 8 == 0) b.push_back(0);
				if ((value >> (i - 1) & 1) != 0) b.back() = static_cast<std::uint8_t>(b.back() | 0x80u >> count % 8);
			}

			return *this;
		}

		msb_bits& align()
		{
			while (count % 8 != 0) put(0, 1);
			return *this;
		}
	};

	std::uint32_t le_fourcc(const std::string_view s)
	{
		return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8 |
			static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16 | static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24;
	}

	std::string describe(const audio_header& h)
	{
		return std::to_string(h.sample_rate) + " Hz, " + std::to_string(h.channels) + " ch, " + std::to_string(h.bits_per_sample) +
			(h.float_samples ? " float" : " int") + ", '" + h.profile + "', " + std::to_string(h.bit_rate) + " b/s, " +
			std::to_string(h.samples_per_frame) + " samples, " + std::to_string(h.frame_bytes) + " bytes";
	}

	std::string describe(const media_stream& s)
	{
		auto text = "'" + s.codec + "' '" + s.codec_tag + "' ";

		if (s.kind == media_stream_kind::audio)
		{
			text += "audio: " + std::to_string(s.audio.sample_rate) + " Hz, " + std::to_string(s.audio.channels) + " ch, " +
				std::to_string(s.audio.bits_per_sample) + (s.audio.float_samples ? " float, '" : " int, '") + s.audio.profile + "'";
		}
		else if (s.kind == media_stream_kind::video)
		{
			text += "video: " + std::to_string(s.video.width) + "x" + std::to_string(s.video.height) + " " + s.video.pixel_format() +
				(s.video.full_range ? " full" : "");
		}
		else
		{
			text += "data";
		}

		return text;
	}

	// What a parser is handed to fill, so that a refusal can be seen to leave it alone.
	audio_header sentinel_header()
	{
		audio_header h;
		h.sample_rate = 1;
		h.channels = 99;
		h.bits_per_sample = 7;
		h.float_samples = true;
		h.profile = "untouched";
		h.bit_rate = 3;
		h.samples_per_frame = 5;
		h.frame_bytes = 9;
		return h;
	}

	media_stream sentinel_stream()
	{
		media_stream s;
		s.codec = "untouched";
		s.codec_tag = "untouched";
		s.audio.sample_rate = 1;
		s.video.width = 3;
		return s;
	}

	struct outcome
	{
		bool ok = false;
		std::string found;
		// Whether a refusal left every output as it was.
		bool untouched = true;
	};

	using parser = std::function<outcome(const bytes&)>;

	parser audio_parser(bool (*parse)(std::span<const std::uint8_t>, audio_header&))
	{
		return [parse](const bytes& data)
		{
			auto h = sentinel_header();
			outcome o;
			o.ok = parse(data, h);
			o.found = describe(h);
			o.untouched = o.ok || o.found == describe(sentinel_header());
			return o;
		};
	}

	outcome run_ac3_frame(const bytes& data)
	{
		auto h = sentinel_header();
		std::string codec = "untouched";
		outcome o;
		o.ok = libfiles::detail::parse_ac3_frame(data, h, codec);
		o.found = codec + ": " + describe(h);
		o.untouched = o.ok || o.found == "untouched: " + describe(sentinel_header());
		return o;
	}

	outcome run_opus_head(const bytes& data)
	{
		auto h = sentinel_header();
		std::uint32_t pre_skip = 77;
		outcome o;
		o.ok = libfiles::detail::parse_opus_head(data, h, pre_skip);
		o.found = std::to_string(pre_skip) + ": " + describe(h);
		o.untouched = o.ok || o.found == "77: " + describe(sentinel_header());
		return o;
	}

	outcome run_flac_streaminfo(const bytes& data)
	{
		auto h = sentinel_header();
		std::uint64_t samples = 77;
		outcome o;
		o.ok = libfiles::detail::parse_flac_streaminfo(data, h, samples);
		o.found = std::to_string(samples) + ": " + describe(h);
		o.untouched = o.ok || o.found == "77: " + describe(sentinel_header());
		return o;
	}

	outcome run_wave_format(const bytes& data)
	{
		auto s = sentinel_stream();
		std::uint32_t bytes_per_second = 77;
		std::uint16_t align = 77;
		outcome o;
		o.ok = libfiles::detail::read_wave_format(data, s, &bytes_per_second, &align);
		o.found = std::to_string(bytes_per_second) + " " + std::to_string(align) + ": " + describe(s);
		o.untouched = o.ok || o.found == "77 77: " + describe(sentinel_stream());
		return o;
	}

	outcome run_bitmap_info(const bytes& data)
	{
		auto s = sentinel_stream();
		outcome o;
		o.ok = libfiles::detail::read_bitmap_info(data, s);
		o.found = describe(s);
		o.untouched = o.ok || o.found == describe(sentinel_stream());
		return o;
	}

	// Every prefix of data, each copied to a buffer of its own so that a read past it is caught: refused
	// while shorter than shortest, and read from there on - as the whole is, when stable. A refusal must
	// leave the outputs as they were.
	void check_prefixes(const parser& parse, const bytes& data, const std::size_t shortest, const bool stable = true)
	{
		const auto whole = parse(data);
		CHECK(whole.ok);
		auto wrong = 0;

		for (std::size_t n = 0; n < data.size(); ++n)
		{
			const auto o = parse(bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n)));
			if (!o.untouched || o.ok != (n >= shortest) || (o.ok && stable && o.found != whole.found)) ++wrong;
		}

		CHECK_EQ(wrong, 0);
	}

	// Every bit of 64 bytes from the start of data, or from at, flipped, and each of those bytes cleared and
	// set: whatever the parser makes of it, it reads within the data and a refusal leaves the outputs alone.
	void check_damage(const parser& parse, const bytes& data, const std::size_t from = 0)
	{
		auto wrong = 0;
		const auto reach = std::min<std::size_t>(data.size(), from + 64);

		for (auto bit = from * 8; bit < reach * 8; ++bit)
		{
			auto damaged = data;
			damaged[bit / 8] = static_cast<std::uint8_t>(damaged[bit / 8] ^ 0x80u >> bit % 8);
			if (!parse(damaged).untouched) ++wrong;
		}

		for (auto at = from; at < reach; ++at)
		{
			for (const std::uint8_t value : {std::uint8_t{0x00}, std::uint8_t{0xFF}})
			{
				auto damaged = data;
				damaged[at] = value;
				if (!parse(damaged).untouched) ++wrong;
			}
		}

		CHECK_EQ(wrong, 0);
	}

	// ---- AAC ----------------------------------------------------------------------------------------------

	void put_object_type(msb_bits& w, const unsigned type)
	{
		if (type >= 31)
		{
			w.put(31, 5).put(type - 32, 6);
		}
		else
		{
			w.put(type, 5);
		}
	}

	// An AudioSpecificConfig of a General Audio type that is not error resilient: its common fields, then
	// a GASpecificConfig with no core coder and no extension.
	bytes ga_config(const unsigned type, const unsigned rate_index, const unsigned channels, const bool short_frames = false)
	{
		msb_bits w;
		put_object_type(w, type);
		w.put(rate_index, 4).put(channels, 4).put(short_frames ? 1 : 0, 1).put(0, 1).put(0, 1);
		return w.b;
	}

	// AAC LC at 24 kHz, followed by the backward-compatible extension that signals SBR, and PS with it.
	bytes compatible_he_aac(const unsigned channels, const bool sbr_present, const bool ps)
	{
		msb_bits w;
		w.put(2, 5).put(6, 4).put(channels, 4).put(0, 3);
		w.put(0x2B7, 11).put(5, 5).put(sbr_present ? 1 : 0, 1);
		if (sbr_present) w.put(3, 4);
		if (ps) w.put(0x548, 11).put(1, 1);
		return w.align().b;
	}

	// A program_config_element: a single and a pair at the front, a pair at the side, an LFE, a coupling
	// channel and a comment - six channels output.
	void put_program_config(msb_bits& w)
	{
		// Element tag, object type and rate; then the front, side, back, LFE, data and coupling element counts.
		w.put(0, 4).put(1, 2).put(3, 4);
		w.put(2, 4).put(1, 4).put(0, 4).put(1, 2).put(0, 3).put(1, 4);
		// No mono mixdown; a stereo mixdown, element 1; a matrix mixdown, index 2 with pseudo surround.
		w.put(0, 1).put(1, 1).put(1, 4).put(1, 1).put(2, 2).put(1, 1);
		// The elements, each a pair flag and a tag, in that order; the LFE's tag; the coupling channel.
		w.put(0, 1).put(0, 4).put(1, 1).put(1, 4);
		w.put(1, 1).put(2, 4);
		w.put(0, 4);
		w.put(0, 1).put(0, 4);
		w.align();
		w.put(3, 8).put('a', 8).put('b', 8).put('c', 8);
	}

	audio_header read_aac(const bytes& config, bool& ok)
	{
		audio_header h;
		ok = libfiles::detail::parse_aac_config(config, h);
		return h;
	}
}

TEST("media audio AAC configs give the object type, rate, channels and frame length")
{
	using libfiles::detail::parse_aac_config;

	// LC at 44.1 kHz in stereo, as most MP4 files hold it.
	audio_header lc;
	CHECK(parse_aac_config(bytes{0x12, 0x10}, lc));
	CHECK_EQ(lc.sample_rate, 44100u);
	CHECK_EQ(lc.channels, 2u);
	CHECK_STR(lc.profile, "LC");
	CHECK_EQ(lc.samples_per_frame, 1024u);
	CHECK_EQ(lc.bits_per_sample, 0);
	CHECK(!lc.float_samples);
	CHECK_EQ(lc.bit_rate, 0u);
	CHECK_EQ(lc.frame_bytes, 0u);

	struct named
	{
		unsigned type;
		const char* profile;
	};

	for (const auto& [type, profile] : {named{1, "Main"}, named{2, "LC"}, named{3, "SSR"}, named{4, "LTP"}})
	{
		bool ok = false;
		const auto h = read_aac(ga_config(type, 3, 1, true), ok);
		CHECK(ok);
		CHECK_STR(h.profile, profile);
		CHECK_EQ(h.sample_rate, 48000u);
		CHECK_EQ(h.channels, 1u);
		CHECK_EQ(h.samples_per_frame, 960u);
	}

	constexpr std::uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

	for (unsigned index = 0; index < 13; ++index)
	{
		bool ok = false;
		CHECK_EQ(read_aac(ga_config(2, index, 2), ok).sample_rate, rates[index]);
		CHECK(ok);
	}

	// The 24-bit frequency index 15 escapes to.
	bool ok = false;
	CHECK_EQ(read_aac(msb_bits().put(2, 5).put(15, 4).put(50000, 24).put(2, 4).put(0, 3).align().b, ok).sample_rate, 50000u);
	CHECK(ok);

	constexpr std::uint32_t channels[16] = {0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 24, 8, 0};

	for (unsigned configuration = 1; configuration < 16; ++configuration)
	{
		const auto h = read_aac(ga_config(2, 4, configuration), ok);
		CHECK_EQ(ok, channels[configuration] != 0);
		if (ok) CHECK_EQ(h.channels, channels[configuration]);
	}

	// Reserved rates, an escaped rate of 0, and reserved object types.
	audio_header untouched = sentinel_header();
	CHECK(!parse_aac_config(ga_config(2, 13, 2), untouched));
	CHECK(!parse_aac_config(ga_config(2, 14, 2), untouched));
	CHECK(!parse_aac_config(msb_bits().put(2, 5).put(15, 4).put(0, 24).put(2, 4).put(0, 3).align().b, untouched));

	for (const unsigned type : {0u, 10u, 11u, 18u, 47u, 95u})
	{
		CHECK(!parse_aac_config(ga_config(type, 4, 2), untouched));
	}

	CHECK(!parse_aac_config(bytes{}, untouched));
	CHECK(!parse_aac_config(bytes{0x12}, untouched));
	CHECK_STR(describe(untouched), describe(sentinel_header()));

	// Types without a profile name still give the rate and channels: ALS, escaped as 32 + 4.
	const auto als = read_aac(msb_bits().put(31, 5).put(4, 6).put(3, 4).put(2, 4).align().b, ok);
	CHECK(ok);
	CHECK_EQ(als.sample_rate, 48000u);
	CHECK_EQ(als.channels, 2u);
	CHECK_STR(als.profile, "");
	CHECK_EQ(als.samples_per_frame, 0u);
}

TEST("media audio AAC configs read the low-delay and USAC types")
{
	using libfiles::detail::parse_aac_config;

	// ER AAC LD: a GASpecificConfig whose extension carries the resilience flags, then epConfig.
	const auto ld_bits = [](const unsigned ep_config)
	{
		return msb_bits().put(23, 5).put(3, 4).put(2, 4).put(1, 1).put(0, 1).put(1, 1).put(0, 3).put(0, 1).put(ep_config, 2).align().b;
	};

	bool ok = false;
	const auto ld = read_aac(ld_bits(0), ok);
	CHECK(ok);
	CHECK_STR(ld.profile, "LD");
	CHECK_EQ(ld.sample_rate, 48000u);
	CHECK_EQ(ld.channels, 2u);
	CHECK_EQ(ld.samples_per_frame, 480u);
	CHECK(read_aac(ld_bits(2), ok).channels == 2 && ok);

	// The epConfig must be there.
	check_prefixes(audio_parser(parse_aac_config), ld_bits(0), 3);
	audio_header untouched;

	// ER AAC ELD, escaped as 32 + 7; low-delay SBR leaves the frame length unknown.
	const auto eld = read_aac(msb_bits().put(31, 5).put(7, 6).put(3, 4).put(1, 4).put(0, 1).put(0, 3).put(0, 1).align().b, ok);
	CHECK(ok);
	CHECK_STR(eld.profile, "ELD");
	CHECK_EQ(eld.sample_rate, 48000u);
	CHECK_EQ(eld.channels, 1u);
	CHECK_EQ(eld.samples_per_frame, 512u);
	CHECK_EQ(read_aac(msb_bits().put(31, 5).put(7, 6).put(3, 4).put(1, 4).put(1, 1).put(0, 3).put(1, 1).align().b, ok).samples_per_frame, 0u);
	CHECK(ok);

	// USAC, escaped as 32 + 10: the UsacConfig's own rate, frame length and channels.
	const auto usac = [](const unsigned rate_index, const std::uint32_t explicit_rate, const unsigned core_sbr, const unsigned configuration,
	                     const unsigned outputs)
	{
		msb_bits w;
		w.put(31, 5).put(10, 6).put(3, 4).put(2, 4);
		w.put(rate_index, 5);
		if (rate_index == 31) w.put(explicit_rate, 24);
		w.put(core_sbr, 3).put(configuration, 5);
		if (configuration == 0) w.put(outputs, 5);
		return w.put(0, 8).align().b;
	};

	const auto xhe = read_aac(usac(3, 0, 3, 2, 0), ok);
	CHECK(ok);
	CHECK_STR(xhe.profile, "xHE-AAC");
	CHECK_EQ(xhe.sample_rate, 48000u);
	CHECK_EQ(xhe.channels, 2u);
	CHECK_EQ(xhe.samples_per_frame, 2048u);

	const auto odd_rate = read_aac(usac(15, 0, 1, 0, 6), ok);
	CHECK(ok);
	CHECK_EQ(odd_rate.sample_rate, 57600u);
	CHECK_EQ(odd_rate.channels, 6u);
	CHECK_EQ(odd_rate.samples_per_frame, 1024u);

	const auto escaped = read_aac(usac(31, 44100, 4, 13, 0), ok);
	CHECK(ok);
	CHECK_EQ(escaped.sample_rate, 44100u);
	CHECK_EQ(escaped.channels, 24u);
	CHECK_EQ(escaped.samples_per_frame, 4096u);

	CHECK_EQ(read_aac(usac(3, 0, 0, 1, 0), ok).samples_per_frame, 768u);
	CHECK(ok);

	// numOutChannels escapes to a further byte when its five bits are full.
	CHECK_EQ(read_aac(msb_bits().put(31, 5).put(10, 6).put(3, 4).put(0, 4).put(3, 5).put(1, 3).put(0, 5).put(31, 5).put(2, 8).align().b, ok).channels, 33u);
	CHECK(ok);

	// Reserved frame lengths and rates.
	CHECK(!parse_aac_config(usac(3, 0, 5, 2, 0), untouched));
	CHECK(!parse_aac_config(usac(13, 0, 1, 2, 0), untouched));
	CHECK(!parse_aac_config(usac(31, 0, 1, 2, 0), untouched));
}

TEST("media audio AAC configs signal SBR and PS explicitly and backward-compatibly")
{
	using libfiles::detail::parse_aac_config;

	// Explicit HE-AAC: object type 5, the 48 kHz SBR rate, then LC at 24 kHz.
	bool ok = false;
	const auto he = read_aac(bytes{0x2B, 0x11, 0x88, 0x00}, ok);
	CHECK(ok);
	CHECK_STR(he.profile, "HE-AAC");
	CHECK_EQ(he.sample_rate, 48000u);
	CHECK_EQ(he.channels, 2u);
	CHECK_EQ(he.samples_per_frame, 2048u);

	// Explicit HE-AACv2: PS makes stereo of the mono core.
	const auto v2 = read_aac(bytes{0xEB, 0x09, 0x88, 0x00}, ok);
	CHECK(ok);
	CHECK_STR(v2.profile, "HE-AACv2");
	CHECK_EQ(v2.sample_rate, 48000u);
	CHECK_EQ(v2.channels, 2u);

	// PS signalled over a stereo core does not apply.
	const auto stereo_ps = read_aac(msb_bits().put(29, 5).put(6, 4).put(2, 4).put(3, 4).put(2, 5).put(0, 3).align().b, ok);
	CHECK(ok);
	CHECK_STR(stereo_ps.profile, "HE-AAC");
	CHECK_EQ(stereo_ps.channels, 2u);

	// Downsampled SBR: the output rate is the core's, and so is the frame.
	const auto downsampled = read_aac(msb_bits().put(5, 5).put(3, 4).put(2, 4).put(3, 4).put(2, 5).put(0, 3).align().b, ok);
	CHECK(ok);
	CHECK_STR(downsampled.profile, "HE-AAC");
	CHECK_EQ(downsampled.sample_rate, 48000u);
	CHECK_EQ(downsampled.samples_per_frame, 1024u);

	// Implicit SBR cannot be told from the config: the core's rate stands.
	const auto implicit = read_aac(ga_config(2, 6, 2), ok);
	CHECK(ok);
	CHECK_STR(implicit.profile, "LC");
	CHECK_EQ(implicit.sample_rate, 24000u);

	// The backward-compatible extension after a plain LC config.
	const auto compatible = read_aac(compatible_he_aac(2, true, false), ok);
	CHECK(ok);
	CHECK_STR(compatible.profile, "HE-AAC");
	CHECK_EQ(compatible.sample_rate, 48000u);
	CHECK_EQ(compatible.channels, 2u);
	CHECK_EQ(compatible.samples_per_frame, 2048u);

	const auto compatible_ps = read_aac(compatible_he_aac(1, true, true), ok);
	CHECK(ok);
	CHECK_STR(compatible_ps.profile, "HE-AACv2");
	CHECK_EQ(compatible_ps.sample_rate, 48000u);
	CHECK_EQ(compatible_ps.channels, 2u);

	// sbrPresentFlag 0 says there is none.
	const auto absent = read_aac(compatible_he_aac(2, false, false), ok);
	CHECK(ok);
	CHECK_STR(absent.profile, "LC");
	CHECK_EQ(absent.sample_rate, 24000u);

	// Bytes after the config that are not the extension's sync word change nothing.
	auto trailing = ga_config(2, 6, 2);
	trailing.insert(trailing.end(), {0x12, 0x34, 0x56});
	CHECK_EQ(read_aac(trailing, ok).sample_rate, 24000u);
	CHECK(ok);

	// Explicit signalling must name a core, at a known rate.
	audio_header untouched;
	CHECK(!parse_aac_config(msb_bits().put(5, 5).put(6, 4).put(2, 4).put(13, 4).put(2, 5).put(0, 3).align().b, untouched));
	CHECK(!parse_aac_config(msb_bits().put(5, 5).put(6, 4).put(2, 4).put(3, 4).put(5, 5).put(0, 3).align().b, untouched));

	// Cut short, the extension is read whole or not at all: the core's figures until it is all there.
	const auto config = compatible_he_aac(1, true, true);
	CHECK(config.size() == 7);

	for (std::size_t n = 0; n <= config.size(); ++n)
	{
		const auto h = read_aac(bytes(config.begin(), config.begin() + static_cast<std::ptrdiff_t>(n)), ok);
		CHECK_EQ(ok, n >= 2);
		if (!ok) continue;

		if (n < 5) CHECK_STR(describe(h), "24000 Hz, 1 ch, 0 int, 'LC', 0 b/s, 1024 samples, 0 bytes");
		else if (n < 7) CHECK_STR(describe(h), "48000 Hz, 1 ch, 0 int, 'HE-AAC', 0 b/s, 2048 samples, 0 bytes");
		else CHECK_STR(describe(h), "48000 Hz, 2 ch, 0 int, 'HE-AACv2', 0 b/s, 2048 samples, 0 bytes");
	}

	check_damage(audio_parser(parse_aac_config), config);
	check_prefixes(audio_parser(parse_aac_config), bytes{0x2B, 0x11, 0x88, 0x00}, 4);
	check_damage(audio_parser(parse_aac_config), bytes{0xEB, 0x09, 0x88, 0x00});
}

TEST("media audio AAC configs count a program config element's channels")
{
	using libfiles::detail::parse_aac_config;

	msb_bits w;
	w.put(2, 5).put(3, 4).put(0, 4).put(0, 3);
	put_program_config(w);
	const auto config = w.align().b;

	bool ok = false;
	const auto h = read_aac(config, ok);
	CHECK(ok);
	CHECK_EQ(h.channels, 6u);
	CHECK_EQ(h.sample_rate, 48000u);
	CHECK_STR(h.profile, "LC");

	// A program with no channels is no program.
	msb_bits empty;
	empty.put(2, 5).put(3, 4).put(0, 4).put(0, 3).put(0, 4).put(1, 2).put(3, 4).put(0, 23).align().put(0, 8);
	audio_header untouched;
	CHECK(!parse_aac_config(empty.b, untouched));

	// The comment is part of the element: cut into, the config is refused.
	check_prefixes(audio_parser(parse_aac_config), config, config.size());
	check_damage(audio_parser(parse_aac_config), config);

	// PS over a program of one channel makes it two.
	msb_bits mono;
	mono.put(29, 5).put(6, 4).put(0, 4).put(3, 4).put(2, 5).put(0, 3);
	mono.put(0, 4).put(1, 2).put(6, 4).put(1, 4).put(0, 4).put(0, 4).put(0, 2).put(0, 3).put(0, 4).put(0, 3);
	mono.put(0, 1).put(0, 4);
	mono.align().put(0, 8);
	const auto parametric = read_aac(mono.b, ok);
	CHECK(ok);
	CHECK_EQ(parametric.channels, 2u);
	CHECK_STR(parametric.profile, "HE-AACv2");
}

namespace
{
	// An ADTS header; with a CRC, the positions of the raw data blocks after the first and the CRC follow it.
	bytes adts(const unsigned profile, const unsigned rate_index, const unsigned channels, const unsigned frame_bytes, const bool crc = false,
	           const unsigned blocks = 1, const unsigned id = 0, const unsigned layer = 0)
	{
		msb_bits w;
		w.put(0xFFF, 12).put(id, 1).put(layer, 2).put(crc ? 0 : 1, 1).put(profile, 2).put(rate_index, 4).put(0, 1).put(channels, 3);
		w.put(0, 4).put(frame_bytes, 13).put(0x7FF, 11).put(blocks - 1, 2);
		for (unsigned i = 0; crc && i < blocks; ++i) w.put(0xABCD, 16);
		return w.b;
	}
}

TEST("media audio ADTS headers give the profile, rate, channels and frame")
{
	using libfiles::detail::parse_adts_header;

	audio_header h;
	CHECK(parse_adts_header(adts(1, 3, 2, 400), h));
	CHECK_STR(describe(h), "48000 Hz, 2 ch, 0 int, 'LC', 150000 b/s, 1024 samples, 400 bytes");

	// MPEG-2's ID, a CRC and four raw data blocks.
	CHECK(parse_adts_header(adts(0, 4, 1, 1600, true, 4, 1), h));
	CHECK_STR(describe(h), "44100 Hz, 1 ch, 0 int, 'Main', 137812 b/s, 4096 samples, 1600 bytes");

	CHECK(parse_adts_header(adts(2, 3, 2, 400), h));
	CHECK_STR(h.profile, "SSR");
	CHECK(parse_adts_header(adts(3, 3, 2, 400), h));
	CHECK_STR(h.profile, "LTP");

	constexpr std::uint32_t channels[8] = {0, 1, 2, 3, 4, 5, 6, 8};

	for (unsigned configuration = 1; configuration < 8; ++configuration)
	{
		CHECK(parse_adts_header(adts(1, 3, configuration, 400), h));
		CHECK_EQ(h.channels, channels[configuration]);
	}

	constexpr std::uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

	for (unsigned index = 0; index < 13; ++index)
	{
		CHECK(parse_adts_header(adts(1, index, 2, 400), h));
		CHECK_EQ(h.sample_rate, rates[index]);
	}

	// Reserved rates, a layer other than 0, a frame shorter than its header, and no sync word.
	auto untouched = sentinel_header();
	CHECK(!parse_adts_header(adts(1, 13, 2, 400), untouched));
	CHECK(!parse_adts_header(adts(1, 15, 2, 400), untouched));
	CHECK(!parse_adts_header(adts(1, 3, 2, 400, false, 1, 0, 1), untouched));
	CHECK(!parse_adts_header(adts(1, 3, 2, 6), untouched));
	CHECK(!parse_adts_header(adts(1, 3, 2, 8, true), untouched));
	CHECK(parse_adts_header(adts(1, 3, 2, 9, true), h));
	auto unsynced = adts(1, 3, 2, 400);
	unsynced[1] = 0xE1;
	CHECK(!parse_adts_header(unsynced, untouched));
	CHECK_STR(describe(untouched), describe(sentinel_header()));

	check_prefixes(audio_parser(parse_adts_header), adts(1, 3, 2, 400), 7);
	check_prefixes(audio_parser(parse_adts_header), adts(0, 4, 1, 1600, true, 4, 1), 7);
	check_damage(audio_parser(parse_adts_header), adts(1, 3, 2, 400));
}

TEST("media audio ADTS headers count the channels of a program config element")
{
	using libfiles::detail::parse_adts_header;

	// Without a channel configuration, the raw data block opens with the element: ID_PCE, then the element.
	auto frame = adts(1, 3, 0, 200);
	msb_bits block;
	block.put(5, 3);
	put_program_config(block);
	frame.insert(frame.end(), block.b.begin(), block.b.end());

	audio_header h;
	CHECK(parse_adts_header(frame, h));
	CHECK_EQ(h.channels, 6u);
	CHECK_EQ(h.sample_rate, 48000u);

	// Cut into, or after a CRC that is not there, the element leaves the channels unknown.
	for (std::size_t n = 7; n < frame.size(); ++n)
	{
		CHECK(parse_adts_header(bytes(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(n)), h));
		CHECK_EQ(h.channels, 0u);
	}

	// A raw data block that opens with a single channel element.
	auto single = adts(1, 3, 0, 200);
	single.insert(single.end(), {0x00, 0x00, 0x00});
	CHECK(parse_adts_header(single, h));
	CHECK_EQ(h.channels, 0u);

	// After a CRC.
	auto protected_frame = adts(1, 3, 0, 200, true);
	protected_frame.insert(protected_frame.end(), block.b.begin(), block.b.end());
	CHECK(parse_adts_header(protected_frame, h));
	CHECK_EQ(h.channels, 6u);

	check_prefixes(audio_parser(parse_adts_header), frame, 7, false);
	check_damage(audio_parser(parse_adts_header), frame);
}

TEST("media audio MPEG audio headers name the version and layer")
{
	using libfiles::detail::parse_mpeg_audio_header;

	struct expected
	{
		std::uint32_t header;
		const char* codec;
		const char* found;
	};

	const expected frames[] = {
		{0xFFFB9064, "mp3", "44100 Hz, 2 ch, 0 int, 'MPEG-1 Layer 3', 128000 b/s, 1152 samples, 417 bytes"},
		{0xFFFB9264, "mp3", "44100 Hz, 2 ch, 0 int, 'MPEG-1 Layer 3', 128000 b/s, 1152 samples, 418 bytes"},
		{0xFFF380C0, "mp3", "22050 Hz, 1 ch, 0 int, 'MPEG-2 Layer 3', 64000 b/s, 576 samples, 208 bytes"},
		{0xFFE31800, "mp3", "8000 Hz, 2 ch, 0 int, 'MPEG-2.5 Layer 3', 8000 b/s, 576 samples, 72 bytes"},
		{0xFFFDA400, "mp2", "48000 Hz, 2 ch, 0 int, 'MPEG-1 Layer 2', 192000 b/s, 1152 samples, 576 bytes"},
		{0xFFF58440, "mp2", "24000 Hz, 2 ch, 0 int, 'MPEG-2 Layer 2', 64000 b/s, 1152 samples, 384 bytes"},
		{0xFFFF1AC0, "mp1", "32000 Hz, 1 ch, 0 int, 'MPEG-1 Layer 1', 32000 b/s, 384 samples, 52 bytes"},
		// Free format: the frame's size is not in its header.
		{0xFFFB0064, "mp3", "44100 Hz, 2 ch, 0 int, 'MPEG-1 Layer 3', 0 b/s, 1152 samples, 0 bytes"},
	};

	for (const auto& [header, codec, found] : frames)
	{
		audio_header h;
		std::string name;
		CHECK(parse_mpeg_audio_header(header, h, name));
		CHECK_STR(name, codec);
		CHECK_STR(describe(h), found);
	}

	// Every version, layer, bit rate, rate and padding: the reserved and forbidden values are refused,
	// and the rest read, a frame size with every bit rate but free format's.
	auto wrong = 0;

	for (std::uint32_t fields = 0; fields < 2048; ++fields)
	{
		const auto version = fields >> 9 & 3;
		const auto layer = fields >> 7 & 3;
		const auto bit_rate = fields >> 3 & 15;
		const auto rate = fields >> 1 & 3;
		const auto header = 0xFFE10000u | version << 19 | layer << 17 | bit_rate << 12 | rate << 10 | (fields & 1) << 9;
		const auto valid = version != 1 && layer != 0 && bit_rate != 15 && rate != 3;

		auto h = sentinel_header();
		std::string codec = "untouched";
		const auto ok = parse_mpeg_audio_header(header, h, codec);
		if (ok != valid) ++wrong;
		if (!ok && (codec != "untouched" || describe(h) != describe(sentinel_header()))) ++wrong;
		if (ok && (h.frame_bytes != 0) != (bit_rate != 0)) ++wrong;
		if (ok && (h.channels != 2 || h.sample_rate == 0 || h.bits_per_sample != 0)) ++wrong;
	}

	CHECK_EQ(wrong, 0);

	audio_header h;
	std::string codec;
	CHECK(!parse_mpeg_audio_header(0xFFDB9064, h, codec));
	CHECK(!parse_mpeg_audio_header(0x00000000, h, codec));
	CHECK(!parse_mpeg_audio_header(0xFFEB9064, h, codec));
	CHECK(!parse_mpeg_audio_header(0xFFF99064, h, codec));
	CHECK(!parse_mpeg_audio_header(0xFFFBF064, h, codec));
	CHECK(!parse_mpeg_audio_header(0xFFFB9C64, h, codec));
	CHECK(codec.empty());
}

namespace
{
	// An AC-3 sync frame's header, padded to size.
	bytes ac3_frame(const unsigned fscod, const unsigned frmsizecod, const unsigned bsid, const unsigned acmod, const bool lfe,
	                const std::size_t size = 16)
	{
		msb_bits w;
		w.put(0x0B77, 16).put(0, 16).put(fscod, 2).put(frmsizecod, 6).put(bsid, 5).put(0, 3).put(acmod, 3);
		if ((acmod & 1) != 0 && acmod != 1) w.put(0, 2);
		if ((acmod & 4) != 0) w.put(0, 2);
		if (acmod == 2) w.put(0, 2);
		w.put(lfe ? 1 : 0, 1).align();
		w.b.resize(std::max(w.b.size(), size), 0);
		return w.b;
	}

	// An E-AC-3 sync frame's header, padded to the frame's size. With fscod 3 the field that otherwise
	// counts the blocks gives the reduced rate; a dependent substream may carry a channel map.
	bytes eac3_frame(const unsigned stream_type, const unsigned substream, const unsigned frame_bytes, const unsigned fscod,
	                 const unsigned blocks_or_rate, const unsigned acmod, const bool lfe, const int chanmap = -1)
	{
		msb_bits w;
		w.put(0x0B77, 16).put(stream_type, 2).put(substream, 3).put(frame_bytes / 2 - 1, 11).put(fscod, 2).put(blocks_or_rate, 2);
		w.put(acmod, 3).put(lfe ? 1 : 0, 1).put(16, 5).put(0, 5).put(0, 1);
		if (acmod == 0) w.put(0, 5).put(0, 1);

		if (stream_type == 1 && chanmap >= 0)
		{
			w.put(1, 1).put(static_cast<unsigned>(chanmap), 16);
		}
		else if (stream_type == 1)
		{
			w.put(0, 1);
		}

		w.align();
		w.b.resize(std::max<std::size_t>(w.b.size(), frame_bytes), 0);
		return w.b;
	}

	// A location of E-AC-3's custom channel map: location 0 is its most significant bit.
	constexpr int chanmap_location(const int location)
	{
		return 0x8000 >> location;
	}
}

TEST("media audio AC-3 sync frames give the rate, channels, bit rate and frame size")
{
	using libfiles::detail::parse_ac3_frame;

	audio_header h;
	std::string codec;
	CHECK(parse_ac3_frame(ac3_frame(0, 30, 8, 7, true), h, codec));
	CHECK_STR(codec, "ac3");
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 448000 b/s, 1536 samples, 1792 bytes");

	// Rows of A/52's frame size table: two-byte words at 48, 44.1 and 32 kHz.
	struct row
	{
		unsigned code;
		unsigned words[3];
	};

	for (const auto& [code, words] : {row{0, {64, 69, 96}}, row{1, {64, 70, 96}}, row{8, {128, 139, 192}}, row{9, {128, 140, 192}},
	                                  row{26, {640, 696, 960}}, row{27, {640, 697, 960}}, row{36, {1280, 1393, 1920}}, row{37, {1280, 1394, 1920}}})
	{
		constexpr std::uint32_t rates[3] = {48000, 44100, 32000};

		for (unsigned fscod = 0; fscod < 3; ++fscod)
		{
			CHECK(parse_ac3_frame(ac3_frame(fscod, code, 8, 2, false), h, codec));
			CHECK_EQ(h.frame_bytes, 2 * words[fscod]);
			CHECK_EQ(h.sample_rate, rates[fscod]);
		}
	}

	constexpr std::uint32_t kbps[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};

	for (unsigned code = 0; code < 38; ++code)
	{
		CHECK(parse_ac3_frame(ac3_frame(0, code, 8, 2, false), h, codec));
		CHECK_EQ(h.bit_rate, kbps[code / 2] * 1000);
	}

	// Each audio coding mode, with and without LFE.
	constexpr std::uint32_t channels[8] = {2, 1, 2, 3, 3, 4, 4, 5};

	for (unsigned acmod = 0; acmod < 8; ++acmod)
	{
		CHECK(parse_ac3_frame(ac3_frame(0, 30, 8, acmod, false), h, codec));
		CHECK_EQ(h.channels, channels[acmod]);
		CHECK(parse_ac3_frame(ac3_frame(0, 30, 6, acmod, true), h, codec));
		CHECK_EQ(h.channels, channels[acmod] + 1);
	}

	// bsid 9 and 10 halve and quarter the rate.
	CHECK(parse_ac3_frame(ac3_frame(0, 30, 9, 2, false), h, codec));
	CHECK_EQ(h.sample_rate, 24000u);
	CHECK_EQ(h.bit_rate, 224000u);
	CHECK(parse_ac3_frame(ac3_frame(0, 30, 10, 2, false), h, codec));
	CHECK_EQ(h.sample_rate, 12000u);
	CHECK_EQ(h.bit_rate, 112000u);

	// A reserved rate or frame size, a bsid past E-AC-3's, and no sync word.
	auto untouched = sentinel_header();
	std::string name = "untouched";
	CHECK(!parse_ac3_frame(ac3_frame(3, 30, 8, 2, false), untouched, name));
	CHECK(!parse_ac3_frame(ac3_frame(0, 38, 8, 2, false), untouched, name));
	CHECK(!parse_ac3_frame(ac3_frame(0, 30, 17, 2, false), untouched, name));
	auto unsynced = ac3_frame(0, 30, 8, 2, false);
	unsynced[1] = 0x78;
	CHECK(!parse_ac3_frame(unsynced, untouched, name));
	CHECK_STR(name, "untouched");
	CHECK_STR(describe(untouched), describe(sentinel_header()));

	check_prefixes(run_ac3_frame, ac3_frame(0, 30, 8, 7, true), 7);
	check_damage(run_ac3_frame, ac3_frame(0, 30, 8, 7, true));
}

TEST("media audio E-AC-3 sync frames add the channels of their dependent substreams")
{
	using libfiles::detail::parse_ac3_frame;

	audio_header h;
	std::string codec;
	const auto independent = eac3_frame(0, 0, 1536, 0, 3, 7, true);
	CHECK(parse_ac3_frame(independent, h, codec));
	CHECK_STR(codec, "eac3");
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 384000 b/s, 1536 samples, 1536 bytes");

	// 7.1: a dependent substream adds the rear surround pair.
	auto seven_one = independent;
	const auto rears = eac3_frame(1, 0, 512, 0, 3, 2, false, chanmap_location(6));
	seven_one.insert(seven_one.end(), rears.begin(), rears.end());
	CHECK(parse_ac3_frame(seven_one, h, codec));
	CHECK_STR(describe(h), "48000 Hz, 8 ch, 0 int, '', 512000 b/s, 1536 samples, 2048 bytes");

	// A map that repeats the surrounds the independent substream has adds only what is new.
	auto repeated = independent;
	const auto surrounds = eac3_frame(1, 0, 512, 0, 3, 6, false, chanmap_location(3) | chanmap_location(4) | chanmap_location(6));
	repeated.insert(repeated.end(), surrounds.begin(), surrounds.end());
	CHECK(parse_ac3_frame(repeated, h, codec));
	CHECK_EQ(h.channels, 8u);

	// Two dependent substreams: the rear pair, then the wide pair and LFE2.
	auto wide = seven_one;
	const auto more = eac3_frame(1, 1, 512, 0, 3, 3, false, chanmap_location(10) | chanmap_location(14));
	wide.insert(wide.end(), more.begin(), more.end());
	CHECK(parse_ac3_frame(wide, h, codec));
	CHECK_EQ(h.channels, 11u);
	CHECK_EQ(h.frame_bytes, 2560u);

	// Without a map, a dependent substream's channels are its acmod's.
	auto unmapped = independent;
	const auto stereo = eac3_frame(1, 0, 512, 0, 3, 2, false);
	unmapped.insert(unmapped.end(), stereo.begin(), stereo.end());
	CHECK(parse_ac3_frame(unmapped, h, codec));
	CHECK_EQ(h.channels, 6u);
	CHECK_EQ(h.frame_bytes, 2048u);

	// One at another rate is not this frame's, as FFmpeg's decoder ignores it.
	auto mismatched = independent;
	const auto other_rate = eac3_frame(1, 0, 512, 1, 3, 2, false, chanmap_location(6));
	mismatched.insert(mismatched.end(), other_rate.begin(), other_rate.end());
	CHECK(parse_ac3_frame(mismatched, h, codec));
	CHECK_EQ(h.channels, 6u);
	CHECK_EQ(h.frame_bytes, 1536u);

	// An AC-3 frame extended by an E-AC-3 dependent substream is E-AC-3.
	auto core = ac3_frame(0, 30, 8, 7, true, 1792);
	core.insert(core.end(), rears.begin(), rears.end());
	CHECK(parse_ac3_frame(core, h, codec));
	CHECK_STR(codec, "eac3");
	CHECK_EQ(h.channels, 8u);
	CHECK_EQ(h.bit_rate, 448000u + 128000u);

	// A reduced rate, and frames of fewer blocks.
	CHECK(parse_ac3_frame(eac3_frame(0, 0, 768, 3, 1, 2, false), h, codec));
	CHECK_STR(describe(h), "22050 Hz, 2 ch, 0 int, '', 88200 b/s, 1536 samples, 768 bytes");
	CHECK(parse_ac3_frame(eac3_frame(0, 0, 512, 0, 1, 1, false), h, codec));
	CHECK_STR(describe(h), "48000 Hz, 1 ch, 0 int, '', 384000 b/s, 512 samples, 512 bytes");
	CHECK(parse_ac3_frame(eac3_frame(0, 0, 256, 1, 0, 0, false), h, codec));
	CHECK_STR(describe(h), "44100 Hz, 2 ch, 0 int, '', 352800 b/s, 256 samples, 256 bytes");
	CHECK(parse_ac3_frame(eac3_frame(2, 0, 1536, 0, 3, 7, true), h, codec));
	CHECK_EQ(h.channels, 6u);

	// A dependent substream or another program's cannot stand alone; a reserved stream type, rate or
	// size is refused.
	auto untouched = sentinel_header();
	std::string name = "untouched";
	CHECK(!parse_ac3_frame(rears, untouched, name));
	CHECK(!parse_ac3_frame(eac3_frame(0, 1, 1536, 0, 3, 7, true), untouched, name));
	CHECK(!parse_ac3_frame(eac3_frame(3, 0, 1536, 0, 3, 7, true), untouched, name));
	CHECK(!parse_ac3_frame(eac3_frame(0, 0, 1536, 3, 3, 7, true), untouched, name));
	CHECK(!parse_ac3_frame(eac3_frame(0, 0, 6, 0, 3, 2, false), untouched, name));
	CHECK_STR(name, "untouched");
	CHECK_STR(describe(untouched), describe(sentinel_header()));

	check_prefixes(run_ac3_frame, independent, 6);
	check_prefixes(run_ac3_frame, seven_one, 6, false);
	CHECK_STR(run_ac3_frame(bytes(seven_one.begin(), seven_one.begin() + 1544)).found,
	          "eac3: 48000 Hz, 6 ch, 0 int, '', 384000 b/s, 1536 samples, 1536 bytes");
	CHECK_STR(run_ac3_frame(bytes(seven_one.begin(), seven_one.begin() + 1545)).found,
	          "eac3: 48000 Hz, 8 ch, 0 int, '', 512000 b/s, 1536 samples, 2048 bytes");
	check_damage(run_ac3_frame, seven_one);
	check_damage(run_ac3_frame, seven_one, 1536);
}

TEST("media audio dac3 and dec3 boxes give the channels of their substreams")
{
	using libfiles::detail::parse_ac3_config;
	using libfiles::detail::parse_eac3_config;

	const auto dac3 = [](const unsigned fscod, const unsigned bsid, const unsigned acmod, const bool lfe, const unsigned bit_rate_code)
	{
		return msb_bits().put(fscod, 2).put(bsid, 5).put(0, 3).put(acmod, 3).put(lfe ? 1 : 0, 1).put(bit_rate_code, 5).put(0, 5).b;
	};

	audio_header h;
	CHECK(parse_ac3_config(dac3(0, 8, 7, true, 15), h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 448000 b/s, 1536 samples, 0 bytes");
	CHECK(parse_ac3_config(dac3(1, 8, 2, false, 18), h));
	CHECK_STR(describe(h), "44100 Hz, 2 ch, 0 int, '', 640000 b/s, 1536 samples, 0 bytes");
	CHECK(parse_ac3_config(dac3(0, 9, 1, false, 10), h));
	CHECK_STR(describe(h), "24000 Hz, 1 ch, 0 int, '', 96000 b/s, 1536 samples, 0 bytes");

	// FFmpeg reads only the channels from the box: a reserved rate or bit rate code is left unknown.
	CHECK(parse_ac3_config(dac3(3, 8, 7, true, 31), h));
	CHECK_STR(describe(h), "0 Hz, 6 ch, 0 int, '', 0 b/s, 1536 samples, 0 bytes");

	const auto dec3 = [](const unsigned data_rate, const unsigned fscod, const unsigned acmod, const bool lfe, const unsigned dependents,
	                     const unsigned chan_loc)
	{
		msb_bits w;
		w.put(data_rate, 13).put(0, 3).put(fscod, 2).put(16, 5).put(0, 1).put(0, 1).put(0, 3).put(acmod, 3).put(lfe ? 1 : 0, 1);
		w.put(0, 3).put(dependents, 4);

		if (dependents != 0)
		{
			w.put(chan_loc, 9);
		}
		else
		{
			w.put(0, 1);
		}

		return w.align().b;
	};

	// chan_loc's bit 1 is the rear surround pair: 7.1.
	CHECK(parse_eac3_config(dec3(640, 0, 7, true, 1, 0x002), h));
	CHECK_STR(describe(h), "48000 Hz, 8 ch, 0 int, '', 640000 b/s, 0 samples, 0 bytes");
	CHECK(parse_eac3_config(dec3(384, 0, 7, true, 0, 0), h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 384000 b/s, 0 samples, 0 bytes");

	// LFE2 in bit 8; the centre pair and the back centre in bits 0 and 2; and the back centre that acmod
	// 5's surround already is.
	CHECK(parse_eac3_config(dec3(640, 0, 7, true, 1, 0x100), h));
	CHECK_EQ(h.channels, 7u);
	CHECK(parse_eac3_config(dec3(640, 0, 7, true, 1, 0x005), h));
	CHECK_EQ(h.channels, 9u);
	CHECK(parse_eac3_config(dec3(640, 2, 5, false, 2, 0x004), h));
	CHECK_EQ(h.channels, 4u);
	CHECK_EQ(h.sample_rate, 32000u);

	// fscod 3 is a reduced rate, which the box does not give.
	CHECK(parse_eac3_config(dec3(96, 3, 2, false, 0, 0), h));
	CHECK_EQ(h.sample_rate, 0u);
	CHECK_EQ(h.channels, 2u);

	check_prefixes(audio_parser(parse_ac3_config), dac3(0, 8, 7, true, 15), 3);
	check_prefixes(audio_parser(parse_eac3_config), dec3(640, 0, 7, true, 1, 0x002), 6);
	check_prefixes(audio_parser(parse_eac3_config), dec3(384, 0, 7, true, 0, 0), 5);
	check_damage(audio_parser(parse_ac3_config), dac3(0, 8, 7, true, 15));
	check_damage(audio_parser(parse_eac3_config), dec3(640, 0, 7, true, 1, 0x002));
	check_damage(audio_parser(parse_eac3_config), dec3(384, 0, 7, true, 0, 0));
}

namespace
{
	struct dts_fields
	{
		unsigned deficit = 31;
		bool crc = false;
		unsigned blocks = 16;
		unsigned frame_bytes = 1006;
		unsigned mode = 9;
		unsigned rate = 13;
		unsigned bit_rate = 15;
		unsigned reserved = 0;
		unsigned extension_type = 0;
		bool extension = false;
		unsigned lfe = 1;
		unsigned resolution = 5;
	};

	// A DTS core frame header as 16-bit big-endian words, padded to 32 bytes.
	bytes dts_core(const dts_fields& f)
	{
		msb_bits w;
		w.put(0x7FFE8001, 32).put(1, 1).put(f.deficit, 5).put(f.crc ? 1 : 0, 1).put(f.blocks - 1, 7).put(f.frame_bytes - 1, 14);
		w.put(f.mode, 6).put(f.rate, 4).put(f.bit_rate, 5).put(f.reserved, 1).put(0, 4).put(f.extension_type, 3).put(f.extension ? 1 : 0, 1);
		w.put(1, 1).put(f.lfe, 2).put(0, 1);
		if (f.crc) w.put(0xABCD, 16);
		w.put(0, 1).put(7, 4).put(0, 2).put(f.resolution, 3).put(0, 2).put(0, 4).align();
		w.b.resize(32, 0);
		return w.b;
	}

	bytes dts_little_endian(bytes b)
	{
		for (std::size_t i = 0; i + 1 < b.size(); i += 2) std::swap(b[i], b[i + 1]);
		return b;
	}

	// The same bits fourteen to a 16-bit word, sign-extended into its top two bits, as CD audio carries DTS.
	bytes dts_fourteen_bit(const bytes& words, const bool little_endian)
	{
		bytes out;
		const auto total = words.size() * 8;

		for (std::size_t at = 0; at < total; at += 14)
		{
			unsigned word = 0;

			for (auto i = at; i < at + 14; ++i)
			{
				word = word << 1 | (i < total ? (words[i / 8] >> (7 - i % 8) & 1u) : 0u);
			}

			if ((word & 0x2000) != 0) word |= 0xC000;
			const auto high = static_cast<std::uint8_t>(word >> 8);
			const auto low = static_cast<std::uint8_t>(word);
			out.push_back(little_endian ? low : high);
			out.push_back(little_endian ? high : low);
		}

		return out;
	}
}

TEST("media audio DTS core headers give the layout, rate and bit rate in each of their forms")
{
	using libfiles::detail::parse_dts_frame;

	const auto core = dts_core({});
	audio_header h;
	CHECK(parse_dts_frame(core, h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 768000 b/s, 512 samples, 1006 bytes");

	// Little-endian words, and fourteen bits to a word either way, which take more bytes to a frame.
	CHECK(parse_dts_frame(dts_little_endian(core), h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 768000 b/s, 512 samples, 1006 bytes");
	CHECK(parse_dts_frame(dts_fourteen_bit(core, false), h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 768000 b/s, 512 samples, 1150 bytes");
	CHECK(parse_dts_frame(dts_fourteen_bit(core, true), h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 768000 b/s, 512 samples, 1150 bytes");

	constexpr std::uint32_t channels[16] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};

	for (unsigned mode = 0; mode < 16; ++mode)
	{
		dts_fields f;
		f.mode = mode;
		f.lfe = 0;
		CHECK(parse_dts_frame(dts_core(f), h));
		CHECK_EQ(h.channels, channels[mode]);
	}

	struct coded
	{
		unsigned code;
		std::uint32_t value;
	};

	for (const auto& [code, hz] : {coded{1, 8000}, coded{2, 16000}, coded{3, 32000}, coded{6, 11025}, coded{7, 22050}, coded{8, 44100},
	                               coded{11, 12000}, coded{12, 24000}, coded{13, 48000}, coded{14, 96000}, coded{15, 192000}})
	{
		dts_fields f;
		f.rate = code;
		CHECK(parse_dts_frame(dts_core(f), h));
		CHECK_EQ(h.sample_rate, hz);
	}

	// Open, variable and lossless rates say none.
	for (const auto& [code, bits] : {coded{0, 32000}, coded{22, 1411200}, coded{24, 1536000}, coded{28, 3840000}, coded{29, 0}, coded{30, 0},
	                                 coded{31, 0}})
	{
		dts_fields f;
		f.bit_rate = code;
		CHECK(parse_dts_frame(dts_core(f), h));
		CHECK_EQ(h.bit_rate, bits);
	}

	// XCh adds a back centre channel, as FFmpeg plays it; X96 adds none; either LFE flag adds one; a header
	// CRC moves the fields after it.
	dts_fields xch;
	xch.extension = true;
	CHECK(parse_dts_frame(dts_core(xch), h));
	CHECK_EQ(h.channels, 7u);
	xch.extension_type = 2;
	CHECK(parse_dts_frame(dts_core(xch), h));
	CHECK_EQ(h.channels, 6u);
	dts_fields lfe;
	lfe.lfe = 2;
	CHECK(parse_dts_frame(dts_core(lfe), h));
	CHECK_EQ(h.channels, 6u);
	dts_fields crc;
	crc.crc = true;
	crc.blocks = 128;
	CHECK(parse_dts_frame(dts_core(crc), h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 768000 b/s, 4096 samples, 1006 bytes");

	// FFmpeg's refusals: a frame cut short of 32 samples, blocks that are not whole subband groups, a frame
	// under 96 bytes, a user-defined layout, a reserved rate, the reserved bit set, LFE flag 3 and an unknown
	// source resolution.
	const auto refused = [](void (*change)(dts_fields&))
	{
		dts_fields f;
		change(f);
		auto untouched = sentinel_header();
		const auto ok = parse_dts_frame(dts_core(f), untouched);
		return !ok && describe(untouched) == describe(sentinel_header());
	};

	CHECK(refused([](dts_fields& f) { f.deficit = 30; }));
	CHECK(refused([](dts_fields& f) { f.blocks = 15; }));
	CHECK(refused([](dts_fields& f) { f.frame_bytes = 95; }));
	CHECK(refused([](dts_fields& f) { f.mode = 16; }));
	CHECK(refused([](dts_fields& f) { f.rate = 0; }));
	CHECK(refused([](dts_fields& f) { f.rate = 9; }));
	CHECK(refused([](dts_fields& f) { f.reserved = 1; }));
	CHECK(refused([](dts_fields& f) { f.lfe = 3; }));
	CHECK(refused([](dts_fields& f) { f.resolution = 4; }));
	CHECK(refused([](dts_fields& f) { f.resolution = 7; }));

	// No sync word, and a 14-bit marker that does not begin a normal frame.
	auto unsynced = core;
	unsynced[3] = 0x02;
	CHECK(!parse_dts_frame(unsynced, h));
	auto abnormal = dts_fourteen_bit(core, false);
	abnormal[4] = 0x06;
	CHECK(!parse_dts_frame(abnormal, h));
	abnormal = dts_fourteen_bit(core, true);
	abnormal[5] = 0x06;
	CHECK(!parse_dts_frame(abnormal, h));

	// The header needs whole words: seven of 16 bits, or eight of 14.
	check_prefixes(audio_parser(parse_dts_frame), core, 14);
	check_prefixes(audio_parser(parse_dts_frame), dts_little_endian(core), 14);
	check_prefixes(audio_parser(parse_dts_frame), dts_fourteen_bit(core, false), 16);
	check_prefixes(audio_parser(parse_dts_frame), dts_fourteen_bit(core, true), 16);
	check_damage(audio_parser(parse_dts_frame), core);
	check_damage(audio_parser(parse_dts_frame), dts_little_endian(core));
	check_damage(audio_parser(parse_dts_frame), dts_fourteen_bit(core, false));
	check_damage(audio_parser(parse_dts_frame), dts_fourteen_bit(core, true));
}

namespace
{
	bytes opus_head(const unsigned version, const unsigned channels, const unsigned pre_skip, const unsigned family, const bytes& table = {})
	{
		builder b;
		b.raw("OpusHead").u8(version).u8(channels).le16(pre_skip).le32(44100).le16(0).u8(family).raw(table);
		return b.b;
	}
}

TEST("media audio Opus heads play at 48 kHz, with their channels and pre-skip")
{
	using libfiles::detail::parse_opus_head;

	audio_header h;
	std::uint32_t pre_skip = 0;
	CHECK(parse_opus_head(opus_head(1, 2, 312, 0), h, pre_skip));
	CHECK_STR(describe(h), "48000 Hz, 2 ch, 0 int, '', 0 b/s, 0 samples, 0 bytes");
	CHECK_EQ(pre_skip, 312u);

	// 5.1 in Vorbis order: four streams, two of them coupled.
	CHECK(parse_opus_head(opus_head(1, 6, 3840, 1, bytes{4, 2, 0, 4, 1, 2, 3, 5}), h, pre_skip));
	CHECK_EQ(h.channels, 6u);
	CHECK_EQ(pre_skip, 3840u);

	// A silent channel; ambisonics of four channels, and of six with a stereo pair; discrete channels; family
	// 3's projection, read as 255 is without its demixing matrix; and the last compatible version.
	CHECK(parse_opus_head(opus_head(1, 2, 0, 1, bytes{1, 0, 0, 255}), h, pre_skip));
	CHECK(parse_opus_head(opus_head(1, 4, 0, 2, bytes{4, 0, 0, 1, 2, 3}), h, pre_skip));
	CHECK(parse_opus_head(opus_head(1, 6, 0, 2, bytes{6, 0, 0, 1, 2, 3, 4, 5}), h, pre_skip));
	CHECK(parse_opus_head(opus_head(1, 3, 0, 255, bytes{3, 0, 0, 1, 2}), h, pre_skip));
	CHECK(parse_opus_head(opus_head(1, 4, 0, 3, bytes{2, 2, 9, 9, 9, 9}), h, pre_skip));
	CHECK_EQ(h.channels, 4u);
	CHECK(parse_opus_head(opus_head(15, 1, 0, 0), h, pre_skip));
	CHECK_EQ(h.channels, 1u);

	const bytes refused[] = {
		opus_head(16, 2, 312, 0),
		opus_head(1, 0, 312, 0),
		opus_head(1, 3, 312, 0),
		opus_head(1, 9, 0, 1, bytes{9, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}),
		opus_head(1, 2, 0, 1, bytes{0, 0, 0, 0}),
		opus_head(1, 2, 0, 1, bytes{1, 2, 0, 0}),
		opus_head(1, 1, 0, 255, bytes{200, 100, 0}),
		opus_head(1, 2, 0, 1, bytes{1, 0, 0, 1}),
		opus_head(1, 5, 0, 2, bytes{5, 0, 0, 1, 2, 3, 4}),
		opus_head(1, 6, 0, 1, bytes{4, 2, 0, 4, 1, 2, 3}),
		builder().raw("OpusTags").u8(1).u8(2).le16(312).le32(44100).le16(0).u8(0).b,
	};

	for (const auto& head : refused)
	{
		CHECK(!run_opus_head(head).ok);
		CHECK(run_opus_head(head).untouched);
	}

	check_prefixes(run_opus_head, opus_head(1, 2, 312, 0), 19);
	check_prefixes(run_opus_head, opus_head(1, 6, 3840, 1, bytes{4, 2, 0, 4, 1, 2, 3, 5}), 27);
	check_damage(run_opus_head, opus_head(1, 2, 312, 0));
	check_damage(run_opus_head, opus_head(1, 6, 3840, 1, bytes{4, 2, 0, 4, 1, 2, 3, 5}));
}

namespace
{
	bytes vorbis_identification(const unsigned channels, const std::uint32_t rate, const std::int32_t nominal, const unsigned block_sizes = 0xB8,
	                            const unsigned framing = 1, const std::uint32_t version = 0)
	{
		builder b;
		b.u8(1).raw("vorbis").le32(version).u8(channels).le32(rate).le32(0).le32(static_cast<std::uint32_t>(nominal)).le32(0);
		return b.u8(block_sizes).u8(framing).b;
	}
}

TEST("media audio Vorbis identification headers give the rate, channels and nominal bit rate")
{
	using libfiles::detail::parse_vorbis_header;

	audio_header h;
	CHECK(parse_vorbis_header(vorbis_identification(2, 44100, 128000), h));
	CHECK_STR(describe(h), "44100 Hz, 2 ch, 0 int, '', 128000 b/s, 0 samples, 0 bytes");
	CHECK(parse_vorbis_header(vorbis_identification(6, 48000, -1, 0x66), h));
	CHECK_STR(describe(h), "48000 Hz, 6 ch, 0 int, '', 0 b/s, 0 samples, 0 bytes");
	CHECK(parse_vorbis_header(vorbis_identification(1, 8000, 0, 0xDD), h));
	CHECK_EQ(h.bit_rate, 0u);

	// What follows the packet does not matter.
	auto longer = vorbis_identification(2, 44100, 128000);
	longer.push_back(0x03);
	CHECK(parse_vorbis_header(longer, h));

	const bytes refused[] = {
		vorbis_identification(2, 44100, 128000, 0xB8, 1, 1),
		vorbis_identification(0, 44100, 128000),
		vorbis_identification(2, 0, 128000),
		vorbis_identification(2, 44100, 128000, 0x5B),
		vorbis_identification(2, 44100, 128000, 0xB5),
		vorbis_identification(2, 44100, 128000, 0xE8),
		vorbis_identification(2, 44100, 128000, 0xB8, 0),
		builder().u8(3).raw("vorbis").zeros(23).b,
		builder().u8(1).raw("vorbiz").le32(0).u8(2).le32(44100).zeros(12).u8(0xB8).u8(1).b,
	};

	for (const auto& header : refused)
	{
		auto untouched = sentinel_header();
		CHECK(!parse_vorbis_header(header, untouched));
		CHECK_STR(describe(untouched), describe(sentinel_header()));
	}

	check_prefixes(audio_parser(parse_vorbis_header), vorbis_identification(2, 44100, 128000), 30);
	check_damage(audio_parser(parse_vorbis_header), vorbis_identification(2, 44100, 128000));
}

namespace
{
	bytes streaminfo(const unsigned min_block, const unsigned max_block, const std::uint32_t rate, const unsigned channels, const unsigned bits,
	                 const std::uint64_t samples)
	{
		msb_bits w;
		w.put(min_block, 16).put(max_block, 16).put(14, 24).put(12000, 24).put(rate, 20).put(channels - 1, 3).put(bits - 1, 5).put(samples, 36);
		for (auto i = 0; i < 16; ++i) w.put(0xA5, 8);
		return w.b;
	}
}

TEST("media audio FLAC STREAMINFO gives the sample format and length")
{
	using libfiles::detail::parse_flac_streaminfo;

	audio_header h;
	std::uint64_t samples = 0;
	CHECK(parse_flac_streaminfo(streaminfo(4096, 4096, 44100, 2, 16, 441000), h, samples));
	CHECK_STR(describe(h), "44100 Hz, 2 ch, 16 int, '', 0 b/s, 4096 samples, 0 bytes");
	CHECK_EQ(samples, std::uint64_t{441000});

	// Blocks of varying size, and a length past 32 bits.
	CHECK(parse_flac_streaminfo(streaminfo(1152, 4608, 96000, 6, 24, 0x900000001), h, samples));
	CHECK_STR(describe(h), "96000 Hz, 6 ch, 24 int, '', 0 b/s, 0 samples, 0 bytes");
	CHECK_EQ(samples, std::uint64_t{0x900000001});
	CHECK(parse_flac_streaminfo(streaminfo(16, 65535, 655350, 8, 32, 0), h, samples));
	CHECK_STR(describe(h), "655350 Hz, 8 ch, 32 int, '', 0 b/s, 0 samples, 0 bytes");
	CHECK_EQ(samples, std::uint64_t{0});

	// A rate of 0 is left to the frames, as FFmpeg leaves it.
	CHECK(parse_flac_streaminfo(streaminfo(4096, 4096, 0, 2, 16, 0), h, samples));
	CHECK_EQ(h.sample_rate, 0u);

	// FFmpeg's refusals: blocks under 16 samples, and fewer than 4 bits.
	auto untouched = sentinel_header();
	std::uint64_t unchanged = 7;
	CHECK(!parse_flac_streaminfo(streaminfo(15, 15, 44100, 2, 16, 0), untouched, unchanged));
	CHECK(!parse_flac_streaminfo(streaminfo(4096, 4096, 44100, 2, 3, 0), untouched, unchanged));
	CHECK_STR(describe(untouched), describe(sentinel_header()));
	CHECK_EQ(unchanged, std::uint64_t{7});

	check_prefixes(run_flac_streaminfo, streaminfo(4096, 4096, 44100, 2, 16, 441000), 34);
	check_damage(run_flac_streaminfo, streaminfo(4096, 4096, 44100, 2, 16, 441000));
}

namespace
{
	bytes alac_config(const std::uint32_t frame_length, const unsigned bits, const unsigned channels, const std::uint32_t rate, const bool box)
	{
		builder b;
		if (box) b.be32(36).raw("alac").be32(0);
		b.be32(frame_length).u8(0).u8(bits).u8(40).u8(10).u8(14).u8(channels).be16(255).be32(0).be32(900000).be32(rate);
		return b.b;
	}
}

TEST("media audio ALAC configs give the sample format, with or without their box header")
{
	using libfiles::detail::parse_alac_config;

	audio_header h;
	CHECK(parse_alac_config(alac_config(4096, 16, 2, 44100, false), h));
	CHECK_STR(describe(h), "44100 Hz, 2 ch, 16 int, '', 900000 b/s, 4096 samples, 0 bytes");
	CHECK(parse_alac_config(alac_config(4096, 16, 2, 44100, true), h));
	CHECK_STR(describe(h), "44100 Hz, 2 ch, 16 int, '', 900000 b/s, 4096 samples, 0 bytes");
	CHECK(parse_alac_config(alac_config(4096, 24, 6, 96000, true), h));
	CHECK_STR(describe(h), "96000 Hz, 6 ch, 24 int, '', 900000 b/s, 4096 samples, 0 bytes");

	// With no channel count FFmpeg takes the container's.
	CHECK(parse_alac_config(alac_config(4096, 20, 0, 48000, false), h));
	CHECK_EQ(h.channels, 0u);
	CHECK_EQ(h.bits_per_sample, 20);

	const bytes refused[] = {
		alac_config(4096, 12, 2, 44100, false),
		alac_config(4096, 16, 9, 44100, false),
		alac_config(0, 16, 2, 44100, true),
		alac_config(4096 * 4096 + 1, 16, 2, 44100, true),
	};

	for (const auto& config : refused)
	{
		auto untouched = sentinel_header();
		CHECK(!parse_alac_config(config, untouched));
		CHECK_STR(describe(untouched), describe(sentinel_header()));
	}

	// Cut into, the box header is read as a config, whose bit depth its type's letters cannot be.
	check_prefixes(audio_parser(parse_alac_config), alac_config(4096, 16, 2, 44100, false), 24);
	check_prefixes(audio_parser(parse_alac_config), alac_config(4096, 16, 2, 44100, true), 36);
	check_damage(audio_parser(parse_alac_config), alac_config(4096, 16, 2, 44100, true));
}

namespace
{
	bytes speex_header(const std::int32_t rate, const std::int32_t mode, const std::int32_t channels, const std::int32_t bit_rate,
	                   const std::int32_t frame_size)
	{
		const auto field = [](const std::int32_t v) { return static_cast<std::uint32_t>(v); };
		builder b;
		b.raw("Speex   ").raw("speex-1.2").zeros(11).le32(1).le32(80).le32(field(rate)).le32(field(mode)).le32(4).le32(field(channels));
		b.le32(field(bit_rate)).le32(field(frame_size)).le32(0).le32(1).le32(0).le32(0).le32(0);
		return b.b;
	}
}

TEST("media audio Speex headers give the rate, channels and frame")
{
	using libfiles::detail::parse_speex_header;

	audio_header h;
	CHECK(parse_speex_header(speex_header(16000, 1, 1, -1, 320), h));
	CHECK_STR(describe(h), "16000 Hz, 1 ch, 0 int, '', 0 b/s, 320 samples, 0 bytes");
	CHECK(parse_speex_header(speex_header(8000, 0, 2, 15000, 160), h));
	CHECK_STR(describe(h), "8000 Hz, 2 ch, 0 int, '', 15000 b/s, 160 samples, 0 bytes");
	CHECK(parse_speex_header(speex_header(32000, 2, 1, 0, 640), h));
	CHECK_EQ(h.sample_rate, 32000u);

	auto other = speex_header(16000, 1, 1, -1, 320);
	other[0] = 's';

	const bytes refused[] = {
		speex_header(16000, 3, 1, -1, 320),
		speex_header(16000, -1, 1, -1, 320),
		speex_header(16000, 1, 0, -1, 320),
		speex_header(16000, 1, 3, -1, 320),
		speex_header(0, 1, 1, -1, 320),
		speex_header(-8000, 1, 1, -1, 320),
		other,
	};

	for (const auto& header : refused)
	{
		auto untouched = sentinel_header();
		CHECK(!parse_speex_header(header, untouched));
		CHECK_STR(describe(untouched), describe(sentinel_header()));
	}

	// FFmpeg reads the header up to its frames per packet.
	check_prefixes(audio_parser(parse_speex_header), speex_header(16000, 1, 1, -1, 320), 68);
	check_damage(audio_parser(parse_speex_header), speex_header(16000, 1, 1, -1, 320));
}

namespace
{
	// WAVEFORMATEX: PCMWAVEFORMAT's sixteen bytes, then cbSize and the bytes it counts.
	bytes wave_format(const unsigned tag, const unsigned channels, const std::uint32_t rate, const unsigned bits, const bytes& extra = {})
	{
		const auto align = channels * ((bits + 7) / 8);
		builder b;
		b.le16(tag).le16(channels).le32(rate).le32(rate * align).le16(align).le16(bits).le16(static_cast<unsigned>(extra.size())).raw(extra);
		return b.b;
	}

	// A KSDATAFORMAT_SUBTYPE GUID, as the file stores it: the format tag, then the common base.
	bytes subtype(const std::uint32_t tag)
	{
		return builder().le32(tag).raw(bytes{0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}).b;
	}

	// WAVEFORMATEXTENSIBLE's 22 bytes after cbSize: the valid bits, the channel mask and the SubFormat.
	bytes extensible(const unsigned valid_bits, const std::uint32_t mask, const bytes& subformat)
	{
		return builder().le16(valid_bits).le32(mask).raw(subformat).b;
	}

	std::string wave(const bytes& data)
	{
		media_stream s;
		return libfiles::detail::read_wave_format(data, s) ? describe(s) : "refused";
	}
}

TEST("media audio WAVEFORMATEX gives the codec, rate, channels and coded sample format")
{
	using libfiles::detail::read_wave_format;

	media_stream s;
	std::uint32_t bytes_per_second = 0;
	std::uint16_t align = 0;
	const auto pcm = wave_format(1, 2, 44100, 16);
	CHECK(read_wave_format(pcm, s, &bytes_per_second, &align));
	CHECK(s.kind == media_stream_kind::audio);
	CHECK_STR(describe(s), "'pcm' '0x0001' audio: 44100 Hz, 2 ch, 16 int, ''");
	CHECK_EQ(bytes_per_second, 176400u);
	CHECK_EQ(align, 4);

	// PCMWAVEFORMAT, without cbSize; and WAVEFORMAT, without the bits, which PCM's block alignment gives.
	CHECK_STR(wave(bytes(pcm.begin(), pcm.begin() + 16)), "'pcm' '0x0001' audio: 44100 Hz, 2 ch, 16 int, ''");
	CHECK_STR(wave(bytes(pcm.begin(), pcm.begin() + 14)), "'pcm' '0x0001' audio: 44100 Hz, 2 ch, 16 int, ''");

	// 8-bit PCM is unsigned in WAV, which the model does not record; 24 bits; IEEE float.
	CHECK_STR(wave(wave_format(1, 1, 8000, 8)), "'pcm' '0x0001' audio: 8000 Hz, 1 ch, 8 int, ''");
	CHECK_STR(wave(wave_format(1, 6, 96000, 24)), "'pcm' '0x0001' audio: 96000 Hz, 6 ch, 24 int, ''");
	CHECK_STR(wave(wave_format(3, 2, 48000, 32)), "'pcm' '0x0003' audio: 48000 Hz, 2 ch, 32 float, ''");
	CHECK_STR(wave(wave_format(3, 2, 48000, 64)), "'pcm' '0x0003' audio: 48000 Hz, 2 ch, 64 float, ''");

	// Lossy codecs have no sample format; the lossless ones keep theirs.
	CHECK_STR(wave(wave_format(0x0161, 2, 48000, 16, bytes(10, 0))), "'wmav2' '0x0161' audio: 48000 Hz, 2 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x0163, 2, 96000, 24, bytes(18, 0))), "'wmalossless' '0x0163' audio: 96000 Hz, 2 ch, 24 int, ''");
	CHECK_STR(wave(wave_format(0xF1AC, 2, 44100, 16)), "'flac' '0xF1AC' audio: 44100 Hz, 2 ch, 16 int, ''");
	CHECK_STR(wave(wave_format(0x0055, 2, 44100, 0, bytes(12, 0))), "'mp3' '0x0055' audio: 44100 Hz, 2 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x0006, 1, 8000, 8)), "'alaw' '0x0006' audio: 8000 Hz, 1 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x0007, 1, 8000, 8)), "'mulaw' '0x0007' audio: 8000 Hz, 1 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x0011, 2, 22050, 4, bytes(2, 0))), "'adpcm_ima_wav' '0x0011' audio: 22050 Hz, 2 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x674F, 2, 44100, 16)), "'vorbis' '0x674F' audio: 44100 Hz, 2 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x1234, 2, 44100, 16)), "'' '0x1234' audio: 44100 Hz, 2 ch, 0 int, ''");

	// The pointers are optional.
	CHECK(read_wave_format(pcm, s));

	// A rate of 0, and a structure cut short of WAVEFORMAT.
	auto untouched = sentinel_stream();
	CHECK(!read_wave_format(wave_format(1, 2, 0, 16), untouched));
	CHECK(!read_wave_format(bytes(pcm.begin(), pcm.begin() + 13), untouched, &bytes_per_second, &align));
	CHECK_STR(describe(untouched), describe(sentinel_stream()));
	CHECK_EQ(bytes_per_second, 176400u);

	check_prefixes(run_wave_format, pcm, 14);
	check_damage(run_wave_format, pcm);
}

TEST("media audio WAVEFORMATEXTENSIBLE stands for the format tag of its SubFormat")
{
	// 24 valid bits of 32, in 5.1.
	const auto pcm = wave_format(0xFFFE, 6, 48000, 32, extensible(24, 0x3F, subtype(1)));
	CHECK_STR(wave(pcm), "'pcm' '0x0001' audio: 48000 Hz, 6 ch, 24 int, ''");
	CHECK_STR(wave(wave_format(0xFFFE, 2, 48000, 32, extensible(32, 3, subtype(3)))), "'pcm' '0x0003' audio: 48000 Hz, 2 ch, 32 float, ''");

	// With no valid bits stated, the container's bits stand.
	CHECK_STR(wave(wave_format(0xFFFE, 2, 48000, 24, extensible(0, 3, subtype(1)))), "'pcm' '0x0001' audio: 48000 Hz, 2 ch, 24 int, ''");

	// A lossy subtype, and the ambisonic subtype of PCM.
	CHECK_STR(wave(wave_format(0xFFFE, 2, 44100, 0, extensible(0, 3, subtype(0x55)))), "'mp3' '0x0055' audio: 44100 Hz, 2 ch, 0 int, ''");
	const bytes ambisonic = {0x01, 0x00, 0x00, 0x00, 0x21, 0x07, 0xD3, 0x11, 0x86, 0x44, 0xC8, 0xC1, 0xCA, 0x00, 0x00, 0x00};
	CHECK_STR(wave(wave_format(0xFFFE, 4, 48000, 16, extensible(16, 0, ambisonic))), "'pcm' '0x0001' audio: 48000 Hz, 4 ch, 16 int, ''");

	// SubFormats that stand for no tag: AC-3's, and one nobody knows; and a tag too wide for WAVE.
	const bytes ac3 = {0x2C, 0x80, 0x6D, 0xE0, 0x46, 0xDB, 0xCF, 0x11, 0xB4, 0xD1, 0x00, 0x80, 0x5F, 0x6C, 0xBB, 0xEA};
	CHECK_STR(wave(wave_format(0xFFFE, 6, 48000, 16, extensible(0, 0x3F, ac3))), "'ac3' '0xFFFE' audio: 48000 Hz, 6 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0xFFFE, 2, 48000, 16, extensible(16, 3, bytes(16, 0x42)))), "'' '0xFFFE' audio: 48000 Hz, 2 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0xFFFE, 2, 48000, 16, extensible(16, 3, subtype(0x10001)))), "'' '0xFFFE' audio: 48000 Hz, 2 ch, 0 int, ''");

	// cbSize short of the extension, or more than the structure holds.
	CHECK_STR(wave(wave_format(0xFFFE, 2, 48000, 16)), "'' '0xFFFE' audio: 48000 Hz, 2 ch, 0 int, ''");
	auto overstated = pcm;
	overstated[16] = 0xFF;
	CHECK_STR(wave(overstated), "'pcm' '0x0001' audio: 48000 Hz, 6 ch, 24 int, ''");
	overstated.resize(39);
	CHECK_STR(wave(overstated), "'' '0xFFFE' audio: 48000 Hz, 6 ch, 0 int, ''");

	check_prefixes(run_wave_format, pcm, 14, false);
	check_damage(run_wave_format, pcm);
}

TEST("media audio WAVEFORMATEX applies the AudioSpecificConfig the AAC tags carry")
{
	// Explicit HE-AAC after raw AAC's tags: the output rate, not the core's the structure states.
	const bytes he_aac = {0x2B, 0x11, 0x88, 0x00};
	CHECK_STR(wave(wave_format(0x00FF, 2, 24000, 16, he_aac)), "'aac' '0x00FF' audio: 48000 Hz, 2 ch, 0 int, 'HE-AAC'");
	CHECK_STR(wave(wave_format(0x1601, 2, 44100, 16, bytes{0x12, 0x10})), "'aac' '0x1601' audio: 44100 Hz, 2 ch, 0 int, 'LC'");
	CHECK_STR(wave(wave_format(0x706D, 2, 24000, 16, he_aac)), "'aac' '0x706D' audio: 48000 Hz, 2 ch, 0 int, 'HE-AAC'");

	// A config that does not read leaves the structure's figures; ADTS carries none.
	CHECK_STR(wave(wave_format(0x00FF, 2, 24000, 16, bytes{0x00, 0x00})), "'aac' '0x00FF' audio: 24000 Hz, 2 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x1600, 2, 44100, 16, bytes{0x12, 0x10})), "'aac' '0x1600' audio: 44100 Hz, 2 ch, 0 int, ''");

	// HEAACWAVEFORMAT: an HEAACWAVEINFO - payload type, profile, structure type and reserved - then the config.
	const auto info = [](const unsigned payload, const unsigned structure)
	{
		return builder().le16(payload).le16(0x29).le16(structure).le16(0).le32(0).b;
	};

	auto heaac = info(0, 0);
	heaac.insert(heaac.end(), he_aac.begin(), he_aac.end());
	CHECK_STR(wave(wave_format(0x1610, 2, 24000, 16, heaac)), "'aac' '0x1610' audio: 48000 Hz, 2 ch, 0 int, 'HE-AAC'");

	auto other_structure = info(0, 1);
	other_structure.insert(other_structure.end(), he_aac.begin(), he_aac.end());
	CHECK_STR(wave(wave_format(0x1610, 2, 24000, 16, other_structure)), "'aac' '0x1610' audio: 24000 Hz, 2 ch, 0 int, ''");

	// LATM, whose rate and channels are its core's: FFmpeg leaves them to the decoder.
	auto latm = info(3, 0);
	latm.insert(latm.end(), he_aac.begin(), he_aac.end());
	CHECK_STR(wave(wave_format(0x1610, 2, 24000, 16, latm)), "'aac_latm' '0x1610' audio: 0 Hz, 0 ch, 0 int, ''");
	CHECK_STR(wave(wave_format(0x1602, 2, 24000, 16)), "'aac_latm' '0x1602' audio: 0 Hz, 0 ch, 0 int, ''");

	check_damage(run_wave_format, wave_format(0x1610, 2, 24000, 16, heaac));
	check_prefixes(run_wave_format, wave_format(0x1610, 2, 24000, 16, heaac), 14, false);
}

TEST("media audio XMAWAVEFORMAT counts the channels of its streams")
{
	const auto xma = [](const unsigned streams, const unsigned written)
	{
		builder b;
		b.le16(0x0165).le16(16).le16(0).le16(0).le16(streams).u8(0).u8(3);

		for (unsigned i = 0; i < written; ++i)
		{
			b.le32(176400).le32(44100).le32(0).le32(0).u8(0).u8(i == 0 ? 2 : 1).le16(0);
		}

		return b.b;
	};

	CHECK_STR(wave(xma(2, 2)), "'xma1' '0x0165' audio: 44100 Hz, 3 ch, 0 int, ''");
	CHECK_STR(wave(xma(1, 1)), "'xma1' '0x0165' audio: 44100 Hz, 2 ch, 0 int, ''");
	CHECK_STR(wave(xma(0, 1)), "'xma1' '0x0165' audio: 44100 Hz, 0 ch, 0 int, ''");
	CHECK_STR(wave(xma(2, 1)), "refused");

	check_prefixes(run_wave_format, xma(2, 2), 52);
	check_damage(run_wave_format, xma(2, 2));
}

namespace
{
	bytes bitmap_info(const std::int32_t width, const std::int32_t height, const unsigned bit_count, const std::uint32_t compression,
	                  const std::uint32_t size = 40)
	{
		builder b;
		b.le32(size).le32(static_cast<std::uint32_t>(width)).le32(static_cast<std::uint32_t>(height)).le16(1).le16(bit_count);
		b.le32(compression).le32(0).le32(2835).le32(2835).le32(0).le32(0);
		return b.b;
	}

	std::string bitmap(const bytes& data)
	{
		media_stream s;
		return libfiles::detail::read_bitmap_info(data, s) ? describe(s) : "refused";
	}
}

TEST("media audio BITMAPINFOHEADER gives the codec, size and the sampling its format fixes")
{
	CHECK_STR(bitmap(bitmap_info(640, 480, 12, le_fourcc("XVID"))), "'mpeg4' 'XVID' video: 640x480 yuv420");
	CHECK_STR(bitmap(bitmap_info(640, 480, 12, le_fourcc("xvid"))), "'mpeg4' 'xvid' video: 640x480 yuv420");
	CHECK_STR(bitmap(bitmap_info(1920, 1080, 24, le_fourcc("H264"))), "'h264' 'H264' video: 1920x1080 ");
	CHECK_STR(bitmap(bitmap_info(854, 480, 24, le_fourcc("WMV3"))), "'wmv3' 'WMV3' video: 854x480 yuv420");
	CHECK_STR(bitmap(bitmap_info(320, 240, 24, le_fourcc("cvid"))), "'cinepak' 'cvid' video: 320x240 yuv420");
	CHECK_STR(bitmap(bitmap_info(320, 240, 24, le_fourcc("IV50"))), "'indeo5' 'IV50' video: 320x240 yuv410");
	CHECK_STR(bitmap(bitmap_info(720, 576, 24, le_fourcc("dvsd"))), "'dv' 'dvsd' video: 720x576 ");
	CHECK_STR(bitmap(bitmap_info(640, 480, 24, le_fourcc("MJPG"))), "'mjpeg' 'MJPG' video: 640x480 ");

	// Uncompressed: a negative height is a picture stored top-down.
	CHECK_STR(bitmap(bitmap_info(320, -240, 24, 0)), "'rawvideo' '0x00000000' video: 320x240 rgb");
	CHECK_STR(bitmap(bitmap_info(320, 240, 32, 0)), "'rawvideo' '0x00000000' video: 320x240 rgb");
	CHECK_STR(bitmap(bitmap_info(320, 240, 32, 3)), "'rawvideo' '0x00000003' video: 320x240 rgb");
	CHECK_STR(bitmap(bitmap_info(320, 240, 8, 0)), "'rawvideo' '0x00000000' video: 320x240 ");
	CHECK_STR(bitmap(bitmap_info(320, 240, 8, 1)), "'msrle' '0x00000001' video: 320x240 ");
	CHECK_STR(bitmap(bitmap_info(320, 240, 4, 2)), "'msrle' '0x00000002' video: 320x240 ");

	media_stream s;
	CHECK(libfiles::detail::read_bitmap_info(bitmap_info(320, 240, 16, 0), s));
	CHECK(s.video.chroma == chroma_format::rgb);
	CHECK_EQ(s.video.bit_depth, 5);
	CHECK(libfiles::detail::read_bitmap_info(bitmap_info(320, 240, 16, 3), s));
	CHECK(s.video.chroma == chroma_format::rgb);
	CHECK_EQ(s.video.bit_depth, 0);

	// Uncompressed by FourCC.
	struct raw
	{
		const char* fourcc;
		const char* format;
	};

	for (const auto& [fourcc, format] : {raw{"YUY2", "yuv422"}, raw{"UYVY", "yuv422"}, raw{"YV12", "yuv420"}, raw{"I420", "yuv420"},
	                                     raw{"IYUV", "yuv420"}, raw{"NV12", "yuv420"}, raw{"Y800", "gray"}, raw{"GREY", "gray"},
	                                     raw{"YV16", "yuv422"}, raw{"YV24", "yuv444"}, raw{"Y41B", "yuv411"}, raw{"YVU9", "yuv410"},
	                                     raw{"AYUV", "yuva444"}, raw{"I40A", "yuva420"}, raw{"Y210", "yuv422 10bit"}, raw{"Y416", "yuv444 16bit"},
	                                     raw{"J420", "yuv420 full"}, raw{"I440", "yuv440"}, raw{"RGBA", "rgba"}, raw{"yuv2", "yuv422"}})
	{
		CHECK_STR(bitmap(bitmap_info(64, 64, 16, le_fourcc(fourcc))), "'rawvideo' '" + std::string(fourcc) + "' video: 64x64 " + format);
	}

	// A FourCC no table holds keeps its name.
	CHECK_STR(bitmap(bitmap_info(64, 64, 24, le_fourcc("ABCD"))), "'' 'ABCD' video: 64x64 ");
	CHECK_STR(bitmap(bitmap_info(64, 64, 24, 0x12345678)), "'' '0x12345678' video: 64x64 ");

	// Sizes past what a scan reports are unknown; a later header's larger biSize is no matter.
	CHECK_STR(bitmap(bitmap_info(0, 480, 12, le_fourcc("XVID"))), "'mpeg4' 'XVID' video: 0x480 yuv420");
	CHECK_STR(bitmap(bitmap_info(-640, 480, 12, le_fourcc("XVID"))), "'mpeg4' 'XVID' video: 0x480 yuv420");
	CHECK_STR(bitmap(bitmap_info(65536, 480, 12, le_fourcc("XVID"))), "'mpeg4' 'XVID' video: 0x480 yuv420");
	CHECK_STR(bitmap(bitmap_info(640, std::numeric_limits<std::int32_t>::min(), 12, le_fourcc("XVID"))), "'mpeg4' 'XVID' video: 640x0 yuv420");
	CHECK_STR(bitmap(bitmap_info(640, 480, 12, le_fourcc("XVID"), 108)), "'mpeg4' 'XVID' video: 640x480 yuv420");

	// A header smaller than BITMAPINFOHEADER, or cut short.
	auto untouched = sentinel_stream();
	CHECK(!libfiles::detail::read_bitmap_info(bitmap_info(640, 480, 12, le_fourcc("XVID"), 39), untouched));
	const auto whole = bitmap_info(640, 480, 12, le_fourcc("XVID"));
	CHECK(!libfiles::detail::read_bitmap_info(bytes(whole.begin(), whole.begin() + 39), untouched));
	CHECK_STR(describe(untouched), describe(sentinel_stream()));

	check_prefixes(run_bitmap_info, whole, 40);
	check_damage(run_bitmap_info, whole);
}

TEST("media audio codec tables name FourCCs, WAVE tags and object types")
{
	using libfiles::detail::codec_from_fourcc;
	using libfiles::detail::codec_from_object_type;
	using libfiles::detail::codec_from_wave_tag;

	struct named
	{
		const char* fourcc;
		const char* codec;
	};

	for (const auto& [fourcc, codec] : {
		     named{"H264", "h264"}, named{"h264", "h264"}, named{"avc1", "h264"}, named{"AVC1", "h264"}, named{"X264", "h264"},
		     named{"DAVC", "h264"}, named{"HEVC", "hevc"}, named{"H265", "hevc"}, named{"HVC1", "hevc"}, named{"hvc1", "hevc"},
		     named{"XVID", "mpeg4"}, named{"xvid", "mpeg4"}, named{"DIVX", "mpeg4"}, named{"DX50", "mpeg4"}, named{"FMP4", "mpeg4"},
		     named{"MP4V", "mpeg4"}, named{"3IV2", "mpeg4"}, named{"DIV3", "msmpeg4v3"}, named{"MP43", "msmpeg4v3"},
		     named{"MPG3", "msmpeg4v3"}, named{"div3", "msmpeg4v3"}, named{"MP42", "msmpeg4v2"}, named{"MP41", "msmpeg4v1"},
		     named{"MPG4", "msmpeg4v1"}, named{"WMV1", "wmv1"}, named{"WMV2", "wmv2"}, named{"WMV3", "wmv3"}, named{"WVC1", "vc1"},
		     named{"WMVA", "vc1"}, named{"MJPG", "mjpeg"}, named{"AVRn", "mjpeg"}, named{"AVRN", "mjpeg"}, named{"LJPG", "mjpeg"},
		     named{"dmb1", "mjpeg"}, named{"DMB1", "mjpeg"}, named{"MJLS", "jpegls"}, named{"mjls", "jpegls"}, named{"dvsd", "dv"},
		     named{"dv25", "dv"}, named{"dv50", "dv"}, named{"dvhd", "dv"}, named{"dvsl", "dv"}, named{"CDVC", "dv"},
		     named{"mpg1", "mpeg1"}, named{"mpg2", "mpeg2"}, named{"MPEG", "mpeg2"}, named{"H263", "h263"}, named{"U263", "h263"},
		     named{"I263", "h263i"}, named{"VP80", "vp8"}, named{"VP90", "vp9"}, named{"AV01", "av1"}, named{"cvid", "cinepak"},
		     named{"MSVC", "msvideo1"}, named{"CRAM", "msvideo1"}, named{"WHAM", "msvideo1"}, named{"IV31", "indeo3"},
		     named{"IV32", "indeo3"}, named{"IV41", "indeo4"}, named{"IV50", "indeo5"}, named{"HFYU", "huffyuv"},
		     named{"FFVH", "ffvhuff"}, named{"FFV1", "ffv1"}, named{"tscc", "tscc"}, named{"YUY2", "rawvideo"}, named{"Y8  ", "rawvideo"},
		     named{"FLV1", "flv1"}, named{"VP62", "vp6"}, named{"theo", "theora"}, named{"SVQ3", "svq3"}, named{"RV40", "rv40"},
		     named{"ULY2", "utvideo"}, named{"MAGY", "magicyuv"}, named{"v210", "v210"}, named{"rpza", "rpza"}, named{"IMM5", "imm5"},
		     named{"IMM6", "imm6"}})
	{
		CHECK_STR(std::string(codec_from_fourcc(le_fourcc(fourcc))), codec);
	}

	// BITMAPINFOHEADER's compression values, and the MPEG FourCCs written as numbers.
	CHECK_STR(std::string(codec_from_fourcc(0)), "rawvideo");
	CHECK_STR(std::string(codec_from_fourcc(1)), "msrle");
	CHECK_STR(std::string(codec_from_fourcc(3)), "rawvideo");
	CHECK_STR(std::string(codec_from_fourcc(0x10000001)), "mpeg1");
	CHECK_STR(std::string(codec_from_fourcc(0x10000002)), "mpeg2");
	CHECK(codec_from_fourcc(le_fourcc("ABCD")).empty());
	CHECK(codec_from_fourcc(0xFFFFFFFF).empty());
	CHECK(codec_from_fourcc(0x20202020).empty());

	struct tagged
	{
		std::uint16_t tag;
		const char* codec;
	};

	for (const auto& [tag, codec] : {
		     tagged{0x0001, "pcm"}, tagged{0x0003, "pcm"}, tagged{0x0002, "adpcm_ms"}, tagged{0x0006, "alaw"}, tagged{0x0007, "mulaw"},
		     tagged{0x0011, "adpcm_ima_wav"}, tagged{0x0031, "gsm_ms"}, tagged{0x0050, "mp2"}, tagged{0x0055, "mp3"},
		     tagged{0x00FF, "aac"}, tagged{0x1600, "aac"}, tagged{0x1601, "aac"}, tagged{0x1610, "aac"}, tagged{0x4143, "aac"},
		     tagged{0x706D, "aac"}, tagged{0x0160, "wmav1"}, tagged{0x0161, "wmav2"}, tagged{0x0162, "wmapro"},
		     tagged{0x0163, "wmalossless"}, tagged{0x000A, "wmavoice"}, tagged{0x2000, "ac3"}, tagged{0x2001, "dts"},
		     tagged{0xF1AC, "flac"}, tagged{0x674F, "vorbis"}, tagged{0x6750, "vorbis"}, tagged{0x6771, "vorbis"},
		     tagged{0x0057, "amr_nb"}, tagged{0x0058, "amr_wb"}, tagged{0xA109, "speex"}, tagged{0x1602, "aac_latm"},
		     tagged{0x0400, "adpcm_citrix"}, tagged{0x0401, "imc"}, tagged{0x0402, "iac"}})
	{
		CHECK_STR(std::string(codec_from_wave_tag(tag)), codec);
	}

	CHECK(codec_from_wave_tag(0x0000).empty());
	CHECK(codec_from_wave_tag(0xFFFE).empty());
	CHECK(codec_from_wave_tag(0x1234).empty());

	struct typed
	{
		std::uint8_t type;
		const char* codec;
	};

	for (const auto& [type, codec] : {
		     typed{0x20, "mpeg4"}, typed{0x21, "h264"}, typed{0x23, "hevc"}, typed{0x40, "aac"}, typed{0x60, "mpeg2"}, typed{0x65, "mpeg2"},
		     typed{0x66, "aac"}, typed{0x67, "aac"}, typed{0x68, "aac"}, typed{0x69, "mp3"}, typed{0x6A, "mpeg1"}, typed{0x6B, "mp3"},
		     typed{0x6C, "mjpeg"}, typed{0x6D, "png"}, typed{0x6E, "jpeg2000"}, typed{0xA3, "vc1"}, typed{0xA4, "dirac"},
		     typed{0xA5, "ac3"}, typed{0xA6, "eac3"}, typed{0xA9, "dts"}, typed{0xAD, "opus"}, typed{0xB1, "vp9"}, typed{0xDD, "vorbis"}})
	{
		CHECK_STR(std::string(codec_from_object_type(type)), codec);
	}

	CHECK(codec_from_object_type(0x00).empty());
	CHECK(codec_from_object_type(0x22).empty());
	CHECK(codec_from_object_type(0xFF).empty());
}

TEST("media audio fixed video formats are known only where every stream shares them")
{
	using libfiles::detail::fixed_video_format;

	struct fixed
	{
		const char* codec;
		const char* format;
	};

	for (const auto& [codec, format] : {
		     fixed{"vp8", "yuv420"}, fixed{"h263", "yuv420"}, fixed{"flv1", "yuv420"}, fixed{"mpeg1", "yuv420"}, fixed{"mpeg4", "yuv420"},
		     fixed{"msmpeg4v3", "yuv420"}, fixed{"wmv1", "yuv420"}, fixed{"wmv2", "yuv420"}, fixed{"wmv3", "yuv420"}, fixed{"vc1", "yuv420"},
		     fixed{"cinepak", "yuv420"}, fixed{"svq1", "yuv410"}, fixed{"svq3", "yuv420"}, fixed{"indeo3", "yuv410"},
		     fixed{"indeo5", "yuv410"}, fixed{"vp6a", "yuva420"}, fixed{"v210", "yuv422 10bit"}, fixed{"r210", "rgb 10bit"}})
	{
		video_header h;
		CHECK(fixed_video_format(codec, h));
		libfiles::video_format v;
		libfiles::detail::apply_video_header(h, v);
		CHECK_STR(v.pixel_format(), format);
		CHECK(h.bit_depth >= 8);
	}

	for (const auto codec : {"h264", "hevc", "av1", "vp9", "mpeg2", "prores", "theora", "ffv1", "huffyuv", "mjpeg", "dv", "rawvideo", ""})
	{
		video_header h;
		h.chroma = chroma_format::yuv444;
		h.bit_depth = 12;
		CHECK(!fixed_video_format(codec, h));
		CHECK(h.chroma == chroma_format::yuv444);
		CHECK_EQ(h.bit_depth, 12);
	}
}
