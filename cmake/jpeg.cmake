# libjpeg-turbo for libfiles' JPEG decoder and for its consumers, fetched as the release archive at
# its SHA-256 (cmake/dd-dependencies.json) and built by this recipe. The source lists, the SIMD wiring
# and the configuration headers in cmake/libjpeg-turbo/ are Diffractor's, which shipped them; the
# fetched tree is upstream's own, unmodified. CMakeLists.txt includes this when LIBFILES_JPEG is on.
#
# The configuration headers are reached by a quoted include from jpeglib.h. The fetched tree holds
# none, so the quoted search falls through to the include path, where this recipe puts libfiles' own.
#
# The 8-bit library takes NASM SIMD on x86-64 and x86, the assembler found as cmake/nasm.cmake finds
# it. Without one, and on other architectures, the C paths are built - the same pixels, more slowly.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/nasm.cmake")

set(_lf_manifest "${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json")

set(_lf_jpeg_simd_dir "${_lf_x86}")

if(NOT CMAKE_ASM_NASM_COMPILER_LOADED)
    if(_lf_jpeg_simd_dir)
        message(STATUS "libfiles: no nasm for ${_lf_arch}, so libjpeg-turbo is built without SIMD")
    endif()
    set(_lf_jpeg_simd_dir "")
endif()

