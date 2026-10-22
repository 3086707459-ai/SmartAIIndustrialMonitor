#pragma once

// ---------------------------------------------------------------------------
// Device.h - the runtime model of one industrial asset.
//
// A Device is *state*, not behaviour: it owns the latest known telemetry, the
// derived health state and the alarm bookkeeping, and it announces every change
// through signals.  It deliberately knows nothing about sockets, SQL or Qt
// Charts, which is what lets the same object be driven by a real PLC, by the
// simulator, or by a unit test.
//
// Threading contract
// ------------------
// A Device is *owned* by the DeviceManager, which lives on the main (UI) thread.
// Worker threads never touch the object directly; the controller drains bounded
// value mailboxes on the UI thread. "affinity" is therefore always the UI thread, and every
// mutation happens there - which removes an entire class of data race by
// construction rather than by locking.
//
// This rolling window serves the UI and report snapshots. AIWorker maintains
// its own sample window and never reads Device from a worker thread.
// ---------------------------------------------------------------------------

#include "common/RingBuffer.h"
#include "common/Types.h"

#include <QObject>
#include <QElapsedTimer>
#include <QString>
#include <QVector>

#include <memory>

namespace saim {

class Device final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString deviceId READ deviceId CONSTANT)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY metadataChanged)
    Q_PROPERTY(int state READ stateInt NOTIFY statusChanged)

public:
    /// `historyCapacity` bounds memory per device:
    /// capacity * sizeof(SensorSample) * deviceCount.  At 60 samples/device and
    /// 6 devices that is a few tens of kB - trivial, and enough for the models.
    explicit Device(QString deviceId,
                    QString deviceName = {},
                    QObject* parent = nullptr);
    ~Device() override;

    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;

    // -- identity -----------------------------------------------------------

    [[nodiscard]] QString deviceId() const noexcept { return m_deviceId; }
    [[nodiscard]] QString deviceName() const noexcept { return m_deviceName; }
    [[nodiscard]] QString deviceType() const noexcept { return m_deviceType; }
    [[nodiscard]] QString ipAddress() const noexcept { return m_ipAddress; }
    [[nodiscard]] quint16 port() const noexcept { return m_port; }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }

    void setDeviceName(const QString& name);
    void setDeviceType(const QString& type);
    void setEndpoint(const QString& ip, quint16 port);
    void setLimits(const DeviceLimits& limits);
    void setEnabled(bool enabled);
    void setHistoryCapacity(std::size_t capacity);

    [[nodiscard]] const DeviceLimits& limits() const noexcept { return m_limits; }

    // -- live state ---------------------------------------------------------

    [[nodiscard]] DeviceState state() const noexcept { return m_state; }
    [[nodiscard]] int stateInt() const noexcept { return static_cast<int>(m_state); }

    /// Latest sample.  `isValid()` is false before the first frame arrives.
    [[nodiscard]] const SensorSample& latest() const noexcept { return m_latest; }

    [[nodiscard]] QDateTime lastSeen() const noexcept { return m_lastSeen; }
    [[nodiscard]] qint64 sampleCount() const noexcept { return m_sampleCount; }
    [[nodiscard]] qint64 alarmCount() const noexcept { return m_alarmCount; }

    /// Most recent AI verdict for this device (default-constructed until the
    /// inference worker has produced one).
    [[nodiscard]] const FaultPrediction& prediction() const noexcept { return m_prediction; }
    [[nodiscard]] const AnomalyEvent& lastAnomaly() const noexcept { return m_lastAnomaly; }

    /// Rolling window, oldest first.  Copy - callers on other threads mutate
    /// their copy, never ours.
    [[nodiscard]] std::vector<SensorSample> history() const;
    [[nodiscard]] std::size_t historySize() const noexcept { return m_history.size(); }

    /// Aggregate over the retained window - used by the report generator.
    [[nodiscard]] DeviceStatistics statistics() const;

    /// A machine-readable snapshot for the protocol / persistence layers.
    [[nodiscard]] QJsonObject toJson() const;

    /// Applies one complete worker result atomically. Returns false when the
    /// device has no live telemetry or either result belongs to another device.
    bool applyAnalysis(const FaultPrediction& prediction, const AnomalyEvent& anomaly);

public slots:
    /// Telemetry entry point, called by the controller on the UI thread.
    void onSampleReceived(const saim::SensorSample& sample);

    /// Applies an AI verdict produced off-thread.
    void onPredictionReady(const saim::FaultPrediction& prediction);

    /// Applies an anomaly verdict produced off-thread.
    void onAnomalyDetected(const saim::AnomalyEvent& event);

    /// Marks the device offline if no telemetry arrived within `timeoutMs`.
    /// Driven by a QTimer owned by the manager, not by a thread of its own.
    void checkLiveness(int timeoutMs);

    /// Clears the alarm latch so the device can re-alarm after maintenance.
    void acknowledgeAlarm();

signals:
    void statusChanged(const QString& deviceId, saim::DeviceState state);
    void dataUpdated(const saim::SensorSample& sample);
    void alarmTriggered(const saim::AlarmRecord& alarm);
    void predictionUpdated(const saim::FaultPrediction& prediction);
    void anomalyDetected(const saim::AnomalyEvent& event);
    void metadataChanged();

private:
    /// Recomputes the state from the rule-based envelope and the AI verdict, and
    /// emits statusChanged() only on an actual transition.
    void reevaluateState(bool fromTelemetry);

    /// Builds and emits an alarm, honouring the repeat-suppression window so a
    /// device stuck in Warning does not generate 20 identical alarms a second.
    void raiseAlarm(DeviceState state, const QString& message, int suppressionMs);
    [[nodiscard]] QString describeAnomaly(const SensorSample& sample) const;
    [[nodiscard]] bool acceptsDiagnostic(const QString& deviceId, const QDateTime& timestamp) const;
    void clearDiagnostics();

    QString   m_deviceId;
    QString   m_deviceName;
    QString   m_deviceType = QStringLiteral("generic");
    QString   m_ipAddress;
    quint16   m_port = 0;
    bool      m_enabled = true;

    DeviceLimits m_limits;

    DeviceState   m_state = DeviceState::Offline;
    SensorSample  m_latest;
    QDateTime     m_lastSeen;
    QElapsedTimer m_lastTelemetryClock;
    qint64        m_sampleCount = 0;
    qint64        m_alarmCount  = 0;

    RingBuffer<SensorSample> m_history;
    FaultPrediction          m_prediction;
    AnomalyEvent             m_lastAnomaly;

    // Alarm bookkeeping for repeat suppression.
    DeviceState m_lastAlarmedState = DeviceState::Normal;
    QElapsedTimer m_lastAlarmClock;
    bool        m_alarmLatched = false;
};

}   // namespace saim
