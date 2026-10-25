// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for the HEIF and JPEG XL scanners and their packet reads. Each file is built here box
// by box and, for JPEG XL, header bit by header bit, so every field a scanner reports is one the test
// put there; every file a test scans is also cut short at every byte near its ends, which must never
// throw or read past the end.

#include "test.h"

#include "builders.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace test_build;

namespace
{
	class counting_stream final : public libfiles::stream
	{
	public:
		explicit counting_stream(bytes data) : _data(std::move(data))
		{
		}

		std::uint64_t size() override { return _data.size(); }

		std::size_t read(const std::uint64_t offset, void* data, const std::size_t size) override
		{
			++reads;
			if (offset >= _data.size()) return 0;
			const auto n = std::min<std::size_t>(size, _data.size() - static_cast<std::size_t>(offset));
			std::memcpy(data, _data.data() + offset, n);
			return n;
		}

		int reads = 0;

	private:
		bytes _data;
	};

	// Every cut of a file must scan and read its packet without throwing, and a scan that fails must
	// report nothing but the format. A large file is cut at every byte near either end, where its
	// boxes are, and at a stride through the media data between.
	void sweep_truncations(const bytes& file, const libfiles::scan_limits& limits)
	{
		constexpr std::size_t ends = 4096;

		for (std::size_t cut = 0; cut < file.size(); cut += cut < ends || file.size() - cut <= ends ? 1 : 4093)
		{
			const bytes part(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut));
			libfiles::reader r(part);
			const auto index = libfiles::scan(r, libfiles::scan_intent::index, limits);
			const auto inspect = libfiles::scan(r, libfiles::scan_intent::inspect, limits);
			const auto packet = libfiles::read_packet(r);

			if (!index.error.empty() && (index.success || index.width != 0 || !index.metadata.exif.empty()))
			{
				CHECK(!"a scan that failed reported more than its format");
			}

			if (index.success != inspect.success || index.width != inspect.width) CHECK(!"inspect and index disagree");
			if (packet.readable && cut < 16) CHECK(!"a packet was read from a file too short to identify");
		}
	}

	// Every file a test scans is also swept, once for each way it is scanned.
	libfiles::scan_result scan_bytes(const bytes& file, const libfiles::scan_intent intent = libfiles::scan_intent::index,
	                                 const libfiles::scan_limits& limits = {})
	{
		// Keyed by a string: GCC 13, optimizing a three-way comparison of byte vectors, warns of a memcmp
		// past any object.
		static std::set<std::pair<std::string, bool>> swept;
		if (swept.emplace(std::string(file.begin(), file.end()), static_cast<bool>(limits.brotli)).second) sweep_truncations(file, limits);

		libfiles::reader r(file);
		return libfiles::scan(r, intent, limits);
	}

	libfiles::packet_reading packet_of(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::read_packet(r);
	}

	const libfiles::structure_row* find_row(const std::vector<libfiles::structure_row>& rows, const std::string_view key)
	{
		const auto it = std::ranges::find(rows, key, &libfiles::structure_row::key);
		return it == rows.end() ? nullptr : &*it;
	}

	bool has_row_named(const std::vector<libfiles::structure_row>& rows, const std::string_view key)
	{
		return find_row(rows, key) != nullptr;
	}

	std::string row_value(const std::vector<libfiles::structure_row>& rows, const std::string_view key)
	{
		const auto* const row = find_row(rows, key);
		return row ? row->value : std::string("(none)");
	}

	// A big-endian TIFF stream with one IFD0 orientation entry.
	bytes tiff_with_orientation(const unsigned orientation)
	{
		return builder().raw("MM").be16(42).be32(8).be16(1).be16(0x0112).be16(3).be32(1).be16(orientation).be16(0).be32(0).zeros(8).b;
	}

	// --- HEIF

	bytes ispe(const std::uint32_t width, const std::uint32_t height)
	{
		return iso_full_box("ispe", 0, 0, builder().be32(width).be32(height).b);
	}

	bytes irot(const unsigned degrees) { return iso_box("irot", builder().u8(degrees / 90).b); }
	bytes imir(const unsigned axis) { return iso_box("imir", builder().u8(axis).b); }

	bytes clap(const std::uint32_t width, const std::uint32_t height)
	{
		return iso_box("clap", builder().be32(width).be32(1).be32(height).be32(1).be32(0).be32(1).be32(0).be32(1).b);
	}

	bytes hvcc(const unsigned chroma, const unsigned luma_bits = 8, const unsigned chroma_bits = 8)
	{
		return iso_box("hvcC", builder().u8(1).u8(1).be32(0x60000000).zeros(6).u8(90).be16(0xF000).u8(0xFC)
		                                .u8(0xFC | chroma).u8(0xF8 | (luma_bits - 8)).u8(0xF8 | (chroma_bits - 8))
		                                .be16(0).u8(0x0F).u8(0).b);
	}

	bytes av1c(const bool monochrome, const unsigned subsampling_x, const unsigned subsampling_y, const unsigned bits)
	{
		const auto high = bits > 8 ? 1u : 0u;
		const auto twelve = bits == 12 ? 1u : 0u;
		return iso_box("av1C", builder().u8(0x81).u8(bits == 12 ? 0x40 : 0).u8(high << 6 | twelve << 5 | (monochrome ? 1u : 0u) << 4 |
		                                                                        subsampling_x << 3 | subsampling_y << 2).u8(0).b);
	}

	bytes colr_nclx(const unsigned primaries, const unsigned transfer, const unsigned matrix, const bool full_range)
	{
		return iso_box("colr", builder().raw("nclx").be16(primaries).be16(transfer).be16(matrix).u8(full_range ? 0x80 : 0).b);
	}

	bytes colr_icc(const std::string_view type, const std::string_view profile)
	{
		return iso_box("colr", builder().raw(type).raw(profile).b);
	}

	bytes pasp(const std::uint32_t h, const std::uint32_t v) { return iso_box("pasp", builder().be32(h).be32(v).b); }

	bytes auxc(const std::string_view urn)
	{
		return iso_full_box("auxC", 0, 0, builder().raw(urn).u8(0).b);
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
		bool mdat_first = false;
		bool large_meta_header = false;
		bool mdat_to_end = false;
	};

	bytes make_meta(const heif_spec& s, const std::uint32_t mdat_payload)
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

		// iloc version 1: four-byte offsets, lengths and base offsets, and a construction method.
		builder iloc;
		iloc.be16(0x4440).be16(static_cast<unsigned>(s.items.size()));
		std::uint32_t mdat_at = 0;
		std::uint32_t idat_at = 0;
		bytes idat;

		for (const auto& item : s.items)
		{
			iloc.be16(item.id).be16(item.in_idat ? 1 : 0).be16(0);
			iloc.be32(item.in_idat ? 0 : mdat_payload);
			iloc.be16(static_cast<unsigned>(item.extents));

			auto& at = item.in_idat ? idat_at : mdat_at;
			const auto size = static_cast<std::uint32_t>(item.data.size());

			for (std::size_t e = 0; e < item.extents; ++e)
			{
				const auto begin = static_cast<std::uint32_t>(size * e / item.extents);
				const auto end = static_cast<std::uint32_t>(size * (e + 1) / item.extents);
				iloc.be32(at + begin).be32(end - begin);
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
		meta.raw(iso_full_box("iloc", 1, 0, iloc.b));
		meta.raw(iso_box("iprp", builder().raw(iso_box("ipco", ipco.b)).raw(iso_full_box("ipma", 0, 0, ipma.b)).b));
		if (!s.references.empty()) meta.raw(iso_full_box("iref", 0, 0, iref.b));
		if (!idat.empty()) meta.raw(iso_box("idat", idat));
		for (const auto& box : s.meta_boxes) meta.raw(box);

		const auto payload = builder().be32(0).raw(meta.b).b;

		if (s.large_meta_header)
		{
			return builder().be32(1).raw("meta").be32(0).be32(static_cast<std::uint32_t>(payload.size() + 16)).raw(payload).b;
		}

		return iso_box("meta", payload);
	}

	bytes make_heif(const heif_spec& s)
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

		const auto mdat = s.mdat_to_end ? builder().be32(0).raw("mdat").raw(mdat_payload).b : iso_box("mdat", mdat_payload);

		// The meta box's size does not depend on the offsets it holds, so it is built twice.
		const auto probe = make_meta(s, 0);
		const auto mdat_payload_at = static_cast<std::uint32_t>(ftyp.size() + s.before_meta.size() + (s.mdat_first ? 0 : probe.size()) + 8);
		const auto meta = make_meta(s, mdat_payload_at);

		builder file;
		file.raw(ftyp).raw(s.before_meta);
		if (s.mdat_first) file.raw(mdat).raw(meta);
		else file.raw(meta).raw(mdat);
		return file.b;
	}

	heif_item image_item(const std::uint32_t id, const std::string_view type, std::vector<std::pair<unsigned, bool>> properties)
	{
		heif_item item;
		item.id = id;
		item.type = type;
		item.data = bytes(32, 0x55);
		item.properties = std::move(properties);
		return item;
	}

	heif_item exif_item(const std::uint32_t id, const unsigned orientation)
	{
		heif_item item;
		item.id = id;
		item.type = "Exif";
		item.data = builder().be32(6).raw(lit("Exif\0\0")).raw(tiff_with_orientation(orientation)).b;
		return item;
	}

	heif_item mime_item(const std::uint32_t id, const std::string_view content_type, const std::string_view text)
	{
		heif_item item;
		item.id = id;
		item.type = "mime";
		item.content_type = content_type;
		item.data = to_bytes(text);
		return item;
	}

	// A HEVC still: one image of 640 x 480, 4:2:0, with the given extra properties after ispe and hvcC.
	heif_spec simple_heic(std::vector<bytes> extra = {})
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
}

