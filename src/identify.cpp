// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The format table and the one identification every operation dispatches through: each format's
// family, names, MIME type, extensions and capabilities in this build, and the order its signatures are
// tried in. What decode() and decode_document() read in this build is answered here too, from the codecs
// built, so that identifying or scanning a file links no decoder.

#include "id3_internal.h"
#include "libfiles/decode.h"
#include "libfiles/document.h"
#include "libfiles/identify.h"
#include "libfiles/media.h"
#include "libfiles/scan.h"
#include "scan_internal.h"

#include <algorithm>
#include <array>
#include <vector>

namespace libfiles
{
	namespace
	{
		using namespace std::string_view_literals;

		constexpr std::array jpeg_ext = {".jpg"sv, ".jpeg"sv, ".jpe"sv, ".jfif"sv};
		constexpr std::array png_ext = {".png"sv};
		constexpr std::array webp_ext = {".webp"sv};
		constexpr std::array gif_ext = {".gif"sv};
		constexpr std::array psd_ext = {".psd"sv, ".psb"sv};
		constexpr std::array bmp_ext = {".bmp"sv, ".dib"sv};
		constexpr std::array tiff_ext = {".tif"sv, ".tiff"sv};
		constexpr std::array heif_ext = {".heic"sv, ".heif"sv, ".avif"sv, ".hif"sv};
		constexpr std::array jxl_ext = {".jxl"sv};
		constexpr std::array pcx_ext = {".pcx"sv};
		constexpr std::array wpg_ext = {".wpg"sv};
		constexpr std::array mp4_ext = {".mp4"sv, ".m4v"sv, ".m4a"sv, ".m4b"sv, ".mov"sv, ".qt"sv, ".3gp"sv, ".3g2"sv};
		constexpr std::array mkv_ext = {".mkv"sv, ".mka"sv, ".mks"sv, ".webm"sv};
		constexpr std::array avi_ext = {".avi"sv};
		constexpr std::array wav_ext = {".wav"sv, ".wave"sv};
		constexpr std::array aiff_ext = {".aif"sv, ".aiff"sv, ".aifc"sv};
		constexpr std::array asf_ext = {".wmv"sv, ".wma"sv, ".asf"sv};
		constexpr std::array mpa_ext = {".mp3"sv, ".mp2"sv, ".mp1"sv, ".mpa"sv};
		constexpr std::array flac_ext = {".flac"sv};
		constexpr std::array ogg_ext = {".ogg"sv, ".oga"sv, ".ogv"sv, ".opus"sv, ".spx"sv};
		constexpr std::array flv_ext = {".flv"sv};
		constexpr std::array ts_ext = {".ts"sv, ".m2ts"sv, ".mts"sv, ".m2t"sv};
		constexpr std::array ps_ext = {".mpg"sv, ".mpeg"sv, ".m2p"sv, ".vob"sv, ".mod"sv, ".tod"sv};

		constexpr auto c_scan = capability::scan;
		constexpr auto c_media = capability::scan_media | capability::read_packet;
		constexpr auto c_updated = capability::read_packet | capability::update;

		// What the build adds to a still format: its decoders and encoder.
		capability built(const image_format image)
		{
			auto c = capability::none;
			if (image == image_format::unknown) return c;
			if (can_scan(image)) c = c | capability::scan;
			if (can_decode(image)) c = c | capability::decode;
			if (can_decode_document(image)) c = c | capability::decode_document;
#ifdef LIBFILES_JPEG
			if (image == image_format::jpeg) c = c | capability::encode;
#endif
#ifdef LIBFILES_PNG
			if (image == image_format::png) c = c | capability::encode;
#endif
#ifdef LIBFILES_WEBP
			if (image == image_format::webp) c = c | capability::encode;
#endif
			return c;
		}

		file_type_info row(const file_type type, const file_family family, const std::string_view name, const std::string_view mime,
		                   const std::span<const std::string_view> extensions, const image_format image, const media_format media,
		                   const file_format update, const capability fixed)
		{
			file_type_info i;
			i.type = type;
			i.family = family;
			i.name = name;
			i.mime_type = mime;
			i.extensions = extensions;
			i.image = image;
			i.media = media;
			i.update_format = update;
			i.capabilities = fixed | built(image);
			return i;
		}

