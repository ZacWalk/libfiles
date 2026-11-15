# libfiles — agent instructions

[README.md](README.md) says what libfiles covers and how to build and use it.
[docs/design.md](docs/design.md) says how each format is read, written and
decoded; read its section for the format you are changing. The headers in
`include/libfiles/` own the exact contracts, and every source file opens with a
`// Purpose:` comment that owns its own rules.

## Non-negotiables

- **Standard C++20 only.** No OS header, no application type, no third-party
  library in the public API. A file is reached through an interface the caller
  supplies (`stream.h`), so the library builds on Windows and Linux, for x64,
  x86 and ARM64.
- **Codecs are fetched, not vendored, and built by libfiles' own recipes**, as
  [README.md](README.md#building) describes. Never copy a codec's source into
  the repository or patch a fetched tree: carry any configuration it needs
  beside its recipe in `cmake/`. Keep each codec optional, and the core building
  without any. A tool a recipe needs is found on the path or fetched by
  SHA-256, as NASM is on Windows.
- **Never set the CRT.** `CMAKE_MSVC_RUNTIME_LIBRARY` is the consumer's choice.
- **Parity in reading; writing may improve.** Applications already read their
  files through libfiles, so what a file *reads as* must not change. Reading
  matches its references - the XMP Toolkit, FFmpeg, libheif and libjxl, and the
  readings Diffractor's and ImageWalker's tests record - unless a difference is
  deliberate, and every deliberate difference is listed in
  [docs/design.md](docs/design.md#where-libfiles-differs-on-purpose). How files
  are *written* is designed for Diffractor, not copied from the toolkit.
- **One parser per format, one owner per concept.** A format's structure is read
  by one parser, which its scan, decoder, writer and listing share. A value
  every area shares has one type and one conversion rule, a tag's number or name
  is given once, and a property is read by one table, which the writers follow.
  [The layers](docs/design.md#the-layers) name each owner: extend it rather than
  add a second. The ASF scanner keeps its own copy of `media_tags.cpp`'s words
  for binary values and picture types; change both together.
- **Reading is the hot path.** Diffractor spends most of its time indexing. A
  scanner reads only the structure it needs, never image data it does not, and
  locates rather than reads what the caller may not want. Measure a change by
  the reads it issues (`reader::source_reads`), not only by its time.
- **Malformed input is normal input.** Parsing never reads out of bounds, keeps
  what it can, and never throws for bad bytes - but for an XMP packet the
  toolkit rejects whole, which throws as the toolkit does (`xmp.h`). A scan
  reports a fault in its result; exceptions are for misuse of the API and for a
  stream or codec that fails.
- **A writer never leaves a partial file.** Either the change is bounded and
  patched in place, or the caller's staging protects the original. A patch that
  takes more than one write is ordered in steps so that an interruption anywhere
  leaves a file that reads whole, each field old or new. Every writer's patch is
  tested over every state an interruption could leave (`patch_states` in
  `tests/interruptions.h`), and a mutation that merges two steps must fail it.
- **Every writable format reads back what it wrote.** A writer's new field joins
  `tests/round_trip.h`.

## Layout

| Path | What it holds |
|---|---|
| `include/libfiles/` | The public API, one header per area |
| `src/identify.cpp` | `identify()` and `file_types()`, the one table of formats and what each operation does with each |
| `src/types.cpp`, `src/properties.cpp` | The values every area shares; the properties table, the reconciling rule and `read_properties` |
| `src/dates.cpp`, `src/gps.cpp`, `src/text.cpp`, `src/photo_values.cpp` | The conversions readers and writers share - dates, XMP's GPS text, text encodings and the values in a photo's blocks - each with its `_internal.h` |
| `src/vocabulary_internal.h` | The names ID3, RIFF INFO and ASF give the fields libfiles reads and writes |
| `src/bmff_internal.h`, `src/heif_internal.h`, `src/mp4_internal.h`, `src/riff_internal.h`, `src/asf_internal.h`, `src/flv_internal.h`, `src/id3.cpp`, `src/tiff_internal.h`, `src/photoshop_internal.h` | The one parser of each structure, which the scans and the writers share |
| `src/reader.cpp` | The memory stream and the windowed reader |
| `src/scan.cpp`, `src/scan_<format>.cpp` | `scan()`, `thumbnails()` and one scanner per still format |
| `src/media.cpp`, `src/media_internal.h`, `src/scan_media_<container>.cpp` | `scan_media` and one scanner per container |
| `src/media_video.cpp`, `src/media_audio.cpp`, `src/media_tags.cpp` | The codec headers and the tags the containers share |
| `src/exif.cpp`, `src/exif_write.cpp`, `src/exif_listing.cpp` | Reading, writing and listing EXIF |
| `src/libexif_*`, `tools/port_libexif.py` | libexif's tag names, value text and maker note decoders, ported by the tool and kept close to libexif. Change the tool, not the files it writes; libexif's notices stay |
| `src/iptc.cpp`, `src/icc.cpp` | Reading and listing IPTC-IIM and ICC profiles |
| `src/decode.cpp`, `src/decode_<format>.cpp`, `src/hdr.cpp` | The decode framework, one decoder per format, and an HDR picture's mapping to SDR, its signal's light, and an SDR edit carried into it |
| `src/gain_map.cpp` | A gain map's ISO 21496-1 metadata read and written, a gain map made for an HDR picture, and Apple's said in ISO 21496-1's terms |
| `src/document*.cpp` | Native documents: dispatch, BGRA, and the BMP, GIF, TIFF, PCX and WPG readers |
| `src/encode_*.cpp`, `src/jpeg_carry.cpp`, `src/jpeg_gain_map.cpp` | The encoders with the metadata each carries, native document encoding, lossless JPEG transforms, a JPEG's segments carried into its re-encoding, and its gain map found, described, marked and attached |
| `src/raw.cpp`, `src/raw_listing.cpp`, `src/dng_xmp_sdk.cpp` | Camera raw over LibRaw, and the DNG SDK's XMP backend over libfiles' XMP |
| `src/commodore.cpp` | Listing and drawing Commodore 64 media |
| `src/write.cpp`, `src/write_<format>.cpp` | The update framework and one planner per format |
| `src/legacy.h`, `src/legacy_photo.cpp`, `src/tiff_editor.cpp` | A photo's EXIF, IPTC and Photoshop resources edited, and TIFF directories edited by appending |
| `src/xmp_*.cpp` | The XMP data model: XML, namespaces and paths, RDF, the `meta` API and serialization |
| `src/md5.cpp`, `src/zlib.cpp` | MD5 for the IPTC digest; inflate and CRC-32 for PNG |
| `cmake/*.cmake`, the folders beside them | One recipe per codec, with the headers its own build would generate |
| `cmake/dd-dependencies.json` | The pins |
| `tests/` | `TEST("name")` blocks with `CHECK` macros. `builders.h` and `media_builders.h` build the files the tests read; `round_trip.h` and `interruptions.h` hold every writer to reading back and to surviving interruption; `agreement_tests.cpp` holds the operations to agreeing about a file |

## Verifying a change

Build and test as [README.md](README.md#building) shows. Run one test by name
with `build\x64-windows\debug\libfiles_tests.exe "<part of the name>"`, and read
the test count, not just the exit code. A change is done when both
configurations pass on Windows, with no warning at MSVC's `/W4`, and GCC builds
both without warnings - on Linux or in WSL, `.\dd.ps1 test` builds with GCC -
since some of GCC's warnings, `-Wstringop-overflow` among them, appear only when
it optimizes. On GCC 15 and later the test target alone turns off
`-Warray-bounds`, `-Wstringop-overflow` and `-Wrestrict`, which report false
positives in the libstdc++ code the tests inline; the library keeps them. Lint
a change with clang-tidy and `.clang-tidy`, its bug-finding and performance
checks, and fix what it finds in libfiles' own code.

| After changing | Also |
|---|---|
| A codec recipe or a size computation | Build for x86, where `size_t` is 32 bits, and cross-compile for ARM64, which builds the C paths |
| The HEIF or JPEG XL scanner | Compare it with the pinned libheif and libjxl over real files, whole, cut short and corrupted: it reproduces what they report, down to the damaged files they refuse, and the suite's files are built to agree with them |
| A container scanner or a codec header reader | Compare it with FFmpeg over real files, and run Diffractor's *Should read media with libfiles*, which compares the two over Diffractor's fixtures |
| EXIF reading | Run Diffractor's *Should read EXIF as Diffractor's parser did*: a record of its fixtures' blocks read whole, cut short and corrupted byte by byte, far more than this suite holds. A difference it catches is a fault, or deliberate and listed in docs/design.md and the test's comment |
| Native document decoding | Run ImageWalker's `.\dd.ps1 test --label imagewalker-imaging`, which holds the native readers to its record and checks native storage, metadata, fallbacks, cancellation and allocation limits. Do not replace it with BGRA-only checks, and keep the borrowed-input path free of unnecessary copies |

Diffractor, ImageWalker, fast-draw and zpilot build a sibling `../libfiles`
checkout in place of their pin when there is one, so their tests exercise your
working tree.

## Style

Tabs, snake_case, a `// Purpose:` comment at the top of every file, and comments
only where the code needs explaining — usually why the toolkit does something.
A new file carries `SPDX-License-Identifier: MPL-2.0`. Only code ported from
libexif or libiptcdata keeps `LGPL-2.1-or-later`; never copy that header into a
file of libfiles' own (see the README's [Licence](README.md#licence)).

The layout is `.clang-format`'s - tabs, Allman braces, 120 columns - applied by
`.\dd.ps1 fmt`, which formats `include`, `src` and `tests` and needs clang-format
on the PATH (Visual Studio installs one in `VC\Tools\Llvm\x64\bin`; dd looks only
on the PATH). `.clang-format-ignore` keeps it off the vendored dd, the codec
headers in `cmake/` and the code ported from libexif and libiptcdata, and a
table laid out by hand is fenced with `// clang-format off` and `on`.
