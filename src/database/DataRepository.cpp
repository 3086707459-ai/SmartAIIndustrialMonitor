#include "database/DataRepository.h"

#include "core/Logger.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace saim {

namespace {

/// Binds a QDateTime as an ISO-8601 string.
///
/// Deliberately *not* storing a Unix timestamp: an ISO string sorts
/// lexicographically in the same order as chronologically (so the index on
/// `timestamp` works for range scans), it survives a timezone change, and it is
/// readable when someone opens the .db in a GUI tool at 3 a.m. during an outage.
QString encodeTimestamp(const QDateTime& value)
{
    const QDateTime effective = value.isValid() ? value : QDateTime::currentDateTime();
    return effective.toString(Qt::ISODateWithMs);
}

QDateTime decodeTimestamp(const QString& value)
{
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid())
    {
        parsed = QDateTime::fromString(value, Qt::ISODate);
    }
    return parsed;
}

DeviceState stateFromString(const QString& text)
{
    if (text == QLatin1String("NORMAL"))  return DeviceState::Normal;
    if (text == QLatin1String("WARNING")) return DeviceState::Warning;
    if (text == QLatin1String("FAULT"))   return DeviceState::Fault;
    return DeviceState::Offline;
}

/// Builds a parenthesised, comma-separated run of `?` placeholders.
QString placeholders(int count)
{
    QStringList list;
    list.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        list << QStringLiteral("?");
    }
    return list.join(QLatin1Char(','));
}

}   // namespace

DataRepository::DataRepository(DatabaseManager* manager)
    : m_manager(manager)
{
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

Result<int> DataRepository::insertSamples(const QVector<SensorRecord>& samples)
{
    if (samples.isEmpty())
    {
        return Result<int>::ok(0);
    }
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<int>::fail(1, QStringLiteral("database is not open"));
    }

    QSqlDatabase db = m_manager->database();

    // A batch can be larger than SQLite's default limit on host parameters
    // (SQLITE_MAX_VARIABLE_NUMBER, 999 in older builds).  Chunking keeps the
    // batch insert from failing on a large backlog after a reconnect.
    constexpr int kColumnsPerRow = 8;
    constexpr int kMaxParameters = 900;
    constexpr int kChunkSize = kMaxParameters / kColumnsPerRow;   // 112 rows

    if (!db.transaction())
    {
        return Result<int>::fail(2, QStringLiteral("cannot begin transaction: %1")
                                        .arg(db.lastError().text()));
    }

    int inserted = 0;

    for (int offset = 0; offset < samples.size(); offset += kChunkSize)
    {
        const int count =
            qMin(kChunkSize, static_cast<int>(samples.size()) - offset);

        // Multi-row VALUES: one statement per chunk instead of one per row.
        QString sql = QStringLiteral(
            "INSERT INTO SensorData "
            "(device_id, temperature, voltage, current, vibration, rpm, state, timestamp) VALUES ");
        for (int row = 0; row < count; ++row)
        {
            if (row > 0)
            {
                sql += QStringLiteral(", ");
            }
            sql += QStringLiteral("(%1)").arg(placeholders(kColumnsPerRow));
        }

        QSqlQuery query(db);
        query.prepare(sql);

        for (int row = 0; row < count; ++row)
        {
            const SensorRecord& record = samples.at(offset + row);
            query.addBindValue(record.deviceId);
            query.addBindValue(record.temperature);
            query.addBindValue(record.voltage);
            query.addBindValue(record.current);
            query.addBindValue(record.vibration);
            query.addBindValue(record.rpm);
            query.addBindValue(toString(record.state));
            query.addBindValue(encodeTimestamp(record.timestamp));
        }

        if (!query.exec())
        {
            db.rollback();
            return Result<int>::fail(3, QStringLiteral("batch insert failed: %1")
                                            .arg(query.lastError().text()));
        }
        inserted += count;
    }

    if (!db.commit())
    {
        const QString detail = db.lastError().text();
        db.rollback();
        return Result<int>::fail(4, QStringLiteral("commit failed: %1").arg(detail));
    }

    SAIM_TRACE(logcat::kDatabase, QStringLiteral("Persisted %1 sensor row(s)").arg(inserted));
    return Result<int>::ok(inserted);
}

