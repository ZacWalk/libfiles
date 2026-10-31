// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The ICC profiles the listing is held to, built in code: each takes a branch of the reader or
// of the descriptions - every text type, the colorant sets, curves and white points a summary names,
// tags that share a region, and headers and directories that lie - and every one is listed whole, cut
// short, cut short with its size field following, and corrupted a byte at a time. The fingerprint of
// those listings is the record of the listing as Diffractor made it before it moved here.

#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace icc_corpus
{
	using bytes = std::vector<std::uint8_t>;

	inline void put32(bytes& out, const std::size_t at, const std::uint32_t v)
	{
		out[at] = static_cast<std::uint8_t>(v >> 24);
		out[at + 1] = static_cast<std::uint8_t>(v >> 16);
		out[at + 2] = static_cast<std::uint8_t>(v >> 8);
		out[at + 3] = static_cast<std::uint8_t>(v);
	}

	inline void append16(bytes& out, const std::uint32_t v)
	{
		out.push_back(static_cast<std::uint8_t>(v >> 8));
		out.push_back(static_cast<std::uint8_t>(v));
	}

	inline void append32(bytes& out, const std::uint32_t v)
	{
		append16(out, v >> 16);
		append16(out, v & 0xFFFF);
	}

	inline void append_s15(bytes& out, const double v)
	{
		append32(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v * 65536.0))));
	}

	// A tag's payload starts with its type and four reserved bytes.
	inline bytes typed(const std::uint32_t type)
	{
		bytes b;
		append32(b, type);
		append32(b, 0);
		return b;
	}

	inline bytes desc(const std::string_view text, const std::uint32_t count = 0xFFFFFFFF)
	{
		auto b = typed(0x64657363);
		append32(b, count != 0xFFFFFFFF ? count : static_cast<std::uint32_t>(text.size() + 1));
		b.insert(b.end(), text.begin(), text.end());
		b.push_back(0);
		return b;
	}

	inline bytes text(const std::string_view value, const bool terminated = true)
	{
		auto b = typed(0x74657874);
		b.insert(b.end(), value.begin(), value.end());
		if (terminated) b.push_back(0);
		return b;
	}

	// One record per string, each UTF-16BE, at offsets from the start of the tag.
	inline bytes mluc(const std::vector<std::u16string>& texts, const std::uint32_t record_size = 12)
	{
		auto b = typed(0x6D6C7563);
		append32(b, static_cast<std::uint32_t>(texts.size()));
		append32(b, record_size);
		auto offset = static_cast<std::uint32_t>(16 + texts.size() * record_size);

		for (std::size_t i = 0; i < texts.size(); ++i)
		{
			const auto record_at = b.size();
			append16(b, 0x656E);
			append16(b, static_cast<std::uint32_t>(0x5553 + i));
			append32(b, static_cast<std::uint32_t>(texts[i].size() * 2));
			append32(b, offset);
			offset += static_cast<std::uint32_t>(texts[i].size() * 2);
			b.resize(record_at + record_size, 0);
		}

		for (const auto& t : texts)
		{
			for (const auto c : t) append16(b, c);
		}

		return b;
	}

	inline bytes xyz(const double x, const double y, const double z)
	{
		auto b = typed(0x58595A20);
		append_s15(b, x);
		append_s15(b, y);
		append_s15(b, z);
		return b;
	}

	inline bytes curve(const std::vector<std::uint16_t>& points)
	{
		auto b = typed(0x63757276);
		append32(b, static_cast<std::uint32_t>(points.size()));
		for (const auto p : points) append16(b, p);
		return b;
	}

	inline bytes parametric(const std::uint16_t function, const std::vector<double>& parameters)
	{
		auto b = typed(0x70617261);
		append16(b, function);
		append16(b, 0);
		for (const auto p : parameters) append_s15(b, p);
		return b;
	}

	inline bytes opaque(const std::uint32_t type, const std::size_t size, const std::uint8_t seed)
	{
		auto b = typed(type);
		for (std::size_t i = 0; i < size; ++i) b.push_back(static_cast<std::uint8_t>(seed + i * 7));
		return b;
	}

	struct header
	{
		std::uint32_t cmm = 0x6C636D73; // 'lcms'
		std::uint32_t version = 0x04300000;
		std::uint32_t device_class = 0x6D6E7472; // 'mntr'
		std::uint32_t colour_space = 0x52474220; // 'RGB '
		std::uint32_t connection_space = 0x58595A20; // 'XYZ '
		std::uint16_t created[6] = {2026, 10, 31, 12, 30, 15};
		std::uint32_t signature = 0x61637370; // 'acsp'
		std::uint32_t platform = 0x4D534654; // 'MSFT'
		std::uint32_t flags = 0;
		std::uint32_t manufacturer = 0;
		std::uint32_t model = 0;
		std::uint64_t attributes = 0;
		std::uint32_t intent = 0;
		double illuminant[3] = {0.9642, 1.0, 0.8249};
		std::uint32_t creator = 0x6C636D73; // 'lcms'
	};

	struct tag
	{
		std::uint32_t signature = 0;
		bytes payload;
		// Names an earlier tag's region, by its index, instead of a payload of its own.
		int shares = -1;
	};

	// A profile laid out as the spec has it: the header, the tag count and directory, then the payloads,
	// each on a four-byte boundary, with the size field the profile's length.
	inline bytes profile(const header& h, const std::vector<tag>& tags)
	{
		bytes p;
		append32(p, 0);
		append32(p, h.cmm);
		append32(p, h.version);
		append32(p, h.device_class);
		append32(p, h.colour_space);
		append32(p, h.connection_space);
		for (const auto v : h.created) append16(p, v);
		append32(p, h.signature);
		append32(p, h.platform);
		append32(p, h.flags);
		append32(p, h.manufacturer);
		append32(p, h.model);
		append32(p, static_cast<std::uint32_t>(h.attributes >> 32));
		append32(p, static_cast<std::uint32_t>(h.attributes & 0xFFFFFFFF));
		append32(p, h.intent);
		for (const auto v : h.illuminant) append_s15(p, v);
		append32(p, h.creator);
		p.resize(128, 0);

		append32(p, static_cast<std::uint32_t>(tags.size()));
		const auto directory = p.size();
		p.resize(directory + tags.size() * 12, 0);
		std::vector<std::pair<std::uint32_t, std::uint32_t>> regions;

		for (std::size_t i = 0; i < tags.size(); ++i)
		{
			std::pair<std::uint32_t, std::uint32_t> region;

			if (tags[i].shares >= 0)
			{
				region = regions[static_cast<std::size_t>(tags[i].shares)];
			}
			else
			{
				while (p.size() % 4 != 0) p.push_back(0);
				region = {static_cast<std::uint32_t>(p.size()), static_cast<std::uint32_t>(tags[i].payload.size())};
				p.insert(p.end(), tags[i].payload.begin(), tags[i].payload.end());
			}

			regions.push_back(region);
			put32(p, directory + i * 12, tags[i].signature);
			put32(p, directory + i * 12 + 4, region.first);
			put32(p, directory + i * 12 + 8, region.second);
		}

		put32(p, 0, static_cast<std::uint32_t>(p.size()));
		return p;
	}

	// Where a profile's directory entry starts.
	constexpr std::size_t entry(const std::size_t index) { return 132 + index * 12; }

	constexpr std::uint32_t red_colorant = 0x7258595A; // 'rXYZ'
	constexpr std::uint32_t green_colorant = 0x6758595A; // 'gXYZ'
	constexpr std::uint32_t blue_colorant = 0x6258595A; // 'bXYZ'
	constexpr std::uint32_t red_curve = 0x72545243; // 'rTRC'
	constexpr std::uint32_t green_curve = 0x67545243; // 'gTRC'
	constexpr std::uint32_t blue_curve = 0x62545243; // 'bTRC'

	inline void add_colorants(std::vector<tag>& tags, const double (&v)[9])
	{
		tags.push_back({red_colorant, xyz(v[0], v[1], v[2])});
		tags.push_back({green_colorant, xyz(v[3], v[4], v[5])});
		tags.push_back({blue_colorant, xyz(v[6], v[7], v[8])});
	}

	// One curve shared by all three channels, as the sRGB profile most cameras embed has it.
	inline void add_shared_curves(std::vector<tag>& tags, bytes curve_payload)
	{
		const auto first = static_cast<int>(tags.size());
		tags.push_back({red_curve, std::move(curve_payload)});
		tags.push_back({green_curve, {}, first});
		tags.push_back({blue_curve, {}, first});
	}

	inline std::vector<std::uint16_t> ramp(const std::size_t count)
	{
		std::vector<std::uint16_t> points(count);
		for (std::size_t i = 0; i < count; ++i) points[i] = static_cast<std::uint16_t>(i * 65535 / (count - 1));
		return points;
	}

	inline std::vector<bytes> profiles()
	{
		constexpr double srgb[9] = {0.4360, 0.2225, 0.0139, 0.3851, 0.7169, 0.0971, 0.1431, 0.0606, 0.7141};
		constexpr double adobe[9] = {0.6097, 0.3111, 0.0195, 0.2052, 0.6257, 0.0609, 0.1492, 0.0632, 0.7448};
		constexpr double p3[9] = {0.5151, 0.2412, -0.0011, 0.2920, 0.6922, 0.0419, 0.1571, 0.0666, 0.7841};
		constexpr double rec2020[9] = {0.6734, 0.2790, -0.0019, 0.1656, 0.6757, 0.0299, 0.1250, 0.0453, 0.7973};
		constexpr double prophoto[9] = {0.7977, 0.2880, 0.0000, 0.1352, 0.7119, 0.0000, 0.0313, 0.0001, 0.8249};
		constexpr double custom[9] = {0.5000, 0.3000, 0.1000, 0.3000, 0.6000, 0.1000, 0.1500, 0.1000, 0.7000};

		std::vector<bytes> all;

		{
			// sRGB as the version 2 profile cameras embed it: ASCII names, one table curve for all three.
			header h;
			h.version = 0x02100000;
			h.platform = 0x4D534654; // 'MSFT'
			h.manufacturer = 0x49454320; // 'IEC '
			h.model = 0x73524742; // 'sRGB'
			h.creator = 0x48502020; // 'HP  '
			std::vector<tag> tags;
			tags.push_back({0x63707274, text("Copyright (c) 1998 Hewlett-Packard Company")});
			tags.push_back({0x64657363, desc("sRGB IEC61966-2.1")});
			tags.push_back({0x77747074, xyz(0.9642, 1.0, 0.8249)});
			add_colorants(tags, srgb);
			add_shared_curves(tags, curve(ramp(1024)));
			tags.push_back({0x646D6E64, desc("IEC http://www.iec.ch")});
			tags.push_back({0x646D6464, desc("IEC 61966-2.1 Default RGB colour space - sRGB")});
			tags.push_back({0x6C756D69, xyz(76.0365, 80.0, 87.1246)});
			tags.push_back({0x76696577, opaque(0x76696577, 28, 3)});
			tags.push_back({0x74656368, opaque(0x73696720, 4, 9)});
			all.push_back(profile(h, tags));
		}

		{
			// Display P3 as Apple writes it: version 4, localized names, a parametric sRGB-style curve.
			header h;
			h.version = 0x04000000;
			h.platform = 0x4150504C; // 'APPL'
			h.creator = 0x6170706C; // 'appl'
			h.cmm = 0x6170706C;
			std::vector<tag> tags;
			tags.push_back({0x64657363, mluc({u"Display P3"})});
			tags.push_back({0x63707274, mluc({u"Copyright Apple Inc., 2017"})});
			tags.push_back({0x77747074, xyz(0.96419, 1.0, 0.82489)});
			add_colorants(tags, p3);
			add_shared_curves(tags, parametric(3, {2.39999, 0.94786, 0.05214, 0.07739, 0.04045}));
			tags.push_back({0x63686164, opaque(0x73663332, 36, 17)});
			all.push_back(profile(h, tags));
		}

		{
			// Adobe RGB (1998): a gamma given as a single curve entry, and a D65 media white point.
			header h;
			h.version = 0x02100000;
			h.platform = 0x4150504C;
			h.manufacturer = 0x6E6F6E65; // 'none'
			h.creator = 0x41444245; // 'ADBE'
			h.cmm = 0x41444245;
			std::vector<tag> tags;
			tags.push_back({0x63707274, text("Copyright 1999 Adobe Systems Incorporated")});
			tags.push_back({0x64657363, desc("Adobe RGB (1998)")});
			tags.push_back({0x77747074, xyz(0.9505, 1.0, 1.0891)});
			add_colorants(tags, adobe);
			add_shared_curves(tags, curve({563}));
			all.push_back(profile(h, tags));
		}

		{
			// Rec. 2020: two localized names, of which the first is the profile's, a parametric gamma, and
			// a D55 white point.
			header h;
			h.intent = 1;
			std::vector<tag> tags;
			tags.push_back({0x64657363, mluc({u"Rec. 2020", u"ITU-R BT.2020"})});
			tags.push_back({0x77747074, xyz(0.9568, 1.0, 0.9214)});
			add_colorants(tags, rec2020);
			add_shared_curves(tags, parametric(0, {2.4}));
			all.push_back(profile(h, tags));
		}

		{
			// ProPhoto RGB from a scanner: a linear red curve and gamma green and blue, a D93 white point.
			header h;
			h.version = 0x02200000;
			h.device_class = 0x73636E72; // 'scnr'
			h.platform = 0x53474920; // 'SGI '
			h.creator = 0x4B4F4441; // 'KODA'
			h.intent = 2;
			h.flags = 3;
			h.attributes = 0x0000000100000002ull;
			std::vector<tag> tags;
			tags.push_back({0x64657363, desc("ProPhoto RGB")});
			tags.push_back({0x77747074, xyz(0.9529, 1.0, 1.4130)});
			add_colorants(tags, prophoto);
			tags.push_back({red_curve, curve({})});
			tags.push_back({green_curve, curve({461})});
			tags.push_back({blue_curve, {}, static_cast<int>(tags.size() - 1)});
			all.push_back(profile(h, tags));
		}

		{
			// Nothing recognised: custom primaries, a parametric curve of another type, an unnamed white
			// point, an unknown intent, class, space and platform, and no creator, CMM or date.
			header h;
			h.cmm = 0;
			h.creator = 0;
			h.device_class = 0x73706163; // 'spac'
			h.colour_space = 0x59436272; // 'YCbr'
			h.connection_space = 0x4C616220; // 'Lab '
			h.platform = 0x5A5A5A5A; // 'ZZZZ'
			h.intent = 7;
			for (auto& v : h.created) v = 0;
			h.attributes = 0xFEDCBA9876543210ull;
			h.flags = 0x80000001;
			std::vector<tag> tags;
			tags.push_back({0x64657363, text("Custom profile")});
			tags.push_back({0x77747074, xyz(0.95, 1.0, 0.5)});
			add_colorants(tags, custom);
			add_shared_curves(tags, parametric(4, {2.2, 1.0, 0.0, 0.1, 0.05, 0.0, 0.0}));
			all.push_back(profile(h, tags));
		}

		{
			// Greyscale: one curve, no colorants and a white point of zeros.
			header h;
			h.colour_space = 0x47524159; // 'GRAY'
			h.platform = 0;
			std::vector<tag> tags;
			tags.push_back({0x64657363, text("Gray Gamma 2.2")});
			tags.push_back({0x6B545243, curve({563})});
			tags.push_back({0x77747074, xyz(0.0, 0.0, 0.0)});
			all.push_back(profile(h, tags));
		}

		{
			// A press: CMYK to Lab through tables nothing here reads, so they list by size.
			header h;
			h.version = 0x02400000;
			h.device_class = 0x70727472; // 'prtr'
			h.colour_space = 0x434D594B; // 'CMYK'
			h.connection_space = 0x4C616220; // 'Lab '
			h.platform = 0;
			h.intent = 3;
			h.created[0] = 2001;
			h.created[1] = 1;
			h.created[2] = 1;
			h.created[3] = 0;
			h.created[4] = 0;
			h.created[5] = 0;
			std::vector<tag> tags;
			tags.push_back({0x64657363, desc("Coated FOGRA39 (ISO 12647-2:2004)")});
			tags.push_back({0x63707274, text("Copyright FOGRA")});
			tags.push_back({0x41324230, opaque(0x6D667432, 600, 1)});
			tags.push_back({0x42324130, opaque(0x6D667432, 700, 2)});
			tags.push_back({0x67616D74, opaque(0x6D667431, 300, 3)});
			tags.push_back({0x77747074, xyz(0.9642, 1.0, 0.8249)});
			all.push_back(profile(h, tags));
		}

		{
			// Text that is not what it claims: counts of none and of too much, NULs inside and around,
			// no terminator, and UTF-16 with surrogates paired, alone, and in odd lengths.
			header h;
			std::vector<tag> tags;
			tags.push_back({0x64657363, desc("Name\0Rest", 0)});
			tags.push_back({0x63707274, desc("Copyright", 4000)});
			tags.push_back({0x646D6E64, desc(std::string_view("\0\0Maker\0", 8))});
			tags.push_back({0x646D6464, text("Model", false)});
			tags.push_back({0x74737431, text(std::string_view("\0\0\0", 3))});
			tags.push_back({0x74737432, mluc({u"A\xD800" u"B"})});
			tags.push_back({0x74737433, mluc({u"\xDC00x"})});
			tags.push_back({0x74737434, mluc({u"\xD83D\xDE00 smile"})});
			tags.push_back({0x74737435, mluc({u"Wide", u"Second"}, 16)});
			tags.push_back({0x74737436, mluc({u"Zero"}, 0)});
			tags.push_back({0x74737437, desc(std::string_view("Inner\0NUL", 9))});
			auto odd = mluc({u"Odd"});
			put32(odd, 20, 5);
			tags.push_back({0x74737438, odd});
			auto early = mluc({u"Early", u"Later"});
			put32(early, 24, 0);
			tags.push_back({0x74737439, early});
			auto beyond = mluc({u"Beyond"});
			put32(beyond, 24, 0xFFFFFFFF);
			tags.push_back({0x7473743A, beyond});
			auto many = mluc({u"Many"});
			put32(many, 8, 0x40000000);
			tags.push_back({0x7473743B, many});
			tags.push_back({0x7473743C, typed(0x64657363)});
			tags.push_back({0x7473743D, typed(0x6D6C7563)});
			all.push_back(profile(h, tags));
		}

		{
			// A directory that lies: an entry past the end, one of no size, one running off the end, one
			// whose type is cut off, a signature twice, and tags whose regions add up to more than the
			// profile, the later ones refused.
			header h;
			std::vector<tag> tags;
			tags.push_back({0x64657363, desc("Directory")});
			for (std::uint32_t i = 0; i < 6; ++i) tags.push_back({0x64697230 + i, opaque(0x62696E30 + i, 40 + i, static_cast<std::uint8_t>(i))});
			tags.push_back({0x64657363, desc("Directory again")});
			auto p = profile(h, tags);
			put32(p, entry(1) + 4, static_cast<std::uint32_t>(p.size() + 10));
			put32(p, entry(2) + 8, 0);
			put32(p, entry(3) + 8, static_cast<std::uint32_t>(p.size()));
			put32(p, entry(4) + 4, static_cast<std::uint32_t>(p.size() - 2));
			put32(p, entry(4) + 8, 2);
			all.push_back(p);

			std::vector<tag> overlapping;
			for (std::uint32_t i = 0; i < 12; ++i) overlapping.push_back({0x6F766C30 + i, opaque(0x62696E61, 4, 0)});
			overlapping.push_back({0x70616431, opaque(0x62696E62, 1200, 5)});
			auto q = profile(h, overlapping);
			const auto pad_at = static_cast<std::uint32_t>(q.size() - 1208);
			for (std::uint32_t i = 0; i < 12; ++i)
			{
				put32(q, entry(i) + 4, pad_at + i);
				put32(q, entry(i) + 8, 1000);
			}
			all.push_back(q);

			auto declared = profile(h, tags);
			put32(declared, 128, 60);
			all.push_back(declared);
		}

		{
			// Headers that are not a profile's: too short, a size below the header's own or beyond the
			// block, no signature, a header and nothing else, more tags than any profile has, and none.
			header h;
			const auto base = profile(h, {{0x64657363, desc("Header")}});
			all.emplace_back(base.begin(), base.begin() + 127);
			auto small = base;
			put32(small, 0, 127);
			all.push_back(small);
			auto large = base;
			put32(large, 0, static_cast<std::uint32_t>(base.size() + 1));
			all.push_back(large);
			auto unsigned_header = base;
			put32(unsigned_header, 36, 0x61637371);
			all.push_back(unsigned_header);
			bytes header_only(base.begin(), base.begin() + 128);
			put32(header_only, 0, 128);
			all.push_back(header_only);
			auto crowded = base;
			put32(crowded, 128, 1001);
			all.push_back(crowded);
			auto thousand = base;
			put32(thousand, 128, 1000);
			all.push_back(thousand);
			all.push_back(profile(h, {}));
		}

		return all;
	}

	// A tag larger than the listing reads, in a profile large enough to hold it.
	inline bytes oversized()
	{
		header h;
		return profile(h, {{0x64657363, desc("Oversized")}, {0x6269676F, opaque(0x62696E7A, 1024 * 1024 + 16, 11)}});
	}

	// The profile whole; cut short, the size field still claiming the whole; cut short with the size field
	// following, so the directory outlives its tags; and with one byte inverted and one incremented, at
	// offsets spread over the profile.
	inline std::vector<bytes> variants(const bytes& p, const std::size_t parts = 40)
	{
		std::vector<bytes> out;
		out.push_back(p);
		const auto n = p.size();
		const auto step = n / parts > 0 ? n / parts : 1;

		for (std::size_t length = 0; length < n && length < 140; ++length) out.emplace_back(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(length));
		for (std::size_t length = 140; length < n; length += step) out.emplace_back(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(length));

		for (std::size_t length = 128; length < n; length += step)
		{
			bytes v(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(length));
			put32(v, 0, static_cast<std::uint32_t>(length));
			out.push_back(std::move(v));
		}

		const auto corrupt_step = n / (parts * 2) > 0 ? n / (parts * 2) : 1;

		for (std::size_t at = 0; at < n; at += corrupt_step)
		{
			auto inverted = p;
			inverted[at] = static_cast<std::uint8_t>(~inverted[at]);
			out.push_back(std::move(inverted));
			auto incremented = p;
			incremented[at] = static_cast<std::uint8_t>(incremented[at] + 1);
			out.push_back(std::move(incremented));
		}

		return out;
	}

	// One row of a listing, as both Diffractor's rows and libfiles' are read for the fingerprint.
	struct row
	{
		std::string_view key;
		std::string_view value;
		std::string_view shape;
		std::string_view id;
		int depth = 0;
		bool container = false;
		bool open_by_default = false;
		bool has_bytes = false;
		std::span<const std::uint8_t> bytes;
	};

	// FNV-1a over every field of every row of every listing, in order.
	class fingerprint
	{
	public:
		void add(const std::vector<row>& rows)
		{
			number(rows.size());

			for (const auto& r : rows)
			{
				text(r.key);
				text(r.value);
				text(r.shape);
				text(r.id);
				number(static_cast<std::uint64_t>(static_cast<std::int64_t>(r.depth)));
				number(r.container ? 1 : 0);
				number(r.open_by_default ? 1 : 0);
				number(r.has_bytes ? 1 : 0);

				if (r.has_bytes)
				{
					number(r.bytes.size());
					data(r.bytes.data(), r.bytes.size());
				}
			}

			++listings_;
			rows_ += rows.size();
		}

		std::uint64_t value() const { return hash_; }
		std::size_t listings() const { return listings_; }
		std::size_t rows() const { return rows_; }

	private:
		void data(const void* p, const std::size_t size)
		{
			const auto* b = static_cast<const std::uint8_t*>(p);
			for (std::size_t i = 0; i < size; ++i)
			{
				hash_ ^= b[i];
				hash_ *= 1099511628211ull;
			}
		}

		void number(const std::uint64_t v)
		{
			std::uint8_t b[8];
			for (auto i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(v >> (i * 8));
			data(b, sizeof b);
		}

		void text(const std::string_view s)
		{
			number(s.size());
			data(s.data(), s.size());
		}

		std::uint64_t hash_ = 14695981039346656037ull;
		std::size_t listings_ = 0;
		std::size_t rows_ = 0;
	};
}
