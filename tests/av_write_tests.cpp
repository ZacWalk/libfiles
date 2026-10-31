// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for writing RIFF (WAV, AVI), MPEG audio (ID3v2) and FLV files. Each checks the bytes a
// reader of the format looks at - FFmpeg's places in particular - and that the media never moves where
// the format's offsets would break.

#include "test.h"

#include "builders.h"
#include "interruptions.h"
#include "legacy.h"
#include "libfiles/write.h"
#include "round_trip.h"

#include <bit>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace test_build;

namespace
{
	const bytes media = bytes(64, 0xAB);

	libfiles::xmp::meta rated(const int rating)
	{
		libfiles::xmp::meta meta;
		meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", rating);
		return meta;
	}

	int rating_of(const bytes& file)
	{
		libfiles::reader r(file);
		const auto packet = libfiles::read_packet(r);
		if (!packet.readable) return -101;
		if (packet.packet.empty()) return -100;

		libfiles::xmp::meta meta;
		meta.parse(packet.packet);
		std::int32_t rating = -99;
		meta.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating);
		return rating;
	}

	struct written
	{
		bytes file;
		libfiles::write_plan plan;
	};

	// Applied as to a live file when the plan allows it, otherwise rewritten in one pass.
	written update(const bytes& file, const libfiles::metadata_update& u)
	{
		libfiles::reader r(file);
		auto plan = libfiles::plan_update(r, u);

		if (plan.patchable())
		{
			libfiles::memory_stream live(file);
			libfiles::write_in_place(live, plan);
			return {live.bytes(), std::move(plan)};
		}

		libfiles::memory_stream out;
		if (plan.ok()) libfiles::write_updated(r, plan, out);
		return {out.bytes(), std::move(plan)};
	}

	std::size_t find(const bytes& file, const std::string_view text, const std::size_t from = 0)
	{
		return std::string(file.begin(), file.end()).find(text, from);
	}

	bool holds_media_at(const bytes& file, const std::size_t at)
	{
		return file.size() >= at + media.size() && std::memcmp(file.data() + at, media.data(), media.size()) == 0;
	}

	// RIFF

	bytes riff_chunk(const std::string_view id, const bytes& payload)
	{
		builder b;
		b.raw(id).le32(static_cast<std::uint32_t>(payload.size())).raw(payload);
		if (payload.size() & 1) b.u8(0);
		return b.b;
	}

	bytes riff_list(const std::string_view type, const std::vector<bytes>& children)
	{
		builder b;
		b.raw(type);
		for (const auto& c : children) b.raw(c);
		return riff_chunk("LIST", b.b);
	}

	bytes riff_form(const std::string_view type, const std::vector<bytes>& children)
	{
		builder b;
		b.raw(type);
		for (const auto& c : children) b.raw(c);
		return riff_chunk("RIFF", b.b);
	}

	bytes text(const std::string_view s)
	{
		auto out = to_bytes(s);
		out.push_back(0);
		return out;
	}

	bytes make_wav(const std::vector<bytes>& extra = {}, const bool info = false)
	{
		std::vector<bytes> children = {riff_chunk("fmt ", bytes(16, 1))};
		if (info) children.push_back(riff_list("INFO", {riff_chunk("ISFT", text("Lavf"))}));
		children.push_back(riff_chunk("data", media));
		for (const auto& e : extra) children.push_back(e);
		return riff_form("WAVE", children);
	}

	struct riff_view
	{
		// Every form reads within the size it declares.
		bool whole = false;
		// And the file ends where they do.
		bool exact = false;
		std::vector<std::pair<std::string, std::size_t>> chunks; // id or list type, start, at the top of every form
		std::map<std::string, std::string> info;                 // the INFO a reader takes
		std::size_t movi = 0;

		std::string field(const std::string& key) const
		{
			const auto it = info.find(key);
			return it != info.end() ? it->second : std::string();
		}
	};

	// The file as a reader walks it, each form within the size it declares.
	riff_view walk_riff(const bytes& file, const bool info_before_movi_only)
	{
		riff_view v;
		std::size_t pos = 0;

		while (file.size() - pos >= 12 && std::memcmp(file.data() + pos, "RIFF", 4) == 0)
		{
			const auto form_end = pos + 8 + libfiles::detail::load_le32(file.data() + pos + 4);
			if (form_end > file.size()) return v;

			auto at = pos + 12;

			while (form_end - at >= 8)
			{
				const std::string id(reinterpret_cast<const char*>(file.data() + at), 4);
				const auto size = libfiles::detail::load_le32(file.data() + at + 4);
				if (size > form_end - at - 8) return v;

				const auto type = size >= 4 ? std::string(reinterpret_cast<const char*>(file.data() + at + 8), 4) : std::string();
				if (id == "LIST" && type == "movi") v.movi = at;

				if (id == "LIST" && type == "INFO" && (!info_before_movi_only || v.movi == 0))
				{
					for (auto i = at + 12; i + 8 <= at + 8 + size;)
					{
						const std::string key(reinterpret_cast<const char*>(file.data() + i), 4);
						const auto n = libfiles::detail::load_le32(file.data() + i + 4);
						std::string value(reinterpret_cast<const char*>(file.data() + i + 8), n);
						while (!value.empty() && value.back() == '\0') value.pop_back();
						v.info[key] = value;
						i += 8 + n + (n & 1);
					}
				}

				v.chunks.emplace_back(id == "LIST" ? type : id, at);
				at += 8 + size + (size & 1);
			}

			pos = form_end + (form_end & 1);
		}

		v.whole = true;
		v.exact = pos >= file.size() - std::min<std::size_t>(file.size(), 1);
		return v;
	}

	std::size_t count(const riff_view& v, const std::string_view id)
	{
		return static_cast<std::size_t>(std::ranges::count_if(v.chunks, [id](const auto& c) { return c.first == id; }));
	}

	bytes make_avi(const bool info_with_junk, const std::vector<bytes>& before_movi = {}, const std::vector<bytes>& hdrl_extra = {})
	{
		std::vector<bytes> hdrl = {riff_chunk("avih", bytes(56, 2))};
		for (const auto& h : hdrl_extra) hdrl.push_back(h);

		std::vector<bytes> children = {riff_list("hdrl", hdrl)};
		if (info_with_junk)
		{
			children.push_back(riff_list("INFO", {riff_chunk("ISFT", text("Lavf"))}));
			children.push_back(riff_chunk("JUNK", bytes(400, 0xEE)));
		}

		for (const auto& b : before_movi) children.push_back(b);
		children.push_back(riff_list("movi", {riff_chunk("00dc", media)}));
		children.push_back(riff_chunk("idx1", bytes(16, 3)));
		return riff_form("AVI ", children);
	}

	// ID3

	bytes syncsafe(const std::uint32_t v)
	{
		return {static_cast<std::uint8_t>(v >> 21 & 0x7F), static_cast<std::uint8_t>(v >> 14 & 0x7F),
		        static_cast<std::uint8_t>(v >> 7 & 0x7F), static_cast<std::uint8_t>(v & 0x7F)};
	}

	bytes id3_frame(const int version, const std::string_view id, const bytes& body)
	{
		builder b;
		b.raw(id);
		if (version == 4) b.raw(syncsafe(static_cast<std::uint32_t>(body.size())));
		else b.be32(static_cast<std::uint32_t>(body.size()));
		b.u8(0).u8(0).raw(body);
		return b.b;
	}

	const bytes mpeg_audio = []
	{
		builder b;
		for (auto i = 0; i < 4; ++i) b.u8(0xFF).u8(0xFB).u8(0x90).u8(0x64).raw(media);
		return b.b;
	}();

	bytes make_mp3(const int version, const std::vector<bytes>& frames, const std::size_t padding, const std::uint8_t flags = 0)
	{
		bytes body;
		for (const auto& f : frames) body.insert(body.end(), f.begin(), f.end());
		body.resize(body.size() + padding, 0);

		builder b;
		b.raw("ID3").u8(static_cast<unsigned>(version)).u8(0).u8(flags).raw(syncsafe(static_cast<std::uint32_t>(body.size()))).raw(body);
		b.raw(mpeg_audio);
		return b.b;
	}

	struct id3_frame_view
	{
		std::string id;
		bytes body;
	};

	std::vector<id3_frame_view> id3_frames(const bytes& file, int& version, std::size_t& total)
	{
		std::vector<id3_frame_view> out;
		version = file[3];
		total = 10 + ((file[6] << 21) | (file[7] << 14) | (file[8] << 7) | file[9]);

		for (std::size_t pos = 10; pos + 10 <= total && file[pos] != 0;)
		{
			const auto size = version == 4 ? static_cast<std::size_t>((file[pos + 4] << 21) | (file[pos + 5] << 14) | (file[pos + 6] << 7) | file[pos + 7])
			                               : static_cast<std::size_t>(libfiles::detail::load_be32(file.data() + pos + 4));
			out.push_back({std::string(reinterpret_cast<const char*>(file.data() + pos), 4),
			               bytes(file.begin() + static_cast<std::ptrdiff_t>(pos + 10), file.begin() + static_cast<std::ptrdiff_t>(pos + 10 + size))});
			pos += 10 + size;
		}

		return out;
	}

	const id3_frame_view* frame_of(const std::vector<id3_frame_view>& frames, const std::string_view id)
	{
		const auto it = std::ranges::find(frames, id, &id3_frame_view::id);
		return it != frames.end() ? &*it : nullptr;
	}

	// FLV

	bytes amf_name(const std::string_view s)
	{
		return builder().be16(static_cast<unsigned>(s.size())).raw(s).b;
	}

	bytes amf_number(const double v)
	{
		builder b;
		b.u8(0);
		const auto bits = std::bit_cast<std::uint64_t>(v);
		b.be32(static_cast<std::uint32_t>(bits >> 32)).be32(static_cast<std::uint32_t>(bits));
		return b.b;
	}

	double amf_number_at(const bytes& file, const std::size_t at)
	{
		const std::uint64_t bits = static_cast<std::uint64_t>(libfiles::detail::load_be32(file.data() + at)) << 32 | libfiles::detail::load_be32(file.data() + at + 4);
		return std::bit_cast<double>(bits);
	}

	bytes flv_tag(const std::uint8_t type, const std::uint32_t timestamp, const bytes& data)
	{
		builder b;
		b.u8(type).u8(data.size() >> 16 & 0xFF).u8(data.size() >> 8 & 0xFF).u8(data.size() & 0xFF);
		b.u8(timestamp >> 16 & 0xFF).u8(timestamp >> 8 & 0xFF).u8(timestamp & 0xFF).u8(0).u8(0).u8(0).u8(0);
		b.raw(data).be32(static_cast<std::uint32_t>(11 + data.size()));
		return b.b;
	}

	// An FLV whose onMetaData holds the positions of its two video tags, and its size.
	bytes make_flv(const bool with_meta)
	{
		builder header;
		header.raw("FLV").u8(1).u8(1).be32(9).be32(0);

		const auto video = [](const std::uint32_t ts) { return flv_tag(9, ts, media); };

		if (!with_meta)
		{
			builder b = header;
			b.raw(video(0)).raw(video(40));
			return b.b;
		}

		// Sized first, then filled, since the positions it holds depend on its own size.
		const auto meta_with = [](const double p1, const double p2, const double size)
		{
			builder m;
			m.u8(2).raw(amf_name("onMetaData")).u8(8).be32(3);
			m.raw(amf_name("duration")).raw(amf_number(0.08));
			m.raw(amf_name("filesize")).raw(amf_number(size));
			m.raw(amf_name("keyframes")).u8(3);
			m.raw(amf_name("times")).u8(10).be32(2).raw(amf_number(0)).raw(amf_number(0.04));
			m.raw(amf_name("filepositions")).u8(10).be32(2).raw(amf_number(p1)).raw(amf_number(p2));
			m.be16(0).u8(9);
			m.be16(0).u8(9);
			return flv_tag(18, 0, m.b);
		};

		const auto meta_size = meta_with(0, 0, 0).size();
		const auto first = header.b.size() + meta_size;
		const auto second = first + video(0).size();
		const auto total = second + video(40).size();

		builder b = header;
		b.raw(meta_with(static_cast<double>(first), static_cast<double>(second), static_cast<double>(total))).raw(video(0)).raw(video(40));
		return b.b;
	}

	struct flv_view
	{
		bool whole = false;
		std::vector<std::pair<std::uint8_t, std::size_t>> tags; // type, start
	};

	// The tags as a player walks them, each size field checked against the tag before it.
	flv_view walk_flv(const bytes& file)
	{
		flv_view v;
		std::size_t pos = libfiles::detail::load_be32(file.data() + 5);
		if (libfiles::detail::load_be32(file.data() + pos) != 0) return v;
		pos += 4;

		while (pos + 15 <= file.size())
		{
			const auto size = static_cast<std::size_t>(file[pos + 1] << 16 | file[pos + 2] << 8 | file[pos + 3]);
			if (pos + 11 + size + 4 > file.size()) return v;
			if (libfiles::detail::load_be32(file.data() + pos + 11 + size) != 11 + size) return v;
			v.tags.emplace_back(file[pos], pos);
			pos += 11 + size + 4;
		}

		v.whole = pos == file.size();
		return v;
	}
}

