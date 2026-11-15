// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Tests for writing a HEIF's metadata: its packet item and the EXIF item it holds, in files built box by
// box here in each layout the writer meets, and in libheif's own HEIC and AVIF examples and a sequence it writes,
// which libheif must still decode and play as it did, with every other item's data as it was, and read the packet the
// update wrote.

#include "test.h"

#include "builders.h"
#include "interruptions.h"
#include "libfiles/exif.h"
#include "libfiles/properties.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"
#include "round_trip.h"
#include "scan_internal.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#ifdef LIBFILES_HEIF
#include <fstream>
#include <iterator>

#include "libfiles/decode.h"
#include "libfiles/encode.h"

#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_metadata.h>
#include <libheif/heif_properties.h>
#include <libheif/heif_sequences.h>
#endif

using namespace test_build;

namespace
{
	libfiles::xmp::meta rated(const int rating, const std::string_view title = {})
	{
		libfiles::xmp::meta meta;
		meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", rating);
		if (!title.empty())
			meta.set_localized_text(libfiles::xmp::ns::dc, "title", "", "x-default", std::string(title));
		return meta;
	}

	struct written
	{
		bytes file;
		libfiles::write_plan plan;
	};

	written update(const bytes& file, const libfiles::metadata_update& u)
	{
		libfiles::reader r(file);
		auto plan = libfiles::plan_update(r, u);

		libfiles::memory_stream out;
		if (plan.ok()) libfiles::write_updated(r, plan, out);
		return {out.bytes(), std::move(plan)};
	}

	written update(const bytes& file, const libfiles::xmp::meta& meta)
	{
		return update(file, libfiles::metadata_update{&meta});
	}

	libfiles::scan_result scanned(const bytes& file)
	{
		libfiles::reader r(file);
		return libfiles::scan(r);
	}

	int rating_of(const bytes& file)
	{
		const auto result = scanned(file);
		if (!result.success || result.metadata.xmp.empty()) return -100;

		libfiles::xmp::meta meta;
		meta.parse(to_string(result.metadata.xmp));

		std::int32_t rating = -99;
		meta.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating);
		return rating;
	}

	libfiles::properties exif_properties(const bytes& file)
	{
		return libfiles::properties_from_exif(libfiles::read_exif(scanned(file).metadata.exif));
	}

	// A box as the test reads one, by its header alone.
	struct test_box
	{
		std::string type;
		std::size_t offset = 0;
		std::size_t size = 0;
		std::size_t header = 8;
	};

	std::vector<test_box> boxes_between(const bytes& data, const std::size_t from, const std::size_t to)
	{
		std::vector<test_box> out;
		auto pos = from;

		while (pos + 8 <= to)
		{
			test_box box;
			box.offset = pos;
			box.type = to_string(bytes(data.begin() + static_cast<std::ptrdiff_t>(pos + 4),
			                           data.begin() + static_cast<std::ptrdiff_t>(pos + 8)));
			std::uint64_t size = libfiles::detail::load_be32(data.data() + pos);

			if (size == 1 && pos + 16 <= to)
			{
				size = std::uint64_t{libfiles::detail::load_be32(data.data() + pos + 8)} << 32 |
				       libfiles::detail::load_be32(data.data() + pos + 12);
				box.header = 16;
			}
			else if (size == 0)
			{
				size = to - pos;
			}

			if (size < box.header || size > to - pos) break;
			box.size = static_cast<std::size_t>(size);
			out.push_back(box);
			pos += box.size;
		}

		return out;
	}

	std::string types_of(const bytes& file)
	{
		std::string out;

		for (const auto& box : boxes_between(file, 0, file.size()))
		{
			if (!out.empty()) out += ",";
			out += box.type;
		}

		return out;
	}

	// Each item's data, by id, as the last meta box's item locations give it: a reader of the test's own, so that
	// where the writer says data lies is checked by more than the parser it shares with the scan.
	std::map<std::uint32_t, bytes> items_of(const bytes& file)
	{
		std::map<std::uint32_t, bytes> out;
		const test_box* meta = nullptr;
		const auto top = boxes_between(file, 0, file.size());
		for (const auto& box : top)
			if (box.type == "meta") meta = &box;
		if (!meta) return out;

		std::size_t idat = 0;
		const test_box* iloc = nullptr;
		const auto children = boxes_between(file, meta->offset + meta->header + 4, meta->offset + meta->size);

		for (const auto& child : children)
		{
			if (child.type == "idat" && idat == 0) idat = child.offset + child.header;
			if (child.type == "iloc" && !iloc) iloc = &child;
		}

		if (!iloc) return out;

		auto pos = iloc->offset + iloc->header;
		const auto read = [&file, &pos](const std::uint64_t n)
		{
			std::uint64_t v = 0;
			for (std::uint64_t i = 0; i < n && pos < file.size(); ++i) v = v << 8 | file[pos++];
			return v;
		};

		const auto version = read(1);
		read(3);
		const auto sizes = read(2);
		const auto index_size = version >= 1 ? sizes & 15 : 0;
		const auto count = read(version < 2 ? 2 : 4);

		for (std::uint64_t i = 0; i < count; ++i)
		{
			const auto id = static_cast<std::uint32_t>(read(version < 2 ? 2 : 4));
			const auto method = version >= 1 ? read(2) & 15 : 0;
			read(2);
			const auto base = read(sizes >> 4 & 15);
			const auto extents = read(2);
			bytes data;

			for (std::uint64_t e = 0; e < extents; ++e)
			{
				read(index_size);
				const auto offset = read(sizes >> 12 & 15);
				const auto length = read(sizes >> 8 & 15);
				const auto at = (method == 1 ? idat : 0) + base + offset;

				if (at > file.size() || length > file.size() - at) return {};
				data.insert(data.end(), file.begin() + static_cast<std::ptrdiff_t>(at),
				            file.begin() + static_cast<std::ptrdiff_t>(at + length));
			}

			out[id] = std::move(data);
		}

		return out;
	}

	// An image, its EXIF and a packet, each attached to the image.
	heif_spec photo()
	{
		auto s = simple_heic();
		s.items.push_back(exif_item(2, 1));
		s.items.push_back(mime_item(3, "application/rdf+xml", "<x:xmpmeta xmlns:x='adobe:ns:meta/'/>"));
		s.references.push_back({"cdsc", 2, {1}});
		s.references.push_back({"cdsc", 3, {1}});
		return s;
	}

	std::string padded_packet(const libfiles::xmp::meta& meta)
	{
		libfiles::xmp::serialize_options options;
		options.use_compact_format = true;
		options.padding = 2048;
		return meta.serialize(options);
	}
}

