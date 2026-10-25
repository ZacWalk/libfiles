# libfiles — agent instructions

libfiles loads and updates media files for ImageWalker and Diffractor. Read
[README.md](README.md) first, then the section of [docs/design.md](docs/design.md)
for the format you are changing. Open work is in [docs/todo.md](docs/todo.md).

## Non-negotiables

- **Standard C++20 only.** No OS header, no application type, no third-party
  library in the public API. A file is reached through an interface the caller
  supplies, so the library builds for Diffractor's headless Linux target as well
  as Windows, on x64, x86 and ARM64.
- **Codecs are fetched, not vendored, and built by libfiles' own recipes.** Each
  is pinned by SHA-256 in `cmake/dd-dependencies.json` and built from the
  untouched upstream archive by a recipe in `cmake/`, with any configuration it
  needs carried beside the recipe. Codec recipes live here, so an application
  links libfiles' targets rather than building a second copy. Each is optional
  (`LIBFILES_JPEG`), and the core stays dependency-free without it. A tool a
  recipe needs is found on the path or fetched by SHA-256, as NASM is on Windows.
- **Never set the CRT.** `CMAKE_MSVC_RUNTIME_LIBRARY` is the consumer's choice.
- **Parity in reading; writing may improve.** libfiles replaces Adobe's XMP
  Toolkit and the readers Diffractor and ImageWalker had, under applications
  that already read files with them, so what a file *reads as* must not change:
  match them unless the difference is deliberate, and record every deliberate
  difference in [docs/design.md](docs/design.md)'s *Where libfiles differs on
  purpose*. How files are *written* is designed for Diffractor rather than
  copied from the toolkit.
- **One parser per format.** A format's structure is read by one parser, which
  its scan, decoder, writer and listing share - as every TIFF directory goes
  through `src/tiff_internal.h` and every Photoshop resource through
  `src/photoshop_internal.h`. Do not add a second.
- **Reading is the hot path.** Diffractor spends most of its time indexing. A
  scanner reads only the structure it needs, never image data it does not, and
  locates rather than reads what the caller may not want. Measure a change by
  the reads it issues (`reader::source_reads`), not only by its time.
- **Malformed input is normal input.** Parsing never throws for bad bytes, never
  reads out of bounds, and keeps what it can. `scan()` reports a fault in
  `scan_result::error`; only misuse of the API throws (`libfiles::xmp::error`).
- **A writer never leaves a partial file.** Either the change is bounded and
  patched in place, or the caller's staging protects the original. A patch that
  takes more than one write is ordered in steps so that an interruption anywhere
  leaves a file that reads whole, each field old or new; every writer's patch is
  tested over every state an interruption could leave (`patch_states` in
  `tests/interruptions.h`), and a mutation that merges two steps must fail it.

## Layout