TEST("heif scan reads the primary image's size, pixel format and metadata")
{
	auto s = simple_heic({colr_icc("prof", "an icc profile")});
	s.items.push_back(exif_item(2, 6));
	s.items.push_back(mime_item(3, "application/rdf+xml", "<x:xmpmeta/>"));
	s.references.push_back({"cdsc", 2, {1}});
	s.references.push_back({"cdsc", 3, {1}});
	const auto file = make_heif(s);

	const auto result = scan_bytes(file);

	CHECK(result.success);
	CHECK(result.error.empty());
	CHECK(result.format == libfiles::image_format::heif);
	CHECK_EQ(result.width, 640u);
	CHECK_EQ(result.height, 480u);
	CHECK_STR(result.pixel_format, "yuv420");
	CHECK(result.metadata.exif == tiff_with_orientation(6));
	CHECK(result.orientation == 6);
	CHECK(!result.container_transform);
	CHECK_STR(to_string(result.metadata.xmp), "<x:xmpmeta/>");
	CHECK_STR(to_string(result.metadata.icc), "an icc profile");
	CHECK(result.exif_thumbnail.empty());
	CHECK(result.structure.empty());

	const auto packet = packet_of(file);
	CHECK(packet.readable);
	CHECK_STR(packet.packet, "<x:xmpmeta/>");
}

