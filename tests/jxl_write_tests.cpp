// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests for writing a JPEG XL's metadata: its packet and the EXIF it holds, in files built box by
// box here and in files libjxl makes from a JPEG, which must still give that JPEG back after an edit.

#include "test.h"

#include "builders.h"
#include "interruptions.h"
#include "libfiles/exif.h"
#include "libfiles/properties.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"
#include "round_trip.h"
#include "scan_internal.h"

#include <cstring>
#include <string>
#include <vector>

#ifdef LIBFILES_JXL
#include <brotli/encode.h>
#endif

#if defined(LIBFILES_JXL) && defined(LIBFILES_JPEG)
#include "libfiles/decode.h"
#include "libfiles/encode.h"

#include <brotli/decode.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#endif

using namespace test_build;

namespace
{
	libfiles::xmp::meta rated(const int rating)
	{
		libfiles::xmp::meta meta;
		meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", rating);
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

	libfiles::scan_result scanned(const bytes& file, const libfiles::scan_limits& limits = {})
	{
		libfiles::reader r(file);
		return libfiles::scan(r, libfiles::scan_intent::index, limits);
	}

	int rating_of(const bytes& file, const libfiles::scan_limits& limits = {})
	{
		const auto result = scanned(file, limits);
		if (!result.success || result.metadata.xmp.empty()) return -100;

		libfiles::xmp::meta meta;
		meta.parse(to_string(result.metadata.xmp));

		std::int32_t rating = -99;
		meta.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating);
		return rating;
	}

	libfiles::properties exif_properties(const bytes& file, const libfiles::scan_limits& limits = {})
	{
		const auto result = scanned(file, limits);
		return libfiles::properties_from_exif(libfiles::read_exif(result.metadata.exif));
	}

	struct box_at
	{
		std::string type;
		std::size_t offset = 0;
		std::size_t size = 0;
		std::size_t header = 8;
	};

	// A container's top-level boxes, as their headers give them.
	std::vector<box_at> boxes_of(const bytes& file)
	{
		std::vector<box_at> out;
		std::size_t pos = 0;

		while (pos + 8 <= file.size())
		{
			box_at box;
			box.offset = pos;
			box.type.assign(file.begin() + static_cast<std::ptrdiff_t>(pos + 4), file.begin() + static_cast<std::ptrdiff_t>(pos + 8));
			std::uint64_t size = libfiles::detail::load_be32(file.data() + pos);

			if (size == 1 && pos + 16 <= file.size())
			{
				size = std::uint64_t{libfiles::detail::load_be32(file.data() + pos + 8)} << 32 | libfiles::detail::load_be32(file.data() + pos + 12);
				box.header = 16;
			}
			else if (size == 0)
			{
				size = file.size() - pos;
			}

			if (size < box.header || size > file.size() - pos) break;
			box.size = static_cast<std::size_t>(size);
			out.push_back(box);
			pos += box.size;
		}

		return out;
	}

	std::string types_of(const bytes& file)
	{
		std::string out;

		for (const auto& box : boxes_of(file))
		{
			if (!out.empty()) out += ',';
			out += box.type;
		}

		return out;
	}

	bytes contents_of(const bytes& file, const box_at& box)
	{
		return {file.begin() + static_cast<std::ptrdiff_t>(box.offset + box.header), file.begin() + static_cast<std::ptrdiff_t>(box.offset + box.size)};
	}

	// The contents of the first box of a type, or nothing.
	bytes first_contents(const bytes& file, const std::string_view type)
	{
		for (const auto& box : boxes_of(file))
		{
			if (box.type == type) return contents_of(file, box);
		}

		return {};
	}

	// A little-endian TIFF stream whose one entry is the orientation.
	bytes oriented_tiff(const unsigned orientation)
	{
		return builder().raw("II").le16(42).le32(8).le16(1).le16(0x0112).le16(3).le32(1).le16(orientation).le16(0).le32(0).b;
	}

	bytes exif_contents(const bytes& tiff) { return builder().be32(0).raw(tiff).b; }

#ifdef LIBFILES_JXL
	bytes real_brob(const std::string_view type, const bytes& contents, const bool trailing = false)
	{
		auto compressed = bytes(BrotliEncoderMaxCompressedSize(contents.size()));
		auto size = compressed.size();
		if (!BrotliEncoderCompress(5, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, contents.size(),
		                            contents.data(), &size, compressed.data()))
			return {};
		compressed.resize(size);
		builder payload;
		payload.raw(type);
		payload.b.insert(payload.b.end(), compressed.begin(), compressed.end());
		if (trailing) payload.b.push_back(0x7f);
		return iso_box("brob", payload.b);
	}
#endif

