# Deliberately not a default CTest: this is one sustained workload, not repeated
# short tests. scripts/stress.ps1 selects its wall-clock duration explicitly.
saim_add_executable(pipeline_stress
    SOURCES pipeline_stress.cpp
    LIBS saim::app saim::device Qt6::Core Qt6::Network Qt6::Sql)
set_target_properties(pipeline_stress PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
if(WIN32)
    target_link_libraries(pipeline_stress PRIVATE psapi)
endif()
