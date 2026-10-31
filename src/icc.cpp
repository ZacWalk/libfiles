// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Listing an ICC colour profile: its header and tag directory read with every bound checked,
// and the tags a person asks about - the name, the colorants, the tone curves, the white point -
// described. The listing is Diffractor's, moved here with the reader it was made by.

#include "libfiles/icc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace libfiles
{
	namespace
	{
		constexpr std::uint32_t type_xyz = 0x58595A20; // 'XYZ '
		constexpr std::uint32_t type_para = 0x70617261; // 'para'
		constexpr std::uint32_t type_desc = 0x64657363; // 'desc', textDescriptionType
		constexpr std::uint32_t type_mluc = 0x6D6C7563; // 'mluc', multiLocalizedUnicodeType
		constexpr std::uint32_t type_text = 0x74657874; // 'text'
		constexpr std::uint32_t type_curv = 0x63757276; // 'curv'
		constexpr std::uint32_t tag_desc = 0x64657363; // 'desc'
		constexpr std::uint32_t tag_cprt = 0x63707274; // 'cprt'
		constexpr std::uint32_t tag_dmnd = 0x646D6E64; // 'dmnd'
		constexpr std::uint32_t tag_dmdd = 0x646D6464; // 'dmdd'
		constexpr std::uint32_t tag_wtpt = 0x77747074; // 'wtpt'
		constexpr std::uint32_t tag_red_colorant = 0x7258595A; // 'rXYZ'
		constexpr std::uint32_t tag_green_colorant = 0x6758595A; // 'gXYZ'
		constexpr std::uint32_t tag_blue_colorant = 0x6258595A; // 'bXYZ'
		constexpr std::uint32_t tag_red_curve = 0x72545243; // 'rTRC'

		// The leading bytes of a tag an inspector shows.
		constexpr std::size_t detail_bytes = 4096;

		// The four characters of a signature, a zero byte among them, as printf's "%c" wrote them.
		std::string signature_text(const std::uint32_t u)
		{
			return {static_cast<char>(u >> 24 & 0xFF), static_cast<char>(u >> 16 & 0xFF),
			        static_cast<char>(u >> 8 & 0xFF), static_cast<char>(u & 0xFF)};
		}

		// Without the zero bytes at either end, which is all a signature or fixed-length text pads with.
		std::string_view strip_nuls(const std::string_view s)
		{
			const auto start = s.find_first_not_of('\0');
			if (start == std::string_view::npos) return {};
			const auto end = s.find_last_not_of('\0');
			return s.substr(start, end - start + 1);
		}

		// printf's fixed notation, which the listing has always written its numbers in.
		std::string fixed(const double value, const int places)
		{
			const auto length = std::snprintf(nullptr, 0, "%.*f", places, value);
			if (length <= 0) return {};
			std::string text(static_cast<std::size_t>(length) + 1, '\0');
			std::snprintf(text.data(), text.size(), "%.*f", places, value);
			text.resize(static_cast<std::size_t>(length));
			return text;
		}

		void append_utf8(std::string& out, const std::uint32_t cp)
		{
			if (cp < 0x80)
			{
				out.push_back(static_cast<char>(cp));
			}
			else if (cp < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | cp >> 6));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else if (cp < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | cp >> 12));
				out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | cp >> 18));
				out.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
		}

		// UTF-16 to UTF-8, a surrogate without its partner read as U+FFFD.
		std::string utf16_to_utf8(const std::u16string_view text)
		{
			std::string result;
			result.reserve(text.size() * 3);

			for (std::size_t i = 0; i < text.size();)
			{
				std::uint32_t cp = text[i++];

				if (cp >= 0xD800 && cp <= 0xDBFF)
				{
					if (i < text.size() && text[i] >= 0xDC00 && text[i] <= 0xDFFF)
						cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<std::uint32_t>(text[i++]) - 0xDC00);
					else
						cp = 0xFFFD;
				}
				else if (cp >= 0xDC00 && cp <= 0xDFFF)
				{
					cp = 0xFFFD;
				}

				append_utf8(result, cp);
			}

			return result;
		}

		struct date_time
		{
			std::uint16_t year = 0;
			std::uint16_t month = 0;
			std::uint16_t day = 0;
			std::uint16_t hour = 0;
			std::uint16_t minute = 0;
			std::uint16_t second = 0;
		};

		struct xyz
		{
			double x = 0.0;
			double y = 0.0;
			double z = 0.0;
		};

		struct tag
		{
			std::uint32_t type = 0;

			// Shared, because several tags may name one region of the profile - the spec allows it, and
			// the common sRGB profile points all three tone curves at a single curve. Each is a view of
			// that region, not a copy of it.
			std::shared_ptr<const std::vector<std::uint8_t>> payload;

			const std::vector<std::uint8_t>& data() const
			{
				static const std::vector<std::uint8_t> none;
				return payload ? *payload : none;
			}
		};

		struct profile
		{
			std::uint32_t size = 0;
			std::uint32_t cmm = 0;
			std::uint32_t version = 0;
			std::uint32_t device_class = 0;
			std::uint32_t colour_space = 0;
			std::uint32_t connection_space = 0;
			date_time created;
			std::uint32_t signature = 0;
			std::uint32_t platform = 0;
			std::uint32_t flags = 0;
			std::uint32_t manufacturer = 0;
			std::uint32_t model = 0;
			std::uint64_t attributes = 0;
			std::uint32_t intent = 0;
			xyz illuminant;
			std::uint32_t creator = 0;

			std::map<std::uint32_t, tag> tags;
			std::vector<std::pair<std::uint32_t, std::string>> unread;
			std::uint32_t declared_tags = 0;
		};

		// Big-endian values, each 0 when the profile ends before it, without moving past the end.
		class profile_stream
		{
		public:
			explicit profile_stream(const std::span<const std::uint8_t> data) : data_(data)
			{
			}

			bool eof() const { return index_ >= data_.size(); }
			std::size_t index() const { return index_; }

			std::size_t seek(const std::size_t index)
			{
				const auto current = index_;
				index_ = std::min(index, data_.size());
				return current;
			}

			std::uint8_t u8()
			{
				if (index_ >= data_.size()) return 0;
				return data_[index_++];
			}

			std::uint16_t u16()
			{
				if (index_ + 1 >= data_.size()) return 0;
				const std::uint32_t high = u8();
				const std::uint32_t low = u8();
				return static_cast<std::uint16_t>(high << 8 | low);
			}

			std::int16_t i16()
			{
				if (index_ + 1 >= data_.size()) return 0;
				const int high = static_cast<std::int8_t>(u8());
				const int low = u8();
				return static_cast<std::int16_t>(high * 256 | low);
			}

			std::uint32_t u32()
			{
				if (index_ + 3 >= data_.size()) return 0;
				const std::uint32_t high = u16();
				const std::uint32_t low = u16();
				return high << 16 | low;
			}

			std::int32_t i32()
			{
				if (index_ + 3 >= data_.size()) return 0;
				const auto high = static_cast<std::uint32_t>(static_cast<std::int32_t>(i16()));
				const std::uint32_t low = u16();
				return static_cast<std::int32_t>(high << 16 | low);
			}

			std::uint64_t u64()
			{
				if (index_ + 7 >= data_.size()) return 0;
				const std::uint64_t high = u32();
				const std::uint64_t low = u32();
				return high << 32 | low;
			}

			double s15_fixed16() { return static_cast<double>(i32()) / 0x10000; }

			std::vector<std::uint8_t> bytes(const std::size_t count)
			{
				if (count == 0 || index_ + count > data_.size()) return {};
				std::vector<std::uint8_t> result(data_.begin() + static_cast<std::ptrdiff_t>(index_),
				                                 data_.begin() + static_cast<std::ptrdiff_t>(index_ + count));
				index_ += count;
				return result;
			}

			date_time date()
			{
				date_time d;
				d.year = u16();
				d.month = u16();
				d.day = u16();
				d.hour = u16();
				d.minute = u16();
				d.second = u16();
				return d;
			}

			xyz colour()
			{
				xyz c;
				c.x = s15_fixed16();
				c.y = s15_fixed16();
				c.z = s15_fixed16();
				return c;
			}

		private:
			std::span<const std::uint8_t> data_;
			std::size_t index_ = 0;
		};

		// The profile name is the one thing a user wants from an ICC block, and it is buried in a tag
		// whose payload is either a Mac/ASCII pair or a table of UTF-16BE strings.
		std::string decode_text(const tag& t)
		{
			const auto& d = t.data();

			const auto be32 = [&d](const std::size_t i)
			{
				return static_cast<std::size_t>(d[i]) << 24 | static_cast<std::size_t>(d[i + 1]) << 16 |
					static_cast<std::size_t>(d[i + 2]) << 8 | d[i + 3];
			};

			if (t.type == type_desc)
			{
				// textDescriptionType: 4 byte reserved, ASCII count, then the ASCII string.
				if (d.size() < 8) return {};
				const auto count = be32(4);
				if (count == 0 || count > d.size() - 8) return {};
				const auto* const text = reinterpret_cast<const char*>(d.data() + 8);
				return std::string(text, std::find(text, text + count, '\0'));
			}

			if (t.type == type_mluc)
			{
				// multiLocalizedUnicodeType: 4 byte reserved, record count, record size, then records
				// of language, country, length and tag-relative offset.
				if (d.size() < 24) return {};
				const auto count = be32(4);
				const auto record_size = be32(8);
				constexpr std::size_t records_at = 12;
				constexpr std::size_t minimum_record_size = 12;
				if (count == 0 || record_size < minimum_record_size) return {};
				if (count > (d.size() - records_at) / record_size) return {};

				for (std::size_t record = 0; record < count; ++record)
				{
					const auto record_at = records_at + record * record_size;
					const auto length = be32(record_at + 4);
					auto offset = be32(record_at + 8);
					// Offsets are from the start of the tag, and the payload begins after the 4 byte signature.
					if (offset < 4) continue;
					offset -= 4;
					if (length < 2 || (length & 1u) != 0 || offset > d.size() || length > d.size() - offset) continue;

					std::u16string utf16;
					utf16.reserve(length / 2);

					for (std::size_t i = 0; i + 1 < length; i += 2)
					{
						utf16.push_back(static_cast<char16_t>(d[offset + i] << 8 | d[offset + i + 1]));
					}

					return utf16_to_utf8(utf16);
				}
			}

			if (t.type == type_text)
			{
				// textType: 4 byte reserved then a NUL terminated ASCII string.
				if (d.size() < 5) return {};
				const auto* const text = reinterpret_cast<const char*>(d.data() + 4);
				return std::string(text, std::find(text, text + (d.size() - 4), '\0'));
			}

			return {};
		}

		std::string_view intent_name(const std::uint32_t intent)
		{
			switch (intent)
			{
			case 0: return "Perceptual";
			case 1: return "Relative colorimetric";
			case 2: return "Saturation";
			case 3: return "Absolute colorimetric";
			default: return {};
			}
		}

		std::string_view class_name(const std::uint32_t v)
		{
			switch (v)
			{
			case 0x73636E72: return "Input device";
			case 0x6D6E7472: return "Display device";
			case 0x70727472: return "Output device";
			case 0x6C696E6B: return "Device link";
			case 0x73706163: return "Colour space conversion";
			case 0x61627374: return "Abstract";
			case 0x6E6D636C: return "Named colour";
			default: return {};
			}
		}

		std::string_view platform_name(const std::uint32_t v)
		{
			switch (v)
			{
			case 0x4150504C: return "Apple";
			case 0x4D534654: return "Microsoft";
			case 0x53474920: return "Silicon Graphics";
			case 0x53554E57: return "Sun Microsystems";
			case 0: return "Not identified";
			default: return {};
			}
		}

		std::string_view space_name(const std::uint32_t v)
		{
			switch (v)
			{
			case 0x58595A20: return "CIEXYZ";
			case 0x4C616220: return "CIELAB";
			case 0x52474220: return "RGB";
			case 0x47524159: return "Greyscale";
			case 0x434D594B: return "CMYK";
			case 0x434D5920: return "CMY";
			case 0x59436272: return "YCbCr";
			case 0x48535620: return "HSV";
			case 0x484C5320: return "HLS";
			default: return {};
			}
		}

		// Signatures are four printable characters, so the decoded meaning is shown with the code
		// itself rather than replacing it.
		std::string signature_named(const std::uint32_t v, const std::string_view name)
		{
			const auto code = std::string(strip_nuls(signature_text(v)));
			if (name.empty()) return code.empty() ? "(none)" : code;
			if (code.empty()) return std::string(name);
			return std::format("{} ({})", name, code);
		}

		double s15_fixed16(const std::vector<std::uint8_t>& d, const std::size_t offset)
		{
			if (offset + 4 > d.size()) return 0.0;
			const auto raw = static_cast<std::uint32_t>(d[offset]) << 24 | static_cast<std::uint32_t>(d[offset + 1]) << 16 |
				static_cast<std::uint32_t>(d[offset + 2]) << 8 | d[offset + 3];
			return static_cast<std::int32_t>(raw) / 65536.0;
		}

		bool tag_xyz(const profile& p, const std::uint32_t sig, xyz& out)
		{
			const auto found = p.tags.find(sig);
			if (found == p.tags.end() || found->second.type != type_xyz) return false;
			const auto& d = found->second.data();
			if (d.size() < 16) return false;
			out = {s15_fixed16(d, 4), s15_fixed16(d, 8), s15_fixed16(d, 12)};
			return true;
		}

		// The question an ICC block usually has to answer is how wide the gamut is, and that is only
		// legible once the colorant tags are compared against the sets people actually recognise.
		std::string primaries_name(const profile& p)
		{
			xyz r, g, b;
			if (!tag_xyz(p, tag_red_colorant, r) || !tag_xyz(p, tag_green_colorant, g) ||
				!tag_xyz(p, tag_blue_colorant, b))
				return {};

			struct known
			{
				std::string_view name;
				double v[9];
			};

			// D50 adapted colorants as written by the reference profiles for each space.
			static constexpr known table[] = {
				{"sRGB", {0.4360, 0.2225, 0.0139, 0.3851, 0.7169, 0.0971, 0.1431, 0.0606, 0.7141}},
				{"Adobe RGB (1998)", {0.6097, 0.3111, 0.0195, 0.2052, 0.6257, 0.0609, 0.1492, 0.0632, 0.7448}},
				{"Display P3", {0.5151, 0.2412, -0.0011, 0.2920, 0.6922, 0.0419, 0.1571, 0.0666, 0.7841}},
				{"Rec. 2020", {0.6734, 0.2790, -0.0019, 0.1656, 0.6757, 0.0299, 0.1250, 0.0453, 0.7973}},
				{"ProPhoto RGB", {0.7977, 0.2880, 0.0000, 0.1352, 0.7119, 0.0000, 0.0313, 0.0001, 0.8249}},
			};

			const double actual[9] = {r.x, r.y, r.z, g.x, g.y, g.z, b.x, b.y, b.z};

			for (const auto& k : table)
			{
				auto matches = true;
				for (auto i = 0; i < 9 && matches; ++i) matches = std::abs(actual[i] - k.v[i]) < 0.02;
				if (matches) return std::format("approximately {}", k.name);
			}

			return "custom";
		}

		std::string_view illuminant_name(const double x, const double y)
		{
			if (std::abs(x - 0.3457) < 0.005 && std::abs(y - 0.3585) < 0.005) return "D50";
			if (std::abs(x - 0.3127) < 0.005 && std::abs(y - 0.3290) < 0.005) return "D65";
			if (std::abs(x - 0.3324) < 0.005 && std::abs(y - 0.3474) < 0.005) return "D55";
			if (std::abs(x - 0.2831) < 0.005 && std::abs(y - 0.2971) < 0.005) return "D93";
			return {};
		}

		std::string format_xyz(const xyz& v)
		{
			return std::format("{}, {}, {}", fixed(v.x, 4), fixed(v.y, 4), fixed(v.z, 4));
		}

		std::string describe_chromaticity(const xyz& v)
		{
			const auto sum = v.x + v.y + v.z;
			if (sum <= 0.0) return format_xyz(v);

			const auto x = v.x / sum;
			const auto y = v.y / sum;
			const auto name = illuminant_name(x, y);
			const auto chroma = std::format("x {}, y {}", fixed(x, 4), fixed(y, 4));
			return name.empty() ? chroma : std::format("{} ({})", name, chroma);
		}

		// A tone curve is a table or a parametric function; either way the shape is what matters.
		std::string describe_curve(const tag& t)
		{
			const auto& d = t.data();

			if (t.type == type_curv)
			{
				if (d.size() < 8) return {};
				const auto count = static_cast<std::size_t>(d[4]) << 24 | static_cast<std::size_t>(d[5]) << 16 |
					static_cast<std::size_t>(d[6]) << 8 | d[7];
				if (count == 0) return "linear";
				if (count == 1 && d.size() >= 10)
				{
					const auto gamma = (static_cast<std::uint32_t>(d[8]) << 8 | d[9]) / 256.0;
					return "gamma " + fixed(gamma, 2);
				}
				return std::format("curve, {} points", count);
			}

			if (t.type == type_para)
			{
				if (d.size() < 6) return {};
				const auto function = static_cast<std::uint32_t>(d[4]) << 8 | d[5];
				if (function == 0 && d.size() >= 12) return "gamma " + fixed(s15_fixed16(d, 8), 2);
				if (function == 3) return "sRGB style curve";
				return std::format("parametric, type {}", function);
			}

			return {};
		}

		std::string describe_tag(const tag& t)
		{
			const auto decoded = decode_text(t);
			const auto text = strip_nuls(decoded);
			if (!text.empty()) return std::string(text);

			if (t.type == type_xyz && t.data().size() >= 16)
			{
				return format_xyz({s15_fixed16(t.data(), 4), s15_fixed16(t.data(), 8), s15_fixed16(t.data(), 12)});
			}

			const auto curve = describe_curve(t);
			if (!curve.empty()) return curve;

			return std::format("binary, {} bytes", t.data().size() + 4);
		}

		void add_row(std::vector<structure_row>& rows, std::string key, std::string value, const int depth)
		{
			auto& row = rows.emplace_back();
			row.key = std::move(key);
			row.value = std::move(value);
			row.depth = depth;
		}

		structure_row& add_container(std::vector<structure_row>& rows, std::string key, std::string id)
		{
			auto& row = rows.emplace_back();
			row.key = std::move(key);
			row.container = true;
			row.id = std::move(id);
			return row;
		}

		std::vector<structure_row> list(const profile& p)
		{
			std::vector<structure_row> rows;

			// The profile identity is derived from the tags below it, so it is labelled as a summary
			// rather than presented as more of the file's own content.
			add_container(rows, "Summary", "icc.summary");

			const std::pair<std::uint32_t, std::string_view> named[] = {
				{tag_desc, "Profile"},
				{tag_dmdd, "Device model"},
				{tag_dmnd, "Device manufacturer"},
				{tag_cprt, "Copyright"}
			};

			for (const auto& [sig, name] : named)
			{
				const auto found = p.tags.find(sig);

				if (found != p.tags.end())
				{
					const auto decoded = decode_text(found->second);
					const auto text = strip_nuls(decoded);
					if (!text.empty()) add_row(rows, std::string(name), std::string(text), 1);
				}
			}

			add_row(rows, "Colour space", signature_named(p.colour_space, space_name(p.colour_space)), 1);

			const auto primaries = primaries_name(p);
			if (!primaries.empty()) add_row(rows, "Primaries", primaries, 1);

			const auto curve = p.tags.find(tag_red_curve);
			if (curve != p.tags.end())
			{
				const auto described = describe_curve(curve->second);
				if (!described.empty()) add_row(rows, "Tone response", described, 1);
			}

			xyz white;
			if (tag_xyz(p, tag_wtpt, white)) add_row(rows, "White point", describe_chromaticity(white), 1);

			const auto intent = intent_name(p.intent);
			add_row(rows, "Rendering intent", intent.empty() ? std::format("{}", p.intent) : std::string(intent), 1);

			// The header is fixed content the file really carries, so it is listed whole.
			add_container(rows, "Header", "icc.header");

			add_row(rows, "Profile size", std::format("{} bytes", p.size), 1);
			add_row(rows, "Preferred CMM", signature_named(p.cmm, {}), 1);
			add_row(rows, "Version",
			        std::format("{}.{}.{}", p.version >> 24 & 0xFF, p.version >> 20 & 0x0F, p.version >> 16 & 0x0F), 1);
			add_row(rows, "Class", signature_named(p.device_class, class_name(p.device_class)), 1);
			add_row(rows, "Data colour space", signature_named(p.colour_space, space_name(p.colour_space)), 1);
			add_row(rows, "Connection space", signature_named(p.connection_space, space_name(p.connection_space)), 1);

			if (p.created.year != 0)
			{
				add_row(rows, "Created",
				        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", p.created.year, p.created.month, p.created.day,
				                    p.created.hour, p.created.minute, p.created.second), 1);
			}

			add_row(rows, "File signature", signature_text(p.signature), 1);
			add_row(rows, "Primary platform", signature_named(p.platform, platform_name(p.platform)), 1);
			add_row(rows, "Profile flags", std::format("0x{:08x}", p.flags), 1);
			add_row(rows, "Device manufacturer", signature_named(p.manufacturer, {}), 1);
			add_row(rows, "Device model", signature_named(p.model, {}), 1);
			add_row(rows, "Device attributes", std::format("0x{:016x}", p.attributes), 1);
			add_row(rows, "Rendering intent",
			        intent.empty() ? std::format("{}", p.intent) : std::format("{} ({})", intent, p.intent), 1);
			add_row(rows, "PCS illuminant", format_xyz(p.illuminant), 1);
			add_row(rows, "Creator", signature_named(p.creator, {}), 1);

			// The tag directory is the profile's actual contents, so every entry is listed with its type
			// and extent, and its bytes remain reachable.
			auto& tags_title = add_container(rows, std::format("Tags ({})", p.tags.size()), "icc.tags");
			tags_title.value = std::format("{} declared", p.declared_tags);

			for (const auto& [sig, t] : p.tags)
			{
				const auto sig_text = std::string(strip_nuls(signature_text(sig)));
				const auto type_code = std::string(strip_nuls(signature_text(t.type)));

				auto& row = rows.emplace_back();
				row.key = sig_text;
				row.value = describe_tag(t);
				row.depth = 1;
				row.shape = std::format("{}, {} bytes", type_code, t.data().size() + 4);
				row.id = std::format("icc.tag.{}", sig_text);
				const auto kept = std::min(t.data().size(), detail_bytes);
				row.detail = std::vector<std::uint8_t>(t.data().begin(), t.data().begin() + static_cast<std::ptrdiff_t>(kept));
			}

			if (!p.unread.empty())
			{
				add_container(rows, std::format("Unread tags ({})", p.unread.size()), "icc.unread");

				for (const auto& [sig, reason] : p.unread)
				{
					add_row(rows, "Unread tag", std::format("{}: {}", strip_nuls(signature_text(sig)), reason), 1);
				}
			}

			return rows;
		}

		bool load(profile& p, const std::span<const std::uint8_t> data)
		{
			profile_stream stream(data);

			if (data.size() < 128) return false;

			p.size = stream.u32();
			if (p.size > data.size() || p.size < 128) return false;

			p.cmm = stream.u32();
			p.version = stream.u32();
			p.device_class = stream.u32();
			p.colour_space = stream.u32();
			p.connection_space = stream.u32();
			p.created = stream.date();
			p.signature = stream.u32();
			p.platform = stream.u32();
			p.flags = stream.u32();
			p.manufacturer = stream.u32();
			p.model = stream.u32();
			p.attributes = stream.u64();
			p.intent = stream.u32();
			p.illuminant = stream.colour();
			p.creator = stream.u32();
			stream.seek(128);

			if (p.signature != 0x61637370) return false; // 'acsp'
			if (stream.eof()) return false;

			const auto tag_count = stream.u32();
			if (tag_count > 1000) return false;

			p.declared_tags = tag_count;

			// A tag's payload is a region of this profile, so the regions the tags name cannot legitimately
			// amount to more than the profile itself. Without this running total a thousand tags pointing
			// into the same megabyte copied a gigabyte out of a file that size. A region several tags share
			// is charged once and its payload shared: the spec allows the sharing, and the sRGB profile most
			// cameras embed points all three tone curves at one curve, so charging each tag for it refused
			// two of the three.
			const auto max_total_tag_bytes = data.size();
			std::size_t total_tag_bytes = 0;
			std::map<std::pair<std::uint32_t, std::uint32_t>, std::shared_ptr<const std::vector<std::uint8_t>>> regions;

			for (std::uint32_t u = 0; u < tag_count; u++)
			{
				if (stream.index() + 12 > data.size()) break;

				const auto sig = stream.u32();
				const auto offset = stream.u32();
				const auto size = stream.u32();

				// A tag that cannot be read is still part of the profile, so it is recorded rather than
				// silently dropped.
				if (offset >= data.size() || size == 0 || size > data.size() - offset)
				{
					p.unread.emplace_back(sig, std::format("out of range (offset {}, size {})", offset, size));
					continue;
				}

				if (size > 1024 * 1024)
				{
					p.unread.emplace_back(sig, std::format("too large to read ({} bytes)", size));
					continue;
				}

				const auto current = stream.seek(offset);
				if (stream.index() + 4 > data.size())
				{
					stream.seek(current);
					p.unread.emplace_back(sig, "truncated");
					continue;
				}

				const auto type = stream.u32();
				const std::uint32_t remaining = size >= 4 ? size - 4 : 0;
				const auto region = std::make_pair(offset, size);

				if (const auto shared = regions.find(region); shared != regions.end())
				{
					stream.seek(current);
					p.tags[sig] = tag{type, shared->second};
					continue;
				}

				if (total_tag_bytes + remaining > max_total_tag_bytes)
				{
					stream.seek(current);
					p.unread.emplace_back(sig, std::format("beyond the profile's own size ({} bytes)", size));
					continue;
				}

				total_tag_bytes += remaining;

				auto payload = std::make_shared<const std::vector<std::uint8_t>>(stream.bytes(remaining));
				regions.emplace(region, payload);
				p.tags[sig] = tag{type, std::move(payload)};
				stream.seek(current);
			}

			return true;
		}
	}

	std::vector<structure_row> list_icc(const std::span<const std::uint8_t> data)
	{
		profile p;
		if (!load(p, data)) return {};
		return list(p);
	}
}
