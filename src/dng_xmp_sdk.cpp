// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The DNG SDK's XMP backend, dng_xmp_sdk (the SDK's dng_xmp_sdk.h), over libfiles' XMP data model
// rather than the XMP Toolkit. cmake/dng.cmake builds it into the SDK in place of the SDK's own
// dng_xmp_sdk.cpp, whose behavior it keeps: the namespaces it registers, which faults it reports, swallows
// or throws, the serialization each Serialize argument asks for, and the toolkit utilities that file
// called - property iteration, PackageForJPEG, MergeFromJPEG and DuplicateSubtree. Moved from Diffractor
// unchanged.

#include "dng_xmp_sdk.h"

#if qDNGUseXMP

#include "dng_abort_sniffer.h"
#include "dng_assertions.h"
#include "dng_auto_ptr.h"
#include "dng_exceptions.h"
#include "dng_fingerprint.h"
#include "dng_host.h"
#include "dng_local_string.h"
#include "dng_memory.h"
#include "dng_string.h"
#include "dng_string_list.h"

#include "libfiles/xmp.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// libfiles has neither of the toolkit's XMPFiles nor its document operations.
#if qDNGXMPFiles
#error "Build the DNG SDK with qDNGXMPFiles=0: its XMP backend is libfiles, which has no XMPFiles"
#endif

#if qDNGXMPDocOps
#error "Build the DNG SDK with qDNGXMPDocOps=0: its XMP backend is libfiles, which has no document operations"
#endif

const char* XMP_NS_TIFF = "http://ns.adobe.com/tiff/1.0/";
const char* XMP_NS_EXIF = "http://ns.adobe.com/exif/1.0/";
const char* XMP_NS_EXIFEX = "http://cipa.jp/exif/1.0/";
const char* XMP_NS_PHOTOSHOP = "http://ns.adobe.com/photoshop/1.0/";
const char* XMP_NS_XAP = "http://ns.adobe.com/xap/1.0/";
const char* XMP_NS_XAP_RIGHTS = "http://ns.adobe.com/xap/1.0/rights/";
const char* XMP_NS_DC = "http://purl.org/dc/elements/1.1/";
const char* XMP_NS_DC_TERMS = "http://purl.org/dc/terms/";
const char* XMP_NS_XMP_NOTE = "http://ns.adobe.com/xmp/note/";
const char* XMP_NS_MM = "http://ns.adobe.com/xap/1.0/mm/";

const char* XMP_NS_CRS = "http://ns.adobe.com/camera-raw-settings/1.0/";
const char* XMP_NS_CRSS = "http://ns.adobe.com/camera-raw-saved-settings/1.0/";
const char* XMP_NS_CRD = "http://ns.adobe.com/camera-raw-defaults/1.0/";
const char* XMP_NS_CRLCP = "http://ns.adobe.com/camera-raw-embedded-lens-profile/1.0/";
const char* XMP_NS_CRGI = "http://ns.adobe.com/camera-raw-group-info/1.0/";
const char* XMP_NS_CRVC = "http://ns.adobe.com/camera-raw-virtual-copy/1.0/";

const char* XMP_NS_VFS = "http://ns.adobe.com/video-foundation-settings/1.0/";

const char* XMP_NS_LR = "http://ns.adobe.com/lightroom/1.0/";

const char* XMP_NS_LCP = "http://ns.adobe.com/photoshop/1.0/camera-profile";

const char* XMP_NS_AUX = "http://ns.adobe.com/exif/1.0/aux/";

const char* XMP_NS_IPTC = "http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/";
const char* XMP_NS_IPTC_EXT = "http://iptc.org/std/Iptc4xmpExt/2008-02-29/";

const char* XMP_NS_PLUS = "http://ns.useplus.org/ldf/xmp/1.0/";

const char* XMP_NS_CRX = "http://ns.adobe.com/lightroom-settings-experimental/1.0/";

const char* XMP_NS_DNG = "http://ns.adobe.com/dng/1.0/";

const char* XMP_NS_PANO = "http://ns.adobe.com/photoshop/1.0/panorama-profile";
const char* XMP_NS_GPANO = "http://ns.google.com/photos/1.0/panorama/";

const char* XMP_NS_REGIONS = "http://www.metadataworkinggroup.com/schemas/regions/";

const char* XMP_NS_HDRGM = "http://ns.adobe.com/hdr-gain-map/1.0/";
const char* XMP_NS_HDR_META = "http://ns.adobe.com/hdr-metadata/1.0/";

const char* XMP_NS_APPLE_HDRGM = "http://ns.apple.com/HDRGainMap/1.0/";
const char* XMP_NS_APPLE_PIXELDATA = "http://ns.apple.com/pixeldatainfo/1.0/";

namespace libxmp = libfiles::xmp;

class dng_xmp_private
{
public:
	dng_xmp_private() = default;

	dng_xmp_private(const dng_xmp_private& other) :
		meta(other.meta ? std::make_unique<libxmp::meta>(*other.meta) : nullptr)
	{
	}

	dng_xmp_private& operator=(const dng_xmp_private&) = delete;

	libxmp::meta& edit()
	{
		++generation;
		return *meta;
	}

	std::unique_ptr<libxmp::meta> meta;

	// Counts changes, so that an iteration knows when the tree it listed has changed under it.
	uint64 generation = 0;

	// Set once GetPrivateMeta has handed the tree out, after which anyone may change it unseen.
	bool shared = false;
};

// CR-4208475 Q-M4, kept from the SDK: process-wide set-up and tear-down are serialized, and tear-down is
// refused while any wrapper still holds metadata.
using dng_xmp_lifecycle_lock = std::lock_guard<std::recursive_mutex>;

static std::recursive_mutex s_lifecycle_mutex;
static bool s_initialized = false;
static uint32 s_meta_count = 0;

namespace
{
	using option_bits = libxmp::option_bits;

	std::string_view view_of(const char* s)
	{
		return s ? std::string_view(s) : std::string_view();
	}

	// What the SDK reported when an XMP call threw, in its words. DNG_REPORT shows it in a debug build.
	void report(const char* routine, const char* what)
	{
		char buffer[256];
		snprintf(buffer, sizeof(buffer), "Info: XMP %s threw %s exception", routine, what);
		DNG_REPORT(buffer);
	}

	void report_error(const char* routine, const char* message, const bool silent)
	{
		if (message && strlen(message) <= 128)
		{
			if (silent) return;

			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Info: XMP %s threw '%s' exception", routine, message);
			DNG_REPORT(buffer);
		}
		else
		{
			report(routine, "unnamed");
		}
	}

	// Runs an XMP operation as the SDK's CATCH_XMP blocks did: a fault is reported and, when the caller
	// cannot go on, becomes dng_error_unknown. Running out of memory is always dng_error_memory.
	template <typename F>
	void guard(const char* routine, const bool fatal, F&& operation, const bool silent = false)
	{
		try
		{
			operation();
		}
		catch (const std::bad_alloc&)
		{
			report(routine, "memory");
			ThrowMemoryFull();
		}
		catch (const libxmp::error& e)
		{
			report_error(routine, e.what(), silent);
			if (fatal) ThrowProgramError();
		}
		catch (...)
		{
			report(routine, "unknown");
			if (fatal) ThrowProgramError();
		}
	}

	void require(const std::string_view value, const char* message)
	{
		if (value.empty()) throw libxmp::error(message);
	}

	// Raises what the toolkit's ExpandXPath raises for a path it cannot read. An empty tree finds nothing,
	// so only the path itself can fault.
	void check_path(const std::string_view schema, const std::string_view path)
	{
		static const libxmp::meta empty;
		(void)empty.exists(schema, path);
	}

