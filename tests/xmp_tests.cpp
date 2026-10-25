// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Tests of the XMP data model: parsing and its recovery from malformed packets, the
// toolkit's normalizations and aliases, the property API, iteration order and serialization.

#include "test.h"

#include "libfiles/xmp.h"

#include <string>

namespace xmp = libfiles::xmp;
namespace ns = libfiles::xmp::ns;

namespace
{
	std::string packet(const std::string_view body, const std::string_view extra_ns = {})
	{
		std::string result = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
			"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
			"<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
			"xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmlns:exif=\"http://ns.adobe.com/exif/1.0/\" "
			"xmlns:tiff=\"http://ns.adobe.com/tiff/1.0/\" xmlns:photoshop=\"http://ns.adobe.com/photoshop/1.0/\" "
			"xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\"";
		result += extra_ns;
		result += ">";
		result += body;
		result += "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
		return result;
	}

	xmp::meta parsed(const std::string_view text)
	{
		xmp::meta m;
		m.parse(text);
		return m;
	}

	std::string get(const xmp::meta& m, const std::string_view schema, const std::string_view path)
	{
		std::string value;
		return m.get_property(schema, path, &value) ? value : std::string("<absent>");
	}

	std::string x_default(const xmp::meta& m, const std::string_view schema, const std::string_view array)
	{
		std::string lang, value;
		return m.get_localized_text(schema, array, "", "x-default", &lang, &value) ? value : std::string("<absent>");
	}

	std::string dump(const xmp::meta& m)
	{
		std::string result;
		for (const auto& p : m.properties())
		{
			if (p.path.empty()) continue;
			result += p.path;
			if (!p.value.empty())
			{
				result += '=';
				result += p.value;
			}
			result += ';';
		}
		return result;
	}
}

TEST("xmp parses element and attribute properties")
{
	const auto m = parsed(packet("<xmp:Rating>4</xmp:Rating><xmp:Label>Red</xmp:Label>"));
	CHECK_STR(get(m, ns::xmp, "Rating"), "4");
	CHECK_STR(get(m, ns::xmp, "Label"), "Red");

	const auto attrs = parsed(
		"<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description rdf:about='' xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='5' xmp:Label='Blue'/>"
		"</rdf:RDF></x:xmpmeta>");
	CHECK_STR(get(attrs, ns::xmp, "Rating"), "5");
	CHECK_STR(get(attrs, ns::xmp, "Label"), "Blue");

	std::int32_t rating = 0;
	CHECK(attrs.get_property_int(ns::xmp, "Rating", &rating));
	CHECK_EQ(rating, 5);
}

TEST("xmp reads a packet without x:xmpmeta or a wrapper")
{
	const auto m = parsed("<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description xmlns:xmp='http://ns.adobe.com/xap/1.0/'><xmp:Rating>2</xmp:Rating></rdf:Description></rdf:RDF>");
	CHECK_STR(get(m, ns::xmp, "Rating"), "2");
}

TEST("xmp keeps what precedes a truncation")
{
	auto text = packet("<xmp:Rating>3</xmp:Rating><xmp:Label>Green</xmp:Label>");
	text.resize(text.find("<xmp:Label>") + 6);

	const auto m = parsed(text);
	CHECK_STR(get(m, ns::xmp, "Rating"), "3");
	CHECK_STR(get(m, ns::xmp, "Label"), "<absent>");

	// Expat delivers an unterminated CDATA section's content. A "]" that could still begin "]]>" is
	// held back, but the space the toolkit appends to 8-bit input settles it as text.
	auto cdata = packet("<xmp:Label>a<![CDATA[bc]]></xmp:Label>");
	cdata.resize(cdata.find("]]>") + 1);
	CHECK_STR(get(parsed(cdata), ns::xmp, "Label"), "abc] ");
}

TEST("xmp leaves an object empty for text with no RDF")
{
	CHECK(parsed("").empty());
	CHECK(parsed("not xml at all").empty());
	CHECK(parsed("<x:xmpmeta xmlns:x='adobe:ns:meta/'/>").empty());
	CHECK(parsed("<?xpacket begin=''?><x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF "
		"xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'><rdf:Description/></rdf:RDF></x:xmpmeta>").empty());
}

TEST("xmp stops at junk after the root but keeps the root")
{
	const auto m = parsed(packet("<xmp:Rating>1</xmp:Rating>") + "<junk/>");
	CHECK_STR(get(m, ns::xmp, "Rating"), "1");
}