// RIFF

TEST("riff and mpeg audio and flv are told apart")
{
	CHECK(libfiles::detect_file_format(make_wav()) == libfiles::file_format::riff);
	CHECK(libfiles::detect_file_format(make_avi(false)) == libfiles::file_format::riff);
	CHECK(libfiles::detect_file_format(make_mp3(3, {}, 10)) == libfiles::file_format::mp3);
	CHECK(libfiles::detect_file_format(mpeg_audio) == libfiles::file_format::mp3);
	CHECK(libfiles::detect_file_format(make_flv(true)) == libfiles::file_format::flv);
	CHECK(libfiles::detect_file_format(riff_form("RMID", {riff_chunk("data", media)})) == libfiles::file_format::unknown);
}

TEST("a wav gains its packet and info at the end of its form")
{
	const auto original = make_wav();
	const auto meta = rated(3);
	libfiles::metadata_update u{&meta};
	u.legacy.title = "Song";
	u.legacy.artist = "Zoë";

	const auto result = update(original, u);
	CHECK(result.plan.patchable());
	CHECK(!result.plan.in_place());

	const auto v = walk_riff(result.file, false);
	CHECK(v.whole && v.exact);
	CHECK(holds_media_at(result.file, find(original, "data") + 8));
	CHECK_EQ(count(v, "_PMX"), 1u);
	CHECK(v.field("INAM") == "Song");
	CHECK(v.field("IART") == "Zoë");
	CHECK_EQ(rating_of(result.file), 3);

	// The second packet fits the first.
	const auto again = rated(5);
	const auto twice = update(result.file, {&again});
	CHECK(twice.plan.in_place());
	CHECK_EQ(rating_of(twice.file), 5);
}

