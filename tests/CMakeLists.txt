# ---------------------------------------------------------------------------
# tests/CMakeLists.txt
#
# Two kinds of test, kept apart on purpose.
#
#   unit/  links a single layer and nothing above it.  These run with no
#          database file, no socket and no widgets, in milliseconds.
#
#   integration/ links saim::app - the whole stack minus main() - so a whole
#          pipeline can be driven end to end against the in-process gateway.
#
# The separation is the point of the layered build: if a "unit" test needs the
# UI to compile, the layer it is testing depends on the UI, and that is a bug in
# the layering rather than a missing test dependency.
# ---------------------------------------------------------------------------

# -- a tiny harness instead of a framework ----------------------------------
#
# Qt Test would work, and is used for the widget tests.  For the pure-logic
# tests this header is deliberately not a framework: a test is a function
# registered with SAIM_TEST, the runner calls them all, and a failure prints the
# file and line.  Nothing to install, nothing to configure, and the whole thing
# fits on one screen - which matters because a reviewer will read it.
add_library(saim_test_support INTERFACE)
find_package(Qt6 REQUIRED COMPONENTS Test)
target_include_directories(saim_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(saim_test_support INTERFACE Qt6::Core)

set(SAIM_TEST_TARGETS "")

# Registers a test executable and adds it to CTest.
function(saim_add_unit_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})

    saim_add_executable(${name}
        SOURCES ${ARG_SOURCES}
        LIBS ${ARG_LIBS} saim_test_support Qt6::Core
    )
    # Console application: no window, so no Qt platform plugin is loaded and the
    # tests run on a headless CI machine without an offscreen platform hack.
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)

    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    set(SAIM_TEST_TARGETS ${SAIM_TEST_TARGETS} ${name} PARENT_SCOPE)
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(stress)
add_subdirectory(deployment)