	bytes packet_bytes(const libfiles::xmp::meta& meta, const libfiles::xmp::serialize_options& options = {})
	{
		return to_bytes(meta.serialize(options));
	}

	// An EXIF block as a camera leaves one: a description and the time the picture was taken.
	bytes camera_exif()
	{
		libfiles::exif_fields fields;
		fields.description = "Short";
		fields.original = "2001:02:03 04:05:06";
		return libfiles::make_exif(fields);
	}
}

TEST("a jpeg xl takes its first packet before its codestream, and keeps every other box")
{
	const auto stream = codestream({.width = 320, .height = 200});
	const auto original = container({iso_box("Exif", exif_contents(oriented_tiff(1))), iso_box("jxlc", stream)});
	const auto first = update(original, rated(4));

	CHECK(first.plan.ok());
	CHECK(!first.plan.patchable());
	CHECK_STR(types_of(first.file), "JXL ,ftyp,Exif,xml ,jxlc");
	CHECK_EQ(rating_of(first.file), 4);
	CHECK_EQ(scanned(first.file).width, 320u);

	// The boxes before the packet are as they were, and the codestream's every byte.
	const auto boxes = boxes_of(first.file);
	CHECK(std::equal(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(boxes[3].offset), first.file.begin()));
	CHECK(contents_of(first.file, boxes[4]) == stream);

	// Partial codestream boxes, with a box between them: the packet goes before the first.
	const auto split = std::span(stream).first(10);
	const auto rest = std::span(stream).subspan(10);
	const auto parted = container({jxlp(0, false, split), iso_box("free", bytes(3, 0)), jxlp(1, true, rest)});
	const auto into_parts = update(parted, rated(3));
	CHECK(into_parts.plan.ok());
	CHECK_STR(types_of(into_parts.file), "JXL ,ftyp,xml ,jxlp,free,jxlp");
	CHECK_EQ(rating_of(into_parts.file), 3);
}

TEST("a jpeg xl packet that fits is patched where it lies, and an interrupted patch reads whole")
{
	const auto first = update(container({iso_box("jxlc", codestream({}))}), rated(1));
	const auto second = update(first.file, rated(5));

	CHECK(second.plan.ok());
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
		CHECK(rating == 1 || rating == 5);
	}

	// A packet in a box with a 64-bit size keeps it, so the edit is still a patch; one in a box that runs to
	// the end of the file is given its size.
	const auto packet = packet_bytes(rated(2));
	const auto wide = container({iso_box("jxlc", codestream({})),
	                             builder().be32(1).raw("xml ").be32(0).be32(static_cast<std::uint32_t>(packet.size() + 16)).raw(packet).b});
	const auto wide_edit = update(wide, rated(4));
	CHECK(wide_edit.plan.in_place());
	CHECK_EQ(rating_of(wide_edit.file), 4);
	CHECK_EQ(boxes_of(wide_edit.file).back().header, 16u);

	auto open = container({iso_box("jxlc", codestream({}))});
	const auto open_box = builder().be32(0).raw("xml ").raw(packet).b;
	open.insert(open.end(), open_box.begin(), open_box.end());
	const auto open_edit = update(open, rated(4));
	CHECK(open_edit.plan.in_place());
	CHECK_EQ(rating_of(open_edit.file), 4);
	CHECK_EQ(libfiles::detail::load_be32(open_edit.file.data() + boxes_of(open_edit.file).back().offset),
	         static_cast<std::uint32_t>(packet.size() + 8));
}

TEST("a bare jpeg xl codestream is put in a container to take a packet")
{
	const auto stream = codestream({.width = 640, .height = 480, .orientation = 6});
	const auto first = update(stream, rated(4));

	CHECK(first.plan.ok());
	CHECK_STR(types_of(first.file), "JXL ,ftyp,xml ,jxlc");
	CHECK(first_contents(first.file, "jxlc") == stream);
	CHECK(std::equal(jxl_signature.begin(), jxl_signature.end(), first.file.begin()));
	CHECK_EQ(rating_of(first.file), 4);

	const auto result = scanned(first.file);
	CHECK_EQ(result.width, 640u);
	CHECK(result.orientation == 6);

	// What a bare codestream keeps outside a packet is nothing, so an edit of that alone changes nothing.
	libfiles::metadata_update legacy_only;
	legacy_only.legacy.rating = 3;
	const auto untouched = update(stream, legacy_only);
	CHECK(untouched.plan.ok());
	CHECK(untouched.plan.replacements.empty());
}