Status DataRepository::insertSample(const SensorRecord& sample)
{
    QVector<SensorRecord> single;
    single.append(sample);
    const Result<int> result = insertSamples(single);
    return result.isOk() ? Status::ok() : Status::fail(result.error().code, result.error().message);
}

Status DataRepository::insertAlarm(const AlarmRecord& alarm)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Status::fail(1, QStringLiteral("database is not open"));
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(
        "INSERT INTO Alarm (device_id, state, message, timestamp, acknowledged) "
        "VALUES (:device, :state, :message, :timestamp, 0)"));
    query.bindValue(QStringLiteral(":device"), alarm.deviceId);
    query.bindValue(QStringLiteral(":state"), toString(alarm.state));
    query.bindValue(QStringLiteral(":message"), alarm.message);
    query.bindValue(QStringLiteral(":timestamp"), encodeTimestamp(alarm.timestamp));

    if (!query.exec())
    {
        return Status::fail(2, query.lastError().text());
    }
    return Status::ok();
}

Status DataRepository::insertPrediction(const FaultPrediction& prediction)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Status::fail(1, QStringLiteral("database is not open"));
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(
        "INSERT INTO Prediction "
        "(device_id, health_score, failure_probability, fault_type, confidence, backend, timestamp) "
        "VALUES (:device, :health, :probability, :fault, :confidence, :backend, :timestamp)"));
    query.bindValue(QStringLiteral(":device"), prediction.deviceId);
    query.bindValue(QStringLiteral(":health"), prediction.healthScore);
    query.bindValue(QStringLiteral(":probability"), prediction.failureProbability);
    query.bindValue(QStringLiteral(":fault"), prediction.faultType);
    query.bindValue(QStringLiteral(":confidence"), prediction.confidence);
    query.bindValue(QStringLiteral(":backend"), prediction.backend);
    query.bindValue(QStringLiteral(":timestamp"), encodeTimestamp(prediction.timestamp));

    if (!query.exec())
    {
        return Status::fail(2, query.lastError().text());
    }
    return Status::ok();
}

Status DataRepository::insertAnomaly(const AnomalyEvent& event)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Status::fail(1, QStringLiteral("database is not open"));
    }

    QStringList featureNames;
    featureNames.reserve(event.offendingFeatures.size());
    for (const int index : event.offendingFeatures)
    {
        if (index >= 0 && index < kFeatureCount)
        {
            featureNames << QString::fromLatin1(kFeatureNames[static_cast<std::size_t>(index)]);
        }
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(
        "INSERT INTO Anomaly (device_id, level, score, reason, features, timestamp) "
        "VALUES (:device, :level, :score, :reason, :features, :timestamp)"));
    query.bindValue(QStringLiteral(":device"), event.deviceId);
    query.bindValue(QStringLiteral(":level"), toString(event.level));
    query.bindValue(QStringLiteral(":score"), event.score);
    query.bindValue(QStringLiteral(":reason"), event.reason);
    query.bindValue(QStringLiteral(":features"), featureNames.join(QLatin1Char(',')));
    query.bindValue(QStringLiteral(":timestamp"), encodeTimestamp(event.timestamp));

    if (!query.exec())
    {
        return Status::fail(2, query.lastError().text());
    }
    return Status::ok();
}

Status DataRepository::insertAdvice(const QString& deviceId, const QString& summary,
                                    const QString& rootCause, const QString& action,
                                    const QString& riskLevel, const QString& source)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Status::fail(1, QStringLiteral("database is not open"));
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(
        "INSERT INTO MaintenanceAdvice "
        "(device_id, summary, root_cause, action, risk_level, source, timestamp) "
        "VALUES (:device, :summary, :cause, :action, :risk, :source, :timestamp)"));
    query.bindValue(QStringLiteral(":device"), deviceId);
    query.bindValue(QStringLiteral(":summary"), summary);
    query.bindValue(QStringLiteral(":cause"), rootCause);
    query.bindValue(QStringLiteral(":action"), action);
    query.bindValue(QStringLiteral(":risk"), riskLevel);
    query.bindValue(QStringLiteral(":source"), source);
    query.bindValue(QStringLiteral(":timestamp"), encodeTimestamp(QDateTime::currentDateTime()));

    if (!query.exec())
    {
        return Status::fail(2, query.lastError().text());
    }
    return Status::ok();
}