TEST("a heif gains a packet item attached to its primary image, as libheif adds one, and is patched after")
{
	CHECK(libfiles::can_update(libfiles::file_format::heif));

	const auto original = make_heif(simple_heic());
	CHECK(libfiles::detect_file_format(original) == libfiles::file_format::heif);

	const auto first = update(original, rated(3));
	CHECK_STR(first.plan.error, "");
	CHECK(!first.plan.patchable());
	CHECK(scanned(first.file).success);
	CHECK_EQ(rating_of(first.file), 3);
	CHECK_STR(types_of(first.file), "ftyp,meta,mdat");

	// The image's data is as it was, and the packet item, the next id, holds the packet whole.
	const auto before = items_of(original);
	const auto after = items_of(first.file);
	CHECK_EQ(after.size(), before.size() + 1);
	CHECK(after.contains(1) && after.at(1) == before.at(1));
	CHECK(after.contains(2) && to_string(after.at(2)).find("Rating") != std::string::npos);

	libfiles::reader r(first.file);
	const auto packet = libfiles::read_packet(r);
	CHECK(packet.readable && packet.packet == to_string(after.at(2)));

	// The packet was given room, so the next edit is a patch, and an interrupted patch reads whole.
	const auto second = update(first.file, rated(5));
	CHECK(second.plan.in_place());
	CHECK_EQ(second.file.size(), first.file.size());
	CHECK_EQ(rating_of(second.file), 5);

	const auto states = patch_states(first.file, second.plan);
	CHECK(states.size() >= 2);
	CHECK(!states.empty() && states.back() == second.file);

	for (const auto& state : states)
	{
		CHECK(scanned(state).success);
		const auto rating = rating_of(state);
		CHECK(rating == 3 || rating == 5);
	}
}

TEST("a heif's exif and packet that fit are patched where they lie, each old or new when interrupted")
{
	auto s = simple_heic();
	s.items.push_back(exif_item(2, 1));
	s.references.push_back({"cdsc", 2, {1}});

	const auto one = rated(1);
	libfiles::metadata_update first_edit{&one};
	first_edit.legacy.rating = 1;
	const auto first = update(make_heif(s), first_edit);
	CHECK_STR(first.plan.error, "");
	CHECK_EQ(rating_of(first.file), 1);
	CHECK(exif_properties(first.file).rating == 1);

	const auto five = rated(5);
	libfiles::metadata_update second_edit{&five};
	second_edit.legacy.rating = 5;
	const auto second = update(first.file, second_edit);
	CHECK(second.plan.in_place());
	CHECK_EQ(rating_of(second.file), 5);
	CHECK(exif_properties(second.file).rating == 5);

	const auto states = patch_states(first.file, second.plan);
	CHECK(states.size() >= 4);

	for (const auto& state : states)
	{
		CHECK(scanned(state).success);
		const auto packet = rating_of(state);
		const auto exif = exif_properties(state).rating;
		CHECK(packet == 1 || packet == 5);
		CHECK(exif == 1 || exif == 5);
	}
}

