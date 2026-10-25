// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Serializes the XMP tree as RDF/XML in the XMP Toolkit's layouts: the default form with
// every property an element, and the compact form file handlers embed, where simple properties are
// attributes. The packet wrapper and padding follow the toolkit, so a packet can be rewritten where
// it lies.

#include "xmp_internal.h"

#include <string>

namespace libfiles::xmp
{
	using namespace detail;

	namespace
	{
		constexpr std::string_view packet_header = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>";
		constexpr std::string_view packet_trailer = "<?xpacket end=\"w\"?>";
		constexpr std::string_view xmpmeta_start = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"libfiles XMP 1.0\">";
		constexpr std::string_view xmpmeta_end = "</x:xmpmeta>";
		constexpr std::string_view rdf_start = "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">";
		constexpr std::string_view rdf_end = "</rdf:RDF>";

		struct writer
		{
			std::string& out;
			std::string_view newline;
			std::string_view indent_str;

			void indent(int level) const
			{
				for (; level > 0; --level) out += indent_str;
			}
		};

		// Escapes &, <, > and the tab, LF and CR controls, and also " inside an attribute.
		void append_value(std::string& out, const std::string_view value, const bool for_attribute)
		{
			constexpr char hex[] = "0123456789ABCDEF";

			for (const auto c : value)
			{
				const auto u = static_cast<unsigned char>(c);

				if (u < 0x20)
				{
					out += "&#x";
					out += hex[u & 0xF];
					out += ';';
				}
				else if (c == '&') out += "&amp;";
				else if (c == '<') out += "&lt;";
				else if (c == '>') out += "&gt;";
				else if (c == '"' && for_attribute) out += "&quot;";
				else out += c;
			}
		}

		bool can_be_attribute(const node* n)
		{
			if (n->name.starts_with('[')) return false;
			if (!n->qualifiers.empty()) return false;
			if (n->options & value_is_uri) return false;
			if (n->options & composite_mask) return false;
			return true;
		}

		bool is_attribute_qualifier(const std::string_view name)
		{
			return name == "xml:lang" || name == "rdf:resource" || name == "rdf:ID" || name == "rdf:bagID" ||
				name == "rdf:nodeID";
		}

		void declare_one_namespace(const writer& w, const std::string& prefix, const std::string& uri, std::string& used,
		                           const int indent)
		{
			const auto bounded = ':' + prefix;
			if (used.find(bounded) != std::string::npos) return;

			w.out += w.newline;
			w.indent(indent);
			w.out += "xmlns:";
			w.out += prefix;
			if (!w.out.empty() && w.out.back() == ':') w.out.back() = '=';
			else w.out += '=';
			w.out += '"';
			w.out += uri;
			w.out += '"';

			used += prefix;
		}

		void declare_element_namespace(const writer& w, const std::string& name, std::string& used, const int indent)
		{
			const auto colon = name.find(':');
			if (colon == std::string::npos) return;

			const auto prefix = name.substr(0, colon + 1);
			std::string uri;
			if (!registry_uri(prefix, &uri)) throw error("Unknown namespace prefix in serialization");
			declare_one_namespace(w, prefix, uri, used, indent);
		}

		void declare_used_namespaces(const writer& w, const node* n, std::string& used, const int indent)
		{
			if (n->options & schema_node)
			{
				declare_one_namespace(w, n->value, n->name, used, indent);
			}
			else if (n->options & value_is_struct)
			{
				for (const auto& field : n->children) declare_element_namespace(w, field->name, used, indent);
			}

			for (const auto& child : n->children) declare_used_namespaces(w, child.get(), used, indent);

			for (const auto& qual : n->qualifiers)
			{
				declare_element_namespace(w, qual->name, used, indent);
				declare_used_namespaces(w, qual.get(), used, indent);
			}
		}

		void emit_array_tag(const writer& w, const option_bits form, const int indent, const std::size_t size,
		                    const bool start)
		{
			if (!start && size == 0) return;

			w.indent(indent);
			w.out += start ? "<rdf:" : "</rdf:";

			if (form & array_is_alternate) w.out += "Alt";
			else if (form & array_is_ordered) w.out += "Seq";
			else w.out += "Bag";

			if (start && size == 0) w.out += '/';
			w.out += '>';
			w.out += w.newline;
		}