		// unknown first, then each type at its own index.
		const std::vector<file_type_info>& table()
		{
			static const std::vector<file_type_info> rows = []
			{
				using enum file_type;
				const auto img = file_family::image;
				const auto av = file_family::media;
				const auto none = capability::none;
				const auto no_media = media_format::unknown;
				const auto no_image = image_format::unknown;
				const auto no_update = file_format::unknown;

				return std::vector<file_type_info>{
					file_type_info{},
					row(jpeg, img, "JPEG", "image/jpeg", jpeg_ext, image_format::jpeg, no_media, file_format::jpeg, c_scan | c_updated),
					row(png, img, "PNG", "image/png", png_ext, image_format::png, no_media, file_format::png, c_scan | c_updated),
					row(webp, img, "WebP", "image/webp", webp_ext, image_format::webp, no_media, file_format::webp, c_scan | c_updated),
					row(gif, img, "GIF", "image/gif", gif_ext, image_format::gif, no_media, file_format::gif, c_scan | c_updated),
					row(psd, img, "Photoshop", "image/vnd.adobe.photoshop", psd_ext, image_format::psd, no_media, file_format::psd, c_scan | c_updated),
					row(bmp, img, "BMP", "image/bmp", bmp_ext, image_format::bmp, no_media, no_update, none),
					row(tiff, img, "TIFF", "image/tiff", tiff_ext, image_format::tiff, no_media, file_format::tiff, c_scan | c_updated),
					row(heif, img, "HEIF and AVIF", "image/heif", heif_ext, image_format::heif, no_media, no_update, capability::read_packet),
					row(jxl, img, "JPEG XL", "image/jxl", jxl_ext, image_format::jxl, no_media, no_update, capability::read_packet),
					row(pcx, img, "PCX", "image/vnd.zbrush.pcx", pcx_ext, image_format::pcx, no_media, no_update, none),
					row(wpg, img, "WordPerfect Graphics", "image/x-wpg", wpg_ext, image_format::wpg, no_media, no_update, none),
					row(mp4, av, "MP4 and QuickTime", "video/mp4", mp4_ext, no_image, media_format::mp4, file_format::mp4, c_media | c_updated),
					row(matroska, av, "Matroska and WebM", "video/x-matroska", mkv_ext, no_image, media_format::matroska, no_update,
					    capability::scan_media),
					row(avi, av, "AVI", "video/x-msvideo", avi_ext, no_image, media_format::avi, file_format::riff, c_media | c_updated),
					row(wav, av, "WAV", "audio/wav", wav_ext, no_image, media_format::wav, file_format::riff, c_media | c_updated),
					row(aiff, av, "AIFF", "audio/aiff", aiff_ext, no_image, media_format::aiff, no_update, c_media),
					row(asf, av, "ASF", "video/x-ms-asf", asf_ext, no_image, media_format::asf, file_format::asf, c_media | c_updated),
					row(mpeg_audio, av, "MPEG audio", "audio/mpeg", mpa_ext, no_image, media_format::mpeg_audio, file_format::mp3,
					    c_media | c_updated),
					row(flac, av, "FLAC", "audio/flac", flac_ext, no_image, media_format::flac, no_update, c_media),
					row(ogg, av, "Ogg", "audio/ogg", ogg_ext, no_image, media_format::ogg, no_update, capability::scan_media),
					row(flv, av, "FLV", "video/x-flv", flv_ext, no_image, media_format::flv, file_format::flv, c_media | c_updated),
					row(mpeg_ts, av, "MPEG transport stream", "video/mp2t", ts_ext, no_image, media_format::mpeg_ts, no_update,
					    capability::scan_media),
					row(mpeg_ps, av, "MPEG program stream", "video/mpeg", ps_ext, no_image, media_format::mpeg_ps, no_update,
					    capability::scan_media),
				};
			}();

			return rows;
		}

		// WordPerfect's own header, "\xFFWPC", naming a WPG (type 0x16) that is not encrypted, as the native
		// reader requires.
		bool is_wpg(const std::span<const std::uint8_t> h) noexcept
		{
			return h.size() >= 16 && h[0] == 0xFF && h[1] == 'W' && h[2] == 'P' && h[3] == 'C' && h[9] == 0x16 && h[14] == 0 && h[15] == 0;
		}

		// A ZSoft header the native reader takes: run-length encoded, and a layout it decodes - 1 to 8 bits
		// in one plane, or 8 bits in three. The planes lie at byte 65, so fewer bytes name no PCX.
		bool is_pcx(const std::span<const std::uint8_t> h) noexcept
		{
			if (h.size() < 128 || h[0] != 0x0A || h[2] != 1) return false;
			if (h[1] != 0 && (h[1] < 2 || h[1] > 5)) return false;

			const auto bits = h[3];
			const auto planes = h[65];
			return (planes == 3 && bits == 8) || (planes == 1 && (bits == 1 || bits == 2 || bits == 4 || bits == 8));
		}

