#pragma once

// ---------------------------------------------------------------------------
// TcpClient.h - non-blocking acquisition client for the device gateway.
//
// Threading
// ---------
// This object is designed to be moved to a worker thread with
// `moveToThread()`; it is never touched from the UI thread.  Everything it
// exposes is either a slot (invoked via a queued connection, hence executed on
// the worker thread) or a signal.
//
// The reconnect loop is *not* a sleep-and-retry inside a slot - that would block
// the worker's event loop and stall telemetry from every other source.  It is a
// QTimer with exponential backoff plus jitter, so the event loop keeps running
// and a `stop()` takes effect immediately even mid-backoff.
//
// Jitter matters: if a switch reboots and 200 clients reconnect on a fixed 1 s
// schedule, they stampede the gateway and it falls over again.  Randomising the
// delay is the standard remedy and is worth calling out in a review.
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "device/DeviceProtocol.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

namespace saim {

class TcpClient final : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString host = QStringLiteral("127.0.0.1");
        quint16 port = 9100;
        int reconnectBaseMs   = 1000;
        int reconnectMaxMs    = 30000;
        int heartbeatMs       = 5000;
        int connectTimeoutMs  = 5000;
        bool autoReconnect    = true;
    };

    explicit TcpClient(QObject* parent = nullptr);
    ~TcpClient() override;

    TcpClient(const TcpClient&)            = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void setOptions(const Options& options);
    [[nodiscard]] const Options& options() const noexcept { return m_options; }

    [[nodiscard]] ConnectionState state() const noexcept { return m_state; }
    [[nodiscard]] bool isConnected() const noexcept;

    /// Cumulative counters, surfaced on the diagnostics page - and the reason a
    /// reviewer can tell "the link flapped 40 times" from "the link is fine".
    struct Statistics
    {
        quint64 bytesReceived   = 0;
        quint64 bytesSent       = 0;
        quint64 framesReceived  = 0;
        quint64 framesDiscarded = 0;
        quint64 connectAttempts = 0;
        quint64 connectFailures = 0;
        quint64 reconnects      = 0;
    };
    [[nodiscard]] Statistics statistics() const noexcept { return m_statistics; }

public slots:
    /// Starts the connection state machine.  Idempotent: calling it on an
    /// already-running client only updates the endpoint.
    void start();
    void stop();

    /// Sends a frame.  Returns false (and logs) when not connected, so a caller
    /// in a retry loop can distinguish "queued" from "dropped".
    bool send(saim::MessageType type, const QJsonObject& payload);

    /// Convenience for the register handshake.
    void sendRegistration(const QString& clientId);

signals:
    void connected();
    void disconnected();
    void stateChanged(saim::ConnectionState state);
    void sampleReceived(const saim::SensorSample& sample);
    void frameReceived(saim::MessageType type, const QJsonObject& payload);
    void errorOccurred(const QString& message);
    void statisticsChanged(const saim::TcpClient::Statistics& statistics);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onReconnectTimeout();
    void onHeartbeatTimeout();
    void onConnectTimeout();

private:
    void setState(ConnectionState state);
    void scheduleReconnect();
    void resetBackoff();

    QTcpSocket   m_socket;
    ProtocolCodec m_codec;
    Options      m_options;

    ConnectionState m_state = ConnectionState::Disconnected;
    Statistics      m_statistics;

    QTimer m_reconnectTimer;
    QTimer m_heartbeatTimer;
    QTimer m_connectTimer;

    /// Current backoff delay; doubles on each failure up to reconnectMaxMs.
    int  m_currentBackoffMs = 1000;
    bool m_running          = false;

    /// Diagnostics: time of the last frame, used to detect a silent (half-open)
    /// connection that the OS has not yet reported as dead.
    qint64 m_lastFrameMs = 0;
};

}   // namespace saim

// The statistics struct crosses a thread boundary inside a queued signal, so it
// must be a registered metatype or the connection silently drops every emission.
Q_DECLARE_METATYPE(saim::TcpClient::Statistics)
