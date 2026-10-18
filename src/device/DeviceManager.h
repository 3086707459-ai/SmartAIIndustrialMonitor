#pragma once

// ---------------------------------------------------------------------------
// DeviceManager.h - registry and router for every device on the line.
//
// Responsibilities (and deliberately nothing else):
//
//   * owns the Device objects and their lifetime
//   * routes an incoming sample to the right Device by id
//   * owns the liveness timer (one timer for the whole fleet, not one per
//     device - 6 devices x 1 timer would be 6 wake-ups where 1 does)
//   * aggregates fleet-level statistics for the dashboard header
//   * offers a thread-safe snapshot API for workers that need history
//
// It does NOT talk to the network, the database, or the UI.  The controller
// wires those together.  Keeping the registry free of I/O is what makes it
// unit-testable without a socket or a database file.
// ---------------------------------------------------------------------------

#include "common/Result.h"
#include "common/Types.h"
#include "device/Device.h"

#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QTimer>

#include <memory>
#include <vector>

namespace saim {

class ConfigManager;

class DeviceManager final : public QObject
{
    Q_OBJECT
public:
    explicit DeviceManager(ConfigManager* config, QObject* parent = nullptr);
    ~DeviceManager() override;

    DeviceManager(const DeviceManager&)            = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // -- population ---------------------------------------------------------

    /// Creates a device.  Fails if the id is empty or already registered.
    Status addDevice(const QString& deviceId,
                     const QString& deviceName = {},
                     const QString& deviceType = QStringLiteral("generic"),
                     const QString& ipAddress = {},
                     quint16 port = 0);

    Status removeDevice(const QString& deviceId);
    Status updateDevice(const QString& deviceId, const QString& deviceName,
                        const QString& ipAddress, quint16 port);
    Status setDeviceEnabled(const QString& deviceId, bool enabled);
    Status setDeviceLimits(const QString& deviceId, const DeviceLimits& limits);

    /// Builds the fleet from the "devices" array in config.json.
    /// Called on every config reload, and it reconciles rather than rebuilds:
    /// devices that already exist keep their history and state.
    void loadFromConfig();

    /// Removes every device.  Used by tests and by "clear configuration".
    void clear();

    // -- queries ------------------------------------------------------------

    [[nodiscard]] bool contains(const QString& deviceId) const;
    [[nodiscard]] std::shared_ptr<Device> device(const QString& deviceId) const;
    [[nodiscard]] QList<std::shared_ptr<Device>> devices() const;
    [[nodiscard]] QStringList deviceIds() const;
    [[nodiscard]] int deviceCount() const;

    /// Fleet roll-up for the dashboard header: totals per state.
    struct FleetSummary
    {
        int total   = 0;
        int online  = 0;
        int normal  = 0;
        int warning = 0;
        int fault   = 0;
        int offline = 0;
        double averageTemperature = 0.0;
        double averageHealth      = 100.0;
        qint64 totalSamples       = 0;
        qint64 totalAlarms        = 0;
    };
    [[nodiscard]] FleetSummary summary() const;

    /// Copy of a device's rolling window, safe to call from a worker thread.
    /// Returns an empty vector for an unknown id; the caller decides whether
    /// that is an error (the inference worker simply skips the device).
    [[nodiscard]] std::vector<SensorSample> historySnapshot(const QString& deviceId,
                                                            int maxSamples = 0) const;

    /// Copy of every device's metadata, for the persistence and report layers.
    [[nodiscard]] QJsonArray snapshotJson() const;

signals:
    void deviceAdded(const QString& deviceId);
    void deviceRemoved(const QString& deviceId);
    void deviceMetadataChanged(const QString& deviceId);

    // Re-broadcast from the individual devices so consumers (persistence, UI,
    // AI) can connect once to the manager instead of tracking every device.
    void sampleReceived(const saim::SensorSample& sample);
    void stateChanged(const QString& deviceId, saim::DeviceState state);
    void alarmRaised(const saim::AlarmRecord& alarm);
    void predictionUpdated(const saim::FaultPrediction& prediction);
    void anomalyDetected(const saim::AnomalyEvent& event);
    void summaryChanged(const saim::DeviceManager::FleetSummary& summary);

public slots:
    /// Routes a decoded sample into the registry.
    void onSampleReceived(const saim::SensorSample& sample);

    /// Called by the liveness timer.
    void onLivenessTick();

    /// Recomputes and publishes the fleet summary.  Called on a coalescing timer
    /// rather than on every sample: recomputing over 6 devices at 20 Hz to
    /// repaint a header is wasted work.
    void refreshSummary();

private:
    [[nodiscard]] Device* findDevice(const QString& deviceId) const;
    void connectDeviceSignals(const std::shared_ptr<Device>& device);

    /// The map itself is only touched on the UI thread; the read-write lock
    /// exists for historySnapshot(), which workers call from other threads
    /// while the UI thread may be adding or removing a device.
    mutable QReadWriteLock m_lock{QReadWriteLock::Recursive};
    QHash<QString, std::shared_ptr<Device>> m_devices;

    ConfigManager* m_config = nullptr;
    QTimer         m_livenessTimer;
    QTimer         m_summaryTimer;
    int            m_offlineTimeoutMs = 15000;

    /// Cheap counters maintained incrementally so that summary() does not have
    /// to walk the fleet - it is queried on every UI refresh.
    FleetSummary m_cachedSummary;
};

}   // namespace saim

// The summary travels inside `summaryChanged` as a queued signal payload when a
// consumer lives on another thread, so it needs a registered metatype.  It is a
// trivially copyable aggregate, so the default Qt marshalling is enough.
Q_DECLARE_METATYPE(saim::DeviceManager::FleetSummary)
