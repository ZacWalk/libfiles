// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The HEIF scanner - HEIC, AVIF and the other ISO base media still images - and the read of
// a HEIF file's XMP packet. Everything comes from the 'meta' box and the items it locates: the coded
// image is never read, except for a JPEG item's frame header. What a file reads as, and which files
// read at all, follow libheif 1.23 as Diffractor builds it, which scanned these files before: the
// primary item's size after its transforms, libheif's preferred decoding chroma, how it attaches
// metadata items, thumbnails and auxiliary images, and the faults for which it refuses a whole file.

#include "bmff_internal.h"
#include "scan_internal.h"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		using bmff::fourcc;

		// libheif's security limits, which decide what it refuses.
		constexpr std::uint64_t max_items = 1000;
		constexpr std::size_t max_extents = 32;
		constexpr std::size_t max_children = 100;
		constexpr std::uint64_t max_brands = 1000;
		constexpr std::uint64_t max_file_position = 0x007FFFFFFFFFFFFFull;
		constexpr std::uint64_t max_large_box = 0x0FFFFFFFFFFFFFFFull;
		constexpr std::uint64_t max_colour_profile = 100ull * 1024 * 1024;
		constexpr std::uint64_t max_components = 256;
		constexpr std::uint32_t max_pixel_size = 256;
		constexpr std::size_t max_overlay_images = 5;
		constexpr std::uint32_t max_entity_group = 64;
		constexpr int max_box_nesting = 20;
		constexpr std::int64_t max_fraction = 0x10000;

		// libheif reads the file type box from the first kilobyte, and looks at a top-level box only
		// while 32 bytes of the file remain from its start.
		constexpr std::uint64_t ftyp_window = 1024;
		constexpr std::uint64_t top_box_window = 32;

		// libfiles' own bounds, where libheif has none.
		constexpr std::uint64_t max_meta_box = 64ull * 1024 * 1024;
		constexpr std::size_t max_top_level_boxes = 65536;
		constexpr std::size_t max_metadata_block = 16u * 1024u * 1024u;

		// A JPEG item's frame header is looked for in growing reads rather than by reading the item.
		constexpr std::size_t jpeg_search_step = 64u * 1024u;

		constexpr auto box_ftyp = fourcc("ftyp");
		constexpr auto box_meta = fourcc("meta");
		constexpr auto box_moov = fourcc("moov");
		constexpr auto box_mini = fourcc("mini");
		constexpr auto box_hdlr = fourcc("hdlr");
		constexpr auto box_pitm = fourcc("pitm");
		constexpr auto box_iloc = fourcc("iloc");
		constexpr auto box_iinf = fourcc("iinf");
		constexpr auto box_infe = fourcc("infe");
		constexpr auto box_iprp = fourcc("iprp");
		constexpr auto box_ipco = fourcc("ipco");
		constexpr auto box_ipma = fourcc("ipma");
		constexpr auto box_iref = fourcc("iref");
		constexpr auto box_idat = fourcc("idat");
		constexpr auto box_uuid = fourcc("uuid");
		constexpr auto box_grpl = fourcc("grpl");
		constexpr auto box_dinf = fourcc("dinf");
		constexpr auto box_dref = fourcc("dref");
		constexpr auto box_url = fourcc("url ");

		constexpr auto group_altr = fourcc("altr");
		constexpr auto group_ster = fourcc("ster");
		constexpr auto group_pymd = fourcc("pymd");

		constexpr auto prop_ispe = fourcc("ispe");
		constexpr auto prop_irot = fourcc("irot");
		constexpr auto prop_imir = fourcc("imir");
		constexpr auto prop_clap = fourcc("clap");
		constexpr auto prop_colr = fourcc("colr");
		constexpr auto prop_pasp = fourcc("pasp");
		constexpr auto prop_pixi = fourcc("pixi");
		constexpr auto prop_auxc = fourcc("auxC");
		constexpr auto prop_hvcc = fourcc("hvcC");
		constexpr auto prop_av1c = fourcc("av1C");
		constexpr auto prop_avcc = fourcc("avcC");
		constexpr auto prop_vvcc = fourcc("vvcC");
		constexpr auto prop_jpgc = fourcc("jpgC");
		constexpr auto prop_j2kh = fourcc("j2kH");
		constexpr auto prop_mskc = fourcc("mskC");
		constexpr auto prop_uncc = fourcc("uncC");
		constexpr auto prop_cmpd = fourcc("cmpd");
		constexpr auto prop_rref = fourcc("rref");
		constexpr auto prop_cmin = fourcc("cmin");
		constexpr auto prop_iscl = fourcc("iscl");
		constexpr auto prop_a1op = fourcc("a1op");
		constexpr auto prop_a1lx = fourcc("a1lx");
		constexpr auto prop_taic = fourcc("taic");
		constexpr auto prop_itai = fourcc("itai");

		constexpr auto item_hvc1 = fourcc("hvc1");
		constexpr auto item_av01 = fourcc("av01");
		constexpr auto item_avc1 = fourcc("avc1");
		constexpr auto item_vvc1 = fourcc("vvc1");
		constexpr auto item_jpeg = fourcc("jpeg");
		constexpr auto item_mime = fourcc("mime");
		constexpr auto item_unci = fourcc("unci");
		constexpr auto item_j2k1 = fourcc("j2k1");
		constexpr auto item_lhv1 = fourcc("lhv1");
		constexpr auto item_mski = fourcc("mski");
		constexpr auto item_grid = fourcc("grid");
		constexpr auto item_iovl = fourcc("iovl");
		constexpr auto item_iden = fourcc("iden");
		constexpr auto item_tili = fourcc("tili");
		constexpr auto item_rgan = fourcc("rgan");
		constexpr auto item_exif = fourcc("Exif");
		constexpr auto item_uri = fourcc("uri ");

		constexpr auto ref_thmb = fourcc("thmb");
		constexpr auto ref_auxl = fourcc("auxl");
		constexpr auto ref_cdsc = fourcc("cdsc");
		constexpr auto ref_dimg = fourcc("dimg");
		constexpr auto ref_prem = fourcc("prem");
		constexpr auto ref_text = fourcc("text");

		constexpr std::string_view jpeg_content_type = "image/jpeg";

		// Every box type libheif parses. Any other property it keeps unparsed, and refuses the file when
		// an image item holds one as essential.
		constexpr std::uint32_t libheif_box_types[] = {
			fourcc("ftyp"), fourcc("free"), fourcc("skip"), fourcc("meta"), fourcc("hdlr"), fourcc("pitm"),
			fourcc("iloc"), fourcc("iinf"), fourcc("infe"), fourcc("iprp"), fourcc("ipco"), fourcc("ipma"),
			fourcc("ispe"), fourcc("auxC"), fourcc("irot"), fourcc("imir"), fourcc("clap"), fourcc("iscl"),
			fourcc("iref"), fourcc("rref"), fourcc("hvcC"), fourcc("hvc1"), fourcc("av1C"), fourcc("av01"),
			fourcc("vvcC"), fourcc("vvc1"), fourcc("idat"), fourcc("grpl"), fourcc("pymd"), fourcc("altr"),
			fourcc("ster"), fourcc("dinf"), fourcc("dref"), fourcc("url "), fourcc("colr"), fourcc("pixi"),
			fourcc("pasp"), fourcc("lsel"), fourcc("a1op"), fourcc("a1lx"), fourcc("clli"), fourcc("mdcv"),
			fourcc("amve"), fourcc("ndwt"), fourcc("cmin"), fourcc("cmex"), fourcc("udes"), fourcc("jpgC"),
			fourcc("mjpg"), fourcc("elng"), fourcc("cmpd"), fourcc("uncC"), fourcc("cmpC"), fourcc("icef"),
			fourcc("cpat"), fourcc("splz"), fourcc("sbpm"), fourcc("snuc"), fourcc("cloc"), fourcc("uncv"),
			fourcc("j2kH"), fourcc("cdef"), fourcc("cmap"), fourcc("pclr"), fourcc("j2kL"), fourcc("j2ki"),
			fourcc("mskC"), fourcc("itai"), fourcc("taic"), fourcc("avcC"), fourcc("avc1"), fourcc("mini"),
			fourcc("mdat"), fourcc("moov"), fourcc("mvhd"), fourcc("trak"), fourcc("tkhd"), fourcc("mdia"),
			fourcc("mdhd"), fourcc("minf"), fourcc("vmhd"), fourcc("stbl"), fourcc("stsd"), fourcc("stts"),
			fourcc("ctts"), fourcc("stsc"), fourcc("stco"), fourcc("stsz"), fourcc("stss"), fourcc("ccst"),
			fourcc("auxi"), fourcc("edts"), fourcc("elst"), fourcc("sbgp"), fourcc("sgpd"), fourcc("btrt"),
			fourcc("saiz"), fourcc("saio"), fourcc("urim"), fourcc("uri "), fourcc("nmhd"), fourcc("tref"),
			fourcc("sdtp"), fourcc("prfr"),
		};

		// The 'uuid' properties libheif parses: cmin, cmex and the two GIMI content identifiers.
		constexpr std::uint8_t libheif_uuids[][16] = {
			{0x22, 0xcc, 0x04, 0xc7, 0xd6, 0xd9, 0x4e, 0x07, 0x9d, 0x90, 0x4e, 0xb6, 0xec, 0xba, 0xf3, 0xa3},
			{0x43, 0x63, 0xe9, 0x14, 0x5b, 0x7d, 0x4a, 0xab, 0x97, 0xae, 0xbe, 0xa6, 0x98, 0x03, 0xb4, 0x34},
			{0x26, 0x1e, 0xf3, 0x74, 0x1d, 0x97, 0x5b, 0xba, 0xac, 0xbd, 0x9d, 0x2c, 0x8e, 0xa7, 0x35, 0x22},
			{0x9d, 0xb9, 0xdd, 0x6e, 0x37, 0x3c, 0x5a, 0x4e, 0x81, 0x10, 0x21, 0xfc, 0x83, 0xa9, 0x11, 0xfd},
		};

		constexpr std::size_t uuid_cmin = 0;
		constexpr std::size_t uuid_component_ids = 3;

		// The reference types libheif processes; an item whose 'rref' requires any other cannot be decoded.
		constexpr std::uint32_t supported_references[] = {
			fourcc("thmb"), fourcc("base"), fourcc("cdsc"), fourcc("dimg"), fourcc("auxl"), fourcc("prem"),
		};

		using bmff::fourcc_text;

		// A four-character code as libheif hands it to C, which ends it at its first NUL.
		std::string_view c_text(const std::string& text)
		{
			return std::string_view(text).substr(0, text.find('\0'));
		}

		bool equal_ignoring_case(const std::string_view a, const std::string_view b)
		{
			const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
			return a.size() == b.size() &&
				std::equal(a.begin(), a.end(), b.begin(), [&lower](const char x, const char y) { return lower(x) == lower(y); });
		}

		// A box's payload read as libheif's BitstreamRange reads one: a read past the end yields zero and
		// marks the range failed, so a box is judged on whether it was read whole.
		class cursor
		{
		public:
			explicit cursor(const std::span<const std::uint8_t> data) : _data(data)
			{
			}

			bool failed() const { return _failed; }
			bool at_end() const { return _pos >= _data.size(); }
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
			std::uint32_t u32() { return static_cast<std::uint32_t>(read(4)); }
			std::uint64_t u64() { return read(8); }

			// iloc's field widths: four or eight bytes, and nothing read for any other width.
			std::uint64_t sized(const unsigned bytes)
			{
				return bytes == 4 || bytes == 8 ? read(bytes) : 0;
			}

			// A NUL-terminated string as libheif reads one: empty at the end of the box, and a string the
			// box ends inside loses its last character.
			std::string string()
			{
				std::string text;

				while (_pos < _data.size())
				{
					const auto c = _data[_pos++];
					if (c == 0 || _pos == _data.size()) break;
					text.push_back(static_cast<char>(c));
				}

				return text;
			}

			std::span<const std::uint8_t> rest()
			{
				const auto bytes = _data.subspan(_pos);
				_pos = _data.size();
				return bytes;
			}

			void skip(const std::size_t bytes)
			{
				if (remaining() < bytes)
				{
					_pos = _data.size();
					_failed = true;
					return;
				}

				_pos += bytes;
			}

			std::size_t position() const { return _pos; }

		private:
			std::span<const std::uint8_t> _data;
			std::size_t _pos = 0;
			bool _failed = false;
		};

		struct full_box
		{
			std::uint8_t version = 0;
			std::uint32_t flags = 0;
		};

		full_box read_full_box(cursor& c)
		{
			const auto word = c.u32();
			return {static_cast<std::uint8_t>(word >> 24), word & 0xffffffu};
		}

		struct child_box
		{
			std::uint32_t type = 0;
			// The size field: zero runs to the end of the box holding it.
			std::uint64_t declared = 0;
			std::span<const std::uint8_t> payload;
			std::uint64_t payload_offset = 0;
			const std::uint8_t* uuid = nullptr;
		};

		// The boxes a payload holds, split as libheif's read_children splits them: every byte must belong
		// to a whole box. When count is given, that many are read and anything after them is not.
		std::vector<child_box> read_children(const std::span<const std::uint8_t> data, const std::uint64_t offset,
		                                     const std::optional<std::size_t> count = {})
		{
			if (count && *count > data.size() / 8) throw format_error("a HEIF box declares more boxes than it can hold");

			std::vector<child_box> boxes;
			std::size_t pos = 0;

			while (pos < data.size() && (!count || boxes.size() < *count))
			{
				if (!count && boxes.size() > max_children) throw format_error("a HEIF box holds too many boxes");

				const auto left = data.size() - pos;
				bmff::box_header h;
				if (bmff::read_header(data.subspan(pos), h) != bmff::header_status::box) throw format_error("a HEIF box is cut short");

				child_box box;
				box.type = h.type;
				const auto size = h.size;
				std::size_t header = h.header;

				if (box.type == box_uuid)
				{
					if (left < header + 16) throw format_error("a HEIF box is cut short");
					box.uuid = data.data() + pos + header;
					header += 16;
				}

				if (size > max_large_box) throw format_error("a HEIF box is too large");

				std::uint64_t payload_size = left - header;

				if (size != 0)
				{
					if (size < header) throw format_error("a HEIF box is smaller than its header");
					payload_size = size - header;
				}

				if (payload_size > left - header) throw format_error("a HEIF box runs past the box holding it");

				box.declared = size;
				box.payload = data.subspan(pos + header, static_cast<std::size_t>(payload_size));
				box.payload_offset = offset + pos + header;
				boxes.push_back(box);
				pos += header + static_cast<std::size_t>(payload_size);
			}

			if (count && boxes.size() != *count) throw format_error("a HEIF box holds fewer boxes than it declares");
			return boxes;
		}

		// An nclx colour description. libheif's own default is all unspecified, which reads as none.
		struct nclx_profile
		{
			std::uint16_t primaries = 2;
			std::uint16_t transfer = 2;
			std::uint16_t matrix = 2;
			bool full_range = true;

			friend bool operator==(const nclx_profile&, const nclx_profile&) = default;
		};

		// libheif's heif_chroma values, which name the pixel formats.
		enum class chroma : int
		{
			monochrome = 0,
			c420 = 1,
			c422 = 2,
			c444 = 3,
			interleaved_rgb = 10,
			interleaved_rgba = 11,
			interleaved_rrggbb = 12,
			interleaved_rrggbbaa = 13,
			undefined = 99,
		};

		enum class colourspace : std::uint8_t
		{
			ycbcr,
			rgb,
			monochrome,
			undefined,
		};

		// A property libheif could not parse. Its fatal faults refuse the whole file as the box is read;
		// these two are kept, and an ignorable one refuses the file only when an image item uses it.
		enum class parse_fault : std::uint8_t
		{
			none,
			ignorable,
			optional,
		};

		struct unc_component
		{
			std::uint32_t index = 0;
			int bits = 0;
		};

		// One child of 'ipco', with what the scan needs from it.
		struct property
		{
			std::uint32_t type = 0;
			const std::uint8_t* uuid = nullptr;
			parse_fault fault = parse_fault::none;

			// ispe
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			// irot, in degrees anticlockwise
			std::uint32_t rotation = 0;
			// clap, the clean aperture rounded as libheif rounds it
			std::int64_t clap_width = 0;
			std::int64_t clap_height = 0;
			// pasp
			std::uint32_t h_spacing = 1;
			std::uint32_t v_spacing = 1;
			// colr
			std::uint32_t colour_type = 0;
			nclx_profile nclx;
			std::span<const std::uint8_t> icc;
			// auxC
			std::string aux_type;
			// The coding configuration: libheif's chroma and the bit depths.
			chroma coded_chroma = chroma::undefined;
			int luma_bits = -1;
			int chroma_bits = -1;
			// jpgC
			std::span<const std::uint8_t> data;
			// uncC and cmpd
			std::uint8_t version = 0;
			std::uint32_t profile = 0;
			std::vector<unc_component> components;
			std::uint8_t sampling = 0;
			std::vector<std::uint16_t> component_types;
			// rref: the reference types the item requires
			std::vector<std::uint32_t> required_references;
			// cmin, read whole: a camera model, which needs the image's size
			bool intrinsics = false;
		};

		struct item_entry
		{
			std::uint32_t id = 0;
			std::uint32_t type = 0;
			bool hidden = false;
			std::string content_type;
			std::string content_encoding;
		};

		struct extent
		{
			std::uint64_t offset = 0;
			std::uint64_t length = 0;
		};

		struct location
		{
			std::uint32_t item = 0;
			std::uint8_t method = 0;
			std::uint64_t base = 0;
			std::vector<extent> extents;
		};

		struct association
		{
			std::uint32_t index = 0;
			bool essential = false;
		};

		struct property_list
		{
			std::uint32_t item = 0;
			std::vector<association> associations;
		};

		struct reference
		{
			std::uint32_t type = 0;
			std::uint32_t from = 0;
			std::vector<std::uint32_t> to;
		};

		struct idat_box
		{
			std::uint64_t data_offset = 0;
			// The size field, header included, which is what libheif bounds a read with.
			std::uint64_t declared = 0;
		};

		void check_box(const child_box& box, int depth);

		item_entry parse_infe(const std::span<const std::uint8_t> payload)
		{
			cursor c(payload);
			const auto box = read_full_box(c);
			if (box.version > 3) throw format_error("a HEIF item entry has an unknown version");

			item_entry entry;

			if (box.version <= 1)
			{
				entry.id = c.u16();
				c.u16();
				c.string();
				entry.content_type = c.string();
				entry.content_encoding = c.string();
			}
			else
			{
				entry.hidden = (box.flags & 1u) != 0;
				entry.id = box.version == 2 ? c.u16() : c.u32();
				c.u16();
				entry.type = c.u32();
				c.string();

				if (entry.type == item_mime)
				{
					entry.content_type = c.string();
					entry.content_encoding = c.string();
				}
				else if (entry.type == item_uri)
				{
					c.string();
				}
			}

			if (c.failed()) throw format_error("a HEIF item entry is cut short");
			return entry;
		}

		std::vector<item_entry> parse_iinf(const child_box& box, const int depth)
		{
			cursor c(box.payload);
			const auto header = read_full_box(c);
			const std::uint32_t count = header.version > 0 ? c.u32() : c.u16();

			// libheif takes an item count it could not read as zero, and so an empty list.
			if (count == 0) return {};
			if (count > max_items) throw format_error("a HEIF file declares too many items");

			const auto start = c.position();
			std::vector<item_entry> entries;

			for (const auto& child : read_children(c.rest(), box.payload_offset + start, count))
			{
				if (child.type == box_infe) entries.push_back(parse_infe(child.payload));
				else check_box(child, depth + 1);
			}

			return entries;
		}

		std::vector<location> parse_iloc(const std::span<const std::uint8_t> payload)
		{
			cursor c(payload);
			const auto box = read_full_box(c);
			if (box.version > 2) throw format_error("the HEIF item locations have an unknown version");

			const auto sizes = c.u16();
			const unsigned offset_size = sizes >> 12 & 0xfu;
			const unsigned length_size = sizes >> 8 & 0xfu;
			const unsigned base_size = sizes >> 4 & 0xfu;
			const unsigned index_size = box.version >= 1 ? sizes & 0xfu : 0u;
			const std::uint32_t count = box.version < 2 ? c.u16() : c.u32();

			if (count > max_items) throw format_error("the HEIF item locations list too many items");

			std::vector<location> locations;

			for (std::uint32_t i = 0; i < count; ++i)
			{
				if (c.at_end()) throw format_error("the HEIF item locations are cut short");

				location item;
				item.item = box.version < 2 ? c.u16() : c.u32();
				if (box.version >= 1) item.method = static_cast<std::uint8_t>(c.u16() & 0xfu);
				c.u16();
				item.base = c.sized(base_size);

				const auto extent_count = c.u16();
				if (extent_count > max_extents) throw format_error("a HEIF item has too many extents");

				for (std::uint32_t e = 0; e < extent_count; ++e)
				{
					if (c.at_end()) throw format_error("the HEIF item locations are cut short");

					c.sized(index_size);
					extent x;
					x.offset = c.sized(offset_size);
					x.length = c.sized(length_size);
					item.extents.push_back(x);
				}

				if (!c.failed()) locations.push_back(std::move(item));
			}

			if (c.failed()) throw format_error("the HEIF item locations are cut short");
			return locations;
		}

		void parse_ipma(const std::span<const std::uint8_t> payload, std::vector<property_list>& lists)
		{
			cursor c(payload);
			const auto box = read_full_box(c);
			if (box.version > 1) throw format_error("the HEIF property associations have an unknown version");

			const auto count = c.u32();
			if (count > max_items) throw format_error("the HEIF property associations list too many items");

			for (std::uint32_t i = 0; i < count && !c.failed() && !c.at_end(); ++i)
			{
				property_list list;
				list.item = box.version < 1 ? c.u16() : c.u32();
				const auto associations = c.u8();

				for (unsigned k = 0; k < associations; ++k)
				{
					association a;

					if (box.flags & 1u)
					{
						const auto value = c.u16();
						a.essential = (value & 0x8000u) != 0;
						a.index = value & 0x7fffu;
					}
					else
					{
						const auto value = c.u8();
						a.essential = (value & 0x80u) != 0;
						a.index = value & 0x7fu;
					}

					list.associations.push_back(a);
				}

				lists.push_back(std::move(list));
			}

			if (c.failed()) throw format_error("the HEIF property associations are cut short");
		}

		std::vector<reference> parse_iref(const std::span<const std::uint8_t> payload)
		{
			cursor c(payload);
			const auto box = read_full_box(c);
			if (box.version > 1) throw format_error("the HEIF item references have an unknown version");

			const auto id_bytes = box.version == 0 ? 2u : 4u;
			std::vector<reference> references;

			while (!c.at_end())
			{
				if (references.size() >= max_items) throw format_error("a HEIF file has too many item references");

				// Each reference has a box header, but libheif never uses its size: the entries follow
				// one another however long they say they are.
				const auto size = c.u32();
				reference ref;
				ref.type = c.u32();
				if (size == 1) c.u64();
				if (ref.type == box_uuid) c.read(16);
				if (c.failed()) throw format_error("the HEIF item references are cut short");

				ref.from = static_cast<std::uint32_t>(c.read(id_bytes));
				const auto count = c.u16();
				if (count == 0) throw format_error("a HEIF item reference names no items");
				if (count > max_items) throw format_error("a HEIF item reference names too many items");

				for (unsigned i = 0; i < count; ++i)
				{
					if (c.at_end()) throw format_error("the HEIF item references are cut short");
					ref.to.push_back(static_cast<std::uint32_t>(c.read(id_bytes)));
				}

				references.push_back(std::move(ref));
			}

			if (c.failed()) throw format_error("the HEIF item references are cut short");
			return references;
		}

		// libheif's Fraction: both terms within 32 bits and a denominator that is not zero, reduced until
		// the denominator, and then the numerator, fit in 17 bits.
		std::optional<std::pair<std::int64_t, std::int64_t>> fraction(std::int64_t numerator, std::int64_t denominator)
		{
			constexpr std::int64_t low = std::numeric_limits<std::int32_t>::min();
			constexpr std::int64_t high = std::numeric_limits<std::int32_t>::max();

			if (numerator < low || numerator > high || denominator < low || denominator > high) return {};

			while (denominator > max_fraction || denominator < -max_fraction)
			{
				numerator /= 2;
				denominator /= 2;
			}

			while (denominator > 1 && (numerator > max_fraction || numerator < -max_fraction))
			{
				numerator /= 2;
				denominator /= 2;
			}

			if (denominator == 0) return {};
			return std::pair{numerator, denominator};
		}

		std::int64_t round_fraction(const std::pair<std::int64_t, std::int64_t> f)
		{
			return static_cast<std::int32_t>((f.first + f.second / 2) / f.second);
		}

		void parse_hvcc(cursor& c, property& p)
		{
			c.read(12);
			c.u8();
			c.u16();
			c.u8();
			p.coded_chroma = static_cast<chroma>(c.u8() & 3u);
			p.luma_bits = (c.u8() & 7) + 8;
			p.chroma_bits = (c.u8() & 7) + 8;
			c.u16();
			c.u8();

			const auto arrays = c.u8();

			for (unsigned i = 0; i < arrays && !c.failed(); ++i)
			{
				c.u8();
				const auto units = c.u16();

				for (unsigned u = 0; u < units && !c.failed(); ++u)
				{
					c.skip(c.u16());
				}
			}
		}

		void parse_avcc(cursor& c, property& p)
		{
			c.u8();
			const auto profile = c.u8();
			c.u8();
			c.u8();
			c.u8();

			p.coded_chroma = chroma::c420;
			p.luma_bits = 8;
			p.chroma_bits = 8;

			const auto sequence_sets = c.u8() & 0x1fu;
			for (unsigned i = 0; i < sequence_sets; ++i) c.skip(c.u16());

			const auto picture_sets = c.u8();
			for (unsigned i = 0; i < picture_sets; ++i) c.skip(c.u16());

			// The high profiles' extension, which states the chroma format and bit depths.
			if (c.remaining() > 0 && profile != 66 && profile != 77 && profile != 88)
			{
				p.coded_chroma = static_cast<chroma>(c.u8() & 3u);
				p.luma_bits = 8 + (c.u8() & 7);
				p.chroma_bits = 8 + (c.u8() & 7);

				const auto extensions = c.u8();
				for (unsigned i = 0; i < extensions; ++i) c.skip(c.u16());
			}
		}

		void parse_vvcc(cursor& c, property& p)
		{
			read_full_box(c);
			const auto flags = c.u8();
			auto bit_depth_minus8 = 0u;
			p.coded_chroma = chroma::monochrome;

			if (flags & 1u)
			{
				const auto word = c.u16();
				p.coded_chroma = static_cast<chroma>(word & 3u);
				bit_depth_minus8 = c.u8() >> 5 & 7u;

				const auto constraint_bytes = c.u8() & 0x3fu;
				if (!c.failed() && constraint_bytes == 0) throw format_error("a HEIF VVC configuration is malformed");

				c.u8();
				c.u8();
				c.skip(constraint_bytes);
			}

			p.luma_bits = (flags & 1u) ? static_cast<int>(bit_depth_minus8) + 8 : 8;
			p.chroma_bits = p.luma_bits;
		}

		void parse_uncc(cursor& c, property& p)
		{
			static constexpr std::uint32_t profiles[] = {
				fourcc("rgb3"), fourcc("rgba"), fourcc("abgr"), fourcc("2vuy"), fourcc("yuv2"), fourcc("yvyu"),
				fourcc("vyuy"), fourcc("yuv1"), fourcc("v308"), fourcc("v408"), fourcc("y210"), fourcc("v410"),
				fourcc("v210"), fourcc("i420"), fourcc("nv12"), fourcc("nv21"), fourcc("yu22"), fourcc("yv22"),
				fourcc("yv20"),
			};

			const auto box = read_full_box(c);
			p.version = box.version;
			p.profile = c.u32();

			if (box.version == 1)
			{
				if (!c.failed() && std::ranges::find(profiles, p.profile) == std::end(profiles))
				{
					throw format_error("a HEIF uncompressed image has an unknown profile");
				}
			}
			else if (box.version == 0)
			{
				const auto count = c.u32();
				if (count > max_components) throw format_error("a HEIF uncompressed image has too many components");

				for (std::uint32_t i = 0; i < count && !c.failed() && !c.at_end(); ++i)
				{
					unc_component component;
					component.index = c.u16();
					component.bits = c.u8() + 1;
					const auto format = c.u8();
					const auto align = c.u8();
					p.components.push_back(component);

					if (format > 3) throw format_error("a HEIF uncompressed image has an invalid component format");
					if (align != 0 && align * 8 < component.bits) throw format_error("a HEIF uncompressed image has an invalid alignment");
				}

				p.sampling = c.u8();
				if (p.sampling > 3) throw format_error("a HEIF uncompressed image has an invalid sampling mode");
				if (c.u8() > 5) throw format_error("a HEIF uncompressed image has an invalid interleave mode");

				c.u8();
				c.u8();
				if (c.u32() > max_pixel_size) throw format_error("a HEIF uncompressed image's pixels are too large");
				c.u32();
				c.u32();
				if (c.u32() == 0xffffffffu || c.u32() == 0xffffffffu) throw format_error("a HEIF uncompressed image has too many tiles");
			}
		}

		void parse_cmpd(cursor& c, property& p)
		{
			const auto count = c.u32();
			if (count > max_components) throw format_error("a HEIF component definition has too many components");

			for (std::uint32_t i = 0; i < count; ++i)
			{
				if (c.at_end()) throw format_error("a HEIF component definition is cut short");

				const auto type = c.u16();
				if (type >= 0x8000u) c.string();
				p.component_types.push_back(type);
			}
		}

		void parse_rref(cursor& c, property& p)
		{
			if (read_full_box(c).version > 1) throw format_error("a HEIF required references property has an unknown version");

			// The count is a byte, but libheif reads it as a word when the rest of the box is a whole
			// number of words, as two conformance files store it, and keeps its low byte.
			const auto words = c.remaining() > 0 && c.remaining() % 4 == 0;
			const auto count = static_cast<std::uint8_t>(words ? c.u32() : c.u8());

			for (unsigned i = 0; i < count; ++i) p.required_references.push_back(c.u32());
			if (c.failed()) throw format_error("a HEIF required references property is cut short");
		}

		// A camera model fault is optional to libheif; a model read whole needs the image's size.
		void parse_cmin(cursor& c, property& p)
		{
			const auto header = read_full_box(c);
			if (header.version > 0) return;

			c.skip(header.flags & 1u ? 20 : 12);
			p.intrinsics = !c.failed();
		}

		void parse_component_ids(cursor& c, property& p)
		{
			const auto count = c.u32();

			if (count > max_components)
			{
				p.fault = parse_fault::ignorable;
				return;
			}

			for (std::uint32_t i = 0; i < count && p.fault == parse_fault::none; ++i)
			{
				if (c.at_end()) p.fault = parse_fault::ignorable;
				else c.string();
			}

			if (c.failed()) p.fault = parse_fault::ignorable;
		}

		// An entity group as libheif parses one: the group, its entities, and how many it allows. It
		// reads past a group's end as zeros without complaint.
		bool entity_group_valid(const child_box& box)
		{
			cursor c(box.payload);
			read_full_box(c);
			if (c.failed()) return false;

			c.u32();
			const auto entities = c.u32();
			if (entities > c.remaining() / 4 || entities > max_entity_group) return false;
			return box.type != group_ster || entities == 2;
		}

		// One property, parsed as libheif parses it. A fault libheif treats as fatal throws, since it
		// refuses the file for it however the property is used.
		property parse_property(const child_box& box)
		{
			property p;
			p.type = box.type;
			p.uuid = box.uuid;
			cursor c(box.payload);

			const auto fatal_if_failed = [&c]
			{
				if (c.failed()) throw format_error("a HEIF item property is malformed");
			};

			if (box.type == box_uuid && box.uuid)
			{
				const auto is = [&box](const std::size_t which)
				{
					return std::equal(box.uuid, box.uuid + 16, libheif_uuids[which]);
				};

				if (is(uuid_cmin)) parse_cmin(c, p);
				else if (is(uuid_component_ids)) parse_component_ids(c, p);
				return p;
			}

			switch (box.type)
			{
			case prop_ispe:
				{
					if (read_full_box(c).version != 0) throw format_error("a HEIF image size has an unknown version");
					p.width = c.u32();
					p.height = c.u32();
					fatal_if_failed();
				}
				break;

			case prop_colr:
				{
					p.colour_type = c.u32();

					if (p.colour_type == fourcc("nclx") || p.colour_type == fourcc("nclc"))
					{
						p.nclx.primaries = c.u16();
						p.nclx.transfer = c.u16();
						p.nclx.matrix = c.u16();
						p.nclx.full_range = p.colour_type == fourcc("nclx") ? (c.u8() & 0x80u) != 0 : p.nclx.matrix == 0;
						fatal_if_failed();
					}
					else if (p.colour_type == fourcc("prof") || p.colour_type == fourcc("rICC"))
					{
						if (box.declared == 0) throw format_error("a HEIF colour profile has no size");
						if (c.remaining() > max_colour_profile) throw format_error("a HEIF colour profile is too large");
						p.icc = c.rest();
					}
					else
					{
						throw format_error("a HEIF colour description has an unknown type");
					}
				}
				break;

			case prop_auxc:
				{
					if (read_full_box(c).version != 0) throw format_error("a HEIF auxiliary type has an unknown version");
					p.aux_type = c.string();
					fatal_if_failed();
				}
				break;

			case prop_irot:
				p.rotation = (c.u8() & 3u) * 90u;
				if (c.failed()) p.fault = parse_fault::ignorable;
				break;

			case prop_imir:
				c.u8();
				if (c.failed()) p.fault = parse_fault::ignorable;
				break;

			case prop_clap:
				{
					const auto width_num = c.u32();
					const auto width_den = c.u32();
					const auto height_num = c.u32();
					const auto height_den = c.u32();
					const auto h_offset_num = static_cast<std::int32_t>(c.u32());
					const auto h_offset_den = c.u32();
					const auto v_offset_num = static_cast<std::int32_t>(c.u32());
					const auto v_offset_den = c.u32();

					const auto width = fraction(width_num, width_den);
					const auto height = fraction(height_num, height_den);

					if (c.failed() || !width || !height || !fraction(h_offset_num, h_offset_den) ||
						!fraction(v_offset_num, v_offset_den))
					{
						p.fault = parse_fault::ignorable;
						break;
					}

					p.clap_width = round_fraction(*width);
					p.clap_height = round_fraction(*height);
				}
				break;

			case prop_pasp:
				p.h_spacing = c.u32();
				p.v_spacing = c.u32();
				if (c.failed()) p.fault = parse_fault::optional;
				break;

			case prop_pixi:
				{
					if (read_full_box(c).version != 0)
					{
						p.fault = parse_fault::optional;
						break;
					}

					c.skip(c.u8());
					if (c.failed()) p.fault = parse_fault::optional;
				}
				break;

			case prop_hvcc:
				parse_hvcc(c, p);
				fatal_if_failed();
				break;

			case prop_av1c:
				{
					if (box.declared == 0) throw format_error("a HEIF AV1 configuration has no size");

					c.u8();
					c.u8();
					const auto flags = c.u8();
					c.u8();
					fatal_if_failed();

					const auto high_bitdepth = (flags >> 6 & 1u) != 0;
					const auto twelve_bit = (flags >> 5 & 1u) != 0;
					const auto monochrome = (flags >> 4 & 1u) != 0;
					const auto subsampling_x = flags >> 3 & 1u;
					const auto subsampling_y = flags >> 2 & 1u;

					if (monochrome) p.coded_chroma = chroma::monochrome;
					else if (subsampling_x == 1 && subsampling_y == 1) p.coded_chroma = chroma::c420;
					else if (subsampling_x == 1 && subsampling_y == 0) p.coded_chroma = chroma::c422;
					else if (subsampling_x == 0 && subsampling_y == 0) p.coded_chroma = chroma::c444;
					else p.coded_chroma = chroma::undefined;

					p.luma_bits = !high_bitdepth ? 8 : twelve_bit ? 12 : 10;
					p.chroma_bits = p.luma_bits;
				}
				break;

			case prop_avcc:
				parse_avcc(c, p);
				fatal_if_failed();
				break;

			case prop_vvcc:
				parse_vvcc(c, p);
				fatal_if_failed();
				break;

			case prop_jpgc:
				if (box.declared == 0) throw format_error("a HEIF JPEG configuration has no size");
				p.data = c.rest();
				break;

			case prop_mskc:
				read_full_box(c);
				p.luma_bits = c.u8();
				fatal_if_failed();
				break;

			case prop_uncc:
				parse_uncc(c, p);
				fatal_if_failed();
				break;

			case prop_cmpd:
				parse_cmpd(c, p);
				fatal_if_failed();
				break;

			case prop_rref:
				parse_rref(c, p);
				break;

			case prop_cmin:
				parse_cmin(c, p);
				break;

			case prop_iscl:
				{
					const auto header = read_full_box(c);
					const auto width_num = c.u16();
					const auto width_den = c.u16();
					const auto height_num = c.u16();
					const auto height_den = c.u16();

					if (header.version != 0 || width_num == 0 || width_den == 0 || height_num == 0 || height_den == 0 || c.failed())
					{
						p.fault = parse_fault::ignorable;
					}
				}
				break;

			// Properties libheif reads only to keep: a fault in one is the box cut short.
			case prop_a1op:
				c.skip(1);
				fatal_if_failed();
				break;

			case prop_a1lx:
				c.skip(c.u8() & 1u ? 12 : 6);
				fatal_if_failed();
				break;

			case prop_taic:
				c.skip(21);
				fatal_if_failed();
				break;

			case prop_itai:
				c.skip(13);
				fatal_if_failed();
				break;

			// A pyramid group libheif cannot parse is kept as a fault, which matters only to an image
			// that names the group as its property.
			case group_pymd:
				if (!entity_group_valid(box)) p.fault = parse_fault::ignorable;
				break;

			default:
				break;
			}

			return p;
		}

		bool libheif_parses(const property& p)
		{
			if (p.type == box_uuid)
			{
				return p.uuid && std::ranges::any_of(libheif_uuids, [&p](const auto& id) { return std::equal(id, id + 16, p.uuid); });
			}

			return std::ranges::find(libheif_box_types, p.type) != std::end(libheif_box_types);
		}

		// A box that is not a property, read as libheif reads it: by its type alone, wherever it sits, so
		// a box the scan has no use for still refuses the file when libheif could not parse it. False
		// for a property or a box libheif keeps unparsed. Of the boxes only a movie holds, none is read.
		bool check_structure(const child_box& box, const int depth)
		{
			if (depth > max_box_nesting) throw format_error("HEIF boxes are nested too deeply");

			const auto children = [&box, depth](const std::size_t skip, const std::optional<std::size_t> count = {})
			{
				for (const auto& child : read_children(box.payload.subspan(skip), box.payload_offset + skip, count))
				{
					check_box(child, depth + 1);
				}
			};

			cursor c(box.payload);

			switch (box.type)
			{
			case box_meta:
				if (read_full_box(c).version != 0) throw format_error("a HEIF meta box has an unknown version");
				if (c.failed()) throw format_error("a HEIF meta box is cut short");
				children(4);
				return true;

			case box_hdlr:
				if (read_full_box(c).version != 0) throw format_error("a HEIF handler box has an unknown version");
				c.skip(20);
				c.string();
				if (c.failed()) throw format_error("a HEIF handler box is cut short");
				return true;

			case box_pitm:
				{
					const auto header = read_full_box(c);
					if (header.version > 1) throw format_error("a HEIF primary item box has an unknown version");
					c.skip(header.version == 0 ? 2 : 4);
					if (c.failed()) throw format_error("a HEIF primary item box is cut short");
				}
				return true;

			case box_iloc:
				parse_iloc(box.payload);
				return true;

			case box_iinf:
				parse_iinf(box, depth);
				return true;

			case box_infe:
				parse_infe(box.payload);
				return true;

			case box_ipma:
				{
					std::vector<property_list> lists;
					parse_ipma(box.payload, lists);
				}
				return true;

			case box_iref:
				parse_iref(box.payload);
				return true;

			case box_iprp:
			case box_ipco:
			case box_grpl:
			case box_dinf:
				children(0);
				return true;

			case box_dref:
				{
					if (read_full_box(c).version != 0) throw format_error("a HEIF data reference box has an unknown version");
					const auto count = c.u32();
					if (c.failed()) throw format_error("a HEIF data reference box is cut short");
					if (count > max_children) throw format_error("a HEIF data reference box holds too many boxes");

					// libheif reads on through a count of zero, and refuses the box for any box it finds.
					if (count == 0 && !read_children(box.payload.subspan(8), box.payload_offset + 8).empty())
					{
						throw format_error("a HEIF data reference box holds more boxes than it declares");
					}

					if (count > 0) children(8, count);
				}
				return true;

			case box_url:
				{
					const auto header = read_full_box(c);
					if (header.version > 0) throw format_error("a HEIF data location has an unknown version");
					if (!(header.flags & 1u)) c.string();
					if (c.failed()) throw format_error("a HEIF data location is cut short");
				}
				return true;

			case group_altr:
			case group_ster:
				if (!entity_group_valid(box)) throw format_error("a HEIF entity group is malformed");
				return true;

			default:
				return false;
			}
		}

		void check_box(const child_box& box, const int depth)
		{
			if (!check_structure(box, depth)) parse_property(box);
		}

		// What libheif reads of a HEIF file's boxes: the file type, and the meta box's items, their
		// locations, properties and references.
		struct heif_boxes
		{
			std::uint64_t file_size = 0;
			std::uint32_t major_brand = 0;
			std::uint32_t minor_version = 0;
			std::vector<std::uint32_t> compatible_brands;

			// The meta box's payload, held while the scan runs.
			std::vector<std::uint8_t> meta;
			std::uint64_t meta_offset = 0;

			std::optional<std::uint32_t> handler;
			std::optional<std::uint32_t> primary;
			bool have_iinf = false;
			bool have_iloc = false;
			bool have_iprp = false;
			bool have_ipco = false;
			bool have_ipma = false;
			std::map<std::uint32_t, item_entry> items;
			std::vector<location> locations;
			std::vector<property> properties;
			std::vector<property_list> property_lists;
			// Each item's first list in property_lists.
			std::map<std::uint32_t, std::size_t> first_lists;
			std::optional<std::vector<reference>> references;
			std::optional<idat_box> idat;

			bool has_brand(const std::uint32_t brand) const
			{
				return std::ranges::find(compatible_brands, brand) != compatible_brands.end();
			}

			const item_entry* item(const std::uint32_t id) const
			{
				const auto it = items.find(id);
				return it == items.end() ? nullptr : &it->second;
			}

			std::uint32_t item_type(const std::uint32_t id) const
			{
				const auto* const entry = item(id);
				return entry ? entry->type : 0;
			}

			const location* location_of(const std::uint32_t id) const
			{
				const auto it = std::ranges::find(locations, id, &location::item);
				return it == locations.end() ? nullptr : &*it;
			}

			// The first reference of a type from an item, as libheif's get_references takes it.
			std::vector<std::uint32_t> references_from(const std::uint32_t id, const std::uint32_t type) const
			{
				if (!references) return {};

				for (const auto& ref : *references)
				{
					if (ref.from == id && ref.type == type) return ref.to;
				}

				return {};
			}

			// The property indices an item's first association list names, as libheif collects them.
			// False for an item with no list, or one that names a property ipco does not hold.
			bool properties_of(const std::uint32_t id, std::vector<std::uint32_t>& out) const
			{
				out.clear();
				const auto* const list = list_of(id);
				if (!list) return false;

				for (const auto& a : list->associations)
				{
					if (a.index > properties.size()) return false;
					if (a.index > 0) out.push_back(a.index - 1);
				}

				return true;
			}

			// Whether the item's first association with a property marks it essential. libheif looks
			// through every list, but finds the item's first, from which the property came.
			bool essential(const std::uint32_t id, const std::uint32_t index) const
			{
				const auto* const list = list_of(id);
				if (!list) return false;

				const auto a = std::ranges::find(list->associations, index + 1, &association::index);
				return a != list->associations.end() && a->essential;
			}

			const property_list* list_of(const std::uint32_t id) const
			{
				const auto it = first_lists.find(id);
				return it == first_lists.end() ? nullptr : &property_lists[it->second];
			}
		};

		void parse_ftyp(reader& r, heif_boxes& f, const std::uint64_t size, const std::size_t header)
		{
			if (size < 8 || size - 8 < header) throw format_error("the HEIF file type box is too small");

			const auto box = r.view(0, static_cast<std::size_t>(size));
			f.major_brand = load_be32(box.data() + header);
			f.minor_version = load_be32(box.data() + header + 4);

			const auto brands = (size - header - 8) / 4;
			if (brands > max_brands) throw format_error("the HEIF file type box lists too many brands");

			for (std::uint64_t i = 0; i < brands; ++i)
			{
				f.compatible_brands.push_back(load_be32(box.data() + header + 8 + 4 * i));
			}
		}

		void parse_iprp(heif_boxes& f, const child_box& iprp)
		{
			std::vector<property_list> lists;
			auto first_ipco = true;

			for (const auto& child : read_children(iprp.payload, iprp.payload_offset))
			{
				if (child.type == box_ipco)
				{
					std::vector<property> properties;

					for (const auto& box : read_children(child.payload, child.payload_offset))
					{
						check_structure(box, 3);
						properties.push_back(parse_property(box));
					}

					if (first_ipco) f.properties = std::move(properties);
					first_ipco = false;
					f.have_ipco = true;
				}
				else if (child.type == box_ipma)
				{
					parse_ipma(child.payload, lists);
					f.have_ipma = true;
				}
				else
				{
					check_box(child, 2);
				}
			}

			f.property_lists = std::move(lists);
			for (std::size_t i = 0; i < f.property_lists.size(); ++i) f.first_lists.try_emplace(f.property_lists[i].item, i);
		}

		void parse_meta(heif_boxes& f)
		{
			cursor c(f.meta);
			if (read_full_box(c).version != 0) throw format_error("the HEIF meta box has an unknown version");

			auto first_iloc = true;
			auto first_iinf = true;
			auto first_iprp = true;

			for (const auto& child : read_children(c.rest(), f.meta_offset + 4))
			{
				switch (child.type)
				{
				case box_hdlr:
					{
						cursor h(child.payload);
						if (read_full_box(h).version != 0) throw format_error("the HEIF handler box has an unknown version");
						h.u32();
						const auto handler = h.u32();
						h.read(12);
						h.string();
						if (h.failed()) throw format_error("the HEIF handler box is cut short");
						if (!f.handler) f.handler = handler;
					}
					break;

				case box_pitm:
					{
						cursor p(child.payload);
						const auto box = read_full_box(p);
						if (box.version > 1) throw format_error("the HEIF primary item box has an unknown version");
						const std::uint32_t id = box.version == 0 ? p.u16() : p.u32();
						if (p.failed()) throw format_error("the HEIF primary item box is cut short");
						if (!f.primary) f.primary = id;
					}
					break;

				case box_iloc:
					{
						auto locations = parse_iloc(child.payload);
						if (first_iloc) f.locations = std::move(locations);
						first_iloc = false;
						f.have_iloc = true;
					}
					break;

				case box_iinf:
					{
						auto entries = parse_iinf(child, 1);

						if (first_iinf)
						{
							// A second entry for an item is ignored, as libheif's map keeps the first.
							for (auto& entry : entries) f.items.try_emplace(entry.id, std::move(entry));
						}

						first_iinf = false;
						f.have_iinf = true;
					}
					break;

				case box_iprp:
					if (first_iprp) parse_iprp(f, child);
					else check_box(child, 1);
					first_iprp = false;
					f.have_iprp = true;
					break;

				case box_iref:
					{
						auto references = parse_iref(child.payload);
						if (!f.references) f.references = std::move(references);
					}
					break;

				case box_idat:
					if (!f.idat) f.idat = idat_box{child.payload_offset, child.declared};
					break;

				default:
					check_box(child, 1);
					break;
				}
			}
		}

		// The file's boxes, read as libheif's file layout and parser read them: the file type box from
		// the first kilobyte, then every top-level box header, with the last meta box the one taken.
		heif_boxes read_boxes(reader& r)
		{
			heif_boxes f;
			f.file_size = r.size();

			if (f.file_size < top_box_window) throw format_error("the HEIF file is too small");

			bmff::box_header ftyp;
			bmff::read_header(r, 0, f.file_size, ftyp);
			if (ftyp.type != box_ftyp) throw format_error("the HEIF file does not start with a file type box");

			const auto ftyp_size = ftyp.size;
			const std::size_t ftyp_header = ftyp.header;

			if (ftyp_size == 0) throw format_error("the HEIF file type box is the only box");
			if (ftyp_size > std::min(f.file_size, ftyp_window)) throw format_error("the HEIF file type box is too large");
			if (ftyp_size < ftyp_header) throw format_error("the HEIF file type box is smaller than its header");

			parse_ftyp(r, f, ftyp_size, ftyp_header);

			auto found_meta = false;
			auto found_moov = false;
			std::vector<std::pair<std::uint64_t, std::uint64_t>> metas;
			std::uint64_t next = ftyp_size;

			for (std::size_t boxes = 0;; ++boxes)
			{
				if (boxes == max_top_level_boxes) throw format_error("the HEIF file has too many boxes");

				if (f.file_size - next < top_box_window)
				{
					if (!found_meta && !found_moov) throw format_error("the HEIF file has no meta box");
					break;
				}

				// The window holds a whole header, in either form.
				bmff::box_header h;
				bmff::read_header(r.view(next, static_cast<std::size_t>(top_box_window)), h);
				const auto size = h.size;
				const auto type = h.type;
				const std::uint64_t header = h.header;
				if (h.large() && size > max_large_box) throw format_error("a HEIF top-level box is too large");

				if (type == box_meta || type == box_moov || type == box_mini)
				{
					const auto end = size == 0 ? f.file_size : next + size;
					if (size != 0 && size > f.file_size - next) throw format_error("a HEIF top-level box runs past the end of the file");
					if (end - next < header) throw format_error("a HEIF top-level box is smaller than its header");

					if (type == box_mini) throw format_error("a low-overhead HEIF file is not read");

					if (type == box_meta)
					{
						metas.emplace_back(next + header, end);
						found_meta = true;
					}
					else
					{
						found_moov = true;
					}
				}

				if (size == 0)
				{
					if (!found_meta && !found_moov) throw format_error("the HEIF file has no meta box");
					break;
				}

				// A box smaller than its header steps on into its own bytes, as libheif steps; the
				// walk is bounded by its count.
				if (size > f.file_size - next) break;
				next += size;
			}

			const auto sequence = f.has_brand(fourcc("msf1")) || f.has_brand(fourcc("isom")) || f.has_brand(fourcc("mp41")) ||
				f.has_brand(fourcc("mp42"));

			const auto supported = f.has_brand(fourcc("heic")) || f.has_brand(fourcc("heix")) || f.has_brand(fourcc("mif1")) ||
				f.has_brand(fourcc("avif")) || f.has_brand(fourcc("1pic")) || f.major_brand == fourcc("mif3") ||
				f.has_brand(fourcc("jpeg")) || sequence;

			if (!supported) throw format_error("the HEIF file names no brand libheif reads");
			if (!found_meta && !sequence) throw format_error("the HEIF file has no meta box");
			if (!found_moov && sequence) throw format_error("the HEIF sequence has no movie box");
			if (!found_meta) throw format_error("the HEIF file holds no still image");

			// libheif parses every meta box as it comes to it and keeps the last, so an earlier one is
			// read only to be checked.
			std::uint64_t meta_bytes = 0;

			for (const auto& [start, end] : metas)
			{
				if (end - start > max_meta_box - meta_bytes) throw format_error("the HEIF meta box is too large to read");
				meta_bytes += end - start;
			}

			for (std::size_t i = 0; i + 1 < metas.size(); ++i)
			{
				const auto [start, end] = metas[i];
				const auto payload = r.read(start, static_cast<std::size_t>(end - start));
				check_structure({.type = box_meta, .declared = end - start, .payload = payload, .payload_offset = start}, 0);
			}

			const auto [meta_start, meta_end] = metas.back();
			f.meta_offset = meta_start;
			f.meta = r.read(meta_start, static_cast<std::size_t>(meta_end - meta_start));
			parse_meta(f);

			if (!f.have_iinf) throw format_error("the HEIF file has no item information");

			if (f.handler == fourcc("pict"))
			{
				if (!f.primary) throw format_error("the HEIF file names no primary item");
				if (!f.have_iprp) throw format_error("the HEIF file has no item properties");
				if (!f.have_ipco) throw format_error("the HEIF file has no property container");
				if (!f.have_ipma) throw format_error("the HEIF file has no property associations");
			}

			if (!f.have_iloc) throw format_error("the HEIF file has no item locations");
			if (f.handler != fourcc("pict")) throw format_error("the HEIF file holds no images");

			return f;
		}

		enum class data_status : std::uint8_t
		{
			ok,
			// libheif cannot read it, which costs the item rather than the file.
			unsupported,
			fault,
		};

		// What libheif's read of an item's data through 'iloc' would do, and the size it would read.
		// Nothing is read here.
		data_status check_item_data(const heif_boxes& f, const std::uint32_t id, std::uint64_t& size)
		{
			size = 0;
			const auto* const loc = f.location_of(id);
			if (!loc) return data_status::fault;

			for (const auto& e : loc->extents)
			{
				// An extent of length zero would mean "to the end of the data"; libheif reads nothing.
				if (e.length == 0) continue;

				if (loc->method == 0)
				{
					if (e.offset > max_file_position || loc->base > max_file_position || e.length > max_file_position)
					{
						return data_status::fault;
					}

					if (e.offset + loc->base + e.length > f.file_size) return data_status::fault;
				}
				else if (loc->method == 1)
				{
					if (!f.idat) return data_status::fault;

					// libheif bounds a read from 'idat' by the box's size field, its header included.
					const auto start = e.offset + loc->base;
					const auto box_size = f.idat->declared;

					if (start < e.offset || e.length > box_size || start > box_size - e.length) return data_status::fault;
					if (f.idat->data_offset + start + e.length > f.file_size) return data_status::fault;
				}
				else
				{
					return data_status::unsupported;
				}

				size += e.length;
			}

			return data_status::ok;
		}

		// Bytes of the file, from the meta box held in memory where they lie in it: 'idat' data does.
		std::vector<std::uint8_t> read_bytes(reader& r, const heif_boxes& f, const std::uint64_t at, const std::size_t count)
		{
			if (at >= f.meta_offset && at - f.meta_offset <= f.meta.size() && count <= f.meta.size() - (at - f.meta_offset))
			{
				const auto begin = f.meta.begin() + static_cast<std::ptrdiff_t>(at - f.meta_offset);
				return {begin, begin + static_cast<std::ptrdiff_t>(count)};
			}

			return r.read(at, count);
		}

		// Up to limit bytes from the start of an item's data, which check_item_data has passed.
		std::vector<std::uint8_t> read_item_data(reader& r, const heif_boxes& f, const std::uint32_t id, const std::uint64_t limit)
		{
			std::vector<std::uint8_t> out;
			const auto* const loc = f.location_of(id);
			if (!loc) return out;

			for (const auto& e : loc->extents)
			{
				if (e.length == 0) continue;
				if (out.size() >= limit) break;

				const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(e.length, limit - out.size()));
				const auto at = loc->method == 0 ? loc->base + e.offset : f.idat->data_offset + loc->base + e.offset;
				const auto bytes = read_bytes(r, f, at, n);
				out.insert(out.end(), bytes.begin(), bytes.end());
			}

			return out;
		}

		// The bytes that come after the first `skip` of an item's data, up to `count` of them.
		std::vector<std::uint8_t> read_item_range(reader& r, const heif_boxes& f, const std::uint32_t id, std::uint64_t skip,
		                                          const std::uint64_t count)
		{
			std::vector<std::uint8_t> out;
			const auto* const loc = f.location_of(id);
			if (!loc) return out;

			for (const auto& e : loc->extents)
			{
				if (e.length == 0) continue;

				if (skip >= e.length)
				{
					skip -= e.length;
					continue;
				}

				const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(e.length - skip, count - out.size()));
				const auto at = (loc->method == 0 ? loc->base + e.offset : f.idat->data_offset + loc->base + e.offset) + skip;
				const auto bytes = read_bytes(r, f, at, n);
				out.insert(out.end(), bytes.begin(), bytes.end());
				skip = 0;

				if (out.size() >= count) break;
			}

			return out;
		}

		enum class codec : std::uint8_t
		{
			hevc,
			av1,
			avc,
			vvc,
			jpeg,
			j2k,
			unci,
			mask,
			grid,
			overlay,
			iden,
			unsupported,
		};

		// The image items libheif makes of item types. Any other item is not an image.
		std::optional<codec> image_codec(const item_entry& entry)
		{
			switch (entry.type)
			{
			case item_jpeg: return codec::jpeg;
			case item_hvc1: return codec::hevc;
			case item_av01: return codec::av1;
			case item_vvc1: return codec::vvc;
			case item_avc1: return codec::avc;
			case item_unci: return codec::unci;
			case item_j2k1: return codec::j2k;
			case item_lhv1: return codec::unsupported;
			case item_mski: return codec::mask;
			case item_grid: return codec::grid;
			case item_iovl: return codec::overlay;
			case item_iden: return codec::iden;
			case item_mime: if (entry.content_type == jpeg_content_type) return codec::jpeg;
				break;
			default: break;
			}

			return {};
		}

		// The item types libheif leaves out of its metadata. It differs from the image items above:
		// 'tili' is not an image to it, and 'lhv1' is one that is also listed as metadata.
		bool image_type_for_metadata(const item_entry& entry)
		{
			switch (entry.type)
			{
			case item_hvc1:
			case item_av01:
			case item_grid:
			case item_tili:
			case item_iden:
			case item_iovl:
			case item_avc1:
			case item_unci:
			case item_vvc1:
			case item_jpeg:
			case item_j2k1:
			case item_mski:
				return true;
			case item_mime:
				return entry.content_type == jpeg_content_type;
			default:
				return false;
			}
		}

		struct image
		{
			std::uint32_t id = 0;
			codec kind = codec::unsupported;
			// libheif's ImageItem_Error: an image it lists but cannot decode.
			bool error = false;
			// Indices into ipco's children, in the order the item's association list names them.
			std::vector<std::uint32_t> properties;
			// The size after the item's transforms: ispe, then clap and irot in property order.
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			bool thumbnail = false;
			std::vector<std::uint32_t> thumbnails;
			std::vector<std::uint32_t> aux_images;
			std::optional<std::uint32_t> alpha;
			std::optional<std::uint32_t> depth;
			bool premultiplied = false;
			nclx_profile nclx;
			const property* icc = nullptr;
			std::vector<std::uint32_t> metadata;
			std::uint32_t grid_rows = 0;
			std::uint32_t grid_columns = 0;
		};

		struct grid_spec
		{
			std::uint32_t rows = 0;
			std::uint32_t columns = 0;
		};

		// libheif's ImageGrid::parse, from the item's data.
		std::optional<grid_spec> parse_grid(reader& r, const heif_boxes& f, const std::uint32_t id)
		{
			std::uint64_t size = 0;
			if (check_item_data(f, id, size) != data_status::ok) return {};

			const auto data = read_item_data(r, f, id, 12);
			if (size < 8 || data[0] != 0) return {};
			if ((data[1] & 1u) && size < 12) return {};

			return grid_spec{data[2] + 1u, data[3] + 1u};
		}

		struct heif_model
		{
			heif_boxes boxes;
			std::map<std::uint32_t, image> images;
			std::vector<std::uint32_t> top_level;
			std::uint32_t primary = 0;

			const property& prop(const std::uint32_t index) const { return boxes.properties[index]; }

			// An image's first property of a type, as libheif's get_property finds it.
			const property* first(const image& img, const std::uint32_t type) const
			{
				for (const auto index : img.properties)
				{
					const auto& p = prop(index);
					if (p.type == type && p.fault == parse_fault::none) return &p;
				}

				return nullptr;
			}

			image* find(const std::uint32_t id)
			{
				const auto it = images.find(id);
				return it == images.end() ? nullptr : &it->second;
			}

			const image* find(const std::uint32_t id) const
			{
				const auto it = images.find(id);
				return it == images.end() ? nullptr : &it->second;
			}

			void remove_top_level(const std::uint32_t id)
			{
				std::erase(top_level, id);
			}
		};

		// libheif's initialize_decoder: false when it makes the item an ImageItem_Error.
		bool decoder_ready(reader& r, const heif_model& m, image& img)
		{
			const auto& f = m.boxes;

			switch (img.kind)
			{
			case codec::hevc: return m.first(img, prop_hvcc) != nullptr;
			case codec::av1: return m.first(img, prop_av1c) != nullptr;
			case codec::avc: return m.first(img, prop_avcc) != nullptr;
			case codec::vvc: return m.first(img, prop_vvcc) != nullptr;
			case codec::j2k: return m.first(img, prop_j2kh) != nullptr;
			case codec::unci:
				{
					const auto* const uncc = m.first(img, prop_uncc);
					if (!uncc || !m.first(img, prop_ispe)) return false;
					return std::ranges::none_of(uncc->components, [](const unc_component& c) { return c.bits > 128; });
				}
			case codec::grid:
				{
					const auto spec = parse_grid(r, f, img.id);
					if (!spec || !f.references) return false;

					img.grid_rows = spec->rows;
					img.grid_columns = spec->columns;
					return f.references_from(img.id, ref_dimg).size() == static_cast<std::size_t>(spec->rows) * spec->columns;
				}
			case codec::overlay:
				{
					if (!f.references) return false;

					const auto inputs = f.references_from(img.id, ref_dimg);
					if (inputs.empty() || inputs.size() > max_overlay_images) return false;

					std::uint64_t size = 0;
					if (check_item_data(f, img.id, size) != data_status::ok) return false;

					const auto data = read_item_data(r, f, img.id, 32);
					if (size < 10 || data[0] != 0) return false;

					const std::uint64_t field = (data[1] & 1u) ? 4 : 2;
					if (2 + 8 + 2 * field + inputs.size() * 2 * field > size) return false;

					const auto width = field == 4 ? load_be32(data.data() + 10) : load_be16(data.data() + 10);
					const auto height = field == 4 ? load_be32(data.data() + 14) : load_be16(data.data() + 12);
					return width != 0 && height != 0;
				}
			case codec::unsupported: return false;
			default: return true;
			}
		}

		bool is_alpha_type(const std::string_view type)
		{
			return type == "urn:mpeg:avc:2015:auxid:1" || type == "urn:mpeg:hevc:2015:auxid:1" ||
				type == "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
		}

		bool is_depth_type(const std::string_view type)
		{
			return type == "urn:mpeg:hevc:2015:auxid:2" || type == "urn:mpeg:mpegB:cicp:systems:auxiliary:depth";
		}

		// libheif's interpret_heif_file_images: which items are images, their sizes, and how they
		// attach to one another, with every fault for which it refuses the file.
		heif_model interpret(reader& r, heif_boxes boxes)
		{
			heif_model m;
			m.boxes = std::move(boxes);
			const auto& f = m.boxes;
			auto found_primary = false;

			for (const auto& [id, entry] : f.items)
			{
				const auto kind = image_codec(entry);
				if (!kind) continue;

				image img;
				img.id = id;
				img.kind = *kind;
				img.error = !f.properties_of(id, img.properties);
				if (!img.error) img.error = !decoder_ready(r, m, img);

				m.images.emplace(id, std::move(img));

				if (!entry.hidden)
				{
					if (id == f.primary) found_primary = true;
					m.top_level.push_back(id);
				}
			}

			if (!found_primary) throw format_error("the HEIF primary item is not an image libheif reads");
			m.primary = *f.primary;

			for (auto& [id, img] : m.images)
			{
				if (img.error) continue;

				for (const auto index : img.properties)
				{
					if (!libheif_parses(m.prop(index)) && f.essential(id, index))
					{
						throw format_error("a HEIF image needs a property libheif cannot read");
					}
				}

				// An image that requires a reference type libheif does not process cannot be decoded,
				// which costs the image rather than the file.
				if (const auto* const rref = m.first(img, prop_rref))
				{
					const auto supported = [](const std::uint32_t type)
					{
						return std::ranges::find(supported_references, type) != std::end(supported_references);
					};

					if (!std::ranges::all_of(rref->required_references, supported))
					{
						img.error = true;
						continue;
					}
				}

				for (const auto index : img.properties)
				{
					if (m.prop(index).fault == parse_fault::ignorable) throw format_error("a HEIF image property is malformed");
				}

				auto have_size = false;

				for (const auto index : img.properties)
				{
					const auto& p = m.prop(index);
					if (p.type != prop_ispe) continue;
					if (p.width == 0 || p.height == 0) throw format_error("a HEIF image has no size");

					img.width = p.width;
					img.height = p.height;
					have_size = true;
				}

				for (const auto index : img.properties)
				{
					const auto& p = m.prop(index);
					if (p.intrinsics && !have_size) throw format_error("a HEIF camera model belongs to an image with no size");
					if (p.type != prop_colr) continue;

					if (!p.icc.empty() || p.colour_type == fourcc("prof") || p.colour_type == fourcc("rICC")) img.icc = &p;
					else img.nclx = p.nclx;
				}

				for (const auto index : img.properties)
				{
					const auto& p = m.prop(index);

					if (p.type == prop_clap)
					{
						if (p.clap_width <= 0 || p.clap_height <= 0) throw format_error("a HEIF clean aperture leaves no image");
						img.width = static_cast<std::uint32_t>(p.clap_width);
						img.height = static_cast<std::uint32_t>(p.clap_height);
					}
					else if (p.type == prop_imir)
					{
						if (!have_size) throw format_error("a mirrored HEIF image has no size");
					}
					else if (p.type == prop_irot && (p.rotation == 90 || p.rotation == 270))
					{
						if (!have_size) throw format_error("a rotated HEIF image has no size");
						std::swap(img.width, img.height);
					}
				}
			}

			if (f.references)
			{
				for (auto& [id, img] : m.images)
				{
					for (const auto& ref : *f.references)
					{
						if (ref.from != id) continue;

						if (ref.type == ref_thmb)
						{
							for (const auto to : ref.to)
							{
								img.thumbnail = true;

								auto* const master = m.find(to);
								if (!master) throw format_error("a HEIF thumbnail belongs to no image");
								if (master->thumbnail) throw format_error("a HEIF thumbnail belongs to another thumbnail");
								if (master == &img) throw format_error("a HEIF thumbnail belongs to itself");

								master->thumbnails.push_back(id);
							}

							m.remove_top_level(id);
						}
						else if (ref.type == ref_auxl)
						{
							const auto* const auxc = m.first(img, prop_auxc);
							if (!auxc) throw format_error("a HEIF auxiliary image does not say what it is");

							const auto attach = [&](const auto& action)
							{
								for (const auto to : ref.to)
								{
									auto* const master = m.find(to);

									if (!master)
									{
										if (!f.item(to)) throw format_error("a HEIF auxiliary image belongs to no item");
										continue;
									}

									if (master == &img) throw format_error("a HEIF auxiliary image belongs to itself");
									action(*master);
								}
							};

							if (is_alpha_type(auxc->aux_type)) attach([id](image& master) { master.alpha = id; });
							if (is_depth_type(auxc->aux_type)) attach([id](image& master) { master.depth = id; });

							attach([&m, id](image& master)
							{
								master.aux_images.push_back(id);
								m.remove_top_level(id);
							});
						}
					}
				}

				// A grid without a colour profile of its own takes its first tile's.
				for (auto& [id, img] : m.images)
				{
					if (img.error || f.item_type(id) != item_grid) continue;

					const auto tiles = f.references_from(id, ref_dimg);
					if (tiles.empty()) continue;

					const auto* const tile = m.find(tiles.front());
					if (!tile) continue;

					if (!img.icc && tile->icc) img.icc = tile->icc;
					if (img.nclx == nclx_profile{} && tile->nclx != nclx_profile{}) img.nclx = tile->nclx;
				}
			}

			// Metadata: every item that is not an image, attached to the images its 'cdsc' names.
			for (const auto& [id, entry] : f.items)
			{
				if (image_type_for_metadata(entry) || entry.type == item_rgan) continue;

				if (entry.type == item_mime && !entry.content_encoding.empty() && entry.content_encoding != "identity")
				{
					// Diffractor's libheif is built without zlib and Brotli, so it skips a compressed item.
					continue;
				}

				std::uint64_t size = 0;
				const auto status = check_item_data(f, id, size);

				if (status == data_status::unsupported) continue;

				if (status == data_status::fault)
				{
					if (entry.type == item_exif || entry.type == item_mime) throw format_error("a HEIF metadata item cannot be read");
					continue;
				}

				for (const auto to : f.references_from(id, ref_cdsc))
				{
					if (auto* const target = m.find(to)) target->metadata.push_back(id);
					else if (!f.item(to)) throw format_error("HEIF metadata belongs to no item");
				}
			}

			for (const auto& [id, entry] : f.items)
			{
				if (f.references_from(id, ref_prem).empty()) continue;

				auto* const img = m.find(id);
				if (!img) throw format_error("a HEIF premultiplied alpha belongs to no image");
				img->premultiplied = true;
			}

			// Region and text items need only be readable and belong to images.
			for (const auto& [id, entry] : f.items)
			{
				if (entry.type != item_rgan && entry.type != item_mime) continue;

				std::uint64_t size = 0;
				const auto status = check_item_data(f, id, size);

				if (entry.type == item_rgan && status != data_status::ok) throw format_error("a HEIF region item cannot be read");

				// A text item libheif cannot read is skipped; one that faults has refused the file above.
				const auto encoded = !entry.content_encoding.empty() && entry.content_encoding != "identity";
				if (entry.type == item_mime && (encoded || status != data_status::ok)) continue;

				if (!f.references) continue;

				const auto owner = entry.type == item_rgan ? ref_cdsc : ref_text;

				for (const auto& ref : *f.references)
				{
					if (ref.from != id || ref.type != owner) continue;

					for (const auto to : ref.to)
					{
						if (!m.find(to)) throw format_error("a HEIF region or text item belongs to no image");
					}
				}
			}

			if (m.images.at(m.primary).error) throw format_error("the HEIF primary image cannot be decoded");
			return m;
		}

		struct coded_format
		{
			colourspace space = colourspace::undefined;
			chroma format = chroma::undefined;
			// libheif leaves the chroma unset for an uncompressed profile it has no layout for.
			bool format_unset = false;
			// The luma's bits, where the coded image's configuration or frame header states them; else -1.
			int bits = -1;
		};

		struct jpeg_frame
		{
			chroma format = chroma::undefined;
			int precision = 0;
		};

		// libheif's search for a JPEG item's frame header in its jpgC data and the item's own: the first
		// byte pair that reads as a SOF marker decides, wherever it lies. libheif reads the whole item for
		// this; it is read here a step at a time, and no further than 16 MiB.
		std::optional<jpeg_frame> jpeg_sof(reader& r, const heif_model& m, const image& img)
		{
			static constexpr bool is_sof[16] = {
				true, true, true, true, false, true, true, true, false, true, true, true, false, true, true, true
			};

			const auto& f = m.boxes;
			std::uint64_t item_size = 0;
			if (check_item_data(f, img.id, item_size) != data_status::ok) return {};

			const auto* const config = m.first(img, prop_jpgc);
			std::vector<std::uint8_t> data;
			if (config) data.assign(config->data.begin(), config->data.end());

			const auto total = data.size() + item_size;
			const auto limit = std::min<std::uint64_t>(total, max_metadata_block);
			std::uint64_t item_read = 0;

			const auto extend = [&](const std::uint64_t to)
			{
				const auto wanted = std::min<std::uint64_t>(to, limit);
				if (wanted <= data.size()) return;

				const auto more = read_item_range(r, f, img.id, item_read, wanted - data.size());
				item_read += more.size();
				data.insert(data.end(), more.begin(), more.end());
			};

			std::size_t searched = 0;
			std::uint64_t want = jpeg_search_step;

			for (;;)
			{
				extend(want);

				for (auto i = searched; i + 1 < data.size(); ++i)
				{
					if (data[i] != 0xFF || (data[i + 1] & 0xF0) != 0xC0 || !is_sof[data[i + 1] & 0x0F]) continue;

					extend(i + 18);

					if (i + 9 >= total || i + 9 >= data.size()) return {};

					jpeg_frame frame;
					frame.precision = data[i + 4];
					const auto components = data[i + 9];

					if (i + 11 + 3u * components >= total) return {};

					int h[3] = {};
					int v[3] = {};

					for (std::size_t c = 0; c < std::min<std::size_t>(components, 3); ++c)
					{
						if (i + 11 + 3 * c >= data.size()) return {};
						h[c] = data[i + 11 + 3 * c] >> 4;
						v[c] = data[i + 11 + 3 * c] & 0x0F;
					}

					if (components == 1) frame.format = chroma::monochrome;
					else if (components != 3) return {};
					else if (h[1] != h[2] || v[1] != v[2]) return {};
					else if (h[0] == 2 && v[0] == 2 && h[1] == 1 && v[1] == 1) frame.format = chroma::c420;
					else if (h[0] == 2 && v[0] == 1 && h[1] == 1 && v[1] == 1) frame.format = chroma::c422;
					else if (h[0] == 1 && v[0] == 1 && h[1] == 1 && v[1] == 1) frame.format = chroma::c444;
					else return {};

					return frame;
				}

				searched = data.empty() ? 0 : data.size() - 1;
				if (data.size() >= limit) return {};
				want *= 2;
			}
		}

		// libheif's get_heif_chroma_uncompressed: the chroma an uncompressed image's components make.
		coded_format unci_components(const property& uncc, const property* cmpd, bool* has_alpha = nullptr)
		{
			coded_format result;
			if (has_alpha) *has_alpha = false;

			if (!cmpd && uncc.version != 1) return result;

			if (cmpd)
			{
				for (const auto& c : uncc.components)
				{
					if (c.index >= cmpd->component_types.size()) return result;
					const auto type = cmpd->component_types[c.index];
					if (type > 7 && type != 11 && type != 12) return result;
				}
			}

			if (uncc.version == 1)
			{
				if (uncc.profile == fourcc("rgb3") || uncc.profile == fourcc("rgba") || uncc.profile == fourcc("abgr"))
				{
					result = {colourspace::rgb, chroma::c444};
					if (has_alpha) *has_alpha = uncc.profile != fourcc("rgb3");
				}

				return result;
			}

			std::uint32_t set = 0;

			for (const auto& c : uncc.components)
			{
				const auto type = cmpd->component_types[c.index];
				if (type == 12) continue;
				set |= 1u << type;
			}

			constexpr auto mono = 1u << 0;
			constexpr auto y = 1u << 1;
			constexpr auto cb = 1u << 2;
			constexpr auto cr = 1u << 3;
			constexpr auto red = 1u << 4;
			constexpr auto green = 1u << 5;
			constexpr auto blue = 1u << 6;
			constexpr auto alpha = 1u << 7;
			constexpr auto filter_array = 1u << 11;

			if (has_alpha) *has_alpha = (set & alpha) != 0;

			if (set == (red | green | blue) || set == (red | green | blue | alpha)) result = {colourspace::rgb, chroma::c444};

			if (set == (y | cb | cr))
			{
				result.space = colourspace::ycbcr;

				switch (uncc.sampling)
				{
				case 0: result.format = chroma::c444;
					break;
				case 1: result.format = chroma::c422;
					break;
				case 2: result.format = chroma::c420;
					break;
				default: break;
				}
			}

			if (set == mono || set == (mono | alpha) || set == y || set == (y | alpha)) result = {colourspace::monochrome, chroma::monochrome};
			if (set == filter_array) result = {colourspace::undefined, chroma::monochrome};
			return result;
		}

		std::optional<coded_format> unci_format(const heif_model& m, const image& img)
		{
			const auto* const uncc = m.first(img, prop_uncc);
			if (!uncc) return {};

			if (uncc->version == 1)
			{
				coded_format result{colourspace::rgb, chroma::undefined, true};

				if (uncc->profile == fourcc("rgb3")) result = {colourspace::rgb, chroma::interleaved_rgb};
				else if (uncc->profile == fourcc("rgba") || uncc->profile == fourcc("abgr")) result = {colourspace::rgb, chroma::interleaved_rgba};

				return result;
			}

			const auto* const cmpd = m.first(img, prop_cmpd);
			if (!cmpd) return {};

			// The decoder keeps the chroma it found, whether or not the components made sense.
			return unci_components(*uncc, cmpd);
		}

		int unci_bits(const heif_model& m, const image& img, const bool luma)
		{
			const auto* const uncc = m.first(img, prop_uncc);
			if (!uncc) return -1;
			if (uncc->version == 1) return 8;

			const auto* const cmpd = m.first(img, prop_cmpd);
			if (!cmpd) return -1;

			auto bits = 0;
			auto alternate = 0;

			for (const auto& c : uncc->components)
			{
				if (c.index >= cmpd->component_types.size()) return -1;

				switch (cmpd->component_types[c.index])
				{
				case 0: case 4: case 5: case 6: case 11:
					alternate = std::max(alternate, c.bits);
					break;
				case 1:
					if (luma) bits = std::max(bits, c.bits);
					break;
				case 2: case 3:
					if (!luma) bits = std::max(bits, c.bits);
					break;
				default:
					break;
				}
			}

			return bits > 0 ? bits : alternate > 0 ? alternate : 8;
		}

		// What a coded image's decoder reports, before libheif's correction for an identity matrix.
		std::optional<coded_format> decoder_format(reader& r, const heif_model& m, const image& img)
		{
			const auto from_config = [&m, &img](const std::uint32_t type) -> std::optional<coded_format>
			{
				const auto* const config = m.first(img, type);
				if (!config) return {};
				const auto space = config->coded_chroma == chroma::monochrome ? colourspace::monochrome : colourspace::ycbcr;
				return coded_format{space, config->coded_chroma, false, config->luma_bits};
			};

			if (img.error) return {};

			switch (img.kind)
			{
			case codec::hevc: return from_config(prop_hvcc);
			case codec::av1: return from_config(prop_av1c);
			case codec::avc: return from_config(prop_avcc);
			case codec::vvc: return from_config(prop_vvcc);
			case codec::j2k: return coded_format{colourspace::ycbcr, chroma::c444};
			case codec::unci:
				{
					auto format = unci_format(m, img);
					if (format) format->bits = unci_bits(m, img, true);
					return format;
				}
			case codec::jpeg:
				{
					const auto frame = jpeg_sof(r, m, img);
					if (!frame) return {};
					return coded_format{frame->format == chroma::monochrome ? colourspace::monochrome : colourspace::ycbcr, frame->format, false, frame->precision};
				}
			default: return {};
			}
		}

		// The first image a derived image's 'dimg' chain reaches, as libheif's find_first_coded_image_id.
		std::optional<std::uint32_t> first_coded(const heif_model& m, std::uint32_t id)
		{
			std::set<std::uint32_t> visited;

			for (;;)
			{
				if (!visited.insert(id).second) return {};

				const auto type = m.boxes.item_type(id);

				if (type == item_grid || type == item_iden || type == item_iovl)
				{
					const auto inputs = m.boxes.references_from(id, ref_dimg);
					if (inputs.empty()) return {};
					id = inputs.front();
					continue;
				}

				if (!m.find(id)) return {};
				return id;
			}
		}

		coded_format corrected(coded_format format, const nclx_profile& nclx)
		{
			// libheif decodes a YCbCr image whose matrix is the identity as RGB.
			if (format.space == colourspace::ycbcr && nclx.matrix == 0)
			{
				format.space = colourspace::rgb;
				format.format = chroma::c444;
			}

			return format;
		}

		// libheif's get_coded_image_colorspace, which heif_image_handle_get_preferred_decoding_colorspace
		// returns.
		std::optional<coded_format> coded_image_format(reader& r, const heif_model& m, const image& img, const int depth = 0)
		{
			if (img.error || depth > 64) return {};

			switch (img.kind)
			{
			case codec::overlay:
				return coded_format{colourspace::rgb, chroma::c444};

			case codec::iden:
				{
					const auto coded = first_coded(m, img.id);
					const auto* const source = coded ? m.find(*coded) : nullptr;
					if (!source) return {};
					return coded_image_format(r, m, *source, depth + 1);
				}

			case codec::grid:
				{
					const auto coded = first_coded(m, img.id);
					const auto* const tile = coded ? m.find(*coded) : nullptr;
					if (!tile || tile->error) return {};

					const auto format = decoder_format(r, m, *tile);
					if (!format) return {};
					return corrected(*format, img.nclx);
				}

			default:
				{
					const auto format = decoder_format(r, m, img);
					if (!format) return {};
					return corrected(*format, img.nclx);
				}
			}
		}

		// libheif's luma or chroma bits per pixel, as heif_image_handle_get_luma_bits_per_pixel reports.
		int bits_per_pixel(reader& r, const heif_model& m, const image& img, const bool luma, const int depth = 0)
		{
			if (img.error || depth > 64) return -1;

			const auto from_config = [&m, &img, luma](const std::uint32_t type)
			{
				const auto* const config = m.first(img, type);
				return config ? (luma ? config->luma_bits : config->chroma_bits) : -1;
			};

			switch (img.kind)
			{
			case codec::hevc: return from_config(prop_hvcc);
			case codec::av1: return from_config(prop_av1c);
			case codec::avc: return from_config(prop_avcc);
			case codec::vvc: return from_config(prop_vvcc);
			case codec::jpeg:
				{
					const auto frame = jpeg_sof(r, m, img);
					return frame ? frame->precision : -1;
				}
			case codec::unci: return unci_bits(m, img, luma);
			case codec::mask:
				{
					if (!luma) return 0;
					const auto* const config = m.first(img, prop_mskc);
					return config ? config->luma_bits : -1;
				}
			case codec::grid:
			case codec::overlay:
			case codec::iden:
				{
					const auto coded = first_coded(m, img.id);
					const auto* const source = coded ? m.find(*coded) : nullptr;
					return source ? bits_per_pixel(r, m, *source, luma, depth + 1) : -1;
				}
			default:
				// libfiles does not read a JPEG 2000 codestream, where libheif finds the depth.
				return -1;
			}
		}

		// libheif's HeifContext::has_alpha, given what coded_image_format found the image coded as. It reads
		// nothing: decoder_ready read a grid's record, and a grid that is no error names a tile for each cell.
		bool has_alpha(const heif_model& m, const image& img, const std::optional<coded_format>& format)
		{
			if (img.alpha) return true;

			if (img.kind == codec::unci && !img.error)
			{
				const auto* const uncc = m.first(img, prop_uncc);
				auto alpha = false;
				if (uncc) unci_components(*uncc, m.first(img, prop_cmpd), &alpha);
				if (alpha) return true;
			}

			if (!format) return false;
			if (format->format == chroma::interleaved_rgba || format->format == chroma::interleaved_rrggbbaa) return true;

			if (m.boxes.item_type(img.id) != item_grid || !m.boxes.references) return false;

			const auto tiles = m.boxes.references_from(img.id, ref_dimg);
			if (tiles.size() != static_cast<std::size_t>(img.grid_rows) * img.grid_columns) return false;
			if (!std::ranges::all_of(tiles, [&m](const std::uint32_t id) { return m.find(id) != nullptr; })) return false;

			return std::ranges::any_of(tiles, [&m](const std::uint32_t id) { return m.find(id)->alpha.has_value(); });
		}

		// The pixel format Diffractor named libheif's preferred decoding chroma.
		std::string pixel_format_name(const coded_format& format)
		{
			if (format.format_unset) return {};

			switch (format.format)
			{
			case chroma::monochrome: return "grayscale";
			case chroma::c420: return "yuv420";
			case chroma::c422: return "yuv422";
			case chroma::c444: return "yuv444";
			case chroma::undefined: return "rgb";
			case chroma::interleaved_rgba: return "rgba";
			case chroma::interleaved_rrggbb: return "rgb48";
			case chroma::interleaved_rrggbbaa: return "rgba64";
			default: return {};
			}
		}

		// The picture of what the decoder reports: its chroma as decoded - RGB for an identity matrix, and
		// nothing for a sensor's filter array - the luma's depth, and alpha wherever has_alpha finds it.
		picture_format picture_of(const coded_format& format, const bool alpha)
		{
			picture_format p;
			if (format.format_unset) return p;

			switch (format.format)
			{
			case chroma::monochrome: p.chroma = format.space == colourspace::undefined ? chroma_format::unknown : chroma_format::gray;
				break;
			case chroma::c420: p.chroma = chroma_format::yuv420;
				break;
			case chroma::c422: p.chroma = chroma_format::yuv422;
				break;
			case chroma::c444: p.chroma = format.space == colourspace::rgb ? chroma_format::rgb : chroma_format::yuv444;
				break;
			case chroma::interleaved_rgb:
			case chroma::interleaved_rgba:
			case chroma::interleaved_rrggbb:
			case chroma::interleaved_rrggbbaa: p.chroma = chroma_format::rgb;
				break;
			case chroma::undefined:
				break;
			}

			if (format.bits > 0 && format.bits <= 255) p.bit_depth = static_cast<std::uint8_t>(format.bits);
			p.alpha = alpha;
			return p;
		}

		// The metadata items Diffractor took from the primary image's: the first Exif, and the last
		// IPTC and XMP. The types are compared as libheif hands them to C.
		struct metadata_choice
		{
			std::optional<std::uint32_t> exif;
			std::optional<std::uint32_t> iptc;
			std::optional<std::uint32_t> xmp;
		};

		metadata_choice choose_metadata(const heif_model& m, const image& img)
		{
			metadata_choice choice;

			for (const auto id : img.metadata)
			{
				const auto& entry = m.boxes.items.at(id);
				const auto text = fourcc_text(entry.type);
				const auto name = c_text(text);

				if (!choice.exif && entry.type == item_exif) choice.exif = id;
				if (equal_ignoring_case(name, "iptc")) choice.iptc = id;

				if (equal_ignoring_case(name, "XMP") ||
					(equal_ignoring_case(name, "mime") && equal_ignoring_case(entry.content_type, "application/rdf+xml")))
				{
					choice.xmp = id;
				}
			}

			return choice;
		}

		// A metadata item's data, or nothing when it is larger than a scan keeps.
		std::optional<std::vector<std::uint8_t>> metadata_data(reader& r, const heif_model& m, const std::uint32_t id)
		{
			std::uint64_t size = 0;
			if (check_item_data(m.boxes, id, size) != data_status::ok || size > max_metadata_block) return {};
			return read_item_data(r, m.boxes, id, size);
		}

		std::int32_t as_int(const std::uint32_t value)
		{
			return static_cast<std::int32_t>(value);
		}

		std::vector<structure_row> heif_structure(reader& r, const heif_model& m, const image& primary)
		{
			std::vector<structure_row> rows;
			const auto& f = m.boxes;

			add_structure_section(rows, "Summary", "heif.summary", true);
			add_structure_row(rows, "File size", std::format("{} bytes", f.file_size));

			// The brands as Diffractor read them: from a file type box of at least 16 and at most 4096
			// bytes, by its 32-bit size.
			const auto ftyp_size = r.be32(0);

			if (ftyp_size >= 16 && ftyp_size <= f.file_size && ftyp_size <= 4096)
			{
				const auto box = r.read(0, ftyp_size);
				add_structure_row(rows, "Major brand", std::string(box.begin() + 8, box.begin() + 12));
				add_structure_row(rows, "Minor version", std::to_string(load_be32(box.data() + 12)));

				std::string compatible;

				for (std::uint32_t offset = 16; offset + 4 <= ftyp_size; offset += 4)
				{
					if (!compatible.empty()) compatible += ' ';
					compatible.append(box.begin() + offset, box.begin() + offset + 4);
				}

				if (!compatible.empty()) add_structure_row(rows, "Compatible brands", compatible);
			}

			const auto* const ispe = m.first(primary, prop_ispe);
			constexpr auto int_max = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
			const auto displayed_width = primary.width <= int_max ? primary.width : 0u;
			const auto displayed_height = primary.height <= int_max ? primary.height : 0u;

			add_structure_row(rows, "Top level images", std::to_string(m.top_level.size()));
			add_structure_row(rows, "Primary item id", std::to_string(m.primary));
			add_structure_row(rows, "Stored size", std::format("{} x {}", as_int(ispe ? ispe->width : 0), as_int(ispe ? ispe->height : 0)));
			add_structure_row(rows, "Displayed size", std::format("{} x {}", displayed_width, displayed_height));

			const auto luma = bits_per_pixel(r, m, primary, true);
			const auto chroma_depth = bits_per_pixel(r, m, primary, false);

			if (luma > 0) add_structure_row(rows, "Luma depth", std::format("{} bits", luma));
			if (chroma_depth > 0) add_structure_row(rows, "Chroma depth", std::format("{} bits", chroma_depth));

			add_structure_row(rows, "Alpha channel", has_alpha(m, primary, coded_image_format(r, m, primary))
				                                         ? primary.premultiplied ? "present, premultiplied" : "present"
				                                         : "none");

			if (const auto* const pasp = m.first(primary, prop_pasp); pasp && pasp->h_spacing != pasp->v_spacing)
			{
				add_structure_row(rows, "Pixel aspect ratio", std::format("{}:{}", pasp->h_spacing, pasp->v_spacing));
			}

			add_structure_row(rows, "Colour profile", primary.icc
				                                          ? "ICC"
				                                          : primary.nclx != nclx_profile{}
				                                          ? "nclx (coded primaries and transfer)"
				                                          : "none");

			add_structure_row(rows, "Thumbnails", std::to_string(primary.thumbnails.size()));
			if (primary.depth) add_structure_row(rows, "Depth images", "1");
			if (!primary.aux_images.empty()) add_structure_row(rows, "Auxiliary images", std::to_string(primary.aux_images.size()));

			add_structure_section(rows, "Metadata items", "heif.metadata");

			for (const auto id : primary.metadata)
			{
				const auto& entry = f.items.at(id);
				const auto text = fourcc_text(entry.type);
				const auto name = c_text(text);

				std::uint64_t size = 0;
				check_item_data(f, id, size);

				auto value = std::format("{} bytes, item {}", size, id);
				if (!entry.content_type.empty()) value = std::format("{}, {}", entry.content_type, value);

				auto& row = add_row(rows, name.empty() ? std::string("unnamed") : std::string(name), std::move(value));
				row.depth = 1;
				row.id = std::format("heif.metadata.{}", id);
			}

			finish_structure_sections(rows);
			return rows;
		}
	}

	void strip_exif_offset(std::vector<std::uint8_t>& exif)
	{
		if (exif.size() >= 4)
		{
			const auto tiff_offset = 4u + static_cast<std::uint64_t>(load_be32(exif.data()));
			if (tiff_offset <= exif.size()) exif.erase(exif.begin(), exif.begin() + static_cast<std::ptrdiff_t>(tiff_offset));
		}

		if (starts_with(exif, {"Exif\0\0", 6})) exif.erase(exif.begin(), exif.begin() + 6);
	}

	scan_result scan_heif(reader& r, const scan_intent intent)
	{
		const auto m = interpret(r, read_boxes(r));
		const auto& primary = m.images.at(m.primary);

		scan_result result;

		// libheif reports a size that does not fit an int as zero.
		constexpr auto int_max = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
		result.width = primary.width <= int_max ? primary.width : 0;
		result.height = primary.height <= int_max ? primary.height : 0;

		if (const auto format = coded_image_format(r, m, primary))
		{
			result.pixel_format = pixel_format_name(*format);
			result.picture = picture_of(*format, has_alpha(m, primary, format));
		}

		result.container_transform = std::ranges::any_of(primary.properties, [&m](const std::uint32_t index)
		{
			const auto type = m.prop(index).type;
			return type == prop_irot || type == prop_imir;
		});

		const auto choice = choose_metadata(m, primary);

		if (choice.exif)
		{
			if (auto data = metadata_data(r, m, *choice.exif))
			{
				strip_exif_offset(*data);
				result.metadata.exif = std::move(*data);
			}
		}

		if (choice.iptc)
		{
			if (auto data = metadata_data(r, m, *choice.iptc)) result.metadata.iptc = std::move(*data);
		}

		if (choice.xmp)
		{
			if (auto data = metadata_data(r, m, *choice.xmp)) result.metadata.xmp = std::move(*data);
		}

		if (primary.icc && !primary.icc->icc.empty() && primary.icc->icc.size() <= max_metadata_block)
		{
			result.metadata.icc.assign(primary.icc->icc.begin(), primary.icc->icc.end());
		}

		if (intent == scan_intent::inspect) result.structure = heif_structure(r, m, primary);

		result.success = result.width != 0 && result.height != 0;
		return result;
	}

	bool heif_packet(reader& r, std::vector<std::uint8_t>& packet)
	{
		const auto m = interpret(r, read_boxes(r));
		const auto choice = choose_metadata(m, m.images.at(m.primary));

		packet.clear();
		if (!choice.xmp) return true;

		auto data = metadata_data(r, m, *choice.xmp);
		if (!data) return false;

		packet = std::move(*data);
		return true;
	}
}
