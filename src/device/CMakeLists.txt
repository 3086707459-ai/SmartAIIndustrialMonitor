# ---------------------------------------------------------------------------
# saim_device - the domain model: Device, DeviceManager, DeviceSimulator.
#
# Depends on core (config + logging) but on no I/O layer.  A device can be
# created, fed samples and queried in a unit test with no socket and no database.
# ---------------------------------------------------------------------------
saim_add_library(saim_device
    SOURCES
        Device.h
        Device.cpp
        DeviceManager.h
        DeviceManager.cpp
        DeviceProtocol.h
        DeviceProtocol.cpp
        DeviceSimulator.h
        DeviceSimulator.cpp
    LIBS
        saim::common
        saim::core
        Qt6::Core
)
