// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The meta class: parsing a packet into the tree, reading and changing properties with the
// XMP Toolkit's rules for paths, aliases, arrays and localized text, and iterating the tree in the
// toolkit's order.

#include "xmp_internal.h"

#include <charconv>
#include <cstdlib>
#include <string>

namespace libfiles::xmp
{
	using namespace detail;

	namespace
	{
		bool is_xmpmeta(const xml_node& n)
		{
			return n.kind == xml_kind::element && (n.name == "x:xmpmeta" || n.name == "x:xapmeta");
		}

		// The first x:xmpmeta among several candidate roots, else the first bare rdf:RDF, searching
		// breadth first so a higher candidate beats a deeper one written earlier.
		const xml_node* pick_best_root(const xml_node& parent)
		{
			for (const auto& child : parent.content)
			{
				if (is_xmpmeta(*child)) return pick_best_root(*child);
			}

			for (const auto& child : parent.content)
			{
				if (child->kind == xml_kind::element && child->name == "rdf:RDF") return child.get();
			}

			for (const auto& child : parent.content)
			{
				if (const auto* const found = pick_best_root(*child)) return found;
			}

			return nullptr;
		}

		// Shared by set_property and the array functions once the node is found or created.
		void set_node(node* n, const std::string_view* value, option_bits options)
		{
			if (options & delete_existing)
			{
				options &= ~delete_existing;
				n->options = options;
				n->value.clear();
				n->remove_children();
				n->qualifiers.clear();
			}

			n->options |= options;

			if (value)
			{
				if (n->options & composite_mask) throw error("Composite nodes can't have values");
				set_node_value(n, *value);
			}
			else
			{
				if (!n->value.empty()) throw error("Composite nodes can't have values");
				if ((n->options & composite_mask) && (options & composite_mask) != (n->options & composite_mask))
				{
					throw error("Requested and existing composite form mismatch");
				}
				n->remove_children();
			}
		}

		enum class lang_match
		{
			no_values,
			specific,
			single_generic,
			multiple_generic,
			x_default,
			first_item,
		};

		lang_match choose_localized_text(const node* array, const std::string_view generic_lang,
		                                 const std::string_view specific_lang, const node** item)
		{
			if (!(is_alt_text(array->options) || (array->children.empty() && is_alternate(array->options))))
			{
				throw error("Localized text array is not alt-text");
			}

			if (array->children.empty())
			{
				*item = nullptr;
				return lang_match::no_values;
			}

			for (const auto& c : array->children)
			{
				if (c->options & composite_mask) throw error("Alt-text array item is not simple");
				if (c->qualifiers.empty() || c->qualifiers[0]->name != "xml:lang")
				{
					throw error("Alt-text array item has no language qualifier");
				}
			}

			for (const auto& c : array->children)
			{
				if (c->qualifiers[0]->value == specific_lang)
				{
					*item = c.get();
					return lang_match::specific;
				}
			}

			if (!generic_lang.empty())
			{
				const auto matches_generic = [generic_lang](const std::string& lang)
				{
					return lang.size() >= generic_lang.size() && lang.compare(0, generic_lang.size(), generic_lang) == 0 &&
						(lang.size() == generic_lang.size() || lang[generic_lang.size()] == '-');
				};

				std::size_t i = 0;
				for (; i < array->children.size(); ++i)
				{
					if (matches_generic(array->children[i]->qualifiers[0]->value))
					{
						*item = array->children[i].get();
						break;
					}
				}

				if (i < array->children.size())
				{
					for (++i; i < array->children.size(); ++i)
					{
						if (matches_generic(array->children[i]->qualifiers[0]->value)) return lang_match::multiple_generic;
					}

					return lang_match::single_generic;
				}
			}

			for (const auto& c : array->children)
			{
				if (c->qualifiers[0]->value == "x-default")
				{
					*item = c.get();
					return lang_match::x_default;
				}
			}

			*item = array->children.front().get();
			return lang_match::first_item;
		}