		char lower(const char c) noexcept
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}
	}

	std::span<const file_type_info> file_types() noexcept
	{
		return std::span(table()).subspan(1);
	}

	// What decode() and decode_document() read depends on the codecs built, so it is answered here, with the
	// table, rather than where the decoders are: a program that only identifies or scans files links none.
	bool can_decode(const image_format format) noexcept
	{
#ifdef LIBFILES_JPEG
		if (format == image_format::jpeg) return true;
#endif
#ifdef LIBFILES_PNG
		if (format == image_format::png) return true;
#endif
#ifdef LIBFILES_WEBP
		if (format == image_format::webp) return true;
#endif
#ifdef LIBFILES_HEIF
		if (format == image_format::heif) return true;
#endif
#ifdef LIBFILES_JXL
		if (format == image_format::jxl) return true;
#endif
		return format == image_format::psd;
	}

	bool can_decode_document(const image_format format) noexcept
	{
#ifdef LIBFILES_JPEG
		if (format == image_format::jpeg) return true;
#endif
#ifdef LIBFILES_PNG
		if (format == image_format::png) return true;
#endif
#ifdef LIBFILES_GIF
		if (format == image_format::gif) return true;
#endif
#ifdef LIBFILES_TIFF
		if (format == image_format::tiff) return true;
#endif
		return format == image_format::pcx || format == image_format::bmp ||
			format == image_format::wpg || format == image_format::psd;
	}

	const file_type_info& info(const file_type type) noexcept
	{
		const auto& rows = table();
		const auto index = static_cast<std::size_t>(type);
		return index < rows.size() ? rows[index] : rows.front();
	}

	file_type file_type_from_extension(std::string_view extension) noexcept
	{
		if (extension.starts_with('.')) extension.remove_prefix(1);
		if (extension.empty()) return file_type::unknown;

		for (const auto& i : file_types())
		{
			for (const auto e : i.extensions)
			{
				const auto name = e.substr(1);
				if (name.size() == extension.size() && std::ranges::equal(name, extension, {}, {}, lower)) return i.type;
			}
		}

		return file_type::unknown;
	}

	file_type identify(const std::span<const std::uint8_t> head) noexcept
	{
		switch (detect_image_format(head))
		{
		case image_format::jpeg: return file_type::jpeg;
		case image_format::png: return file_type::png;
		case image_format::webp: return file_type::webp;
		case image_format::gif: return file_type::gif;
		case image_format::psd: return file_type::psd;
		case image_format::tiff: return file_type::tiff;
		case image_format::heif: return file_type::heif;
		case image_format::jxl: return file_type::jxl;
		default: break;
		}

		switch (detect_media_format(head))
		{
		case media_format::mp4: return file_type::mp4;
		case media_format::matroska: return file_type::matroska;
		case media_format::avi: return file_type::avi;
		case media_format::wav: return file_type::wav;
		case media_format::aiff: return file_type::aiff;
		case media_format::asf: return file_type::asf;
		case media_format::mpeg_audio: return file_type::mpeg_audio;
		case media_format::flac: return file_type::flac;
		case media_format::ogg: return file_type::ogg;
		case media_format::flv: return file_type::flv;
		case media_format::mpeg_ts: return file_type::mpeg_ts;
		case media_format::mpeg_ps: return file_type::mpeg_ps;
		case media_format::unknown: break;
		}

		// "BM" is two bytes, which a QuickTime file's first box size can begin with.
		if (detect_image_format(head) == image_format::bmp) return file_type::bmp;
		if (is_wpg(head)) return file_type::wpg;
		if (is_pcx(head)) return file_type::pcx;
		return file_type::unknown;
	}

	file_type identify(reader& r)
	{
		try
		{
			std::vector<std::uint8_t> head(static_cast<std::size_t>(std::min<std::uint64_t>(r.size(), identify_bytes)));
			r.read(0, head.data(), head.size());
			auto type = identify(head);

			// A leading ID3v2 tag says nothing of what follows it: FLAC is sometimes tagged that way too.
			if (type == file_type::mpeg_audio)
			{
				const auto tag = detail::leading_id3_size(head);

				if (tag != 0 && r.contains(tag, 4))
				{
					std::uint8_t after[4];
					r.read(tag, after, sizeof(after));
					if (detail::starts_with(after, "fLaC")) type = file_type::flac;
				}
			}

			return type;
		}
		catch (const read_error&)
		{
			return file_type::unknown;
		}
	}
}