TEST("a heif reads back the edits it keeps outside the packet, and keeps its orientation in its image's turn")
{
	// Nothing but the packet in a HEIF without EXIF; it does not gain one.
	check_round_trip("heif", make_heif(simple_heic()), {});

	// One that holds EXIF keeps what a JPEG XL's EXIF keeps. The orientation is the image's rotation and mirror,
	// which libheif turns the picture by, and the EXIF's is made the one they come to.
	auto s = simple_heic();
	s.items.push_back(exif_item(2, 3));
	s.references.push_back({"cdsc", 2, {1}});
	const auto with_exif = make_heif(s);
	check_round_trip("heif with exif", with_exif,
	                 {"copyright", "creators", "description", "digitized", "keywords", "location", "orientation",
	                  "rating", "taken", "year"});

	libfiles::metadata_update turned;
	turned.legacy.orientation = 6;
	turned.legacy.rating = 2;
	const auto edited = update(with_exif, turned);
	CHECK_STR(edited.plan.error, "");
	CHECK(exif_properties(edited.file).orientation == 6);
	CHECK(exif_properties(edited.file).rating == 2);
	CHECK(scanned(edited.file).container_transform);
}

TEST("a heif's items keep their data in every layout, wherever an update moves it")
{
	struct layout_case
	{
		std::string_view name;
		heif_spec spec;
		std::string types;
	};

	std::vector<layout_case> cases;
	cases.push_back({"locations with bases", photo(), "ftyp,meta,mdat"});

	auto absolute = photo();
	absolute.iloc_version = 0;
	absolute.iloc_bases = false;
	cases.push_back({"version 0 from the start of the file", absolute, "ftyp,meta,mdat"});

	auto wide_ids = photo();
	wide_ids.iloc_version = 2;
	cases.push_back({"version 2", wide_ids, "ftyp,meta,mdat"});

	auto mdat_first = photo();
	mdat_first.mdat_first = true;
	cases.push_back({"mdat first", mdat_first, "ftyp,mdat,meta"});

	auto to_end = photo();
	to_end.mdat_to_end = true;
	cases.push_back({"mdat to the end", to_end, "ftyp,meta,mdat"});

	auto large = photo();
	large.large_meta_header = true;
	cases.push_back({"64-bit meta size", large, "ftyp,meta,mdat"});

	auto boxes = photo();
	boxes.before_meta = iso_box("free", bytes(16, 0));
	boxes.meta_boxes = {iso_box("free", bytes(4, 0))};
	cases.push_back({"free boxes", boxes, "ftyp,free,meta,mdat"});

	auto pieces = photo();
	pieces.items[0].extents = 3;
	pieces.items[2].extents = 2;
	cases.push_back({"items in pieces", pieces, "ftyp,meta,mdat"});

	auto in_idat = photo();
	in_idat.items[2].in_idat = true;
	cases.push_back({"a packet in idat", in_idat, "ftyp,meta,mdat"});

	// A motion photo's video stays the last thing in the file, where its directory says it is.
	auto video = photo();
	video.after_mdat = iso_box("mpvd", bytes(100, 0x22));
	cases.push_back({"a video after", video, "ftyp,meta,mdat,mpvd"});

	const auto edit_packet = rated(4, std::string(3000, 'T'));
	libfiles::metadata_update edit{&edit_packet};
	edit.legacy.rating = 4;
	edit.legacy.description = "A description long enough to grow the EXIF block";

	for (const auto& c : cases)
	{
		const std::string name(c.name);
		const auto original = make_heif(c.spec);
		CHECK_STR(name + ": " + scan_fault(original), name + ": ");

		const auto edited = update(original, edit);
		CHECK_STR(name + ": " + edited.plan.error, name + ": ");
		CHECK_STR(name + ": " + scan_fault(edited.file), name + ": ");
		CHECK_STR(name + ": " + types_of(edited.file), name + ": " + c.types);
		CHECK_EQ(rating_of(edited.file), 4);
		CHECK(exif_properties(edited.file).rating == 4);
		CHECK_STR(exif_properties(edited.file).description, "A description long enough to grow the EXIF block");

		const auto before = items_of(original);
		const auto after = items_of(edited.file);
		CHECK_STR(name + ": " + std::to_string(after.size()), name + ": " + std::to_string(before.size()));
		CHECK(after.contains(1) && after.at(1) == before.at(1));
		CHECK(after.contains(3) && after.at(3) == to_bytes(to_string(scanned(edited.file).metadata.xmp)));

		if (!c.spec.after_mdat.empty())
		{
			CHECK(std::equal(c.spec.after_mdat.rbegin(), c.spec.after_mdat.rend(), edited.file.rbegin()));
		}

		// And again, now that the packet has room and the EXIF its fields: a patch.
		const auto again_packet = rated(2, std::string(3000, 'T'));
		libfiles::metadata_update again{&again_packet};
		again.legacy.rating = 2;
		const auto patched = update(edited.file, again);
		CHECK_STR(name + ": " + std::to_string(patched.plan.in_place()), name + ": 1");
		CHECK_EQ(rating_of(patched.file), 2);
	}

	// A file whose items are all in idat is given an mdat after its meta box.
	auto no_mdat = photo();
	for (auto& item : no_mdat.items) item.in_idat = true;
	auto original = make_heif(no_mdat);
	original.resize(original.size() - 8);
	CHECK_STR(types_of(original), "ftyp,meta");
	CHECK_STR(scan_fault(original), "");

	const auto edited = update(original, edit);
	CHECK_STR(edited.plan.error, "");
	CHECK_STR(types_of(edited.file), "ftyp,meta,mdat");
	CHECK_EQ(rating_of(edited.file), 4);
	CHECK(items_of(edited.file).at(1) == items_of(original).at(1));
}