TEST("a wav info list keeps the fields an edit does not set")
{
	const auto original = make_wav({}, true);
	libfiles::metadata_update u;
	u.legacy.title = "Song";
	u.legacy.year = 1999;

	const auto result = update(original, u);
	const auto v = walk_riff(result.file, false);
	CHECK(v.whole);
	CHECK(v.field("ISFT") == "Lavf");
	CHECK(v.field("INAM") == "Song");
	CHECK(v.field("ICRD") == "1999");
	CHECK_EQ(count(v, "INFO"), 1u);

	libfiles::metadata_update clear;
	clear.legacy.title = "";
	const auto cleared = walk_riff(update(result.file, clear).file, false);
	CHECK(cleared.whole);
	CHECK(!cleared.info.contains("INAM"));
	CHECK(cleared.field("ISFT") == "Lavf");
}

TEST("a wav's display title follows the edit")
{
	builder disp;
	disp.le32(1).raw(text("Old"));
	const auto original = make_wav({riff_chunk("DISP", disp.b)});

	libfiles::metadata_update u;
	u.legacy.title = "A longer title";
	const auto result = update(original, u);

	CHECK(walk_riff(result.file, false).whole);
	CHECK(find(result.file, "A longer title") != std::string::npos);
	const auto disp_at = find(result.file, "DISP", find(original, "data"));
	CHECK(disp_at != std::string::npos);
	CHECK(find(result.file, "A longer title", disp_at) == disp_at + 12);
}

