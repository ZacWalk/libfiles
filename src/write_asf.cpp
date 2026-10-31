// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Writing metadata to ASF files - WMV, WMA and ASF. The packet lives where the toolkit kept it,
// in a top-level XMP object after the data. Title, the artist as author, copyright and the comment as
// description go to the header's Content Description, where Windows, FFmpeg and the taggers that write WMA
// read them; tags and rating to the WM/Category and WM/SharedUserRating descriptors Windows reads, and a
// created date to the File Properties.
//
// A packet that fits is patched where it lies; one that does not is appended at the end of the file,
// and only then does the old object become padding. The header is rewritten where it lies, at the size
// it has, when its padding can take the change - one write, so the file never holds half of it.
// Otherwise the file is rewritten in one pass with a larger header, given room for the next edit.
// Readers take the first XMP object after the data; an update turns any others into
// padding.

#include "asf_internal.h"
#include "legacy.h"
#include "vocabulary_internal.h"

#include <algorithm>
#include <array>

namespace libfiles::detail
{
	namespace
	{
		using asf::content_description_id;
		using asf::extended_content_id;
		using asf::extension_fixed;
		using asf::file_properties_id;
		using asf::guid;
		using asf::header_extension_id;
		using asf::header_fixed;
		using asf::header_id;
		using asf::load_le64;
		using asf::object;
		using asf::object_header;
		using asf::padding_id;
		using asf::read_layout;
		using asf::xmp_id;

		// The padding a header is given when the file is rewritten, for later edits to grow into.
		constexpr std::uint64_t header_room = 2048;
		// The only step after what is appended: what makes it the one read, and the header.
		constexpr std::uint8_t commit = 0;