	std::string compose_array_item_path(const std::string_view schema, const std::string_view array, const int32 index)
	{
		require(schema, "Empty schema namespace URI");
		require(array, "Empty array name");
		check_path(schema, array);
		if (index < 0 && index != -1) throw libxmp::error("Array index out of bounds");

		std::string path(array);
		path += index == -1 ? std::string("[last()]") : '[' + std::to_string(index) + ']';
		return path;
	}

	// A field or qualifier: the parent's path, checked, then the name, which must be a single step, with
	// its namespace's prefix.
	std::string compose_child_path(const std::string_view schema, const std::string_view parent, const std::string_view child_schema,
	                               const std::string_view child, const std::string_view separator, const char* not_simple)
	{
		check_path(schema, parent);
		check_path(child_schema, child);
		if (child.find_first_of("/[*") != std::string_view::npos) throw libxmp::error(not_simple);

		std::string path(parent);
		path += separator;

		if (child.find(':') == std::string_view::npos)
		{
			std::string prefix;
			libxmp::namespace_prefix(child_schema, &prefix);
			path += prefix;
		}

		path += child;
		return path;
	}

	std::string compose_struct_field_path(const std::string_view schema, const std::string_view struct_name,
	                                      const std::string_view field_schema, const std::string_view field_name)
	{
		require(schema, "Empty schema namespace URI");
		require(struct_name, "Empty struct name");
		require(field_schema, "Empty field namespace URI");
		require(field_name, "Empty field name");
		return compose_child_path(schema, struct_name, field_schema, field_name, "/", "The fieldName must be simple");
	}

	std::string compose_qualifier_path(const std::string_view schema, const std::string_view property,
	                                   const std::string_view qualifier_schema, const std::string_view qualifier_name)
	{
		require(schema, "Empty schema namespace URI");
		require(property, "Empty property name");
		require(qualifier_schema, "Empty qualifier namespace URI");
		require(qualifier_name, "Empty qualifier name");
		return compose_child_path(schema, property, qualifier_schema, qualifier_name, "/?", "The qualifier name must be simple");
	}

	// The toolkit's GetArrayItem, which also takes -1 for the last item.
	bool get_array_item(const libxmp::meta& m, const std::string_view schema, const std::string_view array, const int32 index,
	                    std::string* value, option_bits* options = nullptr)
	{
		require(schema, "Empty schema namespace URI");
		require(array, "Empty array name");
		if (index <= 0 && index != -1) throw libxmp::error("Array index must be larger than zero");
		return m.get_property(schema, compose_array_item_path(schema, array, index), value, options);
	}

	// The toolkit's SetProperty takes a C string, and a null one is no value: the node is made, or emptied,
	// without one.
	void set_property(libxmp::meta& m, const std::string_view schema, const std::string_view path, const char* value,
	                  const option_bits options = 0)
	{
		if (value) m.set_property(schema, path, value, options);
		else m.set_composite(schema, path, options);
	}

	void set_qualifier(libxmp::meta& m, const std::string_view schema, const std::string_view property,
	                   const std::string_view qualifier_schema, const std::string_view qualifier_name, const char* value)
	{
		require(schema, "Empty schema namespace URI");
		require(property, "Empty property name");
		require(qualifier_schema, "Empty qualifier namespace URI");
		require(qualifier_name, "Empty qualifier name");
		if (!m.exists(schema, property)) throw libxmp::error("Specified property does not exist");
		set_property(m, schema, compose_qualifier_path(schema, property, qualifier_schema, qualifier_name), value);
	}

	// The toolkit's AppendArrayItem, which, unlike libfiles', also takes no value, for a struct item.
	void append_array_item(libxmp::meta& m, const std::string_view schema, const std::string_view array,
	                       const option_bits array_options, const char* value, const option_bits item_options)
	{
		if (value)
		{
			m.append_array_item(schema, array, array_options, value, item_options);
			return;
		}

		require(schema, "Empty schema namespace URI");
		require(array, "Empty array name");

		option_bits existing = 0;

		if (m.get_property(schema, array, nullptr, &existing))
		{
			if (!libxmp::is_array(existing)) throw libxmp::error("The named property is not an array");
		}
		else
		{
			if (array_options == 0) throw libxmp::error("Explicit arrayOptions required to create new array");
			m.set_composite(schema, array, array_options);
		}

		const auto item = std::string(array) + '[' + std::to_string(m.count_array_items(schema, array) + 1) + ']';
		m.set_composite(schema, item, item_options);
	}

	// The toolkit refused an outrageous padding before serializing a packet of its own length; libfiles
	// leaves that check to its caller. Packets name libfiles in x:xmptk, as Diffractor's own writes do.
	std::string serialize(const libxmp::meta& m, const libxmp::serialize_options& options)
	{
		if (!options.exact_packet_length && !options.read_only_packet && !options.omit_packet_wrapper &&
			(options.padding >> 28) != 0)
		{
			throw libxmp::error("Outrageously large padding size");
		}

		return m.serialize(options);
	}

	// The tree as properties() lists it, linked into nodes: the public API has no call for a node's
	// qualifiers, fields or subtree.
	struct tree_node
	{
		std::string schema;
		std::string path;
		std::string name;
		std::string value;
		option_bits options = 0;
		std::vector<size_t> qualifiers;
		std::vector<size_t> children;
	};

	// Whether path names a qualifier, field or item directly below the node at parent.
	bool is_offspring(const std::string_view parent, const std::string_view path)
	{
		if (path.size() <= parent.size() || !path.starts_with(parent)) return false;
		auto step = path.substr(parent.size());

		if (step.starts_with('['))
		{
			return step.size() > 2 && step.back() == ']' &&
				std::all_of(step.begin() + 1, step.end() - 1, [](const char c) { return c >= '0' && c <= '9'; });
		}

		if (step.starts_with("/?")) step.remove_prefix(2);
		else if (step.starts_with('/')) step.remove_prefix(1);
		else return false;

		return !step.empty() && step.find_first_of("/[") == std::string_view::npos;
	}

	class tree_snapshot
	{
	public:
		explicit tree_snapshot(const libxmp::meta& m)
		{
			auto listed = m.properties();
			_nodes.reserve(listed.size());
			std::vector<size_t> chain;

			for (auto& p : listed)
			{
				const auto id = _nodes.size();
				std::string name;

				if (p.path.empty())
				{
					name = p.schema;
					_schemas.push_back(id);
					chain.assign(1, id);
				}
				else
				{
					while (chain.size() > 1 && !is_offspring(_nodes[chain.back()].path, p.path)) chain.pop_back();
					if (chain.empty()) continue;

					auto& parent = _nodes[chain.back()];
					const auto step = std::string_view(p.path).substr(parent.path.size());
					auto qualifier = false;

					if (parent.path.empty())
					{
						name = p.path;
					}
					else if (step.starts_with("/?"))
					{
						name = step.substr(2);
						qualifier = true;
					}
					else if (step.starts_with('/'))
					{
						name = step.substr(1);
					}
					else
					{
						name = "[]";
					}

					(qualifier ? parent.qualifiers : parent.children).push_back(id);
					chain.push_back(id);
				}

				_index.emplace(key(p.schema, p.path), id);
				_nodes.push_back({std::move(p.schema), std::move(p.path), std::move(name), std::move(p.value), p.options});
			}
		}

		const tree_node& operator[](const size_t id) const { return _nodes[id]; }
		const std::vector<size_t>& schemas() const { return _schemas; }

		const tree_node* find(const std::string_view schema, const std::string_view path) const
		{
			const auto found = _index.find(key(schema, path));
			return found == _index.end() ? nullptr : &_nodes[found->second];
		}

	private:
		static std::string key(const std::string_view schema, const std::string_view path)
		{
			std::string k;
			k.reserve(schema.size() + path.size() + 1);
			k += schema;
			k += '\x1F';
			k += path;
			return k;
		}

		std::vector<tree_node> _nodes;
		std::vector<size_t> _schemas;
		std::unordered_map<std::string, size_t> _index;
	};

