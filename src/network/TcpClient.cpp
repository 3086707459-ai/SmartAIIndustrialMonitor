#include "network/TcpClient.h"

#include "core/Logger.h"

#include <QDateTime>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QThread>

namespace saim {

TcpClient::TcpClient(QObject* parent)
    : QObject(parent)
{
    setObjectName(QStringLiteral("TcpClient"));
    // Bound Qt's receive buffer. If storage backpressure slows this event loop,
    // TCP's receive window can throttle the gateway instead of growing memory.
    m_socket.setReadBufferSize(64 * 1024);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &TcpClient::onReconnectTimeout);

    // The heartbeat is also the half-open detector: if the peer's machine is
    // powered off, TCP will not report the break for minutes.  A heartbeat that
    // expects a reply within one interval catches it in seconds.
    m_heartbeatTimer.setSingleShot(false);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &TcpClient::onHeartbeatTimeout);

    m_connectTimer.setSingleShot(true);
    connect(&m_connectTimer, &QTimer::timeout, this, &TcpClient::onConnectTimeout);

    connect(&m_socket, &QTcpSocket::connected,    this, &TcpClient::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &TcpClient::onDisconnected);
    connect(&m_socket, &QTcpSocket::readyRead,    this, &TcpClient::onReadyRead);
    connect(&m_socket, &QAbstractSocket::errorOccurred, this, &TcpClient::onSocketError);
}

TcpClient::~TcpClient()
{
    // No deleteLater here: the socket is a value member and is destroyed with
    // us.  Close it explicitly so the peer sees a clean FIN rather than a reset.
    if (m_socket.state() != QAbstractSocket::UnconnectedState)
    {
        m_socket.disconnectFromHost();
    }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TcpClient::setOptions(const Options& options)
{
    m_options = options;
    m_currentBackoffMs = qMax(100, options.reconnectBaseMs);
    m_heartbeatTimer.setInterval(qMax(1000, options.heartbeatMs));
}

bool TcpClient::isConnected() const noexcept
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TcpClient::start()
{
    if (m_running)
    {
        // Already running: an endpoint change should recycle the link.
        if (isConnected())
        {
            SAIM_INFO(logcat::kNetwork,
                      QStringLiteral("Endpoint changed while connected - reconnecting"));
            m_socket.abort();
        }
        return;
    }

    m_running = true;
    resetBackoff();

    SAIM_INFO(logcat::kNetwork,
              QStringLiteral("TCP client starting, target %1:%2")
                  .arg(m_options.host)
                  .arg(m_options.port));

    setState(ConnectionState::Connecting);
    ++m_statistics.connectAttempts;
    m_socket.connectToHost(m_options.host, m_options.port);
    m_connectTimer.start(m_options.connectTimeoutMs);
}

void TcpClient::stop()
{
    m_running = false;
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    m_connectTimer.stop();

    if (m_socket.state() != QAbstractSocket::UnconnectedState)
    {
        // abort() rather than disconnectFromHost(): on stop we do not want to
        // wait for a graceful close, and we do not want onDisconnected to
        // schedule a reconnect.  m_running == false already prevents that.
        m_socket.abort();
    }

    setState(ConnectionState::Disconnected);
    SAIM_INFO(logcat::kNetwork, QStringLiteral("TCP client stopped"));
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool TcpClient::send(MessageType type, const QJsonObject& payload)
{
    if (!isConnected())
    {
        SAIM_DEBUG(logcat::kNetwork,
                   QStringLiteral("Dropping %1 frame - not connected").arg(toString(type)));
        return false;
    }

    const QByteArray frame = ProtocolCodec::encode(type, payload);
    // write() buffers; flush() would block.  Letting Qt drain asynchronously is
    // correct here - a blocking write on the worker thread is exactly the stall
    // the threading design exists to avoid.
    const qint64 written = m_socket.write(frame);
    if (written < 0)
    {
        emit errorOccurred(QStringLiteral("Socket write failed: %1").arg(m_socket.errorString()));
        return false;
    }

    m_statistics.bytesSent += static_cast<quint64>(written);
    emit statisticsChanged(m_statistics);
    return true;
}

void TcpClient::sendRegistration(const QString& clientId)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), clientId);
    payload.insert(QStringLiteral("kind"), QStringLiteral("monitor-client"));
    payload.insert(QStringLiteral("protocolVersion"), static_cast<int>(ProtocolCodec::kVersion));
    send(MessageType::Register, payload);
}

// ---------------------------------------------------------------------------
// Socket slots (all execute on the worker thread)
// ---------------------------------------------------------------------------

void TcpClient::onConnected()
{
    m_connectTimer.stop();
    resetBackoff();

    if (m_statistics.connectAttempts > 1)
    {
        ++m_statistics.reconnects;
    }

    SAIM_INFO(logcat::kNetwork,
              QStringLiteral("Connected to %1:%2").arg(m_options.host).arg(m_options.port));

    // A reconnect resumes mid-stream: whatever the previous connection had
    // buffered is not valid input for the new one.
    m_codec.reset();

    setState(ConnectionState::Connected);
    m_heartbeatTimer.start();
    emit connected();
}

void TcpClient::onDisconnected()
{
    m_connectTimer.stop();
    m_heartbeatTimer.stop();

    SAIM_WARN(logcat::kNetwork,
              QStringLiteral("Disconnected from %1:%2").arg(m_options.host).arg(m_options.port));

    setState(ConnectionState::Disconnected);
    emit disconnected();

    if (m_running && m_options.autoReconnect)
    {
        scheduleReconnect();
    }
}

