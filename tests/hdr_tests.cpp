// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Tests for the HDR still mapping the decoders share: its tables against the mapping they speed up,
// and the mapping's own rules - PQ's midtones untouched and its highlights rolled into white, HLG's peak on
// white, and colours outside BT.709 fitted with their hue kept rather than clipped. And for the conversions hdr.h
// gives an application: an SDR edit carried into a signal, ISO 21496-1's metadata, a gain map made for an HDR
// picture, and Apple's gain map and headroom said in ISO 21496-1's terms.

#include "test.h"

#include "builders.h"
#include "hdr_internal.h"
#include "libfiles/hdr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using libfiles::hdr_description;
using libfiles::hdr_transfer;
using libfiles::detail::hdr_mapping;

namespace
{
	hdr_description bt2100(const hdr_transfer transfer, const double peak = 0.0)
	{
		hdr_description source;
		source.transfer = transfer;
		source.primaries = *libfiles::chromaticities_of(9);
		source.max_cll = peak;
		return source;
	}

	double pq_signal(const double nits)
	{
		const auto y = std::pow(nits / 10000.0, 2610.0 / 16384.0);
		return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * y) / (1.0 + 2392.0 / 128.0 * y), 2523.0 / 32.0);
	}

	double srgb(const double linear)
	{
		return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
	}
}

TEST("The HDR mapping's tables stay within a level of the mapping")
{
	for (const auto transfer : {hdr_transfer::pq, hdr_transfer::hlg})
	{
		const auto source = bt2100(transfer, 4000.0);
		const hdr_mapping mapping(source);
		auto worst = 0.0;

		for (auto r = 0; r <= 40; ++r)
			for (auto g = 0; g <= 40; ++g)
				for (auto b = 0; b <= 40; ++b)
				{
					const auto exact = hdr_mapping::evaluate(source, r / 40.0, g / 40.0, b / 40.0);
					const auto sampled = mapping.map(r / 40.0f, g / 40.0f, b / 40.0f);
					for (auto c = 0; c < 3; ++c) worst = std::max(worst, std::abs(exact[c] - sampled[c]));
				}

		// Within a level even where the fit turns hardest towards white, at a bright saturated yellow; a cube
		// read trilinearly was tens of levels out there.
		CHECK(worst < 1.0 / 255.0);
	}
}

TEST("PQ keeps its midtones and rolls its highlights into white")
{
	const auto source = bt2100(hdr_transfer::pq, 1000.0);
	const auto at = [&](const double nits)
	{
		const auto s = pq_signal(nits);
		return hdr_mapping::evaluate(source, s, s, s)[1];
	};

	CHECK(std::abs(at(50.0) - srgb(50.0 / 203.0)) < 1e-6);
	CHECK(at(203.0) < 1.0);
	CHECK(at(203.0) > at(150.0));
	CHECK(at(1000.0) > 0.999);
	CHECK(at(500.0) < at(1000.0));
	CHECK(at(10000.0) > 0.999);

	// No recorded peak is the 1000 nits most PQ is graded to.
	CHECK(std::abs(hdr_mapping::evaluate(bt2100(hdr_transfer::pq), 0.6, 0.6, 0.6)[0] -
	               hdr_mapping::evaluate(source, 0.6, 0.6, 0.6)[0]) < 1e-12);
}

TEST("HLG puts its peak on white and its reference white at a quarter of white's light")
{
	const auto source = bt2100(hdr_transfer::hlg);
	CHECK(hdr_mapping::evaluate(source, 1.0, 1.0, 1.0)[1] > 0.999);
	CHECK(std::abs(hdr_mapping::evaluate(source, 0.75, 0.75, 0.75)[1] - srgb(0.2649574)) < 1e-5);
	CHECK(hdr_mapping::evaluate(source, 0.0, 0.0, 0.0)[1] < 1e-9);
}

TEST("A wide-gamut colour is fitted into BT.709 with its hue kept")
{
	for (const auto transfer : {hdr_transfer::pq, hdr_transfer::hlg})
	{
		const auto source = bt2100(transfer);

		for (const auto& [r, g, b] : {std::array{0.6, 0.0, 0.0}, std::array{0.0, 0.6, 0.0}, std::array{0.0, 0.0, 0.6},
		                              std::array{0.6, 0.6, 0.0}, std::array{0.9, 0.2, 0.9}})
		{
			const auto out = hdr_mapping::evaluate(source, r, g, b);
			auto inside = true;
			for (const auto c : out) inside = inside && c >= 0.0 && c <= 1.0 && std::isfinite(c);
			CHECK(inside);

			// A single primary's channel stays the strongest.
			const std::array in{r, g, b};
			const auto strongest = std::max_element(in.begin(), in.end()) - in.begin();
			if (std::count(in.begin(), in.end(), in[strongest]) == 1)
			{
				CHECK(std::max_element(out.begin(), out.end()) - out.begin() == strongest);
			}
		}
	}
}

