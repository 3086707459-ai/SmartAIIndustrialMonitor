#pragma once

// ---------------------------------------------------------------------------
// TcpServer.h - the FakeDeviceServer: a plant gateway that streams telemetry.
//
// Why this exists in the shipped tree rather than in tests
// --------------------------------------------------------
// Everything downstream of the socket - framing, decoding, the device state
// machine, the anomaly detector, the predictor, the UI - is only demonstrable
// against a live stream.  A reviewer cloning this repository has no PLC, so the
// simulator *is* the plant.  Keeping it as a first-class module (not a test
// fixture) means the same binary that CI exercises is the one used for a demo.
//
// Threading
// ---------
// The server owns one QTcpServer and a hash of accepted sockets, and runs on a
// worker thread obtained from moveToThread().  The sampling tick is a QTimer on
// that same thread, so the socket writes and the model stepping never contend
// with the UI.
//
// Back-pressure
// -------------
// A slow or wedged client must not grow the server's write buffer without
// bound.  Each connection therefore has a high-water mark: once exceeded, that
// client is disconnected rather than allowed to consume the process's memory.
// This is the kind of detail that separates a demo from something deployable.
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "device/DeviceSimulator.h"

#include <QHash>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <memory>
#include <random>
#include <vector>

namespace saim {

class TcpServer final : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString listenAddress = QStringLiteral("0.0.0.0");
        quint16 port          = 9100;
        int     sampleIntervalMs = 500;
        bool    injectAnomalies  = true;
        int     anomalyRatePercent = 3;
        /// Empty -> the server synthesises a default motor/pump/spindle line.
        QStringList deviceIds;
        bool useDefaultFleetWhenEmpty = true;
        quint32 seed = 20260101;
    };

    explicit TcpServer(QObject* parent = nullptr);
    ~TcpServer() override;

    TcpServer(const TcpServer&)            = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void setOptions(const Options& options);
    [[nodiscard]] const Options& options() const noexcept { return m_options; }

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] quint16 listeningPort() const;
    [[nodiscard]] int clientCount() const noexcept { return static_cast<int>(m_clients.size()); }

    /// The simulated fleet, for tests and for the --print-fleet CLI flag.
    [[nodiscard]] QStringList simulatedDeviceIds() const;

public slots:
    void start();
    void stop();

    /// Forces one device into sustained failure - wired to the UI's
    /// "simulate fault" action so an interviewer can watch the whole pipeline
    /// react without waiting for wear to accumulate.
    void injectFault(const QString& deviceId, int samples);

    /// Sets the simulated bearing wear (0..1) for one device.
    void setWearLevel(const QString& deviceId, double fraction);

    /// Emits one frame immediately for every device, independent of the timer.
    /// Used by tests to avoid sleeping on wall-clock time.
    void emitBurst(int count = 1);

signals:
    void started(quint16 port);
    void stopped();
    void clientConnected(const QString& peerAddress);
    void clientDisconnected(const QString& peerAddress);
    /// Mirrors what was put on the wire, so a test can assert on the stream
    /// without a socket.
    void sampleEmitted(const saim::SensorSample& sample);
    void errorOccurred(const QString& message);

private slots:
    void onNewConnection();
    void onClientDisconnected();
    void onSampleTick();

private:
    struct Connection
    {
        QTcpSocket* socket = nullptr;
        QString     peer;
        quint64     bytesSent = 0;
    };

    void registerDefaultFleet();
    [[nodiscard]] bool mayInjectAnomaly();

    QTcpServer   m_server;
    QTimer       m_sampleTimer;
    Options      m_options;

    QHash<QTcpSocket*, Connection> m_clients;
    std::vector<std::unique_ptr<DeviceSimulator>> m_simulators;
    std::mt19937 m_rng;

    /// Above this many queued bytes for a single client, drop that client.
    static constexpr qint64 kWriteHighWaterMark = 4LL * 1024 * 1024;
};

}   // namespace saim
