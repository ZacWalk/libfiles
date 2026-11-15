# libfiles

[![CI](https://github.com/ZacWalk/libfiles/actions/workflows/ci.yml/badge.svg)](https://github.com/ZacWalk/libfiles/actions/workflows/ci.yml)

A C++20 library for reading and updating media files: fast metadata scans, XMP,
EXIF and IPTC, still-image decoding and encoding, camera raw, and safe metadata
updates to images, audio and video. It uses only the standard library and the
codecs it fetches - no OS or UI dependency. The application supplies a
positional stream and receives pixels in its own memory.

libfiles is the file layer of [Diffractor](https://github.com/ZacWalk/diffractor)
and [ImageWalker](https://github.com/ZacWalk/imagewalker), so a fix to a format
lands in both; [fast-draw](https://github.com/ZacWalk/fast-draw) and
[zpilot](https://github.com/ZacWalk/zpilot) use its PNG encoder.

## What it covers

| Area | Formats | Header |
|---|---|---|
| Identifying a file, and what each operation can do with each format | Every format below | [identify.h](include/libfiles/identify.h) |
| Metadata scans: size, picture format, orientation, metadata blocks and thumbnails | JPEG, PNG, WebP, GIF, TIFF, PSD, BMP, HEIF and AVIF, JPEG XL | [scan.h](include/libfiles/scan.h) |
| Audio and video scans: streams, duration, tags, XMP and cover art, without decoding | MP4 and QuickTime, Matroska and WebM, AVI, WAV, AIFF, ASF, MP3, FLAC, Ogg, FLV, MPEG-TS, MPEG-PS | [media.h](include/libfiles/media.h) |
| Properties an organizer files by - title, people, dates, rating, place - reconciled across every block holding them | Every format libfiles scans | [properties.h](include/libfiles/properties.h) |
| The values every area shares: dates, ratings, places, picture formats, orientation, embedded images | - | [types.h](include/libfiles/types.h) |
| XMP data model: parse, query, edit, iterate, serialize, as Adobe's XMP Toolkit does | XMP packets and sidecars | [xmp.h](include/libfiles/xmp.h) |
| EXIF: read, write a fresh block, patch numbers in place, list every entry with maker notes | EXIF blocks from any container | [exif.h](include/libfiles/exif.h), [exif_listing.h](include/libfiles/exif_listing.h) |
| IPTC-IIM: read the fields an organizer files, and list every dataset | IPTC blocks | [iptc.h](include/libfiles/iptc.h) |
| ICC colour profiles: list the name, gamut, curve and white point, the header and every tag; map the colours one describes to sRGB, and an edit of that rendition back | Profiles from any container | [icc.h](include/libfiles/icc.h) |
| Metadata updates: XMP, with EXIF, IPTC and container tags reconciled, and a turn written where HEIF and JPEG XL keep one | JPEG, TIFF, PSD, PNG, GIF, WebP, JPEG XL, HEIF and AVIF, MP4 and QuickTime, WMV and WMA, WAV and AVI, MP3 and MP2, FLV | [write.h](include/libfiles/write.h) |
| Decoding into caller memory as BGRX, BGRA or NV12, reduced while decoding; sixteen bits, an HDR picture's signal and a HEIF's gain map for a caller that takes them | JPEG, PNG, WebP, PSD, HEIF and AVIF, JPEG XL | [decode.h](include/libfiles/decode.h) |
| HDR pictures and gain maps: a signal mapped to SDR, an SDR edit carried into it, a gain map made for it, ISO 21496-1 metadata, Apple's gain map | HDR HEIF, AVIF, JPEG XL and PNG; gain maps in JPEG, HEIF and AVIF | [hdr.h](include/libfiles/hdr.h) |
| Native documents: palettes, packed and grayscale storage, pages and frames | BMP, GIF, JPEG, PNG, TIFF, PCX, PSD, WPG | [document.h](include/libfiles/document.h) |
| Encoding with metadata, an HDR picture's description and an AVIF's gain map, native documents, lossless JPEG transforms, a JPEG repacked as a JPEG XL and given back, and how a WebP or JPEG XL was compressed | BMP, GIF, JPEG, PNG, TIFF, WebP, JPEG XL, AVIF | [encode.h](include/libfiles/encode.h) |
| Camera raw: largest preview or developed raw data, scan and listing, by path or through a stream | Every format LibRaw and the DNG SDK read | [raw.h](include/libfiles/raw.h) |
| Commodore 64 media: list and draw in the C64's characters | D64, D71, D81, T64, CRT, P00, PRG | [commodore.h](include/libfiles/commodore.h) |

## Building

The library builds with MSVC on Windows and GCC on Linux, for x64, x86 and
ARM64. The vendored [dd](https://github.com/ZacWalk/dd) builds and tests it for
x64:

```pwsh
.\dd.ps1 build            # Debug and Release
.\dd.ps1 test             # the libfiles suite in both
```

Each codec is optional and on by default. It is fetched from upstream at its pin
in [cmake/dd-dependencies.json](cmake/dd-dependencies.json) and built unmodified
by libfiles' own recipe in `cmake/`. Consumers link the codec targets too, so an
application has one copy of each. The CRT is the consumer's choice.
[docs/design.md](docs/design.md#codecs) says how each codec is configured.

| Option | Builds | Targets |
|---|---|---|
| `LIBFILES_JPEG` | libjpeg-turbo | `libfiles::jpeg` |
| `LIBFILES_PNG` | libpng, zlib-ng | `libfiles::png`, `libfiles::zlib` |
| `LIBFILES_WEBP` | libwebp | `libfiles::webp` |
| `LIBFILES_HEIF` | libheif, libde265, dav1d, libaom | `libfiles::heif`, `libfiles::de265`, `libfiles::dav1d`, `libfiles::aom` |
| `LIBFILES_JXL` | libjxl, Highway, Brotli, skcms | `libfiles::jxl`, `libfiles::highway`, `libfiles::brotli`, `libfiles::skcms` |
| `LIBFILES_RAW` | LibRaw, Adobe DNG SDK | `libfiles::raw`, `libfiles::dng` |
| `LIBFILES_GIF`, `LIBFILES_TIFF` | giflib, libtiff | `libfiles::gif`, `libfiles::tiff` |

## Using it

```cmake
include(FetchContent)
FetchContent_Declare(libfiles
    GIT_REPOSITORY https://github.com/ZacWalk/libfiles.git
    GIT_TAG        <commit>)
FetchContent_MakeAvailable(libfiles)
target_link_libraries(app PRIVATE libfiles::libfiles)
```

Setting `FETCHCONTENT_SOURCE_DIR_LIBFILES` to a local checkout builds it in
place of the pinned commit.

```cpp
#include "libfiles/xmp.h"

libfiles::xmp::meta meta;
meta.parse(packet);   // a malformed packet keeps what precedes its first fault

std::string title;
meta.get_localized_text(libfiles::xmp::ns::dc, "title", "", "x-default", nullptr, &title);
meta.set_property_int(libfiles::xmp::ns::xmp, "Rating", 4);
const auto sidecar = meta.serialize({.omit_packet_wrapper = true});
```

Scanning reads through a `libfiles::stream` the application implements over
its own file handle, or straight from memory:

```cpp
#include "libfiles/scan.h"

libfiles::reader r(my_stream);                 // or libfiles::reader r(bytes);
const auto info = libfiles::scan(r);           // never throws for malformed input
// info.width, info.height, info.picture (chroma, bit depth, alpha; name() such as "yuv420 10bit"),
// info.orientation, info.container_transform, info.animated, info.hdr, info.gain_map, info.colour_primaries,
// info.colour_transfer, info.metadata.exif / xmp / iptc / icc / photoshop, and libfiles::thumbnails(info)
```

The properties an organizer files by read the same way from a photo or a
recording - a photo's EXIF, its IPTC and then its XMP packet, each over the last,
or a recording's tags and then its packet - with what each block states:

```cpp
#include "libfiles/properties.h"

const auto p = libfiles::read_properties(r);   // empty for a file libfiles does not read
// p.title, p.artist, p.keywords, p.rating, p.created, p.dates, p.location, p.camera_model, ...
```

An EXIF block - `info.metadata.exif`, or one found elsewhere - reads into what an
application shows of a photograph:

```cpp
#include "libfiles/exif.h"

const auto exif = libfiles::read_exif(info.metadata.exif);
// exif.camera_make, exif.lens, exif.f_number, exif.exposure_time, exif.original.text,
// exif.latitude, exif.rating, exif.orientation, ...
```

Decoding writes into memory the application provides:

```cpp
#include "libfiles/decode.h"

struct my_sink final : libfiles::image_sink
{
    // Rows for frame.width by frame.height pixels in frame.format - a texture's staging memory, a
    // bitmap - or {} to refuse. frame also tells the source's size, the reduction, the memory the
    // decoder holds besides, and the EXIF orientation.
    libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override;
    bool cancelled() override;   // optional, polled between rows
};

libfiles::decoder decoder;                        // one per thread, reused: keeps libjpeg's state
my_sink sink;
const auto decoded = decoder.decode(r, sink, {.target_width = 256, .target_height = 256, .allow_nv12 = true});
// decoded.success, or decoded.refused, decoded.cancelled, decoded.truncated and decoded.error
```

## Documentation

- [docs/design.md](docs/design.md) - how libfiles is layered, how each format is
  read, written and decoded, and where it differs on purpose from the XMP
  Toolkit, FFmpeg, libheif and libjxl.
- [AGENTS.md](AGENTS.md) - rules for contributors and coding agents.

## Licence

MPL-2.0; see [LICENSE](LICENSE). A program that links libfiles statically may be
under any licence. Changes to libfiles' own files stay under the MPL-2.0, and
anyone distributing a program that includes libfiles must tell its recipients
where to get libfiles' source code. Each fetched codec keeps its own licence and
notices, and the vendored dd build driver (`dd.ps1`, `.dd/`) is MIT.

These files keep the LGPL of the code they were ported from, as
LGPL-2.1-or-later, whose text is in [licences/LGPL-2.1.txt](licences/LGPL-2.1.txt):

- `src/libexif_*`, the loading rules in `src/exif_listing.cpp`, and
  `tools/port_libexif.py`, which writes the `libexif_*` files, are derived from
  [libexif](https://github.com/libexif/libexif), Copyright (c) 2001-2022 Lutz
  Mueller and the libexif contributors, under LGPL-2.0-or-later.
- `src/iptc.cpp` holds [libiptcdata](https://github.com/ianw/libiptcdata)'s IPTC
  dataset table, Copyright (c) 2005 David Moore, under LGPL-2.0-or-later.

The linker brings them into a program that lists EXIF or reads IPTC. Anyone
distributing such a program must also let its users relink it with modified
versions of those files (LGPL-2.1 section 6).