void TcpClient::onReadyRead()
{
    const QByteArray chunk = m_socket.readAll();
    if (chunk.isEmpty())
    {
        return;
    }

    m_statistics.bytesReceived += static_cast<quint64>(chunk.size());
    m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();

    // The decoder handles partial and coalesced frames; this loop never sees a
    // half message.  Everything downstream can assume a whole frame.
    const std::vector<Frame> frames = m_codec.append(chunk);
    m_statistics.framesReceived += frames.size();
    m_statistics.framesDiscarded = m_codec.discardedFrameCount();

    for (const Frame& frame : frames)
    {
        switch (frame.type)
        {
        case MessageType::Telemetry:
        {
            const Result<SensorSample> decoded = ProtocolCodec::decodeSample(frame.payload);
            if (decoded.isError())
            {
                SAIM_WARN(logcat::kNetwork,
                          QStringLiteral("Undecodable telemetry frame: %1")
                              .arg(decoded.error().message));
                continue;
            }
            emit sampleReceived(decoded.value());
            break;
        }
        case MessageType::Heartbeat:
            // Nothing to do beyond having received it - the arrival itself is
            // the liveness proof, and m_lastFrameMs was just updated.
            SAIM_TRACE(logcat::kNetwork, QStringLiteral("Heartbeat from gateway"));
            break;

        case MessageType::Error:
            SAIM_ERROR(logcat::kNetwork,
                       QStringLiteral("Gateway reported an error: %1")
                           .arg(frame.payload.value(QStringLiteral("message")).toString()));
            break;

        default:
            break;
        }

        // Always re-emit the raw frame so the diagnostics view and the tests can
        // observe protocol traffic without the client having to know every type.
        emit frameReceived(frame.type, frame.payload);
    }

    emit statisticsChanged(m_statistics);
}

void TcpClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)

    const QString message = m_socket.errorString();

    // RemoteHostClosedError is the normal end of a session, not a fault: the
    // operator stopped the gateway.  Reporting it as an error trains people to
    // ignore the error log.
    if (error == QAbstractSocket::RemoteHostClosedError)
    {
        SAIM_INFO(logcat::kNetwork, QStringLiteral("Gateway closed the connection"));
        return;
    }

    ++m_statistics.connectFailures;
    SAIM_ERROR(logcat::kNetwork,
               QStringLiteral("Socket error on %1:%2 - %3")
                   .arg(m_options.host)
                   .arg(m_options.port)
                   .arg(message));

    setState(ConnectionState::Error);
    emit errorOccurred(message);
    emit statisticsChanged(m_statistics);

    if (m_running && m_options.autoReconnect && m_socket.state() == QAbstractSocket::UnconnectedState)
    {
        scheduleReconnect();
    }
}

void TcpClient::onConnectTimeout()
{
    if (isConnected())
    {
        return;
    }
    SAIM_WARN(logcat::kNetwork,
              QStringLiteral("Connect attempt to %1:%2 timed out after %3 ms")
                  .arg(m_options.host)
                  .arg(m_options.port)
                  .arg(m_options.connectTimeoutMs));
    m_socket.abort();
    // abort() triggers errorOccurred -> scheduleReconnect, so no duplicate
    // scheduling is needed here.
}

void TcpClient::onReconnectTimeout()
{
    if (!m_running || isConnected())
    {
        return;
    }

    ++m_statistics.connectAttempts;
    setState(ConnectionState::Connecting);
    m_socket.connectToHost(m_options.host, m_options.port);
    m_connectTimer.start(m_options.connectTimeoutMs);
}

void TcpClient::onHeartbeatTimeout()
{
    if (!isConnected())
    {
        return;
    }

    // Half-open detection: we have sent heartbeats but heard nothing back for
    // three intervals, so the link is dead even though the socket thinks it is
    // open.  Force the reconnect path rather than waiting on the OS.
    constexpr qint64 kSilenceThresholdMs = 15000;
    if (m_lastFrameMs != 0
        && QDateTime::currentMSecsSinceEpoch() - m_lastFrameMs > kSilenceThresholdMs)
    {
        SAIM_WARN(logcat::kNetwork,
                  QStringLiteral("No traffic for %1 ms - treating link as half-open")
                      .arg(kSilenceThresholdMs));
        m_socket.abort();
        return;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("ts"),
                   QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    send(MessageType::Heartbeat, payload);
}

// ---------------------------------------------------------------------------
// Reconnect policy
// ---------------------------------------------------------------------------

void TcpClient::scheduleReconnect()
{
    // Exponential backoff with full jitter.
    const int base = qMax(100, m_options.reconnectBaseMs);
    const int maxDelay = qMax(base, m_options.reconnectMaxMs);

    const int delay = m_currentBackoffMs;
    m_currentBackoffMs = qMin(maxDelay, m_currentBackoffMs * 2);

    const int jittered = QRandomGenerator::global()->bounded(base, delay + 1);

    SAIM_INFO(logcat::kNetwork,
              QStringLiteral("Reconnecting in %1 ms (backoff now %2 ms)")
                  .arg(jittered)
                  .arg(m_currentBackoffMs));

    setState(ConnectionState::Reconnecting);
    m_reconnectTimer.start(jittered);
}

void TcpClient::resetBackoff()
{
    m_currentBackoffMs = qMax(100, m_options.reconnectBaseMs);
}

void TcpClient::setState(ConnectionState state)
{
    if (m_state == state)
    {
        return;
    }
    m_state = state;
    emit stateChanged(m_state);
}

}   // namespace saim
