# NASM for the codecs whose x86 SIMD is assembly - libjpeg-turbo's and dav1d's - on x86-64 and x86. A
# consumer that has enabled ASM_NASM already lends its assembler; otherwise one on the path is used,
# and on Windows, where none usually is, the release build pinned in the manifest is fetched. Afterwards
# CMAKE_ASM_NASM_COMPILER_LOADED says whether there is one. Included by each recipe that assembles.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/arch.cmake")

if(_lf_x86 AND NOT CMAKE_ASM_NASM_COMPILER_LOADED)
    find_program(LIBFILES_NASM_EXECUTABLE nasm DOC "The nasm assembler for the codecs' x86 SIMD")

    if(NOT LIBFILES_NASM_EXECUTABLE AND CMAKE_HOST_WIN32)
        dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" nasm _lf_nasm_acquisition)
        FetchContent_Declare(libfiles_nasm ${_lf_nasm_acquisition} SOURCE_SUBDIR "no-cmake-project")
        FetchContent_MakeAvailable(libfiles_nasm)
        set(LIBFILES_NASM_EXECUTABLE "${libfiles_nasm_SOURCE_DIR}/nasm.exe" CACHE FILEPATH
            "The nasm assembler for the codecs' x86 SIMD" FORCE)
    endif()

    if(LIBFILES_NASM_EXECUTABLE)
        set(CMAKE_ASM_NASM_COMPILER "${LIBFILES_NASM_EXECUTABLE}")

        if(WIN32 AND _lf_x86 STREQUAL "x86_64")
            set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
        elseif(WIN32)
            set(CMAKE_ASM_NASM_OBJECT_FORMAT win32)
        elseif(_lf_x86 STREQUAL "x86_64")
            set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
        else()
            set(CMAKE_ASM_NASM_OBJECT_FORMAT elf32)
        endif()

        enable_language(ASM_NASM)
    endif()
endif()