TEST("a heif's new packet item takes an id after every id its items, references and groups give")
{
	auto s = simple_heic();
	s.items.push_back(exif_item(7, 1));
	s.references.push_back({"cdsc", 7, {1}});
	s.meta_boxes = {iso_box("grpl", iso_full_box("altr", 0, 0, builder().be32(40).be32(1).be32(1).b))};

	const auto edited = update(make_heif(s), rated(2));
	CHECK_STR(edited.plan.error, "");
	CHECK(items_of(edited.file).contains(41));
	CHECK_EQ(rating_of(edited.file), 2);
}

namespace
{
	// A movie box whose one track's sample table puts its chunks at these offsets, in four-byte fields or eight.
	bytes movie_with_chunks(const std::vector<std::uint64_t>& offsets, const bool wide)
	{
		builder table;
		table.be32(static_cast<std::uint32_t>(offsets.size()));

		for (const auto offset : offsets)
		{
			if (wide) table.be32(static_cast<std::uint32_t>(offset >> 32));
			table.be32(static_cast<std::uint32_t>(offset));
		}

		const auto stbl = iso_box("stbl", iso_full_box(wide ? "co64" : "stco", 0, 0, table.b));
		const auto mdia = iso_box("mdia",
		                          builder().raw(iso_full_box("mdhd", 0, 0, bytes(20, 0))).raw(iso_box("minf", stbl)).b);
		const auto trak = iso_box("trak", builder().raw(iso_full_box("tkhd", 0, 0, bytes(80, 0))).raw(mdia).b);
		return iso_box("moov", builder().raw(iso_box("mvhd", bytes(100, 0))).raw(trak).b);
	}

	// The chunk offsets of a file's first track, as its movie box gives them.
	std::vector<std::uint64_t> chunk_offsets(const bytes& file)
	{
		std::optional<test_box> at;
		for (const auto& b : boxes_between(file, 0, file.size()))
			if (b.type == "moov") at = b;

		for (const auto* const type : {"trak", "mdia", "minf", "stbl"})
		{
			if (!at) return {};
			const auto children = boxes_between(file, at->offset + at->header, at->offset + at->size);
			const auto found = std::ranges::find(children, type, &test_box::type);
			at = found == children.end() ? std::nullopt : std::optional(*found);
		}

		std::vector<std::uint64_t> out;
		if (!at) return out;

		for (const auto& table : boxes_between(file, at->offset + at->header, at->offset + at->size))
		{
			if (table.type != "stco" && table.type != "co64") continue;
			const auto wide = table.type == "co64";
			const auto* const p = file.data() + table.offset + table.header;
			const auto count = libfiles::detail::load_be32(p + 4);

			for (std::uint32_t i = 0; i < count; ++i)
			{
				const auto* const entry = p + 8 + std::size_t{i} * (wide ? 8 : 4);
				out.push_back(wide ? std::uint64_t{libfiles::detail::load_be32(entry)} << 32 |
				                         libfiles::detail::load_be32(entry + 4)
				                   : libfiles::detail::load_be32(entry));
			}
		}

		return out;
	}
}

