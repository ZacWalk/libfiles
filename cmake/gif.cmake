# giflib's portable library, from the same pinned archive ImageWalker used.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")
enable_language(C)

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" giflib _lf_gif_acquisition)
FetchContent_Declare(giflib ${_lf_gif_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(giflib)
set(_lf_gif_sources dgif_lib.c egif_lib.c gifalloc.c gif_err.c gif_font.c gif_hash.c
    openbsd-reallocarray.c quantize.c)
list(TRANSFORM _lf_gif_sources PREPEND "${giflib_SOURCE_DIR}/")
add_library(libfiles-gif STATIC ${_lf_gif_sources})
add_library(libfiles::gif ALIAS libfiles-gif)
target_include_directories(libfiles-gif SYSTEM PUBLIC "${giflib_SOURCE_DIR}")
target_compile_definitions(libfiles-gif PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(libfiles-gif PRIVATE -w)
set_target_properties(libfiles-gif PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