		void start_outer_description(const writer& w, const node& tree, const int base_indent)
		{
			w.indent(base_indent + 2);
			w.out += "<rdf:Description rdf:about=\"";
			w.out += tree.name;
			w.out += '"';

			std::string used = ":xml:rdf:";
			for (const auto& schema : tree.children) declare_used_namespaces(w, schema.get(), used, base_indent + 4);
		}

		// The verbose form: a property element per node, structs and qualified values as
		// rdf:parseType="Resource".
		void serialize_verbose_property(const writer& w, node* n, int indent, const bool as_rdf_value)
		{
			auto emit_end_tag = true;
			auto indent_end_tag = true;
			const auto form = n->options & composite_mask;

			std::string element_name = n->name;
			if (as_rdf_value) element_name = "rdf:value";
			else if (element_name.starts_with('[')) element_name = "rdf:li";

			w.indent(indent);
			w.out += '<';
			w.out += element_name;

			auto has_general_qualifiers = false;
			auto has_resource_qualifier = false;

			for (const auto& qual : n->qualifiers)
			{
				if (!is_attribute_qualifier(qual->name))
				{
					has_general_qualifiers = true;
				}
				else
				{
					if (qual->name == "rdf:resource") has_resource_qualifier = true;
					if (!as_rdf_value)
					{
						w.out += ' ';
						w.out += qual->name;
						w.out += "=\"";
						append_value(w.out, qual->value, true);
						w.out += '"';
					}
				}
			}

			if (has_general_qualifiers && !as_rdf_value)
			{
				if (has_resource_qualifier) throw error("Can't mix rdf:resource and general qualifiers");

				w.out += " rdf:parseType=\"Resource\">";
				w.out += w.newline;

				serialize_verbose_property(w, n, indent + 1, true);

				for (const auto& qual : n->qualifiers)
				{
					if (is_attribute_qualifier(qual->name)) continue;
					serialize_verbose_property(w, qual.get(), indent + 1, false);
				}
			}
			else if (form == 0)
			{
				if (n->options & value_is_uri)
				{
					w.out += " rdf:resource=\"";
					append_value(w.out, n->value, true);
					w.out += "\"/>";
					w.out += w.newline;
					emit_end_tag = false;
				}
				else if (n->value.empty())
				{
					w.out += "/>";
					w.out += w.newline;
					emit_end_tag = false;
				}
				else
				{
					w.out += '>';
					append_value(w.out, n->value, false);
					indent_end_tag = false;
				}
			}
			else if (form & value_is_array)
			{
				w.out += '>';
				w.out += w.newline;
				emit_array_tag(w, n->options, indent + 1, n->children.size(), true);
				if (is_alt_text(n->options)) normalize_lang_array(n);
				for (const auto& child : n->children) serialize_verbose_property(w, child.get(), indent + 2, false);
				emit_array_tag(w, n->options, indent + 1, n->children.size(), false);
			}
			else if (!has_resource_qualifier)
			{
				if (n->children.empty())
				{
					w.out += " rdf:parseType=\"Resource\"/>";
					w.out += w.newline;
					emit_end_tag = false;
				}
				else
				{
					w.out += " rdf:parseType=\"Resource\">";
					w.out += w.newline;
					for (const auto& child : n->children) serialize_verbose_property(w, child.get(), indent + 1, false);
				}
			}
			else
			{
				for (const auto& child : n->children)
				{
					if (!can_be_attribute(child.get())) throw error("Can't mix rdf:resource and complex fields");
					w.out += w.newline;
					w.indent(indent + 1);
					w.out += ' ';
					w.out += child->name;
					w.out += "=\"";
					append_value(w.out, child->value, true);
					w.out += '"';
				}

				w.out += "/>";
				w.out += w.newline;
				emit_end_tag = false;
			}

			if (emit_end_tag)
			{
				if (indent_end_tag) w.indent(indent);
				w.out += "</";
				w.out += element_name;
				w.out += '>';
				w.out += w.newline;
			}
		}