TEST("a heif holding an image sequence moves its tracks' chunk offsets with the media they locate")
{
	// A track whose chunks lie in an mdat of their own after the movie box, and one inside an item's data in the
	// mdat the update puts its packet at the start of; each offset in four bytes, then in eight.
	const std::string_view shared = "QRSTUVWX";
	const bytes samples = {0xA1, 0xA2, 0xA3, 0xA4, 0xB1, 0xB2, 0xB3, 0xB4};

	for (const auto wide : {false, true})
	{
		auto s = simple_heic();
		s.items.push_back(mime_item(2, "application/octet-stream", shared));
		const auto still = make_heif(s);
		const auto in_items = static_cast<std::uint64_t>(
			std::ranges::search(still, bytes(shared.begin(), shared.end())).begin() - still.begin());
		const auto own = still.size() + movie_with_chunks({0, 0, 0}, wide).size() + 8;
		s.after_mdat = builder().raw(movie_with_chunks({own, own + 4, in_items}, wide)).raw(iso_box("mdat", samples)).b;
		const auto sequence = make_heif(s);
		CHECK_STR(scan_fault(sequence), "");
		libfiles::reader sequence_reader{std::span<const std::uint8_t>(sequence)};
		CHECK(libfiles::scan(sequence_reader).animated);
		libfiles::reader still_reader{std::span<const std::uint8_t>(still)};
		CHECK(!libfiles::scan(still_reader).animated);

		const auto edited = update(sequence, rated(2));
		CHECK_STR(edited.plan.error, "");
		CHECK_EQ(rating_of(edited.file), 2);

		const auto before = chunk_offsets(sequence);
		const auto after = chunk_offsets(edited.file);
		CHECK_EQ(before.size(), 3u);
		CHECK_EQ(after.size(), before.size());

		for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i)
		{
			CHECK(after[i] > before[i]);
			CHECK(after[i] + 4 <= edited.file.size() &&
			      std::equal(sequence.begin() + static_cast<std::ptrdiff_t>(before[i]),
			                 sequence.begin() + static_cast<std::ptrdiff_t>(before[i] + 4),
			                 edited.file.begin() + static_cast<std::ptrdiff_t>(after[i])));
		}
	}

	// A fragmented one keeps its offsets in boxes an update does not read, and is refused a rewrite.
	auto fragmented = simple_heic();
	fragmented.after_mdat = builder()
	                            .raw(movie_with_chunks({}, false))
	                            .raw(iso_box("moof", builder()
	                                                     .raw(iso_full_box("mfhd", 0, 0, bytes(4, 0)))
	                                                     .raw(iso_box("traf", iso_full_box("tfhd", 0, 0, bytes(4, 0))))
	                                                     .b))
	                            .raw(iso_box("mdat", samples))
	                            .b;
	const auto refused = update(make_heif(fragmented), rated(2));
	CHECK(!refused.plan.ok());
	CHECK(refused.plan.error.find("fragmented") != std::string::npos);

	// A packet that fits moves nothing, so it is patched.
	auto padded = simple_heic();
	padded.after_mdat = movie_with_chunks({}, false);
	padded.items.push_back(mime_item(2, "application/rdf+xml", padded_packet(rated(1))));
	padded.references.push_back({"cdsc", 2, {1}});
	const auto fits = update(make_heif(padded), rated(5));
	CHECK(fits.plan.in_place());
	CHECK_EQ(rating_of(fits.file), 5);
}