TEST("xmp turns simple Dublin Core values into arrays")
{
	const auto m = parsed(packet("<dc:title>Lisbon</dc:title><dc:subject>tram</dc:subject><dc:creator>Ana</dc:creator>"
		"<dc:rights/>"));

	xmp::option_bits options = 0;
	CHECK(m.get_property(ns::dc, "title", nullptr, &options));
	CHECK(xmp::is_alt_text(options));
	CHECK_STR(x_default(m, ns::dc, "title"), "Lisbon");

	CHECK(m.get_property(ns::dc, "subject", nullptr, &options));
	CHECK(xmp::is_array(options) && !xmp::is_ordered(options));
	CHECK_EQ(m.count_array_items(ns::dc, "subject"), 1);
	CHECK_STR(get(m, ns::dc, "subject[1]"), "tram");

	CHECK(m.get_property(ns::dc, "creator", nullptr, &options));
	CHECK(xmp::is_ordered(options));

	// An empty value is an empty array, not an array holding an empty item.
	CHECK(m.exists(ns::dc, "rights"));
	CHECK_EQ(m.count_array_items(ns::dc, "rights"), 0);
}

TEST("xmp keeps an empty subject bag as an explicit empty list")
{
	const auto m = parsed(packet("<dc:subject><rdf:Bag/></dc:subject>"));
	CHECK(m.exists(ns::dc, "subject"));
	CHECK_EQ(m.count_array_items(ns::dc, "subject"), 0);
}

TEST("xmp moves explicit aliases to their actual properties")
{
	const auto m = parsed(packet("<tiff:Artist>Ann</tiff:Artist><photoshop:Caption>Quay</photoshop:Caption>"
		"<exif:DateTimeDigitized>2020-01-02T03:04:05</exif:DateTimeDigitized>"));

	CHECK_STR(get(m, ns::dc, "creator[1]"), "Ann");
	CHECK_STR(x_default(m, ns::dc, "description"), "Quay");
	CHECK_STR(get(m, ns::xmp, "CreateDate"), "2020-01-02T03:04:05");

	// Reading through an alias reaches the actual property.
	CHECK_STR(get(m, ns::exif, "DateTimeDigitized"), "2020-01-02T03:04:05");
	CHECK_STR(get(m, ns::tiff, "Artist"), "Ann");

	CHECK(dump(m).find("tiff:") == std::string::npos);
	CHECK(dump(m).find("exif:DateTimeDigitized") == std::string::npos);
}

TEST("xmp drops an alias whose actual property exists")
{
	const auto m = parsed(packet("<xmp:CreateDate>2021-05-05</xmp:CreateDate>"
		"<exif:DateTimeDigitized>1999-09-09</exif:DateTimeDigitized>"));
	CHECK_STR(get(m, ns::xmp, "CreateDate"), "2021-05-05");
	CHECK_STR(get(m, ns::exif, "DateTimeDigitized"), "2021-05-05");
}

TEST("xmp rejects an alias to x-default that has its own language")
{
	xmp::meta m;
	CHECK_THROWS(m.parse(packet("<photoshop:Caption xml:lang='fr'>Quai</photoshop:Caption>")));
	CHECK(m.empty());
}

TEST("xmp writes through an alias")
{
	xmp::meta m;
	m.set_property(ns::exif, "DateTimeDigitized", "2022-02-02T02:02:02");
	CHECK_STR(get(m, ns::xmp, "CreateDate"), "2022-02-02T02:02:02");

	m.set_property(ns::tiff, "ImageDescription", "Harbour");
	CHECK_STR(x_default(m, ns::dc, "description"), "Harbour");
}

TEST("xmp chooses localized text as the toolkit does")
{
	const auto m = parsed(packet("<dc:title><rdf:Alt><rdf:li xml:lang='fr'>Quai</rdf:li>"
		"<rdf:li xml:lang='en-us'>Quay</rdf:li></rdf:Alt></dc:title>"));

	std::string lang, value;
	CHECK(m.get_localized_text(ns::dc, "title", "", "x-default", &lang, &value));
	CHECK_STR(lang, "fr");
	CHECK_STR(value, "Quai");

	CHECK(m.get_localized_text(ns::dc, "title", "en", "en-GB", &lang, &value));
	CHECK_STR(lang, "en-US");
	CHECK_STR(value, "Quay");
}

TEST("xmp puts x-default first and mirrors a two-item array")
{
	const auto m = parsed(packet("<dc:title><rdf:Alt><rdf:li xml:lang='de'>Kai</rdf:li>"
		"<rdf:li xml:lang='x-default'>Quay</rdf:li></rdf:Alt></dc:title>"));

	CHECK_STR(get(m, ns::dc, "title[1]/?xml:lang"), "x-default");
	CHECK_STR(get(m, ns::dc, "title[1]"), "Quay");
	CHECK_STR(get(m, ns::dc, "title[2]"), "Quay");
}

