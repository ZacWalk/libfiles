cmake_minimum_required(VERSION 3.21)

project(libfiles LANGUAGES CXX VERSION 0.1.0)

# The library is plain C++20 over the standard library, so it builds wherever its consumers do -
# Windows, Linux, x64, x86 and ARM64 - and leaves the CRT choice to them. Its codecs are optional:
# each is fetched at a pinned SHA-256 and built by libfiles' own recipe in cmake/.
option(LIBFILES_JPEG "Build libjpeg-turbo, for JPEG decoding and for consumers that link libfiles::jpeg" ON)
option(LIBFILES_PNG "Build libpng and zlib-ng, for PNG decoding and for consumers that link libfiles::png and libfiles::zlib" ON)
option(LIBFILES_WEBP "Build libwebp, for WebP decoding and for consumers that link libfiles::webp" ON)
option(LIBFILES_HEIF "Build libheif with libde265 and dav1d, for HEIF decoding and for consumers that link libfiles::heif" ON)
option(LIBFILES_JXL "Build libjxl with Highway, Brotli and skcms, for JPEG XL and for consumers that link libfiles::jxl" ON)
option(LIBFILES_RAW "Build LibRaw with Adobe's DNG SDK, for consumers that link libfiles::raw and libfiles::dng" ON)
option(LIBFILES_GIF "Build giflib for native animated GIF documents" ON)
option(LIBFILES_TIFF "Build libtiff for native multi-page TIFF documents" ON)

if(LIBFILES_JPEG)
    include(cmake/jpeg.cmake)
endif()

if(LIBFILES_PNG)
    include(cmake/png.cmake)
endif()

if(LIBFILES_WEBP)
    include(cmake/webp.cmake)
endif()

if(LIBFILES_HEIF)
    include(cmake/heif.cmake)
endif()

if(LIBFILES_JXL)
    include(cmake/jxl.cmake)
endif()

if(LIBFILES_RAW)
    include(cmake/raw.cmake)
endif()

if(LIBFILES_GIF)
    include(cmake/gif.cmake)
endif()

if(LIBFILES_TIFF)
    include(cmake/tiff.cmake)
endif()

add_library(libfiles STATIC
    include/libfiles/commodore.h
    include/libfiles/decode.h
    include/libfiles/document.h
    include/libfiles/encode.h
    include/libfiles/exif.h
    include/libfiles/exif_listing.h
    include/libfiles/icc.h
    include/libfiles/identify.h
    include/libfiles/iptc.h
    include/libfiles/media.h
    include/libfiles/properties.h
    include/libfiles/reader.h
    include/libfiles/scan.h
    include/libfiles/stream.h
    include/libfiles/types.h
    include/libfiles/write.h
    include/libfiles/xmp.h
    src/bmff_internal.h
    src/commodore.cpp
    src/date_internal.h
    src/dates.cpp
    src/decode.cpp
    src/decode_internal.h
    src/decode_psd.cpp
    src/document.cpp
    src/document_bmp.cpp
    src/document_internal.h
    src/document_pcx.cpp
    src/document_wpg.cpp
    src/encode_internal.h
    src/exif.cpp
    src/exif_listing.cpp
    src/exif_write.cpp
    src/gps.cpp
    src/gps_internal.h
    src/icc.cpp
    src/id3.cpp
    src/id3_internal.h
    src/identify.cpp
    src/iptc.cpp
    src/iptc_internal.h
    src/media.cpp
    src/media_audio.cpp
    src/media_internal.h
    src/media_tags.cpp
    src/media_video.cpp
    src/media_mpeg_internal.h
    src/mp4_internal.h
    src/asf_internal.h
    src/flv_internal.h
    src/photoshop_internal.h
    src/properties.cpp
    src/reader.cpp
    src/riff_internal.h
    src/scan.cpp
    src/scan_gif_bmp.cpp
    src/scan_heif.cpp
    src/scan_internal.h
    src/scan_jpeg.cpp
    src/scan_jxl.cpp
    src/scan_media_aiff.cpp
    src/scan_media_asf.cpp
    src/scan_media_flac.cpp
    src/scan_media_flv.cpp
    src/scan_media_mkv.cpp
    src/scan_media_mp4.cpp
    src/scan_media_mpa.cpp
    src/scan_media_ogg.cpp
    src/scan_media_ps.cpp
    src/scan_media_riff.cpp
    src/scan_media_ts.cpp
    src/scan_png.cpp
    src/scan_psd.cpp
    src/scan_tiff.cpp
    src/scan_webp.cpp
    src/tiff_internal.h
    src/write.cpp
    src/write_asf.cpp
    src/write_flv.cpp
    src/write_gif.cpp
    src/write_internal.h
    src/write_jpeg.cpp
    src/write_mp3.cpp
    src/write_mp4.cpp
    src/write_png.cpp
    src/write_riff.cpp
    src/write_tiff_psd.cpp
    src/write_webp.cpp
    src/legacy.h
    src/legacy_photo.cpp
    src/libexif_canon.cpp
    src/libexif_fuji.cpp
    src/libexif_makernote.cpp
    src/libexif_makernote.h
    src/libexif_olympus.cpp
    src/libexif_pentax.cpp
    src/libexif_port.h
    src/libexif_tags.cpp
    src/libexif_values.cpp
    src/md5.cpp
    src/text.cpp
    src/text_internal.h
    src/tiff_editor.cpp
    src/types.cpp
    src/vocabulary_internal.h
    src/xmp_internal.h
    src/xmp_meta.cpp
    src/xmp_rdf.cpp
    src/xmp_registry.cpp
    src/xmp_serialize.cpp
    src/xmp_xml.cpp
    src/zlib.cpp
)
add_library(libfiles::libfiles ALIAS libfiles)

