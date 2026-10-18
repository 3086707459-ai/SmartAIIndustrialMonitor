# ---------------------------------------------------------------------------
# CompilerWarnings.cmake
#
# Defines the interface target `saim::warnings`, which every first-party target
# links against.  Keeping the flag set in one place means a new module cannot
# silently opt out of the project's quality bar.
# ---------------------------------------------------------------------------
add_library(saim_warnings INTERFACE)
add_library(saim::warnings ALIAS saim_warnings)

if(MSVC)
    target_compile_options(saim_warnings INTERFACE
        /W4
        /permissive-
        /utf-8                 # source files are UTF-8; without this MSVC mangles CJK
        /Zc:__cplusplus
        /wd4127                # conditional expression is constant (Qt macros)
        /wd4251                # needs dll-interface (Qt containers)
    )
    if(SAIM_WARNINGS_AS_ERRORS)
        target_compile_options(saim_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(saim_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wno-unknown-pragmas
    )
    if(SAIM_WARNINGS_AS_ERRORS)
        target_compile_options(saim_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Convenience helper: apply the project's standard setup to a target.
#
#   saim_add_library(saim_core SOURCES ... LIBS Qt6::Core)
#   saim_add_executable(app SOURCES ... LIBS saim::core)
#
# The ALIAS is created under the fully-qualified name `saim::<target>` so that a
# consumer writes `saim::core` rather than the raw target name.  Two details make
# that work:
#
#   * ALIAS targets do not appear in the directory's target list, so anything
#     walking ${CMAKE_CURRENT_LIST_DIR} for enumeration must filter them out.
#   * An ALIAS may not be exported or installed.  That is fine here - every layer
#     is a static library consumed from within this one build tree.
# ---------------------------------------------------------------------------
function(saim_add_library target_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;DEFINES" ${ARGN})

    add_library(${target_name} STATIC ${ARG_SOURCES})

    # Real target is `saim_core`; the alias consumers link is `saim::core`.  The
    # prefix is stripped rather than duplicated, so that the alias reads as a
    # namespace (`saim::core`) instead of a repeated word (`saim::saim_core`).
    # Derived here rather than passed in, so the two can never drift apart.
    set(alias_name "${target_name}")
    string(REGEX REPLACE "^saim_" "" alias_name "${alias_name}")
    add_library(saim::${alias_name} ALIAS ${target_name})

    # Includes are declared on the *real* target and propagate through the alias,
    # so both `#include "core/Logger.h"` and a direct target reference resolve to
    # the same place.
    target_include_directories(${target_name} PUBLIC "${CMAKE_SOURCE_DIR}/src")

    target_link_libraries(${target_name}
        PUBLIC ${ARG_LIBS}
        PRIVATE saim::warnings)

    if(ARG_DEFINES)
        target_compile_definitions(${target_name} PRIVATE ${ARG_DEFINES})
    endif()

    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
endfunction()

function(saim_add_executable target_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;DEFINES" ${ARGN})

    qt_add_executable(${target_name} ${ARG_SOURCES})

    target_include_directories(${target_name} PRIVATE "${CMAKE_SOURCE_DIR}/src")

    target_link_libraries(${target_name}
        PRIVATE ${ARG_LIBS}
        PRIVATE saim::warnings)

    if(ARG_DEFINES)
        target_compile_definitions(${target_name} PRIVATE ${ARG_DEFINES})
    endif()

    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
endfunction()
