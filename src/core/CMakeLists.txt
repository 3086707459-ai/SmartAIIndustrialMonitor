# ---------------------------------------------------------------------------
# saim_core - cross-cutting concerns: logging, configuration, application
# lifecycle.  Everything else depends on this; it depends on nothing but Qt.
# ---------------------------------------------------------------------------
saim_add_library(saim_core
    SOURCES
        Logger.h
        Logger.cpp
        ConfigManager.h
        ConfigManager.cpp
    LIBS
        saim::common
        Qt6::Core
)
