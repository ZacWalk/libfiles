// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The WebP scanner. The dimensions and pixel format come from the RIFF header and the first
// image chunk, read the way libwebp's WebPGetFeatures reads them. The metadata chunks follow the
// image data, so they are found by stepping from chunk header to chunk header: the image data
// itself is never read, where loading the whole file to hand to libwebp's mux read all of it.

#include "riff_internal.h"
#include "scan_internal.h"

#include <cstring>
#include <format>
#include <string>

namespace libfiles::detail
{
	namespace
	{
		constexpr std::uint32_t tag_size = 4;
		constexpr std::uint32_t chunk_header_size = 8;
		constexpr std::uint32_t riff_header_size = 12;
		constexpr std::uint32_t vp8x_chunk_size = 10;
		constexpr std::uint32_t max_chunk_payload = ~0u - chunk_header_size - 1u;
		constexpr std::uint32_t vp8_frame_header_size = 10;
		constexpr std::uint32_t vp8l_frame_header_size = 5;
		constexpr std::uint8_t vp8l_magic_byte = 0x2f;
		constexpr std::uint32_t alpha_flag = 0x10;
		constexpr std::uint32_t animation_flag = 0x02;

		bool is_tag(const std::span<const std::uint8_t> at, const char (&tag)[5])
		{
			return at.size() >= tag_size && std::memcmp(at.data(), tag, tag_size) == 0;
		}

		using webp_features = webp_header;

		// The steps of libwebp's ParseHeadersInternal for a features query: no output headers, and
		// not all data assumed present, so a VP8X file cut short in its optional chunks still yields
		// the canvas.
		class header_parser
		{
			reader& _r;
			std::uint64_t _pos = 0;
			std::uint64_t _size = 0;

			std::span<const std::uint8_t> peek(const std::size_t n)
			{
				return _r.view(_pos, static_cast<std::size_t>(std::min<std::uint64_t>(n, _size)));
			}

		public:
			explicit header_parser(reader& r) : _r(r), _size(r.size())
			{
			}