Status DataRepository::upsertDevice(const QString& deviceId, const QString& name,
                                    const QString& type, const QString& ip, quint16 port,
                                    DeviceState state, bool enabled)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Status::fail(1, QStringLiteral("database is not open"));
    }

    // ON CONFLICT DO UPDATE rather than INSERT OR REPLACE: REPLACE deletes the
    // old row first, which would cascade-delete every SensorData row that
    // references it.  This is a real data-loss trap and worth knowing.
    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(
        "INSERT INTO Device (id, name, type, ip, port, status, enabled, updated_at) "
        "VALUES (:id, :name, :type, :ip, :port, :status, :enabled, :updated) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  name = excluded.name, type = excluded.type, ip = excluded.ip, port = excluded.port, "
        "  status = excluded.status, enabled = excluded.enabled, updated_at = excluded.updated_at"));
    query.bindValue(QStringLiteral(":id"), deviceId);
    query.bindValue(QStringLiteral(":name"), name);
    query.bindValue(QStringLiteral(":type"), type);
    query.bindValue(QStringLiteral(":ip"), ip.isNull() ? QStringLiteral("") : ip);
    query.bindValue(QStringLiteral(":port"), static_cast<int>(port));
    query.bindValue(QStringLiteral(":status"), toString(state));
    query.bindValue(QStringLiteral(":enabled"), enabled ? 1 : 0);
    query.bindValue(QStringLiteral(":updated"), encodeTimestamp(QDateTime::currentDateTime()));

    if (!query.exec())
    {
        return Status::fail(2, query.lastError().text());
    }
    return Status::ok();
}

Status DataRepository::updateDeviceState(const QString& deviceId, DeviceState state)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Status::fail(1, QStringLiteral("database is not open"));
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(
        "UPDATE Device SET status = :status, updated_at = :updated WHERE id = :id"));
    query.bindValue(QStringLiteral(":status"), toString(state));
    query.bindValue(QStringLiteral(":updated"), encodeTimestamp(QDateTime::currentDateTime()));
    query.bindValue(QStringLiteral(":id"), deviceId);

    if (!query.exec())
    {
        return Status::fail(2, query.lastError().text());
    }
    return Status::ok();
}

Status DataRepository::acknowledgeAlarm(qint64 alarmId)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Status::fail(1, QStringLiteral("database is not open"));
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral("UPDATE Alarm SET acknowledged = 1 WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), alarmId);

    if (!query.exec())
    {
        return Status::fail(2, query.lastError().text());
    }
    return Status::ok();
}

Result<int> DataRepository::acknowledgeAllAlarms(const QString& deviceId)
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<int>::fail(1, QStringLiteral("database is not open"));
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(
        "UPDATE Alarm SET acknowledged = 1 WHERE acknowledged = 0 AND device_id = :device"));
    query.bindValue(QStringLiteral(":device"), deviceId);

    if (!query.exec())
    {
        return Result<int>::fail(2, query.lastError().text());
    }
    return Result<int>::ok(query.numRowsAffected());
}