	struct subtree
	{
		std::string name;
		std::string value;
		option_bits options = 0;
		std::vector<subtree> qualifiers;
		std::vector<subtree> children;
	};

	// The toolkit's CloneOffspring. With skip_empty, a qualifier or child holding neither a value nor
	// children is left out, both as found and once its own offspring are copied.
	void copy_offspring(const tree_snapshot& tree, const tree_node& from, subtree& to, const bool skip_empty)
	{
		const auto copy_list = [&](const std::vector<size_t>& ids, std::vector<subtree>& out)
		{
			for (const auto id : ids)
			{
				const auto& node = tree[id];
				if (skip_empty && node.value.empty() && node.children.empty()) continue;

				subtree copy{node.name, node.value, node.options};
				copy_offspring(tree, node, copy, skip_empty);
				if (skip_empty && copy.value.empty() && copy.children.empty()) continue;

				out.push_back(std::move(copy));
			}
		};

		copy_list(from.qualifiers, to.qualifiers);
		copy_list(from.children, to.children);
	}

	subtree copy_subtree(const tree_snapshot& tree, const tree_node& root, const bool skip_empty)
	{
		subtree copy{root.name, root.value, root.options};
		copy_offspring(tree, root, copy, skip_empty);
		return copy;
	}

	// Writes a copied subtree at path. With replace, an existing node keeps its place and loses its value,
	// qualifiers and children first. The bits that only describe a node - whether it has qualifiers, a
	// language or a type - follow from the qualifiers written, as they did where it was copied from.
	void write_subtree(libxmp::meta& m, const std::string_view schema, const std::string& path, const subtree& node,
	                   const bool replace = false)
	{
		const option_bits reset = replace ? libxmp::delete_existing : 0;

		if (libxmp::is_simple(node.options))
		{
			m.set_property(schema, path, node.value, (node.options & libxmp::value_is_uri) | reset);
		}
		else
		{
			m.set_composite(schema, path, (node.options & (libxmp::value_is_struct | libxmp::array_form_mask)) | reset);
		}

		for (const auto& qualifier : node.qualifiers) write_subtree(m, schema, path + "/?" + qualifier.name, qualifier);

		const auto array = libxmp::is_array(node.options);

		for (size_t i = 0; i < node.children.size(); ++i)
		{
			const auto& child = node.children[i];
			write_subtree(m, schema, array ? path + '[' + std::to_string(i + 1) + ']' : path + '/' + child.name, child);
		}
	}

	constexpr std::string_view probe_qualifier = "/?xmpidq:DngXmpSdkProbe";

	std::string with_probe(const std::string_view path)
	{
		return std::string(path) + std::string(probe_qualifier);
	}

	// Where the probe qualifier added to `marked`, and absent from `original`, sits.
	bool find_probe(const libxmp::meta& marked, const libxmp::meta& original, std::string& schema, std::string& path)
	{
		for (auto& p : marked.properties())
		{
			if (p.path.ends_with(probe_qualifier) && !original.exists(p.schema, p.path))
			{
				schema = std::move(p.schema);
				path = p.path.substr(0, p.path.size() - probe_qualifier.size());
				return true;
			}
		}

		return false;
	}

	// Whether a top-level name is one of the toolkit's aliases, which name a property elsewhere: set in an
	// empty tree, it makes something other than itself.
	bool is_alias(const std::string_view schema, const std::string& name)
	{
		libxmp::meta probe;
		probe.set_property(schema, name, "");

		for (const auto& p : probe.properties())
		{
			if (!p.path.empty()) return p.schema != schema || p.path != name;
		}

		return false;
	}

	// The schema and path by which properties() lists the node that path finds, which an alias, a selector
	// or [last()] names otherwise. A copy of the tree marks the node with a qualifier no packet carries.
	bool find_listed(const libxmp::meta& m, const std::string_view schema, const std::string_view path, std::string& listed_schema,
	                 std::string& listed_path)
	{
		if (!m.exists(schema, path)) return false;

		if (path.find_first_of("/[*") == std::string_view::npos)
		{
			std::string name;
			if (path.find(':') == std::string_view::npos) libxmp::namespace_prefix(schema, &name);
			name += path;

			if (!is_alias(schema, name))
			{
				listed_schema = schema;
				listed_path = std::move(name);
				return true;
			}
		}

		libxmp::meta marked(m);
		marked.set_property(schema, with_probe(path), "");
		return find_probe(marked, m, listed_schema, listed_path);
	}

	// The toolkit names a rooted iteration's nodes after the steps of the root's expanded path: the root
	// property with its prefix, then each step - after a '/' when it is a field or qualifier - with '@'
	// read as '?'. The path has been checked already.
	std::string root_name(const std::string_view schema, const std::string_view path)
	{
		auto end = std::min(path.find_first_of("/[*"), path.size());
		const auto root = path.substr(0, end);
		std::string name;

		if (root.find(':') == std::string_view::npos) libxmp::namespace_prefix(schema, &name);
		name += root;

		while (end < path.size())
		{
			auto begin = end;
			if (path[begin] == '/') ++begin;
			if (begin < path.size() && path[begin] == '*') ++begin;
			end = begin;

			if (begin < path.size() && path[begin] == '[')
			{
				++end;

				if (end < path.size() && path[end] >= '0' && path[end] <= '9')
				{
					while (end < path.size() && path[end] >= '0' && path[end] <= '9') ++end;
				}
				else
				{
					while (end < path.size() && path[end] != ']' && path[end] != '=') ++end;

					if (end < path.size() && path[end] == '=')
					{
						end += 2;
						const auto quote = path[end - 1];

						// A doubled quote stands for one.
						while (end < path.size())
						{
							if (path[end] == quote)
							{
								if (end + 1 >= path.size() || path[end + 1] != quote) break;
								++end;
							}

							++end;
						}

						++end;
					}
				}

				++end;
				std::string step(path.substr(begin, end - begin));
				if (step.size() > 1 && step[1] == '@') step[1] = '?';
				name += step;
			}
			else
			{
				end = std::min(path.find_first_of("/[*", begin), path.size());
				std::string step(path.substr(begin, end - begin));
				if (!step.empty() && step[0] == '@') step[0] = '?';
				name += '/';
				name += step;
			}
		}

		return name;
	}

	// The toolkit's property iterator. It lists a node's qualifiers and children only when it reaches the
	// node, and finds each node again by its path before reporting it, so a callback may remove what it is
	// shown, or what is still to come.
	class path_iterator
	{
	public:
		path_iterator(dng_xmp_private& owner, std::string_view schema, std::string_view root, bool just_children);
		path_iterator(const path_iterator&) = delete;
		path_iterator& operator=(const path_iterator&) = delete;

		bool next(std::string& schema, std::string& path);

	private:
		enum class stage
		{
			before_visit,
			visit_self,
			visit_qualifiers,
			visit_children,
		};

		struct node
		{
			option_bits options = 0;
			std::string path;
			std::vector<node> qualifiers;
			std::vector<node> children;
			stage visit = stage::before_visit;
		};

		using position = std::vector<node>::iterator;

		const tree_snapshot& tree();
		const tree_node* listed(const node& n);
		void add_offspring(node& n, const tree_node& found);
		void advance();
		bool next_node();

		dng_xmp_private& _owner;
		const bool _just_children;
		std::vector<node> _roots;
		std::vector<std::pair<position, position>> _ancestors;
		position _current;
		position _end;
		std::string _schema;

		std::unique_ptr<tree_snapshot> _tree;
		uint64 _tree_generation = 0;

		// A rooted iteration names its nodes after the root's own path, which may reach the root through an
		// alias or a selector; the tree lists them under the root's listed path.
		bool _rooted = false;
		std::string _root_name;
		bool _root_listed = false;
		std::string _listed_schema;
		std::string _listed_root;
	};