TEST("an avi keeps its info before movi and its packet after")
{
	const auto original = make_avi(true);
	const auto movi = find(original, "movi") - 8;
	const auto meta = rated(4);
	libfiles::metadata_update u{&meta};
	u.legacy.title = "A title long enough to grow the list";
	u.legacy.genre = "Documentary";

	const auto result = update(original, u);
	CHECK(result.plan.patchable());

	const auto v = walk_riff(result.file, true);
	CHECK(v.whole && v.exact);
	CHECK_EQ(v.movi, movi);
	CHECK(holds_media_at(result.file, find(original, "00dc") + 8));
	CHECK(v.field("INAM") == "A title long enough to grow the list");
	CHECK(v.field("IGNR") == "Documentary");
	CHECK(v.field("ISFT") == "Lavf");

	// Readers take the first _PMX after movi.
	const auto pmx = std::ranges::find(v.chunks, std::string("_PMX"), &std::pair<std::string, std::size_t>::first);
	CHECK(pmx != v.chunks.end() && pmx->second > movi);
	CHECK_EQ(rating_of(result.file), 4);
}

TEST("an avi packet before movi moves after it")
{
	const auto stale = rated(1).serialize({});
	const auto original = make_avi(false, {riff_chunk("_PMX", bytes(stale.begin(), stale.end()))});
	CHECK_EQ(rating_of(original), 1);

	const auto meta = rated(2);
	const auto result = update(original, {&meta});
	const auto v = walk_riff(result.file, true);

	CHECK(v.whole);
	CHECK_EQ(count(v, "_PMX"), 1u);
	const auto pmx = std::ranges::find(v.chunks, std::string("_PMX"), &std::pair<std::string, std::size_t>::first);
	CHECK(pmx != v.chunks.end() && pmx->second > v.movi);
	CHECK_EQ(rating_of(result.file), 2);
}

