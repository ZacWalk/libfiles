# Purpose: Keep archive pin updates rebuildable: dd hands every archive's recipe the extraction time rather
# than the archive's own file times, so a pin change rebuilds what was compiled from the old archive.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../.dd/dependencies.cmake")

set(manifest "${CMAKE_CURRENT_LIST_DIR}/../cmake/dd-dependencies.json")
file(READ "${manifest}" declarations)
string(JSON count LENGTH "${declarations}" dependencies)
math(EXPR last "${count} - 1")
set(archives 0)

foreach(dependency RANGE 0 ${last})
    string(JSON name MEMBER "${declarations}" dependencies ${dependency})
    string(JSON digest ERROR_VARIABLE not_archive GET "${declarations}" dependencies "${name}" sha256)
    dd_dependency_arguments("${manifest}" "${name}" arguments)
    list(FIND arguments DOWNLOAD_EXTRACT_TIMESTAMP timestamp)

    if(not_archive)
        if(timestamp GREATER_EQUAL 0)
            message(FATAL_ERROR "${name}: a commit is not an archive, yet it has an extraction policy")
        endif()
        continue()
    endif()

    if(timestamp LESS 0)
        message(FATAL_ERROR "${name}: the archive has no extraction policy")
    endif()
    math(EXPR timestamp "${timestamp} + 1")
    list(GET arguments ${timestamp} value)
    if(NOT value STREQUAL "FALSE")
        message(FATAL_ERROR "${name}: the archive must extract at its extraction time, not with DOWNLOAD_EXTRACT_TIMESTAMP '${value}'")
    endif()
    math(EXPR archives "${archives} + 1")
endforeach()

message(STATUS "Verified extraction-time timestamps for ${archives} archives of ${count} dependencies")