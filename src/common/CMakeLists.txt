# ---------------------------------------------------------------------------
# saim_common - value types shared by every layer.
#
# Depends on Qt6::Core only.  Adding a dependency here would leak it into all
# seven downstream targets, so keep this target deliberately thin.
# ---------------------------------------------------------------------------
saim_add_library(saim_common
    SOURCES
        Types.h
        Types.cpp
        ErrorCode.h
        Result.h
        RingBuffer.h
    LIBS
        Qt6::Core
)