	path_iterator::path_iterator(dng_xmp_private& owner, const std::string_view schema, const std::string_view root,
	                             const bool just_children) : _owner(owner), _just_children(just_children)
	{
		if (!root.empty())
		{
			if (_owner.meta->exists(schema, root))
			{
				_rooted = true;
				_schema = schema;
				_root_name = root_name(schema, root);

				node first{0, _root_name};

				if (const auto* const found = listed(first))
				{
					first.options = found->options;
					if (_just_children) add_offspring(first, *found);
				}

				_roots.push_back(std::move(first));
			}
		}
		else if (!schema.empty())
		{
			const auto& t = tree();

			if (const auto* const found = t.find(schema, {}))
			{
				node first{libxmp::schema_node, std::string(schema)};
				for (const auto id : found->children) first.children.push_back({t[id].options, t[id].name});

				if (!first.children.empty())
				{
					_roots.push_back(std::move(first));
					_schema = schema;
				}
			}
		}
		else
		{
			const auto& t = tree();

			for (const auto id : t.schemas())
			{
				node first{libxmp::schema_node, t[id].schema};

				if (!_just_children)
				{
					for (const auto child : t[id].children) first.children.push_back({t[child].options, t[child].name});
					if (first.children.empty()) continue;
				}

				_roots.push_back(std::move(first));
			}
		}

		_current = _roots.begin();
		_end = _roots.end();

		// Just the children of a named schema or root, not the schema or root itself.
		if (_just_children && _current != _end && !schema.empty()) _current->visit = stage::visit_self;
	}

	const tree_snapshot& path_iterator::tree()
	{
		if (!_tree || _tree_generation != _owner.generation || _owner.shared)
		{
			_tree = std::make_unique<tree_snapshot>(*_owner.meta);
			_tree_generation = _owner.generation;
			if (_rooted) _root_listed = find_listed(*_owner.meta, _schema, _root_name, _listed_schema, _listed_root);
		}

		return *_tree;
	}

	const tree_node* path_iterator::listed(const node& n)
	{
		const auto& t = tree();
		if (!_rooted) return t.find(_schema, n.path);
		if (!_root_listed) return nullptr;
		return t.find(_listed_schema, _listed_root + n.path.substr(_root_name.size()));
	}

	void path_iterator::add_offspring(node& n, const tree_node& found)
	{
		const auto& t = *_tree;

		for (const auto id : found.qualifiers) n.qualifiers.push_back({t[id].options, n.path + "/?" + t[id].name});

		const auto* const separator = libxmp::is_struct(found.options) ? "/" : "";
		const auto array = libxmp::is_array(found.options);
		size_t index = 0;

		for (const auto id : found.children)
		{
			auto path = array ? n.path + '[' + std::to_string(++index) + ']' : n.path + separator + t[id].name;
			n.children.push_back({t[id].options, std::move(path)});
		}
	}

	// Moves to the next node to visit in a depth-first walk: a node, then its qualifiers, then its children.
	void path_iterator::advance()
	{
		while (true)
		{
			if (_current == _end)
			{
				if (_ancestors.empty()) break;
				std::tie(_current, _end) = _ancestors.back();
				_ancestors.pop_back();
				continue;
			}

			if (_current->visit == stage::before_visit)
			{
				if (libxmp::is_schema(_current->options)) _schema = _current->path;
				break;
			}

			if (_current->visit == stage::visit_self)
			{
				_current->visit = stage::visit_qualifiers;

				if (!_current->qualifiers.empty())
				{
					_ancestors.emplace_back(_current, _end);
					_end = _current->qualifiers.end();
					_current = _current->qualifiers.begin();
					break;
				}
			}

			if (_current->visit == stage::visit_qualifiers)
			{
				_current->qualifiers.clear();
				_current->visit = stage::visit_children;

				if (!_current->children.empty())
				{
					_ancestors.emplace_back(_current, _end);
					_end = _current->children.end();
					_current = _current->children.begin();
					break;
				}
			}

			if (_current->visit == stage::visit_children)
			{
				_current->children.clear();
				++_current;
			}
		}
	}

	bool path_iterator::next_node()
	{
		if (_current->visit != stage::before_visit) advance();

		auto schema_node = false;

		while (_current != _end)
		{
			schema_node = libxmp::is_schema(_current->options);

			// A schema is visited even once its node is gone, as the toolkit's stand-in schema node was.
			if (schema_node)
			{
				_schema = _current->path;
				break;
			}

			if (_owner.meta->exists(_schema, _current->path)) break;

			_current->visit = stage::visit_children;
			_current->children.clear();
			_current->qualifiers.clear();
			advance();
		}

		if (_current == _end) return false;

		if (!schema_node && !_just_children)
		{
			if (const auto* const found = listed(*_current)) add_offspring(*_current, *found);
		}

		_current->visit = stage::visit_self;
		return true;
	}

	bool path_iterator::next(std::string& schema, std::string& path)
	{
		if (!_owner.meta || _current == _end || !next_node()) return false;

		schema = _schema;
		if (libxmp::is_schema(_current->options)) path.clear();
		else path = _current->path;
		return true;
	}

	// Whether making dest_root would put it inside the subtree at source_root, of the same tree. The toolkit
	// compared the nodes; a copy marks the source, makes the destination, and compares where the two land.
	bool lands_inside(const libxmp::meta& m, const std::string_view source_schema, const std::string_view source_root,
	                  const std::string_view dest_schema, const std::string_view dest_root)
	{
		libxmp::meta probe(m);
		probe.set_property(source_schema, with_probe(source_root), "");
		probe.set_composite(dest_schema, dest_root, 0);

		std::string source_listed_schema, source_listed_path;
		std::string dest_listed_schema, dest_listed_path;
		if (!find_probe(probe, m, source_listed_schema, source_listed_path)) return false;
		if (!find_listed(probe, dest_schema, dest_root, dest_listed_schema, dest_listed_path)) return false;

		return dest_listed_schema == source_listed_schema && dest_listed_path.size() > source_listed_path.size() &&
			dest_listed_path.starts_with(source_listed_path) &&
			(dest_listed_path[source_listed_path.size()] == '/' || dest_listed_path[source_listed_path.size()] == '[');
	}