TEST("heif rotation and mirroring turn the size and mark the transform")
{
	const auto rotated = scan_bytes(make_heif(simple_heic({irot(90)})));
	CHECK_EQ(rotated.width, 480u);
	CHECK_EQ(rotated.height, 640u);
	CHECK(rotated.container_transform);

	const auto turned = scan_bytes(make_heif(simple_heic({irot(270)})));
	CHECK_EQ(turned.width, 480u);
	CHECK(turned.container_transform);

	const auto upside_down = scan_bytes(make_heif(simple_heic({irot(180)})));
	CHECK_EQ(upside_down.width, 640u);
	CHECK(upside_down.container_transform);

	const auto mirrored = scan_bytes(make_heif(simple_heic({imir(1)})));
	CHECK_EQ(mirrored.width, 640u);
	CHECK_EQ(mirrored.height, 480u);
	CHECK(mirrored.container_transform);

	// The EXIF orientation is still reported; the flag says it describes the turn already made.
	auto s = simple_heic({irot(90)});
	s.items.push_back(exif_item(2, 6));
	s.references.push_back({"cdsc", 2, {1}});
	const auto both = scan_bytes(make_heif(s));
	CHECK(both.orientation == 6);
	CHECK(both.container_transform);
}

TEST("heif clean aperture crops before the rotation turns it")
{
	const auto cropped = scan_bytes(make_heif(simple_heic({clap(600, 400)})), libfiles::scan_intent::inspect);
	CHECK_EQ(cropped.width, 600u);
	CHECK_EQ(cropped.height, 400u);
	CHECK(!cropped.container_transform);
	CHECK_STR(row_value(cropped.structure, "Stored size"), "640 x 480");
	CHECK_STR(row_value(cropped.structure, "Displayed size"), "600 x 400");

	const auto both = scan_bytes(make_heif(simple_heic({clap(600, 400), irot(90)})));
	CHECK_EQ(both.width, 400u);
	CHECK_EQ(both.height, 600u);

	// A clean aperture that leaves nothing refuses the file, as libheif does.
	CHECK(!scan_bytes(make_heif(simple_heic({clap(0, 400)}))).error.empty());
}

TEST("heif grid takes its tiles' format and colour, and its own size")
{
	heif_spec s;
	s.primary = 10;
	s.properties = {ispe(512, 256), hvcc(2, 10, 10), colr_nclx(9, 16, 9, false), ispe(1024, 256)};

	for (std::uint32_t tile = 1; tile <= 2; ++tile)
	{
		auto item = image_item(tile, "hvc1", {{1, false}, {2, true}, {3, false}});
		item.hidden = true;
		s.items.push_back(item);
	}

	heif_item grid;
	grid.id = 10;
	grid.type = "grid";
	grid.data = builder().u8(0).u8(0).u8(0).u8(1).be16(1024).be16(256).b;
	grid.in_idat = true;
	grid.properties = {{4, false}};
	s.items.push_back(grid);
	s.references.push_back({"dimg", 10, {1, 2}});

	const auto file = make_heif(s);
	const auto result = scan_bytes(file, libfiles::scan_intent::inspect);

	CHECK(result.success);
	CHECK_EQ(result.width, 1024u);
	CHECK_EQ(result.height, 256u);
	CHECK_STR(result.pixel_format, "yuv422");
	CHECK_STR(row_value(result.structure, "Top level images"), "1");
	CHECK_STR(row_value(result.structure, "Primary item id"), "10");
	CHECK_STR(row_value(result.structure, "Luma depth"), "10 bits");
	CHECK_STR(row_value(result.structure, "Colour profile"), "nclx (coded primaries and transfer)");

	// A grid that names fewer tiles than its rows and columns hold cannot be decoded.
	s.references = {{"dimg", 10, {1}}};
	CHECK(!scan_bytes(make_heif(s)).error.empty());
}

TEST("heif metadata: idat, several extents, the older XMP type, IPTC, and the last of each")
{
	auto s = simple_heic();

	auto exif = exif_item(2, 3);
	exif.in_idat = true;
	s.items.push_back(exif);

	auto first_xmp = mime_item(3, "application/rdf+xml", "<first/>");
	first_xmp.extents = 3;
	s.items.push_back(first_xmp);

	heif_item old_xmp;
	old_xmp.id = 4;
	old_xmp.type = std::string("XMP\0", 4);
	old_xmp.data = to_bytes("<older-xmp-type/>");
	old_xmp.extents = 2;
	s.items.push_back(old_xmp);

	heif_item iptc;
	iptc.id = 5;
	iptc.type = "iptc";
	iptc.data = to_bytes("iptc records");
	s.items.push_back(iptc);

	// Neither of these is XMP: another MIME type, and a compressed packet libheif skips.
	s.items.push_back(mime_item(6, "text/plain", "plain text"));
	auto compressed = mime_item(7, "application/rdf+xml", "deflated");
	compressed.content_encoding = "deflate";
	s.items.push_back(compressed);

	for (std::uint32_t id = 2; id <= 7; ++id) s.references.push_back({"cdsc", id, {1}});

	const auto file = make_heif(s);
	const auto result = scan_bytes(file, libfiles::scan_intent::inspect);

	CHECK(result.success);
	CHECK(result.metadata.exif == tiff_with_orientation(3));
	CHECK(result.orientation == 3);
	CHECK_STR(to_string(result.metadata.xmp), "<older-xmp-type/>");
	CHECK_STR(to_string(result.metadata.iptc), "iptc records");

	CHECK(has_row_named(result.structure, "Metadata items (5)"));
	CHECK_STR(row_value(result.structure, "Exif"), std::format("{} bytes, item 2", exif.data.size()));
	CHECK_STR(row_value(result.structure, "XMP"), "17 bytes, item 4");
	CHECK_STR(row_value(result.structure, "iptc"), "12 bytes, item 5");

	const auto packet = packet_of(file);
	CHECK(packet.readable);
	CHECK_STR(packet.packet, "<older-xmp-type/>");
}

