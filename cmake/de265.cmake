# libde265, libheif's HEVC decoder, fetched as the release archive at its SHA-256
# (cmake/dd-dependencies.json) and built by this recipe as libfiles::de265. The source list and the
# headers its own build generates - config.h and de265-version.h, in cmake/libde265/ - are Diffractor's,
# which shipped them; the fetched tree is upstream's own, unmodified. heif.cmake includes this.
#
# config.h turns on the SSE4.1 paths for MSVC on x86 and x64 only, as Diffractor's did, so a GCC build
# runs libde265's C. The SSE sources are built on x86 alone, and Win32's stand-ins in extra/ - condition
# variables and getopt - on Windows alone.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arch.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" libde265 _lf_de265_acquisition)
FetchContent_Declare(libde265 ${_lf_de265_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(libde265)

set(_lf_de265_src "${libde265_SOURCE_DIR}")

set(_lf_de265_sources
    libde265/alloc_pool.cc libde265/bitstream.cc libde265/cabac.cc libde265/contextmodel.cc
    libde265/de265.cc libde265/deblock.cc libde265/decctx.cc libde265/dpb.cc libde265/fallback-dct.cc
    libde265/fallback-deblk.cc libde265/fallback-intrapred.cc libde265/fallback-motion.cc
    libde265/fallback.cc libde265/image-io.cc libde265/image.cc libde265/intrapred.cc libde265/md5.cc
    libde265/motion.cc libde265/nal-parser.cc libde265/nal.cc libde265/pps.cc libde265/quality.cc
    libde265/refpic.cc libde265/sao.cc libde265/scan.cc libde265/sei.cc libde265/slice.cc
    libde265/sps.cc libde265/threads.cc libde265/transform.cc libde265/util.cc libde265/visualize.cc
    libde265/vps.cc libde265/vui.cc libde265)
list(TRANSFORM _lf_de265_sources PREPEND "${_lf_de265_src}/")

set(_lf_de265_x86
    libde265/x86/sse-dct.cc libde265/x86/sse-deblk.cc libde265/x86/sse-intrapred.cc
    libde265/x86/sse-motion.cc libde265/x86/sse.cc)
list(TRANSFORM _lf_de265_x86 PREPEND "${_lf_de265_src}/")

set(_lf_de265_windows
    extra/getopt.c extra/getopt_long.c extra/win32cond.c)
list(TRANSFORM _lf_de265_windows PREPEND "${_lf_de265_src}/")

if(_lf_x86)
    list(APPEND _lf_de265_sources ${_lf_de265_x86})
endif()

if(WIN32)
    list(APPEND _lf_de265_sources ${_lf_de265_windows})
endif()

add_library(libfiles-de265 STATIC ${_lf_de265_sources})
add_library(libfiles::de265 ALIAS libfiles-de265)

# Its generated headers first: "config.h" from the sources' own directory falls through to the include
# path, and <libde265/de265-version.h> is found beside de265.h.
target_include_directories(libfiles-de265 PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/libde265" "${_lf_de265_src}")
target_include_directories(libfiles-de265 PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/libde265/libde265" "${_lf_de265_src}/libde265")
target_compile_definitions(libfiles-de265 PRIVATE HAVE_CONFIG_H)
target_compile_definitions(libfiles-de265 PUBLIC LIBDE265_STATIC_BUILD)

if(WIN32)
    target_compile_definitions(libfiles-de265 PRIVATE UNICODE _UNICODE)
endif()

# MSVC compiles an intrinsic on the baseline; GCC and Clang need the instruction set enabled. image.cc
# picks its aligned allocator from MinGW, MSVC, posix_memalign, then plain memalign, which glibc no
# longer declares.
if(NOT MSVC)
    if(_lf_x86)
        set_source_files_properties(${_lf_de265_x86} PROPERTIES COMPILE_OPTIONS "-mssse3;-msse4.1")
    endif()
    target_compile_definitions(libfiles-de265 PRIVATE HAVE_POSIX_MEMALIGN)
endif()

find_package(Threads REQUIRED)
target_link_libraries(libfiles-de265 PUBLIC Threads::Threads)

set_target_properties(libfiles-de265 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-de265 PRIVATE -w)