#ifdef LIBFILES_HEIF
namespace
{
	bytes read_example(const char* const name)
	{
		std::ifstream in(std::string(LIBFILES_HEIF_EXAMPLES) + "/" + name, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	struct libheif_item
	{
		std::uint32_t type = 0;
		std::string content_type;
		bytes data;
	};

	// What libheif reads of a file: every item's data as the file stores it, the primary image's packet and EXIF
	// as it hands them to an application, and the primary image and its first thumbnail decoded.
	struct libheif_reading
	{
		bool read = false;
		std::map<heif_item_id, libheif_item> items;
		bytes xmp;
		bytes exif;
		bytes pixels;
		int width = 0;
		int height = 0;
		int thumbnail_width = 0;
		int thumbnail_height = 0;
	};

	libheif_reading read_with_libheif(const bytes& file)
	{
		libheif_reading out;
		auto* const ctx = heif_context_alloc();
		heif_image_handle* handle = nullptr;
		heif_image* image = nullptr;

		if (heif_context_read_from_memory_without_copy(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok &&
		    heif_context_get_primary_image_handle(ctx, &handle).code == heif_error_Ok)
		{
			out.read = true;
			const auto count = heif_context_get_number_of_items(ctx);
			std::vector<heif_item_id> ids(static_cast<std::size_t>(std::max(count, 0)));
			heif_context_get_list_of_item_IDs(ctx, ids.data(), count);

			for (const auto id : ids)
			{
				auto& item = out.items[id];
				item.type = heif_item_get_item_type(ctx, id);
				if (const auto* const type = heif_item_get_mime_item_content_type(ctx, id)) item.content_type = type;

				heif_metadata_compression compression{};
				std::uint8_t* data = nullptr;
				std::size_t size = 0;
				if (heif_item_get_item_data(ctx, id, &compression, &data, &size).code == heif_error_Ok && data)
					item.data.assign(data, data + size);
				heif_release_item_data(ctx, &data);
			}

			for (const auto* const filter : {"mime", "Exif"})
			{
				const auto blocks = heif_image_handle_get_number_of_metadata_blocks(handle, filter);
				std::vector<heif_item_id> block_ids(static_cast<std::size_t>(std::max(blocks, 0)));
				heif_image_handle_get_list_of_metadata_block_IDs(handle, filter, block_ids.data(), blocks);

				for (const auto id : block_ids)
				{
					bytes block(heif_image_handle_get_metadata_size(handle, id));
					heif_image_handle_get_metadata(handle, id, block.data());
					const std::string content_type = heif_image_handle_get_metadata_content_type(handle, id);

					if (std::string_view(filter) == "Exif") out.exif = std::move(block);
					else if (content_type == "application/rdf+xml") out.xmp = std::move(block);
				}
			}

			if (heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr).code ==
			    heif_error_Ok)
			{
				int stride = 0;
				const auto* const plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
				const auto width = heif_image_get_width(image, heif_channel_interleaved);
				const auto height = heif_image_get_height(image, heif_channel_interleaved);
				out.width = width;
				out.height = height;

				for (auto y = 0; y < height; ++y)
				{
					const auto* const row = plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
					out.pixels.insert(out.pixels.end(), row, row + static_cast<std::size_t>(width) * 4);
				}
			}

			heif_item_id thumbnail_id = 0;
			heif_image_handle* thumbnail = nullptr;
			heif_image* thumbnail_image = nullptr;

			if (heif_image_handle_get_list_of_thumbnail_IDs(handle, &thumbnail_id, 1) == 1 &&
			    heif_image_handle_get_thumbnail(handle, thumbnail_id, &thumbnail).code == heif_error_Ok &&
			    heif_decode_image(thumbnail, &thumbnail_image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
			                      nullptr)
			            .code == heif_error_Ok)
			{
				out.thumbnail_width = heif_image_get_width(thumbnail_image, heif_channel_interleaved);
				out.thumbnail_height = heif_image_get_height(thumbnail_image, heif_channel_interleaved);
			}

			heif_image_release(thumbnail_image);
			heif_image_handle_release(thumbnail);
		}

		heif_image_release(image);
		heif_image_handle_release(handle);
		heif_context_free(ctx);
		return out;
	}

	bool is_metadata(const libheif_item& item)
	{
		return item.type == heif_item_type_exif ||
		       (item.type == heif_item_type_mime && item.content_type == "application/rdf+xml");
	}
}

TEST("libheif decodes a heif as it did after an update, keeps every other item's data, and reads the packet written")
{
	for (const auto* const name : {"example.heic", "example.avif"})
	{
		const auto original = read_example(name);
		CHECK(original.size() > 1000);
		const auto before = read_with_libheif(original);
		CHECK(before.read && !before.pixels.empty());

		const auto packet = rated(4, "Edited");
		libfiles::metadata_update edit{&packet};
		edit.legacy.rating = 4;
		edit.legacy.date_original = "2021-06-15T10:20:30";
		const auto edited = update(original, edit);
		CHECK_STR(edited.plan.error, "");

		const auto after = read_with_libheif(edited.file);
		CHECK(after.read);
		CHECK(after.pixels == before.pixels);
		CHECK_EQ(rating_of(edited.file), 4);

		// libheif takes the packet the scan takes.
		const auto scan = scanned(edited.file);
		CHECK(!after.xmp.empty() && after.xmp == bytes(scan.metadata.xmp.begin(), scan.metadata.xmp.end()));
		if (!before.exif.empty()) CHECK(!after.exif.empty() && after.exif != before.exif);

		// Every item it held is there with its data, but the metadata the update wrote, and one packet item at most
		// is new.
		CHECK(after.items.size() == before.items.size() || after.items.size() == before.items.size() + 1);

		for (const auto& [id, item] : before.items)
		{
			const auto found = after.items.find(id);
			CHECK(found != after.items.end());
			if (found == after.items.end()) continue;
			CHECK(found->second.type == item.type);
			CHECK(found->second.data == item.data || is_metadata(item));
		}

		// A second edit fits the room the first gave the packet, and libheif reads the patched file the same.
		const auto again = update(edited.file, rated(2, "Edited"));
		CHECK(again.plan.in_place());
		const auto patched = read_with_libheif(again.file);
		CHECK(patched.read && patched.pixels == before.pixels);
		CHECK_EQ(rating_of(again.file), 2);
	}
}

namespace
{
	// The pixels libheif decoded, turned as an EXIF orientation of 2, 6 or 8 turns them.
	bytes turned_pixels(const libheif_reading& r, const int orientation)
	{
		const auto swap = orientation == 6 || orientation == 8;
		const auto width = swap ? r.height : r.width;
		const auto height = swap ? r.width : r.height;
		bytes out;

		for (auto y = 0; y < height; ++y)
		{
			for (auto x = 0; x < width; ++x)
			{
				auto sx = r.width - 1 - x;
				auto sy = y;
				if (orientation == 6) sx = y, sy = r.height - 1 - x;
				if (orientation == 8) sx = r.width - 1 - y, sy = x;
				const auto at = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(r.width) + sx) * 4;
				out.insert(out.end(), r.pixels.begin() + static_cast<std::ptrdiff_t>(at),
				           r.pixels.begin() + static_cast<std::ptrdiff_t>(at + 4));
			}
		}

		return out;
	}

	// The mean difference between two pictures' bytes; a picture of another size is as different as can be.
	double mean_difference(const bytes& a, const bytes& b)
	{
		if (a.size() != b.size() || a.empty()) return 255.0;
		std::uint64_t total = 0;
		for (std::size_t i = 0; i < a.size(); ++i) total += static_cast<std::uint64_t>(std::abs(a[i] - b[i]));
		return static_cast<double>(total) / static_cast<double>(a.size());
	}
}

// An orientation turns the picture libheif decodes, its thumbnail with it: a rotation added where the image had none,
// then turned back where it lies; and a mirror. Every item's data stays as it was. libheif turns a picture before
// it takes its chroma to RGB, so a turned picture is the turned decode but for the chroma's rounding.
TEST("libheif decodes a heif turned as its orientation turns it, its thumbnail with it")
{
	for (const auto* const name : {"example.heic", "example.avif"})
	{
		const auto original = read_example(name);
		const auto before = read_with_libheif(original);
		CHECK(before.read && !before.pixels.empty());

		const auto turn = [](const bytes& file, const int orientation)
		{
			libfiles::metadata_update u;
			u.legacy.orientation = orientation;
			return update(file, u);
		};

		const auto clockwise = turn(original, 6);
		CHECK_STR(clockwise.plan.error, "");
		const auto turned = read_with_libheif(clockwise.file);
		CHECK(turned.read && turned.width == before.height && turned.height == before.width);
		CHECK(mean_difference(turned.pixels, turned_pixels(before, 6)) < 2.0);
		CHECK(mean_difference(turned.pixels, turned_pixels(before, 8)) > 10.0);
		CHECK(turned.thumbnail_width == before.thumbnail_height && turned.thumbnail_height == before.thumbnail_width);
		CHECK(scanned(clockwise.file).container_transform);
		if (!before.exif.empty()) CHECK(exif_properties(clockwise.file).orientation == 6);

		for (const auto& [id, item] : before.items)
		{
			const auto found = turned.items.find(id);
			CHECK(found != turned.items.end() && (found->second.data == item.data || is_metadata(item)));
		}

		const auto back = turn(clockwise.file, 8);
		CHECK_STR(back.plan.error, "");
		CHECK(back.plan.in_place());
		const auto upright = read_with_libheif(back.file);
		CHECK(upright.read && upright.pixels == before.pixels);
		if (!before.exif.empty()) CHECK(exif_properties(back.file).orientation == 1);

		const auto mirrored = turn(original, 2);
		CHECK_STR(mirrored.plan.error, "");
		const auto flipped = read_with_libheif(mirrored.file);
		CHECK(flipped.read && mean_difference(flipped.pixels, turned_pixels(before, 2)) < 2.0);
		CHECK(mean_difference(flipped.pixels, before.pixels) > 10.0);
	}
}

namespace
{
	struct decoded_size
	{
		bool success = false;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	// The size a picture, or its gain map, decodes at.
	decoded_size decoded(const bytes& file, const bool gain_map)
	{
		struct sink final : libfiles::image_sink
		{
			bytes memory;

			libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
			{
				memory.assign(std::size_t{4} * frame.width * frame.height, 0);
				return {memory.data(), std::size_t{4} * frame.width};
			}
		} s;

		libfiles::reader r(file);
		libfiles::decode_options options;
		options.heif_gain_map = gain_map;
		const auto result = libfiles::decode(r, s, options);
		return {result.success, result.frame.width, result.frame.height};
	}
}

// libavif turns a picture that has an ISO 21496-1 gain map by the rotation and mirror of the picture and of its gain
// map, and none of the tone map item that takes both, whose inputs are turned already: a turn written to such a file
// turns the gain map with the picture.
TEST("a heif's gain map turns with its picture, and the tone map item that applies it stays unturned")
{
	const std::uint32_t width = 32, height = 16;
	bytes pixels(std::size_t{4} * width * height, 0x60);
	bytes map(std::size_t{4} * 16 * 8, 0x80);
	libfiles::gain_map_metadata metadata;
	metadata.alternate_headroom = 2.0;
	metadata.gain_max.fill(2.0);

	libfiles::avif_options options;
	options.gain_map = libfiles::gain_map_image{{map.data(), 64, 16, 8}, metadata};
	const auto avif = libfiles::encode_avif({pixels.data(), std::size_t{4} * width, width, height}, 1, {}, options);
	CHECK(!avif.data.empty());

	libfiles::metadata_update u;
	u.legacy.orientation = 6;
	const auto clockwise = update(avif.data, u);
	CHECK_STR(clockwise.plan.error, "");

	const auto picture = decoded(clockwise.file, false);
	const auto gain_map = decoded(clockwise.file, true);
	CHECK(picture.success && picture.width == height && picture.height == width);
	CHECK(gain_map.success && gain_map.width == 8 && gain_map.height == 16);

	auto* const ctx = heif_context_alloc();
	CHECK(heif_context_read_from_memory_without_copy(ctx, clockwise.file.data(), clockwise.file.size(), nullptr).code ==
	      heif_error_Ok);
	const auto count = heif_context_get_number_of_items(ctx);
	std::vector<heif_item_id> ids(static_cast<std::size_t>(std::max(count, 0)));
	heif_context_get_list_of_item_IDs(ctx, ids.data(), count);
	auto tone_maps = 0;

	for (const auto id : ids)
	{
		if (heif_item_get_item_type(ctx, id) != heif_fourcc('t', 'm', 'a', 'p')) continue;
		++tone_maps;
		CHECK_EQ(heif_item_get_properties_of_type(ctx, id, heif_item_property_type_transform_rotation, nullptr, 0), 0);
	}

	CHECK_EQ(tone_maps, 1);
	heif_context_free(ctx);
}

namespace
{
	// A picture libheif encodes, every pixel of it one shade of grey.
	heif_image* flat_image(const int width, const int height, const std::uint8_t shade)
	{
		heif_image* image = nullptr;
		heif_image_create(width, height, heif_colorspace_RGB, heif_chroma_interleaved_RGB, &image);
		heif_image_add_plane(image, heif_channel_interleaved, width, height, 8);
		int stride = 0;
		auto* const plane = heif_image_get_plane(image, heif_channel_interleaved, &stride);
		for (auto y = 0; y < height; ++y)
			std::fill_n(plane + static_cast<std::size_t>(y) * stride, std::size_t{3} * width, shade);
		return image;
	}