TEST("a compressed jpeg xl box is read with the decompressor its scan was given, and replaced by a plain one")
{
	const auto file = container({iso_box("jxlc", codestream({})), brob("Exif", exif_contents(oriented_tiff(1))),
	                             brob("xml ", packet_bytes(rated(2)))});
	libfiles::brotli_decompressor rejects = [](std::span<const std::uint8_t>, std::string_view,
	                                           std::size_t) -> std::optional<bytes> { return {}; };

	// An explicitly supplied decompressor still controls the result, including one that refuses the box.
	const auto rejected_meta = rated(5);
	libfiles::metadata_update rejected{&rejected_meta};
	rejected.brotli = rejects;
	CHECK(!update(file, rejected).plan.ok());

	libfiles::reader r(file);
	CHECK(!libfiles::read_packet(r, rejects).readable);
	const auto packet = libfiles::read_packet(r, fake_brotli);
	CHECK(packet.readable);
	CHECK(packet.packet == to_string(packet_bytes(rated(2))));

	const auto meta = rated(5);
	libfiles::metadata_update u{&meta};
	u.brotli = fake_brotli;
	const auto edited = update(file, u);
	CHECK(edited.plan.ok());
	CHECK_STR(types_of(edited.file), "JXL ,ftyp,jxlc,brob,xml ");
	CHECK(!edited.file.empty());
	CHECK_EQ(rating_of(edited.file, with_brotli()), 5);

	// A compressed EXIF the edit writes to needs one too; a packet alone does not touch it.
	libfiles::metadata_update exif_edit{&meta};
	exif_edit.legacy.rating = 5;
	CHECK(!update(file, exif_edit).plan.ok());
	exif_edit.brotli = fake_brotli;
	const auto both = update(file, exif_edit);
	CHECK(both.plan.ok());
	CHECK_STR(types_of(both.file), "JXL ,ftyp,jxlc,Exif,xml ");
	CHECK(exif_properties(both.file).rating == 5);
}

#ifdef LIBFILES_JXL
TEST("built-in Brotli reads JPEG XL brob boxes for scan packet and update")
{
	const auto* const builtin = libfiles::detail::default_brotli_decompressor();
	CHECK(builtin != nullptr);
	if (builtin)
	{
		const bytes plain{'t', 'i', 'n', 'y'};
		bytes compressed(BrotliEncoderMaxCompressedSize(plain.size()));
		auto compressed_size = compressed.size();
		CHECK(BrotliEncoderCompress(5, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, plain.size(),
		                            plain.data(), &compressed_size, compressed.data()));
		compressed.resize(compressed_size);
		const auto small = (*builtin)(compressed, "xml ", 16u * 1024u * 1024u);
		CHECK(small && *small == plain);
		if (small) CHECK(small->capacity() <= 64u * 1024u);
	}

	const auto file = container({iso_box("jxlc", codestream({})),
	                             real_brob("Exif", exif_contents(oriented_tiff(1))),
	                             real_brob("xml ", packet_bytes(rated(2)))});

	const auto scan = scanned(file);
	CHECK(scan.success);
	CHECK(!scan.metadata.exif.empty());
	CHECK(to_string(scan.metadata.xmp) == to_string(packet_bytes(rated(2))));

	libfiles::reader source(file);
	const auto packet = libfiles::read_packet(source);
	CHECK(packet.readable);
	CHECK(packet.packet == to_string(packet_bytes(rated(2))));

	const auto edited = update(file, rated(5));
	CHECK(edited.plan.ok());
	CHECK_EQ(rating_of(edited.file), 5);

	// A malformed or trailing stream is refused by the built-in decoder, while the explicit
	// callback path remains independently testable above.
	const auto malformed = container({iso_box("jxlc", codestream({})),
	                                  real_brob("xml ", packet_bytes(rated(2)), true)});
	CHECK(scanned(malformed).metadata.xmp.empty());
}
#endif

