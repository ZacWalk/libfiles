// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What LibRaw's parse of a raw file holds, listed by section for an inspector: identification,
// lens, shooting and exposure, temperatures, environment, GPS, colour, sizes, thumbnails and each maker's
// own - and the rule a GPS position LibRaw parsed must meet. Moved from Diffractor's raw scan unchanged,
// its formats with it: "{:0.1}" gives one significant digit, so a focal length of 100 mm lists as
// "1e+02 mm", and a value listed with a name for it lists without the name.

#include "raw_internal.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace libfiles::detail
{
	namespace
	{
		struct id2hr_t
		{
			std::uint64_t id;
			const char* name;
		};

		constexpr id2hr_t MountNames[] = {
			{LIBRAW_MOUNT_Alpa, "Alpa"},
			{LIBRAW_MOUNT_C, "C-mount"},
			{LIBRAW_MOUNT_Canon_EF_M, "Canon EF-M"},
			{LIBRAW_MOUNT_Canon_EF_S, "Canon EF-S"},
			{LIBRAW_MOUNT_Canon_EF, "Canon EF"},
			{LIBRAW_MOUNT_Canon_RF, "Canon RF"},
			{LIBRAW_MOUNT_Contax_N, "Contax N"},
			{LIBRAW_MOUNT_Contax645, "Contax 645"},
			{LIBRAW_MOUNT_FT, "4/3"},
			{LIBRAW_MOUNT_mFT, "m4/3"},
			{LIBRAW_MOUNT_Fuji_GF, "Fuji G"}, // Fujifilm G lenses, GFX cameras
			{LIBRAW_MOUNT_Fuji_GX, "Fuji GX"}, // GX680
			{LIBRAW_MOUNT_Fuji_X, "Fuji X"},
			{LIBRAW_MOUNT_Hasselblad_H, "Hasselblad H"}, // Hn cameras, HC & HCD lenses
			{LIBRAW_MOUNT_Hasselblad_V, "Hasselblad V"},
			{LIBRAW_MOUNT_Hasselblad_XCD, "Hasselblad XCD"}, // Xn cameras, XCD lenses
			{LIBRAW_MOUNT_Leica_M, "Leica M"},
			{LIBRAW_MOUNT_Leica_R, "Leica R"},
			{LIBRAW_MOUNT_Leica_S, "Leica S"},
			{LIBRAW_MOUNT_Leica_SL, "Leica SL"}, // mounts on "L" throat
			{LIBRAW_MOUNT_Leica_TL, "Leica TL"}, // mounts on "L" throat
			{LIBRAW_MOUNT_LPS_L, "LPS L-mount"}, // throat, Leica / Panasonic / Sigma
			{LIBRAW_MOUNT_Mamiya67, "Mamiya RZ/RB"}, // Mamiya RB67, RZ67
			{LIBRAW_MOUNT_Mamiya645, "Mamiya 645"},
			{LIBRAW_MOUNT_Minolta_A, "Sony/Minolta A"},
			{LIBRAW_MOUNT_Nikon_CX, "Nikkor 1"},
			{LIBRAW_MOUNT_Nikon_F, "Nikkor F"},
			{LIBRAW_MOUNT_Nikon_Z, "Nikkor Z"},
			{LIBRAW_MOUNT_Pentax_645, "Pentax 645"},
			{LIBRAW_MOUNT_Pentax_K, "Pentax K"},
			{LIBRAW_MOUNT_Pentax_Q, "Pentax Q"},
			{LIBRAW_MOUNT_RicohModule, "Ricoh module"},
			{LIBRAW_MOUNT_Rollei_bayonet, "Rollei bayonet"}, // Rollei Hy-6: Leaf AFi, Sinar Hy6- models
			{LIBRAW_MOUNT_Samsung_NX_M, "Samsung NX-M"},
			{LIBRAW_MOUNT_Samsung_NX, "Samsung NX"},
			{LIBRAW_MOUNT_Sigma_X3F, "Sigma SA/X3F"},
			{LIBRAW_MOUNT_Sony_E, "Sony E"},
			// generic formats:
			{LIBRAW_MOUNT_LF, "Large format"},
			{LIBRAW_MOUNT_DigitalBack, "Digital Back"},
			{LIBRAW_MOUNT_FixedLens, "Fixed Lens"},
			{LIBRAW_MOUNT_IL_UM, "Interchangeable lens, mount unknown"},
			{LIBRAW_MOUNT_Unknown, "Undefined Mount or Fixed Lens"},
			{LIBRAW_MOUNT_TheLastOne, "The Last One"},
		};

		constexpr id2hr_t FormatNames[] = {
			{LIBRAW_FORMAT_1div2p3INCH, "1/2.3\""},
			{LIBRAW_FORMAT_1div1p7INCH, "1/1.7\""},
			{LIBRAW_FORMAT_1INCH, "1\""},
			{LIBRAW_FORMAT_FT, "4/3"},
			{LIBRAW_FORMAT_APSC, "APS-C"}, // Canon: 22.3x14.9mm; Sony et al: 23.6-23.7x15.6mm
			{LIBRAW_FORMAT_Leica_DMR, "Leica DMR"}, // 26.4x 17.6mm
			{LIBRAW_FORMAT_APSH, "APS-H"}, // Canon: 27.9x18.6mm
			{LIBRAW_FORMAT_FF, "FF 35mm"},
			{LIBRAW_FORMAT_CROP645, "645 crop 44x33mm"},
			{LIBRAW_FORMAT_LeicaS, "Leica S 45x30mm"},
			{LIBRAW_FORMAT_3648, "48x36mm"},
			{LIBRAW_FORMAT_645, "6x4.5"},
			{LIBRAW_FORMAT_66, "6x6"},
			{LIBRAW_FORMAT_67, "6x7"},
			{LIBRAW_FORMAT_68, "6x8"},
			{LIBRAW_FORMAT_69, "6x9"},
			{LIBRAW_FORMAT_SigmaAPSC, "Sigma APS-C"}, //  Sigma Foveon X3 orig: 20.7x13.8mm
			{LIBRAW_FORMAT_SigmaMerrill, "Sigma Merrill"},
			{LIBRAW_FORMAT_SigmaAPSH, "Sigma APS-H"}, // Sigma "H"26.7 x 17.9mm
			{LIBRAW_FORMAT_MF, "Medium Format"},
			{LIBRAW_FORMAT_LF, "Large format"},
			{LIBRAW_FORMAT_Unknown, "Unknown"},
			{LIBRAW_FORMAT_TheLastOne, "The Last One"},
		};

		constexpr id2hr_t NikonCrops[] = {
			{0, "Uncropped"}, {1, "1.3x"}, {2, "DX"},
			{3, "5:4"}, {4, "3:2"}, {6, "16:9"},
			{8, "2.7x"}, {9, "DX Movie"}, {10, "1.3x Movie"},
			{11, "FX Uncropped"}, {12, "DX Uncropped"}, {15, "1.5x Movie"},
			{17, "1:1"},
		};

		constexpr id2hr_t FujiCrops[] = {
			{0, "Uncropped"},
			{1, "GFX FF"},
			{2, "Sports Finder Mode"},
			{4, "Electronic Shutter 1.25x Crop"},
		};

		constexpr id2hr_t FujiDriveModes[] = {
			{0, "Single Frame"},
			{1, "Continuous Low"},
			{2, "Continuous High"},
		};

		constexpr id2hr_t CanonRecordModes[] = {
			{LIBRAW_Canon_RecordMode_JPEG, "JPEG"},
			{LIBRAW_Canon_RecordMode_CRW_THM, "CRW+THM"},
			{LIBRAW_Canon_RecordMode_AVI_THM, "AVI+THM"},
			{LIBRAW_Canon_RecordMode_TIF, "TIF"},
			{LIBRAW_Canon_RecordMode_TIF_JPEG, "TIF+JPEG"},
			{LIBRAW_Canon_RecordMode_CR2, "CR2"},
			{LIBRAW_Canon_RecordMode_CR2_JPEG, "CR2+JPEG"},
			{LIBRAW_Canon_RecordMode_UNKNOWN, "Unknown"},
			{LIBRAW_Canon_RecordMode_MOV, "MOV"},
			{LIBRAW_Canon_RecordMode_MP4, "MP4"},
			{LIBRAW_Canon_RecordMode_CRM, "CRM"},
			{LIBRAW_Canon_RecordMode_CR3, "CR3"},
			{LIBRAW_Canon_RecordMode_CR3_JPEG, "CR3+JPEG"},
			{LIBRAW_Canon_RecordMode_HEIF, "HEIF"},
			{LIBRAW_Canon_RecordMode_CR3_HEIF, "CR3+HEIF"},
		};

		constexpr struct
		{
			const int NumId;
			std::string_view StrId;
		} ColorSpaceToStr[] = {
			{LIBRAW_COLORSPACE_NotFound, "Not Found"},
			{LIBRAW_COLORSPACE_sRGB, "sRGB"},
			{LIBRAW_COLORSPACE_AdobeRGB, "Adobe RGB"},
			{LIBRAW_COLORSPACE_WideGamutRGB, "Wide Gamut RGB"},
			{LIBRAW_COLORSPACE_ProPhotoRGB, "ProPhoto RGB"},
			{LIBRAW_COLORSPACE_ICC, "ICC profile (embedded)"},
			{LIBRAW_COLORSPACE_Uncalibrated, "Uncalibrated"},
			{LIBRAW_COLORSPACE_CameraLinearUniWB, "Camera Linear, no WB"},
			{LIBRAW_COLORSPACE_CameraLinear, "Camera Linear"},
			{LIBRAW_COLORSPACE_CameraGammaUniWB, "Camera non-Linear, no WB"},
			{LIBRAW_COLORSPACE_CameraGamma, "Camera non-Linear"},
			{LIBRAW_COLORSPACE_MonochromeLinear, "Monochrome Linear"},
			{LIBRAW_COLORSPACE_MonochromeGamma, "Monochrome non-Linear"},
			{LIBRAW_COLORSPACE_Unknown, "Unknown"},
		};

		constexpr struct
		{
			const int NumId;
			const int LibRawId;
			std::string_view StrId;
		} Fujifilm_WhiteBalance2Str[] = {
			{0x000, LIBRAW_WBI_Auto, "Auto"},
			{0x100, LIBRAW_WBI_Daylight, "Daylight"},
			{0x200, LIBRAW_WBI_Cloudy, "Cloudy"},
			{0x300, LIBRAW_WBI_FL_D, "Daylight Fluorescent"},
			{0x301, LIBRAW_WBI_FL_N, "Day White Fluorescent"},
			{0x302, LIBRAW_WBI_FL_W, "White Fluorescent"},
			{0x303, LIBRAW_WBI_FL_WW, "Warm White Fluorescent"},
			{0x304, LIBRAW_WBI_FL_L, "Living Room Warm White Fluorescent"},
			{0x400, LIBRAW_WBI_Tungsten, "Incandescent"},
			{0x500, LIBRAW_WBI_Flash, "Flash"},
			{0x600, LIBRAW_WBI_Underwater, "Underwater"},
			{0xf00, LIBRAW_WBI_Custom, "Custom"},
			{0xf01, LIBRAW_WBI_Custom2, "Custom2"},
			{0xf02, LIBRAW_WBI_Custom3, "Custom3"},
			{0xf03, LIBRAW_WBI_Custom4, "Custom4"},
			{0xf04, LIBRAW_WBI_Custom5, "Custom5"},
			{0xff0, LIBRAW_WBI_Kelvin, "Kelvin"},
		};

		constexpr struct
		{
			const int NumId;
			std::string_view StrId;
		} Fujifilm_FilmModeToStr[] = {
			{0x000, "F0/Standard (Provia)"},
			{0x100, "F1/Studio Portrait"},
			{0x110, "F1a/Studio Portrait Enhanced Saturation"},
			{0x120, "F1b/Studio Portrait Smooth Skin Tone (Astia)"},
			{0x130, "F1c/Studio Portrait Increased Sharpness"},
			{0x200, "F2/Fujichrome (Velvia)"},
			{0x300, "F3/Studio Portrait Ex"},
			{0x400, "F4/Velvia"},
			{0x500, "Pro Neg. Std"},
			{0x501, "Pro Neg. Hi"},
			{0x600, "Classic Chrome"},
			{0x700, "Eterna"},
			{0x800, "Classic Negative"},
		};

		constexpr struct
		{
			const int NumId;
			std::string_view StrId;
		} Fujifilm_DynamicRangeSettingToStr[] = {
			{0x0000, "Auto (100-400%)"},
			{0x0001, "Manual"},
			{0x0100, "Standard (100%)"},
			{0x0200, "Wide1 (230%)"},
			{0x0201, "Wide2 (400%)"},
			{0x8000, "Film Simulation"},
		};

		template <typename T, std::size_t N>
		std::string_view find_name_by_id(const T (&table)[N], const int id)
		{
			for (const auto& entry : table)
				if (entry.NumId == id)
					return entry.StrId;
			return {};
		}

		const id2hr_t* lookup_id2hr(const std::uint64_t id, const id2hr_t* table, const std::size_t entries)
		{
			for (std::size_t k = 0; k < entries; k++)
				if (id == table[k].id)
					return &table[k];
			return nullptr;
		}

		bool is_empty(const char* const sz)
		{
			return sz == nullptr || sz[0] == 0;
		}

		std::string safe_string(const char* const sz)
		{
			return is_empty(sz) ? std::string{} : sz;
		}

		// Whitespace and NUL off both ends, as Diffractor trimmed.
		std::string_view trim(const std::string_view s)
		{
			constexpr std::string_view whitespace{" \n\r\t\f\v\0", 7};
			const auto start = s.find_first_not_of(whitespace);
			if (start == std::string_view::npos) return {};
			return s.substr(start, s.find_last_not_of(whitespace) - start + 1);
		}

		int strnicmp_ascii(const char* const a, const char* const b, const std::size_t count)
		{
			for (std::size_t i = 0; i < count; ++i)
			{
				const auto ca = static_cast<unsigned char>(a[i] >= 'A' && a[i] <= 'Z' ? a[i] - 'A' + 'a' : a[i]);
				const auto cb = static_cast<unsigned char>(b[i] >= 'A' && b[i] <= 'Z' ? b[i] - 'A' + 'a' : b[i]);
				if (ca != cb) return ca < cb ? -1 : 1;
				if (ca == 0) return 0;
			}

			return 0;
		}

		// A number to the given decimal places, as Diffractor's str::to_string spelled one.
		std::string fixed(const double v, const int digits)
		{
			return std::format("{:.{}f}", v, digits);
		}

		void add_metadata(std::vector<structure_row>& kv, const std::string_view name, const std::string_view val)
		{
			if (!name.empty() && !val.empty())
			{
				auto& row = kv.emplace_back();
				row.key = name;
				row.value = val;
				row.depth = 1;
			}
		}

		// The value alone, quoted as Diffractor's str::join quoted one that holds a colon, a space, a tab
		// or a quote. Diffractor joined the two values with the second as the separator, so the second,
		// which names the first, was never shown; the listing keeps that.
		void add_metadata(std::vector<structure_row>& kv, const std::string_view name, const std::string_view val1,
		                  const std::string_view)
		{
			std::string val;

			if (!val1.empty())
			{
				if (val1.find_first_of(": \t\'\"") == std::string_view::npos) val = val1;
				else if (val1.find('"') == std::string_view::npos) val = std::format("\"{}\"", val1);
				else if (val1.find('\'') == std::string_view::npos) val = std::format("'{}'", val1);
				else val = val1;
			}

			add_metadata(kv, name, val);
		}

		// LibRaw reports one long flat list, so it is grouped by subject here to give the block the same
		// shape the other metadata standards have.
		void add_section(std::vector<structure_row>& kv, const std::string_view name)
		{
			auto& row = kv.emplace_back();
			row.key = name;
			row.container = true;
			row.id = std::format("raw.{}", name);
		}

		// Most sections are conditional on the camera and format, so the ones that gathered nothing are
		// removed rather than shown as empty headings. The survivors report how much they hold.
		void drop_empty_sections(std::vector<structure_row>& kv)
		{
			std::vector<structure_row> kept;
			kept.reserve(kv.size());

			for (std::size_t i = 0; i < kv.size(); ++i)
			{
				if (kv[i].container)
				{
					std::size_t count = 0;
					while (i + 1 + count < kv.size() && !kv[i + 1 + count].container) ++count;
					if (count == 0) continue;

					kv[i].key = std::format("{} ({})", kv[i].key, count);
				}

				kept.emplace_back(std::move(kv[i]));
			}

			kv = std::move(kept);
		}

		// A table's name for an id, or the id itself.
		template <std::size_t N>
		void add_id2hr_metadata(std::vector<structure_row>& kv, const std::string_view name, const std::uint64_t id,
		                        const id2hr_t (&table)[N])
		{
			if (const auto* entry = lookup_id2hr(id, table, N))
				add_metadata(kv, name, entry->name);
			else
				add_metadata(kv, name, std::to_string(id));
		}

		// The colour filter array LibRaw inferred, spelled from the 2x2 repeat of the filters mask.
		std::string cfa_pattern(const libraw_iparams_t& P1)
		{
			if (P1.filters == 0) return {};
			if (P1.filters == 9) return "X-Trans (6x6)";
			if (P1.filters == 1) return "monochrome or variable";

			std::string result;

			for (auto row = 0; row < 2; ++row)
			{
				for (auto col = 0; col < 2; ++col)
				{
					result += P1.cdesc[(P1.filters >> (((row << 1 & 14) + (col & 1)) << 1)) & 3];
				}
			}

			return result;
		}
	}

	double raw_gps_degrees(const float (&dms)[3])
	{
		return dms[0] + dms[1] / 60.0 + dms[2] / 3600.0;
	}

	bool raw_gps_fix(const bool parsed, const double latitude, const double longitude)
	{
		if (!parsed) return false;
		return std::fabs(latitude) <= 90 && std::fabs(longitude) < 180 && !(latitude == 0.0 && longitude == 0.0);
	}

	bool raw_gps_fix(const libraw_gps_info_t& gps)
	{
		return raw_gps_fix(gps.gpsparsed, raw_gps_degrees(gps.latitude), raw_gps_degrees(gps.longitude));
	}

	std::optional<gps_position> raw_gps_position(const libraw_gps_info_t& gps)
	{
		if (!raw_gps_fix(gps)) return std::nullopt;

		const auto latitude = raw_gps_degrees(gps.latitude);
		const auto longitude = raw_gps_degrees(gps.longitude);
		return gps_position{gps.latref == 'S' ? -latitude : latitude, gps.longref == 'W' ? -longitude : longitude};
	}

	std::vector<structure_row> list_raw(const libraw_data_t& data)
	{
		const auto& P1 = data.idata;
		const auto& P2 = data.other;
		const auto& P3 = data.makernotes.common;

		const auto& mnLens = data.lens.makernotes;
		const auto& exifLens = data.lens;
		const auto& ShootingInfo = data.shootinginfo;

		const auto& S = data.sizes;
		const auto& C = data.color;
		const auto& T = data.thumbnail;

		const auto& Canon = data.makernotes.canon;
		const auto& Hasselblad = data.makernotes.hasselblad;
		const auto& Fuji = data.makernotes.fuji;
		const auto& Nikon = data.makernotes.nikon;
		const auto& Sony = data.makernotes.sony;

		const auto CamMakerName = safe_string(LibRaw::cameramakeridx2maker(P1.maker_index));
		const auto ColorSpaceName = find_name_by_id(ColorSpaceToStr, P3.ColorSpace);

		std::vector<structure_row> kv;

		add_section(kv, "Identification");

		if (!is_empty(C.OriginalRawFileName))
			add_metadata(kv, "OriginalRawFileName", C.OriginalRawFileName);

		add_metadata(kv, "Timestamp", std::to_string(P2.timestamp));
		add_metadata(kv, "Camera",
		             std::format("{} {} ID: 0x{:x}", std::string_view(P1.make), std::string_view(P1.model), mnLens.CamID));
		add_metadata(kv, "Normalized Make/Model",
		             std::format("{}/{}", std::string_view(P1.normalized_make), std::string_view(P1.normalized_model)));
		add_metadata(kv, "CamMaker ID", std::to_string(P1.maker_index));

		if (!CamMakerName.empty())
			add_metadata(kv, "CameraMaker", CamMakerName);

		if (!is_empty(C.UniqueCameraModel))
		{
			add_metadata(kv, "UniqueCameraModel", C.UniqueCameraModel);
		}
		if (!is_empty(C.LocalizedCameraModel))
		{
			add_metadata(kv, "LocalizedCameraModel", C.LocalizedCameraModel);
		}
		if (!is_empty(C.ImageUniqueID))
		{
			add_metadata(kv, "ImageUniqueID", C.ImageUniqueID);
		}
		if (!is_empty(C.RawDataUniqueID))
		{
			add_metadata(kv, "RawDataUniqueID", C.RawDataUniqueID);
		}

		if (!is_empty(ShootingInfo.BodySerial) && std::strcmp(ShootingInfo.BodySerial, "0"))
		{
			add_metadata(kv, "Body#", trim(ShootingInfo.BodySerial));
		}
		else if (C.model2[0] && !strnicmp_ascii(P1.normalized_make, "Kodak", 5))
		{
			add_metadata(kv, "Body#", trim(C.model2));
		}
		if (!is_empty(ShootingInfo.InternalBodySerial))
		{
			add_metadata(kv, "BodyAssy#", trim(ShootingInfo.InternalBodySerial));
		}
		if (!is_empty(exifLens.LensSerial))
		{
			add_metadata(kv, "Lens#", trim(exifLens.LensSerial));
		}
		if (!is_empty(exifLens.InternalLensSerial))
		{
			add_metadata(kv, "LensAssy#", trim(exifLens.InternalLensSerial));
		}
		if (!is_empty(P2.artist))
		{
			add_metadata(kv, "Owner", trim(P2.artist));
		}
		if (!is_empty(P1.software))
		{
			add_metadata(kv, "Software", trim(P1.software));
		}
		if (!is_empty(P3.firmware))
		{
			add_metadata(kv, "Firmware", trim(P3.firmware));
		}
		if (P2.shot_order)
		{
			add_metadata(kv, "Shot order", std::to_string(P2.shot_order));
		}
		if (P1.is_foveon)
		{
			add_metadata(kv, "Foveon sensor", "yes");
		}

		if (P1.dng_version)
		{
			std::string s;
			for (int i = 24; i >= 0; i -= 8)
				s += std::format("{}{}", P1.dng_version >> i & 255, i ? '.' : ' ');

			add_metadata(kv, "DNG Version", s);
		}


		add_section(kv, "Lens (Exif)");
		add_metadata(kv, "MinFocal", std::format("{:0.1} mm", exifLens.MinFocal));
		add_metadata(kv, "MaxFocal", std::format("{:0.1} mm", exifLens.MaxFocal));
		add_metadata(kv, "MaxAp @MinFocal", std::format("f/{:0.1}", exifLens.MaxAp4MinFocal));
		add_metadata(kv, "MaxAp @MaxFocal", std::format("f/{:0.1}", exifLens.MaxAp4MaxFocal));
		add_metadata(kv, "CurFocal", std::format("{:0.1} mm", P2.focal_len));
		add_metadata(kv, "MaxAperture @CurFocal", std::format("f/{:0.1}", exifLens.EXIF_MaxAp));
		add_metadata(kv, "FocalLengthIn35mmFormat", std::format("{} mm", exifLens.FocalLengthIn35mmFormat));
		add_metadata(kv, "LensMake", exifLens.LensMake);
		add_metadata(kv, "Lens", exifLens.Lens);


		add_section(kv, "Shooting");
		add_metadata(kv, "DriveMode", std::to_string(ShootingInfo.DriveMode));
		add_metadata(kv, "FocusMode", std::to_string(ShootingInfo.FocusMode));
		add_metadata(kv, "MeteringMode", std::to_string(ShootingInfo.MeteringMode));
		add_metadata(kv, "AFPoint", std::to_string(ShootingInfo.AFPoint));
		add_metadata(kv, "ExposureMode", std::to_string(ShootingInfo.ExposureMode));
		add_metadata(kv, "ExposureProgram", std::to_string(ShootingInfo.ExposureProgram));
		add_metadata(kv, "ImageStabilization", std::to_string(ShootingInfo.ImageStabilization));

		add_section(kv, "Lens (makernotes)");

		if (!is_empty(mnLens.body))
		{
			add_metadata(kv, "Host Body", mnLens.body);
		}

		if (!is_empty(Hasselblad.CaptureSequenceInitiator))
		{
			add_metadata(kv, "Initiator", Hasselblad.CaptureSequenceInitiator);
		}
		if (!is_empty(Hasselblad.SensorUnitConnector))
		{
			add_metadata(kv, "SU Connector", Hasselblad.SensorUnitConnector);
		}

		add_id2hr_metadata(kv, "CameraFormat", mnLens.CameraFormat, FormatNames);


		if (!strnicmp_ascii(P1.make, "Nikon", 5) && Nikon.SensorHighSpeedCrop.cwidth)
		{
			if (const auto* Crop = lookup_id2hr(Nikon.HighSpeedCropFormat, NikonCrops, std::size(NikonCrops)))
				add_metadata(kv, "Nikon crop", std::format("{}: {}", Nikon.HighSpeedCropFormat, Crop->name));
			else
				add_metadata(kv, "Nikon crop", std::to_string(Nikon.HighSpeedCropFormat));

			add_metadata(kv, "Sensor used area", std::format(
				             "{} x {}; crop from: {} x {} at top left pixel: ({}, {})",
				             Nikon.SensorWidth, Nikon.SensorHeight,
				             Nikon.SensorHighSpeedCrop.cwidth,
				             Nikon.SensorHighSpeedCrop.cheight,
				             Nikon.SensorHighSpeedCrop.cleft,
				             Nikon.SensorHighSpeedCrop.ctop));
		}

		add_id2hr_metadata(kv, "CameraMount", mnLens.CameraMount, MountNames);

		if (mnLens.LensID != 0xffffffff)
			add_metadata(kv, "LensID", std::format("{} 0x{:x}", mnLens.LensID, mnLens.LensID));

		if (!is_empty(mnLens.Lens))
		{
			add_metadata(kv, "Lens", mnLens.Lens);
		}

		add_id2hr_metadata(kv, "LensFormat", mnLens.LensFormat, FormatNames);

		add_id2hr_metadata(kv, "LensMount", mnLens.LensMount, MountNames);

		switch (mnLens.FocalType)
		{
		case LIBRAW_FT_UNDEFINED:
			add_metadata(kv, "FocalType", "Undefined");
			break;
		case LIBRAW_FT_PRIME_LENS:
			add_metadata(kv, "FocalType", "Prime lens");
			break;
		case LIBRAW_FT_ZOOM_LENS:
			add_metadata(kv, "FocalType", "Zoom lens");
			break;
		default:
			add_metadata(kv, "FocalType", std::to_string(mnLens.FocalType));
			break;
		}

		add_metadata(kv, "LensFeatures_pre", mnLens.LensFeatures_pre);
		add_metadata(kv, "LensFeatures_suf", mnLens.LensFeatures_suf);
		add_metadata(kv, "MinFocal", std::format("{:0.1} mm", mnLens.MinFocal));
		add_metadata(kv, "MaxFocal", std::format("{:0.1} mm", mnLens.MaxFocal));
		add_metadata(kv, "MaxAp @MinFocal", std::format("f/{:0.1}", mnLens.MaxAp4MinFocal));
		add_metadata(kv, "MaxAp @MaxFocal", std::format("f/{:0.1}", mnLens.MaxAp4MaxFocal));
		add_metadata(kv, "MinAp @MinFocal", std::format("f/{:0.1}", mnLens.MinAp4MinFocal));
		add_metadata(kv, "MinAp @MaxFocal", std::format("f/{:0.1}", mnLens.MinAp4MaxFocal));
		add_metadata(kv, "MaxAp", std::format("f/{:0.1}", mnLens.MaxAp));
		add_metadata(kv, "MinAp", std::format("f/{:0.1}", mnLens.MinAp));
		add_metadata(kv, "CurFocal", std::format("{:0.1} mm", mnLens.CurFocal));
		add_metadata(kv, "CurAp", std::format("f/{:0.1}", mnLens.CurAp));
		add_metadata(kv, "MaxAp @CurFocal", std::format("f/{:0.1}", mnLens.MaxAp4CurFocal));
		add_metadata(kv, "MinAp @CurFocal", std::format("f/{:0.1}", mnLens.MinAp4CurFocal));

		if (exifLens.makernotes.FocalLengthIn35mmFormat > 1.0f)
			add_metadata(kv, "FocalLengthIn35mmFormat",
			             std::format("{:0.1} mm", exifLens.makernotes.FocalLengthIn35mmFormat));

		if (exifLens.nikon.EffectiveMaxAp > 0.1f)
			add_metadata(kv, "EffectiveMaxAp", std::format("f/{:0.1}", exifLens.nikon.EffectiveMaxAp));

		if (exifLens.makernotes.LensFStops > 0.1f)
			add_metadata(kv, "LensFStops @CurFocal", std::format("{:0.2}", exifLens.makernotes.LensFStops));

		add_metadata(kv, "TeleconverterID", std::to_string(mnLens.TeleconverterID));
		add_metadata(kv, "Teleconverter", mnLens.Teleconverter);
		add_metadata(kv, "AdapterID", std::to_string(mnLens.AdapterID));
		add_metadata(kv, "Adapter", mnLens.Adapter);
		add_metadata(kv, "AttachmentID", std::to_string(mnLens.AttachmentID));
		add_metadata(kv, "Attachment", mnLens.Attachment);

		if (mnLens.MinFocusDistance > 0.f)
			add_metadata(kv, "MinFocusDistance", std::format("{:0.2} m", mnLens.MinFocusDistance));
		if (mnLens.FocusRangeIndex > 0.f)
			add_metadata(kv, "FocusRangeIndex", std::format("{:0.2}", mnLens.FocusRangeIndex));
		if (mnLens.FocalUnits)
			add_metadata(kv, "FocalUnits", std::format("{} per mm", mnLens.FocalUnits));

		add_section(kv, "Exposure");
		add_metadata(kv, "ISO speed", std::to_string(static_cast<int>(P2.iso_speed)));
		if (P3.real_ISO > 0.1f)
			add_metadata(kv, "real ISO speed", std::to_string(static_cast<int>(P3.real_ISO)));

		if (P2.shutter > 0 && P2.shutter < 1)
			add_metadata(kv, "Shutter", std::format("1/{:0.1}", 1.0f / P2.shutter));
		else if (P2.shutter >= 1)
			add_metadata(kv, "Shutter", std::format("{:0.1} sec", P2.shutter));
		else /* negative*/
			add_metadata(kv, "Shutter", "negative value");

		add_metadata(kv, "Aperture", std::format("f/{:0.1}", P2.aperture));
		add_metadata(kv, "Focal length", std::format("{:0.1} mm", P2.focal_len));

		if (P3.exifExposureIndex > 0.f)
			add_metadata(kv, "Exposure index", std::format("{:0.1}", P3.exifExposureIndex));
		if (P3.ExposureCalibrationShift != 0.f)
			add_metadata(kv, "Exposure calibration shift", std::format("{:0.3} EV", P3.ExposureCalibrationShift));
		if (P3.FlashEC != 0.f)
			add_metadata(kv, "Flash exposure compensation", std::format("{:0.2} EV", P3.FlashEC));
		if (C.flash_used != 0.f)
			add_metadata(kv, "Flash used", std::format("{:0.3}", C.flash_used));
		if (C.canon_ev != 0.f)
			add_metadata(kv, "Canon EV", std::format("{:0.3}", C.canon_ev));
		if (P3.afcount > 0)
			add_metadata(kv, "Autofocus records", std::to_string(P3.afcount));

		add_section(kv, "Temperature");

		if (P3.exifAmbientTemperature > -273.15f)
			add_metadata(kv, "Ambient temperature (exif data)", std::format("{:.2f}°C", P3.exifAmbientTemperature));
		if (P3.CameraTemperature > -273.15f)
			add_metadata(kv, "Camera temperature", std::format("{:.2f}°C", P3.CameraTemperature));
		if (P3.SensorTemperature > -273.15f)
			add_metadata(kv, "Sensor temperature", std::format("{:.2f}°C", P3.SensorTemperature));
		if (P3.SensorTemperature2 > -273.15f)
			add_metadata(kv, "Sensor temperature2", std::format("{:.2f}°C", P3.SensorTemperature2));
		if (P3.LensTemperature > -273.15f)
			add_metadata(kv, "Lens temperature", std::format("{:.2f}°C", P3.LensTemperature));
		if (P3.AmbientTemperature > -273.15f)
			add_metadata(kv, "Ambient temperature", std::format("{:.2f}°C", P3.AmbientTemperature));
		if (P3.BatteryTemperature > -273.15f)
			add_metadata(kv, "Battery temperature", std::format("{:.2f}°C", P3.BatteryTemperature));
		if (P3.FlashGN > 1.0f)
			add_metadata(kv, "Flash Guide Number", std::format("{:.2f}", P3.FlashGN));

		add_section(kv, "Environment");

		if (P3.exifHumidity > 0.f)
			add_metadata(kv, "Humidity", std::format("{:.1f} %", P3.exifHumidity));
		if (P3.exifPressure > 0.f)
			add_metadata(kv, "Pressure", std::format("{:.1f} hPa", P3.exifPressure));
		if (P3.exifWaterDepth != 0.f)
			add_metadata(kv, "Water depth", std::format("{:.2f} m", P3.exifWaterDepth));
		if (P3.exifAcceleration != 0.f)
			add_metadata(kv, "Acceleration", std::format("{:.3f}", P3.exifAcceleration));
		if (P3.exifCameraElevationAngle != 0.f)
			add_metadata(kv, "Camera elevation angle", std::format("{:.2f}°", P3.exifCameraElevationAngle));

		add_section(kv, "GPS");

		if (raw_gps_fix(P2.parsed_gps))
		{
			const auto& G = P2.parsed_gps;
			add_metadata(kv, "Latitude", std::format("{:.0f}° {:.0f}' {:.2f}\" {}", G.latitude[0], G.latitude[1],
			                                         G.latitude[2], G.latref ? G.latref : '?'));
			add_metadata(kv, "Longitude", std::format("{:.0f}° {:.0f}' {:.2f}\" {}", G.longitude[0], G.longitude[1],
			                                          G.longitude[2], G.longref ? G.longref : '?'));
			add_metadata(kv, "Altitude", std::format("{:.2f} m{}", G.altitude, G.altref ? " (below sea level)" : ""));
			add_metadata(kv, "GPS timestamp", std::format("{:02.0f}:{:02.0f}:{:05.2f} UTC", G.gpstimestamp[0],
			                                              G.gpstimestamp[1], G.gpstimestamp[2]));

			// 'A' is an active fix; 'V' means the receiver reported the position as unreliable.
			if (G.gpsstatus)
				add_metadata(kv, "Fix status", G.gpsstatus == 'A' ? "A (active)" : std::format("{}", G.gpsstatus));
		}

		add_section(kv, "Color profile");

		if (C.profile)
			add_metadata(kv, "Embedded ICC profile", std::format("yes, {} bytes", C.profile_length));
		else
			add_metadata(kv, "Embedded ICC profile", "no");

		if (C.dng_levels.baseline_exposure > -999.f)
			add_metadata(kv, "Baseline exposure", std::format("{:.3f}", C.dng_levels.baseline_exposure));

		add_section(kv, "Raw image");
		add_metadata(kv, "Number of raw images", std::to_string(P1.raw_count));

		add_section(kv, "Fujifilm");

		if (Fuji.DriveMode != -1)
			add_id2hr_metadata(kv, "Fuji Drive Mode", Fuji.DriveMode, FujiDriveModes);

		if (Fuji.CropMode)
			add_id2hr_metadata(kv, "Fuji Crop Mode", Fuji.CropMode, FujiCrops);

		if (Fuji.WB_Preset != 0xffff)
			add_metadata(kv, "Fuji WB preset", std::format("0x{:03x}", Fuji.WB_Preset),
			             find_name_by_id(Fujifilm_WhiteBalance2Str, Fuji.WB_Preset));
		if (Fuji.ExpoMidPointShift > -999.f) // tag 0x9650
			add_metadata(kv, "Fuji Exposure shift", std::format("{:4.3}", Fuji.ExpoMidPointShift));
		if (Fuji.DynamicRange != 0xffff)
			add_metadata(kv, "Fuji Dynamic Range (0x1400)", std::format("{}", Fuji.DynamicRange),
			             Fuji.DynamicRange == 1 ? "Standard" : "Wide");
		if (Fuji.FilmMode != 0xffff)
			add_metadata(kv, "Fuji Film Mode (0x1401)", std::format("0x{:03x}", Fuji.FilmMode),
			             find_name_by_id(Fujifilm_FilmModeToStr, Fuji.FilmMode));
		if (Fuji.DynamicRangeSetting != 0xffff)
			add_metadata(kv, "Fuji Dynamic Range Setting (0x1402)",
			             std::format("0x{:04x}", Fuji.DynamicRangeSetting),
			             find_name_by_id(Fujifilm_DynamicRangeSettingToStr, Fuji.DynamicRangeSetting));
		if (Fuji.DevelopmentDynamicRange != 0xffff)
			add_metadata(kv, "Fuji Development Dynamic Range (0x1403)", std::to_string(Fuji.DevelopmentDynamicRange));
		if (Fuji.AutoDynamicRange != 0xffff)
			add_metadata(kv, "Fuji Auto Dynamic Range (0x140b)", std::to_string(Fuji.AutoDynamicRange));
		if (Fuji.DRangePriority != 0xffff)
			add_metadata(kv, "Fuji Dynamic Range priority (0x1443)", std::format("{}", Fuji.DRangePriority),
			             Fuji.DRangePriority ? "Fixed" : "Auto");
		if (Fuji.DRangePriorityAuto)
			add_metadata(kv, "Fuji Dynamic Range priority Auto (0x1444)",
			             std::format("{}", Fuji.DRangePriorityAuto),
			             Fuji.DRangePriorityAuto == 1 ? "Weak" : "Strong");
		if (Fuji.DRangePriorityFixed)
			add_metadata(kv, "Fuji Dynamic Range priority Fixed (0x1445)",
			             std::format("{}", Fuji.DRangePriorityFixed),
			             Fuji.DRangePriorityFixed == 1 ? "Weak" : "Strong");

		add_section(kv, "Sizes");

		if (S.pixel_aspect != 1)
			add_metadata(kv, "Pixel Aspect Ratio", std::format("{:0.6}", S.pixel_aspect));

		if (T.tlength)
			add_metadata(kv, "Thumb size", std::format("{:4} x {}", T.twidth, T.theight));

		add_metadata(kv, "Full size", std::format("{:4} x {}", S.raw_width, S.raw_height));

		if (S.raw_inset_crops[0].cwidth)
		{
			auto s = std::format("{:4} x {}", S.raw_inset_crops[0].cwidth, S.raw_inset_crops[0].cheight);

			if (S.raw_inset_crops[0].cleft != 0xffff)
				s += std::format(" left {}", S.raw_inset_crops[0].cleft);
			if (S.raw_inset_crops[0].ctop != 0xffff)
				s += std::format(" top {}", S.raw_inset_crops[0].ctop);

			add_metadata(kv, "Raw inset, width x height", s);
		}

		add_metadata(kv, "Image size", std::format("{:4} x {}", S.width, S.height));
		add_metadata(kv, "Output size", std::format("{:4} x {}", S.iwidth, S.iheight));
		add_metadata(kv, "Image flip", std::to_string(S.flip));

		if (S.top_margin || S.left_margin)
			add_metadata(kv, "Active area margin", std::format("left {}, top {}", S.left_margin, S.top_margin));
		if (S.raw_pitch)
			add_metadata(kv, "Raw row pitch", std::format("{} bytes", S.raw_pitch));
		if (S.raw_aspect)
			add_metadata(kv, "Raw aspect", std::to_string(S.raw_aspect));

		if (S.raw_inset_crops[1].cwidth)
		{
			add_metadata(kv, "Raw inset 2, width x height",
			             std::format("{:4} x {}", S.raw_inset_crops[1].cwidth, S.raw_inset_crops[1].cheight));
		}

		if (C.dng_levels.default_crop[2] || C.dng_levels.default_crop[3])
		{
			add_metadata(kv, "DNG default crop", std::format("{} x {} at ({}, {})", C.dng_levels.default_crop[2],
			                                                 C.dng_levels.default_crop[3], C.dng_levels.default_crop[0],
			                                                 C.dng_levels.default_crop[1]));
		}

		add_section(kv, "Thumbnails");

		if (T.tlength)
		{
			add_metadata(kv, "Preferred thumbnail", std::format("{} x {}, {} bytes, {} colors", T.twidth, T.theight,
			                                                    T.tlength, T.tcolors));
		}

		for (auto i = 0; i < data.thumbs_list.thumbcount && i < LIBRAW_THUMBNAIL_MAXCOUNT; ++i)
		{
			const auto& t = data.thumbs_list.thumblist[i];
			add_metadata(kv, std::format("Thumbnail {}", i + 1),
			             std::format("{} x {}, {} bytes at offset {}", t.twidth, t.theight, t.tlength, t.toffset));
		}

		add_section(kv, "Canon");

		if (Canon.RecordMode)
			add_id2hr_metadata(kv, "Canon record mode", Canon.RecordMode, CanonRecordModes);
		if (Canon.SensorWidth)
			add_metadata(kv, "SensorWidth", std::to_string(Canon.SensorWidth));
		if (Canon.SensorHeight)
			add_metadata(kv, "SensorHeight", std::to_string(Canon.SensorHeight));

		add_section(kv, "Hasselblad");

		if (Hasselblad.BaseISO)
			add_metadata(kv, "Hasselblad base ISO", std::to_string(Hasselblad.BaseISO));
		if (Hasselblad.Gain)
			add_metadata(kv, "Hasselblad gain", fixed(Hasselblad.Gain, 3));

		add_section(kv, "Raw color data");
		add_metadata(kv, "Raw colors", std::to_string(P1.colors));
		add_metadata(kv, "Color description", P1.cdesc);

		if (const auto cfa = cfa_pattern(P1); !cfa.empty())
		{
			add_metadata(kv, "CFA pattern", cfa);
		}

		if (C.raw_bps)
			add_metadata(kv, "Bits per raw pixel", std::to_string(C.raw_bps));
		if (C.maximum)
			add_metadata(kv, "White level", std::to_string(C.maximum));
		if (C.data_maximum)
			add_metadata(kv, "Highest sampled value", std::to_string(C.data_maximum));
		if (C.linear_max[0])
		{
			add_metadata(kv, "Linear maximum", std::format("{} {} {} {}", C.linear_max[0], C.linear_max[1],
			                                               C.linear_max[2], C.linear_max[3]));
		}

		if (C.cam_mul[0] > 0.f)
		{
			add_metadata(kv, "As shot white balance", std::format("{:0.4} {:0.4} {:0.4} {:0.4}", C.cam_mul[0],
			                                                      C.cam_mul[1], C.cam_mul[2], C.cam_mul[3]));
			add_metadata(kv, "White balance applied", C.as_shot_wb_applied ? "yes" : "no");
		}

		if (C.pre_mul[0] > 0.f)
		{
			add_metadata(kv, "Camera daylight multipliers", std::format("{:0.4} {:0.4} {:0.4} {:0.4}", C.pre_mul[0],
			                                                            C.pre_mul[1], C.pre_mul[2], C.pre_mul[3]));
		}

		if (C.dng_levels.asshotneutral[0] > 0.f)
		{
			add_metadata(kv, "DNG AsShotNeutral", std::format("{:0.4} {:0.4} {:0.4}", C.dng_levels.asshotneutral[0],
			                                                  C.dng_levels.asshotneutral[1],
			                                                  C.dng_levels.asshotneutral[2]));
		}

		if (C.dng_levels.LinearResponseLimit != 0.f && C.dng_levels.LinearResponseLimit != 1.f)
			add_metadata(kv, "Linear response limit", std::format("{:0.4}", C.dng_levels.LinearResponseLimit));

		if (C.ExifColorSpace != LIBRAW_COLORSPACE_Unknown)
			add_metadata(kv, "Exif color space", std::to_string(C.ExifColorSpace));

		if (Canon.ChannelBlackLevel[0])
		{
			add_metadata(kv, "Canon makernotes, ChannelBlackLevel", std::format("{} {} {} {}",
			                                                                    Canon.ChannelBlackLevel[0],
			                                                                    Canon.ChannelBlackLevel[1],
			                                                                    Canon.ChannelBlackLevel[2],
			                                                                    Canon.ChannelBlackLevel[3]));
		}

		if (C.black)
		{
			add_metadata(kv, "black", std::to_string(C.black));
		}

		add_metadata(kv, "Color space (makernotes)", std::format("{}, {}", P3.ColorSpace, ColorSpaceName));


		add_section(kv, "Sony");

		if (Sony.PixelShiftGroupID)
		{
			add_metadata(kv, "Sony PixelShiftGroupPrefix",
			             std::format("0x{:x} PixelShiftGroupID {}", Sony.PixelShiftGroupPrefix,
			                         Sony.PixelShiftGroupID));

			if (Sony.numInPixelShiftGroup)
			{
				add_metadata(kv, "shot#", std::format("{} (starts at 1) of total {}", Sony.numInPixelShiftGroup,
				                                      Sony.nShotsInPixelShiftGroup));
			}
			else
			{
				add_metadata(kv, "shots in PixelShiftGroup",
				             std::format("{}, already ARQ", Sony.nShotsInPixelShiftGroup));
			}
		}

		if (Sony.Sony0x9400_version)
		{
			add_metadata(kv, "SONY Sequence data", std::format("tag 0x9400 version '{:x}' ReleaseMode2: {}",
			                                                   Sony.Sony0x9400_version, Sony.Sony0x9400_ReleaseMode2));
			add_metadata(kv, "SequenceImageNumber",
			             std::format("{} (starts at zero)", Sony.Sony0x9400_SequenceImageNumber));
			add_metadata(kv, "SequenceLength1", std::format("{} shot(s)", Sony.Sony0x9400_SequenceLength1));
			add_metadata(kv, "SequenceFileNumber",
			             std::format("{} (starts at zero, exiftool starts at 1)", Sony.Sony0x9400_SequenceFileNumber));
			add_metadata(kv, "SequenceLength2", std::format("{} file(s)", Sony.Sony0x9400_SequenceLength2));
		}

		drop_empty_sections(kv);

		return kv;
	}
}
