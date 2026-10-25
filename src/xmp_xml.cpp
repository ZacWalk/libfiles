// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The XML layer of the XMP parser. Decodes UTF-8, UTF-16 and UTF-32 input, repairs 8-bit
// text the way the XMP Toolkit does (Windows-1252 bytes and ASCII controls), and builds an XML tree
// with namespaces resolved against the shared namespace table. Like the toolkit's Expat adapter it
// stops at the first fault and keeps everything built before it.

#include "xmp_internal.h"

#include "text_internal.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace libfiles::xmp::detail
{
	namespace
	{
		constexpr char32_t invalid_code_point = 0xFFFFFFFF;

		using ::libfiles::detail::append_utf8;

		enum class input_encoding
		{
			utf8,
			utf16_big,
			utf16_little,
			utf32_big,
			utf32_little,
		};

		// The toolkit's guess, which needs only the first bytes: the first character must be U+FEFF or
		// ASCII, and U+0000 is not allowed.
		input_encoding determine_encoding(const std::string_view bytes)
		{
			const auto* const b = reinterpret_cast<const unsigned char*>(bytes.data());
			const auto length = bytes.size();
			if (length < 2) return input_encoding::utf8;

			if (b[0] == 0)
			{
				if (length < 4 || b[1] != 0) return input_encoding::utf16_big;
				return input_encoding::utf32_big;
			}

			if (b[0] < 0x80)
			{
				if (b[1] != 0) return input_encoding::utf8;
				if (length < 4 || b[2] != 0) return input_encoding::utf16_little;
				return input_encoding::utf32_little;
			}

			if (b[0] == 0xEF) return input_encoding::utf8;
			if (b[0] == 0xFE) return input_encoding::utf16_big;
			if (length < 4 || b[2] != 0) return input_encoding::utf16_little;
			return input_encoding::utf32_little;
		}

		// UTF-16 and UTF-32 reach Expat unrepaired, so a malformed unit ends the document there.
		std::string transcode_wide(const std::string_view bytes, const input_encoding encoding)
		{
			std::string out;
			out.reserve(bytes.size());

			const auto* const b = reinterpret_cast<const unsigned char*>(bytes.data());
			const auto size = bytes.size();

			if (encoding == input_encoding::utf16_big || encoding == input_encoding::utf16_little)
			{
				const auto big = encoding == input_encoding::utf16_big;
				const auto unit_at = [b, big](const std::size_t i) -> char32_t
				{
					return big ? static_cast<char32_t>((b[i] << 8) | b[i + 1]) : static_cast<char32_t>(b[i] | (b[i + 1] << 8));
				};

				for (std::size_t i = 0; i + 1 < size; i += 2)
				{
					auto cp = unit_at(i);

					if (cp >= 0xD800 && cp <= 0xDBFF)
					{
						if (i + 3 >= size) break;
						const auto low = unit_at(i + 2);
						if (low < 0xDC00 || low > 0xDFFF) break;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						i += 2;
					}
					else if (cp >= 0xDC00 && cp <= 0xDFFF)
					{
						break;
					}

					append_utf8(out, cp);
				}
			}
			else
			{
				const auto big = encoding == input_encoding::utf32_big;

				for (std::size_t i = 0; i + 3 < size; i += 4)
				{
					const auto cp = big
						                ? static_cast<char32_t>((b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3])
						                : static_cast<char32_t>(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24));
					if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) break;
					append_utf8(out, cp);
				}
			}

			return out;
		}

		// Windows-1252, but for the five bytes it leaves undefined, which become spaces, being XML
		// restricted characters otherwise.
		void append_latin1(std::string& out, const unsigned char byte)
		{
			const auto cp = ::libfiles::detail::windows_1252(byte);
			append_utf8(out, cp >= 0x80 && cp < 0xA0 ? U' ' : cp);
		}

		// Counts a multi-byte sequence the toolkit's loose way: high bits of the lead byte, then that
		// many continuation bytes. Zero for an invalid sequence, negative for one cut off by the end.
		int count_utf8(const unsigned char* start, const unsigned char* end)
		{
			if (start >= end || (*start & 0xC0) != 0xC0) return 0;

			auto count = 2;
			for (auto lead = static_cast<unsigned char>(*start << 2); (lead & 0x80) != 0; lead = static_cast<unsigned char>(lead << 1))
			{
				++count;
			}

			if (start + count > end) return -count;

			for (auto i = 1; i < count; ++i)
			{
				if ((start[i] & 0xC0) != 0x80) return 0;
			}

			return count;
		}

		// "&#x1F;" style escapes of the controls Expat refuses. Returns their length, 0 for anything
		// else. The toolkit also blanks two-digit escapes of ordinary characters, so "&#xA9;" reads
		// as a space there; only real controls are replaced here.
		int count_control_escape(const unsigned char* start, const unsigned char* end)
		{
			const auto tail = static_cast<std::size_t>(end - start);
			if (tail < 5) return 0;
			if (start[1] != '#' || start[2] != 'x') return 0;

			unsigned value = 0;
			const auto* p = start + 3;

			for (auto digits = 0; digits < 2 && p < end; ++digits)
			{
				const auto c = *p;
				if (c >= '0' && c <= '9') value = (value << 4) + (c - '0');
				else if (c >= 'A' && c <= 'F') value = (value << 4) + (c - 'A' + 10);
				else if (c >= 'a' && c <= 'f') value = (value << 4) + (c - 'a' + 10);
				else break;
				++p;
			}

			if (p >= end || *p != ';') return 0;
			const auto length = static_cast<int>(p - start + 1);
			if (length < 5) return 0;
			if (value == 0x09 || value == 0x0A || value == 0x0D) return 0;
			if (value >= 0x20 && value != 0x7F) return 0;
			return length;
		}

		// 8-bit input may be Latin-1 rather than UTF-8 and may hold raw ASCII controls, neither of
		// which XML allows. Invalid UTF-8 bytes are taken as Windows-1252, controls other than tab,
		// LF and CR become spaces, as do numeric escapes of them. The toolkit ends the input with a
		// space, which can show up at the end of a truncated value.
		std::string repair_utf8(const std::string_view bytes)
		{
			std::string out;
			out.reserve(bytes.size() + 1);

			const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
			const auto* const end = p + bytes.size();

			while (p < end)
			{
				const auto c = *p;

				if (c >= 0x20 && c <= 0x7E && c != '&')
				{
					out += static_cast<char>(c);
					++p;
				}
				else if (c >= 0x80)
				{
					const auto length = count_utf8(p, end);

					if (length > 0)
					{
						out.append(reinterpret_cast<const char*>(p), static_cast<std::size_t>(length));
						p += length;
					}
					else
					{
						append_latin1(out, c);
						++p;
					}
				}
				else if (c < 0x20 || c == 0x7F)
				{
					out += (c == 0x09 || c == 0x0A || c == 0x0D) ? static_cast<char>(c) : ' ';
					++p;
				}
				else
				{
					const auto escape = count_control_escape(p, end);

					if (escape > 0)
					{
						out += ' ';
						p += escape;
					}
					else
					{
						out += '&';
						++p;
					}
				}
			}

			out += ' ';
			return out;
		}

		// Decodes one UTF-8 sequence strictly, as Expat reads its input.
		char32_t decode_utf8(const std::string_view text, std::size_t& i)
		{
			const auto* const b = reinterpret_cast<const unsigned char*>(text.data());
			const auto size = text.size();
			const auto c = b[i];

			if (c < 0x80)
			{
				++i;
				return c;
			}

			int count;
			char32_t cp;

			if ((c & 0xE0) == 0xC0)
			{
				count = 2;
				cp = c & 0x1F;
			}
			else if ((c & 0xF0) == 0xE0)
			{
				count = 3;
				cp = c & 0x0F;
			}
			else if ((c & 0xF8) == 0xF0)
			{
				count = 4;
				cp = c & 0x07;
			}
			else
			{
				return invalid_code_point;
			}

			if (i + count > size) return invalid_code_point;

			for (auto k = 1; k < count; ++k)
			{
				const auto cc = b[i + k];
				if ((cc & 0xC0) != 0x80) return invalid_code_point;
				cp = (cp << 6) | (cc & 0x3F);
			}

			constexpr char32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
			if (cp < minimum[count] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return invalid_code_point;

			i += count;
			return cp;
		}

		bool is_xml_char(const char32_t cp)
		{
			if (cp < 0x20) return cp == 0x09 || cp == 0x0A || cp == 0x0D;
			if (cp <= 0xD7FF) return true;
			if (cp < 0xE000) return false;
			if (cp <= 0xFFFD) return true;
			return cp >= 0x10000 && cp <= 0x10FFFF;
		}

		// Expat stops at the first character that is not legal XML. Data before it is still
		// delivered, so cutting the text there gives the same tree.
		std::string_view cut_at_invalid_char(const std::string_view text)
		{
			std::size_t i = 0;

			while (i < text.size())
			{
				const auto c = static_cast<unsigned char>(text[i]);

				if (c >= 0x20 && c < 0x80)
				{
					++i;
					continue;
				}

				const auto start = i;
				const auto cp = decode_utf8(text, i);
				if (cp == invalid_code_point || !is_xml_char(cp)) return text.substr(0, start);
			}

			return text;
		}

		bool is_name_start(const unsigned char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
		}

		bool is_name_char(const unsigned char c)
		{
			return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
		}

		bool is_space(const char c)
		{
			return c == ' ' || c == '\t' || c == '\n' || c == '\r';
		}

		struct ns_binding
		{
			std::string prefix; // Empty for the default namespace.
			std::string uri;
		};

		struct open_element
		{
			xml_node* node = nullptr;
			std::string qname; // As written, to match the end tag.
			std::size_t bindings = 0; // Namespace bindings this element declared.
		};

		struct raw_attribute
		{
			std::string qname;
			std::string value;
		};

		// The `dc` namespace early Flash wrote into SWF files.
		std::string_view fix_dc_uri(const std::string_view uri)
		{
			return uri == "http://purl.org/dc/1.1/" ? ns::dc : uri;
		}

		class xml_parser
		{
		public:
			xml_parser(const std::string_view text, xml_document& doc) : _text(text), _doc(doc)
			{
				_bindings.push_back({"xml", std::string(ns::xml)});
			}

			void run()
			{
				// A UTF-8 byte-order mark is not content.
				if (_text.substr(0, 3) == "\xEF\xBB\xBF") _pos = 3;

				if (!prolog()) return;
				if (!root_and_content()) return;
				epilog();
			}

		private:
			std::string_view _text;
			xml_document& _doc;
			std::size_t _pos = 0;
			std::vector<ns_binding> _bindings;
			std::vector<open_element> _stack;
			std::string _pending_text;

			bool at_end() const { return _pos >= _text.size(); }
			bool starts_with(const std::string_view s) const { return _text.substr(_pos, s.size()) == s; }

			void skip_space()
			{
				while (!at_end() && is_space(_text[_pos])) ++_pos;
			}

			xml_node* current() { return _stack.empty() ? &_doc.root : _stack.back().node; }

			void flush_text()
			{
				if (_pending_text.empty()) return;

				auto* const parent = current();
				auto text = std::make_unique<xml_node>();
				text->kind = xml_kind::text;
				text->value = std::move(_pending_text);
				text->parent = parent;
				parent->content.push_back(std::move(text));
				_pending_text.clear();
			}

			// Before the root: an XML declaration, comments, processing instructions, a DOCTYPE and
			// white space.
			bool prolog()
			{
				if (starts_with("<?xml") && _pos + 5 < _text.size() && is_space(_text[_pos + 5]))
				{
					const auto end = _text.find("?>", _pos);
					if (end == std::string_view::npos) return false;
					_pos = end + 2;
				}

				while (true)
				{
					skip_space();
					if (at_end()) return false; // No element found.
					if (starts_with("<!--"))
					{
						if (!comment()) return false;
					}
					else if (starts_with("<?"))
					{
						if (!processing_instruction(false)) return false;
					}
					else if (starts_with("<!DOCTYPE"))
					{
						if (!doctype()) return false;
					}
					else if (_text[_pos] == '<')
					{
						return true;
					}
					else
					{
						return false; // Text before the root.
					}
				}
			}

			bool comment()
			{
				const auto start = _pos + 4;
				const auto end = _text.find("--", start);
				if (end == std::string_view::npos) return false;
				if (end + 2 >= _text.size() || _text[end + 2] != '>') return false; // "--" inside a comment.
				_pos = end + 3;
				return true;
			}

			bool processing_instruction(const bool in_content)
			{
				auto p = _pos + 2;
				const auto target_start = p;
				if (p >= _text.size() || !is_name_start(static_cast<unsigned char>(_text[p]))) return false;
				while (p < _text.size() && (is_name_char(static_cast<unsigned char>(_text[p])) || _text[p] == ':')) ++p;
				const auto target = _text.substr(target_start, p - target_start);

				if (target.size() == 3 && (target[0] | 0x20) == 'x' && (target[1] | 0x20) == 'm' && (target[2] | 0x20) == 'l')
				{
					return false; // A reserved target, or a misplaced XML declaration.
				}

				const auto end = _text.find("?>", p);
				if (end == std::string_view::npos) return false;

				auto data_start = p;
				if (data_start < end)
				{
					if (!is_space(_text[data_start])) return false;
					while (data_start < end && is_space(_text[data_start])) ++data_start;
				}

				_pos = end + 2;

				if (target == "xpacket")
				{
					if (in_content) flush_text();
					auto* const parent = current();
					auto pi = std::make_unique<xml_node>();
					pi->kind = xml_kind::processing_instruction;
					pi->name = "xpacket";
					pi->value = std::string(_text.substr(data_start, end - data_start));
					pi->parent = parent;
					parent->content.push_back(std::move(pi));
				}

				return true;
			}

			// The DOCTYPE and any internal subset are skipped. Entities they declare are therefore
			// undefined, a difference from Expat that no XMP writer is known to exercise.
			bool doctype()
			{
				auto p = _pos + 9;
				auto depth = 0;

				while (p < _text.size())
				{
					const auto c = _text[p];

					if (c == '"' || c == '\'')
					{
						const auto close = _text.find(c, p + 1);
						if (close == std::string_view::npos) return false;
						p = close + 1;
						continue;
					}

					if (c == '[') ++depth;
					else if (c == ']') --depth;
					else if (c == '>' && depth <= 0)
					{
						_pos = p + 1;
						return true;
					}

					++p;
				}

				return false;
			}

			bool read_qname(std::string& qname)
			{
				const auto start = _pos;
				if (at_end() || !is_name_start(static_cast<unsigned char>(_text[_pos]))) return false;
				auto colons = 0;

				while (!at_end())
				{
					const auto c = static_cast<unsigned char>(_text[_pos]);
					if (c == ':')
					{
						++colons;
						++_pos;
						if (at_end() || !is_name_start(static_cast<unsigned char>(_text[_pos]))) return false;
						continue;
					}
					if (!is_name_char(c)) break;
					++_pos;
				}

				if (colons > 1) return false;
				qname.assign(_text.substr(start, _pos - start));
				return true;
			}

			// Character and entity references. Only the five predefined entities exist.
			bool read_reference(std::string& out)
			{
				const auto end = _text.find(';', _pos);
				if (end == std::string_view::npos) return false;
				const auto body = _text.substr(_pos + 1, end - _pos - 1);
				_pos = end + 1;

				if (!body.empty() && body[0] == '#')
				{
					char32_t cp = 0;
					const auto hex = body.size() > 1 && body[1] == 'x';
					const auto digits = body.substr(hex ? 2 : 1);
					if (digits.empty() || digits.size() > 8) return false;

					for (const auto c : digits)
					{
						unsigned d;
						if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
						else if (hex && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
						else if (hex && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
						else return false;
						cp = cp * (hex ? 16 : 10) + d;
						if (cp > 0x10FFFF) return false;
					}

					if (!is_xml_char(cp)) return false;
					append_utf8(out, cp);
					return true;
				}

				if (body == "lt") out += '<';
				else if (body == "gt") out += '>';
				else if (body == "amp") out += '&';
				else if (body == "apos") out += '\'';
				else if (body == "quot") out += '"';
				else return false;

				return true;
			}

			bool read_attribute_value(std::string& value)
			{
				if (at_end()) return false;
				const auto quote = _text[_pos];
				if (quote != '"' && quote != '\'') return false;
				++_pos;

				while (true)
				{
					if (at_end()) return false;
					const auto c = _text[_pos];

					if (c == quote)
					{
						++_pos;
						return true;
					}

					if (c == '<') return false;

					if (c == '&')
					{
						if (!read_reference(value)) return false;
						continue;
					}

					if (c == '\r')
					{
						value += ' ';
						++_pos;
						if (!at_end() && _text[_pos] == '\n') ++_pos;
						continue;
					}

					value += (c == '\n' || c == '\t') ? ' ' : c;
					++_pos;
				}
			}

			const std::string* lookup_binding(const std::string_view prefix) const
			{
				for (auto i = _bindings.size(); i-- > 0;)
				{
					if (_bindings[i].prefix == prefix) return &_bindings[i].uri;
				}

				return nullptr;
			}

			// The toolkit names nodes by the registered prefix of their URI, not the prefix in the text.
			void set_qualified_name(xml_node& node, const std::string_view uri, const std::string_view local)
			{
				node.ns.assign(fix_dc_uri(uri));
				std::string prefix;
				registry_prefix(node.ns, &prefix);
				node.name = prefix;
				node.name += local;
			}

			bool start_tag()
			{
				++_pos; // '<'
				std::string qname;
				if (!read_qname(qname)) return false;

				std::vector<raw_attribute> raw;
				auto self_closing = false;

				while (true)
				{
					const auto before_space = _pos;
					skip_space();
					if (at_end()) return false;

					if (_text[_pos] == '>')
					{
						++_pos;
						break;
					}

					if (starts_with("/>"))
					{
						_pos += 2;
						self_closing = true;
						break;
					}

					if (_pos == before_space) return false; // Attributes need white space between them.

					raw_attribute attr;
					if (!read_qname(attr.qname)) return false;
					skip_space();
					if (at_end() || _text[_pos] != '=') return false;
					++_pos;
					skip_space();
					if (!read_attribute_value(attr.value)) return false;

					for (const auto& other : raw)
					{
						if (other.qname == attr.qname) return false;
					}

					raw.push_back(std::move(attr));
				}

				// The start tag is complete, so this is the point where Expat reports it: namespace
				// declarations first, in order, then the element.
				const auto bindings_before = _bindings.size();

				for (const auto& attr : raw)
				{
					std::string_view prefix;

					if (attr.qname == "xmlns")
					{
						prefix = {};
					}
					else if (attr.qname.starts_with("xmlns:"))
					{
						prefix = std::string_view(attr.qname).substr(6);
						if (attr.value.empty()) return false; // A prefix cannot be undeclared.
						if (prefix == "xmlns") return false;
						if (prefix == "xml" && attr.value != ns::xml) return false;
					}
					else
					{
						continue;
					}

					_bindings.push_back({std::string(prefix), attr.value});

					if (!attr.value.empty())
					{
						register_namespace(fix_dc_uri(attr.value), prefix.empty() ? std::string_view("_dflt_") : prefix);
					}
				}

				auto* const parent = current();
				auto element = std::make_unique<xml_node>();
				element->kind = xml_kind::element;
				element->parent = parent;

				const auto split = [](const std::string_view name, std::string_view& prefix, std::string_view& local)
				{
					const auto colon = name.find(':');
					if (colon == std::string_view::npos)
					{
						prefix = {};
						local = name;
					}
					else
					{
						prefix = name.substr(0, colon);
						local = name.substr(colon + 1);
					}
				};

				std::string_view prefix, local;
				split(qname, prefix, local);

				if (!prefix.empty())
				{
					if (prefix == "xmlns") return unwind(bindings_before);
					const auto* const uri = lookup_binding(prefix);
					if (!uri) return unwind(bindings_before);
					set_qualified_name(*element, *uri, local);
				}
				else
				{
					const auto* const uri = lookup_binding({});
					if (uri && !uri->empty())
					{
						set_qualified_name(*element, *uri, local);
					}
					else
					{
						element->name = std::string(local);
						qualify_rdf_description_term(*element, parent);
					}
				}

				std::vector<std::pair<std::string, std::string>> expanded_names;

				for (auto& attr : raw)
				{
					if (attr.qname == "xmlns" || attr.qname.starts_with("xmlns:")) continue;

					auto node = std::make_unique<xml_node>();
					node->kind = xml_kind::attribute;
					node->parent = element.get();

					split(attr.qname, prefix, local);

					if (!prefix.empty())
					{
						const auto* const uri = lookup_binding(prefix);
						if (!uri) return unwind(bindings_before);
						set_qualified_name(*node, *uri, local);
					}
					else
					{
						node->name = std::string(local);
						qualify_rdf_description_term(*node, element.get());
					}

					// Two attributes may not share an expanded name even when spelled differently.
					for (const auto& [seen_ns, seen_name] : expanded_names)
					{
						if (seen_ns == node->ns && seen_name == node->name) return unwind(bindings_before);
					}

					expanded_names.emplace_back(node->ns, node->name);

					node->value = std::move(attr.value);
					if (node->name == "xml:lang") normalize_lang_value(node->value);
					element->attrs.push_back(std::move(node));
				}

				auto* const raw_element = element.get();
				parent->content.push_back(std::move(element));

				if (raw_element->name == "rdf:RDF")
				{
					_doc.last_rdf = raw_element;
					++_doc.rdf_count;
				}

				if (self_closing)
				{
					_bindings.resize(bindings_before);
				}
				else
				{
					_stack.push_back({raw_element, std::move(qname), _bindings.size() - bindings_before});
				}

				return true;
			}

			bool unwind(const std::size_t bindings_before)
			{
				_bindings.resize(bindings_before);
				return false;
			}

			// A compatibility hack the toolkit applies: an unqualified "about" or "ID" under
			// rdf:Description is rdf:about or rdf:ID.
			static void qualify_rdf_description_term(xml_node& node, const xml_node* parent)
			{
				if (!parent || parent->name != "rdf:Description") return;

				if (node.name == "about")
				{
					node.ns = ns::rdf;
					node.name = "rdf:about";
				}
				else if (node.name == "ID")
				{
					node.ns = ns::rdf;
					node.name = "rdf:ID";
				}
			}

			bool end_tag()
			{
				_pos += 2;
				std::string qname;
				if (!read_qname(qname)) return false;
				skip_space();
				if (at_end() || _text[_pos] != '>') return false;
				++_pos;

				if (_stack.empty() || _stack.back().qname != qname) return false;

				_bindings.resize(_bindings.size() - _stack.back().bindings);
				_stack.pop_back();
				return true;
			}

			bool cdata_section()
			{
				const auto start = _pos + 9;
				const auto end = _text.find("]]>", start);

				if (end == std::string_view::npos)
				{
					// Expat delivers an unterminated section's content before it fails, all but a
					// trailing "]" or "]]" that could still have begun the terminator.
					auto content = _text.substr(std::min(start, _text.size()));
					for (auto i = 0; i < 2 && content.ends_with(']'); ++i) content.remove_suffix(1);
					append_text(content);
					return false;
				}

				append_text(_text.substr(start, end - start));
				_pos = end + 3;
				return true;
			}

			// Line ends are normalized to LF, as XML requires of character data.
			void append_text(const std::string_view text)
			{
				for (std::size_t i = 0; i < text.size(); ++i)
				{
					if (text[i] == '\r')
					{
						_pending_text += '\n';
						if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
					}
					else
					{
						_pending_text += text[i];
					}
				}
			}

			bool root_and_content()
			{
				if (!start_tag()) return false;

				while (!_stack.empty())
				{
					if (at_end())
					{
						flush_text();
						return false; // Unclosed elements.
					}

					const auto c = _text[_pos];

					if (c == '<')
					{
						if (starts_with("</"))
						{
							flush_text();
							if (!end_tag()) return false;
						}
						else if (starts_with("<!--"))
						{
							if (!comment()) return fail_with_text();
						}
						else if (starts_with("<![CDATA["))
						{
							if (!cdata_section()) return fail_with_text();
						}
						else if (starts_with("<?"))
						{
							if (!processing_instruction(true)) return fail_with_text();
						}
						else
						{
							flush_text();
							if (!start_tag()) return false;
						}
					}
					else if (c == '&')
					{
						if (!read_reference(_pending_text)) return fail_with_text();
					}
					else
					{
						const auto next = _text.find_first_of("<&", _pos);
						const auto run = _text.substr(_pos, next == std::string_view::npos ? std::string_view::npos : next - _pos);
						if (run.find("]]>") != std::string_view::npos) return fail_with_text();
						append_text(run);
						_pos += run.size();
					}
				}

				return true;
			}

			bool fail_with_text()
			{
				flush_text();
				return false;
			}

			// After the root only white space, comments and processing instructions may follow.
			void epilog()
			{
				while (true)
				{
					skip_space();
					if (at_end()) return;
					if (starts_with("<!--"))
					{
						if (!comment()) return;
					}
					else if (starts_with("<?"))
					{
						if (!processing_instruction(false)) return;
					}
					else
					{
						return; // Junk after the document element.
					}
				}
			}
		};
	}

	bool xml_node::is_whitespace() const
	{
		if (kind != xml_kind::text) return false;

		for (const auto c : value)
		{
			if (!is_space(c)) return false;
		}

		return true;
	}

	void parse_xml(const std::string_view bytes, xml_document& doc)
	{
		const auto encoding = determine_encoding(bytes);
		std::string text;

		if (encoding == input_encoding::utf8)
		{
			text = repair_utf8(bytes);
		}
		else
		{
			text = transcode_wide(bytes, encoding);
			if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
		}

		xml_parser parser(cut_at_invalid_char(text), doc);
		parser.run();
	}
}
