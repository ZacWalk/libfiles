// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The tags media files share - ID3v2, ID3v1, APE and Vorbis comments, with FLAC pictures - the
// mapping of every container's tag names onto media_properties, and dates and places as tags write them.
// An ID3v2 tag's structure is read by id3.cpp, which the MP3 writer and read_packet share.

#include "id3_internal.h"
#include "media_internal.h"
#include "vocabulary_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace libfiles::detail
{
	namespace
	{
		// The most a tag reader holds of one picture, and of one value or frame: room for a picture that
		// large and the fields before it. What is larger is located, or described and left.
		constexpr std::size_t max_picture = 16u << 20;
		constexpr std::size_t max_value = max_picture + (64u << 10);

		// The most frames, items or fields one tag is read for, and the most keywords, comments and genres
		// kept: far past any tagger's, and short of what a crafted tag could make a scan hold or compare.
		constexpr std::size_t max_entries = 65536;
		constexpr std::size_t max_kept = 4096;

		constexpr auto npos = std::string_view::npos;

		// ---- Text ------------------------------------------------------------------------------------

		constexpr char ascii_lower(const char c) noexcept
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}

		constexpr bool is_digit(const char c) noexcept
		{
			return c >= '0' && c <= '9';
		}

		constexpr bool is_alpha(const char c) noexcept
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		}

		bool iequals(const std::string_view a, const std::string_view b) noexcept
		{
			return a.size() == b.size() &&
				std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) { return ascii_lower(x) == ascii_lower(y); });
		}

		bool istarts_with(const std::string_view text, const std::string_view prefix) noexcept
		{
			return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
		}

		std::string_view as_text(const std::span<const std::uint8_t> bytes) noexcept
		{
			return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
		}

		// A whole number of at most nine digits, so that it fits an int.
		std::optional<int> parse_count(std::string_view text) noexcept
		{
			text = trim_text(text);
			if (text.empty() || text.size() > 9) return {};

			auto v = 0;

			for (const auto c : text)
			{
				if (!is_digit(c)) return {};
				v = v * 10 + (c - '0');
			}

			return v;
		}

		// Digits, perhaps with a fraction after a point, as ratings are written.
		std::optional<double> parse_decimal(std::string_view text) noexcept
		{
			text = trim_text(text);
			std::size_t i = 0;
			std::size_t digits = 0;
			auto v = 0.0;

			for (; i < text.size() && is_digit(text[i]); ++i, ++digits)
			{
				if (digits == 9) return {};
				v = v * 10 + (text[i] - '0');
			}

			if (i < text.size() && text[i] == '.')
			{
				auto scale = 0.1;

				for (++i; i < text.size() && is_digit(text[i]); ++i, ++digits, scale /= 10)
				{
					v += (text[i] - '0') * scale;
				}
			}

			if (digits == 0 || i != text.size()) return {};
			return v;
		}

		// "7", "7/12" or "/12": the number and the total, each 0 when it is not there.
		std::pair<int, int> number_and_total(const std::string_view text) noexcept
		{
			const auto slash = text.find('/');
			const auto number = parse_count(text.substr(0, slash)).value_or(0);
			const auto total = slash == npos ? 0 : parse_count(text.substr(slash + 1)).value_or(0);
			return {number, total};
		}

		// Several values of one tag, trimmed and joined as Windows shows them; empty ones are dropped.
		void append_value(std::string& joined, const std::string_view value)
		{
			const auto v = trim_text(value);
			if (v.empty()) return;
			if (!joined.empty()) joined += "; ";
			joined += v;
		}

		std::string byte_count(const std::uint64_t count)
		{
			return std::to_string(count) + (count == 1 ? " byte" : " bytes");
		}

		// A tag whose value is described rather than reproduced: listed, though no property takes it, as one
		// would take "7 bytes" for the title a frame could not be read for.
		void add_description(media_scan_result& out, const std::string_view key, std::string description)
		{
			out.tags.push_back({std::string(key), std::move(description), {}});
		}

		// The ID3 and FLAC picture types, as a tag's description of a picture names them.
		constexpr std::array<std::string_view, 21> picture_types = {
			"other", "file icon", "other file icon", "front cover", "back cover", "leaflet page", "media",
			"lead artist", "artist", "conductor", "band", "composer", "lyricist", "recording location",
			"during recording", "during performance", "video capture", "bright coloured fish", "illustration",
			"band logo", "publisher logo",
		};

		// "image/jpeg, front cover, 51234 bytes": a picture described in place of its bytes.
		std::string describe_picture(const std::string_view mime, const int type, const std::uint64_t size)
		{
			std::string text(mime);
			if (!text.empty()) text += ", ";

			if (type >= 0 && type < static_cast<int>(picture_types.size())) text += picture_types[static_cast<std::size_t>(type)];
			else text += "picture type " + std::to_string(type);

			return text + ", " + byte_count(size);
		}

		// The image type a file name's extension names, for a tag that names no MIME type but a file.
		std::string_view mime_of_file_name(const std::string_view name) noexcept
		{
			const auto dot = name.rfind('.');
			if (dot == npos) return {};

			const auto extension = name.substr(dot + 1);
			if (iequals(extension, "jpg") || iequals(extension, "jpeg") || iequals(extension, "jpe")) return "image/jpeg";
			if (iequals(extension, "png")) return "image/png";
			if (iequals(extension, "gif")) return "image/gif";
			if (iequals(extension, "bmp")) return "image/bmp";
			if (iequals(extension, "webp")) return "image/webp";
			if (iequals(extension, "tif") || iequals(extension, "tiff")) return "image/tiff";
			return {};
		}

		// A language as a comment names it: ISO 639-2 in lower case, or nothing for "und", "xxx" and what
		// is not a language code at all.
		std::string comment_language(const std::string_view language)
		{
			std::string code;

			for (const auto c : trim_text(language))
			{
				if (!is_alpha(c) && c != '-') return {};
				code += ascii_lower(c);
			}

			if (code == "und" || code == "xxx") return {};
			return code;
		}

		// ---- Genres ----------------------------------------------------------------------------------

		// ID3v1's genres, with Winamp's additions, by number.
		constexpr std::array<std::string_view, 192> id3v1_genres = {
			"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
			"New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
			"Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
			"Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game",
			"Sound Clip", "Gospel", "Noise", "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
			"Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
			"Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
			"Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
			"Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka",
			"Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing",
			"Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
			"Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
			"Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata",
			"Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
			"Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo",
			"A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore Techno",
			"Terror", "Indie", "BritPop", "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap",
			"Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian", "Christian Rock", "Merengue",
			"Salsa", "Thrash Metal", "Anime", "Jpop", "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra",
			"Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro",
			"Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth",
			"Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz",
			"Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music",
			"Neoclassical", "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock",
			"G-Funk", "Dubstep", "Garage Rock", "Psybient",
		};

		std::string_view genre_name(const int number) noexcept
		{
			return number >= 0 && number < static_cast<int>(id3v1_genres.size()) ? id3v1_genres[static_cast<std::size_t>(number)] : std::string_view{};
		}

		// One genre as an ID3v2 frame writes it - a name, an ID3v1 number, ID3v2.4's "RX" and "CR", or
		// ID3v2.3's "(17)" references with perhaps a refinement after them - as names.
		void resolve_genre(std::string_view text, std::vector<std::string_view>& names)
		{
			text = trim_text(text);
			if (text.empty()) return;

			const auto reference = [](const std::string_view r) -> std::string_view
			{
				if (r == "RX") return "Remix";
				if (r == "CR") return "Cover";
				if (const auto number = parse_count(r)) return genre_name(*number);
				return {};
			};

			if (const auto name = reference(text); !name.empty())
			{
				names.push_back(name);
				return;
			}

			std::vector<std::string_view> referenced;
			auto rest = text;

			while (rest.size() >= 3 && rest[0] == '(' && rest[1] != '(')
			{
				const auto close = rest.find(')');
				if (close == npos) break;

				const auto name = reference(rest.substr(1, close - 1));
				if (name.empty()) break;

				referenced.push_back(name);
				rest.remove_prefix(close + 1);
			}

			// A refinement is the more specific name; "((" stands for a parenthesis that begins one.
			if (rest.starts_with("((")) rest.remove_prefix(1);
			rest = trim_text(rest);

			if (!rest.empty()) names.push_back(rest);
			else names.insert(names.end(), referenced.begin(), referenced.end());
		}

		// A genre frame's values, joined where it held several, with every reference named.
		std::string resolve_genres(const std::string_view value)
		{
			std::vector<std::string_view> names;

			for (std::size_t start = 0; names.size() < max_kept;)
			{
				const auto semicolon = value.find(';', start);
				resolve_genre(value.substr(start, semicolon == npos ? npos : semicolon - start), names);
				if (semicolon == npos) break;
				start = semicolon + 1;
			}

			std::string joined;

			for (auto i = names.begin(); i != names.end(); ++i)
			{
				const auto same = [i](const std::string_view n) { return iequals(n, *i); };
				if (std::none_of(names.begin(), i, same)) append_value(joined, *i);
			}

			return joined;
		}

		// ---- The mapping -----------------------------------------------------------------------------

		// What a tag's name maps to: a property, or one of the ways a property is written.
		enum class field : std::uint8_t
		{
			none,
			title,
			// The title only when the file has none: QuickTime's display name.
			display_name,
			artist,
			album,
			album_artist,
			composer,
			performer,
			genre,
			// ID3v2's genre: names, ID3v1 numbers and references.
			genre_references,
			// iTunes' gnre: an ID3v1 genre's number plus one.
			genre_number,
			publisher,
			copyright,
			description,
			synopsis,
			show,
			encoder,
			camera_make,
			camera_model,
			comment,
			keywords,
			// Windows' categories, which may hold commas: split only at semicolons.
			categories,
			// Stars from 0 to 5, a fraction rounded; a larger number is a percent.
			rating,
			// Windows' percent, where 0 means unrated.
			percent_rating,
			year,
			created,
			// A date that stands for the recording only when nothing better does: a release or encoding date.
			created_fallback,
			original_release,
			track,
			// ASF's zero-based WM/Track, taken only when no track number is known.
			track_from_zero,
			track_count,
			disc,
			disc_count,
			season,
			episode,
			location,
		};

		struct mapping
		{
			std::string_view key;
			field target;
		};

		// The names below are those FFmpeg's demuxers normalise each container's tags from, so that a
		// recording files where it did when Diffractor read FFmpeg's metadata.

		// MP4 ilst atoms, named as box_type_text gives them.
		constexpr mapping itunes_names[] = {
			{"\xC2\xA9" "nam", field::title},
			{"\xC2\xA9" "ART", field::artist},
			{"\xC2\xA9" "aut", field::artist},
			{"aART", field::album_artist},
			{"\xC2\xA9" "alb", field::album},
			{"\xC2\xA9" "cmt", field::comment},
			{"\xC2\xA9" "inf", field::comment},
			{"\xC2\xA9" "des", field::description},
			{"desc", field::description},
			{"ldes", field::synopsis},
			{"\xC2\xA9" "day", field::created},
			{"\xC2\xA9" "gen", field::genre},
			{"gnre", field::genre_number},
			{"trkn", field::track},
			{"disk", field::disc},
			{"\xC2\xA9" "wrt", field::composer},
			{"\xC2\xA9" "com", field::composer},
			{"\xC2\xA9" "prf", field::performer},
			{"\xC2\xA9" "too", field::encoder},
			{"\xC2\xA9" "enc", field::encoder},
			{"\xC2\xA9" "swr", field::encoder},
			{"cprt", field::copyright},
			{"\xC2\xA9" "cpy", field::copyright},
			{"\xC2\xA9" "pub", field::publisher},
			{"tvsh", field::show},
			{"tvsn", field::season},
			{"tves", field::episode},
			{"keyw", field::keywords},
			{"\xC2\xA9" "key", field::keywords},
			{"\xC2\xA9" "mak", field::camera_make},
			{"\xC2\xA9" "mod", field::camera_model},
			{"\xC2\xA9" "xyz", field::location},
			// iTunes' content advisory, not a rating.
			{"rtng", field::none},
		};

		// QuickTime's udta atoms, 3GPP's asset boxes and the keys of a 'mdta' handler.
		constexpr mapping quicktime_names[] = {
			{"\xC2\xA9" "nam", field::title},
			{"titl", field::title},
			{"\xC2\xA9" "ART", field::artist},
			{"\xC2\xA9" "aut", field::artist},
			{"auth", field::artist},
			{"aART", field::album_artist},
			{"\xC2\xA9" "alb", field::album},
			{"albm", field::album},
			{"\xC2\xA9" "cmt", field::comment},
			{"\xC2\xA9" "inf", field::comment},
			{"\xC2\xA9" "des", field::description},
			{"dscp", field::description},
			{"desc", field::description},
			{"ldes", field::synopsis},
			{"\xC2\xA9" "day", field::created},
			{"yrrc", field::created},
			{"\xC2\xA9" "wrt", field::composer},
			{"\xC2\xA9" "com", field::composer},
			{"\xC2\xA9" "prf", field::performer},
			{"perf", field::performer},
			{"\xC2\xA9" "gen", field::genre},
			{"gnre", field::genre},
			{"\xC2\xA9" "cpy", field::copyright},
			{"cprt", field::copyright},
			{"\xC2\xA9" "swr", field::encoder},
			{"\xC2\xA9" "too", field::encoder},
			{"\xC2\xA9" "enc", field::encoder},
			{"\xC2\xA9" "pub", field::publisher},
			{"\xC2\xA9" "mak", field::camera_make},
			{"manu", field::camera_make},
			{"\xC2\xA9" "mod", field::camera_model},
			{"modl", field::camera_model},
			{"\xC2\xA9" "xyz", field::location},
			{"loci", field::location},
			{"\xC2\xA9" "key", field::keywords},
			{"kywd", field::keywords},
			{"keyw", field::keywords},
			{"trkn", field::track},
			{"disk", field::disc},
			{"tvsh", field::show},
			{"tvsn", field::season},
			{"tves", field::episode},
			// 3GPP's content rating, not stars.
			{"rtng", field::none},
			{"com.apple.quicktime.make", field::camera_make},
			{"com.apple.quicktime.model", field::camera_model},
			{"com.apple.quicktime.software", field::encoder},
			{"com.apple.quicktime.creationdate", field::created},
			{"com.apple.quicktime.location.ISO6709", field::location},
			{"com.apple.quicktime.title", field::title},
			{"com.apple.quicktime.displayname", field::display_name},
			{"com.apple.quicktime.artist", field::artist},
			{"com.apple.quicktime.author", field::artist},
			{"com.apple.quicktime.album", field::album},
			{"com.apple.quicktime.comment", field::comment},
			{"com.apple.quicktime.information", field::comment},
			{"com.apple.quicktime.description", field::description},
			{"com.apple.quicktime.keywords", field::keywords},
			{"com.apple.quicktime.copyright", field::copyright},
			{"com.apple.quicktime.genre", field::genre},
			{"com.apple.quicktime.publisher", field::publisher},
			{"com.apple.quicktime.year", field::year},
			{"com.apple.quicktime.rating.user", field::rating},
		};

		// ID3v2.3 and 2.4 frame IDs, and ID3v2.2's three-letter ones; those the MP3 writer writes by the names
		// vocabulary_internal.h gives them.
		constexpr mapping id3v2_names[] = {
			{vocabulary::id3_title.v23, field::title},
			{vocabulary::id3_title.v22, field::title},
			{vocabulary::id3_artist.v23, field::artist},
			{vocabulary::id3_artist.v22, field::artist},
			{vocabulary::id3_album_artist.v23, field::album_artist},
			{vocabulary::id3_album_artist.v22, field::album_artist},
			{vocabulary::id3_album.v23, field::album},
			{vocabulary::id3_album.v22, field::album},
			{"TCOM", field::composer},
			{"TCM", field::composer},
			{"TPE3", field::performer},
			{"TP3", field::performer},
			{vocabulary::id3_genre.v23, field::genre_references},
			{vocabulary::id3_genre.v22, field::genre_references},
			{"TPUB", field::publisher},
			{"TPB", field::publisher},
			{vocabulary::id3_copyright.v23, field::copyright},
			{vocabulary::id3_copyright.v22, field::copyright},
			{"TENC", field::encoder},
			{"TEN", field::encoder},
			{"TSSE", field::encoder},
			{"TSS", field::encoder},
			{vocabulary::id3_track.v23, field::track},
			{vocabulary::id3_track.v22, field::track},
			{vocabulary::id3_disc.v23, field::disc},
			{vocabulary::id3_disc.v22, field::disc},
			// The year alone; read_id3v2 makes a date of it with the day and time ID3v2.3 keeps in TDAT and TIME.
			{vocabulary::id3_year.v23, field::year},
			{vocabulary::id3_year.v22, field::year},
			{vocabulary::id3_recording_time, field::created},
			{"TDRL", field::created_fallback},
			{"TDEN", field::created_fallback},
			{"TDOR", field::original_release},
			{"TORY", field::original_release},
			{"TOR", field::original_release},
			// Only a comment without a description: "COMM:iTunNORM" and its like are data, not comments.
			{vocabulary::id3_comment.v23, field::comment},
			{vocabulary::id3_comment.v22, field::comment},
		};

		// ID3v1's fields, as read_id3v1 names them after "ID3v1:".
		constexpr mapping id3v1_names[] = {
			{"title", field::title},
			{"artist", field::artist},
			{"album", field::album},
			{"year", field::year},
			{"comment", field::comment},
			{"track", field::track},
			{"genre", field::genre},
		};

		constexpr mapping ape_names[] = {
			{"Title", field::title},
			{"Artist", field::artist},
			{"Album", field::album},
			{"Album Artist", field::album_artist},
			{"AlbumArtist", field::album_artist},
			{"Composer", field::composer},
			{"Conductor", field::performer},
			{"Genre", field::genre},
			{"Year", field::created},
			{"Record Date", field::created},
			{"Track", field::track},
			{"Disc", field::disc},
			{"Comment", field::comment},
			{"Copyright", field::copyright},
			{"Publisher", field::publisher},
			{"Label", field::publisher},
			{"Rating", field::rating},
		};

		constexpr mapping riff_info_names[] = {
			{vocabulary::info_title, field::title},
			{"TITL", field::title},
			{vocabulary::info_artist, field::artist},
			{"IPRD", field::album},
			{vocabulary::info_comment, field::comment},
			{"COMM", field::comment},
			{vocabulary::info_created, field::created},
			{"IDIT", field::created},
			{vocabulary::info_genre, field::genre},
			{vocabulary::info_copyright, field::copyright},
			{"ISFT", field::encoder},
			{"ITCH", field::encoder},
			{"ISBJ", field::description},
			{"IKEY", field::keywords},
			{"ITRK", field::track},
			{"IPRT", field::track},
		};

		constexpr mapping asf_names[] = {
			{vocabulary::asf_content_description[vocabulary::asf_title], field::title},
			{vocabulary::asf_content_description[vocabulary::asf_author], field::artist},
			{vocabulary::asf_content_description[vocabulary::asf_copyright], field::copyright},
			// FFmpeg files the Content Description's description as the comment.
			{vocabulary::asf_content_description[vocabulary::asf_description], field::comment},
			// The Content Description's rating names an audience, not stars.
			{"Rating", field::none},
			{"WM/AlbumTitle", field::album},
			{"WM/AlbumArtist", field::album_artist},
			{"WM/Composer", field::composer},
			{"WM/Conductor", field::performer},
			{"WM/Genre", field::genre},
			{"WM/Year", field::year},
			{"WM/TrackNumber", field::track},
			{"WM/Track", field::track_from_zero},
			{"WM/PartOfSet", field::disc},
			{"WM/Publisher", field::publisher},
			{"WM/EncodedBy", field::encoder},
			{"WM/ToolName", field::encoder},
			{"WM/Tool", field::encoder},
			{"WM/EncodingSettings", field::encoder},
			{"WM/OriginalReleaseYear", field::original_release},
			{"WM/OriginalReleaseTime", field::original_release},
			{vocabulary::wm_category, field::categories},
			{vocabulary::wm_shared_user_rating, field::percent_rating},
		};

		constexpr mapping matroska_names[] = {
			{"TITLE", field::title},
			{"ARTIST", field::artist},
			{"ALBUM_ARTIST", field::album_artist},
			{"COMPOSER", field::composer},
			{"LEAD_PERFORMER", field::performer},
			{"PERFORMER", field::performer},
			{"GENRE", field::genre},
			{"COMMENT", field::comment},
			{"DESCRIPTION", field::description},
			{"SUMMARY", field::description},
			{"SYNOPSIS", field::synopsis},
			{"COPYRIGHT", field::copyright},
			{"PUBLISHER", field::publisher},
			{"LABEL", field::publisher},
			{"ENCODER", field::encoder},
			{"ENCODED_BY", field::encoder},
			{"DATE_RECORDED", field::created},
			{"DATE", field::created},
			{"DATE_RELEASED", field::created_fallback},
			{"DATE_ORIGINAL", field::original_release},
			{"ORIGINAL_DATE", field::original_release},
			{"KEYWORDS", field::keywords},
			{"RATING", field::rating},
			{"PART_NUMBER", field::track},
			{"TOTAL_PARTS", field::track_count},
		};

		constexpr mapping vorbis_names[] = {
			{"TITLE", field::title},
			{"ARTIST", field::artist},
			{"ALBUM", field::album},
			{"ALBUMARTIST", field::album_artist},
			{"ALBUM ARTIST", field::album_artist},
			{"COMPOSER", field::composer},
			{"PERFORMER", field::performer},
			{"GENRE", field::genre},
			{"DATE", field::created},
			{"YEAR", field::year},
			{"TRACKNUMBER", field::track},
			{"TRACKTOTAL", field::track_count},
			{"TOTALTRACKS", field::track_count},
			{"DISCNUMBER", field::disc},
			{"DISCTOTAL", field::disc_count},
			{"TOTALDISCS", field::disc_count},
			{"COMMENT", field::comment},
			{"DESCRIPTION", field::comment},
			{"COPYRIGHT", field::copyright},
			{"ORGANIZATION", field::publisher},
			{"LABEL", field::publisher},
			{"PUBLISHER", field::publisher},
			{"ENCODER", field::encoder},
			{"ENCODED-BY", field::encoder},
			{"ORIGINALDATE", field::original_release},
			{"ORIGINALYEAR", field::original_release},
			{"RATING", field::rating},
		};

		constexpr mapping flv_names[] = {
			{"title", field::title},
			{"artist", field::artist},
			{"author", field::artist},
			{"comment", field::comment},
			{"description", field::description},
			{"keywords", field::keywords},
			{"creationdate", field::created},
			{"encoder", field::encoder},
			{"metadatacreator", field::encoder},
		};

		constexpr mapping aiff_names[] = {
			{"NAME", field::title},
			{"AUTH", field::artist},
			{"(c) ", field::copyright},
			{"ANNO", field::comment},
			{"COMT", field::comment},
		};

		// FFmpeg's own names, which its muxers write into Matroska, Vorbis, APE, FLV, ASF and QuickTime
		// tags, its demuxers pass on unchanged and ID3v2 user text frames are named by.
		constexpr mapping generic_names[] = {
			{"title", field::title},
			{"artist", field::artist},
			{"album", field::album},
			{"album_artist", field::album_artist},
			{"composer", field::composer},
			{"performer", field::performer},
			{"genre", field::genre},
			{"publisher", field::publisher},
			{"copyright", field::copyright},
			{"comment", field::comment},
			{"description", field::description},
			{"synopsis", field::synopsis},
			{"show", field::show},
			{"programme", field::show},
			{"encoder", field::encoder},
			{"encoded_by", field::encoder},
			{"make", field::camera_make},
			{"maker", field::camera_make},
			{"model", field::camera_model},
			{"keywords", field::keywords},
			{"rating", field::rating},
			{"date", field::created},
			{"creation_time", field::created},
			{"year", field::year},
			{"originaldate", field::original_release},
			{"originalyear", field::original_release},
			{"original_year", field::original_release},
			{"track", field::track},
			{"disc", field::disc},
			{"disk", field::disc},
			{"episode_sort", field::episode},
			{"season_number", field::season},
			{"location", field::location},
		};

		std::optional<field> find_field(const std::span<const mapping> names, const std::string_view key, const bool any_case) noexcept
		{
			for (const auto& m : names)
			{
				if (any_case ? iequals(m.key, key) : m.key == key) return m.target;
			}

			return {};
		}

		struct target
		{
			field f = field::none;
			// ID3v1 fills only what the file's other tags left empty.
			bool only_lacking = false;
		};

		target target_of(const tag_family family, std::string_view key) noexcept
		{
			const auto with_generic = [key](const std::span<const mapping> names, const bool any_case) -> target
			{
				if (const auto f = find_field(names, key, any_case)) return {*f};
				return {find_field(generic_names, key, true).value_or(field::none)};
			};

			switch (family)
			{
			case tag_family::itunes: return with_generic(itunes_names, false);
			case tag_family::quicktime: return with_generic(quicktime_names, false);
			case tag_family::id3v2:
				for (const std::string_view prefix : {"TXXX:", "TXX:"})
				{
					if (key.starts_with(prefix)) return {find_field(generic_names, key.substr(prefix.size()), true).value_or(field::none)};
				}

				return {find_field(id3v2_names, key, false).value_or(field::none)};
			case tag_family::id3v1:
				if (key.starts_with("ID3v1:")) key.remove_prefix(6);
				return {find_field(id3v1_names, key, false).value_or(field::none), true};
			case tag_family::ape: return with_generic(ape_names, true);
			case tag_family::riff_info: return {find_field(riff_info_names, key, false).value_or(field::none)};
			case tag_family::asf: return with_generic(asf_names, true);
			case tag_family::matroska: return with_generic(matroska_names, true);
			case tag_family::vorbis: return with_generic(vorbis_names, true);
			case tag_family::flv: return with_generic(flv_names, true);
			case tag_family::aiff: return {find_field(aiff_names, key, false).value_or(field::none)};
			}

			return {};
		}

		void set_text(std::string& to, const std::string_view text, const bool only_lacking)
		{
			if (!text.empty() && (!only_lacking || to.empty())) to.assign(text);
		}

		void set_count(int& to, const std::string_view text, const bool only_lacking)
		{
			if (const auto n = parse_count(text); n && *n > 0 && (!only_lacking || to == 0)) to = *n;
		}

		void set_number_and_total(int& number, int& total, const std::string_view text, const bool only_lacking)
		{
			const auto [n, of] = number_and_total(text);
			if (n > 0 && (!only_lacking || number == 0)) number = n;
			if (of > 0 && (!only_lacking || total == 0)) total = of;
		}

		void add_comment(std::vector<media_text>& comments, const std::string_view text, const std::string_view language,
		                 const bool only_lacking)
		{
			if ((only_lacking && !comments.empty()) || comments.size() >= max_kept) return;

			auto code = comment_language(language);
			const auto same = [&](const media_text& c) { return c.text == text && c.language == code; };
			if (std::ranges::any_of(comments, same)) return;

			comments.push_back({std::string(text), std::move(code)});
		}

		void add_keywords(std::vector<std::string>& keywords, const std::string_view text, const std::string_view separators)
		{
			for (std::size_t start = 0; keywords.size() < max_kept;)
			{
				const auto end = text.find_first_of(separators, start);
				const auto word = trim_text(text.substr(start, end == npos ? npos : end - start));
				const auto same = [word](const std::string& k) { return iequals(k, word); };
				if (!word.empty() && std::ranges::none_of(keywords, same)) keywords.emplace_back(word);
				if (end == npos) break;
				start = end + 1;
			}
		}

		// Stars from a number up to 5, else from a percent; nothing for no stars.
		std::optional<int> stars_of(const std::string_view text) noexcept
		{
			const auto value = parse_decimal(text);
			if (!value) return {};

			const auto rounded = std::lround(*value);
			const auto stars = rounded <= 5 ? static_cast<int>(rounded) : stars_from_percent(static_cast<int>(std::min(rounded, 100l)));
			if (stars <= 0) return {};
			return stars;
		}

		void apply_date(media_properties& p, const media_date& date, const bool only_lacking)
		{
			if (!date.valid() || (only_lacking && p.created.valid())) return;
			p.created = date;
			if (!only_lacking || p.year == 0) p.year = date.year;
		}

		// The first of the values a tag joined - a repeated Vorbis field, an ID3v2.4 frame of several - for a property
		// that holds one, as FFmpeg reads only the first of an ID3v2.4 or APE tag's values.
		std::string_view first_value(const std::string_view text) noexcept
		{
			return trim_text(text.substr(0, text.find(';')));
		}

		void apply_field(media_properties& p, const target t, const std::string_view text, const std::string_view language)
		{
			const auto lacking = t.only_lacking;
			const auto one = first_value(text);

			switch (t.f)
			{
			case field::none: break;
			case field::title: set_text(p.title, text, lacking);
				break;
			case field::display_name: set_text(p.title, text, true);
				break;
			case field::artist: set_text(p.artist, text, lacking);
				break;
			case field::album: set_text(p.album, text, lacking);
				break;
			case field::album_artist: set_text(p.album_artist, text, lacking);
				break;
			case field::composer: set_text(p.composer, text, lacking);
				break;
			case field::performer: set_text(p.performer, text, lacking);
				break;
			case field::genre: set_text(p.genre, text, lacking);
				break;
			case field::genre_references: set_text(p.genre, resolve_genres(text), lacking);
				break;
			case field::genre_number:
				if (const auto number = parse_count(one)) set_text(p.genre, genre_name(*number - 1), lacking);
				else set_text(p.genre, text, lacking);
				break;
			case field::publisher: set_text(p.publisher, text, lacking);
				break;
			case field::copyright: set_text(p.copyright, text, lacking);
				break;
			case field::description: set_text(p.description, text, lacking);
				break;
			case field::synopsis: set_text(p.synopsis, text, lacking);
				break;
			case field::show: set_text(p.show, text, lacking);
				break;
			case field::encoder: set_text(p.encoder, text, lacking);
				break;
			case field::camera_make: set_text(p.camera_make, text, lacking);
				break;
			case field::camera_model: set_text(p.camera_model, text, lacking);
				break;
			case field::comment: add_comment(p.comments, text, language, lacking);
				break;
			case field::keywords: add_keywords(p.keywords, text, ",;");
				break;
			case field::categories: add_keywords(p.keywords, text, ";");
				break;
			case field::rating:
				if (const auto stars = stars_of(one)) p.rating = *stars;
				break;
			case field::percent_rating:
				if (const auto percent = parse_count(one); percent && stars_from_percent(*percent) > 0) p.rating = stars_from_percent(*percent);
				break;
			case field::year:
				if (const auto date = parse_media_date(one); date.valid() && (!lacking || p.year == 0)) p.year = date.year;
				break;
			case field::created: apply_date(p, parse_media_date(one), lacking);
				break;
			case field::created_fallback:
				if (!p.created.valid()) apply_date(p, parse_media_date(one), lacking);
				break;
			case field::original_release:
				if (const auto date = parse_media_date(one); date.valid() && (!lacking || !p.original_release.valid())) p.original_release = date;
				break;
			case field::track: set_number_and_total(p.track, p.track_count, one, lacking);
				break;
			case field::track_from_zero:
				if (const auto n = parse_count(one); n && p.track == 0) p.track = *n + 1;
				break;
			case field::track_count: set_count(p.track_count, one, lacking);
				break;
			case field::disc: set_number_and_total(p.disc, p.disc_count, one, lacking);
				break;
			case field::disc_count: set_count(p.disc_count, one, lacking);
				break;
			case field::season: set_count(p.season, one, lacking);
				break;
			case field::episode:
				if (const auto n = number_and_total(one).first; n > 0 && (!lacking || p.episode == 0)) p.episode = n;
				break;
			case field::location:
				{
					auto altitude = std::numeric_limits<double>::quiet_NaN();

					if (const auto position = parse_iso6709(one, &altitude))
					{
						p.location = position;
						p.altitude = std::isnan(altitude) ? std::optional<double>() : altitude;
					}

					break;
				}
			}
		}

		// ---- Dates and places ------------------------------------------------------------------------

		std::size_t digits_at(const std::string_view s, const std::size_t at) noexcept
		{
			auto i = at;
			while (i < s.size() && is_digit(s[i])) ++i;
			return i - at;
		}

		int number_at(const std::string_view s, const std::size_t at, const std::size_t count) noexcept
		{
			auto v = 0;
			for (std::size_t i = 0; i < count; ++i) v = v * 10 + (s[at + i] - '0');
			return v;
		}

		bool valid_day(const int year, const int month, const int day) noexcept
		{
			constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
			if (year < 1 || year > 9999 || month < 0 || month > 12 || day < 0) return false;
			if (month == 0) return day == 0;

			const auto leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
			return day <= (month == 2 && leap ? 29 : days[month - 1]);
		}

		// The time of day from at: hh, hh:mm or hh:mm:ss, or hhmm and hhmmss as compact dates write it, any
		// fraction of a second dropped.
		bool parse_clock(const std::string_view s, std::size_t& at, media_date& d) noexcept
		{
			auto i = at;
			const auto lead = digits_at(s, i);
			auto hour = 0;
			auto minute = 0;
			auto second = 0;

			if (lead == 4 || lead == 6)
			{
				hour = number_at(s, i, 2);
				minute = number_at(s, i + 2, 2);
				if (lead == 6) second = number_at(s, i + 4, 2);
				i += lead;
			}
			else if (lead == 1 || lead == 2)
			{
				hour = number_at(s, i, lead);
				i += lead;

				for (auto* part : {&minute, &second})
				{
					if (i >= s.size() || s[i] != ':') break;

					const auto n = digits_at(s, i + 1);
					if (n < 1 || n > 2) return false;
					*part = number_at(s, i + 1, n);
					i += 1 + n;
				}
			}
			else
			{
				return false;
			}

			if (i < s.size() && s[i] == '.' && digits_at(s, i + 1) > 0) i += 1 + digits_at(s, i + 1);
			if (hour > 23 || minute > 59 || second > 59) return false;

			d.has_time = true;
			d.hour = hour;
			d.minute = minute;
			d.second = second;
			at = i;
			return true;
		}

		// What follows a time of day to the end of the text: nothing, "Z", UTC or GMT, or an offset as
		// +hh, +hhmm or +hh:mm.
		bool parse_zone(const std::string_view s, std::size_t at, media_date& d) noexcept
		{
			while (at < s.size() && s[at] == ' ') ++at;
			const auto zone = s.substr(at);
			if (zone.empty()) return true;

			if (zone == "Z" || zone == "z" || iequals(zone, "UTC") || iequals(zone, "GMT"))
			{
				d.zone = media_date::time_zone::utc;
				return true;
			}

			if (zone[0] != '+' && zone[0] != '-') return false;

			const auto hours = digits_at(zone, 1);
			auto minutes = 0;
			std::size_t end = 0;

			if (hours == 2 && zone.size() > 3 && zone[3] == ':')
			{
				if (digits_at(zone, 4) != 2) return false;
				minutes = number_at(zone, 4, 2);
				end = 6;
			}
			else if (hours == 2 || hours == 4)
			{
				if (hours == 4) minutes = number_at(zone, 3, 2);
				end = 1 + hours;
			}
			else
			{
				return false;
			}

			const auto hh = number_at(zone, 1, 2);
			if (end != zone.size() || hh > 14 || minutes > 59) return false;

			d.zone = media_date::time_zone::offset;
			d.offset_minutes = (zone[0] == '-' ? -1 : 1) * (hh * 60 + minutes);
			return true;
		}

		// ISO 8601 and the forms near it: "2011", "2011-03", "2011-03-13" with "-", ":" or "/" between,
		// then a time after "T" or spaces; and the compact "20110313", "20110313T151349" and
		// "20110313151349".
		media_date parse_numeric_date(const std::string_view s) noexcept
		{
			const auto lead = digits_at(s, 0);
			std::size_t i = 0;
			auto year = 0;
			auto month = 0;
			auto day = 0;

			if (lead == 4)
			{
				year = number_at(s, 0, 4);
				i = 4;

				if (i < s.size() && (s[i] == '-' || s[i] == ':' || s[i] == '/'))
				{
					const auto separator = s[i];
					const auto m = digits_at(s, i + 1);
					if (m < 1 || m > 2) return {};
					month = number_at(s, i + 1, m);
					i += 1 + m;

					if (i < s.size() && s[i] == separator)
					{
						const auto n = digits_at(s, i + 1);
						if (n < 1 || n > 2) return {};
						day = number_at(s, i + 1, n);
						i += 1 + n;
					}
				}
			}
			else if (lead == 8 || lead == 12 || lead == 14)
			{
				year = number_at(s, 0, 4);
				month = number_at(s, 4, 2);
				day = number_at(s, 6, 2);
				i = 8;
			}
			else
			{
				return {};
			}

			// A month or day of zero is one the writer did not know, as in "1999-00-00".
			if (!valid_day(year, month, day)) return {};

			media_date d;
			d.year = year;
			d.month = month;
			d.day = day;

			if (lead > 8)
			{
				if (!parse_clock(s, i, d) || i != lead) return {};
			}
			else if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' '))
			{
				if (day == 0) return {};

				if (s[i] == ' ')
				{
					while (i < s.size() && s[i] == ' ') ++i;
				}
				else
				{
					++i;
				}

				if (!parse_clock(s, i, d)) return {};
			}

			if (i == s.size()) return d;
			if (!d.has_time || !parse_zone(s, i, d)) return {};
			return d;
		}

		constexpr std::array<std::string_view, 7> day_names = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
		constexpr std::array<std::string_view, 12> month_names = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

		// Which of the names a word begins with - "Mar" or "March" for March - or -1.
		template <std::size_t N>
		int name_index(const std::string_view word, const std::array<std::string_view, N>& names) noexcept
		{
			if (word.size() < 3 || !std::ranges::all_of(word, is_alpha)) return -1;

			for (std::size_t i = 0; i < N; ++i)
			{
				if (iequals(word.substr(0, 3), names[i])) return static_cast<int>(i);
			}

			return -1;
		}

		// The C library's asctime, as AVI's IDIT and FLV's creationdate write dates: "Sun Mar 13 15:13:49
		// 2011", the day of the week optional, and UTC or GMT before the year as the date command puts it.
		media_date parse_asctime(const std::string_view s) noexcept
		{
			std::array<std::string_view, 6> words;
			std::size_t count = 0;

			for (std::size_t i = 0; i < s.size();)
			{
				if (s[i] == ' ' || s[i] == '\t')
				{
					++i;
					continue;
				}

				const auto start = i;
				while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
				if (count == words.size()) return {};
				words[count++] = s.substr(start, i - start);
			}

			std::size_t w = 0;
			if (w < count && name_index(words[w], day_names) >= 0) ++w;
			if (count - w < 4) return {};

			const auto month = name_index(words[w++], month_names) + 1;
			const auto day = parse_count(words[w]).value_or(0);
			if (month == 0 || words[w++].size() > 2) return {};

			media_date d;
			std::size_t at = 0;
			if (!parse_clock(words[w], at, d) || at != words[w].size()) return {};
			++w;

			if (count - w == 2)
			{
				if (!iequals(words[w], "UTC") && !iequals(words[w], "GMT")) return {};
				d.zone = media_date::time_zone::utc;
				++w;
			}

			if (count - w != 1 || words[w].size() != 4) return {};
			const auto year = parse_count(words[w]).value_or(0);
			if (day == 0 || !valid_day(year, month, day)) return {};

			d.year = year;
			d.month = month;
			d.day = day;
			return d;
		}

		// One ISO 6709 coordinate from at: a sign, then degrees, degrees and minutes, or degrees, minutes
		// and seconds, told apart by how many whole digits there are, with a fraction of the last.
		bool parse_coordinate(const std::string_view s, std::size_t& at, const std::size_t degree_digits, const double limit, double& out) noexcept
		{
			if (at >= s.size() || (s[at] != '+' && s[at] != '-')) return false;

			const auto negative = s[at] == '-';
			auto i = at + 1;
			const auto whole = digits_at(s, i);
			if (whole == 0 || whole > degree_digits + 4) return false;

			const auto units = number_at(s, i, whole);
			i += whole;

			auto fraction = 0.0;

			if (i < s.size() && s[i] == '.')
			{
				auto scale = 0.1;
				for (++i; i < s.size() && is_digit(s[i]); ++i, scale /= 10) fraction += (s[i] - '0') * scale;
			}

			double value;

			if (whole <= degree_digits)
			{
				value = units + fraction;
			}
			else if (whole == degree_digits + 2)
			{
				const auto minutes = units % 100 + fraction;
				if (minutes >= 60) return false;
				value = units / 100 + minutes / 60;
			}
			else if (whole == degree_digits + 4)
			{
				const auto minutes = units / 100 % 100;
				const auto seconds = units % 100 + fraction;
				if (minutes >= 60 || seconds >= 60) return false;
				value = units / 10000 + minutes / 60.0 + seconds / 3600;
			}
			else
			{
				return false;
			}

			if (value > limit) return false;

			out = negative ? -value : value;
			at = i;
			return true;
		}

		// ---- ID3v2 -----------------------------------------------------------------------------------

		constexpr std::string_view wmp_owner = vocabulary::wmp_rating_owner;

		// Where a string ends in a frame's encoding, and where what follows its terminator begins.
		struct string_extent
		{
			std::size_t length = 0;
			std::size_t next = 0;
			bool terminated = false;
		};

		string_extent id3_string(const std::span<const std::uint8_t> bytes, const std::uint8_t encoding) noexcept
		{
			if (encoding == 1 || encoding == 2)
			{
				for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
				{
					if (bytes[i] == 0 && bytes[i + 1] == 0) return {i, i + 2, true};
				}
			}
			else
			{
				for (std::size_t i = 0; i < bytes.size(); ++i)
				{
					if (bytes[i] == 0) return {i, i + 1, true};
				}
			}

			return {bytes.size(), bytes.size(), false};
		}

		// Text in a frame's encoding: 0 Latin-1, 1 UTF-16 with a byte order mark, 2 UTF-16BE, 3 UTF-8.
		struct id3_decoder
		{
			std::uint8_t encoding = 0;
			// UTF-16 without a mark follows the frame's last mark, else Windows' little-endian.
			bool big_endian = false;

			std::string operator()(const std::span<const std::uint8_t> bytes)
			{
				switch (encoding)
				{
				case 0: return text_from_windows_1252(bytes);
				case 1:
					if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) big_endian = true;
					if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) big_endian = false;
					return text_from_utf16(bytes, big_endian);
				case 2: return text_from_utf16(bytes, true);
				default: return text_from_utf8(bytes);
				}
			}
		};

		// A text frame's strings: ID3v2.4 separates several with terminators, while before it the first
		// terminator ends the text.
		std::string id3_values(std::span<const std::uint8_t> bytes, id3_decoder& decode, const bool several)
		{
			std::string joined;

			while (!bytes.empty())
			{
				const auto extent = id3_string(bytes, decode.encoding);
				append_value(joined, decode(bytes.first(extent.length)));
				if (!several || !extent.terminated) break;
				bytes = bytes.subspan(extent.next);
			}

			return joined;
		}

		// "TXXX:Rip date": a frame's ID with the description or owner that tells it from others of its kind.
		std::string frame_key(const std::string_view id, const std::string_view description)
		{
			const auto name = trim_text(description);
			return name.empty() ? std::string(id) : std::string(id) + ":" + std::string(name);
		}

		// What the frames of one tag are read into, and what the end of the tag needs of them.
		struct id3_pass
		{
			// Starts as the properties the tag is applied over, so that its frames apply as one by one.
			media_scan_result found;
			// What the scan may still inflate of compressed frames.
			media_budget* budget = nullptr;
			bool damaged = false;
			// A frame ran past the end of the tag, which makes its packet unreadable to an update.
			bool overran = false;
			bool xmp_seen = false;
			std::optional<int> wmp_rating;
			std::optional<int> first_rating;
			// ID3v2.3's year, DDMM and HHMM, which together make one date.
			std::string year;
			std::string day_month;
			std::string time;
		};

		void read_text_frame(id3_pass& pass, const std::string_view id, const std::span<const std::uint8_t> data, const int version)
		{
			if (data.empty() || data[0] > 3) return;

			id3_decoder decode{data[0]};
			const auto body = data.subspan(1);

			if (id == "TXXX" || id == "TXX")
			{
				const auto description = id3_string(body, decode.encoding);
				const auto name = decode(body.first(description.length));
				const auto value = description.terminated ? id3_values(body.subspan(description.next), decode, version == 4) : std::string();
				add_tag(pass.found, tag_family::id3v2, frame_key(id, name), value);
				return;
			}

			const auto value = id3_values(body, decode, version == 4);
			if ((id == "TYER" || id == "TYE") && pass.year.empty()) pass.year = trim_text(value);
			if ((id == "TDAT" || id == "TDA") && pass.day_month.empty()) pass.day_month = trim_text(value);
			if ((id == "TIME" || id == "TIM") && pass.time.empty()) pass.time = trim_text(value);
			add_tag(pass.found, tag_family::id3v2, id, value);
		}

		// COMM and USLT: a language, a description, then the text.
		void read_comment_frame(id3_pass& pass, const std::string_view id, const std::span<const std::uint8_t> data)
		{
			if (data.size() < 4 || data[0] > 3) return;

			id3_decoder decode{data[0]};
			const auto language = comment_language(text_from_windows_1252(data.subspan(1, 3)));
			const auto body = data.subspan(4);
			const auto description = id3_string(body, decode.encoding);
			const auto name = decode(body.first(description.length));
			const auto text = description.terminated ? decode(body.subspan(description.next)) : std::string();
			add_tag(pass.found, tag_family::id3v2, frame_key(id, name), text, language);
		}

		void read_url_frame(id3_pass& pass, const std::string_view id, const std::span<const std::uint8_t> data)
		{
			if (id != "WXXX" && id != "WXX")
			{
				add_tag(pass.found, tag_family::id3v2, id, text_from_windows_1252(data));
				return;
			}

			if (data.empty() || data[0] > 3) return;

			id3_decoder decode{data[0]};
			const auto body = data.subspan(1);
			const auto description = id3_string(body, decode.encoding);
			const auto name = decode(body.first(description.length));
			if (description.terminated) add_tag(pass.found, tag_family::id3v2, frame_key(id, name), text_from_windows_1252(body.subspan(description.next)));
		}

		// POPM: who rated, the rating from 1 to 255, and a play count. The rating is chosen at the end.
		void read_popularimeter(id3_pass& pass, const std::string_view id, const std::span<const std::uint8_t> data)
		{
			const auto email = id3_string(data, 0);
			if (!email.terminated || email.next >= data.size()) return;

			const auto owner = text_from_windows_1252(data.first(email.length));
			const int rating = data[email.next];
			if (owner == wmp_owner && !pass.wmp_rating) pass.wmp_rating = rating;
			if (!pass.first_rating) pass.first_rating = rating;
			add_tag(pass.found, tag_family::id3v2, frame_key(id, owner), std::to_string(rating));
		}

		// PRIV: an owner, then data. The first owned by "XMP" is the packet, by the rule id3_packet keeps for
		// read_packet and the writer.
		void read_private_frame(id3_pass& pass, const std::string_view id, const std::span<const std::uint8_t> data)
		{
			const auto end = id3_string(data, 0);
			const auto owner = text_from_windows_1252(data.first(end.length));
			const auto payload = data.subspan(end.next);

			if (!pass.xmp_seen)
			{
				if (auto packet = id3_xmp_payload(data))
				{
					pass.xmp_seen = true;
					pass.found.xmp = std::move(*packet);
				}
			}

			add_description(pass.found, frame_key(id, owner), byte_count(payload.size()));
		}

		// The fields before an attached picture's image.
		struct picture_fields
		{
			std::string mime;
			int type = 3;
			std::string description;
			// Where the image begins.
			std::size_t image = 0;
		};

		// ID3v2.2 names a picture's format in three letters rather than by MIME type.
		std::string_view mime_of_format(const std::string_view format) noexcept
		{
			if (iequals(format, "JPG")) return "image/jpeg";
			if (iequals(format, "PNG")) return "image/png";
			if (iequals(format, "GIF")) return "image/gif";
			if (iequals(format, "BMP")) return "image/bmp";
			if (format == "-->") return "-->";
			return {};
		}

		// APIC: the encoding, a MIME type, the picture type and a description; PIC has a format for the type.
		std::optional<picture_fields> parse_picture_fields(const std::span<const std::uint8_t> b, const int version)
		{
			if (b.empty() || b[0] > 3) return {};

			id3_decoder decode{b[0]};
			picture_fields p;
			std::size_t at = 1;

			if (version == 2)
			{
				if (b.size() < 5) return {};
				p.mime = mime_of_format(text_from_windows_1252(b.subspan(1, 3)));
				at = 4;
			}
			else
			{
				const auto mime = id3_string(b.subspan(at), 0);
				if (!mime.terminated) return {};
				p.mime = trim_text(text_from_windows_1252(b.subspan(at, mime.length)));
				at += mime.next;
			}

			if (at >= b.size()) return {};
			p.type = b[at++];

			const auto description = id3_string(b.subspan(at), decode.encoding);
			if (!description.terminated) return {};

			p.description = trim_text(decode(b.subspan(at, description.length)));
			p.image = at + description.next;
			return p;
		}

		// An attached picture, located where its image lies as stored in the file, else copied.
		void read_picture_frame(id3_pass& pass, reader& source, const bool in_file, const id3_header& tag, const id3_frame& f,
		                        const id3_frame_layout& l)
		{
			std::optional<picture_fields> fields;
			std::uint64_t image_size = 0;
			media_picture picture;

			if (l.plain())
			{
				if (f.size < l.prefix) return;

				const auto data = f.body() + l.prefix;
				const auto data_size = f.size - l.prefix;

				// The fields are short: a little is read, and more only for a description that is not.
				for (const std::uint64_t want : {std::uint64_t{1024}, std::uint64_t{64 * 1024}})
				{
					const auto length = std::min(want, data_size);
					fields = parse_picture_fields(source.read(data, static_cast<std::size_t>(length)), tag.version);
					if (fields || length == data_size) break;
				}

				if (!fields)
				{
					add_description(pass.found, f.name(), byte_count(f.size));
					return;
				}

				image_size = data_size - fields->image;
				if (in_file) picture.location = {data + fields->image, image_size};
				else if (image_size <= max_picture) picture.data = source.read(data + fields->image, static_cast<std::size_t>(image_size));
			}
			else
			{
				const auto data = id3_frame_data(source, f, l, l.compressed ? pass.budget->inflate_limit(max_value) : max_value);
				if (data && l.compressed) pass.budget->inflated(data->size());
				if (data) fields = parse_picture_fields(*data, tag.version);

				if (!fields)
				{
					add_description(pass.found, f.name(), byte_count(f.size));
					return;
				}

				image_size = data->size() - fields->image;
				if (image_size <= max_picture) picture.data.assign(data->begin() + static_cast<std::ptrdiff_t>(fields->image), data->end());
			}

			add_description(pass.found, f.name(), describe_picture(fields->mime, fields->type, image_size));

			// "-->" links to a picture elsewhere rather than holding one.
			if (image_size == 0 || fields->mime == "-->" || (picture.location.empty() && picture.data.empty())) return;

			picture.mime_type = std::move(fields->mime);
			picture.type = fields->type;
			picture.description = std::move(fields->description);
			pass.found.pictures.push_back(std::move(picture));
		}

		void read_frame(id3_pass& pass, reader& source, const bool in_file, const id3_header& tag, const id3_frame& f)
		{
			if (f.size == 0) return;

			const auto l = id3_layout_of(tag, f);
			if (l.encrypted) return;

			const auto id = f.name();
			const auto is = [id](const std::string_view v22, const std::string_view v23) { return id == v22 || id == v23; };

			if (is("PIC", "APIC"))
			{
				read_picture_frame(pass, source, in_file, tag, f, l);
				return;
			}

			const auto known = id[0] == 'T' || id[0] == 'W' || is("COM", "COMM") || is("ULT", "USLT") || is("POP", "POPM") ||
				is_id3_private_frame(id);
			// A compressed frame inflates within what the scan has left; one that would pass it is described.
			const auto limit = l.compressed ? pass.budget->inflate_limit(max_value) : max_value;
			const auto data = known ? id3_frame_data(source, f, l, limit) : std::nullopt;
			if (data && l.compressed) pass.budget->inflated(data->size());

			if (!data)
			{
				add_description(pass.found, id, byte_count(f.size));
				return;
			}

			if (id[0] == 'T') read_text_frame(pass, id, *data, tag.version);
			else if (id[0] == 'W') read_url_frame(pass, id, *data);
			else if (is("POP", "POPM")) read_popularimeter(pass, id, *data);
			else if (is_id3_private_frame(id)) read_private_frame(pass, id, *data);
			else read_comment_frame(pass, id, *data);
		}

		// What the end of a tag settles: the rating among its POPM frames, and the date ID3v2.3 spreads
		// over three frames.
		void finish_id3(id3_pass& pass)
		{
			// Windows Media Player's own rating when it left one, as Diffractor writes it; else the first.
			if (const auto popm = pass.wmp_rating ? pass.wmp_rating : pass.first_rating)
			{
				if (const auto stars = stars_from_popm(*popm); stars > 0) pass.found.properties.rating = stars;
			}

			const auto all_digits = [](const std::string& s, const std::size_t n) { return s.size() == n && std::ranges::all_of(s, is_digit); };
			if (!all_digits(pass.year, 4) || !all_digits(pass.day_month, 4)) return;

			auto text = pass.year + "-" + pass.day_month.substr(2, 2) + "-" + pass.day_month.substr(0, 2);
			if (all_digits(pass.time, 4)) text += "T" + pass.time.substr(0, 2) + ":" + pass.time.substr(2, 2);
			apply_date(pass.found.properties, parse_media_date(text), false);
		}

		// One reading of a tag's frames, keeping what was read before any damage.
		id3_pass read_id3_frames(reader& r, const std::uint64_t offset, const id3_header& tag, const media_properties& properties,
		                         const id3_sizes sizes, media_budget& budget)
		{
			id3_pass pass;
			pass.found.properties = properties;
			pass.budget = &budget;

			try
			{
				const auto end = std::min(offset + tag.end(), r.size());
				std::optional<id3_resynchronised> whole;
				std::optional<reader> resynchronised_reader;
				auto* source = &r;
				auto begin = offset + id3_header_size;
				auto stop = end;

				if (tag.unsynchronised() && tag.version < 4)
				{
					whole.emplace(r, begin, end);
					resynchronised_reader.emplace(*whole);
					source = &*resynchronised_reader;
					begin = 0;
					stop = whole->size();
				}

				if (const auto first = id3_frames_begin(*source, tag, begin, stop))
				{
					id3_frame_walker frames(*source, tag.version, *first, stop, sizes);
					std::size_t count = 0;

					while (const auto f = frames.next())
					{
						read_frame(pass, *source, source == &r, tag, *f);
						if (++count == max_entries) break;
					}

					pass.overran = frames.overran();
					pass.damaged = frames.overran() || frames.stray();
				}
				else
				{
					pass.damaged = true;
				}
			}
			catch (const read_error&)
			{
				pass.damaged = true;
			}

			finish_id3(pass);
			return pass;
		}

		// ---- APE -------------------------------------------------------------------------------------

		constexpr std::uint64_t ape_footer_size = 32;
		constexpr std::uint32_t ape_contains_header = 1u << 31;
		constexpr std::uint32_t ape_is_header = 1u << 29;

		// The picture type a cover art item's name gives in parentheses: "Cover Art (Back)".
		int cover_art_type(const std::string_view key) noexcept
		{
			constexpr std::array<std::string_view, 21> names = {
				"Other", "Icon", "Other Icon", "Front", "Back", "Leaflet", "Media", "Lead Artist", "Artist", "Conductor",
				"Band", "Composer", "Lyricist", "Recording Location", "During Recording", "During Performance",
				"Video Capture", "Fish", "Illustration", "Band Logotype", "Publisher Logotype",
			};

			const auto open = key.find('(');
			const auto close = key.rfind(')');
			if (open == npos || close == npos || close < open) return 3;

			const auto name = trim_text(key.substr(open + 1, close - open - 1));

			for (std::size_t i = 0; i < names.size(); ++i)
			{
				if (iequals(name, names[i])) return static_cast<int>(i);
			}

			return 0;
		}

		// A binary item: cover art is a file's name, a NUL, then the image, which is located.
		void read_ape_binary(reader& r, const std::string& key, const std::uint64_t at, const std::uint64_t size, media_scan_result& out)
		{
			if (!istarts_with(key, "Cover Art"))
			{
				add_description(out, key, byte_count(size));
				return;
			}

			const auto head = r.read(at, static_cast<std::size_t>(std::min<std::uint64_t>(size, 4096)));
			const auto nul = std::ranges::find(head, std::uint8_t{0});

			if (nul == head.end())
			{
				add_description(out, key, byte_count(size));
				return;
			}

			const auto name_length = static_cast<std::uint64_t>(nul - head.begin());
			const auto name = text_from_utf8(std::span(head).first(static_cast<std::size_t>(name_length)));
			const auto image_size = size - name_length - 1;

			media_picture picture;
			picture.location = {at + name_length + 1, image_size};
			picture.mime_type = mime_of_file_name(name);
			picture.type = cover_art_type(key);
			picture.description = trim_text(name);

			add_description(out, key, describe_picture(picture.mime_type, picture.type, image_size));
			if (image_size > 0) out.pictures.push_back(std::move(picture));
		}

		// The items between an APE tag's header, or its start, and its footer.
		void read_ape_items(reader& r, std::uint64_t at, const std::uint64_t end, std::uint32_t count, const bool version2,
		                    media_scan_result& out)
		{
			for (count = std::min(count, static_cast<std::uint32_t>(max_entries)); count > 0 && at <= end && end - at >= 9; --count)
			{
				const auto size = r.le32(at);
				const auto flags = r.le32(at + 4);

				// The key: printable ASCII ended by a NUL, at most 255 characters.
				const auto room = r.view(at + 8, static_cast<std::size_t>(std::min<std::uint64_t>(256, end - at - 8)));
				const auto nul = std::ranges::find(room, std::uint8_t{0});
				if (nul == room.begin() || nul == room.end()) return;

				const std::string key(room.begin(), nul);
				if (!std::ranges::all_of(key, [](const char c) { return c >= 0x20 && c <= 0x7E; })) return;

				const auto value = at + 8 + key.size() + 1;
				if (size > end - value) return;
				at = value + size;

				// APEv2 types an item's value: UTF-8 text, binary, or a locator that is text too. APEv1's are all text.
				const auto type = version2 ? flags >> 1 & 3 : 0;

				if (type == 1)
				{
					read_ape_binary(r, key, value, size, out);
					continue;
				}

				if (type == 3 || size > max_value)
				{
					add_description(out, key, byte_count(size));
					continue;
				}

				// Several values are separated by NULs.
				const auto bytes = r.read(value, size);
				std::string joined;
				auto rest = std::span<const std::uint8_t>(bytes);

				while (!rest.empty())
				{
					const auto part = id3_string(rest, 0);
					const auto slice = rest.first(part.length);
					append_value(joined, version2 ? text_from_utf8(slice) : text_from_unknown(slice));
					rest = rest.subspan(part.next);
				}

				add_tag(out, tag_family::ape, key, joined);
			}
		}

		// ---- Vorbis comments -------------------------------------------------------------------------

		// Base64, as a Vorbis comment holds a picture; nothing for what is not, or decodes past limit.
		std::optional<std::vector<std::uint8_t>> base64_decode(const std::string_view text, const std::size_t limit)
		{
			if (text.size() / 4 > limit / 3 + 1) return {};

			std::vector<std::uint8_t> out;
			out.reserve(text.size() / 4 * 3);
			std::uint32_t bits = 0;
			auto count = 0;
			auto padded = false;

			for (const auto c : text)
			{
				std::uint32_t v;

				if (c >= 'A' && c <= 'Z') v = static_cast<std::uint32_t>(c - 'A');
				else if (c >= 'a' && c <= 'z') v = static_cast<std::uint32_t>(c - 'a' + 26);
				else if (c >= '0' && c <= '9') v = static_cast<std::uint32_t>(c - '0' + 52);
				else if (c == '+' || c == '-') v = 62;
				else if (c == '/' || c == '_') v = 63;
				else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
				else if (c == '=')
				{
					padded = true;
					continue;
				}
				else return {};

				if (padded) return {};

				bits = bits << 6 | v;

				if (++count == 4)
				{
					out.push_back(static_cast<std::uint8_t>(bits >> 16));
					out.push_back(static_cast<std::uint8_t>(bits >> 8));
					out.push_back(static_cast<std::uint8_t>(bits));
					bits = 0;
					count = 0;
				}
			}

			if (count == 1) return {};
			if (count == 2) out.push_back(static_cast<std::uint8_t>(bits >> 4));

			if (count == 3)
			{
				out.push_back(static_cast<std::uint8_t>(bits >> 10));
				out.push_back(static_cast<std::uint8_t>(bits >> 2));
			}

			if (out.size() > limit) return {};
			return out;
		}
	}

	// ---- Tags ------------------------------------------------------------------------------------------

	void apply_tag(media_properties& properties, const tag_family family, const std::string_view key, const std::string_view value,
	               const std::string_view language)
	{
		const auto text = trim_text(value);
		if (!text.empty()) apply_field(properties, target_of(family, key), text, language);
	}

	void add_tag(media_scan_result& out, const tag_family family, const std::string_view key, const std::string_view value,
	             const std::string_view language)
	{
		const auto text = trim_text(value);
		if (text.empty()) return;

		// "und" names no language, which is reported as none, as a stream's is.
		const auto named = iequals(trim_text(language), "und") ? std::string_view{} : language;
		out.tags.push_back({std::string(key), std::string(text), std::string(named)});
		apply_tag(out.properties, family, key, text, named);
	}

	std::uint64_t read_id3v2(reader& r, const std::uint64_t offset, media_budget& budget, media_scan_result& out)
	{
		std::optional<id3_header> tag;

		try
		{
			if (!r.contains(offset, id3_header_size)) return 0;
			tag = parse_id3_header(r.view(offset, id3_header_size));
		}
		catch (const read_error&)
		{
			return 0;
		}

		if (!tag || !tag->known_version() || !tag->size_syncsafe) return 0;
		if (tag->compressed()) return tag->total();

		auto pass = read_id3_frames(r, offset, *tag, out.properties, id3_sizes::standard, budget);

		if (id3_retry_with_plain_sizes(r, offset, *tag, pass.damaged))
		{
			pass = read_id3_frames(r, offset, *tag, out.properties, id3_sizes::plain, budget);
		}

		// The packet is the one id3_packet reads: none from a tag an update could not take whole.
		if (id3_fault(*tag, offset, r.size()) || pass.overran) pass.found.xmp.clear();

		out.properties = std::move(pass.found.properties);
		out.tags.insert(out.tags.end(), std::make_move_iterator(pass.found.tags.begin()), std::make_move_iterator(pass.found.tags.end()));
		out.pictures.insert(out.pictures.end(), std::make_move_iterator(pass.found.pictures.begin()),
		                    std::make_move_iterator(pass.found.pictures.end()));
		if (out.xmp.empty()) out.xmp = std::move(pass.found.xmp);
		return tag->total();
	}

	bool read_id3v1(reader& r, const std::uint64_t offset, media_scan_result& out)
	{
		std::array<std::uint8_t, 128> tag{};

		try
		{
			if (!r.contains(offset, tag.size())) return false;
			r.read(offset, tag.data(), tag.size());
		}
		catch (const read_error&)
		{
			return false;
		}

		if (!starts_with(tag, "TAG")) return false;

		const auto text = [&tag](const std::size_t at, const std::size_t length) { return text_from_unknown(std::span(tag).subspan(at, length)); };

		// ID3v1.1 gives the comment's last two bytes to a zero and the track.
		const auto track = tag[125] == 0 && tag[126] != 0 ? tag[126] : 0;

		add_tag(out, tag_family::id3v1, "ID3v1:title", text(3, 30));
		add_tag(out, tag_family::id3v1, "ID3v1:artist", text(33, 30));
		add_tag(out, tag_family::id3v1, "ID3v1:album", text(63, 30));
		add_tag(out, tag_family::id3v1, "ID3v1:year", text(93, 4));
		add_tag(out, tag_family::id3v1, "ID3v1:comment", text(97, track != 0 ? 28 : 30));
		if (track != 0) add_tag(out, tag_family::id3v1, "ID3v1:track", std::to_string(track));
		add_tag(out, tag_family::id3v1, "ID3v1:genre", genre_name(tag[127]));
		return true;
	}

	std::uint64_t read_ape_tag(reader& r, const std::uint64_t end, media_scan_result& out)
	{
		std::array<std::uint8_t, ape_footer_size> footer{};

		try
		{
			if (end < ape_footer_size || end > r.size()) return 0;
			r.read(end - ape_footer_size, footer.data(), footer.size());
		}
		catch (const read_error&)
		{
			return 0;
		}

		if (!starts_with(footer, "APETAGEX")) return 0;

		const auto version = load_le32(footer.data() + 8);
		const std::uint64_t size = load_le32(footer.data() + 12);
		const auto count = load_le32(footer.data() + 16);
		const auto flags = load_le32(footer.data() + 20);

		// The size counts the items and the footer; APEv2 may put a header of the footer's size before them.
		if ((version != 1000 && version != 2000) || (flags & ape_is_header) || size < ape_footer_size || size > end) return 0;

		const auto version2 = version == 2000;
		const auto total = size + (version2 && (flags & ape_contains_header) ? ape_footer_size : 0);
		if (total > end) return 0;

		try
		{
			read_ape_items(r, end - size, end - ape_footer_size, count, version2, out);
		}
		catch (const read_error&)
		{
		}

		return total;
	}

	bool read_vorbis_comments(const std::span<const std::uint8_t> block, media_scan_result& out)
	{
		if (block.size() < 8) return false;

		const auto vendor_length = load_le32(block.data());
		if (vendor_length > block.size() - 8) return false;

		const auto vendor = text_from_utf8(block.subspan(4, vendor_length));
		std::size_t at = 4 + std::size_t{vendor_length};
		auto count = std::min(load_le32(block.data() + at), static_cast<std::uint32_t>(max_entries));
		at += 4;

		add_tag(out, tag_family::vorbis, "vendor", vendor);

		// The fields in the order their names first appear. A name that repeats, in any case, is one tag of
		// every value joined, as FFmpeg joins them; a picture is always its own, listed where it lies.
		struct field_entry
		{
			std::string key;
			std::string value;
			bool picture = false;
		};

		std::vector<field_entry> entries;
		std::map<std::string, std::size_t> by_name;
		std::vector<std::string> cover_mimes;
		std::vector<std::size_t> covers;

		for (; count > 0 && block.size() - at >= 4; --count)
		{
			const auto length = load_le32(block.data() + at);
			at += 4;
			if (length > block.size() - at) break;

			const auto comment = block.subspan(at, length);
			at += length;

			// NAME=value, the name ASCII and the value UTF-8.
			const auto equals = std::ranges::find(comment, std::uint8_t{'='});
			if (equals == comment.begin() || equals == comment.end()) continue;

			auto key = text_from_windows_1252(comment.first(static_cast<std::size_t>(equals - comment.begin())));
			const auto value = comment.subspan(static_cast<std::size_t>(equals - comment.begin()) + 1);

			if (iequals(key, "METADATA_BLOCK_PICTURE"))
			{
				// A FLAC picture block in base64: the form Xiph recommends.
				auto described = byte_count(value.size());
				media_picture picture;

				if (const auto decoded = base64_decode(as_text(value), max_value); decoded && read_flac_picture(*decoded, 0, picture))
				{
					described = describe_picture(picture.mime_type, picture.type, picture.data.size());
					out.pictures.push_back(std::move(picture));
				}

				entries.push_back({std::move(key), std::move(described), true});
				continue;
			}

			if (iequals(key, "COVERART"))
			{
				// The older form: the image alone in base64, its type in a COVERARTMIME field.
				auto described = byte_count(value.size());

				if (auto decoded = base64_decode(as_text(value), max_picture); decoded && !decoded->empty())
				{
					described = describe_picture({}, 3, decoded->size());
					media_picture picture;
					picture.data = std::move(*decoded);
					covers.push_back(out.pictures.size());
					out.pictures.push_back(std::move(picture));
				}

				entries.push_back({std::move(key), std::move(described), true});
				continue;
			}

			const auto text = text_from_utf8(value);
			if (iequals(key, "COVERARTMIME")) cover_mimes.emplace_back(trim_text(text));

			std::string name(key);
			std::ranges::transform(name, name.begin(), ascii_lower);
			const auto [named, first] = by_name.try_emplace(std::move(name), entries.size());
			if (first) entries.push_back({std::move(key), {}, false});
			append_value(entries[named->second].value, text);
		}

		auto encoder_field = false;

		for (const auto& e : entries)
		{
			if (e.picture)
			{
				add_description(out, e.key, e.value);
				continue;
			}

			if (!e.value.empty() && target_of(tag_family::vorbis, e.key).f == field::encoder) encoder_field = true;
			add_tag(out, tag_family::vorbis, e.key, e.value);
		}

		// Each COVERART takes the COVERARTMIME given in its turn, or the last one given.
		for (std::size_t i = 0; i < covers.size() && !cover_mimes.empty(); ++i)
		{
			auto& mime = out.pictures[covers[i]].mime_type;
			if (mime.empty()) mime = cover_mimes[std::min(i, cover_mimes.size() - 1)];
		}

		// The library that wrote the stream, when no field names the encoder.
		if (!encoder_field && out.properties.encoder.empty()) out.properties.encoder = trim_text(vendor);
		return true;
	}

	bool read_flac_picture(const std::span<const std::uint8_t> block, const std::uint64_t block_offset, media_picture& out)
	{
		std::size_t at = 0;

		const auto next = [&](std::uint32_t& v)
		{
			if (block.size() - at < 4) return false;
			v = load_be32(block.data() + at);
			at += 4;
			return true;
		};

		std::uint32_t type = 0;
		std::uint32_t mime_length = 0;
		std::uint32_t description_length = 0;
		std::uint32_t size = 0;
		std::uint32_t ignored = 0;

		if (!next(type) || !next(mime_length) || mime_length > block.size() - at) return false;
		const auto mime = text_from_windows_1252(block.subspan(at, mime_length));
		at += mime_length;

		if (!next(description_length) || description_length > block.size() - at) return false;
		const auto description = text_from_utf8(block.subspan(at, description_length));
		at += description_length;

		// The width, height, depth and colour count, then the image's size.
		for (auto i = 0; i < 4; ++i)
		{
			if (!next(ignored)) return false;
		}

		if (!next(size) || size == 0 || mime == "-->") return false;

		media_picture picture;
		picture.type = static_cast<int>(std::min<std::uint32_t>(type, 0x7FFFFFFF));
		picture.mime_type = trim_text(mime);
		picture.description = trim_text(description);

		if (block_offset != 0)
		{
			// Located, so the block need only hold the fields: a scan need not read the image to find it.
			picture.location = {block_offset + at, size};
		}
		else
		{
			if (size > block.size() - at || size > max_picture) return false;
			picture.data.assign(block.begin() + static_cast<std::ptrdiff_t>(at), block.begin() + static_cast<std::ptrdiff_t>(at + size));
		}

		out = std::move(picture);
		return true;
	}

	// ---- Dates and places ------------------------------------------------------------------------------

	media_date parse_media_date(const std::string_view text) noexcept
	{
		const auto s = trim_text(text);
		if (s.empty()) return {};
		return is_alpha(s[0]) ? parse_asctime(s) : parse_numeric_date(s);
	}

	std::optional<gps_position> parse_iso6709(const std::string_view text, double* const altitude) noexcept
	{
		const auto s = trim_text(text);
		std::size_t at = 0;
		gps_position position;
		if (!parse_coordinate(s, at, 2, 90, position.latitude) || !parse_coordinate(s, at, 3, 180, position.longitude)) return {};

		std::optional<double> height;

		if (at < s.size() && (s[at] == '+' || s[at] == '-'))
		{
			const auto negative = s[at] == '-';
			auto end = at + 1;
			while (end < s.size() && (is_digit(s[end]) || s[end] == '.')) ++end;

			const auto metres = parse_decimal(s.substr(at + 1, end - at - 1));
			if (!metres) return {};
			height = negative ? -*metres : *metres;
			at = end;
		}

		// A coordinate reference system may follow, up to the terminating slash.
		if (s.substr(at).starts_with("CRS"))
		{
			const auto slash = s.find('/', at);
			at = slash == npos ? s.size() : slash;
		}

		// After the terminating slash, the place a 3GPP 'loci' box names, as FFmpeg writes it.
		if (at < s.size() && s[at] == '/') at = s.size();
		if (at != s.size() || (position.latitude == 0 && position.longitude == 0)) return {};

		if (altitude && height) *altitude = *height;
		return position;
	}
}
