// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Building test files byte by byte, shared by the scan and write tests, so every field a test
// reads back is one it put there.

#pragma once

#include "scan_internal.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_build
{
	using bytes = std::vector<std::uint8_t>;

	struct builder
	{
		bytes b;

		builder& raw(const std::string_view s)
		{
			// Byte by byte: GCC 13, optimizing vector::insert into an empty vector, warns of a memmove from null.
			for (const auto c : s) b.push_back(static_cast<std::uint8_t>(c));
			return *this;
		}

		builder& raw(const bytes& v)
		{
			b.insert(b.end(), v.begin(), v.end());
			return *this;
		}

		builder& u8(const unsigned v)
		{
			b.push_back(static_cast<std::uint8_t>(v));
			return *this;
		}

		builder& be16(const unsigned v) { return u8(v >> 8).u8(v); }
		builder& le16(const unsigned v) { return u8(v).u8(v >> 8); }
		builder& be32(const std::uint32_t v) { return be16(v >> 16).be16(v & 0xffff); }
		builder& le32(const std::uint32_t v) { return le16(v & 0xffff).le16(v >> 16); }

		builder& zeros(const std::size_t n)
		{
			b.insert(b.end(), n, 0);
			return *this;
		}

		std::size_t size() const { return b.size(); }
	};

	inline bytes to_bytes(const std::string_view s)
	{
		return {s.begin(), s.end()};
	}

	// Every character of a literal, NULs included.
	template <std::size_t N> bytes lit(const char (&s)[N])
	{
		return bytes(s, s + N - 1);
	}

	inline bytes rgb_icc_profile()
	{
		bytes icc(132, 0);
		icc[3] = 132;
		icc[8] = 0x02;
		icc[9] = 0x10;
		std::memcpy(icc.data() + 12, "mntrRGB XYZ ", 12);
		std::memcpy(icc.data() + 36, "acsp", 4);
		const std::uint8_t d50[] = {0, 0, 0xf6, 0xd6, 0, 1, 0, 0, 0, 0, 0xd3, 0x2d};
		std::memcpy(icc.data() + 68, d50, sizeof d50);
		return icc;
	}

	inline bytes rgb_icc_profile_with_copyright()
	{
		auto icc = rgb_icc_profile();
		icc.resize(144, 0);
		std::memcpy(icc.data() + 132, "cprt", 4);
		const std::string copyright =
			"Copyright 2026 Example. This RGB profile retains a standard copyright tag while testing "
			"lossless metadata transfer between independent image encoders and decoders.";
		icc.insert(icc.end(), {'t', 'e', 'x', 't', 0, 0, 0, 0});
		icc.insert(icc.end(), copyright.begin(), copyright.end());
		icc.push_back(0);
		const auto tag_size = static_cast<std::uint32_t>(icc.size() - 144);
		while (icc.size() % 4) icc.push_back(0);
		const auto put32 = [&](const std::size_t offset, const std::uint32_t value)
		{
			for (unsigned i = 0; i < 4; ++i) icc[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
		};
		put32(0, static_cast<std::uint32_t>(icc.size()));
		put32(128, 1);
		put32(136, 144);
		put32(140, tag_size);
		return icc;
	}

	inline std::string to_string(const std::vector<std::uint8_t>& v)
	{
		return {v.begin(), v.end()};
	}

	inline void png_chunk(builder& p, const std::string_view type, const bytes& data, const bool damage = false)
	{
		p.be32(static_cast<std::uint32_t>(data.size()));
		const auto start = p.size();
		p.raw(type).raw(data);
		auto crc = libfiles::detail::crc32(0, std::span(p.b).subspan(start));
		if (damage) crc ^= 1;
		p.be32(crc);
	}

	inline bytes png_ihdr(const std::uint32_t width, const std::uint32_t height, const unsigned color_type)
	{
		return builder().be32(width).be32(height).u8(8).u8(color_type).u8(0).u8(0).u8(0).b;
	}

	inline void riff_chunk(builder& w, const std::string_view fourcc, const bytes& data)
	{
		w.raw(fourcc).le32(static_cast<std::uint32_t>(data.size())).raw(data);
		if (data.size() & 1) w.u8(0);
	}

	inline bytes finish_riff(const builder& body)
	{
		builder w;
		w.raw("RIFF").le32(static_cast<std::uint32_t>(body.size() + 4)).raw("WEBP").raw(body.b);
		return w.b;
	}

	// A VP8 key frame header for a width x height image.
	inline bytes vp8_frame(const unsigned width, const unsigned height)
	{
		// Key frame, version 0, shown, first partition 16 bytes.
		const std::uint32_t bits = 0u | (0u << 1) | (1u << 4) | (16u << 5);
		return builder()
		    .u8(bits & 0xff)
		    .u8((bits >> 8) & 0xff)
		    .u8(bits >> 16)
		    .u8(0x9d)
		    .u8(0x01)
		    .u8(0x2a)
		    .le16(width)
		    .le16(height)
		    .zeros(200)
		    .b;
	}

	// A VP8L header for a width x height image.
	inline bytes vp8l_frame(const unsigned width, const unsigned height, const bool alpha)
	{
		const auto bits = (width - 1) | (height - 1) << 14 | (alpha ? 1u : 0u) << 28;
		return builder().u8(0x2f).le32(bits).zeros(8).b;
	}

	inline bytes vp8x(const std::uint32_t flags, const unsigned width, const unsigned height)
	{
		const auto w = width - 1;
		const auto h = height - 1;
		return builder()
		    .le32(flags)
		    .u8(w & 0xff)
		    .u8((w >> 8) & 0xff)
		    .u8(w >> 16)
		    .u8(h & 0xff)
		    .u8((h >> 8) & 0xff)
		    .u8(h >> 16)
		    .b;
	}

	// A JPEG of the segments given, then tables, a frame and a scan whose entropy-coded data holds a stuffed
	// byte and a restart, as an encoder writes them. fill tells one picture's data from another's.
	inline bytes jpeg_of(const std::initializer_list<std::pair<unsigned, bytes>> segments,
	                     const std::uint8_t fill = 0x11)
	{
		builder j;
		j.u8(0xFF).u8(0xD8);

		const auto segment = [&j](const unsigned marker, const bytes& payload)
		{
			j.u8(0xFF).u8(marker).be16(static_cast<unsigned>(payload.size() + 2)).raw(payload);
		};

		for (const auto& [marker, payload] : segments) segment(marker, payload);
		segment(0xDB, to_bytes("tables"));
		segment(0xC0, to_bytes("frame"));
		segment(0xDA, to_bytes("scan"));
		j.u8(fill).u8(0xFF).u8(0x00).u8(fill).u8(0xFF).u8(0xD0).u8(fill).u8(0xFF).u8(0xD9);
		return j.b;
	}

	// The segments before a JPEG's first scan: each marker and its payload.
	inline std::vector<std::pair<unsigned, bytes>> jpeg_segments(const bytes& jpeg)
	{
		std::vector<std::pair<unsigned, bytes>> segments;
		std::size_t at = 2;

		while (at + 4 <= jpeg.size() && jpeg[at] == 0xFF && jpeg[at + 1] != 0xDA && jpeg[at + 1] != 0xD9)
		{
			const std::size_t length = static_cast<std::size_t>(jpeg[at + 2]) << 8 | jpeg[at + 3];
			segments.emplace_back(jpeg[at + 1], bytes(jpeg.begin() + static_cast<std::ptrdiff_t>(at + 4),
			                                          jpeg.begin() + static_cast<std::ptrdiff_t>(at + 2 + length)));
			at += 2 + length;
		}

		return segments;
	}

	// An XMP packet of the rdf:Description given, and the APP1 payload that holds it.
	inline std::string xmp_packet(const std::string_view description)
	{
		return "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
		       "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">" +
		       std::string(description) + "</rdf:RDF></x:xmpmeta>";
	}

	inline bytes xmp_app1(const std::string_view description)
	{
		return builder().raw(std::string_view("http://ns.adobe.com/xap/1.0/\0", 29)).raw(xmp_packet(description)).b;
	}

	// What an Ultra HDR primary's XMP says of the gain map after it, stated at length bytes.
	inline std::string ultra_hdr_primary_description(const std::size_t length)
	{
		return std::format(
			R"(<rdf:Description rdf:about="" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" )"
			R"(xmlns:Container="http://ns.google.com/photos/1.0/container/" )"
			R"(xmlns:Item="http://ns.google.com/photos/1.0/container/item/" hdrgm:Version="1.0">)"
			R"(<Container:Directory><rdf:Seq>)"
			R"(<rdf:li rdf:parseType="Resource"><Container:Item Item:Semantic="Primary" )"
			R"(Item:Mime="image/jpeg"/></rdf:li>)"
			R"(<rdf:li rdf:parseType="Resource"><Container:Item Item:Semantic="GainMap" Item:Mime="image/jpeg" )"
			R"(Item:Length="{}"/></rdf:li>)"
			R"(</rdf:Seq></Container:Directory></rdf:Description>)",
			length);
	}

	// A gain map's own XMP, in Adobe's scheme.
	inline constexpr std::string_view gain_map_description =
		R"(<rdf:Description rdf:about="" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" )"
		R"(hdrgm:Version="1.0" hdrgm:GainMapMax="2.5" hdrgm:HDRCapacityMax="2.5"/>)";

	// ISO 21496-1's APP2: its identifier, then the metadata - in a primary only its two versions.
	inline bytes iso_gain_map_app2(const bytes& metadata)
	{
		return builder().raw(std::string_view("urn:iso:std:iso:ts:21496:-1\0", 28)).raw(metadata).b;
	}

	// An ISO base media box, as HEIF and the JPEG XL container are made of.
	inline bytes iso_box(const std::string_view type, const bytes& payload)
	{
		return builder().be32(static_cast<std::uint32_t>(payload.size() + 8)).raw(type).raw(payload).b;
	}

	inline bytes iso_full_box(const std::string_view type, const unsigned version, const std::uint32_t flags,
	                          const bytes& payload)
	{
		return iso_box(type, builder().u8(version).u8(flags >> 16).u8(flags >> 8).u8(flags).raw(payload).b);
	}

	// --- HEIF, built box by box: a file type box, the meta box with its items, and an mdat holding their data

	// A big-endian TIFF stream with one IFD0 orientation entry.
	inline bytes big_endian_orientation_tiff(const unsigned orientation)
	{
		return builder()
		    .raw("MM")
		    .be16(42)
		    .be32(8)
		    .be16(1)
		    .be16(0x0112)
		    .be16(3)
		    .be32(1)
		    .be16(orientation)
		    .be16(0)
		    .be32(0)
		    .zeros(8)
		    .b;
	}

	inline bytes ispe(const std::uint32_t width, const std::uint32_t height)
	{
		return iso_full_box("ispe", 0, 0, builder().be32(width).be32(height).b);
	}

	inline bytes hvcc(const unsigned chroma, const unsigned luma_bits = 8, const unsigned chroma_bits = 8)
	{
		// clang-format off
		return iso_box("hvcC", builder().u8(1).u8(1).be32(0x60000000).zeros(6).u8(90).be16(0xF000).u8(0xFC)
		                                .u8(0xFC | chroma).u8(0xF8 | (luma_bits - 8)).u8(0xF8 | (chroma_bits - 8))
		                                .be16(0).u8(0x0F).u8(0).b);
		// clang-format on
	}

	struct heif_item
	{
		std::uint32_t id = 0;
		std::string type;
		bytes data;
		bool hidden = false;
		std::string content_type;
		std::string content_encoding;
		bool in_idat = false;
		std::size_t extents = 1;
		// ipco indices, from 1, and whether each is essential.
		std::vector<std::pair<unsigned, bool>> properties;
		std::optional<std::uint32_t> idat_offset;
	};

	struct heif_reference
	{
		std::string type;
		std::uint32_t from = 0;
		std::vector<std::uint32_t> to;
	};

	struct heif_spec
	{
		std::string major = "heic";
		std::vector<std::string> brands = {"mif1", "heic"};
		std::uint32_t primary = 1;
		std::vector<bytes> properties;
		std::vector<heif_item> items;
		std::vector<heif_reference> references;
		// Boxes the scan has no use for: in the meta box after its own, and between the file type
		// and meta boxes.
		std::vector<bytes> meta_boxes;
		bytes before_meta;
		// Boxes after everything else: a motion photo's video, a movie.
		bytes after_mdat;
		bool mdat_first = false;
		bool large_meta_header = false;
		bool mdat_to_end = false;
		// The item locations' version: 0 has no construction method, so nothing is in idat; 2 has four-byte item
		// ids. Without bases, each extent's offset is from the start of the file.
		unsigned iloc_version = 1;
		bool iloc_bases = true;
	};

	inline bytes make_heif_meta(const heif_spec& s, const std::uint32_t mdat_payload)
	{
		builder iinf;
		iinf.be16(static_cast<unsigned>(s.items.size()));

		for (const auto& item : s.items)
		{
			builder infe;
			infe.be16(item.id).be16(0).raw(item.type).u8(0);
			if (item.type == "mime") infe.raw(item.content_type).u8(0).raw(item.content_encoding).u8(0);
			iinf.raw(iso_full_box("infe", 2, item.hidden ? 1u : 0u, infe.b));
		}

		// Four-byte offsets and lengths, with four-byte base offsets unless they are left out.
		builder iloc;
		iloc.u8(0x44).u8(s.iloc_bases ? 0x40 : 0x00);
		if (s.iloc_version < 2) iloc.be16(static_cast<unsigned>(s.items.size()));
		else iloc.be32(static_cast<std::uint32_t>(s.items.size()));
		std::uint32_t mdat_at = 0;
		std::uint32_t idat_at = 0;
		bytes idat;

		for (const auto& item : s.items)
		{
			if (s.iloc_version < 2) iloc.be16(item.id);
			else iloc.be32(item.id);
			if (s.iloc_version >= 1) iloc.be16(item.in_idat ? 1 : 0);
			iloc.be16(0);
			const auto base = item.in_idat ? 0 : mdat_payload;
			if (s.iloc_bases) iloc.be32(base);
			iloc.be16(static_cast<unsigned>(item.extents));

			auto& at = item.in_idat ? idat_at : mdat_at;
			const auto size = static_cast<std::uint32_t>(item.data.size());

			for (std::size_t e = 0; e < item.extents; ++e)
			{
				const auto begin = static_cast<std::uint32_t>(size * e / item.extents);
				const auto end = static_cast<std::uint32_t>(size * (e + 1) / item.extents);
				iloc.be32((s.iloc_bases ? 0 : base) + item.idat_offset.value_or(at) + begin).be32(end - begin);
			}

			if (item.in_idat) idat.insert(idat.end(), item.data.begin(), item.data.end());
			at += size;
		}

		builder ipco;
		for (const auto& p : s.properties) ipco.raw(p);

		builder ipma;
		ipma.be32(static_cast<std::uint32_t>(s.items.size()));

		for (const auto& item : s.items)
		{
			ipma.be16(item.id).u8(static_cast<unsigned>(item.properties.size()));
			for (const auto& [index, essential] : item.properties) ipma.u8((essential ? 0x80u : 0u) | index);
		}

		builder iref;

		for (const auto& ref : s.references)
		{
			builder entry;
			entry.be16(ref.from).be16(static_cast<unsigned>(ref.to.size()));
			for (const auto to : ref.to) entry.be16(to);
			iref.raw(iso_box(ref.type, entry.b));
		}

		builder meta;
		meta.raw(iso_full_box("hdlr", 0, 0, builder().be32(0).raw("pict").zeros(12).u8(0).b));
		meta.raw(iso_full_box("pitm", 0, 0, builder().be16(s.primary).b));
		meta.raw(iso_full_box("iinf", 0, 0, iinf.b));
		meta.raw(iso_full_box("iloc", s.iloc_version, 0, iloc.b));
		meta.raw(iso_box("iprp", builder().raw(iso_box("ipco", ipco.b)).raw(iso_full_box("ipma", 0, 0, ipma.b)).b));
		if (!s.references.empty()) meta.raw(iso_full_box("iref", 0, 0, iref.b));
		if (!idat.empty()) meta.raw(iso_box("idat", idat));
		for (const auto& box : s.meta_boxes) meta.raw(box);

		const auto payload = builder().be32(0).raw(meta.b).b;

		if (s.large_meta_header)
		{
			return builder()
			    .be32(1)
			    .raw("meta")
			    .be32(0)
			    .be32(static_cast<std::uint32_t>(payload.size() + 16))
			    .raw(payload)
			    .b;
		}

		return iso_box("meta", payload);
	}

	inline bytes make_heif(const heif_spec& s)
	{
		builder ftyp_payload;
		ftyp_payload.raw(s.major).be32(0);
		for (const auto& brand : s.brands) ftyp_payload.raw(brand);
		const auto ftyp = iso_box("ftyp", ftyp_payload.b);

		bytes mdat_payload;
		for (const auto& item : s.items)
		{
			if (!item.in_idat) mdat_payload.insert(mdat_payload.end(), item.data.begin(), item.data.end());
		}

		const auto mdat = s.mdat_to_end ? builder().be32(0).raw("mdat").raw(mdat_payload).b
		                                : iso_box("mdat", mdat_payload);

		// The meta box's size does not depend on the offsets it holds, so it is built twice.
		const auto probe = make_heif_meta(s, 0);
		const auto mdat_payload_at = static_cast<std::uint32_t>(ftyp.size() + s.before_meta.size() +
		                                                        (s.mdat_first ? 0 : probe.size()) + 8);
		const auto meta = make_heif_meta(s, mdat_payload_at);

		builder file;
		file.raw(ftyp).raw(s.before_meta);
		if (s.mdat_first) file.raw(mdat).raw(meta);
		else file.raw(meta).raw(mdat);
		file.raw(s.after_mdat);
		return file.b;
	}

	inline heif_item image_item(const std::uint32_t id, const std::string_view type,
	                            std::vector<std::pair<unsigned, bool>> properties)
	{
		heif_item item;
		item.id = id;
		item.type = type;
		item.data = bytes(32, 0x55);
		item.properties = std::move(properties);
		return item;
	}

	inline heif_item exif_item(const std::uint32_t id, const unsigned orientation)
	{
		heif_item item;
		item.id = id;
		item.type = "Exif";
		item.data = builder().be32(6).raw(lit("Exif\0\0")).raw(big_endian_orientation_tiff(orientation)).b;
		return item;
	}

	inline heif_item mime_item(const std::uint32_t id, const std::string_view content_type, const std::string_view text)
	{
		heif_item item;
		item.id = id;
		item.type = "mime";
		item.content_type = content_type;
		item.data = to_bytes(text);
		return item;
	}

	// A HEVC still: one image of 640 x 480, 4:2:0, with the given extra properties after ispe and hvcC.
	inline heif_spec simple_heic(std::vector<bytes> extra = {})
	{
		heif_spec s;
		s.properties = {ispe(640, 480), hvcc(1)};
		std::vector<std::pair<unsigned, bool>> props = {{1, false}, {2, true}};

		for (auto& p : extra)
		{
			s.properties.push_back(std::move(p));
			props.emplace_back(static_cast<unsigned>(s.properties.size()), false);
		}

		s.items.push_back(image_item(1, "hvc1", props));
		return s;
	}

	// Bits written least significant first, as a JPEG XL codestream holds them.
	struct bit_writer
	{
		bytes b;
		std::size_t bits = 0;

		bit_writer& put(const std::uint64_t value, const unsigned count)
		{
			for (unsigned i = 0; i < count; ++i, ++bits)
			{
				if (bits % 8 == 0) b.push_back(0);
				b.back() = static_cast<std::uint8_t>(b.back() | ((value >> i) & 1u) << (bits % 8));
			}

			return *this;
		}

		bit_writer& pad()
		{
			while (bits % 8) put(0, 1);
			return *this;
		}
	};

	// --- JPEG XL

	struct jxl_header
	{
		std::uint32_t width = 64;
		std::uint32_t height = 48;
		unsigned orientation = 1;
		bool grey = false;
		bool alpha = false;
		bool premultiplied = false;
		bool small = false;
		// Alpha channels with names of name_length bytes, in place of the one alpha says.
		unsigned named_channels = 0;
		unsigned name_length = 48;
	};

	// A codestream: its signature, size header, image metadata and transform data, then bytes that
	// stand for the frames, which a scan never reads.
	inline bytes codestream(const jxl_header& h)
	{
		bit_writer w;
		w.put(0xFF, 8).put(0x0A, 8);

		if (h.small)
		{
			w.put(1, 1).put(h.height / 8 - 1, 5).put(0, 3).put(h.width / 8 - 1, 5);
		}
		else
		{
			w.put(0, 1).put(1, 2).put(h.height - 1, 13).put(0, 3).put(1, 2).put(h.width - 1, 13);
		}

		// Not all default, with the extra fields: the orientation, and no size, preview or animation.
		w.put(0, 1).put(1, 1).put(h.orientation - 1, 3).put(0, 1).put(0, 1).put(0, 1);
		// Eight-bit integer samples, and the modular buffer flag.
		w.put(0, 1).put(0, 2).put(1, 1);

		if (h.named_channels > 0)
		{
			// Alpha, eight bits, full size, and a name of 48 to 1071 bytes.
			w.put(3, 2).put(h.named_channels - 1, 12);

			for (unsigned c = 0; c < h.named_channels; ++c)
			{
				w.put(0, 1).put(0, 2).put(0, 1).put(0, 2).put(0, 2).put(3, 2).put(h.name_length - 48, 10);
				for (unsigned i = 0; i < h.name_length; ++i) w.put('a' + i % 26, 8);
				w.put(0, 1);
			}
		}
		else if (h.alpha)
		{
			// One extra channel: alpha, eight bits, full size, unnamed.
			w.put(1, 2).put(0, 1).put(0, 2).put(0, 1).put(0, 2).put(0, 2).put(0, 2).put(h.premultiplied ? 1 : 0, 1);
		}
		else
		{
			w.put(0, 2);
		}

		w.put(1, 1);

		if (h.grey)
		{
			// Grey, D65, the sRGB transfer function, relative intent.
			w.put(0, 1).put(0, 1).put(1, 2).put(1, 2).put(0, 1).put(2, 2).put(13 - 2, 4).put(1, 2);
		}
		else
		{
			w.put(1, 1);
		}

		// Default tone mapping, no extensions, default transform data.
		w.put(1, 1).put(0, 2).put(1, 1).pad();
		w.b.insert(w.b.end(), 48, 0x5A);
		return w.b;
	}

	inline const bytes jxl_signature = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};

	inline bytes jxl_ftyp()
	{
		return iso_box("ftyp", builder().raw("jxl ").be32(0).raw("jxl ").b);
	}

	inline bytes jxlp(const std::uint32_t index, const bool last, const std::span<const std::uint8_t> part)
	{
		return iso_box("jxlp",
		               builder().be32(index | (last ? 0x80000000u : 0u)).raw(bytes(part.begin(), part.end())).b);
	}

	// A stand-in for Brotli: "Z" then the bytes as they are.
	inline bytes brob(const std::string_view type, const bytes& contents)
	{
		return iso_box("brob", builder().raw(type).raw("Z").raw(contents).b);
	}

	inline std::optional<bytes> fake_brotli(const std::span<const std::uint8_t> compressed, std::string_view,
	                                        const std::size_t max_size)
	{
		if (compressed.empty() || compressed[0] != 'Z' || compressed.size() - 1 > max_size) return {};
		return bytes(compressed.begin() + 1, compressed.end());
	}

	inline libfiles::scan_limits with_brotli()
	{
		libfiles::scan_limits limits;
		limits.brotli = fake_brotli;
		return limits;
	}

	inline bytes container(const std::initializer_list<bytes> boxes)
	{
		builder file;
		file.raw(jxl_signature).raw(jxl_ftyp());
		for (const auto& box : boxes) file.raw(box);
		return file.b;
	}
}