TEST("heif colour profiles: prof, rICC and nclx with an identity matrix")
{
	const auto icc = scan_bytes(make_heif(simple_heic({colr_icc("rICC", "restricted")})), libfiles::scan_intent::inspect);
	CHECK_STR(to_string(icc.metadata.icc), "restricted");
	CHECK_STR(row_value(icc.structure, "Colour profile"), "ICC");

	// An identity matrix is decoded as RGB, which libheif names 4:4:4.
	const auto rgb = scan_bytes(make_heif(simple_heic({colr_nclx(1, 13, 0, true)})), libfiles::scan_intent::inspect);
	CHECK_STR(rgb.pixel_format, "yuv444");
	CHECK(rgb.metadata.icc.empty());
	CHECK_STR(row_value(rgb.structure, "Colour profile"), "nclx (coded primaries and transfer)");

	// Unspecified everything, full range, is libheif's own default: no profile at all.
	const auto none = scan_bytes(make_heif(simple_heic({colr_nclx(2, 2, 2, true)})), libfiles::scan_intent::inspect);
	CHECK_STR(row_value(none.structure, "Colour profile"), "none");

	// A colour description of a type libheif does not know refuses the file.
	CHECK(!scan_bytes(make_heif(simple_heic({iso_box("colr", to_bytes("what"))}))).error.empty());
}

TEST("heif auxiliary images: alpha with premultiplication, depth, and thumbnails")
{
	auto s = simple_heic({pasp(4, 3)});
	s.properties.push_back(auxc("urn:mpeg:hevc:2015:auxid:1"));
	s.properties.push_back(auxc("urn:mpeg:hevc:2015:auxid:2"));
	s.properties.push_back(hvcc(0));
	const auto alpha_index = static_cast<unsigned>(s.properties.size() - 2);
	const auto depth_index = alpha_index + 1;
	const auto mono_index = depth_index + 1;

	s.items.push_back(image_item(2, "hvc1", {{1, false}, {mono_index, true}, {alpha_index, false}}));
	s.items.push_back(image_item(3, "hvc1", {{1, false}, {mono_index, true}, {depth_index, false}}));
	s.items.push_back(image_item(4, "hvc1", {{1, false}, {2, true}}));
	s.items.push_back(image_item(5, "hvc1", {{1, false}, {2, true}}));
	s.references.push_back({"auxl", 2, {1}});
	s.references.push_back({"auxl", 3, {1}});
	s.references.push_back({"thmb", 4, {1}});
	s.references.push_back({"thmb", 5, {1}});
	s.references.push_back({"prem", 1, {2}});

	const auto file = make_heif(s);
	const auto result = scan_bytes(file, libfiles::scan_intent::inspect);

	CHECK(result.success);
	CHECK_STR(result.pixel_format, "yuv420");
	CHECK_STR(row_value(result.structure, "Alpha channel"), "present, premultiplied");
	CHECK_STR(row_value(result.structure, "Pixel aspect ratio"), "4:3");
	CHECK_STR(row_value(result.structure, "Thumbnails"), "2");
	CHECK_STR(row_value(result.structure, "Depth images"), "1");
	CHECK_STR(row_value(result.structure, "Auxiliary images"), "2");
	CHECK_STR(row_value(result.structure, "Top level images"), "1");

	// Thumbnails are counted, never located: libfiles does not decode HEVC.
	CHECK(result.jfif_thumbnail.empty() && result.exif_thumbnail.empty() && result.tiff_thumbnail.empty());

	// An auxiliary image that does not say what it is refuses the file.
	auto unnamed = s;
	unnamed.items[1].properties.pop_back();
	CHECK(!scan_bytes(make_heif(unnamed)).error.empty());
}

TEST("avif scan reads the AV1 configuration's chroma and depth")
{
	const auto avif = [](const bytes& config)
	{
		heif_spec s;
		s.major = "avif";
		s.brands = {"avif", "mif1", "miaf"};
		s.properties = {ispe(300, 200), config};
		s.items.push_back(image_item(1, "av01", {{1, false}, {2, true}}));
		return make_heif(s);
	};

	const auto mono = scan_bytes(avif(av1c(true, 1, 1, 8)), libfiles::scan_intent::inspect);
	CHECK_STR(mono.pixel_format, "grayscale");
	CHECK_STR(row_value(mono.structure, "Luma depth"), "8 bits");

	CHECK_STR(scan_bytes(avif(av1c(false, 1, 1, 10))).pixel_format, "yuv420");
	CHECK_STR(scan_bytes(avif(av1c(false, 1, 0, 8))).pixel_format, "yuv422");
	CHECK_STR(scan_bytes(avif(av1c(false, 0, 0, 12))).pixel_format, "yuv444");
	CHECK_STR(row_value(scan_bytes(avif(av1c(false, 0, 0, 12)), libfiles::scan_intent::inspect).structure, "Luma depth"), "12 bits");

	// Subsampled vertically alone is no chroma format libheif knows, which Diffractor named RGB.
	CHECK_STR(scan_bytes(avif(av1c(false, 0, 1, 8))).pixel_format, "rgb");
}

