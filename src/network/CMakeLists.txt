# ---------------------------------------------------------------------------
# saim_network - transport.  TcpClient (acquisition), TcpServer (the simulated
# plant gateway), NetworkManager (thread ownership + the UI-facing seam).
# ---------------------------------------------------------------------------
saim_add_library(saim_network
    SOURCES
        TcpClient.h
        TcpClient.cpp
        TcpServer.h
        TcpServer.cpp
        NetworkManager.h
        NetworkManager.cpp
    LIBS
        saim::common
        saim::core
        saim::device
        Qt6::Core
        Qt6::Network
)
