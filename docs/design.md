# libfiles design

How libfiles reads, writes and decodes files, and where it deliberately differs
from the code it replaced. [README.md](../README.md) has the overview and the
API at a glance; the headers in `include/libfiles/` own the exact contracts.

Three rules shape everything here:

- **Reading is the hot path.** Diffractor spends most of its time indexing, so
  a scan reads as little of a file as its format allows - a few block reads
  over a network share - and locates rather than reads what a caller may not
  want.
- **What a file reads as does not change.** libfiles replaced Adobe's XMP
  Toolkit and the readers in Diffractor and ImageWalker, so reading matches
  them unless a difference is deliberate, and every deliberate difference is
  listed below. Each move was held to a record of the old code's outcomes over
  real fixtures, whole, cut short and corrupted.
- **A writer never leaves a partial file.** A bounded edit is patched in place,
  ordered to survive interruption; anything larger is a one-pass rewrite to a
  destination the caller has staged.

## Codecs

Codecs are optional, and fetched rather than vendored: each is pinned to its
release archive's SHA-256 in [cmake/dd-dependencies.json](../cmake/dd-dependencies.json) -
skcms, which makes no releases, to its commit - and built from the untouched
source by libfiles' own recipe. With
`LIBFILES_JPEG` on, the default, libfiles builds libjpeg-turbo 3.2.0 - at 8 bits
with its NASM SIMD on x64 and x86, and its 12- and 16-bit libraries - and a
consumer links it as `libfiles::jpeg`. NASM is found on the path, or on Windows
fetched by SHA-256; a consumer that has enabled `ASM_NASM` lends its own.
Without NASM, and on ARM64, the C paths are built, which decode the same pixels
more slowly.

With `LIBFILES_PNG` on, also the default, libfiles builds libpng 1.6.59 and
zlib-ng 2.3.3, built zlib-compatible, and a consumer links them as
`libfiles::png` and `libfiles::zlib` - so an application that takes them has one
zlib, not a second beside its own. zlib-ng picks its x86 SIMD at run time on x64
and x86, and runs its C elsewhere; libpng is built in C alone. The headers their
own builds would generate - `zlib.h`, `zconf.h`, `zlib_name_mangling.h` and
`pnglibconf.h` - are kept in `cmake/`.

With `LIBFILES_WEBP` on, also the default, libfiles builds libwebp 1.6.0 - its
decoder, encoder, sharpyuv, mux and demux, as one library - and a consumer
links it as `libfiles::webp`, including its headers as `webp/decode.h` and the
rest. libwebp picks its SIMD from what the compiler announces and dispatches at
run time: SSE2, SSE4.1 and AVX2 with MSVC on x64 and x86, NEON on ARM64, and
the baseline SSE2 with GCC on x86-64.

With `LIBFILES_HEIF` on, also the default, libfiles builds libheif 1.23.5 with its
JPEG, libde265, dav1d and uncompressed decoders, and libde265 1.1.3 and dav1d
1.5.4 beside it, as `libfiles::heif`, `libfiles::de265` and `libfiles::dav1d`.
dav1d's x86 assembly is built where NASM is, with the configuration in
`cmake/dav1d/`; elsewhere, ARM64 among them, it and libde265 run their C. Under
GCC, dav1d's carried `config.h` takes its 32-bit C paths beside the 64-bit
assembly and decodes on one thread (see [todo.md](todo.md)). libheif's FFmpeg
decoder is not built - libfiles does not take on FFmpeg - so an application
that has FFmpeg builds `plugins/decoder_ffmpeg.cc` from the fetched libheif
(`FetchContent_GetProperties(libheif)`) and registers it with
`heif_register_decoder_plugin` before it decodes, as Diffractor does.

With `LIBFILES_JXL` on, also the default, libfiles builds libjxl 0.12.0 - its
decoder, encoder, colour management and thread runners, without the extras its
tools use - with Highway 1.4.0, Brotli 1.2.0 and skcms beside it, as
`libfiles::jxl`, `libfiles::highway`, `libfiles::brotli` and `libfiles::skcms`.
skcms is cloned at its pinned commit: Skia's generated archives differ from one
download to the next, so no archive's SHA-256 can pin it. The headers libjxl's
own build would generate - `jxl/version.h` and its export headers - are kept in
`cmake/jxl/include/`. libjxl and Highway are optimised like the rest: the MSVC
miscompile that once decoded a JPEG XL to an empty image does not occur with
these versions and MSVC 14.51.