TEST("heif files libheif refuses are refused")
{
	// The primary item hidden, missing, or not an image.
	auto hidden = simple_heic();
	hidden.items[0].hidden = true;
	CHECK(!scan_bytes(make_heif(hidden)).error.empty());

	auto missing = simple_heic();
	missing.primary = 9;
	CHECK(!scan_bytes(make_heif(missing)).error.empty());

	// A HEVC image without its configuration cannot be decoded.
	heif_spec no_config;
	no_config.properties = {ispe(64, 64)};
	no_config.items.push_back(image_item(1, "hvc1", {{1, false}}));
	CHECK(!scan_bytes(make_heif(no_config)).error.empty());

	// An essential property libheif cannot read.
	auto essential = simple_heic({iso_box("zzzz", to_bytes("unknown"))});
	essential.items[0].properties.back().second = true;
	CHECK(!scan_bytes(make_heif(essential)).error.empty());

	auto optional = simple_heic({iso_box("zzzz", to_bytes("unknown"))});
	CHECK(scan_bytes(make_heif(optional)).success);

	// A zero image size, a thumbnail of nothing, and no brand libheif reads.
	heif_spec zero;
	zero.properties = {ispe(0, 64), hvcc(1)};
	zero.items.push_back(image_item(1, "hvc1", {{1, false}, {2, true}}));
	CHECK(!scan_bytes(make_heif(zero)).error.empty());

	auto orphan = simple_heic();
	orphan.items.push_back(image_item(2, "hvc1", {{1, false}, {2, true}}));
	orphan.references.push_back({"thmb", 2, {7}});
	CHECK(!scan_bytes(make_heif(orphan)).error.empty());

	auto brands = simple_heic();
	brands.brands = {"abcd"};
	const auto unread = scan_bytes(make_heif(brands));
	CHECK(!unread.error.empty());
	CHECK(unread.format == libfiles::image_format::heif);
	CHECK(!unread.success);
	CHECK(!packet_of(make_heif(brands)).readable);

	// An image without a size reads, but not as a success.
	heif_spec sizeless;
	sizeless.properties = {hvcc(1)};
	sizeless.items.push_back(image_item(1, "hvc1", {{1, true}}));
	const auto no_size = scan_bytes(make_heif(sizeless));
	CHECK(no_size.error.empty());
	CHECK(!no_size.success);
	CHECK_STR(no_size.pixel_format, "yuv420");

	// Unless it has a camera model, which needs one; a model libheif cannot parse is passed over.
	const auto camera = [](const bytes& cmin)
	{
		heif_spec s;
		s.properties = {hvcc(1), cmin};
		s.items.push_back(image_item(1, "hvc1", {{1, true}, {2, false}}));
		return make_heif(s);
	};

	CHECK(!scan_bytes(camera(iso_full_box("cmin", 0, 0, builder().be32(1).be32(2).be32(3).b))).error.empty());
	CHECK(scan_bytes(camera(iso_full_box("cmin", 0, 0, builder().be32(1).b))).error.empty());

	// An image that requires a reference type libheif does not process cannot be decoded.
	const auto required = [](const std::vector<std::string_view>& types)
	{
		builder b;
		b.u8(static_cast<unsigned>(types.size()));
		for (const auto type : types) b.raw(type);
		return iso_full_box("rref", 0, 0, b.b);
	};

	CHECK(scan_bytes(make_heif(simple_heic({required({"dimg", "cdsc"})}))).success);
	CHECK(!scan_bytes(make_heif(simple_heic({required({"dimg", "tbas"})}))).error.empty());

	// A scaling with a zero term, and a property cut short that libheif parses only to keep.
	CHECK(!scan_bytes(make_heif(simple_heic({iso_full_box("iscl", 0, 0, builder().be16(1).be16(0).be16(1).be16(1).b)}))).error.empty());
	CHECK(scan_bytes(make_heif(simple_heic({iso_box("a1op", builder().u8(0).b)}))).success);
	CHECK(!scan_bytes(make_heif(simple_heic({iso_box("a1op", {})}))).error.empty());
}

TEST("heif boxes: a 64-bit meta size, a media box to the end, the meta box last")
{
	auto large = simple_heic();
	large.large_meta_header = true;
	CHECK(scan_bytes(make_heif(large)).success);

	auto to_end = simple_heic();
	to_end.items.push_back(exif_item(2, 8));
	to_end.references.push_back({"cdsc", 2, {1}});
	to_end.mdat_to_end = true;
	const auto open_ended = scan_bytes(make_heif(to_end));
	CHECK(open_ended.success);
	CHECK(open_ended.orientation == 8);

	auto last = simple_heic();
	last.items.push_back(exif_item(2, 5));
	last.references.push_back({"cdsc", 2, {1}});
	last.mdat_first = true;
	const auto file = make_heif(last);
	const auto meta_last = scan_bytes(file);
	CHECK(meta_last.success);
	CHECK(meta_last.orientation == 5);
}

TEST("heif boxes the scan has no use for are checked as libheif checks them")
{
	const auto group = [](const std::string_view type, const std::vector<std::uint32_t>& entities)
	{
		builder b;
		b.be32(1).be32(static_cast<std::uint32_t>(entities.size()));
		for (const auto id : entities) b.be32(id);
		return iso_full_box(type, 0, 0, b.b);
	};

	const auto dref = [](const std::uint32_t count)
	{
		return iso_box("dinf", iso_full_box("dref", 0, 0, builder().be32(count).raw(iso_full_box("url ", 0, 1, {})).b));
	};

	auto well_formed = simple_heic();
	well_formed.meta_boxes = {iso_box("grpl", builder().raw(group("altr", {1, 2})).raw(group("ster", {1, 2})).b), dref(1)};
	const auto file = make_heif(well_formed);
	CHECK(scan_bytes(file).success);

	// An entity group that claims more entities than it holds, a stereo pair of one image, and a data
	// reference box declaring more boxes than it holds each refuse the file.
	auto short_group = simple_heic();
	short_group.meta_boxes = {iso_box("grpl", iso_full_box("altr", 0, 0, builder().be32(1).be32(5).be32(1).b))};
	CHECK(!scan_bytes(make_heif(short_group)).error.empty());

	auto lone_pair = simple_heic();
	lone_pair.meta_boxes = {iso_box("grpl", group("ster", {1}))};
	CHECK(!scan_bytes(make_heif(lone_pair)).error.empty());

	// Though a pyramid group libheif cannot parse is passed over.
	auto pyramid = simple_heic();
	pyramid.meta_boxes = {iso_box("grpl", iso_full_box("pymd", 0, 0, builder().be32(1).be32(5).be32(1).b))};
	CHECK(scan_bytes(make_heif(pyramid)).success);

	auto missing_url = simple_heic();
	missing_url.meta_boxes = {dref(2)};
	CHECK(!scan_bytes(make_heif(missing_url)).error.empty());

	// libheif follows boxes twenty deep from the meta box, and no deeper.
	const auto nested = [](const int depth)
	{
		auto box = iso_box("free", {});
		for (int i = 1; i < depth; ++i) box = iso_box("dinf", box);
		auto s = simple_heic();
		s.meta_boxes = {box};
		return make_heif(s);
	};

	CHECK(scan_bytes(nested(20)).success);
	CHECK(!scan_bytes(nested(21)).error.empty());

	// Every meta box is parsed, and the last one read: an earlier one libheif cannot parse refuses the
	// file, and one it can is passed over.
	auto earlier = simple_heic();
	earlier.before_meta = iso_full_box("meta", 0, 0, iso_full_box("hdlr", 0, 0, builder().be32(0).raw("none").zeros(12).u8(0).b));
	CHECK(scan_bytes(make_heif(earlier)).success);
	earlier.before_meta = iso_full_box("meta", 1, 0, {});
	CHECK(!scan_bytes(make_heif(earlier)).error.empty());

	// A top-level box's 64-bit size past libheif's limit refuses the file, even after the meta box.
	const auto with_box = [&file](const std::uint32_t high)
	{
		return builder().raw(file).be32(1).raw("free").be32(high).be32(0).zeros(16).b;
	};

	CHECK(scan_bytes(with_box(0x0FFFFFFF)).success);
	CHECK(!scan_bytes(with_box(0x10000000)).error.empty());
}

