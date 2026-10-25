// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The loading rules are libexif's exif_data_load_data and exif_data_load_data_content (exif-data.c),
// Copyright (c) 2001 Lutz Mueller <lutz@users.sourceforge.net> and the libexif contributors,
// LGPL-2.0-or-later.

// Purpose: Listing an EXIF block. The block is read with libfiles' TIFF reader, by libexif's rules for
// what a listing holds: which entries a damaged block still yields and where, the directories a pointer
// may lead to, a thumbnail found in any directory, and the first 65,534 bytes only. Then each entry is
// named and rendered by libexif's ported code, and the maker note decoded by its ported decoders.

#include "libfiles/exif_listing.h"

#include "libexif_makernote.h"
#include "libexif_port.h"
#include "tiff_internal.h"

#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>

namespace libfiles
{
	namespace
	{
		using namespace detail::libexif;

		// The buffer libexif's renderers write into, as Diffractor sized it.
		constexpr unsigned int value_text_size = 65536;

		constexpr std::uint8_t exif_signature[] = {'E', 'x', 'i', 'f', 0, 0};

		// The block as libexif's loader leaves it, in libexif's structures.
		struct loaded_block
		{
			ExifData data{};
			std::array<ExifContent, EXIF_IFD_COUNT> contents{};
			std::array<std::vector<ExifEntry*>, EXIF_IFD_COUNT> ifds;
			std::deque<ExifEntry> entries;
			std::deque<std::vector<unsigned char>> values;
			std::vector<unsigned char> thumbnail;
			unsigned int offset_mnote = 0;
			// Whether the load reached the maker note: libexif stops short of it when IFD0's link to IFD1
			// lies past the end.
			bool reached_maker_note = false;
			ExifMem mem;
			ExifEntryPrivate entry_private{&mem};

			loaded_block()
			{
				for (auto i = 0; i < EXIF_IFD_COUNT; ++i)
				{
					contents[i].parent = &data;
					data.ifd[i] = &contents[i];
				}
			}

			// Points libexif's structures at what was loaded, once nothing more will be.
			void finish()
			{
				for (auto i = 0; i < EXIF_IFD_COUNT; ++i)
				{
					contents[i].entries = ifds[i].data();
					contents[i].count = static_cast<unsigned int>(ifds[i].size());
				}

				data.data = thumbnail.empty() ? nullptr : thumbnail.data();
				data.size = static_cast<unsigned int>(thumbnail.size());
			}
		};

		bool overflows(const unsigned int offset, const unsigned int size, const unsigned int length)
		{
			return offset >= size || length > size || offset > size - length;
		}

		unsigned int level_cost(const unsigned int n)
		{
			static const double log_1_1 = 0.09531017980432493;
			return static_cast<unsigned int>(std::ceil(std::log(n + 0.1) / log_1_1));
		}

		// libexif's loader, over libfiles' reader: d is the TIFF stream from its byte-order mark, ds its
		// size.
		class loader
		{
			const detail::tiff_bytes& _d;
			const unsigned int _ds;
			loaded_block& _block;

			std::uint16_t u16(const unsigned int at) const { return _d.u16(at); }
			std::uint32_t u32(const unsigned int at) const { return _d.u32(at); }

		public:
			loader(const detail::tiff_bytes& d, const unsigned int ds, loaded_block& block) : _d(d), _ds(ds), _block(block)
			{
			}