	// A HEIF libheif writes with its own uncompressed codec: a still primary image, and a track of frames, one of
	// each shade.
	bytes libheif_sequence(const std::vector<std::uint8_t>& shades)
	{
		auto* const ctx = heif_context_alloc();
		heif_encoder* encoder = nullptr;
		bytes file;

		if (heif_context_get_encoder_for_format(ctx, heif_compression_uncompressed, &encoder).code == heif_error_Ok)
		{
			auto* const still = flat_image(16, 8, 0x40);
			const auto encoded = heif_context_encode_image(ctx, still, encoder, nullptr, nullptr).code;
			heif_image_release(still);

			heif_context_set_sequence_timescale(ctx, 30);
			heif_track* track = nullptr;
			auto ok = encoded == heif_error_Ok &&
			          heif_context_add_visual_sequence_track(ctx, 16, 8, heif_track_type_image_sequence, nullptr,
			                                                 nullptr, &track)
			                  .code == heif_error_Ok;

			for (const auto shade : shades)
			{
				auto* const frame = flat_image(16, 8, shade);
				heif_image_set_duration(frame, 1);
				ok = ok && heif_track_encode_sequence_image(track, frame, encoder, nullptr).code == heif_error_Ok;
				heif_image_release(frame);
			}

			ok = ok && heif_track_encode_end_of_sequence(track, encoder).code == heif_error_Ok;
			heif_track_release(track);
			heif_encoder_release(encoder);

			heif_writer writer{};
			writer.writer_api_version = 1;
			writer.write = [](heif_context*, const void* data, const size_t size, void* userdata)
			{
				auto* const out = static_cast<bytes*>(userdata);
				out->insert(out->end(), static_cast<const std::uint8_t*>(data),
				            static_cast<const std::uint8_t*>(data) + size);
				return heif_error{heif_error_Ok, heif_suberror_Unspecified, ""};
			};
			if (!ok || heif_context_write(ctx, &writer, &file).code != heif_error_Ok) file.clear();
		}

		heif_context_free(ctx);
		return file;
	}