TEST("heif over a stream reads the boxes and the metadata, never the image data")
{
	auto s = simple_heic();
	s.items[0].data = bytes(300000, 0x11);
	s.items.push_back(mime_item(2, "application/rdf+xml", "<x/>"));
	s.references.push_back({"cdsc", 2, {1}});
	s.mdat_first = true;
	const auto file = make_heif(s);

	counting_stream stream(file);
	libfiles::reader r(stream);
	const auto result = libfiles::scan(r);

	CHECK(result.success);
	CHECK_STR(to_string(result.metadata.xmp), "<x/>");
	CHECK(r.source_bytes() < file.size() / 2);
	CHECK(stream.reads <= 3);

	// The same from memory, which also cuts the file short.
	CHECK_STR(to_string(scan_bytes(file).metadata.xmp), "<x/>");
}

namespace
{
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
	bytes codestream(const jxl_header& h)
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

	const bytes jxl_signature = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};

	bytes jxl_ftyp() { return iso_box("ftyp", builder().raw("jxl ").be32(0).raw("jxl ").b); }

	bytes jxlp(const std::uint32_t index, const bool last, const std::span<const std::uint8_t> part)
	{
		return iso_box("jxlp", builder().be32(index | (last ? 0x80000000u : 0u)).raw(bytes(part.begin(), part.end())).b);
	}

	bytes exif_box(const unsigned orientation)
	{
		return iso_box("Exif", builder().be32(0).raw(tiff_with_orientation(orientation)).b);
	}

	// A stand-in for Brotli: "Z" then the bytes as they are.
	bytes brob(const std::string_view type, const bytes& contents)
	{
		return iso_box("brob", builder().raw(type).raw("Z").raw(contents).b);
	}

	std::optional<bytes> fake_brotli(const std::span<const std::uint8_t> compressed, std::string_view, const std::size_t max_size)
	{
		if (compressed.empty() || compressed[0] != 'Z' || compressed.size() - 1 > max_size) return {};
		return bytes(compressed.begin() + 1, compressed.end());
	}

	libfiles::scan_limits with_brotli()
	{
		libfiles::scan_limits limits;
		limits.brotli = fake_brotli;
		return limits;
	}

	bytes container(const std::initializer_list<bytes> boxes)
	{
		builder file;
		file.raw(jxl_signature).raw(jxl_ftyp());
		for (const auto& box : boxes) file.raw(box);
		return file.b;
	}
}

TEST("jxl bare codestream gives its size, orientation and channels")
{
	const auto rgb = scan_bytes(codestream({.width = 640, .height = 480, .orientation = 6}));
	CHECK(rgb.success);
	CHECK(rgb.error.empty());
	CHECK(rgb.format == libfiles::image_format::jxl);
	CHECK_EQ(rgb.width, 640u);
	CHECK_EQ(rgb.height, 480u);
	CHECK(rgb.orientation == 6);
	CHECK(!rgb.container_transform);
	CHECK_STR(rgb.pixel_format, "rgb");

	CHECK_STR(scan_bytes(codestream({.grey = true})).pixel_format, "grayscale");
	CHECK_STR(scan_bytes(codestream({.grey = true, .alpha = true})).pixel_format, "grayscale");
	CHECK_STR(scan_bytes(codestream({.alpha = true})).pixel_format, "rgba");
	CHECK_STR(scan_bytes(codestream({.alpha = true, .premultiplied = true})).pixel_format, "rgba");

	const auto small = scan_bytes(codestream({.width = 256, .height = 128, .small = true}));
	CHECK_EQ(small.width, 256u);
	CHECK_EQ(small.height, 128u);

	// A bare codestream has nowhere to keep a packet, so it has none.
	const auto packet = packet_of(codestream({}));
	CHECK(packet.readable);
	CHECK(packet.packet.empty());
}

TEST("jxl container gives its metadata boxes, and the EXIF orientation wins")
{
	const auto file = container({exif_box(8), iso_box("xml ", to_bytes("<x:xmpmeta/>")), iso_box("jxlc", codestream({.orientation = 3}))});
	const auto result = scan_bytes(file, libfiles::scan_intent::inspect);

	CHECK(result.success);
	CHECK_EQ(result.width, 64u);
	CHECK(result.metadata.exif == tiff_with_orientation(8));
	CHECK(result.orientation == 8);
	CHECK_STR(to_string(result.metadata.xmp), "<x:xmpmeta/>");
	CHECK(has_row_named(result.structure, "Boxes (5)"));
	CHECK_STR(row_value(result.structure, "Orientation"), "3");

	const auto packet = packet_of(file);
	CHECK(packet.readable);
	CHECK_STR(packet.packet, "<x:xmpmeta/>");
}

