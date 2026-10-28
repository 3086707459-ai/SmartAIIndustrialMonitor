cmake_minimum_required(VERSION 3.21)

project(MedSignalWorkbench
    VERSION     1.2.0
    DESCRIPTION "Medical waveform replay and reproducible PhysioNet evaluation"
    LANGUAGES   CXX)

# ---------------------------------------------------------------------------
# Global language / build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SAIM_BUILD_TESTS      "Build unit tests"                 ON)
option(SAIM_BUILD_SIMULATOR  "Build the FakeDeviceServer tool"  ON)
option(SAIM_WITH_ONNX        "Enable ONNX Runtime inference"    ON)
option(SAIM_WITH_OPENCV      "Enable OpenCV/YOLO vision module" ON)
option(SAIM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Charts Svg Concurrent)

qt_standard_project_setup()          # AUTOMOC / AUTOUIC / AUTORCC + sensible defaults

# ---------------------------------------------------------------------------
# Third-party: ONNX Runtime (optional, graceful degradation)
# ---------------------------------------------------------------------------
if(SAIM_WITH_ONNX)
    find_package(ONNXRuntime QUIET)
    if(ONNXRuntime_FOUND)
        message(STATUS "[SAIM] ONNX Runtime found -> real neural-network inference enabled")
    else()
        message(STATUS "[SAIM] ONNX Runtime NOT found -> FaultPredictor falls back to "
                       "the built-in physics/heuristic model (identical interface)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Third-party: OpenCV (optional)
# ---------------------------------------------------------------------------
if(SAIM_WITH_OPENCV)
    find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs dnn)
    if(OpenCV_FOUND)
        message(STATUS "[SAIM] OpenCV ${OpenCV_VERSION} found -> vision defect detection enabled")
        add_compile_definitions(SAIM_HAVE_OPENCV)
    else()
        message(STATUS "[SAIM] OpenCV NOT found -> CameraWidget runs in simulation mode")
    endif()
endif()

find_package(Threads REQUIRED)

include(CompilerWarnings)

# ---------------------------------------------------------------------------
# Layers.  Dependency direction is strictly top -> bottom, never upward.
#
#   app  ->  ui  ->  thread  ->  ai / network / database  ->  device  ->  core
#
# saim_app is the composition root: the only target that links both the UI and
# the worker layers.  Keeping it separate is what lets every other target keep a
# one-way link list, so the direction above is enforced by the build rather than
# by convention.  See src/app/ApplicationController.h.
# ---------------------------------------------------------------------------
add_subdirectory(src/common)
add_subdirectory(src/medical)
add_subdirectory(src/core)
add_subdirectory(src/device)
add_subdirectory(src/database)
add_subdirectory(src/network)
add_subdirectory(src/ai)
add_subdirectory(src/thread)
add_subdirectory(src/ui)
add_subdirectory(src/app)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_subdirectory(apps)

if(SAIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Deployment helper: copy the Qt runtime next to the executable
# ---------------------------------------------------------------------------
if(WIN32 AND TARGET MedSignalWorkbench)
    find_program(SAIM_WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin"
                 "${CMAKE_PREFIX_PATH}/bin")
    if(SAIM_WINDEPLOYQT)
        add_custom_target(deploy
            COMMAND "${SAIM_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler
                    "$<TARGET_FILE:MedSignalWorkbench>"
            DEPENDS MedSignalWorkbench
            COMMENT "Deploying Qt runtime next to MedSignalWorkbench.exe")
    endif()
endif()

message(STATUS "")
message(STATUS "[SAIM] ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "[SAIM]   build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "[SAIM]   compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "[SAIM]   Qt         : ${Qt6_VERSION}")
message(STATUS "")