	// The toolkit's DuplicateSubtree. A schema of "*" stands for the whole tree: every top-level property
	// becomes a field of an empty destination struct, or every field of a source struct a top-level property
	// of an empty destination tree.
	void duplicate_subtree(const libxmp::meta& source, libxmp::meta& dest, const std::string_view source_schema,
	                       const std::string_view source_root, std::string_view dest_schema, std::string_view dest_root)
	{
		require(source_schema, "Empty source schema URI");
		require(source_root, "Empty source root name");
		if (dest_schema.empty()) dest_schema = source_schema;
		if (dest_root.empty()) dest_root = source_root;

		const auto same = &source == &dest;
		const auto full_source = source_schema == "*";
		const auto full_dest = dest_schema == "*";

		if (same && (full_source || full_dest)) throw libxmp::error("Can't duplicate tree onto itself");
		if (full_source && full_dest) throw libxmp::error("Use Clone for full tree to full tree");

		if (full_source)
		{
			option_bits options = 0;
			if (!dest.get_property(dest_schema, dest_root, nullptr, &options) || !libxmp::is_struct(options))
			{
				throw libxmp::error("Destination must be an existing struct");
			}

			std::string listed_schema, listed_path;
			find_listed(dest, dest_schema, dest_root, listed_schema, listed_path);
			const tree_snapshot dest_tree(dest);
			const auto* const found = dest_tree.find(listed_schema, listed_path);
			if (found && !found->children.empty()) throw libxmp::error("Destination must be an empty struct");

			const tree_snapshot tree(source);

			for (const auto schema : tree.schemas())
			{
				for (const auto id : tree[schema].children)
				{
					write_subtree(dest, dest_schema, std::string(dest_root) + '/' + tree[id].name, copy_subtree(tree, tree[id], false));
				}
			}
		}
		else if (full_dest)
		{
			std::string listed_schema, listed_path;
			const auto exists = find_listed(source, source_schema, source_root, listed_schema, listed_path);
			const tree_snapshot tree(source);
			const auto* const found = exists ? tree.find(listed_schema, listed_path) : nullptr;
			if (!found || !libxmp::is_struct(found->options)) throw libxmp::error("Source must be an existing struct");
			if (!dest.empty()) throw libxmp::error("Destination tree must be empty");

			for (const auto id : found->children)
			{
				const auto& field = tree[id];
				const auto colon = field.name.find(':');
				if (colon == std::string::npos) continue;

				std::string uri;
				if (!libxmp::namespace_uri(field.name.substr(0, colon), &uri)) throw libxmp::error("Source field namespace is not global");
				write_subtree(dest, uri, field.name, copy_subtree(tree, field, false));
			}
		}
		else
		{
			check_path(source_schema, source_root);
			check_path(dest_schema, dest_root);

			std::string listed_schema, listed_path;
			if (!find_listed(source, source_schema, source_root, listed_schema, listed_path)) throw libxmp::error("Can't find source subtree");
			if (dest.exists(dest_schema, dest_root)) throw libxmp::error("Destination subtree must not exist");

			const tree_snapshot tree(source);
			const auto* const found = tree.find(listed_schema, listed_path);
			if (!found) throw libxmp::error("Can't find source subtree");

			const auto copy = copy_subtree(tree, *found, false);
			const auto inside = same && lands_inside(source, source_schema, source_root, dest_schema, dest_root);

			// Made as the toolkit made it - no value, no options - and left in place if it lands inside the source.
			dest.set_composite(dest_schema, dest_root, 0);
			if (inside) throw libxmp::error("Destination subtree is within the source subtree");

			write_subtree(dest, dest_schema, std::string(dest_root), copy);
		}
	}

	// The toolkit's MergeFromJPEG: ApplyTemplate replacing existing properties, internal ones included, then
	// the standard XMP's pointer to the extended XMP removed.
	void merge_from_jpeg(libxmp::meta& full, const libxmp::meta& extended)
	{
		if (&full == &extended) throw libxmp::error("Full and extended XMP pointers match");

		const tree_snapshot tree(extended);

		for (const auto schema : tree.schemas())
		{
			const auto& uri = tree[schema].schema;

			for (const auto id : tree[schema].children)
			{
				const auto& property = tree[id];
				const auto value_is_empty = libxmp::is_simple(property.options) ? property.value.empty() : property.children.empty();
				if (value_is_empty) continue;

				const auto copy = copy_subtree(tree, property, true);

				if (!full.exists(uri, property.name))
				{
					if (!copy.value.empty() || !copy.children.empty()) write_subtree(full, uri, property.name, copy);
				}
				else if (!libxmp::is_simple(copy.options) && copy.children.empty())
				{
					full.delete_property(uri, property.name);
				}
				else
				{
					write_subtree(full, uri, property.name, copy, true);
				}
			}
		}

		full.delete_property(libxmp::ns::xmp_note, "HasExtendedXMP");
	}

	// The toolkit's MoveOneProperty: a top-level property goes to the end of its schema in `to`.
	bool move_property(libxmp::meta& from, libxmp::meta& to, const std::string_view schema, const std::string_view name)
	{
		const tree_snapshot tree(from);
		const auto* const found = tree.find(schema, name);
		if (!found) return false;

		write_subtree(to, schema, std::string(name), copy_subtree(tree, *found, false));
		from.delete_property(schema, name);
		return true;
	}

	// The toolkit's estimate of a node's serialized size, by which PackageForJPEG picks what to move first.
	size_t estimate_size(const tree_snapshot& tree, const tree_node& node, const bool in_array)
	{
		size_t size = 0;
		const auto name_size = node.name.size();

		if (libxmp::is_simple(node.options))
		{
			if (!in_array) size += name_size + 3;
			size += node.value.size();
		}
		else if (libxmp::is_array(node.options))
		{
			if (!in_array) size += 2 * name_size + 5;
			size += 9 + 10 + node.children.size() * (8 + 9);
			for (const auto id : node.children) size += estimate_size(tree, tree[id], true);
		}
		else
		{
			if (!in_array) size += 2 * name_size + 5;
			size += 25;
			for (const auto id : node.children) size += estimate_size(tree, tree[id], false);
		}

		return size;
	}

	// The toolkit's PackageForJPEG: the standard packet, which must fit in a JPEG APP1 segment, and, when not
	// everything fits, the extended XMP and its MD5 digest, which the standard packet then names.
	void package_for_jpeg(const libxmp::meta& original, std::string& standard, std::string& extended, std::string& digest)
	{
		constexpr size_t limit = 65000;
		constexpr std::string_view trailer = "<?xpacket end=\"w\"?>";

		const auto small = [](const libxmp::meta& m, const bool packet)
		{
			libxmp::serialize_options options;
			options.use_compact_format = true;
			options.omit_all_formatting = true;
			options.omit_packet_wrapper = !packet;
			options.padding = packet ? 1 : 0;
			return serialize(m, options);
		};

		standard.clear();
		extended.clear();
		digest.clear();

		libxmp::meta std_xmp;
		libxmp::meta ext_xmp;
		auto text = small(original, true);

		if (text.size() > limit)
		{
			std_xmp = original;

			if (std_xmp.exists(libxmp::ns::xmp, "Thumbnails"))
			{
				std_xmp.delete_property(libxmp::ns::xmp, "Thumbnails");
				text = small(std_xmp, true);
			}
		}

		if (text.size() > limit)
		{
			// A stand-in of the digest's length until the digest is known.
			std_xmp.set_property(libxmp::ns::xmp_note, "HasExtendedXMP", "123456789-123456789-123456789-12");

			const tree_snapshot tree(std_xmp);

			if (const auto* const camera_raw = tree.find(libxmp::ns::camera_raw, {}))
			{
				for (const auto id : camera_raw->children)
				{
					write_subtree(ext_xmp, libxmp::ns::camera_raw, tree[id].name, copy_subtree(tree, tree[id], false));
				}

				for (const auto id : camera_raw->children) std_xmp.delete_property(libxmp::ns::camera_raw, tree[id].name);
				text = small(std_xmp, true);
			}
		}

		if (text.size() > limit)
		{
			if (move_property(std_xmp, ext_xmp, libxmp::ns::photoshop, "photoshop:History")) text = small(std_xmp, true);
		}

		if (text.size() > limit)
		{
			// Largest first. Among properties of one size the one earliest in the tree goes first, because the
			// map is filled from the end of the tree and an equal key goes after those already there.
			std::multimap<size_t, std::pair<std::string, std::string>> sizes;
			const tree_snapshot tree(std_xmp);

			for (auto s = tree.schemas().size(); s > 0; --s)
			{
				const auto& schema = tree[tree.schemas()[s - 1]];

				for (auto p = schema.children.size(); p > 0; --p)
				{
					const auto& property = tree[schema.children[p - 1]];
					if (schema.schema == libxmp::ns::xmp_note && property.name == "xmpNote:HasExtendedXMP") continue;

					const auto size = estimate_size(tree, property, false);
					sizes.insert(sizes.upper_bound(size), {size, {schema.schema, property.name}});
				}
			}

			while (text.size() > limit && !sizes.empty())
			{
				auto remaining = text.size();

				while (remaining > limit && !sizes.empty())
				{
					const auto largest = std::prev(sizes.end());
					auto size = largest->first;
					move_property(std_xmp, ext_xmp, largest->second.first, largest->second.second);
					sizes.erase(largest);

					if (size > remaining) size = remaining;
					remaining -= size;
				}

				text = small(std_xmp, true);
			}
		}

		if (text.size() > limit) throw libxmp::error("Can't reduce XMP enough for JPEG file");

		if (ext_xmp.empty())
		{
			standard = std::move(text);
		}
		else
		{
			extended = small(ext_xmp, false);

			dng_md5_direct_printer printer;
			printer.ProcessPtr(extended.data(), static_cast<uint32>(extended.size()));
			const auto* const bytes = printer.Result().Data();

			constexpr char hex[] = "0123456789ABCDEF";
			for (size_t i = 0; i < dng_fingerprint::kDNGFingerprintSize; ++i)
			{
				digest += hex[bytes[i] >> 4];
				digest += hex[bytes[i] & 0xF];
			}

			std_xmp.set_property(libxmp::ns::xmp_note, "HasExtendedXMP", digest);
			standard = small(std_xmp, true);
		}

		// Up to 2 KB of padding, as far as the limit allows.
		const auto padding = std::min<size_t>(limit - standard.size(), 2047);
		standard.erase(standard.size() - trailer.size());
		standard.append(padding, ' ');
		standard += trailer;
	}