			void load_content(const ExifIfd ifd, unsigned int offset, const unsigned int recursion_cost)
			{
				if (recursion_cost > 170) return;
				if (overflows(offset, _ds, 2)) return;

				unsigned int n = u16(offset);
				offset += 2;

				if (overflows(offset, _ds, 12 * n)) n = (_ds - offset) / 12;

				unsigned int thumbnail_offset = 0;
				unsigned int thumbnail_length = 0;

				for (unsigned int i = 0; i < n; i++)
				{
					const auto at = offset + 12 * i;
					const auto tag = u16(at);

					switch (tag)
					{
					case EXIF_TAG_EXIF_IFD_POINTER:
					case EXIF_TAG_GPS_INFO_IFD_POINTER:
					case EXIF_TAG_INTEROPERABILITY_IFD_POINTER:
					case EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH:
					case EXIF_TAG_JPEG_INTERCHANGE_FORMAT:
						{
							const auto o = u32(at + 8);

							if (o >= _ds)
							{
								if (recursion_cost > 0) return;
								break;
							}

							const auto sub = [&](const ExifIfd target)
							{
								if (target == ifd || !_block.ifds[target].empty()) return;
								load_content(target, o, recursion_cost + level_cost(n));
							};

							switch (tag)
							{
							case EXIF_TAG_EXIF_IFD_POINTER:
								sub(EXIF_IFD_EXIF);
								break;
							case EXIF_TAG_GPS_INFO_IFD_POINTER:
								sub(EXIF_IFD_GPS);
								break;
							case EXIF_TAG_INTEROPERABILITY_IFD_POINTER:
								sub(EXIF_IFD_INTEROPERABILITY);
								break;
							case EXIF_TAG_JPEG_INTERCHANGE_FORMAT:
								thumbnail_offset = o;
								if (thumbnail_offset && thumbnail_length) load_thumbnail(thumbnail_offset, thumbnail_length);
								break;
							default:
								thumbnail_length = o;
								if (thumbnail_offset && thumbnail_length) load_thumbnail(thumbnail_offset, thumbnail_length);
								break;
							}
						}
						break;
					default:
						// An unknown tag is kept, as libexif kept it with its option to ignore them off; only an
						// entry of tag and format 0, which Photoshop writes, is skipped.
						if (!exif_tag_get_name_in_ifd(static_cast<ExifTag>(tag), ifd) && u16(at) == 0 && u16(at + 2) == 0)
						{
							break;
						}

						load_entry(ifd, at);
						break;
					}
				}
			}

		private:
			void load_thumbnail(const unsigned int o, const unsigned int s)
			{
				if (o >= _ds || overflows(o, _ds, s)) return;

				_block.thumbnail.resize(s);
				for (unsigned int i = 0; i < s; ++i) _block.thumbnail[i] = static_cast<unsigned char>(_d.at(o + i, 1)[0]);
			}

			void load_entry(const ExifIfd ifd, const unsigned int offset)
			{
				const auto tag = static_cast<ExifTag>(u16(offset));
				const auto format = static_cast<ExifFormat>(u16(offset + 2));
				const unsigned long components = u32(offset + 4);

				const unsigned int s = static_cast<unsigned int>(exif_format_get_size(format) * components);
				if (s < components || s == 0) return;

				const unsigned int doff = s > 4 ? u32(offset + 8) : offset + 8;
				if (doff >= _ds) return;
				if (s > _ds - doff) return;

				auto& value = _block.values.emplace_back(s);
				std::memcpy(value.data(), _d.at(doff, s), s);

				if (tag == EXIF_TAG_MAKER_NOTE) _block.offset_mnote = doff;

				// One tag can only be added once to a directory.
				for (const auto* const e : _block.ifds[ifd])
				{
					if (e->tag == tag) return;
				}

				auto& e = _block.entries.emplace_back();
				e.tag = tag;
				e.format = format;
				e.components = components;
				e.data = value.data();
				e.size = s;
				e.parent = &_block.contents[ifd];
				e.priv = &_block.entry_private;
				_block.ifds[ifd].push_back(&e);
			}
		};

		exif_directory to_directory(const int ifd)
		{
			switch (ifd)
			{
			case EXIF_IFD_0: return exif_directory::primary;
			case EXIF_IFD_1: return exif_directory::thumbnail;
			case EXIF_IFD_EXIF: return exif_directory::exif;
			case EXIF_IFD_GPS: return exif_directory::gps;
			default: return exif_directory::interoperability;
			}
		}

		// libexif's exif_data_load_data for a block that starts with its signature.
		void load(const std::span<const std::uint8_t> block, loaded_block& out)
		{
			const auto fullds = static_cast<unsigned int>(std::min<std::size_t>(block.size(), 0xffffffffu));
			auto ds = fullds;

			if (ds < 14) return;
			if (ds > 0xfffe) ds = 0xfffe;

			const auto* const d = block.data();
			if (std::memcmp(d + 6, "II", 2) == 0) out.data.order = EXIF_BYTE_ORDER_INTEL;
			else if (std::memcmp(d + 6, "MM", 2) == 0) out.data.order = EXIF_BYTE_ORDER_MOTOROLA;
			else return;

			reader bytes(std::span<const std::uint8_t>(d + 6, ds - 6));
			const detail::tiff_bytes tiff(bytes, out.data.order == EXIF_BYTE_ORDER_INTEL);

			if (tiff.u16(2) != 0x002a) return;

			auto offset = tiff.u32(4);
			if (offset > ds || offset + 6 + 2 > ds) return;

			loader l(tiff, ds - 6, out);
			l.load_content(EXIF_IFD_0, offset, 0);

			const unsigned int n = tiff.u16(offset);
			if (offset + 6 + 2 + 12 * n + 4 > ds) return;

			offset = tiff.u32(offset + 2 + 12 * n);

			if (offset && offset <= ds - 6) l.load_content(EXIF_IFD_1, offset, 0);

			out.reached_maker_note = true;
		}

