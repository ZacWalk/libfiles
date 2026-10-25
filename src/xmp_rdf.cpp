// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Builds the XMP tree from the RDF in an XML tree, then normalizes it: Dublin Core
// arrays written as simple values, explicit aliases, and the special cases the XMP Toolkit repairs.
// Every RDF fault is recovered the way the toolkit recovers it, by skipping the offending construct.

#include "xmp_internal.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace libfiles::xmp::detail
{
	namespace
	{
		enum rdf_term
		{
			term_other = 0,
			term_rdf = 1,
			term_id = 2,
			term_about = 3,
			term_parse_type = 4,
			term_resource = 5,
			term_node_id = 6,
			term_datatype = 7,
			term_description = 8,
			term_li = 9,
			term_about_each = 10,
			term_about_each_prefix = 11,
			term_bag_id = 12,
		};

		rdf_term term_of(const std::string_view name)
		{
			if (name.size() <= 4 || !name.starts_with("rdf:")) return term_other;
			if (name == "rdf:li") return term_li;
			if (name == "rdf:parseType") return term_parse_type;
			if (name == "rdf:Description") return term_description;
			if (name == "rdf:about") return term_about;
			if (name == "rdf:resource") return term_resource;
			if (name == "rdf:RDF") return term_rdf;
			if (name == "rdf:ID") return term_id;
			if (name == "rdf:nodeID") return term_node_id;
			if (name == "rdf:datatype") return term_datatype;
			if (name == "rdf:aboutEach") return term_about_each;
			if (name == "rdf:aboutEachPrefix") return term_about_each_prefix;
			if (name == "rdf:bagID") return term_bag_id;
			return term_other;
		}

		bool is_core_syntax_term(const rdf_term t) { return t >= term_rdf && t <= term_datatype; }
		bool is_old_term(const rdf_term t) { return t >= term_about_each && t <= term_bag_id; }

		bool is_property_element_name(const rdf_term t)
		{
			if (t == term_description || is_old_term(t)) return false;
			return !is_core_syntax_term(t);
		}

		bool is_numbered_array_item_name(const std::string_view name)
		{
			if (name.size() <= 5 || !name.starts_with("rdf:_")) return false;
			return std::all_of(name.begin() + 5, name.end(), [](const char c) { return c >= '0' && c <= '9'; });
		}

		// Fatal faults, which the toolkit reports by throwing out of the parse.
		[[noreturn]] void fatal(const char* message)
		{
			throw error(message);
		}

		class rdf_parser
		{
		public:
			explicit rdf_parser(node& tree) : _tree(tree)
			{
			}

			void rdf(const xml_node& xml)
			{
				node_element_list(&_tree, xml);
			}

		private:
			node& _tree;

			node* add_child_node(node* parent, const xml_node& xml, const std::string_view value, const bool top)
			{
				if (xml.ns.empty()) return nullptr; // Every element and attribute needs a namespace.

				const auto is_array_parent = is_array(parent->options);
				auto is_array_item = xml.name == "rdf:li";
				const auto is_value_node = xml.name == "rdf:value";
				option_bits child_options = 0;
				std::string child_name = xml.name;

				if (top)
				{
					auto* const schema = find_schema(parent, xml.ns, true);
					schema->options &= ~new_implicit_node;
					parent = schema;

					if (find_alias(xml.name))
					{
						child_options |= is_alias;
						_tree.options |= has_aliases;
					}
				}

				if (is_array_item)
				{
					if (!is_array_parent) return nullptr; // Misplaced rdf:li.
					child_name = array_item_name;
				}
				else if (is_array_parent)
				{
					if (!is_numbered_array_item_name(xml.name)) return nullptr; // Arrays hold only items.
					child_name = array_item_name;
					is_array_item = true;
				}

				if (!(is_array_item || is_value_node))
				{
					if (parent->options & (schema_node | value_is_struct))
					{
						for (const auto& child : parent->children)
						{
							if (child->name == child_name) return nullptr; // A duplicate property or field.
						}
					}
				}

				if (is_value_node)
				{
					if (top || !(parent->options & value_is_struct)) return nullptr; // A misplaced rdf:value.
					parent->options |= has_value_element;
				}

				auto child = std::make_unique<node>(parent, std::move(child_name), std::string(value), child_options);
				auto* const raw = child.get();

				if (!is_value_node || parent->children.empty())
				{
					parent->children.push_back(std::move(child));
				}
				else
				{
					parent->children.insert(parent->children.begin(), std::move(child));
				}

				return raw;
			}

			static node* add_qualifier_node(node* parent, const std::string_view name, const std::string_view value)
			{
				const auto is_lang = name == "xml:lang";
				const auto is_type = name == "rdf:type";

				auto qual = std::make_unique<node>(parent, std::string(name), std::string(value), is_qualifier);
				auto* const raw = qual.get();

				if (!(is_lang || is_type))
				{
					parent->qualifiers.push_back(std::move(qual));
				}
				else if (is_lang)
				{
					parent->qualifiers.insert(parent->qualifiers.begin(), std::move(qual));
					parent->options |= has_lang;
				}
				else
				{
					auto at = parent->qualifiers.begin();
					if (!parent->qualifiers.empty() && (parent->options & has_lang)) ++at;
					parent->qualifiers.insert(at, std::move(qual));
					parent->options |= has_type;
				}

				parent->options |= has_qualifiers;
				return raw;
			}

			static node* add_qualifier_node(node* parent, const xml_node& attr)
			{
				if (attr.ns.empty()) return nullptr;
				return add_qualifier_node(parent, attr.name, attr.value);
			}

			static node* find_qualifier_named(node* parent, const std::string_view name, std::size_t* index = nullptr)
			{
				for (std::size_t i = 0; i < parent->qualifiers.size(); ++i)
				{
					if (parent->qualifiers[i]->name == name)
					{
						if (index) *index = i;
						return parent->qualifiers[i].get();
					}
				}

				return nullptr;
			}

			// The parent is a pseudo-struct holding an rdf:value: the value node's form, value and
			// children are the real ones, and the other fields are qualifiers.
			static void fixup_qualified_node(node* parent)
			{
				auto value_node = std::move(parent->children.front());
				std::vector<std::unique_ptr<node>> others;
				for (std::size_t i = 1; i < parent->children.size(); ++i) others.push_back(std::move(parent->children[i]));
				parent->children.clear();

				std::size_t first = 0;

				if (value_node->options & has_lang)
				{
					if (parent->options & has_lang)
					{
						parent->qualifiers.erase(parent->qualifiers.begin()); // The rdf:value's language wins.
					}

					auto lang = std::move(value_node->qualifiers.front());
					lang->parent = parent;
					parent->options |= has_lang;
					value_node->options &= ~has_lang;
					parent->qualifiers.insert(parent->qualifiers.begin(), std::move(lang));
					first = 1;
				}

				for (auto i = first; i < value_node->qualifiers.size(); ++i)
				{
					auto& qual = value_node->qualifiers[i];
					std::size_t existing = 0;

					if (find_qualifier_named(parent, qual->name, &existing))
					{
						parent->qualifiers.erase(parent->qualifiers.begin() + static_cast<std::ptrdiff_t>(existing));
					}

					qual->parent = parent;
					parent->qualifiers.push_back(std::move(qual));
				}

				value_node->qualifiers.clear();

				for (auto& qual : others)
				{
					const auto is_lang = qual->name == "xml:lang";

					if (find_qualifier_named(parent, qual->name))
					{
						continue; // A duplicate qualifier is dropped.
					}

					qual->options |= is_qualifier;
					qual->parent = parent;

					if (is_lang) parent->options |= has_lang;
					else if (qual->name == "rdf:type") parent->options |= has_type;

					if (!is_lang || parent->qualifiers.empty())
					{
						parent->qualifiers.push_back(std::move(qual));
					}
					else
					{
						parent->qualifiers.insert(parent->qualifiers.begin(), std::move(qual));
					}
				}

				if (!parent->qualifiers.empty()) parent->options |= has_qualifiers;

				parent->options &= ~(value_is_struct | has_value_element);
				parent->options |= value_node->options;
				parent->value = std::move(value_node->value);
				parent->children = std::move(value_node->children);

				for (const auto& child : parent->children) child->parent = parent;
			}

			void node_element_list(node* parent, const xml_node& xml)
			{
				for (const auto& child : xml.content)
				{
					if (child->is_whitespace()) continue;
					node_element(parent, *child, true);
				}
			}

			void node_element(node* parent, const xml_node& xml, const bool top)
			{
				const auto term = term_of(xml.name);
				if (term != term_description && term != term_other) return; // Not a node element.
				if (top && term == term_other) return; // A typed node at the top level.

				node_element_attrs(parent, xml, top);
				property_element_list(parent, xml, top);
			}

			void node_element_attrs(node* parent, const xml_node& xml, const bool top)
			{
				unsigned exclusive = 0;
				constexpr unsigned exclusive_mask = (1u << term_id) | (1u << term_node_id) | (1u << term_about);

				for (const auto& attr : xml.attrs)
				{
					const auto term = term_of(attr->name);

					switch (term)
					{
					case term_id:
					case term_node_id:
					case term_about:
						if (exclusive & exclusive_mask) continue; // About, ID and nodeID exclude each other.
						exclusive |= 1u << term;

						if (top && term == term_about && _tree.name.empty())
						{
							_tree.name = attr->value;
						}
						break;

					case term_other:
						add_child_node(parent, *attr, attr->value, top);
						break;

					default:
						continue; // Not a node element attribute.
					}
				}
			}

			void property_element_list(node* parent, const xml_node& xml, const bool top)
			{
				for (const auto& child : xml.content)
				{
					if (child->is_whitespace()) continue;
					if (child->kind != xml_kind::element) continue; // Expected a property element.
					property_element(parent, *child, top);
				}
			}

			void property_element(node* parent, const xml_node& xml, const bool top)
			{
				if (!is_property_element_name(term_of(xml.name))) return;

				if (xml.attrs.size() > 3)
				{
					// Only an empty property element can have more than three attributes.
					empty_property_element(parent, xml, top);
					return;
				}

				const xml_node* deciding = nullptr;

				for (const auto& attr : xml.attrs)
				{
					if (attr->name != "xml:lang" && attr->name != "rdf:ID")
					{
						deciding = attr.get();
						break;
					}
				}

				if (deciding)
				{
					if (deciding->name == "rdf:datatype")
					{
						literal_property_element(parent, xml, top);
					}
					else if (deciding->name != "rdf:parseType")
					{
						empty_property_element(parent, xml, top);
					}
					else if (deciding->value == "Resource")
					{
						parse_type_resource_property_element(parent, xml, top);
					}

					// parseType Literal, Collection and anything else are not XMP.
					return;
				}

				if (xml.content.empty())
				{
					empty_property_element(parent, xml, top);
					return;
				}

				const auto all_text = std::all_of(xml.content.begin(), xml.content.end(),
				                                  [](const auto& c) { return c->kind == xml_kind::text; });

				if (all_text)
				{
					literal_property_element(parent, xml, top);
				}
				else
				{
					resource_property_element(parent, xml, top);
				}
			}

			void resource_property_element(node* parent, const xml_node& xml, const bool top)
			{
				if (top && xml.name == "iX:changes") return; // Old "punchcard" chaff.

				auto* const compound = add_child_node(parent, xml, {}, top);
				if (!compound) return;

				for (const auto& attr : xml.attrs)
				{
					if (attr->name == "xml:lang") add_qualifier_node(compound, *attr);
				}

				auto child = xml.content.begin();
				while (child != xml.content.end() && (*child)->is_whitespace()) ++child;

				// With no element child the property stays as it was added: simple and empty.
				if (child == xml.content.end()) return;
				if ((*child)->kind != xml_kind::element) return;

				const auto& inner = **child;

				if (inner.name == "rdf:Bag")
				{
					compound->options |= value_is_array;
				}
				else if (inner.name == "rdf:Seq")
				{
					compound->options |= value_is_array | array_is_ordered;
				}
				else if (inner.name == "rdf:Alt")
				{
					compound->options |= value_is_array | array_is_ordered | array_is_alternate;
				}
				else
				{
					if (inner.name != "rdf:Description")
					{
						// A typed node, recorded as an rdf:type qualifier holding the type's URI.
						const auto colon = inner.name.find(':');
						if (colon == std::string::npos) return;
						auto type_name = inner.ns + inner.name.substr(colon + 1);
						auto* const type = add_qualifier_node(compound, "rdf:type", type_name);
						type->options |= value_is_uri;
					}

					compound->options |= value_is_struct;
				}

				node_element(compound, inner, false);

				if (compound->options & has_value_element)
				{
					fixup_qualified_node(compound);
				}
				else if (compound->options & array_is_alternate)
				{
					detect_alt_text(compound);
				}
			}

			void literal_property_element(node* parent, const xml_node& xml, const bool top)
			{
				auto* const child = add_child_node(parent, xml, {}, top);
				if (!child) return;

				for (const auto& attr : xml.attrs)
				{
					if (attr->name == "xml:lang") add_qualifier_node(child, *attr);
				}

				std::size_t size = 0;
				for (const auto& c : xml.content) size += c->value.size();
				child->value.reserve(size);
				for (const auto& c : xml.content) child->value += c->value;
			}

			void parse_type_resource_property_element(node* parent, const xml_node& xml, const bool top)
			{
				auto* const structure = add_child_node(parent, xml, {}, top);
				if (!structure) return;
				structure->options |= value_is_struct;

				for (const auto& attr : xml.attrs)
				{
					if (attr->name == "xml:lang") add_qualifier_node(structure, *attr);
				}

				property_element_list(structure, xml, false);

				if (structure->options & has_value_element) fixup_qualified_node(structure);
			}

			// <ns:Prop1/>, <ns:Prop2 rdf:resource="URI"/>, <ns:Prop3 rdf:value="..." ns:Qual="..."/> and
			// <ns:Prop4 ns:Field1="..." ns:Field2="..."/>.
			void empty_property_element(node* parent, const xml_node& xml, const bool top)
			{
				auto has_property_attrs = false;
				auto has_resource_attr = false;
				auto has_node_id_attr = false;
				auto has_value_attr = false;
				const xml_node* value_node = nullptr;

				if (!xml.content.empty()) return; // Nested content is not allowed here.

				for (const auto& attr : xml.attrs)
				{
					switch (term_of(attr->name))
					{
					case term_id:
						break;

					case term_resource:
						if (has_node_id_attr || has_value_attr) return;
						has_resource_attr = true;
						value_node = attr.get();
						break;

					case term_node_id:
						if (has_resource_attr) return;
						has_node_id_attr = true;
						break;

					case term_other:
						if (attr->name == "rdf:value")
						{
							if (has_resource_attr) return;
							has_value_attr = true;
							value_node = attr.get();
						}
						else if (attr->name != "xml:lang")
						{
							has_property_attrs = true;
						}
						break;

					default:
						return; // An unrecognized attribute.
					}
				}

				auto* const child = add_child_node(parent, xml, {}, top);
				if (!child) return;
				auto child_is_struct = false;

				if (has_value_attr || has_resource_attr)
				{
					child->value = value_node->value;
					if (!has_value_attr) child->options |= value_is_uri;
				}
				else if (has_property_attrs)
				{
					child->options |= value_is_struct;
					child_is_struct = true;
				}

				for (const auto& attr : xml.attrs)
				{
					if (attr.get() == value_node) continue;

					switch (term_of(attr->name))
					{
					case term_id:
					case term_node_id:
						break;

					case term_resource:
						add_qualifier_node(child, *attr);
						break;

					case term_other:
						if (!child_is_struct || attr->name == "xml:lang")
						{
							add_qualifier_node(child, *attr);
						}
						else
						{
							add_child_node(child, *attr, attr->value, false);
						}
						break;

					default:
						break;
					}
				}
			}
		};

		// -----------------------------------------------------------------------------------------
		// Normalization

		void normalize_dc_arrays(node& tree)
		{
			auto* const dc = find_schema(&tree, ns::dc, false);
			if (!dc) return;

			for (auto& prop : dc->children)
			{
				if (!is_simple(prop->options)) continue;

				option_bits form = 0;
				const auto& name = prop->name;

				if (name == "dc:creator" || name == "dc:date")
				{
					form = array_is_ordered;
				}
				else if (name == "dc:description" || name == "dc:rights" || name == "dc:title")
				{
					form = array_is_alt_text;
				}
				else if (name == "dc:contributor" || name == "dc:language" || name == "dc:publisher" ||
					name == "dc:relation" || name == "dc:subject" || name == "dc:type")
				{
					form = value_is_array;
				}

				if (form == 0) continue;

				form = verify_set_options(form, false);
				auto array = std::make_unique<node>(dc, name, std::string(), form);

				// An empty value becomes an empty array, not an array holding an empty item.
				if (!prop->value.empty())
				{
					auto item = std::move(prop);
					item->parent = array.get();
					item->name = array_item_name;

					if (is_alt_text(form) && !(item->options & has_lang))
					{
						item->qualifiers.insert(item->qualifiers.begin(),
						                        std::make_unique<node>(item.get(), "xml:lang", "x-default", is_qualifier));
						item->options |= has_qualifiers | has_lang;
					}

					array->children.push_back(std::move(item));
				}

				prop = std::move(array);
			}
		}

		void transplant_array_item_alias(node* old_schema, const std::size_t index, node* new_parent)
		{
			auto child = std::move(old_schema->children[index]);
			old_schema->children.erase(old_schema->children.begin() + static_cast<std::ptrdiff_t>(index));

			if (is_alt_text(new_parent->options))
			{
				if (child->options & has_lang) fatal("Alias to x-default already has a language qualifier");

				child->options |= has_qualifiers | has_lang;
				child->qualifiers.insert(child->qualifiers.begin(),
				                         std::make_unique<node>(child.get(), "xml:lang", "x-default", is_qualifier));
			}

			child->name = array_item_name;
			child->parent = new_parent;
			new_parent->children.insert(new_parent->children.begin(), std::move(child));
		}

		// An alias written explicitly moves to its actual property, unless the actual already exists,
		// in which case the alias is dropped.
		void move_explicit_aliases(node& tree)
		{
			tree.options &= ~has_aliases;

			std::size_t schema_index = 0;

			while (schema_index < tree.children.size())
			{
				auto* const schema = tree.children[schema_index].get();
				std::size_t prop_index = 0;

				while (prop_index < schema->children.size())
				{
					auto* const prop = schema->children[prop_index].get();

					if (!(prop->options & is_alias))
					{
						++prop_index;
						continue;
					}

					prop->options &= ~is_alias;

					const auto& base = *find_alias(prop->name);
					const auto array_options = base[1].options & array_form_mask;

					auto* const base_schema = find_schema(&tree, base[0].step, true);
					base_schema->options &= ~new_implicit_node;

					node* base_node = nullptr;
					for (const auto& c : base_schema->children)
					{
						if (c->name == base[1].step)
						{
							base_node = c.get();
							break;
						}
					}

					if (!base_node)
					{
						if (base.size() == 2)
						{
							auto moved = std::move(schema->children[prop_index]);
							schema->children.erase(schema->children.begin() + static_cast<std::ptrdiff_t>(prop_index));
							moved->name = base[1].step;
							moved->parent = base_schema;
							base_schema->children.push_back(std::move(moved));
						}
						else
						{
							base_schema->children.push_back(
								std::make_unique<node>(base_schema, base[1].step, std::string(), array_options));
							transplant_array_item_alias(schema, prop_index, base_schema->children.back().get());
						}
					}
					else if (base.size() == 2)
					{
						schema->children.erase(schema->children.begin() + static_cast<std::ptrdiff_t>(prop_index));
					}
					else
					{
						node* item = nullptr;

						if (array_options & array_is_alt_text)
						{
							const auto index = lookup_lang_item(base_node, "x-default");
							if (index != -1) item = base_node->children[static_cast<std::size_t>(index)].get();
						}
						else if (!base_node->children.empty())
						{
							item = base_node->children.front().get();
						}

						if (!item)
						{
							transplant_array_item_alias(schema, prop_index, base_node);
						}
						else
						{
							schema->children.erase(schema->children.begin() + static_cast<std::ptrdiff_t>(prop_index));
						}
					}
				}

				if (!schema->children.empty())
				{
					++schema_index;
				}
				else
				{
					tree.children.erase(tree.children.begin() + static_cast<std::ptrdiff_t>(schema_index));
				}
			}
		}

		struct date_time
		{
			int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
			int nanosecond = 0;
			int tz_sign = 0, tz_hour = 0, tz_minute = 0;
			bool has_time_zone = false;
			bool has_time = false;
		};

		bool gather_int(const std::string_view s, std::size_t& pos, int& value)
		{
			const auto start = pos;
			long long v = 0;

			while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
			{
				v = v * 10 + (s[pos] - '0');
				if (v > 0x7FFFFFFF) return false;
				++pos;
			}

			value = static_cast<int>(v);
			return pos != start;
		}

		// The toolkit's ISO 8601 reader, including its silent clamping of out-of-range parts.
		bool convert_to_date(const std::string_view s, date_time& d)
		{
			if (s.empty()) return false;
			d = {};
			std::size_t pos = 0;
			int temp;
			const auto at = [&s](const std::size_t i) { return i < s.size() ? s[i] : '\0'; };

			const auto time_only = s[0] == 'T' || (s.size() >= 2 && s[1] == ':') || (s.size() >= 3 && s[2] == ':');

			if (!time_only)
			{
				if (s[0] == '-') pos = 1;
				if (!gather_int(s, pos, temp)) return false;
				if (at(pos) != 0 && at(pos) != '-') return false;
				d.year = s[0] == '-' ? -temp : temp;
				if (at(pos) == 0) return true;

				++pos;
				if (!gather_int(s, pos, temp)) return false;
				if (at(pos) != 0 && at(pos) != '-') return false;
				if (d.year != 0 && temp < 1) temp = 1;
				if (temp > 12) temp = 12;
				d.month = temp;
				if (at(pos) == 0) return true;

				++pos;
				if (!gather_int(s, pos, temp)) return false;
				if (at(pos) != 0 && at(pos) != 'T') return false;
				if (temp > 31) temp = 31;
				d.day = temp;
				if (at(pos) == 0) return true;

				if (d.year != 0 || d.month != 0 || d.day != 0)
				{
					if (d.month < 1) d.month = 1;
					if (d.day < 1) d.day = 1;
				}
			}

			if (at(pos) == 'T') ++pos;
			else if (!time_only) return false;

			d.has_time = true;

			if (!gather_int(s, pos, temp) || at(pos) != ':') return false;
			d.hour = std::min(temp, 23);

			++pos;
			if (!gather_int(s, pos, temp)) return false;
			if (at(pos) != ':' && at(pos) != 'Z' && at(pos) != '+' && at(pos) != '-' && at(pos) != 0) return false;
			d.minute = std::min(temp, 59);

			if (at(pos) == ':')
			{
				++pos;
				if (!gather_int(s, pos, temp)) return false;
				if (at(pos) != '.' && at(pos) != 'Z' && at(pos) != '+' && at(pos) != '-' && at(pos) != 0) return false;
				d.second = std::min(temp, 59);

				if (at(pos) == '.')
				{
					++pos;
					const auto digits_start = pos;
					if (!gather_int(s, pos, temp)) return false;
					if (at(pos) != 'Z' && at(pos) != '+' && at(pos) != '-' && at(pos) != 0) return false;
					auto digits = pos - digits_start;
					for (; digits > 9; --digits) temp /= 10;
					for (; digits < 9; ++digits) temp *= 10;
					if (temp >= 1000000000) return false;
					d.nanosecond = temp;
				}
			}

			if (at(pos) == 0) return true;

			d.has_time_zone = true;

			if (at(pos) == 'Z')
			{
				++pos;
			}
			else
			{
				if (at(pos) == '+') d.tz_sign = 1;
				else if (at(pos) == '-') d.tz_sign = -1;
				else return false;

				++pos;
				if (!gather_int(s, pos, temp) || at(pos) != ':' || temp > 23) return false;
				d.tz_hour = temp;
				++pos;
				if (!gather_int(s, pos, temp) || temp > 59) return false;
				d.tz_minute = temp;
			}

			return at(pos) == 0;
		}

		// Formats a full date and time, which is the only form the GPS touch-up produces.
		std::string format_date_time(const date_time& d)
		{
			char buffer[64];

			if (d.nanosecond == 0)
			{
				std::snprintf(buffer, sizeof(buffer), "%.4d-%02d-%02dT%02d:%02d:%02d", d.year, d.month, d.day, d.hour,
				              d.minute, d.second);
			}
			else
			{
				std::snprintf(buffer, sizeof(buffer), "%.4d-%02d-%02dT%02d:%02d:%02d.%09d", d.year, d.month, d.day,
				              d.hour, d.minute, d.second, d.nanosecond);
				for (auto i = std::char_traits<char>::length(buffer) - 1; buffer[i] == '0'; --i) buffer[i] = 0;
			}

			std::string result = buffer;

			if (d.has_time_zone)
			{
				if (d.tz_sign == 0)
				{
					result += 'Z';
				}
				else
				{
					std::snprintf(buffer, sizeof(buffer), "%c%02d:%02d", d.tz_sign < 0 ? '-' : '+', d.tz_hour, d.tz_minute);
					result += buffer;
				}
			}

			return result;
		}

		// Some writers store exif:GPSTimeStamp as a time only. The date comes from the capture date.
		void fix_gps_time_stamp(node* exif_schema, node* gps)
		{
			date_time stamp;
			if (!convert_to_date(gps->value, stamp)) return;
			if (stamp.year != 0 || stamp.month != 0 || stamp.day != 0) return;

			node* other = nullptr;
			for (const auto& c : exif_schema->children)
			{
				if (c->name == "exif:DateTimeOriginal") other = c.get();
			}

			if (!other)
			{
				for (const auto& c : exif_schema->children)
				{
					if (c->name == "exif:DateTimeDigitized") other = c.get();
				}
			}

			if (!other) return;

			date_time other_date;
			if (!convert_to_date(other->value, other_date)) return;

			stamp.year = other_date.year;
			stamp.month = other_date.month;
			stamp.day = other_date.day;

			// The toolkit fixes up an out-of-range month or day before formatting.
			if (stamp.month == 0) stamp.month = 1;
			if (stamp.day == 0) stamp.day = 1;
			gps->value = format_date_time(stamp);
		}

		// Early WAV support put a legacy copyright in xmpDM:copyright. It belongs in
		// dc:rights['x-default'], after a double linefeed.
		void migrate_audio_copyright(meta& owner, node* dm_copyright)
		{
			try
			{
				auto dm_value = dm_copyright->value;
				constexpr std::string_view double_lf = "\n\n";

				auto* const dc = find_schema(&owner.tree(), ns::dc, true);
				node* rights = nullptr;
				for (const auto& c : dc->children)
				{
					if (c->name == "dc:rights") rights = c.get();
				}

				if (!rights || rights->children.empty())
				{
					dm_value.insert(0, double_lf);
					owner.set_localized_text(ns::dc, "rights", "", "x-default", dm_value);
				}
				else
				{
					auto index = lookup_lang_item(rights, "x-default");

					if (index < 0)
					{
						const auto first = rights->children[0]->value;
						owner.set_localized_text(ns::dc, "rights", "", "x-default", first);
						index = lookup_lang_item(rights, "x-default");
					}

					auto& default_value = rights->children[static_cast<std::size_t>(index)]->value;
					const auto lf = default_value.find(double_lf);

					if (lf == std::string::npos)
					{
						if (dm_value != default_value)
						{
							default_value += double_lf;
							default_value += dm_value;
						}
					}
					else if (default_value.compare(lf + 2, std::string::npos, dm_value) != 0)
					{
						default_value.replace(lf + 2, std::string::npos, dm_value);
					}
				}

				owner.delete_property(ns::dm, "copyright");
			}
			catch (...)
			{
				// A malformed dc:rights must not stop the other touch-ups.
			}
		}

		// Each item of an alt-text array must be simple and have a language.
		void repair_alt_text(node& tree, const std::string_view schema_uri, const std::string_view array_name)
		{
			auto* const schema = find_schema(&tree, schema_uri, false);
			if (!schema) return;

			node* array = nullptr;
			for (const auto& c : schema->children)
			{
				if (c->name == array_name) array = c.get();
			}

			if (!array || is_alt_text(array->options)) return;
			if (!is_array(array->options)) return;

			array->options |= array_is_ordered | array_is_alternate | array_is_alt_text;

			for (auto i = static_cast<int>(array->children.size()) - 1; i >= 0; --i)
			{
				auto& child = array->children[static_cast<std::size_t>(i)];

				if (!is_simple(child->options))
				{
					array->children.erase(array->children.begin() + i);
				}
				else if (!(child->options & has_lang))
				{
					if (child->value.empty())
					{
						array->children.erase(array->children.begin() + i);
					}
					else
					{
						child->qualifiers.insert(child->qualifiers.begin(),
						                         std::make_unique<node>(child.get(), "xml:lang", "x-repair", is_qualifier));
						child->options |= has_qualifiers | has_lang;
					}
				}
			}
		}

		bool looks_like_uuid(const std::string& name)
		{
			if (name.starts_with("uuid:")) return true;
			if (name.size() != 36) return false;

			for (std::size_t i = 0; i < 36; ++i)
			{
				const auto c = name[i];
				if (c == '-')
				{
					if (i == 8 || i == 13 || i == 18 || i == 23) continue;
					return false;
				}
				if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) continue;
				return false;
			}

			return true;
		}

		void touch_up_data_model(meta& owner)
		{
			auto& tree = owner.tree();

			if (auto* const exif = find_schema(&tree, ns::exif, false))
			{
				node* gps = nullptr;
				node* comment = nullptr;

				for (const auto& c : exif->children)
				{
					if (c->name == "exif:GPSTimeStamp") gps = c.get();
					else if (c->name == "exif:UserComment") comment = c.get();
				}

				if (gps) fix_gps_time_stamp(exif, gps);

				// A simple exif:UserComment becomes the x-default item of an alt-text array.
				if (comment && is_simple(comment->options))
				{
					auto item = std::make_unique<node>(comment, std::string(array_item_name), comment->value,
					                                   comment->options);
					item->qualifiers.swap(comment->qualifiers);
					for (const auto& q : item->qualifiers) q->parent = item.get();

					if (!(item->options & has_lang))
					{
						item->qualifiers.insert(item->qualifiers.begin(),
						                        std::make_unique<node>(item.get(), "xml:lang", "x-default", is_qualifier));
						item->options |= has_qualifiers | has_lang;
					}

					comment->value.clear();
					comment->options = array_form_mask;
					comment->children.push_back(std::move(item));
				}
			}

			if (auto* const dm = find_schema(&tree, ns::dm, false))
			{
				node* copyright = nullptr;
				for (const auto& c : dm->children)
				{
					if (c->name == "xmpDM:copyright") copyright = c.get();
				}

				if (copyright) migrate_audio_copyright(owner, copyright);
			}

			if (auto* const dc = find_schema(&tree, ns::dc, false))
			{
				for (const auto& c : dc->children)
				{
					if (c->name == "dc:subject")
					{
						c->options &= ~(array_is_ordered | array_is_alternate | array_is_alt_text);
					}
				}
			}

			repair_alt_text(tree, ns::dc, "dc:description");
			repair_alt_text(tree, ns::dc, "dc:rights");
			repair_alt_text(tree, ns::dc, "dc:title");
			repair_alt_text(tree, ns::xmp_rights, "xmpRights:UsageTerms");
			repair_alt_text(tree, ns::exif, "exif:UserComment");

			// An old instance ID in rdf:about moves to xmpMM:InstanceID.
			if (!tree.name.empty() && looks_like_uuid(tree.name))
			{
				auto* const id = find_node(&tree, expand_path(ns::xmp_mm, "InstanceID"), true);
				id->options = 0;
				id->value = tree.name;
				id->remove_children();
				id->remove_qualifiers();
				tree.name.clear();
			}
		}
	}

	void process_rdf(node& tree, const xml_node& rdf)
	{
		rdf_parser parser(tree);
		parser.rdf(rdf);
	}

	void normalize_parsed(meta& owner)
	{
		auto& tree = owner.tree();

		normalize_dc_arrays(tree);
		if (tree.options & has_aliases) move_explicit_aliases(tree);
		touch_up_data_model(owner);

		std::erase_if(tree.children, [](const auto& schema) { return schema->children.empty(); });
	}
}