	void remove_properties(libxmp::meta& m, const std::string_view schema)
	{
		const tree_snapshot tree(m);

		for (const auto id : tree.schemas())
		{
			if (!schema.empty() && tree[id].schema != schema) continue;
			for (const auto child : tree[id].children) m.delete_property(tree[id].schema, tree[child].name);
		}
	}
}

dng_xmp_sdk::dng_xmp_sdk() : fPrivate(new dng_xmp_private)
{
}

dng_xmp_sdk::dng_xmp_sdk(const dng_xmp_sdk& sdk) : fPrivate(nullptr)
{
	dng_xmp_lifecycle_lock lock(s_lifecycle_mutex);

	fPrivate = new dng_xmp_private(*sdk.fPrivate);
	if (fPrivate->meta) s_meta_count++;
}

dng_xmp_sdk::~dng_xmp_sdk()
{
	dng_xmp_lifecycle_lock lock(s_lifecycle_mutex);

	if (fPrivate)
	{
		if (fPrivate->meta)
		{
			DNG_REQUIRE(s_meta_count > 0, "Invalid XMP metadata count");
			s_meta_count--;
		}

		delete fPrivate;
	}
}

void dng_xmp_sdk::InitializeSDK(dng_xmp_namespace* extraNamespaces, const char* software)
{
	dng_xmp_lifecycle_lock lock(s_lifecycle_mutex);

	if (!s_initialized)
	{
		guard("Initialization", true, [&]
		{
			// libfiles needs no start-up. These are the SDK's own namespaces, which neither libfiles nor the
			// toolkit registers, with the prefixes the SDK writes them with. crx is no longer read, but is
			// registered so that saving new settings can clean it up.
			const dng_xmp_namespace namespaces[] = {
				{XMP_NS_CRX, "crx"},
				{XMP_NS_CRSS, "crss"},
				{XMP_NS_CRD, "crd"},
				{XMP_NS_CRLCP, "crlcp"},
				{XMP_NS_CRGI, "crgi"},
				{XMP_NS_CRVC, "crvc"},
				{XMP_NS_VFS, "vfs"},
				{XMP_NS_LR, "lr"},
				{XMP_NS_LCP, "stCamera"},
				{XMP_NS_DNG, "dng"},
				{XMP_NS_PANO, "panorama"},
				{XMP_NS_GPANO, "GPano"},
				{XMP_NS_REGIONS, "mwg-rs"},
				{XMP_NS_HDRGM, "hdrgm"},
				{XMP_NS_APPLE_HDRGM, "HDRGainMap"},
				{XMP_NS_APPLE_PIXELDATA, "apdi"},
				{XMP_NS_HDR_META, "hdr_metadata"},
				{XMP_NS_DC_TERMS, "dcterms"},
			};

			for (const auto& n : namespaces) libxmp::register_namespace(n.fullName, n.shortName);

			for (; extraNamespaces && extraNamespaces->fullName; ++extraNamespaces)
			{
				libxmp::register_namespace(extraNamespaces->fullName, view_of(extraNamespaces->shortName));
			}

			(void)software;
		});

		s_initialized = true;
	}
}

void dng_xmp_sdk::TerminateSDK()
{
	dng_xmp_lifecycle_lock lock(s_lifecycle_mutex);

	if (s_initialized)
	{
		// CR-4210035, kept from the SDK: this never throws, because it runs during shutdown from noexcept
		// destructors, and it refuses while metadata is still alive. libfiles keeps its namespace table for
		// the life of the process, so there is nothing else to tear down.
		if (s_meta_count != 0)
		{
			DNG_ASSERT(s_meta_count == 0, "XMP termination with active metadata");
			return;
		}

		s_initialized = false;
	}
}

bool dng_xmp_sdk::HasMeta() const
{
	return fPrivate->meta != nullptr;
}

void dng_xmp_sdk::ClearMeta()
{
	dng_xmp_lifecycle_lock lock(s_lifecycle_mutex);

	if (HasMeta())
	{
		fPrivate->meta.reset();
		++fPrivate->generation;

		DNG_REQUIRE(s_meta_count > 0, "Invalid XMP metadata count");
		s_meta_count--;
	}
}

void dng_xmp_sdk::MakeMeta()
{
	dng_xmp_lifecycle_lock lock(s_lifecycle_mutex);

	ClearMeta();
	InitializeSDK();

	guard("MakeMeta", true, [&]
	{
		fPrivate->meta = std::make_unique<libxmp::meta>();
		++fPrivate->generation;
		s_meta_count++;
	});
}

void dng_xmp_sdk::NeedMeta()
{
	if (!HasMeta()) MakeMeta();
}

// A libfiles::xmp::meta, where the SDK's own backend handed out an SXMPMeta.
void* dng_xmp_sdk::GetPrivateMeta()
{
	NeedMeta();

	fPrivate->shared = true;
	return fPrivate->meta.get();
}

void dng_xmp_sdk::Parse(dng_host& host, const char* buffer, uint32 count)
{
	MakeMeta();

	try
	{
		guard("ParseFromBuffer", true, [&]
		{
			if (!buffer && count) throw libxmp::error("Null parse buffer");

			// The toolkit's kXMP_UseNullTermination.
			const auto length = count == 0xFFFFFFFFu ? strnlen(buffer, count) : count;

			fPrivate->edit().parse(std::string_view(buffer, length));
		});
	}
	catch (dng_exception& except)
	{
		ClearMeta();
		if (host.IsTransientError(except.ErrorCode())) throw;
		ThrowBadFormat();
	}
}

void dng_xmp_sdk::AppendArrayItem(const char* ns, const char* arrayName, const char* itemValue, bool isBag, bool propIsStruct)
{
	NeedMeta();

	guard("AppendArrayItem", true, [&]
	{
		append_array_item(fPrivate->edit(), view_of(ns), view_of(arrayName),
		                  isBag ? libxmp::value_is_array : libxmp::array_is_ordered, itemValue,
		                  propIsStruct ? libxmp::value_is_struct : 0);
	});
}

int32 dng_xmp_sdk::CountArrayItems(const char* ns, const char* path) const
{
	int32 count = 0;

	if (HasMeta())
	{
		guard("CountArrayItems", false, [&] { count = fPrivate->meta->count_array_items(view_of(ns), view_of(path)); });
	}

	return count;
}

bool dng_xmp_sdk::Exists(const char* ns, const char* path) const
{
	if (HasMeta())
	{
		try
		{
			return fPrivate->meta->exists(view_of(ns), view_of(path));
		}
		catch (...)
		{
			// A path that cannot be read names nothing.
		}
	}

	return false;
}