		void append_lang_item(node* array, const std::string_view lang, const std::string_view value)
		{
			auto item = std::make_unique<node>(array, std::string(array_item_name), std::string(),
			                                   has_qualifiers | has_lang);
			auto lang_qual = std::make_unique<node>(item.get(), "xml:lang", std::string(), is_qualifier);

			set_node_value(item.get(), value);
			set_node_value(lang_qual.get(), lang);
			const auto is_default = lang_qual->value == "x-default";
			item->qualifiers.push_back(std::move(lang_qual));

			if (array->children.empty() || !is_default)
			{
				array->children.push_back(std::move(item));
			}
			else
			{
				array->children.insert(array->children.begin(), std::move(item));
			}
		}

		// The toolkit's integer conversion: decimal, or hexadecimal after "0x", with nothing after.
		bool convert_to_int(const std::string_view text, std::int32_t& value)
		{
			if (text.empty()) throw error("Empty convert-from string");

			const std::string s(text);
			const char* p = s.c_str();
			auto base = 10;

			if (s.starts_with("0x"))
			{
				p += 2;
				base = 16;
			}

			while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') ++p;

			char* end = nullptr;
			errno = 0;
			const auto parsed = std::strtol(p, &end, base);
			if (end == p || *end != 0) throw error("Invalid integer string");

			value = static_cast<std::int32_t>(parsed);
			return true;
		}

		void add_offspring(std::vector<property_info>& out, const std::string& schema, const node* n,
		                   const std::string& path)
		{
			for (const auto& qual : n->qualifiers)
			{
				const auto qual_path = path + "/?" + qual->name;
				out.push_back({schema, qual_path, is_simple(qual->options) ? qual->value : std::string(), qual->options});
				add_offspring(out, schema, qual.get(), qual_path);
			}

			for (std::size_t i = 0; i < n->children.size(); ++i)
			{
				const auto* const child = n->children[i].get();
				std::string child_path = path;

				if (is_array(n->options))
				{
					child_path += '[';
					child_path += std::to_string(i + 1);
					child_path += ']';
				}
				else
				{
					if (is_struct(n->options)) child_path += '/';
					child_path += child->name;
				}

				out.push_back({schema, child_path, is_simple(child->options) ? child->value : std::string(), child->options});
				add_offspring(out, schema, child, child_path);
			}
		}
	}

	meta::meta() : _tree(std::make_unique<node>(nullptr, std::string(), std::string(), 0))
	{
	}

	meta::~meta() = default;

	meta::meta(const meta& other) : _tree(other._tree->clone(nullptr))
	{
	}

	meta& meta::operator=(const meta& other)
	{
		if (this != &other) _tree = other._tree->clone(nullptr);
		return *this;
	}

	meta::meta(meta&& other) noexcept : _tree(std::move(other._tree))
	{
		other._tree = std::make_unique<node>(nullptr, std::string(), std::string(), 0);
	}

	meta& meta::operator=(meta&& other) noexcept
	{
		if (this != &other)
		{
			_tree = std::move(other._tree);
			other._tree = std::make_unique<node>(nullptr, std::string(), std::string(), 0);
		}
		return *this;
	}

	void meta::parse(const std::string_view text)
	{
		clear();
		if (text.empty()) return;

		try
		{
			xml_document doc;
			parse_xml(text, doc);

			const auto* root = doc.last_rdf;
			if (doc.rdf_count > 1) root = pick_best_root(doc.root);
			if (!root) return;

			process_rdf(*_tree, *root);
			normalize_parsed(*this);
		}
		catch (...)
		{
			clear();
			throw;
		}
	}

	bool meta::empty() const
	{
		return _tree->children.empty();
	}

	void meta::clear()
	{
		_tree = std::make_unique<node>(nullptr, std::string(), std::string(), 0);
	}

	const std::string& meta::about() const
	{
		return _tree->name;
	}

