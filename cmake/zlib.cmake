# zlib-ng, built zlib-compatible, for libpng and for libfiles' consumers: fetched as the release archive
# at its SHA-256 (cmake/dd-dependencies.json) and built by this recipe. The source list, the definitions
# and the three headers zlib-ng's own build generates - zlib.h, zconf.h and zlib_name_mangling.h, in
# cmake/zlib-ng/ - are Diffractor's, which shipped them; the fetched tree is upstream's own, unmodified.
# CMakeLists.txt includes this when LIBFILES_PNG is on.
#
# The generated headers are reached by a quoted include from the sources. The fetched tree holds only
# their templates, so the quoted search falls through to the include path, where this recipe puts
# libfiles' own. gzread.c is left out, as Diffractor left it: nothing reads a gzip file through zlib.
#
# zlib-ng compiles one source per instruction set and dispatches among them at run time. The x86
# sources are built on x86-64 and x86; on other architectures the generic C serves.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arch.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" zlib-ng _lf_zlib_acquisition)
FetchContent_Declare(zlib_ng ${_lf_zlib_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(zlib_ng)

set(_lf_zlib_src "${zlib_ng_SOURCE_DIR}")

set(_lf_zlib_sources
    adler32.c compress.c cpu_features.c crc32.c crc32_braid_comb.c deflate.c deflate_fast.c
    deflate_huff.c deflate_medium.c deflate_quick.c deflate_rle.c deflate_slow.c deflate_stored.c
    functable.c gzlib.c gzwrite.c infback.c inflate.c inftrees.c insert_string.c insert_string_roll.c
    trees.c uncompr.c zutil.c
    arch/generic/adler32_c.c arch/generic/adler32_fold_c.c arch/generic/chunkset_c.c
    arch/generic/compare256_c.c arch/generic/crc32_braid_c.c arch/generic/crc32_chorba_c.c
    arch/generic/crc32_fold_c.c arch/generic/slide_hash_c.c)

set(_lf_zlib_definitions
    ZLIB_COMPAT WITH_ALL_FALLBACKS WITH_GZFILEOP NO_FSEEKO HAVE_BUILTIN_ASSUME_ALIGNED WITH_OPTIM)

set(_lf_zlib_x86_sources "")

if(_lf_x86)
    set(_lf_zlib_x86_sources
        adler32_avx2.c adler32_avx512.c adler32_avx512_vnni.c adler32_sse42.c adler32_ssse3.c
        chorba_sse2.c chorba_sse41.c chunkset_avx2.c chunkset_avx512.c chunkset_sse2.c chunkset_ssse3.c
        compare256_avx2.c compare256_avx512.c compare256_sse2.c crc32_pclmulqdq.c crc32_vpclmulqdq.c
        slide_hash_avx2.c slide_hash_sse2.c x86_features.c)
    list(TRANSFORM _lf_zlib_x86_sources PREPEND "${_lf_zlib_src}/arch/x86/")

    list(APPEND _lf_zlib_definitions
        X86_FEATURES X86_HAVE_XSAVE_INTRIN X86_SSE2 X86_SSSE3 X86_SSE41 X86_SSE42 X86_PCLMULQDQ_CRC
        X86_AVX2 X86_AVX512 X86_AVX512VNNI X86_VPCLMULQDQ_CRC)

    # x86_features.c reads the CPU through <intrin.h> for MSVC and <cpuid.h> for the GNU compilers.
    if(MSVC)
        list(APPEND _lf_zlib_definitions HAVE_CPUID_MS)
    else()
        list(APPEND _lf_zlib_definitions HAVE_CPUID_GNU)
    endif()
endif()

# MSVC has no __builtin_ctz, which fallback_builtins.h exists to supply; the GNU compilers do, and
# several arch functions are declared only when they say so.
if(NOT MSVC)
    list(APPEND _lf_zlib_definitions HAVE_BUILTIN_CTZ HAVE_BUILTIN_CTZLL)
endif()

if(WIN32)
    list(APPEND _lf_zlib_definitions UNICODE _UNICODE)
endif()

list(TRANSFORM _lf_zlib_sources PREPEND "${_lf_zlib_src}/")

add_library(libfiles-zlib STATIC ${_lf_zlib_sources} ${_lf_zlib_x86_sources})
add_library(libfiles::zlib ALIAS libfiles-zlib)

target_include_directories(libfiles-zlib PUBLIC "${CMAKE_CURRENT_LIST_DIR}/zlib-ng" "${_lf_zlib_src}")
target_compile_definitions(libfiles-zlib PRIVATE ${_lf_zlib_definitions})
set_target_properties(libfiles-zlib PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-zlib PRIVATE -w)

# MSVC compiles an intrinsic on the baseline; GCC and Clang refuse to inline one into code that has not
# enabled its instruction set, so each x86 source is built for the set its name gives. AVX-512 hardware
# always has BMI2, but the compiler still wants to be told: chunkset_avx512.c calls _bzhi_u32.
if(_lf_x86 AND NOT MSVC)
    set(_lf_zlib_avx512 -mavx512f -mavx512dq -mavx512bw -mavx512vl -mbmi2)

    foreach(_lf_source IN LISTS _lf_zlib_x86_sources)
        get_filename_component(_lf_name "${_lf_source}" NAME)
        set(_lf_flags "")

        if(_lf_name MATCHES "vpclmulqdq")
            set(_lf_flags ${_lf_zlib_avx512} -mvpclmulqdq -mpclmul)
        elseif(_lf_name MATCHES "avx512_vnni")
            set(_lf_flags ${_lf_zlib_avx512} -mavx512vnni)
        elseif(_lf_name MATCHES "avx512")
            set(_lf_flags ${_lf_zlib_avx512})
        elseif(_lf_name MATCHES "avx2")
            set(_lf_flags -mavx2)
        elseif(_lf_name MATCHES "pclmulqdq")
            set(_lf_flags -mpclmul -msse4.1)
        elseif(_lf_name MATCHES "sse42")
            set(_lf_flags -msse4.2)
        elseif(_lf_name MATCHES "sse41")
            set(_lf_flags -msse4.1)
        elseif(_lf_name MATCHES "ssse3")
            set(_lf_flags -mssse3)
        elseif(_lf_name MATCHES "sse2")
            set(_lf_flags -msse2)
        elseif(_lf_name STREQUAL "x86_features.c")
            # X86_HAVE_XSAVE_INTRIN makes the feature check itself call _xgetbv.
            set(_lf_flags -mxsave)
        endif()

        if(_lf_flags)
            set_source_files_properties("${_lf_source}" PROPERTIES COMPILE_OPTIONS "${_lf_flags}")
        endif()
    endforeach()
endif()
