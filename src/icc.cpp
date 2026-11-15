// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: MPL-2.0

// Purpose: Reading an ICC colour profile, for a person and for pixels. The listing is Diffractor's, moved here with
// the reader it was made by: the header and tag directory read with every bound checked, and the tags a person asks
// about - the name, the colorants, the tone curves, the white point - described. The mapping takes the pixels an RGB
// profile of colorants and tone curves, or a grey one of a tone curve, describes into sRGB, and carries an edit made
// to that sRGB rendition back into them.

#include "libfiles/icc.h"

#include "hdr_internal.h"

#include <algorithm>
#include <array>
#include <bit>
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
		constexpr std::uint32_t type_xyz = 0x58595A20;           // 'XYZ '
		constexpr std::uint32_t type_para = 0x70617261;          // 'para'
		constexpr std::uint32_t type_desc = 0x64657363;          // 'desc', textDescriptionType
		constexpr std::uint32_t type_mluc = 0x6D6C7563;          // 'mluc', multiLocalizedUnicodeType
		constexpr std::uint32_t type_text = 0x74657874;          // 'text'
		constexpr std::uint32_t type_curv = 0x63757276;          // 'curv'
		constexpr std::uint32_t tag_desc = 0x64657363;           // 'desc'
		constexpr std::uint32_t tag_cprt = 0x63707274;           // 'cprt'
		constexpr std::uint32_t tag_dmnd = 0x646D6E64;           // 'dmnd'
		constexpr std::uint32_t tag_dmdd = 0x646D6464;           // 'dmdd'
		constexpr std::uint32_t tag_wtpt = 0x77747074;           // 'wtpt'
		constexpr std::uint32_t tag_red_colorant = 0x7258595A;   // 'rXYZ'
		constexpr std::uint32_t tag_green_colorant = 0x6758595A; // 'gXYZ'
		constexpr std::uint32_t tag_blue_colorant = 0x6258595A;  // 'bXYZ'
		constexpr std::uint32_t tag_red_curve = 0x72545243;      // 'rTRC'
		constexpr std::uint32_t tag_green_curve = 0x67545243;    // 'gTRC'
		constexpr std::uint32_t tag_blue_curve = 0x62545243;     // 'bTRC'
		constexpr std::uint32_t tag_grey_curve = 0x6B545243;     // 'kTRC'
		constexpr std::uint32_t space_rgb = 0x52474220;          // 'RGB '
		constexpr std::uint32_t space_grey = 0x47524159;         // 'GRAY'
		constexpr std::uint32_t connection_xyz = 0x58595A20;     // 'XYZ '

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
					else cp = 0xFFFD;
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
			explicit profile_stream(const std::span<const std::uint8_t> data) : data_(data) {}

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

			std::int16_t i16() { return static_cast<std::int16_t>(u16()); }

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
			const auto raw = static_cast<std::uint32_t>(d[offset]) << 24 |
			                 static_cast<std::uint32_t>(d[offset + 1]) << 16 |
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

			auto curve = describe_curve(t);
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

		icc_summary summarize(const profile& p)
		{
			icc_summary s;

			const std::pair<std::uint32_t, std::string*> named[] = {
				{tag_desc, &s.name},
				{tag_dmdd, &s.device_model},
				{tag_dmnd, &s.device_manufacturer},
				{tag_cprt, &s.copyright},
			};

			for (const auto& [sig, field] : named)
			{
				const auto found = p.tags.find(sig);

				if (found != p.tags.end())
				{
					const auto decoded = decode_text(found->second);
					*field = strip_nuls(decoded);
				}
			}

			s.colour_space = signature_named(p.colour_space, space_name(p.colour_space));
			s.primaries = primaries_name(p);

			const auto curve = p.tags.find(tag_red_curve);
			if (curve != p.tags.end()) s.tone_response = describe_curve(curve->second);

			xyz white;
			if (tag_xyz(p, tag_wtpt, white)) s.white_point = describe_chromaticity(white);

			const auto intent = intent_name(p.intent);
			s.rendering_intent = intent.empty() ? std::format("{}", p.intent) : std::string(intent);
			return s;
		}

		std::vector<structure_row> list(const profile& p)
		{
			std::vector<structure_row> rows;

			// The profile identity is derived from the tags below it, so it is labelled as a summary
			// rather than presented as more of the file's own content.
			add_container(rows, "Summary", "icc.summary");

			const auto s = summarize(p);
			const std::pair<std::string_view, const std::string*> summary[] = {
				{"Profile", &s.name},
				{"Device model", &s.device_model},
				{"Device manufacturer", &s.device_manufacturer},
				{"Copyright", &s.copyright},
				{"Colour space", &s.colour_space},
				{"Primaries", &s.primaries},
				{"Tone response", &s.tone_response},
				{"White point", &s.white_point},
				{"Rendering intent", &s.rendering_intent},
			};

			for (const auto& [name, value] : summary)
			{
				if (!value->empty()) add_row(rows, std::string(name), *value, 1);
			}

			const auto intent = intent_name(p.intent);

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
				        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", p.created.year, p.created.month,
				                    p.created.day, p.created.hour, p.created.minute, p.created.second),
				        1);
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
				row.detail = std::vector<std::uint8_t>(t.data().begin(),
				                                       t.data().begin() + static_cast<std::ptrdiff_t>(kept));
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

	namespace
	{
		using matrix = std::array<double, 9>;

		matrix multiply(const matrix& a, const matrix& b)
		{
			matrix r{};
			for (auto i = 0; i < 3; ++i)
				for (auto j = 0; j < 3; ++j)
					for (auto k = 0; k < 3; ++k) r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
			return r;
		}

		double determinant(const matrix& m)
		{
			return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
			       m[2] * (m[3] * m[7] - m[4] * m[6]);
		}

		// sRGB's colorants as the IEC 61966-2.1 profile states them, adapted to the D50 connection space: the
		// columns are red, green and blue, the rows X, Y and Z. Every RGB profile's colorants are adapted alike, so
		// a profile's white lands on sRGB's.
		constexpr matrix srgb_colorants{0.436066, 0.385147, 0.143066, 0.222488, 0.716873,
		                                0.060608, 0.013916, 0.097076, 0.714096};

		// A tone curve: the linear light, 0 to 1, an encoded value, 0 to 1, stands for.
		class tone_curve
		{
		public:
			// A curv tag - no points, linear; one, a gamma; more, a table read between its points - or one of para's
			// five functions. Nothing for anything else, a gamma that is not above 0, or a table cut short.
			static std::optional<tone_curve> read(const tag& t)
			{
				const auto& d = t.data();
				const auto be16 = [&d](const std::size_t i)
				{
					return static_cast<std::uint32_t>(d[i]) << 8 | d[i + 1];
				};
				tone_curve curve;

				if (t.type == type_curv)
				{
					if (d.size() < 8) return {};
					const auto count = static_cast<std::size_t>(be16(4)) << 16 | be16(6);

					if (count <= 1)
					{
						curve._function = 0;
						curve._p[0] = count == 0 ? 1.0 : d.size() >= 10 ? be16(8) / 256.0 : 0.0;
						if (curve._p[0] <= 0.0) return {};
						return curve;
					}

					if (count > (d.size() - 8) / 2) return {};
					curve._table.resize(count);
					for (std::size_t i = 0; i < count; ++i) curve._table[i] = be16(8 + 2 * i) / 65535.0;
					return curve;
				}

				if (t.type == type_para)
				{
					static constexpr std::size_t parameters[] = {1, 3, 4, 5, 7};
					if (d.size() < 8) return {};
					const auto function = be16(4);
					if (function > 4 || d.size() < 8 + 4 * parameters[function]) return {};

					curve._function = static_cast<int>(function);
					for (std::size_t i = 0; i < parameters[function]; ++i) curve._p[i] = s15_fixed16(d, 8 + 4 * i);
					if (curve._p[0] <= 0.0) return {};
					return curve;
				}

				return {};
			}

			double linear(const double encoded) const
			{
				const auto x = std::clamp(encoded, 0.0, 1.0);

				if (_function < 0)
				{
					const auto at = x * static_cast<double>(_table.size() - 1);
					const auto i = std::min(static_cast<std::size_t>(at), _table.size() - 2);
					return _table[i] + (_table[i + 1] - _table[i]) * (at - static_cast<double>(i));
				}

				const auto [g, a, b, c, d, e, f] = _p;
				const auto power = [g](const double base)
				{
					return base > 0.0 ? std::pow(base, g) : 0.0;
				};

				switch (_function)
				{
				case 0: return power(x);
				case 1: return power(a * x + b);
				case 2: return power(a * x + b) + c;
				case 3: return x >= d ? power(a * x + b) : c * x;
				default: return x >= d ? power(a * x + b) + e : c * x + f;
				}
			}

		private:
			// -1 for a table; else para's function, a gamma being function 0.
			int _function = -1;
			// g, a, b, c, d, e and f.
			std::array<double, 7> _p{};
			std::vector<double> _table;
		};

		// What a profile's pixels are in sRGB's terms: a tone curve for each channel - one for all three of a grey's -
		// and the matrix, row major, that takes their linear light into sRGB's.
		struct srgb_transform
		{
			std::array<tone_curve, 3> curves;
			matrix to_srgb{1, 0, 0, 0, 1, 0, 0, 0, 1};
			bool grey = false;
		};

		std::optional<srgb_transform> transform_of(const std::span<const std::uint8_t> data)
		{
			profile p;
			if (!load(p, data) || p.connection_space != connection_xyz) return {};

			srgb_transform t;

			const auto curve = [&p](const std::uint32_t sig) -> std::optional<tone_curve>
			{
				const auto found = p.tags.find(sig);
				if (found == p.tags.end()) return {};
				return tone_curve::read(found->second);
			};

			if (p.colour_space == space_grey)
			{
				const auto grey = curve(tag_grey_curve);
				if (!grey) return {};
				t.grey = true;
				t.curves = {*grey, *grey, *grey};
				return t;
			}

			if (p.colour_space != space_rgb) return {};

			xyz r, g, b;
			const auto red = curve(tag_red_curve);
			const auto green = curve(tag_green_curve);
			const auto blue = curve(tag_blue_curve);
			if (!tag_xyz(p, tag_red_colorant, r) || !tag_xyz(p, tag_green_colorant, g) ||
			    !tag_xyz(p, tag_blue_colorant, b) || !red || !green || !blue)
			{
				return {};
			}

			const matrix colorants{r.x, g.x, b.x, r.y, g.y, b.y, r.z, g.z, b.z};
			// Colorants that leave white far from a luminance of 1, or that a matrix cannot invert, describe no display.
			if (std::abs(r.y + g.y + b.y - 1.0) > 0.25 || std::abs(determinant(colorants)) < 1e-6) return {};

			t.curves = {*red, *green, *blue};
			t.to_srgb = multiply(detail::invert(srgb_colorants), colorants);

			// White - the colorants' sum - lands on sRGB's exactly, and every grey stays grey, as lcms fixes white:
			// what lies between the two whites is the rounding of the profiles' fixed-point values, or a white an
			// old profile left unadapted.
			for (auto row = 0; row < 3; ++row)
			{
				const auto sum = t.to_srgb[row * 3] + t.to_srgb[row * 3 + 1] + t.to_srgb[row * 3 + 2];
				if (!(sum > 0.5 && sum < 2.0)) return {};
				for (auto column = 0; column < 3; ++column) t.to_srgb[row * 3 + column] /= sum;
			}

			return t;
		}

		// A transform that moves no colour as much as half a level: sRGB's own, or as near it as its profile can say.
		bool is_identity(const srgb_transform& t)
		{
			constexpr matrix identity{1, 0, 0, 0, 1, 0, 0, 0, 1};

			for (auto i = 0; i < 9; ++i)
			{
				if (std::abs(t.to_srgb[i] - identity[i]) > 0.0015) return false;
			}

			for (const auto& curve : t.curves)
			{
				for (auto i = 0; i <= 64; ++i)
				{
					const auto encoded = i / 64.0;
					if (std::abs(detail::srgb_encode(curve.linear(encoded)) - encoded) > 0.5 / 255.0) return false;
				}
			}

			return true;
		}

		bool is_wide(const pixel_format format)
		{
			return format == pixel_format::rgbx16 || format == pixel_format::rgba16;
		}

		bool is_mappable(const pixel_format format)
		{
			return format == pixel_format::bgrx8 || format == pixel_format::bgra8 || is_wide(format);
		}

		// A row's red, green and blue at pixel x as values 0 to the format's top level, and its fourth channel.
		struct pixel_access
		{
			pixel_format format;

			std::array<std::uint32_t, 4> read(const std::uint8_t* const row, const std::uint32_t x) const
			{
				if (is_wide(format))
				{
					const auto* const p = std::bit_cast<const std::uint16_t*>(row) + std::size_t{4} * x;
					return {p[0], p[1], p[2], p[3]};
				}

				const auto* const p = row + std::size_t{4} * x;
				return {p[2], p[1], p[0], p[3]};
			}

			void write(std::uint8_t* const row, const std::uint32_t x, const std::array<std::uint32_t, 4>& v) const
			{
				if (is_wide(format))
				{
					auto* const p = std::bit_cast<std::uint16_t*>(row) + std::size_t{4} * x;
					for (auto c = 0; c < 4; ++c) p[c] = static_cast<std::uint16_t>(v[c]);
					return;
				}

				auto* const p = row + std::size_t{4} * x;
				p[0] = static_cast<std::uint8_t>(v[2]);
				p[1] = static_cast<std::uint8_t>(v[1]);
				p[2] = static_cast<std::uint8_t>(v[0]);
				p[3] = static_cast<std::uint8_t>(v[3]);
			}

			std::uint32_t top() const { return is_wide(format) ? 65535u : 255u; }
		};

		// A value from a table over 0 to 1, read between its entries. NaN - from a curve that overflowed - reads as 0.
		float read_table(const std::vector<float>& table, const float position)
		{
			const auto at = (position > 0.0f ? std::min(position, 1.0f) : 0.0f) * static_cast<float>(table.size() - 1);
			const auto index = std::min(static_cast<std::size_t>(at), table.size() - 2);
			return table[index] + (table[index + 1] - table[index]) * (at - static_cast<float>(index));
		}

		// A curve tabulated for each level of a format: 256 or 65536 entries.
		std::vector<float> levels_of(const std::uint32_t top, const auto& curve)
		{
			std::vector<float> table(std::size_t{top} + 1);
			for (std::uint32_t i = 0; i <= top; ++i) table[i] = static_cast<float>(curve(i / static_cast<double>(top)));
			return table;
		}

		// An encoding by the square root of linear light, which spends its entries where the eye sees steps: near
		// black.
		constexpr int encode_size = 4096;

		std::vector<float> encode_table(const auto& encode)
		{
			std::vector<float> table(encode_size);

			for (auto i = 0; i < encode_size; ++i)
			{
				const auto root = i / (encode_size - 1.0);
				table[i] = static_cast<float>(encode(root * root));
			}

			return table;
		}

		// The encoded value, 0 to 1, a monotonic curve takes to this linear light: found by halving.
		double invert_curve(const tone_curve& curve, const double light)
		{
			auto low = 0.0;
			auto high = 1.0;

			for (auto i = 0; i < 40; ++i)
			{
				const auto middle = (low + high) / 2.0;
				if (curve.linear(middle) < light) low = middle;
				else high = middle;
			}

			return (low + high) / 2.0;
		}

		std::uint32_t quantize(const float v, const std::uint32_t top)
		{
			return v > 0.0f ? static_cast<std::uint32_t>(std::min(v, 1.0f) * static_cast<float>(top) + 0.5f) : 0u;
		}

		// Linear light in sRGB's primaries brought inside the cube sRGB can show by clipping each channel, as a
		// colour-managed viewer shows a matrix profile's colours (relative colorimetric). Desaturating towards
		// luminance instead, as hdr_mapping fits a colour, would keep hue but wash out the saturated colours a wide
		// gamut is for: Display P3's red would come out pink.
		std::array<float, 3> fit_to_srgb(const std::array<float, 3>& rgb)
		{
			return {std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f), std::clamp(rgb[2], 0.0f, 1.0f)};
		}

		std::array<float, 3> times(const std::array<float, 9>& m, const std::array<float, 3>& v)
		{
			return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
			        m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
		}

		std::array<float, 9> to_float(const matrix& m)
		{
			std::array<float, 9> result{};
			for (auto i = 0; i < 9; ++i) result[i] = static_cast<float>(m[i]);
			return result;
		}

		bool fits(const image_view& view, const std::uint32_t width, const std::uint32_t height)
		{
			return view.pixels && view.width == width && view.height == height && is_mappable(view.format) &&
			       view.stride >= std::size_t{width} * (is_wide(view.format) ? 8 : 4);
		}

		// A byte a channel, mapped with integer tables alone, as fast as a display's decode needs: each output channel
		// the sum of each input channel's linear light times its matrix entry, in units of 1/16384 of white, clipped,
		// and encoded by a table of sRGB's curve whose step, at its steepest near black, is a fifth of a level.
		constexpr int byte_white = 1 << 14;

		void map_bytes(const image_view& pixels, const srgb_transform& t, const pixel_buffer& out)
		{
			static const auto encode = []
			{
				std::array<std::uint8_t, byte_white + 1> table{};
				for (auto i = 0; i <= byte_white; ++i)
					table[i] = static_cast<std::uint8_t>(
						std::lround(detail::srgb_encode(i / double{byte_white}) * 255.0));
				return table;
			}();

			// products[output * 3 + input][level]
			std::array<std::array<std::int32_t, 256>, 9> products{};

			for (auto output = 0; output < 3; ++output)
			{
				for (auto input = 0; input < 3; ++input)
				{
					const auto m = t.grey ? (output == input ? 1.0 : 0.0) : t.to_srgb[output * 3 + input];

					for (auto level = 0; level < 256; ++level)
					{
						const auto light = t.curves[input].linear(level / 255.0) * m * byte_white;
						// Bounded well past white, and NaN - from a curve that overflowed - taken as black.
						const auto bounded = std::isnan(light) ? 0.0
						                                       : std::clamp(light, -4.0 * byte_white, 4.0 * byte_white);
						products[output * 3 + input][level] = static_cast<std::int32_t>(std::lround(bounded));
					}
				}
			}

			const auto channel =
				[&products](const int output, const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
			{
				const auto sum = products[output * 3][r] + products[output * 3 + 1][g] + products[output * 3 + 2][b];
				return encode[static_cast<std::size_t>(std::clamp(sum, 0, byte_white))];
			};

			for (std::uint32_t y = 0; y < pixels.height; ++y)
			{
				const auto* source = pixels.pixels + pixels.stride * y;
				auto* target = out.pixels + out.stride * y;

				for (std::uint32_t x = 0; x < pixels.width; ++x, source += 4, target += 4)
				{
					const auto b = source[0];
					const auto g = source[1];
					const auto r = source[2];
					const auto fourth = source[3];
					target[0] = channel(2, r, g, b);
					target[1] = channel(1, r, g, b);
					target[2] = channel(0, r, g, b);
					target[3] = fourth;
				}
			}
		}
	}

	bool icc_declares_rgb(const std::span<const std::uint8_t> data) noexcept
	{
		return data.size() >= 20 && std::equal(data.begin() + 16, data.begin() + 20, "RGB ");
	}

	std::vector<structure_row> list_icc(const std::span<const std::uint8_t> data)
	{
		profile p;
		if (!load(p, data)) return {};
		return list(p);
	}

	std::optional<icc_summary> summarize_icc(const std::span<const std::uint8_t> data)
	{
		profile p;
		if (!load(p, data)) return {};
		return summarize(p);
	}

	srgb_mapping icc_srgb_mapping(const std::span<const std::uint8_t> profile)
	{
		const auto t = transform_of(profile);
		if (!t) return srgb_mapping::unmapped;
		return is_identity(*t) ? srgb_mapping::srgb : srgb_mapping::mapped;
	}

	bool icc_maps_to_srgb(const std::span<const std::uint8_t> profile)
	{
		return icc_srgb_mapping(profile) == srgb_mapping::mapped;
	}

	bool map_icc_to_srgb(const image_view& pixels, const std::span<const std::uint8_t> profile, const pixel_buffer& out)
	{
		if (!fits(pixels, pixels.width, pixels.height) || !out.pixels) return false;

		const auto t = transform_of(profile);
		if (!t || is_identity(*t)) return false;

		if (!is_wide(pixels.format))
		{
			map_bytes(pixels, *t, out);
			return true;
		}

		const pixel_access access{pixels.format};
		const auto top = access.top();
		std::array<std::vector<float>, 3> linear;
		for (auto c = 0; c < 3; ++c)
			linear[c] = levels_of(top, [&t, c](const double v) { return t->curves[c].linear(v); });

		const auto encode = encode_table([](const double v) { return detail::srgb_encode(v); });
		const auto to_srgb = to_float(t->to_srgb);

		for (std::uint32_t y = 0; y < pixels.height; ++y)
		{
			const auto* const source = pixels.pixels + pixels.stride * y;
			auto* const target = out.pixels + out.stride * y;

			for (std::uint32_t x = 0; x < pixels.width; ++x)
			{
				const auto v = access.read(source, x);
				const std::array light{linear[0][v[0]], linear[1][v[1]], linear[2][v[2]]};
				const auto rgb = t->grey ? light : fit_to_srgb(times(to_srgb, light));

				access.write(target, x,
				             {quantize(read_table(encode, std::sqrt(rgb[0])), top),
				              quantize(read_table(encode, std::sqrt(rgb[1])), top),
				              quantize(read_table(encode, std::sqrt(rgb[2])), top), v[3]});
			}
		}

		return true;
	}

	bool carry_srgb_edit(const image_view& pixels, const std::span<const std::uint8_t> profile,
	                     const image_view& before, const image_view& after, const pixel_buffer& out)
	{
		if (!fits(pixels, pixels.width, pixels.height) || !fits(before, pixels.width, pixels.height) ||
		    !fits(after, pixels.width, pixels.height) || !out.pixels)
		{
			return false;
		}

		const auto t = transform_of(profile);
		if (!t || is_identity(*t)) return false;

		const pixel_access access{pixels.format};
		const pixel_access before_access{before.format};
		const pixel_access after_access{after.format};
		const auto top = access.top();

		std::array<std::vector<float>, 3> linear;
		std::array<std::vector<float>, 3> encode;

		for (auto c = 0; c < 3; ++c)
		{
			const auto& curve = t->curves[c];
			linear[c] = levels_of(top, [&curve](const double v) { return curve.linear(v); });
			encode[c] = encode_table([&curve](const double v) { return invert_curve(curve, v); });
		}

		const auto srgb_light = [](const std::uint32_t level_top)
		{
			return levels_of(level_top, [](const double v) { return detail::srgb_decode(v); });
		};
		const auto before_light = srgb_light(before_access.top());
		const auto after_light = before_access.top() == after_access.top() ? before_light
		                                                                   : srgb_light(after_access.top());
		const auto from_srgb = to_float(t->grey ? matrix{1, 0, 0, 0, 1, 0, 0, 0, 1} : detail::invert(t->to_srgb));

		// ISO 21496-1's offset: near black the edit's change is added rather than multiplied, and a zero is no pole.
		constexpr float offset = 1.0f / 64.0f;

		for (std::uint32_t y = 0; y < pixels.height; ++y)
		{
			const auto* const source = pixels.pixels + pixels.stride * y;
			const auto* const was = before.pixels + before.stride * y;
			const auto* const now = after.pixels + after.stride * y;
			auto* const target = out.pixels + out.stride * y;

			for (std::uint32_t x = 0; x < pixels.width; ++x)
			{
				const auto v = access.read(source, x);
				const auto b = before_access.read(was, x);
				const auto a = after_access.read(now, x);

				if (b[0] == a[0] && b[1] == a[1] && b[2] == a[2] && before_access.top() == after_access.top())
				{
					access.write(target, x, v);
					continue;
				}

				const auto was_light = times(from_srgb, {before_light[b[0]], before_light[b[1]], before_light[b[2]]});
				const auto now_light = times(from_srgb, {after_light[a[0]], after_light[a[1]], after_light[a[2]]});
				std::array<std::uint32_t, 4> result{0, 0, 0, v[3]};

				for (auto c = 0; c < 3; ++c)
				{
					const auto scale = (std::max(now_light[c], 0.0f) + offset) /
					                   (std::max(was_light[c], 0.0f) + offset);
					const auto light = std::clamp((linear[c][v[c]] + offset) * scale - offset, 0.0f, 1.0f);
					result[c] = quantize(read_table(encode[c], std::sqrt(light)), top);
				}

				access.write(target, x, result);
			}
		}

		return true;
	}
}
