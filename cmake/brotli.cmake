# Brotli - its C decoder and encoder - for libfiles' JPEG XL support and for its consumers: fetched as
# the release archive at its SHA-256 (cmake/dd-dependencies.json) and built by this recipe as
# libfiles::brotli. The source list is Diffractor's, which shipped it: every source of c/common, c/dec and
# c/enc. The fetched tree is upstream's own, unmodified. cmake/jxl.cmake includes this.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")

enable_language(C)

dd_dependency_arguments("${CMAKE_CURRENT_LIST_DIR}/dd-dependencies.json" brotli _lf_brotli_acquisition)
FetchContent_Declare(brotli ${_lf_brotli_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(brotli)

set(_lf_brotli_src "${brotli_SOURCE_DIR}/c")

set(_lf_brotli_sources
    common/constants.c common/context.c common/dictionary.c common/platform.c common/shared_dictionary.c
    common/transform.c dec/bit_reader.c dec/decode.c dec/huffman.c dec/prefix.c dec/state.c
    dec/static_init.c enc/backward_references.c enc/backward_references_hq.c enc/bit_cost.c
    enc/block_splitter.c enc/brotli_bit_stream.c enc/cluster.c enc/command.c enc/compound_dictionary.c
    enc/compress_fragment.c enc/compress_fragment_two_pass.c enc/dictionary_hash.c enc/encode.c
    enc/encoder_dict.c enc/entropy_encode.c enc/fast_log.c enc/histogram.c enc/literal_cost.c
    enc/memory.c enc/metablock.c enc/static_dict.c enc/static_dict_lut.c enc/static_init.c
    enc/utf8_util.c)
list(TRANSFORM _lf_brotli_sources PREPEND "${_lf_brotli_src}/")

add_library(libfiles-brotli STATIC ${_lf_brotli_sources})
add_library(libfiles::brotli ALIAS libfiles-brotli)

target_include_directories(libfiles-brotli PUBLIC "${_lf_brotli_src}/include")

if(WIN32)
    target_compile_definitions(libfiles-brotli PRIVATE UNICODE _UNICODE)
endif()

set_target_properties(libfiles-brotli PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Upstream is not warning-clean, and it is not libfiles' to fix.
target_compile_options(libfiles-brotli PRIVATE -w)
