# Shim. libtiff calls find_package(JPEG); point it at the libjpeg-turbo libfiles builds instead of
# letting it look for an installed copy. libtiff's own JPEG support stays off.
if(NOT TARGET JPEG::JPEG)
    set(JPEG_FOUND FALSE)
    return()
endif()

get_target_property(JPEG_INCLUDE_DIR libfiles-jpeg INTERFACE_INCLUDE_DIRECTORIES)
set(JPEG_FOUND TRUE)
set(JPEG_INCLUDE_DIRS ${JPEG_INCLUDE_DIR})
set(JPEG_LIBRARY JPEG::JPEG)
set(JPEG_LIBRARIES JPEG::JPEG)
# The libjpeg API version, as CMake's own FindJPEG reports it.
set(JPEG_VERSION 80)