Result<int> DataRepository::insertLogRecords(const QVector<LogRecord>& records)
{
    if (records.isEmpty())
    {
        return Result<int>::ok(0);
    }
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<int>::fail(1, QStringLiteral("database is not open"));
    }

    QSqlDatabase db = m_manager->database();
    if (!db.transaction())
    {
        return Result<int>::fail(2, QStringLiteral("cannot begin transaction"));
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO SystemLog (level, category, thread, message, file, line, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"));

    // QVariantList batch binding: Qt sends the whole list in one round trip,
    // which is measurably faster than N prepared exec() calls.
    QVariantList levels;
    QVariantList categories;
    QVariantList threads;
    QVariantList messages;
    QVariantList files;
    QVariantList lines;
    QVariantList timestamps;
    levels.reserve(records.size());
    categories.reserve(records.size());
    threads.reserve(records.size());
    messages.reserve(records.size());
    files.reserve(records.size());
    lines.reserve(records.size());
    timestamps.reserve(records.size());

    for (const LogRecord& record : records)
    {
        levels     << toString(record.level);
        categories << record.category;
        threads    << record.threadName;
        messages   << record.message;
        files      << record.file;
        lines      << record.line;
        timestamps << encodeTimestamp(record.timestamp);
    }

    query.addBindValue(levels);
    query.addBindValue(categories);
    query.addBindValue(threads);
    query.addBindValue(messages);
    query.addBindValue(files);
    query.addBindValue(lines);
    query.addBindValue(timestamps);

    if (!query.execBatch())
    {
        const QString detail = query.lastError().text();
        db.rollback();
        return Result<int>::fail(3, QStringLiteral("log batch insert failed: %1").arg(detail));
    }

    if (!db.commit())
    {
        const QString detail = db.lastError().text();
        db.rollback();
        return Result<int>::fail(4, QStringLiteral("commit failed: %1").arg(detail));
    }
    return Result<int>::ok(static_cast<int>(records.size()));
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

SensorRecord DataRepository::readSensorRecord(const QSqlQuery& query)
{
    SensorRecord record;
    record.id          = query.value(QStringLiteral("id")).toLongLong();
    record.deviceId    = query.value(QStringLiteral("device_id")).toString();
    record.timestamp   = decodeTimestamp(query.value(QStringLiteral("timestamp")).toString());
    record.temperature = query.value(QStringLiteral("temperature")).toDouble();
    record.voltage     = query.value(QStringLiteral("voltage")).toDouble();
    record.current     = query.value(QStringLiteral("current")).toDouble();
    record.vibration   = query.value(QStringLiteral("vibration")).toDouble();
    record.rpm         = query.value(QStringLiteral("rpm")).toDouble();
    record.state       = stateFromString(query.value(QStringLiteral("state")).toString());
    return record;
}

Result<QVector<SensorRecord>> DataRepository::queryHistory(const HistoryQuery& request) const
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<QVector<SensorRecord>>::fail(1, QStringLiteral("database is not open"));
    }

    // Only filters the caller actually set are added, so SQLite can use the
    // composite index on (device_id, timestamp) instead of scanning.
    QString sql = QStringLiteral(
        "SELECT id, device_id, timestamp, temperature, voltage, current, vibration, rpm, state "
        "FROM SensorData WHERE 1 = 1");
    QVariantMap binds;

    if (!request.deviceId.isEmpty())
    {
        sql += QStringLiteral(" AND device_id = :device");
        binds.insert(QStringLiteral(":device"), request.deviceId);
    }
    if (request.from.isValid())
    {
        sql += QStringLiteral(" AND timestamp >= :from");
        binds.insert(QStringLiteral(":from"), encodeTimestamp(request.from));
    }
    if (request.to.isValid())
    {
        sql += QStringLiteral(" AND timestamp <= :to");
        binds.insert(QStringLiteral(":to"), encodeTimestamp(request.to));
    }

    // Ordering is applied before LIMIT, so "last 500 points" returns the most
    // recent 500 and then re-sorts them for the chart - taking the *first* 500
    // in ascending order would show the oldest data instead of the newest.
    const bool ascending = request.ascending;
    sql += ascending ? QStringLiteral(" ORDER BY timestamp ASC") : QStringLiteral(" ORDER BY timestamp DESC");

    const int limit = request.limit > 0 ? request.limit : 1000;
    sql += QStringLiteral(" LIMIT :limit");
    binds.insert(QStringLiteral(":limit"), limit);

    QSqlQuery query(m_manager->database());
    query.prepare(sql);
    for (auto it = binds.constBegin(); it != binds.constEnd(); ++it)
    {
        query.bindValue(it.key(), it.value());
    }

    if (!query.exec())
    {
        return Result<QVector<SensorRecord>>::fail(2, query.lastError().text());
    }

    QVector<SensorRecord> records;
    while (query.next())
    {
        records.append(readSensorRecord(query));
    }

    if (!ascending)
    {
        std::reverse(records.begin(), records.end());
    }
    return Result<QVector<SensorRecord>>::ok(records);
}