		// The maker note's values, as libexif's decoder for its format gives them.
		std::vector<exif_maker_note_value> maker_note_values(loaded_block& loaded, const std::span<const std::uint8_t> block,
		                                                     char* const buffer, const ExifDataOption options)
		{
			std::vector<exif_maker_note_value> values;
			if (!loaded.reached_maker_note) return values;

			const auto size = static_cast<unsigned int>(std::min<std::size_t>(block.size(), 0xffffffffu));
			auto* const md = interpret_maker_note(loaded.data, &loaded.mem, loaded.offset_mnote, block.data(), size, options);
			if (!md) return values;

			const auto count = exif_mnote_data_count(md);

			// The listing named the note after the camera's make before it read a value, in the same
			// buffer, so a decoder that reads what it did not write finds what it always found.
			if (count)
			{
				if (auto* const make = exif_data_get_entry(&loaded.data, EXIF_TAG_MAKE))
				{
					buffer[0] = 0;
					exif_entry_get_value(make, buffer, value_text_size);
				}
			}

			for (unsigned int i = 0; i < count; ++i)
			{
				auto& v = values.emplace_back();
				v.id = exif_mnote_data_get_id(md, i);
				if (const auto* const name = exif_mnote_data_get_name(md, i)) v.name = name;
				if (const auto* const title = exif_mnote_data_get_title(md, i)) v.title = title;
				if (const auto* const description = exif_mnote_data_get_description(md, i)) v.description = description;
				buffer[0] = 0;
				if (const auto* const text = exif_mnote_data_get_value(md, i, buffer, value_text_size))
					v.value.assign(text, strnlen(text, value_text_size));
			}

			exif_mnote_data_unref(md);
			return values;
		}
	}

	exif_listing list_exif(const std::span<const std::uint8_t> block, const exif_listing_options& options)
	{
		exif_listing result;

		// Without its signature, the block is given one, as the listing always gave it.
		std::vector<std::uint8_t> signed_block;
		auto bytes = block;

		if (block.size() < 6 || std::memcmp(block.data(), exif_signature, 6) != 0)
		{
			signed_block.assign(std::begin(exif_signature), std::end(exif_signature));
			signed_block.insert(signed_block.end(), block.begin(), block.end());
			bytes = signed_block;
		}

		loaded_block loaded;

		try
		{
			load(bytes, loaded);
		}
		catch (const read_error&)
		{
			// Every read is bounded as libexif bounded it; a fault would leave what was loaded.
		}

		loaded.finish();
		result.little_endian = loaded.data.order == EXIF_BYTE_ORDER_INTEL;

		const auto buffer = std::make_unique<char[]>(value_text_size);

		for (auto i = 0; i < EXIF_IFD_COUNT; ++i)
		{
			if (loaded.ifds[i].empty()) continue;

			auto& directory = result.directories.emplace_back();
			directory.directory = to_directory(i);

			for (auto* const e : loaded.ifds[i])
			{
				const auto* const name = exif_tag_get_name_in_ifd(e->tag, static_cast<ExifIfd>(i));
				if (!name && options.ignore_unknown_tags) continue;

				auto& entry = directory.entries.emplace_back();
				entry.tag = static_cast<std::uint16_t>(e->tag);
				entry.type = static_cast<std::uint16_t>(e->format);
				entry.count = static_cast<std::uint32_t>(e->components);
				if (name) entry.name = name;
				if (const auto* const title = exif_tag_get_title_in_ifd(e->tag, static_cast<ExifIfd>(i))) entry.title = title;
				if (const auto* const description = exif_tag_get_description_in_ifd(e->tag, static_cast<ExifIfd>(i)))
					entry.description = description;
				buffer[0] = 0;
				exif_entry_get_value(e, buffer.get(), value_text_size);
				entry.value = buffer.get();
				entry.bytes.assign(e->data, e->data + e->size);
			}

			if (directory.entries.empty()) result.directories.pop_back();
		}

		const auto mnote_options = options.ignore_unknown_tags ? EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS : ExifDataOption{};
		result.maker_note = maker_note_values(loaded, bytes, buffer.get(), mnote_options);
		result.thumbnail.assign(loaded.thumbnail.begin(), loaded.thumbnail.end());
		return result;
	}

	const char* exif_type_name(const std::uint16_t type) noexcept
	{
		return exif_format_get_name(static_cast<ExifFormat>(type));
	}
}