			bool parse(webp_features& f)
			{
				if (_size < riff_header_size) return false;

				std::uint64_t riff_size = 0;

				{
					const auto h = peek(riff_header_size);

					if (is_tag(h, "RIFF"))
					{
						if (std::memcmp(h.data() + 8, "WEBP", tag_size) != 0) return false;

						const auto size = load_le32(h.data() + tag_size);

						if (size < tag_size + chunk_header_size) return false;
						if (size > max_chunk_payload) return false;

						riff_size = size;
						_pos += riff_header_size;
						_size -= riff_header_size;
					}
				}

				const auto found_riff = riff_size > 0;
				auto found_vp8x = false;
				auto canvas_width = 0;
				auto canvas_height = 0;
				std::uint32_t flags = 0;

				// VP8X
				{
					if (_size < chunk_header_size) return false;

					const auto h = peek(chunk_header_size);

					if (is_tag(h, "VP8X"))
					{
						if (load_le32(h.data() + tag_size) != vp8x_chunk_size) return false;
						if (_size < chunk_header_size + vp8x_chunk_size) return false;

						const auto x = peek(chunk_header_size + vp8x_chunk_size);
						flags = load_le32(x.data() + 8);
						canvas_width = 1 + static_cast<int>(x[12] | x[13] << 8 | x[14] << 16);
						canvas_height = 1 + static_cast<int>(x[15] | x[16] << 8 | x[17] << 16);

						if (static_cast<std::uint64_t>(canvas_width) * static_cast<std::uint64_t>(canvas_height) >= 1ull << 32)
						{
							return false;
						}

						_pos += chunk_header_size + vp8x_chunk_size;
						_size -= chunk_header_size + vp8x_chunk_size;
						found_vp8x = true;
					}
				}

				if (!found_riff && found_vp8x) return false;

				const auto animation_present = (flags & animation_flag) != 0;
				f.has_alpha = (flags & alpha_flag) != 0;
				f.has_animation = animation_present;
				f.format = 0;

				auto image_width = canvas_width;
				auto image_height = canvas_height;
				auto found_alpha_chunk = false;

				// Not-enough-data answers with the canvas when there is one, and fails otherwise.
				const auto short_of_data = [&]
				{
					if (!found_vp8x) return false;
					f.has_alpha |= found_alpha_chunk;
					f.width = image_width;
					f.height = image_height;
					return true;
				};

				if (found_vp8x && animation_present)
				{
					f.width = image_width;
					f.height = image_height;
					return true;
				}

				if (_size < tag_size) return short_of_data();

				// Optional chunks: skipped where the data began "RIFF" + "VP8X" or with "ALPH".
				if ((found_riff && found_vp8x) || (!found_riff && !found_vp8x && is_tag(peek(tag_size), "ALPH")))
				{
					std::uint32_t total_size = tag_size + chunk_header_size + vp8x_chunk_size;

					for (;;)
					{
						if (_size < chunk_header_size) return short_of_data();

						const auto h = peek(chunk_header_size);
						const auto chunk_size = load_le32(h.data() + tag_size);

						if (chunk_size > max_chunk_payload) return false;

						const auto disk_chunk_size = (chunk_header_size + chunk_size + 1u) & ~1u;
						total_size += disk_chunk_size;

						if (riff_size > 0 && total_size > riff_size) return false;

						if (is_tag(h, "VP8 ") || is_tag(h, "VP8L")) break;

						if (_size < disk_chunk_size) return short_of_data();

						if (is_tag(h, "ALPH")) found_alpha_chunk = true;

						_pos += disk_chunk_size;
						_size -= disk_chunk_size;
					}
				}

				// The VP8 or VP8L chunk header, or a bare bitstream.
				if (_size < chunk_header_size) return short_of_data();

				std::uint64_t compressed_size = 0;
				auto is_lossless = false;

				{
					const auto h = peek(chunk_header_size);
					const auto is_vp8 = is_tag(h, "VP8 ");
					const auto is_vp8l = is_tag(h, "VP8L");
					constexpr std::uint32_t minimal_size = tag_size + chunk_header_size;

					if (is_vp8 || is_vp8l)
					{
						const auto size = load_le32(h.data() + tag_size);
						if (riff_size >= minimal_size && size > riff_size - minimal_size) return false;

						compressed_size = size;
						_pos += chunk_header_size;
						_size -= chunk_header_size;
						is_lossless = is_vp8l;
					}
					else
					{
						const auto b = peek(vp8l_frame_header_size);
						is_lossless = b.size() >= vp8l_frame_header_size && b[0] == vp8l_magic_byte && (b[4] >> 5) == 0;
						compressed_size = _size;
					}
				}

				if (compressed_size > max_chunk_payload) return false;

				if (!animation_present) f.format = is_lossless ? 2 : 1;

				if (!is_lossless)
				{
					if (_size < vp8_frame_header_size) return short_of_data();

					const auto d = peek(vp8_frame_header_size);

					if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) return false;

					const auto bits = static_cast<std::uint32_t>(d[0] | d[1] << 8 | d[2] << 16);
					const auto key_frame = (bits & 1u) == 0;
					const auto w = (d[7] << 8 | d[6]) & 0x3fff;
					const auto h = (d[9] << 8 | d[8]) & 0x3fff;

					if (!key_frame) return false;
					if (((bits >> 1) & 7u) > 3u || ((bits >> 4) & 1u) == 0 || (bits >> 5) >= compressed_size) return false;
					if (w == 0 || h == 0) return false;

					image_width = w;
					image_height = h;
				}
				else
				{
					if (_size < vp8l_frame_header_size) return short_of_data();

					const auto d = peek(vp8l_frame_header_size);

					if (d[0] != vp8l_magic_byte || (d[4] >> 5) != 0) return false;

					const auto bits = load_le32(d.data() + 1);
					image_width = static_cast<int>(bits & 0x3fff) + 1;
					image_height = static_cast<int>((bits >> 14) & 0x3fff) + 1;
					f.has_alpha = ((bits >> 28) & 1u) != 0;
				}

				if (found_vp8x && (canvas_width != image_width || canvas_height != image_height)) return false;

				f.has_alpha |= found_alpha_chunk;
				f.width = image_width;
				f.height = image_height;
				return true;
			}
		};

		std::string_view chunk_description(const std::uint32_t id)
		{
			switch (id)
			{
			case riff::vp8_id: return "lossy image data";
			case riff::vp8l_id: return "lossless image data";
			case riff::vp8x_id: return "extended format header";
			case riff::alph_id: return "alpha channel";
			case riff::anim_id: return "animation parameters";
			case riff::anmf_id: return "animation frame";
			case riff::iccp_id: return "ICC colour profile";
			case riff::exif_id: return "Exif metadata";
			case riff::xmp_id: return "XMP metadata";
			default: return {};
			}
		}

