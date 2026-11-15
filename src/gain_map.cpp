// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: A gain map's metadata and arithmetic: ISO 21496-1's GainMapMetadata read and written - the one syntax a
// JPEG's APP2 and a HEIF's tone map item both hold - the gain map made for an HDR picture and its SDR rendition, and
// Apple's HDR gain map said in ISO 21496-1's terms: its headroom, from its own XMP or the photo's maker note by
// Apple's formula, and its values.

#include "gain_map_internal.h"
#include "hdr_internal.h"
#include "tiff_internal.h"

#include "libfiles/hdr.h"
#include "libfiles/xmp.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace libfiles
{
	namespace
	{
		// ISO 21496-1's flags: three sets of channel values rather than one; the gain applied in the base
		// rendition's colour space; the alternate rendition as the base, which libfiles does not read; and one
		// denominator shared by every value, which a writer may use.
		constexpr std::uint8_t flag_multichannel = 0x80;
		constexpr std::uint8_t flag_base_colour_space = 0x40;
		constexpr std::uint8_t flag_backward = 0x04;
		constexpr std::uint8_t flag_common_denominator = 0x08;

		struct fraction
		{
			std::int64_t numerator = 0;
			std::uint32_t denominator = 1;
		};

		// The value over the largest power of ten, up to 10^max_digits, that keeps the numerator in 32 bits, signed
		// or not.
		std::optional<fraction> to_fraction(const double value, const bool is_signed, const int max_digits)
		{
			if (!std::isfinite(value) || (!is_signed && value < 0.0)) return {};

			const auto limit = is_signed ? static_cast<double>(std::numeric_limits<std::int32_t>::max())
			                             : static_cast<double>(std::numeric_limits<std::uint32_t>::max());
			std::uint32_t denominator = 1;
			for (auto i = 0; i < max_digits; ++i) denominator *= 10u;

			for (;;)
			{
				const auto scaled = std::round(value * denominator);
				if (std::abs(scaled) <= limit) return fraction{static_cast<std::int64_t>(scaled), denominator};
				if (denominator == 1) return {};
				denominator /= 10u;
			}
		}

		void put16(std::vector<std::uint8_t>& out, const std::uint32_t v)
		{
			out.push_back(static_cast<std::uint8_t>(v >> 8));
			out.push_back(static_cast<std::uint8_t>(v));
		}

		void put32(std::vector<std::uint8_t>& out, const std::uint32_t v)
		{
			for (auto shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
		}

		bool put(std::vector<std::uint8_t>& out, const double value, const bool is_signed, const int digits)
		{
			const auto f = to_fraction(value, is_signed, digits);
			if (!f) return false;
			put32(out, static_cast<std::uint32_t>(f->numerator));
			put32(out, f->denominator);
			return true;
		}

		// BT.709's transfer, undone: its signal, 0 to 1, as the linear light Apple's gain map values stand for.
		double rec709_decode(const double v)
		{
			return v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
		}

		constexpr std::string_view apple_hdr_gain_map_ns = "http://ns.apple.com/HDRGainMap/1.0/";
		constexpr std::uint16_t apple_tag_headroom = 0x0021;
		constexpr std::uint16_t apple_tag_gain = 0x0030;

		std::optional<double> parse_number(std::string_view text)
		{
			while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
			double value = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (error != std::errc{} || !std::isfinite(value)) return {};
			return value;
		}

		// A maker note value of any numeric type, as a number.
		std::optional<double> number_of(const detail::entry& e)
		{
			switch (e.type)
			{
			case detail::type_short: return e.first_u16();
			case detail::type_long: return static_cast<double>(e.data.u32(e.value_offset()));
			case detail::type_rational: return e.rational_at();
			case 9: return static_cast<double>(static_cast<std::int32_t>(e.data.u32(e.value_offset())));
			case 10: return e.signed_rational_at();
			case 11:
			{
				const auto bits = e.data.u32(e.value_offset());
				float f = 0;
				std::memcpy(&f, &bits, sizeof(f));
				return std::isfinite(f) ? std::optional<double>(f) : std::nullopt;
			}
			default: return {};
			}
		}

		// Apple's maker note tags 33 and 48, in the EXIF block's Exif directory: "Apple iOS", two bytes, its byte
		// order, and then its directory, whose offsets count from the note's start.
		std::optional<std::pair<double, double>> apple_maker_note(std::span<const std::uint8_t> exif)
		{
			if (exif.size() >= 6 && std::memcmp(exif.data(), "Exif\0\0", 6) == 0) exif = exif.subspan(6);
			if (exif.size() < 8 || (exif[0] != exif[1]) || (exif[0] != 'I' && exif[0] != 'M')) return {};

			reader block(exif);
			const detail::tiff_bytes tiff(block, exif[0] == 'I');
			const auto ifd0 = detail::first_directory(tiff);
			if (!ifd0) return {};

			std::optional<std::uint32_t> exif_ifd;
			detail::for_each_entry(tiff, *ifd0, detail::directory_kind::exif, [&](const detail::entry& e)
			{
				if (e.tag == detail::tag_exif_ifd && (e.type == detail::type_long || e.type == 13))
					exif_ifd = e.field_u32();
			});
			if (!exif_ifd) return {};

			std::span<const std::uint8_t> note;
			detail::for_each_entry(tiff, *exif_ifd, detail::directory_kind::exif, [&](const detail::entry& e)
			{
				if (e.tag != detail::tag_maker_note) return;
				const auto raw = e.raw_text();
				note = {reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()};
			});

			constexpr std::string_view apple{"Apple iOS\0", 10};
			if (note.size() < 16 || std::memcmp(note.data(), apple.data(), apple.size()) != 0) return {};
			if (note[12] != note[13] || (note[12] != 'M' && note[12] != 'I')) return {};

			// The note is copied out: the entries borrow from the block it lies in.
			const std::vector<std::uint8_t> copy(note.begin(), note.end());
			reader note_reader{std::span<const std::uint8_t>(copy)};
			const detail::tiff_bytes values(note_reader, copy[12] == 'I');

			std::optional<double> headroom;
			std::optional<double> gain;
			detail::for_each_entry(values, 14, detail::directory_kind::exif, [&](const detail::entry& e)
			{
				if (e.tag == apple_tag_headroom) headroom = number_of(e);
				else if (e.tag == apple_tag_gain) gain = number_of(e);
			});

			if (!headroom || !gain) return {};
			return std::pair{*headroom, *gain};
		}
	}

	std::vector<std::uint8_t> write_gain_map_metadata(const gain_map_metadata& m)
	{
		if (!(m.base_headroom >= 0.0) || !(m.alternate_headroom >= 0.0)) return {};
		for (const auto g : m.gamma)
			if (!(g > 0.0)) return {};

		const auto alike = [](const std::array<double, 3>& v)
		{
			return v[0] == v[1] && v[1] == v[2];
		};
		const auto channels = alike(m.gain_min) && alike(m.gain_max) && alike(m.gamma) && alike(m.base_offset) &&
		                              alike(m.alternate_offset)
		                          ? 1
		                          : 3;

		std::vector<std::uint8_t> out;
		put16(out, 0);
		put16(out, 0);
		out.push_back(static_cast<std::uint8_t>((channels == 3 ? flag_multichannel : 0) |
		                                        (m.use_base_colour_space ? flag_base_colour_space : 0)));

		auto ok = put(out, m.base_headroom, false, 7) && put(out, m.alternate_headroom, false, 7);

		for (auto c = 0; c < channels && ok; ++c)
		{
			ok = put(out, m.gain_min[c], true, 6) && put(out, m.gain_max[c], true, 6) &&
			     put(out, m.gamma[c], false, 6) && put(out, m.base_offset[c], true, 7) &&
			     put(out, m.alternate_offset[c], true, 7);
		}

		if (!ok) out.clear();
		return out;
	}

	std::optional<gain_map_metadata> read_gain_map_metadata(const std::span<const std::uint8_t> data)
	{
		std::size_t at = 0;
		const auto u8 = [&]() -> std::optional<std::uint32_t>
		{
			if (at >= data.size()) return {};
			return data[at++];
		};
		const auto u16 = [&]() -> std::optional<std::uint32_t>
		{
			if (at > data.size() || data.size() - at < 2) return {};
			const auto v = static_cast<std::uint32_t>(data[at] << 8 | data[at + 1]);
			at += 2;
			return v;
		};
		const auto u32 = [&]() -> std::optional<std::uint32_t>
		{
			if (at > data.size() || data.size() - at < 4) return {};
			const auto v = static_cast<std::uint32_t>(data[at]) << 24 | static_cast<std::uint32_t>(data[at + 1]) << 16 |
			               static_cast<std::uint32_t>(data[at + 2]) << 8 | data[at + 3];
			at += 4;
			return v;
		};

		const auto minimum = u16();
		const auto writer = u16();
		const auto flags = u8();
		if (!minimum || !writer || !flags || *minimum != 0 || (*flags & flag_backward) != 0) return {};

		std::optional<std::uint32_t> common;
		if ((*flags & flag_common_denominator) != 0)
		{
			common = u32();
			if (!common || *common == 0) return {};
		}

		// A value as its fraction: its numerator, then its denominator unless one is shared.
		const auto value = [&](const bool is_signed) -> std::optional<double>
		{
			const auto n = u32();
			const auto d = common ? common : u32();
			if (!n || !d || *d == 0) return {};
			const auto numerator = is_signed ? static_cast<double>(static_cast<std::int32_t>(*n))
			                                 : static_cast<double>(*n);
			return numerator / static_cast<double>(*d);
		};

		gain_map_metadata m;
		m.use_base_colour_space = (*flags & flag_base_colour_space) != 0;

		const auto base = value(false);
		const auto alternate = value(false);
		if (!base || !alternate) return {};
		m.base_headroom = *base;
		m.alternate_headroom = *alternate;

		const auto channels = (*flags & flag_multichannel) != 0 ? 3 : 1;

		for (auto c = 0; c < channels; ++c)
		{
			const auto min = value(true);
			const auto max = value(true);
			const auto gamma = value(false);
			const auto base_offset = value(true);
			const auto alternate_offset = value(true);
			if (!min || !max || !gamma || !base_offset || !alternate_offset || *gamma <= 0.0) return {};

			m.gain_min[c] = *min;
			m.gain_max[c] = *max;
			m.gamma[c] = *gamma;
			m.base_offset[c] = *base_offset;
			m.alternate_offset[c] = *alternate_offset;
		}

		for (auto c = channels; c < 3; ++c)
		{
			m.gain_min[c] = m.gain_min[0];
			m.gain_max[c] = m.gain_max[0];
			m.gamma[c] = m.gamma[0];
			m.base_offset[c] = m.base_offset[0];
			m.alternate_offset[c] = m.alternate_offset[0];
		}

		return m;
	}

	made_gain_map make_gain_map(const image_view& signal, const hdr_description& hdr, const image_view& base,
	                            const std::uint32_t scale)
	{
		made_gain_map made;
		const auto wide = [](const pixel_format f)
		{
			return f == pixel_format::rgbx16 || f == pixel_format::rgba16;
		};
		const auto packed = [](const pixel_format f)
		{
			return f == pixel_format::bgrx8 || f == pixel_format::bgra8;
		};

		if (scale == 0 || !wide(signal.format) || !signal.pixels || signal.width == 0 || signal.height == 0 ||
		    signal.stride < std::size_t{8} * signal.width || !base.pixels || base.width != signal.width ||
		    base.height != signal.height || (!wide(base.format) && !packed(base.format)) ||
		    base.stride < (wide(base.format) ? std::size_t{8} : std::size_t{4}) * base.width)
		{
			return made;
		}

		constexpr double offset = 1.0 / 64.0;
		const auto to_709 = detail::to_bt709(hdr.primaries);
		const detail::hdr_light light(hdr.transfer);

		std::vector<double> decode_8(256);
		for (auto i = 0; i < 256; ++i) decode_8[i] = detail::srgb_decode(i / 255.0);

		made.width = (signal.width + scale - 1) / scale;
		made.height = (signal.height + scale - 1) / scale;
		std::vector<double> sums(std::size_t{made.width} * made.height);
		std::vector<std::uint32_t> counts(sums.size());
		auto brightest = 1.0;

		for (std::uint32_t y = 0; y < signal.height; ++y)
		{
			const auto* const s = reinterpret_cast<const std::uint16_t*>(signal.pixels + signal.stride * y);
			const auto* const b = base.pixels + base.stride * y;

			for (std::uint32_t x = 0; x < signal.width; ++x)
			{
				const auto* const p = s + std::size_t{x} * 4;
				const std::array<double, 3> l{light.light(p[0]), light.light(p[1]), light.light(p[2])};
				const auto r = to_709[0] * l[0] + to_709[1] * l[1] + to_709[2] * l[2];
				const auto g = to_709[3] * l[0] + to_709[4] * l[1] + to_709[5] * l[2];
				const auto bl = to_709[6] * l[0] + to_709[7] * l[1] + to_709[8] * l[2];
				const auto hdr_luminance = std::max(0.2126 * r + 0.7152 * g + 0.0722 * bl, 0.0);
				brightest = std::max(brightest, hdr_luminance);

				std::array<double, 3> sdr{};
				if (wide(base.format))
				{
					const auto* const q = reinterpret_cast<const std::uint16_t*>(b) + std::size_t{x} * 4;
					for (auto c = 0; c < 3; ++c) sdr[c] = detail::srgb_decode(q[c] / 65535.0);
				}
				else
				{
					const auto* const q = b + std::size_t{x} * 4;
					sdr = {decode_8[q[2]], decode_8[q[1]], decode_8[q[0]]};
				}

				const auto sdr_luminance = 0.2126 * sdr[0] + 0.7152 * sdr[1] + 0.0722 * sdr[2];
				const auto at = std::size_t{y / scale} * made.width + x / scale;
				sums[at] += std::log2((hdr_luminance + offset) / (sdr_luminance + offset));
				++counts[at];
			}
		}

		auto low = std::numeric_limits<double>::max();
		auto high = std::numeric_limits<double>::lowest();

		for (std::size_t i = 0; i < sums.size(); ++i)
		{
			sums[i] /= counts[i];
			low = std::min(low, sums[i]);
			high = std::max(high, sums[i]);
		}

		auto& m = made.metadata;
		m.base_headroom = 0.0;
		m.gain_min.fill(low);
		m.gain_max.fill(high);
		m.gamma.fill(1.0);
		m.base_offset.fill(offset);
		m.alternate_offset.fill(offset);
		m.alternate_headroom = std::max({std::log2(brightest), high, 0.0});
		m.use_base_colour_space = true;

		made.pixels.resize(std::size_t{4} * made.width * made.height);
		const auto range = high - low;

		for (std::size_t i = 0; i < sums.size(); ++i)
		{
			const auto v = range > 0.0 ? (sums[i] - low) / range : 0.0;
			const auto byte = static_cast<std::uint8_t>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
			std::fill_n(made.pixels.begin() + static_cast<std::ptrdiff_t>(i * 4), 3, byte);
			made.pixels[i * 4 + 3] = 255;
		}

		return made;
	}

	gain_map_metadata convert_apple_gain_map(const pixel_buffer& pixels, const std::uint32_t width,
	                                         const std::uint32_t height, const double headroom)
	{
		const auto stops = headroom > 1.0 ? std::log2(headroom) : 0.0;
		std::array<std::uint8_t, 256> rewritten{};

		for (auto i = 0; i < 256; ++i)
		{
			const auto factor = 1.0 + (headroom - 1.0) * rec709_decode(i / 255.0);
			const auto v = stops > 0.0 ? std::log2(factor) / stops : 0.0;
			rewritten[i] = static_cast<std::uint8_t>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
		}

		for (std::uint32_t y = 0; y < height && pixels.pixels; ++y)
		{
			auto* p = pixels.pixels + pixels.stride * y;
			for (std::uint32_t x = 0; x < width; ++x, p += 4)
				for (auto c = 0; c < 3; ++c) p[c] = rewritten[p[c]];
		}

		return detail::apple_gain_map_metadata(headroom);
	}

	std::optional<double> apple_gain_map_headroom(const std::span<const std::uint8_t> gain_map_xmp,
	                                              const std::span<const std::uint8_t> exif)
	{
		if (!gain_map_xmp.empty())
		{
			try
			{
				xmp::register_namespace(apple_hdr_gain_map_ns, "HDRGainMap");
				xmp::meta meta;
				meta.parse({reinterpret_cast<const char*>(gain_map_xmp.data()), gain_map_xmp.size()});
				std::string text;
				if (meta.get_property(apple_hdr_gain_map_ns, "HDRGainMapHeadroom", &text))
				{
					if (const auto headroom = parse_number(text); headroom && *headroom > 1.0) return headroom;
				}
			}
			catch (const std::exception&)
			{
				// A packet the toolkit rejects whole says nothing; the maker note may.
			}
		}

		const auto note = apple_maker_note(exif);
		if (!note) return {};

		// Apple's formula, from "Applying Apple HDR effect to your photos": the stops the gain map brightens by.
		const auto [maker33, maker48] = *note;
		const auto stops = maker33 < 1.0 ? (maker48 <= 0.01 ? -20.0 * maker48 + 1.8 : -0.101 * maker48 + 1.601)
		                                 : (maker48 <= 0.01 ? -70.0 * maker48 + 3.0 : -0.303 * maker48 + 2.303);
		const auto headroom = std::pow(2.0, std::max(stops, 0.0));
		if (!(headroom > 1.0) || !std::isfinite(headroom)) return {};
		return headroom;
	}

	namespace detail
	{
		gain_map_metadata apple_gain_map_metadata(const double headroom)
		{
			const auto stops = headroom > 1.0 ? std::log2(headroom) : 0.0;
			gain_map_metadata m;
			m.base_headroom = 0.0;
			m.alternate_headroom = stops;
			m.gain_min.fill(0.0);
			m.gain_max.fill(stops);
			m.gamma.fill(1.0);
			m.base_offset.fill(0.0);
			m.alternate_offset.fill(0.0);
			m.use_base_colour_space = true;
			return m;
		}
	}
}