TEST("xmp sets localized text as the toolkit does")
{
	xmp::meta m;
	m.set_localized_text(ns::dc, "title", "", "x-default", "One");
	CHECK_EQ(m.count_array_items(ns::dc, "title"), 1);
	CHECK_STR(x_default(m, ns::dc, "title"), "One");

	m.set_localized_text(ns::dc, "title", "", "x-default", "Two");
	CHECK_EQ(m.count_array_items(ns::dc, "title"), 1);
	CHECK_STR(x_default(m, ns::dc, "title"), "Two");

	// A specific language gets its own item; x-default, the only item until now, follows it.
	m.set_localized_text(ns::dc, "title", "", "fr", "Deux");
	CHECK_EQ(m.count_array_items(ns::dc, "title"), 2);
	CHECK_STR(x_default(m, ns::dc, "title"), "Deux");

	auto other = parsed(packet("<dc:title><rdf:Alt><rdf:li xml:lang='fr'>Quai</rdf:li></rdf:Alt></dc:title>"));
	other.set_localized_text(ns::dc, "title", "", "x-default", "Quay");
	CHECK_EQ(other.count_array_items(ns::dc, "title"), 2);
	CHECK_STR(get(other, ns::dc, "title[1]/?xml:lang"), "x-default");
	CHECK_STR(get(other, ns::dc, "title[2]"), "Quai");
}

TEST("xmp appends and counts array items")
{
	xmp::meta m;
	m.append_array_item(ns::dc, "subject", xmp::value_is_array, "a");
	m.append_array_item(ns::dc, "subject", xmp::value_is_array, "b");
	CHECK_EQ(m.count_array_items(ns::dc, "subject"), 2);

	std::string value;
	CHECK(m.get_array_item(ns::dc, "subject", 2, &value));
	CHECK_STR(value, "b");
	CHECK(!m.get_array_item(ns::dc, "subject", 3, &value));
	CHECK_THROWS(m.get_array_item(ns::dc, "subject", 0, &value));

	m.set_composite(ns::dc, "creator", xmp::array_is_ordered);
	xmp::option_bits options = 0;
	CHECK(m.get_property(ns::dc, "creator", nullptr, &options));
	CHECK(xmp::is_ordered(options));
	CHECK_EQ(m.count_array_items(ns::dc, "creator"), 0);
	CHECK_THROWS(m.append_array_item(ns::dc, "description", 0, "x"));
}

TEST("xmp refuses a value for a composite and replaces with delete_existing")
{
	auto m = parsed(packet("<tiff:Orientation>6</tiff:Orientation><dc:title>T</dc:title>"));
	CHECK_THROWS(m.set_property(ns::dc, "title", "plain"));

	m.set_property(ns::tiff, "Orientation", "1", xmp::delete_existing);
	CHECK_STR(get(m, ns::tiff, "Orientation"), "1");
}

TEST("xmp deletes a property and an emptied schema")
{
	auto m = parsed(packet("<xmp:Label>Red</xmp:Label><dc:title>T</dc:title>"));
	m.delete_property(ns::xmp, "Label");
	CHECK(!m.exists(ns::xmp, "Label"));

	for (const auto& p : m.properties())
	{
		CHECK(p.schema != ns::xmp);
	}

	m.delete_property(ns::xmp, "Label");
}

TEST("xmp iterates schemas, properties, qualifiers and children in order")
{
	const auto m = parsed(packet("<dc:title><rdf:Alt><rdf:li xml:lang='x-default'>T</rdf:li></rdf:Alt></dc:title>"
		"<dc:subject><rdf:Bag><rdf:li>a</rdf:li><rdf:li>b</rdf:li></rdf:Bag></dc:subject>"
		"<exif:Flash rdf:parseType='Resource'><exif:Fired>True</exif:Fired></exif:Flash>"));

	CHECK_STR(dump(m), "dc:title;dc:title[1]=T;dc:title[1]/?xml:lang=x-default;dc:subject;dc:subject[1]=a;"
		"dc:subject[2]=b;exif:Flash;exif:Flash/exif:Fired=True;");

	const auto props = m.properties();
	CHECK(!props.empty() && props.front().path.empty() && xmp::is_schema(props.front().options));
	CHECK_STR(props.front().schema, ns::dc);
}