dd_dependency_arguments("${_lf_manifest}" libjpeg-turbo _lf_jpeg_acquisition)
FetchContent_Declare(libjpeg_turbo ${_lf_jpeg_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(libjpeg_turbo)

set(_lf_jpeg_src "${libjpeg_turbo_SOURCE_DIR}/src")
set(_lf_jpeg_simd "${libjpeg_turbo_SOURCE_DIR}/simd")

if(MSVC)
    set(_lf_jpeg_config "${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo/msvc")
else()
    set(_lf_jpeg_config "${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo/gcc")
endif()

# The 8-bit library, without the NEON, TurboJPEG and test sources.
set(_lf_jpeg_sources_8
    jaricom.c jcapimin.c jcapistd.c jcarith.c jccoefct.c jccolor.c jcdctmgr.c jcdiffct.c
    jchuff.c jcinit.c jclhuff.c jclossls.c jcmainct.c jcmarker.c jcmaster.c jcomapi.c
    jcparam.c jcphuff.c jcprepct.c jcsample.c jctrans.c jdapimin.c jdapistd.c jdarith.c
    jdatadst.c jdatasrc.c jdcoefct.c jdcolor.c jddctmgr.c jddiffct.c jdhuff.c jdicc.c
    jdinput.c jdlhuff.c jdlossls.c jdmainct.c jdmarker.c jdmaster.c jdmerge.c jdphuff.c
    jdpostct.c jdsample.c jdtrans.c jerror.c jfdctflt.c jfdctfst.c jfdctint.c jidctflt.c
    jidctfst.c jidctint.c jidctred.c jmemmgr.c jmemnobs.c jquant1.c jquant2.c jutils.c
    transupp.c)

# Compiled again at 12 and 16 bits, for lossless and high-precision JPEG.
set(_lf_jpeg_sources_12
    jcapistd.c jccoefct.c jccolor.c jcdctmgr.c jcdiffct.c jclossls.c jcmainct.c jcprepct.c
    jcsample.c jdapistd.c jdcoefct.c jdcolor.c jddctmgr.c jddiffct.c jdlossls.c jdmainct.c
    jdmerge.c jdpostct.c jdsample.c jfdctfst.c jfdctint.c jidctflt.c jidctfst.c jidctint.c
    jidctred.c jquant1.c jquant2.c jutils.c)

set(_lf_jpeg_sources_16
    jcapistd.c jccolor.c jcdiffct.c jclossls.c jcmainct.c jcprepct.c jcsample.c jdapistd.c
    jdcolor.c jddiffct.c jdlossls.c jdmainct.c jdpostct.c jdsample.c jutils.c)

list(TRANSFORM _lf_jpeg_sources_8 PREPEND "${_lf_jpeg_src}/")
list(TRANSFORM _lf_jpeg_sources_12 PREPEND "${_lf_jpeg_src}/")
list(TRANSFORM _lf_jpeg_sources_16 PREPEND "${_lf_jpeg_src}/")

add_library(libfiles-jpeg12 OBJECT ${_lf_jpeg_sources_12})
target_compile_definitions(libfiles-jpeg12 PRIVATE BITS_IN_JSAMPLE=12)

add_library(libfiles-jpeg16 OBJECT ${_lf_jpeg_sources_16})
target_compile_definitions(libfiles-jpeg16 PRIVATE BITS_IN_JSAMPLE=16)

set(_lf_jpeg_simd_sources "")

if(_lf_jpeg_simd_dir)
    file(GLOB _lf_jpeg_asm "${_lf_jpeg_simd}/${_lf_jpeg_simd_dir}/*.asm")

    # The *ext-*.asm files are %include fragments their parents assemble once per pixel format, with
    # RGB_RED and the rest defined first; assembled alone they fail on undefined symbols.
    list(FILTER _lf_jpeg_asm EXCLUDE REGEX "ext-(avx2|sse2|mmx)\\.asm$")
    set(_lf_jpeg_simd_sources ${_lf_jpeg_asm} "${_lf_jpeg_simd}/jsimd.c")

    # ELF, WIN64 and WIN32 choose jsimdext.inc's symbol decoration. On x86-64 WIN32 is undefined
    # rather than merely not passed: jsimdext.inc tests it before WIN64, and a WIN32 arriving from a
    # consumer's directory definitions would decorate every symbol with an underscore. On i386 that
    # underscore is right. -DPIC selects the i386 GOT paths, which x86-64 does not need.
    if(WIN32 AND _lf_jpeg_simd_dir STREQUAL "x86_64")
        set(_lf_jpeg_asm_abi "-D__x86_64__ -DWIN64 -UWIN32")
    elseif(WIN32)
        set(_lf_jpeg_asm_abi "-DWIN32")
    elseif(_lf_jpeg_simd_dir STREQUAL "x86_64")
        set(_lf_jpeg_asm_abi "-D__x86_64__ -DELF")
    else()
        set(_lf_jpeg_asm_abi "-DELF -DPIC")
    endif()

    set_source_files_properties(${_lf_jpeg_asm} PROPERTIES COMPILE_FLAGS
        "${_lf_jpeg_asm_abi} -I${_lf_jpeg_simd}/nasm/ -I${_lf_jpeg_simd}/${_lf_jpeg_simd_dir}/")
endif()

add_library(libfiles-jpeg STATIC
    ${_lf_jpeg_sources_8}
    ${_lf_jpeg_simd_sources}
    $<TARGET_OBJECTS:libfiles-jpeg12>
    $<TARGET_OBJECTS:libfiles-jpeg16>
)
add_library(libfiles::jpeg ALIAS libfiles-jpeg)

target_include_directories(libfiles-jpeg PUBLIC "${_lf_jpeg_src}" "${_lf_jpeg_config}" "${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo")
target_include_directories(libfiles-jpeg PRIVATE "${_lf_jpeg_simd}" "${_lf_jpeg_simd}/${_lf_jpeg_simd_dir}")

foreach(_lf_target IN ITEMS libfiles-jpeg12 libfiles-jpeg16)
    target_include_directories(${_lf_target} PRIVATE "${_lf_jpeg_src}" "${_lf_jpeg_config}" "${CMAKE_CURRENT_LIST_DIR}/libjpeg-turbo")
endforeach()

if(NOT _lf_jpeg_simd_dir)
    target_compile_definitions(libfiles-jpeg PUBLIC LIBFILES_JPEG_NO_SIMD)
    target_compile_definitions(libfiles-jpeg12 PRIVATE LIBFILES_JPEG_NO_SIMD)
    target_compile_definitions(libfiles-jpeg16 PRIVATE LIBFILES_JPEG_NO_SIMD)
endif()

set_target_properties(libfiles-jpeg libfiles-jpeg12 libfiles-jpeg16 PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)

# Upstream is not warning-clean, and it is not libfiles' to fix. Kept from nasm, where -w takes an
# argument and would swallow the next define.
foreach(_lf_target IN ITEMS libfiles-jpeg libfiles-jpeg12 libfiles-jpeg16)
    target_compile_options(${_lf_target} PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)
endforeach()

set(LIBFILES_JPEG_SIMD "${_lf_jpeg_simd_dir}" CACHE INTERNAL "The SIMD libjpeg-turbo was built with, or empty")