TEST("a jpeg xl reads back the edits it keeps outside the packet, and leaves its orientation to its codestream")
{
	CHECK(libfiles::can_update(libfiles::file_format::jxl));
	CHECK(libfiles::detect_file_format(codestream({})) == libfiles::file_format::jxl);
	CHECK(libfiles::detect_file_format(container({iso_box("jxlc", codestream({}))})) == libfiles::file_format::jxl);

	// Nothing but the packet in a JPEG XL without EXIF; it does not gain one.
	check_round_trip("jxl", container({iso_box("jxlc", codestream({}))}), {});
	check_round_trip("jxl codestream", codestream({}), {});

	// One that holds EXIF keeps what a WebP's EXIF keeps, but for the orientation, which its codestream
	// keeps: an EXIF orientation alone would contradict the picture.
	const auto with_exif = container({iso_box("Exif", exif_contents(oriented_tiff(3))), iso_box("jxlc", codestream({.orientation = 3}))});
	check_round_trip("jxl with exif", with_exif,
	                 {"copyright", "creators", "description", "digitized", "keywords", "location", "rating", "taken", "year"});

	libfiles::metadata_update turned;
	turned.legacy.orientation = 6;
	turned.legacy.rating = 2;
	const auto edited = update(with_exif, turned);
	CHECK(edited.plan.ok());
	CHECK(exif_properties(edited.file).orientation == 3);
	CHECK(exif_properties(edited.file).rating == 2);
	CHECK(scanned(edited.file).orientation == 3);
}

TEST("a jpeg xl made from a jpeg keeps the boxes it gives the jpeg back from at their size")
{
	const auto jbrd = iso_box("jbrd", bytes{1, 2, 3, 4});
	const auto padded = packet_bytes(rated(1));
	const auto exif = camera_exif();
	const auto file = container({jbrd, iso_box("Exif", exif_contents(exif)), iso_box("xml ", padded), iso_box("jxlc", codestream({}))});

	libfiles::metadata_update u;
	const auto meta = rated(4);
	u.xmp = &meta;
	u.legacy.date_original = "2021-06-15T10:20:30";
	u.legacy.description = "A description far longer than the one the camera wrote";
	u.legacy.rating = 4;

	const auto edited = update(file, u);
	CHECK(edited.plan.ok());
	CHECK(edited.plan.in_place());
	CHECK_EQ(edited.file.size(), file.size());
	CHECK_EQ(rating_of(edited.file), 4);

	// What fits where it lies is written, and what would grow the block is left to the packet.
	const auto kept = exif_properties(edited.file);
	CHECK(same_moment(kept.created, 2021, 6, 15, 10, 20, 30));
	CHECK_STR(kept.description, "Short");
	CHECK(!kept.rating.has_value());

	// A file without the box takes every field.
	const auto plain = update(container({iso_box("Exif", exif_contents(exif)), iso_box("xml ", padded), iso_box("jxlc", codestream({}))}), u);
	CHECK(plain.plan.ok());
	const auto grown = exif_properties(plain.file);
	CHECK_STR(grown.description, "A description far longer than the one the camera wrote");
	CHECK(grown.rating == 4);

	// A packet that has no room is refused rather than written at another size.
	const auto tight = container({jbrd, iso_box("xml ", packet_bytes(rated(1), {.omit_packet_wrapper = true})), iso_box("jxlc", codestream({}))});
	const auto refused = update(tight, rated(5));
	CHECK(!refused.plan.ok());
	CHECK(refused.plan.error.find("no room") != std::string::npos);

	// A second packet box is not the one the JPEG is given back with, and takes any size.
	const auto two = container({jbrd, iso_box("xml ", padded), iso_box("xml ", packet_bytes(rated(2), {.omit_packet_wrapper = true})),
	                            iso_box("jxlc", codestream({}))});
	const auto second = update(two, rated(5));
	CHECK(second.plan.ok());
	CHECK_EQ(rating_of(second.file), 5);
	CHECK(first_contents(second.file, "xml ") == padded);
}