namespace
{
	double pq_nits(const double signal)
	{
		const auto p = std::pow(signal, 32.0 / 2523.0);
		return 10000.0 *
		       std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
	}

	double srgb_linear(const double encoded)
	{
		return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
	}

	// Sixteen-bit pixels, red first, the fourth channel full.
	struct signal_picture
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::vector<std::uint16_t> samples;

		libfiles::image_view view() const
		{
			return {reinterpret_cast<const std::uint8_t*>(samples.data()), std::size_t{8} * width, width, height,
			        libfiles::pixel_format::rgbx16};
		}

		libfiles::pixel_buffer buffer()
		{
			return {reinterpret_cast<std::uint8_t*>(samples.data()), std::size_t{8} * width};
		}

		std::uint16_t* at(const std::uint32_t x, const std::uint32_t y)
		{
			return samples.data() + (std::size_t{y} * width + x) * 4;
		}

		const std::uint16_t* at(const std::uint32_t x, const std::uint32_t y) const
		{
			return samples.data() + (std::size_t{y} * width + x) * 4;
		}
	};

	signal_picture greys(const std::vector<double>& nits)
	{
		signal_picture p{static_cast<std::uint32_t>(nits.size()), 1, std::vector<std::uint16_t>(nits.size() * 4)};
		for (std::size_t i = 0; i < nits.size(); ++i)
		{
			const auto v = static_cast<std::uint16_t>(std::lround(pq_signal(nits[i]) * 65535.0));
			p.samples[i * 4] = p.samples[i * 4 + 1] = p.samples[i * 4 + 2] = v;
			p.samples[i * 4 + 3] = 0xffff;
		}
		return p;
	}

	signal_picture rendition(const signal_picture& signal, const hdr_description& hdr)
	{
		signal_picture sdr{signal.width, signal.height, std::vector<std::uint16_t>(signal.samples.size())};
		CHECK(libfiles::map_hdr_to_sdr(signal.view(), hdr, libfiles::pixel_format::rgbx16, sdr.buffer()));
		return sdr;
	}
}

// What the edit left alone keeps its signal exactly; what it brightened is brightened in light by the factor its
// rendition was, with ISO 21496-1's offsets - so a highlight above SDR white keeps its headroom over the new base.
TEST("An SDR edit carried into an HDR signal scales each pixel's light as it scaled the rendition's")
{
	const auto hdr = bt2100(hdr_transfer::pq, 1000.0);
	const auto signal = greys({100.0, 600.0, 20.0});
	const auto before = rendition(signal, hdr);

	signal_picture out{signal.width, 1, std::vector<std::uint16_t>(signal.samples.size())};
	CHECK(libfiles::carry_sdr_edit(signal.view(), hdr, before.view(), before.view(), out.buffer()));
	CHECK(out.samples == signal.samples);

	// The first two brightened by a fifth in linear light; the third left as it was.
	auto after = before;
	for (std::uint32_t x = 0; x < 2; ++x)
		for (auto c = 0; c < 3; ++c)
		{
			auto& v = after.at(x, 0)[c];
			const auto linear = std::min(srgb_linear(v / 65535.0) * 1.2, 1.0);
			v = static_cast<std::uint16_t>(std::lround(srgb(linear) * 65535.0));
		}

	CHECK(libfiles::carry_sdr_edit(signal.view(), hdr, before.view(), after.view(), out.buffer()));
	constexpr auto offset = 1.0 / 64.0;

	for (std::uint32_t x = 0; x < 2; ++x)
	{
		const auto was = srgb_linear(before.at(x, 0)[1] / 65535.0);
		const auto now = srgb_linear(after.at(x, 0)[1] / 65535.0);
		const auto light = pq_nits(signal.samples[x * 4 + 1] / 65535.0) / 203.0;
		const auto expected = (now + offset) * (light + offset) / (was + offset) - offset;
		const auto got = pq_nits(out.at(x, 0)[1] / 65535.0) / 203.0;
		CHECK(std::abs(got - expected) / expected < 0.01);
	}

	// 600 nits is past SDR white, and stays past it, brighter than it was: its rendition was rolled off close to white,
	// so the edit could brighten that only to white, and the light follows.
	const auto brighter = pq_nits(out.at(1, 0)[1] / 65535.0);
	CHECK(brighter > pq_nits(signal.samples[5] / 65535.0) && brighter > 203.0);
	CHECK(std::equal(out.at(2, 0), out.at(2, 0) + 4, signal.samples.begin() + 8));

	// Renditions of another size are refused.
	const auto small = greys({100.0});
	CHECK(!libfiles::carry_sdr_edit(signal.view(), hdr, small.view(), small.view(), out.buffer()));
}