	bool meta::get_property(const std::string_view schema, const std::string_view path, std::string* value,
	                        option_bits* options) const
	{
		const auto* const n = find_const_node(_tree.get(), expand_path(schema, path));
		if (!n) return false;
		if (value) *value = n->value;
		if (options) *options = n->options;
		return true;
	}

	bool meta::exists(const std::string_view schema, const std::string_view path) const
	{
		return find_const_node(_tree.get(), expand_path(schema, path)) != nullptr;
	}

	bool meta::get_property_int(const std::string_view schema, const std::string_view path, std::int32_t* value,
	                            option_bits* options) const
	{
		std::string text;
		option_bits found_options = 0;
		if (!get_property(schema, path, &text, &found_options)) return false;
		if (found_options & composite_mask) throw error("Property must be simple");

		std::int32_t parsed = 0;
		convert_to_int(text, parsed);
		if (value) *value = parsed;
		if (options) *options = found_options;
		return true;
	}

	int meta::count_array_items(const std::string_view schema, const std::string_view array) const
	{
		const auto* const n = find_const_node(_tree.get(), expand_path(schema, array));
		if (!n) return 0;
		if (!is_array(n->options)) throw error("The named property is not an array");
		return static_cast<int>(n->children.size());
	}

	bool meta::get_array_item(const std::string_view schema, const std::string_view array, const int index,
	                          std::string* value, option_bits* options) const
	{
		if (index <= 0) throw error("Array index must be larger than zero");
		return get_property(schema, compose_array_item_path(schema, array, index), value, options);
	}

	bool meta::get_localized_text(const std::string_view schema, const std::string_view array,
	                              const std::string_view generic_lang, const std::string_view specific_lang,
	                              std::string* actual_lang, std::string* value, option_bits* options) const
	{
		std::string generic(generic_lang), specific(specific_lang);
		normalize_lang_value(generic);
		normalize_lang_value(specific);

		const auto* const n = find_const_node(_tree.get(), expand_path(schema, array));
		if (!n) return false;

		const node* item = nullptr;
		if (choose_localized_text(n, generic, specific, &item) == lang_match::no_values) return false;

		if (actual_lang) *actual_lang = item->qualifiers[0]->value;
		if (value) *value = item->value;
		if (options) *options = item->options;
		return true;
	}

	void meta::set_property(const std::string_view schema, const std::string_view path, const std::string_view value,
	                        option_bits options)
	{
		options = verify_set_options(options, true);
		auto* const n = find_node(_tree.get(), expand_path(schema, path), true, options);
		if (!n) throw error("Specified property does not exist");
		set_node(n, &value, options);
	}

	void meta::set_composite(const std::string_view schema, const std::string_view path, option_bits options)
	{
		options = verify_set_options(options, false);
		auto* const n = find_node(_tree.get(), expand_path(schema, path), true, options);
		if (!n) throw error("Specified property does not exist");
		set_node(n, nullptr, options);
	}

	void meta::set_property_int(const std::string_view schema, const std::string_view path, const std::int32_t value,
	                            const option_bits options)
	{
		set_property(schema, path, std::to_string(value), options);
	}