TEST("a jpeg xl the read stops in is not updated")
{
	const auto meta = rated(2);

	for (const auto& file : {container({iso_box("xml ", packet_bytes(rated(1)))}), builder().raw(jxl_signature).raw(iso_box("jxlc", codestream({}))).b,
	                         container({iso_box("jxlc", codestream({})), builder().be32(400).raw("xml ").raw("cut").b})})
	{
		libfiles::reader r(file);
		const auto plan = libfiles::plan_update(r, {&meta});
		CHECK(!plan.ok());
		CHECK(plan.replacements.empty());
	}

	// Nor is any cut of one, which never throws.
	const auto whole = update(container({iso_box("Exif", exif_contents(oriented_tiff(1))), iso_box("jxlc", codestream({}))}), rated(1)).file;
	libfiles::metadata_update u{&meta};
	u.legacy.rating = 3;

	for (std::size_t cut = 16; cut < whole.size(); ++cut)
	{
		const bytes part(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut));
		libfiles::reader r(part);
		const auto plan = libfiles::plan_update(r, u);
		if (plan.ok() && !libfiles::scan(r).success) CHECK(!"a cut the scan fails was planned");
	}
}

#if defined(LIBFILES_JXL) && defined(LIBFILES_JPEG)

namespace
{
	// Brotli for the boxes libjxl compresses, as an application gives it.
	std::optional<bytes> real_brotli(const std::span<const std::uint8_t> compressed, std::string_view, const std::size_t max_size)
	{
		bytes out(max_size);
		auto size = out.size();
		if (BrotliDecoderDecompress(compressed.size(), compressed.data(), &size, out.data()) != BROTLI_DECODER_RESULT_SUCCESS) return {};
		out.resize(size);
		return out;
	}

	bytes make_jpeg(const libfiles::embedded_metadata& metadata)
	{
		constexpr std::uint32_t width = 48;
		constexpr std::uint32_t height = 32;
		bytes pixels(std::size_t{width} * height * 4);

		for (std::uint32_t y = 0; y < height; ++y)
		{
			for (std::uint32_t x = 0; x < width; ++x)
			{
				auto* const p = pixels.data() + (std::size_t{y} * width + x) * 4;
				p[0] = static_cast<std::uint8_t>(x * 5);
				p[1] = static_cast<std::uint8_t>(y * 7);
				p[2] = static_cast<std::uint8_t>(255 - x * y);
			}
		}

		libfiles::jpeg_encoder encoder;
		return encoder.encode(width, height, pixels.data(), std::size_t{width} * 4, 1, metadata, {});
	}

	// The JPEG XL libjxl makes from a JPEG, keeping what it needs to give the JPEG back.
	bytes transcode(const bytes& jpeg, const bool compress_boxes)
	{
		auto* const encoder = JxlEncoderCreate(nullptr);
		bytes file;

		if (JxlEncoderUseContainer(encoder, JXL_TRUE) == JXL_ENC_SUCCESS && JxlEncoderStoreJPEGMetadata(encoder, JXL_TRUE) == JXL_ENC_SUCCESS)
		{
			auto* const settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
			JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_JPEG_COMPRESS_BOXES, compress_boxes ? 1 : 0);

			if (JxlEncoderAddJPEGFrame(settings, jpeg.data(), jpeg.size()) == JXL_ENC_SUCCESS)
			{
				JxlEncoderCloseInput(encoder);
				file.resize(4096);
				auto* next = file.data();
				auto available = file.size();
				auto status = JXL_ENC_NEED_MORE_OUTPUT;

				while ((status = JxlEncoderProcessOutput(encoder, &next, &available)) == JXL_ENC_NEED_MORE_OUTPUT)
				{
					const auto done = static_cast<std::size_t>(next - file.data());
					file.resize(file.size() * 2);
					next = file.data() + done;
					available = file.size() - done;
				}

				file.resize(status == JXL_ENC_SUCCESS ? static_cast<std::size_t>(next - file.data()) : 0);
			}
		}