TEST("xmp names nodes by the registered prefix")
{
	const auto m = parsed(
		"<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description xmlns:d='http://purl.org/dc/elements/1.1/'><d:format>image/jpeg</d:format>"
		"</rdf:Description></rdf:RDF>");
	CHECK_STR(dump(m), "dc:format=image/jpeg;");
}

TEST("xmp registers unknown namespaces and keeps prefixes unique")
{
	const auto m = parsed(
		"<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description xmlns:dc='http://example.com/not-dc/'><dc:thing>x</dc:thing></rdf:Description></rdf:RDF>");

	std::string prefix;
	CHECK(xmp::namespace_prefix("http://example.com/not-dc/", &prefix));
	CHECK_STR(prefix, "dc_1_:");
	CHECK_STR(get(m, "http://example.com/not-dc/", "thing"), "x");
	CHECK_STR(xmp::register_namespace("http://ns.google.com/photos/1.0/panorama/", "GPano"), "GPano:");
}

TEST("xmp reads qualified values written with rdf:value")
{
	const auto m = parsed(packet("<dc:format rdf:parseType='Resource'><rdf:value>image/png</rdf:value>"
		"<xmp:Label>q</xmp:Label></dc:format>"));
	CHECK_STR(get(m, ns::dc, "format"), "image/png");
	CHECK_STR(get(m, ns::dc, "format/?xmp:Label"), "q");
}

TEST("xmp reads structs written as nested descriptions and as attributes")
{
	const auto nested = parsed(packet("<exif:Flash><rdf:Description><exif:Fired>False</exif:Fired>"
		"<exif:Mode>2</exif:Mode></rdf:Description></exif:Flash>"));
	CHECK_STR(get(nested, ns::exif, "Flash/exif:Fired"), "False");
	CHECK_STR(get(nested, ns::exif, "Flash/exif:Mode"), "2");

	const auto attributes = parsed(packet("<exif:Flash exif:Fired='True' exif:Mode='1'/>"));
	CHECK_STR(get(attributes, ns::exif, "Flash/exif:Fired"), "True");

	const auto uri = parsed(packet("<xmp:BaseURL rdf:resource='http://example.com/'/>"));
	xmp::option_bits options = 0;
	CHECK(uri.get_property(ns::xmp, "BaseURL", nullptr, &options));
	CHECK((options & xmp::value_is_uri) != 0);
}

TEST("xmp repairs Latin-1 bytes and controls in 8-bit input")
{
	const auto m = parsed(packet("<xmp:Label>caf\xE9\x01x &#x1F;y \x80</xmp:Label>"));
	CHECK_STR(get(m, ns::xmp, "Label"), "caf\xC3\xA9 x  y \xE2\x82\xAC");

	// The toolkit blanks any two-digit hex reference; libfiles blanks only controls.
	const auto copyright = parsed(packet("<xmp:Label>&#xA9; Ann</xmp:Label>"));
	CHECK_STR(get(copyright, ns::xmp, "Label"), "\xC2\xA9 Ann");
}

