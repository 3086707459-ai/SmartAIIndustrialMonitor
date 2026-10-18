#pragma once

// ---------------------------------------------------------------------------
// PersistenceWorker.h - owns the SQLite connection and every write to it.
//
// Why persistence gets its own thread
// -----------------------------------
// SQLite commits are durable, and durable means fsync.  An fsync on a consumer
// SSD costs single-digit milliseconds; on a spinning disk or a network volume it
// costs tens.  Doing that on the UI thread would drop frames every flush.  Doing
// it on the acquisition thread would stall the socket read loop and the gateway
// would eventually time out.  So it gets its own thread, and the only thing that
// crosses into it is a value.
//
// Batching, and why the numbers are what they are
// ----------------------------------------------
// Samples arrive on `sampleAcquired` at the full inbound rate.  Writing each one
// immediately is the naive approach and it is wrong twice over: one transaction
// per row (one fsync per row), and one queued call per row, which floods the
// worker's event queue.
//
// Instead samples accumulate in a buffer and flush when *either* condition holds:
//
//   * `flushIntervalMs` has elapsed - bounds data loss on an unexpected exit to
//     one interval, and keeps the dashboard's history query fresh.
//   * the buffer reached `maxBatchSize` - bounds memory if the inbound rate
//     spikes, and keeps the multi-row INSERT inside SQLite's parameter limit.
//
// Both bounds are needed.  A time-only policy allocates without limit during a
// burst; a size-only policy never writes anything on a quiet line, so the last
// hour of data before a crash is lost.
//
// Queries
// -------
// Reads are asynchronous request/response keyed by a caller-supplied id, not
// blocking calls.  A blocking query issued from the UI thread while the worker
// is mid-commit would freeze the interface for the duration of that commit - the
// exact stall this thread exists to prevent.
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "database/DataRepository.h"
#include "database/DatabaseManager.h"
#include "thread/ThreadedWorker.h"
#include "thread/SampleInbox.h"
#include "thread/RecordInbox.h"
#include <atomic>

#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QVector>

#include <memory>

namespace saim {

class ConfigManager;

class PersistenceWorker final : public ThreadedWorker
{
    Q_OBJECT
public:
    explicit PersistenceWorker(ConfigManager* config, QObject* parent = nullptr);
    ~PersistenceWorker() override;

    // Thread-safe producer entry; storeSample itself remains worker-thread-only.
    bool enqueueSample(const SensorSample& sample) { return m_inbox.push(sample); }
    [[nodiscard]] SampleInbox::Statistics inboxStatistics() const { return m_inbox.statistics(); }
    [[nodiscard]] quint64 droppedSamples() const noexcept { return m_droppedSamples.load(); }
    bool enqueueRecord(RecordInbox::Record record) { return m_records.push(std::move(record)); }
    [[nodiscard]] RecordInbox::Statistics recordStatistics() const { return m_records.statistics(); }
    [[nodiscard]] quint64 failedRecords() const noexcept { return m_failedRecords.load(); }

    [[nodiscard]] bool isDatabaseOpen() const noexcept { return m_open; }
    [[nodiscard]] DatabaseManager::HealthReport health() const { return m_health; }

    /// Rows written since start.  Surfaced in the diagnostics panel - a
    /// persistence layer that has silently stopped writing is a worse failure
    /// than one that crashed, because nothing looks wrong.
    [[nodiscard]] quint64 rowsWritten() const noexcept { return m_rowsWritten; }
    [[nodiscard]] quint64 batchesWritten() const noexcept { return m_batchesWritten; }

    /// Requests the worker flush its buffer now.  Queued, so it returns
    /// immediately; `batchCommitted` reports when it actually happened.
    void requestFlush();

public slots:
    void setDevices(const QVector<saim::DeviceConfiguration>& devices);
    void applyConfiguration();
    // -- writes (queued from the acquisition / AI threads) -------------------

    void storeSample(const saim::SensorSample& sample);
    void storeAlarm(const saim::AlarmRecord& alarm);
    void storePrediction(const saim::FaultPrediction& prediction);
    void storeAnomaly(const saim::AnomalyEvent& event);
    void storeAdvice(const QString& deviceId, const QString& summary,
                     const QString& rootCause, const QString& action,
                     const QString& riskLevel, const QString& source);
    void storeLogRecords(const QVector<saim::LogRecord>& records);
    void upsertDevice(const QString& deviceId, const QString& name, const QString& type,
                      const QString& ip, quint16 port, int state, bool enabled);
    void updateDeviceState(const QString& deviceId, int state);

    // -- reads (async, keyed) ------------------------------------------------

    void requestHistory(quint64 requestId, const saim::HistoryQuery& query);
    void requestLatestSamples(quint64 requestId);
    void requestAlarms(quint64 requestId, const QString& deviceId, int limit,
                       bool onlyUnacknowledged);
    void requestStatistics(quint64 requestId, const QString& deviceId,
                           const QDateTime& from, const QDateTime& to);
    void requestHealthCheck(quint64 requestId);

    // -- maintenance ---------------------------------------------------------

    void runRetention();

signals:
    void databaseOpened(const saim::DatabaseManager::HealthReport& health);
    void databaseFailed(const QString& reason);

    /// Emitted after a successful commit; `rows` is the batch size.
    void batchCommitted(int rows);

    void historyReady(quint64 requestId, const QVector<saim::SensorRecord>& records);
    void latestSamplesReady(quint64 requestId, const QVector<saim::SensorRecord>& records);
    void alarmsReady(quint64 requestId, const QVector<saim::AlarmRecord>& alarms);
    void statisticsReady(quint64 requestId, const saim::DeviceStatistics& statistics);
    void healthCheckReady(quint64 requestId, const saim::DatabaseManager::HealthReport& health);

    /// Any read or write that failed, already formatted for the operator.
    void persistenceError(const QString& message);

protected:
    void onStart() override;
    void onStop() override;

private slots:
    void drainInbox();
    void drainRecords();
    /// Timer tick: write whatever has accumulated.
    void flushBuffer();

private:
    void openDatabase();
    void emitError(const QString& context, const QString& detail);

    ConfigManager* m_config = nullptr;
    SampleInbox m_inbox;
    RecordInbox m_records;
    std::atomic<quint64> m_failedRecords{0};
    QTimer m_inboxTimer;
    QVector<DeviceConfiguration> m_devices;
    QHash<QString, DeviceLimits> m_deviceLimits;

    // Created on the worker thread in onStart() - see DatabaseManager's comment
    // on QSqlDatabase thread affinity.  Constructing them here would bind the
    // connection to the UI thread and every subsequent query would fail.
    std::unique_ptr<DatabaseManager> m_database;
    std::unique_ptr<DataRepository>  m_repository;

    DatabaseManager::HealthReport m_health;
    bool m_open = false;

    QVector<SensorRecord> m_sampleBuffer;
    QTimer m_flushTimer;

    std::atomic<quint64> m_rowsWritten{0};
    std::atomic<quint64> m_batchesWritten{0};
    std::atomic<quint64> m_droppedSamples{0};
    QElapsedTimer m_uptime;
    bool m_stopping = false;
    qint64 m_retryAfterMs = 0;
};

}   // namespace saim
