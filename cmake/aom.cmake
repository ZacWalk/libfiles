# libaom, the AV1 encoder libheif writes AVIF with, fetched as the release archive at its SHA-256
# (cmake/dd-dependencies.json) and built by this recipe as libfiles::aom: its encoder alone, without its decoder -
# dav1d decodes AV1 - its examples or its tools. The configuration its own build generates, in cmake/aom/, is
# libfiles': libaom's CMake run with Perl, on Windows and on Linux for x86-64 and once more for generic C, with
# -DCONFIG_AV1_DECODER=0 -DCONFIG_LIBYUV=0 -DCONFIG_WEBM_IO=0 and its docs, examples, test data, tests and tools off.
# The two x86-64 configurations differ only in what the platform sets, which aom_config.h and aom_config.asm test.
# The source lists are what that build links into its library. The fetched tree is upstream's own, unmodified.
# heif.cmake includes this.
#
# On x86-64 with NASM its SIMD is built - each intrinsics file for the instruction set it is written for, which
# libaom picks between as the processor allows, and its assembly. Elsewhere - x86, another architecture, or no
# NASM - cmake/aom/generic builds its C alone, which encodes the same pictures more slowly.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/nasm.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" aom _lf_aom_acquisition)
FetchContent_Declare(aom ${_lf_aom_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(aom)

set(_lf_aom_src "${aom_SOURCE_DIR}")
set(_lf_aom_cmake "${CMAKE_CURRENT_LIST_DIR}/aom")

set(_lf_aom_sources
    aom/src/aom_codec.c aom/src/aom_decoder.c aom/src/aom_encoder.c aom/src/aom_image.c aom/src/aom_integer.c
    aom_dsp/aom_convolve.c aom_dsp/aom_dsp_rtcd.c aom_dsp/avg.c aom_dsp/binary_codes_writer.c
    aom_dsp/bitreader_buffer.c aom_dsp/bitwriter.c aom_dsp/bitwriter_buffer.c aom_dsp/blend_a64_hmask.c
    aom_dsp/blend_a64_mask.c aom_dsp/blend_a64_vmask.c aom_dsp/blk_sse_sum.c aom_dsp/entcode.c aom_dsp/entenc.c
    aom_dsp/fft.c aom_dsp/flow_estimation/corner_detect.c aom_dsp/flow_estimation/corner_match.c
    aom_dsp/flow_estimation/disflow.c aom_dsp/flow_estimation/flow_estimation.c aom_dsp/flow_estimation/ransac.c
    aom_dsp/fwd_txfm.c aom_dsp/grain_table.c aom_dsp/intrapred.c aom_dsp/loopfilter.c aom_dsp/noise_model.c
    aom_dsp/noise_util.c aom_dsp/odintrin.c aom_dsp/psnr.c aom_dsp/pyramid.c aom_dsp/quantize.c aom_dsp/sad.c
    aom_dsp/sad_av1.c aom_dsp/sse.c aom_dsp/ssim.c aom_dsp/subtract.c aom_dsp/sum_squares.c aom_dsp/variance.c
    aom_mem/aom_mem.c aom_scale/aom_scale_rtcd.c aom_scale/generic/yv12config.c aom_scale/generic/yv12extend.c
    aom_util/aom_thread.c av1/arg_defs.c av1/av1_cx_iface.c av1/common/alloccommon.c av1/common/av1_inv_txfm1d.c
    av1/common/av1_inv_txfm2d.c av1/common/av1_loopfilter.c av1/common/av1_rtcd.c av1/common/av1_txfm.c
    av1/common/blockd.c av1/common/cdef.c av1/common/cdef_block.c av1/common/cfl.c av1/common/common_data.c
    av1/common/convolve.c av1/common/debugmodes.c av1/common/entropy.c av1/common/entropymode.c
    av1/common/entropymv.c av1/common/frame_buffers.c av1/common/idct.c av1/common/mvref_common.c
    av1/common/obu_util.c av1/common/pred_common.c av1/common/quant_common.c av1/common/reconinter.c
    av1/common/reconintra.c av1/common/resize.c av1/common/restoration.c av1/common/scale.c av1/common/scan.c
    av1/common/seg_common.c av1/common/thread_common.c av1/common/tile_common.c av1/common/timing.c
    av1/common/txb_common.c av1/common/warped_motion.c av1/encoder/allintra_vis.c av1/encoder/aq_complexity.c
    av1/encoder/aq_cyclicrefresh.c av1/encoder/aq_variance.c av1/encoder/av1_ext_ratectrl.c
    av1/encoder/av1_fwd_txfm1d.c av1/encoder/av1_fwd_txfm2d.c av1/encoder/av1_noise_estimate.c
    av1/encoder/av1_quantize.c av1/encoder/bitstream.c av1/encoder/cnn.c av1/encoder/compound_type.c
    av1/encoder/context_tree.c av1/encoder/cost.c av1/encoder/dwt.c av1/encoder/encode_strategy.c
    av1/encoder/encodeframe.c av1/encoder/encodeframe_utils.c av1/encoder/encodemb.c av1/encoder/encodemv.c
    av1/encoder/encoder.c av1/encoder/encoder_utils.c av1/encoder/encodetxb.c av1/encoder/ethread.c
    av1/encoder/extend.c av1/encoder/external_partition.c av1/encoder/firstpass.c av1/encoder/global_motion.c
    av1/encoder/global_motion_facade.c av1/encoder/gop_structure.c av1/encoder/hash.c av1/encoder/hash_motion.c
    av1/encoder/hybrid_fwd_txfm.c av1/encoder/interp_search.c av1/encoder/intra_mode_search.c
    av1/encoder/level.c av1/encoder/lookahead.c av1/encoder/mcomp.c av1/encoder/ml.c
    av1/encoder/motion_search_facade.c av1/encoder/mv_prec.c av1/encoder/nonrd_opt.c
    av1/encoder/nonrd_pickmode.c av1/encoder/palette.c av1/encoder/partition_search.c
    av1/encoder/partition_strategy.c av1/encoder/pass2_strategy.c av1/encoder/pickcdef.c av1/encoder/picklpf.c
    av1/encoder/pickrst.c av1/encoder/ratectrl.c av1/encoder/rd.c av1/encoder/rdopt.c
    av1/encoder/reconinter_enc.c av1/encoder/segmentation.c av1/encoder/speed_features.c
    av1/encoder/superres_scale.c av1/encoder/svc_layercontext.c av1/encoder/temporal_filter.c
    av1/encoder/tokenize.c av1/encoder/tpl_model.c av1/encoder/tx_search.c av1/encoder/txb_rdopt.c
    av1/encoder/var_based_part.c av1/encoder/wedge_utils.c common/args_helper.c third_party/fastfeat/fast.c
    third_party/fastfeat/fast_9.c third_party/fastfeat/nonmax.c third_party/vector/vector.c)

set(_lf_aom_sse2
    aom_dsp/x86/adaptive_quantize_sse2.c aom_dsp/x86/aom_convolve_copy_sse2.c aom_dsp/x86/avg_intrin_sse2.c
    aom_dsp/x86/blk_sse_sum_sse2.c aom_dsp/x86/fft_sse2.c aom_dsp/x86/fwd_txfm_sse2.c
    aom_dsp/x86/highbd_adaptive_quantize_sse2.c aom_dsp/x86/highbd_convolve_sse2.c
    aom_dsp/x86/highbd_intrapred_sse2.c aom_dsp/x86/highbd_loopfilter_sse2.c
    aom_dsp/x86/highbd_quantize_intrin_sse2.c aom_dsp/x86/highbd_subtract_sse2.c
    aom_dsp/x86/highbd_variance_sse2.c aom_dsp/x86/intrapred_sse2.c aom_dsp/x86/loopfilter_sse2.c
    aom_dsp/x86/quantize_sse2.c aom_dsp/x86/sum_squares_sse2.c aom_dsp/x86/variance_sse2.c
    av1/common/x86/cfl_sse2.c av1/common/x86/convolve_2d_sse2.c av1/common/x86/convolve_sse2.c
    av1/common/x86/jnt_convolve_sse2.c av1/common/x86/resize_sse2.c av1/common/x86/wiener_convolve_sse2.c
    av1/encoder/x86/av1_fwd_txfm_sse2.c av1/encoder/x86/av1_k_means_sse2.c av1/encoder/x86/av1_quantize_sse2.c
    av1/encoder/x86/encodetxb_sse2.c av1/encoder/x86/error_intrin_sse2.c
    av1/encoder/x86/highbd_block_error_intrin_sse2.c av1/encoder/x86/highbd_temporal_filter_sse2.c
    av1/encoder/x86/model_rd_sse2.c av1/encoder/x86/reconinter_enc_sse2.c av1/encoder/x86/temporal_filter_sse2.c
    av1/encoder/x86/wedge_utils_sse2.c)

set(_lf_aom_sse3
    av1/encoder/x86/ml_sse3.c)

set(_lf_aom_ssse3
    aom_dsp/x86/aom_subpixel_8t_intrin_ssse3.c aom_dsp/x86/highbd_convolve_ssse3.c aom_dsp/x86/intrapred_ssse3.c
    aom_dsp/x86/masked_sad_intrin_ssse3.c aom_dsp/x86/masked_variance_intrin_ssse3.c
    aom_dsp/x86/quantize_ssse3.c aom_dsp/x86/variance_impl_ssse3.c aom_dsp/x86/variance_ssse3.c
    av1/common/x86/av1_inv_txfm_ssse3.c av1/common/x86/cfl_ssse3.c av1/common/x86/highbd_convolve_2d_ssse3.c
    av1/common/x86/highbd_wiener_convolve_ssse3.c av1/common/x86/jnt_convolve_ssse3.c
    av1/common/x86/reconinter_ssse3.c av1/common/x86/resize_ssse3.c)

set(_lf_aom_sse4_1
    aom_dsp/flow_estimation/x86/corner_match_sse4.c aom_dsp/flow_estimation/x86/disflow_sse4.c
    aom_dsp/x86/avg_intrin_sse4.c aom_dsp/x86/blend_a64_hmask_sse4.c aom_dsp/x86/blend_a64_mask_sse4.c
    aom_dsp/x86/blend_a64_vmask_sse4.c aom_dsp/x86/highbd_variance_sse4.c aom_dsp/x86/intrapred_sse4.c
    aom_dsp/x86/obmc_sad_sse4.c aom_dsp/x86/obmc_variance_sse4.c aom_dsp/x86/sse_sse4.c
    av1/common/x86/av1_convolve_horiz_rs_sse4.c av1/common/x86/av1_convolve_scale_sse4.c
    av1/common/x86/av1_txfm_sse4.c av1/common/x86/cdef_block_sse4.c av1/common/x86/filterintra_sse4.c
    av1/common/x86/highbd_convolve_2d_sse4.c av1/common/x86/highbd_inv_txfm_sse4.c
    av1/common/x86/highbd_jnt_convolve_sse4.c av1/common/x86/highbd_warp_plane_sse4.c
    av1/common/x86/intra_edge_sse4.c av1/common/x86/reconinter_sse4.c av1/common/x86/selfguided_sse4.c
    av1/common/x86/warp_plane_sse4.c av1/encoder/x86/av1_fwd_txfm1d_sse4.c av1/encoder/x86/av1_fwd_txfm2d_sse4.c
    av1/encoder/x86/av1_highbd_quantize_sse4.c av1/encoder/x86/encodetxb_sse4.c
    av1/encoder/x86/highbd_fwd_txfm_sse4.c av1/encoder/x86/pickrst_sse4.c av1/encoder/x86/rdopt_sse4.c)

set(_lf_aom_sse4_2
    av1/encoder/x86/hash_sse42.c)

set(_lf_aom_avx
    aom_dsp/x86/aom_quantize_avx.c)

set(_lf_aom_avx2
    aom_dsp/flow_estimation/x86/corner_match_avx2.c aom_dsp/flow_estimation/x86/disflow_avx2.c
    aom_dsp/x86/adaptive_quantize_avx2.c aom_dsp/x86/aom_convolve_copy_avx2.c
    aom_dsp/x86/aom_subpixel_8t_intrin_avx2.c aom_dsp/x86/avg_intrin_avx2.c aom_dsp/x86/blend_a64_mask_avx2.c
    aom_dsp/x86/blk_sse_sum_avx2.c aom_dsp/x86/fft_avx2.c aom_dsp/x86/highbd_adaptive_quantize_avx2.c
    aom_dsp/x86/highbd_convolve_avx2.c aom_dsp/x86/highbd_loopfilter_avx2.c
    aom_dsp/x86/highbd_quantize_intrin_avx2.c aom_dsp/x86/highbd_sad_avx2.c aom_dsp/x86/highbd_variance_avx2.c
    aom_dsp/x86/intrapred_avx2.c aom_dsp/x86/loopfilter_avx2.c aom_dsp/x86/masked_sad_intrin_avx2.c
    aom_dsp/x86/obmc_sad_avx2.c aom_dsp/x86/obmc_variance_avx2.c aom_dsp/x86/quantize_avx2.c
    aom_dsp/x86/sad4d_avx2.c aom_dsp/x86/sad_avx2.c aom_dsp/x86/sad_impl_avx2.c aom_dsp/x86/sse_avx2.c
    aom_dsp/x86/subtract_avx2.c aom_dsp/x86/sum_squares_avx2.c aom_dsp/x86/variance_avx2.c
    aom_dsp/x86/variance_impl_avx2.c av1/common/x86/av1_inv_txfm_avx2.c av1/common/x86/cdef_block_avx2.c
    av1/common/x86/cfl_avx2.c av1/common/x86/convolve_2d_avx2.c av1/common/x86/convolve_avx2.c
    av1/common/x86/highbd_convolve_2d_avx2.c av1/common/x86/highbd_inv_txfm_avx2.c
    av1/common/x86/highbd_jnt_convolve_avx2.c av1/common/x86/highbd_warp_affine_avx2.c
    av1/common/x86/highbd_wiener_convolve_avx2.c av1/common/x86/jnt_convolve_avx2.c
    av1/common/x86/reconinter_avx2.c av1/common/x86/resize_avx2.c av1/common/x86/selfguided_avx2.c
    av1/common/x86/warp_plane_avx2.c av1/common/x86/wiener_convolve_avx2.c av1/encoder/x86/av1_fwd_txfm2d_avx2.c
    av1/encoder/x86/av1_highbd_quantize_avx2.c av1/encoder/x86/av1_k_means_avx2.c
    av1/encoder/x86/av1_quantize_avx2.c av1/encoder/x86/cnn_avx2.c av1/encoder/x86/encodetxb_avx2.c
    av1/encoder/x86/error_intrin_avx2.c av1/encoder/x86/highbd_block_error_intrin_avx2.c
    av1/encoder/x86/highbd_fwd_txfm_avx2.c av1/encoder/x86/highbd_temporal_filter_avx2.c
    av1/encoder/x86/ml_avx2.c av1/encoder/x86/pickrst_avx2.c av1/encoder/x86/rdopt_avx2.c
    av1/encoder/x86/temporal_filter_avx2.c av1/encoder/x86/wedge_utils_avx2.c)

set(_lf_aom_asm
    aom_dsp/x86/aom_high_subpixel_8t_sse2.asm aom_dsp/x86/aom_high_subpixel_bilinear_sse2.asm
    aom_dsp/x86/aom_subpixel_8t_ssse3.asm aom_dsp/x86/aom_subpixel_bilinear_ssse3.asm
    aom_dsp/x86/fwd_txfm_ssse3_x86_64.asm aom_dsp/x86/highbd_intrapred_asm_sse2.asm
    aom_dsp/x86/highbd_sad4d_sse2.asm aom_dsp/x86/highbd_sad_sse2.asm
    aom_dsp/x86/highbd_subpel_variance_impl_sse2.asm aom_dsp/x86/highbd_variance_impl_sse2.asm
    aom_dsp/x86/intrapred_asm_sse2.asm aom_dsp/x86/quantize_ssse3_x86_64.asm aom_dsp/x86/sad4d_sse2.asm
    aom_dsp/x86/sad_sse2.asm aom_dsp/x86/ssim_sse2_x86_64.asm aom_dsp/x86/subpel_variance_ssse3.asm
    aom_dsp/x86/subtract_sse2.asm aom_ports/float.asm av1/encoder/x86/av1_quantize_ssse3_x86_64.asm
    av1/encoder/x86/dct_sse2.asm av1/encoder/x86/error_sse2.asm)

foreach(_lf_aom_list _lf_aom_sources _lf_aom_sse2 _lf_aom_sse3 _lf_aom_ssse3 _lf_aom_sse4_1 _lf_aom_sse4_2
                     _lf_aom_avx _lf_aom_avx2 _lf_aom_asm)
    list(TRANSFORM ${_lf_aom_list} PREPEND "${_lf_aom_src}/")
endforeach()

set(_lf_aom_simd_build OFF)
if(_lf_x86 STREQUAL "x86_64" AND CMAKE_ASM_NASM_COMPILER_LOADED)
    set(_lf_aom_simd_build ON)
endif()

if(_lf_aom_simd_build)
    add_library(libfiles-aom STATIC ${_lf_aom_sources} "${_lf_aom_cmake}/config/aom_config.c" ${_lf_aom_sse2}
                                    ${_lf_aom_sse3} ${_lf_aom_ssse3} ${_lf_aom_sse4_1} ${_lf_aom_sse4_2} ${_lf_aom_avx}
                                    ${_lf_aom_avx2} ${_lf_aom_asm})
    target_include_directories(libfiles-aom PRIVATE "${_lf_aom_cmake}/x86")

    # MSVC compiles intrinsics up to SSE4.2 as they are, and AVX's only for /arch:AVX and above.
    if(MSVC)
        set_source_files_properties(${_lf_aom_avx} PROPERTIES COMPILE_OPTIONS "/arch:AVX")
        set_source_files_properties(${_lf_aom_avx2} PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(${_lf_aom_sse2} PROPERTIES COMPILE_OPTIONS "-msse2")
        set_source_files_properties(${_lf_aom_sse3} PROPERTIES COMPILE_OPTIONS "-msse3")
        set_source_files_properties(${_lf_aom_ssse3} PROPERTIES COMPILE_OPTIONS "-mssse3")
        set_source_files_properties(${_lf_aom_sse4_1} PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties(${_lf_aom_sse4_2} PROPERTIES COMPILE_OPTIONS "-msse4.2")
        set_source_files_properties(${_lf_aom_avx} PROPERTIES COMPILE_OPTIONS "-mavx")
        set_source_files_properties(${_lf_aom_avx2} PROPERTIES COMPILE_OPTIONS
            "-mavx2;-mno-avx256-split-unaligned-load;-mno-avx256-split-unaligned-store")
    endif()

    set_source_files_properties(${_lf_aom_asm} PROPERTIES COMPILE_FLAGS "-I${_lf_aom_src}/ -I${_lf_aom_cmake}/x86/")
else()
    add_library(libfiles-aom STATIC ${_lf_aom_sources} "${_lf_aom_cmake}/config/aom_config.c")
    target_include_directories(libfiles-aom PRIVATE "${_lf_aom_cmake}/generic")
endif()
add_library(libfiles::aom ALIAS libfiles-aom)

# Its public headers are aom/ under its root, as its consumers include them.
target_include_directories(libfiles-aom PUBLIC "${_lf_aom_src}")
target_include_directories(libfiles-aom PRIVATE "${_lf_aom_cmake}")

if(NOT WIN32)
    target_compile_definitions(libfiles-aom PRIVATE _LARGEFILE_SOURCE _FILE_OFFSET_BITS=64)
    find_package(Threads REQUIRED)
    target_link_libraries(libfiles-aom PUBLIC Threads::Threads)
endif()

set_target_properties(libfiles-aom PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# Upstream is not warning-clean, and it is not libfiles' to fix. Kept from nasm, where -w takes an argument and would
# swallow the next define.
target_compile_options(libfiles-aom PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)