		bool serialize_compact_attr_props(const writer& w, const node* parent, const int indent)
		{
			auto all_are_attrs = true;

			for (const auto& prop : parent->children)
			{
				if (!can_be_attribute(prop.get()))
				{
					all_are_attrs = false;
					continue;
				}

				w.out += w.newline;
				w.indent(indent);
				w.out += prop->name;
				w.out += "=\"";
				append_value(w.out, prop->value, true);
				w.out += '"';
			}

			return all_are_attrs;
		}

		void serialize_compact_elem_props(const writer& w, node* parent, const int indent)
		{
			for (const auto& child : parent->children)
			{
				auto* const n = child.get();
				if (can_be_attribute(n)) continue;

				auto emit_end_tag = true;
				auto indent_end_tag = true;
				const auto form = n->options & composite_mask;

				std::string element_name = n->name;
				if (element_name.starts_with('[')) element_name = "rdf:li";

				w.indent(indent);
				w.out += '<';
				w.out += element_name;

				auto has_general_qualifiers = false;
				auto has_resource_qualifier = false;

				for (const auto& qual : n->qualifiers)
				{
					if (!is_attribute_qualifier(qual->name))
					{
						has_general_qualifiers = true;
					}
					else
					{
						if (qual->name == "rdf:resource") has_resource_qualifier = true;
						w.out += ' ';
						w.out += qual->name;
						w.out += "=\"";
						append_value(w.out, qual->value, true);
						w.out += '"';
					}
				}

				if (has_general_qualifiers)
				{
					w.out += " rdf:parseType=\"Resource\">";
					w.out += w.newline;

					serialize_verbose_property(w, n, indent + 1, true);

					auto first = std::size_t{0};
					if (n->options & has_lang) ++first;
					for (auto q = first; q < n->qualifiers.size(); ++q)
					{
						serialize_verbose_property(w, n->qualifiers[q].get(), indent + 1, false);
					}
				}
				else if (form == 0)
				{
					if (n->options & value_is_uri)
					{
						w.out += " rdf:resource=\"";
						append_value(w.out, n->value, true);
						w.out += "\"/>";
						w.out += w.newline;
						emit_end_tag = false;
					}
					else if (n->value.empty())
					{
						w.out += "/>";
						w.out += w.newline;
						emit_end_tag = false;
					}
					else
					{
						w.out += '>';
						append_value(w.out, n->value, false);
						indent_end_tag = false;
					}
				}
				else if (form & value_is_array)
				{
					w.out += '>';
					w.out += w.newline;
					emit_array_tag(w, n->options, indent + 1, n->children.size(), true);
					if (is_alt_text(n->options)) normalize_lang_array(n);
					serialize_compact_elem_props(w, n, indent + 2);
					emit_array_tag(w, n->options, indent + 1, n->children.size(), false);
				}
				else
				{
					auto has_attr_fields = false;
					auto has_elem_fields = false;

					for (const auto& field : n->children)
					{
						if (can_be_attribute(field.get())) has_attr_fields = true;
						else has_elem_fields = true;
					}

					if (has_resource_qualifier && has_elem_fields)
					{
						throw error("Can't mix rdf:resource qualifier and element fields");
					}

					if (n->children.empty())
					{
						w.out += " rdf:parseType=\"Resource\"/>";
						w.out += w.newline;
						emit_end_tag = false;
					}
					else if (!has_elem_fields)
					{
						serialize_compact_attr_props(w, n, indent + 1);
						w.out += "/>";
						w.out += w.newline;
						emit_end_tag = false;
					}
					else if (!has_attr_fields)
					{
						w.out += " rdf:parseType=\"Resource\">";
						w.out += w.newline;
						serialize_compact_elem_props(w, n, indent + 1);
					}
					else
					{
						w.out += '>';
						w.out += w.newline;
						w.indent(indent + 1);
						w.out += "<rdf:Description";
						serialize_compact_attr_props(w, n, indent + 2);
						w.out += '>';
						w.out += w.newline;
						serialize_compact_elem_props(w, n, indent + 1);
						w.indent(indent + 1);
						w.out += "</rdf:Description>";
						w.out += w.newline;
					}
				}

				if (emit_end_tag)
				{
					if (indent_end_tag) w.indent(indent);
					w.out += "</";
					w.out += element_name;
					w.out += '>';
					w.out += w.newline;
				}
			}
		}

