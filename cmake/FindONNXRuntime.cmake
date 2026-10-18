# ---------------------------------------------------------------------------
# FindONNXRuntime.cmake
#
# Locates the ONNX Runtime C/C++ library.
#
# The import library layout has been unstable across releases, so we probe the
# historical names explicitly rather than relying on a single find_library call.
#
# Hints (checked in order):
#   -DONNXRuntime_ROOT=<dir>            explicit override
#   $ENV{ONNXRUNTIME_ROOT}
#   <project>/third_party/onnxruntime   vendored drop-in
#   C:/onnxruntime, /usr/local, /opt/onnxruntime
#
# Provides: ONNXRuntime_FOUND, ONNXRuntime::ONNXRuntime
# ---------------------------------------------------------------------------
include(FindPackageHandleStandardArgs)

set(_onnx_hints
    ${ONNXRuntime_ROOT}
    $ENV{ONNXRUNTIME_ROOT}
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime"
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime"
    "C:/onnxruntime"
    "/usr/local"
    "/opt/onnxruntime")

find_path(ONNXRuntime_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATH_SUFFIXES include include/onnxruntime include/onnxruntime/core/session
    HINTS ${_onnx_hints})

find_library(ONNXRuntime_LIBRARY
    NAMES onnxruntime libonnxruntime
    PATH_SUFFIXES lib lib64
    HINTS ${_onnx_hints})

find_package_handle_standard_args(ONNXRuntime
    REQUIRED_VARS ONNXRuntime_LIBRARY ONNXRuntime_INCLUDE_DIR)

if(ONNXRuntime_FOUND AND NOT TARGET ONNXRuntime::ONNXRuntime)
    add_library(ONNXRuntime::ONNXRuntime UNKNOWN IMPORTED)
    set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
        IMPORTED_LOCATION             "${ONNXRuntime_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRuntime_INCLUDE_DIR}")
endif()

mark_as_advanced(ONNXRuntime_INCLUDE_DIR ONNXRuntime_LIBRARY)