With `LIBFILES_RAW` on, also the default, libfiles builds LibRaw 0.22.2 with
Adobe's DNG SDK 1.7.1 beside it, as `libfiles::raw` and `libfiles::dng`: given a
`dng_host`, LibRaw hands a DNG it cannot read alone - lossy, or JPEG XL - to the
SDK. The SDK's own XMP backend is written over the XMP Toolkit, so libfiles
builds its own in its place, [src/dng_xmp_sdk.cpp](../src/dng_xmp_sdk.cpp), over
its XMP data model. The SDK's `RawEnvironment.h`, which it leaves to the
application, is in `cmake/dng/`. Adobe's archive is 80 MB, its copies of
libjpeg, libjxl and the toolkit not built.

With `LIBFILES_GIF` and `LIBFILES_TIFF` on, also the defaults, libfiles builds
giflib 5.2.2 and libtiff 4.7.0 from their pinned archives, with ImageWalker's
codec settings. TIFF uses the same zlib-ng; JPEG-compressed TIFF and its
optional external codecs are disabled. Consumers that do not use native
GIF/TIFF documents can turn these options off.


## How it reads

A `reader` reads its stream through one block-aligned window, 64 KB by default,
keeping what a moved window still overlaps and sending large reads straight to
the caller's buffer, and it counts the reads it issues. Scanners touch only the
structure they need: JPEG's segments up to the first scan, PNG's and WebP's
chunk headers - never their image data - and TIFF's first two directories.
Embedded thumbnails are located, not read: a caller that wants one reads it.

A HEIF file - HEIC, AVIF, or any other ISO base media still image - is read
from its file type box, the headers of its top-level boxes and its `meta` box,
which says what each item is, where its data lies (`iloc`: in the file or in
`idat`, in one extent or several) and which properties and references it has.
Of the items' data only the metadata items' is read - Exif, XMP, IPTC - and a
JPEG item's frame header; a coded image never is. The size is the primary
item's after its clean aperture and rotation, and `container_transform` says
that an `irot` or `imir` property turns the image, so that the EXIF orientation
it carries must not be applied again. The pixel format is the chroma libheif
decodes to: from `hvcC` or `av1C`, a grid's from its first tile. A thumbnail is
counted for the structure report but not located, since it is a coded image a
caller would have to decode. A file whose `meta` box follows the media data
costs a read at each end.

A JPEG XL file is read as libjxl reads it for its basic information and boxes:
the codestream's size header and image metadata - from a bare codestream, the
container's `jxlc` box, or `jxlp` parts with other boxes between them - and the
`Exif` and `xml ` boxes. The width and height are the codestream's, unturned,
and the orientation is the codestream's own unless an EXIF block states one.
A container may keep its metadata Brotli-compressed in `brob` boxes. libfiles
carries no Brotli decoder, so such a box is left out unless the caller supplies
one in `scan_limits::brotli`, as Diffractor does with its own.

An EXIF block is read by walking IFD0 and the Exif, GPS and Interoperability
directories it points to; a directory is read once however many pointers lead
to it, and no more than 32 in all. An entry counts only if it is well formed.
Text is decoded however a camera or Windows wrote it - ASCII, UTF-8, a
UserComment marked `UNICODE`, the UTF-16 of an unmarked tag, the UTF-16LE
Windows stores as bytes in its XP tags - and text that is binary reads as empty. This is Diffractor's EXIF parser, moved and held to what
it read by a comparison over every EXIF block Diffractor's fixtures carry,
whole, cut short and corrupted.

Every TIFF directory libfiles reads for what it says, it reads with that
parser's entries: an EXIF block for `read_exif` and `summarize_exif`, and a
TIFF file's own directories. One rule gives the orientation - the last
Orientation in IFD0, else the last in IFD1, which describes the thumbnail - so
a scan, a decode and a reading of the same block cannot disagree, and a copy of
the tag in any other directory is not the image's. A TIFF file's camera,
exposure, dates and GPS are read by that parser into `scan_result::tiff_metadata`,
without copying the file's pixels into an EXIF block. Individual metadata
values above 64 KiB are ignored; XMP retains its separate 8 MiB limit. A damaged
metadata directory costs its fields, not an otherwise readable image.

