// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The XMP data model - parse, query, edit, iterate and serialize XMP packets. It behaves
// as Adobe's XMP Toolkit (XMPCore) does for the surface image and media applications use: the same
// recovery from malformed packets, the same alias table, the same touch-ups of legacy forms, and
// the same rules for localized text and arrays.

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace libfiles::xmp
{
	// Namespace URIs. Each of these is registered with its conventional prefix before any packet is
	// parsed, so a packet that spells a known URI with another prefix still reads under the usual one.
	namespace ns
	{
		inline constexpr std::string_view xml = "http://www.w3.org/XML/1998/namespace";
		inline constexpr std::string_view rdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
		inline constexpr std::string_view x = "adobe:ns:meta/";
		inline constexpr std::string_view dc = "http://purl.org/dc/elements/1.1/";
		inline constexpr std::string_view xmp = "http://ns.adobe.com/xap/1.0/";
		inline constexpr std::string_view xmp_rights = "http://ns.adobe.com/xap/1.0/rights/";
		inline constexpr std::string_view xmp_mm = "http://ns.adobe.com/xap/1.0/mm/";
		inline constexpr std::string_view xmp_bj = "http://ns.adobe.com/xap/1.0/bj/";
		inline constexpr std::string_view xmp_note = "http://ns.adobe.com/xmp/note/";
		inline constexpr std::string_view xmp_text = "http://ns.adobe.com/xap/1.0/t/";
		inline constexpr std::string_view xmp_paged_file = "http://ns.adobe.com/xap/1.0/t/pg/";
		inline constexpr std::string_view xmp_graphics = "http://ns.adobe.com/xap/1.0/g/";
		inline constexpr std::string_view xmp_image = "http://ns.adobe.com/xap/1.0/g/img/";
		inline constexpr std::string_view xmp_font = "http://ns.adobe.com/xap/1.0/sType/Font#";
		inline constexpr std::string_view xmp_dimensions = "http://ns.adobe.com/xap/1.0/sType/Dimensions#";
		inline constexpr std::string_view xmp_resource_event = "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#";
		inline constexpr std::string_view xmp_resource_ref = "http://ns.adobe.com/xap/1.0/sType/ResourceRef#";
		inline constexpr std::string_view xmp_version = "http://ns.adobe.com/xap/1.0/sType/Version#";
		inline constexpr std::string_view xmp_job = "http://ns.adobe.com/xap/1.0/sType/Job#";
		inline constexpr std::string_view xmp_manifest_item = "http://ns.adobe.com/xap/1.0/sType/ManifestItem#";
		inline constexpr std::string_view xmp_identifier_qual = "http://ns.adobe.com/xmp/Identifier/qual/1.0/";
		inline constexpr std::string_view pdf = "http://ns.adobe.com/pdf/1.3/";
		inline constexpr std::string_view pdfx = "http://ns.adobe.com/pdfx/1.3/";
		inline constexpr std::string_view pdfx_id = "http://www.npes.org/pdfx/ns/id/";
		inline constexpr std::string_view pdfa_schema = "http://www.aiim.org/pdfa/ns/schema#";
		inline constexpr std::string_view pdfa_property = "http://www.aiim.org/pdfa/ns/property#";
		inline constexpr std::string_view pdfa_type = "http://www.aiim.org/pdfa/ns/type#";
		inline constexpr std::string_view pdfa_field = "http://www.aiim.org/pdfa/ns/field#";
		inline constexpr std::string_view pdfa_id = "http://www.aiim.org/pdfa/ns/id/";
		inline constexpr std::string_view pdfa_extension = "http://www.aiim.org/pdfa/ns/extension/";
		inline constexpr std::string_view photoshop = "http://ns.adobe.com/photoshop/1.0/";
		inline constexpr std::string_view ps_album = "http://ns.adobe.com/album/1.0/";
		inline constexpr std::string_view exif = "http://ns.adobe.com/exif/1.0/";
		inline constexpr std::string_view exif_aux = "http://ns.adobe.com/exif/1.0/aux/";
		inline constexpr std::string_view exif_ex = "http://cipa.jp/exif/1.0/";
		inline constexpr std::string_view tiff = "http://ns.adobe.com/tiff/1.0/";
		inline constexpr std::string_view png = "http://ns.adobe.com/png/1.0/";
		inline constexpr std::string_view jpeg = "http://ns.adobe.com/jpeg/1.0/";
		inline constexpr std::string_view jp2k = "http://ns.adobe.com/jp2k/1.0/";
		inline constexpr std::string_view camera_raw = "http://ns.adobe.com/camera-raw-settings/1.0/";
		inline constexpr std::string_view dm = "http://ns.adobe.com/xmp/1.0/DynamicMedia/";
		inline constexpr std::string_view script = "http://ns.adobe.com/xmp/1.0/Script/";
		inline constexpr std::string_view asf = "http://ns.adobe.com/asf/1.0/";
		inline constexpr std::string_view wav = "http://ns.adobe.com/xmp/wav/1.0/";
		inline constexpr std::string_view bwf = "http://ns.adobe.com/bwf/bext/1.0/";
		inline constexpr std::string_view aes_cart = "http://ns.adobe.com/aes/cart/";
		inline constexpr std::string_view riff_info = "http://ns.adobe.com/riff/info/";
		inline constexpr std::string_view ixml = "http://ns.adobe.com/ixml/1.0/";
		inline constexpr std::string_view stock_photo = "http://ns.adobe.com/StockPhoto/1.0/";
		inline constexpr std::string_view creator_atom = "http://ns.adobe.com/creatorAtom/1.0/";
		inline constexpr std::string_view iptc_core = "http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/";
		inline constexpr std::string_view iptc_ext = "http://iptc.org/std/Iptc4xmpExt/2008-02-29/";
		inline constexpr std::string_view dicom = "http://ns.adobe.com/DICOM/";
		inline constexpr std::string_view plus = "http://ns.useplus.org/ldf/xmp/1.0/";
		inline constexpr std::string_view ix = "http://ns.adobe.com/iX/1.0/";
		inline constexpr std::string_view microsoft_photo = "http://ns.microsoft.com/photo/1.0/";
	}

	// Option bits describing a node. The values are those of the XMP Toolkit, so code written
	// against it reads naturally.
	using option_bits = std::uint32_t;

	inline constexpr option_bits value_is_uri = 0x00000002;
	inline constexpr option_bits has_qualifiers = 0x00000010;
	inline constexpr option_bits is_qualifier = 0x00000020;
	inline constexpr option_bits has_lang = 0x00000040;
	inline constexpr option_bits has_type = 0x00000080;
	inline constexpr option_bits value_is_struct = 0x00000100;
	inline constexpr option_bits value_is_array = 0x00000200;
	inline constexpr option_bits array_is_ordered = 0x00000400;
	inline constexpr option_bits array_is_alternate = 0x00000800;
	inline constexpr option_bits array_is_alt_text = 0x00001000;
	inline constexpr option_bits is_alias = 0x00010000;
	inline constexpr option_bits has_aliases = 0x00020000;
	inline constexpr option_bits delete_existing = 0x20000000;
	inline constexpr option_bits schema_node = 0x80000000;

	inline constexpr option_bits composite_mask = value_is_struct | value_is_array;
	inline constexpr option_bits array_form_mask = value_is_array | array_is_ordered | array_is_alternate |
		array_is_alt_text;

	constexpr bool is_array(const option_bits o) { return (o & value_is_array) != 0; }
	constexpr bool is_struct(const option_bits o) { return (o & value_is_struct) != 0; }
	constexpr bool is_simple(const option_bits o) { return (o & composite_mask) == 0; }
	constexpr bool is_ordered(const option_bits o) { return (o & array_is_ordered) != 0; }
	constexpr bool is_alternate(const option_bits o) { return (o & array_is_alternate) != 0; }
	constexpr bool is_alt_text(const option_bits o) { return (o & array_is_alt_text) != 0; }
	constexpr bool is_qualifier_node(const option_bits o) { return (o & is_qualifier) != 0; }
	constexpr bool is_schema(const option_bits o) { return (o & schema_node) != 0; }

	// Misuse of the data model - a value for an array, an index of zero, an unregistered namespace -
	// throws this, as the toolkit does. Parsing never throws for malformed input.
	class error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// One node of the tree, as iteration reports it.
	struct property_info
	{
		std::string schema; // The namespace URI of the top-level property.
		std::string path; // Empty for the schema node itself, otherwise "dc:subject[1]" and so on.
		std::string value; // Empty for arrays and structs.
		option_bits options = 0;
	};

	struct serialize_options
	{
		bool omit_packet_wrapper = false; // No <?xpacket?> header, padding or trailer: a sidecar.
		bool read_only_packet = false; // end="r" and no padding.
		bool use_compact_format = false; // Simple unqualified properties become attributes.
		bool exact_packet_length = false; // Pad the whole packet to exactly `padding` bytes.
		bool omit_all_formatting = false; // No newlines or indentation.
		std::uint32_t padding = 0; // Bytes of padding, 0 for the 2048 default.
		std::string_view newline = {}; // Defaults to LF.
		std::string_view indent = {}; // Defaults to three spaces, or one for the compact form.
		int base_indent = 0;
	};

	namespace detail
	{
		struct node;
	}

	class meta
	{
	public:
		meta();
		~meta();
		meta(const meta& other);
		meta& operator=(const meta& other);
		meta(meta&& other) noexcept;
		meta& operator=(meta&& other) noexcept;

		// Replaces the content with the packet in `text`, which may be UTF-8, UTF-16 or UTF-32, with or
		// without a byte-order mark or packet wrapper. A malformed packet is not an error: whatever
		// precedes the first fault is kept, as the toolkit keeps it. Text with no rdf:RDF element
		// leaves the object empty.
		void parse(std::string_view text);

		bool empty() const;
		void clear();

		// The value of rdf:about, which names the resource the packet describes.
		const std::string& about() const;

		// Paths name a top-level property in `schema` and may continue into it: "creator[2]",
		// "title[?xml:lang='x-default']", "Flash/exif:Fired", "subject[last()]". A top-level alias
		// such as tiff:Artist resolves to its actual property, dc:creator[1].
		bool get_property(std::string_view schema, std::string_view path, std::string* value = nullptr,
		                  option_bits* options = nullptr) const;
		bool exists(std::string_view schema, std::string_view path) const;
		bool get_property_int(std::string_view schema, std::string_view path, std::int32_t* value,
		                      option_bits* options = nullptr) const;

		// Arrays are one-based, as in XPath. A missing array counts zero items.
		int count_array_items(std::string_view schema, std::string_view array) const;
		bool get_array_item(std::string_view schema, std::string_view array, int index,
		                    std::string* value = nullptr, option_bits* options = nullptr) const;

		// Picks an item of an alt-text array: the specific language, then the generic one, then
		// x-default, then the first item.
		bool get_localized_text(std::string_view schema, std::string_view array, std::string_view generic_lang,
		                        std::string_view specific_lang, std::string* actual_lang, std::string* value,
		                        option_bits* options = nullptr) const;

		void set_property(std::string_view schema, std::string_view path, std::string_view value,
		                  option_bits options = 0);
		// Creates, or empties, an array or struct.
		void set_composite(std::string_view schema, std::string_view path, option_bits options);
		void set_property_int(std::string_view schema, std::string_view path, std::int32_t value,
		                      option_bits options = 0);
		void set_localized_text(std::string_view schema, std::string_view array, std::string_view generic_lang,
		                        std::string_view specific_lang, std::string_view value);
		void append_array_item(std::string_view schema, std::string_view array, option_bits array_options,
		                       std::string_view value, option_bits item_options = 0);
		void delete_property(std::string_view schema, std::string_view path);

		// Every node in document order: each schema, then each property followed by its qualifiers
		// and then its children.
		std::vector<property_info> properties() const;

		std::string serialize(const serialize_options& options = {}) const;

		const detail::node& tree() const { return *_tree; }
		detail::node& tree() { return *_tree; }

	private:
		std::unique_ptr<detail::node> _tree;
	};

	// The namespace table is shared by every meta object, as the toolkit's is. Registering a URI that
	// is already known returns its existing prefix; a prefix already taken by another URI is made
	// unique with a numeric suffix. Prefixes are returned with their trailing colon.
	std::string register_namespace(std::string_view uri, std::string_view suggested_prefix);
	bool namespace_prefix(std::string_view uri, std::string* prefix);
	bool namespace_uri(std::string_view prefix, std::string* uri);
}