		void store_le64(std::uint8_t* const p, const std::uint64_t v)
		{
			for (auto i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
		}

		void store_le32(std::uint8_t* const p, const std::uint32_t v)
		{
			for (auto i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
		}

		void append_le16(bytes& to, const std::uint16_t v)
		{
			to.push_back(static_cast<std::uint8_t>(v));
			to.push_back(static_cast<std::uint8_t>(v >> 8));
		}

		void append_le64(bytes& to, const std::uint64_t v)
		{
			for (auto i = 0; i < 8; ++i) to.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
		}

		// An object: its GUID and size, then the payload.
		bytes make_object(const guid& id, const std::span<const std::uint8_t> payload)
		{
			bytes out(id.begin(), id.end());
			append_le64(out, object_header + payload.size());
			out.insert(out.end(), payload.begin(), payload.end());
			return out;
		}

		// A header object held whole, header included, so what is not edited is written back byte for byte.
		struct child
		{
			guid id{};
			bytes raw;
			// Where it lay in the header the file holds.
			std::size_t at = 0;
		};

		// The objects in a payload, each whole; a damaged one leaves the header unedited.
		std::vector<child> parse_objects(const std::span<const std::uint8_t> payload, const std::size_t base)
		{
			std::vector<child> out;
			reader r(payload);

			const auto stop = asf::walk_objects(r, 0, payload.size(), [&](const object& o)
			{
				const auto start = static_cast<std::size_t>(o.start);
				child c;
				c.id = o.id;
				c.raw.assign(payload.begin() + static_cast<std::ptrdiff_t>(start), payload.begin() + static_cast<std::ptrdiff_t>(start + o.size));
				c.at = base + start;
				out.push_back(std::move(c));
			});

			if (stop != payload.size()) throw format_error("an ASF header holds a damaged object");
			return out;
		}

		// The header with its padding held as a size rather than as objects: written back, its objects
		// come first and the padding they leave follows as one padding object.
		struct header
		{
			bytes fixed;
			std::vector<child> children;
			std::uint64_t size = 0;
			bool changed = false;

			child* find(const guid& id)
			{
				const auto it = std::ranges::find(children, id, &child::id);
				return it != children.end() ? &*it : nullptr;
			}

			std::uint64_t need() const
			{
				std::uint64_t n = header_fixed;
				for (const auto& c : children) n += c.raw.size();
				return n;
			}

			// The header at the given size, its objects first and the rest one padding object.
			bytes serialize(const std::uint64_t at_size) const
			{
				bytes out = fixed;
				for (const auto& c : children) out.insert(out.end(), c.raw.begin(), c.raw.end());

				auto count = children.size();

				if (const auto left = at_size - need(); left != 0)
				{
					const auto pad = make_object(padding_id, bytes(static_cast<std::size_t>(left - object_header), 0));
					out.insert(out.end(), pad.begin(), pad.end());
					++count;
				}

				store_le64(out.data() + 16, out.size());
				store_le32(out.data() + 24, static_cast<std::uint32_t>(count));
				return out;
			}
		};

		header parse_header(const bytes& raw)
		{
			if (raw.size() < header_fixed) throw format_error("the ASF header is cut short");

			header h;
			h.size = raw.size();
			h.fixed.assign(raw.begin(), raw.begin() + header_fixed);
			h.children = parse_objects(std::span(raw).subspan(header_fixed), header_fixed);

			// Padding is room, in the header and in its extension, where real files keep it.
			std::erase_if(h.children, [](const child& c) { return c.id == padding_id; });

			if (auto* const extension = h.find(header_extension_id))
			{
				auto& e = extension->raw;
				if (e.size() < extension_fixed) throw format_error("an ASF header holds a damaged extension");

				const auto data_size = static_cast<std::size_t>(load_le32(e.data() + asf::extension_data_size_at));
				if (data_size != e.size() - extension_fixed) throw format_error("an ASF header holds a damaged extension");

				auto inner = parse_objects(std::span(e).subspan(extension_fixed), 0);
				if (std::erase_if(inner, [](const child& c) { return c.id == padding_id; }) != 0)
				{
					bytes rebuilt(e.begin(), e.begin() + extension_fixed);
					for (const auto& c : inner) rebuilt.insert(rebuilt.end(), c.raw.begin(), c.raw.end());
					store_le64(rebuilt.data() + 16, rebuilt.size());
					store_le32(rebuilt.data() + asf::extension_data_size_at, static_cast<std::uint32_t>(rebuilt.size() - extension_fixed));
					e = std::move(rebuilt);
				}
			}

			return h;
		}

		bool broadcast(const child& file_properties)
		{
			return asf::broadcast(file_properties.raw);
		}

		// UTF-16LE and its terminator, cut at a whole character to what a 16-bit length holds.
		bytes asf_string(const std::string_view text)
		{
			auto s = utf16le(text);
			constexpr std::size_t max_text = 0xFFFC;

			if (s.size() > max_text)
			{
				s.resize(max_text);
				const auto last = load_le16(s.data() + max_text - 2);
				if (last >= 0xD800 && last <= 0xDBFF) s.resize(max_text - 2);
			}

			s.push_back(0);
			s.push_back(0);
			return s;
		}

		void set_text(bytes& field, const std::optional<std::string>& value)
		{
			if (value) field = value->empty() ? bytes{} : asf_string(*value);
		}

		// The author an edit gives: its artist, or failing one its creators, joined as properties' artist joins
		// them.
		std::optional<std::string> author_of(const legacy_edits& e)
		{
			if (e.artist) return e.artist;
			if (!e.creators) return {};

			std::string joined;

			for (const auto& creator : *e.creators)
			{
				if (creator.empty()) continue;
				if (!joined.empty()) joined += "; ";
				joined += creator;
			}

			return joined;
		}

		// Windows, FFmpeg and the taggers that write WMA read the Author as the artist and the Description as
		// the comment, as an MP3's TPE1 and COMM are read, so they are written from those: the toolkit wrote the
		// creators and the description there, which read back as neither. A description stays in the packet.
		void update_content_description(header& h, const legacy_edits& e)
		{
			const auto author = author_of(e);
			if (!e.title && !author && !e.copyright && !e.comment) return;

			// Title, author, copyright, description and rating, each a length then the text.
			std::array<bytes, 5> fields;
			auto* const existing = h.find(content_description_id);

			if (existing)
			{
				const auto& raw = existing->raw;
				reader r(raw);
				std::array<byte_range, 5> where;

				if (asf::content_description_fields(r, object_header, raw.size(), where) != fields.size())
				{
					throw format_error("an ASF header holds a damaged content description");
				}

				for (std::size_t i = 0; i < fields.size(); ++i)
				{
					const auto at = raw.begin() + static_cast<std::ptrdiff_t>(where[i].offset);
					fields[i].assign(at, at + static_cast<std::ptrdiff_t>(where[i].size));
				}
			}

			set_text(fields[vocabulary::asf_title], e.title);
			set_text(fields[vocabulary::asf_author], author);
			set_text(fields[vocabulary::asf_copyright], e.copyright);
			set_text(fields[vocabulary::asf_description], e.comment);

			bytes payload;
			for (const auto& f : fields) append_le16(payload, static_cast<std::uint16_t>(f.size()));
			for (const auto& f : fields) payload.insert(payload.end(), f.begin(), f.end());
			auto raw = make_object(content_description_id, payload);

			if (existing)
			{
				if (raw == existing->raw) return;
				existing->raw = std::move(raw);
				h.changed = true;
			}
			else if (std::ranges::any_of(fields, [](const bytes& f) { return !f.empty(); }))
			{
				h.children.push_back({content_description_id, std::move(raw), 0});
				h.changed = true;
			}
		}

		// The descriptor's name without its terminator.
		bool named(const std::span<const std::uint8_t> name, const std::string_view text)
		{
			auto n = name.size();
			while (n >= 2 && name[n - 1] == 0 && name[n - 2] == 0) n -= 2;

			const auto wanted = utf16le(text);
			return n == wanted.size() && std::equal(wanted.begin(), wanted.end(), name.begin());
		}

		void append_descriptor(bytes& to, const std::string_view name, const std::uint16_t type, const bytes& value)
		{
			auto n = utf16le(name);
			n.push_back(0);
			n.push_back(0);
			append_le16(to, static_cast<std::uint16_t>(n.size()));
			to.insert(to.end(), n.begin(), n.end());
			append_le16(to, type);
			append_le16(to, static_cast<std::uint16_t>(value.size()));
			to.insert(to.end(), value.begin(), value.end());
		}

		// The tags and rating Windows Explorer and Media Player read, as Diffractor's fork of the
		// toolkit wrote them. Descriptors the edit does not own are kept as they are.
		void update_extended_content(header& h, const legacy_edits& e)
		{
			const auto owns_category = e.keywords.has_value();
			const auto owns_rating = e.rating.has_value();
			if (!owns_category && !owns_rating) return;

			bytes descriptors;
			std::size_t count = 0;
			auto* const existing = h.find(extended_content_id);

			if (existing)
			{
				const auto& raw = existing->raw;
				reader r(raw);

				const auto whole = asf::walk_content_descriptors(r, object_header, raw.size(), [&](const asf::descriptor& d)
				{
					const std::span<const std::uint8_t> name(raw.data() + d.name_at, d.name_length);
					const auto owned = (owns_category && named(name, vocabulary::wm_category)) || (owns_rating && named(name, vocabulary::wm_shared_user_rating));

					if (!owned)
					{
						descriptors.insert(descriptors.end(), raw.begin() + static_cast<std::ptrdiff_t>(d.start),
						                   raw.begin() + static_cast<std::ptrdiff_t>(d.end()));
						++count;
					}
				});

				if (!whole) throw format_error("an ASF header holds damaged descriptors");
			}

			if (owns_category)
			{
				for (const auto& keyword : *e.keywords)
				{
					if (keyword.empty() || count >= 0xFFFF) continue;
					auto value = asf_string(keyword);
					append_descriptor(descriptors, vocabulary::wm_category, 0, value);
					++count;
				}
			}

			if (owns_rating && *e.rating > 0 && count < 0xFFFF)
			{
				bytes value;
				append_le32(value, static_cast<std::uint32_t>(percent_from_stars(*e.rating)));
				append_descriptor(descriptors, vocabulary::wm_shared_user_rating, 3, value);
				++count;
			}

			bytes payload;
			append_le16(payload, static_cast<std::uint16_t>(count));
			payload.insert(payload.end(), descriptors.begin(), descriptors.end());
			auto raw = make_object(extended_content_id, payload);

			if (existing)
			{
				if (raw == existing->raw) return;
				existing->raw = std::move(raw);
				h.changed = true;
			}
			else if (count > 0)
			{
				h.children.push_back({extended_content_id, std::move(raw), 0});
				h.changed = true;
			}
		}

		// The created date, as the 100-nanosecond intervals since 1601 the File Properties hold. A
		// broadcast file's date is not a creation date, so it is left as it is.
		void update_created(header& h, const legacy_edits& e)
		{
			if (!e.date_digitized) return;

			auto* const properties = h.find(file_properties_id);
			if (!properties || properties->raw.size() < asf::file_properties_size || broadcast(*properties)) return;

			const auto d = parse_xmp_date(*e.date_digitized);
			if (!d) return;

			const auto seconds = seconds_since(1601, *d);
			if (seconds < 0) return;

			const auto filetime = static_cast<std::uint64_t>(seconds) * 10'000'000;
			if (load_le64(properties->raw.data() + asf::creation_date_at) == filetime) return;

			store_le64(properties->raw.data() + asf::creation_date_at, filetime);
			h.changed = true;
		}
	}

	bool is_asf(const std::span<const std::uint8_t> head) noexcept
	{
		return asf::is(head, header_id);
	}

	void read_asf_packet(reader& r, packet_reading& out)
	{
		const auto l = read_layout(r);

		if (const auto* const x = l.xmp())
		{
			const auto packet = r.read(x->start + object_header, static_cast<std::size_t>(x->size - object_header));
			out.packet.assign(packet.begin(), packet.end());
		}

		out.readable = true;
	}

	void plan_asf(reader& r, const metadata_update& update, write_plan& plan)
	{
		const auto l = read_layout(r);
		const auto& ho = l.header();
		bytes appended;

		const auto patch = [&plan](const std::uint64_t offset, bytes data)
		{
			const auto length = data.size();
			plan.replacements.push_back({offset, length, std::move(data), commit});
		};

		// An object becomes padding by its GUID alone, so it keeps its size and what follows it stays.
		const auto pad_object_at = [&patch](const std::uint64_t start)
		{
			patch(start, bytes(padding_id.begin(), padding_id.end()));
		};

		// The packet.
		if (update.xmp)
		{
			const auto* const current = l.xmp();
			const auto old_length = current ? current->size - object_header : 0;
			const auto packet = serialize_packet(*update.xmp, old_length);

			if (current && packet.size() == old_length)
			{
				patch(current->start + object_header, to_bytes(packet));
			}
			else
			{
				const auto packet_object = make_object(xmp_id, {reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size()});
				appended.insert(appended.end(), packet_object.begin(), packet_object.end());
				if (current) pad_object_at(current->start);
			}

			// Packets after the first are stale, and a reader that takes the last would show one.
			for (std::size_t i = 1; i < l.xmp_at.size(); ++i) pad_object_at(l.objects[l.xmp_at[i]].start);
		}

		const auto original = r.read(ho.start, static_cast<std::size_t>(ho.size));
		auto h = parse_header(original);

		update_content_description(h, update.legacy);
		update_extended_content(h, update.legacy);
		update_created(h, update.legacy);

		const auto need = h.need();
		const auto fits = need == ho.size || need + object_header <= ho.size;
		const auto header_size = !h.changed ? ho.size : fits ? ho.size : need + header_room;
		const auto result_size = r.size() - ho.size + header_size + appended.size();

		// The File Properties keep the file's size, which a broadcast file does not know.
		const auto* const properties = h.find(file_properties_id);
		const auto keeps_size = properties && properties->raw.size() >= asf::file_properties_size && !broadcast(*properties);

		if (h.changed)
		{
			auto updated = h.serialize(header_size);

			if (keeps_size)
			{
				std::size_t at = header_fixed;
				for (const auto& c : h.children)
				{
					if (&c == properties) break;
					at += c.raw.size();
				}

				store_le64(updated.data() + at + asf::file_size_at, result_size);
			}

			if (fits)
			{
				if (auto p = changed_span(ho.start, original, updated, commit)) plan.replacements.push_back(std::move(*p));
			}
			else
			{
				plan.replacements.push_back({ho.start, ho.size, std::move(updated)});
			}
		}
		else if (keeps_size && result_size != r.size() && load_le64(properties->raw.data() + asf::file_size_at) != result_size)
		{
			bytes size;
			append_le64(size, result_size);
			patch(ho.start + properties->at + asf::file_size_at, std::move(size));
		}

		if (!appended.empty())
		{
			if (l.objects.back().end() != r.size()) throw format_error("the ASF ends with bytes that are not an object");
			plan.replacements.push_back({r.size(), 0, std::move(appended)});
		}
	}
}