An inspector's listing of a block - `list_exif` - names every entry of every
directory, renders its value as text and decodes the maker note of a Canon,
Casio, Fuji, Nikon, Olympus, Pentax or Sanyo camera, as libexif listed them:
the same entries from a damaged block, the same names, the same words. The
block is read by libfiles' TIFF reader, with libexif's rules for what a listing
holds - DOUBLE entries and unknown tags included, directories within the first
65,534 bytes. The names, words and maker note decoders are libexif's own code,
ported by [tools/port_libexif.py](../tools/port_libexif.py) into `src/libexif_*`
and kept close to it, so that a fix to libexif can be carried over.

An IPTC-IIM block - Photoshop's resource 0x0404, a TIFF's tag 33723 - is read
by `read_iptc` for the fields an organizer files - keywords and by-lines in
order, the caption, title, place, credit, source and copyright, and the dates
and times of its creation as stored - and listed by `list_iptc`, every dataset
but the binary ones under its IIM title. The walk follows IIM 4.1's extended
lengths and stops at a header it cannot trust; text is UTF-8 where it is valid
and Windows-1252 where it is not, as the XMP parser reads 8-bit text. The
dataset table is libiptcdata's.

Photoshop's image resources - a PSD's resource section, a JPEG's APP13 - are
read by one rule in the scans, the writers and the JPEG encoder: a resource's
data lies inside the block, and the pad byte after the last one's may be
missing, as some writers leave it. Every resource is written with its pad byte.
Of a kind a file repeats - IPTC, XMP, EXIF or the colour profile - the first
resource that holds anything is the one a scan reads and an edit changes.

A camera raw file is read by LibRaw, by its path, with `scan_raw`, and nothing
is developed: the size and orientation, the XMP packet, the camera and its
exposure, the date its clock gave - LibRaw's timestamp taken back to the time
of day it was made from - and a GPS position, as an index wants them; for an
inspector, everything LibRaw parsed, listed by section, each maker's own fields
among them; and for a caller that gives a sink, the largest thumbnail a decoder
reads, a JPEG one as its bytes and a bitmap one written. Each thread scans with
an instance of LibRaw it keeps, as constructing one zero-fills half a megabyte.
The listing is Diffractor's, moved with its formats - `{:0.1}` keeps one
significant digit - and the scan was compared with Diffractor's, whole, cut
short and corrupted, over its CR2 fixtures and the DNG SDK's samples, with no
difference.

In HEIF and JPEG XL, an Exif block begins with the offset of its TIFF header. Everything
before the header is removed, and then the JPEG `Exif` signature some writers
put there, so that `metadata.exif` starts at the byte-order mark, as for every
other format.

The scanners replace Diffractor's, and report the same for every file. Diffractor
proves it with a test that runs both over its fixtures and a wider local corpus,
whole, cut short and with bytes corrupted. PNG keeps the rules of the libpng
1.6.59 header read and the chunk walk it replaced, which libfiles now does in one
pass. HEIF and JPEG XL follow libheif 1.23.5 and libjxl as Diffractor builds
them, down to which damaged files they refuse; a local harness compared the two
field by field, the structure report included, over Diffractor's fixtures, about
250 public conformance and sample files, files written with libjxl's encoder and
the suite's own files, each whole, cut short and with bytes corrupted.

## How it writes

An update is planned from a structural read of the file: a sorted list of source
byte ranges, each replaced by new bytes, with everything between them kept as it
is. A plan that moves nothing the file holds - every replacement keeps its
length, apart from what is added at the very end - can be patched where the file
lies (`write_in_place`); any other is written in one pass from the source to a
destination the caller has staged (`write_updated`), so a source is never left
half rewritten. Patching writes what is appended first, then the patches in the
steps the planner orders them in, flushing after each, so that wherever it is
interrupted the file reads whole; the writes within a step may land in any
order, and each is taken to land whole. Every format's patch is replayed over
each state an interruption could leave. A packet is first serialized at the old
packet's exact length, as the toolkit did, so that an edit which fits is a patch,
and a rewrite that keeps its length is written as the one span of it that
changes: a rating in a packet that keeps its length is a byte or two.

- **PNG**: an uncompressed `iTXt` keyed `XML:com.adobe.xmp`. A rewrite removes
  every XMP text chunk, in whatever form, and puts one after `IHDR`.
