#pragma once

// ---------------------------------------------------------------------------
// DataRepository.h - every SQL statement in the application lives here.
//
// Why a repository and not "just call QSqlQuery where you need data"
// ------------------------------------------------------------------------
//  * One place to audit.  A reviewer can read one file and know the application
//    cannot be SQL-injected, because every statement here is prepared and every
//    value is bound - never concatenated.
//  * Testability.  The domain and AI layers depend on this interface, not on
//    QSqlDatabase, so a test can drive them against an in-memory database.
//  * Performance.  Batch inserts are a property of this layer, so an optimisation
//    (one transaction, one prepared statement, rebind in a loop) is written once
//    instead of being rediscovered at each call site.
//
// Thread affinity: a DataRepository is bound to the DatabaseManager it is
// constructed with, and therefore to that thread.  The persistence worker owns
// its own pair.
// ---------------------------------------------------------------------------

#include "common/Result.h"
#include "common/Types.h"
#include "core/Logger.h"
#include "database/DatabaseManager.h"

#include <QDateTime>
#include <QString>
#include <QVector>

class QSqlQuery;

namespace saim {

/// One row of the dashboard's history query.
struct SensorRecord
{
    qint64      id = -1;
    QString     deviceId;
    QDateTime   timestamp;
    double      temperature = 0.0;
    double      voltage     = 0.0;
    double      current     = 0.0;
    double      vibration   = 0.0;
    double      rpm         = 0.0;
    DeviceState state = DeviceState::Normal;
};

/// Filter for history queries; every field is optional.
struct HistoryQuery
{
    QString   deviceId;             ///< empty -> all devices
    QDateTime from;
    QDateTime to;
    int       limit = 1000;
    bool      ascending = true;     ///< chronological order (charts want this)
    DeviceState minState = DeviceState::Offline;   ///< inclusive filter
};

class DataRepository
{
public:
    /// `manager` must outlive this object.
    explicit DataRepository(DatabaseManager* manager);
    ~DataRepository() = default;

    DataRepository(const DataRepository&)            = delete;
    DataRepository& operator=(const DataRepository&) = delete;

    // -- writes -------------------------------------------------------------

    /// Inserts many samples in ONE transaction with ONE prepared statement.
    ///
    /// Why this matters: a naive loop of individual INSERTs on SQLite costs one
    /// fsync per row.  At 6 devices x 2 Hz that is 12 fsyncs a second and the
    /// worker falls behind within minutes.  Batching turns it into 1 transaction
    /// per flush interval, which is a two-orders-of-magnitude difference and the
    /// single highest-leverage optimisation in the persistence path.
    [[nodiscard]] Result<int> insertSamples(const QVector<SensorRecord>& samples);

    [[nodiscard]] Status insertSample(const SensorRecord& sample);

    [[nodiscard]] Status insertAlarm(const AlarmRecord& alarm);

    [[nodiscard]] Status insertPrediction(const FaultPrediction& prediction);

    [[nodiscard]] Status insertAnomaly(const AnomalyEvent& event);

    /// Maintenance advice produced by the assistant.
    [[nodiscard]] Status insertAdvice(const QString& deviceId, const QString& summary,
                                      const QString& rootCause, const QString& action,
                                      const QString& riskLevel, const QString& source);

    /// Upserts a device row.  Called whenever the fleet metadata changes, so the
    /// Device table always reflects the running configuration.
    [[nodiscard]] Status upsertDevice(const QString& deviceId, const QString& name,
                                      const QString& type, const QString& ip, quint16 port,
                                      DeviceState state, bool enabled);

    [[nodiscard]] Status updateDeviceState(const QString& deviceId, DeviceState state);

    [[nodiscard]] Status acknowledgeAlarm(qint64 alarmId);
    [[nodiscard]] Result<int> acknowledgeAllAlarms(const QString& deviceId);

    /// Persists logger output.  The Logger forwards records here through the
    /// persistence worker, so the SystemLog table is a durable copy of the
    /// in-memory log - which is what makes a post-mortem possible after a crash.
    [[nodiscard]] Result<int> insertLogRecords(const QVector<LogRecord>& records);

    // -- reads --------------------------------------------------------------

    [[nodiscard]] Result<QVector<SensorRecord>> queryHistory(const HistoryQuery& query) const;

    /// Latest sample per device, in one query.  The dashboard needs exactly this
    /// on start-up, and N per-device queries would be N round trips.
    [[nodiscard]] Result<QVector<SensorRecord>> queryLatestPerDevice() const;

    [[nodiscard]] Result<QVector<AlarmRecord>> queryAlarms(const QString& deviceId,
                                                           int limit,
                                                           bool onlyUnacknowledged) const;

    [[nodiscard]] Result<QVector<FaultPrediction>> queryPredictions(const QString& deviceId,
                                                                    int limit) const;

    /// Aggregate statistics for the report generator, computed in SQL rather than
    /// by pulling every row into memory.  On a month of 2 Hz data per device that
    /// is the difference between a few milliseconds and a 500 MB allocation.
    [[nodiscard]] Result<DeviceStatistics> queryStatistics(const QString& deviceId,
                                                            const QDateTime& from,
                                                            const QDateTime& to) const;

    [[nodiscard]] Result<int> countSamplesSince(const QDateTime& since) const;

private:
    /// Maps a SensorData row onto the domain type.
    [[nodiscard]] static SensorRecord readSensorRecord(const QSqlQuery& query);

    DatabaseManager* m_manager = nullptr;
};

}   // namespace saim
