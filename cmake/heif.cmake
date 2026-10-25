# libheif, for libfiles' HEIF and AVIF decoder and for its consumers, fetched as the release archive at
# its SHA-256 (cmake/dd-dependencies.json) and built by this recipe as libfiles::heif, with its JPEG,
# libde265, dav1d and uncompressed decoders and libde265 and dav1d beside it. The source list and the
# version header its own build generates, in cmake/libheif/, are Diffractor's, which shipped them; the
# fetched tree is upstream's own, unmodified. CMakeLists.txt includes this when LIBFILES_HEIF is on.
#
# Its FFmpeg decoder is not built here: libfiles does not take on FFmpeg. An application that has FFmpeg
# builds plugins/decoder_ffmpeg.cc from this tree - FetchContent_GetProperties(libheif) gives its
# libheif_SOURCE_DIR - and registers it with heif_register_decoder_plugin before it decodes. libheif
# keeps a plugin until heif_deinit. Decoding is multithreaded, tiles in parallel.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/jpeg.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/de265.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/dav1d.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" libheif _lf_heif_acquisition)
FetchContent_Declare(libheif ${_lf_heif_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(libheif)

# The library's own subtree of the archive.
set(_lf_heif_src "${libheif_SOURCE_DIR}/libheif")

set(_lf_heif_sources
    api/libheif/heif_aux_images.cc api/libheif/heif_brands.cc api/libheif/heif_color.cc
    api/libheif/heif_components.cc api/libheif/heif_context.cc api/libheif/heif_decoding.cc
    api/libheif/heif_encoding.cc api/libheif/heif_entity_groups.cc api/libheif/heif_image_handle.cc
    api/libheif/heif_image.cc api/libheif/heif_items.cc api/libheif/heif_library.cc
    api/libheif/heif_metadata.cc api/libheif/heif_omaf.cc api/libheif/heif_plugin.cc
    api/libheif/heif_properties.cc api/libheif/heif_regions.cc api/libheif/heif_security.cc
    api/libheif/heif_sequences.cc api/libheif/heif_tai_timestamps.cc api/libheif/heif_text.cc
    api/libheif/heif_tiling.cc api/libheif/heif_uncompressed.cc api/libheif/heif.cc bitstream.cc box.cc
    brands.cc codecs/avc_boxes.cc codecs/avc_dec.cc codecs/avc_enc.cc codecs/avif_boxes.cc
    codecs/avif_dec.cc codecs/avif_enc.cc codecs/decoder.cc codecs/encoder.cc codecs/hevc_boxes.cc
    codecs/hevc_dec.cc codecs/hevc_enc.cc codecs/jpeg_boxes.cc codecs/jpeg_dec.cc codecs/jpeg_enc.cc
    codecs/jpeg2000_boxes.cc codecs/jpeg2000_dec.cc codecs/jpeg2000_enc.cc
    codecs/uncompressed/unc_boxes.cc codecs/uncompressed/unc_codec.cc codecs/uncompressed/unc_dec.cc
    codecs/uncompressed/unc_decoder_block_component_interleave.cc
    codecs/uncompressed/unc_decoder_block_pixel_interleave.cc
    codecs/uncompressed/unc_decoder_bytealign_component_interleave.cc
    codecs/uncompressed/unc_decoder_component_interleave.cc
    codecs/uncompressed/unc_decoder_legacybase.cc codecs/uncompressed/unc_decoder_mixed_interleave.cc
    codecs/uncompressed/unc_decoder_pixel_interleave.cc
    codecs/uncompressed/unc_decoder_row_interleave.cc codecs/uncompressed/unc_decoder.cc
    codecs/uncompressed/unc_enc.cc codecs/uncompressed/unc_encoder_component_interleave.cc
    codecs/uncompressed/unc_encoder_rgb_block_pixel_interleave.cc
    codecs/uncompressed/unc_encoder_rgb_bytealign_pixel_interleave.cc
    codecs/uncompressed/unc_encoder_rgb_pixel_interleave.cc codecs/uncompressed/unc_encoder.cc
    codecs/vvc_boxes.cc codecs/vvc_dec.cc codecs/vvc_enc.cc color-conversion/alpha.cc
    color-conversion/bayer_bilinear.cc color-conversion/chroma_sampling.cc
    color-conversion/colorconversion.cc color-conversion/hdr_sdr.cc color-conversion/monochrome.cc
    color-conversion/rgb2rgb.cc color-conversion/rgb2yuv_sharp.cc color-conversion/rgb2yuv.cc
    color-conversion/yuv2rgb.cc common_utils.cc compression_brotli.cc compression_zlib.cc
    compression.cc context.cc error.cc file_layout.cc file.cc id_creator.cc image-items/avc.cc
    image-items/avif.cc image-items/grid.cc image-items/hevc.cc image-items/iden.cc
    image-items/image_item.cc image-items/jpeg.cc image-items/jpeg2000.cc image-items/mask_image.cc
    image-items/overlay.cc image-items/tiled.cc image-items/unc_image.cc image-items/vvc.cc
    image/image_description.cc image/pixelimage.cc init.cc logging.cc mini.cc nclx.cc omaf_boxes.cc
    plugin_registry.cc plugins/decoder_dav1d.cc plugins/decoder_jpeg.cc plugins/decoder_libde265.cc
    plugins/decoder_uncompressed.cc plugins/encoder_mask.cc plugins/encoder_uncompressed.cc
    plugins/nalu_utils.cc region.cc security_limits.cc sequences/chunk.cc sequences/seq_boxes.cc
    sequences/track_metadata.cc sequences/track_visual.cc sequences/track.cc text.cc api)
list(TRANSFORM _lf_heif_sources PREPEND "${_lf_heif_src}/")

add_library(libfiles-heif STATIC ${_lf_heif_sources})
add_library(libfiles::heif ALIAS libfiles-heif)

target_include_directories(libfiles-heif PUBLIC "${CMAKE_CURRENT_LIST_DIR}/libheif" "${_lf_heif_src}/api")
target_include_directories(libfiles-heif PRIVATE "${_lf_heif_src}")
target_link_libraries(libfiles-heif PUBLIC libfiles::de265 libfiles::dav1d libfiles::jpeg)

target_compile_definitions(libfiles-heif PUBLIC LIBHEIF_STATIC_BUILD)
target_compile_definitions(libfiles-heif PRIVATE
    HAVE_JPEG_DECODER HAVE_DAV1D HAVE_LIBDE265 WITH_UNCOMPRESSED_CODEC HAVE_BIT
    ENABLE_MULTITHREADING_SUPPORT=1 ENABLE_PARALLEL_TILE_DECODING=1 IS_BIG_ENDIAN=0 _CRT_NONSTDC_NO_DEPRECATE)

if(WIN32)
    target_compile_definitions(libfiles-heif PRIVATE UNICODE _UNICODE)
endif()

target_compile_features(libfiles-heif PUBLIC cxx_std_20)
set_target_properties(libfiles-heif PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-heif PRIVATE -w)