TEST("xmp decodes UTF-16 input")
{
	const auto utf8 = packet("<xmp:Label>Gr\xC3\xBCn</xmp:Label>");
	std::string utf16 = "\xFF\xFE";

	for (std::size_t i = 0; i < utf8.size(); ++i)
	{
		auto c = static_cast<unsigned char>(utf8[i]);
		char32_t cp = c;
		if (c >= 0xC0 && c < 0xE0)
		{
			cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
			++i;
		}
		else if (c >= 0xE0)
		{
			cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
				(static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
			i += 2;
		}
		utf16 += static_cast<char>(cp & 0xFF);
		utf16 += static_cast<char>(cp >> 8);
	}

	const auto m = parsed(utf16);
	CHECK_STR(get(m, ns::xmp, "Label"), "Gr\xC3\xBCn");
}

TEST("xmp decodes references, CDATA and line ends")
{
	const auto m = parsed(packet("<xmp:Label>a &amp; b &lt;&#x41;&#66;<![CDATA[<c>]]>\r\nd</xmp:Label>"));
	CHECK_STR(get(m, ns::xmp, "Label"), "a & b <AB<c>\nd");
}

TEST("xmp skips a DOCTYPE")
{
	const auto m = parsed("<?xml version='1.0'?><!DOCTYPE x [<!ELEMENT x ANY>]>"
		"<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='1'/></rdf:RDF>");
	CHECK_STR(get(m, ns::xmp, "Rating"), "1");
}

TEST("xmp prefers the x:xmpmeta root among several")
{
	const auto m = parsed("<doc><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='1'/></rdf:RDF>"
		"<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='2'/></rdf:RDF></x:xmpmeta></doc>");
	CHECK_STR(get(m, ns::xmp, "Rating"), "2");
}

TEST("xmp applies the toolkit's touch-ups")
{
	const auto m = parsed(packet("<exif:UserComment>note</exif:UserComment>"
		"<exif:DateTimeOriginal>2019-03-04T10:00:00</exif:DateTimeOriginal>"
		"<exif:GPSTimeStamp>T12:30:00Z</exif:GPSTimeStamp>"
		"<xmpDM:copyright>(c) Ann</xmpDM:copyright>"));

	CHECK_STR(x_default(m, ns::exif, "UserComment"), "note");
	CHECK_STR(get(m, ns::exif, "GPSTimeStamp"), "2019-03-04T12:30:00Z");
	CHECK_STR(x_default(m, ns::dc, "rights"), "\n\n(c) Ann");
	CHECK(!m.exists(ns::dm, "copyright"));

	const auto uuid = parsed("<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		"<rdf:Description rdf:about='uuid:1234' xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='1'/></rdf:RDF>");
	CHECK_STR(get(uuid, ns::xmp_mm, "InstanceID"), "uuid:1234");
	CHECK_STR(uuid.about(), "");
}

TEST("xmp normalizes language tags")
{
	const auto m = parsed(packet("<dc:title><rdf:Alt><rdf:li xml:lang='EN-gb-OXENDICT'>Colour</rdf:li></rdf:Alt>"
		"</dc:title>"));
	CHECK_STR(get(m, ns::dc, "title[1]/?xml:lang"), "en-GB-oxendict");
}

TEST("xmp serializes in the toolkit's layout")
{
	xmp::meta m;
	m.set_property_int(ns::xmp, "Rating", 4);
	m.set_localized_text(ns::dc, "title", "", "x-default", "A & <B>");
	m.append_array_item(ns::dc, "subject", xmp::value_is_array, "tag");

	const auto sidecar = m.serialize({.omit_packet_wrapper = true});
	CHECK(sidecar.starts_with("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\""));
	CHECK(sidecar.find("<xmp:Rating>4</xmp:Rating>") != std::string::npos);
	CHECK(sidecar.find("<rdf:li xml:lang=\"x-default\">A &amp; &lt;B&gt;</rdf:li>") != std::string::npos);
	CHECK(sidecar.find("xpacket") == std::string::npos);

	const auto wrapped = m.serialize();
	CHECK(wrapped.starts_with("<?xpacket begin=\"\xEF\xBB\xBF\""));
	CHECK(wrapped.ends_with("<?xpacket end=\"w\"?>"));

	const auto compact = m.serialize({.use_compact_format = true});
	CHECK(compact.find("xmp:Rating=\"4\"") != std::string::npos);

	const auto exact = m.serialize({.exact_packet_length = true, .padding = 4096});
	CHECK_EQ(exact.size(), std::size_t{4096});

	for (const auto& text : {sidecar, wrapped, compact, exact})
	{
		const auto back = parsed(text);
		CHECK_STR(dump(back), dump(m));
	}
}

TEST("xmp round-trips structs, qualifiers and URIs")
{
	const auto original = parsed(packet("<exif:Flash rdf:parseType='Resource'><exif:Fired>True</exif:Fired>"
		"<exif:Mode>2</exif:Mode></exif:Flash><dc:format rdf:parseType='Resource'><rdf:value>image/png</rdf:value>"
		"<xmp:Label>q</xmp:Label></dc:format><xmp:BaseURL rdf:resource='http://example.com/a?b&amp;c'/>"
		"<dc:description><rdf:Alt><rdf:li xml:lang='x-default'>line one\nline two</rdf:li></rdf:Alt></dc:description>"));

	for (const auto compact : {false, true})
	{
		const auto back = parsed(original.serialize({.use_compact_format = compact}));
		CHECK_STR(dump(back), dump(original));
	}
}

TEST("xmp keeps a value's control characters escaped")
{
	xmp::meta m;
	m.set_property(ns::xmp, "Label", "a\tb\x01" "c");
	CHECK_STR(get(m, ns::xmp, "Label"), "a\tb c");
	CHECK(m.serialize().find("a&#x9;b c") != std::string::npos);
}

TEST("xmp refuses malformed paths and unregistered schemas")
{
	const xmp::meta m;
	CHECK_THROWS(m.exists("http://unregistered.example/", "x"));
	CHECK_THROWS(m.exists(ns::dc, "title[x]"));
	CHECK_THROWS(m.exists(ns::dc, ""));
}
