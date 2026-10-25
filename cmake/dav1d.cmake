# dav1d, libheif's AV1 decoder and FFmpeg's, fetched as the release archive at its SHA-256
# (cmake/dd-dependencies.json) and built by this recipe as libfiles::dav1d. The source list, the
# configuration its own build generates and the four files that compile its templates at 8 and 16 bits -
# all in cmake/dav1d/ - are Diffractor's, which shipped them; the fetched tree is upstream's own,
# unmodified. heif.cmake includes this.
#
# On x86-64 and x86 with NASM, its assembly is built with Diffractor's configuration: cmake/dav1d/x86/
# config.h, which reads _M_AMD64 and so takes the 32-bit C paths beside 64-bit assembly under GCC, and
# without unistd.h cannot count processors there, so decodes on one thread, as Diffractor's build did;
# and a config.asm for Windows' object formats or for ELF. Elsewhere, and without NASM,
# cmake/dav1d/generic/config.h builds its C alone.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/nasm.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" dav1d _lf_dav1d_acquisition)
FetchContent_Declare(dav1d ${_lf_dav1d_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(dav1d)

set(_lf_dav1d_src "${dav1d_SOURCE_DIR}")
set(_lf_dav1d_cmake "${CMAKE_CURRENT_LIST_DIR}/dav1d")

set(_lf_dav1d_sources
    src/cdf.c src/cpu.c src/ctx.c src/data.c src/decode.c src/dequant_tables.c src/getbits.c
    src/intra_edge.c src/itx_1d.c src/lf_mask.c src/lib.c src/log.c src/mem.c src/msac.c src/obu.c
    src/pal.c src/picture.c src/qm.c src/ref.c src/refmvs.c src/scan.c src/tables.c src/thread_task.c
    src/warpmv.c src/wedge.c src/win32/thread.c include)
list(TRANSFORM _lf_dav1d_sources PREPEND "${_lf_dav1d_src}/")

set(_lf_dav1d_x86
    src/x86/cpu.c src/x86/cdef16_avx2.asm src/x86/cdef16_avx512.asm src/x86/cdef16_sse.asm
    src/x86/cdef_avx2.asm src/x86/cdef_avx512.asm src/x86/cdef_sse.asm src/x86/cpuid.asm
    src/x86/filmgrain16_avx2.asm src/x86/filmgrain16_avx512.asm src/x86/filmgrain16_sse.asm
    src/x86/filmgrain_avx2.asm src/x86/filmgrain_avx512.asm src/x86/filmgrain_sse.asm
    src/x86/ipred16_avx2.asm src/x86/ipred16_avx512.asm src/x86/ipred16_sse.asm src/x86/ipred_avx2.asm
    src/x86/ipred_avx512.asm src/x86/ipred_sse.asm src/x86/itx16_avx2.asm src/x86/itx16_avx512.asm
    src/x86/itx16_sse.asm src/x86/itx_avx2.asm src/x86/itx_avx512.asm src/x86/itx_sse.asm
    src/x86/loopfilter16_avx2.asm src/x86/loopfilter16_avx512.asm src/x86/loopfilter16_sse.asm
    src/x86/loopfilter_avx2.asm src/x86/loopfilter_avx512.asm src/x86/loopfilter_sse.asm
    src/x86/looprestoration16_avx2.asm src/x86/looprestoration16_avx512.asm
    src/x86/looprestoration16_sse.asm src/x86/looprestoration_avx2.asm
    src/x86/looprestoration_avx512.asm src/x86/looprestoration_sse.asm src/x86/mc16_avx2.asm
    src/x86/mc16_avx512.asm src/x86/mc16_sse.asm src/x86/mc_avx2.asm src/x86/mc_avx512.asm
    src/x86/mc_sse.asm src/x86/msac.asm src/x86/pal.asm src/x86/refmvs.asm src/x86/cdef16_avx2.asm
    src/x86/cdef16_avx512.asm src/x86/cdef16_sse.asm src/x86/cdef_avx2.asm src/x86/cdef_avx512.asm
    src/x86/cdef_sse.asm src/x86/cpuid.asm src/x86/filmgrain16_avx2.asm src/x86/filmgrain16_avx512.asm
    src/x86/filmgrain16_sse.asm src/x86/filmgrain_avx2.asm src/x86/filmgrain_avx512.asm
    src/x86/filmgrain_sse.asm src/x86/ipred16_avx2.asm src/x86/ipred16_avx512.asm
    src/x86/ipred16_sse.asm src/x86/ipred_avx2.asm src/x86/ipred_avx512.asm src/x86/ipred_sse.asm
    src/x86/itx16_avx2.asm src/x86/itx16_avx512.asm src/x86/itx16_sse.asm src/x86/itx_avx2.asm
    src/x86/itx_avx512.asm src/x86/itx_sse.asm src/x86/loopfilter16_avx2.asm
    src/x86/loopfilter16_avx512.asm src/x86/loopfilter16_sse.asm src/x86/loopfilter_avx2.asm
    src/x86/loopfilter_avx512.asm src/x86/loopfilter_sse.asm src/x86/looprestoration16_avx2.asm
    src/x86/looprestoration16_avx512.asm src/x86/looprestoration16_sse.asm
    src/x86/looprestoration_avx2.asm src/x86/looprestoration_avx512.asm src/x86/looprestoration_sse.asm
    src/x86/mc16_avx2.asm src/x86/mc16_avx512.asm src/x86/mc16_sse.asm src/x86/mc_avx2.asm
    src/x86/mc_avx512.asm src/x86/mc_sse.asm src/x86/msac.asm src/x86/pal.asm src/x86/refmvs.asm)
list(TRANSFORM _lf_dav1d_x86 PREPEND "${_lf_dav1d_src}/")

set(_lf_dav1d_templates
    tmpl16.c tmpl16b.c tmpl8.c tmpl8b.c)
list(TRANSFORM _lf_dav1d_templates PREPEND "${_lf_dav1d_cmake}/")

set(_lf_dav1d_asm_build OFF)
if(_lf_x86 AND CMAKE_ASM_NASM_COMPILER_LOADED)
    set(_lf_dav1d_asm_build ON)
endif()

if(_lf_dav1d_asm_build)
    add_library(libfiles-dav1d STATIC ${_lf_dav1d_sources} ${_lf_dav1d_templates} ${_lf_dav1d_x86})
    target_include_directories(libfiles-dav1d PRIVATE "${_lf_dav1d_cmake}/x86")
else()
    add_library(libfiles-dav1d STATIC ${_lf_dav1d_sources} ${_lf_dav1d_templates})
    target_include_directories(libfiles-dav1d PRIVATE "${_lf_dav1d_cmake}/generic")
endif()
add_library(libfiles::dav1d ALIAS libfiles-dav1d)

target_include_directories(libfiles-dav1d PUBLIC "${_lf_dav1d_src}/include")
target_include_directories(libfiles-dav1d PRIVATE "${_lf_dav1d_cmake}" "${_lf_dav1d_src}")

# The template files include each template by its name alone, from dav1d's src/.
set_source_files_properties(${_lf_dav1d_templates} PROPERTIES INCLUDE_DIRECTORIES "${_lf_dav1d_src}/src")

if(_lf_dav1d_asm_build)
    if(WIN32)
        set(_lf_dav1d_asm_config "${_lf_dav1d_cmake}/asm-windows")
    else()
        set(_lf_dav1d_asm_config "${_lf_dav1d_cmake}/asm-elf")
    endif()

    set(_lf_dav1d_asm ${_lf_dav1d_x86})
    list(FILTER _lf_dav1d_asm INCLUDE REGEX "\\.asm$")
    set_source_files_properties(${_lf_dav1d_asm} PROPERTIES COMPILE_FLAGS
        "-I${_lf_dav1d_asm_config}/ -I${_lf_dav1d_src}/src/x86/ -I${_lf_dav1d_src}/ -I${_lf_dav1d_src}/src/")
endif()

if(WIN32)
    target_compile_definitions(libfiles-dav1d PRIVATE $<$<COMPILE_LANGUAGE:C>:UNICODE> $<$<COMPILE_LANGUAGE:C>:_UNICODE>)
endif()

if(MSVC)
    target_compile_options(libfiles-dav1d PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()

find_package(Threads REQUIRED)
target_link_libraries(libfiles-dav1d PUBLIC Threads::Threads)

set_target_properties(libfiles-dav1d PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# Upstream is not warning-clean, and it is not libfiles' to fix. Kept from nasm, where -w takes an
# argument and would swallow the next define.
target_compile_options(libfiles-dav1d PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)
