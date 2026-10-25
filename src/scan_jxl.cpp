// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The JPEG XL scanner, for a bare codestream and the box container, and the read of a JPEG
// XL file's XMP packet. The codestream's headers - its size header and image metadata - are read bit
// by bit as libjxl reads them, for the size, orientation, colour channels and alpha its basic info
// reports; the image data never is. The 'Exif' and 'xml ' boxes are taken as libjxl hands them to a
// caller that subscribes to its box events, as Diffractor did: a box counts once the decoder moves on
// past it, so a fault anywhere ends the boxes there, and the last of each kind wins.

#include "scan_internal.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint32_t fourcc(const char (&name)[5])
		{
			return static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[0])) << 24 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[1])) << 16 |
				static_cast<std::uint32_t>(static_cast<std::uint8_t>(name[2])) << 8 |
				static_cast<std::uint8_t>(name[3]);
		}

		constexpr auto box_signature = fourcc("JXL ");
		constexpr auto box_ftyp = fourcc("ftyp");
		constexpr auto box_jxlc = fourcc("jxlc");
		constexpr auto box_jxlp = fourcc("jxlp");
		constexpr auto box_brob = fourcc("brob");
		constexpr auto box_exif = fourcc("Exif");
		constexpr auto box_xml = fourcc("xml ");
		constexpr auto box_jxll = fourcc("jxll");
		constexpr auto brand_jxl = fourcc("jxl ");

		constexpr std::uint8_t container_signature[12] = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};

		// A metadata box larger than this stops the scan of the boxes, as it stopped Diffractor's.
		constexpr std::size_t max_metadata_block = 16u * 1024u * 1024u;

		// What the caller's decompressor may produce for one scan, across every compressed box: twice
		// the largest block, which lets through an Exif block and a packet of the largest size - all a
		// scan keeps - while a file of many small boxes that each expand to the limit costs no more than
		// two of them. Once it is spent the boxes stop, as at a box too large to keep.
		static_assert(max_jxl_decompressed == 2 * max_metadata_block);

		// The codestream bytes the headers are read from: a little first, more as they need it, and no
		// more than 8 MiB, which no header that libjxl writes comes near. However the codestream is cut
		// into parts, the reads of its headers together read no more than eight times that.
		constexpr std::size_t header_read_step = 4096;
		constexpr std::size_t max_header_bytes = 8u * 1024u * 1024u;
		constexpr std::uint64_t max_header_work = 8ull * max_header_bytes;

		// Out-of-order 'jxlp' boxes libjxl will hold, and boxes a structure report lists.
		constexpr std::size_t max_buffered_parts = 1024;
		constexpr std::size_t max_listed_boxes = 4096;

		// The ICC profile libjxl will decode.
		constexpr std::uint64_t max_icc_size = 268435456;

		std::string fourcc_text(const std::uint32_t code)
		{
			std::string text(4, ' ');

			for (std::size_t i = 0; i < 4; ++i)
			{
				text[i] = static_cast<char>(static_cast<std::uint8_t>(code >> (24 - 8 * i)));
			}

			return text;
		}

		// libjxl's header bits read least significant first. A read past the end yields zero bits and
		// is remembered, which is how a header too short to read tells itself from one that is wrong.
		class bit_reader
		{
		public:
			explicit bit_reader(const std::span<const std::uint8_t> data) : _data(data)
			{
			}

			std::uint64_t read(const unsigned count)
			{
				std::uint64_t value = 0;

				for (unsigned done = 0; done < count;)
				{
					const auto byte = _position / 8;
					const auto shift = static_cast<unsigned>(_position % 8);
					const auto take = std::min(count - done, 8u - shift);

					if (byte < _data.size())
					{
						value |= static_cast<std::uint64_t>(_data[static_cast<std::size_t>(byte)] >> shift & ((1u << take) - 1u)) << done;
					}

					done += take;
					_position += take;
				}

				return value;
			}

			void skip(const std::uint64_t count)
			{
				_position = count > std::numeric_limits<std::uint64_t>::max() - _position
					            ? std::numeric_limits<std::uint64_t>::max()
					            : _position + count;
			}

			bool within_bounds() const { return _position <= static_cast<std::uint64_t>(_data.size()) * 8; }
			std::uint64_t position() const { return _position; }

		private:
			std::span<const std::uint8_t> _data;
			std::uint64_t _position = 0;
		};

		// Thrown while reading a header: not enough bytes to read it, or bytes libjxl refuses.
		struct need_more
		{
		};

		struct invalid_header
		{
		};

		// One of a U32 field's four distributions: a value, or a number of bits and an offset.
		struct distribution
		{
			std::uint32_t value_or_offset = 0;
			unsigned bits = 0;
			bool direct = true;
		};

		constexpr distribution val(const std::uint32_t value) { return {value, 0, true}; }
		constexpr distribution bits_offset(const unsigned bits, const std::uint32_t offset) { return {offset, bits, false}; }
		constexpr distribution bits_only(const unsigned bits) { return {0, bits, false}; }

		// The visitor libjxl's Bundle::Read applies, for the field encodings the headers use.
		class field_reader
		{
		public:
			explicit field_reader(bit_reader& bits) : _bits(bits)
			{
			}

			std::uint32_t bits(const unsigned count)
			{
				const auto value = static_cast<std::uint32_t>(_bits.read(count));
				if (!_bits.within_bounds()) throw need_more{};
				return value;
			}

			bool flag() { return bits(1) == 1; }

			std::uint32_t u32(const distribution d0, const distribution d1, const distribution d2, const distribution d3)
			{
				const distribution d[4] = {d0, d1, d2, d3};
				const auto& chosen = d[bits(2)];
				if (chosen.direct) return chosen.value_or_offset;
				return bits(chosen.bits) + chosen.value_or_offset;
			}

			std::uint64_t u64()
			{
				const auto selector = bits(2);
				if (selector == 0) return 0;
				if (selector == 1) return 1 + bits(4);
				if (selector == 2) return 17 + bits(8);

				std::uint64_t value = bits(12);
				unsigned shift = 12;

				while (bits(1))
				{
					if (shift == 60)
					{
						value |= static_cast<std::uint64_t>(bits(4)) << shift;
						break;
					}

					value |= static_cast<std::uint64_t>(bits(8)) << shift;
					shift += 8;
				}

				return value;
			}

			// A half float. libjxl refuses infinity and NaN only once the whole bundle is read, and
			// leaves the field at its default meanwhile.
			float f16(const float default_value)
			{
				const auto word = bits(16);
				const auto sign = word >> 15;
				const auto exponent = word >> 10 & 0x1Fu;
				const auto mantissa = word & 0x3FFu;

				if (exponent == 31)
				{
					_f16_failed = true;
					return default_value;
				}

				float value = 0;

				if (exponent == 0)
				{
					value = (1.0f / 16384) * (static_cast<float>(mantissa) * (1.0f / 1024));
				}
				else
				{
					const std::uint32_t bits32 = sign << 31 | (exponent + 112) << 23 | mantissa << 13;
					std::memcpy(&value, &bits32, sizeof(value));
					return value;
				}

				return sign ? -value : value;
			}

			// An enum, with the values libjxl knows for it.
			std::uint32_t enumeration(const std::uint64_t valid)
			{
				const auto value = u32(val(0), val(1), bits_offset(4, 2), bits_offset(6, 18));
				if (value >= 64 || !(valid >> value & 1u)) throw invalid_header{};
				return value;
			}

			// The extension fields a bundle ends with: libjxl skips the ones it does not know.
			void extensions()
			{
				const auto present = u64();
				if (present == 0) return;

				std::uint64_t total = 0;

				for (auto remaining = present; remaining != 0; remaining &= remaining - 1)
				{
					const auto size = u64();
					if (size > std::numeric_limits<std::uint64_t>::max() - total) throw invalid_header{};
					total += size;
				}

				skip(total);
			}

			// Bits libjxl reads and libfiles has no use for, such as a channel's name.
			void skip(const std::uint64_t count)
			{
				_bits.skip(count);
				if (!_bits.within_bounds()) throw need_more{};
			}

			bool f16_failed() const { return _f16_failed; }
			std::uint64_t position() const { return _bits.position(); }

		private:
			bit_reader& _bits;
			bool _f16_failed = false;
		};

		constexpr std::uint64_t bit(const unsigned n) { return 1ull << n; }

		// The colour spaces, white points, primaries, transfer functions and rendering intents
		// libjxl's enums allow.
		constexpr auto valid_colour_spaces = bit(0) | bit(1) | bit(2) | bit(3);
		constexpr auto valid_white_points = bit(1) | bit(2) | bit(10) | bit(11);
		constexpr auto valid_primaries = bit(1) | bit(2) | bit(9) | bit(11);
		constexpr auto valid_transfer_functions = bit(1) | bit(2) | bit(8) | bit(13) | bit(16) | bit(17) | bit(18);
		constexpr auto valid_rendering_intents = bit(0) | bit(1) | bit(2) | bit(3);
		constexpr auto valid_extra_channels = bit(0) | bit(1) | bit(2) | bit(3) | bit(4) | bit(5) | bit(6) | bit(15) | bit(16);

		constexpr std::uint32_t colour_space_grey = 1;
		constexpr std::uint32_t colour_space_xyb = 2;
		constexpr std::uint32_t colour_space_unknown = 3;
		constexpr std::uint32_t extra_channel_alpha = 0;
		constexpr std::uint32_t extra_channel_spot = 2;
		constexpr std::uint32_t extra_channel_cfa = 5;
		constexpr std::uint32_t extra_channel_unknown = 15;

		// What libjxl's JxlDecoderGetBasicInfo reports, and what reading the rest of the headers needs.
		struct basic_info
		{
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint32_t orientation = 1;
			std::uint32_t bits_per_sample = 8;
			bool floating_point = false;
			bool grey = false;
			std::uint32_t alpha_bits = 0;
			bool alpha_premultiplied = false;
			std::uint32_t extra_channels = 0;
			bool xyb_encoded = true;
			bool want_icc = false;
			bool have_preview = false;
			bool have_animation = false;
		};

		std::uint32_t aspect_width(const std::uint32_t ratio, const std::uint32_t height)
		{
			static constexpr std::uint32_t ratios[7][2] = {{1, 1}, {12, 10}, {4, 3}, {3, 2}, {16, 9}, {5, 4}, {2, 1}};
			return static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * ratios[ratio - 1][0] / ratios[ratio - 1][1]);
		}

		void read_size_header(field_reader& f, std::uint32_t& width, std::uint32_t& height)
		{
			const auto small = f.flag();

			if (small) height = (f.bits(5) + 1) * 8;
			else height = f.u32(bits_offset(9, 1), bits_offset(13, 1), bits_offset(18, 1), bits_offset(30, 1));

			const auto ratio = f.bits(3);

			if (ratio != 0) width = aspect_width(ratio, height);
			else if (small) width = (f.bits(5) + 1) * 8;
			else width = f.u32(bits_offset(9, 1), bits_offset(13, 1), bits_offset(18, 1), bits_offset(30, 1));
		}

		void read_preview_header(field_reader& f)
		{
			const auto div8 = f.flag();

			if (div8) f.u32(val(16), val(32), bits_offset(5, 1), bits_offset(9, 33));
			else f.u32(bits_offset(6, 1), bits_offset(8, 65), bits_offset(10, 321), bits_offset(12, 1345));

			if (f.bits(3) == 0)
			{
				if (div8) f.u32(val(16), val(32), bits_offset(5, 1), bits_offset(9, 33));
				else f.u32(bits_offset(6, 1), bits_offset(8, 65), bits_offset(10, 321), bits_offset(12, 1345));
			}
		}

		void read_animation_header(field_reader& f)
		{
			f.u32(val(100), val(1000), bits_offset(10, 1), bits_offset(30, 1));
			f.u32(val(1), val(1001), bits_offset(8, 1), bits_offset(10, 1));
			f.u32(val(0), bits_only(3), bits_only(16), bits_only(32));
			f.flag();
		}

		void read_bit_depth(field_reader& f, std::uint32_t& bits_per_sample, bool& floating_point)
		{
			floating_point = f.flag();

			if (!floating_point)
			{
				bits_per_sample = f.u32(val(8), val(10), val(12), bits_offset(6, 1));
				if (bits_per_sample > 31) throw invalid_header{};
				return;
			}

			bits_per_sample = f.u32(val(32), val(16), val(24), bits_offset(6, 1));
			const auto exponent = f.bits(4) + 1;
			const auto mantissa = static_cast<int>(bits_per_sample) - static_cast<int>(exponent) - 1;

			if (exponent < 2 || exponent > 8 || mantissa < 2 || mantissa > 23) throw invalid_header{};
		}

		void read_extra_channel(field_reader& f, basic_info& info, const bool first_alpha)
		{
			std::uint32_t type = extra_channel_alpha;
			std::uint32_t bits = 8;
			auto floating = false;
			auto premultiplied = false;

			if (!f.flag())
			{
				type = f.enumeration(valid_extra_channels);
				read_bit_depth(f, bits, floating);

				const auto dim_shift = f.u32(val(0), val(3), val(4), bits_offset(3, 1));
				if ((1u << dim_shift) > 8) throw invalid_header{};

				const auto name_length = f.u32(val(0), bits_only(4), bits_offset(5, 16), bits_offset(10, 48));
				f.skip(8ull * name_length);

				if (type == extra_channel_alpha) premultiplied = f.flag();

				if (type == extra_channel_spot)
				{
					for (auto i = 0; i < 4; ++i) f.f16(0);
				}

				if (type == extra_channel_cfa) f.u32(val(1), bits_only(2), bits_offset(4, 3), bits_offset(8, 19));
				if (type == extra_channel_unknown) throw invalid_header{};
			}

			if (type == extra_channel_alpha && first_alpha)
			{
				info.alpha_bits = bits;
				info.alpha_premultiplied = premultiplied;
			}
		}

		// A custom white point must make an ICC profile; its x and y are millionths.
		void check_white_point(const std::int64_t x, const std::int64_t y)
		{
			const auto wx = static_cast<double>(x) / 1000000.0;
			const auto wy = static_cast<double>(y) / 1000000.0;
			if (wx < 0 || wx > 1 || wy <= 0 || wy > 1) throw invalid_header{};
		}

		std::int64_t custom_xy(field_reader& f)
		{
			const auto packed = f.u32(bits_only(19), bits_offset(19, 524288), bits_offset(20, 1048576), bits_offset(21, 2097152));
			return (packed & 1u) ? -static_cast<std::int64_t>((packed + 1u) / 2) : static_cast<std::int64_t>(packed / 2);
		}

		void read_colour_encoding(field_reader& f, basic_info& info)
		{
			if (f.flag()) return;

			info.want_icc = f.flag();
			const auto space = f.enumeration(valid_colour_spaces);
			info.grey = space == colour_space_grey;

			if (info.want_icc) return;

			if (space != colour_space_xyb)
			{
				const auto white_point = f.enumeration(valid_white_points);

				if (white_point == 2)
				{
					const auto x = custom_xy(f);
					const auto y = custom_xy(f);
					check_white_point(x, y);
				}
			}

			if (space != colour_space_grey && space != colour_space_xyb)
			{
				if (f.enumeration(valid_primaries) == 2)
				{
					for (auto i = 0; i < 6; ++i) custom_xy(f);
				}
			}

			auto transfer_unknown = false;

			if (space != colour_space_xyb)
			{
				if (f.flag())
				{
					// The gamma, in ten-millionths: from 1/8192 to 1.
					const auto gamma = f.bits(24);
					if (gamma > 10000000u || static_cast<std::uint64_t>(gamma) * 8192 < 10000000u) throw invalid_header{};
				}
				else
				{
					transfer_unknown = f.enumeration(valid_transfer_functions) == 2;
				}
			}

			const auto intent = f.enumeration(valid_rendering_intents);

			if (space == colour_space_unknown || transfer_unknown) throw invalid_header{};

			// libjxl builds an ICC profile from these fields, and has one only for a perceptual XYB.
			if (space == colour_space_xyb && intent != 0) throw invalid_header{};
		}

		void read_tone_mapping(field_reader& f)
		{
			if (f.flag()) return;

			const auto intensity_target = f.f16(255.0f);
			if (intensity_target <= 0.0f) throw invalid_header{};

			const auto min_nits = f.f16(0.0f);
			if (min_nits < 0.0f || min_nits > intensity_target) throw invalid_header{};

			const auto relative = f.flag();
			const auto linear_below = f.f16(0.0f);
			if (linear_below < 0.0f || (relative && linear_below > 1.0f)) throw invalid_header{};
		}

		void read_image_metadata(field_reader& f, basic_info& info)
		{
			if (f.flag()) return;

			const auto extra_fields = f.flag();

			if (extra_fields)
			{
				info.orientation = f.bits(3) + 1;

				if (f.flag())
				{
					std::uint32_t width = 0;
					std::uint32_t height = 0;
					read_size_header(f, width, height);
				}

				info.have_preview = f.flag();
				if (info.have_preview) read_preview_header(f);

				info.have_animation = f.flag();
				if (info.have_animation) read_animation_header(f);
			}

			read_bit_depth(f, info.bits_per_sample, info.floating_point);
			f.flag();

			info.extra_channels = f.u32(val(0), val(1), bits_offset(4, 2), bits_offset(12, 1));

			// Every alpha channel has at least one bit, so a nonzero depth marks the first found.
			for (std::uint32_t i = 0; i < info.extra_channels; ++i) read_extra_channel(f, info, info.alpha_bits == 0);

			info.xyb_encoded = f.flag();
			read_colour_encoding(f, info);
			if (extra_fields) read_tone_mapping(f);
			f.extensions();
		}

		void read_transform_data(field_reader& f, const bool xyb_encoded)
		{
			if (f.flag()) return;

			if (xyb_encoded && !f.flag())
			{
				for (auto i = 0; i < 16; ++i) f.f16(0);
			}

			const auto weights = f.bits(3);
			if (weights & 1u) for (auto i = 0; i < 15; ++i) f.f16(0);
			if (weights & 2u) for (auto i = 0; i < 55; ++i) f.f16(0);
			if (weights & 4u) for (auto i = 0; i < 210; ++i) f.f16(0);
		}

		enum class header_status : std::uint8_t
		{
			ok,
			more,
			invalid,
		};

		struct header_read
		{
			header_status basic = header_status::more;
			// Everything libjxl reads before it calls the codestream done for a caller that wants no
			// pixels: the transform data, and the colour profile's start.
			header_status all = header_status::more;
			basic_info info;
			// While a status is more: the codestream bytes the read ran out at, which it needs to get
			// any further.
			std::uint64_t needed = 2;
		};

		// The codestream's headers as libjxl reads them from the bytes it has so far, the FF 0A
		// signature first. libjxl decodes an embedded ICC profile here too; libfiles reads only its
		// size, and takes the rest of it to be sound.
		header_read read_headers(const std::span<const std::uint8_t> codestream)
		{
			header_read result;

			if (codestream.size() < 2) return result;

			if (codestream[0] != 0xFF || codestream[1] != 0x0A)
			{
				result.basic = header_status::invalid;
				return result;
			}

			bit_reader bits(codestream.subspan(2));

			const auto needed = [&bits]
			{
				const auto position = bits.position();
				return std::min<std::uint64_t>(position / 8 + (position % 8 != 0 ? 1 : 0), max_header_bytes) + 2;
			};

			try
			{
				field_reader size(bits);
				read_size_header(size, result.info.width, result.info.height);

				field_reader metadata(bits);
				read_image_metadata(metadata, result.info);
				if (metadata.f16_failed()) throw invalid_header{};
			}
			catch (const need_more&)
			{
				result.needed = needed();
				return result;
			}
			catch (const invalid_header&)
			{
				result.basic = header_status::invalid;
				return result;
			}

			result.basic = header_status::ok;

			try
			{
				field_reader transform(bits);
				read_transform_data(transform, result.info.xyb_encoded);
				if (transform.f16_failed()) throw invalid_header{};

				if (result.info.want_icc)
				{
					field_reader icc(bits);
					if (icc.u64() > max_icc_size) throw invalid_header{};
				}
				else
				{
					// libjxl then moves to the next byte, whose padding must be zero; bits past the end
					// read as zero to it.
					const auto padding = static_cast<unsigned>((8 - bits.position() % 8) % 8);
					if (padding != 0 && bits.read(padding) != 0) throw invalid_header{};
				}

				result.all = header_status::ok;
			}
			catch (const need_more&)
			{
				result.needed = needed();
			}
			catch (const invalid_header&)
			{
				result.all = header_status::invalid;
			}

			return result;
		}

		bool settled(const header_read& h)
		{
			return h.basic == header_status::invalid || (h.basic == header_status::ok && h.all != header_status::more);
		}

		// A read that cannot get further is refused, as libjxl refuses a header it cannot read.
		void give_up(header_read& h)
		{
			(h.basic == header_status::more ? h.basic : h.all) = header_status::invalid;
		}

		// The codestream as libjxl assembles it from 'jxlc' or 'jxlp' boxes, read only as far as its
		// headers need. A part is kept only until its bytes are read, and one with none is not kept, so
		// what this holds does not grow with the number of parts.
		class codestream
		{
		public:
			void add(const std::uint64_t offset, const std::uint64_t size)
			{
				if (size > 0) _parts.push_back({offset, size});
			}

			std::size_t parts_held() const { return _parts.size(); }

			// libjxl reads the headers again as each part arrives. A read here is repeated only once
			// there are the bytes the last one ran out at, and the reads together are bounded, so a
			// header cut into many small parts costs no more than reading it whole a few times.
			header_read headers(reader& r)
			{
				for (;;)
				{
					if (settled(_last)) return _last;

					if (_bytes.size() >= _last.needed)
					{
						_work += _bytes.size();

						if (_work > max_header_work)
						{
							give_up(_last);
							return _last;
						}

						_last = read_headers(_bytes);
						if (settled(_last)) return _last;
					}

					if (_parts.empty()) return _last;

					if (_bytes.size() >= max_header_bytes)
					{
						give_up(_last);
						return _last;
					}

					read_more(r, std::max<std::uint64_t>({header_read_step, _bytes.size(), _last.needed > _bytes.size() ? _last.needed - _bytes.size() : 0}));
				}
			}

		private:
			struct part
			{
				std::uint64_t offset = 0;
				std::uint64_t size = 0;
			};

			void read_more(reader& r, std::uint64_t count)
			{
				count = std::min<std::uint64_t>(count, max_header_bytes - _bytes.size());

				while (count > 0 && !_parts.empty())
				{
					const auto p = _parts.front();
					const auto take = std::min(count, p.size - _part_read);
					const auto bytes = r.read(p.offset + _part_read, static_cast<std::size_t>(take));
					_bytes.insert(_bytes.end(), bytes.begin(), bytes.end());
					_part_read += take;
					count -= take;

					if (_part_read == p.size)
					{
						_parts.pop_front();
						_part_read = 0;
					}
				}
			}

			// The parts not yet read whole, the first of them read as far as _part_read.
			std::deque<part> _parts;
			std::uint64_t _part_read = 0;
			std::vector<std::uint8_t> _bytes;
			header_read _last;
			std::uint64_t _work = 0;
		};

		enum class box_kind : std::uint8_t
		{
			exif,
			xmp,
		};

		// A metadata box whose contents are read, waiting for the decoder to move past it.
		struct metadata_box
		{
			box_kind kind = box_kind::exif;
			std::vector<std::uint8_t> data;
			// Compressed, with no decompressor to hand: left out.
			bool unknown = false;
		};

		struct jxl_read
		{
			bool container = false;
			std::optional<basic_info> info;
			std::optional<std::vector<std::uint8_t>> exif;
			std::optional<std::vector<std::uint8_t>> xmp;
			// The last packet box read was compressed, and could not be.
			bool xmp_unknown = false;
			// The decoder reached the end of the file without a fault.
			bool complete = false;
			// The most codestream parts held at once, waiting to be read or held out of order, which
			// the tests check stays small.
			std::size_t peak_parts = 0;
		};

		// Reads the file as libjxl decodes it for a caller that wants its basic info and every box, with
		// the input closed at the end of the file. A box's contents count once the decoder moves past it:
		// to the next box's header, or to the end with nothing wrong.
		jxl_read read_jxl(reader& r, const brotli_decompressor* const brotli)
		{
			jxl_read out;
			const auto file_size = r.size();

			if (r.u8(0) == 0xFF && r.u8(1) == 0x0A)
			{
				codestream bare;
				bare.add(0, file_size);
				const auto h = bare.headers(r);
				if (h.basic == header_status::ok) out.info = h.info;
				out.complete = true;
				return out;
			}

			out.container = true;
			if (file_size < sizeof(container_signature) ||
				std::memcmp(r.view(0, sizeof(container_signature)).data(), container_signature, sizeof(container_signature)) != 0)
			{
				throw format_error("invalid JPEG XL signature");
			}

			std::optional<metadata_box> pending;

			const auto finish = [&]
			{
				if (pending)
				{
					if (pending->kind == box_kind::exif)
					{
						if (!pending->unknown) out.exif = std::move(pending->data);
					}
					else if (pending->unknown)
					{
						out.xmp_unknown = true;
					}
					else
					{
						out.xmp = std::move(pending->data);
						out.xmp_unknown = false;
					}
				}

				pending.reset();
			};

			struct buffered_part
			{
				std::uint64_t offset = 0;
				std::uint64_t size = 0;
				bool last = false;
			};

			codestream stream;
			std::map<std::uint32_t, buffered_part> buffered;
			std::uint64_t pos = 0;
			std::uint64_t box_count = 0;
			std::uint32_t version = 0;
			std::uint32_t next_index = 0;
			auto last_codestream_seen = false;
			auto finished = false;
			std::size_t decompression_left = max_jxl_decompressed;

			const auto note_parts = [&]
			{
				out.peak_parts = std::max(out.peak_parts, stream.parts_held() + buffered.size());
			};

			for (;;)
			{
				if (pos == file_size)
				{
					// libjxl stops at the end of the input; it fails there when the codestream is not done.
					if (finished)
					{
						finish();
						out.complete = true;
					}

					return out;
				}

				const auto remaining = file_size - pos;
				const auto codestream_done = finished && last_codestream_seen;

				// A second codestream or container after the first ends this one.
				if (codestream_done && remaining >= 2 && r.u8(pos) == 0xFF && r.u8(pos + 1) == 0x0A)
				{
					finish();
					out.complete = true;
					return out;
				}

				if (remaining < 8) return out;

				std::uint64_t box_size = r.be32(pos);
				const auto type = r.be32(pos + 4);
				std::uint64_t header = 8;

				if (box_size == 1)
				{
					if (remaining < 16) return out;
					box_size = r.be64(pos + 8);
					header = 16;
				}

				if (box_size > 0 && box_size < header) return out;
				if (box_size > std::numeric_limits<std::uint64_t>::max() - pos) return out;

				auto inner = type;

				if (type == box_brob)
				{
					if (remaining < header + 4) return out;
					inner = r.be32(pos + header);
				}

				++box_count;

				if (codestream_done && type == box_signature)
				{
					finish();
					out.complete = true;
					return out;
				}

				if ((box_count == 2) != (type == box_ftyp)) return out;
				if (type == box_jxlc && last_codestream_seen) return out;

				// The box event: the box before this one is done.
				finish();

				const auto unbounded = box_size == 0;
				const auto truncated = !unbounded && box_size > remaining;
				const auto end = unbounded || truncated ? file_size : pos + box_size;
				const auto contents = pos + header;
				const auto contents_size = unbounded ? 0 : box_size - header;
				const auto available = end - contents;

				if (type == box_ftyp)
				{
					if (contents_size < 12 || available < 8) return out;
					if (r.be32(contents) != brand_jxl) return out;

					version = r.be32(contents + 4);
					if (version > 1 || truncated) return out;

					pos = end;
					continue;
				}

				if (type == box_jxlc || type == box_jxlp)
				{
					auto data = contents;

					if (type == box_jxlp)
					{
						if (last_codestream_seen || available < 4) return out;
						if (!unbounded && contents_size < 4) return out;

						const auto index = r.be32(contents);
						const auto counter = index & 0x7FFFFFFFu;
						const auto last = (index & 0x80000000u) != 0;
						data = contents + 4;

						if (counter < next_index) return out;

						if (counter != next_index)
						{
							// Out of order: version 1 files may, and libjxl holds the part until its turn.
							if (version < 1 || buffered.size() >= max_buffered_parts || buffered.contains(counter)) return out;
							if (unbounded || truncated) return out;

							buffered[counter] = {data, end - data, last};
							note_parts();
							pos = end;
							continue;
						}

						++next_index;
						if (last) last_codestream_seen = true;
					}
					else
					{
						last_codestream_seen = true;
					}

					if (!finished)
					{
						stream.add(data, end - data);
						note_parts();

						for (;;)
						{
							const auto h = stream.headers(r);
							if (h.basic == header_status::ok && !out.info) out.info = h.info;
							if (h.basic == header_status::invalid || h.all == header_status::invalid) return out;

							if (h.all == header_status::ok)
							{
								finished = true;
								break;
							}

							// The box is spent: the next part is one held back, or in the boxes to come.
							if (unbounded || truncated) return out;

							const auto held = buffered.find(next_index);
							if (held == buffered.end()) break;

							stream.add(held->second.offset, held->second.size);
							if (held->second.last) last_codestream_seen = true;
							buffered.erase(held);
							note_parts();
							++next_index;
						}
					}

					if (finished && unbounded)
					{
						finish();
						out.complete = true;
						return out;
					}

					if (truncated) return out;

					pos = end;
					continue;
				}

				// Any other box is skipped; an Exif or XMP box is read as it goes past.
				if (inner == box_exif || inner == box_xml)
				{
					if (truncated) return out;

					metadata_box box;
					box.kind = inner == box_exif ? box_kind::exif : box_kind::xmp;
					const auto compressed = type == box_brob;

					if (compressed && !unbounded && contents_size < 4) return out;

					const auto data = contents + (compressed ? 4 : 0);
					const auto data_size = end - data;

					// Diffractor gave a box at most 16 MiB, and stopped reading boxes at one larger.
					if (data_size > max_metadata_block) return out;

					if (!compressed)
					{
						box.data = r.read(data, static_cast<std::size_t>(data_size));
					}
					else if (!brotli || !*brotli)
					{
						box.unknown = true;
					}
					else
					{
						// The scan's decompression spent, the boxes stop as at one too large to keep.
						if (decompression_left == 0) return out;

						const auto limit = std::min(max_metadata_block, decompression_left);
						const auto bytes = r.read(data, static_cast<std::size_t>(data_size));
						auto decompressed = (*brotli)(bytes, fourcc_text(inner), limit);

						// libjxl stops at a box it cannot decompress; libfiles stops too at one that
						// would overspend the scan's decompression.
						if (!decompressed || decompressed->size() > limit) return out;

						decompression_left -= decompressed->size();
						box.data = std::move(*decompressed);
					}

					pending = std::move(box);
				}

				// libjxl skips an unbounded box to the end of the input, and fails there when the
				// codestream is not done.
				if (unbounded)
				{
					if (finished)
					{
						finish();
						out.complete = true;
					}

					return out;
				}

				if (truncated) return out;
				pos = end;
			}
		}

		std::string_view box_description(const std::uint32_t type)
		{
			switch (type)
			{
			case box_signature: return "JPEG XL signature";
			case box_ftyp: return "file type";
			case box_jxll: return "codestream level";
			case box_jxlc: return "codestream";
			case box_jxlp: return "partial codestream";
			case box_exif: return "Exif metadata";
			case box_xml: return "XMP metadata";
			case box_brob: return "Brotli-compressed box";
			case fourcc("jumb"): return "JUMBF metadata";
			case fourcc("jbrd"): return "JPEG reconstruction data";
			case fourcc("jxli"): return "frame index";
			default: return {};
			}
		}

		// A box inventory and the codestream's header fields. The boxes are walked by their headers
		// alone, to the first that does not fit the file.
		std::vector<structure_row> jxl_structure(reader& r, const jxl_read& read)
		{
			std::vector<structure_row> rows;
			const auto file_size = r.size();

			add_structure_section(rows, "Summary", "jxl.summary", true);
			add_structure_row(rows, "File size", std::format("{} bytes", file_size));
			add_structure_row(rows, "Container", read.container ? "ISO base media boxes" : "none, a bare codestream");

			if (read.info)
			{
				const auto& info = *read.info;
				add_structure_row(rows, "Dimensions", std::format("{} x {}", info.width, info.height));
				add_structure_row(rows, "Orientation", std::to_string(info.orientation));
				add_structure_row(rows, "Bits per sample",
				                  info.floating_point ? std::format("{}, floating point", info.bits_per_sample) : std::to_string(info.bits_per_sample));
				add_structure_row(rows, "Colour channels", info.grey ? "1, grey" : "3");
				add_structure_row(rows, "Alpha channel", info.alpha_bits == 0
					                                         ? std::string("none")
					                                         : std::format("{} bits{}", info.alpha_bits,
					                                                       info.alpha_premultiplied ? ", premultiplied" : ""));

				if (info.extra_channels > 0) add_structure_row(rows, "Extra channels", std::to_string(info.extra_channels));

				add_structure_row(rows, "Colour profile", info.want_icc ? "ICC, in the codestream" : "described by the codestream");
				add_structure_row(rows, "Coding", info.xyb_encoded ? "XYB (lossy)" : "original colour space");
				if (info.have_preview) add_structure_row(rows, "Preview", "present");
				if (info.have_animation) add_structure_row(rows, "Animation", "present");
			}

			if (!read.container)
			{
				finish_structure_sections(rows);
				return rows;
			}

			add_structure_section(rows, "Boxes", "jxl.boxes");

			std::uint64_t offset = 0;

			for (std::size_t listed = 0; listed < max_listed_boxes && file_size - offset >= 8; ++listed)
			{
				std::uint64_t size = r.be32(offset);
				const auto type = r.be32(offset + 4);
				std::uint64_t header = 8;

				if (size == 1)
				{
					if (file_size - offset < 16) break;
					size = r.be64(offset + 8);
					header = 16;
				}

				if (size == 0) size = file_size - offset;
				if (size < header || size > file_size - offset) break;

				auto name = fourcc_text(type);
				name = name.substr(0, name.find('\0'));
				auto description = std::string(box_description(type));

				if (type == box_brob && size >= header + 4)
				{
					auto inner = fourcc_text(r.be32(offset + header));
					inner = inner.substr(0, inner.find('\0'));
					description = std::format("Brotli-compressed {}", inner);
				}
				else if (type == box_jxll && size > header)
				{
					description = std::format("codestream level {}", r.u8(offset + header));
				}
				else if (type == box_jxlp && size >= header + 4)
				{
					const auto index = r.be32(offset + header);
					description = std::format("partial codestream {}{}", index & 0x7FFFFFFFu, (index & 0x80000000u) ? ", the last" : "");
				}

				auto value = std::format("offset {}, {} bytes", offset, size);
				if (!description.empty()) value = std::format("{}, {}", description, value);

				auto& row = add_row(rows, name, std::move(value));
				row.depth = 1;
				row.id = std::format("jxl.box.{}", offset);

				offset += size;
			}

			finish_structure_sections(rows);
			return rows;
		}
	}

	scan_result scan_jxl(reader& r, const scan_intent intent, const scan_limits& limits)
	{
		auto read = read_jxl(r, &limits.brotli);
		scan_result result;

		if (read.info)
		{
			// The size as stored and the orientation apart, as libjxl reports them when told to keep
			// the orientation.
			result.width = read.info->width;
			result.height = read.info->height;
			result.orientation = static_cast<std::uint16_t>(read.info->orientation);
			result.pixel_format = read.info->grey ? "grayscale" : read.info->alpha_bits > 0 ? "rgba" : "rgb";
		}

		if (read.exif)
		{
			result.metadata.exif = std::move(*read.exif);
			strip_exif_offset(result.metadata.exif);
		}

		if (read.xmp) result.metadata.xmp = std::move(*read.xmp);
		if (intent == scan_intent::inspect) result.structure = jxl_structure(r, read);

		result.success = read.info.has_value();
		return result;
	}

	bool jxl_packet(reader& r, std::vector<std::uint8_t>& packet)
	{
		auto read = read_jxl(r, nullptr);

		packet.clear();
		if (!read.complete || read.xmp_unknown) return false;
		if (read.xmp) packet = std::move(*read.xmp);
		return true;
	}

	std::size_t jxl_parts_held(reader& r)
	{
		return read_jxl(r, nullptr).peak_parts;
	}
}
