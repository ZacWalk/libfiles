// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The names each tag family files a property under, where libfiles both reads and writes it -
// ID3v2 frames, RIFF INFO items, ASF fields and descriptors, and the Xtra box Windows keeps in an MP4 -
// named once, so that the reader's table that maps a name onto a property and the writer that writes an
// edit's field under it cannot drift apart. Names libfiles only reads stay in media_tags.cpp's tables.

#pragma once

#include <cstddef>
#include <string_view>

namespace libfiles::detail::vocabulary
{
	// An ID3v2 frame: ID3v2.2's three letters, and the four of ID3v2.3 and 2.4.
	struct id3_frame_name
	{
		std::string_view v22;
		std::string_view v23;

		constexpr std::string_view in(const int version) const noexcept { return version == 2 ? v22 : v23; }
	};

	inline constexpr id3_frame_name id3_title{"TT2", "TIT2"};
	inline constexpr id3_frame_name id3_artist{"TP1", "TPE1"};
	inline constexpr id3_frame_name id3_album{"TAL", "TALB"};
	inline constexpr id3_frame_name id3_album_artist{"TP2", "TPE2"};
	inline constexpr id3_frame_name id3_genre{"TCO", "TCON"};
	inline constexpr id3_frame_name id3_track{"TRK", "TRCK"};
	inline constexpr id3_frame_name id3_disc{"TPA", "TPOS"};
	inline constexpr id3_frame_name id3_copyright{"TCR", "TCOP"};
	inline constexpr id3_frame_name id3_comment{"COM", "COMM"};
	inline constexpr id3_frame_name id3_copyright_url{"WCP", "WCOP"};
	inline constexpr id3_frame_name id3_rating{"POP", "POPM"};
	// The year, day and month, and time ID3v2.3 keeps a date in; ID3v2.4's recording time replaces them.
	inline constexpr id3_frame_name id3_year{"TYE", "TYER"};
	inline constexpr id3_frame_name id3_day_month{"TDA", "TDAT"};
	inline constexpr id3_frame_name id3_time{"TIM", "TIME"};
	inline constexpr std::string_view id3_recording_time = "TDRC";
	// The owner of the POPM frame Windows Media Player reads and writes a rating in.
	inline constexpr std::string_view wmp_rating_owner = "Windows Media Player 9 Series";

	// RIFF INFO list items, as the four characters of their ids.
	inline constexpr char info_title[5] = "INAM";
	inline constexpr char info_artist[5] = "IART";
	inline constexpr char info_comment[5] = "ICMT";
	inline constexpr char info_copyright[5] = "ICOP";
	inline constexpr char info_genre[5] = "IGNR";
	inline constexpr char info_created[5] = "ICRD";

	// ASF's Content Description, whose five fields have no names of their own: these are the names the scan
	// gives them, in the order the object keeps them.
	inline constexpr std::string_view asf_content_description[5] = {"Title", "Author", "Copyright", "Description", "Rating"};
	inline constexpr std::size_t asf_title = 0;
	inline constexpr std::size_t asf_author = 1;
	inline constexpr std::size_t asf_copyright = 2;
	inline constexpr std::size_t asf_description = 3;

	// The ASF descriptors Windows reads keywords and a rating from - and finds in an MP4's Xtra box too.
	inline constexpr std::string_view wm_category = "WM/Category";
	inline constexpr std::string_view wm_shared_user_rating = "WM/SharedUserRating";
}
