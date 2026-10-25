# libjxl - its decoder, encoder, colour management and thread runners - for libfiles' JPEG XL decoder and
# for its consumers, with Highway, Brotli and skcms beside it: libjxl and Highway fetched as release
# archives at their SHA-256s (cmake/dd-dependencies.json) and built by this recipe as libfiles::jxl and
# libfiles::highway. The source lists and the headers libjxl's own build generates, in cmake/jxl/include/,
# are Diffractor's, which shipped them - less libjxl's extras, its tools' image readers and writers, which
# nothing links. The fetched trees are upstream's own, unmodified. CMakeLists.txt includes this when
# LIBFILES_JXL is on.
#
# Diffractor built them unoptimised under MSVC for years, for a miscompile that decoded a JPEG XL to an
# empty image. With libjxl 0.12.0, Highway 1.4.0 and MSVC 14.51 it does not happen - under Diffractor's
# own release flags, /O2 /Os /Oy /GL, its JPEG XL record holds - so they are optimised like the rest, and
# decode some fifty times faster.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/brotli.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/skcms.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" highway _lf_highway_acquisition)
FetchContent_Declare(highway ${_lf_highway_acquisition} SOURCE_SUBDIR "no-cmake-project")
dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" libjxl _lf_jxl_acquisition)
FetchContent_Declare(libjxl ${_lf_jxl_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(highway libjxl)

set(_lf_highway_src "${highway_SOURCE_DIR}")

set(_lf_highway_sources
    hwy/abort.cc hwy/aligned_allocator.cc hwy/contrib/image/image.cc hwy/contrib/sort/print_network.cc
    hwy/contrib/sort/vqsort.cc hwy/contrib/sort/vqsort_128a.cc hwy/contrib/sort/vqsort_128d.cc
    hwy/contrib/sort/vqsort_f32a.cc hwy/contrib/sort/vqsort_f32d.cc hwy/contrib/sort/vqsort_f64a.cc
    hwy/contrib/sort/vqsort_f64d.cc hwy/contrib/sort/vqsort_i16a.cc hwy/contrib/sort/vqsort_i16d.cc
    hwy/contrib/sort/vqsort_i32a.cc hwy/contrib/sort/vqsort_i32d.cc hwy/contrib/sort/vqsort_i64a.cc
    hwy/contrib/sort/vqsort_i64d.cc hwy/contrib/sort/vqsort_u16a.cc hwy/contrib/sort/vqsort_u16d.cc
    hwy/contrib/sort/vqsort_u32a.cc hwy/contrib/sort/vqsort_u32d.cc hwy/contrib/sort/vqsort_u64a.cc
    hwy/contrib/sort/vqsort_u64d.cc hwy/per_target.cc hwy/perf_counters.cc hwy/print.cc hwy/profiler.cc
    hwy/targets.cc hwy/timer.cc)
list(TRANSFORM _lf_highway_sources PREPEND "${_lf_highway_src}/")

add_library(libfiles-highway STATIC ${_lf_highway_sources})
add_library(libfiles::highway ALIAS libfiles-highway)

target_include_directories(libfiles-highway PUBLIC "${_lf_highway_src}")

if(WIN32)
    target_compile_definitions(libfiles-highway PRIVATE UNICODE _UNICODE)
endif()

target_compile_features(libfiles-highway PUBLIC cxx_std_17)
set_target_properties(libfiles-highway PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-highway PRIVATE -w)

set(_lf_jxl_src "${libjxl_SOURCE_DIR}")

set(_lf_jxl_sources
    lib/jxl/ac_strategy.cc lib/jxl/alpha.cc lib/jxl/ans_common.cc lib/jxl/blending.cc
    lib/jxl/box_content_decoder.cc lib/jxl/butteraugli/butteraugli.cc lib/jxl/chroma_from_luma.cc
    lib/jxl/cms/jxl_cms.cc lib/jxl/coeff_order.cc lib/jxl/color_encoding_internal.cc
    lib/jxl/compressed_dc.cc lib/jxl/convolve_slow.cc lib/jxl/convolve_symmetric5.cc
    lib/jxl/dct_scales.cc lib/jxl/dec_ans.cc lib/jxl/dec_bit_reader.cc lib/jxl/dec_cache.cc
    lib/jxl/dec_context_map.cc lib/jxl/dec_external_image.cc lib/jxl/dec_frame.cc lib/jxl/dec_group.cc
    lib/jxl/dec_group_border.cc lib/jxl/dec_huffman.cc lib/jxl/dec_modular.cc lib/jxl/dec_noise.cc
    lib/jxl/dec_patch_dictionary.cc lib/jxl/dec_xyb.cc lib/jxl/decode.cc lib/jxl/decode_to_jpeg.cc
    lib/jxl/enc_ac_strategy.cc lib/jxl/enc_adaptive_quantization.cc lib/jxl/enc_ans.cc
    lib/jxl/enc_ans_simd.cc lib/jxl/enc_aux_out.cc lib/jxl/enc_bit_writer.cc
    lib/jxl/enc_butteraugli_comparator.cc lib/jxl/enc_cache.cc lib/jxl/enc_chroma_from_luma.cc
    lib/jxl/enc_cluster.cc lib/jxl/enc_coeff_order.cc lib/jxl/enc_comparator.cc
    lib/jxl/enc_context_map.cc lib/jxl/enc_convolve_separable5.cc lib/jxl/enc_debug_image.cc
    lib/jxl/enc_detect_dots.cc lib/jxl/enc_dot_dictionary.cc lib/jxl/enc_entropy_coder.cc
    lib/jxl/enc_external_image.cc lib/jxl/enc_fast_lossless.cc lib/jxl/enc_fields.cc
    lib/jxl/enc_frame.cc lib/jxl/enc_gaborish.cc lib/jxl/enc_group.cc lib/jxl/enc_heuristics.cc
    lib/jxl/enc_huffman.cc lib/jxl/enc_huffman_tree.cc lib/jxl/enc_icc_codec.cc
    lib/jxl/enc_image_bundle.cc lib/jxl/enc_linalg.cc lib/jxl/enc_lz77.cc lib/jxl/enc_modular.cc
    lib/jxl/enc_modular_simd.cc lib/jxl/enc_noise.cc lib/jxl/enc_patch_dictionary.cc
    lib/jxl/enc_photon_noise.cc lib/jxl/enc_progressive_split.cc lib/jxl/enc_quant_weights.cc
    lib/jxl/enc_splines.cc lib/jxl/enc_toc.cc lib/jxl/enc_transforms.cc lib/jxl/enc_xyb.cc
    lib/jxl/encode.cc lib/jxl/entropy_coder.cc lib/jxl/epf.cc lib/jxl/fields.cc lib/jxl/frame_header.cc
    lib/jxl/headers.cc lib/jxl/huffman_table.cc lib/jxl/icc_codec.cc lib/jxl/icc_codec_common.cc
    lib/jxl/image.cc lib/jxl/image_bundle.cc lib/jxl/image_metadata.cc lib/jxl/image_ops.cc
    lib/jxl/jpeg/dec_jpeg_data.cc lib/jxl/jpeg/dec_jpeg_data_writer.cc lib/jxl/jpeg/enc_jpeg_data.cc
    lib/jxl/jpeg/enc_jpeg_data_reader.cc lib/jxl/jpeg/enc_jpeg_huffman_decode.cc
    lib/jxl/jpeg/jpeg_data.cc lib/jxl/loop_filter.cc lib/jxl/luminance.cc
    lib/jxl/memory_manager_internal.cc lib/jxl/modular/encoding/dec_ma.cc
    lib/jxl/modular/encoding/enc_debug_tree.cc lib/jxl/modular/encoding/enc_encoding.cc
    lib/jxl/modular/encoding/enc_ma.cc lib/jxl/modular/encoding/encoding.cc
    lib/jxl/modular/modular_image.cc lib/jxl/modular/transform/enc_palette.cc
    lib/jxl/modular/transform/enc_rct.cc lib/jxl/modular/transform/enc_squeeze.cc
    lib/jxl/modular/transform/enc_transform.cc lib/jxl/modular/transform/palette.cc
    lib/jxl/modular/transform/rct.cc lib/jxl/modular/transform/squeeze.cc
    lib/jxl/modular/transform/squeeze_params.cc lib/jxl/modular/transform/transform.cc
    lib/jxl/opsin_params.cc lib/jxl/passes_state.cc lib/jxl/quant_weights.cc lib/jxl/quantizer.cc
    lib/jxl/render_pipeline/low_memory_render_pipeline.cc lib/jxl/render_pipeline/render_pipeline.cc
    lib/jxl/render_pipeline/render_pipeline_stage.cc lib/jxl/render_pipeline/simple_render_pipeline.cc
    lib/jxl/render_pipeline/stage_blending.cc lib/jxl/render_pipeline/stage_chroma_upsampling.cc
    lib/jxl/render_pipeline/stage_cms.cc lib/jxl/render_pipeline/stage_epf.cc
    lib/jxl/render_pipeline/stage_from_linear.cc lib/jxl/render_pipeline/stage_gaborish.cc
    lib/jxl/render_pipeline/stage_noise.cc lib/jxl/render_pipeline/stage_patches.cc
    lib/jxl/render_pipeline/stage_splines.cc lib/jxl/render_pipeline/stage_spot.cc
    lib/jxl/render_pipeline/stage_to_linear.cc lib/jxl/render_pipeline/stage_tone_mapping.cc
    lib/jxl/render_pipeline/stage_upsampling.cc lib/jxl/render_pipeline/stage_write.cc
    lib/jxl/render_pipeline/stage_xyb.cc lib/jxl/render_pipeline/stage_ycbcr.cc lib/jxl/simd_util.cc
    lib/jxl/splines.cc lib/jxl/toc.cc lib/threads/resizable_parallel_runner.cc
    lib/threads/thread_parallel_runner.cc lib/threads/thread_parallel_runner_internal.cc)
list(TRANSFORM _lf_jxl_sources PREPEND "${_lf_jxl_src}/")

add_library(libfiles-jxl STATIC ${_lf_jxl_sources})
add_library(libfiles::jxl ALIAS libfiles-jxl)

# Its public headers, those its build generates, and the root its own sources include from.
target_include_directories(libfiles-jxl PUBLIC "${_lf_jxl_src}/lib/include" "${CMAKE_CURRENT_LIST_DIR}/jxl/include")
target_include_directories(libfiles-jxl PRIVATE "${_lf_jxl_src}")
target_link_libraries(libfiles-jxl PUBLIC libfiles::highway libfiles::brotli libfiles::skcms)

target_compile_definitions(libfiles-jxl PRIVATE JPEGXL_ENABLE_SKCMS=1)

if(WIN32)
    target_compile_definitions(libfiles-jxl PRIVATE UNICODE _UNICODE)
endif()

target_compile_features(libfiles-jxl PUBLIC cxx_std_17)
set_target_properties(libfiles-jxl PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-jxl PRIVATE -w)
