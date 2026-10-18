# ---------------------------------------------------------------------------
# saim_app - the composition root.
#
# This is the only target in the project that links both saim_ui and the worker
# layers, and the only one that is allowed to.  Keeping it separate is what lets
# every other target keep a one-way dependency list (see the note in
# ApplicationController.h).
#
# It builds a static library rather than adding its sources straight to the
# executable so that a test can construct a controller without a second main().
# ---------------------------------------------------------------------------
saim_add_library(saim_app
    SOURCES
        ApplicationController.h
        ApplicationController.cpp
    LIBS
        saim::common
        saim::core
        saim::device
        saim::network
        saim::database
        saim::ai
        saim::thread
        saim::ui
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
)