		// A RIFF chunk inventory. WebP hides quite a lot behind its feature flags - alpha, animation,
		// colour profile - and the chunk list is the plain statement of what the file holds.
		std::vector<structure_row> webp_structure(reader& r)
		{
			std::vector<structure_row> kv;
			const auto file_size = r.size();

			add_structure_section(kv, "Summary", "webp.summary", true);
			add_structure_row(kv, "File size", std::format("{} bytes", file_size));
			add_structure_row(kv, "RIFF size", std::format("{} bytes", r.le32(4) + 8u));

			add_structure_section(kv, "Chunks", "webp.chunks");

			std::string_view simple_format;

			riff::for_each_chunk(r, riff_header_size, file_size, nullptr, [&](const riff::chunk& c)
			{
				if (!riff::fits(c, file_size)) return false;

				const auto description = chunk_description(c.id);

				if (simple_format.empty() && (c.id == riff::vp8_id || c.id == riff::vp8l_id))
				{
					simple_format = c.id == riff::vp8l_id ? "lossless (VP8L)" : "lossy (VP8)";
				}

				auto value = std::format("offset {}, {} bytes", c.start, c.size);
				if (!description.empty()) value = std::format("{}, {}", description, value);

				// A FourCC is four bytes, and a NUL among them ends the name as the C string it was.
				const std::string name{
					static_cast<char>(c.id), static_cast<char>(c.id >> 8), static_cast<char>(c.id >> 16), static_cast<char>(c.id >> 24)
				};
				auto& row = add_row(kv, name.substr(0, name.find('\0')), std::move(value));
				row.depth = 1;
				row.id = std::format("webp.chunk.{}", c.start);
				return true;
			});

			if (!simple_format.empty())
			{
				// Reported next to the summary rather than buried in the chunk list.
				structure_row compression;
				compression.key = "Compression";
				compression.value = simple_format;
				compression.depth = 1;
				kv.insert(kv.begin() + 1, std::move(compression));
			}

			finish_structure_sections(kv);
			return kv;
		}
	}

	bool webp_canvas(reader& r, std::uint32_t& width, std::uint32_t& height, bool& has_alpha)
	{
		webp_features features;
		if (!header_parser(r).parse(features) || features.width <= 0 || features.height <= 0) return false;

		width = static_cast<std::uint32_t>(features.width);
		height = static_cast<std::uint32_t>(features.height);
		has_alpha = features.has_alpha;
		return true;
	}

	scan_result scan_webp(reader& r, const scan_intent intent)
	{
		scan_result result;
		webp_features features;

		if (header_parser(r).parse(features))
		{
			result.width = static_cast<std::uint32_t>(features.width);
			result.height = static_cast<std::uint32_t>(features.height);

			const auto lossless = features.format == 2;

			// Lossless WebP stores RGB(A) directly; lossy WebP is always 4:2:0, with any alpha riding
			// alongside in its own plane.
			result.pixel_format = lossless
				                      ? (features.has_alpha ? "rgba" : "rgb")
				                      : (features.has_alpha ? "yuva420" : "yuv420");
			result.picture = {lossless ? chroma_format::rgb : chroma_format::yuv420, 8, features.has_alpha};

			// The metadata chunks, by their headers. A writer may emit a chunk the VP8X flags do not
			// declare; it is read all the same, where libwebp's mux refused the whole file.
			const auto riff_end = std::min<std::uint64_t>(r.size(), static_cast<std::uint64_t>(r.le32(4)) + 8u);
			auto found_icc = false;
			auto found_exif = false;
			auto found_xmp = false;

			riff::for_each_chunk(r, riff_header_size, riff_end, nullptr, [&](const riff::chunk& c)
			{
				if (!riff::fits(c, riff_end)) return false;

				const auto size = static_cast<std::size_t>(c.size);

				if (!found_icc && c.id == riff::iccp_id)
				{
					result.metadata.icc = r.read(c.payload(), size);
					found_icc = true;
				}
				else if (!found_exif && c.id == riff::exif_id)
				{
					auto exif = r.read(c.payload(), size);

					// Some writers keep the JPEG "Exif\0\0" signature; only that leading signature is
					// trimmed.
					if (starts_with(exif, {"Exif\0\0", 6})) exif.erase(exif.begin(), exif.begin() + 6);

					result.metadata.exif = std::move(exif);
					found_exif = true;
				}
				else if (!found_xmp && c.id == riff::xmp_id)
				{
					result.metadata.xmp = r.read(c.payload(), size);
					found_xmp = true;
				}

				return true;
			});
		}

		if (intent == scan_intent::inspect && r.size() >= 12u && r.le32(0) == riff::riff_id && r.le32(8) == riff::webp_type)
		{
			result.structure = webp_structure(r);
		}

		result.success = result.width != 0 && result.height != 0;
		return result;
	}
}

namespace libfiles
{
	std::optional<webp_header> probe_webp_header(reader& input)
	{
		webp_header result;
		try
		{
			if (detail::header_parser(input).parse(result) && result.width > 0 && result.height > 0)
				return result;
		}
		catch (const read_error&)
		{
			return std::nullopt;
		}
		return std::nullopt;
	}
}