TEST("an avi that continues in further forms takes only what fits")
{
	auto file = make_avi(false);
	const auto avix = riff_form("AVIX", {riff_list("movi", {riff_chunk("00dc", media)})});
	file.insert(file.end(), avix.begin(), avix.end());

	const auto meta = rated(2);
	libfiles::reader r(file);
	CHECK(!libfiles::plan_update(r, {&meta}).ok());

	// A packet already after movi in the first form is patched.
	const auto packet = rated(1).serialize({});
	auto with_packet = make_avi(false);
	const auto chunk = riff_chunk("_PMX", bytes(packet.begin(), packet.end()));
	with_packet.insert(with_packet.end(), chunk.begin(), chunk.end());
	const auto form_size = libfiles::detail::load_le32(with_packet.data() + 4) + static_cast<std::uint32_t>(chunk.size());
	for (auto i = 0; i < 4; ++i) with_packet[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(form_size >> (8 * i));
	with_packet.insert(with_packet.end(), avix.begin(), avix.end());

	const auto result = update(with_packet, {&meta});
	CHECK(result.plan.in_place());
	CHECK_EQ(rating_of(result.file), 2);
}

TEST("an avi's IDIT takes the original date")
{
	const auto original = make_avi(false, {}, {riff_chunk("IDIT", text("Mon Jan 01 00:00:00 2001\n"))});
	libfiles::metadata_update u;
	u.legacy.date_original = "2008-12-06T13:58:13";

	const auto result = update(original, u);
	CHECK(result.plan.in_place());
	CHECK(find(result.file, "Sat Dec 06 13:58:13 2008\n") != std::string::npos);
}

TEST("an interrupted riff update leaves a file that reads whole")
{
	const auto original = make_avi(true);
	const auto movi = find(original, "movi") - 8;
	const auto meta = rated(4);
	libfiles::metadata_update u{&meta};
	u.legacy.title = "A title long enough to grow the list";

	libfiles::reader r(original);
	const auto plan = libfiles::plan_update(r, u);
	CHECK(plan.patchable());

	recording_stream live(original);
	libfiles::write_in_place(live, plan);
	const auto states = interrupted_states(original, live.writes);
	CHECK(states.size() > 4);

	for (const auto& state : states)
	{
		const auto v = walk_riff(state, true);
		CHECK(v.whole);
		CHECK_EQ(v.movi, movi);
		const auto rating = rating_of(state);
		CHECK(rating == -100 || rating == 4);
		const auto title = v.field("INAM");
		CHECK(title.empty() || title == "A title long enough to grow the list");
	}

	CHECK_EQ(rating_of(states.back()), 4);
}

// MPEG audio

TEST("an mp3 without a tag gains one, with room for the next edit")
{
	const auto meta = rated(3);
	libfiles::metadata_update u{&meta};
	u.legacy.title = "Song";

	const auto result = update(mpeg_audio, u);
	CHECK(result.plan.ok());
	CHECK(!result.plan.patchable());

	int version = 0;
	std::size_t total = 0;
	const auto frames = id3_frames(result.file, version, total);
	CHECK_EQ(version, 3);
	CHECK(total >= 10 + 2048);
	CHECK(std::equal(mpeg_audio.begin(), mpeg_audio.end(), result.file.begin() + static_cast<std::ptrdiff_t>(total)));
	CHECK(frame_of(frames, "TIT2") && frame_of(frames, "TIT2")->body == bytes({0, 'S', 'o', 'n', 'g'}));
	CHECK_EQ(rating_of(result.file), 3);

	const auto again = rated(5);
	libfiles::metadata_update v{&again};
	v.legacy.title = "Another song";
	const auto twice = update(result.file, v);
	CHECK(twice.plan.in_place());
	CHECK_EQ(twice.file.size(), result.file.size());
	CHECK_EQ(rating_of(twice.file), 5);
}

TEST("an mp3 tag with room is patched, keeping the frames an edit does not own")
{
	const auto wm = id3_frame(4, "PRIV", builder().raw(text("WM/Provider")).raw("AMG").b);
	const auto other_popm = id3_frame(4, "POPM", builder().raw(text("someone@example.com")).u8(10).be32(0).b);
	const auto itunes = id3_frame(4, "COMM", builder().u8(0).raw("eng").raw(text("iTunNORM")).raw(" 0000").b);
	const auto plain = id3_frame(4, "COMM", builder().u8(0).raw("eng").u8(0).raw("Old comment").b);
	const auto original = make_mp3(4, {wm, other_popm, itunes, plain, id3_frame(4, "TIT2", builder().u8(0).raw("Old").b)}, 3000);

	const auto meta = rated(4);
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 4;
	u.legacy.title = "Zoë";
	u.legacy.comment = "New comment";
	u.legacy.album_artist = "Band";
	u.legacy.date_digitized = "1999-05-04T10:11:12";

	const auto result = update(original, u);
	CHECK(result.plan.in_place());
	CHECK_EQ(result.file.size(), original.size());

	int version = 0;
	std::size_t total = 0;
	const auto frames = id3_frames(result.file, version, total);
	CHECK_EQ(version, 4);
	CHECK(find(result.file, std::string(wm.begin(), wm.end())) != std::string::npos);
	CHECK(find(result.file, std::string(other_popm.begin(), other_popm.end())) != std::string::npos);
	CHECK(find(result.file, std::string(itunes.begin(), itunes.end())) != std::string::npos);
	CHECK(find(result.file, "Old comment") == std::string::npos);

	const auto wmp = std::ranges::find_if(frames, [](const id3_frame_view& f)
	{
		return f.id == "POPM" && find(f.body, "Windows Media Player 9 Series") == 0;
	});
	CHECK(wmp != frames.end() && wmp->body.size() == 30 + 5 && wmp->body[30] == 196);

	const auto* const title = frame_of(frames, "TIT2");
	CHECK(title && title->body.size() >= 3 && title->body[0] == 1 && title->body[1] == 0xFF && title->body[2] == 0xFE);
	CHECK(frame_of(frames, "TPE2") && frame_of(frames, "TPE2")->body == bytes({0, 'B', 'a', 'n', 'd'}));
	CHECK(frame_of(frames, "TDRC") && find(frame_of(frames, "TDRC")->body, "1999-05-04T10:11:12") == 1);
	CHECK(!frame_of(frames, "TYER"));
	CHECK_EQ(rating_of(result.file), 4);
}

TEST("an mp3 tag's dates follow its version, and no stars removes the rating")
{
	const auto popm = id3_frame(3, "POPM", builder().raw(text("Windows Media Player 9 Series")).u8(255).be32(0).b);
	const auto original = make_mp3(3, {popm, id3_frame(3, "TDRC", builder().u8(0).raw("2001").b)}, 500);

	libfiles::metadata_update u;
	u.legacy.date_digitized = "1999-05-04T10:11:12";
	u.legacy.rating = 0;

	const auto result = update(original, u);
	int version = 0;
	std::size_t total = 0;
	const auto frames = id3_frames(result.file, version, total);

	CHECK(frame_of(frames, "TYER") && frame_of(frames, "TYER")->body == bytes({0, '1', '9', '9', '9'}));
	CHECK(frame_of(frames, "TDAT") && frame_of(frames, "TDAT")->body == bytes({0, '0', '4', '0', '5'}));
	CHECK(frame_of(frames, "TIME") && frame_of(frames, "TIME")->body == bytes({0, '1', '0', '1', '1'}));
	CHECK(!frame_of(frames, "TDRC"));
	CHECK(!frame_of(frames, "POPM"));
}

TEST("an mp3 tag with unsynchronisation is refused")
{
	const auto original = make_mp3(3, {id3_frame(3, "TIT2", builder().u8(0).raw("Old").b)}, 100, 0x80);
	const auto meta = rated(1);
	libfiles::reader r(original);
	CHECK(!libfiles::plan_update(r, {&meta}).ok());
}

// FLV

TEST("an flv gains its packet after onMetaData, and its index moves with the media")
{
	const auto original = make_flv(true);
	const auto meta = rated(4);
	const auto result = update(original, {&meta});

	CHECK(result.plan.ok());
	CHECK(!result.plan.patchable());

	const auto v = walk_flv(result.file);
	CHECK(v.whole);
	CHECK(v.tags.size() == 4);
	if (v.tags.size() == 4)
	{
		CHECK_EQ(v.tags[0].first, 18);
		CHECK_EQ(v.tags[1].first, 18);
		CHECK(find(result.file, "onXMPData") == v.tags[1].second + 14);

		const auto positions = find(result.file, "filepositions") + 13 + 5;
		CHECK_EQ(amf_number_at(result.file, positions + 1), static_cast<double>(v.tags[2].second));
		CHECK_EQ(amf_number_at(result.file, positions + 10), static_cast<double>(v.tags[3].second));
	}

	const auto size_at = find(result.file, "filesize") + 8 + 1;
	CHECK_EQ(amf_number_at(result.file, size_at), static_cast<double>(result.file.size()));
	CHECK_EQ(rating_of(result.file), 4);

	const auto again = rated(2);
	const auto twice = update(result.file, {&again});
	CHECK(twice.plan.in_place());
	CHECK_EQ(rating_of(twice.file), 2);
}

TEST("an flv without onMetaData takes its packet as its first tag")
{
	const auto original = make_flv(false);
	const auto meta = rated(3);
	const auto result = update(original, {&meta});

	const auto v = walk_flv(result.file);
	CHECK(v.whole);
	CHECK(v.tags.size() == 3 && v.tags[0].first == 18);
	CHECK_EQ(rating_of(result.file), 3);
}

TEST("interrupted mp3 and flv patches read whole")
{
	const auto replay = [](const bytes& file, const libfiles::metadata_update& u, const int old_rating, const int new_rating,
	                       const auto& whole)
	{
		libfiles::reader r(file);
		const auto plan = libfiles::plan_update(r, u);
		const auto states = patch_states(file, plan);
		CHECK(states.size() >= 2);
		CHECK(!states.empty() && states.back() == update(file, u).file);

		for (const auto& state : states)
		{
			CHECK(whole(state));
			const auto rating = rating_of(state);
			CHECK(rating == old_rating || rating == new_rating);
		}
	};

	// A tag with room takes the edit in its padding; the audio after it never moves.
	const auto mp3 = make_mp3(4, {id3_frame(4, "TIT2", builder().u8(0).raw("Old").b)}, 3000);
	const auto meta = rated(4);
	libfiles::metadata_update u{&meta};
	u.legacy.title = "A title that takes some of the padding";
	replay(mp3, u, -100, 4, [&mp3](const bytes& state)
	{
		int version = 0;
		std::size_t total = 0;
		id3_frames(state, version, total);
		return version == 4 && total == mp3.size() - mpeg_audio.size() &&
			std::equal(mpeg_audio.begin(), mpeg_audio.end(), state.begin() + static_cast<std::ptrdiff_t>(total));
	});

	const auto first = rated(4);
	const auto flv = update(make_flv(true), {&first}).file;
	const auto again = rated(2);
	replay(flv, {&again}, 4, 2, [](const bytes& state) { return walk_flv(state).whole; });
}

TEST("wav and flv files read back the edits they keep outside the packet")
{
	// RIFF INFO keeps the title, artist, comment, copyright and genre, and the creation date, which is when a
	// WAV was recorded. FLV keeps nothing outside the packet.
	check_round_trip("wav", make_wav(), {"artist", "comment", "copyright", "genre", "recorded", "title", "year"});
	check_round_trip("flv", make_flv(true), {});
}