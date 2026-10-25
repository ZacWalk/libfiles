# libfiles

[![CI](https://github.com/ZacWalk/libfiles/actions/workflows/ci.yml/badge.svg)](https://github.com/ZacWalk/libfiles/actions/workflows/ci.yml)

A C++20 library for reading and updating media files: fast metadata scans, XMP,
EXIF and IPTC, still-image decoding and encoding, camera raw, and safe metadata
updates to images, audio and video. It uses only the standard library and the
codecs it fetches - no OS or UI dependency. The application supplies a
positional stream and receives pixels in its own memory.

libfiles is the shared file layer of [Diffractor](https://github.com/ZacWalk/diffractor)
and [ImageWalker](https://github.com/ZacWalk/imagewalker), so a fix to a format
lands in both.

- **Diffractor** scans and decodes its photos through it, and writes metadata
  to photos, video and audio with it: the scans, XMP, EXIF and IPTC, the
  decoders and encoders, camera raw and the Commodore 64 listings.
- **ImageWalker**'s `libiw` takes its native readers (JPEG, PNG, GIF, TIFF,
  PCX, PSD, WPG and BMP), WebP display decoding, metadata and codecs from it,
  and keeps image storage, encoding and its Windows imaging adapters.

Reading is the hot path - a scan reads only the structure it needs, a few block
reads over a network share - and writing is safe: an edit is either patched in
place, ordered to survive interruption, or written in one pass to a destination
the caller stages.

## What it covers

| Area | Formats | Header |
|---|---|---|
| Format detection and metadata scans | JPEG, PNG, WebP, GIF, TIFF, PSD, BMP, HEIF and AVIF, JPEG XL | [scan.h](include/libfiles/scan.h) |
| XMP data model: parse, query, edit, iterate, serialize, as Adobe's XMP Toolkit does | XMP packets and sidecars | [xmp.h](include/libfiles/xmp.h) |
| EXIF: read, write a fresh block, patch numbers in place, list every entry with maker notes | EXIF blocks from any container | [exif.h](include/libfiles/exif.h), [exif_listing.h](include/libfiles/exif_listing.h) |
| IPTC-IIM: read the fields an organizer files, and list every dataset | IPTC blocks | [iptc.h](include/libfiles/iptc.h) |
| Metadata updates: XMP, with EXIF, IPTC and container tags reconciled | JPEG, TIFF, PSD, PNG, GIF, WebP, MP4 and QuickTime, WMV and WMA, WAV and AVI, MP3 and MP2, FLV | [write.h](include/libfiles/write.h) |
| Decoding into caller memory as BGRX, BGRA or NV12, reduced while decoding | JPEG, PNG, WebP, PSD, HEIF and AVIF, JPEG XL | [decode.h](include/libfiles/decode.h) |
| Native documents: palettes, packed and grayscale storage, pages and frames | BMP, GIF, JPEG, PNG, TIFF, PCX, PSD, WPG | [document.h](include/libfiles/document.h) |
| Encoding with metadata, and lossless JPEG rotation | JPEG, PNG, WebP | [encode.h](include/libfiles/encode.h) |
| Camera raw: largest preview or developed raw data, scan and listing | Every format LibRaw and the DNG SDK read | [raw.h](include/libfiles/raw.h) |
| Commodore 64 media: list and draw in the C64's characters | D64, D71, D81, T64, CRT, P00, PRG | [commodore.h](include/libfiles/commodore.h) |

## Building

Windows with MSVC and Linux with GCC, on x64, x86 and ARM64. The CRT is the
consumer's choice. Build and test with the vendored
[dd](https://github.com/ZacWalk/dd):

```pwsh
.\dd.ps1 build            # Debug and Release
.\dd.ps1 test             # the libfiles suite in both
```

Each codec is optional, on by default, fetched from its upstream release
archive pinned by SHA-256 in [cmake/dd-dependencies.json](cmake/dd-dependencies.json),
and built unmodified by libfiles' own recipe in `cmake/`. Consumers link the
codec targets too, so an application has one copy of each:

| Option | Builds | Targets |
|---|---|---|
| `LIBFILES_JPEG` | libjpeg-turbo 3.2.0 | `libfiles::jpeg` |
| `LIBFILES_PNG` | libpng 1.6.59, zlib-ng 2.3.3 | `libfiles::png`, `libfiles::zlib` |
| `LIBFILES_WEBP` | libwebp 1.6.0 | `libfiles::webp` |
| `LIBFILES_HEIF` | libheif 1.23.5, libde265 1.1.3, dav1d 1.5.4 | `libfiles::heif`, `libfiles::de265`, `libfiles::dav1d` |
| `LIBFILES_JXL` | libjxl 0.12.0, Highway 1.4.0, Brotli 1.2.0, skcms | `libfiles::jxl`, `libfiles::highway`, `libfiles::brotli`, `libfiles::skcms` |
| `LIBFILES_RAW` | LibRaw 0.22.2, Adobe DNG SDK 1.7.1 | `libfiles::raw`, `libfiles::dng` |
| `LIBFILES_GIF`, `LIBFILES_TIFF` | giflib 5.2.2, libtiff 4.7.0 | `libfiles::gif`, `libfiles::tiff` |

NASM is found on the path, or fetched by SHA-256 on Windows; without it, and on
ARM64, the codecs build their C paths. libheif's FFmpeg decoder is not built:
an application with FFmpeg builds `plugins/decoder_ffmpeg.cc` from the fetched
libheif and registers it, as Diffractor does.
[docs/design.md](docs/design.md#codecs) has each codec's configuration.

## Using it

```cmake
include(FetchContent)
FetchContent_Declare(libfiles
    GIT_REPOSITORY https://github.com/ZacWalk/libfiles.git
    GIT_TAG        <commit>)
FetchContent_MakeAvailable(libfiles)
target_link_libraries(app PRIVATE libfiles::libfiles)
```

Diffractor and ImageWalker declare libfiles this way at a pinned commit, and
build a sibling `../libfiles` checkout instead when one exists, whatever its
HEAD. CI and a fresh clone use the pin.

```cpp
#include "libfiles/xmp.h"

libfiles::xmp::meta meta;
meta.parse(packet);   // never throws for malformed input

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
// info.width, info.height, info.pixel_format, info.orientation, info.container_transform,
// info.metadata.exif / xmp / iptc / icc / photoshop, and the thumbnails' locations
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

- [docs/design.md](docs/design.md) - how libfiles reads, writes and decodes,
  and where it deliberately differs from the XMP Toolkit and the readers it
  replaced.
- [docs/todo.md](docs/todo.md) - open work.
- [AGENTS.md](AGENTS.md) - rules for contributors and coding agents.

## Licence

LGPL-2.1-or-later; see [LICENSE](LICENSE). A program that links libfiles
statically must let its users relink it against a modified libfiles.

The native ImageWalker readers and their sample conversions retain Zac Walker's
1998-2026 copyright and MIT permission notice in
[LICENSE.imagewalker](LICENSE.imagewalker). The combined library remains
LGPL-2.1-or-later. Each fetched codec retains its own licence and notices.

`src/libexif_*` and the loading rules in `src/exif_listing.cpp` are derived from
[libexif](https://github.com/libexif/libexif), Copyright (c) 2001-2022 Lutz
Mueller and the libexif contributors, under LGPL-2.0-or-later; their notices
stay with the code. The IPTC dataset table in `src/iptc.cpp` is
[libiptcdata](https://github.com/ianw/libiptcdata)'s, Copyright (c) 2005 David
Moore, under LGPL-2.0-or-later.
