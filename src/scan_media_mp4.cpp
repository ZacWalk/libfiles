// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning ISO base media and QuickTime files: MP4, MOV, M4A, M4V, 3GP and 3G2. A scan walks the
// top-level boxes by their headers and reads the movie box: each track's header, media header, handler
// and first sample description with the codec configuration it holds, the counts of its sample tables
// but never their entries, and the movie's user data and metadata - QuickTime's text atoms, 3GPP's
// asset boxes, the iTunes and QuickTime metadata lists, and the Xtra box Windows writes. The media is
// never read, and cover art is located rather than read. The boxes are read through mp4_internal.h as
// the writer reads them, so the XMP packet a scan returns is the one read_packet returns.

#include "media_internal.h"
#include "mp4_internal.h"
#include "text_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		using mp4::box;
		using mp4::box_list;
		using mp4::fourcc;

		constexpr auto box_trak = fourcc("trak");
		constexpr auto box_tkhd = fourcc("tkhd");
		constexpr auto box_mdia = fourcc("mdia");
		constexpr auto box_mdhd = fourcc("mdhd");
		constexpr auto box_hdlr = fourcc("hdlr");
		constexpr auto box_minf = fourcc("minf");
		constexpr auto box_stbl = fourcc("stbl");
		constexpr auto box_stsd = fourcc("stsd");
		constexpr auto box_stts = fourcc("stts");
		constexpr auto box_stsz = fourcc("stsz");
		constexpr auto box_stz2 = fourcc("stz2");
		constexpr auto box_tref = fourcc("tref");
		constexpr auto box_chap = fourcc("chap");
		constexpr auto box_meta = fourcc("meta");
		constexpr auto box_keys = fourcc("keys");
		constexpr auto box_ilst = fourcc("ilst");
		constexpr auto box_data = fourcc("data");
		constexpr auto box_mean = fourcc("mean");
		constexpr auto box_name = fourcc("name");
		constexpr auto box_kind = fourcc("kind");
		constexpr auto box_mvex = fourcc("mvex");
		constexpr auto box_mehd = fourcc("mehd");
		constexpr auto box_trex = fourcc("trex");
		constexpr auto box_moof = fourcc("moof");
		constexpr auto box_sidx = fourcc("sidx");
		constexpr auto box_cmov = fourcc("cmov");
		constexpr auto box_dcom = fourcc("dcom");
		constexpr auto box_cmvd = fourcc("cmvd");

		constexpr auto box_esds = fourcc("esds");
		constexpr auto box_wave = fourcc("wave");
		constexpr auto box_sinf = fourcc("sinf");
		constexpr auto box_frma = fourcc("frma");
		constexpr auto box_avcc = fourcc("avcC");
		constexpr auto box_hvcc = fourcc("hvcC");
		constexpr auto box_av1c = fourcc("av1C");
		constexpr auto box_vpcc = fourcc("vpcC");
		constexpr auto box_d263 = fourcc("d263");
		constexpr auto box_colr = fourcc("colr");
		constexpr auto box_pasp = fourcc("pasp");
		constexpr auto box_btrt = fourcc("btrt");
		constexpr auto box_fiel = fourcc("fiel");
		constexpr auto box_dac3 = fourcc("dac3");
		constexpr auto box_dec3 = fourcc("dec3");
		constexpr auto box_dops = fourcc("dOps");
		constexpr auto box_dfla = fourcc("dfLa");
		constexpr auto box_alac = fourcc("alac");
		constexpr auto box_pcmc = fourcc("pcmC");
		constexpr auto box_srat = fourcc("srat");

		constexpr auto gpp_titl = fourcc("titl");
		constexpr auto gpp_auth = fourcc("auth");
		constexpr auto gpp_perf = fourcc("perf");
		constexpr auto gpp_gnre = fourcc("gnre");
		constexpr auto gpp_dscp = fourcc("dscp");
		constexpr auto gpp_albm = fourcc("albm");
		constexpr auto gpp_yrrc = fourcc("yrrc");
		constexpr auto gpp_loci = fourcc("loci");
		constexpr auto gpp_kywd = fourcc("kywd");

		constexpr auto handler_mdir = fourcc("mdir");
		constexpr auto handler_mdta = fourcc("mdta");
		constexpr auto colour_nclx = fourcc("nclx");
		constexpr auto colour_nclc = fourcc("nclc");

		constexpr auto item_covr = fourcc("covr");
		constexpr auto item_free_form = fourcc("----");
		constexpr auto item_trkn = fourcc("trkn");
		constexpr auto item_disk = fourcc("disk");
		constexpr auto item_gnre = fourcc("gnre");

		constexpr auto type_dvh1 = fourcc("dvh1");
		constexpr auto type_mp4s = fourcc("mp4s");
		constexpr auto type_raw = fourcc("raw ");
		constexpr auto type_ap4h = fourcc("ap4h");
		constexpr auto type_ap4x = fourcc("ap4x");

		// The most of a box read to parse it: a sample description's field, a codec's configuration, a
		// tag's value, a box of keys.
		constexpr std::size_t max_box_read = 1024 * 1024;
		// The most boxes of one list in the movie that are looked at.
		constexpr std::size_t max_list = 4096;
		constexpr std::size_t max_tracks = 1024;
		// A time-to-sample table of no more entries than this is read for the frame rate; a longer one is
		// not, and the rate comes from the sample count and the media's duration.
		constexpr std::uint32_t max_stts_entries = 4096;
		// The largest packet a scan keeps, as the image scans keep their metadata blocks.
		constexpr std::uint64_t max_packet = 16u * 1024 * 1024;
		// The largest compressed movie box inflated, before and after.
		constexpr std::uint32_t max_compressed_movie = 16u * 1024 * 1024;
		// The top-level boxes a structure report lists.
		constexpr std::size_t max_listed_boxes = 256;
		// Seconds from QuickTime's epoch, 1904, to 1970.
		constexpr std::int64_t epoch_1904 = 2082844800;

		// Big-endian fields of a payload held in memory. A read past the end yields zero and fails the
		// cursor, so a parser reads what it needs and checks once.
		class cursor
		{
		public:
			explicit cursor(const std::span<const std::uint8_t> data) : _data(data)
			{
			}

			bool failed() const { return _failed; }
			std::size_t remaining() const { return _data.size() - _pos; }

			std::uint64_t read(const std::size_t bytes)
			{
				if (remaining() < bytes)
				{
					_pos = _data.size();
					_failed = true;
					return 0;
				}

				std::uint64_t value = 0;
				for (std::size_t i = 0; i < bytes; ++i) value = value << 8 | _data[_pos + i];
				_pos += bytes;
				return value;
			}

			std::uint8_t u8() { return static_cast<std::uint8_t>(read(1)); }
			std::uint16_t u16() { return static_cast<std::uint16_t>(read(2)); }
			std::uint32_t u24() { return static_cast<std::uint32_t>(read(3)); }
			std::uint32_t u32() { return static_cast<std::uint32_t>(read(4)); }
			std::uint64_t u64() { return read(8); }
			std::int32_t s32() { return static_cast<std::int32_t>(u32()); }

			void skip(const std::size_t bytes) { take(bytes); }

			std::span<const std::uint8_t> take(const std::size_t bytes)
			{
				if (remaining() < bytes)
				{
					_pos = _data.size();
					_failed = true;
					return {};
				}

				const auto part = _data.subspan(_pos, bytes);
				_pos += bytes;
				return part;
			}

		private:
			std::span<const std::uint8_t> _data;
			std::size_t _pos = 0;
			bool _failed = false;
		};

		// Mac OS Roman from 0x80 up, as QuickTime's oldest text is written.
		constexpr char16_t mac_roman[128] = {
			0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
			0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
			0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
			0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
			0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
			0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
			0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
			0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
		};

		// Text in Mac OS Roman, to the first NUL.
		std::string text_from_mac_roman(const std::span<const std::uint8_t> bytes)
		{
			std::string out;

			for (const auto b : bytes)
			{
				if (b == 0) break;
				append_utf8(out, b < 0x80 ? char32_t{b} : char32_t{mac_roman[b - 0x80]});
			}

			return out;
		}

		// The Macintosh language codes QuickTime's oldest files name languages by, in ISO 639-2/T, as the
		// packed codes are. Codes 95 to 127 are unassigned.
		constexpr std::string_view mac_languages[] = {
			"eng", "fra", "deu", "ita", "nld", "swe", "spa", "dan", "por", "nor",
			"heb", "jpn", "ara", "fin", "ell", "isl", "mlt", "tur", "hrv", "zho",
			"urd", "hin", "tha", "kor", "lit", "pol", "hun", "est", "lav", "smi",
			"fao", "fas", "rus", "zho", "nld", "gle", "sqi", "ron", "ces", "slk",
			"slv", "yid", "srp", "mkd", "bul", "ukr", "bel", "uzb", "kaz", "aze",
			"aze", "hye", "kat", "ron", "kir", "tgk", "tuk", "mon", "mon", "pus",
			"kur", "kas", "snd", "bod", "nep", "san", "mar", "ben", "asm", "guj",
			"pan", "ori", "mal", "kan", "tam", "tel", "sin", "mya", "khm", "lao",
			"vie", "ind", "tgl", "msa", "msa", "amh", "tir", "orm", "som", "swa",
			"kin", "run", "nya", "mlg", "epo", "", "", "", "", "",
			"", "", "", "", "", "", "", "", "", "",
			"", "", "", "", "", "", "", "", "", "",
			"", "", "", "", "", "", "", "",
			"cym", "eus", "cat", "lat", "que", "grn", "aym", "tat", "uig", "dzo",
			"jav", "sun", "glg", "afr", "bre", "iku", "gla", "glv", "gle", "ton",
			"grc", "kal", "aze", "nno",
		};

		// The ISO 639-2 code a 16-bit language field names: three letters of five bits each, or below
		// 0x400 a Macintosh language code. Empty for "und", for 0x7FFF, which QuickTime writes for none,
		// and for anything that is not a code.
		std::string language_name(const std::uint16_t code)
		{
			if (code == 0x7FFF) return {};
			if (code < 0x400) return code < std::size(mac_languages) ? std::string(mac_languages[code]) : std::string();

			const char letters[3] = {
				static_cast<char>(0x60 + (code >> 10 & 0x1F)), static_cast<char>(0x60 + (code >> 5 & 0x1F)), static_cast<char>(0x60 + (code & 0x1F))
			};

			if (!std::ranges::all_of(letters, [](const char c) { return c >= 'a' && c <= 'z'; })) return {};

			std::string text(letters, 3);
			return text == "und" ? std::string() : text;
		}

		// A binary value, described rather than reproduced.
		std::string binary_text(const std::size_t size)
		{
			return size == 0 ? std::string() : std::format("binary, {} bytes", size);
		}

		// A big-endian integer of one to eight bytes.
		std::optional<std::uint64_t> be_unsigned(const std::span<const std::uint8_t> bytes)
		{
			if (bytes.empty() || bytes.size() > 8) return {};
			std::uint64_t v = 0;
			for (const auto b : bytes) v = v << 8 | b;
			return v;
		}

		// A little-endian integer of one to eight bytes, as Windows writes the Xtra box's numbers.
		std::optional<std::uint64_t> le_unsigned(const std::span<const std::uint8_t> bytes)
		{
			if (bytes.empty() || bytes.size() > 8) return {};
			std::uint64_t v = 0;
			for (auto i = bytes.size(); i-- > 0;) v = v << 8 | bytes[i];
			return v;
		}

		// An integer of that many bytes read as two's complement.
		std::optional<std::int64_t> as_signed(const std::optional<std::uint64_t> v, const std::size_t size)
		{
			if (!v) return {};
			if (size == 8) return static_cast<std::int64_t>(*v);

			const auto sign = std::uint64_t{1} << (size * 8 - 1);
			return static_cast<std::int64_t>(*v ^ sign) - static_cast<std::int64_t>(sign);
		}

		std::optional<std::int64_t> be_signed(const std::span<const std::uint8_t> bytes)
		{
			return as_signed(be_unsigned(bytes), bytes.size());
		}

		std::optional<std::int64_t> le_signed(const std::span<const std::uint8_t> bytes)
		{
			return as_signed(le_unsigned(bytes), bytes.size());
		}

		template <typename T>
		std::string number_text(const std::optional<T> v)
		{
			return v ? std::to_string(*v) : std::string();
		}

		struct named_type
		{
			std::uint32_t type;
			std::string_view codec;
		};

		// The video sample descriptions of ISO base media, QuickTime and the registries beside them.
		constexpr named_type video_types[] = {
			{fourcc("avc1"), "h264"}, {fourcc("avc2"), "h264"}, {fourcc("avc3"), "h264"}, {fourcc("avc4"), "h264"},
			{fourcc("dva1"), "h264"}, {fourcc("dvav"), "h264"}, {fourcc("ai5p"), "h264"}, {fourcc("ai5q"), "h264"},
			{fourcc("ai52"), "h264"}, {fourcc("ai53"), "h264"}, {fourcc("ai55"), "h264"}, {fourcc("ai56"), "h264"},
			{fourcc("ai1p"), "h264"}, {fourcc("ai1q"), "h264"}, {fourcc("ai12"), "h264"}, {fourcc("ai13"), "h264"},
			{fourcc("ai15"), "h264"}, {fourcc("ai16"), "h264"}, {fourcc("AVin"), "h264"}, {fourcc("aivx"), "h264"},
			{fourcc("xalg"), "h264"}, {fourcc("avlg"), "h264"},
			{fourcc("hvc1"), "hevc"}, {fourcc("hev1"), "hevc"}, {fourcc("dvh1"), "hevc"}, {fourcc("dvhe"), "hevc"},
			{fourcc("vvc1"), "vvc"}, {fourcc("vvi1"), "vvc"},
			{fourcc("av01"), "av1"}, {fourcc("dav1"), "av1"},
			{fourcc("vp08"), "vp8"}, {fourcc("vp09"), "vp9"},
			{fourcc("mp4v"), "mpeg4"}, {fourcc("DIVX"), "mpeg4"}, {fourcc("XVID"), "mpeg4"}, {fourcc("3IV2"), "mpeg4"},
			{fourcc("apco"), "prores"}, {fourcc("apcs"), "prores"}, {fourcc("apcn"), "prores"}, {fourcc("apch"), "prores"},
			{fourcc("ap4h"), "prores"}, {fourcc("ap4x"), "prores"},
			{fourcc("jpeg"), "mjpeg"}, {fourcc("mjpa"), "mjpeg"}, {fourcc("AVDJ"), "mjpeg"}, {fourcc("AVRn"), "mjpeg"},
			{fourcc("dmb1"), "mjpeg"}, {fourcc("mjpb"), "mjpegb"},
			{fourcc("mjp2"), "jpeg2000"}, {fourcc("png "), "png"}, {fourcc("tiff"), "tiff"}, {fourcc("gif "), "gif"},
			{fourcc("h263"), "h263"}, {fourcc("s263"), "h263"}, {fourcc("H263"), "h263"},
			{fourcc("SVQ1"), "svq1"}, {fourcc("svq1"), "svq1"}, {fourcc("svqi"), "svq1"}, {fourcc("SVQ3"), "svq3"},
			{fourcc("cvid"), "cinepak"}, {fourcc("rle "), "qtrle"}, {fourcc("rpza"), "rpza"}, {fourcc("smc "), "smc"},
			{fourcc("8BPS"), "8bps"},
			{fourcc("dvc "), "dv"}, {fourcc("dvcp"), "dv"}, {fourcc("dvl "), "dv"}, {fourcc("dvlp"), "dv"},
			{fourcc("dvpp"), "dv"}, {fourcc("dv5n"), "dv"}, {fourcc("dv5p"), "dv"}, {fourcc("AVdv"), "dv"},
			{fourcc("AVd1"), "dv"}, {fourcc("dvhq"), "dv"}, {fourcc("dvhp"), "dv"}, {fourcc("dvh2"), "dv"},
			{fourcc("dvh3"), "dv"}, {fourcc("dvh4"), "dv"}, {fourcc("dvh5"), "dv"}, {fourcc("dvh6"), "dv"},
			{fourcc("m1v "), "mpeg1"}, {fourcc("m1v1"), "mpeg1"}, {fourcc("mpeg"), "mpeg1"}, {fourcc("mp1v"), "mpeg1"},
			{fourcc("m2v1"), "mpeg2"}, {fourcc("mp2v"), "mpeg2"}, {fourcc("hdv1"), "mpeg2"}, {fourcc("hdv2"), "mpeg2"},
			{fourcc("hdv3"), "mpeg2"}, {fourcc("hdv4"), "mpeg2"}, {fourcc("hdv5"), "mpeg2"}, {fourcc("hdv6"), "mpeg2"},
			{fourcc("hdv7"), "mpeg2"}, {fourcc("hdv8"), "mpeg2"}, {fourcc("hdv9"), "mpeg2"}, {fourcc("hdva"), "mpeg2"},
			{fourcc("mx3n"), "mpeg2"}, {fourcc("mx3p"), "mpeg2"}, {fourcc("mx4n"), "mpeg2"}, {fourcc("mx4p"), "mpeg2"},
			{fourcc("mx5n"), "mpeg2"}, {fourcc("mx5p"), "mpeg2"}, {fourcc("xd51"), "mpeg2"}, {fourcc("xd54"), "mpeg2"},
			{fourcc("xd55"), "mpeg2"}, {fourcc("xd59"), "mpeg2"}, {fourcc("xd5a"), "mpeg2"}, {fourcc("xd5b"), "mpeg2"},
			{fourcc("xd5c"), "mpeg2"}, {fourcc("xd5d"), "mpeg2"}, {fourcc("xd5e"), "mpeg2"}, {fourcc("xd5f"), "mpeg2"},
			{fourcc("xdv1"), "mpeg2"}, {fourcc("xdv2"), "mpeg2"}, {fourcc("xdv3"), "mpeg2"}, {fourcc("xdv4"), "mpeg2"},
			{fourcc("xdv5"), "mpeg2"}, {fourcc("xdv6"), "mpeg2"}, {fourcc("xdv7"), "mpeg2"}, {fourcc("xdv8"), "mpeg2"},
			{fourcc("xdv9"), "mpeg2"}, {fourcc("xdva"), "mpeg2"}, {fourcc("xdvb"), "mpeg2"}, {fourcc("xdvc"), "mpeg2"},
			{fourcc("xdvd"), "mpeg2"}, {fourcc("xdve"), "mpeg2"}, {fourcc("xdvf"), "mpeg2"}, {fourcc("xdhd"), "mpeg2"},
			{fourcc("xdh2"), "mpeg2"}, {fourcc("AVmp"), "mpeg2"},
			{fourcc("AVdn"), "dnxhd"}, {fourcc("AVdh"), "dnxhd"}, {fourcc("icod"), "aic"}, {fourcc("drac"), "dirac"},
			{fourcc("vc-1"), "vc1"}, {fourcc("ovc1"), "vc1"}, {fourcc("cfhd"), "cfhd"},
			{fourcc("Hap1"), "hap"}, {fourcc("Hap5"), "hap"}, {fourcc("HapY"), "hap"}, {fourcc("HapA"), "hap"},
			{fourcc("HapM"), "hap"},
			{fourcc("raw "), "rawvideo"}, {fourcc("2vuy"), "rawvideo"}, {fourcc("yuv2"), "rawvideo"}, {fourcc("yuvs"), "rawvideo"},
			{fourcc("v308"), "rawvideo"}, {fourcc("v408"), "rawvideo"}, {fourcc("v410"), "rawvideo"}, {fourcc("b16g"), "rawvideo"},
			{fourcc("b48r"), "rawvideo"}, {fourcc("b64a"), "rawvideo"}, {fourcc("BGRA"), "rawvideo"}, {fourcc("RGBA"), "rawvideo"},
			{fourcc("ABGR"), "rawvideo"}, {fourcc("24BG"), "rawvideo"}, {fourcc("L555"), "rawvideo"}, {fourcc("L565"), "rawvideo"},
			{fourcc("v210"), "v210"}, {fourcc("r210"), "r210"}, {fourcc("R10k"), "r10k"}, {fourcc("R10g"), "r10k"},
			{fourcc("AVrp"), "avrp"}, {fourcc("AVUI"), "avui"},
		};

		constexpr named_type audio_types[] = {
			{fourcc("mp4a"), "aac"},
			{fourcc("ac-3"), "ac3"}, {fourcc("sac3"), "ac3"}, {fourcc("ec-3"), "eac3"}, {fourcc("ac-4"), "ac4"},
			{fourcc("Opus"), "opus"}, {fourcc("fLaC"), "flac"}, {fourcc("alac"), "alac"},
			{fourcc("samr"), "amr_nb"}, {fourcc("sawb"), "amr_wb"}, {fourcc("sevc"), "evrc"}, {fourcc("sqcp"), "qcelp"},
			{fourcc("Qclp"), "qcelp"}, {fourcc("ssmv"), "smv"},
			{fourcc(".mp3"), "mp3"}, {fourcc("mp3 "), "mp3"}, {fourcc(".mp2"), "mp2"}, {fourcc(".mp1"), "mp1"},
			{fourcc("dtsc"), "dts"}, {fourcc("dtsh"), "dts"}, {fourcc("dtsl"), "dts"}, {fourcc("dtse"), "dts"},
			{fourcc("dts+"), "dts"}, {fourcc("dts-"), "dts"}, {fourcc("DTS "), "dts"}, {fourcc("mlpa"), "truehd"},
			{fourcc("twos"), "pcm"}, {fourcc("sowt"), "pcm"}, {fourcc("in24"), "pcm"}, {fourcc("in32"), "pcm"},
			{fourcc("fl32"), "pcm"}, {fourcc("fl64"), "pcm"}, {fourcc("raw "), "pcm"}, {fourcc("NONE"), "pcm"},
			{fourcc("lpcm"), "pcm"}, {fourcc("ipcm"), "pcm"}, {fourcc("fpcm"), "pcm"},
			{fourcc("ima4"), "adpcm_ima_qt"}, {fourcc("MAC3"), "mace3"}, {fourcc("MAC6"), "mace6"},
			{fourcc("QDM2"), "qdm2"}, {fourcc("QDMC"), "qdmc"}, {fourcc("agsm"), "gsm"}, {fourcc("spex"), "speex"},
			{fourcc("nmos"), "nellymoser"}, {fourcc("mha1"), "mpegh_3d_audio"}, {fourcc("mhm1"), "mpegh_3d_audio"},
		};

		constexpr named_type subtitle_types[] = {
			{fourcc("tx3g"), "mov_text"}, {fourcc("text"), "mov_text"}, {fourcc("wvtt"), "webvtt"},
			{fourcc("stpp"), "ttml"}, {fourcc("c608"), "eia_608"}, {fourcc("mp4s"), "dvd_subtitle"},
		};

		std::string_view codec_of(const std::span<const named_type> table, const std::uint32_t type)
		{
			const auto it = std::ranges::find(table, type, &named_type::type);
			return it == table.end() ? std::string_view() : it->codec;
		}

		// QuickTime names some sound by its WAVE format tag: "ms" and the tag. 'ulaw' and 'alaw' are the
		// WAVE formats 7 and 6.
		std::optional<std::uint16_t> wave_tag_of(const std::uint32_t type)
		{
			if (type >> 16 == 0x6D73) return static_cast<std::uint16_t>(type);
			if (type == fourcc("ulaw")) return std::uint16_t{7};
			if (type == fourcc("alaw")) return std::uint16_t{6};
			return {};
		}

		std::string_view audio_codec_of(const std::uint32_t type)
		{
			if (const auto tag = wave_tag_of(type)) return codec_from_wave_tag(*tag);
			return codec_of(audio_types, type);
		}

		// The codec a video type names, from libfiles' table of them or, as FFmpeg falls back, from the
		// FourCCs AVI files use, which lie in the file in the same order.
		std::string_view video_codec_of(const std::uint32_t type)
		{
			if (const auto codec = codec_of(video_types, type); !codec.empty()) return codec;
			return codec_from_fourcc((type >> 24) | (type >> 8 & 0xFF00u) | (type << 8 & 0xFF0000u) | (type << 24));
		}

		// A track is the kind its handler names; a track whose handler names none of them is known by its
		// sample description.
		media_stream_kind kind_of(const std::uint32_t handler, const std::uint32_t type)
		{
			if (handler == fourcc("vide") || handler == fourcc("auxv") || handler == fourcc("pict")) return media_stream_kind::video;
			if (handler == fourcc("soun")) return media_stream_kind::audio;

			if (handler == fourcc("sbtl") || handler == fourcc("subt") || handler == fourcc("text") || handler == fourcc("subp") ||
			    handler == fourcc("clcp"))
			{
				return media_stream_kind::subtitle;
			}

			if (!codec_of(audio_types, type).empty() || wave_tag_of(type)) return media_stream_kind::audio;
			if (!codec_of(video_types, type).empty()) return media_stream_kind::video;
			if (!codec_of(subtitle_types, type).empty()) return media_stream_kind::subtitle;
			return media_stream_kind::data;
		}

		// The pictures a sample description's type fixes - ProRes 422, the uncompressed QuickTime formats
		// and DV - so that no header need be read.
		struct entry_format
		{
			std::uint32_t type;
			chroma_format chroma;
			std::uint8_t bit_depth;
			bool alpha;
		};

		constexpr entry_format entry_formats[] = {
			{fourcc("apco"), chroma_format::yuv422, 10, false}, {fourcc("apcs"), chroma_format::yuv422, 10, false},
			{fourcc("apcn"), chroma_format::yuv422, 10, false}, {fourcc("apch"), chroma_format::yuv422, 10, false},
			{fourcc("2vuy"), chroma_format::yuv422, 8, false}, {fourcc("yuv2"), chroma_format::yuv422, 8, false},
			{fourcc("yuvs"), chroma_format::yuv422, 8, false}, {fourcc("v210"), chroma_format::yuv422, 10, false},
			{fourcc("v308"), chroma_format::yuv444, 8, false}, {fourcc("v408"), chroma_format::yuv444, 8, true},
			{fourcc("v410"), chroma_format::yuv444, 10, false}, {fourcc("r210"), chroma_format::rgb, 10, false},
			{fourcc("R10k"), chroma_format::rgb, 10, false}, {fourcc("R10g"), chroma_format::rgb, 10, false},
			{fourcc("AVrp"), chroma_format::rgb, 10, false}, {fourcc("b48r"), chroma_format::rgb, 16, false},
			{fourcc("b64a"), chroma_format::rgb, 16, true}, {fourcc("BGRA"), chroma_format::rgb, 8, true},
			{fourcc("RGBA"), chroma_format::rgb, 8, true}, {fourcc("ABGR"), chroma_format::rgb, 8, true},
			{fourcc("24BG"), chroma_format::rgb, 8, false}, {fourcc("b16g"), chroma_format::gray, 16, false},
			{fourcc("dvc "), chroma_format::yuv411, 8, false}, {fourcc("dvl "), chroma_format::yuv411, 8, false},
			{fourcc("dvpp"), chroma_format::yuv411, 8, false}, {fourcc("dvcp"), chroma_format::yuv420, 8, false},
			{fourcc("dvlp"), chroma_format::yuv420, 8, false}, {fourcc("dv5n"), chroma_format::yuv422, 8, false},
			{fourcc("dv5p"), chroma_format::yuv422, 8, false}, {fourcc("AVd1"), chroma_format::yuv422, 8, false},
			{fourcc("dvhq"), chroma_format::yuv422, 8, false}, {fourcc("dvhp"), chroma_format::yuv422, 8, false},
			{fourcc("dvh2"), chroma_format::yuv422, 8, false}, {fourcc("dvh3"), chroma_format::yuv422, 8, false},
			{fourcc("dvh4"), chroma_format::yuv422, 8, false}, {fourcc("dvh5"), chroma_format::yuv422, 8, false},
			{fourcc("dvh6"), chroma_format::yuv422, 8, false}, {fourcc("dvh1"), chroma_format::yuv422, 8, false},
		};

		constexpr named_type prores_profiles[] = {
			{fourcc("apco"), "422 Proxy"}, {fourcc("apcs"), "422 LT"}, {fourcc("apcn"), "422"},
			{fourcc("apch"), "422 HQ"}, {fourcc("ap4h"), "4444"}, {fourcc("ap4x"), "4444 XQ"},
		};

		// What a video sample description's type, the codec it names and its depth field say of its picture.
		video_header entry_picture(const std::uint32_t type, const std::string_view codec, const std::uint16_t depth)
		{
			video_header h;

			if (const auto it = std::ranges::find(entry_formats, type, &entry_format::type); it != std::end(entry_formats))
			{
				h.chroma = it->chroma;
				h.bit_depth = it->bit_depth;
				h.alpha = it->alpha;
			}
			else if (type == type_ap4h || type == type_ap4x)
			{
				h.chroma = chroma_format::yuv444;
				h.bit_depth = 12;
				h.alpha = depth == 32;
			}
			else if (type == type_raw)
			{
				// QuickTime's uncompressed RGB: 16 bits a pixel is RGB 5-5-5, 32 has alpha, and 33 to 40 are
				// grey of depth less 32. Below 16 the pixels index a palette.
				if (depth == 16)
				{
					h.chroma = chroma_format::rgb;
					h.bit_depth = 5;
				}
				else if (depth == 24 || depth == 32)
				{
					h.chroma = chroma_format::rgb;
					h.bit_depth = 8;
					h.alpha = depth == 32;
				}
				else if (depth > 32 && depth <= 40)
				{
					h.chroma = chroma_format::gray;
					h.bit_depth = static_cast<std::uint8_t>(depth - 32);
				}
			}
			else if (codec == "mpeg2")
			{
				// The professional MPEG-2 formats fix their sampling: HDV's and XDCAM's 4:2:0, and XDCAM
				// HD422's and IMX's 4:2:2.
				const auto name = box_type_text(type);

				if (name.starts_with("hdv") || name.starts_with("xdv") || name == "xdhd")
				{
					h.chroma = chroma_format::yuv420;
					h.bit_depth = 8;
				}
				else if (name.starts_with("xd5") || name.starts_with("mx") || name == "xdh2" || name == "AVmp")
				{
					h.chroma = chroma_format::yuv422;
					h.bit_depth = 8;
				}
			}

			h.profile = std::string(codec_of(prores_profiles, type));
			return h;
		}

		// The decoder configuration an esds box's ES_Descriptor holds (ISO/IEC 14496-1).
		struct decoder_config
		{
			std::uint8_t object_type = 0;
			std::uint32_t average_bit_rate = 0;
			std::span<const std::uint8_t> specific;
			bool found = false;
		};

		// A descriptor's length: up to four bytes of seven bits.
		std::uint32_t descriptor_length(cursor& c)
		{
			std::uint32_t length = 0;

			for (auto i = 0; i < 4; ++i)
			{
				const auto b = c.u8();
				length = length << 7 | (b & 0x7Fu);
				if (!(b & 0x80)) break;
			}

			return length;
		}

		decoder_config read_esds(const std::span<const std::uint8_t> payload)
		{
			decoder_config config;
			cursor c(payload);
			c.skip(4); // version and flags

			auto tag = c.u8();
			descriptor_length(c);

			// The ES_Descriptor's identifier and flags come before the decoder configuration, which some
			// writers put first.
			if (tag == 0x03)
			{
				c.skip(2);
				const auto flags = c.u8();
				if (flags & 0x80) c.skip(2);
				if (flags & 0x40) c.skip(c.u8());
				if (flags & 0x20) c.skip(2);
				tag = c.u8();
				descriptor_length(c);
			}

			if (c.failed() || tag != 0x04) return config;

			config.object_type = c.u8();
			c.skip(1 + 3 + 4); // stream type, buffer size and maximum bit rate
			config.average_bit_rate = c.u32();
			if (c.failed()) return config;

			config.found = true;

			if (c.remaining() >= 2 && c.u8() == 0x05)
			{
				const auto length = descriptor_length(c);
				if (!c.failed()) config.specific = c.take(std::min<std::size_t>(length, c.remaining()));
			}

			return config;
		}

		// The iTunes atoms whose untyped values are numbers rather than text.
		constexpr std::uint32_t numeric_items[] = {
			fourcc("cpil"), fourcc("pgap"), fourcc("pcst"), fourcc("hdvd"), fourcc("stik"), fourcc("rtng"),
			fourcc("akID"), fourcc("tmpo"), fourcc("tves"), fourcc("tvsn"), fourcc("sfID"), fourcc("cnID"),
			fourcc("atID"), fourcc("geID"), fourcc("plID"), fourcc("cmID"), fourcc("shwm"),
		};

		// Those whose untyped values are text, beside the ones named with a copyright sign.
		constexpr std::uint32_t text_items[] = {
			fourcc("aART"), fourcc("desc"), fourcc("ldes"), fourcc("sdes"), fourcc("tvsh"), fourcc("tven"),
			fourcc("tvnn"), fourcc("soal"), fourcc("soar"), fourcc("soaa"), fourcc("soco"), fourcc("sonm"),
			fourcc("sosn"), fourcc("catg"), fourcc("keyw"), fourcc("purd"), fourcc("purl"), fourcc("egid"),
			fourcc("apID"), fourcc("ownr"), fourcc("cprt"), fourcc("xid "),
		};

		bool copyright_named(const std::uint32_t type)
		{
			return type >> 24 == 0xA9;
		}

		// A value an iTunes atom holds without a type, as the atom's meaning reads it: a track or disc
		// number and the count it is out of, a genre's number, another number, or text, which FFmpeg reads
		// as Mac OS Roman.
		std::string implicit_value(const std::uint32_t item, const std::span<const std::uint8_t> v)
		{
			if (item == item_trkn || item == item_disk)
			{
				if (v.size() < 4) return {};
				const auto number = load_be16(v.data() + 2);
				const auto total = v.size() >= 6 ? load_be16(v.data() + 4) : 0;
				return total != 0 ? std::format("{}/{}", number, total) : std::to_string(number);
			}

			// The genre is iTunes' number for it, one more than ID3v1's.
			if (item == item_gnre) return v.size() >= 2 ? std::to_string(load_be16(v.data())) : number_text(be_unsigned(v));
			if (std::ranges::find(numeric_items, item) != std::end(numeric_items)) return number_text(be_unsigned(v));
			if (copyright_named(item) || std::ranges::find(text_items, item) != std::end(text_items)) return text_from_mac_roman(v);
			return binary_text(v.size());
		}

		bool text_type(const std::uint32_t type)
		{
			return type >= 1 && type <= 5;
		}

		// The size of Apple's well-known fixed-size integer types: signed 8, 16, 32 and 64 bits, then
		// unsigned.
		std::size_t integer_size(const std::uint32_t type)
		{
			switch (type)
			{
			case 65:
			case 75: return 1;
			case 66:
			case 76: return 2;
			case 67:
			case 77: return 4;
			case 74:
			case 78: return 8;
			default: return 0;
			}
		}

		// A metadata 'data' box's value as text, by its well-known type. An iTunes item gives an untyped
		// value its meaning, and track, disc and genre numbers read as that whatever their type, as FFmpeg
		// reads them.
		std::string data_text(const std::uint32_t type, const std::span<const std::uint8_t> v, const std::uint32_t item)
		{
			if (item != 0 && !text_type(type) && (type == 0 || item == item_trkn || item == item_disk || item == item_gnre))
			{
				return implicit_value(item, v);
			}

			switch (type)
			{
			case 1:
			case 4: return text_from_utf8(v);
			case 2:
			case 5: return text_from_utf16(v, true);
			case 3: return text_from_unknown(v);
			case 21: return v.size() <= 4 || v.size() == 8 ? number_text(be_signed(v)) : binary_text(v.size());
			case 22: return v.size() <= 4 || v.size() == 8 ? number_text(be_unsigned(v)) : binary_text(v.size());
			case 23: return v.size() == 4 ? std::format("{}", std::bit_cast<float>(load_be32(v.data()))) : binary_text(v.size());
			case 24: return v.size() == 8 ? std::format("{}", std::bit_cast<double>(*be_unsigned(v))) : binary_text(v.size());
			default: break;
			}

			if (const auto size = integer_size(type); size != 0 && v.size() == size)
			{
				return type < 75 ? number_text(be_signed(v)) : number_text(be_unsigned(v));
			}

			return binary_text(v.size());
		}

		// An Xtra value as text, by its VARTYPE: text, a number, or a binary value described.
		std::string xtra_text(const mp4::xtra_value& value)
		{
			switch (value.type)
			{
			case 8: // VT_BSTR, as Windows writes the box's text
			case 31: return text_from_utf16(value.data, false);
			case 30: return text_from_unknown(value.data);
			case 11: return value.data.empty() ? std::string() : std::ranges::all_of(value.data, [](const std::uint8_t b) { return b == 0; }) ? "0" : "1";
			case 2:
			case 3:
			case 16:
			case 20:
			case 22: return number_text(le_signed(value.data));
			case 17:
			case 18:
			case 19:
			case 21:
			case 23:
			case 64: return number_text(le_unsigned(value.data));
			default: return binary_text(value.data.size());
			}
		}

		// An OpusHead packet from a dOps box's payload, which holds the same fields big-endian and without
		// the signature.
		std::vector<std::uint8_t> opus_head(const std::span<const std::uint8_t> dops)
		{
			if (dops.size() < 11) return {};

			std::vector<std::uint8_t> head = {
				'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, dops[1], dops[3], dops[2], dops[7], dops[6], dops[5], dops[4], dops[9],
				dops[8], dops[10]
			};

			head.insert(head.end(), dops.begin() + 11, dops.end());
			return head;
		}

		// A 3GPP asset's string: UTF-8, or UTF-16 after a byte-order mark, to a NUL. used is what it took,
		// its NUL included.
		std::string asset_string(const std::span<const std::uint8_t> bytes, std::size_t& used)
		{
			if (bytes.size() >= 2 && ((bytes[0] == 0xFE && bytes[1] == 0xFF) || (bytes[0] == 0xFF && bytes[1] == 0xFE)))
			{
				std::size_t end = 2;
				while (end + 1 < bytes.size() && (bytes[end] != 0 || bytes[end + 1] != 0)) end += 2;
				used = std::min(end + 2, bytes.size());
				return text_from_utf16(bytes.first(std::min(end, bytes.size())), true);
			}

			const auto nul = static_cast<std::size_t>(std::ranges::find(bytes, std::uint8_t{0}) - bytes.begin());
			used = std::min(nul + 1, bytes.size());
			return text_from_utf8(bytes.first(nul));
		}

		// A place as FFmpeg gives a 3GPP location: ISO 6709 coordinates, then its name after the slash.
		std::string location_text(const double latitude, const double longitude, const double altitude, const std::string_view place)
		{
			auto text = std::format("{:+08.4f}{:+09.4f}", latitude, longitude);
			if (altitude != 0) text += std::format("{:+08.3f}", altitude);
			text += '/';
			text += place;
			return text;
		}

		// Whether decoded text is text rather than a binary atom read as text: it holds no control
		// characters but tab and the line breaks.
		bool plain_text(const std::string_view text)
		{
			return std::ranges::none_of(text, [](const char c)
			{
				const auto u = static_cast<unsigned char>(c);
				return u < 0x20 && u != '\t' && u != '\n' && u != '\r';
			});
		}

		struct track
		{
			media_stream stream;
			std::uint32_t handler = 0;
			std::string handler_name;
			// The first sample description's type, and how many the track has.
			std::uint32_t entry_type = 0;
			std::uint32_t entry_count = 0;
			std::uint32_t timescale = 0;
			std::optional<std::uint64_t> media_duration;
			std::uint64_t sample_count = 0;
			// The time-to-sample table's totals, when it was short enough to read.
			bool timed = false;
			std::uint64_t timed_samples = 0;
			std::uint64_t timed_duration = 0;
			// The track header's transform - its a, b, c and d - and its size.
			double matrix[4] = {1, 0, 0, 1};
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			// The tracks this one names as its chapters.
			std::vector<std::uint32_t> chapters;
		};

		struct movie
		{
			// The movie box's boxes, from which the packet's place is found.
			box_list boxes;
			std::uint32_t timescale = 0;
			std::optional<std::uint64_t> duration;
			// The movie header's transform, which a player applies after each track's.
			double matrix[4] = {1, 0, 0, 1};
			// An mvex box: the movie may go on in fragments.
			bool extends = false;
			std::uint64_t fragment_duration = 0;
			// Each track's default sample duration, from its trex box.
			std::map<std::uint32_t, std::uint32_t> default_durations;
			std::vector<track> tracks;
		};

		// Where tags go: to the file, whose properties they are filed by, or to a track.
		class tag_target
		{
		public:
			explicit tag_target(media_scan_result& file) : _file(&file)
			{
			}

			explicit tag_target(media_stream& stream) : _stream(&stream)
			{
			}

			void add(const tag_family family, const std::string_view key, const std::string_view value, const std::string_view language) const
			{
				if (_file)
				{
					add_tag(*_file, family, key, value, language);
					return;
				}

				const auto text = trim_text(value);
				if (!text.empty()) _stream->tags.push_back({std::string(key), std::string(text), std::string(language)});
			}

			// Cover art is the file's alone.
			std::vector<media_picture>* pictures() const { return _file ? &_file->pictures : nullptr; }

		private:
			media_scan_result* _file = nullptr;
			media_stream* _stream = nullptr;
		};

		// A transform matrix's a, b, c and d, in 16.16 fixed point; the rest only moves the picture.
		void read_matrix(cursor& c, double (&matrix)[4])
		{
			const auto a = c.s32();
			const auto b = c.s32();
			c.skip(4);
			const auto cc = c.s32();
			const auto d = c.s32();
			c.skip(16);
			if (c.failed()) return;

			matrix[0] = a / 65536.0;
			matrix[1] = b / 65536.0;
			matrix[2] = cc / 65536.0;
			matrix[3] = d / 65536.0;
		}

		// Reads a movie box's boxes into a movie and the scan's tags. A compressed movie is read from
		// memory, where what it holds cannot be located in the file.
		class movie_reader
		{
		public:
			movie_reader(reader& r, movie& m, media_scan_result& out, const bool quicktime, const bool in_file) :
				_r(r), _m(m), _out(out), _quicktime(quicktime), _in_file(in_file)
			{
			}

			// Reads the movie box's boxes as each is met, so that the file is read forward, and returns them
			// as listed, for the packet's place to be found from.
			box_list read(const box& moov)
			{
				const tag_target file(_out);
				auto header = false;

				return mp4::list_boxes(_r, moov.payload(), moov.end(), max_list, [&](const box& b)
				{
					if (b.type == mp4::mvhd && !header)
					{
						header = true;
						read_movie_header(b);
					}
					else if (b.type == box_trak && _m.tracks.size() < max_tracks)
					{
						read_track(b);
					}
					else if (b.type == mp4::udta)
					{
						read_user_data(b, file);
					}
					else if (b.type == box_meta)
					{
						read_meta(b, file);
					}
					else if (b.type == box_mvex)
					{
						read_movie_extends(b);
					}
					else if (b.type == box_cmov && _in_file)
					{
						read_compressed_movie(b);
					}
				});
			}

		private:
			std::vector<std::uint8_t> payload(const box& b, const std::size_t limit = max_box_read) const
			{
				return _r.read(b.payload(), static_cast<std::size_t>(std::min<std::uint64_t>(b.payload_size(), limit)));
			}

			// The boxes a box holds, from skip bytes into its payload.
			box_list children(const box& b, const std::uint64_t skip = 0) const
			{
				if (b.payload_size() < skip) return {};
				return mp4::list_boxes(_r, b.payload() + skip, b.end(), max_list);
			}

			// Passes the boxes a box holds, from skip bytes into its payload, to visit as each is read: the
			// first max_list of them.
			template <typename Visit>
			void each_child(const box& b, Visit&& visit, const std::uint64_t skip = 0) const
			{
				if (b.payload_size() < skip) return;

				std::size_t count = 0;

				mp4::for_each_box(_r, b.payload() + skip, b.end(), [&count, &visit](const box& child)
				{
					if (count++ < max_list) visit(child);
				});
			}

			void read_movie_header(const box& b)
			{
				const auto p = payload(b, 112);
				cursor c(p);
				const auto version = c.u8();
				c.skip(3);
				const auto created = version == 1 ? c.u64() : c.u32();
				c.skip(version == 1 ? 8 : 4);
				const auto timescale = c.u32();
				const auto duration = version == 1 ? c.u64() : c.u32();
				if (c.failed()) return;

				_m.timescale = timescale;
				if (duration != (version == 1 ? std::numeric_limits<std::uint64_t>::max() : 0xFFFFFFFFu)) _m.duration = duration;

				c.skip(4 + 2 + 10); // rate, volume and reserved
				read_matrix(c, _m.matrix);

				std::int64_t seconds = 0;

				if (version == 1)
				{
					if (created <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) seconds = static_cast<std::int64_t>(created);
				}
				else
				{
					// A time before 1970 is taken to count from 1970, as some writers do and FFmpeg reads it.
					seconds = static_cast<std::int64_t>(created);
					if (seconds > 0 && seconds < epoch_1904) seconds += epoch_1904;
				}

				if (seconds == 0) return;

				// The movie's date is applied as a tag's would be, before any tag's, which replaces it; a tag
				// read before the movie header has replaced it already.
				const auto date = utc_date_from_seconds(seconds - epoch_1904);
				auto& properties = _out.properties;
				if (!date.valid() || properties.created.valid()) return;

				properties.created = date;
				if (properties.year == 0) properties.year = date.year;
			}

			void read_track(const box& trak)
			{
				track t;
				const tag_target target(t.stream);
				auto header = false;
				auto media = false;

				each_child(trak, [&](const box& b)
				{
					if (b.type == box_tkhd && !header)
					{
						header = true;
						read_track_header(b, t);
					}
					else if (b.type == box_mdia && !media)
					{
						media = true;
						read_media(b, t);
					}
					else if (b.type == box_tref)
					{
						read_references(b, t);
					}
					else if (b.type == mp4::udta)
					{
						read_track_user_data(b, t);
					}
					else if (b.type == box_meta)
					{
						read_meta(b, target);
					}
				});

				t.stream.kind = kind_of(t.handler, t.entry_type);
				_m.tracks.push_back(std::move(t));
			}

			void read_track_header(const box& b, track& t)
			{
				const auto p = payload(b, 96);
				cursor c(p);
				const auto version = c.u8();
				const auto flags = c.u24();
				c.skip(version == 1 ? 16 : 8);
				const auto id = c.u32();
				if (c.failed()) return;

				t.stream.id = id;
				t.stream.is_default = (flags & 1) != 0;

				// Reserved, duration, reserved, layer, alternate group, volume and reserved.
				c.skip(4 + (version == 1 ? 8 : 4) + 8 + 2 + 2 + 2 + 2);
				read_matrix(c, t.matrix);
				const auto width = c.u32();
				const auto height = c.u32();
				if (c.failed()) return;

				t.width = width >> 16;
				t.height = height >> 16;
			}

			void read_media(const box& mdia, track& t)
			{
				const auto boxes = children(mdia);
				const auto first = [&boxes](const std::uint32_t type) -> const box*
				{
					const auto it = std::ranges::find(boxes.boxes, type, &box::type);
					return it == boxes.boxes.end() ? nullptr : &*it;
				};

				// The handler comes before the sample descriptions, which it says how to read.
				if (const auto* const header = first(box_mdhd)) read_media_header(*header, t);
				if (const auto* const handler = first(box_hdlr)) read_handler(*handler, t);

				if (const auto* const information = first(box_minf))
				{
					auto table = false;

					each_child(*information, [&](const box& b)
					{
						if (b.type != box_stbl || table) return;
						table = true;
						read_sample_table(b, t);
					});
				}
			}

			void read_media_header(const box& b, track& t)
			{
				const auto p = payload(b, 36);
				cursor c(p);
				const auto version = c.u8();
				c.skip(3 + (version == 1 ? 16 : 8));
				const auto timescale = c.u32();
				const auto duration = version == 1 ? c.u64() : c.u32();
				if (c.failed()) return;

				t.timescale = timescale;
				if (duration != (version == 1 ? std::numeric_limits<std::uint64_t>::max() : 0xFFFFFFFFu)) t.media_duration = duration;

				const auto language = c.u16();
				if (!c.failed()) t.stream.language = language_name(language);
			}

			void read_handler(const box& b, track& t)
			{
				const auto p = payload(b, 24 + 256);
				if (p.size() < 12) return;

				t.handler = load_be32(p.data() + 8);
				if (p.size() <= 24) return;

				// QuickTime counts the name's length in its first byte, where ISO ends it with a NUL.
				auto name = std::span(p).subspan(24);
				if (_quicktime && static_cast<std::size_t>(name[0]) == name.size() - 1) name = name.subspan(1);
				t.handler_name = std::string(trim_text(text_from_utf8(name)));
			}

			// The sample tables, read as each is met: only the first sample description, the time-to-sample
			// table when it is short, and the sample count.
			void read_sample_table(const box& stbl, track& t)
			{
				auto descriptions = false;
				auto times = false;
				auto sizes = false;

				each_child(stbl, [&](const box& b)
				{
					if (b.type == box_stsd && !descriptions)
					{
						descriptions = true;
						read_sample_descriptions(b, t);
					}
					else if (b.type == box_stts && !times)
					{
						times = true;
						read_sample_times(b, t);
					}
					else if ((b.type == box_stsz || b.type == box_stz2) && !sizes)
					{
						// The count alone: the sizes themselves are never read.
						sizes = true;
						if (b.payload_size() >= 12) t.sample_count = _r.be32(b.payload() + 8);
					}
				});
			}

			void read_sample_times(const box& b, track& t)
			{
				if (b.payload_size() < 8) return;

				const auto count = _r.be32(b.payload() + 4);
				if (count == 0 || count > max_stts_entries || b.payload_size() - 8 < 8ull * count) return;

				const auto entries = _r.view(b.payload() + 8, 8u * count);
				std::uint64_t samples = 0;
				std::uint64_t duration = 0;

				for (std::size_t i = 0; i < count; ++i)
				{
					const auto n = load_be32(entries.data() + 8 * i);
					const auto span = std::uint64_t{n} * load_be32(entries.data() + 8 * i + 4);
					samples += n;
					duration = span > std::numeric_limits<std::uint64_t>::max() - duration ? std::numeric_limits<std::uint64_t>::max() : duration + span;
				}

				t.timed = true;
				t.timed_samples = samples;
				t.timed_duration = duration;
			}

			void read_sample_descriptions(const box& stsd, track& t)
			{
				if (stsd.payload_size() < 8) return;

				const auto version = _r.u8(stsd.payload());
				t.entry_count = _r.be32(stsd.payload() + 4);

				box entry;
				if (t.entry_count == 0 || mp4::read_box(_r, stsd.payload() + 8, stsd.end(), entry) != mp4::header_status::box) return;

				t.entry_type = entry.type;
				t.stream.codec_tag = box_type_text(entry.type);

				switch (kind_of(t.handler, entry.type))
				{
				case media_stream_kind::video: read_video_entry(entry, t);
					break;
				case media_stream_kind::audio: read_audio_entry(entry, version, t);
					break;
				case media_stream_kind::subtitle: read_subtitle_entry(entry, t);
					break;
				case media_stream_kind::data: break;
				}
			}

			// The format an encrypted sample description stands for, from its protection scheme's frma box.
			std::optional<std::uint32_t> original_format(const box_list& boxes) const
			{
				for (const auto& b : boxes.boxes)
				{
					if (b.type != box_sinf) continue;

					for (const auto& inner : children(b).boxes)
					{
						if (inner.type == box_frma && inner.payload_size() >= 4) return _r.be32(inner.payload());
					}
				}

				return {};
			}

			struct colour_box
			{
				video_header header;
				bool states_range = false;
			};

			// A colr box's nclx (ISO) or nclc (QuickTime) colour: H.273's primaries, transfer and matrix,
			// and in nclx whether the range is full.
			std::optional<colour_box> read_colour(const box& b) const
			{
				const auto p = payload(b, 11);
				if (p.size() < 10) return {};

				const auto kind = load_be32(p.data());
				if (kind != colour_nclx && kind != colour_nclc) return {};

				const auto code = [&p](const std::size_t at)
				{
					const auto value = load_be16(p.data() + at);
					return static_cast<std::uint8_t>(value <= 255 ? value : 2);
				};

				colour_box c;
				c.header.color_primaries = code(4);
				c.header.transfer = code(6);
				c.header.matrix = code(8);
				c.states_range = kind == colour_nclx && p.size() == 11;
				c.header.full_range = c.states_range && (p[10] & 0x80) != 0;
				return c;
			}

			void read_video_entry(const box& entry, track& t)
			{
				auto& s = t.stream;
				auto& v = s.video;
				auto type = entry.type;
				box_list boxes;
				std::uint16_t depth = 0;

				// A visual sample description's fields take 78 bytes. QuickTime puts a colour table after them
				// when its depth calls for a palette and it names no standard one.
				if (entry.payload_size() >= 78)
				{
					const auto fields = payload(entry, 78);
					v.width = load_be16(fields.data() + 24);
					v.height = load_be16(fields.data() + 26);
					depth = load_be16(fields.data() + 74);
					std::uint64_t first = 78;

					if (load_be16(fields.data() + 76) == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8) && entry.payload_size() - first >= 8)
					{
						const auto last = _r.be16(entry.payload() + first + 6);
						first += 8 + (last <= 255 ? (last + 1u) * 8u : 0u);
					}

					boxes = children(entry, first);
				}

				if (const auto format = original_format(boxes)) type = *format;

				const auto has = [&boxes](const std::uint32_t child)
				{
					return std::ranges::find(boxes.boxes, child, &box::type) != boxes.boxes.end();
				};

				// Dolby Vision's HEVC shares DVCPRO HD's 'dvh1', and is told apart by its HEVC configuration.
				const auto dolby_vision = type == type_dvh1 && has(box_hvcc);
				s.codec = type == type_dvh1 && !dolby_vision ? "dv" : std::string(video_codec_of(type));

				video_header coded;
				auto coded_found = false;
				std::optional<colour_box> colour;
				std::string stated_profile;
				std::uint32_t stated_rate = 0;
				std::uint32_t es_rate = 0;

				for (const auto& b : boxes.boxes)
				{
					if (b.type == box_avcc)
					{
						coded_found = parse_avc_config(payload(b), coded) || coded_found;
					}
					else if (b.type == box_hvcc)
					{
						coded_found = parse_hevc_config(payload(b), coded) || coded_found;
					}
					else if (b.type == box_av1c)
					{
						coded_found = parse_av1_config(payload(b), coded) || coded_found;
					}
					else if (b.type == box_vpcc)
					{
						const auto p = payload(b);
						if (p.size() > 4) coded_found = parse_vp_config(std::span(p).subspan(4), coded) || coded_found;
					}
					else if (b.type == box_esds)
					{
						const auto p = payload(b);
						const auto es = read_esds(p);
						if (!es.found) continue;

						if (const auto codec = codec_from_object_type(es.object_type); !codec.empty()) s.codec = codec;
						es_rate = es.average_bit_rate;

						if (s.codec == "mpeg4") coded_found = parse_mpeg4_video(es.specific, coded) || coded_found;
						else if (s.codec == "mpeg2" || s.codec == "mpeg1") coded_found = parse_mpeg12_video(es.specific, coded) || coded_found;
					}
					else if (b.type == box_d263)
					{
						// Its vendor and decoder version, then H.263's level and profile.
						const auto p = payload(b, 7);
						if (p.size() == 7) stated_profile = p[6] == 0 ? std::string("Baseline") : std::format("Profile {}", p[6]);
					}
					else if (b.type == box_colr)
					{
						if (auto c = read_colour(b)) colour = std::move(c);
					}
					else if (b.type == box_pasp)
					{
						const auto p = payload(b, 8);

						if (p.size() == 8 && load_be32(p.data()) != 0 && load_be32(p.data() + 4) != 0)
						{
							v.pixel_aspect_width = load_be32(p.data());
							v.pixel_aspect_height = load_be32(p.data() + 4);
						}
					}
					else if (b.type == box_btrt)
					{
						const auto p = payload(b, 12);
						if (p.size() == 12) stated_rate = load_be32(p.data() + 8);
					}
					else if (b.type == box_fiel)
					{
						const auto p = payload(b, 2);
						if (!p.empty() && p[0] == 2) v.interlaced = true;
					}
				}

				// What the codec, and the type and depth of the description, fix of the picture, which its
				// configuration may then say more exactly.
				video_header base;
				fixed_video_format(s.codec, base);

				if (!dolby_vision)
				{
					const auto picture = entry_picture(type, s.codec, depth);

					if (picture.chroma != chroma_format::unknown)
					{
						base.chroma = picture.chroma;
						base.bit_depth = picture.bit_depth;
						base.alpha = picture.alpha;
					}

					if (!picture.profile.empty()) base.profile = picture.profile;
				}

				if (!stated_profile.empty()) base.profile = stated_profile;

				apply_video_header(base, v);
				if (coded_found) apply_video_header(coded, v);

				// The container's colour is what players follow, where the stream says otherwise; nclx states
				// the range too.
				if (colour)
				{
					apply_video_header(colour->header, v);
					if (colour->states_range) v.full_range = colour->header.full_range;
				}

				s.bit_rate = stated_rate != 0 ? stated_rate : es_rate;
			}

			void read_audio_entry(const box& entry, const std::uint8_t stsd_version, track& t)
			{
				auto& s = t.stream;
				auto& a = s.audio;
				auto type = entry.type;
				box_list boxes;
				std::uint32_t sample_size = 0;
				std::uint32_t lpcm_flags = 0;

				if (entry.payload_size() >= 28)
				{
					const auto fields = payload(entry, 64);
					const auto version = load_be16(fields.data() + 8);
					a.channels = load_be16(fields.data() + 16);
					sample_size = load_be16(fields.data() + 18);
					a.sample_rate = load_be32(fields.data() + 24) >> 16;
					std::uint64_t first = 28;

					// QuickTime's sound descriptions add fields in their versions 1 and 2, as FFmpeg reads
					// them unless an ISO sample description box says its entries are ISO's own version 1.
					if (_quicktime || stsd_version == 0)
					{
						if (version == 1 && fields.size() >= 44)
						{
							first = 44;
						}
						else if (version == 2 && fields.size() >= 64)
						{
							const auto bits = static_cast<std::uint64_t>(load_be32(fields.data() + 32)) << 32 | load_be32(fields.data() + 36);
							const auto rate = std::bit_cast<double>(bits);
							a.sample_rate = rate >= 1 && rate < 4294967295.0 ? static_cast<std::uint32_t>(std::llround(rate)) : 0;
							a.channels = load_be32(fields.data() + 40);
							sample_size = load_be32(fields.data() + 48);
							lpcm_flags = load_be32(fields.data() + 52);
							first = 64;
						}
					}

					boxes = children(entry, first);
				}

				if (const auto format = original_format(boxes)) type = *format;
				s.codec = std::string(audio_codec_of(type));

				std::vector<std::uint8_t> esds;
				audio_header coded;
				auto coded_found = false;
				std::uint32_t stated_rate = 0;
				std::uint32_t stated_sample_rate = 0;
				std::uint32_t pcm_size = 0;

				const auto visit = [&](const box& b)
				{
					if (b.type == box_esds && esds.empty())
					{
						esds = payload(b);
					}
					else if (b.type == box_dac3)
					{
						coded_found = parse_ac3_config(payload(b), coded) || coded_found;
					}
					else if (b.type == box_dec3)
					{
						coded_found = parse_eac3_config(payload(b), coded) || coded_found;
					}
					else if (b.type == box_dops)
					{
						std::uint32_t pre_skip = 0;
						coded_found = parse_opus_head(opus_head(payload(b)), coded, pre_skip) || coded_found;
					}
					else if (b.type == box_dfla)
					{
						// A full box, then FLAC's metadata blocks, of which the first is STREAMINFO.
						const auto p = payload(b);
						std::uint64_t total = 0;

						if (p.size() >= 42 && (p[4] & 0x7F) == 0 && (p[5] << 16 | p[6] << 8 | p[7]) >= 34)
						{
							coded_found = parse_flac_streaminfo(std::span(p).subspan(8, 34), coded, total) || coded_found;
						}
					}
					else if (b.type == box_alac)
					{
						const auto config = _r.read(b.start, static_cast<std::size_t>(std::min<std::uint64_t>(b.size, 256)));
						coded_found = parse_alac_config(config, coded) || coded_found;
					}
					else if (b.type == box_btrt)
					{
						const auto p = payload(b, 12);
						if (p.size() == 12) stated_rate = load_be32(p.data() + 8);
					}
					else if (b.type == box_pcmc)
					{
						// A full box, the format's flags, then the sample size.
						const auto p = payload(b, 6);
						if (p.size() == 6) pcm_size = p[5];
					}
					else if (b.type == box_srat)
					{
						const auto p = payload(b, 8);
						if (p.size() == 8) stated_sample_rate = load_be32(p.data() + 4);
					}
				};

				// QuickTime keeps a sound's configuration in a wave box.
				for (const auto& b : boxes.boxes)
				{
					if (b.type == box_wave)
					{
						for (const auto& inner : children(b).boxes) visit(inner);
					}
					else
					{
						visit(b);
					}
				}

				if (stated_sample_rate != 0) a.sample_rate = stated_sample_rate;

				std::uint32_t es_rate = 0;

				if (!esds.empty())
				{
					const auto es = read_esds(esds);

					if (es.found)
					{
						if (const auto codec = codec_from_object_type(es.object_type); !codec.empty()) s.codec = codec;
						es_rate = es.average_bit_rate;
						audio_header config;

						if (s.codec == "aac" && parse_aac_config(es.specific, config))
						{
							// AAC whose SBR only its stream signals gives the core's rate, half the rate that
							// plays, which the sample description states.
							if (config.sample_rate != 0 && a.sample_rate == 2 * config.sample_rate) config.sample_rate = a.sample_rate;
							coded = config;
							coded_found = true;
						}
					}
				}

				if (s.codec == "pcm") set_pcm_format(type, sample_size, lpcm_flags, pcm_size, a);
				if (coded_found) apply_audio_header(coded, a);
				s.bit_rate = stated_rate != 0 ? stated_rate : es_rate != 0 ? es_rate : coded.bit_rate;
			}

			// The sample format of QuickTime's and ISO's PCM: fixed by the type, or stated by the sound
			// description - its sample size, or version 2's flags - or by ISO's pcmC box.
			static void set_pcm_format(const std::uint32_t type, const std::uint32_t sample_size, const std::uint32_t lpcm_flags,
			                           const std::uint32_t pcm_size, audio_format& a)
			{
				auto bits = sample_size;
				auto floating = false;

				if (type == fourcc("raw ")) bits = 8;
				else if (type == fourcc("in24")) bits = 24;
				else if (type == fourcc("in32") || type == fourcc("fl32")) bits = 32;
				else if (type == fourcc("fl64")) bits = 64;
				else if ((type == fourcc("ipcm") || type == fourcc("fpcm")) && pcm_size != 0) bits = pcm_size;

				if (type == fourcc("fl32") || type == fourcc("fl64") || type == fourcc("fpcm")) floating = true;
				else if (type == fourcc("lpcm")) floating = (lpcm_flags & 1) != 0;

				if (bits == 0 || bits > 64) return;
				a.bits_per_sample = static_cast<std::uint8_t>(bits);
				a.float_samples = floating;
			}

			void read_subtitle_entry(const box& entry, track& t)
			{
				auto& s = t.stream;
				s.codec = std::string(codec_of(subtitle_types, entry.type));
				if (entry.type != type_mp4s) return;

				// An MPEG-4 systems stream, as DVD subpictures are carried, names its codec in an esds box.
				for (const auto& b : children(entry, 8).boxes)
				{
					if (b.type != box_esds) continue;

					const auto p = payload(b);
					const auto es = read_esds(p);
					if (const auto codec = codec_from_object_type(es.object_type); es.found && !codec.empty()) s.codec = codec;
					break;
				}
			}

			void read_references(const box& tref, track& t)
			{
				each_child(tref, [&](const box& b)
				{
					if (b.type != box_chap) return;

					const auto p = payload(b, 4 * 256);
					for (std::size_t i = 0; i + 4 <= p.size(); i += 4) t.chapters.push_back(load_be32(p.data() + i));
				});
			}

			void read_track_user_data(const box& udta, track& t)
			{
				const tag_target target(t.stream);

				each_child(udta, [&](const box& b)
				{
					if (b.type == box_kind)
					{
						read_kind(b, t);
						return;
					}

					if (b.type != box_name && !copyright_named(b.type)) return;

					const auto atom = read_text_atom(b);
					if (!atom) return;

					target.add(tag_family::quicktime, box_type_text(b.type), atom->text, atom->language);

					// A track's name is its title, or failing one its ©nam.
					if (b.type == box_name || (b.type == fourcc("\xA9nam") && t.stream.title.empty())) t.stream.title = trim_text(atom->text);
				});
			}

			// A track's role, as DASH names roles: commentary, or a description for those who cannot see.
			void read_kind(const box& b, track& t) const
			{
				const auto p = payload(b, 1024);
				if (p.size() <= 4) return;

				const std::string_view strings(reinterpret_cast<const char*>(p.data() + 4), p.size() - 4);
				const auto end = strings.find('\0');
				if (end == std::string_view::npos || strings.substr(0, end) != "urn:mpeg:dash:role:2011") return;

				auto value = strings.substr(end + 1);
				value = value.substr(0, value.find('\0'));
				if (value == "commentary") t.stream.commentary = true;
				else if (value == "description") t.stream.audio_description = true;
			}

			struct text_atom
			{
				std::string text;
				std::string language;
			};

			// A QuickTime user data text atom: a list of strings, each after its length and language, of
			// which the first is read, as FFmpeg reads it. An atom too short for that is its text alone.
			// Nothing for an atom whose text holds control characters, which is binary.
			std::optional<text_atom> read_text_atom(const box& b) const
			{
				const auto p = payload(b);
				text_atom atom;

				if (p.size() >= 4 && load_be16(p.data()) <= p.size() - 4)
				{
					const auto code = load_be16(p.data() + 2);
					const auto text = std::span(p).subspan(4, load_be16(p.data()));

					// A Macintosh language code, or none, means Mac OS Roman; a packed code, UTF-8.
					atom.text = code < 0x400 || code == 0x7FFF ? text_from_mac_roman(text) : text_from_utf8(text);
					atom.language = language_name(code);
				}
				else
				{
					atom.text = text_from_utf8(p);
				}

				if (!plain_text(atom.text)) return {};
				return atom;
			}

			static bool asset_box(const std::uint32_t type)
			{
				return type == gpp_titl || type == gpp_auth || type == gpp_perf || type == gpp_gnre || type == gpp_dscp ||
					type == mp4::cprt || type == gpp_albm || type == gpp_yrrc || type == gpp_loci || type == gpp_kywd;
			}

			void read_user_data(const box& udta, const tag_target& target)
			{
				each_child(udta, [&](const box& b)
				{
					if (copyright_named(b.type) || b.type == box_name || b.type == fourcc("manu") || b.type == fourcc("modl") ||
					    b.type == fourcc("keyw"))
					{
						if (const auto atom = read_text_atom(b)) target.add(tag_family::quicktime, box_type_text(b.type), atom->text, atom->language);
					}
					else if (asset_box(b.type))
					{
						read_asset(b, target);
					}
					else if (b.type == box_meta)
					{
						read_meta(b, target);
					}
					else if (b.type == mp4::xtra)
					{
						read_xtra(b, target);
					}
				});
			}

			// A 3GPP asset box (TS 26.244): a full box, a packed language, then a string - but for the
			// recording year, a number alone, and a place, whose coordinates follow its name.
			void read_asset(const box& b, const tag_target& target) const
			{
				const auto p = payload(b);
				if (p.size() < 6) return;

				const auto key = box_type_text(b.type);

				if (b.type == gpp_yrrc)
				{
					target.add(tag_family::quicktime, key, std::to_string(load_be16(p.data() + 4)), {});
					return;
				}

				const auto language = language_name(load_be16(p.data() + 4));
				const auto rest = std::span(p).subspan(6);
				std::size_t used = 0;

				if (b.type == gpp_loci)
				{
					const auto place = asset_string(rest, used);
					cursor c(rest.subspan(used));
					c.skip(1); // role
					const auto longitude = c.s32() / 65536.0;
					const auto latitude = c.s32() / 65536.0;
					const auto altitude = c.s32() / 65536.0;
					if (!c.failed()) target.add(tag_family::quicktime, key, location_text(latitude, longitude, altitude, place), language);
					return;
				}

				if (b.type == gpp_kywd)
				{
					cursor c(rest);
					const auto count = c.u8();

					for (unsigned i = 0; i < count; ++i)
					{
						const auto keyword = c.take(c.u8());
						if (c.failed()) break;
						target.add(tag_family::quicktime, key, asset_string(keyword, used), language);
					}

					return;
				}

				target.add(tag_family::quicktime, key, asset_string(rest, used), language);
			}

			// The Xtra box Windows keeps tags and a rating in: each entry's values, under its name.
			void read_xtra(const box& b, const tag_target& target) const
			{
				const auto content = payload(b);

				mp4::for_each_xtra_entry(content, [&target](const mp4::xtra_entry& entry)
				{
					const auto name = entry.name.substr(0, entry.name.find('\0'));
					if (name.empty()) return;

					mp4::for_each_xtra_value(entry, [&](const mp4::xtra_value& value)
					{
						target.add(tag_family::asf, name, xtra_text(value), {});
					});
				});
			}

			void read_meta(const box& meta, const tag_target& target)
			{
				// ISO's meta box is a full box; QuickTime's is not, and begins with its handler.
				std::uint64_t skip = 0;
				if (meta.payload_size() >= 12 && _r.be32(meta.payload() + 8) == box_hdlr) skip = 4;
				else if (meta.payload_size() < 8 || _r.be32(meta.payload() + 4) != box_hdlr) return;

				std::uint32_t handler = 0;
				std::optional<std::vector<std::string>> keys;

				each_child(meta, [&](const box& b)
				{
					if (b.type == box_hdlr && handler == 0)
					{
						if (b.payload_size() >= 12) handler = _r.be32(b.payload() + 8);
					}
					else if (b.type == box_keys && handler == handler_mdta && !keys)
					{
						keys = read_keys(b);
					}
					else if (b.type == box_ilst && handler == handler_mdir)
					{
						read_items(b, target);
					}
					else if (b.type == box_ilst && keys)
					{
						read_keyed_items(b, *keys, target);
					}
				}, skip);
			}

			// The keys an 'mdta' handler names its items by, in order. A key outside the mdta namespace is
			// left unnamed, as FFmpeg leaves it.
			std::vector<std::string> read_keys(const box& b) const
			{
				const auto p = payload(b);
				cursor c(p);
				c.skip(4);
				const auto count = c.u32();
				std::vector<std::string> names;

				for (std::uint32_t i = 0; i < count && c.remaining() >= 8; ++i)
				{
					const auto size = c.u32();
					const auto space = c.u32();
					if (size < 8) break;

					const auto name = c.take(size - 8);
					if (c.failed()) break;
					names.push_back(space == handler_mdta ? text_from_utf8(name) : std::string());
				}

				return names;
			}

			// An iTunes item list: each item named by its type, holding data boxes.
			void read_items(const box& ilst, const tag_target& target)
			{
				each_child(ilst, [&](const box& item)
				{
					if (item.type == item_free_form)
					{
						read_free_form(item, target);
						return;
					}

					const auto key = box_type_text(item.type);

					// An item too short to hold a data box holds its value bare, as some writers left one and
					// FFmpeg reads it.
					if (item.payload_size() <= 8)
					{
						target.add(tag_family::itunes, key, implicit_value(item.type, payload(item)), {});
						return;
					}

					each_child(item, [&](const box& data)
					{
						if (data.type != box_data || data.payload_size() < 8) return;

						if (item.type == item_covr) add_picture(data, target);
						else read_data(data, key, item.type, tag_family::itunes, target);
					});
				});
			}

			// A free-form item, named by its mean and name boxes: "----:com.apple.iTunes:iTunSMPB".
			void read_free_form(const box& item, const tag_target& target)
			{
				std::string mean;
				std::string name;

				each_child(item, [&](const box& b)
				{
					if (b.type == box_mean && b.payload_size() > 4)
					{
						const auto p = payload(b);
						mean = text_from_utf8(std::span(p).subspan(4));
					}
					else if (b.type == box_name && b.payload_size() > 4)
					{
						const auto p = payload(b);
						name = text_from_utf8(std::span(p).subspan(4));
					}
					else if (b.type == box_data && b.payload_size() >= 8 && !name.empty())
					{
						read_data(b, std::format("----:{}:{}", mean, name), 0, tag_family::itunes, target);
					}
				});
			}

			// A QuickTime metadata item list: each item named by its key's place in the keys box, counted
			// from one.
			void read_keyed_items(const box& ilst, const std::vector<std::string>& keys, const tag_target& target)
			{
				each_child(ilst, [&](const box& item)
				{
					if (item.type == 0 || item.type > keys.size() || keys[item.type - 1].empty()) return;

					const auto& key = keys[item.type - 1];

					each_child(item, [&](const box& data)
					{
						if (data.type != box_data || data.payload_size() < 8) return;

						if (key == "com.apple.quicktime.artwork") add_picture(data, target);
						else read_data(data, key, 0, tag_family::quicktime, target);
					});
				});
			}

			// A data box: its type, its locale - a country, then a language, of which none means any - then
			// its value.
			void read_data(const box& data, const std::string_view key, const std::uint32_t item, const tag_family family,
			               const tag_target& target) const
			{
				const auto type = _r.be32(data.payload());
				const auto language = static_cast<std::uint16_t>(_r.be32(data.payload() + 4));
				const auto size = data.payload_size() - 8;
				std::string text;

				if (size <= max_box_read) text = data_text(type, _r.read(data.payload() + 8, static_cast<std::size_t>(size)), item);
				else text = binary_text(static_cast<std::size_t>(size));

				target.add(family, key, text, language == 0 ? std::string() : language_name(language));
			}

			// Cover art, located where its data box holds the image, and named by the box's type.
			void add_picture(const box& data, const tag_target& target) const
			{
				auto* const pictures = target.pictures();
				if (!pictures || !_in_file || data.payload_size() <= 8) return;

				media_picture picture;
				picture.location = {data.payload() + 8, data.payload_size() - 8};

				switch (_r.be32(data.payload()))
				{
				case 13: picture.mime_type = "image/jpeg";
					break;
				case 14: picture.mime_type = "image/png";
					break;
				case 27: picture.mime_type = "image/bmp";
					break;
				default: break;
				}

				pictures->push_back(std::move(picture));
			}

			void read_movie_extends(const box& mvex)
			{
				_m.extends = true;

				each_child(mvex, [&](const box& b)
				{
					const auto p = payload(b, 24);

					if (b.type == box_mehd && p.size() >= 8)
					{
						if (p[0] == 1 && p.size() >= 12) _m.fragment_duration = static_cast<std::uint64_t>(load_be32(p.data() + 4)) << 32 | load_be32(p.data() + 8);
						else if (p[0] == 0) _m.fragment_duration = load_be32(p.data() + 4);
					}
					else if (b.type == box_trex && p.size() >= 16 && _m.default_durations.size() < max_tracks)
					{
						_m.default_durations[load_be32(p.data() + 4)] = load_be32(p.data() + 12);
					}
				});
			}

			// A compressed movie, as QuickTime wrote from version 3: the movie box deflated in a cmvd box.
			void read_compressed_movie(const box& cmov)
			{
				const auto boxes = children(cmov);
				std::uint32_t method = 0;
				const box* data = nullptr;

				for (const auto& b : boxes.boxes)
				{
					if (b.type == box_dcom && b.payload_size() >= 4) method = _r.be32(b.payload());
					else if (b.type == box_cmvd && !data) data = &b;
				}

				if (method != fourcc("zlib") || !data || data->payload_size() < 4 || data->payload_size() - 4 > max_compressed_movie) return;

				const auto size = _r.be32(data->payload());
				if (size < 8 || size > max_compressed_movie) return;

				std::vector<std::uint8_t> inflated;
				if (!zlib_inflate(_r.read(data->payload() + 4, static_cast<std::size_t>(data->payload_size() - 4)), inflated, size)) return;

				reader inner(inflated);
				box moov;
				if (mp4::read_box(inner, 0, inner.size(), moov) != mp4::header_status::box || moov.type != mp4::moov) return;

				movie_reader(inner, _m, _out, _quicktime, false).read(moov);
			}

			reader& _r;
			movie& _m;
			media_scan_result& _out;
			bool _quicktime;
			// False for a compressed movie, whose offsets are not the file's.
			bool _in_file;
		};

		// Where a track's segment indexes start and end, in seconds.
		struct indexed_time
		{
			double start = 0;
			double end = 0;
		};

		class mp4_scan
		{
		public:
			mp4_scan(reader& r, const scan_intent intent, media_scan_result& out) : _r(r), _intent(intent), _out(out)
			{
			}

			void run()
			{
				const auto status = mp4::for_each_top_level_box(_r, [this](const box& b) { visit(b); });

				if (!_moov)
				{
					throw format_error(status == mp4::header_status::none ? "the MP4 has no movie box" : "an MP4 box runs past the end of the file");
				}

				finish_tracks();
				finish_duration();
				read_packet(status == mp4::header_status::none);
				if (_intent == scan_intent::inspect) describe();
			}

		private:
			void visit(const box& b)
			{
				++_box_count;
				if (_intent == scan_intent::inspect && _listed.size() < max_listed_boxes) _listed.push_back(b);

				if (b.type == mp4::ftyp && !_ftyp)
				{
					_ftyp = b;
					read_file_type(b);
				}
				else if (b.type == mp4::moov && !_moov)
				{
					// Read where it is met, while what the walk read of it is at hand. Movie boxes after the
					// first are not read, as the writer and FFmpeg read none.
					_moov = b;
					_movie.boxes = movie_reader(_r, _movie, _out, quicktime(), true).read(b);
				}
				else if (b.type == box_moof)
				{
					++_fragments;
				}
				else if (b.type == box_sidx)
				{
					read_segment_index(b);
				}
				else if (mp4::is_xmp_uuid(_r, b))
				{
					_xmp_uuid = b;
				}
			}

			bool quicktime()
			{
				return _ftyp ? _ftyp_quicktime : mp4::is_quicktime(_r, nullptr);
			}

			void read_file_type(const box& b)
			{
				// Judged now, while the box is at hand, rather than read again from the start of the file.
				_ftyp_quicktime = mp4::is_quicktime(_r, &b);
				if (b.payload_size() < 4) return;

				const auto p = _r.read(b.payload(), static_cast<std::size_t>(std::min<std::uint64_t>(b.payload_size(), 4 * 64)));
				_out.brand = box_type_text(load_be32(p.data()));
				if (p.size() >= 8) _minor_version = load_be32(p.data() + 4);
				for (std::size_t i = 8; i + 4 <= p.size(); i += 4) _brands.push_back(load_be32(p.data() + i));
			}

			// A segment index: the time its references span, from the earliest presentation time.
			void read_segment_index(const box& b)
			{
				const auto p = _r.read(b.payload(), static_cast<std::size_t>(std::min<std::uint64_t>(b.payload_size(), max_box_read)));
				cursor c(p);
				const auto version = c.u8();
				c.skip(3);
				const auto id = c.u32();
				const auto timescale = c.u32();
				const auto earliest = version == 1 ? c.u64() : c.u32();
				c.skip((version == 1 ? 8 : 4) + 2); // the first offset, and reserved
				const auto count = c.u16();
				if (c.failed() || timescale == 0) return;

				std::uint64_t total = 0;

				for (unsigned i = 0; i < count; ++i)
				{
					c.skip(4); // the reference's type and size
					total += c.u32();
					c.skip(4); // its stream access point
				}

				if (c.failed()) return;
				if (!_indexed.contains(id) && _indexed.size() >= max_tracks) return;

				const auto start = static_cast<double>(earliest) / timescale;
				const auto end = (static_cast<double>(earliest) + static_cast<double>(total)) / timescale;
				const auto [it, added] = _indexed.try_emplace(id, indexed_time{start, end});

				if (!added)
				{
					it->second.start = std::min(it->second.start, start);
					it->second.end = std::max(it->second.end, end);
				}
			}

			bool fragmented() const
			{
				return _fragments > 0 && _movie.extends;
			}

			void finish_tracks()
			{
				// The tracks another names as its chapters hold chapter titles or pictures, not a stream to play.
				std::vector<std::uint64_t> chapters;

				for (const auto& t : _movie.tracks)
				{
					for (const auto id : t.chapters)
					{
						if (id != t.stream.id) chapters.push_back(id);
					}
				}

				for (auto& t : _movie.tracks)
				{
					auto& s = t.stream;

					if (s.kind != media_stream_kind::audio && std::ranges::find(chapters, s.id) != chapters.end()) s.kind = media_stream_kind::data;
					if (t.timescale != 0 && t.media_duration) s.duration = static_cast<double>(*t.media_duration) / t.timescale;

					if (!(s.duration > 0))
					{
						if (const auto it = _indexed.find(static_cast<std::uint32_t>(s.id)); it != _indexed.end()) s.duration = it->second.end - it->second.start;
					}

					if (s.kind == media_stream_kind::video) finish_video(t);
					_out.streams.push_back(s);
				}
			}

			void finish_video(track& t) const
			{
				auto& v = t.stream.video;

				if (v.width == 0 || v.height == 0)
				{
					v.width = t.width;
					v.height = t.height;
				}

				// Frames a second: the time-to-sample table's count over its time where it was read, else the
				// sample count over the media's duration, else, in a fragmented movie, its default duration.
				double rate = 0;

				if (t.timed && t.timed_duration != 0 && t.timescale != 0)
				{
					rate = static_cast<double>(t.timed_samples) * t.timescale / static_cast<double>(t.timed_duration);
				}
				else if (t.sample_count != 0 && t.media_duration.value_or(0) != 0 && t.timescale != 0)
				{
					rate = static_cast<double>(t.sample_count) * t.timescale / static_cast<double>(*t.media_duration);
				}
				else if (fragmented() && t.timescale != 0)
				{
					const auto it = _movie.default_durations.find(static_cast<std::uint32_t>(t.stream.id));
					if (it != _movie.default_durations.end() && it->second != 0) rate = static_cast<double>(t.timescale) / it->second;
				}

				if (rate > 0 && std::isfinite(rate)) v.frame_rate = rate;

				// The track's transform, then the movie's. A transform that mirrors has a negative
				// determinant; undoing the mirror leaves the turn.
				const auto& m = _movie.matrix;
				const auto a = t.matrix[0] * m[0] + t.matrix[1] * m[2];
				const auto b = t.matrix[0] * m[1] + t.matrix[1] * m[3];
				const auto c = t.matrix[2] * m[0] + t.matrix[3] * m[2];
				const auto d = t.matrix[2] * m[1] + t.matrix[3] * m[3];
				v.mirrored = a * d - b * c < 0;

				const auto cosine = v.mirrored ? -a : a;
				const auto sine = v.mirrored ? -b : b;

				if (cosine != 0 || sine != 0)
				{
					const auto quarters = std::lround(std::atan2(sine, cosine) / (std::numbers::pi / 2));
					v.rotation = static_cast<int>((quarters % 4 + 4) % 4) * 90;
				}
			}

			void finish_duration()
			{
				// A fragmented movie's header times only what the movie box holds. The whole is what mehd
				// states, else what the segment indexes span; else it is left to the tracks.
				if (fragmented())
				{
					if (_movie.fragment_duration != 0 && _movie.timescale != 0)
					{
						_out.duration = static_cast<double>(_movie.fragment_duration) / _movie.timescale;
					}
					else
					{
						for (const auto& [id, time] : _indexed) _out.duration = std::max(_out.duration, time.end - time.start);
					}
				}
				else if (_movie.duration && _movie.timescale != 0)
				{
					_out.duration = static_cast<double>(*_movie.duration) / _movie.timescale;
				}
			}

			// The packet read_packet returns, from what the walk found: none when the walk stopped at
			// damage or the movie box is larger than an update reads, as read_packet then reads none.
			void read_packet(const bool walked_whole)
			{
				if (!walked_whole || _moov->size > mp4::max_moov) return;

				try
				{
					const auto in_movie = mp4::movie_xmp(_r, _movie.boxes);
					const auto place = mp4::packet_place(quicktime(), in_movie, _xmp_uuid ? &*_xmp_uuid : nullptr);
					if (place && place->size <= max_packet) _out.xmp = _r.read(place->offset, static_cast<std::size_t>(place->size));
				}
				catch (const format_error&)
				{
					// The movie's boxes are damaged where an update reads them, so read_packet reads none.
				}
			}

			std::string describe_box(const box& b)
			{
				std::string_view what;

				if (b.type == mp4::ftyp) what = "file type";
				else if (b.type == mp4::moov) what = b.start == _moov->start ? "movie" : "movie, not read";
				else if (b.type == mp4::mdat) what = "media data";
				else if (b.type == mp4::free_ || b.type == mp4::skip || b.type == mp4::wide) what = "free space";
				else if (b.type == box_moof) what = "movie fragment";
				else if (b.type == box_sidx) what = "segment index";
				else if (b.type == fourcc("mfra")) what = "fragment index";
				else if (b.type == mp4::pnot) what = "preview";
				else if (b.type == mp4::uuid) what = mp4::is_xmp_uuid(_r, b) ? "XMP packet" : "extension";

				auto value = std::format("offset {}, {} bytes", b.start, b.size);
				if (!what.empty()) value = std::format("{}, {}", what, value);
				return value;
			}

			// The file type, the top-level boxes and the tracks, with their handlers and sample descriptions.
			void describe()
			{
				auto& rows = _out.structure;

				add_structure_section(rows, "Summary", "mp4.summary", true);
				add_structure_row(rows, "File size", std::format("{} bytes", _r.size()));

				if (_ftyp)
				{
					if (!_out.brand.empty()) add_structure_row(rows, "Major brand", _out.brand);
					add_structure_row(rows, "Minor version", std::to_string(_minor_version));

					std::string brands;
					for (const auto brand : _brands) brands += (brands.empty() ? "" : ", ") + box_type_text(brand);
					if (!brands.empty()) add_structure_row(rows, "Compatible brands", brands);
				}
				else
				{
					add_structure_row(rows, "File type", "none, as in the oldest QuickTime movies");
				}

				if (_movie.timescale != 0) add_structure_row(rows, "Movie time scale", std::format("{} a second", _movie.timescale));
				if (_movie.duration) add_structure_row(rows, "Movie duration", std::format("{} units", *_movie.duration));
				if (_fragments != 0) add_structure_row(rows, "Movie fragments", std::to_string(_fragments));
				if (!_out.xmp.empty()) add_structure_row(rows, "XMP packet", std::format("{} bytes", _out.xmp.size()));

				add_structure_section(rows, "Boxes", "mp4.boxes");

				for (const auto& b : _listed)
				{
					auto& row = add_row(rows, box_type_text(b.type), describe_box(b));
					row.depth = 1;
					row.id = std::format("mp4.box.{}", b.start);
				}

				if (_box_count > _listed.size()) add_structure_row(rows, "Boxes not listed", std::to_string(_box_count - _listed.size()));

				add_structure_section(rows, "Tracks", "mp4.tracks");

				for (std::size_t i = 0; i < _movie.tracks.size(); ++i)
				{
					const auto& t = _movie.tracks[i];
					auto value = t.handler != 0 ? std::format("{} handler", box_type_text(t.handler)) : std::string("no handler");
					if (!t.handler_name.empty()) value += std::format(" \"{}\"", t.handler_name);
					if (t.entry_type != 0) value += std::format(", sample description {}", box_type_text(t.entry_type));
					if (t.entry_count > 1) value += std::format(" and {} more", t.entry_count - 1);
					if (t.timescale != 0) value += std::format(", time scale {}", t.timescale);
					if (t.sample_count != 0) value += std::format(", {} samples", t.sample_count);

					auto& row = add_row(rows, std::format("Track {}", t.stream.id), std::move(value));
					row.depth = 1;
					row.id = std::format("mp4.track.{}", i);
				}

				finish_structure_sections(rows);
			}

			reader& _r;
			scan_intent _intent;
			media_scan_result& _out;
			std::optional<box> _ftyp;
			bool _ftyp_quicktime = false;
			std::optional<box> _moov;
			// The last XMP uuid box, which read_packet takes.
			std::optional<box> _xmp_uuid;
			std::uint32_t _minor_version = 0;
			std::vector<std::uint32_t> _brands;
			movie _movie;
			std::uint64_t _fragments = 0;
			std::map<std::uint32_t, indexed_time> _indexed;
			std::uint64_t _box_count = 0;
			std::vector<box> _listed;
		};
	}

	void scan_mp4_media(reader& r, const scan_intent intent, media_scan_result& out)
	{
		mp4_scan(r, intent, out).run();
	}
}
