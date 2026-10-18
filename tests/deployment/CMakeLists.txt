# Deployment acceptance is intentionally separate from CTest: it must run from
# the packaged directory with the native Windows platform and a scrubbed PATH.
saim_add_executable(saim_deployment_probe
    SOURCES deployment_probe.cpp
    LIBS saim::app Qt6::Widgets Qt6::Sql Qt6::Network Qt6::Charts Qt6::Svg
)
set_target_properties(saim_deployment_probe PROPERTIES WIN32_EXECUTABLE OFF)
