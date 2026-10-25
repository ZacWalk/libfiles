# The architecture libfiles is being built FOR, for the codec recipes that choose SIMD sources by it:
# _lf_arch, in lower case, and _lf_x86 - x86_64, i386, or empty for any other. CMAKE_SYSTEM_PROCESSOR
# answers for the host on Windows, so a Win32 or ARM64 build on an x64 machine would read as x64; MSVC
# states the compiler's own. Included by each recipe, in the scope that includes the recipe.

enable_language(C)

if(MSVC AND CMAKE_VS_PLATFORM_NAME)
    set(_lf_arch "${CMAKE_VS_PLATFORM_NAME}")
elseif(MSVC AND CMAKE_C_COMPILER_ARCHITECTURE_ID)
    set(_lf_arch "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
else()
    set(_lf_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_lf_arch}" _lf_arch)

set(_lf_x86 "")
if(_lf_arch MATCHES "^(x64|amd64|x86_64)$")
    set(_lf_x86 "x86_64")
elseif(_lf_arch MATCHES "^(win32|x86|i[3-6]86)$")
    set(_lf_x86 "i386")
endif()
