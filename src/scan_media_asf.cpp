// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Scanning ASF files - WMV, WMA and ASF - from their header object: the File Properties'
// duration and date; each stream's properties, with the WAVEFORMATEX or BITMAPINFOHEADER they hold; the
// frame rate, name and language the extended stream properties give a stream, and its bit rate; and the
// tags of the Content Description, the Extended Content Description and the Metadata and Metadata Library
// objects, a WM/Picture located rather than read. The data object's packets are never read. The packet is
// the one the writer reads, in the first XMP object after the data.

#include "asf_internal.h"
#include "media_internal.h"
#include "vocabulary_internal.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace libfiles::detail
{
	namespace
	{
		// The most of a text value read; a longer one is cut there.
		constexpr std::uint64_t max_text = 64 * 1024;
		// The most of a stream's type-specific data read: its WAVEFORMATEX or BITMAPINFOHEADER, and the
		// codec configuration after it.
		constexpr std::uint64_t max_type_specific = 1024 * 1024;
		// The most a WM/Picture's MIME type and description may take before its image.
		constexpr std::uint64_t max_picture_strings = 64 * 1024;
		// FILETIME counts 100 ns from 1601; this many seconds lie between then and 1970.
		constexpr std::int64_t filetime_to_unix = 11'644'473'600;
		// The last stream number ASF has, in seven bits.
		constexpr std::size_t max_stream = 127;

		struct object_name
		{
			const asf::guid* id;
			std::string_view name;
		};

		constexpr object_name object_names[] = {
			{&asf::header_id, "Header"},
			{&asf::data_id, "Data"},
			{&asf::simple_index_id, "Simple Index"},
			{&asf::index_id, "Index"},
			{&asf::media_object_index_id, "Media Object Index"},
			{&asf::timecode_index_id, "Timecode Index"},
			{&asf::xmp_id, "XMP"},
			{&asf::file_properties_id, "File Properties"},
			{&asf::stream_properties_id, "Stream Properties"},
			{&asf::header_extension_id, "Header Extension"},
			{&asf::codec_list_id, "Codec List"},
			{&asf::script_command_id, "Script Command"},
			{&asf::marker_id, "Marker"},
			{&asf::bitrate_mutual_exclusion_id, "Bitrate Mutual Exclusion"},
			{&asf::error_correction_id, "Error Correction"},
			{&asf::content_description_id, "Content Description"},
			{&asf::extended_content_id, "Extended Content Description"},
			{&asf::content_branding_id, "Content Branding"},
			{&asf::stream_bitrate_id, "Stream Bitrate Properties"},
			{&asf::content_encryption_id, "Content Encryption"},
			{&asf::extended_content_encryption_id, "Extended Content Encryption"},
			{&asf::digital_signature_id, "Digital Signature"},
			{&asf::padding_id, "Padding"},
			{&asf::extended_stream_properties_id, "Extended Stream Properties"},
			{&asf::advanced_mutual_exclusion_id, "Advanced Mutual Exclusion"},
			{&asf::group_mutual_exclusion_id, "Group Mutual Exclusion"},
			{&asf::stream_prioritization_id, "Stream Prioritization"},
			{&asf::bandwidth_sharing_id, "Bandwidth Sharing"},
			{&asf::language_list_id, "Language List"},
			{&asf::metadata_id, "Metadata"},
			{&asf::metadata_library_id, "Metadata Library"},
			{&asf::index_parameters_id, "Index Parameters"},
			{&asf::media_object_index_parameters_id, "Media Object Index Parameters"},
			{&asf::timecode_index_parameters_id, "Timecode Index Parameters"},
			{&asf::compatibility_id, "Compatibility"},
			{&asf::advanced_content_encryption_id, "Advanced Content Encryption"},
		};

		// "{75B22630-668E-11CF-A6D9-00AA0062CE6C}": the first three fields are stored little-endian.
		std::string guid_text(const std::span<const std::uint8_t> g)
		{
			return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}", load_le32(g.data()),
			                   load_le16(g.data() + 4), load_le16(g.data() + 6), unsigned{g[8]}, unsigned{g[9]}, unsigned{g[10]},
			                   unsigned{g[11]}, unsigned{g[12]}, unsigned{g[13]}, unsigned{g[14]}, unsigned{g[15]});
		}

		std::string name_of(const asf::guid& id)
		{
			for (const auto& n : object_names)
			{
				if (*n.id == id) return std::string(n.name);
			}

			return guid_text(id);
		}

		std::string utf16_text(reader& r, const std::uint64_t at, const std::uint64_t length)
		{
			return text_from_utf16(r.view(at, static_cast<std::size_t>(std::min(length, max_text))), false);
		}

		// Bytes described in place of their value, in the words the other tag readers use: "7 bytes", and
		// "image/jpeg, front cover, 51234 bytes" for a picture.
		std::string byte_count(const std::uint64_t count)
		{
			return std::to_string(count) + (count == 1 ? " byte" : " bytes");
		}

		std::string describe_picture(const media_picture& p)
		{
			// The ID3 picture types, which WM/Picture shares.
			static constexpr std::string_view types[] = {
				"other", "file icon", "other file icon", "front cover", "back cover", "leaflet page", "media",
				"lead artist", "artist", "conductor", "band", "composer", "lyricist", "recording location",
				"during recording", "during performance", "video capture", "bright coloured fish", "illustration",
				"band logo", "publisher logo",
			};

			std::string text = p.mime_type;
			if (!text.empty()) text += ", ";
			text += types[static_cast<std::size_t>(p.type)];
			return text + ", " + byte_count(p.location.size);
		}

		// What the header says of a stream outside its Stream Properties object.
		struct stream_extras
		{
			double frame_rate = 0.0;
			std::uint64_t data_bitrate = 0;
			std::uint64_t average_bitrate = 0;
			std::optional<std::uint16_t> language;
			// A Metadata or Metadata Library record's WM/Language for the stream, the last that gives one: the
			// stream's language when the Language List gives it none, as FFmpeg reads it.
			std::string language_record;
			std::string name;
			std::vector<media_tag> tags;
			std::uint64_t aspect_x = 0;
			std::uint64_t aspect_y = 0;
		};

		class header_reader
		{
		public:
			header_reader(reader& r, const scan_intent intent, media_scan_result& out) :
				_r(r),
				_inspect(intent == scan_intent::inspect),
				_out(out)
			{
			}

			void read(const std::uint64_t header_size)
			{
				const auto stop = asf::walk_objects(_r, asf::header_fixed, header_size, [this](const asf::object& o) { child(o, 1); });
				if (stop != header_size) damaged(stop, header_size, 1);
				if (_out.streams.empty()) throw format_error("the ASF header describes no streams");

				finish_streams();
				finish_file();
			}

			// For the structure report: the File Properties, and a row for each object the header holds.
			std::vector<structure_row> summary_rows;
			std::vector<structure_row> object_rows;
			// By their index in the result, the streams declared inside an Extended Stream Properties
			// object, and those whose content is encrypted.
			std::vector<std::size_t> hidden;
			std::vector<std::size_t> encrypted;

		private:
			void child(const asf::object& o, const int depth)
			{
				if (_inspect) object_row(o, depth);

				if (o.id == asf::file_properties_id) file_properties(o);
				else if (o.id == asf::stream_properties_id) stream_properties(o, depth > 1);
				else if (o.id == asf::header_extension_id && depth == 1) header_extension(o);
				else if (o.id == asf::content_description_id) content_description(o);
				else if (o.id == asf::extended_content_id) extended_content(o);
				else if (o.id == asf::stream_bitrate_id) stream_bitrates(o);
				else if (o.id == asf::extended_stream_properties_id) extended_stream_properties(o, depth);
				else if (o.id == asf::language_list_id) language_list(o);
				else if (o.id == asf::metadata_id || o.id == asf::metadata_library_id) metadata(o);
			}

			void object_row(const asf::object& o, const int depth)
			{
				auto& row = add_row(object_rows, name_of(o.id), std::format("offset {}, {} bytes", o.start, o.size));
				row.depth = depth;
				row.id = std::format("asf.object.{}", o.start);
			}

			void damaged(const std::uint64_t at, const std::uint64_t end, const int depth)
			{
				if (!_inspect) return;

				auto& row = add_row(object_rows, "Unreadable", std::format("offset {}, {} bytes", at, end - at));
				row.depth = depth;
			}

			void file_properties(const asf::object& o)
			{
				if (_properties || o.size < asf::file_properties_size) return;
				_properties = _r.read(o.start, asf::file_properties_size);
			}

			void stream_properties(const asf::object& o, const bool embedded)
			{
				if (o.size < asf::stream_properties_fixed) return;

				asf::guid type;
				std::uint64_t specific_length = 0;
				std::uint16_t flags = 0;

				{
					const auto h = _r.view(o.start, asf::stream_properties_fixed);
					std::copy_n(h.begin() + 24, type.size(), type.begin());
					specific_length = load_le32(h.data() + 64);
					flags = load_le16(h.data() + 72);
				}

				const auto available = o.size - asf::stream_properties_fixed;
				const auto specific = _r.read(o.start + asf::stream_properties_fixed,
				                              static_cast<std::size_t>(std::min({specific_length, available, max_type_specific})));

				media_stream s;
				s.id = flags & 0x7Fu;

				if (type == asf::audio_media_id)
				{
					s.kind = media_stream_kind::audio;
					std::uint32_t bytes_per_second = 0;
					read_wave_format(specific, s, &bytes_per_second);
					if (s.bit_rate == 0) s.bit_rate = std::uint64_t{bytes_per_second} * 8;
				}
				else if (type == asf::video_media_id)
				{
					s.kind = media_stream_kind::video;
					read_video_format(specific, s);
				}
				else
				{
					// Command, JFIF, binary and file transfer streams: script commands, slides and opaque data.
					s.kind = media_stream_kind::data;
					if (type == asf::jfif_media_id || type == asf::degradable_jpeg_media_id) s.codec = "mjpeg";
				}

				if (embedded) hidden.push_back(_out.streams.size());
				if (flags & 0x8000) encrypted.push_back(_out.streams.size());
				_out.streams.push_back(std::move(s));
			}

			// The encoded picture's size, a reserved byte, the size of the format data, and the format data:
			// a BITMAPINFOHEADER and what the codec keeps after it. The BITMAPINFOHEADER's size is the one
			// FFmpeg reads; the encoded size stands in when it gives none.
			static void read_video_format(const std::span<const std::uint8_t> data, media_stream& s)
			{
				std::uint32_t encoded_width = 0;
				std::uint32_t encoded_height = 0;

				if (data.size() >= 8 && load_le32(data.data()) <= max_image_dimension && load_le32(data.data() + 4) <= max_image_dimension)
				{
					encoded_width = load_le32(data.data());
					encoded_height = load_le32(data.data() + 4);
				}

				if (data.size() >= 11)
				{
					const auto format = data.subspan(11, std::min<std::size_t>(load_le16(data.data() + 9), data.size() - 11));
					read_bitmap_info(format, s);
					read_codec_configuration(format, s);
				}

				if (s.video.width == 0 || s.video.height == 0)
				{
					s.video.width = encoded_width;
					s.video.height = encoded_height;
				}
			}

			// The bytes after a BITMAPINFOHEADER's 40, for the codecs that keep their configuration there.
			static void read_codec_configuration(const std::span<const std::uint8_t> format, media_stream& s)
			{
				if (format.size() <= 40) return;

				const auto declared = load_le32(format.data());
				if (declared <= 40) return;

				const auto extra = format.subspan(40, static_cast<std::size_t>(std::min<std::uint64_t>(declared - 40, format.size() - 40)));
				video_header header;
				auto found = false;

				if (s.codec == "h264" || s.codec == "hevc")
				{
					const auto hevc = s.codec == "hevc";

					if (extra[0] == 1)
					{
						found = hevc ? parse_hevc_config(extra, header) : parse_avc_config(extra, header);
					}
					else
					{
						for_each_annexb_nal(extra, [&](const std::span<const std::uint8_t> nal)
						{
							if (nal.empty()) return true;

							const auto type = hevc ? nal[0] >> 1 & 0x3F : nal[0] & 0x1F;
							if (type != (hevc ? 33 : 7)) return true;

							found = hevc ? parse_hevc_sps(nal, header) : parse_h264_sps(nal, header);
							return false;
						});
					}
				}
				else if (s.codec == "mpeg4")
				{
					found = parse_mpeg4_video(extra, header);
				}

				if (found) apply_video_header(header, s.video);
			}

			void header_extension(const asf::object& o)
			{
				if (o.size < asf::extension_fixed) return;

				const auto begin = o.start + asf::extension_fixed;
				const auto stop = asf::walk_objects(_r, begin, o.end(), [this](const asf::object& c) { child(c, 2); });
				if (stop != o.end()) damaged(stop, o.end(), 2);
			}

			void content_description(const asf::object& o)
			{
				std::array<byte_range, 5> fields;
				const auto count = asf::content_description_fields(_r, o.start + asf::object_header, o.end(), fields);

				for (std::size_t i = 0; i < count; ++i)
				{
					add_tag(_out, tag_family::asf, vocabulary::asf_content_description[i], utf16_text(_r, fields[i].offset, fields[i].size));
				}
			}

			void extended_content(const asf::object& o)
			{
				asf::walk_content_descriptors(_r, o.start + asf::object_header, o.end(),
				                              [this](const asf::descriptor& d) { descriptor(d); }, true);
			}

			void metadata(const asf::object& o)
			{
				asf::walk_metadata_records(_r, o.start + asf::object_header, o.end(), [this](const asf::descriptor& d) { descriptor(d); });
			}

			std::uint64_t number(const asf::descriptor& d) const
			{
				const auto size = d.type == asf::qword_value ? 8u : d.type == asf::word_value ? 2u : 4u;
				const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(d.value_length, size));
				const auto bytes = _r.view(d.value_at, n);

				std::uint64_t v = 0;
				for (auto i = n; i-- > 0;) v = v << 8 | bytes[i];
				return v;
			}

			static bool numeric(const asf::descriptor& d)
			{
				return d.type >= asf::bool_value && d.type <= asf::word_value && d.value_length != 0;
			}

			// A value as text: text as it is, numbers in decimal and a GUID in its registry form. Empty for
			// bytes, which are described instead, and for a type the specification does not define.
			std::string value_text(const asf::descriptor& d) const
			{
				switch (d.type)
				{
				case asf::unicode_value:
					return utf16_text(_r, d.value_at, d.value_length);
				case asf::bool_value:
				case asf::dword_value:
				case asf::qword_value:
				case asf::word_value:
					return d.value_length != 0 ? std::to_string(number(d)) : std::string();
				case asf::guid_value:
					return d.value_length >= 16 ? guid_text(_r.view(d.value_at, 16)) : std::string();
				default:
					return {};
				}
			}

			// A descriptor or record: a tag of the file or, for a record of a stream, of that stream. Bytes
			// are described, and so give no property its value.
			void descriptor(const asf::descriptor& d)
			{
				const auto name = std::string(trim_text(utf16_text(_r, d.name_at, d.name_length)));
				if (name.empty()) return;

				std::string text;
				const auto described = d.type == asf::byte_array_value;

				if (described)
				{
					if (d.value_length == 0) return;

					auto p = name == "WM/Picture" ? picture(d) : std::nullopt;
					text = p ? describe_picture(*p) : byte_count(d.value_length);
					if (p) _out.pictures.push_back(std::move(*p));
				}
				else
				{
					// The pixel aspect ratio, for the stream the record names or, given for the file, for each
					// video stream that does not give its own.
					if ((name == "AspectRatioX" || name == "AspectRatioY") && numeric(d) && d.stream <= max_stream)
					{
						auto& x = d.stream == 0 ? _file_aspect : _streams[d.stream];
						(name == "AspectRatioX" ? x.aspect_x : x.aspect_y) = number(d);
					}

					text = value_text(d);
				}

				if (d.stream > max_stream) return;

				if (d.stream == 0 && !described)
				{
					add_tag(_out, tag_family::asf, name, text);
					return;
				}

				auto& tags = d.stream == 0 ? _out.tags : _streams[d.stream].tags;
				const auto trimmed = trim_text(text);
				if (!trimmed.empty()) tags.push_back({name, std::string(trimmed), {}});
				if (d.stream != 0 && d.type == asf::unicode_value && !trimmed.empty() && language_name(name))
				{
					_streams[d.stream].language_record = trimmed;
				}
			}

			// FFmpeg maps names to its own case-insensitively.
			static bool language_name(const std::string_view name)
			{
				constexpr std::string_view wanted = "wm/language";
				return name.size() == wanted.size() && std::equal(name.begin(), name.end(), wanted.begin(), [](const char a, const char b)
				{
					return (a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : a) == b;
				});
			}

			// WM/Picture: the picture type, the image's size, the MIME type and the description - each a
			// NUL-terminated UTF-16 string - and then the image, which is located rather than read.
			std::optional<media_picture> picture(const asf::descriptor& d)
			{
				if (d.value_length < 9) return std::nullopt;

				std::uint8_t type = 0;
				std::uint64_t image_size = 0;

				{
					const auto head = _r.view(d.value_at, 5);
					type = head[0];
					image_size = load_le32(head.data() + 1);
				}

				if (image_size == 0 || image_size > d.value_length - 9) return std::nullopt;

				const auto strings_length = d.value_length - 5 - image_size;
				if (strings_length > max_picture_strings) return std::nullopt;

				media_picture p;

				{
					const auto strings = _r.view(d.value_at + 5, static_cast<std::size_t>(strings_length));
					std::size_t mime_end = 0;
					while (mime_end + 1 < strings.size() && (strings[mime_end] | strings[mime_end + 1]) != 0) mime_end += 2;
					if (mime_end + 1 >= strings.size()) return std::nullopt;

					p.mime_type = std::string(trim_text(text_from_utf16(strings.first(mime_end), false)));
					p.description = std::string(trim_text(text_from_utf16(strings.subspan(mime_end + 2), false)));
				}

				// The types are ID3's; FFmpeg takes any other for "Other", as this does.
				p.type = type <= 20 ? type : 0;
				p.location = {d.value_at + 5 + strings_length, image_size};
				return p;
			}

			// A stream's leaky bucket, language, frame time and names, then the systems that extend its
			// payloads, and last, for a stream the header lists nowhere else, its Stream Properties object.
			void extended_stream_properties(const asf::object& o, const int depth)
			{
				constexpr std::size_t fixed = 88;
				if (o.size < fixed) return;

				std::uint32_t data_bitrate = 0;
				std::uint16_t number = 0;
				std::uint16_t language = 0;
				std::uint64_t frame_time = 0;
				unsigned name_count = 0;
				unsigned extension_count = 0;

				{
					const auto h = _r.view(o.start, fixed);
					data_bitrate = load_le32(h.data() + 40);
					number = load_le16(h.data() + 72);
					language = load_le16(h.data() + 74);
					frame_time = asf::load_le64(h.data() + 76);
					name_count = load_le16(h.data() + 84);
					extension_count = load_le16(h.data() + 86);
				}

				stream_extras ignored;
				auto& x = number >= 1 && number <= max_stream ? _streams[number] : ignored;
				x.data_bitrate = data_bitrate;
				x.language = language;

				// In 100 ns units. A frame time under 10 microseconds is no frame rate.
				if (frame_time >= 100) x.frame_rate = 10'000'000.0 / static_cast<double>(frame_time);

				const auto end = o.end();
				auto pos = o.start + fixed;

				for (unsigned i = 0; i < name_count; ++i)
				{
					if (end - pos < 4) return;
					const std::uint64_t length = _r.le16(pos + 2);
					if (end - pos - 4 < length) return;

					if (x.name.empty()) x.name = std::string(trim_text(utf16_text(_r, pos + 4, length)));
					pos += 4 + length;
				}

				for (unsigned i = 0; i < extension_count; ++i)
				{
					if (end - pos < 22) return;
					const std::uint64_t length = _r.le32(pos + 18);
					if (end - pos - 22 < length) return;
					pos += 22 + length;
				}

				asf::walk_objects(_r, pos, end, [&](const asf::object& c)
				{
					if (_inspect) object_row(c, depth + 1);
					if (c.id == asf::stream_properties_id) stream_properties(c, true);
				});
			}

			void stream_bitrates(const asf::object& o)
			{
				if (o.size < asf::object_header + 2) return;

				const auto count = _r.le16(o.start + asf::object_header);
				auto pos = o.start + asf::object_header + 2;

				for (unsigned i = 0; i < count && o.end() - pos >= 6; ++i, pos += 6)
				{
					const auto number = _r.le16(pos) & 0x7Fu;
					if (number != 0) _streams[number].average_bitrate = _r.le32(pos + 2);
				}
			}

			void language_list(const asf::object& o)
			{
				if (!_languages.empty() || o.size < asf::object_header + 2) return;

				const auto count = _r.le16(o.start + asf::object_header);
				auto pos = o.start + asf::object_header + 2;

				for (unsigned i = 0; i < count && pos < o.end(); ++i)
				{
					const std::uint64_t length = _r.u8(pos);
					if (o.end() - pos - 1 < length) return;

					add_language(trim_text(utf16_text(_r, pos + 1, length)));
					pos += 1 + length;
				}
			}

			void finish_streams()
			{
				std::array<bool, max_stream + 1> claimed{};

				for (auto& s : _out.streams)
				{
					if (s.id == 0 || s.id > max_stream) continue;

					const auto number = static_cast<std::size_t>(s.id);
					if (claimed[number]) continue;

					claimed[number] = true;
					auto& x = _streams[number];

					if (s.kind == media_stream_kind::video)
					{
						if (!(s.video.frame_rate > 0)) s.video.frame_rate = x.frame_rate;

						const auto& aspect = x.aspect_x != 0 && x.aspect_y != 0 ? x : _file_aspect;

						if (aspect.aspect_x != 0 && aspect.aspect_y != 0)
						{
							const auto divisor = std::gcd(aspect.aspect_x, aspect.aspect_y);
							const auto width = aspect.aspect_x / divisor;
							const auto height = aspect.aspect_y / divisor;

							if (width <= UINT32_MAX && height <= UINT32_MAX)
							{
								s.video.pixel_aspect_width = static_cast<std::uint32_t>(width);
								s.video.pixel_aspect_height = static_cast<std::uint32_t>(height);
							}
						}
					}

					if (s.bit_rate == 0) s.bit_rate = x.average_bitrate != 0 ? x.average_bitrate : x.data_bitrate;
					if (x.language && *x.language < _languages.size()) s.language = _languages[*x.language];
					if (s.language.empty()) s.language = known_language(x.language_record);
					if (!x.name.empty()) s.title = x.name;
					s.tags.insert(s.tags.end(), std::make_move_iterator(x.tags.begin()), std::make_move_iterator(x.tags.end()));
				}
			}

			// The Language List's tags as the file keeps them, "en-us", but for "und", which names none.
			void add_language(const std::string_view language)
			{
				_languages.emplace_back(known_language(language));
			}

			static std::string_view known_language(const std::string_view language)
			{
				const auto undetermined = language.size() == 3 && (language[0] | 0x20) == 'u' && (language[1] | 0x20) == 'n' &&
					(language[2] | 0x20) == 'd';
				return undetermined ? std::string_view() : language;
			}

			// What the File Properties say of the whole file. A broadcast file is still being written, and
			// its duration and date are not yet true.
			void finish_file()
			{
				if (!_properties) return;

				const auto& p = *_properties;
				const auto broadcast = asf::broadcast(p);
				const auto created = asf::load_le64(p.data() + asf::creation_date_at);
				const auto play = asf::load_le64(p.data() + asf::play_duration_at);
				const auto preroll = asf::load_le64(p.data() + asf::preroll_at);

				if (_inspect)
				{
					const auto flags = load_le32(p.data() + asf::flags_at);
					std::string flag_text = broadcast ? "broadcast" : "";
					if (flags & 2) flag_text += flag_text.empty() ? "seekable" : ", seekable";

					const auto min_packet = load_le32(p.data() + asf::min_packet_size_at);
					const auto max_packet = load_le32(p.data() + asf::max_packet_size_at);
					const auto max_bitrate = load_le32(p.data() + asf::max_bitrate_at);

					add_structure_row(summary_rows, "File size", std::format("{} bytes", asf::load_le64(p.data() + asf::file_size_at)));
					add_structure_row(summary_rows, "Play duration", std::format("{:.3f} s", static_cast<double>(play) / 1e7));
					add_structure_row(summary_rows, "Preroll", std::format("{} ms", preroll));
					add_structure_row(summary_rows, "Flags", flag_text.empty() ? std::string("none") : flag_text);
					add_structure_row(summary_rows, "Data packets", std::to_string(asf::load_le64(p.data() + asf::data_packets_at)));
					add_structure_row(summary_rows, "Packet size", std::format("{} to {} bytes", min_packet, max_packet));
					add_structure_row(summary_rows, "Maximum bit rate", std::format("{} bits a second", max_bitrate));
				}

				if (broadcast) return;

				const auto seconds = static_cast<double>(play) / 1e7 - static_cast<double>(preroll) / 1e3;

				if (seconds > 0)
				{
					_out.duration = seconds;
					for (auto& s : _out.streams) s.duration = seconds;
				}

				// Tags that say when the recording was made come before the file's own date.
				if (created != 0 && !_out.properties.created.valid())
				{
					_out.properties.created = utc_date_from_seconds(static_cast<std::int64_t>(created / 10'000'000) - filetime_to_unix);
				}
			}

			reader& _r;
			bool _inspect = false;
			media_scan_result& _out;
			std::optional<std::vector<std::uint8_t>> _properties;
			std::vector<stream_extras> _streams = std::vector<stream_extras>(max_stream + 1);
			stream_extras _file_aspect;
			std::vector<std::string> _languages;
		};

		std::string stream_text(const media_stream& s)
		{
			std::string text;

			switch (s.kind)
			{
			case media_stream_kind::video: text = "video";
				break;
			case media_stream_kind::audio: text = "audio";
				break;
			case media_stream_kind::subtitle: text = "subtitle";
				break;
			case media_stream_kind::data: text = "data";
				break;
			}

			if (!s.codec.empty()) text += ", " + s.codec;
			if (!s.codec_tag.empty()) text += " (" + s.codec_tag + ")";
			if (s.kind == media_stream_kind::video && s.video.width != 0) text += std::format(", {} x {}", s.video.width, s.video.height);
			if (s.kind == media_stream_kind::audio && s.audio.sample_rate != 0) text += std::format(", {} Hz", s.audio.sample_rate);
			if (s.kind == media_stream_kind::audio && s.audio.channels != 0) text += std::format(", {} channels", s.audio.channels);
			if (s.bit_rate != 0) text += std::format(", {} bits a second", s.bit_rate);
			return text;
		}

		std::vector<structure_row> structure(reader& r, const header_reader& h, const media_scan_result& out)
		{
			std::vector<structure_row> rows;

			add_structure_section(rows, "Summary", "asf.summary", true);
			rows.insert(rows.end(), h.summary_rows.begin(), h.summary_rows.end());

			add_structure_section(rows, "Objects", "asf.objects", true);
			const auto size = r.size();
			const auto stop = asf::walk_objects(r, 0, size, [&](const asf::object& o)
			{
				auto& row = add_row(rows, name_of(o.id), std::format("offset {}, {} bytes", o.start, o.size));
				row.depth = 1;
				row.id = std::format("asf.top.{}", o.start);
			});

			if (stop != size) add_structure_row(rows, "Unreadable", std::format("offset {}, {} bytes", stop, size - stop));

			add_structure_section(rows, "Header", "asf.header");
			rows.insert(rows.end(), h.object_rows.begin(), h.object_rows.end());

			add_structure_section(rows, "Streams", "asf.streams", true);

			for (std::size_t i = 0; i < out.streams.size(); ++i)
			{
				const auto& s = out.streams[i];
				auto text = stream_text(s);
				if (std::ranges::find(h.hidden, i) != h.hidden.end()) text += ", hidden";
				if (std::ranges::find(h.encrypted, i) != h.encrypted.end()) text += ", encrypted";
				add_structure_row(rows, std::format("Stream {}", s.id), std::move(text));
			}

			finish_structure_sections(rows);
			return rows;
		}
	}

	void scan_asf_media(reader& r, const scan_intent intent, media_scan_result& out)
	{
		out.brand = "ASF";

		if (r.size() < asf::header_fixed) throw format_error("the ASF header is cut short");

		const auto header_size = r.le64(16);
		if (header_size < asf::header_fixed) throw format_error("the ASF header is damaged");
		if (header_size > r.size()) throw format_error("the ASF header is cut short");

		header_reader h(r, intent, out);
		h.read(header_size);

		// The packet the writer reads and would replace: the first XMP object after the data, or none when
		// the file's objects do not let it read one. The header object is not read again.
		try
		{
			asf::object header;
			header.id = asf::header_id;
			header.size = header_size;

			const auto l = asf::read_layout(r, header);

			if (const auto* const x = l.xmp())
			{
				const auto packet = r.read(x->start + asf::object_header, static_cast<std::size_t>(x->size - asf::object_header));
				out.xmp.assign(packet.begin(), packet.end());
			}
		}
		catch (const std::exception&)
		{
			out.xmp.clear();
		}

		if (intent == scan_intent::inspect) out.structure = structure(r, h, out);
	}
}
