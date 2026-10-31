// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing EXIF. make_exif lays out a fresh block as libexif lays one out, so that the block an
// application wrote through libexif comes out byte for byte the same. set_exif_numbers overwrites the
// numbers a turned picture changes where they stand, finding each directory as libexif's loader does.

#include "libfiles/exif.h"
#include "tiff_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>

namespace libfiles
{
	using namespace detail;

	namespace
	{
		constexpr std::uint16_t type_slong = 9;

		// libexif writes no entry whose count passes this; it stops halfway through the entry instead.
		constexpr std::uint32_t max_entry_count = 65536;

		// One entry of a block being written, its value already in little-endian order.
		struct new_entry
		{
			std::uint16_t tag = 0;
			std::uint16_t type = 0;
			std::uint32_t count = 0;
			std::vector<std::uint8_t> value;
		};

		using new_directory = std::vector<new_entry>;

		void put16(std::vector<std::uint8_t>& out, const std::size_t at, const std::uint16_t v)
		{
			out[at] = static_cast<std::uint8_t>(v);
			out[at + 1] = static_cast<std::uint8_t>(v >> 8);
		}

		void put32(std::vector<std::uint8_t>& out, const std::size_t at, const std::uint32_t v)
		{
			out[at] = static_cast<std::uint8_t>(v);
			out[at + 1] = static_cast<std::uint8_t>(v >> 8);
			out[at + 2] = static_cast<std::uint8_t>(v >> 16);
			out[at + 3] = static_cast<std::uint8_t>(v >> 24);
		}

		new_entry short_entry(const std::uint16_t tag, const std::uint16_t value)
		{
			return {tag, static_cast<std::uint16_t>(type_short), 1,
			        {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)}};
		}

		new_entry long_entry(const std::uint16_t tag, const std::uint32_t value)
		{
			std::vector<std::uint8_t> bytes(4);
			put32(bytes, 0, value);
			return {tag, static_cast<std::uint16_t>(type_long), 1, std::move(bytes)};
		}

		new_entry rational_entry(const std::uint16_t tag, const std::uint32_t numerator, const std::uint32_t denominator)
		{
			std::vector<std::uint8_t> bytes(8);
			put32(bytes, 0, numerator);
			put32(bytes, 4, denominator);
			return {tag, static_cast<std::uint16_t>(type_rational), 1, std::move(bytes)};
		}

		new_entry undefined_entry(const std::uint16_t tag, const std::string_view bytes)
		{
			return {tag, static_cast<std::uint16_t>(type_undefined), static_cast<std::uint32_t>(bytes.size()),
			        {bytes.begin(), bytes.end()}};
		}

		void add_ascii(new_directory& d, const std::uint16_t tag, const std::string& text)
		{
			if (text.empty() || text.size() + 1 > max_entry_count) return;

			new_entry e{tag, static_cast<std::uint16_t>(type_ascii), static_cast<std::uint32_t>(text.size() + 1),
			            {text.begin(), text.end()}};
			e.value.push_back(0);
			d.push_back(std::move(e));
		}

		// A real as a rational: scaled by a fixed denominator and reduced, not searched for the closest
		// fraction.
		void add_real(new_directory& d, const std::uint16_t tag, const double value)
		{
			constexpr std::uint32_t scale = 10000u;

			if (value <= 0.0 || value > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) / scale) return;

			auto numerator = static_cast<std::uint32_t>(std::llround(value * scale));
			auto denominator = scale;
			const auto divisor = std::gcd(numerator, denominator);

			if (divisor > 1)
			{
				numerator /= divisor;
				denominator /= divisor;
			}