TEST("ISO 21496-1's gain map metadata reads back as it was written, one channel or three")
{
	libfiles::gain_map_metadata one;
	one.base_headroom = 0.0;
	one.alternate_headroom = 2.5;
	one.gain_min.fill(-0.25);
	one.gain_max.fill(2.5);
	one.gamma.fill(1.0);

	const auto written = libfiles::write_gain_map_metadata(one);
	// Versions, flags, two headrooms, and one channel of five fractions.
	CHECK_EQ(written.size(), 4u + 1u + 16u + 40u);
	CHECK_EQ(written[4], 0x40);
	const auto read = libfiles::read_gain_map_metadata(written);
	CHECK(read.has_value());
	CHECK(read && *read == one);

	auto three = one;
	three.gain_max = {2.0, 2.5, 3.0};
	three.base_offset = {0.0, 1.0 / 64.0, 1.0 / 32.0};
	three.use_base_colour_space = false;
	const auto multi = libfiles::write_gain_map_metadata(three);
	CHECK_EQ(multi.size(), 4u + 1u + 16u + 120u);
	CHECK_EQ(multi[4], 0x80);
	const auto read_three = libfiles::read_gain_map_metadata(multi);
	CHECK(read_three && *read_three == three);

	// One denominator shared by every value, as libultrahdr reads it.
	const auto common = test_build::builder()
	                        .be16(0)
	                        .be16(0)
	                        .u8(0x48)
	                        .be32(4)
	                        .be32(0)
	                        .be32(10)
	                        .be32(0)
	                        .be32(10)
	                        .be32(4)
	                        .be32(1)
	                        .be32(1)
	                        .b;
	const auto shared = libfiles::read_gain_map_metadata(common);
	CHECK(shared && shared->alternate_headroom == 2.5 && shared->gain_max[2] == 2.5 && shared->gamma[0] == 1.0 &&
	      shared->base_offset[1] == 0.25);

	// What cannot be written is not, and what is malformed or newer is not read.
	auto negative = one;
	negative.alternate_headroom = -1.0;
	CHECK(libfiles::write_gain_map_metadata(negative).empty());
	auto flat = one;
	flat.gamma[1] = 0.0;
	CHECK(libfiles::write_gain_map_metadata(flat).empty());
	CHECK(!libfiles::read_gain_map_metadata(std::span(written).first(written.size() - 1)));
	auto newer = written;
	newer[1] = 1;
	CHECK(!libfiles::read_gain_map_metadata(newer));
	auto backward = written;
	backward[4] |= 0x04;
	CHECK(!libfiles::read_gain_map_metadata(backward));
}

// Applied by its metadata to the SDR rendition it was made for, the gain map gives back the HDR picture's light.
TEST("A gain map made for an HDR picture brings its SDR rendition back to the picture's light")
{
	const auto hdr = bt2100(hdr_transfer::pq, 1000.0);
	signal_picture signal{8, 4, std::vector<std::uint16_t>(8 * 4 * 4)};
	for (std::uint32_t y = 0; y < 4; ++y)
		for (std::uint32_t x = 0; x < 8; ++x)
		{
			const auto v = static_cast<std::uint16_t>(std::lround(pq_signal(x < 4 ? 100.0 : 1000.0) * 65535.0));
			auto* const p = signal.at(x, y);
			p[0] = p[1] = p[2] = v;
			p[3] = 0xffff;
		}

	std::vector<std::uint8_t> base(8 * 4 * 4);
	CHECK(libfiles::map_hdr_to_sdr(signal.view(), hdr, libfiles::pixel_format::bgrx8, {base.data(), 32}));
	const libfiles::image_view base_view{base.data(), 32, 8, 4, libfiles::pixel_format::bgrx8};

	const auto made = libfiles::make_gain_map(signal.view(), hdr, base_view, 2);
	CHECK_EQ(made.width, 4u);
	CHECK_EQ(made.height, 2u);
	CHECK(made.pixels.size() == std::size_t{4} * 4 * 2);

	const auto& m = made.metadata;
	CHECK(m.gain_max[0] > m.gain_min[0] && m.alternate_headroom >= m.gain_max[0] && m.base_headroom == 0.0);

	for (std::uint32_t x = 0; x < 4; ++x)
	{
		const auto v = made.pixels[std::size_t{x} * 4] / 255.0;
		const auto gain = m.gain_min[0] + v * (m.gain_max[0] - m.gain_min[0]);
		const auto sdr = srgb_linear(base[std::size_t{x} * 2 * 4 + 1] / 255.0);
		const auto light = (sdr + m.base_offset[0]) * std::exp2(gain) - m.alternate_offset[0];
		const auto expected = (x < 2 ? 100.0 : 1000.0) / 203.0;
		CHECK(std::abs(light - expected) / expected < 0.03);
	}

	CHECK(libfiles::make_gain_map(signal.view(), hdr, base_view, 0).pixels.empty());
}

