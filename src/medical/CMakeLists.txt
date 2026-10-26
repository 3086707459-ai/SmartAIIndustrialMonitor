saim_add_library(saim_medical
    SOURCES BIDMCRecord.h BIDMCRecord.cpp RespiratoryAnalysis.h RespiratoryAnalysis.cpp
    LIBS saim::common Qt6::Core
)
qt_add_resources(saim_medical "bidmc_provenance"
    PREFIX "/medical" BASE "${CMAKE_SOURCE_DIR}/datasets"
    FILES "${CMAKE_SOURCE_DIR}/datasets/SHA256SUMS-bidmc-1.0.0.txt"
)
saim_add_library(saim_medical_ui
    SOURCES MedicalWindow.h MedicalWindow.cpp
    LIBS saim::medical Qt6::Widgets Qt6::Charts Qt6::Concurrent Qt6::Svg
)
qt_add_resources(saim_medical_ui "medsignal_assets"
    PREFIX "/" BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES "${CMAKE_SOURCE_DIR}/resources/icons/medsignal.svg"
)