	// The shade of each frame libheif decodes from a file's first track, once through.
	std::vector<std::uint8_t> track_shades(const bytes& file)
	{
		std::vector<std::uint8_t> out;
		auto* const ctx = heif_context_alloc();

		if (heif_context_read_from_memory_without_copy(ctx, file.data(), file.size(), nullptr).code == heif_error_Ok &&
		    heif_context_number_of_sequence_tracks(ctx) > 0)
		{
			std::vector<std::uint32_t> ids(static_cast<std::size_t>(heif_context_number_of_sequence_tracks(ctx)));
			heif_context_get_track_ids(ctx, ids.data());
			auto* const track = heif_context_get_track(ctx, ids.front());
			auto* const options = heif_decoding_options_alloc();
			options->ignore_sequence_editlist = 1;
			heif_image* frame = nullptr;

			while (
				track && out.size() < 64 &&
				heif_track_decode_next_image(track, &frame, heif_colorspace_RGB, heif_chroma_interleaved_RGB, options)
						.code == heif_error_Ok)
			{
				int stride = 0;
				out.push_back(*heif_image_get_plane_readonly(frame, heif_channel_interleaved, &stride));
				heif_image_release(frame);
			}

			heif_decoding_options_free(options);
			heif_track_release(track);
		}

		heif_context_free(ctx);
		return out;
	}
}

TEST("libheif plays every frame of a heif sequence as it did after an update moves its media")
{
	const std::vector<std::uint8_t> shades = {0x20, 0x60, 0xA0, 0xE0};
	const auto sequence = libheif_sequence(shades);
	CHECK(!sequence.empty());
	CHECK(track_shades(sequence) == shades);

	// A first packet: the file is rewritten, its media moved to make room.
	const auto edited = update(sequence, rated(3));
	CHECK_STR(edited.plan.error, "");
	CHECK(!edited.plan.patchable());
	CHECK_EQ(rating_of(edited.file), 3);
	CHECK(track_shades(edited.file) == shades);
	CHECK(read_with_libheif(edited.file).pixels == read_with_libheif(sequence).pixels);
}
#endif