	void meta::set_localized_text(const std::string_view schema, const std::string_view array,
	                              const std::string_view generic_lang, const std::string_view specific_lang,
	                              const std::string_view value)
	{
		std::string generic(generic_lang), specific(specific_lang);
		normalize_lang_value(generic);
		normalize_lang_value(specific);

		auto* const n = find_node(_tree.get(), expand_path(schema, array), true,
		                          value_is_array | array_is_ordered | array_is_alternate);
		if (!n) throw error("Failed to find or create array node");

		if (!is_alt_text(n->options))
		{
			if (n->children.empty() && is_alternate(n->options))
			{
				n->options |= array_is_alt_text;
			}
			else
			{
				throw error("Localized text array is not alt-text");
			}
		}

		// The x-default item, if any, goes first.
		node* xd_item = nullptr;
		auto have_x_default = false;

		for (std::size_t i = 0; i < n->children.size(); ++i)
		{
			const auto& c = n->children[i];
			if (c->qualifiers.empty() || c->qualifiers[0]->name != "xml:lang") throw error("Language qualifier must be first");

			if (c->qualifiers[0]->value == "x-default")
			{
				xd_item = c.get();
				have_x_default = true;
				if (i != 0) std::swap(n->children[0], n->children[i]);
				break;
			}
		}

		const node* chosen = nullptr;
		const auto match = choose_localized_text(n, generic, specific, &chosen);
		auto* const item = const_cast<node*>(chosen);
		const auto specific_x_default = specific == "x-default";

		switch (match)
		{
		case lang_match::no_values:
			append_lang_item(n, "x-default", value);
			have_x_default = true;
			if (!specific_x_default) append_lang_item(n, specific, value);
			break;

		case lang_match::specific:
			if (!specific_x_default)
			{
				if (xd_item && have_x_default && xd_item != item && xd_item->value == item->value)
				{
					set_node_value(xd_item, value);
				}
				set_node_value(item, value);
			}
			else
			{
				const auto old_default = xd_item->value;

				for (const auto& c : n->children)
				{
					if (c.get() == xd_item || c->value != old_default) continue;
					set_node_value(c.get(), value);
				}

				set_node_value(xd_item, value);
			}
			break;

		case lang_match::single_generic:
			if (xd_item && have_x_default && xd_item != item && xd_item->value == item->value)
			{
				set_node_value(xd_item, value);
			}
			set_node_value(item, value);
			break;

		case lang_match::multiple_generic:
			append_lang_item(n, specific, value);
			if (specific_x_default) have_x_default = true;
			break;

		case lang_match::x_default:
			if (n->children.size() == 1) set_node_value(xd_item, value);
			append_lang_item(n, specific, value);
			break;

		case lang_match::first_item:
			append_lang_item(n, specific, value);
			if (specific_x_default) have_x_default = true;
			break;
		}

		if (!have_x_default && n->children.size() == 1)
		{
			append_lang_item(n, "x-default", value);
		}
	}

	void meta::append_array_item(const std::string_view schema, const std::string_view array, option_bits array_options,
	                             const std::string_view value, const option_bits item_options)
	{
		array_options = verify_set_options(array_options, false);
		if (array_options & ~array_form_mask) throw error("Only array form flags allowed for arrayOptions");

		const auto path = expand_path(schema, array);
		auto* n = find_node(_tree.get(), path, false);

		if (n)
		{
			if (!is_array(n->options)) throw error("The named property is not an array");
		}
		else
		{
			if (array_options == 0) throw error("Explicit arrayOptions required to create new array");
			n = find_node(_tree.get(), path, true, array_options);
			if (!n) throw error("Failure creating array node");
		}

		const auto options = verify_set_options(item_options, true);
		n->children.push_back(std::make_unique<node>(n, std::string(array_item_name), std::string(), 0));
		set_node(n->children.back().get(), &value, options);
	}

	void meta::delete_property(const std::string_view schema, const std::string_view path)
	{
		auto* const n = find_node(_tree.get(), expand_path(schema, path), false);
		if (!n) return;

		auto* const parent = n->parent;
		const auto qualifier = (n->options & is_qualifier) != 0;
		delete_node(n);
		if (!qualifier) delete_empty_schema(parent);
	}

	std::vector<property_info> meta::properties() const
	{
		std::vector<property_info> result;

		for (const auto& schema : _tree->children)
		{
			result.push_back({schema->name, std::string(), std::string(), schema->options});

			for (const auto& prop : schema->children)
			{
				result.push_back({schema->name, prop->name, is_simple(prop->options) ? prop->value : std::string(),
				                  prop->options});
				add_offspring(result, schema->name, prop.get(), prop->name);
			}
		}

		return result;
	}
}