Result<QVector<SensorRecord>> DataRepository::queryLatestPerDevice() const
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<QVector<SensorRecord>>::fail(1, QStringLiteral("database is not open"));
    }

    // A correlated subquery on an indexed column: "for each device, the row
    // whose timestamp equals the maximum for that device".  One round trip
    // instead of one per device.
    QSqlQuery query(m_manager->database());
    const bool ok = query.exec(QStringLiteral(R"SQL(
SELECT s.id, s.device_id, s.timestamp, s.temperature, s.voltage, s.current,
       s.vibration, s.rpm, s.state
FROM SensorData s
JOIN (
    SELECT device_id, MAX(timestamp) AS max_ts
    FROM SensorData
    GROUP BY device_id
) latest
  ON s.device_id = latest.device_id AND s.timestamp = latest.max_ts
)SQL"));

    if (!ok)
    {
        return Result<QVector<SensorRecord>>::fail(2, query.lastError().text());
    }

    QVector<SensorRecord> records;
    while (query.next())
    {
        records.append(readSensorRecord(query));
    }
    return Result<QVector<SensorRecord>>::ok(records);
}

Result<QVector<AlarmRecord>> DataRepository::queryAlarms(const QString& deviceId, int limit,
                                                          bool onlyUnacknowledged) const
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<QVector<AlarmRecord>>::fail(1, QStringLiteral("database is not open"));
    }

    QString sql = QStringLiteral(
        "SELECT id, device_id, state, message, timestamp, acknowledged FROM Alarm WHERE 1 = 1");

    if (!deviceId.isEmpty())
    {
        sql += QStringLiteral(" AND device_id = :device");
    }
    if (onlyUnacknowledged)
    {
        sql += QStringLiteral(" AND acknowledged = 0");
    }
    sql += QStringLiteral(" ORDER BY timestamp DESC LIMIT :limit");

    QSqlQuery query(m_manager->database());
    query.prepare(sql);
    if (!deviceId.isEmpty())
    {
        query.bindValue(QStringLiteral(":device"), deviceId);
    }
    query.bindValue(QStringLiteral(":limit"), limit > 0 ? limit : 200);

    if (!query.exec())
    {
        return Result<QVector<AlarmRecord>>::fail(2, query.lastError().text());
    }

    QVector<AlarmRecord> alarms;
    while (query.next())
    {
        AlarmRecord alarm;
        alarm.id          = query.value(QStringLiteral("id")).toLongLong();
        alarm.deviceId    = query.value(QStringLiteral("device_id")).toString();
        alarm.state       = stateFromString(query.value(QStringLiteral("state")).toString());
        alarm.message     = query.value(QStringLiteral("message")).toString();
        alarm.timestamp   = decodeTimestamp(query.value(QStringLiteral("timestamp")).toString());
        alarm.acknowledged = query.value(QStringLiteral("acknowledged")).toInt() != 0;
        alarms.append(alarm);
    }
    return Result<QVector<AlarmRecord>>::ok(alarms);
}

Result<QVector<FaultPrediction>> DataRepository::queryPredictions(const QString& deviceId,
                                                                   int limit) const
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<QVector<FaultPrediction>>::fail(1, QStringLiteral("database is not open"));
    }

    QString sql = QStringLiteral(
        "SELECT device_id, health_score, failure_probability, fault_type, confidence, backend, "
        "timestamp FROM Prediction WHERE 1 = 1");
    if (!deviceId.isEmpty())
    {
        sql += QStringLiteral(" AND device_id = :device");
    }
    sql += QStringLiteral(" ORDER BY timestamp DESC LIMIT :limit");

    QSqlQuery query(m_manager->database());
    query.prepare(sql);
    if (!deviceId.isEmpty())
    {
        query.bindValue(QStringLiteral(":device"), deviceId);
    }
    query.bindValue(QStringLiteral(":limit"), limit > 0 ? limit : 100);

    if (!query.exec())
    {
        return Result<QVector<FaultPrediction>>::fail(2, query.lastError().text());
    }

    QVector<FaultPrediction> predictions;
    while (query.next())
    {
        FaultPrediction prediction;
        prediction.deviceId           = query.value(QStringLiteral("device_id")).toString();
        prediction.healthScore        = query.value(QStringLiteral("health_score")).toDouble();
        prediction.failureProbability = query.value(QStringLiteral("failure_probability")).toDouble();
        prediction.faultType          = query.value(QStringLiteral("fault_type")).toString();
        prediction.confidence         = query.value(QStringLiteral("confidence")).toDouble();
        prediction.backend            = query.value(QStringLiteral("backend")).toString();
        prediction.timestamp          = decodeTimestamp(query.value(QStringLiteral("timestamp")).toString());
        predictions.append(prediction);
    }
    return Result<QVector<FaultPrediction>>::ok(predictions);
}