TEST("jxl headers split across partial codestream boxes read whole")
{
	const auto stream = codestream({.width = 320, .height = 200, .alpha = true});
	const std::span<const std::uint8_t> all(stream);

	// The headers cut after five bytes, with the metadata boxes between the two parts.
	const auto file = container({jxlp(0, false, all.first(5)), exif_box(1), iso_box("xml ", to_bytes("<p/>")), jxlp(1, true, all.subspan(5))});
	const auto result = scan_bytes(file);

	CHECK(result.success);
	CHECK_EQ(result.width, 320u);
	CHECK_EQ(result.height, 200u);
	CHECK_STR(result.pixel_format, "rgba");
	CHECK(result.orientation == 1);
	CHECK_STR(to_string(result.metadata.xmp), "<p/>");

	// A part out of order is a fault in a version 0 file: the boxes before it stand.
	const auto disordered = scan_bytes(container({exif_box(2), jxlp(1, true, all.subspan(5)), jxlp(0, false, all.first(5))}));
	CHECK(!disordered.success);
	CHECK(disordered.orientation == 2);
}

TEST("jxl headers cut into a part for every byte read as they do whole")
{
	// Named channels make headers of more than a kilobyte, which libjxl reads again as each part
	// arrives.
	const auto stream = codestream({.width = 200, .height = 100, .named_channels = 8, .name_length = 160});
	const std::span<const std::uint8_t> all(stream);
	CHECK_STR(scan_bytes(stream).pixel_format, "rgba");

	// Each byte in a part of its own, and an empty part before each, which gives the headers
	// nothing new to read. The file is too large to cut at every byte.
	builder file;
	file.raw(jxl_signature).raw(jxl_ftyp());
	std::uint32_t index = 0;

	for (std::size_t i = 0; i < all.size(); ++i)
	{
		file.raw(jxlp(index++, false, {}));
		file.raw(jxlp(index++, i + 1 == all.size(), all.subspan(i, 1)));
	}

	file.raw(iso_box("xml ", to_bytes("<after/>")));

	libfiles::reader r(file.b);
	const auto split = libfiles::scan(r);
	CHECK(split.success);
	CHECK_EQ(split.width, 200u);
	CHECK_EQ(split.height, 100u);
	CHECK_STR(split.pixel_format, "rgba");
	CHECK_STR(to_string(split.metadata.xmp), "<after/>");
}

TEST("jxl compressed boxes need a decompressor, and one that fails stops the boxes")
{
	const auto file = container({iso_box("jxlc", codestream({})), brob("Exif", builder().be32(0).raw(tiff_with_orientation(5)).b),
	                             brob("xml ", to_bytes("<compressed/>"))});

	// Without one, the compressed boxes are left out.
	const auto plain = scan_bytes(file);
	CHECK(plain.success);
	CHECK(plain.metadata.exif.empty());
	CHECK(plain.metadata.xmp.empty());

	const auto decompressed = scan_bytes(file, libfiles::scan_intent::index, with_brotli());
	CHECK(decompressed.orientation == 5);
	CHECK_STR(to_string(decompressed.metadata.xmp), "<compressed/>");

	// A compressed packet is not one read_packet can read.
	CHECK(!packet_of(file).readable);

	// A box the decompressor refuses ends the boxes, as libjxl ends at it.
	const auto broken = container({iso_box("jxlc", codestream({})), iso_box("xml ", to_bytes("<before/>")),
	                               iso_box("brob", builder().raw("Exif").raw("not brotli").b), iso_box("xml ", to_bytes("<after/>"))});
	const auto stopped = scan_bytes(broken, libfiles::scan_intent::index, with_brotli());
	CHECK(stopped.success);
	CHECK_STR(to_string(stopped.metadata.xmp), "<before/>");
}

TEST("jxl compressed boxes decompress to a bounded total, however many there are")
{
	// A thousand tiny boxes, each of which the decompressor below expands to all it is allowed, as a
	// stream of a few bytes can: what a hostile file's boxes cost at most. A late plain packet is
	// never reached.
	builder file;
	file.raw(jxl_signature).raw(jxl_ftyp()).raw(iso_box("jxlc", codestream({.width = 120, .height = 90})));
	for (int i = 0; i < 1000; ++i) file.raw(brob(i % 2 ? "xml " : "Exif", to_bytes("tiny")));
	file.raw(iso_box("xml ", to_bytes("<late/>")));

	std::vector<std::size_t> asked;
	std::uint64_t returned = 0;

	const auto scan_expanding = [&](const std::function<std::size_t(std::size_t)>& expand)
	{
		asked.clear();
		returned = 0;

		libfiles::scan_limits limits;
		limits.brotli = [&](std::span<const std::uint8_t>, std::string_view, const std::size_t max_size) -> std::optional<bytes>
		{
			asked.push_back(max_size);
			const auto size = expand(max_size);
			if (size > max_size) return {};
			returned += size;
			return bytes(size);
		};

		// Too costly a file to cut at every byte, so it is scanned once.
		libfiles::reader r(file.b);
		return libfiles::scan(r, libfiles::scan_intent::index, limits);
	};

	constexpr std::size_t mib = 1024u * 1024u;
	constexpr auto budget = libfiles::detail::max_jxl_decompressed;

	// Boxes that fill all they are allowed spend the scan's decompression in two, and the boxes stop
	// at the third as at one too large to keep. The codestream before them is read all the same.
	const auto full = scan_expanding([](const std::size_t max_size) { return max_size; });
	CHECK(full.success);
	CHECK_EQ(full.width, 120u);
	CHECK_EQ(full.height, 90u);
	CHECK_EQ(asked.size(), 2u);
	CHECK_EQ(returned, budget);
	CHECK_EQ(full.metadata.xmp.size(), 16 * mib);

	// Each box is offered what is left, a block at most: here 16, 16, 12 and then 2 MiB.
	const auto partial = scan_expanding([](const std::size_t max_size) { return std::min(max_size, 10 * mib); });
	CHECK(partial.success);
	CHECK(asked == std::vector<std::size_t>({16 * mib, 16 * mib, 12 * mib, 2 * mib}));
	CHECK_EQ(returned, budget);
	CHECK_EQ(partial.metadata.xmp.size(), 2 * mib);
}

