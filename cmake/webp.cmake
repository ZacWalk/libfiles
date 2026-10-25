# libwebp - its decoder, encoder, sharpyuv, mux and demux - for libfiles' WebP decoder and for its
# consumers: fetched as the release archive at its SHA-256 (cmake/dd-dependencies.json) and built by this
# recipe, as libfiles::webp. The source list is Diffractor's, which shipped it: every library source but
# the MIPS MSA ones. The fetched tree is upstream's own, unmodified. CMakeLists.txt includes this when
# LIBFILES_WEBP is on.
#
# libwebp chooses its SIMD from what the compiler announces, and dispatches among them at run time:
# MSVC compiles its SSE2, SSE4.1 and AVX2 paths on x86 and x64, and NEON on ARM64; GCC and Clang, told
# nothing here, their baseline - SSE2 on x86-64, as Diffractor built it. WEBP_USE_THREAD is not
# defined, so a decode runs on the caller's thread alone.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")

enable_language(C)

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" libwebp _lf_webp_acquisition)
FetchContent_Declare(libwebp ${_lf_webp_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(libwebp)

set(_lf_webp_src "${libwebp_SOURCE_DIR}")

set(_lf_webp_sharpyuv
    sharpyuv.c sharpyuv_cpu.c sharpyuv_csp.c sharpyuv_dsp.c sharpyuv_gamma.c sharpyuv_neon.c
    sharpyuv_sse2.c)
list(TRANSFORM _lf_webp_sharpyuv PREPEND "${_lf_webp_src}/sharpyuv/")

set(_lf_webp_dec
    alpha_dec.c buffer_dec.c frame_dec.c idec_dec.c io_dec.c quant_dec.c tree_dec.c vp8_dec.c
    vp8l_dec.c webp_dec.c)
list(TRANSFORM _lf_webp_dec PREPEND "${_lf_webp_src}/src/dec/")

set(_lf_webp_demux
    anim_decode.c demux.c)
list(TRANSFORM _lf_webp_demux PREPEND "${_lf_webp_src}/src/demux/")

set(_lf_webp_dsp
    alpha_processing.c alpha_processing_mips_dsp_r2.c alpha_processing_neon.c alpha_processing_sse2.c
    alpha_processing_sse41.c cost.c cost_mips32.c cost_mips_dsp_r2.c cost_neon.c cost_sse2.c cpu.c
    dec.c dec_clip_tables.c dec_mips32.c dec_mips_dsp_r2.c dec_neon.c dec_sse2.c dec_sse41.c enc.c
    enc_mips32.c enc_mips_dsp_r2.c enc_neon.c enc_sse2.c enc_sse41.c filters.c filters_mips_dsp_r2.c
    filters_neon.c filters_sse2.c lossless.c lossless_avx2.c lossless_enc.c lossless_enc_avx2.c
    lossless_enc_mips32.c lossless_enc_mips_dsp_r2.c lossless_enc_neon.c lossless_enc_sse2.c
    lossless_enc_sse41.c lossless_mips_dsp_r2.c lossless_neon.c lossless_sse2.c lossless_sse41.c
    rescaler.c rescaler_mips32.c rescaler_mips_dsp_r2.c rescaler_neon.c rescaler_sse2.c ssim.c
    ssim_sse2.c upsampling.c upsampling_mips_dsp_r2.c upsampling_neon.c upsampling_sse2.c
    upsampling_sse41.c yuv.c yuv_mips32.c yuv_mips_dsp_r2.c yuv_neon.c yuv_sse2.c yuv_sse41.c)
list(TRANSFORM _lf_webp_dsp PREPEND "${_lf_webp_src}/src/dsp/")

set(_lf_webp_enc
    alpha_enc.c analysis_enc.c backward_references_cost_enc.c backward_references_enc.c config_enc.c
    cost_enc.c filter_enc.c frame_enc.c histogram_enc.c iterator_enc.c near_lossless_enc.c
    picture_csp_enc.c picture_enc.c picture_psnr_enc.c picture_rescale_enc.c picture_tools_enc.c
    predictor_enc.c quant_enc.c syntax_enc.c token_enc.c tree_enc.c vp8l_enc.c webp_enc.c)
list(TRANSFORM _lf_webp_enc PREPEND "${_lf_webp_src}/src/enc/")

set(_lf_webp_mux
    anim_encode.c muxedit.c muxinternal.c muxread.c)
list(TRANSFORM _lf_webp_mux PREPEND "${_lf_webp_src}/src/mux/")

set(_lf_webp_utils
    bit_reader_utils.c bit_writer_utils.c color_cache_utils.c filters_utils.c huffman_encode_utils.c
    huffman_utils.c palette.c quant_levels_dec_utils.c quant_levels_utils.c random_utils.c
    rescaler_utils.c thread_utils.c utils.c)
list(TRANSFORM _lf_webp_utils PREPEND "${_lf_webp_src}/src/utils/")

add_library(libfiles-webp STATIC
    ${_lf_webp_sharpyuv}
    ${_lf_webp_dec}
    ${_lf_webp_demux}
    ${_lf_webp_dsp}
    ${_lf_webp_enc}
    ${_lf_webp_mux}
    ${_lf_webp_utils})
add_library(libfiles::webp ALIAS libfiles-webp)

# The sources include each other from the tree's root - "src/dsp/dsp.h" - and a consumer includes the
# public headers as the system package installs them - "webp/decode.h" - which live under src/.
target_include_directories(libfiles-webp PUBLIC "${_lf_webp_src}/src" "${_lf_webp_src}")

if(WIN32)
    target_compile_definitions(libfiles-webp PRIVATE UNICODE _UNICODE)
endif()

set_target_properties(libfiles-webp PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-webp PRIVATE -w)