- **GIF**: the `XMP DataXMP` extension with its 258-byte magic trailer, before
  the GIF trailer. GIF87a cannot carry one and is refused, as the toolkit did.
- **WebP**: the `XMP ` chunk, replaced where it stands or appended, with every
  other chunk kept in place. A simple file gains the VP8X header it needs, its
  canvas and alpha read from the image itself. The packet keeps its wrapper and
  padding, where the toolkit wrote none, so that later edits can be patches. A
  first packet appended at the end of the file is declared in VP8X only once the
  RIFF size takes it in. An `EXIF` chunk the file already has takes the edit's
  EXIF fields.
- **JPEG**: the packet's APP1, EXIF's APP1 and the IPTC in APP13's Photoshop
  resources, with its MD5 digest beside it. Segments the file lacks are added
  after its leading APP0 and APP1 segments. Extended XMP is left as it is.
- **TIFF**: by appending. A packet that fits is written over the old one;
  anything else is appended at the end of the file and its entry repointed, so
  the image data never moves. A DNG carries no IPTC.
- **PSD**: the image resources section is rebuilt, unless only a packet that
  fits changes, which is then a patch.
- **MP4 and QuickTime**: the packet in a top-level XMP `uuid` box in an ISO
  file and in `moov/udta/XMP_` in a QuickTime one; tags and rating in the `Xtra`
  box Windows reads, a copyright in `cprt` when the movie has user data, and a
  created date in `mvhd`. Nothing the file holds moves, so chunk offsets stay
  valid and the media is never copied. A packet that outgrows its box is
  appended at the end of the file, and the old box becomes `free`. The movie's
  user data grows into the free space it holds; when it cannot, the movie box is
  appended with 4 KB of room and the old one freed. The packet's box always
  follows the movie box, since FFmpeg reads no `uuid` box before it, so it moves
  with a movie box that moves. A last box that runs to the end of the file is
  closed before anything appended is made part of the file. A CR3 raw, in the
  same container, is not an MP4 to libfiles.
  Of several XMP `uuid` boxes the last is read, as FFmpeg reads it, and of
  several movie boxes the first; an update frees the others, which only an
  interrupted update leaves.
- **ASF** (WMV, WMA): the packet in a top-level XMP object after the data;
  title, author, copyright and description in the header's Content
  Description, tags and rating in the WM/Category and WM/SharedUserRating
  descriptors, a created date in the File Properties, whose file size is kept
  true. A packet that outgrows its object is appended and the old object
  becomes padding. The header is rewritten where it lies, in one write, when
  its padding - in the header or its extension - takes the change; otherwise
  the file is rewritten in one pass with 2 KB of padding in its header for the
  next edit. The first XMP object after the data is read, as FFmpeg reads it.
- **RIFF** (WAV, AVI): the packet in a `_PMX` chunk at the top of the RIFF
  form; title, artist, comment, copyright, date and genre in LIST INFO, a WAV's
  title also in an existing DISP chunk, and an AVI's original date in an
  existing IDIT. Nothing that holds media moves, so an AVI's index offsets stay
  true. A chunk that fits is patched; one that grows takes the JUNK chunk after
  it - written inside the JUNK, then made the chunk read by one 8-byte header
  write - or a free JUNK chunk, or is appended at the end of the form, which
  the form's size then takes in before the old chunk becomes JUNK. FFmpeg reads
  an AVI's INFO only before `movi` and its packet only after, so they are kept
  there; an AVI that continues in AVIX forms cannot grow its first form, and
  takes only what fits.
- **MPEG audio** (MP3, MP2, MPA): the packet in an ID3v2 `PRIV` frame owned by
  `XMP`; title, artist, album, album artist, genre, comment, track, disc,
  copyright, its URL and the date in their text frames, and the rating in the
  `POPM` frame Windows Media Player reads. The tag keeps its version, and every
  frame the edit does not own is kept byte for byte. When the frames fit the
  tag's space it is patched where it lies; otherwise the file is rewritten in
  one pass with 2 KB of padding.
- **FLV**: the packet as the `liveXML` string of an `onXMPData` script tag after
  `onMetaData`. A packet that fits is patched; otherwise the file is rewritten
  in one pass, and the keyframe file positions and file size `onMetaData`
  keeps are moved with the media.