TEST("jxl partial codestream boxes are not kept, however many there are")
{
	// The signature in the first part, a hundred thousand empty parts, and then the rest of the
	// codestream: every empty part is a box the read passes without keeping anything of it. The
	// file is too large to cut at every byte.
	const auto stream = codestream({.width = 150, .height = 75});
	const std::span<const std::uint8_t> all(stream);
	constexpr std::uint32_t empty_parts = 100000;

	builder file;
	file.raw(jxl_signature).raw(jxl_ftyp()).raw(jxlp(0, false, all.first(2)));
	for (std::uint32_t i = 1; i <= empty_parts; ++i) file.be32(12).raw("jxlp").be32(i);
	const auto unfinished = file.b;
	file.raw(jxlp(empty_parts + 1, true, all.subspan(2)));

	counting_stream source(file.b);
	libfiles::reader r(source);
	const auto result = libfiles::scan(r);
	CHECK(result.success);
	CHECK_EQ(result.width, 150u);
	CHECK_EQ(result.height, 75u);

	// One pass over the file, a read for each block, and never more than one part held at once.
	CHECK(source.reads <= static_cast<int>(file.b.size() / libfiles::reader::default_block_size) + 4);

	libfiles::reader again(file.b);
	CHECK(libfiles::detail::jxl_parts_held(again) <= 1u);

	// Without the rest the codestream never finishes, and nothing piles up while it waits.
	libfiles::reader cut(unfinished);
	CHECK(!libfiles::scan(cut).success);

	libfiles::reader cut_again(unfinished);
	CHECK(libfiles::detail::jxl_parts_held(cut_again) <= 1u);
}

TEST("jxl boxes count only once the decoder moves past them")
{
	// The last packet wins, but only a box followed by a whole box or the end of the file counts.
	const auto both = scan_bytes(container({iso_box("jxlc", codestream({})), iso_box("xml ", to_bytes("<one/>")),
	                                        iso_box("xml ", to_bytes("<two/>"))}));
	CHECK_STR(to_string(both.metadata.xmp), "<two/>");

	auto trailing = container({iso_box("jxlc", codestream({})), iso_box("xml ", to_bytes("<one/>")), iso_box("xml ", to_bytes("<two/>"))});
	trailing.insert(trailing.end(), {1, 2, 3});
	CHECK_STR(to_string(scan_bytes(trailing).metadata.xmp), "<one/>");

	// A container whose codestream never comes keeps nothing of its last box.
	const auto no_codestream = scan_bytes(container({iso_box("xml ", to_bytes("<one/>")), iso_box("xml ", to_bytes("<two/>"))}));
	CHECK(!no_codestream.success);
	CHECK(no_codestream.error.empty());
	CHECK_STR(to_string(no_codestream.metadata.xmp), "<one/>");

	// A box of size zero runs to the end of the file.
	auto open = container({iso_box("jxlc", codestream({}))});
	builder last;
	last.be32(0).raw("xml ").raw("<open/>");
	open.insert(open.end(), last.b.begin(), last.b.end());
	CHECK_STR(to_string(scan_bytes(open).metadata.xmp), "<open/>");

	// Unless the codestream never came: libjxl fails at the end of the file, and the box is lost.
	auto never = container({iso_box("xml ", to_bytes("<one/>"))});
	never.insert(never.end(), last.b.begin(), last.b.end());
	const auto unfinished = scan_bytes(never);
	CHECK(!unfinished.success);
	CHECK_STR(to_string(unfinished.metadata.xmp), "<one/>");

	// So does a codestream box: nothing after it is a box.
	const auto open_stream = scan_bytes(container({iso_box("xml ", to_bytes("<x/>")), builder().be32(0).raw("jxlc").raw(codestream({})).b}));
	CHECK(open_stream.success);
	CHECK_STR(to_string(open_stream.metadata.xmp), "<x/>");

	// A 64-bit box size reads as any other.
	const auto stream = codestream({.width = 96});
	const auto wide = container({builder().be32(1).raw("jxlc").be32(0).be32(static_cast<std::uint32_t>(stream.size() + 16)).raw(stream).b});
	CHECK_EQ(scan_bytes(wide).width, 96u);
}

TEST("jxl files libjxl refuses are refused")
{
	auto bad_signature = container({iso_box("jxlc", codestream({}))});
	bad_signature[10] = 0;
	const auto refused = scan_bytes(bad_signature);
	CHECK(!refused.error.empty());
	CHECK(refused.format == libfiles::image_format::jxl);

	// A file type box anywhere but second ends the boxes, and a container without one has none.
	CHECK(!scan_bytes(container({jxl_ftyp(), iso_box("jxlc", codestream({}))})).success);
	CHECK(!scan_bytes(builder().raw(jxl_signature).raw(iso_box("jxlc", codestream({}))).b).success);

	// A header field libjxl rejects: an extra channel of a reserved type.
	bit_writer reserved;
	reserved.put(0xFF, 8).put(0x0A, 8).put(0, 1).put(1, 2).put(47, 13).put(0, 3).put(1, 2).put(63, 13);
	reserved.put(0, 1).put(1, 1).put(0, 3).put(0, 1).put(0, 1).put(0, 1).put(0, 1).put(0, 2).put(1, 1);
	reserved.put(1, 2).put(0, 1).put(2, 2).put(7 - 2, 4).pad();
	reserved.b.insert(reserved.b.end(), 32, 0);
	const auto rejected = scan_bytes(reserved.b);
	CHECK(rejected.error.empty());
	CHECK(!rejected.success);
	CHECK_EQ(rejected.width, 0u);
}