		void serialize_schemas(const writer& w, node& tree, const int base_indent, const bool compact)
		{
			start_outer_description(w, tree, base_indent);

			if (compact)
			{
				auto all_are_attrs = true;
				for (const auto& schema : tree.children)
				{
					all_are_attrs &= serialize_compact_attr_props(w, schema.get(), base_indent + 3);
				}

				if (all_are_attrs)
				{
					w.out += "/>";
					w.out += w.newline;
					return;
				}

				w.out += '>';
				w.out += w.newline;

				for (const auto& schema : tree.children) serialize_compact_elem_props(w, schema.get(), base_indent + 3);
			}
			else
			{
				if (tree.children.empty())
				{
					w.out += "/>";
					w.out += w.newline;
					return;
				}

				w.out += '>';
				w.out += w.newline;

				for (const auto& schema : tree.children)
				{
					for (const auto& prop : schema->children)
					{
						serialize_verbose_property(w, prop.get(), base_indent + 3, false);
					}
				}
			}

			w.indent(base_indent + 2);
			w.out += "</rdf:Description>";
			w.out += w.newline;
		}
	}

	std::string meta::serialize(const serialize_options& options) const
	{
		std::string_view newline = options.newline;
		std::string_view indent = options.indent;

		if (options.omit_all_formatting)
		{
			newline = " ";
			indent = "";
		}
		else
		{
			if (newline.empty()) newline = "\n";
			if (indent.empty()) indent = options.use_compact_format ? " " : "   ";
		}

		std::size_t padding = options.padding;

		if (options.exact_packet_length)
		{
			if (options.omit_packet_wrapper) throw error("Inconsistent options for exact size serialize");
		}
		else if (options.read_only_packet || options.omit_packet_wrapper)
		{
			padding = 0;
		}
		else if (padding == 0)
		{
			padding = 2048;
		}

		// Serializing an alt-text array puts its x-default item first, as the toolkit does.
		auto& tree = const_cast<node&>(*_tree);

		std::string rdf;
		const writer rdf_writer{rdf, newline, indent};
		rdf += rdf_start;
		rdf += newline;
		serialize_schemas(rdf_writer, tree, options.base_indent, options.use_compact_format);
		rdf_writer.indent(options.base_indent + 1);
		rdf += rdf_end;

		std::string result;
		const writer head{result, newline, indent};

		if (!options.omit_packet_wrapper)
		{
			head.indent(options.base_indent);
			result += packet_header;
			result += newline;
		}

		head.indent(options.base_indent);
		result += xmpmeta_start;
		result += newline;
		head.indent(options.base_indent + 1);
		result += rdf;
		result += newline;
		head.indent(options.base_indent);
		result += xmpmeta_end;
		result += newline;

		std::string tail;
		if (!options.omit_packet_wrapper)
		{
			const writer tail_writer{tail, newline, indent};
			tail_writer.indent(options.base_indent);
			tail += packet_trailer;
			if (options.read_only_packet) tail[tail.size() - 4] = 'r';
		}

		if (options.exact_packet_length)
		{
			const auto minimum = result.size() + tail.size();
			if (minimum > padding) throw error("Can't fit into specified packet size");
			padding -= minimum;
		}

		if (padding < newline.size())
		{
			result.append(padding, ' ');
		}
		else if (padding > 0)
		{
			padding -= newline.size();
			while (padding >= 100 + newline.size())
			{
				result.append(100, ' ');
				result += newline;
				padding -= 100 + newline.size();
			}
			result.append(padding, ' ');
			result += newline;
		}

		result += tail;
		return result;
	}
}