Outside the packet, an edit is semantic (`legacy_edits`): each field it sets is
written to every place the format keeps it - a title to IPTC's Object Name, a
description to EXIF's ImageDescription and IPTC's Caption, tags to IPTC's
Keywords and Windows' XPKeywords, a rating to EXIF's Rating and RatingPercent,
dates, GPS and the orientation to EXIF. Nothing the edit does not set is
rewritten. EXIF is edited without moving a byte it holds: a changed value that
fits is written where it lies, and anything else is appended to the block and
repointed, so a maker note's absolute offsets and the thumbnail stay valid. A
value fits when it is the same size, or shorter text, which the zeros after it
end; a shorter value of any other type is appended, since the old entry's count
would read the zeros as part of it. Patched, a value is written before the entry
that names it and an appended directory before the pointer to it, so an
interruption between them leaves the old entry over the new value.
IPTC is written as UTF-8 and says so. Each dataset's header in the block is
read by the reader's rule, every dataset is kept - empty ones, and ones longer
than the reader takes, too - and its text is carried over as `read_iptc` reads
it.

Three EXIF writers stand alone, for blocks an application handles itself.
`make_exif` writes a fresh block for a picture whose file carries none - one
re-encoded from a format without EXIF - laid out byte for byte as libexif lays
out a new block, its mandatory entries included, so a block an application
wrote through libexif comes out the same. `set_exif_numbers` overwrites numbers
a block already holds where they stand, for a picture turned without being
re-encoded: the orientation goes upright and the dimensions follow, and nothing
else in the block changes or moves, maker notes included. `unlink_exif_thumbnail`
unlinks IFD1 where it stands, for a block carried into a re-encoded picture
whose thumbnail would show the old pixels.

Where libfiles writes differently from the toolkit, on purpose:

- The packet keeps the `exif:` and `tiff:` properties it holds. The toolkit
  deleted them all after copying them into EXIF, and with them anything its
  copy did not cover.
- Legacy metadata is not imported into the packet before a write, nor exported
  back from all of it after: only the fields the edit sets are written.
- IPTC is created when an edit writes a field it holds; EXIF is created when
  an edit writes a field EXIF holds. The toolkit did the same, but also wrote
  every field it had imported.
- An MP4 or QuickTime movie box that outgrows its space moves to the end of the
  file with room to grow. The toolkit rewrote it where it lay when free space
  followed it, which an interruption could leave half written.
- A QuickTime movie's created date is written to `mvhd`, as an ISO file's is,
  and its packet keeps `xmp:CreateDate`. The toolkit wrote `mvhd` for ISO files
  only, and removed `xmp:CreateDate` from a QuickTime packet.
- An ASF header is patched where it lies whenever its padding takes the change.
  Diffractor's fork of the toolkit rewrote the whole file whenever a packet held
  tags or a rating. The header's copyright and licence URLs are not written,
  since neither FFmpeg nor Windows reads them. A packet's `asf:NativeDigest` is
  left as it is: a toolkit that finds it stale imports the header's fields,
  which libfiles wrote from the same edit.
- A RIFF or ID3 field the packet lacks is left as it is. The toolkit exported
  every field it maps from the packet, deleting each one the packet did not
  hold, and wrote a new DISP chunk and ID3v1 tag where the file had none;
  libfiles writes only the fields the edit sets, and leaves ID3v1 alone.
- An AVI's packet stays in its first form, after `movi`, where FFmpeg reads it.
  The toolkit moved it to the end of the last form, which in a file that
  continues in AVIX forms no FFmpeg reader saw, and could move `movi` itself.
- ID3 text is written without a terminator, and a genre as its name rather than
  a numbered ID3v1 code. A rating of no stars removes Windows Media Player's
  `POPM` frame rather than writing a zero.
- MP2 and MPA files take an ID3v2 tag, as MP3 files do. The toolkit wrote their
  packet to a sidecar `.xmp` file instead.
- A rewritten FLV's `onMetaData` keyframe index and file size follow the media.
  The toolkit left them pointing where the media had been.

## How it decodes

