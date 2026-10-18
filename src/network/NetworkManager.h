#pragma once

// ---------------------------------------------------------------------------
// NetworkManager.h - owns the transport objects and their thread affinity.
//
// This is the seam between "what the application wants" (connect, disconnect,
// send) and "how it is done" (a QTcpSocket on a worker thread).  The UI holds a
// NetworkManager and never sees a socket, a thread, or a moveToThread call -
// which is the entire point of the layering.
//
// Lifetime and shutdown
// ---------------------
// The worker thread is created *here* and torn down in the destructor with the
// quit/wait/deleteLater sequence, in that order:
//
//     thread->quit();     // stop the event loop
//     thread->wait();     // join; blocks until the loop has actually exited,
//                         // so no slot can be mid-execution afterwards
//     deleteLater();      // the QThread object deletes itself on its own thread
//
// Skipping wait() is the classic mistake: the process then exits while a worker
// still holds a socket, and the result is an intermittent crash or a leaked
// handle that only shows up under a debugger.
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "network/TcpClient.h"

#include <QObject>
#include <QThread>

namespace saim {

class ConfigManager;

class NetworkManager final : public QObject
{
    Q_OBJECT
public:
    explicit NetworkManager(ConfigManager* config, QObject* parent = nullptr);
    ~NetworkManager() override;

    NetworkManager(const NetworkManager&)            = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    [[nodiscard]] ConnectionState state() const noexcept { return m_state; }
    [[nodiscard]] TcpClient::Statistics statistics() const noexcept { return m_statistics; }
    [[nodiscard]] bool isConnected() const noexcept { return m_state == ConnectionState::Connected; }

    /// The client object, for the rare caller that needs to post a direct
    /// queued invocation.  Never call its methods directly from the UI thread.
    [[nodiscard]] TcpClient* client() const noexcept { return m_client; }

    [[nodiscard]] QString endpointDescription() const;

signals:
    void connected();
    void disconnected();
    void stateChanged(saim::ConnectionState state);
    void sampleReceived(const saim::SensorSample& sample);
    void errorOccurred(const QString& message);
    void statisticsChanged(const saim::TcpClient::Statistics& statistics);

public slots:
    /// Starts the worker event loop and issues a connect.  Idempotent.
    void start();

    /// Requests a graceful shutdown: stops the client, then drains the thread.
    void stop();

    /// Re-reads the endpoint from configuration and reconnects if it changed.
    void applyConfiguration();

    /// Fire-and-forget send; the actual write happens on the worker thread.
    void sendCommand(const QString& deviceId, const QString& command);

private:
    void wireClientSignals();

    ConfigManager* m_config = nullptr;
    QThread*       m_thread = nullptr;
    TcpClient*     m_client = nullptr;

    ConnectionState       m_state = ConnectionState::Disconnected;
    TcpClient::Statistics m_statistics;
    bool                  m_started = false;
};

}   // namespace saim
