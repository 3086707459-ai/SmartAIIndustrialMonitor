# ---------------------------------------------------------------------------
# tests/integration/CMakeLists.txt
#
# One executable for the whole pipeline, and it links saim::app.
#
# That single link line is the entire justification for this directory existing.
# A unit test may only link the layer it exercises; this one links the
# composition root, which links everything below it, so a break *anywhere* in
# the wire between the socket and the database shows up as a failure here.  It
# is also the only target besides the application itself that links saim::app -
# which is as it should be, because saim::app exists to be linked by a main(),
# and this test is a main() that does not paint.
#
# Note what is absent: Qt6::Widgets.  The test never builds the window, so
# linking the widget layer would slow the build down and imply a dependency that
# is not there.
# ---------------------------------------------------------------------------

saim_add_unit_test(test_pipeline
    SOURCES test_pipeline.cpp
    LIBS
        saim::app
        saim::core
        saim::device
        saim::database
        saim::thread
        Qt6::Core
        Qt6::Network
        Qt6::Sql
)

# The pipeline test opens real loopback sockets and waits on real worker
# threads, so it needs a longer budget than a unit test that finishes in
# milliseconds.  The timeout sits well above the internal per-assertion
# deadlines on purpose: a failure should come from the test's own condition
# message, naming what it waited for, rather than from being killed from
# outside with no explanation.
set_tests_properties(test_pipeline PROPERTIES TIMEOUT 300)

saim_add_unit_test(test_ui_smoke
    SOURCES test_ui_smoke.cpp
    LIBS saim::app Qt6::Widgets
)
set_tests_properties(test_ui_smoke PROPERTIES TIMEOUT 30)
saim_add_unit_test(test_medical_ui SOURCES test_medical_ui.cpp LIBS saim::medical_ui)
target_compile_definitions(test_medical_ui PRIVATE SAIM_MEDICAL_FIXTURE="${CMAKE_SOURCE_DIR}/datasets/bidmc-demo")

saim_add_unit_test(test_overload
    SOURCES test_overload.cpp
    LIBS saim::app Qt6::Network Qt6::Sql
)
set_tests_properties(test_overload PROPERTIES TIMEOUT 60)