A decoder writes into memory its caller provides (`image_sink`), so an image
lands in the caller's own surface - Diffractor's texture staging memory, a
bitmap - with nothing copied between. The sink is asked once, as soon as the
file has given the size and before the pixels are read, so a caller refuses an
image it cannot afford before any of it is decoded. A HEIF's codec, not its
container, gives the size it decodes to, so the sink first accepts or refuses
the size the file declares (`accept`), and then allocates the size decoded;
LibRaw builds a raw file's whole frame in its own memory before a row can be
written, so a development is accepted or refused the same way before it starts.
The decoder writes every byte of each row's pixels and nothing in a row's
padding. The result says whether the file was malformed, cut short
(`truncated`), or refused.

A decoder that can reduce while it decodes does so for the size the caller
will show the image at: the largest of 8, 4 and 2 that leaves the source at
least that large. Where the caller takes NV12, a YCbCr JPEG is written as NV12
without colour conversion, for a GPU to convert. A `decoder` keeps what the
codecs need between images, so a stream of thumbnails does not set libjpeg up
for each.

- **JPEG**, with libjpeg-turbo (`LIBFILES_JPEG`): baseline, extended and
  progressive, and lossless up to 16 bits, in YCbCr, RGB, grayscale, CMYK and
  YCCK, as B8G8R8X8 or NV12; high precision scaled to 8 bits, CMYK multiplied
  out, inverted unless an Adobe marker says it is stored inverted. The frame
  carries the last EXIF block's orientation, read as `summarize_exif` reads
  one - and, while the sink allocates, the block itself, for a caller that
  reads it its own way - and, for a progressive or non-interleaved file, the
  memory its coefficients take, which libjpeg holds whole at full size before
  the first row: a reduction does not reduce it. A file cut short decodes as
  far as its data reaches, the rest as libjpeg makes of the end of image put
  in its place - except a progressive one, which has no row until every scan
  is in. Moved from Diffractor's decoder call for call, and held to it byte for
  byte before Diffractor switched.
- **PSD**: the composite image Photoshop keeps after the layers, in bitmap,
  grayscale, indexed, RGB, CMYK, multichannel, duotone and Lab modes, at 8 bits
  a channel or 1 in bitmap mode, raw or PackBits, as B8G8R8X8. Moved from
  Diffractor's loader unchanged, byte for byte - what each mode leaves in the
  fourth byte included - and held to it, whole, cut short and corrupted, before
  Diffractor switched to it.
- **PNG**, with libpng (`LIBFILES_PNG`): every colour type and bit depth as
  B8G8R8A8 with straight alpha - 16 bits scaled to 8, low bit depths and
  palettes expanded, tRNS made alpha, opaque where the file has none - and a
  file's own gamma corrected for an sRGB display when it gives one and no sRGB
  chunk. The frame carries the orientation of the last EXIF in the file, which
  may follow the image data, read as `summarize_exif` reads one. An interlaced
  file decodes whole. What libpng warns of - a chunk with a bad CRC, skipped -
  comes back in `warnings`, even when the decode then fails; a file cut short
  is not decoded. Moved from Diffractor's loader call for call.
- **WebP**, with libwebp (`LIBFILES_WEBP`): a still image, lossy or lossless, as
  B8G8R8A8 with straight alpha where it has alpha and B8G8R8X8 where it has
  none - libwebp writes 255 in the fourth byte - reduced by libwebp's scaler
  while it decodes to the size `scaled_size_for` gives for the target, which is
  how Diffractor sizes an image to fit. An opaque lossy image the caller takes
  as NV12, and does not reduce, comes as VP8's own limited-range YCbCr
  (`limited_range`), an odd edge cropped. The frame carries the orientation of
  the first EXIF chunk, found through libwebp's mux and read as
  `summarize_exif` reads one, with or without a JPEG `Exif` signature leading
  it. An animated WebP is not decoded, nor a file cut short; a decode is one
  call, which the sink cannot cancel. Moved from Diffractor's loader unchanged.
- **HEIF and AVIF**, with libheif (`LIBFILES_HEIF`): the primary image, or its
  first thumbnail item (`heif_thumbnail`), as B8G8R8A8 with straight alpha where
  it has alpha - premultiplied alpha made straight - and B8G8R8X8 where it has
  none. libheif turns the image by its item's own rotation and mirror, and the
  frame says so (`transformed`); otherwise it carries the primary's EXIF
  orientation. An HEVC image - an `hvc1` item, or a grid, overlay or identity
  image of them - goes to a decoder plugin the caller names and has registered
  (`heif_hevc_decoder`), as Diffractor sends it to FFmpeg's. The sink accepts or
  refuses the size the item declares before libheif decodes; a malformed file
  may declare none, or one other than its codec decodes - a damaged `clap` crop
  - and the frame allocated is the size decoded. A file cut short within its
  image data decodes as libheif decodes it. Moved from Diffractor's loader
  unchanged, and compared with it - its outcomes, every pixel, and the size its
  budget was judged on, whole, cut short and corrupted, over libheif's examples
  and fuzzing corpus and generated files - with no difference.
