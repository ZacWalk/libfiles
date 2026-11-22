# libfiles design

How libfiles reads, writes and decodes files, and where it differs on purpose
from the references it is held to. [README.md](../README.md) has the overview
and the API at a glance. The headers in `include/libfiles/` own the exact
contracts, and each source file's `// Purpose:` comment owns its own rules;
this document holds what spans them.

## The layers

Each concept has one owner, in one of six layers, so that a photo, a raw file
and a recording describe the same thing the same way, and a reader and a
writer of the same field agree:

- **Identification.** `identify()` names a file from its first bytes, once, for
  every operation: `scan_media`, `read_packet` and `plan_update` dispatch on it.
  It looks past an ID3v2 tag, which says nothing of what follows it.
  `file_types()` is the one table of what each operation does with each format.
- **Structure.** One reader decodes each family of structures, for the scanners
  and the writers alike: ISO base media box headers (`bmff_internal.h`) for MP4,
  HEIF and the JPEG XL container; RIFF chunks (`riff_internal.h`) for WAV, AVI
  and WebP; ID3v2 tags (`id3.cpp`) for MPEG audio and the ID3 a WAV, AIFF or
  FLAC carries; TIFF directories (`tiff_internal.h`) for EXIF and TIFF;
  Photoshop image resources (`photoshop_internal.h`) for PSD and JPEG; and a
  HEIF's items, their locations and references, which the HEIF scanner reads
  for its writer too (`heif_internal.h`). What a
  damaged structure means stays each reader's rule, since each follows its own
  reference - FFmpeg, libheif, libjxl, libwebp, the XMP Toolkit. Where two
  references read one record differently, as libheif and FFmpeg read `av1C`,
  their readers share a test rather than code: a still and a video read a
  well-formed record as the same picture.
- **Values.** `types.h` holds what every area shares - dates, ratings, places,
  picture formats, orientation, embedded images and the scans' own types - and
  each conversion has one rule, which readers and writers both use: Windows'
  rating percent; a container tag's date (`parse_media_date`), a photo block's
  (`parse_photo_date`, with the zones, fractions, rationals and coordinates of
  `photo_internal.h`) and an edit's (`parse_xmp_date`); XMP's GPS text
  (`gps.cpp`); and text in every encoding a tag is written in (`text.cpp`).
  An HDR picture's description and a gain map's metadata are `decode.h`'s, which
  the decoders read and the encoders write; their arithmetic is `hdr.cpp`'s and
  `gain_map.cpp`'s.
  Tags are named once: TIFF's and EXIF's numbers in `tiff_internal.h`, ID3's,
  RIFF INFO's and ASF's names in `vocabulary_internal.h`.
- **Scans.** `scan` reads a still, `scan_media` a recording and `scan_raw` a
  camera raw file, each in its format's own terms, with the shared values
  beside them: `picture` beside each scanner's own `pixel_format` name, a
  video's `orientation()` beside its rotation and mirror, and `thumbnails()`
  beside the thumbnails' byte ranges.