TEST("Apple's gain map, said in ISO 21496-1's terms, gives the light Apple's formula gives")
{
	std::vector<std::uint8_t> pixels;
	for (const auto v : {0, 64, 128, 200, 255})
	{
		const auto byte = static_cast<std::uint8_t>(v);
		pixels.insert(pixels.end(), {byte, byte, byte, 255});
	}
	const auto original = pixels;

	const auto m = libfiles::convert_apple_gain_map({pixels.data(), pixels.size()}, 5, 1, 4.0);
	CHECK(m.gain_min[0] == 0.0 && m.gain_max[0] == 2.0 && m.alternate_headroom == 2.0 && m.base_offset[0] == 0.0 &&
	      m.alternate_offset[0] == 0.0);

	for (std::size_t i = 0; i < 5; ++i)
	{
		const auto v = original[i * 4] / 255.0;
		const auto linear = v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
		const auto apple = 1.0 + 3.0 * linear;
		const auto iso = std::exp2(m.gain_max[0] * pixels[i * 4] / 255.0);
		CHECK(std::abs(iso - apple) / apple < 0.01);
		CHECK(pixels[i * 4] == pixels[i * 4 + 1] && pixels[i * 4 + 1] == pixels[i * 4 + 2] && pixels[i * 4 + 3] == 255);
	}
}

namespace
{
	// An EXIF block whose Exif directory holds an Apple maker note with tags 33 and 48: the note's own big-endian
	// directory after "Apple iOS", its values counted from the note's start.
	test_build::bytes apple_exif(const double maker33, const double maker48)
	{
		test_build::builder note;
		note.raw(std::string_view("Apple iOS\0", 10)).be16(1).raw("MM");
		note.be16(2);
		note.be16(0x0021).be16(10).be32(1).be32(44);
		note.be16(0x0030).be16(10).be32(1).be32(52);
		note.be32(0);
		note.be32(static_cast<std::uint32_t>(std::lround(maker33 * 1000000.0))).be32(1000000);
		note.be32(static_cast<std::uint32_t>(std::lround(maker48 * 1000000.0))).be32(1000000);

		test_build::builder tiff;
		tiff.raw("II").le16(42).le32(8);
		tiff.le16(1).le16(0x8769).le16(4).le32(1).le32(26).le32(0);
		tiff.le16(1).le16(0x927c).le16(7).le32(static_cast<std::uint32_t>(note.b.size())).le32(44).le32(0);
		tiff.raw(note.b);
		return tiff.b;
	}
}

TEST("Apple's gain map headroom comes from its own XMP, else the photo's maker note by Apple's formula")
{
	const std::string_view packet =
		R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about="" xmlns:HDRGainMap="http://ns.apple.com/HDRGainMap/1.0/" )"
		R"(HDRGainMap:HDRGainMapVersion="65536" HDRGainMap:HDRGainMapHeadroom="2.987"/></rdf:RDF></x:xmpmeta>)";
	const std::span xmp(reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size());

	const auto from_xmp = libfiles::apple_gain_map_headroom(xmp, apple_exif(1.01, 0.0098));
	CHECK(from_xmp && std::abs(*from_xmp - 2.987) < 1e-9);

	// 33 at least 1 and 48 at most 0.01: 3 - 70 * 0.0098 stops.
	const auto from_note = libfiles::apple_gain_map_headroom({}, apple_exif(1.01, 0.0098));
	CHECK(from_note && std::abs(*from_note - std::exp2(3.0 - 70.0 * 0.0098)) < 1e-6);
	// 33 below 1 and 48 above 0.01: 1.601 - 0.101 * 48 stops.
	const auto other = libfiles::apple_gain_map_headroom({}, apple_exif(0.5, 0.2));
	CHECK(other && std::abs(*other - std::exp2(1.601 - 0.101 * 0.2)) < 1e-6);

	CHECK(!libfiles::apple_gain_map_headroom({}, {}));
	CHECK(!libfiles::apple_gain_map_headroom({}, test_build::builder().raw("II").le16(42).le32(8).le16(0).le32(0).b));
}