target_compile_features(libfiles PUBLIC cxx_std_20)
target_include_directories(libfiles PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(LIBFILES_JPEG)
    target_sources(libfiles PRIVATE src/decode_jpeg.cpp src/encode_jpeg.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::jpeg)
    target_compile_definitions(libfiles PRIVATE LIBFILES_JPEG)
endif()

if(LIBFILES_PNG)
    target_sources(libfiles PRIVATE src/decode_png.cpp src/encode_png.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::png)
    target_compile_definitions(libfiles PRIVATE LIBFILES_PNG)
endif()

if(LIBFILES_WEBP)
    target_sources(libfiles PRIVATE src/decode_webp.cpp src/encode_webp.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::webp)
    target_compile_definitions(libfiles PRIVATE LIBFILES_WEBP)
endif()

if(LIBFILES_HEIF)
    target_sources(libfiles PRIVATE src/decode_heif.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::heif)
    target_compile_definitions(libfiles PRIVATE LIBFILES_HEIF)
endif()

if(LIBFILES_JXL)
    target_sources(libfiles PRIVATE src/decode_jxl.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::jxl)
    target_compile_definitions(libfiles PRIVATE LIBFILES_JXL)
endif()

if(LIBFILES_RAW)
    target_sources(libfiles PRIVATE include/libfiles/raw.h src/raw.cpp src/raw_internal.h src/raw_listing.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::raw)
    target_compile_definitions(libfiles PRIVATE LIBFILES_RAW)
endif()

if(LIBFILES_GIF)
    target_sources(libfiles PRIVATE src/document_gif.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::gif)
    target_compile_definitions(libfiles PRIVATE LIBFILES_GIF)
endif()

if(LIBFILES_TIFF)
    target_sources(libfiles PRIVATE src/document_tiff.cpp)
    target_link_libraries(libfiles PRIVATE libfiles::tiff)
    target_compile_definitions(libfiles PRIVATE LIBFILES_TIFF)
endif()

# files.lib on Windows and libfiles.a elsewhere, rather than liblibfiles.a.
set_target_properties(libfiles PROPERTIES OUTPUT_NAME files)

# /EHsc is stated, not inherited: libfiles throws and catches, and a consumer's CMAKE_CXX_FLAGS may not
# carry CMake's default - ImageWalker's build trees have been seen with it empty - which would leave
# libfiles' frames without unwinding.
if(MSVC)
    target_compile_options(libfiles PRIVATE /EHsc /W4 /permissive- /utf-8)
else()
    target_compile_options(libfiles PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Only when built directly; a consumer that fetches libfiles does not want its tests.
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    add_executable(libfiles_tests
        tests/agreement_tests.cpp
        tests/asf_write_tests.cpp
        tests/codec_tests.cpp
        tests/av_write_tests.cpp
        tests/builders.h
        tests/commodore_tests.cpp
        tests/decode_tests.cpp
        tests/document_tests.cpp
        tests/encode_tests.cpp
        tests/exif_tests.cpp
        tests/heif_jxl_scan_tests.cpp
        tests/icc_corpus.h
        tests/icc_tests.cpp
        tests/iptc_tests.cpp
        tests/interruptions.h
        tests/jpeg_decode_tests.cpp
        tests/jxl_codec_tests.cpp
        tests/jxl_decode_tests.cpp
        tests/media_asf_flv_tests.cpp
        tests/media_audio_files_tests.cpp
        tests/media_audio_tests.cpp
        tests/media_builders.h
        tests/media_mkv_tests.cpp
        tests/media_mp4_tests.cpp
        tests/media_mpeg_tests.cpp
        tests/media_riff_tests.cpp
        tests/media_tags_tests.cpp
        tests/media_tests.cpp
        tests/media_video_tests.cpp
        tests/test.h
        tests/main.cpp
        tests/mp4_write_tests.cpp
        tests/photo_write_tests.cpp
        tests/properties_tests.cpp
        tests/round_trip.h
        tests/heif_decode_tests.cpp
        tests/png_decode_tests.cpp
        tests/raw_codec_tests.cpp
        tests/raw_load_tests.cpp
        tests/raw_scan_tests.cpp
        tests/webp_decode_tests.cpp
        tests/scan_tests.cpp
        tests/write_tests.cpp
        tests/xmp_tests.cpp
    )
    target_link_libraries(libfiles_tests PRIVATE libfiles::libfiles)

    if(LIBFILES_JPEG)
        target_link_libraries(libfiles_tests PRIVATE libfiles::jpeg)
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_JPEG)
    endif()

    if(LIBFILES_PNG)
        target_link_libraries(libfiles_tests PRIVATE libfiles::png)
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_PNG)
    endif()

    if(LIBFILES_WEBP)
        target_link_libraries(libfiles_tests PRIVATE libfiles::webp)
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_WEBP)
    endif()

    if(LIBFILES_GIF)
        target_link_libraries(libfiles_tests PRIVATE libfiles::gif)
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_GIF)
    endif()

    if(LIBFILES_TIFF)
        target_link_libraries(libfiles_tests PRIVATE libfiles::tiff)
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_TIFF)
    endif()

    if(LIBFILES_HEIF)
        target_link_libraries(libfiles_tests PRIVATE libfiles::heif)
        # libheif's own example images, an HEVC one and an AV1 one, and its fuzzing corpus, from the
        # fetched archive.
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_HEIF
            LIBFILES_HEIF_EXAMPLES="${libheif_SOURCE_DIR}/examples"
            LIBFILES_HEIF_CORPUS="${libheif_SOURCE_DIR}/fuzzing/data/corpus")
    endif()

    if(LIBFILES_JXL)
        target_link_libraries(libfiles_tests PRIVATE libfiles::jxl)
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_JXL)
    endif()

    if(LIBFILES_RAW)
        target_link_libraries(libfiles_tests PRIVATE libfiles::raw)
        # The DNG SDK's own sample DNGs, from the fetched archive.
        target_compile_definitions(libfiles_tests PRIVATE LIBFILES_RAW
            LIBFILES_DNG_SAMPLES="${dngsdk_SOURCE_DIR}/sample_files")
    endif()
    target_include_directories(libfiles_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(libfiles_tests PRIVATE /EHsc /W4 /permissive- /utf-8)
    else()
        target_compile_options(libfiles_tests PRIVATE -Wall -Wextra)
        # GCC 15's optimizer reports bounds false positives in the libstdc++ vector code the tests inline:
        # writes into copies of buffers it cannot prove non-empty. The library builds without them.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
            target_compile_options(libfiles_tests PRIVATE -Wno-array-bounds -Wno-stringop-overflow -Wno-restrict)
        endif()
    endif()

    add_test(NAME libfiles COMMAND libfiles_tests)
    set_tests_properties(libfiles PROPERTIES LABELS libfiles)
    add_test(NAME libfiles-dependency-arguments
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_arguments.cmake")
    set_tests_properties(libfiles-dependency-arguments PROPERTIES LABELS libfiles)
endif()
