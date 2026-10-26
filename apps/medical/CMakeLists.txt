saim_add_executable(MedSignalWorkbench SOURCES main.cpp LIBS saim::medical_ui)
set_target_properties(MedSignalWorkbench PROPERTIES WIN32_EXECUTABLE ON)
saim_add_executable(bidmc_benchmark SOURCES benchmark.cpp LIBS saim::medical)
set_target_properties(bidmc_benchmark PROPERTIES WIN32_EXECUTABLE OFF)
