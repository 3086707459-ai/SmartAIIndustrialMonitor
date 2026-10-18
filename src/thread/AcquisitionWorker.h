#pragma once
#include <atomic>

// ---------------------------------------------------------------------------
// AcquisitionWorker.h - owns the TCP client and the decode/dispatch pipeline.
//
// First stage of the data path:
//
//     socket -> frame decode -> bounded mailboxes -> consumer timers
//
// Everything expensive happens here, on a thread with no widgets: socket reads,
// framing, JSON parsing and the conversion to a domain type.  What crosses to
// the UI thread is a small, fully-decoded value.
//
// sampleAcquired is wired directly to thread-safe bounded mailbox entry points.
// UI coalescing lives in the controller's mailbox, so a stalled UI creates no
// per-sample queued Qt events. Persistence may briefly wait and then reject;
// every rejected sample is counted and surfaced by pipeline statistics.
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "network/TcpClient.h"
#include "thread/ThreadedWorker.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

namespace saim {

class ConfigManager;

class AcquisitionWorker final : public ThreadedWorker
{
    Q_OBJECT
public:
    explicit AcquisitionWorker(ConfigManager* config, QObject* parent = nullptr);
    ~AcquisitionWorker() override;

    [[nodiscard]] ConnectionState connectionState() const noexcept { return m_connectionState; }
    [[nodiscard]] TcpClient::Statistics statistics() const { QMutexLocker lock(&m_statisticsMutex); return m_statistics; }

    /// Decoded samples since start, and a rolling rate estimate.
    [[nodiscard]] quint64 receivedSampleCount() const noexcept { return m_receivedSamples; }
    [[nodiscard]] double samplesPerSecond() const noexcept { return m_samplesPerSecond; }

public slots:
    /// Issues the connect on the worker thread.
    void connectToGateway();

    /// Closes the link cleanly; the worker thread keeps running.
    void disconnectFromGateway();

    /// Sends a command frame to one device through the gateway.
    void sendCommand(const QString& deviceId, const QString& command);

signals:
    void connectionStateChanged(saim::ConnectionState state);

    /// Every decoded sample, in arrival order.
    void sampleAcquired(const saim::SensorSample& sample);

    void statisticsUpdated(const saim::TcpClient::Statistics& statistics);
    void gatewayError(const QString& message);

protected:
    void onStart() override;
    void onStop() override;

private:
    void applyOptionsFromConfig();
    void updateRateEstimate();

    ConfigManager* m_config = nullptr;

    /// Created inside onStart(), i.e. on the worker thread - see the comment
    /// there for why that matters.
    TcpClient* m_client = nullptr;

    std::atomic<ConnectionState> m_connectionState{ConnectionState::Disconnected};
    mutable QMutex m_statisticsMutex;
    TcpClient::Statistics m_statistics;

    std::atomic<quint64> m_receivedSamples{0};
    std::atomic<double> m_samplesPerSecond{0.0};
    qint64        m_rateWindowStartMs = 0;
    int           m_rateWindowCount = 0;
    QElapsedTimer m_uptime;
};

}   // namespace saim
