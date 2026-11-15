// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: The HDR still mapping and the 16-bit row writer the still decoders share, an HDR signal's light and
// back, and the public conversions hdr.h declares for a picture's signal: shown as SDR, and given the edit its SDR
// rendition was given; hdr_internal.h has the rules. The PQ and BT.2390 arithmetic follows Diffractor's HDR video,
// so a still and a clip from the same phone show alike.

#include "hdr_internal.h"
#include "libfiles/hdr.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace libfiles
{
	std::optional<chromaticities> chromaticities_of(const int colour_primaries) noexcept
	{
		switch (colour_primaries)
		{
		case 1: return chromaticities{};
		case 9: return chromaticities{0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
		case 11: return chromaticities{0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351};
		case 12: return chromaticities{0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
		default: return {};
		}
	}

	double hdr_description::peak_nits() const noexcept
	{
		if (max_cll > 0.0) return max_cll;
		if (mastering && mastering->max_luminance > 0.0) return mastering->max_luminance;
		if (intensity_target > 0.0 && intensity_target < 10000.0) return intensity_target;
		return 0.0;
	}
}

namespace libfiles::detail
{
	namespace
	{
		constexpr double default_pq_peak_nits = 1000.0;
		constexpr int gain_size = 4096;
		constexpr int encode_size = 4096;

		// SMPTE ST 2084: PQ signal, 0 to 1, against absolute light in nits.
		namespace pq
		{
			constexpr double m1 = 2610.0 / 16384.0;
			constexpr double m2 = 2523.0 / 4096.0 * 128.0;
			constexpr double c1 = 3424.0 / 4096.0;
			constexpr double c2 = 2413.0 / 4096.0 * 32.0;
			constexpr double c3 = 2392.0 / 4096.0 * 32.0;

			double to_nits(const double signal)
			{
				const auto p = std::pow(std::clamp(signal, 0.0, 1.0), 1.0 / m2);
				return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
			}

			double from_nits(const double nits)
			{
				const auto y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
				return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
			}
		}

		// BT.2100's HLG inverse OETF: signal, 0 to 1, to scene light, 0 to 1; and its OETF.
		namespace hlg
		{
			constexpr double a = 0.17883277;
			constexpr double b = 0.28466892;
			constexpr double c = 0.55991073;

			double to_scene(const double signal)
			{
				const auto e = std::clamp(signal, 0.0, 1.0);
				return e <= 0.5 ? e * e / 3.0 : (std::exp((e - c) / a) + b) / 12.0;
			}

			double from_scene(const double light)
			{
				const auto e = std::clamp(light, 0.0, 1.0);
				return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e) : a * std::log(12.0 * e - b) + c;
			}
		}

		// The BT.2390 EETF, between PQ signals: below the knee the signal passes untouched, so midtones and
		// reference white keep their brightness; above it a Hermite spline rolls the highlights into the
		// target's peak rather than clipping them.
		double bt2390_eetf(const double signal, const double source_peak, const double target_peak)
		{
			if (source_peak <= target_peak) return std::min(signal, target_peak);

			const auto max_lum = target_peak / source_peak;
			const auto knee = 1.5 * max_lum - 0.5;
			const auto e = std::clamp(signal / source_peak, 0.0, 1.0);
			if (e < knee) return e * source_peak;

			const auto t = (e - knee) / (1.0 - knee);
			const auto t2 = t * t;
			const auto t3 = t2 * t;
			const auto rolled = (2.0 * t3 - 3.0 * t2 + 1.0) * knee + (t3 - 2.0 * t2 + t) * (1.0 - knee) +
			                    (-2.0 * t3 + 3.0 * t2) * max_lum;

			return rolled * source_peak;
		}

		using matrix = std::array<double, 9>;
		using vector3 = std::array<double, 3>;

		matrix multiply(const matrix& a, const matrix& b)
		{
			matrix r{};
			for (auto i = 0; i < 3; ++i)
				for (auto j = 0; j < 3; ++j)
					for (auto k = 0; k < 3; ++k) r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
			return r;
		}

		vector3 transform_vector(const matrix& m, const vector3& v)
		{
			return {
				m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
				m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
				m[6] * v[0] + m[7] * v[1] + m[8] * v[2],
			};
		}

		vector3 xyz_of(const double x, const double y)
		{
			return {x / y, 1.0, (1.0 - x - y) / y};
		}

		// Linear RGB to XYZ, white at a luminance of 1.
		matrix rgb_to_xyz(const chromaticities& c)
		{
			const auto r = xyz_of(c.red_x, c.red_y);
			const auto g = xyz_of(c.green_x, c.green_y);
			const auto b = xyz_of(c.blue_x, c.blue_y);
			const matrix primaries{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
			const auto s = transform_vector(invert(primaries), xyz_of(c.white_x, c.white_y));
			return {r[0] * s[0], g[0] * s[1], b[0] * s[2], r[1] * s[0], g[1] * s[1],
			        b[1] * s[2], r[2] * s[0], g[2] * s[1], b[2] * s[2]};
		}

		// Bradford's adaptation from one white to another, for primaries not on D65 - DCI-P3's.
		matrix bradford(const chromaticities& from, const chromaticities& to)
		{
			constexpr matrix cone{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
			const auto source = transform_vector(cone, xyz_of(from.white_x, from.white_y));
			const auto target = transform_vector(cone, xyz_of(to.white_x, to.white_y));
			const matrix scale{target[0] / source[0], 0, 0, 0, target[1] / source[1], 0, 0, 0, target[2] / source[2]};
			return multiply(invert(cone), multiply(scale, cone));
		}

		struct mapping_constants
		{
			matrix to_709{};
			bool hlg = false;
			double source_peak = 0;
			double target_peak = 0;
		};

		mapping_constants constants_of(const hdr_description& source)
		{
			mapping_constants k;
			k.to_709 = to_bt709(source.primaries);
			k.hlg = source.transfer == hdr_transfer::hlg;
			const auto stated = source.peak_nits();
			const auto peak = stated > 0.0 ? std::clamp(stated, sdr_white_nits, 10000.0) : default_pq_peak_nits;
			k.source_peak = pq::from_nits(peak);
			k.target_peak = pq::from_nits(sdr_white_nits);
			return k;
		}

		std::array<double, 3> evaluate_with(const mapping_constants& k, const double r, const double g, const double b)
		{
			const vector3 signal{r, g, b};
			vector3 light{};

			for (auto c = 0; c < 3; ++c)
			{
				light[c] = k.hlg ? hlg::to_scene(signal[c]) * sdr_white_nits : pq::to_nits(signal[c]);
			}

			const auto rgb = transform_vector(k.to_709, light);
			const auto luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
			const auto mapped = k.hlg || luminance <= 0.0
			                        ? std::max(luminance, 0.0)
			                        : pq::to_nits(bt2390_eetf(pq::from_nits(luminance), k.source_peak, k.target_peak));
			const auto gain = luminance > 0.0 ? mapped / luminance : 0.0;
			const auto white = std::clamp(mapped / sdr_white_nits, 0.0, 1.0);

			vector3 relative{};
			auto fit = 1.0;

			for (auto c = 0; c < 3; ++c)
			{
				relative[c] = rgb[c] * gain / sdr_white_nits;
				if (relative[c] > 1.0) fit = std::min(fit, (1.0 - white) / (relative[c] - white));
				else if (relative[c] < 0.0) fit = std::min(fit, white / (white - relative[c]));
			}

			std::array<double, 3> out{};
			for (auto c = 0; c < 3; ++c)
				out[c] = srgb_encode(std::clamp(white + (relative[c] - white) * fit, 0.0, 1.0));
			return out;
		}

		std::uint16_t to_16(const float v)
		{
			return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
		}

		std::uint8_t to_8(const float v)
		{
			return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		bool is_wide(const pixel_format format)
		{
			return format == pixel_format::rgbx16 || format == pixel_format::rgba16;
		}

		bool is_packed(const pixel_format format)
		{
			return format == pixel_format::bgrx8 || format == pixel_format::bgra8;
		}

		bool covers(const image_view& v, const std::uint32_t width, const std::uint32_t height)
		{
			const auto pixel = is_wide(v.format) ? std::size_t{8} : std::size_t{4};
			return v.pixels && v.width == width && v.height == height && v.stride >= pixel * width &&
			       (is_wide(v.format) || is_packed(v.format));
		}

		// A pixel's red, green and blue, each 0 to 1, from bytes blue first or sixteen bits red first.
		std::array<float, 3> read_rgb(const image_view& v, const std::uint32_t x, const std::uint32_t y)
		{
			const auto* const row = v.pixels + v.stride * y;

			if (is_wide(v.format))
			{
				const auto* const p = reinterpret_cast<const std::uint16_t*>(row) + std::size_t{x} * 4;
				return {p[0] / 65535.0f, p[1] / 65535.0f, p[2] / 65535.0f};
			}

			const auto* const p = row + std::size_t{x} * 4;
			return {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f};
		}
	}

	std::optional<hdr_transfer> hdr_transfer_of(const int transfer_characteristics)
	{
		if (transfer_characteristics == 16) return hdr_transfer::pq;
		if (transfer_characteristics == 18) return hdr_transfer::hlg;
		return {};
	}

	int code_point_of(const hdr_transfer transfer)
	{
		return transfer == hdr_transfer::hlg ? 18 : 16;
	}

	std::string colour_primaries_text(const int code)
	{
		switch (code)
		{
		case 1: return "BT.709 primaries";
		case 2: return "unspecified primaries";
		case 4: return "BT.470 M primaries";
		case 5: return "BT.601 625-line primaries";
		case 6: return "BT.601 525-line primaries";
		case 7: return "SMPTE 240M primaries";
		case 8: return "film primaries";
		case 9: return "BT.2020 primaries";
		case 10: return "CIE XYZ";
		case 11: return "DCI-P3 primaries";
		case 12: return "Display P3 primaries";
		case 22: return "EBU 3213 primaries";
		default: return std::format("primaries {}", code);
		}
	}

	std::string transfer_text(const int code)
	{
		switch (code)
		{
		case 1:
		case 6:
		case 14:
		case 15: return "BT.709 transfer";
		case 2: return "unspecified transfer";
		case 4: return "gamma 2.2 transfer";
		case 5: return "gamma 2.8 transfer";
		case 7: return "SMPTE 240M transfer";
		case 8: return "linear transfer";
		case 13: return "sRGB transfer";
		case 16: return "PQ transfer (HDR)";
		case 17: return "DCI gamma 2.6 transfer";
		case 18: return "HLG transfer (HDR)";
		default: return std::format("transfer {}", code);
		}
	}

	std::string matrix_text(const int code)
	{
		switch (code)
		{
		case 0: return "identity matrix (RGB)";
		case 1: return "BT.709 matrix";
		case 2: return "unspecified matrix";
		case 4: return "FCC matrix";
		case 5:
		case 6: return "BT.601 matrix";
		case 7: return "SMPTE 240M matrix";
		case 8: return "YCgCo matrix";
		case 9: return "BT.2020 non-constant luminance matrix";
		case 10: return "BT.2020 constant luminance matrix";
		case 12: return "chromaticity-derived non-constant luminance matrix";
		case 13: return "chromaticity-derived constant luminance matrix";
		case 14: return "ICtCp matrix";
		default: return std::format("matrix {}", code);
		}
	}

	std::string nits_text(const double nits)
	{
		return nits >= 10.0 ? std::format("{:.0f} nits", nits) : std::format("{:.3g} nits", nits);
	}

	std::string light_level_text(const double max_cll, const double max_fall)
	{
		std::string text;
		if (max_cll > 0) text = std::format("brightest pixel {}", nits_text(max_cll));
		if (max_fall > 0)
			text += std::format("{}brightest frame average {}", text.empty() ? "" : ", ", nits_text(max_fall));
		return text.empty() ? std::string("unknown") : text;
	}

	std::string headroom_text(const double log2_headroom)
	{
		return std::format("up to {:.3g} times SDR white ({:.2f} stops)", std::exp2(log2_headroom), log2_headroom);
	}

	matrix invert(const matrix& m)
	{
		const auto c0 = m[4] * m[8] - m[5] * m[7];
		const auto c1 = m[5] * m[6] - m[3] * m[8];
		const auto c2 = m[3] * m[7] - m[4] * m[6];
		const auto det = m[0] * c0 + m[1] * c1 + m[2] * c2;
		if (std::abs(det) < 1e-12) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
		const auto inv = 1.0 / det;
		return {c0 * inv, (m[2] * m[7] - m[1] * m[8]) * inv, (m[1] * m[5] - m[2] * m[4]) * inv,
		        c1 * inv, (m[0] * m[8] - m[2] * m[6]) * inv, (m[2] * m[3] - m[0] * m[5]) * inv,
		        c2 * inv, (m[1] * m[6] - m[0] * m[7]) * inv, (m[0] * m[4] - m[1] * m[3]) * inv};
	}

	matrix to_bt709(const chromaticities& source)
	{
		const chromaticities bt709;
		auto m = rgb_to_xyz(source);
		if (std::abs(source.white_x - bt709.white_x) > 1e-4 || std::abs(source.white_y - bt709.white_y) > 1e-4)
		{
			m = multiply(bradford(source, bt709), m);
		}
		return multiply(invert(rgb_to_xyz(bt709)), m);
	}

	double srgb_encode(const double linear)
	{
		return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
	}

	double srgb_decode(const double encoded)
	{
		return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
	}

	hdr_light::hdr_light(const hdr_transfer transfer) : _hlg(transfer == hdr_transfer::hlg), _light(65536)
	{
		for (auto i = 0; i < 65536; ++i)
		{
			const auto signal = i / 65535.0;
			_light[i] = static_cast<float>(_hlg ? hlg::to_scene(signal) : pq::to_nits(signal) / sdr_white_nits);
		}
	}

	double hdr_light::signal(const double light) const noexcept
	{
		return _hlg ? hlg::from_scene(light) : pq::from_nits(light * sdr_white_nits);
	}

	double hdr_light::peak() const noexcept
	{
		return _hlg ? 1.0 : 10000.0 / sdr_white_nits;
	}

	hdr_mapping::hdr_mapping(const hdr_description& source)
	{
		const auto k = constants_of(source);
		_hlg = k.hlg;
		for (auto i = 0; i < 9; ++i) _to_709[i] = static_cast<float>(k.to_709[i]);

		_light.resize(65536);
		for (auto i = 0; i < 65536; ++i)
		{
			const auto signal = i / 65535.0;
			_light[i] = static_cast<float>(_hlg ? hlg::to_scene(signal) * sdr_white_nits : pq::to_nits(signal));
		}

		if (!_hlg)
		{
			_gain.resize(gain_size);
			for (auto i = 0; i < gain_size; ++i)
			{
				const auto root = i / (gain_size - 1.0);
				const auto nits = 10000.0 * root * root * root * root;
				_gain[i] = nits > 0.0
				               ? static_cast<float>(
									 pq::to_nits(bt2390_eetf(pq::from_nits(nits), k.source_peak, k.target_peak)) / nits)
				               : 1.0f;
			}
		}

		_encode.resize(encode_size);
		for (auto i = 0; i < encode_size; ++i)
		{
			const auto root = i / (encode_size - 1.0);
			_encode[i] = static_cast<float>(srgb_encode(root * root));
		}
	}

	std::array<double, 3> hdr_mapping::evaluate(const hdr_description& source, const double r, const double g,
	                                            const double b)
	{
		return evaluate_with(constants_of(source), r, g, b);
	}

	std::array<float, 3> hdr_mapping::map(const float r, const float g, const float b) const noexcept
	{
		const auto light = [this](const float v)
		{
			return _light[static_cast<std::size_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f)];
		};
		const auto read = [](const std::vector<float>& table, const float position)
		{
			const auto at = std::clamp(position, 0.0f, 1.0f) * static_cast<float>(table.size() - 1);
			const auto index = std::min(static_cast<std::size_t>(at), table.size() - 2);
			return table[index] + (table[index + 1] - table[index]) * (at - static_cast<float>(index));
		};

		const std::array linear{light(r), light(g), light(b)};
		std::array<float, 3> rgb{};
		for (auto c = 0; c < 3; ++c)
		{
			rgb[c] = _to_709[c * 3] * linear[0] + _to_709[c * 3 + 1] * linear[1] + _to_709[c * 3 + 2] * linear[2];
		}

		const auto luminance = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
		if (luminance <= 0.0f) return {};

		const auto gain = _hlg ? 1.0f : read(_gain, std::sqrt(std::sqrt(std::min(luminance / 10000.0f, 1.0f))));
		const auto white = std::clamp(luminance * gain / static_cast<float>(sdr_white_nits), 0.0f, 1.0f);

		std::array<float, 3> relative{};
		auto fit = 1.0f;

		for (auto c = 0; c < 3; ++c)
		{
			relative[c] = rgb[c] * gain / static_cast<float>(sdr_white_nits);
			if (relative[c] > 1.0f) fit = std::min(fit, (1.0f - white) / (relative[c] - white));
			else if (relative[c] < 0.0f) fit = std::min(fit, white / (white - relative[c]));
		}

		std::array<float, 3> out{};
		for (auto c = 0; c < 3; ++c)
		{
			out[c] = read(_encode, std::sqrt(std::clamp(white + (relative[c] - white) * fit, 0.0f, 1.0f)));
		}

		return out;
	}

	void write_wide_rows(const std::uint8_t* const source, const std::size_t source_stride,
	                     const std::uint32_t max_value, const std::uint32_t width, const std::uint32_t height,
	                     const bool premultiplied, const hdr_mapping* const mapping, const pixel_format format,
	                     const pixel_buffer& pixels)
	{
		const auto scale = 1.0f / static_cast<float>(std::max(max_value, 1u));
		const auto wide = is_wide(format);
		const auto opaque = format == pixel_format::rgbx16 || format == pixel_format::bgrx8;

		for (std::uint32_t y = 0; y < height; ++y)
		{
			const auto* src = reinterpret_cast<const std::uint16_t*>(source + source_stride * y);
			auto* const row = pixels.pixels + pixels.stride * y;

			for (std::uint32_t x = 0; x < width; ++x, src += 4)
			{
				auto r = src[0] * scale;
				auto g = src[1] * scale;
				auto b = src[2] * scale;
				const auto a = src[3] * scale;

				if (premultiplied)
				{
					if (a > 0.0f)
					{
						r = std::min(r / a, 1.0f);
						g = std::min(g / a, 1.0f);
						b = std::min(b / a, 1.0f);
					}
					else
					{
						r = g = b = 0.0f;
					}
				}

				if (mapping)
				{
					const auto mapped = mapping->map(r, g, b);
					r = mapped[0];
					g = mapped[1];
					b = mapped[2];
				}

				if (wide)
				{
					auto* const out = reinterpret_cast<std::uint16_t*>(row) + static_cast<std::size_t>(x) * 4;
					out[0] = to_16(r);
					out[1] = to_16(g);
					out[2] = to_16(b);
					out[3] = opaque ? std::uint16_t{0xffff} : to_16(a);
				}
				else
				{
					auto* const out = row + static_cast<std::size_t>(x) * 4;
					out[0] = to_8(b);
					out[1] = to_8(g);
					out[2] = to_8(r);
					out[3] = opaque ? std::uint8_t{0xff} : to_8(a);
				}
			}
		}
	}

	pixel_format output_format(const int bits_per_channel, const bool has_alpha, const decode_options& options,
	                           const bool hdr)
	{
		if ((bits_per_channel > 8 && options.allow_16bit) || (hdr && options.keep_hdr))
			return has_alpha ? pixel_format::rgba16 : pixel_format::rgbx16;
		return has_alpha ? pixel_format::bgra8 : pixel_format::bgrx8;
	}
}

namespace libfiles
{
	bool map_hdr_to_sdr(const image_view& signal, const hdr_description& hdr, const pixel_format format,
	                    const pixel_buffer& out)
	{
		if (!detail::is_wide(signal.format) || !detail::covers(signal, signal.width, signal.height) || !out.pixels ||
		    (!detail::is_wide(format) && !detail::is_packed(format)) ||
		    out.stride < (detail::is_wide(format) ? std::size_t{8} : std::size_t{4}) * signal.width)
		{
			return false;
		}

		const detail::hdr_mapping mapping(hdr);
		detail::write_wide_rows(signal.pixels, signal.stride, 65535, signal.width, signal.height, false, &mapping,
		                        format, out);
		return true;
	}

	bool carry_sdr_edit(const image_view& signal, const hdr_description& hdr, const image_view& before,
	                    const image_view& after, const pixel_buffer& out)
	{
		const auto width = signal.width;
		const auto height = signal.height;

		if (!detail::is_wide(signal.format) || !detail::covers(signal, width, height) ||
		    !detail::covers(before, width, height) || !detail::covers(after, width, height) || !out.pixels ||
		    out.stride < std::size_t{8} * width)
		{
			return false;
		}

		// The renditions are BT.709 and sRGB-encoded; the gain each channel takes is worked out in the signal's own
		// primaries, where the picture's colours outside BT.709 still are.
		const auto from_709 = detail::invert(detail::to_bt709(hdr.primaries));
		const detail::hdr_light light(hdr.transfer);
		const auto peak = light.peak();
		constexpr double offset = 1.0 / 64.0;

		std::vector<float> decode_16(65536);
		for (auto i = 0; i < 65536; ++i) decode_16[i] = static_cast<float>(detail::srgb_decode(i / 65535.0));

		const auto linear = [&](const image_view& v, const std::uint32_t x, const std::uint32_t y)
		{
			const auto encoded = detail::read_rgb(v, x, y);
			std::array<double, 3> rgb{};
			for (auto c = 0; c < 3; ++c)
				rgb[c] = decode_16[static_cast<std::size_t>(std::clamp(encoded[c], 0.0f, 1.0f) * 65535.0f + 0.5f)];

			return std::array<double, 3>{
				from_709[0] * rgb[0] + from_709[1] * rgb[1] + from_709[2] * rgb[2],
				from_709[3] * rgb[0] + from_709[4] * rgb[1] + from_709[5] * rgb[2],
				from_709[6] * rgb[0] + from_709[7] * rgb[1] + from_709[8] * rgb[2],
			};
		};

		for (std::uint32_t y = 0; y < height; ++y)
		{
			const auto* const src = reinterpret_cast<const std::uint16_t*>(signal.pixels + signal.stride * y);
			auto* const dst = reinterpret_cast<std::uint16_t*>(out.pixels + out.stride * y);

			for (std::uint32_t x = 0; x < width; ++x)
			{
				const auto* const s = src + std::size_t{x} * 4;
				auto* const d = dst + std::size_t{x} * 4;
				const auto was = linear(before, x, y);
				const auto now = linear(after, x, y);

				for (auto c = 0; c < 3; ++c)
				{
					// What the edit left alone keeps its signal exactly, rather than whatever a round trip through
					// the transfer gives back.
					if (std::abs(now[c] - was[c]) < 1e-9)
					{
						d[c] = s[c];
						continue;
					}

					const auto l = static_cast<double>(light.light(s[c]));
					const auto edited = (std::max(now[c], 0.0) + offset) * (l + offset) /
					                        (std::max(was[c], 0.0) + offset) -
					                    offset;
					d[c] = static_cast<std::uint16_t>(
						std::clamp(light.signal(std::clamp(edited, 0.0, peak)), 0.0, 1.0) * 65535.0 + 0.5);
				}

				d[3] = signal.format == pixel_format::rgba16 ? s[3] : std::uint16_t{0xffff};
			}
		}

		return true;
	}
}