- **JPEG XL**, with libjxl (`LIBFILES_JXL`): the first frame, as B8G8R8A8 with
  straight alpha where it has alpha - premultiplied alpha made straight by
  libjxl - and B8G8R8X8 where it has none, libjxl writing 255 in the fourth
  byte. The pixels are left as the codestream stores them and its orientation
  reported, so the size decoded is the size a scan reports; an Exif box's
  orientation is not read. libjxl writes RGBA straight into the caller's rows,
  and red and blue are swapped there. One thread decodes. A file cut short is
  not decoded: its input closed, libjxl does not tell a file cut short from a
  damaged one. Moved from Diffractor's loader unchanged, and compared with it -
  its outcomes, every byte of every row, and the size its budget judged, whole,
  cut short and corrupted, over libjxl's conformance files and Diffractor's
  fixture and generated files - with no difference.
- **Camera raw**, with LibRaw and the DNG SDK (`LIBFILES_RAW`), through
  `load_raw` in [raw.h](../include/libfiles/raw.h), which takes the file's path: a
  raw format has no signature to dispatch on, and LibRaw opens a companion file
  a format keeps beside it. Asked for a preview, it takes the largest embedded
  one a decoder reads - a CR3's HEVC preview passed over for its JPEG - and
  hands a JPEG back as the file holds it, for the caller to decode as it decodes
  any image, once a scan finds it an image of some size; a bitmap preview it
  writes to the sink as B8G8R8X8. Otherwise, or without a preview, it develops
  the raw data with the camera's white balance and LibRaw's auto-brightness, as
  `dcraw_make_mem_image` would, but straight into the caller's rows, as
  B8G8R8X8, left unturned. A preview keeps its own orientation where, turned by
  it, it is the raw image's shape as the raw data turns that; otherwise - a
  preview stored as the sensor saw it - it takes the raw data's
  (`raw_preview_orientation`). Moved from Diffractor's loader unchanged.
- **Commodore 64 images**, which hold no picture of their own: `list_c64_media`
  lists a D64, D71 or D81 disk's directory, a T64 tape's, a CRT cartridge by its
  name, or a P00 or PRG program, as the C64 prints a directory - at most 512
  rows, however long a damaged directory's chain - and `draw_c64_listing` draws
  that listing in the C64's upper-case character set, 8 by 8 pixels a
  character, as B8G8R8X8. Moved from Diffractor unchanged, and compared with it
  - every row and every pixel, over files whole, cut short and corrupted, and
  thousands of generated disks, tapes, cartridges and programs - with no
  difference.

### Native documents

[document.h](../include/libfiles/document.h) serves image editors that must keep
packed indices, palettes, grayscale storage, frame offsets, transparency,
disposal and delays rather than flatten everything to BGRA. `decode_document`
reads a named format through the same positional `reader`. Its `document_sink`
admits each frame, receives top-down, four-byte-aligned rows, and receives
borrowed metadata blocks. `metadata_complete` ends each metadata group; the
application decides how to present or retain it. The sink owns the output and
its total memory budget. `maximum_working_bytes` bounds decoder scratch
rasters and strips separately; it is not a process-wide memory limit.

JPEG, PNG and PSD share their codec/parser implementation with the display
path while preserving the native sample policy: grayscale/BGR JPEG with its
original DCT settings, PNG without display gamma conversion and with packed
palettes, and PSD with its native colour/storage conventions. GIF and TIFF
keep complete frames/pages before a damaged later record when their legacy
reader did. PCX and WPG read through bounded cursors; a WPG's first raster
streams directly and subsequent rasters are staged until complete.

