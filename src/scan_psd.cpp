// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The PSD scanner, and the Photoshop image resources that PSD stores in its header and
// JPEG in APP13. The resources carry IPTC, XMP, EXIF and the colour profile.

#include "scan_internal.h"
#include "photoshop_internal.h"

#include <algorithm>
#include <cstring>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::size_t max_psd_metadata_bytes = 16u * 1024u * 1024u;
		constexpr std::string_view photoshop_signature{"Photoshop 3.0\0", 14};

		constexpr std::uint32_t max_psd_dimension = 30000;
		constexpr std::uint64_t max_psd_pixels = 256ull * 1024ull * 1024ull;

		enum psd_mode : std::uint16_t
		{
			bitmap_mode = 0,
			grayscale_mode = 1,
			indexed_mode = 2,
			rgb_mode = 3,
			cmyk_mode = 4,
			multichannel_mode = 7,
			duotone_mode = 8,
			lab_mode = 9
		};

		// The header limits under which a PSD can be decoded: 8 bits a channel, or a single 1-bit
		// plane in bitmap mode.
		bool is_valid_psd_header(const std::uint32_t columns, const std::uint32_t rows, const std::uint16_t channels,
		                         const std::uint16_t depth, const std::uint16_t mode)
		{
			const auto depth_ok = depth == 8 || (depth == 1 && mode == bitmap_mode && channels == 1);

			return columns != 0 && rows != 0 && columns <= max_psd_dimension && rows <= max_psd_dimension &&
				static_cast<std::uint64_t>(columns) * rows <= max_psd_pixels && channels != 0 && channels <= 56 && depth_ok;
		}

		// Big-endian reads at a cursor that never passes the end of the file.
		class msb_cursor
		{
			reader& _r;
			std::uint64_t _len = 0;
			std::uint64_t _pos = 0;

			void need(const std::uint64_t n) const
			{
				if (remaining() < n) throw read_error("read past the end of the file");
			}

		public:
			explicit msb_cursor(reader& r) : _r(r), _len(r.size())
			{
			}

			std::uint64_t remaining() const
			{
				if (_pos > _len) throw read_error("read past the end of the file");
				return _len - _pos;
			}

			std::uint64_t pos() const { return _pos; }

			void pos(const std::uint64_t p)
			{
				if (p > _len) throw read_error("read past the end of the file");
				_pos = p;
			}

			void skip(const std::uint64_t n)
			{
				need(n);
				_pos += n;
			}

			std::uint8_t u8()
			{
				need(1);
				return _r.u8(_pos++);
			}

			std::uint16_t u16()
			{
				need(2);
				const auto v = _r.be16(_pos);
				_pos += 2;
				return v;
			}

			std::uint32_t u32()
			{
				need(4);
				const auto v = _r.be32(_pos);
				_pos += 4;
				return v;
			}

			std::vector<std::uint8_t> bytes(const std::uint64_t n)
			{
				need(n);
				auto v = _r.read(_pos, static_cast<std::size_t>(n));
				_pos += n;
				return v;
			}
		};

		// The blocks Photoshop resources fill - IPTC, XMP, EXIF and the colour profile - each from the first
		// resource of its kind that holds anything, which is the one an edit changes, within the 16 MiB
		// they share.
		class metadata_resources
		{
			metadata_blocks& _blocks;
			bool _taken[4] = {};
			std::size_t _accepted = 0;

		public:
			explicit metadata_resources(metadata_blocks& blocks) : _blocks(blocks)
			{
			}

			static bool holds_metadata(const std::uint16_t id)
			{
				return id == 0x0404 || id == 0x0424 || id == 0x0422 || id == 0x040f;
			}

			// The block a resource is read into: nothing when it is empty, an earlier one of its kind was
			// taken, or it would pass the cap.
			std::vector<std::uint8_t>* take(const std::uint16_t id, const std::uint64_t length)
			{
				const auto kind = id == 0x0404 ? 0 : id == 0x0424 ? 1 : id == 0x0422 ? 2 : 3;
				if (length == 0 || _taken[kind]) return nullptr;
				_taken[kind] = true;

				if (length > max_psd_metadata_bytes || _accepted > max_psd_metadata_bytes - length) return nullptr;
				_accepted += static_cast<std::size_t>(length);
				return kind == 0 ? &_blocks.iptc : kind == 1 ? &_blocks.xmp : kind == 2 ? &_blocks.exif : &_blocks.icc;
			}
		};
	}

	scan_result scan_psd(reader& r)
	{
		scan_result result;
		msb_cursor c(r);

		const auto signature = c.u32();
		const auto version = c.u16();

		if (signature != 0x38425053 || version != 1) return result;

		c.skip(6); // reserved

		const auto channels = c.u16();
		const auto rows = c.u32();
		const auto columns = c.u32();
		const auto depth = c.u16();
		const auto mode = c.u16();

		if (!is_valid_psd_header(columns, rows, channels, depth, mode)) return result;

		const auto bits = static_cast<std::uint8_t>(depth);

		switch (mode)
		{
		case bitmap_mode: result.pixel_format = "mono";
			result.picture = {chroma_format::gray, 1};
			break;
		case rgb_mode: result.pixel_format = channels >= 4 ? "argb32" : "rgb32";
			result.picture = {chroma_format::rgb, bits, channels >= 4};
			break;
		case lab_mode: result.pixel_format = "lab";
			result.picture = {chroma_format::lab, bits};
			break;
		case cmyk_mode: result.pixel_format = "cmyk";
			result.picture = {chroma_format::cmyk, bits};
			break;
		case grayscale_mode: result.pixel_format = "gray8";
			result.picture = {chroma_format::gray, bits};
			break;
		case indexed_mode: result.pixel_format = "pal8";
			result.picture = {chroma_format::rgb, 8, false, true};
			break;
		case multichannel_mode: result.pixel_format = "multichannel";
			result.picture.bit_depth = bits;
			break;
		case duotone_mode: result.pixel_format = "duotone";
			// One channel of gray, printed in inks the colour data names.
			result.picture = {chroma_format::gray, bits};
			break;
		default:
			break;
		}

		result.width = columns;
		result.height = rows;

		// The colour map, present only for indexed and duotone images.
		const auto colormap_len = c.u32();
		if (colormap_len != 0) c.skip(colormap_len);

		const auto resources_len = c.u32();

		// A truncated file keeps the dimensions already read: the resources that run past the end
		// are not walked.
		if (resources_len > c.remaining())
		{
			result.success = true;
			return result;
		}

		const auto after_resources = c.pos() + resources_len;

		if (resources_len > 0)
		{
			metadata_resources blocks(result.metadata);

			while (c.pos() < after_resources)
			{
				const auto start = c.pos();
				const auto available = after_resources - start;
				const auto resource = read_photoshop_resource(
					r.view(start, static_cast<std::size_t>(std::min<std::uint64_t>(max_photoshop_resource_header, available))),
					available);
				if (!resource) break;

				if (metadata_resources::holds_metadata(resource->id))
				{
					if (auto* const block = blocks.take(resource->id, resource->length))
					{
						c.pos(start + resource->data_offset);
						*block = c.bytes(resource->length);
					}
				}

				c.pos(start + resource->size);
			}

			c.pos(after_resources);
		}

		result.success = true;
		return result;
	}
}

namespace libfiles
{
	bool parse_photoshop_resources(metadata_blocks& metadata, std::span<const std::uint8_t> resources,
	                               const bool has_photoshop_signature)
	{
		using namespace detail;

		if (has_photoshop_signature)
		{
			if (!starts_with(resources, photoshop_signature)) return false;
			resources = resources.subspan(photoshop_signature.size());
		}

		std::size_t pos = 0;
		metadata_resources blocks(metadata);

		while (pos + 12u <= resources.size())
		{
			const auto resource = read_photoshop_resource(resources.subspan(pos));
			if (!resource) return false;

			if (metadata_resources::holds_metadata(resource->id))
			{
				if (auto* const block = blocks.take(resource->id, resource->length))
				{
					const auto payload = resources.subspan(pos + resource->data_offset, resource->length);
					block->assign(payload.begin(), payload.end());
				}
			}
			else if (resource->id != 0x0425)
			{
				const auto record = resources.subspan(pos, resource->size);
				metadata.photoshop.insert(metadata.photoshop.end(), record.begin(), record.end());
				if (resource->lacks_pad()) metadata.photoshop.push_back(0);
			}

			pos += resource->size;
		}

		return true;
	}
}
