// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Internal types of the XMP implementation: the XMP node tree, the XML tree the parser
// builds, the namespace and alias tables, and expanded paths. Not part of the public API.

#pragma once

#include "libfiles/xmp.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::xmp::detail
{
	// Options used only while building or searching the tree, never reported.
	inline constexpr option_bits new_implicit_node = 0x00008000;
	inline constexpr option_bits has_value_element = 0x10000000;

	// The name of every array item node, as in the toolkit.
	inline constexpr std::string_view array_item_name = "[]";

	struct node
	{
		node(node* parent_node, std::string node_name, std::string node_value, const option_bits node_options) :
			name(std::move(node_name)), value(std::move(node_value)), options(node_options), parent(parent_node)
		{
		}

		// A schema node's name is its namespace URI and its value its prefix. Properties and fields are
		// named "prefix:local", array items "[]".
		std::string name;
		std::string value;
		option_bits options = 0;
		node* parent = nullptr;
		std::vector<std::unique_ptr<node>> children;
		std::vector<std::unique_ptr<node>> qualifiers;

		std::unique_ptr<node> clone(node* new_parent) const;
		void remove_children() { children.clear(); }
		void remove_qualifiers();
	};

	// ---------------------------------------------------------------------------------------------
	// XML

	enum class xml_kind : unsigned char
	{
		element,
		attribute,
		text,
		processing_instruction,
	};

	struct xml_node
	{
		xml_kind kind = xml_kind::element;
		std::string ns; // The namespace URI; empty when the name has none.
		std::string name; // "prefix:local" with the registered prefix, or a bare name.
		std::string value; // Attribute value, character data, or processing instruction data.
		xml_node* parent = nullptr;
		std::vector<std::unique_ptr<xml_node>> attrs;
		std::vector<std::unique_ptr<xml_node>> content;

		bool is_whitespace() const;
	};

	struct xml_document
	{
		xml_node root; // An unnamed element holding the document's top-level content.
		const xml_node* last_rdf = nullptr;
		std::size_t rdf_count = 0;
	};

	// Builds the XML tree for an XMP packet. Like the toolkit's Expat adapter, it stops at the first
	// fault and keeps everything built before it, so it never fails.
	void parse_xml(std::string_view bytes, xml_document& doc);

	// ---------------------------------------------------------------------------------------------
	// Namespaces and aliases

	bool registry_prefix(std::string_view uri, std::string* prefix);
	bool registry_uri(std::string_view prefix, std::string* uri);

	enum step_kind : unsigned
	{
		schema_step = 0,
		struct_field_step = 1,
		qualifier_step = 2,
		array_index_step = 3,
		array_last_step = 4,
		qual_selector_step = 5,
		field_selector_step = 6,
		step_kind_mask = 0x0F,
		step_is_alias = 0x10,
	};

	struct path_step
	{
		std::string step;
		option_bits options = 0; // The step kind, the alias flag, and for an alias's actual the array form.
	};

	using expanded_path = std::vector<path_step>;

	inline unsigned kind_of(const path_step& s) { return s.options & step_kind_mask; }

	// The actual path an alias names, or null for a name that is not an alias. Both are
	// "prefix:local" names.
	const expanded_path* find_alias(std::string_view alias_name);

	expanded_path expand_path(std::string_view schema, std::string_view path);
	std::string compose_array_item_path(std::string_view schema, std::string_view array, int index);

	node* find_schema(node* tree, std::string_view uri, bool create);
	node* find_child(node* parent, std::string_view name, bool create);
	node* find_qualifier(node* parent, std::string_view name, bool create);
	int lookup_lang_item(const node* array, std::string_view lang);
	node* find_node(node* tree, const expanded_path& path, bool create, option_bits leaf_options = 0);
	const node* find_const_node(const node* tree, const expanded_path& path);
	void delete_node(node* n);
	void delete_empty_schema(node* schema);

	void normalize_lang_value(std::string& value);
	void normalize_lang_array(node* array);
	void detect_alt_text(node* array);
	option_bits verify_set_options(option_bits options, bool has_value);
	void set_node_value(node* n, std::string_view value);

	// RDF to XMP, then the toolkit's normalization of the result.
	void process_rdf(node& tree, const xml_node& rdf);
	void normalize_parsed(meta& owner);
}
