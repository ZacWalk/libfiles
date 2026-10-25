// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The shared namespace table, the toolkit's standard aliases, path expansion, and the
// primitives that find, create and remove nodes. Every rule here follows the XMP Toolkit, because
// the paths and prefixes are what callers and serialized packets see.

#include "xmp_internal.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace libfiles::xmp::detail
{
	namespace
	{
		bool is_name_start_char(const unsigned char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
		}

		bool is_name_char(const unsigned char c)
		{
			return is_name_start_char(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
		}

		void verify_simple_name(const std::string_view name)
		{
			if (name.empty() || !is_name_start_char(static_cast<unsigned char>(name[0])))
			{
				throw error("Bad XML name");
			}

			for (const auto c : name)
			{
				if (!is_name_char(static_cast<unsigned char>(c))) throw error("Bad XML name");
			}
		}

		struct namespace_table
		{
			std::shared_mutex lock;
			std::unordered_map<std::string, std::string> uri_to_prefix;
			std::unordered_map<std::string, std::string> prefix_to_uri;

			namespace_table()
			{
				const std::pair<std::string_view, std::string_view> standard[] = {
					{ns::xml, "xml"},
					{ns::rdf, "rdf"},
					{ns::dc, "dc"},
					{ns::xmp, "xmp"},
					{ns::pdf, "pdf"},
					{ns::photoshop, "photoshop"},
					{ns::ps_album, "album"},
					{ns::exif, "exif"},
					{ns::exif_aux, "aux"},
					{ns::exif_ex, "exifEX"},
					{ns::tiff, "tiff"},
					{ns::png, "png"},
					{ns::jpeg, "jpeg"},
					{ns::jp2k, "jp2k"},
					{ns::camera_raw, "crs"},
					{ns::asf, "asf"},
					{ns::wav, "wav"},
					{ns::stock_photo, "bmsp"},
					{ns::creator_atom, "creatorAtom"},
					{ns::xmp_rights, "xmpRights"},
					{ns::xmp_mm, "xmpMM"},
					{ns::xmp_bj, "xmpBJ"},
					{ns::xmp_note, "xmpNote"},
					{ns::dm, "xmpDM"},
					{ns::script, "xmpScript"},
					{ns::bwf, "bext"},
					{ns::aes_cart, "AEScart"},
					{ns::riff_info, "riffinfo"},
					{ns::xmp_text, "xmpT"},
					{ns::xmp_paged_file, "xmpTPg"},
					{ns::xmp_graphics, "xmpG"},
					{ns::xmp_image, "xmpGImg"},
					{ns::xmp_font, "stFnt"},
					{ns::xmp_dimensions, "stDim"},
					{ns::xmp_resource_event, "stEvt"},
					{ns::xmp_resource_ref, "stRef"},
					{ns::xmp_version, "stVer"},
					{ns::xmp_job, "stJob"},
					{ns::xmp_manifest_item, "stMfs"},
					{ns::xmp_identifier_qual, "xmpidq"},
					{ns::iptc_core, "Iptc4xmpCore"},
					{ns::iptc_ext, "Iptc4xmpExt"},
					{ns::dicom, "DICOM"},
					{ns::plus, "plus"},
					{ns::pdfa_schema, "pdfaSchema"},
					{ns::pdfa_property, "pdfaProperty"},
					{ns::pdfa_type, "pdfaType"},
					{ns::pdfa_field, "pdfaField"},
					{ns::pdfa_id, "pdfaid"},
					{ns::pdfa_extension, "pdfaExtension"},
					{ns::pdfx, "pdfx"},
					{ns::pdfx_id, "pdfxid"},
					{ns::x, "x"},
					{ns::ix, "iX"},
					{ns::ixml, "iXML"},
					{ns::microsoft_photo, "MicrosoftPhoto"},
				};

				for (const auto& [uri, prefix] : standard)
				{
					define(uri, prefix);
				}
			}

			// Caller holds the write lock.
			std::string define(const std::string_view uri, const std::string_view suggested)
			{
				const auto found = uri_to_prefix.find(std::string(uri));
				if (found != uri_to_prefix.end()) return found->second;

				std::string base(suggested);
				if (!base.empty() && base.back() == ':') base.pop_back();

				auto unique = base + ':';

				for (auto suffix = 1; prefix_to_uri.contains(unique); ++suffix)
				{
					unique = base + '_' + std::to_string(suffix) + "_:";
				}

				uri_to_prefix.emplace(std::string(uri), unique);
				prefix_to_uri.emplace(unique, std::string(uri));
				return unique;
			}
		};

		namespace_table& namespaces()
		{
			static namespace_table table;
			return table;
		}

		struct alias_table
		{
			std::map<std::string, expanded_path, std::less<>> map;

			alias_table()
			{
				struct entry
				{
					std::string_view alias_ns;
					std::string_view alias_prop;
					std::string_view actual_ns;
					std::string_view actual_prop;
					option_bits array_form;
				};

				const entry standard[] = {
					{ns::xmp, "Author", ns::dc, "creator", array_is_ordered},
					{ns::xmp, "Authors", ns::dc, "creator", 0},
					{ns::xmp, "Description", ns::dc, "description", 0},
					{ns::xmp, "Format", ns::dc, "format", 0},
					{ns::xmp, "Keywords", ns::dc, "subject", 0},
					{ns::xmp, "Locale", ns::dc, "language", 0},
					{ns::xmp, "Title", ns::dc, "title", 0},
					{ns::xmp_rights, "Copyright", ns::dc, "rights", 0},

					{ns::pdf, "Author", ns::dc, "creator", array_is_ordered},
					{ns::pdf, "BaseURL", ns::xmp, "BaseURL", 0},
					{ns::pdf, "CreationDate", ns::xmp, "CreateDate", 0},
					{ns::pdf, "Creator", ns::xmp, "CreatorTool", 0},
					{ns::pdf, "ModDate", ns::xmp, "ModifyDate", 0},
					{ns::pdf, "Subject", ns::dc, "description", array_is_alt_text},
					{ns::pdf, "Title", ns::dc, "title", array_is_alt_text},

					{ns::photoshop, "Author", ns::dc, "creator", array_is_ordered},
					{ns::photoshop, "Caption", ns::dc, "description", array_is_alt_text},
					{ns::photoshop, "Copyright", ns::dc, "rights", array_is_alt_text},
					{ns::photoshop, "Keywords", ns::dc, "subject", 0},
					{ns::photoshop, "Marked", ns::xmp_rights, "Marked", 0},
					{ns::photoshop, "Title", ns::dc, "title", array_is_alt_text},
					{ns::photoshop, "WebStatement", ns::xmp_rights, "WebStatement", 0},

					{ns::tiff, "Artist", ns::dc, "creator", array_is_ordered},
					{ns::tiff, "Copyright", ns::dc, "rights", 0},
					{ns::tiff, "DateTime", ns::xmp, "ModifyDate", 0},
					{ns::exif, "DateTimeDigitized", ns::xmp, "CreateDate", 0},
					{ns::tiff, "ImageDescription", ns::dc, "description", array_is_alt_text},
					{ns::tiff, "Software", ns::xmp, "CreatorTool", 0},

					{ns::png, "Author", ns::dc, "creator", array_is_ordered},
					{ns::png, "Copyright", ns::dc, "rights", array_is_alt_text},
					{ns::png, "CreationTime", ns::xmp, "CreateDate", 0},
					{ns::png, "Description", ns::dc, "description", array_is_alt_text},
					{ns::png, "ModificationTime", ns::xmp, "ModifyDate", 0},
					{ns::png, "Software", ns::xmp, "CreatorTool", 0},
					{ns::png, "Title", ns::dc, "title", array_is_alt_text},
				};

				for (const auto& e : standard)
				{
					std::string alias_prefix, actual_prefix;
					namespace_prefix(e.alias_ns, &alias_prefix);
					namespace_prefix(e.actual_ns, &actual_prefix);

					expanded_path actual;
					actual.push_back({std::string(e.actual_ns), schema_step});
					actual.push_back({actual_prefix + std::string(e.actual_prop), struct_field_step});

					if (e.array_form != 0)
					{
						actual[1].options |= verify_set_options(e.array_form, false);

						if (is_alt_text(e.array_form))
						{
							actual.push_back({"[?xml:lang=\"x-default\"]", qual_selector_step});
						}
						else
						{
							actual.push_back({"[1]", array_index_step});
						}
					}

					map.emplace(alias_prefix + std::string(e.alias_prop), std::move(actual));
				}
			}
		};

		const alias_table& aliases()
		{
			static const alias_table table;
			return table;
		}

		void split_name_and_value(const std::string_view step, std::string& name, std::string& value)
		{
			// [name="value"] or [?name="value"], with a doubled quote standing for one.
			auto p = std::size_t{1};
			if (step[p] == '?') ++p;
			const auto equals = step.find('=', p);
			name.assign(step.substr(p, equals - p));

			const auto quote = step[equals + 1];
			const auto value_end = step.size() - 2;
			value.clear();

			for (auto i = equals + 2; i < value_end; ++i)
			{
				if (step[i] == quote && i + 1 < value_end && step[i + 1] == quote) ++i;
				value += step[i];
			}
		}

		int find_indexed_item(node* array, const std::string_view step, const bool create)
		{
			long long index = 0;

			for (std::size_t i = 1; i + 1 < step.size(); ++i)
			{
				index = index * 10 + (step[i] - '0');
				if (index > 0x7FFFFFFF) throw error("Array index overflow");
			}

			--index;
			if (index < 0) throw error("Array index must be larger than zero");

			if (index == static_cast<long long>(array->children.size()) && create)
			{
				array->children.push_back(std::make_unique<node>(array, std::string(array_item_name), std::string(),
				                                                 new_implicit_node));
			}

			if (index >= static_cast<long long>(array->children.size())) return -1;
			return static_cast<int>(index);
		}

		int lookup_field_selector(const node* array, const std::string_view field_name, const std::string_view field_value)
		{
			for (std::size_t i = 0; i < array->children.size(); ++i)
			{
				const auto* const item = array->children[i].get();
				if (!is_struct(item->options)) throw error("Field selector must be used on array of struct");

				for (const auto& field : item->children)
				{
					if (field->name == field_name && field->value == field_value) return static_cast<int>(i);
				}
			}

			return -1;
		}

		int lookup_qual_selector(const node* array, const std::string_view qual_name, std::string qual_value)
		{
			if (qual_name == "xml:lang")
			{
				normalize_lang_value(qual_value);
				return lookup_lang_item(array, qual_value);
			}

			for (std::size_t i = 0; i < array->children.size(); ++i)
			{
				for (const auto& qual : array->children[i]->qualifiers)
				{
					if (qual->name == qual_name && qual->value == qual_value) return static_cast<int>(i);
				}
			}

			return -1;
		}

		struct node_position
		{
			node* parent = nullptr;
			bool is_qualifier = false;
			std::size_t index = 0;
		};

		node* follow_step(node* parent, const expanded_path& path, const std::size_t step_index, const bool create,
		                  node_position* position, const bool aliased_array_item = false)
		{
			node* next = nullptr;
			const auto& step = path[step_index];
			const auto kind = kind_of(step);

			if (kind == struct_field_step)
			{
				next = find_child(parent, step.step, create);
				if (next && position)
				{
					position->parent = parent;
					position->is_qualifier = false;
					position->index = static_cast<std::size_t>(
						std::find_if(parent->children.begin(), parent->children.end(),
						             [next](const auto& c) { return c.get() == next; }) - parent->children.begin());
				}
			}
			else if (kind == qualifier_step)
			{
				next = find_qualifier(parent, std::string_view(step.step).substr(1), create);
				if (next && position)
				{
					position->parent = parent;
					position->is_qualifier = true;
					position->index = static_cast<std::size_t>(
						std::find_if(parent->qualifiers.begin(), parent->qualifiers.end(),
						             [next](const auto& c) { return c.get() == next; }) - parent->qualifiers.begin());
				}
			}
			else
			{
				if (!is_array(parent->options)) throw error("Indexing applied to non-array");

				int index = -1;

				if (kind == array_index_step)
				{
					index = find_indexed_item(parent, step.step, create);
				}
				else if (kind == array_last_step)
				{
					index = static_cast<int>(parent->children.size()) - 1;
				}
				else if (kind == field_selector_step)
				{
					std::string name, value;
					split_name_and_value(step.step, name, value);
					index = lookup_field_selector(parent, name, value);
				}
				else if (kind == qual_selector_step)
				{
					std::string name, value;
					split_name_and_value(step.step, name, value);
					index = lookup_qual_selector(parent, name, value);
				}

				if (index >= 0 && index < static_cast<int>(parent->children.size()))
				{
					next = parent->children[static_cast<std::size_t>(index)].get();
				}

				if (index == -1 && create && aliased_array_item && kind == qual_selector_step)
				{
					// An alias to the x-default item of an alt-text array: setting it through the alias
					// creates the item.
					auto item = std::make_unique<node>(parent, std::string(array_item_name), std::string(),
					                                   has_qualifiers | has_lang | new_implicit_node);
					item->qualifiers.push_back(std::make_unique<node>(item.get(), "xml:lang", "x-default", is_qualifier));
					next = item.get();
					parent->children.insert(parent->children.begin(), std::move(item));
					index = 0;
				}

				if (next && position)
				{
					position->parent = parent;
					position->is_qualifier = false;
					position->index = static_cast<std::size_t>(index);
				}
			}

			if (next && (next->options & new_implicit_node))
			{
				next->options |= step.options & array_form_mask;
			}

			return next;
		}

		void check_implicit_struct(node* n, const expanded_path& path, const std::size_t step_index)
		{
			if (step_index < path.size() && (n->options & composite_mask) == 0 &&
				kind_of(path[step_index]) == struct_field_step)
			{
				n->options |= value_is_struct;
			}
		}

		void remove_at(const node_position& position)
		{
			auto& list = position.is_qualifier ? position.parent->qualifiers : position.parent->children;
			list.erase(list.begin() + static_cast<std::ptrdiff_t>(position.index));
		}
	}

	bool registry_prefix(const std::string_view uri, std::string* prefix)
	{
		auto& table = namespaces();
		std::shared_lock lock(table.lock);
		const auto found = table.uri_to_prefix.find(std::string(uri));
		if (found == table.uri_to_prefix.end()) return false;
		if (prefix) *prefix = found->second;
		return true;
	}

	bool registry_uri(const std::string_view prefix, std::string* uri)
	{
		std::string key(prefix);
		if (key.empty() || key.back() != ':') key += ':';

		auto& table = namespaces();
		std::shared_lock lock(table.lock);
		const auto found = table.prefix_to_uri.find(key);
		if (found == table.prefix_to_uri.end()) return false;
		if (uri) *uri = found->second;
		return true;
	}

	const expanded_path* find_alias(const std::string_view alias_name)
	{
		const auto& table = aliases().map;
		const auto found = table.find(alias_name);
		return found == table.end() ? nullptr : &found->second;
	}

	expanded_path expand_path(const std::string_view schema, const std::string_view path)
	{
		if (schema.empty()) throw error("Schema namespace URI is required");
		if (path.empty()) throw error("Empty property name");

		expanded_path result;

		auto step_end = path.find_first_of("/[*");
		const auto root = path.substr(0, step_end);
		if (root.empty()) throw error("Empty initial XPath step");
		if (root[0] == '?' || root[0] == '@') throw error("Top level name must not be a qualifier");

		std::string schema_prefix;
		if (!registry_prefix(schema, &schema_prefix)) throw error("Unregistered schema namespace URI");

		const auto colon = root.find(':');
		verify_simple_name(root.substr(0, colon));

		result.push_back({std::string(schema), schema_step});

		if (colon == std::string_view::npos)
		{
			result.push_back({schema_prefix + std::string(root), struct_field_step});
		}
		else
		{
			verify_simple_name(root.substr(colon + 1));
			if (root.substr(0, colon + 1) != schema_prefix) throw error("Schema namespace URI and prefix mismatch");
			result.push_back({std::string(root), struct_field_step});
		}

		if (find_alias(result[1].step)) result[1].options |= step_is_alias;

		while (step_end != std::string_view::npos && step_end < path.size())
		{
			auto step_begin = step_end;
			if (path[step_begin] == '/') ++step_begin;

			if (step_begin < path.size() && path[step_begin] == '*')
			{
				++step_begin;
				if (step_begin >= path.size() || path[step_begin] != '[') throw error("Missing '[' after '*'");
			}

			step_end = step_begin;
			option_bits kind;
			std::string_view qual_name;

			if (step_begin >= path.size()) throw error("Empty XPath step");

			if (path[step_begin] != '[')
			{
				while (step_end < path.size() && path[step_end] != '/' && path[step_end] != '[' && path[step_end] != '*') ++step_end;
				qual_name = path.substr(step_begin, step_end - step_begin);
				kind = struct_field_step;
			}
			else
			{
				++step_end;

				if (step_end < path.size() && path[step_end] >= '0' && path[step_end] <= '9')
				{
					while (step_end < path.size() && path[step_end] >= '0' && path[step_end] <= '9') ++step_end;
					if (step_end >= path.size() || path[step_end] != ']') throw error("Missing ']' for integer array index");
					kind = array_index_step;
				}
				else
				{
					while (step_end < path.size() && path[step_end] != ']' && path[step_end] != '=') ++step_end;
					if (step_end >= path.size()) throw error("Missing ']' or '=' for array index");

					if (path[step_end] == ']')
					{
						if (path.substr(step_begin, step_end - step_begin) != "[last()") throw error("Invalid non-numeric array index");
						kind = array_last_step;
					}
					else
					{
						qual_name = path.substr(step_begin + 1, step_end - step_begin - 1);
						++step_end;
						if (step_end >= path.size()) throw error("Invalid quote in array selector");
						const auto quote = path[step_end];
						if (quote != '\'' && quote != '"') throw error("Invalid quote in array selector");
						++step_end;

						while (step_end < path.size())
						{
							if (path[step_end] == quote)
							{
								if (step_end + 1 >= path.size() || path[step_end + 1] != quote) break;
								++step_end;
							}
							++step_end;
						}

						if (step_end >= path.size()) throw error("No terminating quote for array selector");
						++step_end;
						kind = field_selector_step;
					}
				}

				if (step_end >= path.size() || path[step_end] != ']') throw error("Missing ']' for array index");
				++step_end;
			}

			if (step_end == step_begin) throw error("Empty XPath step");
			std::string step(path.substr(step_begin, step_end - step_begin));

			const auto check_qual_name = [](std::string_view name)
			{
				if (name.empty()) throw error("Empty qualified name");
				const auto c = name.find(':');
				if (c == 0 || c == std::string_view::npos) throw error("Ill-formed qualified name");
				verify_simple_name(name.substr(0, c));
				verify_simple_name(name.substr(c + 1));
				if (!registry_uri(name.substr(0, c + 1), nullptr)) throw error("Unknown namespace prefix for qualified name");
			};

			if (kind == struct_field_step)
			{
				if (step[0] == '@')
				{
					step[0] = '?';
					if (step != "?xml:lang") throw error("Only xml:lang allowed with '@'");
				}

				if (step[0] == '?')
				{
					qual_name.remove_prefix(1);
					kind = qualifier_step;
				}

				check_qual_name(qual_name);
			}
			else if (kind == field_selector_step)
			{
				if (step[1] == '@')
				{
					step[1] = '?';
					if (!step.starts_with("[?xml:lang=")) throw error("Only xml:lang allowed with '@'");
				}

				if (step[1] == '?')
				{
					qual_name.remove_prefix(1);
					kind = qual_selector_step;
				}

				check_qual_name(qual_name);
			}

			result.push_back({std::move(step), kind});
		}

		return result;
	}

	std::string compose_array_item_path(const std::string_view schema, const std::string_view array, const int index)
	{
		if (index < 0) throw error("Array index out of bounds");
		if (schema.empty()) throw error("Schema namespace URI is required");

		std::string result(array);
		result += '[';
		result += std::to_string(index);
		result += ']';
		return result;
	}

	std::unique_ptr<node> node::clone(node* new_parent) const
	{
		auto copy = std::make_unique<node>(new_parent, name, value, options);

		for (const auto& c : children) copy->children.push_back(c->clone(copy.get()));
		for (const auto& q : qualifiers) copy->qualifiers.push_back(q->clone(copy.get()));

		return copy;
	}

	void node::remove_qualifiers()
	{
		qualifiers.clear();
		options &= ~(has_qualifiers | has_lang | has_type);
	}

	node* find_schema(node* tree, const std::string_view uri, const bool create)
	{
		for (const auto& schema : tree->children)
		{
			if (schema->name == uri) return schema.get();
		}

		if (!create) return nullptr;

		std::string prefix;
		registry_prefix(uri, &prefix);
		tree->children.push_back(std::make_unique<node>(tree, std::string(uri), std::move(prefix),
		                                                schema_node | new_implicit_node));
		return tree->children.back().get();
	}

	node* find_child(node* parent, const std::string_view name, const bool create)
	{
		if (!(parent->options & (schema_node | value_is_struct)))
		{
			if (!(parent->options & new_implicit_node)) throw error("Named children only allowed for schemas and structs");
			if (parent->options & value_is_array) throw error("Named children not allowed for arrays");
			if (!create) throw error("Parent is new implicit node, but createNodes is false");
			parent->options |= value_is_struct;
		}

		for (const auto& child : parent->children)
		{
			if (child->name == name) return child.get();
		}

		if (!create) return nullptr;

		parent->children.push_back(std::make_unique<node>(parent, std::string(name), std::string(), new_implicit_node));
		return parent->children.back().get();
	}

	node* find_qualifier(node* parent, const std::string_view name, const bool create)
	{
		for (const auto& qual : parent->qualifiers)
		{
			if (qual->name == name) return qual.get();
		}

		if (!create) return nullptr;

		auto qual = std::make_unique<node>(parent, std::string(name), std::string(), is_qualifier | new_implicit_node);
		auto* const raw = qual.get();
		parent->options |= has_qualifiers;

		const auto is_lang = name == "xml:lang";
		const auto is_type = name == "rdf:type";

		if (is_lang) parent->options |= has_lang;
		else if (is_type) parent->options |= has_type;

		if (parent->qualifiers.empty() || !(is_lang || is_type))
		{
			parent->qualifiers.push_back(std::move(qual));
		}
		else
		{
			auto at = parent->qualifiers.begin();
			if (is_type && (parent->options & has_lang)) ++at;
			parent->qualifiers.insert(at, std::move(qual));
		}

		return raw;
	}

	int lookup_lang_item(const node* array, const std::string_view lang)
	{
		if (!is_array(array->options)) throw error("Language item must be used on array");

		for (std::size_t i = 0; i < array->children.size(); ++i)
		{
			const auto& item = array->children[i];
			if (item->qualifiers.empty() || item->qualifiers[0]->name != "xml:lang") continue;
			if (item->qualifiers[0]->value == lang) return static_cast<int>(i);
		}

		return -1;
	}

	node* find_node(node* tree, const expanded_path& path, const bool create, const option_bits leaf_options)
	{
		if (path.empty()) throw error("Empty XPath");

		node* current;
		node* new_root = nullptr; // The topmost node created on the way down, removed if the leaf is not.
		node_position new_root_position;
		node_position position;
		auto step = std::size_t{1};

		const auto note_new = [&](node* n, const node_position& at)
		{
			if (n->options & new_implicit_node)
			{
				n->options &= ~new_implicit_node;
				if (!new_root)
				{
					new_root = n;
					new_root_position = at;
				}
				return true;
			}
			return false;
		};

		const auto schema_position = [tree](const node* schema)
		{
			node_position at;
			at.parent = tree;
			at.index = static_cast<std::size_t>(
				std::find_if(tree->children.begin(), tree->children.end(), [schema](const auto& c) { return c.get() == schema; }) -
				tree->children.begin());
			return at;
		};

		if (!(path[1].options & step_is_alias))
		{
			current = find_schema(tree, path[0].step, create);
			if (!current) return nullptr;
			note_new(current, schema_position(current));
		}
		else
		{
			step = 2;
			const auto* const actual = find_alias(path[1].step);

			current = find_schema(tree, (*actual)[0].step, create);
			if (!current) return nullptr;
			note_new(current, schema_position(current));

			try
			{
				current = follow_step(current, *actual, 1, create, &position);
				if (current)
				{
					if (note_new(current, position)) check_implicit_struct(current, path, 2);

					if ((*actual)[1].options & array_form_mask)
					{
						current = follow_step(current, *actual, 2, create, &position, true);
						if (current && note_new(current, position)) check_implicit_struct(current, path, 2);
					}
				}
			}
			catch (...)
			{
				if (new_root) remove_at(new_root_position);
				throw;
			}

			if (!current)
			{
				if (new_root) remove_at(new_root_position);
				return nullptr;
			}
		}

		try
		{
			for (; step < path.size(); ++step)
			{
				current = follow_step(current, path, step, create, &position);
				if (!current) break;
				if (note_new(current, position)) check_implicit_struct(current, path, step + 1);
			}
		}
		catch (...)
		{
			if (new_root) remove_at(new_root_position);
			throw;
		}

		if (new_root)
		{
			if (current)
			{
				current->options |= leaf_options;
			}
			else
			{
				remove_at(new_root_position);
			}
		}

		return current;
	}

	const node* find_const_node(const node* tree, const expanded_path& path)
	{
		return find_node(const_cast<node*>(tree), path, false);
	}

	void delete_node(node* n)
	{
		auto* const parent = n->parent;

		if (!(n->options & is_qualifier))
		{
			std::erase_if(parent->children, [n](const auto& c) { return c.get() == n; });
		}
		else
		{
			const auto is_lang = n->name == "xml:lang";
			const auto is_type = n->name == "rdf:type";
			std::erase_if(parent->qualifiers, [n](const auto& c) { return c.get() == n; });
			if (parent->qualifiers.empty()) parent->options &= ~has_qualifiers;
			if (is_lang) parent->options &= ~has_lang;
			else if (is_type) parent->options &= ~has_type;
		}
	}

	void delete_empty_schema(node* schema)
	{
		if ((schema->options & schema_node) && schema->children.empty())
		{
			std::erase_if(schema->parent->children, [schema](const auto& c) { return c.get() == schema; });
		}
	}

	void normalize_lang_value(std::string& value)
	{
		// The primary subtag is lower case, a two-letter second subtag upper case, the rest lower.
		std::size_t i = 0;
		const auto size = value.size();

		const auto lower_until_dash = [&]()
		{
			while (i < size && value[i] != '-')
			{
				if (value[i] >= 'A' && value[i] <= 'Z') value[i] = static_cast<char>(value[i] + 0x20);
				++i;
			}
		};

		lower_until_dash();
		if (i < size && value[i] == '-') ++i;
		const auto second = i;
		lower_until_dash();

		if (i == second + 2)
		{
			for (auto k = second; k < second + 2; ++k)
			{
				if (value[k] >= 'a' && value[k] <= 'z') value[k] = static_cast<char>(value[k] - 0x20);
			}
		}

		while (i < size)
		{
			if (value[i] == '-') ++i;
			lower_until_dash();
		}
	}

	void normalize_lang_array(node* array)
	{
		auto item = std::size_t{0};
		auto has_default = false;

		for (; item < array->children.size(); ++item)
		{
			const auto& child = array->children[item];
			if (child->qualifiers.empty() || child->qualifiers[0]->name != "xml:lang")
			{
				throw error("AltText array items must have an xml:lang qualifier");
			}

			if (child->qualifiers[0]->value == "x-default")
			{
				has_default = true;
				break;
			}
		}

		if (has_default)
		{
			if (item != 0) std::swap(array->children[0], array->children[item]);
			if (array->children.size() == 2) array->children[1]->value = array->children[0]->value;
		}
	}

	void detect_alt_text(node* array)
	{
		std::size_t item = 0;

		for (; item < array->children.size(); ++item)
		{
			const auto options = array->children[item]->options;
			if ((options & composite_mask) || !(options & has_lang)) break;
		}

		if (!array->children.empty() && item == array->children.size())
		{
			array->options |= array_is_alt_text;
			normalize_lang_array(array);
		}
	}

	option_bits verify_set_options(option_bits options, const bool has_value)
	{
		if (options & array_is_alt_text) options |= array_is_alternate;
		if (options & array_is_alternate) options |= array_is_ordered;
		if (options & array_is_ordered) options |= value_is_array;

		constexpr option_bits allowed = value_is_uri | value_is_struct | value_is_array | array_is_ordered |
			array_is_alternate | array_is_alt_text | delete_existing;

		if (options & ~allowed) throw error("Unrecognized option flags");
		if ((options & value_is_struct) && (options & value_is_array))
		{
			throw error("IsStruct and IsArray options are mutually exclusive");
		}
		if ((options & value_is_uri) && (options & composite_mask)) throw error("Structs and arrays can't have \"value\" options");
		if (has_value && (options & composite_mask)) throw error("Structs and arrays can't have string values");

		return options;
	}

	void set_node_value(node* n, const std::string_view value)
	{
		// The toolkit takes a C string, so a value stops at its first NUL. Controls other than tab,
		// LF and CR become spaces; malformed UTF-8 and the noncharacters U+FFFE and U+FFFF are refused.
		auto text = value.substr(0, value.find('\0'));
		std::string result;
		result.reserve(text.size());

		for (std::size_t i = 0; i < text.size();)
		{
			const auto c = static_cast<unsigned char>(text[i]);

			if (c < 0x80)
			{
				result += (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) || c == 0x7F ? ' ' : static_cast<char>(c);
				++i;
				continue;
			}

			int count;
			char32_t cp;
			if ((c & 0xE0) == 0xC0) { count = 2; cp = c & 0x1F; }
			else if ((c & 0xF0) == 0xE0) { count = 3; cp = c & 0x0F; }
			else if ((c & 0xF8) == 0xF0) { count = 4; cp = c & 0x07; }
			else throw error("Invalid UTF-8 data byte");

			if (i + count > text.size()) throw error("Invalid UTF-8 data byte");

			for (auto k = 1; k < count; ++k)
			{
				const auto cc = static_cast<unsigned char>(text[i + k]);
				if ((cc & 0xC0) != 0x80) throw error("Invalid UTF-8 data byte");
				cp = (cp << 6) | (cc & 0x3F);
			}

			if (cp == 0xFFFE || cp == 0xFFFF) throw error("U+FFFE and U+FFFF are not allowed in XML");

			result.append(text.substr(i, static_cast<std::size_t>(count)));
			i += static_cast<std::size_t>(count);
		}

		if ((n->options & is_qualifier) && n->name == "xml:lang") normalize_lang_value(result);
		n->value = std::move(result);
	}
}

namespace libfiles::xmp
{
	std::string register_namespace(const std::string_view uri, const std::string_view suggested_prefix)
	{
		if (uri.empty()) throw error("Empty namespace URI");

		auto prefix = suggested_prefix;
		if (!prefix.empty() && prefix.back() == ':') prefix.remove_suffix(1);
		detail::verify_simple_name(prefix);

		auto& table = detail::namespaces();
		std::unique_lock lock(table.lock);
		return table.define(uri, prefix);
	}

	bool namespace_prefix(const std::string_view uri, std::string* prefix)
	{
		return detail::registry_prefix(uri, prefix);
	}

	bool namespace_uri(const std::string_view prefix, std::string* uri)
	{
		return detail::registry_uri(prefix, uri);
	}
}