| Path | What it holds |
|---|---|
| `include/libfiles/` | The public API, one header per area |
| `src/reader.cpp` | The memory stream and the windowed reader |
| `src/scan.cpp` | Format detection, `scan()`, the structure-row helpers |
| `src/scan_<format>.cpp` | One scanner per format; `scan_heif.cpp` (HEIC, AVIF and the other ISO base media stills) and `scan_jxl.cpp` (JPEG XL) also find their packets for `read_packet` |
| `src/exif.cpp`, `src/tiff_internal.h` | The one reader of TIFF directories: `read_exif`, `summarize_exif` and the orientation rule, and the TIFF scanner's camera, exposure, dates, orientation, thumbnail and GPS. The listing and the TIFF editor read through it too |
| `src/exif_write.cpp` | Writing EXIF blocks: `make_exif`, laid out as libexif lays out a new block, and `set_exif_numbers`, in place |
| `src/exif_listing.cpp` | `list_exif`: a block read with the TIFF reader by libexif's loading rules, then named, rendered and its maker note decoded |
| `src/libexif_*`, `tools/port_libexif.py` | libexif's tag names, value text and maker note decoders, ported by the tool from libexif's source and kept close to it. Change the tool, not the files it writes; libexif's notices stay |
| `src/decode.cpp`, `src/decode_internal.h` | The decode framework: dispatch, the header cursor, and the failures every decoder reports alike |
| `src/decode_<format>.cpp` | One decoder per format, writing into the caller's memory: PSD always; JPEG, PNG, WebP, HEIF and JPEG XL when their codec's option is on |
| `src/encode_<format>.cpp`, `src/encode_internal.h` | The JPEG, PNG and WebP encoders, with the metadata each carries, and the lossless JPEG turn |
| `src/raw.cpp`, `src/raw_listing.cpp`, `src/dng_xmp_sdk.cpp` | Camera raw over LibRaw: loading a preview or the developed raw data, the scan and the listing; and the DNG SDK's XMP backend over libfiles' XMP |
| `src/commodore.cpp` | Listing and drawing Commodore 64 disks, tapes, cartridges and programs |
| `src/iptc.cpp` | Reading and listing IPTC-IIM, with libiptcdata's dataset table |
| `include/libfiles/document.h`, `src/document*.cpp` | Native row streams, packed storage and multiple pages. JPEG/PNG/PSD share their parser/codec with the display path; BMP, GIF, TIFF, PCX and WPG have their native readers here |
| `src/write.cpp` | The update framework: plans, in-place patches, one-pass rewrites, packet sizing |
| `src/write_<format>.cpp` | One planner per format; `write_tiff_psd.cpp` holds two, `write_mp4.cpp` serves ISO base media and QuickTime, `write_asf.cpp` WMV and WMA, `write_riff.cpp` WAV and AVI, `write_mp3.cpp` MPEG audio through ID3v2 |
| `src/legacy.h`, `src/legacy_photo.cpp` | IPTC and Photoshop resources, and the mapping of an edit onto EXIF and IPTC |
| `src/photoshop_internal.h` | The one reader of a Photoshop image resource, for the PSD and JPEG scans, the writers and the JPEG encoder |
| `src/tiff_editor.cpp` | Editing TIFF directories by appending, without moving anything |
| `src/md5.cpp` | MD5, for the IPTC digest |
| `src/zlib.cpp` | Inflate and CRC-32, for PNG |
| `src/xmp_xml.cpp` | Input decoding and the XML parser |
| `src/xmp_registry.cpp` | Namespaces, aliases, paths, node primitives |
| `src/xmp_rdf.cpp` | RDF to the XMP tree, and the toolkit's normalization |
| `src/xmp_meta.cpp` | The `meta` API and iteration |
| `src/xmp_serialize.cpp` | RDF/XML output |
| `cmake/<codec>.cmake`, `cmake/<codec>/` | One recipe per codec - jpeg, png and zlib, webp, heif with de265 and dav1d, jxl with highway, brotli and skcms, raw with dng, gif, tiff - with the configuration headers its own build would generate, for MSVC and for GCC and Clang. TIFF finds the one shared zlib |
| `cmake/dd-dependencies.json` | The pins: each codec's release archive and the NASM build Windows fetches, by SHA-256 |
| `tests/` | The suite: `TEST("name")` blocks, `CHECK` macros; `builders.h` builds the files they read, box by box and, for JPEG XL headers, bit by bit |
| `tests/heif_jxl_scan_tests.cpp` | HEIF and JPEG XL files built box by box and header field by field, each also cut short at every byte near its ends |

## Building and testing

```pwsh
.\dd.ps1 test --label libfiles      # Debug and Release, Windows
```

On Linux, or in WSL, `.\dd.ps1 test` builds with GCC. Run one test by name with
`build\x64-windows\debug\libfiles_tests.exe "<part of the name>"`. A change is
done when both configurations pass on Windows and GCC builds both without
warnings: some of GCC's warnings, `-Wstringop-overflow` among them, appear only
when it optimizes. On GCC 15 and later the test target alone turns off
`-Warray-bounds`, `-Wstringop-overflow` and `-Wrestrict`, which report false
positives in the libstdc++ code the tests inline ([docs/todo.md](docs/todo.md));
the library keeps them. Read the test count, not just the exit code. A change that
touches a codec recipe or a size computation is also built for x86, where
`size_t` is 32 bits, and cross-compiled for ARM64, which builds the C paths.

The HEIF and JPEG XL scanners reproduce what libheif 1.23.5 and libjxl report as
Diffractor builds them, down to the damaged files they refuse. The suite's files
are built to agree with them; check a change to either scanner against those
libraries over real files too, whole, cut short and corrupted.

After changing native document decoding, also run ImageWalker's `libiw` tests:
they retain the old readers' golden records and verify native storage, metadata,
SDK fallbacks, cancellation and allocation limits. Do not replace these with
BGRA-only checks. Keep the borrowed-input path free of unnecessary copies.

## Style

Tabs, snake_case, a `// Purpose:` comment at the top of every file, and comments
only where the code needs explaining — usually why the toolkit does something.