			d.push_back(rational_entry(tag, numerator, denominator));
		}

		// A sub-second exposure is conventionally recorded as 1/n.
		void add_exposure_time(new_directory& d, const double seconds)
		{
			if (seconds <= 0.0) return;

			if (seconds < 1.0)
			{
				const auto denominator = std::llround(1.0 / seconds);

				if (denominator > 0 && denominator <= std::numeric_limits<std::uint32_t>::max())
				{
					d.push_back(rational_entry(tag_exposure_time, 1u, static_cast<std::uint32_t>(denominator)));
					return;
				}
			}

			add_real(d, tag_exposure_time, seconds);
		}

		// Writes a directory where out ends, as libexif's exif_data_save_data_content does: the count,
		// the entries, and the next-directory link, which is always 0 here; each value longer than four
		// bytes after them in the order the entries were made, padded to an even length; then the Exif
		// directory, where its pointer is; and last the entries sorted by tag.
		void write_directory(std::vector<std::uint8_t>& out, const new_directory& entries, const new_directory* const exif)
		{
			const auto pointers = exif && !exif->empty() ? 1u : 0u;
			const auto count = entries.size() + pointers;
			const auto start = out.size();

			out.resize(start + 2 + count * 12 + 4);
			put16(out, start, static_cast<std::uint16_t>(count));

			auto at = start + 2;

			for (const auto& e : entries)
			{
				put16(out, at, e.tag);
				put16(out, at + 2, e.type);
				put32(out, at + 4, e.count);

				if (e.value.size() > 4)
				{
					put32(out, at + 8, static_cast<std::uint32_t>(out.size()));
					out.insert(out.end(), e.value.begin(), e.value.end());
					if (e.value.size() & 1) out.push_back(0);
				}
				else
				{
					std::copy(e.value.begin(), e.value.end(), out.begin() + static_cast<std::ptrdiff_t>(at + 8));
				}

				at += 12;
			}

			if (pointers)
			{
				put16(out, at, tag_exif_ifd);
				put16(out, at + 2, static_cast<std::uint16_t>(type_long));
				put32(out, at + 4, 1);
				put32(out, at + 8, static_cast<std::uint32_t>(out.size()));
				write_directory(out, *exif, nullptr);
				at += 12;
			}

			std::vector<std::array<std::uint8_t, 12>> sorted(count);

			for (std::size_t i = 0; i < count; ++i)
			{
				std::memcpy(sorted[i].data(), out.data() + start + 2 + 12 * i, 12);
			}

			std::ranges::sort(sorted, [](const auto& a, const auto& b)
			{
				return (a[0] | a[1] << 8) < (b[0] | b[1] << 8);
			});

			for (std::size_t i = 0; i < count; ++i)
			{
				std::memcpy(out.data() + start + 2 + 12 * i, sorted[i].data(), 12);
			}

			put32(out, at, 0);
		}

		// Where each directory of a block is, 0 for none. libexif loads a sub-directory from the first
		// pointer to it it meets, reading IFD0's entries in order and each directory as it is pointed to.
		struct directory_offsets
		{
			std::array<std::uint32_t, 5> offsets = {};

			std::uint32_t& operator[](const exif_directory d) { return offsets[static_cast<std::size_t>(d)]; }
		};

		void locate(const tiff_bytes& data, const std::uint32_t offset, directory_offsets& found, int depth)
		{
			if (depth > 4) return;

			for_each_entry(data, offset, directory_kind::exif, [&](const entry& e)
			{
				auto role = exif_directory::primary;

				switch (e.tag)
				{
				case tag_exif_ifd: role = exif_directory::exif;
					break;
				case tag_gps_ifd: role = exif_directory::gps;
					break;
				case tag_interop_ifd: role = exif_directory::interoperability;
					break;
				default:
					return;
				}

				const auto target = e.field_u32();
				if (found[role] != 0 || target == 0 || data.overflows(target, 2)) return;

				for (const auto known : found.offsets)
				{
					if (known == target) return;
				}

				found[role] = target;
				locate(data, target, found, depth + 1);
			});
		}
	}

	std::vector<std::uint8_t> make_exif(const exif_fields& fields)
	{
		new_directory ifd0 = {
			rational_entry(0x011a, 72, 1), // XResolution
			rational_entry(0x011b, 72, 1), // YResolution
			short_entry(0x0128, 2), // ResolutionUnit: inches
			short_entry(0x0213, 1), // YCbCrPositioning: centred
		};

		new_directory exif = {
			undefined_entry(0x9000, "0210"), // ExifVersion
			undefined_entry(0x9101, std::string_view("\x01\x02\x03\x00", 4)), // ComponentsConfiguration
			undefined_entry(0xa000, "0100"), // FlashpixVersion
			short_entry(0xa001, 0xffff), // ColorSpace: uncalibrated
			long_entry(0xa002, 0), // PixelXDimension
			long_entry(0xa003, 0), // PixelYDimension
		};

		// The order Diffractor's writer made them in, which decides where each long value lies.
		if (fields.orientation) ifd0.push_back(short_entry(tag_orientation, *fields.orientation));
		if (fields.f_number) add_real(exif, 0x829d, *fields.f_number);
		if (fields.exposure_time) add_exposure_time(exif, *fields.exposure_time);
		if (fields.iso_speed) exif.push_back(short_entry(0x8827, *fields.iso_speed));
		if (fields.focal_length) add_real(exif, 0x920a, *fields.focal_length);
		if (fields.focal_length_35mm) exif.push_back(short_entry(0xa405, *fields.focal_length_35mm));
		add_ascii(ifd0, 0x010f, fields.camera_make);
		add_ascii(ifd0, 0x0110, fields.camera_model);
		add_ascii(ifd0, 0x013b, fields.artist);
		add_ascii(exif, 0xa434, fields.lens);
		add_ascii(ifd0, 0x8298, fields.copyright);
		add_ascii(ifd0, 0x010e, fields.description);

		if (!fields.comment.empty() && fields.comment.size() + 8 <= max_entry_count)
		{
			auto comment = std::string("ASCII\0\0\0", 8) + fields.comment;
			exif.push_back(undefined_entry(0x9286, comment));
		}

		add_ascii(exif, 0x9003, fields.original);
		add_ascii(ifd0, 0x0132, fields.modified);
		add_ascii(exif, 0x9004, fields.digitized);

		std::vector<std::uint8_t> out = {'I', 'I', 42, 0, 8, 0, 0, 0};
		write_directory(out, ifd0, &exif);
		return out;
	}

	std::size_t set_exif_numbers(const std::span<std::uint8_t> tiff, const std::span<const exif_number> numbers)
	{
		if (tiff.size() < 8) return 0;

		const auto little_endian = tiff[0] == 0x49 && tiff[1] == 0x49;
		if (!little_endian && !(tiff[0] == 0x4d && tiff[1] == 0x4d)) return 0;

		reader bytes(std::span<const std::uint8_t>(tiff.data(), tiff.size()));
		const tiff_bytes data(bytes, little_endian);

		const auto ifd0 = first_directory(data);
		if (!ifd0 || data.overflows(*ifd0, 2)) return 0;

		directory_offsets found;
		found[exif_directory::primary] = *ifd0;
		locate(data, *ifd0, found, 0);
		found[exif_directory::thumbnail] = next_directory(data, *ifd0);

		struct patch
		{
			std::uint32_t at;
			std::uint32_t size;
			std::uint32_t value;
		};

		std::vector<patch> patches;

		for (const auto& n : numbers)
		{
			const auto offset = found[n.directory];
			if (offset == 0) continue;

			for_each_entry(data, offset, directory_kind::exif, [&](const entry& e)
			{
				if (e.tag != n.tag) return;

				if (e.type == type_short) patches.push_back({e.value_offset(), 2, n.value});
				else if (e.type == type_long || e.type == type_slong) patches.push_back({e.value_offset(), 4, n.value});
			});
		}

		for (const auto& p : patches)
		{
			auto* const at = tiff.data() + p.at;

			for (std::uint32_t i = 0; i < p.size; ++i)
			{
				const auto shift = 8 * (little_endian ? i : p.size - 1 - i);
				at[i] = static_cast<std::uint8_t>(p.value >> shift);
			}
		}

		return patches.size();
	}

	bool unlink_exif_thumbnail(const std::span<std::uint8_t> tiff)
	{
		if (tiff.size() < 8) return false;

		const auto little_endian = tiff[0] == 0x49 && tiff[1] == 0x49;
		if (!little_endian && !(tiff[0] == 0x4d && tiff[1] == 0x4d)) return false;

		reader bytes(std::span<const std::uint8_t>(tiff.data(), tiff.size()));
		const tiff_bytes data(bytes, little_endian);

		// IFD0 follows the header, and its link follows its entries.
		const auto ifd0 = first_directory(data);
		if (!ifd0 || *ifd0 < 8 || data.overflows(*ifd0, 2)) return false;

		const auto link = static_cast<std::uint64_t>(*ifd0) + 2 + 12ull * data.u16(*ifd0);
		if (link > tiff.size() || tiff.size() - link < 4) return false;

		std::fill_n(tiff.begin() + static_cast<std::ptrdiff_t>(link), 4, std::uint8_t{0});
		return true;
	}
}
