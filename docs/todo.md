# To do

Open development tasks and their acceptance criteria. Keep completed work out
of this list; record lasting behaviour in [design.md](design.md).

## Codecs

- [ ] **Tests warning-free under GCC 15 without suppression.** GCC 15's
      `-Warray-bounds`, `-Wstringop-overflow` and `-Wrestrict` report false
      positives in the libstdc++ vector code four tests inline
      (`exif_tests.cpp`, `document_tests.cpp`, `encode_tests.cpp`,
      `jpeg_decode_tests.cpp`'s `decode_reference`), so `CMakeLists.txt` turns
      the three off for the test target on GCC 15 and later. Done when those
      tests are written so GCC 15 is quiet with the warnings on, in Debug and
      Release.
- [ ] **dav1d under GCC.** `cmake/dav1d/x86/config.h` tests `_M_AMD64`, so a
      GCC x86-64 build takes dav1d's 32-bit C paths beside its 64-bit
      assembly, and with `HAVE_UNISTD_H` 0 it cannot count processors and
      decodes on one thread. Done when GCC builds select `ARCH_X86_64` and use
      every core, with AV1 decode times measured before and after on the same
      fixtures.
- [ ] **libheif grid race, upstream.** libheif makes a grid's canvas from
      whichever tile's thread finishes first and pastes the others into its
      format, so a damaged grid whose tiles decode to different formats can
      decode differently from run to run. A local patch cost every grid up to
      one tile's decoding and no record has shown the race, so it was not
      kept. Report it to libheif; take a fix with a release.

## Reading

- [ ] **JPEG XL `brob` boxes.** libfiles builds Brotli, so the scanner could
      decompress compressed `Exif` and `xml ` boxes itself instead of asking
      the caller through `scan_limits::brotli`. This changes what a scan
      returns without a decompressor. Done when a scan reads them with none
      supplied, within the existing 32 MiB budget, and Diffractor drops its
      own decompressor.
- [ ] **Native TIFF grayscale strips.** The native document reader rejects
      valid MINISWHITE and MINISBLACK strips, as ImageWalker's reader did.
      Add grayscale and monochrome fixtures first, then accept them; done when
      ImageWalker's native and display paths agree on them.
- [ ] **Animated WebP.** `decode` refuses animated WebP. Add frame decoding
      when a consumer shows animation (ImageWalker 3.0 plans animated GIF and
      WebP), with canvas size, frame offsets, blending and delays tested.

## Listing

- [ ] **Raw listing format quirks.** Two behaviours were kept as they were
      when the raw listing moved here. `{:0.1}` keeps one significant digit,
      so 100 mm lists as "1e+02 mm"; and a value that has a name - a Fuji film
      mode, a white-balance preset - lists without the name. Done when both
      list as a person reads them, with Diffractor's raw record updated in the
      same change.