		JxlEncoderDestroy(encoder);
		return file;
	}

	// The JPEG libjxl gives back from a JPEG XL, or nothing when it cannot.
	std::optional<bytes> reconstruct(const bytes& file)
	{
		auto* const decoder = JxlDecoderCreate(nullptr);
		std::optional<bytes> out;
		bytes jpeg;

		if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS &&
			JxlDecoderSetInput(decoder, file.data(), file.size()) == JXL_DEC_SUCCESS)
		{
			JxlDecoderCloseInput(decoder);
			auto reconstructing = false;

			for (;;)
			{
				const auto status = JxlDecoderProcessInput(decoder);

				if (status == JXL_DEC_JPEG_RECONSTRUCTION)
				{
					reconstructing = true;
					jpeg.resize(4096);
					if (JxlDecoderSetJPEGBuffer(decoder, jpeg.data(), jpeg.size()) != JXL_DEC_SUCCESS) break;
				}
				else if (status == JXL_DEC_JPEG_NEED_MORE_OUTPUT)
				{
					const auto used = jpeg.size() - JxlDecoderReleaseJPEGBuffer(decoder);
					jpeg.resize(jpeg.size() * 2);
					if (JxlDecoderSetJPEGBuffer(decoder, jpeg.data() + used, jpeg.size() - used) != JXL_DEC_SUCCESS) break;
				}
				else if (status == JXL_DEC_FULL_IMAGE)
				{
					if (reconstructing)
					{
						jpeg.resize(jpeg.size() - JxlDecoderReleaseJPEGBuffer(decoder));
						out = jpeg;
					}

					break;
				}
				else
				{
					break;
				}
			}
		}

		JxlDecoderDestroy(decoder);
		return out;
	}

	// A JPEG from its first scan to its end: the picture, whatever its segments before it hold.
	bytes entropy_coded(const bytes& jpeg)
	{
		std::size_t pos = 2;

		while (pos + 4 <= jpeg.size() && jpeg[pos] == 0xFF)
		{
			if (jpeg[pos + 1] == 0xDA) return {jpeg.begin() + static_cast<std::ptrdiff_t>(pos), jpeg.end()};
			pos += 2 + (std::size_t{jpeg[pos + 2]} << 8 | jpeg[pos + 3]);
		}

		return {};
	}

	struct pixels_sink final : libfiles::image_sink
	{
		bytes memory;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			memory.assign(std::size_t{4} * frame.width * frame.height, 0);
			return {memory.data(), std::size_t{4} * frame.width};
		}
	};
}

TEST("a jpeg xl libjxl made from a jpeg still gives the jpeg back after an edit")
{
	const auto exif = camera_exif();
	const auto packet = packet_bytes(rated(1));
	libfiles::embedded_metadata metadata;
	metadata.exif = exif;
	metadata.xmp = packet;
	const auto jpeg = make_jpeg(metadata);
	CHECK(!jpeg.empty());

	libfiles::scan_limits limits;
	limits.brotli = real_brotli;

	for (const auto compress : {true, false})
	{
		const auto file = transcode(jpeg, compress);
		CHECK(!file.empty());
		CHECK(reconstruct(file) == jpeg);
		CHECK(types_of(file).find("jbrd") != std::string::npos);
		CHECK((types_of(file).find("brob") != std::string::npos) == compress);

		const auto meta = rated(5);
		libfiles::metadata_update u{&meta};
		u.legacy.rating = 5;
		u.legacy.date_original = "2021-06-15T10:20:30";
		u.legacy.description = "A description far longer than the one the camera wrote";
		u.brotli = real_brotli;

		const auto edited = update(file, u);
		CHECK_STR(edited.plan.error, "");
		CHECK(edited.plan.patchable() == !compress);
		CHECK_EQ(rating_of(edited.file, limits), 5);
		CHECK(same_moment(exif_properties(edited.file, limits).created, 2021, 6, 15, 10, 20, 30));

		// The JPEG it gives back is the one it was made from, with the edit in its packet and in the EXIF
		// that had room for it.
		const auto back = reconstruct(edited.file);
		CHECK(back.has_value());
		if (!back) continue;

		CHECK_EQ(back->size(), jpeg.size());
		CHECK(entropy_coded(*back) == entropy_coded(jpeg));
		CHECK(!entropy_coded(jpeg).empty());
		CHECK_EQ(rating_of(*back), 5);
		const auto back_exif = exif_properties(*back);
		CHECK(same_moment(back_exif.created, 2021, 6, 15, 10, 20, 30));
		CHECK_STR(back_exif.description, "Short");

		// And libfiles decodes its picture as before.
		pixels_sink before;
		pixels_sink after;
		libfiles::reader before_reader(file);
		libfiles::reader after_reader(edited.file);
		CHECK(libfiles::decode(before_reader, before).success);
		CHECK(libfiles::decode(after_reader, after).success);
		CHECK(!before.memory.empty() && before.memory == after.memory);

		// A packet the JPEG's segment has no room for is refused.
		libfiles::xmp::meta crowded = rated(5);
		crowded.set_property(libfiles::xmp::ns::xmp, "Label", std::string(4000, 'x'));
		libfiles::metadata_update too_much{&crowded};
		too_much.brotli = real_brotli;
		const auto refused = update(file, too_much);
		CHECK(!refused.plan.ok());
		CHECK(refused.plan.error.find("no room") != std::string::npos);
	}
}

#endif
