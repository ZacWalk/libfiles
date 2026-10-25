# skcms, Skia's colour management, for libjxl's colour transforms and for libfiles' consumers: fetched at
# its pinned commit (cmake/dd-dependencies.json) and built by this recipe as libfiles::skcms. skcms makes
# no releases, and Skia's generated archives differ from one download to the next, so it is the one
# dependency pinned by commit rather than by an archive's SHA-256. The source list is Diffractor's, which
# shipped it; the fetched tree is upstream's own, unmodified. cmake/jxl.cmake includes this.
#
# skcms compiles one translation unit per instruction set - the baseline, Haswell's AVX2 and Skylake's
# AVX-512 - and chooses among them at run time. MSVC lets the intrinsics through on the baseline; GCC and
# Clang refuse to inline an intrinsic into a function whose target has not enabled it, so each file is
# told what it is for.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" skcms _lf_skcms_acquisition)
FetchContent_Declare(skcms ${_lf_skcms_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(skcms)

set(_lf_skcms_src "${skcms_SOURCE_DIR}")

set(_lf_skcms_sources
    skcms.cc src/skcms_TransformBaseline.cc src/skcms_TransformHsw.cc src/skcms_TransformSkx.cc)
list(TRANSFORM _lf_skcms_sources PREPEND "${_lf_skcms_src}/")

add_library(libfiles-skcms STATIC ${_lf_skcms_sources})
add_library(libfiles::skcms ALIAS libfiles-skcms)

# Its header uses a nameless struct, which a consumer warning at /W4 or -Wpedantic would hear of.
target_include_directories(libfiles-skcms SYSTEM PUBLIC "${_lf_skcms_src}")

if(WIN32)
    target_compile_definitions(libfiles-skcms PRIVATE UNICODE _UNICODE)
endif()

if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86|x86)$")
    set_source_files_properties("${_lf_skcms_src}/src/skcms_TransformHsw.cc"
        PROPERTIES COMPILE_OPTIONS "-mavx2;-mf16c;-mfma")
    set_source_files_properties("${_lf_skcms_src}/src/skcms_TransformSkx.cc"
        PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512dq;-mavx512cd;-mavx512bw;-mavx512vl")
endif()

target_compile_features(libfiles-skcms PUBLIC cxx_std_17)
set_target_properties(libfiles-skcms PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-skcms PRIVATE -w)
