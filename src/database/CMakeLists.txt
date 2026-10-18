# ---------------------------------------------------------------------------
# saim_database - SQLite persistence.
#
# No Qt6::Widgets, no Qt6::Network: the storage layer cannot reach the UI or the
# socket even by accident, because it does not link them.
# ---------------------------------------------------------------------------
saim_add_library(saim_database
    SOURCES
        DatabaseManager.h
        DatabaseManager.cpp
        DataRepository.h
        DataRepository.cpp
    LIBS
        saim::common
        saim::core
        Qt6::Core
        Qt6::Sql
)