Result<DeviceStatistics> DataRepository::queryStatistics(const QString& deviceId,
                                                          const QDateTime& from,
                                                          const QDateTime& to) const
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<DeviceStatistics>::fail(1, QStringLiteral("database is not open"));
    }

    // Aggregation happens in SQLite: the report needs four numbers, not 50 000
    // rows, and pulling them into Qt to average would be the wrong trade at
    // every scale.
    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral(R"SQL(
SELECT COUNT(*)                                AS sample_count,
       COALESCE(MAX(temperature), 0)           AS max_temp,
       COALESCE(AVG(temperature), 0)           AS avg_temp,
       COALESCE(MAX(vibration), 0)             AS max_vib,
       COALESCE(AVG(current), 0)               AS avg_current,
       COALESCE(SUM(CASE WHEN state = 'NORMAL' THEN 1 ELSE 0 END), 0) AS normal_count
FROM SensorData
WHERE device_id = :device AND timestamp >= :from AND timestamp <= :to
)SQL"));
    query.bindValue(QStringLiteral(":device"), deviceId);
    query.bindValue(QStringLiteral(":from"), encodeTimestamp(from));
    query.bindValue(QStringLiteral(":to"), encodeTimestamp(to));

    if (!query.exec() || !query.next())
    {
        return Result<DeviceStatistics>::fail(2, query.lastError().text());
    }

    DeviceStatistics stats;
    stats.deviceId       = deviceId;
    stats.from           = from;
    stats.to             = to;
    stats.sampleCount    = query.value(QStringLiteral("sample_count")).toLongLong();
    stats.maxTemperature = query.value(QStringLiteral("max_temp")).toDouble();
    stats.avgTemperature = query.value(QStringLiteral("avg_temp")).toDouble();
    stats.maxVibration   = query.value(QStringLiteral("max_vib")).toDouble();
    stats.avgCurrent     = query.value(QStringLiteral("avg_current")).toDouble();

    const qint64 normalCount = query.value(QStringLiteral("normal_count")).toLongLong();
    stats.uptimeRatio = stats.sampleCount > 0
                            ? static_cast<double>(normalCount) / static_cast<double>(stats.sampleCount)
                            : 0.0;

    // A second, cheap query for the alarm count - keeping it out of the aggregate
    // above avoids a JOIN that would multiply the AVG() over sensor rows.
    QSqlQuery alarmQuery(m_manager->database());
    alarmQuery.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM Alarm WHERE device_id = :device "
        "AND timestamp >= :from AND timestamp <= :to"));
    alarmQuery.bindValue(QStringLiteral(":device"), deviceId);
    alarmQuery.bindValue(QStringLiteral(":from"), encodeTimestamp(from));
    alarmQuery.bindValue(QStringLiteral(":to"), encodeTimestamp(to));
    if (alarmQuery.exec() && alarmQuery.next())
    {
        stats.anomalyCount = alarmQuery.value(0).toLongLong();
    }

    return Result<DeviceStatistics>::ok(stats);
}

Result<int> DataRepository::countSamplesSince(const QDateTime& since) const
{
    if (m_manager == nullptr || !m_manager->isOpen())
    {
        return Result<int>::fail(1, QStringLiteral("database is not open"));
    }

    QSqlQuery query(m_manager->database());
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM SensorData WHERE timestamp >= :since"));
    query.bindValue(QStringLiteral(":since"), encodeTimestamp(since));

    if (!query.exec() || !query.next())
    {
        return Result<int>::fail(2, query.lastError().text());
    }
    return Result<int>::ok(query.value(0).toInt());
}

}   // namespace saim
