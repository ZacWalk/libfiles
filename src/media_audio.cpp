// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The audio codec headers the media scanners read - AAC, MPEG audio, AC-3 and E-AC-3, DTS, Opus,
// Vorbis, FLAC, ALAC and Speex - the Windows structures that describe a stream in AVI, ASF, WAV and
// Matroska, and the tables that name a codec from the identifiers containers give it.

#include "media_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace libfiles::detail
{
	namespace
	{
		// ---- AAC ----------------------------------------------------------------------------------------

		// The ISO 14496-3 audio object types the configs below are read by.
		enum : std::uint32_t
		{
			aot_main = 1,
			aot_lc = 2,
			aot_ssr = 3,
			aot_ltp = 4,
			aot_sbr = 5,
			aot_scalable = 6,
			aot_er_lc = 17,
			aot_er_ltp = 19,
			aot_er_scalable = 20,
			aot_er_bsac = 22,
			aot_er_ld = 23,
			aot_ps = 29,
			aot_er_eld = 39,
			aot_usac = 42,
		};

		constexpr std::uint64_t object_types(const std::initializer_list<std::uint32_t> types)
		{
			std::uint64_t set = 0;
			for (const auto type : types) set |= 1ull << type;
			return set;
		}

		// The object types a GASpecificConfig follows; the error-resilient ones, whose config ends with an
		// epConfig; and those an HE-AAC stream's AAC core may have.
		constexpr auto general_audio_types = object_types({1, 2, 3, 4, 6, 7, 17, 19, 20, 21, 22, 23});
		constexpr auto error_resilient_types = object_types({17, 19, 20, 21, 22, 23, 24, 25, 26, 27, 39});
		constexpr auto aac_core_types = object_types({1, 2, 3, 4, 6, 17, 19, 20, 23});

		constexpr bool is_one_of(const std::uint64_t set, const std::uint32_t type)
		{
			return type < 64 && (set >> type & 1) != 0;
		}

		constexpr bool reserved_object_type(const std::uint32_t type)
		{
			return type == 0 || type == 10 || type == 11 || type == 18 || type > 46;
		}

		constexpr std::uint32_t aac_sample_rates[16] = {
			96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0,
		};

		// USAC fills some of the indices AAC reserves; 31 escapes to an explicit rate.
		constexpr std::uint32_t usac_sample_rates[32] = {
			96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 57600,
			51200, 40000, 38400, 34150, 28800, 25600, 20000, 19200, 17075, 14400, 12800, 9600, 0, 0, 0, 0,
		};

		// The channels a channelConfiguration stands for. Zero for 0, whose program_config_element counts
		// them, and for the reserved 8 to 10 and 15.
		constexpr std::uint8_t aac_channels[16] = {0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 24, 8, 0};

		std::string_view aac_profile(const std::uint32_t type)
		{
			switch (type)
			{
			case aot_main: return "Main";
			case aot_lc: return "LC";
			case aot_ssr: return "SSR";
			case aot_ltp: return "LTP";
			case aot_er_ld: return "LD";
			case aot_er_eld: return "ELD";
			case aot_usac: return "xHE-AAC";
			default: return {};
			}
		}

		std::uint32_t read_object_type(bit_reader& b)
		{
			const auto type = b.u(5);
			return type == 31 ? 32 + b.u(6) : type;
		}

		// A samplingFrequencyIndex, or the 24-bit frequency 15 escapes to. Zero for a reserved index.
		std::uint32_t read_sampling_frequency(bit_reader& b)
		{
			const auto index = b.u(4);
			return index == 15 ? b.u(24) : aac_sample_rates[index];
		}

		// USAC's escapedValue(): a field that grows by a second, then a third, when the one before is full.
		std::uint32_t read_escaped_value(bit_reader& b, const unsigned first, const unsigned second, const unsigned third)
		{
			auto value = b.u(first);
			if (value != (1u << first) - 1) return value;

			const auto more = b.u(second);
			value += more;
			if (more == (1u << second) - 1) value += b.u(third);
			return value;
		}

		// A program_config_element's output channels: one for each single channel and LFE element, two for
		// each channel pair; coupling channels are not output. Its byte alignment counts from the start of
		// the reader's data, where the AudioSpecificConfig or the raw data block that holds it begins.
		std::uint32_t read_program_config_channels(bit_reader& b)
		{
			b.skip(4 + 2 + 4);
			const auto front = b.u(4);
			const auto side = b.u(4);
			const auto back = b.u(4);
			const auto lfe = b.u(2);
			const auto data = b.u(3);
			const auto coupling = b.u(4);

			if (b.flag()) b.skip(4);
			if (b.flag()) b.skip(4);
			if (b.flag()) b.skip(3);

			auto channels = lfe;

			for (auto i = front + side + back; i > 0; --i)
			{
				channels += b.flag() ? 2u : 1u;
				b.skip(4);
			}

			b.skip(4 * (lfe + data) + 5 * coupling);
			b.align();
			b.skip(8 * b.u(8));
			return channels;
		}

		// A GASpecificConfig: the frame length, and the channels of the program_config_element that a
		// channelConfiguration of 0 brings.
		bool read_ga_specific_config(bit_reader& b, const std::uint32_t type, const std::uint32_t channel_configuration,
		                             std::uint32_t& channels, std::uint32_t& frame_length)
		{
			const auto short_frames = b.flag();
			frame_length = type == aot_er_ld ? (short_frames ? 480u : 512u) : (short_frames ? 960u : 1024u);
			if (b.flag()) b.skip(14);
			const auto extension = b.flag();
			if (channel_configuration == 0) channels = read_program_config_channels(b);
			if (type == aot_scalable || type == aot_er_scalable) b.skip(3);

			if (extension)
			{
				if (type == aot_er_bsac) b.skip(5 + 11);
				if (type == aot_er_lc || type == aot_er_ltp || type == aot_er_scalable || type == aot_er_ld) b.skip(3);
				b.skip(1);
			}

			return !b.overrun();
		}

		// A UsacConfig's rate, channels and output frame length; its decoder configuration is not read.
		bool read_usac_config(bit_reader& b, audio_header& h)
		{
			constexpr std::uint32_t output_frame_lengths[5] = {768, 1024, 2048, 2048, 4096};

			const auto index = b.u(5);
			const auto rate = index == 31 ? b.u(24) : usac_sample_rates[index];
			const auto core_sbr_frame_length = b.u(3);
			const auto channel_configuration = b.u(5);
			std::uint32_t channels = 0;

			if (channel_configuration == 0)
			{
				channels = read_escaped_value(b, 5, 8, 16);
			}
			else if (channel_configuration < 16)
			{
				channels = aac_channels[channel_configuration];
			}

			if (b.overrun() || rate == 0 || core_sbr_frame_length > 4 || channels > 64) return false;

			h.sample_rate = rate;
			h.channels = channels;
			h.samples_per_frame = output_frame_lengths[core_sbr_frame_length];
			return true;
		}

		// The extension an encoder appends to a config that older decoders read as plain AAC: SBR, and PS
		// with it, signalled after a sync word rather than by the object type. Taken whole or not at all.
		void read_sync_extension(bit_reader b, std::uint32_t& extension_rate, bool& sbr, bool& ps)
		{
			if (b.u(11) != 0x2B7) return;

			const auto type = read_object_type(b);
			if ((type != aot_sbr && type != aot_er_bsac) || !b.flag()) return;

			const auto rate = read_sampling_frequency(b);
			auto parametric_stereo = false;

			if (type == aot_sbr)
			{
				if (b.remaining() >= 12 && b.u(11) == 0x548) parametric_stereo = b.flag();
			}
			else
			{
				b.skip(4);
			}

			if (b.overrun() || rate == 0) return;
			extension_rate = rate;
			sbr = true;
			ps = parametric_stereo;
		}

		// ---- AC-3 and E-AC-3 ----------------------------------------------------------------------------

		constexpr std::uint32_t ac3_sample_rates[3] = {48000, 44100, 32000};
		constexpr std::uint32_t ac3_bit_rates[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
		constexpr std::uint32_t eac3_blocks[4] = {1, 2, 3, 6};

		// Channel locations as E-AC-3's custom channel map numbers them - left, centre, right, the
		// surrounds, then the pairs and singles past 5.1, LFE last - so that the channels a dependent
		// substream adds are counted against those its independent substream's acmod and lfeon give.
		constexpr std::uint16_t location_l = 1 << 0;
		constexpr std::uint16_t location_c = 1 << 1;
		constexpr std::uint16_t location_r = 1 << 2;
		constexpr std::uint16_t location_ls = 1 << 3;
		constexpr std::uint16_t location_rs = 1 << 4;
		constexpr std::uint16_t location_cs = 1 << 7;
		constexpr std::uint16_t location_lfe2 = 1 << 14;
		constexpr std::uint16_t location_lfe = 1 << 15;
		constexpr std::uint16_t location_pairs = 1 << 5 | 1 << 6 | 1 << 9 | 1 << 10 | 1 << 11 | 1 << 13;

		// acmod 0 is two independent mono channels.
		constexpr std::uint16_t acmod_locations[8] = {
			location_l | location_r,
			location_c,
			location_l | location_r,
			location_l | location_c | location_r,
			location_l | location_r | location_cs,
			location_l | location_c | location_r | location_cs,
			location_l | location_r | location_ls | location_rs,
			location_l | location_c | location_r | location_ls | location_rs,
		};

		std::uint32_t location_channels(const std::uint16_t locations)
		{
			return static_cast<std::uint32_t>(std::popcount(locations) +
				std::popcount(static_cast<std::uint16_t>(locations & location_pairs)));
		}

		std::uint16_t standard_locations(const std::uint32_t acmod, const bool lfe)
		{
			return static_cast<std::uint16_t>(acmod_locations[acmod] | (lfe ? location_lfe : 0));
		}

		// chanmap's first bit, its most significant, is location 0.
		std::uint16_t chanmap_locations(const std::uint32_t chanmap)
		{
			std::uint16_t locations = 0;

			for (auto i = 0; i < 16; ++i)
			{
				if (chanmap >> (15 - i) & 1) locations = static_cast<std::uint16_t>(locations | 1u << i);
			}

			return locations;
		}

		// dec3's chan_loc: bits 0 to 7, least significant first, are locations 5 to 12, and bit 8 is LFE2,
		// as Diffractor's FFmpeg writes them.
		std::uint16_t chan_loc_locations(const std::uint32_t chan_loc)
		{
			std::uint16_t locations = 0;

			for (auto bit = 0; bit < 8; ++bit)
			{
				if (chan_loc >> bit & 1) locations = static_cast<std::uint16_t>(locations | 1u << (5 + bit));
			}

			if (chan_loc >> 8 & 1) locations |= location_lfe2;
			return locations;
		}

		struct ac3_sync_frame
		{
			bool enhanced = false;
			// E-AC-3's strmtyp: 0 independent, 1 dependent, 2 independent and converted from AC-3.
			std::uint32_t stream_type = 0;
			std::uint32_t substream = 0;
			std::uint32_t bytes = 0;
			std::uint32_t sample_rate = 0;
			std::uint32_t blocks = 6;
			std::uint32_t bit_rate = 0;
			std::uint16_t locations = 0;
		};

		bool read_ac3_sync_frame(const std::span<const std::uint8_t> data, ac3_sync_frame& f)
		{
			if (data.size() < 6 || data[0] != 0x0B || data[1] != 0x77) return false;

			// bsid lies in the same bits in both syntaxes, and tells them apart.
			const std::uint32_t bsid = data[5] >> 3;
			bit_reader b(data.subspan(2));

			if (bsid <= 10)
			{
				b.skip(16);
				const auto fscod = b.u(2);
				const auto frmsizecod = b.u(6);
				b.skip(5 + 3);
				const auto acmod = b.u(3);
				if ((acmod & 1) != 0 && acmod != 1) b.skip(2);
				if ((acmod & 4) != 0) b.skip(2);
				if (acmod == 2) b.skip(2);
				const auto lfe = b.flag();
				if (b.overrun() || fscod == 3 || frmsizecod > 37) return false;

				// bsid 9 and 10 halve and quarter the rate.
				const auto shift = bsid > 8 ? bsid - 8 : 0;
				const auto kbps = ac3_bit_rates[frmsizecod / 2];
				// Two-byte words: a 44.1 kHz frame rounds down, and the odd codes add one to it.
				const auto words = fscod == 0 ? 2 * kbps : fscod == 2 ? 3 * kbps : 96000 * kbps / 44100 + (frmsizecod & 1);

				f = {};
				f.bytes = 2 * words;
				f.sample_rate = ac3_sample_rates[fscod] >> shift;
				f.bit_rate = kbps * 1000 >> shift;
				f.locations = standard_locations(acmod, lfe);
				return true;
			}

			if (bsid > 16) return false;

			const auto stream_type = b.u(2);
			const auto substream = b.u(3);
			const auto bytes = 2 * (b.u(11) + 1);
			const auto fscod = b.u(2);
			std::uint32_t rate = 0;
			std::uint32_t blocks = 6;

			if (fscod == 3)
			{
				const auto reduced = b.u(2);
				if (reduced == 3) return false;
				rate = ac3_sample_rates[reduced] / 2;
			}
			else
			{
				blocks = eac3_blocks[b.u(2)];
				rate = ac3_sample_rates[fscod];
			}

			const auto acmod = b.u(3);
			const auto lfe = b.flag();
			auto locations = standard_locations(acmod, lfe);
			b.skip(5);

			if (stream_type == 1)
			{
				b.skip(5);
				if (b.flag()) b.skip(8);

				if (acmod == 0)
				{
					b.skip(5);
					if (b.flag()) b.skip(8);
				}

				if (b.flag()) locations = chanmap_locations(b.u(16));
			}

			if (b.overrun() || stream_type == 3 || bytes < 7) return false;

			f = {};
			f.enhanced = true;
			f.stream_type = stream_type;
			f.substream = substream;
			f.bytes = bytes;
			f.sample_rate = rate;
			f.blocks = blocks;
			f.bit_rate = static_cast<std::uint32_t>(8ull * bytes * rate / (256ull * blocks));
			f.locations = locations;
			return true;
		}

		// ---- DTS ----------------------------------------------------------------------------------------

		constexpr std::uint8_t dts_channels[16] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
		constexpr std::uint32_t dts_sample_rates[16] = {
			0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 96000, 192000,
		};

		// RATE's nominal bit rates; the last three codes are open, variable and lossless, and say none.
		constexpr std::uint32_t dts_bit_rates[32] = {
			32000, 56000, 64000, 96000, 112000, 128000, 192000, 224000, 256000, 320000, 384000, 448000, 512000,
			576000, 640000, 768000, 896000, 1024000, 1152000, 1280000, 1344000, 1408000, 1411200, 1472000, 1536000,
			1920000, 2048000, 3072000, 3840000, 0, 0, 0,
		};

		// The start of a DTS core frame as the 16-bit big-endian form holds it. Streams are also stored
		// with their words little-endian, and with 14 bits in each word - as CD audio carries DTS - either
		// way; FFmpeg accepts the 14-bit forms only where the bits after the sync word begin a normal frame.
		std::size_t dts_core_start(const std::span<const std::uint8_t> data, std::array<std::uint8_t, 16>& start,
		                           bool& fourteen_bit)
		{
			if (data.size() < 6) return 0;

			const auto signature = load_be32(data.data());
			const auto little = signature == 0xFE7F0180 || signature == 0xFF1F00E8;
			fourteen_bit = signature == 0x1FFFE800 || signature == 0xFF1F00E8;

			if (signature == 0x1FFFE800 && (data[4] != 0x07 || (data[5] & 0xF0) != 0xF0)) return 0;
			if (signature == 0xFF1F00E8 && ((data[4] & 0xF0) != 0xF0 || data[5] != 0x07)) return 0;
			if (signature != 0x7FFE8001 && !little && !fourteen_bit) return 0;

			std::size_t count = 0;
			std::uint32_t pending = 0;
			unsigned pending_bits = 0;

			for (std::size_t at = 0; at + 1 < data.size() && count < start.size(); at += 2)
			{
				const std::uint32_t word = little ? load_le16(data.data() + at) : load_be16(data.data() + at);

				if (!fourteen_bit)
				{
					start[count++] = static_cast<std::uint8_t>(word >> 8);
					start[count++] = static_cast<std::uint8_t>(word);
					continue;
				}

				pending = (pending << 14 | (word & 0x3FFF)) & 0x3FFFFF;
				pending_bits += 14;

				while (pending_bits >= 8 && count < start.size())
				{
					pending_bits -= 8;
					start[count++] = static_cast<std::uint8_t>(pending >> pending_bits);
				}
			}

			return count;
		}

		// ---- Windows structures -------------------------------------------------------------------------

		constexpr std::uint32_t le_tag(const char (&s)[5])
		{
			return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24;
		}

		constexpr std::uint32_t upper_fourcc(const std::uint32_t fourcc)
		{
			std::uint32_t upper = 0;

			for (auto shift = 0; shift < 32; shift += 8)
			{
				auto c = fourcc >> shift & 0xFF;
				if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
				upper |= c << shift;
			}

			return upper;
		}

		std::string wave_tag_text(const std::uint32_t tag)
		{
			constexpr char digits[] = "0123456789ABCDEF";
			std::string text = "0x";
			for (auto shift = 12; shift >= 0; shift -= 4) text += digits[tag >> shift & 15];
			return text;
		}

		struct fourcc_group
		{
			std::string_view codec;
			// FourCCs four characters each, as files store them, in FFmpeg's order where two would match.
			std::string_view fourccs;
		};

		// The FourCCs FFmpeg reads AVI, ASF and Matroska's VfW video by, with HEVC's unofficial ones.
		constexpr fourcc_group fourcc_groups[] = {
			{"h264", "H264h264X264x264avc1DAVCSMV2VSSHQ264V264GAVCUMSVtshdINMC"},
			{"hevc", "HEVCH265HVC1"},
			{"h263", "H263X263T263L263VX1KZyGoM263lsvmU263VSM4"},
			{"h263i", "I263"},
			{"h261", "H261"},
			{"mpeg4", "FMP4DIVXDX50XVIDMP4SM4S2ZMP4DIV1BLZ0mp4vUMP4WV1FSEDGRMP43IV2WAWVFFDSFVFWDCODMVXMPM4VSMP4"
			          "DXGMVIDMM4T3GEOXG264HDX4DM4VDMK2DYM4DIGIEPHVEM4AM4CCSN40VSPXULDXGEOVSIPPSM4VXVIXDreXQMP4PLV1"
			          "GLV4GMP4MNM4GTM4"},
			{"msmpeg4v3", "MP43DIV3MPG3DIV5DIV6DIV4DVX3AP41COL1COL0"},
			{"msmpeg4v2", "MP42DIV2"},
			{"msmpeg4v1", "MPG4MP41"},
			{"wmv1", "WMV1"},
			{"wmv2", "WMV2GXVE"},
			{"wmv3", "WMV3"},
			{"wmv3image", "WMVP"},
			{"vc1", "WVC1WMVA"},
			{"vc1image", "WVP2"},
			{"dv", "dvsddvhddvh1dvsldv25dv50cdvcCDVHCDV5dvc dvcsdvispdvcSL25SLDVAVd1"},
			{"mpeg1", "mpg1PIM1BW10XMPG"},
			{"mpeg2", "mpg2MPEGPIM2VCR2DVR MMESLMP2slifEM2VM701M702M703M704M705mpgv"},
			{"mjpeg", "MJPGMSC2LJPGdmb1mjpaJR24JPGLjpegIJPGAVRnACDVQIVGSLMJCJPGIJLVMVJPAVI1AVI2MTSJZJPGMMJP"},
			{"jpegls", "MJLS"},
			{"huffyuv", "HFYU"},
			{"ffvhuff", "FFVH"},
			{"cyuv", "CYUV"},
			{"rawvideo", "I420YUY2Y210Y216Y416Y422V422Y410YUNVUYNVUYNYuyv12Vu12vuyyuvsyuv2P422YV12YV16YV24UYVYVYUY"
			             "IYUVAYUVY800Y8  HDYCYVU9VDTZY411NV12NV21Y41BY42BYUV9auv2YVYUYUYVI410I411I422I440I444J420J422"
			             "J440J444YUVAI40AI42ARGB2RV15RV16RV24RV32RGBAAV32GREYI09LI09BI29LI29BI49LI49BI0ALI0ABI2ALI2AB"
			             "I4ALI4ABI4FLI4FBI0CLI0CBI2CLI2CBI4CLI4CBI0FLI0FBv308v408v410y408"},
			{"frwu", "FRWU"},
			{"r10k", "R10k"},
			{"r210", "r210"},
			{"v210", "v210C210"},
			{"yuv4", "yuv4"},
			{"indeo3", "IV31IV32"},
			{"indeo4", "IV41"},
			{"indeo5", "IV50"},
			{"vp3", "VP31VP30"},
			{"vp4", "VP40"},
			{"vp5", "VP50"},
			{"vp6", "VP60VP61VP62"},
			{"vp6a", "VP6A"},
			{"vp6f", "VP6FFLV4"},
			{"vp7", "VP70VP71"},
			{"vp8", "VP80"},
			{"vp9", "VP90"},
			{"asv1", "ASV1"},
			{"asv2", "ASV2"},
			{"vcr1", "VCR1"},
			{"ffv1", "FFV1"},
			{"xan_wc4", "Xxan"},
			{"mimic", "LM20"},
			{"msrle", "mrle"},
			{"msvideo1", "MSVCmsvcCRAMcramWHAMwham"},
			{"cinepak", "cvid"},
			{"truemotion1", "DUCKPVEZ"},
			{"mszh", "MSZH"},
			{"zlib", "ZLIB"},
			{"snow", "SNOW"},
			{"4xm", "4XMV"},
			{"flv1", "FLV1S263"},
			{"flashsv", "FSV1"},
			{"svq1", "svq1"},
			{"tscc", "tscc"},
			{"ulti", "ULTI"},
			{"vixl", "VIXL"},
			{"qpeg", "QPEGQ1.0Q1.1"},
			{"loco", "LOCO"},
			{"wnv1", "WNV1YUV8"},
			{"aasc", "AAS4AASC"},
			{"indeo2", "RT21"},
			{"fraps", "FPS1"},
			{"theora", "theo"},
			{"truemotion2", "TM20"},
			{"truemotion2rt", "TR20"},
			{"cscd", "CSCD"},
			{"zmbv", "ZMBV"},
			{"kmvc", "KMVC"},
			{"cavs", "CAVS"},
			{"avs2", "AVS2"},
			{"jpeg2000", "mjp2MJ2CLJ2CLJ2KIPJ2AVj2"},
			{"vmnc", "VMnc"},
			{"targa", "tga "},
			{"png", "MPNGPNG1png "},
			{"cljr", "CLJR"},
			{"dirac", "drac"},
			{"rpza", "azprRPZArpza"},
			{"sp5x", "SP54"},
			{"aura", "AURA"},
			{"aura2", "AUR2"},
			{"dpx", "dpx "},
			{"kgv1", "KGV1"},
			{"lagarith", "LAGS"},
			{"amv", "AMVF"},
			{"utvideo", "ULRAULRGULY0ULY2ULY4ULH0ULH2ULH4UQY0UQY2UQRAUQRGUMY2UMH2UMY4UMH4UMRAUMRG"},
			{"vble", "VBLE"},
			{"escape130", "E130"},
			{"dxtory", "xtor"},
			{"zerocodec", "ZECO"},
			{"y41p", "Y41P"},
			{"flic", "AFLC"},
			{"mss1", "MSS1"},
			{"msa1", "MSA1"},
			{"tscc2", "TSC2"},
			{"mts2", "MTS2"},
			{"cllc", "CLLC"},
			{"mss2", "MSS2"},
			{"svq3", "SVQ3"},
			{"012v", "012va12v"},
			{"g2m", "G2M2G2M3G2M4G2M5"},
			{"fic", "FICV"},
			{"hqx", "CHQX"},
			{"tdsc", "TDSC"},
			{"hq_hqa", "CUVC"},
			{"rv40", "RV40"},
			{"screenpresso", "SPV1"},
			{"rscc", "RSCCISCC"},
			{"cfhd", "CFHD"},
			{"m101", "M101M102"},
			{"magicyuv", "MAGYM8RGM8RAM8G0M8Y0M8Y2M8Y4M8YAM0RAM0RGM0G0M0Y0M0Y2M0Y4M2RAM2RGM4RAM4RG"},
			{"ylc", "YLC0"},
			{"speedhq", "SHQ0SHQ1SHQ2SHQ3SHQ4SHQ5SHQ7SHQ9"},
			{"fmvc", "FMVC"},
			{"scpr", "SCPR"},
			{"clearvideo", "UCOD"},
			{"av1", "AV01"},
			{"mscc", "MSCC"},
			{"srgc", "SRGC"},
			{"imm4", "IMM4"},
			{"prosumer", "BT20"},
			{"mwsc", "MWSC"},
			{"wcmv", "WCMV"},
			{"rasc", "RASC"},
			{"hymt", "HYMT"},
			{"arbc", "ARBC"},
			{"agm", "AGM0AGM1AGM2AGM3AGM4AGM5AGM6AGM7"},
			{"lscr", "LSCR"},
			{"imm5", "IMM5"},
			{"imm6", "IMM6"},
			{"mvdv", "MVDV"},
			{"mvha", "MVHA"},
			{"mv30", "MV30"},
			{"notchlc", "nlc1"},
			{"vqc", "VQC1VQC2"},
			{"rtv1", "RTV1"},
			{"vmix", "VMX1"},
			{"lead", "LEAD"},
			{"evc", "evc1"},
			{"rv60", "RV60"},
		};

		static_assert(std::ranges::all_of(fourcc_groups, [](const fourcc_group& g) { return g.fourccs.size() % 4 == 0; }));

		// The compression values BITMAPINFOHEADER gives uncompressed and run-length pictures, and the
		// MPEG FourCCs some writers store as numbers.
		constexpr std::pair<std::uint32_t, std::string_view> numeric_fourccs[] = {
			{0x00000000, "rawvideo"},
			{0x00000001, "msrle"},
			{0x00000002, "msrle"},
			{0x00000003, "rawvideo"},
			{0x00000004, "mpeg4"},
			{0x10000001, "mpeg1"},
			{0x10000002, "mpeg2"},
			{0x10000004, "mpeg4"},
		};

		template <typename Same>
		std::string_view find_fourcc(const std::uint32_t fourcc, Same same)
		{
			for (const auto& [number, codec] : numeric_fourccs)
			{
				if (same(number, fourcc)) return codec;
			}

			for (const auto& group : fourcc_groups)
			{
				for (std::size_t i = 0; i < group.fourccs.size(); i += 4)
				{
					const auto entry = static_cast<std::uint32_t>(static_cast<std::uint8_t>(group.fourccs[i])) |
						static_cast<std::uint32_t>(static_cast<std::uint8_t>(group.fourccs[i + 1])) << 8 |
						static_cast<std::uint32_t>(static_cast<std::uint8_t>(group.fourccs[i + 2])) << 16 |
						static_cast<std::uint32_t>(static_cast<std::uint8_t>(group.fourccs[i + 3])) << 24;
					if (same(entry, fourcc)) return group.codec;
				}
			}

			return {};
		}

		struct wave_tag
		{
			std::uint16_t tag;
			std::string_view codec;
		};

		// The WAVE format tags FFmpeg knows, with raw AAC's and the Ogg Vorbis ACM codec's six modes.
		constexpr wave_tag wave_tags[] = {
			{0x0001, "pcm"}, {0x0002, "adpcm_ms"}, {0x0003, "pcm"}, {0x0006, "alaw"}, {0x0007, "mulaw"},
			{0x000A, "wmavoice"}, {0x0010, "adpcm_ima_oki"}, {0x0011, "adpcm_ima_wav"}, {0x0014, "adpcm_g726"},
			{0x0017, "adpcm_ima_oki"}, {0x0020, "adpcm_yamaha"}, {0x0022, "truespeech"}, {0x0031, "gsm_ms"},
			{0x0032, "gsm_ms"}, {0x0038, "amr_nb"}, {0x0040, "adpcm_g726"}, {0x0041, "g728"}, {0x0042, "g723_1"},
			{0x0045, "adpcm_g726"}, {0x0050, "mp2"}, {0x0055, "mp3"}, {0x0057, "amr_nb"}, {0x0058, "amr_wb"},
			{0x0061, "adpcm_ima_dk4"}, {0x0062, "adpcm_ima_dk3"}, {0x0064, "adpcm_g726"}, {0x0069, "adpcm_ima_xbox"},
			{0x0075, "metasound"}, {0x0083, "g729"}, {0x00FF, "aac"}, {0x0100, "adpcm_rhetorex"}, {0x0111, "g723_1"},
			{0x0125, "adpcm_sanyo"}, {0x0130, "sipr"}, {0x0135, "acelp.kelvin"}, {0x0160, "wmav1"}, {0x0161, "wmav2"},
			{0x0162, "wmapro"}, {0x0163, "wmalossless"}, {0x0165, "xma1"}, {0x0166, "xma2"}, {0x0180, "ftr"},
			{0x0200, "adpcm_ct"}, {0x0215, "dvaudio"}, {0x0216, "dvaudio"}, {0x0270, "atrac3"}, {0x028E, "msnsiren"},
			{0x028F, "adpcm_g722"}, {0x0350, "misc4"}, {0x0400, "adpcm_citrix"}, {0x0401, "imc"}, {0x0402, "iac"},
			{0x0500, "on2avc"}, {0x0501, "on2avc"}, {0x1500, "gsm_ms"}, {0x1501, "truespeech"}, {0x1600, "aac"}, {0x1601, "aac"},
			{0x1602, "aac_latm"}, {0x1610, "aac"}, {0x2000, "ac3"}, {0x2001, "dts"}, {0x2048, "sonic"}, {0x2222, "g729"},
			{0x4143, "aac"}, {0x4180, "ftr"}, {0x5346, "adpcm_swf"}, {0x566F, "vorbis"}, {0x594A, "xan_dpcm"},
			{0x674F, "vorbis"}, {0x6750, "vorbis"}, {0x6751, "vorbis"}, {0x676F, "vorbis"}, {0x6770, "vorbis"},
			{0x6771, "vorbis"}, {0x6C75, "mulaw"}, {0x706D, "aac"}, {0x729A, "g729"}, {0x8180, "ftr"}, {0xA100, "g723_1"},
			{0xA106, "aac"}, {0xA109, "speex"}, {0xCD02, "g728"}, {0xF1AC, "flac"},
		};

		// The last twelve bytes of the GUIDs whose first four are a WAVE format tag, as a file stores them:
		// KSDATAFORMAT_SUBTYPE_PCM and its kind, the ambisonic subtypes, and the base shifted by four bytes
		// that some writers stored and FFmpeg reads too.
		constexpr std::uint8_t subtype_bases[3][12] = {
			{0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71},
			{0x21, 0x07, 0xD3, 0x11, 0x86, 0x44, 0xC8, 0xC1, 0xCA, 0x00, 0x00, 0x00},
			{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA},
		};

		struct wave_subformat
		{
			std::uint8_t guid[16];
			std::string_view codec;
		};

		// Subformats that stand for no format tag.
		constexpr wave_subformat wave_subformats[] = {
			{{0x2C, 0x80, 0x6D, 0xE0, 0x46, 0xDB, 0xCF, 0x11, 0xB4, 0xD1, 0x00, 0x80, 0x5F, 0x6C, 0xBB, 0xEA}, "ac3"},
			{{0x2B, 0x80, 0x6D, 0xE0, 0x46, 0xDB, 0xCF, 0x11, 0xB4, 0xD1, 0x00, 0x80, 0x5F, 0x6C, 0xBB, 0xEA}, "mp2"},
			{{0xAF, 0x87, 0xFB, 0xA7, 0x02, 0x2D, 0xFB, 0x42, 0xA4, 0xD4, 0x05, 0xCD, 0x93, 0x84, 0x3B, 0xDD}, "eac3"},
			{{0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62}, "atrac3p"},
			{{0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D, 0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C}, "atrac9"},
			{{0x82, 0xEC, 0x1F, 0x6A, 0xCA, 0xDB, 0x19, 0x45, 0xBD, 0xE7, 0x56, 0xD3, 0xB3, 0xEF, 0x98, 0x1D}, "adpcm_agm"},
			{{0x3A, 0xC1, 0xFA, 0x38, 0x81, 0x1D, 0x43, 0x61, 0xA4, 0x0D, 0xCE, 0x53, 0xCA, 0x60, 0x7C, 0xD1}, "dfpwm"},
		};

		struct picture_format
		{
			chroma_format chroma = chroma_format::unknown;
			std::uint8_t depth = 0;
			bool alpha = false;
			bool full_range = false;
		};

		struct fixed_format
		{
			std::string_view codec;
			picture_format format;
		};

		constexpr picture_format yuv420_8 = {chroma_format::yuv420, 8};
		constexpr picture_format yuv422_8 = {chroma_format::yuv422, 8};
		constexpr picture_format yuv444_8 = {chroma_format::yuv444, 8};
		constexpr picture_format yuv411_8 = {chroma_format::yuv411, 8};
		constexpr picture_format yuv410_8 = {chroma_format::yuv410, 8};
		constexpr picture_format gray_8 = {chroma_format::gray, 8};
		constexpr picture_format rgb_8 = {chroma_format::rgb, 8};
		constexpr picture_format rgb_5 = {chroma_format::rgb, 5};

		// The codecs whose every stream is sampled and deep alike.
		constexpr fixed_format fixed_formats[] = {
			{"vp8", yuv420_8}, {"vp3", yuv420_8}, {"vp4", yuv420_8}, {"vp5", yuv420_8}, {"vp6", yuv420_8},
			{"vp6f", yuv420_8}, {"vp6a", {chroma_format::yuv420, 8, true}}, {"vp7", yuv420_8}, {"h261", yuv420_8},
			{"h263", yuv420_8}, {"h263i", yuv420_8}, {"h263p", yuv420_8}, {"flv1", yuv420_8}, {"mpeg1", yuv420_8},
			{"mpeg4", yuv420_8}, {"msmpeg4v1", yuv420_8}, {"msmpeg4v2", yuv420_8}, {"msmpeg4v3", yuv420_8},
			{"wmv1", yuv420_8}, {"wmv2", yuv420_8}, {"wmv3", yuv420_8}, {"wmv3image", yuv420_8}, {"vc1", yuv420_8},
			{"vc1image", yuv420_8}, {"rv10", yuv420_8}, {"rv20", yuv420_8}, {"rv30", yuv420_8}, {"rv40", yuv420_8},
			{"rv60", yuv420_8}, {"cavs", yuv420_8}, {"cinepak", yuv420_8}, {"svq1", yuv410_8}, {"svq3", yuv420_8},
			{"indeo2", yuv410_8}, {"indeo3", yuv410_8}, {"indeo4", yuv410_8}, {"indeo5", yuv410_8},
			{"yuv4", yuv420_8}, {"y41p", yuv411_8}, {"v210", {chroma_format::yuv422, 10}},
			{"012v", {chroma_format::yuv422, 10}}, {"r210", {chroma_format::rgb, 10}}, {"r10k", {chroma_format::rgb, 10}},
		};

		// Uncompressed pictures, by FourCC.
		constexpr std::pair<std::uint32_t, picture_format> raw_formats[] = {
			{le_tag("I420"), yuv420_8}, {le_tag("IYUV"), yuv420_8}, {le_tag("YV12"), yuv420_8}, {le_tag("NV12"), yuv420_8},
			{le_tag("NV21"), yuv420_8}, {le_tag("J420"), {chroma_format::yuv420, 8, false, true}},
			{le_tag("I40A"), {chroma_format::yuv420, 8, true}},
			{le_tag("YUY2"), yuv422_8}, {le_tag("YUYV"), yuv422_8}, {le_tag("YUNV"), yuv422_8}, {le_tag("V422"), yuv422_8},
			{le_tag("Y422"), yuv422_8}, {le_tag("VYUY"), yuv422_8}, {le_tag("YVYU"), yuv422_8}, {le_tag("UYVY"), yuv422_8},
			{le_tag("UYNV"), yuv422_8}, {le_tag("UYNY"), yuv422_8}, {le_tag("HDYC"), yuv422_8}, {le_tag("uyv1"), yuv422_8},
			{le_tag("2Vu1"), yuv422_8}, {le_tag("2vuy"), yuv422_8}, {le_tag("yuv2"), yuv422_8}, {le_tag("yuvs"), yuv422_8},
			{le_tag("VDTZ"), yuv422_8}, {le_tag("auv2"), yuv422_8}, {le_tag("P422"), yuv422_8}, {le_tag("YV16"), yuv422_8},
			{le_tag("Y42B"), yuv422_8}, {le_tag("I422"), yuv422_8}, {le_tag("J422"), {chroma_format::yuv422, 8, false, true}},
			{le_tag("I42A"), {chroma_format::yuv422, 8, true}}, {le_tag("Y210"), {chroma_format::yuv422, 10}},
			{le_tag("Y216"), {chroma_format::yuv422, 16}},
			{le_tag("YV24"), yuv444_8}, {le_tag("I444"), yuv444_8}, {le_tag("v308"), yuv444_8},
			{le_tag("J444"), {chroma_format::yuv444, 8, false, true}}, {le_tag("YUVA"), {chroma_format::yuv444, 8, true}},
			{le_tag("AYUV"), {chroma_format::yuv444, 8, true}}, {le_tag("v408"), {chroma_format::yuv444, 8, true}},
			{le_tag("y408"), {chroma_format::yuv444, 8, true}}, {le_tag("Y410"), {chroma_format::yuv444, 10}},
			{le_tag("v410"), {chroma_format::yuv444, 10}}, {le_tag("Y416"), {chroma_format::yuv444, 16}},
			{le_tag("Y411"), yuv411_8}, {le_tag("Y41B"), yuv411_8}, {le_tag("I411"), yuv411_8},
			{le_tag("YUV9"), yuv410_8}, {le_tag("YVU9"), yuv410_8}, {le_tag("I410"), yuv410_8},
			{le_tag("I440"), {chroma_format::yuv440, 8}}, {le_tag("J440"), {chroma_format::yuv440, 8, false, true}},
			{le_tag("Y800"), gray_8}, {le_tag("Y8  "), gray_8}, {le_tag("GREY"), gray_8},
			{le_tag("RV24"), rgb_8}, {le_tag("RV32"), rgb_8}, {le_tag("RGBA"), {chroma_format::rgb, 8, true}},
			{le_tag("AV32"), {chroma_format::rgb, 8, true}},
		};

		void apply_picture_format(const picture_format& format, video_header& out)
		{
			out.chroma = format.chroma;
			out.bit_depth = format.depth;
			out.alpha = format.alpha;
			out.full_range = format.full_range;
		}
	}

	// ---- Audio codec headers ----------------------------------------------------------------------------

	bool parse_aac_config(const std::span<const std::uint8_t> config, audio_header& out)
	{
		bit_reader b(config);
		auto type = read_object_type(b);
		const auto core_rate = read_sampling_frequency(b);
		const auto channel_configuration = b.u(4);
		std::uint32_t extension_rate = 0;
		auto sbr = false;
		auto ps = false;

		// Explicit signalling: SBR, or PS with it, as the object type, then the core's own.
		if (type == aot_sbr || type == aot_ps)
		{
			sbr = true;
			ps = type == aot_ps;
			extension_rate = read_sampling_frequency(b);
			type = read_object_type(b);
			if (type == aot_er_bsac) b.skip(4);
			if (extension_rate == 0 || type == aot_sbr || type == aot_ps || type == aot_usac) return false;
		}

		if (b.overrun() || core_rate == 0 || reserved_object_type(type)) return false;

		audio_header h;
		h.sample_rate = core_rate;

		if (type == aot_usac)
		{
			if (!read_usac_config(b, h)) return false;
			h.profile = aac_profile(type);
			out = std::move(h);
			return true;
		}

		if ((channel_configuration >= 8 && channel_configuration <= 10) || channel_configuration == 15) return false;
		h.channels = aac_channels[channel_configuration];

		std::uint32_t frame_length = 0;
		auto extension_may_follow = false;

		if (is_one_of(general_audio_types, type))
		{
			if (!read_ga_specific_config(b, type, channel_configuration, h.channels, frame_length)) return false;
			if (h.channels == 0) return false;
			// epConfig 2 and 3 bring an ErrorProtectionSpecificConfig, which is not read.
			extension_may_follow = !is_one_of(error_resilient_types, type) || b.u(2) < 2;
		}
		else if (type == aot_er_eld)
		{
			const auto short_frames = b.flag();
			b.skip(3);
			// Low-delay SBR changes the output frame by a ratio its own header gives, which is not read.
			frame_length = b.flag() ? 0u : short_frames ? 480u : 512u;
		}

		if (b.overrun()) return false;
		if (!sbr && extension_may_follow && b.remaining() >= 16) read_sync_extension(b, extension_rate, sbr, ps);

		if (sbr)
		{
			if (extension_rate > core_rate) frame_length *= 2;
			h.sample_rate = extension_rate;
		}

		// PS makes stereo of a mono core, and is not applied to any other.
		ps = ps && h.channels == 1;
		if (ps) h.channels = 2;

		h.samples_per_frame = frame_length;
		h.profile = sbr && is_one_of(aac_core_types, type) ? std::string_view(ps ? "HE-AACv2" : "HE-AAC") : aac_profile(type);
		out = std::move(h);
		return true;
	}

	bool parse_adts_header(const std::span<const std::uint8_t> data, audio_header& out)
	{
		constexpr std::string_view profiles[4] = {"Main", "LC", "SSR", "LTP"};

		if (data.size() < 7) return false;

		bit_reader b(data);
		if (b.u(12) != 0xFFF) return false;
		b.skip(1);
		const auto layer = b.u(2);
		const auto protection_absent = b.flag();
		const auto profile = b.u(2);
		const auto rate = aac_sample_rates[b.u(4)];
		b.skip(1);
		const auto channel_configuration = b.u(3);
		b.skip(4);
		const auto frame_bytes = b.u(13);
		b.skip(11);
		const auto blocks = b.u(2) + 1;
		// A CRC follows the header, after the position of each raw data block past the first.
		const auto header_bytes = protection_absent ? 7u : 7u + 2 * blocks;
		if (layer != 0 || rate == 0 || frame_bytes < header_bytes) return false;

		audio_header h;
		h.sample_rate = rate;
		h.channels = aac_channels[channel_configuration];

		// Without a channel configuration, the first raw data block opens with a program_config_element.
		if (channel_configuration == 0 && data.size() > header_bytes)
		{
			bit_reader block(data.subspan(header_bytes));

			if (block.u(3) == 5)
			{
				const auto channels = read_program_config_channels(block);
				if (!block.overrun()) h.channels = channels;
			}
		}

		h.profile = profiles[profile];
		h.samples_per_frame = 1024 * blocks;
		h.frame_bytes = frame_bytes;
		h.bit_rate = static_cast<std::uint32_t>(8ull * frame_bytes * rate / h.samples_per_frame);
		out = std::move(h);
		return true;
	}

	bool parse_mpeg_audio_header(const std::uint32_t header, audio_header& out, std::string& codec)
	{
		// Kilobits a second by MPEG-1 or the lower rates of MPEG-2 and 2.5, layer and index; 0 is free format.
		constexpr std::uint32_t bit_rates[2][3][15] = {
			{
				{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
				{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
				{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
			},
			{
				{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
				{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
				{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
			},
		};
		constexpr std::uint32_t sample_rates[3] = {44100, 48000, 32000};
		constexpr std::string_view codecs[3] = {"mp1", "mp2", "mp3"};
		constexpr std::string_view versions[4] = {"MPEG-2.5", "", "MPEG-2", "MPEG-1"};

		const auto version = header >> 19 & 3;
		const auto layer = 4 - (header >> 17 & 3);
		const auto bit_rate_index = header >> 12 & 15;
		const auto rate_index = header >> 10 & 3;

		// Version 1 and layer 4 are reserved, as is rate 3, and bit rate 15 is forbidden.
		if ((header & 0xFFE00000) != 0xFFE00000 || version == 1 || layer == 4 || bit_rate_index == 15 || rate_index == 3)
		{
			return false;
		}

		const auto lower = version != 3;
		const auto rate = sample_rates[rate_index] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
		const auto kbps = bit_rates[lower ? 1 : 0][layer - 1][bit_rate_index];
		const auto samples = layer == 1 ? 384u : layer == 3 && lower ? 576u : 1152u;
		const auto padding = header >> 9 & 1;

		audio_header h;
		h.sample_rate = rate;
		h.channels = (header >> 6 & 3) == 3 ? 1 : 2;
		h.bit_rate = kbps * 1000;
		h.samples_per_frame = samples;

		// Layer I counts four-byte slots.
		if (kbps != 0) h.frame_bytes = layer == 1 ? (12 * kbps * 1000 / rate + padding) * 4 : samples / 8 * kbps * 1000 / rate + padding;

		h.profile = std::string(versions[version]) + " Layer " + std::to_string(layer);
		codec = codecs[layer - 1];
		out = std::move(h);
		return true;
	}

	bool parse_ac3_frame(const std::span<const std::uint8_t> data, audio_header& out, std::string& codec)
	{
		// The first frame must stand alone: an independent substream, and the first program's.
		ac3_sync_frame first;
		if (!read_ac3_sync_frame(data, first) || first.stream_type == 1 || first.substream != 0) return false;

		auto locations = first.locations;
		std::uint64_t bytes = first.bytes;
		std::uint64_t bit_rate = first.bit_rate;
		auto enhanced = first.enhanced;

		// Channels past 5.1 come in dependent substreams, each in a frame after the one it extends, and
		// count when they code the same rate and blocks, as FFmpeg's decoder takes them.
		for (auto i = 0; i < 8 && bytes < data.size(); ++i)
		{
			ac3_sync_frame next;

			if (!read_ac3_sync_frame(data.subspan(static_cast<std::size_t>(bytes)), next) || next.stream_type != 1 ||
				next.sample_rate != first.sample_rate || next.blocks != first.blocks)
			{
				break;
			}

			locations |= next.locations;
			bytes += next.bytes;
			bit_rate += next.bit_rate;
			enhanced = true;
		}

		audio_header h;
		h.sample_rate = first.sample_rate;
		h.channels = location_channels(locations);
		h.samples_per_frame = 256 * first.blocks;
		h.frame_bytes = static_cast<std::uint32_t>(bytes);
		h.bit_rate = static_cast<std::uint32_t>(bit_rate);
		codec = enhanced ? "eac3" : "ac3";
		out = std::move(h);
		return true;
	}

	bool parse_ac3_config(const std::span<const std::uint8_t> config, audio_header& out)
	{
		if (config.size() < 3) return false;

		bit_reader b(config);
		const auto fscod = b.u(2);
		const auto bsid = b.u(5);
		b.skip(3);
		const auto acmod = b.u(3);
		const auto lfe = b.flag();
		const auto bit_rate_code = b.u(5);
		const auto shift = bsid > 8 && bsid <= 10 ? bsid - 8 : 0;

		// FFmpeg reads only the channels from the box, so a rate or bit rate code it does not know leaves
		// those unknown rather than refusing the box.
		audio_header h;
		h.sample_rate = fscod < 3 ? ac3_sample_rates[fscod] >> shift : 0;
		h.channels = location_channels(standard_locations(acmod, lfe));
		h.bit_rate = bit_rate_code < 19 ? ac3_bit_rates[bit_rate_code] * 1000 >> shift : 0;
		h.samples_per_frame = 1536;
		out = std::move(h);
		return true;
	}

	bool parse_eac3_config(const std::span<const std::uint8_t> config, audio_header& out)
	{
		bit_reader b(config);
		const auto data_rate = b.u(13);
		b.skip(3);

		// The first independent substream, with the locations its dependent substreams add.
		const auto fscod = b.u(2);
		b.skip(5 + 1 + 1 + 3);
		const auto acmod = b.u(3);
		const auto lfe = b.flag();
		b.skip(3);
		const auto dependents = b.u(4);
		auto locations = standard_locations(acmod, lfe);
		if (dependents != 0) locations |= chan_loc_locations(b.u(9));
		if (b.overrun()) return false;

		// fscod 3 is a reduced rate, which the box does not say.
		audio_header h;
		h.sample_rate = fscod < 3 ? ac3_sample_rates[fscod] : 0;
		h.channels = location_channels(locations);
		h.bit_rate = data_rate * 1000;
		out = std::move(h);
		return true;
	}

	bool parse_dts_frame(const std::span<const std::uint8_t> data, audio_header& out)
	{
		std::array<std::uint8_t, 16> start{};
		auto fourteen_bit = false;
		const auto count = dts_core_start(data, start, fourteen_bit);
		if (count == 0) return false;

		const std::span<const std::uint8_t> header(start.data(), count);
		bit_reader b(header);
		if (b.u(32) != 0x7FFE8001) return false;
		b.skip(1);
		const auto deficit = b.u(5);
		const auto crc = b.flag();
		const auto blocks = b.u(7) + 1;
		const auto frame_bytes = b.u(14) + 1;
		const auto mode = b.u(6);
		const auto rate = dts_sample_rates[b.u(4)];
		const auto bit_rate = dts_bit_rates[b.u(5)];
		const auto reserved = b.flag();
		b.skip(4);
		const auto extension_type = b.u(3);
		const auto extension = b.flag();
		b.skip(1);
		const auto lfe = b.u(2);
		b.skip(1);
		if (crc) b.skip(16);
		b.skip(1 + 4 + 2);
		const auto resolution = b.u(3);

		// FFmpeg's checks: a normal frame of whole subband blocks, with a known layout, rate and source
		// resolution; AMODE past 15 is user-defined.
		if (b.overrun() || deficit != 31 || blocks % 8 != 0 || frame_bytes < 96 || mode > 15 || rate == 0 || reserved ||
			lfe == 3 || resolution == 4 || resolution == 7)
		{
			return false;
		}

		audio_header h;
		h.sample_rate = rate;
		// The XCh extension adds a back centre channel to the core's, as FFmpeg's decoder plays it.
		h.channels = dts_channels[mode] + (lfe != 0 ? 1u : 0u) + (extension && extension_type == 0 ? 1u : 0u);
		h.bit_rate = bit_rate;
		h.samples_per_frame = 32 * blocks;
		// FSIZE counts the frame as 16-bit words carry it; 14-bit words take more of them.
		h.frame_bytes = fourteen_bit ? 2 * ((8 * frame_bytes + 13) / 14) : frame_bytes;
		out = std::move(h);
		return true;
	}

	bool parse_opus_head(const std::span<const std::uint8_t> data, audio_header& out, std::uint32_t& pre_skip)
	{
		// Under RFC 7845 a version with any of its upper four bits set is one this reader cannot read.
		if (data.size() < 19 || !starts_with(data, "OpusHead") || data[8] > 15) return false;

		const std::uint32_t channels = data[9];
		const auto family = data[18];
		if (channels == 0 || (family == 0 && channels > 2)) return false;

		if (family != 0)
		{
			if (data.size() < 21 + channels) return false;

			const std::uint32_t streams = data[19];
			const std::uint32_t coupled = data[20];
			if (streams == 0 || coupled > streams || streams + coupled > 255) return false;
			if (family == 1 && channels > 8) return false;

			// Ambisonics of order n: (n + 1)² channels, and two more for a stereo pair.
			if (family == 2)
			{
				auto ambisonic = false;

				for (std::uint32_t order = 1; order * order <= channels; ++order)
				{
					ambisonic = ambisonic || channels == order * order || channels == order * order + 2;
				}

				if (!ambisonic) return false;
			}

			// Families 1, 2 and 255 map each channel to a stream, or to silence. Other families are read as
			// 255 is, but their tables - family 3's demixing matrix among them - are not.
			if (family == 1 || family == 2 || family == 255)
			{
				for (const std::uint32_t stream : data.subspan(21, channels))
				{
					if (stream != 255 && stream >= streams + coupled) return false;
				}
			}
		}

		audio_header h;
		h.sample_rate = 48000;
		h.channels = channels;
		pre_skip = load_le16(data.data() + 10);
		out = std::move(h);
		return true;
	}

	bool parse_vorbis_header(const std::span<const std::uint8_t> data, audio_header& out)
	{
		if (data.size() < 30 || data[0] != 1 || !starts_with(data.subspan(1), "vorbis")) return false;

		const auto version = load_le32(data.data() + 7);
		const auto channels = data[11];
		const auto rate = load_le32(data.data() + 12);
		const auto nominal = static_cast<std::int32_t>(load_le32(data.data() + 20));
		const auto short_block = data[28] & 15;
		const auto long_block = data[28] >> 4;

		if (version != 0 || channels == 0 || rate == 0 || short_block < 6 || long_block > 13 || short_block > long_block ||
			(data[29] & 1) == 0)
		{
			return false;
		}

		audio_header h;
		h.sample_rate = rate;
		h.channels = channels;
		h.bit_rate = nominal > 0 ? static_cast<std::uint32_t>(nominal) : 0;
		out = std::move(h);
		return true;
	}

	bool parse_flac_streaminfo(const std::span<const std::uint8_t> data, audio_header& out, std::uint64_t& total_samples)
	{
		if (data.size() < 34) return false;

		bit_reader b(data.first(34));
		const auto min_block = b.u(16);
		const auto max_block = b.u(16);
		b.skip(24 + 24);
		const auto rate = b.u(20);
		const auto channels = b.u(3) + 1;
		const auto bits = b.u(5) + 1;
		const auto high = b.u(4);
		const std::uint64_t samples = static_cast<std::uint64_t>(high) << 32 | b.u(32);

		// FFmpeg's checks. A rate of 0 is left to the frames, which state their own.
		if (max_block < 16 || bits < 4) return false;

		audio_header h;
		h.sample_rate = rate;
		h.channels = channels;
		h.bits_per_sample = static_cast<std::uint8_t>(bits);
		h.samples_per_frame = min_block == max_block ? max_block : 0;
		total_samples = samples;
		out = std::move(h);
		return true;
	}

	bool parse_alac_config(std::span<const std::uint8_t> data, audio_header& out)
	{
		if (data.size() >= 36 && starts_with(data.subspan(4), "alac")) data = data.subspan(12);
		if (data.size() < 24) return false;

		const auto frame_length = load_be32(data.data());
		const auto bits = data[5];
		const auto channels = data[9];
		const auto average_bit_rate = load_be32(data.data() + 16);
		const auto rate = load_be32(data.data() + 20);

		// What FFmpeg's decoder accepts. With no channel count it takes the container's.
		if (frame_length == 0 || frame_length > 4096 * 4096 || (bits != 16 && bits != 20 && bits != 24 && bits != 32) ||
			channels > 8)
		{
			return false;
		}

		audio_header h;
		h.sample_rate = rate;
		h.channels = channels;
		h.bits_per_sample = bits;
		h.bit_rate = average_bit_rate;
		h.samples_per_frame = frame_length;
		out = std::move(h);
		return true;
	}

	bool parse_speex_header(const std::span<const std::uint8_t> data, audio_header& out)
	{
		// FFmpeg reads the header up to its frames per packet; the reserved fields after need not be there.
		if (data.size() < 68 || !starts_with(data, "Speex   ")) return false;

		const auto rate = static_cast<std::int32_t>(load_le32(data.data() + 36));
		const auto mode = static_cast<std::int32_t>(load_le32(data.data() + 40));
		const auto channels = static_cast<std::int32_t>(load_le32(data.data() + 48));
		const auto bit_rate = static_cast<std::int32_t>(load_le32(data.data() + 52));
		const auto frame_size = static_cast<std::int32_t>(load_le32(data.data() + 56));

		// Narrowband, wideband or ultra-wideband; mono or stereo.
		if (rate <= 0 || mode < 0 || mode > 2 || channels < 1 || channels > 2) return false;

		audio_header h;
		h.sample_rate = static_cast<std::uint32_t>(rate);
		h.channels = static_cast<std::uint32_t>(channels);
		h.bit_rate = bit_rate > 0 ? static_cast<std::uint32_t>(bit_rate) : 0;
		h.samples_per_frame = frame_size > 0 ? static_cast<std::uint32_t>(frame_size) : 0;
		out = std::move(h);
		return true;
	}

	// ---- Windows structures and codec names -------------------------------------------------------------

	bool read_wave_format(const std::span<const std::uint8_t> data, media_stream& stream, std::uint32_t* const avg_bytes_per_second,
	                      std::uint16_t* const block_align)
	{
		// WAVEFORMAT, then PCMWAVEFORMAT's bits, then WAVEFORMATEX's cbSize and the bytes it counts.
		if (data.size() < 14) return false;

		const auto tag = load_le16(data.data());
		std::uint32_t channels = load_le16(data.data() + 2);
		auto rate = load_le32(data.data() + 4);
		const auto bytes_per_second = load_le32(data.data() + 8);
		const auto align = load_le16(data.data() + 12);
		std::uint32_t bits = data.size() >= 16 ? load_le16(data.data() + 14) : 0;
		auto extra = std::span<const std::uint8_t>{};
		if (data.size() >= 18) extra = data.subspan(18, std::min<std::size_t>(load_le16(data.data() + 16), data.size() - 18));

		std::uint32_t format = tag;
		auto codec = codec_from_wave_tag(tag);

		if (tag == 0xFFFE)
		{
			codec = {};

			// WAVEFORMATEXTENSIBLE: the valid bits, the channel mask, then the SubFormat.
			if (extra.size() >= 22)
			{
				const auto valid_bits = load_le16(extra.data());
				if (valid_bits != 0) bits = valid_bits;

				const auto guid = extra.subspan(6, 16);
				const auto subtype = load_le32(guid.data());
				const auto base = std::ranges::any_of(subtype_bases, [&](const auto& known) { return std::ranges::equal(guid.subspan(4), known); });

				if (base && subtype <= 0xFFFF)
				{
					format = subtype;
					codec = codec_from_wave_tag(static_cast<std::uint16_t>(subtype));
				}
				else if (!base)
				{
					for (const auto& subformat : wave_subformats)
					{
						if (std::ranges::equal(guid, subformat.guid)) codec = subformat.codec;
					}
				}
			}
		}
		else if (tag == 0x0165)
		{
			// XMAWAVEFORMAT lays its streams out after a header of its own, each with its rate and channels.
			if (data.size() < 32) return false;

			const std::size_t streams = load_le16(data.data() + 8);
			if (data.size() < 12 + streams * 20) return false;

			rate = load_le32(data.data() + 16);
			channels = 0;
			for (std::size_t i = 0; i < streams; ++i) channels += data[29 + i * 20];
			bits = 0;
		}

		if (rate == 0) return false;

		stream.kind = media_stream_kind::audio;
		stream.codec = codec;
		stream.codec_tag = wave_tag_text(format);
		stream.audio.sample_rate = rate;
		stream.audio.channels = channels;

		// The samples as coded: for linear PCM, IEEE float and the lossless tags. WAVEFORMAT, which has no
		// bits, leaves PCM's to its block alignment.
		stream.audio.bits_per_sample = 0;
		stream.audio.float_samples = false;

		if (format == 0x0001 || format == 0x0003 || format == 0x0163 || format == 0xF1AC)
		{
			if (data.size() < 16 && format <= 0x0003 && channels != 0 && align % channels == 0) bits = align / channels * 8;
			stream.audio.bits_per_sample = static_cast<std::uint8_t>(std::min<std::uint32_t>(bits, 255));
			stream.audio.float_samples = format == 0x0003;
		}

		// The AAC tags carry an AudioSpecificConfig; HEAACWAVEFORMAT's follows an HEAACWAVEINFO whose payload
		// type 3 is LATM. LATM's rate and channels are its core's, which FFmpeg leaves to the decoder.
		auto config = std::span<const std::uint8_t>{};
		if (format == 0x00FF || format == 0x1601 || format == 0x4143 || format == 0x706D || format == 0xA106) config = extra;

		if (format == 0x1610 && extra.size() >= 12)
		{
			if (load_le16(extra.data()) == 3) stream.codec = "aac_latm";
			if (load_le16(extra.data() + 4) == 0) config = extra.subspan(12);
		}

		if (stream.codec == "aac_latm")
		{
			stream.audio.sample_rate = 0;
			stream.audio.channels = 0;
		}
		else if (!config.empty())
		{
			audio_header h;
			if (parse_aac_config(config, h)) apply_audio_header(h, stream.audio);
		}

		if (avg_bytes_per_second) *avg_bytes_per_second = bytes_per_second;
		if (block_align) *block_align = align;
		return true;
	}

	bool read_bitmap_info(const std::span<const std::uint8_t> data, media_stream& stream)
	{
		if (data.size() < 40 || load_le32(data.data()) < 40) return false;

		const auto width = static_cast<std::int32_t>(load_le32(data.data() + 4));
		// A negative height is a picture stored top-down.
		const auto height = std::abs(static_cast<std::int64_t>(static_cast<std::int32_t>(load_le32(data.data() + 8))));
		const auto bit_count = load_le16(data.data() + 14);
		const auto compression = load_le32(data.data() + 16);
		const auto codec = codec_from_fourcc(compression);

		video_header format;

		if (compression == 0 || compression == 3)
		{
			// BI_RGB and BI_BITFIELDS: 24 and 32 bits are 8-bit RGB, and BI_RGB's 16 are RGB555, where the
			// masks of BI_BITFIELDS, not read, may make them 565. Fewer bits are a palette.
			if (bit_count == 24 || bit_count == 32) apply_picture_format(rgb_8, format);
			if (bit_count == 15 || bit_count == 16) apply_picture_format(compression == 0 ? rgb_5 : picture_format{chroma_format::rgb}, format);
		}
		else if (codec == "rawvideo")
		{
			const auto found = std::ranges::find(raw_formats, compression, &std::pair<std::uint32_t, picture_format>::first);
			if (found != std::end(raw_formats)) apply_picture_format(found->second, format);
		}
		else
		{
			fixed_video_format(codec, format);
		}

		stream.kind = media_stream_kind::video;
		stream.codec = codec;
		stream.codec_tag = fourcc_text(compression);
		stream.video.width = width > 0 && width <= max_image_dimension ? static_cast<std::uint32_t>(width) : 0;
		stream.video.height = height > 0 && height <= max_image_dimension ? static_cast<std::uint32_t>(height) : 0;

		if (format.chroma != chroma_format::unknown)
		{
			stream.video.chroma = format.chroma;
			stream.video.bit_depth = format.bit_depth;
			stream.video.alpha = format.alpha;
			stream.video.full_range = format.full_range;
		}

		// VC-1's sequence header follows the 40 bytes, inside the size biSize gives, as FFmpeg's AVI, ASF
		// and Matroska demuxers all take it. WMVP's sprites are left alone: FFmpeg names no profile for
		// WMV9 Image, and sizes its pictures by the container.
		if (codec == "wmv3" || codec == "vc1")
		{
			video_header sequence;
			if (parse_vc1_sequence(data.subspan(40), sequence)) apply_video_header(sequence, stream.video);
		}

		return true;
	}

	std::string_view codec_from_fourcc(const std::uint32_t little_endian_fourcc) noexcept
	{
		// FFmpeg's way: the exact FourCC first, then one that differs only in case.
		const auto exact = find_fourcc(little_endian_fourcc, [](const std::uint32_t a, const std::uint32_t b) { return a == b; });
		if (!exact.empty()) return exact;

		return find_fourcc(little_endian_fourcc, [](const std::uint32_t a, const std::uint32_t b) { return upper_fourcc(a) == upper_fourcc(b); });
	}

	std::string_view codec_from_wave_tag(const std::uint16_t tag) noexcept
	{
		const auto found = std::ranges::find(wave_tags, tag, &wave_tag::tag);
		return found == std::end(wave_tags) ? std::string_view() : found->codec;
	}

	std::string_view codec_from_object_type(const std::uint8_t object_type) noexcept
	{
		switch (object_type)
		{
		case 0x08: return "mov_text";
		case 0x20: return "mpeg4";
		case 0x21: return "h264";
		case 0x23: return "hevc";
		case 0x33: return "vvc";
		case 0x40:
		case 0x66:
		case 0x67:
		case 0x68: return "aac";
		case 0x60:
		case 0x61:
		case 0x62:
		case 0x63:
		case 0x64:
		case 0x65: return "mpeg2";
		// MPEG-1 and MPEG-2 audio of any layer; the frames say which.
		case 0x69:
		case 0x6B: return "mp3";
		case 0x6A: return "mpeg1";
		case 0x6C: return "mjpeg";
		case 0x6D: return "png";
		case 0x6E: return "jpeg2000";
		case 0xA3: return "vc1";
		case 0xA4: return "dirac";
		case 0xA5: return "ac3";
		case 0xA6: return "eac3";
		// DTS core, and DTS-HD's high resolution, master audio and express as MP4RA registers them.
		case 0xA9:
		case 0xAA:
		case 0xAB:
		case 0xAC: return "dts";
		case 0xAD: return "opus";
		case 0xB1: return "vp9";
		case 0xD0: return "tscc2";
		case 0xD1: return "evrc";
		case 0xDD: return "vorbis";
		case 0xE0: return "dvd_subtitle";
		case 0xE1: return "qcelp";
		default: return {};
		}
	}

	bool fixed_video_format(const std::string_view codec, video_header& out)
	{
		const auto found = std::ranges::find(fixed_formats, codec, &fixed_format::codec);
		if (found == std::end(fixed_formats)) return false;

		apply_picture_format(found->format, out);
		return true;
	}
}
