# ---------------------------------------------------------------------------
# The application executable.
#
# Note the link list: exactly one first-party library, saim::app.  Everything
# else arrives transitively, which is the point of having a composition root -
# if the executable ever needs to name saim::database directly, something has
# reached past the controller and the layering has sprung a leak.
# ---------------------------------------------------------------------------
saim_add_executable(SmartAIIndustrialMonitor
    SOURCES
        main.cpp
    LIBS
        saim::app
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

# ---------------------------------------------------------------------------
# The standalone FakeDeviceServer.
#
# It exists separately from the in-process gateway for the cases where the two
# roles genuinely belong to two machines: demonstrating that the client survives
# a disconnect, or running the monitor on one PC and the "plant" on another.
# ---------------------------------------------------------------------------
if(SAIM_BUILD_SIMULATOR)
    saim_add_executable(saim_simulator
        SOURCES
            ../simulator/main.cpp
        LIBS
            saim::common
            saim::core
            saim::device
            saim::network
            Qt6::Core
            Qt6::Network
    )
endif()
