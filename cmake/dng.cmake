# Adobe's DNG SDK, for LibRaw's DNG decoding - lossy, JPEG XL and other DNGs LibRaw does not read alone -
# and for libfiles' consumers: fetched as Adobe's release archive at its SHA-256 (cmake/dd-dependencies.json)
# and built by this recipe as libfiles::dng, from the source list Diffractor built, which is every source
# of dng_sdk/source - but one. The SDK's XMP backend, dng_xmp_sdk.cpp, is written over the XMP Toolkit;
# src/dng_xmp_sdk.cpp implements the same dng_xmp_sdk.h over libfiles' XMP data model, and is built in its
# place, so the SDK needs no toolkit. The archive's own copies of libjpeg, libjxl and the toolkit are not
# built. RawEnvironment.h, the platform switches the SDK leaves to the application, is Diffractor's, in
# cmake/dng/. The fetched tree is upstream's own, unmodified. cmake/raw.cmake includes this.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/zlib.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/jxl.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" dngsdk _lf_dng_acquisition)
FetchContent_Declare(dngsdk ${_lf_dng_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(dngsdk)

set(_lf_dng_src "${dngsdk_SOURCE_DIR}/dng_sdk/source")

set(_lf_dng_sources
    dng_1d_function.cpp dng_1d_table.cpp dng_abort_sniffer.cpp dng_area_task.cpp dng_bad_pixels.cpp
    dng_big_table.cpp dng_bmff.cpp dng_bottlenecks.cpp dng_camera_profile.cpp dng_color_space.cpp
    dng_color_spec.cpp dng_date_time.cpp dng_exceptions.cpp dng_exif.cpp dng_file_stream.cpp
    dng_filter_task.cpp dng_fingerprint.cpp dng_gain_map.cpp dng_globals.cpp dng_host.cpp
    dng_hue_sat_map.cpp dng_ifd.cpp dng_image.cpp dng_image_writer.cpp dng_info.cpp dng_iptc.cpp
    dng_jpeg_image.cpp dng_jpeg_memory_source.cpp dng_jxl.cpp dng_lens_correction.cpp
    dng_linearization_info.cpp dng_local_string.cpp dng_lossless_jpeg.cpp dng_lossless_jpeg_shared.cpp
    dng_matrix.cpp dng_memory.cpp dng_memory_stream.cpp dng_misc_opcodes.cpp dng_mosaic_info.cpp
    dng_mutex.cpp dng_negative.cpp dng_opcode_list.cpp dng_opcodes.cpp dng_orientation.cpp
    dng_parse_utils.cpp dng_pixel_buffer.cpp dng_point.cpp dng_preview.cpp dng_pthread.cpp
    dng_rational.cpp dng_read_image.cpp dng_rect.cpp dng_ref_counted_block.cpp dng_reference.cpp
    dng_render.cpp dng_resample.cpp dng_safe_arithmetic.cpp dng_shared.cpp dng_simple_image.cpp
    dng_spline.cpp dng_stream.cpp dng_string.cpp dng_string_list.cpp dng_tag_types.cpp
    dng_temperature.cpp dng_tile_iterator.cpp dng_tone_curve.cpp dng_update_meta.cpp dng_utils.cpp
    dng_validate.cpp dng_xmp.cpp dng_xy_coord.cpp)
list(TRANSFORM _lf_dng_sources PREPEND "${_lf_dng_src}/")

add_library(libfiles-dng STATIC ${_lf_dng_sources} "${CMAKE_CURRENT_LIST_DIR}/../src/dng_xmp_sdk.cpp")
add_library(libfiles::dng ALIAS libfiles-dng)

# Its headers are not warning-clean either - dng_safe_arithmetic.h multiplies in __int128, which GCC's
# -Wpedantic refuses outside a system header - so they reach consumers as system headers.
target_include_directories(libfiles-dng SYSTEM PUBLIC "${_lf_dng_src}" "${CMAKE_CURRENT_LIST_DIR}/dng")
target_link_libraries(libfiles-dng PUBLIC libfiles::zlib libfiles::jxl)
# The XMP backend reads and writes through libfiles' XMP data model.
target_link_libraries(libfiles-dng PRIVATE libfiles::libfiles)

# The backend has no XMPFiles. The SDK uses XMPFiles only to start and stop it, in InitializeSDK and
# TerminateSDK, which only its own dng_validate tool calls; dng_flags.h guards the flag with #ifndef, so
# naming it here is enough.
target_compile_definitions(libfiles-dng PRIVATE qDNGXMPFiles=0)

# Nor does it have the toolkit's document operations, so the SDK's document-history support cannot be
# built. RawEnvironment.h switches it off in its Windows branch alone; elsewhere it is named here.
if(NOT WIN32)
    target_compile_definitions(libfiles-dng PRIVATE qDNGXMPDocOps=0)
endif()

if(WIN32)
    target_compile_definitions(libfiles-dng PRIVATE UNICODE _UNICODE)
endif()

target_compile_features(libfiles-dng PUBLIC cxx_std_20)
set_target_properties(libfiles-dng PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Upstream is not warning-clean, and it is not libfiles' to fix. The backend is built as part of the SDK,
# as it always was, under its warnings.
target_compile_options(libfiles-dng PRIVATE -w)