- **Properties.** One table reads what an organizer files a file by from each
  block, marking what the block states, and one rule, `overlay`, reconciles the
  blocks ([Properties](#properties)).
- **Writers.** `plan_update` writes the packet, and each legacy field where the
  format keeps it, by the tables the readers read; every writable format is held
  to reading back what it wrote
  ([below](#what-each-writable-format-keeps-outside-its-packet)).

### What each format is read and written as

What `file_types()` reports in the default build; turning a codec's option off
removes what that codec does:

| Format | MIME | scan | scan_media | decode | decode_document | read_packet | update | encode |
|---|---|---|---|---|---|---|---|---|
| JPEG | `image/jpeg` | yes |  | yes | yes | yes | yes | yes |
| PNG | `image/png` | yes |  | yes | yes | yes | yes | yes |
| WebP | `image/webp` | yes |  | yes |  | yes | yes | yes |
| GIF | `image/gif` | yes |  |  | yes | yes | yes | yes |
| Photoshop | `image/vnd.adobe.photoshop` | yes |  | yes | yes | yes | yes |  |
| BMP | `image/bmp` | yes |  |  | yes |  |  | yes |
| TIFF | `image/tiff` | yes |  |  | yes | yes | yes | yes |
| HEIF and AVIF | `image/heif` | yes |  | yes |  | yes | yes |  |
| JPEG XL | `image/jxl` | yes |  | yes |  | yes | yes |  |
| PCX | `image/vnd.zbrush.pcx` |  |  |  | yes |  |  |  |
| WordPerfect Graphics | `image/x-wpg` |  |  |  | yes |  |  |  |
| MP4 and QuickTime | `video/mp4` |  | yes |  |  | yes | yes |  |
| Matroska and WebM | `video/x-matroska` |  | yes |  |  |  |  |  |
| AVI | `video/x-msvideo` |  | yes |  |  | yes | yes |  |
| WAV | `audio/wav` |  | yes |  |  | yes | yes |  |
| AIFF | `audio/aiff` |  | yes |  |  | yes |  |  |
| ASF | `video/x-ms-asf` |  | yes |  |  | yes | yes |  |
| MPEG audio | `audio/mpeg` |  | yes |  |  | yes | yes |  |
| FLAC | `audio/flac` |  | yes |  |  | yes |  |  |
| Ogg | `audio/ogg` |  | yes |  |  |  |  |  |
| FLV | `video/x-flv` |  | yes |  |  | yes | yes |  |
| MPEG transport stream | `video/mp2t` |  | yes |  |  |  |  |  |
| MPEG program stream | `video/mpeg` |  | yes |  |  |  |  |  |

Camera raw files have no signature to identify them by: a caller knows one by
its name and hands it to `scan_raw` and `load_raw`, by its path or through a
reader.

### Properties

A still's blocks are read EXIF, then IPTC, then the packet, each over what
those before it gave (`overlay`). What a block states replaces theirs - EXIF and
IPTC state a property by holding text, a number or a date for it, the packet by
holding the property at all, so a property present but empty clears one - but
IPTC's keywords join the keywords before them, sorted and each once, and its
by-lines join the artist's names, whole and each once, after them; and every
date is kept with its tag. A recording's container tags come first, filling the
properties FFmpeg's demuxers file them under, and its packet over them. A
packet's property in another form than its schema gives - a simple value where
the schema has an array or alt-text - reads as none, and the rest of the packet
still reads; a block that cannot be read costs the others nothing. Text is as
the block holds it. The table, the rule and the readers of the values in them -
dates as cameras and editors write them, and rationals, numbers and coordinates
as sscanf reads them (`photo_internal.h`) - are the ones Diffractor files a
library by, and its tests hold libfiles to them.

| Property | XMP | EXIF | IPTC |
|---|---|---|---|
| title | `dc:title` | XPTitle | Object Name |
| description | `dc:description` | ImageDescription, else XPSubject | Caption |
| artist | `xmpDM:artist` | Artist, else XPAuthor | By-lines, joining the artist's names |
| creators | `dc:creator` | | |
| copyright, its URL | `dc:rights`; `xmpRights:WebStatement` | Copyright | Copyright Notice |
| keywords | `dc:subject`, even empty | XPKeywords, split at ";" | Keywords, joining those before them |
| rating | `xmp:Rating`, else `MicrosoftPhoto:Rating` in fifths rounded up | Rating, or RatingPercent by Windows' rule | |
| label | `xmp:Label` | | |
| dates, each with its tag | `photoshop:DateCreated`; `xmp:CreateDate`, or the year when it is one alone; `xmp:ModifyDate`; `exif:DateTimeDigitized`; `exif:DateTimeOriginal` | DateTimeOriginal, DateTimeDigitized and DateTime, each with its offset and fraction; the GPS stamp, in UTC | Date Created and Digital Creation Date, each with its time |
| location | `exif:GPSLatitude` and `GPSLongitude`, as a pair | the GPS directory, with its altitude and speed | |
| camera, lens, exposure, orientation | `tiff:Make`, `Model`, `Orientation`; `aux:Lens`; `exif:FNumber`, `ExposureTime`, `FocalLength`, `FocalLengthIn35mmFilm` | their tags, and Software as the encoder | |
| city, state, country, credit, source | `photoshop:` of each name | | each dataset |
| album, album artist, genre, show, synopsis, track, disc, season, episode | `xmpDM:` of each name | | |
| comment | `xmpDM:logComment`, else `exif:UserComment` | UserComment, else XPComment | |
| raw file, panorama | `crs:RawFileName`; `GPano:ProjectionType`, or a panorama property alone | | |
| people, faces | Each `mwg-rs:RegionList` item whose `mwg-rs:Type` is `Face`, with `mwg-rs:AppliedToDimensions`; else each Microsoft `MPRI:Regions` item; and `Iptc4xmpExt:PersonInImage` - stated by any of the three, even empty | | |

A face is its name, which may be empty, and its area: a Metadata Working Group
area's centre and size, or a Microsoft rectangle's left, top, width and height,
both read as fractions of the picture as it is stored, measured from its top
left. An area in pixels is read against the dimensions given beside it, and one
of no size is none. The people are each face's name, then each
`PersonInImage`, each once whatever its case. Microsoft's regions are read only
from a packet holding no Metadata Working Group face.

`created` is the first of DateTimeOriginal, `exif:DateTimeOriginal`, Date
Created and `photoshop:DateCreated`, else the date digitized; `digitized` the
first of DateTimeDigitized, `exif:DateTimeDigitized`, Digital Creation Date and
`xmp:CreateDate`; and `modified` DateTime, else `xmp:ModifyDate`. A photo's date
before 1800, or on the first of January 1970 or 1904 - a clock never set - is
none.

### What each writable format keeps outside its packet

An update writes each legacy field the edit sets where the format keeps it. The
round trip (`tests/round_trip.h`) writes every field to each format, patched and
rewritten, and requires its own blocks to read back exactly these:

| Format | Read back from its own blocks |
|---|---|
| JPEG, TIFF, PSD | title, description, creators, copyright, keywords, rating, the dates taken and digitized, the place, credit and source, location and orientation |
| WebP holding EXIF | as JPEG, but for the title and the place, which only IPTC keeps |
| JPEG XL holding EXIF | as WebP holding EXIF, and the orientation, which its codestream keeps too; in one made from a JPEG, only what fits the block where it lies |
| HEIF holding EXIF | as WebP holding EXIF, and the orientation its image's rotation and mirror come to |
| PNG, GIF, FLV, a WebP, JPEG XL or HEIF without EXIF | nothing: only the packet |
| MPEG audio | title, artist, album, album artist, genre, comment, copyright, track, disc, rating and year; and the date, to the minute in ID3v2.3, which keeps no seconds |
| WAV | title, artist, comment, copyright, genre and the creation date, which is when it was recorded |
| AVI | as WAV, with IDIT's date taken, which its scan reads over the creation date |
| MP4 and QuickTime | copyright, keywords, rating and the creation date. The iTunes atoms are not written |
| ASF | title, the artist as its author, or the creators where the edit names no artist, the comment as its description, copyright, keywords, rating and the creation date |

An edit leaves a Broadcast WAV's `bext` origination date - which its scan reads
as the recording date, over INFO's - as the toolkit does, so the edited date
reads from the packet.

## Codecs

Each codec is optional, on by default, and built from upstream's untouched
source by its recipe in `cmake/`, whose comments hold the details. Each is
pinned in [cmake/dd-dependencies.json](../cmake/dd-dependencies.json) by its
release archive's SHA-256 - skcms, whose generated archives differ from one
download to the next, by commit. Archives extract with the time they are
extracted, not their own file times, so a changed pin rebuilds everything built
from the old archive; `tests/dependency_arguments.cmake` holds every pin to it.
The headers an upstream build would generate are carried beside the recipe, for
MSVC and for GCC and Clang.

- **SIMD.** NASM assembles libjpeg-turbo's and dav1d's x86 SIMD on x64 and x86,
  and libaom's on x64: a consumer that has enabled `ASM_NASM` lends its own, else
  one on the path is used, or on Windows the pinned build is fetched. Without
  NASM, and on ARM64, the C paths are built, which give the same pixels more
  slowly. libaom's intrinsics are compiled each for the instruction set it is
  written for, and chosen at run time as the processor allows. zlib-ng,
  libwebp and skcms choose their SIMD at run time; libpng is built in C alone;
  libde265 takes its SSE4.1 paths with MSVC on x86 and x64 only.
- **dav1d under GCC.** `cmake/dav1d/x86/config.h` tests `_M_AMD64`, so a GCC
  build on x86-64 takes dav1d's 32-bit C paths beside its 64-bit assembly, and,
  without `unistd.h`, it cannot count processors and decodes on one thread.
- **libheif** is built with its JPEG, libde265, dav1d and uncompressed decoders,
  and its libaom and uncompressed encoders, and decodes a grid's tiles in
  parallel. Its FFmpeg decoder is not built -
  libfiles does not take on FFmpeg - so an application that has FFmpeg builds
  `plugins/decoder_ffmpeg.cc` from the fetched libheif
  (`FetchContent_GetProperties(libheif)`), registers it with
  `heif_register_decoder_plugin` before it decodes, and names it in
  `decode_options::heif_hevc_decoder`, as Diffractor does. libheif has no HEVC
  encoder here - x265, the one it takes, is GPL - so libfiles writes AVIF and not
  HEIC.
- **libaom** is built as its encoder alone, without its decoder, its examples or
  its tools. Its configuration was generated by its own CMake, with Perl and
  `-DCONFIG_AV1_DECODER=0 -DCONFIG_LIBYUV=0 -DCONFIG_WEBM_IO=0 -DENABLE_DOCS=0
  -DENABLE_EXAMPLES=0 -DENABLE_TESTDATA=0 -DENABLE_TESTS=0 -DENABLE_TOOLS=0`, for
  x86-64 on Windows and on Linux and, with `-DAOM_TARGET_CPU=generic`, for generic
  C; the x86-64 pair differ only in what the platform sets, which the carried
  `aom_config.h` and `aom_config.asm` test. `cmake/aom.cmake` lists the sources
  that configuration builds, each intrinsics source with its instruction set's
  flags. The x86 assembly configuration is 64-bit only, so a 32-bit x86 build, as
  an ARM64 one, builds libaom's C alone.
- **libjxl** is built with its decoder, encoder, colour management and thread
  runners, without the extras its tools use.
- **libwebp** is built as one library - decoder, encoder, sharpyuv, mux and
  demux - and decodes on the caller's thread.
- **LibRaw** is built with Adobe's DNG SDK, to which it hands the DNGs it cannot
  read alone - lossy, or JPEG XL. The SDK's own XMP backend is written over the
  XMP Toolkit, so libfiles builds [src/dng_xmp_sdk.cpp](../src/dng_xmp_sdk.cpp)
  in its place, over its own XMP data model. The SDK's `RawEnvironment.h` is in
  `cmake/dng/`; the archive's copies of libjpeg, libjxl and the toolkit are not
  built.
- **giflib and libtiff** serve native documents. libtiff links the shared
  zlib-ng; its JPEG, old-JPEG, WebP, Zstandard, LZMA, JBIG, LERC and libdeflate
  codecs are off.

## How it reads

A `reader` reads its stream through one block-aligned window, keeping what a
moved window still overlaps and sending large reads straight to the caller's
buffer, and counts the reads it issues (`source_reads`). Scanners touch only
the structure they need: JPEG's segments up to the first scan, PNG's and WebP's
chunk headers - never their image data - and TIFF's first two directories.
Embedded thumbnails are located, not read.

### Stills

A HEIF file - HEIC, AVIF or any other ISO base media still image - is read from
its file type box, the headers of its top-level boxes and its `meta` box, which
says what each item is, where its data lies (`iloc`: in the file or in `idat`,
in one extent or several) and which properties and references it has. Of the
items' data only the metadata items' is read - Exif, XMP, IPTC - and a JPEG
item's frame header; a coded image never is. The size is the primary item's
after its clean aperture and rotation. The pixel format is the chroma libheif
decodes to: from `hvcC` or `av1C`, a grid's from its first tile. A file whose
`meta` box follows the media data costs a read at each end.

A JPEG XL file is read as libjxl reads it for its basic information and boxes:
the codestream's size header and image metadata - from a bare codestream, the
container's `jxlc` box, or `jxlp` parts with other boxes between them - and the
`Exif` and `xml ` boxes. The width and height are the codestream's, unturned.
Boxes a container keeps Brotli-compressed in `brob` boxes are read with
libfiles' bounded Brotli decoder when `LIBFILES_JXL` is on, or with the
decompressor a caller gives in `scan_limits::brotli`; `read_packet` and an
update read them the same way.

Which HEIF and JPEG XL files read at all, and as what, follows libheif and
libjxl as libfiles builds them, down to the damaged files they refuse, but for
the [differences listed below](#where-libfiles-differs-on-purpose). An Exif item
or box begins with the offset of its TIFF header; what precedes the header, and
the JPEG `Exif` signature some writers put there, is removed, so
`metadata.exif` starts at the byte-order mark as for every other format.

A scan says whether a still holds an animation (`scan_result::animated`): a
WebP or JPEG XL whose header says so, a PNG with an `acTL` chunk before its
image data, a HEIF file with a movie box, which holds an image sequence, or a GIF
whose block chain - walked to its end for the XMP extension anyway - holds a
second image. A decode gives the first image alone, whether the rest are frames
or the tiles of one picture, so either is an animation here.

A scan says too whether a still is HDR by its own colour description
(`scan_result::hdr`): a PNG whose cICP chunk - the one libpng keeps - names PQ or
HLG, or a HEIF whose primary item's `nclx` colour does. A decode maps such a
picture by that description rather than by an ICC profile beside it. A JPEG
XL's colour encoding is not looked at. The primaries and transfer the same
description names are reported as H.273's code points
(`scan_result::colour_primaries`, `scan_result::colour_transfer`), and as 2,
unspecified, where colour is described by an ICC profile alone.

A scan says whether a photo carries a gain map (`scan_result::gain_map`) without
reading the gain map. A HEIF's items say so: an ISO 21496-1 tone map item derived
from the primary, or Apple's auxiliary gain map image. A JPEG's are said beside a
multi-picture index that lists a second image: by its primary's own segments -
the hdrgm namespace in its XMP, or ISO 21496-1's versions in an APP2 segment -
and failing those by the first segments of a listed image that is no large
thumbnail, read as a gain map's own metadata is read, which is the only place
Apple says it. Only then is a head read, of at most 64 KB, and never for a
camera's thumbnails. An inspect scan, which reads every listed image's head,
comes to the same answer by the same rule.

An inspect scan's listing (`scan_result::structure`) describes a picture's colour
in one vocabulary across formats: a HEIF's `nclx`, a PNG's cICP and a JPEG XL
codestream's colour encoding are named in H.273's terms - primaries, transfer,
with PQ and HLG marked HDR, and for HEIF the matrix, and the range - with the
content light level and mastering display a HEIF's `clli` and `mdcv` or a PNG's
cLLI and mDCV give, and a JPEG XL's intensity target. A gain map is named with
its kind, size and reach: a HEIF's ISO 21496-1 tone map item or Apple auxiliary
image, and the JPEG a JPEG's MPF index lists that its own first segments say is
one - Apple's, Ultra HDR's or ISO 21496-1's - its headroom read as a decode reads
it. A PNG's listing has its header's fields and each chunk type, with its count
and bytes.

### Metadata blocks

An EXIF block is read by walking IFD0 and the Exif, GPS and Interoperability
directories it points to; a directory is read once however many pointers lead
to it, and no more than 32 in all. An entry counts only if it is well formed.
Text is decoded however a camera or Windows wrote it - ASCII, UTF-8, a
UserComment marked `UNICODE`, the UTF-16 of an unmarked tag, the UTF-16LE
Windows stores as bytes in its XP tags - and text that is binary reads as
empty. `exif_metadata::altitude` is 0 both at sea level and where the block
gives none.

Every TIFF directory libfiles reads for what it says, it reads with that
parser's entries: an EXIF block for `read_exif` and `summarize_exif`, and a
TIFF file's own directories, into `scan_result::tiff_metadata`, without copying
the file's pixels into an EXIF block. One rule gives the orientation - the last
Orientation in IFD0, else the last in IFD1 - so a scan, a decode and a reading
of the same block cannot disagree, and a copy of the tag in any other directory
is not the image's. In a TIFF file, values above 64 KiB and a packet above
8 MiB are ignored, and a damaged metadata directory costs its fields, not an
otherwise readable image.

An inspector's listing of an EXIF block, `list_exif`, names every entry of every
directory, renders its value as text and decodes the maker note of a Canon,
Casio, Fuji, Nikon, Olympus, Pentax or Sanyo camera as libexif lists them: the
same entries from a damaged block, the same names, the same words. The block is
read by libfiles' TIFF reader with libexif's rules for what a listing holds;
the names, words and maker note decoders are libexif's own code, ported by
[tools/port_libexif.py](../tools/port_libexif.py) into `src/libexif_*` and kept
close to it, so that a fix to libexif can be carried over.

An IPTC-IIM block - Photoshop's resource 0x0404, a TIFF's tag 33723 - is read
by `read_iptc` for the fields an organizer files and listed by `list_iptc`
(`iptc.h`), with libiptcdata's dataset table.

An ICC profile is summarized by `summarize_icc` and listed by `list_icc`
(`icc.h`), which recognise the colorants of sRGB, Adobe RGB (1998), Display P3,
Rec. 2020 and ProPhoto RGB. Tags that share a region share its bytes, and the
regions charged may not add up to more than the profile. `icc_declares_rgb`
reads only the header's colour space signature, for a caller deciding whether a
profile can describe RGB output. `icc_srgb_mapping` says what a decode makes of
the colours a profile describes ([How it decodes](#how-it-decodes)).

Photoshop's image resources - a PSD's resource section, a JPEG's APP13 - are
read by one rule in the scans, the writers and the JPEG encoder: a resource's
data lies inside the block, and the pad byte after the last one's may be
missing, as some writers leave it. Every resource is written with its pad byte.
Of a kind a file repeats - IPTC, XMP, EXIF or the colour profile - the first
resource that holds anything is the one a scan reads and an edit changes.

### Camera raw

A camera raw file is read by LibRaw, by its path or through a reader, and
nothing is developed (`raw.h`). Its date is LibRaw's timestamp taken back to
the time of day it was made from. An inspector's listing holds everything
LibRaw parsed, by section, each maker's own fields among them. Its focal
lengths, apertures and the like have one significant digit - 100 mm lists as
"1e+02 mm" - and a value LibRaw names, such as a Fuji film mode or a
white-balance preset, lists without its name.

### Audio and video

`scan_media` describes a recording without decoding it (`media.h`). It reads the
container's headers, a stream's first frames only where its configuration is
not in them, and the end of the file where the duration or the tags are there.
`complete` says that every audio and video stream was described: Diffractor
opens FFmpeg to decode, and to describe only a file that is not complete.

The model is FFmpeg's description of a stream. Codecs have FFmpeg's short
names - `h264`, `aac`, `mp3`, and `pcm` with its sample format beside it rather
than in the name. A pixel format is its chroma and depth (`yuv420`,
`yuv422 10bit`, `rgb`), and a sample format is stated only where the codec
stores one, for PCM and lossless audio. Streams are numbered as FFmpeg's
demuxers number them - an MP4 track ID, an AVI stream index, a PID - so a player
finds a stream FFmpeg opened by libfiles' description of it. A language FFmpeg
would give as `und` is empty. Tags keep the names the container gives them, and
fill the properties FFmpeg's demuxers file them under. A date keeps the zone the
file states: a UTC instant, a local time with its offset, or a local time with
none; a date alone is a day, not midnight in UTC.

Each container's structure is read by one parser, which its scan and its writer
share - `mp4_internal.h`, `riff_internal.h`, `asf_internal.h`,
`flv_internal.h`, and `id3_internal.h` for MP3's ID3v2 tag - so a scan finds the
packet `read_packet` finds and an edit reads back as it was written. Each codec
header - an H.264 or HEVC SPS, an AV1 sequence header, an `esds`, a
`WAVEFORMATEX` - has one reader, which every container shares.

An MPEG transport stream - TS, or the 192-byte packets of M2TS and MTS - is read
from its program association table to the first program's map; a recording that
kept the whole table but carries one other program is read for the program
whose map comes round again. Each stream is named by its type and descriptors,
numbered by its PID, and its own header - an H.264 or HEVC SPS, an MPEG video
sequence header, a VC-1 sequence header and entry point, an audio frame header,
an LPCM header - is read from its first PES packets; the scan stops as soon as
every audio and video stream is known, and reads no further than 8 MiB. A
TrueHD stream's own header is not read, so it has no sample rate or channels,
and the scan of a file that carries one is not complete. An MPEG program
stream - MPG, VOB, MOD - is read pack by pack, its streams numbered by their
stream ids, and private stream 1's by their substream ids, as FFmpeg's demuxer
numbers them. No table need list them, so its start is read until the streams
found are known and a second of the stream has passed, or until each stream a
program stream map lists has been seen. Both are timed as FFmpeg times them:
each stream from its first PTS to its last plus a frame, the last found by
reading backwards from the end in steps from 256 KiB to 8 MiB, across the 33-bit
wrap; the file from the earliest start to the latest end. A transport stream
whose payloads are scrambled is timed by its PCR, a program stream without
timestamps by its packs' SCR. An AVCHD camcorder keeps the recording date, its
maker and its model in the SEI of the H.264 stream's first access unit, where
the scan reads them.

## How it writes

An update is planned from a structural read of the file as source byte ranges,
each replaced by new bytes, with everything between them kept as it is
(`write.h`). A plan that moves nothing the file holds - every replacement keeps
its length, apart from what is added at the very end - can be patched where the
file lies (`write_in_place`); any other is written in one pass to a destination
the caller has staged (`write_updated`), so a source is never left half
rewritten. Patching writes what is appended first, then the patches in the
steps the planner orders them in, flushing after each, so that wherever it is
interrupted the file reads whole; the writes within a step may land in any
order, and each is taken to land whole. Every format's patch is replayed over
each state an interruption could leave (`tests/interruptions.h`).

A packet is first serialized at the old packet's exact length, as the toolkit
does, so that an edit which fits is a patch, and a rewrite that keeps its length
is written as the one span of it that changes: a rating in a packet that keeps
its length is a byte or two.

- **PNG**: an uncompressed `iTXt` keyed `XML:com.adobe.xmp`. A rewrite removes
  every XMP text chunk, in whatever form, and puts one after `IHDR`.
- **GIF**: the `XMP DataXMP` extension with its 258-byte magic trailer, before
  the GIF trailer. GIF87a cannot carry one and is refused.
- **WebP**: the `XMP ` chunk, replaced where it stands or appended, with every
  other chunk kept in place. A simple file gains the VP8X header it needs, its
  canvas and alpha read from the image itself. The packet keeps its wrapper and
  padding, so that later edits can be patches. A first packet appended at the
  end of the file is declared in VP8X only once the RIFF size takes it in. An
  `EXIF` chunk the file already has takes the edit's EXIF fields.
- **JPEG XL**: the packet's `xml ` box and the `Exif` box, found as the scan
  finds them - the last of each - and replaced where they stand, with every
  other box kept in place. A first packet goes before the codestream's first
  box, where libjxl's encoder puts one, and a bare codestream is put in a
  container for it: the signature and file type boxes, the packet, then the
  codestream whole in a `jxlc` box. A box Brotli-compressed in a `brob` is read
  as the scan reads it and replaced by a plain one; one that cannot be read
  refuses an update that would change it. An `Exif` box the file already has
  takes the edit's EXIF fields. The orientation is the codestream's, which
  libjxl turns the picture by: it is written in the codestream's header, its
  three bits rewritten where the header holds them, else written in with what
  keeps every byte after the header aligned - an explicit default tone mapping,
  or a bit depth of 8 in its longest form - so the codestream's box grows by
  whole bytes; and the EXIF's is made to agree. An orientation the header cannot
  take - one whose codestream begins in a part read out of order, or one a frame
  index would see bytes move under - is refused. A file made from a JPEG
  keeps a `jbrd` box, from which libjxl gives that JPEG back with the first
  `Exif` and `xml ` boxes as its segments, and refuses to unless each is the
  size it was. So in such a file those boxes keep their size: a packet that
  cannot be written at its old length is refused, an EXIF field that would
  grow the block is left as it is, for the packet to hold, and an orientation is
  written only where that EXIF holds one to turn with it, so the JPEG given back
  turns with the file.
- **HEIF and AVIF**: the packet in the XMP item the scan reads from the primary
  image, and the EXIF in its first Exif item. A file without a packet gains one
  as libheif adds one: a hidden `mime` item of `application/rdf+xml` with a
  `cdsc` reference to the primary image, under the next id after every id its
  items, references and entity groups give. A file without EXIF gains none. An
  orientation turns the picture as libheif decodes it: the primary image's
  rotation and mirror, and its thumbnails' and auxiliary images' - Apple's HDR
  gain map among them - and those of the gain map an ISO 21496-1 tone map item
  applies to it, are written anew as libheif writes them for the orientation
  they come to - patched where the properties hold them, else in a rebuilt
  `iprp` - and the EXIF and a packet that states one take that orientation. The
  tone map item itself is left unturned, as libavif writes and reads it: its
  inputs are turned before it applies one to the other. Items whose data keeps
  its length in one unshared piece are patched where they lie. Anything else is
  rewritten in one pass: unshared data in an `mdat` is replaced where it lies,
  and shared data, data elsewhere, or new data goes at the start of the first
  `mdat`, or in an `mdat` of its own after the
  meta box when the file has none; the meta box is rebuilt with the item, its
  reference and its location; an `mdat` that changes length is given its new
  size; and every item location moves with the bytes it locates, as an image
  sequence's chunk offsets do - each track's `stco` or `co64` table rewritten
  where it lies, at its size. Every other box keeps its place in the order of
  the file and every other item its data, so a motion photo's video stays last.
  A fragmented sequence, whose offsets lie in movie fragments an update does not
  read, is refused a rewrite.
- **JPEG**: the packet's APP1, EXIF's APP1 and the IPTC in APP13's Photoshop
  resources, with its MD5 digest beside it. Segments the file lacks are added
  after its leading APP0 and APP1 segments. Extended XMP is left as it is. A
  multi-picture index's sizes and offsets follow the pictures after the primary
  as the header grows or shrinks, and the packet's gain map signalling is kept
  true to the file: the length of the gain map the index locates, or, when
  nothing follows the primary, nothing said of one - and then an index pointing
  past the end, and ISO 21496-1's version segment, go too.
- **TIFF**: by appending. A packet that fits is written over the old one;
  anything else is appended at the end of the file and its entry repointed, so
  the image data never moves. A DNG carries no IPTC.
- **PSD**: the image resources section is rebuilt, unless only a packet that
  fits changes, which is then a patch.
- **MP4 and QuickTime**: the packet in a top-level XMP `uuid` box in an ISO
  file and in `moov/udta/XMP_` in a QuickTime one; tags and rating in the
  `Xtra` box Windows reads, a copyright in `cprt` when the movie has user data,
  and a created date in `mvhd` - a QuickTime movie's as an ISO file's, its
  packet keeping `xmp:CreateDate`. Nothing the file holds moves, so chunk
  offsets stay valid and the media is never copied. A packet that outgrows its
  box is appended at the end of the file, and the old box becomes `free`. The
  movie's user data grows into the free space it holds; when it cannot, the
  movie box is appended with 4 KB of room and the old one freed, rather than
  rewritten where it lies. The packet's box always follows the movie box, since
  FFmpeg reads no `uuid` box before it, so it moves with a movie box that moves.
  A last box that runs to the end of the file is closed before anything
  appended is made part of the file. Of several XMP `uuid` boxes the last is
  read, as FFmpeg reads it, and of several movie boxes the first; an update
  frees the others, which only an interrupted update leaves. A box whose 64-bit
  size is too small to hold its own header is refused, as the media scan
  refuses it. A CR3 raw, in the same container, is not an MP4 to libfiles.
- **ASF** (WMV, WMA): the packet in a top-level XMP object after the data; the
  title, the artist as its Author - or the creators, where the edit names no
  artist - the copyright, and the comment as its Description in the header's
  Content Description, where Windows Explorer, FFmpeg and the taggers that write
  WMA read them; tags and rating in the WM/Category and WM/SharedUserRating
  descriptors; and a created date in the File Properties, whose file size is
  kept true. The edit's description stays in the packet. A packet that outgrows
  its object is appended and the old object becomes padding. The header is
  patched where it lies, in one write, whenever its padding - in the header or
  its extension - takes the change; otherwise the file is rewritten in one pass
  with 2 KB of padding in its header for the next edit. The copyright and
  licence URLs are not written, since neither FFmpeg nor Windows reads them, and
  a packet's `asf:NativeDigest` is left as it is: a toolkit that finds it stale
  imports the header's fields, which libfiles wrote from the same edit. The
  first XMP object after the data is read; upstream FFmpeg reads none.
- **RIFF** (WAV, AVI): the packet in a `_PMX` chunk at the top of the RIFF
  form; title, artist, comment, copyright, date and genre in LIST INFO, a WAV's
  title also in an existing DISP chunk, and an AVI's original date in an
  existing IDIT. Nothing that holds media moves, so an AVI's index offsets stay
  true. A chunk that fits is patched; one that grows takes the JUNK chunk after
  it - written inside the JUNK, then made the chunk read by one 8-byte header
  write - or a free JUNK chunk, or is appended at the end of the form, which the
  form's size then takes in before the old chunk becomes JUNK. libfiles reads an
  AVI's packet after `movi` in its first form, and of two INFO lists that give a
  field takes the one before `movi`, so they are written there; an AVI that
  continues in AVIX forms cannot grow its first form, and takes only what fits.
- **MPEG audio** (MP3, MP2, MPA): the packet in an ID3v2 `PRIV` frame owned by
  `XMP`; title, artist, album, album artist, genre, comment, track, disc,
  copyright, its URL and the date in their text frames, and the rating in the
  `POPM` frame Windows Media Player reads, which a rating of no stars removes.
  Text is written without a terminator, and a genre as its name rather than a
  numbered ID3v1 code; ID3v1 is left alone. The tag keeps its version, and
  every frame the edit does not own is kept byte for byte - an ID3v2.4 tag whose
  frames iTunes sized as plain integers is edited with those sizes, as a scan
  reads it. When the frames fit the tag's space it is patched where it lies;
  otherwise the file is rewritten in one pass with 2 KB of padding.
- **FLV**: the packet as the `liveXML` string of an `onXMPData` script tag after
  `onMetaData`. A packet that fits is patched; otherwise the file is rewritten
  in one pass, and the keyframe file positions and file size `onMetaData` keeps
  are moved with the media.

Outside the packet, an edit is semantic (`legacy_edits`): each field it sets is
written to every place the format keeps it - a title to IPTC's Object Name, a
description to EXIF's ImageDescription and IPTC's Caption, tags to IPTC's
Keywords and Windows' XPKeywords, a rating to EXIF's Rating and RatingPercent,
dates, GPS and the orientation to EXIF. Nothing the edit does not set is
written: legacy metadata is neither imported into the packet before a write nor
exported from it after, IPTC and EXIF are created only when an edit writes a
field they hold, and the packet keeps the `exif:` and `tiff:` properties it
holds, which the toolkit deletes once it has copied them into EXIF.

EXIF is edited without moving a byte it holds: a changed value that fits is
written where it lies, and anything else is appended to the block and
repointed, so a maker note's absolute offsets and the thumbnail stay valid. A
value fits when it is the same size, or shorter text, which the zeros after it
end; a shorter value of any other type is appended, since the old entry's count
would read the zeros as part of it. Patched, a value is written before the entry
that names it and an appended directory before the pointer to it, so an
interruption between them leaves the old entry over the new value. IPTC is
written as UTF-8 and says so; each dataset's header is read by the reader's
rule, every dataset is kept - empty ones, and ones longer than the reader takes,
too - and its text is carried over as `read_iptc` reads it.

`make_exif`, `set_exif_numbers` and `unlink_exif_thumbnail` (`exif.h`) write
blocks an application handles itself: a fresh block laid out as libexif lays
one out, numbers overwritten where they stand for a picture turned without being
re-encoded, and IFD1 unlinked for a block carried into a re-encoded picture.
`read_webp_compression` (`encode.h`) says how a WebP was compressed - losslessly,
or lossily at the quality libwebp's estimate from its quantizers gives - and
`read_jxl_compression` whether a JPEG XL was - losslessly where its colour is
coded as the file describes it rather than in XYB, but for one repacked from a
JPEG, which holds that JPEG's loss - so an application writing one again can
keep it so.

A JPEG's gain map - the JPEG after its primary that an Ultra HDR, Adobe, Apple
or ISO 21496-1 file holds, which says how much brighter than the primary each
part of the scene is shown on an HDR display - is found through the primary's
multi-picture index and its own metadata (`find_jpeg_gain_map`, `encode.h`).
`attach_jpeg_gain_map` puts one after a primary with the index Ultra HDR writes,
in place of any the primary held, and the signalling the gain map's metadata
calls for: hdrgm:Version, and its length in the XMP container directory's
GainMap item, for Adobe's hdrgm XMP; the version segment for ISO 21496-1's. An
application editing a photo edits its gain map with it, at its own resolution,
and attaches the result; a re-encoding that leaves it behind says none follows,
as `carry_jpeg_metadata` and an attach of no gain map make it.
`read_jpeg_gain_map_metadata` reads how a gain map applies from its own ISO
21496-1 segment, else its hdrgm XMP, and `mark_jpeg_gain_map` writes both into a
JPEG an application encoded as a gain map - after its JFIF segment, as Ultra HDR
lays one out - for `attach_jpeg_gain_map` to signal.

A gain map's metadata is ISO 21496-1's, one syntax wherever a file keeps it
(`gain_map_metadata`, `decode.h`; `write_gain_map_metadata` and
`read_gain_map_metadata`, `hdr.h`): each value a fraction over a power of ten,
the channels written once where all three agree. Adobe's hdrgm values say the
same in other words, and Apple's HDR gain map - a grey map coded by BT.709's
transfer, each value multiplying the photo's light by 1 + (headroom - 1) ×
value - is said in ISO 21496-1's terms by rewriting its values as the log2 of
that factor over the headroom's (`convert_apple_gain_map`). Its headroom is its
own XMP's `HDRGainMapHeadroom`, else what the photo's maker note tags 33 and 48
give by Apple's formula (`apple_gain_map_headroom`). `make_gain_map` makes one
for an HDR picture and its SDR rendition: the log2 of their luminance ratio, a
value for each block of pixels, offset by 1/64 of SDR white as ISO 21496-1
offsets it and spread over its range.

`transcode_jpeg_to_jxl` (`encode.h`) repacks a JPEG as a JPEG XL without loss,
as cjxl does by default: libjxl compresses its coefficients again and keeps, in
a 'jbrd' box, what it needs to give the JPEG back byte for byte. The JPEG's
EXIF and XMP go in plain boxes - the ones it is given back from, whose size the
metadata writer keeps - and its other segments, IPTC among them, only in the
'jbrd' box. `reconstruct_jpeg` gives the JPEG back, with the EXIF and XMP the
boxes hold by then; neither is possible for a JPEG libjxl cannot keep - CMYK,
arithmetic-coded or of 12 bits - or a JPEG XL made from pixels.

`encode_avif` (`encode.h`) writes an AVIF through libheif with libaom, as
libheif's heif-enc does: lossy as YCbCr 4:2:0, its alpha at the colour's
quality, or lossless as the RGB itself at 4:4:4, alpha and all - an identity
matrix, which libheif does not choose by itself, and without which the
conversion to YCbCr would lose. Sixteen bits a sample are written as their ten
most significant, the most AVIF's baseline and advanced profiles hold, or as
twelve where asked, which takes AV1's professional profile; either gives back
exactly what a decode of ten or twelve widened, so only that comes back bit for
bit. The orientation is the image's rotation and mirror, which a HEIF reader
turns the picture by whatever the EXIF says, so the EXIF item's is set to agree;
the ICC profile describes the pixels, and without one they are sRGB. A gain map
is written as libavif writes one: an image of its own, coded at a quality of 90
and turned as the picture is, and a `tmap` item holding its ISO 21496-1 metadata,
whose `dimg` references take the picture and the gain map, with the picture's
extent and no turn of its own, and the `tmap` brand. libheif writes all that,
but neither marks the gain map hidden nor groups the `tmap` item with the
picture, so the file it writes is finished by the HEIF writer's own rewrite:
the gain map's entry marked hidden and an `altr` group added, the `tmap` item
preferred to the picture it falls back to.

An HDR picture's signal - what a decode that keeps HDR gives - is written by the
AVIF, JPEG XL and PNG encoders with its description in place of an ICC profile
(`embedded_metadata::hdr`): an AVIF's `nclx` colour with its content light level
and mastering display, a JPEG XL's colour encoding with its peak as the
intensity target, a PNG's cICP, cLLI and mDCV chunks. The JPEG and WebP encoders
refuse it, as neither says what PQ or HLG are, and an application maps the
signal to SDR for them first (`map_hdr_to_sdr`, `hdr.h`).

## How it decodes

A decoder writes into memory its caller provides (`image_sink`), so an image
lands in the caller's own surface with nothing copied between;
[decode.h](../include/libfiles/decode.h) owns when the sink is asked, the
reductions, NV12 and what a result reports. A `decoder` keeps libjpeg's state
between images, so a stream of thumbnails does not set it up for each.

- **JPEG** (`LIBFILES_JPEG`): baseline, extended and progressive, and lossless
  up to 16 bits, in YCbCr, RGB, grayscale, CMYK and YCCK, as B8G8R8X8 or NV12;
  high precision scaled to 8 bits, CMYK multiplied out, inverted unless an Adobe
  marker says it is stored inverted. The frame carries the last EXIF block's
  orientation. A file cut short decodes as far as its data reaches, the rest as
  libjpeg makes of the end of image put in its place - except a progressive
  one, which has no row until every scan is in.
- **PSD**: the composite image Photoshop keeps after the layers, in bitmap,
  grayscale, indexed, RGB, CMYK, multichannel, duotone and Lab modes, at 8 bits
  a channel or 1 in bitmap mode, raw or PackBits, as B8G8R8X8.
- **PNG** (`LIBFILES_PNG`): every colour type and bit depth as B8G8R8A8 with
  straight alpha - 16 bits scaled to 8, low bit depths and palettes expanded,
  tRNS made alpha, opaque where the file has none - and a file's own gamma
  corrected for an sRGB display when it gives one and no sRGB chunk, and no iCCP
  profile a decode maps. A 16-bit
  file, for a caller that takes them (`decode_options::allow_16bit`), keeps its
  sixteen bits, as R16G16B16X16 or with alpha R16G16B16A16. An interlaced file
  decodes whole.
- **WebP** (`LIBFILES_WEBP`): lossy or lossless, as B8G8R8A8
  with straight alpha where it has alpha and B8G8R8X8 where it has none. An
  opaque lossy image the caller takes as NV12, and does not reduce, comes as
  VP8's own limited-range YCbCr, an odd edge cropped. The orientation is the
  first EXIF chunk's, with or without a JPEG `Exif` signature leading it. An
  animated WebP gives its first frame, as libwebp's animation decoder draws it
  on the canvas - transparent where the frame leaves the canvas uncovered - at
  the canvas's size, since that decoder does not scale.
- **HEIF and AVIF** (`LIBFILES_HEIF`): the primary image, its first
  thumbnail item, or its gain map, as B8G8R8A8 with straight alpha where it has
  alpha - premultiplied alpha made straight - and B8G8R8X8 where it has none;
  one of more than eight bits, for a caller that takes them, at sixteen. libheif
  turns the image by its item's own rotation and mirror, and the frame says so
  (`transformed`); otherwise it carries the primary's EXIF orientation. The gain
  map (`decode_options::heif_gain_map`) is the input an ISO 21496-1 `tmap` item
  takes beside the primary, with the metadata the item holds, else Apple's HDR
  gain map auxiliary image, its values said in ISO 21496-1's terms; the frame
  carries how to apply it (`frame_info::gain_map`). A damaged file decodes as
  libheif recovers it.
- **JPEG XL** (`LIBFILES_JXL`): the first frame, as B8G8R8A8 with straight
  alpha where it has alpha - libjxl making premultiplied alpha straight - and
  B8G8R8X8 where it has none; one of more than eight bits a sample, for a
  caller that takes them, at sixteen. The pixels are left as the codestream
  stores them and its orientation reported, so the size decoded is the size a
  scan reports. libjxl writes RGBA straight into the caller's rows, where red
  and blue are swapped. libjxl's resizable runner decodes on a thread for each
  group of 256 by 256 pixels, up to eight, the caller's among them. A file cut
  short is not decoded: libjxl does not tell a file cut short from a damaged one.
- **Camera raw** (`LIBFILES_RAW`), through `load_raw` (`raw.h`): a preview a
  decoder reads, or the raw data developed as LibRaw's `dcraw_make_mem_image`
  would - the camera's white balance and LibRaw's auto-brightness - but
  straight into the caller's rows, at sixteen bits a channel for a caller that
  takes them, and with its highlights kept: a channel that clips is blended
  towards white, and light past white rolls off from 80% of white rather than
  clipping.
- **Commodore 64 media** hold no picture of their own: `list_c64_media` lists a
  disk, tape, cartridge or program as the C64 lists a directory, and
  `draw_c64_listing` draws the listing in the C64's upper-case character set
  (`commodore.h`). Each row also carries the file's stored PETSCII name and
  type byte, so a caller can load exactly the file a row names. The listing
  carries the name the image gives itself too, in the rows' plain text: a disk's
  from the header the C64 lists above its directory, a tape's and a P00
  program's; and a cartridge's as the ASCII its header holds, in its own case.

A PQ or HLG still - a PNG its cICP chunk marks so, a HEIF its `nclx` colour, a
JPEG XL its colour encoding - is decoded as the SDR picture an SDR display can
show (`frame_info::tone_mapped`), as Diffractor shows HDR video: 203 nits, where
BT.2408 puts HDR reference white, as SDR white; PQ rolled off on luminance by
the BT.2390 EETF from its peak - its content light level, else its mastering
display's, else 1000 nits; HLG through the BT.2100 EOTF at a system gamma of 1.0
for a display whose peak is SDR white; wide gamut taken into BT.709 with
Bradford adaptation, and a colour that leaves BT.709's cube desaturated towards
its luminance rather than clipped. Each pixel is mapped through tables, to
within a level of the exact arithmetic (`hdr.cpp`), and encoded with the sRGB
curve. The frame says how the picture is coded (`frame_info::hdr`): its transfer
and primaries, and the light levels the file states. A caller that keeps HDR
(`decode_options::keep_hdr`) is given the signal itself instead, at sixteen bits,
for an editor to work on and write back; `map_hdr_to_sdr` maps it as the decode
would have, and `carry_sdr_edit` (`hdr.h`) carries an edit made to that SDR
rendition into it, scaling each pixel's light, channel by channel in its own
primaries, by the ratio its rendition's light was scaled by, with ISO 21496-1's
offsets - as a gain map scales whatever an edit made of the picture under it.

A picture whose ICC profile describes colours other than sRGB's - a phone's
Display P3, a camera's Adobe RGB (1998) - is decoded in sRGB's for a caller that
asks (`decode_options::map_to_srgb`), as an organizer shows it, and otherwise as
the file holds it, as an editor that writes the profile back wants it and every
reader before took it. libfiles maps an RGB profile of colorants and tone
curves, as display, camera and working-space profiles are, and a grey one of a
tone curve (`icc_srgb_mapping`, `icc.h`): each channel made linear by its own
curve, taken into sRGB's primaries through the D50 connection space with white
fixed to white, a colour sRGB cannot show clipped channel by channel - as
colour-managed viewers show such a profile's colours - and encoded with sRGB's
curve, a byte a channel through integer tables and sixteen bits in floating
point. A profile of lookup tables alone, or a CMYK, Lab or named-colour one, is
not applied; sRGB's own, or one that would move no colour half a level, needs
nothing done. The profile is the one a scan finds - a JPEG's, PNG's, WebP's,
HEIF's, AVIF's or PSD's - and a JPEG or WebP so mapped is never NV12. An HDR
still is mapped by its own description instead (`scan_result::hdr`), and a
PNG's iCCP, when mapped, takes the place of its gAMA, as the PNG specification
has it. `map_icc_to_srgb` maps pixels as the decode does, and `carry_srgb_edit`
carries an edit made to that rendition back into the profile's colours as
`carry_sdr_edit` carries one into an HDR signal: a pixel the edit left comes
back exactly as it was, and a colour outside sRGB keeps what of its gamut the
edit does not take.

### Native documents

[document.h](../include/libfiles/document.h) serves image editors that keep
packed indices, palettes, grayscale storage, frame offsets, transparency,
disposal and delays rather than flatten everything to BGRA. `decode_document`
reads a named format through the same positional `reader` into a
`document_sink`, which admits each frame, receives top-down, four-byte-aligned
rows and borrowed metadata blocks, and owns the output and its memory budget;
`maximum_working_bytes` bounds only the decoder's scratch rasters and strips. A
reader over memory borrows its bytes, so a document in memory is never copied
through another stream. With `pixels` off, headers and metadata are read
without delivering or keeping a row, and pixels are not validated: PNG uses its
scan's chunk walker, TIFF visits directories without reading strips, and JPEG
stops before decompressing, progressive input included. A `decoded_document`
keeps a document whole and owned (`document_keeper`), and `copy_bgra` reads any
document storage as BGRA.

JPEG, PNG and PSD share their parser and codec with `decode` but keep native
samples: grayscale or BGR JPEG with its original DCT settings, PNG without
display gamma and with packed palettes, PSD with its own colour and storage
conventions. GIF and TIFF keep the complete frames and pages before a damaged
later one, with a warning. PCX and WPG read through bounded cursors; a WPG's
first raster streams directly and later rasters are staged until complete.
Native TIFF rejects a grayscale (MINISWHITE or MINISBLACK) strip image of one
sample of up to 8 bits; tiled images, deeper samples and the other layouts it
does not read itself go through libtiff's RGBA reader.
A TIFF's own EXIF directories are extracted without copying image strips,
keeping opaque value bytes and converting the byte order of numbers; descriptive
TIFF fields and an orientation other than upright can supply an EXIF block, but
an upright plain image gains none. The native readers' deliberate differences
are [listed below](#where-libfiles-differs-on-purpose).

`encode_document` (`encode.h`) keeps palettes, packed storage and pages where
the output format permits them, and refuses a layout it cannot keep rather than
flatten it. BMP, PNG and GIF keep indexed and packed rows; GIF also keeps frame
offsets, delays, disposal and local palettes, and TIFF its pages and
resolution. BMP has no standard metadata blocks and its BI_RGB alpha byte is
advisory, and GIF carries no metadata but comments; each says so in a warning.
PNG documents keep ICC, EXIF, XMP, Author, Comment and any other `tEXt` or
uncompressed `iTXt` chunk, keyword and language included. A TIFF's EXIF is
imported through the shared directory editor into standard primary, Exif, GPS
and Interoperability directories, with no private wrapper tag. An application
that has made pixels upright asks for `reset_orientation_metadata`: EXIF and XMP
orientation and dimensions are corrected and stale EXIF thumbnails unlinked, and
metadata that cannot be corrected fails the encode rather than carry the old
turn onto upright pixels. A JPEG coefficient transform never falls back to
recompression.

## How it behaves

The XMP data model follows the XMP Toolkit, so that what an application reads
from a packet is what the toolkit gives it:

- **Recovery.** A malformed packet keeps whatever precedes its first fault, as
  the toolkit does with Expat; a packet the toolkit rejects whole - an alias to
  x-default that carries its own language - throws `xmp::error`. Invalid UTF-8
  is read as Windows-1252, and ASCII controls become spaces.
- **Normalization.** Dublin Core arrays written as simple values become arrays;
  the toolkit's standard aliases (`tiff:Artist` is `dc:creator[1]`,
  `exif:DateTimeDigitized` is `xmp:CreateDate`, and the rest) are moved to their
  actual properties; `exif:UserComment`, `xmpDM:copyright`, GPS time stamps and
  broken alt-text arrays are repaired the same way.
- **The API.** Paths, localized-text selection, array rules, iteration order and
  the namespace table, with its numbered prefixes, are the toolkit's - but for
  two namespaces the table knows from the start, which the toolkit's does not:
  Windows' `MicrosoftPhoto` and Google's `GPano`, which the property table
  reads. A packet that declares either under another prefix is written with
  these.
- **Serialization.** The toolkit's layouts, wrapper and padding, so a packet can
  be rewritten where it lies.

### Where libfiles differs on purpose

Reading matches its references - the XMP Toolkit for packets, FFmpeg for audio
and video, libheif and libjxl for HEIF and JPEG XL, and for the rest the
readings Diffractor's and ImageWalker's tests record - but for these
differences. A difference not listed here is a fault.

**XMP**

- A two-digit hexadecimal reference such as `&#xA9;` is read as its character.
  The toolkit turns any such reference into a space, while meaning only to
  remove escaped control characters.
- A DOCTYPE is skipped, so entities it declares are undefined. Expat would
  expand them; no XMP writer is known to use one.

**Photos**

- XPKeywords are keywords separated by semicolons, each kept whole.
- IPTC's by-lines join the artist as whole names, each once and separated by
  "; ".
- A packet's property in another form than its schema gives reads as none, and
  the rest of the packet still reads.
- A rating Windows keeps as a percent - EXIF's RatingPercent, a Rating above 5,
  and the `WM/SharedUserRating` of ASF and an MP4's `Xtra` box - reads by
  Windows' one rule (`stars_from_percent`), whichever block holds it, and a
  negative RatingPercent reads as rejected.
- A TIFF scan reads the camera, exposure and dates in the file's own
  directories by the rules an embedded EXIF block is read by, and reports the
  IPTC block in tag 33723, where the writer keeps it, as `metadata.iptc`.
- A WebP's pixel format and metadata chunks are read whenever its header is.
  libwebp's mux refuses the whole file for a cut-short RIFF, an `ALPH`, `ICCP`,
  `EXIF` or `XMP ` chunk the VP8X flags do not declare, or a duplicated chunk.
  The first of each metadata chunk is taken.

**HEIF and JPEG XL**

- A HEIF file's movie box (`moov`), which holds an image sequence, is not read
  by a scan, which notes only that it is there; an update reads no more of it
  than its tracks' chunk offsets. libheif parses it whole and refuses the
  file over a damaged track; libfiles still reports the still image.
- A HEIF thumbnail is counted, not located, by a scan. It is a coded image of
  its own, which only a decoder reads: `decode` with `heif_thumbnail` decodes
  it.
- In a HEIF `meta` box, libfiles parses the boxes that locate the items and
  their metadata, the properties it uses or libheif checks, entity groups and
  data references, and refuses a file for any fault libheif refuses it for. It
  does not parse the properties of uncompressed (ISO/IEC 23001-17) images beyond
  `uncC` and `cmpd`, those of JPEG 2000 images, a region item's geometry, or a
  movie's box out of place in `meta`. libheif refuses a file for a fault in one,
  and follows a region's mask to an image it then counts as no longer top-level.
- A HEIF low-overhead file (`mini`), which libheif reads, is refused. Such a
  file names `mif3` as its major brand, which libfiles does not take for HEIF.
- A HEIF `meta` box of more than 64 MiB, or a file of more than 65,536 top-level
  boxes, is refused where libheif reads it, and a metadata item or colour
  profile of more than 16 MiB is left out, as the other scanners leave a block
  out. A JPEG item's frame header is looked for in its first 16 MiB only.
- A JPEG 2000 item's depth is not in the structure report: libheif reads it from
  the codestream, which libfiles does not read.
- A JPEG XL file's embedded ICC profile is not decoded; libfiles reads only its
  size. libjxl decodes it before it reads the boxes after the codestream, and
  stops there at one it cannot decode, where libfiles reads on. Custom primaries
  are not checked either: libjxl refuses a codestream it cannot make an ICC
  profile from, and libfiles checks only that its white point is in range.
- A JPEG XL codestream's headers are read from their first 8 MiB at most, and a
  codestream cut into so many parts that reading its headers again as each part
  arrives, as libjxl does, would read more than 64 MiB is refused. libjxl reads
  on through both.
- A JPEG XL's Brotli-compressed boxes decompress to at most 16 MiB each and
  32 MiB in all - enough for an Exif block and a packet of the largest size
  together - and the boxes stop at the first compressed one that is malformed,
  has trailing bytes or would pass that budget. A `scan_limits::brotli`
  callback replaces the built-in decoder.

**Audio and video**

- An ID3v2 tag before a FLAC stream does not make the file MPEG audio: every
  operation identifies it as FLAC, `read_packet` reads the tag's packet by the
  rule an MP3's is read by, and an update is refused. `read_packet` reads an
  AIFF's packet too.
- An ID3v2 tag that is damaged - an unknown version, a size past the file, a
  damaged extended header, a frame that overruns it - has no packet in a scan,
  as it has none in `read_packet`; an unsynchronised tag's packet is read by
  both.
- A compressed ID3 frame and a Matroska track's compressed codec header inflate
  within `scan_limits::media_inflated_bytes`; past it a frame is described
  rather than read. An Ogg file's capture patterns are checked against a bound
  of twice what the scan reads of the file's ends, so a file of patterns that
  are not pages cannot cost a CRC for every seven bytes.
- An MP4's user data box is found however many boxes come before it in the
  movie box.
- An audio or video stream is described from what its container and codec
  headers state, where FFmpeg also decodes frames. A stream's bit rate is the
  one the file states: an MP4 or AVI stream that states none has none, where
  FFmpeg works one out from the sample sizes or the index, and AAC in an MPEG
  stream has none, where FFmpeg estimates one from the frames it decoded. A
  stream's duration is its container's - an MP4 track's `mdhd`, not trimmed to
  its edit list. An ASF stream without extended stream properties has no frame
  rate, where FFmpeg estimates one from the packets. A pixel format comes from
  the codec configuration, so it is known for a file whose first video frame
  lies past FFmpeg's analysis window; FFV1, ProRes and uncompressed video in
  Matroska have none.
- An MP4's duration is its movie header's (`mvhd`), as QuickTime and Windows
  show it; FFmpeg's is the longest of its streams' after their edit lists, a few
  hundredths of a second shorter. An AVI's is its longest video stream's;
  FFmpeg's is its longest stream's.
- Cover pictures - an MP4's `covr`, a FLAC `PICTURE`, an ID3 `APIC`, a Matroska
  attachment that is an image - are listed in `pictures` and located; FFmpeg
  makes each an attached-picture video stream. A Matroska attachment that is not
  an image is not a picture.
- An Opus stream's duration leaves out its pre-skip (RFC 7845), and one whose
  first page starts late is timed from that start. A FLAC stream with no sample
  count is timed by its last frame, found in its last 64 KiB; FFmpeg gives it no
  duration. A chained Ogg file is described by its first link and given no
  duration, so Diffractor asks FFmpeg for it.
- APE, Lyrics3 and ID3v1 tags at the end of an MP3 are not counted as audio, and
  an Info header found past junk before the first frame is used; FFmpeg counts
  the tags and misses the header. Mono HE-AAC has one channel; FFmpeg reports
  the two its decoder makes.
- A Vorbis comment field that repeats gives one tag, its values joined with
  "; " as ID3v2.4's and APE's multiple values are; FFmpeg joins them with ";". A
  property that holds one value - a date, a track number - takes the first. An
  ID3 genre refined in the "(4)Eurodisco" form reads as "Eurodisco", where FFmpeg
  reads "Disco". Keywords split at commas and semicolons only. A tag that only
  describes binary data - "7 bytes", a picture's caption - fills no property.
- An ASF file's `WM/Category` descriptors are each a tag and a keyword; FFmpeg
  keeps the last. An ASF stream's language is the file's RFC 1766 tag (`en-us`),
  where FFmpeg gives ISO 639-2; for a stream the Language List gives none, both
  take the stream's own `WM/Language` record, if it is text. An ASF file's
  duration is kept when the file size its header states is wrong, where FFmpeg
  drops it. An ID3v2 tag kept in an ASF "ID3" descriptor is described, not read;
  FFmpeg reads its frames.
- An AVI's INFO lists are read before and after `movi`. Where both give a field,
  the one before wins, since that is the list the writer edits; FFmpeg reads the
  one after last.
- Every top-level `onMetaData` value of an FLV is a tag; FFmpeg keeps only the
  encoder.
- A Matroska TITLE tag with a TagLanguage is still the title, and the DURATION
  and BPS statistics tags give a track's duration and bit rate. A series tagged
  by level - `COLLECTION/TITLE`, `SEASON/PART_NUMBER` and `EPISODE/PART_NUMBER`,
  keyed under their TargetType as FFmpeg keys them - gives the show, season and
  episode, which FFmpeg files under no property.
- An MP4's `stik` atom gives `media_kind`: what iTunes says the recording is, so
  a reader can tell a film whose `tvsn` and `tves` are junk from an episode.
- An MPEG-4 Part 2 stream without a fixed frame rate has none, where FFmpeg
  reports its time resolution. HEVC whose profile says its source is interlaced
  is interlaced, as well as one with `field_seq_flag`. The SPS in an `hvcC` or
  `av1C` record wins over the record's own fields. A stated 1:1 pixel aspect is
  kept.
- An MPEG transport stream's first program is read; FFmpeg lists every
  program's streams. A DVD's navigation packets, in private stream 2, are not a
  stream of a program stream's; FFmpeg lists them as a data stream.
- An MPEG stream's video ends a frame after its last timestamp, at the frame
  rate its codec header states. FFmpeg adds one period of the rate it guesses,
  which for interlaced H.264 and for MPEG-1 video is the field rate, half a
  frame. E-AC-3's last frame is counted too, where FFmpeg counts none.
- An AVCHD recording's date, maker and model are read from its H.264 stream,
  which FFmpeg does not read. Daylight saving adds its hour to the zone the
  camera states. A zone byte of all zeros states none: it cannot be told from a
  camera that keeps no zone, and Panasonic-made cameras write it in summer too.
- MPEG audio whose frames a scan does not find is named `mp2`, the layer
  broadcasts and discs carry; FFmpeg's transport stream demuxer names it `mp3`
  until its parser reads a frame.

**Decoding**

- A PQ or HLG still decodes as SDR, tone mapped as [above](#how-it-decodes),
  where libpng, libheif and libjxl give the coded signal as it is - unless the
  caller keeps HDR, when it is that signal too.
- Developed raw pixels keep their highlights: a clipped channel is blended
  towards white - LibRaw's highlight mode 2, where `dcraw_make_mem_image` by
  default clips it - and light past white rolls off from 80% of white, where
  LibRaw clips it. Below that the pixels are LibRaw's own developer's in that
  mode.
- Developed raw pixels truncate LibRaw's brightness intermediates - the
  histogram percentile and the gamma white point - as LibRaw's own output
  conversion does, which can change a pixel at a fractional brightness
  boundary.
- Native PSD takes 8-bit samples and 1-bit bitmap mode, and rejects incomplete
  planes, other sample layouts and invalid PackBits rows rather than expose
  partly initialized pixels. A bitmap's pixels are initialized and its black and
  white palette is explicit.
- Native interlaced PNG keeps each row between passes, rather than combining a
  pass with another row's scratch data.
- Native WPG rejects incomplete raw rasters and row repeats that have no row
  before them; complete earlier rasters are kept, with a warning.
- Native TIFF marks valid unassociated strip alpha as BGRA; `legacy_tiff_storage`
  classifies it as BGRX instead, without changing the stored row bytes. A
  resolution that a signed 32-bit number of pixels per metre cannot hold takes
  MSVC's indefinite value, `0x80000000`, with a warning, rather than an undefined
  cast. Invalid metadata does not discard otherwise decodable pixels.