bool dng_xmp_sdk::HasNameSpace(const char* ns) const
{
	auto result = false;

	if (HasMeta())
	{
		guard("HasNameSpace", true, [&]
		{
			// The toolkit asked a schema iterator, which is empty unless the schema holds a property; with no
			// schema it walks them all.
			const auto schema = view_of(ns);

			for (const auto& p : fPrivate->meta->properties())
			{
				if (!p.path.empty() && (schema.empty() || p.schema == schema))
				{
					result = true;
					break;
				}
			}
		});
	}

	return result;
}

void dng_xmp_sdk::Remove(const char* ns, const char* path)
{
	if (HasMeta())
	{
		guard("DeleteProperty", false, [&] { fPrivate->edit().delete_property(view_of(ns), view_of(path)); });
	}
}

void dng_xmp_sdk::RemoveProperties(const char* ns)
{
	if (HasMeta())
	{
		try
		{
			remove_properties(fPrivate->edit(), view_of(ns));
		}
		catch (...)
		{
			// The SDK ignored a failure here; what was removed stays removed.
		}
	}
}

bool dng_xmp_sdk::IsEmptyString(const char* ns, const char* path)
{
	auto result = false;

	if (HasMeta())
	{
		guard("IsEmptyString", false, [&]
		{
			std::string value;
			option_bits options = 0;

			if (fPrivate->meta->get_property(view_of(ns), view_of(path), &value, &options) && libxmp::is_simple(options))
			{
				result = value.empty();
			}
		});
	}

	return result;
}

bool dng_xmp_sdk::IsEmptyArray(const char* ns, const char* path)
{
	auto result = false;

	if (HasMeta())
	{
		guard("IsEmptyArray", false, [&]
		{
			const auto& meta = *fPrivate->meta;
			std::string value;
			option_bits options = 0;

			if (!meta.get_property(view_of(ns), view_of(path), &value, &options) || !libxmp::is_array(options)) return;

			// No first item, or a first item that is an empty string and no second.
			if (!get_array_item(meta, view_of(ns), view_of(path), 1, &value, &options))
			{
				result = true;
			}
			else if (libxmp::is_simple(options) && value.empty())
			{
				result = !get_array_item(meta, view_of(ns), view_of(path), 2, &value, &options);
			}
		});
	}

	return result;
}

void dng_xmp_sdk::ComposeArrayItemPath(const char* ns, const char* arrayName, int32 index, dng_string& s) const
{
	guard("ComposeArrayItemPath", true, [&] { s.Set(compose_array_item_path(view_of(ns), view_of(arrayName), index).c_str()); });
}

void dng_xmp_sdk::ComposeStructFieldPath(const char* ns, const char* structName, const char* fieldNS, const char* fieldName,
                                         dng_string& s) const
{
	guard("ComposeStructFieldPath", true, [&]
	{
		s.Set(compose_struct_field_path(view_of(ns), view_of(structName), view_of(fieldNS), view_of(fieldName)).c_str());
	});
}

bool dng_xmp_sdk::GetNamespacePrefix(const char* uri, dng_string& s) const
{
	auto result = false;

	if (HasMeta())
	{
		guard("GetNamespacePrefix", false, [&]
		{
			require(view_of(uri), "Empty namespace URI");

			// An unregistered namespace has an empty prefix.
			std::string prefix;
			libxmp::namespace_prefix(uri, &prefix);
			s.Set(prefix.c_str());
			result = true;
		});
	}

	return result;
}

bool dng_xmp_sdk::GetString(const char* ns, const char* path, dng_string& s) const
{
	auto result = false;

	if (HasMeta())
	{
		guard("GetProperty", false, [&]
		{
			std::string value;

			if (fPrivate->meta->get_property(view_of(ns), view_of(path), &value))
			{
				s.Set(value.c_str());
				result = true;
			}
		});
	}

	return result;
}

void dng_xmp_sdk::ValidateStringList(const char* ns, const char* path)
{
	if (Exists(ns, path))
	{
		auto bogus = true;

		try
		{
			std::string value;
			for (int32 index = 1; get_array_item(*fPrivate->meta, view_of(ns), view_of(path), index, &value); ++index)
			{
			}

			bogus = false;
		}
		catch (...)
		{
			// Not an array that can be read: it is removed below.
		}

		if (bogus) Remove(ns, path);
	}
}

bool dng_xmp_sdk::GetStringList(const char* ns, const char* path, dng_string_list& list, dng_abort_sniffer* sniffer) const
{
	auto result = false;

	if (HasMeta())
	{
		guard("GetStringList", false, [&]
		{
			std::string value;

			for (int32 index = 1; get_array_item(*fPrivate->meta, view_of(ns), view_of(path), index, &value); ++index)
			{
				if ((list.Count() & 0x3FF) == 0) dng_abort_sniffer::SniffForAbort(sniffer);

				dng_string s;
				s.Set(value.c_str());
				list.Append(s);
				result = true;
			}
		});
	}

	return result;
}

bool dng_xmp_sdk::GetAltLangDefault(const char* ns, const char* path, dng_string& s, bool silent) const
{
	auto result = false;

	if (HasMeta())
	{
		guard("GetLocalizedText", false, [&]
		{
			const auto& meta = *fPrivate->meta;
			std::string value;

			if (meta.get_localized_text(view_of(ns), view_of(path), "x-default", "x-default", nullptr, &value))
			{
				s.Set(value.c_str());
				result = true;
			}
			// An empty alt-text array reads as an empty string, as an array whose only item is empty does.
			else if (meta.get_property(view_of(ns), view_of(path), &value) && value.empty())
			{
				s.Clear();
				result = true;
			}
		}, silent);
	}

	return result;
}

bool dng_xmp_sdk::GetLocalString(const char* ns, const char* path, dng_local_string& s) const
{
	dng_string defaultText;
	if (!GetAltLangDefault(ns, path, defaultText, true)) return false;

	s.SetDefaultText(defaultText);

	guard("GetLocalString", false, [&]
	{
		const int32 count = CountArrayItems(ns, path);
		if (count <= 1) return;

		// The first item is the default. As in the SDK, the walk runs one past the last item.
		for (int32 index = 1; index <= count; index++)
		{
			dng_string itemPath;
			ComposeArrayItemPath(ns, path, index + 1, itemPath);

			std::string lang;
			const auto lang_path = compose_qualifier_path(view_of(ns), itemPath.Get(), libxmp::ns::xml, "lang");
			if (!fPrivate->meta->get_property(view_of(ns), lang_path, &lang)) continue;

			dng_string language;
			language.Set(lang.c_str());
			if (language.IsEmpty() || language.Matches("x-default")) continue;

			std::string text;

			if (fPrivate->meta->get_property(view_of(ns), itemPath.Get(), &text))
			{
				dng_string translation;
				translation.Set(text.c_str());
				s.AddTranslation(language, translation);
			}
		}
	});

	return true;
}

bool dng_xmp_sdk::GetStructField(const char* ns, const char* path, const char* fieldNS, const char* fieldName, dng_string& s) const
{
	auto result = false;

	if (HasMeta())
	{
		guard("GetStructField", false, [&]
		{
			const auto field = compose_struct_field_path(view_of(ns), view_of(path), view_of(fieldNS), view_of(fieldName));
			std::string value;

			if (fPrivate->meta->get_property(view_of(ns), field, &value))
			{
				s.Set(value.c_str());
				result = true;
			}
		});
	}

	return result;
}

void dng_xmp_sdk::Set(const char* ns, const char* path, const char* text)
{
	NeedMeta();

	try
	{
		set_property(fPrivate->edit(), view_of(ns), view_of(path), text);
		return;
	}
	catch (...)
	{
		// Failed for some reason: the existing value is removed and the set tried again.
	}

	Remove(ns, path);

	guard("SetProperty", true, [&] { set_property(fPrivate->edit(), view_of(ns), view_of(path), text); });
}

