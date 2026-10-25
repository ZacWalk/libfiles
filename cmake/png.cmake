# libpng, for libfiles' PNG decoder and for its consumers: fetched as the release archive at its SHA-256
# (cmake/dd-dependencies.json) and built by this recipe against libfiles' zlib. pnglibconf.h, which
# libpng's own build generates, is Diffractor's, in cmake/libpng/; the fetched tree is upstream's own,
# unmodified. CMakeLists.txt includes this when LIBFILES_PNG is on.
#
# Built as Diffractor built it: in C alone. libpng's SIMD filters are opt-in - Intel's behind
# PNG_INTEL_SSE, which Diffractor never defined - and NEON is turned off rather than left to a
# compiler that announces it, so every architecture runs the code Diffractor shipped. pngtest.c, a test
# program, is not built.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/zlib.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" libpng _lf_png_acquisition)
FetchContent_Declare(libpng ${_lf_png_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(libpng)

set(_lf_png_src "${libpng_SOURCE_DIR}")

set(_lf_png_sources
    png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c pngrtran.c pngrutil.c pngset.c
    pngtrans.c pngwio.c pngwrite.c pngwtran.c pngwutil.c)
list(TRANSFORM _lf_png_sources PREPEND "${_lf_png_src}/")

add_library(libfiles-png STATIC ${_lf_png_sources})
add_library(libfiles::png ALIAS libfiles-png)

target_include_directories(libfiles-png PUBLIC "${CMAKE_CURRENT_LIST_DIR}/libpng" "${_lf_png_src}")
target_link_libraries(libfiles-png PUBLIC libfiles::zlib)
target_compile_definitions(libfiles-png PRIVATE PNG_ARM_NEON_OPT=0)

if(WIN32)
    target_compile_definitions(libfiles-png PRIVATE UNICODE _UNICODE)
endif()

set_target_properties(libfiles-png PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-png PRIVATE -w)
