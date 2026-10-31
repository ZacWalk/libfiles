// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Reading an EXIF block's picture metadata, and the summary a scan takes from one. The walk
// covers IFD0 and the Exif, GPS and Interoperability directories it points to - and any they point to
// in turn - with a Canon maker note's lens. It moved from Diffractor's EXIF parser: its walk, what it
// takes for a well-formed entry, how it decodes text, and which of two tags that record the same thing
// wins. The orientation and IFD1's thumbnail are the summary's, by the one rule the scan, the TIFF
// scanner and the JPEG decoder share.

#include "libfiles/exif.h"
#include "libfiles/types.h"
#include "text_internal.h"
#include "tiff_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace libfiles
{
	using namespace detail;

	namespace
	{
		// A Canon maker note's settings and lens, beside the tags tiff_internal.h names.
		constexpr std::uint16_t tag_canon_settings = 0x0001;
		constexpr std::uint16_t tag_canon_lens = 0x0095;

		// How many directories one walk reads, whatever the block's pointers say.
		constexpr std::uint32_t max_directories = 32;

		// A thumbnail's length is an unvalidated 32-bit field; this caps what a scan may be asked to read.
		constexpr std::uint32_t max_embedded_thumbnail_bytes = 8u * 1024u * 1024u;

		// The text tags a Samsung camera filled with its own binary records start with these bytes.
		bool is_junk(const std::uint8_t* const p, const std::size_t size)
		{
			static constexpr std::uint8_t junk_marker[] = {0x12, 0xf8, 0x0f, 0x3b};
			return size >= 4 && std::memcmp(p, junk_marker, 4) == 0;
		}

		// Control characters other than tab, newline and return show a payload is not readable text.
		bool is_binary_text(const std::string_view text)
		{
			for (const auto c : text)
			{
				const auto u = static_cast<std::uint8_t>(c);
				if (u < 0x20 && u != '\t' && u != '\n' && u != '\r') return true;
				if (u == 0x7f) return true;
			}

			return false;
		}

		std::string_view trim(const std::string_view s)
		{
			if (s.empty()) return {};

			constexpr std::string_view whitespace{" \n\r\t\f\v\0", 7};
			const auto start = s.find_first_not_of(whitespace);
			if (start == std::string_view::npos) return {};

			const auto end = s.find_last_not_of(whitespace);
			return s.substr(start, end - start + 1);
		}

		std::string_view strip_nul(const std::string_view s)
		{
			if (s.empty()) return {};

			constexpr std::string_view nul{"\0", 1};
			const auto start = s.find_first_not_of(nul);
			if (start == std::string_view::npos) return {};

			const auto end = s.find_last_not_of(nul);
			return s.substr(start, end - start + 1);
		}

		// Padding goes before the binary test: trailing NULs and spaces are normal in EXIF text.
		std::string clean_text(const std::string_view text)
		{
			const auto trimmed = trim(text);
			if (trimmed.empty() || is_binary_text(trimmed)) return {};
			return std::string(strip_nul(trimmed));
		}

		// Unmarked UTF-16 leaves every other byte of Latin text zero, and which side holds the zero says
		// which way round it is.
		bool is_utf16(const std::uint8_t* const p, const std::size_t size)
		{
			if (size < 2) return false;
			if (p[0] == 0xff && p[1] == 0xfe) return !(size >= 4 && p[2] == 0 && p[3] == 0);
			if (p[0] == 0xfe && p[1] == 0xff) return true;
			if (size < 4) return false;
			return (p[0] == 0 && p[2] == 0) || (p[1] == 0 && p[3] == 0);
		}

		std::u16string utf16_from_bytes(const std::uint8_t* p, std::size_t size)
		{
			if (size < 2) return {};

			auto big_endian = false;

			if (p[0] == 0xfe && p[1] == 0xff)
			{
				big_endian = true;
				p += 2;
				size -= 2;
			}
			else if (p[0] == 0xff && p[1] == 0xfe)
			{
				p += 2;
				size -= 2;
			}
			else
			{
				big_endian = size >= 4 && p[0] == 0 && p[2] == 0 && !(p[1] == 0 && p[3] == 0);
			}

			std::u16string result(size / 2, u'\0');

			for (std::size_t i = 0; i < result.size(); ++i)
			{
				const auto first = p[i * 2];
				const auto second = p[i * 2 + 1];
				result[i] = static_cast<char16_t>(big_endian ? first << 8 | second : second << 8 | first);
			}

			return result;
		}

		std::string trimmed_utf8(const std::u16string_view text)
		{
			const auto utf8 = utf8_from_utf16(text);
			return std::string(trim(utf8));
		}

		bool equals_ignoring_ascii_case(const std::string_view text, const std::string_view lower)
		{
			if (text.size() != lower.size()) return false;

			for (std::size_t i = 0; i < text.size(); ++i)
			{
				auto c = static_cast<unsigned char>(text[i]);
				if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
				if (c != static_cast<unsigned char>(lower[i])) return false;
			}

			return true;
		}

		bool is_zero(const float x)
		{
			return std::fabs(x) <= std::numeric_limits<float>::epsilon();
		}

		// A rating of -1 is "rejected"; above 5 it is Windows' percent, read as every scale libfiles reads is.
		int safe_rating(const int r)
		{
			if (r < 0) return -1;
			if (r > 5) return stars_from_percent(r);
			return r;
		}

		double aperture_to_f_number(const double apex)
		{
			return std::exp(std::log(2.0) * apex / 2.0);
		}

		// Canon's lens ids, from its CameraSettings. Where an id names more than one lens, the first.
		std::string canon_lens_name(const int id)
		{
			static const std::unordered_map<int, const char*> names = {
			{1, "Canon EF 50mm f/1.8"},
			{2, "Canon EF 28mm f/2.8"},
			{3, "Canon EF 135mm f/2.8 Soft"},
			{4, "Canon EF 35-105mm f/3.5-4.5"}, // 0
			{4, "Sigma UC Zoom 35-135mm f/4-5.6"}, // 1
			{5, "Canon EF 35-70mm f/3.5-4.5"},
			{6, "Canon EF 28-70mm f/3.5-4.5"}, // 0
			{6, "Sigma 18-50mm f/3.5-5.6 DC"}, // 1
			{6, "Sigma 18-125mm f/3.5-5.6 DC IF ASP"}, // 2
			{6, "Tokina AF193-2 19-35mm f/3.5-4.5"}, // 3
			{6, "Sigma 28-80mm f/3.5-5.6 II Macro"}, // 4
			{7, "Canon EF 100-300mm f/5.6L"},
			{8, "Canon EF 100-300mm f/5.6"}, // 0
			{8, "Sigma 70-300mm f/4-5.6 [APO] DG Macro"}, // 1
			{8, "Tokina AT-X 242 AF 24-200mm f/3.5-5.6"}, // 2
			{9, "Canon EF 70-210mm f/4"}, // 0
			{9, "Sigma 55-200mm f/4-5.6 DC"}, // 1
			{10, "Canon EF 50mm f/2.5 Macro"}, // 0
			{10, "Sigma 50mm f/2.8 EX"}, // 1
			{10, "Sigma 28mm f/1.8"}, // 2
			{10, "Sigma 105mm f/2.8 Macro EX"}, // 3
			{10, "Sigma 70mm f/2.8 EX DG Macro EF"}, // 4
			{11, "Canon EF 35mm f/2"},
			{13, "Canon EF 15mm f/2.8 Fisheye"},
			{14, "Canon EF 50-200mm f/3.5-4.5L"},
			{15, "Canon EF 50-200mm f/3.5-4.5"},
			{16, "Canon EF 35-135mm f/3.5-4.5"},
			{17, "Canon EF 35-70mm f/3.5-4.5A"},
			{18, "Canon EF 28-70mm f/3.5-4.5"},
			{20, "Canon EF 100-200mm f/4.5A"},
			{21, "Canon EF 80-200mm f/2.8L"},
			{22, "Canon EF 20-35mm f/2.8L"}, // 0
			{22, "Tokina AT-X 280 AF PRO 28-80mm f/2.8 Aspherical"}, // 1
			{23, "Canon EF 35-105mm f/3.5-4.5"},
			{24, "Canon EF 35-80mm f/4-5.6 Power Zoom"},
			{25, "Canon EF 35-80mm f/4-5.6 Power Zoom"},
			{26, "Canon EF 100mm f/2.8 Macro"}, // 0
			{26, "Cosina 100mm f/3.5 Macro AF"}, // 1
			{26, "Tamron SP AF 90mm f/2.8 Di Macro"}, // 2
			{26, "Tamron SP AF 180mm f/3.5 Di Macro"}, // 3
			{26, "Carl Zeiss Planar T* 50mm f/1.4"}, // 4
			{27, "Canon EF 35-80mm f/4-5.6"},
			{28, "Canon EF 80-200mm f/4.5-5.6"}, // 0
			{28, "Tamron SP AF 28-105mm f/2.8 LD Aspherical IF"}, // 1
			{28, "Tamron SP AF 28-75mm f/2.8 XR Di LD Aspherical [IF] Macro"}, // 2
			{28, "Tamron AF 70-300mm f/4.5-5.6 Di LD 1:2 Macro Zoom"}, // 3
			{28, "Tamron AF Aspherical 28-200mm f/3.8-5.6"}, // 4
			{29, "Canon EF 50mm f/1.8 MkII"},
			{30, "Canon EF 35-105mm f/4.5-5.6"},
			{31, "Canon EF 75-300mm f/4-5.6"}, // 0
			{31, "Tamron SP AF 300mm f/2.8 LD IF"}, // 1
			{32, "Canon EF 24mm f/2.8"}, // 0
			{32, "Sigma 15mm f/2.8 EX Fisheye"}, // 1
			{33, "Voigtlander or Carl Zeiss Lens"}, // 0
			{33, "Voigtlander Ultron 40mm f/2 SLII Aspherical"}, // 1
			{33, "Carl Zeiss Distagon 15mm T* f/2.8 ZE"}, // 2
			{33, "Carl Zeiss Distagon 18mm T* f/3.5 ZE"}, // 3
			{33, "Carl Zeiss Distagon 21mm T* f/2.8 ZE"}, // 4
			{33, "Carl Zeiss Distagon 28mm T* f/2 ZE"}, // 5
			{33, "Carl Zeiss Distagon 35mm T* f/2 ZE"}, // 6
			{33, "Carl Zeiss Planar 50mm T* f/1.4 ZE"}, // 7
			{35, "Canon EF 35-80mm f/4-5.6"},
			{36, "Canon EF 38-76mm f/4.5-5.6"},
			{37, "Canon EF 35-80mm f/4-5.6"}, // 0
			{37, "Tamron 70-200mm f/2.8 Di LD IF Macro"}, // 1
			{37, "Tamron AF 28-300mm f/3.5-6.3 XR Di VC LD Aspherical [IF] Macro Model A20"}, // 2
			{37, "Tamron SP AF 17-50mm f/2.8 XR Di II VC LD Aspherical [IF] "}, // 3
			{37, "Tamron AF 18-270mm f/3.5-6.3 Di II VC LD Aspherical [IF] Macro"}, // 4
			{38, "Canon EF 80-200mm f/4.5-5.6"},
			{39, "Canon EF 75-300mm f/4-5.6"},
			{40, "Canon EF 28-80mm f/3.5-5.6"},
			{41, "Canon EF 28-90mm f/4-5.6"},
			{42, "Canon EF 28-200mm f/3.5-5.6"}, // 0
			{42, "Tamron AF 28-300mm f/3.5-6.3 XR Di VC LD Aspherical [IF] Macro Model A20"}, // 1
			{43, "Canon EF 28-105mm f/4-5.6"},
			{44, "Canon EF 90-300mm f/4.5-5.6"},
			{45, "Canon EF-S 18-55mm f/3.5-5.6"},
			{46, "Canon EF 28-90mm f/4-5.6"},
			{48, "Canon EF-S 18-55mm f/3.5-5.6 IS"},
			{49, "Canon EF-S 55-250mm f/4-5.6 IS"},
			{50, "Canon EF-S 18-200mm f/3.5-5.6 IS"},
			{51, "Canon EF-S 18-135mm f/3.5-5.6 IS"},
			{52, "Canon EF-S 18-55mm f/3.5-5.6 IS II"},
			{53, "Canon EF-S 18-55mm f/3.5-5.6 III"},
			{54, "Canon EF-S 55-250mm f/4-5.6 IS II"},
			{94, "Canon TS-E 17mm f/4L"},
			{95, "Canon TS-E 24.0mm f/3.5 L II"},
			{124, "Canon MP-E 65mm f/2.8 1-5x Macro Photo"},
			{125, "Canon TS-E 24mm f/3.5L"},
			{126, "Canon TS-E 45mm f/2.8"},
			{127, "Canon TS-E 90mm f/2.8"},
			{129, "Canon EF 300mm f/2.8L"},
			{130, "Canon EF 50mm f/1.0L"},
			{131, "Canon EF 28-80mm f/2.8-4L"}, // 0
			{131, "Sigma 8mm f/3.5 EX DG Circular Fisheye"}, // 1
			{131, "Sigma 17-35mm f/2.8-4 EX DG Aspherical HSM"}, // 2
			{131, "Sigma 17-70mm f/2.8-4.5 DC Macro"}, // 3
			{131, "Sigma APO 50-150mm f/2.8 EX DC HSM"}, // 4
			{131, "Sigma APO 120-300mm f/2.8 EX DG HSM"}, // 5
			{131, "Sigma 4.5mm F2.8 EX DC HSM Circular Fisheye"}, // 6
			{131, "Sigma 70-200mm f/2.8 APO EX HSM"}, // 7
			{132, "Canon EF 1200mm f/5.6L"},
			{134, "Canon EF 600mm f/4L IS"},
			{135, "Canon EF 200mm f/1.8L"},
			{136, "Canon EF 300mm f/2.8L"},
			{137, "Canon EF 85mm f/1.2L"}, // 0
			{137, "Sigma 18-50mm f/2.8-4.5 DC OS HSM"}, // 1
			{137, "Sigma 50-200mm f/4-5.6 DC OS HSM"}, // 2
			{137, "Sigma 18-250mm f/3.5-6.3 DC OS HSM"}, // 3
			{137, "Sigma 24-70mm f/2.8 IF EX DG HSM"}, // 4
			{137, "Sigma 18-125mm f/3.8-5.6 DC OS HSM"}, // 5
			{137, "Sigma 17-70mm f/2.8-4 DC Macro OS HSM"}, // 6
			{137, "Sigma 17-50mm f/2.8 OS HSM"}, // 7
			{137, "Sigma 18-200mm f/3.5-6.3 II DC OS HSM"}, // 8
			{137, "Tamron AF 18-270mm f/3.5-6.3 Di II VC PZD"}, // 9
			{137, "Sigma 8-16mm f/4.5-5.6 DC HSM"}, // 10
			{137, "Tamron SP 17-50mm f/2.8 XR Di II VC"}, // 11
			{137, "Tamron SP 60mm f/2 Macro Di II"}, // 12
			{137, "Sigma 10-20mm f/3.5 EX DC HSM"}, // 13
			{137, "Tamron SP 24-70mm f/2.8 Di VC USD"}, // 14
			{137, "Sigma 18-35mm f/1.8 DC HSM"}, // 15
			{137, "Sigma 12-24mm f/4.5-5.6 DG HSM II"}, // 16
			{138, "Canon EF 28-80mm f/2.8-4L"},
			{139, "Canon EF 400mm f/2.8L"},
			{140, "Canon EF 500mm f/4.5L"},
			{141, "Canon EF 500mm f/4.5L"},
			{142, "Canon EF 300mm f/2.8L IS"},
			{143, "Canon EF 500mm f/4L IS"},
			{144, "Canon EF 35-135mm f/4-5.6 USM"},
			{145, "Canon EF 100-300mm f/4.5-5.6 USM"},
			{146, "Canon EF 70-210mm f/3.5-4.5 USM"},
			{147, "Canon EF 35-135mm f/4-5.6 USM"},
			{148, "Canon EF 28-80mm f/3.5-5.6 USM"},
			{149, "Canon EF 100mm f/2 USM"},
			{150, "Canon EF 14mm f/2.8L"}, // 0
			{150, "Sigma 20mm EX f/1.8"}, // 1
			{150, "Sigma 30mm f/1.4 DC HSM"}, // 2
			{150, "Sigma 24mm f/1.8 DG Macro EX"}, // 3
			{151, "Canon EF 200mm f/2.8L"},
			{152, "Canon EF 300mm f/4L"}, // 0
			{152, "Sigma 12-24mm f/4.5-5.6 EX DG ASPHERICAL HSM"}, // 1
			{152, "Sigma 14mm f/2.8 EX Aspherical HSM"}, // 2
			{152, "Sigma 10-20mm f/4-5.6"}, // 3
			{152, "Sigma 100-300mm f/4"}, // 4
			{153, "Canon EF 35-350mm f/3.5-5.6L"}, // 0
			{153, "Sigma 50-500mm f/4-6.3 APO HSM EX"}, // 1
			{153, "Tamron AF 28-300mm f/3.5-6.3 XR LD Aspherical [IF] Macro"}, // 2
			{153, "Tamron AF 18-200mm f/3.5-6.3 XR Di II LD Aspherical [IF] Macro Model A14"}, // 3
			{153, "Tamron 18-250mm f/3.5-6.3 Di II LD Aspherical [IF] Macro"}, // 4
			{154, "Canon EF 20mm f/2.8 USM"},
			{155, "Canon EF 85mm f/1.8 USM"},
			{156, "Canon EF 28-105mm f/3.5-4.5 USM"}, // 0
			{156, "Tamron SP AF 70-300mm f/4-5.6 Di VC USD"}, // 1
			{160, "Canon EF 20-35mm f/3.5-4.5 USM"}, // 0
			{160, "Tamron AF 19-35mm f/3.5-4.5"}, // 1
			{160, "Tokina AT-X 124 AF 12-24mm f/4 DX"}, // 2
			{160, "Tokina AT-X 107 AF DX Fish-eye 10-17mm f/3.5-4.5"}, // 3
			{160, "Tokina AT-X 116 PRO DX AF 11-16mm f/2.8"}, // 4
			{161, "Canon EF 28-70mm f/2.8L"}, // 0
			{161, "Sigma 24-70mm EX f/2.8"}, // 1
			{161, "Sigma 28-70mm f/2.8 EX"}, // 2
			{161, "Tamron AF 17-50mm f/2.8 Di-II LD Aspherical"}, // 3
			{161, "Tamron 90mm f/2.8"}, // 4
			{161, "Sigma 24-60mm f/2.8 EX DG"}, // 5
			{162, "Canon EF 200mm f/2.8L"},
			{163, "Canon EF 300mm f/4L"},
			{164, "Canon EF 400mm f/5.6L"},
			{165, "Canon EF 70-200mm f/2.8 L"},
			{166, "Canon EF 70-200mm f/2.8 L + 1.4x"},
			{167, "Canon EF 70-200mm f/2.8 L + 2x"},
			{168, "Canon EF 28mm f/1.8 USM"},
			{169, "Canon EF 17-35mm f/2.8L"}, // 0
			{169, "Sigma 18-200mm f/3.5-6.3 DC OS"}, // 1
			{169, "Sigma 15-30mm f/3.5-4.5 EX DG Aspherical"}, // 2
			{169, "Sigma 18-50mm f/2.8 Macro"}, // 3
			{169, "Sigma 50mm f/1.4 EX DG HSM"}, // 4
			{169, "Sigma 85mm f/1.4 EX DG HSM"}, // 5
			{169, "Sigma 30mm f/1.4 EX DC HSM"}, // 6
			{169, "Sigma 35mm f/1.4 DG HSM"}, // 7
			{170, "Canon EF 200mm f/2.8L II"},
			{171, "Canon EF 300mm f/4L"},
			{172, "Canon EF 400mm f/5.6L"},
			{173, "Canon EF 180mm Macro f/3.5L"}, // 0
			{173, "Sigma 180mm EX HSM Macro f/3.5"}, // 1
			{173, "Sigma APO Macro 150mm f/3.5 EX DG IF HSM"}, // 2
			{174, "Canon EF 135mm f/2L"}, // 0
			{174, "Sigma 70-200mm f/2.8 EX DG APO OS HSM"}, // 1
			{174, "Sigma 50-500mm f/4.5-6.3 APO DG OS HSM"}, // 2
			{174, "Sigma 150-500mm f/5-6.3 APO DG OS HSM"}, // 3
			{175, "Canon EF 400mm f/2.8L"},
			{176, "Canon EF 24-85mm f/3.5-4.5 USM"},
			{177, "Canon EF 300mm f/4L IS"},
			{178, "Canon EF 28-135mm f/3.5-5.6 IS"},
			{179, "Canon EF 24mm f/1.4L"},
			{180, "Canon EF 35mm f/1.4L"},
			{181, "Canon EF 100-400mm f/4.5-5.6L IS + 1.4x"},
			{182, "Canon EF 100-400mm f/4.5-5.6L IS + 2x"},
			{183, "Canon EF 100-400mm f/4.5-5.6L IS"}, // 0
			{183, "Sigma 150mm f/2.8 EX DG OS HSM APO Macro"}, // 1
			{183, "Sigma 105mm f/2.8 EX DG OS HSM Macro"}, // 2
			{184, "Canon EF 400mm f/2.8L + 2x"},
			{185, "Canon EF 600mm f/4L IS"},
			{186, "Canon EF 70-200mm f/4L"},
			{187, "Canon EF 70-200mm f/4L + 1.4x"},
			{188, "Canon EF 70-200mm f/4L + 2x"},
			{189, "Canon EF 70-200mm f/4L + 2.8x"},
			{190, "Canon EF 100mm f/2.8 Macro"},
			{191, "Canon EF 400mm f/4 DO IS"},
			{193, "Canon EF 35-80mm f/4-5.6 USM"},
			{194, "Canon EF 80-200mm f/4.5-5.6 USM"},
			{195, "Canon EF 35-105mm f/4.5-5.6 USM"},
			{196, "Canon EF 75-300mm f/4-5.6 USM"},
			{197, "Canon EF 75-300mm f/4-5.6 IS USM"},
			{198, "Canon EF 50mm f/1.4 USM"}, // 0
			{198, "Zeiss Otus 55mm f/1.4 ZE"}, // 1
			{199, "Canon EF 28-80mm f/3.5-5.6 USM"},
			{200, "Canon EF 75-300mm f/4-5.6 USM"},
			{201, "Canon EF 28-80mm f/3.5-5.6 USM"},
			{202, "Canon EF 28-80mm f/3.5-5.6 USM IV"},
			{208, "Canon EF 22-55mm f/4-5.6 USM"},
			{209, "Canon EF 55-200mm f/4.5-5.6"},
			{210, "Canon EF 28-90mm f/4-5.6 USM"},
			{211, "Canon EF 28-200mm f/3.5-5.6 USM"},
			{212, "Canon EF 28-105mm f/4-5.6 USM"},
			{213, "Canon EF 90-300mm f/4.5-5.6 USM"},
			{214, "Canon EF-S 18-55mm f/3.5-5.6 USM"},
			{215, "Canon EF 55-200mm f/4.5-5.6 II USM"},
			{224, "Canon EF 70-200mm f/2.8L IS"},
			{225, "Canon EF 70-200mm f/2.8L IS + 1.4x"},
			{226, "Canon EF 70-200mm f/2.8L IS + 2x"},
			{227, "Canon EF 70-200mm f/2.8L IS + 2.8x"},
			{228, "Canon EF 28-105mm f/3.5-4.5 USM"},
			{229, "Canon EF 16-35mm f/2.8L"},
			{230, "Canon EF 24-70mm f/2.8L"},
			{231, "Canon EF 17-40mm f/4L"},
			{232, "Canon EF 70-300mm f/4.5-5.6 DO IS USM"},
			{233, "Canon EF 28-300mm f/3.5-5.6L IS"},
			{234, "Canon EF-S 17-85mm f4-5.6 IS USM"},
			{235, "Canon EF-S 10-22mm f/3.5-4.5 USM"},
			{236, "Canon EF-S 60mm f/2.8 Macro USM"},
			{237, "Canon EF 24-105mm f/4L IS"},
			{238, "Canon EF 70-300mm f/4-5.6 IS USM"},
			{239, "Canon EF 85mm f/1.2L II"},
			{240, "Canon EF-S 17-55mm f/2.8 IS USM"},
			{241, "Canon EF 50mm f/1.2L"},
			{242, "Canon EF 70-200mm f/4L IS"},
			{243, "Canon EF 70-200mm f/4L IS + 1.4x"},
			{244, "Canon EF 70-200mm f/4L IS + 2x"},
			{245, "Canon EF 70-200mm f/4L IS + 2.8x"},
			{246, "Canon EF 16-35mm f/2.8L II"},
			{247, "Canon EF 14mm f/2.8L II USM"},
			{248, "Canon EF 200mm f/2L IS"},
			{249, "Canon EF 800mm f/5.6L IS"},
			{250, "Canon EF 24 f/1.4L II"},
			{251, "Canon EF 70-200mm f/2.8L IS II USM"},
			{252, "Canon EF 70-200mm f/2.8L IS II USM + 1.4x"},
			{253, "Canon EF 70-200mm f/2.8L IS II USM + 2x"},
			{254, "Canon EF 100mm f/2.8L Macro IS USM"},
			{488, "Canon EF-S 15-85mm f/3.5-5.6 IS USM"},
			{489, "Canon EF 70-300mm f/4-5.6L IS USM"},
			{490, "Canon EF 8-15mm f/4L USM"},
			{491, "Canon EF 300mm f/2.8L IS II USM"},
			{492, "Canon EF 400mm f/2.8L IS II USM"},
			{493, "Canon EF 24-105mm f/4L IS USM"},
			{494, "Canon EF 600mm f/4.0L IS II USM"},
			{495, "Canon EF 24-70mm f/2.8L II USM"},
			{496, "Canon EF 200-400mm f/4L IS USM"},
			{502, "Canon EF 28mm f/2.8 IS USM"},
			{503, "Canon EF 24mm f/2.8 IS USM"},
			{504, "Canon EF 24-70mm f/4L IS USM"},
			{505, "Canon EF 35mm f/2 IS USM"},
			{4142, "Canon EF-S 18-135mm f/3.5-5.6 IS STM"},
			{4143, "Canon EF-M 18-55mm f/3.5-5.6 IS STM"},
			{4144, "Canon EF 40mm f/2.8 STM"},
			{4145, "Canon EF-M 22mm f/2 STM"},
			{4146, "Canon EF-S 18-55mm f/3.5-5.6 IS STM"},
			{4147, "Canon EF-M 11-22mm f/4-5.6 IS STM"}
			};

			const auto found = names.find(id);
			return found != names.end() ? std::string(found->second) : std::string();
		}

	}

	std::string detail::tiff_bytes::text(const std::uint32_t i, const std::uint32_t size) const
	{
		const auto* p = at(i, size);
		if (!p) return {};

		if (size > 7 && std::memcmp(p, "UNICODE", 7) == 0)
		{
			if (size > 9)
			{
				p += 8;
				std::u16string units((size - 8) / 2, u'\0');

				for (std::size_t c = 0; c < units.size(); ++c)
				{
					units[c] = static_cast<char16_t>(_little_endian
						                                 ? p[c * 2] | p[c * 2 + 1] << 8
						                                 : p[c * 2] << 8 | p[c * 2 + 1]);
				}

				return trimmed_utf8(units);
			}
		}
		else if (size > 5 && std::memcmp(p, "ASCII", 5) == 0)
		{
			if (size > 8)
			{
				p += 8;
				return is_junk(p, size - 8)
					       ? std::string()
					       : clean_text({reinterpret_cast<const char*>(p), size - 8u});
			}
		}
		else if (is_junk(p, size))
		{
			return {};
		}
		else if (is_utf16(p, size))
		{
			return trimmed_utf8(utf16_from_bytes(p, size));
		}
		else if (detail::is_utf8(p, size))
		{
			return clean_text({reinterpret_cast<const char*>(p), size});
		}
		else
		{
			return "?";
		}

		return {};
	}

	std::string detail::entry::text() const
	{
		if (type != type_ascii && type != type_undefined) return "?";
		return data.text(value_offset(), size());
	}

	namespace
	{
		// Windows writes its XP tags - XPTitle, XPComment, XPAuthor, XPKeywords and XPSubject - as BYTE
		// arrays of UTF-16LE text ending in a NUL, whatever the block's byte order. A writer that stores
		// one as text is read as any text tag is.
		std::string xp_text(const entry& e)
		{
			if (e.type != type_byte) return e.text();

			const auto bytes = e.raw_text();
			std::u16string units;

			for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
			{
				const auto unit = static_cast<char16_t>(static_cast<std::uint8_t>(bytes[i]) |
					static_cast<std::uint8_t>(bytes[i + 1]) << 8);
				if (unit == 0) break;
				units += unit;
			}

			return trimmed_utf8(units);
		}

		// Turns the entries into metadata, in the order the walk reads them.
		class reading
		{
			exif_metadata& _m;
			bool _south = false;
			bool _west = false;
			double _latitude = 180.0;
			double _longitude = 180.0;
			bool _below_sea_level = false;
			float _speed = 0.0f;
			float _speed_to_kmh = 1.0f;

		public:
			explicit reading(exif_metadata& m) : _m(m)
			{
			}

			void finish() const
			{
				// 180 marks a coordinate not read. Only the exact pair 0, 0 is refused besides, which is
				// what a receiver with no fix writes: the equator and the Greenwich meridian are places.
				if (std::fabs(_latitude) < 180.0 && std::fabs(_longitude) < 180.0 &&
					!(_latitude == 0.0 && _longitude == 0.0))
				{
					auto latitude = std::fabs(_latitude);
					auto longitude = std::fabs(_longitude);
					if (_south) latitude = 0.0 - latitude;
					if (_west) longitude = 0.0 - longitude;
					_m.latitude = latitude;
					_m.longitude = longitude;
				}

				if (_below_sea_level) _m.altitude = -_m.altitude;
				_m.speed = _speed * _speed_to_kmh;
			}

			void read(const entry& e)
			{
				switch (e.kind)
				{
				case directory_kind::exif:
					read_exif_tag(e);
					break;
				case directory_kind::gps:
					read_gps_tag(e);
					break;
				case directory_kind::canon:
					read_canon_tag(e);
					break;
				case directory_kind::thumbnail:
					// IFD1 is the summary's, not the reading's.
					break;
				}
			}

		private:
			void read_exif_tag(const entry& e)
			{
				switch (e.tag)
				{
				case tag_aperture:
					if (is_zero(_m.f_number)) _m.f_number = static_cast<float>(aperture_to_f_number(e.rational_at()));
					break;
				case tag_f_number:
					_m.f_number = static_cast<float>(e.rational_at());
					break;
				case tag_max_aperture:
					if (is_zero(_m.f_number)) _m.f_number = static_cast<float>(aperture_to_f_number(e.rational_at()));
					break;
				case tag_exposure_time:
					_m.exposure_time = static_cast<float>(e.signed_rational_at());
					break;
				case tag_shutter_speed:
					if (is_zero(_m.exposure_time))
						_m.exposure_time = static_cast<float>(std::pow(2.0, -e.signed_rational_at()));
					break;
				case tag_iso_speed:
					_m.iso_speed = e.first_u16();
					break;
				case tag_focal_length:
					_m.focal_length = static_cast<float>(e.rational_at());
					break;
				case tag_focal_length_35mm:
					_m.focal_length_35mm = e.first_u16();
					break;
				case tag_make:
					_m.camera_make = e.text();
					break;
				case tag_model:
					_m.camera_model = e.text();
					break;
				case tag_software:
					if (_m.software.empty()) _m.software = e.text();
					break;
				case tag_artist:
					if (_m.artist.empty()) _m.artist = e.text();
					break;
				case tag_lens_model:
					_m.lens = e.text();
					break;
				case tag_copyright:
					_m.copyright = e.text();
					break;
				case tag_image_description:
					_m.description = e.text();
					break;
				case tag_user_comment:
					_m.comment = e.text();
					break;
				case tag_xp_comment:
					if (_m.comment.empty()) _m.comment = xp_text(e);
					break;
				case tag_xp_title:
					if (_m.title.empty()) _m.title = xp_text(e);
					break;
				case tag_xp_author:
					if (_m.artist.empty()) _m.artist = xp_text(e);
					break;
				case tag_xp_keywords:
					if (_m.keywords.empty()) _m.keywords = xp_text(e);
					break;
				case tag_xp_subject:
					if (_m.description.empty()) _m.description = xp_text(e);
					break;
				case tag_rating:
					_m.rating = static_cast<std::int16_t>(safe_rating(e.field_s16()));
					break;
				case tag_rating_percent:
					_m.rating = static_cast<std::int16_t>(e.field_s16() < 0 ? -1 : stars_from_percent(e.field_s16()));
					break;
				case tag_date_time:
					_m.modified.text = e.ascii();
					break;
				case tag_date_time_original:
					_m.original.text = e.ascii();
					break;
				case tag_date_time_digitized:
					_m.digitized.text = e.ascii();
					break;
				case tag_offset_time:
					_m.modified.offset = e.ascii();
					break;
				case tag_offset_time_original:
					_m.original.offset = e.ascii();
					break;
				case tag_offset_time_digitized:
					_m.digitized.offset = e.ascii();
					break;
				case tag_sub_sec_time:
					_m.modified.sub_second = e.ascii();
					break;
				case tag_sub_sec_time_original:
					_m.original.sub_second = e.ascii();
					break;
				case tag_sub_sec_time_digitized:
					_m.digitized.sub_second = e.ascii();
					break;
				default:
					break;
				}
			}

			void read_gps_tag(const entry& e)
			{
				switch (e.tag)
				{
				case tag_gps_latitude:
					_latitude = e.rational_at(0) + e.rational_at(1) / 60.0 + e.rational_at(2) / 3600.0;
					break;
				case tag_gps_latitude_ref:
					_south = !e.raw_text().empty() && e.raw_text()[0] == 'S';
					break;
				case tag_gps_longitude:
					_longitude = e.rational_at(0) + e.rational_at(1) / 60.0 + e.rational_at(2) / 3600.0;
					break;
				case tag_gps_longitude_ref:
					_west = !e.raw_text().empty() && e.raw_text()[0] == 'W';
					break;
				case tag_gps_altitude:
					_m.altitude = static_cast<float>(e.rational_at());
					break;
				case tag_gps_altitude_ref:
					// A byte, not text: 1 means below sea level. The sign is applied once the walk is over,
					// because the reference may come either side of the value.
					_below_sea_level = e.first_byte() == 1;
					break;
				case tag_gps_speed:
					_speed = static_cast<float>(e.rational_at());
					break;
				case tag_gps_speed_ref:
					{
						const auto unit = e.raw_text();
						const auto first = unit.empty() ? '\0' : unit[0];
						// 'K' kilometres an hour, the default; 'M' miles an hour; 'N' knots.
						_speed_to_kmh = first == 'M' ? 1.609344f : first == 'N' ? 1.852f : 1.0f;
					}
					break;
				case tag_gps_date_stamp:
					_m.gps_date = e.ascii();
					break;
				case tag_gps_time_stamp:
					// Three rationals, or it is not a time: one that reads short would record midnight.
					if (e.count >= 3 && e.type == type_rational)
					{
						_m.gps_time = std::array<double, 3>{e.rational_at(0), e.rational_at(1), e.rational_at(2)};
					}
					break;
				default:
					break;
				}
			}

			void read_canon_tag(const entry& e)
			{
				switch (e.tag)
				{
				case tag_canon_settings:
					// Windows often corrupted this array.
					if (e.count > 27 && e.count < 1000)
					{
						const auto id = e.s16_at(22);
						const auto high = e.s16_at(23);
						const auto low = e.s16_at(24);
						const auto units = e.s16_at(25);

						std::string name;
						if (id > 0) name = canon_lens_name(id);

						if (name.empty() && (low >= 0 || high >= 0) && units > 0)
						{
							const auto low_mm = low / static_cast<double>(units);
							const auto high_mm = high / static_cast<double>(units);

							if (std::fabs(low_mm - high_mm) < 0.1 || low_mm < 0.1)
							{
								name = std::format("{:.1}mm", high_mm);
							}
							else if (low_mm >= 0.1 && high_mm >= 0.1)
							{
								name = std::format("{:.1}f-{:.1}mm", low_mm, high_mm);
							}
						}

						if (!name.empty()) _m.lens = name;
					}
					break;
				case tag_canon_lens:
					{
						const auto text = e.raw_text();
						if (!text.empty() && text[0] != 0) _m.lens = std::string(strip_nul(text));
					}
					break;
				default:
					break;
				}
			}
		};

		// The walk: IFD0 depth first through every directory it points to. A directory is read once
		// however many pointers lead to it, and no more than max_directories in all.
		class walk
		{
			const tiff_bytes& _data;
			reading& _reading;
			const std::uint32_t _value_limit;
			std::uint32_t _visited[max_directories] = {};
			std::uint32_t _visited_count = 0;
			std::string _make;

		public:
			walk(const tiff_bytes& data, reading& r, const std::uint32_t value_limit) :
				_data(data), _reading(r), _value_limit(value_limit)
			{
			}

			void run()
			{
				if (const auto ifd0 = first_directory(_data)) directory(*ifd0, directory_kind::exif);
			}

		private:
			void directory(const std::uint32_t offset, const directory_kind kind)
			{
				if (_data.overflows(offset, 2)) return;

				for (std::uint32_t i = 0; i < _visited_count; ++i)
				{
					if (_visited[i] == offset) return;
				}

				if (_visited_count >= max_directories) return;
				_visited[_visited_count++] = offset;

				for_each_entry(_data, offset, kind, [this](const entry& e) { visit(e); });
			}

			// A maker note is Canon's when the camera says it is, and it is not one of the formats that
			// open with their own vendor's name.
			bool is_canon_maker_note(const entry& e) const
			{
				const auto size = e.size();
				const auto* const p = _data.at(e.field_u32(), size >= 8 ? 8u : 4u);
				if (!p) return false;

				if (size >= 8 && (std::memcmp(p, "OLYMP", 6) == 0 || std::memcmp(p, "OLYMPUS", 8) == 0 ||
					std::memcmp(p, "SANYO", 6) == 0 || std::memcmp(p, "EPSON", 6) == 0 ||
					std::memcmp(p, "Nikon", 6) == 0))
				{
					return false;
				}

				return equals_ignoring_ascii_case(_make, "canon");
			}

			void visit(const entry& e)
			{
				switch (e.tag)
				{
				case tag_exif_ifd:
					directory(e.field_u32(), directory_kind::exif);
					break;
				case tag_gps_ifd:
					directory(e.field_u32(), directory_kind::gps);
					break;
				case tag_interop_ifd:
					directory(e.field_u32(), directory_kind::exif);
					break;
				case tag_maker_note:
					if (is_canon_maker_note(e)) directory(e.field_u32(), directory_kind::canon);
					break;
				// The orientation and the thumbnail are the summary's: IFD0's and IFD1's, and no others.
				case tag_orientation:
				case tag_jpeg_interchange_format:
				case tag_jpeg_interchange_format_length:
					break;
				default:
					// A TIFF file can point a text tag at gigabytes of image data. EXIF blocks are
					// already bounded by their container; the TIFF scanner supplies a per-value cap.
					if (_value_limit != std::numeric_limits<std::uint32_t>::max() &&
						std::uint64_t{e.count} * bytes_per_type[e.type] > _value_limit) break;
					if (e.tag == tag_make) _make = e.text();
					_reading.read(e);
					break;
				}
			}
		};
	}

	exif_summary detail::summarize(const tiff_bytes& data)
	{
		exif_summary summary;

		const auto ifd0 = first_directory(data);
		if (!ifd0) return summary;

		for_each_entry(data, *ifd0, directory_kind::exif, [&summary](const entry& e)
		{
			if (e.tag == tag_orientation) summary.orientation = e.field_u16();
		});

		const auto ifd1 = next_directory(data, *ifd0);
		if (!ifd1) return summary;

		std::optional<std::uint16_t> thumbnail_orientation;
		std::uint32_t thumbnail_offset = 0;
		std::uint32_t thumbnail_size = 0;

		for_each_entry(data, ifd1, directory_kind::thumbnail, [&](const entry& e)
		{
			// One SHORT or LONG. Anything else read as one yields a plausible but fabricated place.
			const auto scalar = [&e]() -> std::uint32_t
			{
				if (e.count != 1) return 0;
				if (e.type == type_short) return e.field_u16();
				if (e.type == type_long) return e.field_u32();
				return 0;
			};

			switch (e.tag)
			{
			case tag_orientation:
				thumbnail_orientation = e.field_u16();
				break;
			case tag_jpeg_interchange_format:
				thumbnail_offset = scalar();
				break;
			case tag_jpeg_interchange_format_length:
				thumbnail_size = scalar();
				break;
			default:
				break;
			}
		});

		// IFD1 describes the thumbnail, which is often stored upright while the primary image is not, so
		// its orientation stands in only where IFD0 gives none.
		if (!summary.orientation) summary.orientation = thumbnail_orientation;

		if (thumbnail_offset != 0 && thumbnail_size != 0 && thumbnail_size <= max_embedded_thumbnail_bytes &&
			!data.overflows(thumbnail_offset, thumbnail_size))
		{
			summary.thumbnail = {thumbnail_offset, thumbnail_size};
		}

		return summary;
	}

	exif_metadata detail::read_metadata(const tiff_bytes& data, const std::uint32_t value_limit)
	{
		exif_metadata m;
		reading r(m);
		walk(data, r, value_limit).run();
		r.finish();
		m.orientation = summarize(data).orientation;
		return m;
	}

	namespace
	{
		// The byte order a TIFF stream opens with, or nothing for a stream too short to hold any of it.
		std::optional<bool> little_endian(const std::span<const std::uint8_t> tiff)
		{
			if (tiff.size() <= 16) return {};
			if (tiff[0] == 0x49 && tiff[1] == 0x49) return true;
			if (tiff[0] == 0x4d && tiff[1] == 0x4d) return false;
			return {};
		}
	}

	exif_metadata read_exif(const std::span<const std::uint8_t> tiff)
	{
		exif_metadata result;

		const auto order = little_endian(tiff);
		if (!order) return result;

		reader bytes(tiff);
		return detail::read_metadata(tiff_bytes(bytes, *order), std::numeric_limits<std::uint32_t>::max());
	}

	exif_summary summarize_exif(const std::span<const std::uint8_t> tiff) noexcept
	{
		const auto order = little_endian(tiff);
		if (!order) return {};

		// Every read is bounded first, so nothing here throws; a fault would still not escape.
		try
		{
			reader bytes(tiff);
			return summarize(tiff_bytes(bytes, *order));
		}
		catch (const std::exception&)
		{
			return {};
		}
	}
}