void dng_xmp_sdk::SetString(const char* ns, const char* path, const dng_string& s)
{
	dng_string ss(s);
	ss.SetLineEndings('\n');
	ss.StripLowASCII();

	Set(ns, path, ss.Get());
}

void dng_xmp_sdk::SetStringList(const char* ns, const char* path, const dng_string_list& list, bool isBag)
{
	Remove(ns, path);

	if (list.Count())
	{
		NeedMeta();

		for (uint32 index = 0; index < list.Count(); index++)
		{
			dng_string s(list[index]);
			s.SetLineEndings('\n');
			s.StripLowASCII();

			guard("AppendArrayItem", true, [&]
			{
				fPrivate->edit().append_array_item(view_of(ns), view_of(path),
				                                   isBag ? libxmp::value_is_array : libxmp::array_is_ordered, s.Get());
			});
		}
	}
}

void dng_xmp_sdk::SetAltLangDefault(const char* ns, const char* path, const dng_string& s)
{
	NeedMeta();

	Remove(ns, path);

	dng_string ss(s);
	ss.SetLineEndings('\n');
	ss.StripLowASCII();

	guard("SetLocalizedText", true, [&]
	{
		fPrivate->edit().set_localized_text(view_of(ns), view_of(path), "x-default", "x-default", ss.Get());
	});
}

void dng_xmp_sdk::SetLocalString(const char* ns, const char* path, const dng_local_string& s)
{
	SetAltLangDefault(ns, path, s.DefaultText());

	guard("SetLocalizedText", true, [&]
	{
		for (uint32 index = 0; index < s.TranslationCount(); index++)
		{
			dng_string itemPath;
			ComposeArrayItemPath(ns, path, index + 2, itemPath);

			auto& meta = fPrivate->edit();
			meta.set_property(view_of(ns), itemPath.Get(), s.Translation(index).Get());
			set_qualifier(meta, view_of(ns), itemPath.Get(), libxmp::ns::xml, "lang", s.Language(index).Get());
		}
	});
}

void dng_xmp_sdk::SetStructField(const char* ns, const char* path, const char* fieldNS, const char* fieldName, const char* text)
{
	NeedMeta();

	guard("SetStructField", true, [&]
	{
		const auto field = compose_struct_field_path(view_of(ns), view_of(path), view_of(fieldNS), view_of(fieldName));
		set_property(fPrivate->edit(), view_of(ns), field, text);
	});
}

void dng_xmp_sdk::DeleteStructField(const char* ns, const char* structName, const char* fieldNS, const char* fieldName)
{
	if (HasMeta())
	{
		try
		{
			const auto field = compose_struct_field_path(view_of(ns), view_of(structName), view_of(fieldNS), view_of(fieldName));
			fPrivate->edit().delete_property(view_of(ns), field);
		}
		catch (...)
		{
			// The SDK ignored a field it could not delete.
		}
	}
}

dng_memory_block* dng_xmp_sdk::Serialize(dng_memory_allocator& allocator, bool asPacket, uint32 targetBytes, uint32 padBytes,
                                         bool forJPEG, bool compact) const
{
	// The largest XMP packet that can be embedded in a JPEG by the normal methods.
	constexpr uint32 kJPEG_XMP_Limit = 65504;

	if (!HasMeta()) return nullptr;

	std::string s;
	auto havePacket = false;

	libxmp::serialize_options options;
	options.use_compact_format = compact;
	options.indent = " ";

	if (asPacket && targetBytes)
	{
		try
		{
			auto exact = options;
			exact.exact_packet_length = true;
			exact.padding = targetBytes;
			s = serialize(*fPrivate->meta, exact);
			havePacket = true;
		}
		catch (...)
		{
			// Most likely the packet cannot fit in the target byte count, so it is tried again without the limit.
		}
	}

	if (!havePacket)
	{
		guard("SerializeToBuffer", true, [&]
		{
			auto plain = options;
			plain.omit_packet_wrapper = !asPacket;
			plain.padding = asPacket ? padBytes : 0;
			s = serialize(*fPrivate->meta, plain);
		});
	}

	auto packetLen = static_cast<uint32>(s.size());

	if (forJPEG && asPacket && padBytes > 0 && targetBytes <= kJPEG_XMP_Limit && packetLen > kJPEG_XMP_Limit)
	{
		const auto overLimitCount = packetLen - kJPEG_XMP_Limit;
		padBytes = overLimitCount > padBytes ? 0 : padBytes - overLimitCount;

		guard("SerializeToBuffer", true, [&]
		{
			auto packet = options;
			packet.padding = padBytes;
			s = serialize(*fPrivate->meta, packet);
		});

		packetLen = static_cast<uint32>(s.size());
	}

	if (!packetLen) return nullptr;

	AutoPtr<dng_memory_block> buffer(allocator.Allocate(packetLen));
	memcpy(buffer->Buffer(), s.data(), packetLen);
	return buffer.Release();
}

void dng_xmp_sdk::PackageForJPEG(dng_memory_allocator& allocator, AutoPtr<dng_memory_block>& stdBlock,
                                 AutoPtr<dng_memory_block>& extBlock, dng_string& extDigest) const
{
	if (!HasMeta()) return;

	std::string stdStr;
	std::string extStr;
	std::string digestStr;

	guard("PackageForJPEG", true, [&] { package_for_jpeg(*fPrivate->meta, stdStr, extStr, digestStr); });

	if (!stdStr.empty())
	{
		stdBlock.Reset(allocator.Allocate(static_cast<uint32>(stdStr.size())));
		memcpy(stdBlock->Buffer(), stdStr.data(), stdStr.size());
	}

	if (!extStr.empty())
	{
		extBlock.Reset(allocator.Allocate(static_cast<uint32>(extStr.size())));
		memcpy(extBlock->Buffer(), extStr.data(), extStr.size());

		if (digestStr.size() != 32) ThrowProgramError();
		extDigest.Set(digestStr.c_str());
	}
}

void dng_xmp_sdk::MergeFromJPEG(const dng_xmp_sdk* xmp)
{
	if (xmp && xmp->HasMeta())
	{
		NeedMeta();

		guard("MergeFromJPEG", true, [&] { merge_from_jpeg(fPrivate->edit(), *xmp->fPrivate->meta); });
	}
}

void dng_xmp_sdk::ReplaceXMP(dng_xmp_sdk* xmp)
{
	ClearMeta();

	if (xmp && xmp->HasMeta())
	{
		fPrivate->meta = std::move(xmp->fPrivate->meta);
		++fPrivate->generation;
		++xmp->fPrivate->generation;
	}
}

bool dng_xmp_sdk::IteratePaths(IteratePathsCallback* callback, void* callbackData, const char* startingNS,
                               const char* startingPath, bool justChildren)
{
	if (!HasMeta()) return true;

	auto stopped = false;

	guard("IteratePaths", true, [&]
	{
		path_iterator iterator(*fPrivate, view_of(startingNS), view_of(startingPath), justChildren);
		std::string ns;
		std::string path;

		while (iterator.next(ns, path))
		{
			if (!callback(ns.c_str(), path.c_str(), callbackData))
			{
				stopped = true;
				return;
			}
		}
	});

	return !stopped;
}

void dng_xmp_sdk::DuplicateSubtree(const dng_xmp_sdk& source, const char* sourceNS, const char* sourceRoot, const char* destNS,
                                   const char* destRoot)
{
	if (source.HasMeta())
	{
		NeedMeta();

		guard("DuplicateSubtree", true, [&]
		{
			duplicate_subtree(*source.fPrivate->meta, fPrivate->edit(), view_of(sourceNS), view_of(sourceRoot),
			                  destNS ? view_of(destNS) : view_of(sourceNS), destRoot ? view_of(destRoot) : view_of(sourceRoot));
		});
	}
}

#endif // qDNGUseXMP
