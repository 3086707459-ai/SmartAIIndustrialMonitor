# ---------------------------------------------------------------------------
# saim_ai - feature extraction, fault prediction, anomaly detection, vision,
# report generation.
#
# Note the LIBS list: saim::device (for the domain types) and saim::core (for
# config and logging), but *never* Qt6::Widgets and never saim::network.  The AI
# layer cannot touch a widget or a socket, which is what makes it unit-testable
# on a machine with no gateway running.
#
# ONNX Runtime and OpenCV are attached below, conditionally, and only to this
# target - no other layer links them.
# ---------------------------------------------------------------------------
saim_add_library(saim_ai
    SOURCES
        FeatureExtractor.h
        FeatureExtractor.cpp
        AIInferenceEngine.h
        AIInferenceEngine.cpp
        FaultPredictor.h
        FaultPredictor.cpp
        AnomalyDetector.h
        AnomalyDetector.cpp
        VisionDetector.h
        VisionDetector.cpp
        AIReportGenerator.h
        AIReportGenerator.cpp
    LIBS
        saim::common
        saim::core
        saim::device
        Qt6::Core
        Qt6::Gui          # QImage for the vision path
)

# ---------------------------------------------------------------------------
# ONNX Runtime: define SAIM_HAVE_ONNX so the engine compiles its real body.
# Without it the same file compiles the "not available" branch and every caller
# keeps working - that is the whole point of the Pimpl boundary.
# ---------------------------------------------------------------------------
if(SAIM_WITH_ONNX AND ONNXRuntime_FOUND)
    target_link_libraries(saim_ai PRIVATE ONNXRuntime::ONNXRuntime)
    target_compile_definitions(saim_ai PRIVATE SAIM_HAVE_ONNX)
endif()

# ---------------------------------------------------------------------------
# OpenCV: SAIM_HAVE_OPENCV is defined globally at the top level (the UI also
# needs to know whether the vision path is real), so nothing extra is needed
# here beyond linking the libraries into this target.
# ---------------------------------------------------------------------------
if(SAIM_WITH_OPENCV AND OpenCV_FOUND)
    target_link_libraries(saim_ai PRIVATE ${OpenCV_LIBS})
    target_include_directories(saim_ai PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
