# ---------------------------------------------------------------------------
# saim_thread - the concurrency layer.
#
# This is the only target that links *all* the worker-facing layers together
# (network, database, ai), because it is the layer whose job is to own their
# threads.  Nothing else is allowed to.
#
# Dependency direction stays one-way: thread -> {ai, network, database} ->
# device -> core.  The UI depends on this; this never depends on the UI.
# ---------------------------------------------------------------------------
saim_add_library(saim_thread
    SOURCES
        ThreadedWorker.h
        ThreadedWorker.cpp
        AcquisitionWorker.h
        AcquisitionWorker.cpp
        PersistenceWorker.h
        PersistenceWorker.cpp
        AIWorker.h
        AIWorker.cpp
        WorkerPool.h
        WorkerPool.cpp
    LIBS
        saim::common
        saim::core
        saim::device
        saim::database
        saim::network
        saim::ai
        Qt6::Core
        Qt6::Sql
        Qt6::Gui
)
