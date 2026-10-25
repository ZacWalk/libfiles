# Shim. libtiff calls find_package(ZLIB); point it at the zlib-ng libfiles builds, zlib-compatible,
# instead of letting it look for an installed copy.
if(NOT TARGET ZLIB::ZLIB)
    message(FATAL_ERROR "The TIFF recipe requires libfiles-zlib")
endif()

get_target_property(ZLIB_INCLUDE_DIR libfiles-zlib INTERFACE_INCLUDE_DIRECTORIES)
set(ZLIB_FOUND TRUE)
set(ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR})
set(ZLIB_LIBRARY ZLIB::ZLIB)
set(ZLIB_LIBRARIES ZLIB::ZLIB)
# zlib-ng's compatible API is zlib 1.3.1's.
set(ZLIB_VERSION 1.3.1)
set(ZLIB_VERSION_STRING 1.3.1)
