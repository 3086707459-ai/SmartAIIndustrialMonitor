#include "network/TcpServer.h"

#include "core/Logger.h"
#include "device/DeviceProtocol.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QThread>

#include <algorithm>

namespace saim {

namespace {

/// A plausible factory line: motors dominate, with pumps, a spindle and a fan.
struct FleetTemplate
{
    const char* id;
    const char* type;
};

constexpr FleetTemplate kDefaultFleet[] = {
    {"MOTOR-001", "motor"},
    {"MOTOR-002", "motor"},
    {"PUMP-001",  "pump"},
    {"PUMP-002",  "pump"},
    {"CNC-001",   "spindle"},
    {"FAN-001",   "fan"},
};

}   // namespace

TcpServer::TcpServer(QObject* parent)
    : QObject(parent)
    , m_rng(20260101)
{
    setObjectName(QStringLiteral("FakeDeviceServer"));

    connect(&m_server, &QTcpServer::newConnection, this, &TcpServer::onNewConnection);

    // Single-shot is not used: the tick is a fixed-rate sampling clock, and a
    // repeating timer keeps the sample interval honest over hours of running.
    m_sampleTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_sampleTimer, &QTimer::timeout, this, &TcpServer::onSampleTick);
}

TcpServer::~TcpServer()
{
    stop();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TcpServer::setOptions(const Options& options)
{
    const bool fleetChanged = options.deviceIds != m_options.deviceIds
                              || options.useDefaultFleetWhenEmpty != m_options.useDefaultFleetWhenEmpty;
    m_options = options;
    m_sampleTimer.setInterval(std::max(20, options.sampleIntervalMs));
    m_rng.seed(options.seed != 0 ? options.seed : 20260101);
    if (fleetChanged && isListening()) { registerDefaultFleet(); }
}

void TcpServer::registerDefaultFleet()
{
    m_simulators.clear();
    if (m_options.deviceIds.isEmpty() && !m_options.useDefaultFleetWhenEmpty) { return; }

    const QStringList ids = m_options.deviceIds.isEmpty()
                                ? QStringList{}
                                : m_options.deviceIds;

    if (!ids.isEmpty())
    {
        for (int i = 0; i < ids.size(); ++i)
        {
            // Derive a stable per-device seed so the same device behaves the
            // same way across runs - essential for a reproducible bug report.
            const quint32 seed = m_options.seed + static_cast<quint32>(i) * 7919u;
            auto simulator = std::make_unique<DeviceSimulator>(ids.at(i), seed);
            simulator->setProfile(DeviceProfile::forType(QLatin1String("generic")));
            m_simulators.push_back(std::move(simulator));
        }
        return;
    }

    for (std::size_t i = 0; i < std::size(kDefaultFleet); ++i)
    {
        const quint32 seed = m_options.seed + static_cast<quint32>(i) * 7919u;
        auto simulator = std::make_unique<DeviceSimulator>(
            QString::fromLatin1(kDefaultFleet[i].id), seed);
        simulator->setProfile(DeviceProfile::forType(QString::fromLatin1(kDefaultFleet[i].type)));
        m_simulators.push_back(std::move(simulator));
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TcpServer::start()
{
    if (m_server.isListening())
    {
        SAIM_DEBUG(logcat::kNetwork, QStringLiteral("Server already listening"));
        return;
    }

    registerDefaultFleet();

    const QHostAddress address(m_options.listenAddress);
    if (!m_server.listen(address, m_options.port))
    {
        const QString message = QStringLiteral("Cannot listen on %1:%2 - %3")
                                    .arg(m_options.listenAddress)
                                    .arg(m_options.port)
                                    .arg(m_server.errorString());
        SAIM_ERROR(logcat::kNetwork, message);
        emit errorOccurred(message);
        return;
    }

    SAIM_INFO(logcat::kNetwork,
              QStringLiteral("FakeDeviceServer listening on port %1, simulating %2 device(s) "
                             "at %3 ms interval")
                  .arg(m_server.serverPort())
                  .arg(m_simulators.size())
                  .arg(m_sampleTimer.interval()));

    m_sampleTimer.start();
    emit started(m_server.serverPort());
}

void TcpServer::stop()
{
    m_sampleTimer.stop();

    if (m_server.isListening())
    {
        m_server.close();
    }

    // Copy the keys first: closing emits disconnected(), which mutates the hash
    // we would otherwise be iterating - a classic invalidation bug.
    const QList<QTcpSocket*> sockets = m_clients.keys();
    for (QTcpSocket* socket : sockets)
    {
        if (socket != nullptr)
        {
            socket->disconnectFromHost();
            socket->deleteLater();
        }
    }
    m_clients.clear();

    if (m_server.isListening() || !sockets.isEmpty())
    {
        SAIM_INFO(logcat::kNetwork, QStringLiteral("FakeDeviceServer stopped"));
    }
    emit stopped();
}

bool TcpServer::isListening() const
{
    return m_server.isListening();
}

quint16 TcpServer::listeningPort() const
{
    return m_server.serverPort();
}

QStringList TcpServer::simulatedDeviceIds() const
{
    QStringList ids;
    ids.reserve(static_cast<int>(m_simulators.size()));
    for (const auto& simulator : m_simulators)
    {
        ids << simulator->deviceId();
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

void TcpServer::onNewConnection()
{
    while (m_server.hasPendingConnections())
    {
        QTcpSocket* socket = m_server.nextPendingConnection();
        if (socket == nullptr)
        {
            break;
        }

        const QString peer = QStringLiteral("%1:%2")
                                 .arg(socket->peerAddress().toString())
                                 .arg(socket->peerPort());

        Connection connection;
        connection.socket = socket;
        connection.peer   = peer;
        m_clients.insert(socket, connection);

        connect(socket, &QTcpSocket::disconnected, this, &TcpServer::onClientDisconnected);
        connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket, peer](
                                                              QAbstractSocket::SocketError error) {
            Q_UNUSED(error)
            SAIM_WARN(logcat::kNetwork,
                      QStringLiteral("Client %1 socket error: %2").arg(peer, socket->errorString()));
        });

        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);   // disable Nagle
        SAIM_INFO(logcat::kNetwork,
                  QStringLiteral("Client connected: %1 (total %2)").arg(peer).arg(m_clients.size()));
        emit clientConnected(peer);
    }
}

void TcpServer::onClientDisconnected()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (socket == nullptr)
    {
        return;
    }

    const auto it = m_clients.find(socket);
    if (it != m_clients.end())
    {
        const QString peer = it->peer;
        const quint64 bytes = it->bytesSent;
        m_clients.erase(it);
        SAIM_INFO(logcat::kNetwork,
                  QStringLiteral("Client disconnected: %1 (%2 bytes sent, %3 remaining)")
                      .arg(peer)
                      .arg(bytes)
                      .arg(m_clients.size()));
        emit clientDisconnected(peer);
    }
    socket->deleteLater();
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

bool TcpServer::mayInjectAnomaly()
{
    if (!m_options.injectAnomalies || m_options.anomalyRatePercent <= 0)
    {
        return false;
    }
    std::uniform_int_distribution<int> distribution(1, 100);
    return distribution(m_rng) <= m_options.anomalyRatePercent;
}

void TcpServer::onSampleTick()
{
    if (m_simulators.empty())
    {
        return;
    }

    for (const auto& simulator : m_simulators)
    {
        if (mayInjectAnomaly())
        {
            // A short, sharp excursion - the transient case the anomaly detector
            // is meant to catch, as opposed to the slow wear trend.
            std::uniform_int_distribution<int> length(2, 6);
            std::uniform_real_distribution<double> magnitude(2.0, 4.0);
            simulator->injectTransientFault(length(m_rng), magnitude(m_rng));
        }

        const SensorSample sample = simulator->step();
        const QByteArray frame = ProtocolCodec::encodeSample(sample);

        emit sampleEmitted(sample);

        // Send the same frame to every subscriber; a new client is caught up on
        // the next tick at 500 ms, so no snapshot handshake is required for the
        // dashboard to look live immediately.
        // abort()/write() can synchronously emit disconnected and erase entries.
        // Iterate a key snapshot and never retain an iterator across socket I/O.
        const auto sockets = m_clients.keys();
        for (QTcpSocket* socket : sockets)
        {
            auto it = m_clients.find(socket);
            if (it == m_clients.end()) continue;
            if (socket == nullptr || socket->state() != QAbstractSocket::ConnectedState)
            {
                m_clients.erase(it);
                continue;
            }

            if (socket->bytesToWrite() > kWriteHighWaterMark)
            {
                SAIM_ERROR(logcat::kNetwork,
                           QStringLiteral("Client %1 stalled (%2 bytes queued) - disconnecting")
                               .arg(it->peer)
                               .arg(socket->bytesToWrite()));
                socket->abort();
                continue;
            }

            const qint64 written = socket->write(frame);
            it = m_clients.find(socket);
            if (written > 0 && it != m_clients.end())
            {
                it->bytesSent += static_cast<quint64>(written);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Manual fault injection (UI / tests)
// ---------------------------------------------------------------------------

void TcpServer::injectFault(const QString& deviceId, int samples)
{
    for (const auto& simulator : m_simulators)
    {
        if (simulator->deviceId() == deviceId)
        {
            simulator->injectFailure(samples);
            return;
        }
    }
    SAIM_WARN(logcat::kNetwork,
              QStringLiteral("injectFault: unknown simulated device '%1'").arg(deviceId));
}

void TcpServer::setWearLevel(const QString& deviceId, double fraction)
{
    for (const auto& simulator : m_simulators)
    {
        if (simulator->deviceId() == deviceId)
        {
            simulator->setWearLevel(fraction);
            return;
        }
    }
    SAIM_WARN(logcat::kNetwork,
              QStringLiteral("setWearLevel: unknown simulated device '%1'").arg(deviceId));
}

void TcpServer::emitBurst(int count)
{
    for (int i = 0; i < std::max(1, count); ++i)
    {
        onSampleTick();
    }
}

}   // namespace saim