ImageWalker's adapter keeps statistics, its metadata presentation, encoded
blobs needed for lossless saves and its orientation policy. Memory streams
lend their bytes instead of being copied through another stream. Windows
codecs, compressed BMP/DIB rendering, metafiles and OLE previews remain with
the host. Native encoders in `libiw` are not moved by this reader migration.
Inherited native limits remain, including TIFF's rejected grayscale-strip
path and disabled JPEG compression; TIFF tiles and its RGBA fallback work as
before. Native PSD accepts 8-bit samples and 1-bit bitmap mode.

ImageWalker's retained tests include a pre-migration record of 1,135 whole,
cut and corrupted inputs, checking native storage, pixels and metadata.
Incomplete PSD planes are tested separately: the old reader returned
uninitialized pixels, so those bytes cannot be a valid parity record.

`probe_webp_header` reuses the scanner's header parser without reading image
data. WebP display decoding remains in `decode`; `round_webp_size_down` keeps
ImageWalker's thumbnail rounding while the default preserves Diffractor's.
An explicit `source_format` also admits a codec's bare bitstream without
changing signature-based dispatch.

## How it behaves

The data model follows the XMP Toolkit, because what an application reads from
a packet must not change when the toolkit is replaced:

- **Recovery.** A malformed packet keeps whatever precedes its first fault, as
  the toolkit does with Expat. Invalid UTF-8 is read as Windows-1252, and ASCII
  controls become spaces.
- **Normalization.** Dublin Core arrays written as simple values become arrays;
  the toolkit's standard aliases (`tiff:Artist` is `dc:creator[1]`,
  `exif:DateTimeDigitized` is `xmp:CreateDate`, and the rest) are moved to their
  actual properties; `exif:UserComment`, `xmpDM:copyright`, GPS time stamps and
  broken alt-text arrays are repaired the same way.
- **The API.** Paths, localized-text selection, array rules, iteration order and
  the namespace table, with its numbered prefixes, are the toolkit's.
- **Serialization.** The toolkit's layouts, wrapper and padding, so a packet can
  be rewritten where it lies.

Where libfiles differs on purpose:

- Native PSD rejects incomplete planes, unsupported sample layouts and invalid
  PackBits rows rather than exposing partially initialized pixels. Bitmap
  pixels are initialized and their black/white palette is explicit.
- Native interlaced PNG retains each row between passes instead of combining
  a pass with another row's scratch data. Native WPG rejects incomplete raw
  rasters and row repeats that have no preceding row; complete earlier
  rasters are retained with a warning.
- Native TIFF makes the old Windows signed-resolution conversion explicit,
  including its indefinite value, rather than relying on undefined casts.
  Invalid metadata does not discard otherwise decodable pixels.
- A TIFF scan includes the camera, exposure and dates in its own directories,
  rather than only its orientation, thumbnail and GPS. Consumers can use these
  dates in the same resolution rules as an embedded EXIF block.
- A two-digit hexadecimal reference such as `&#xA9;` is read as its character.
  The toolkit turns any such reference into a space, while meaning only to remove
  escaped control characters.
- A DOCTYPE is skipped, so entities it declares are undefined. Expat would
  expand them; no XMP writer is known to use one.
- A WebP's pixel format and metadata chunks are read whenever its header is.
  Diffractor read them through libwebp's mux, which refuses the whole file for a
  cut-short RIFF, an `ALPH`, `ICCP`, `EXIF` or `XMP ` chunk the VP8X flags do not
  declare, or a duplicated chunk - and so reported no pixel format or metadata
  for a file it could otherwise size. The first of each metadata chunk is taken.
- A HEIF file's movie box (`moov`), which holds an image sequence, is not read.
  libheif parses it whole and refuses the file over a damaged track; libfiles
  still reports the still image.
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
  file names `mif3` as its major brand, which neither Diffractor nor libfiles
  takes for HEIF.
- A HEIF `meta` box of more than 64 MiB, or a file of more than 65,536 top-level
  boxes, is refused where libheif reads it, and a metadata item or colour profile
  of more than 16 MiB is left out, as the other scanners leave a block out. A
  JPEG item's frame header is looked for in its first 16 MiB only.
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
- A JPEG XL scan lets the caller's Brotli decompressor produce 32 MiB in all,
  twice the 16 MiB it keeps of one block: enough for an Exif block and a packet
  of the largest size together. Its boxes stop at the first compressed box that
  would take it past that, as at a box too large to keep. Diffractor let libjxl
  decompress every box, each to 16 MiB.

