#include "thread/PersistenceWorker.h"

#include "core/ConfigManager.h"
#include "core/Logger.h"

#include <QMetaObject>
#include <QSqlError>
#include <algorithm>

namespace saim {

namespace {
/// Hard ceiling on the pending buffer.  `maxBatchSize` governs the *flush*
/// cadence; this governs *memory*.  If the database stalls (a locked file, a
/// full disk) the buffer would otherwise grow until the process is killed, which
/// loses everything.  Shedding the oldest samples and counting them is the
/// behaviour that keeps the monitoring application alive to report the problem.
constexpr int kMaxBufferedSamples = 20000;
}   // namespace

PersistenceWorker::PersistenceWorker(ConfigManager* config, QObject* parent)
    : ThreadedWorker(QStringLiteral("PersistenceThread"), parent)
    , m_config(config)
    , m_inbox(config ? config->getInt(QStringLiteral("pipeline.persistenceCapacity"), 8192) : 8192,
              SampleInbox::Policy::WaitThenReject)
{
    m_flushTimer.setParent(this);
    m_inboxTimer.setParent(this);
    m_inboxTimer.setInterval(10);
    connect(&m_inboxTimer, &QTimer::timeout, this, &PersistenceWorker::drainInbox);
    connect(&m_inboxTimer, &QTimer::timeout, this, &PersistenceWorker::drainRecords);
}

PersistenceWorker::~PersistenceWorker() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle - both of these run on the worker thread
// ---------------------------------------------------------------------------

void PersistenceWorker::onStart()
{
    m_uptime.start();
    m_stopping = false;
    m_retryAfterMs = 0;
    m_inbox.reopen();
    m_records.reopen();
    openDatabase();
    m_inboxTimer.start();

    m_flushTimer.setInterval(m_config != nullptr ? m_config->database().flushIntervalMs : 1000);
    connect(&m_flushTimer, &QTimer::timeout, this, &PersistenceWorker::flushBuffer,
            Qt::UniqueConnection);
    m_flushTimer.start();

    SAIM_INFO(logcat::kDatabase,
              QStringLiteral("Persistence worker started (flush interval %1 ms)")
                  .arg(m_flushTimer.interval()));
}

void PersistenceWorker::onStop()
{
    m_flushTimer.stop();
    m_inboxTimer.stop();
    m_inbox.close();
    m_records.close();
    // Drain into the bounded batch buffer, then try ONE final transaction.
    // A locked database must not impose its busy timeout once per inbox batch.
    m_stopping = true;
    while (m_inbox.statistics().depth > 0) drainInbox();

    // Final flush.  Without it, up to one flush interval of samples - the most
    // recent data, which is the data an operator is most likely to want after a
    // restart - is thrown away by a clean shutdown.
    flushBuffer();
    while (m_records.statistics().depth > 0) drainRecords();

    if (m_droppedSamples > 0)
    {
        SAIM_WARN(logcat::kDatabase,
                  QStringLiteral("Persistence: %1 sample(s) were shed under back-pressure")
                      .arg(m_droppedSamples.load()));
    }

    // Closing here, on this thread, is required: QSqlDatabase must be released
    // by the thread that opened it.
    if (m_repository)
    {
        m_repository.reset();
    }
    if (m_database)
    {
        m_database->close();
        m_database.reset();
    }
    m_open = false;

    SAIM_INFO(logcat::kDatabase,
              QStringLiteral("Persistence worker stopped after %1 row(s) in %2 batch(es)")
                  .arg(m_rowsWritten.load())
                  .arg(m_batchesWritten.load()));
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

void PersistenceWorker::openDatabase()
{
    const ConfigManager::DatabaseConfig& config =
        m_config != nullptr ? m_config->database() : ConfigManager::DatabaseConfig{};

    // A per-thread connection name.  Qt keys connections by name globally, so if
    // the UI thread ever opens the same file for a read it must use a different
    // name - which is why the name includes the thread.
    const QString connectionName =
        QStringLiteral("saim_persistence_%1")
            .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);

    m_database = std::make_unique<DatabaseManager>(connectionName);

    const QString path = m_config != nullptr
                             ? m_config->resolveDataPath(config.filePath)
                             : config.filePath;

    const Status status = m_database->open(path);
    if (!status.isOk())
    {
        m_open = false;
        SAIM_ERROR(logcat::kDatabase,
                   QStringLiteral("Database open failed: %1").arg(status.error().message));
        // Report rather than abort: the rest of the application keeps running,
        // samples are shed, and the UI shows the failure.  A monitoring tool that
        // exits because its own history file is locked is not much of a monitor.
        emit databaseFailed(status.error().message);
        return;
    }

    m_repository = std::make_unique<DataRepository>(m_database.get());
    m_open = true;
    setDevices(m_devices);
    m_health = m_database->healthCheck();

    SAIM_INFO(logcat::kDatabase,
              QStringLiteral("Database ready: %1 (schema v%2, %3 device row(s))")
                  .arg(path)
                  .arg(m_health.schemaVersion)
                  .arg(m_health.deviceRows));

    emit databaseOpened(m_health);
}

void PersistenceWorker::emitError(const QString& context, const QString& detail)
{
    const QString message = QStringLiteral("%1: %2").arg(context, detail);
    SAIM_ERROR(logcat::kDatabase, message);
    emit persistenceError(message);
}

void PersistenceWorker::setDevices(const QVector<DeviceConfiguration>& devices)
{
    QHash<QString, DeviceLimits> next;
    for (const auto& device : devices)
    {
        if (m_open && m_repository)
        {
            const auto result = m_repository->upsertDevice(
                device.id, device.name, device.type, device.ip, device.port,
                DeviceState::Offline, device.enabled);
            if (result.isError())
            {
                emitError(QStringLiteral("Registering %1").arg(device.id), result.error().message);
                continue;
            }
        }
        if (device.enabled) { next.insert(device.id, device.limits); }
    }
    if (m_open && m_repository)
    {
        for (const auto& previous : m_devices)
        {
            const bool stillPresent = std::any_of(devices.cbegin(), devices.cend(),
                [&previous](const DeviceConfiguration& device) { return device.id == previous.id; });
            if (!stillPresent)
            {
                const auto result = m_repository->upsertDevice(previous.id, previous.name,
                    previous.type, previous.ip, previous.port, DeviceState::Offline, false);
                if (result.isError()) { emitError(QStringLiteral("Disabling removed device"), result.error().message); }
            }
        }
    }
    // Retain removed device rows for foreign-key history, but stop accepting
    // late packets from devices no longer in the active configuration.
    m_devices = devices;
    m_deviceLimits = std::move(next);
}

void PersistenceWorker::applyConfiguration()
{
    if (m_config != nullptr)
    {
        m_flushTimer.setInterval(qMax(1, m_config->database().flushIntervalMs));
    }
}

// ---------------------------------------------------------------------------
// Write path
// ---------------------------------------------------------------------------

void PersistenceWorker::drainInbox()
{
    for (const auto& sample : m_inbox.take(256)) storeSample(sample);
}

void PersistenceWorker::drainRecords()
{
    for (const auto& record : m_records.take(16))
    {
        std::visit([this](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AlarmRecord>) storeAlarm(value);
            else if constexpr (std::is_same_v<T, FaultPrediction>) storePrediction(value);
            else storeAnomaly(value);
        }, record);
    }
}

void PersistenceWorker::storeSample(const SensorSample& sample)
{
    if (!m_open || !sample.isValid() || !m_deviceLimits.contains(sample.deviceId))
    {
        ++m_droppedSamples;
        return;
    }

    SensorRecord record;
    record.deviceId    = sample.deviceId;
    record.timestamp   = sample.timestamp;
    record.temperature = sample.temperature;
    record.voltage     = sample.voltage;
    record.current     = sample.current;
    record.vibration   = sample.vibration;
    record.rpm         = sample.rpm;
    // Historical state is the physical envelope at ingestion time. Later AI
    // verdicts are stored separately and must not rewrite this measurement.
    record.state       = m_deviceLimits.value(sample.deviceId).evaluate(sample);

    m_sampleBuffer.append(record);

    if (m_sampleBuffer.size() >= kMaxBufferedSamples)
    {
        const int shed = static_cast<int>(m_sampleBuffer.size()) / 4;
        m_sampleBuffer.remove(0, shed);
        m_droppedSamples += static_cast<quint64>(shed);
    }

    const int batchSize = m_config != nullptr ? m_config->database().batchSize : 200;
    if (!m_stopping && m_sampleBuffer.size() >= batchSize)
    {
        flushBuffer();
    }
}

void PersistenceWorker::storeAlarm(const AlarmRecord& alarm)
{
    // An alarm is never buffered: it is rare, it is the record an investigator
    // will look for first, and losing it to a crash would be the worst possible
    // thing to lose.  Flush the pending samples first so the alarm's timestamp
    // ordering against them is preserved in the file.
    flushBuffer();

    if (!m_open || !m_repository || m_uptime.elapsed() < m_retryAfterMs)
    {
        ++m_failedRecords;
        return;
    }

    const Status status = m_repository->insertAlarm(alarm);
    if (!status.isOk())
    {
        ++m_failedRecords;
        m_retryAfterMs = m_uptime.elapsed() + 5000;
        emitError(QStringLiteral("Storing alarm for %1").arg(alarm.deviceId),
                  status.error().message);
    }
}

void PersistenceWorker::storePrediction(const FaultPrediction& prediction)
{
    if (!m_open || !m_repository || m_uptime.elapsed() < m_retryAfterMs)
    {
        ++m_failedRecords;
        return;
    }
    const Status status = m_repository->insertPrediction(prediction);
    if (!status.isOk())
    {
        ++m_failedRecords;
        m_retryAfterMs = m_uptime.elapsed() + 5000;
        emitError(QStringLiteral("Storing prediction for %1").arg(prediction.deviceId),
                  status.error().message);
    }
}

void PersistenceWorker::storeAnomaly(const AnomalyEvent& event)
{
    if (event.level == AnomalyLevel::None) return;
    if (!m_open || !m_repository || m_uptime.elapsed() < m_retryAfterMs)
    {
        ++m_failedRecords;
        return;
    }
    const Status status = m_repository->insertAnomaly(event);
    if (!status.isOk())
    {
        ++m_failedRecords;
        m_retryAfterMs = m_uptime.elapsed() + 5000;
        emitError(QStringLiteral("Storing anomaly for %1").arg(event.deviceId),
                  status.error().message);
    }
}

void PersistenceWorker::storeAdvice(const QString& deviceId, const QString& summary,
                                    const QString& rootCause, const QString& action,
                                    const QString& riskLevel, const QString& source)
{
    if (!m_open || !m_repository)
    {
        return;
    }
    const Status status =
        m_repository->insertAdvice(deviceId, summary, rootCause, action, riskLevel, source);
    if (!status.isOk())
    {
        emitError(QStringLiteral("Storing advice for %1").arg(deviceId), status.error().message);
    }
}

void PersistenceWorker::storeLogRecords(const QVector<LogRecord>& records)
{
    if (!m_open || !m_repository || records.isEmpty())
    {
        return;
    }
    const Result<int> result = m_repository->insertLogRecords(records);
    if (!result.isOk())
    {
        // Deliberately *not* routed through emitError: that logs, and logging on
        // a failed log-write is the one recursion this application must not have.
        emit persistenceError(
            QStringLiteral("Storing %1 log record(s): %2")
                .arg(records.size())
                .arg(result.error().message));
    }
}

void PersistenceWorker::upsertDevice(const QString& deviceId, const QString& name,
                                     const QString& type, const QString& ip, quint16 port,
                                     int state, bool enabled)
{
    if (!m_open || !m_repository)
    {
        return;
    }
    const Status status = m_repository->upsertDevice(
        deviceId, name, type, ip, port, static_cast<DeviceState>(state), enabled);
    if (!status.isOk())
    {
        emitError(QStringLiteral("Upserting device %1").arg(deviceId), status.error().message);
    }
}

void PersistenceWorker::updateDeviceState(const QString& deviceId, int state)
{
    if (!m_open || !m_repository)
    {
        return;
    }
    const Status status =
        m_repository->updateDeviceState(deviceId, static_cast<DeviceState>(state));
    if (!status.isOk())
    {
        emitError(QStringLiteral("Updating state of %1").arg(deviceId), status.error().message);
    }
}

// ---------------------------------------------------------------------------
// The flush itself
// ---------------------------------------------------------------------------

void PersistenceWorker::flushBuffer()
{
    if (m_sampleBuffer.isEmpty() || !m_open || !m_repository)
    {
        return;
    }
    if (!m_stopping && m_uptime.elapsed() < m_retryAfterMs) return;

    // Swap rather than copy-and-clear: a sample queued while the insert runs
    // must land in the fresh vector, not be appended to the one being written.
    QVector<SensorRecord> batch;
    batch.swap(m_sampleBuffer);

    const Result<int> result = m_repository->insertSamples(batch);
    if (!result.isOk())
    {
        // The failed transaction is discarded; account for every lost row.
        m_droppedSamples += static_cast<quint64>(batch.size());
        m_retryAfterMs = m_uptime.elapsed() + 5000;
        emitError(QStringLiteral("Flushing %1 sample(s)").arg(batch.size()),
                  result.error().message);
        return;
    }

    m_rowsWritten += static_cast<quint64>(result.value());
    ++m_batchesWritten;
    emit batchCommitted(result.value());
}

void PersistenceWorker::requestFlush()
{
    // Queued, so a caller on the UI thread returns immediately and the flush
    // happens on the worker thread in its own time.
    QMetaObject::invokeMethod(this, &PersistenceWorker::flushBuffer, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Read path - every one is a queued round trip tagged with a request id
// ---------------------------------------------------------------------------

void PersistenceWorker::requestHistory(quint64 requestId, const HistoryQuery& query)
{
    if (!m_open || !m_repository)
    {
        emit historyReady(requestId, {});
        return;
    }
    const Result<QVector<SensorRecord>> result = m_repository->queryHistory(query);
    if (result.isOk())
    {
        emit historyReady(requestId, result.value());
    }
    else
    {
        emitError(QStringLiteral("History query"), result.error().message);
        emit historyReady(requestId, {});
    }
}

void PersistenceWorker::requestLatestSamples(quint64 requestId)
{
    if (!m_open || !m_repository)
    {
        emit latestSamplesReady(requestId, {});
        return;
    }
    const Result<QVector<SensorRecord>> result = m_repository->queryLatestPerDevice();
    if (result.isOk())
    {
        emit latestSamplesReady(requestId, result.value());
    }
    else
    {
        emitError(QStringLiteral("Latest-sample query"), result.error().message);
        emit latestSamplesReady(requestId, {});
    }
}

void PersistenceWorker::requestAlarms(quint64 requestId, const QString& deviceId, int limit,
                                      bool onlyUnacknowledged)
{
    if (!m_open || !m_repository)
    {
        emit alarmsReady(requestId, {});
        return;
    }
    const Result<QVector<AlarmRecord>> result =
        m_repository->queryAlarms(deviceId, limit, onlyUnacknowledged);
    if (result.isOk())
    {
        emit alarmsReady(requestId, result.value());
    }
    else
    {
        emitError(QStringLiteral("Alarm query"), result.error().message);
        emit alarmsReady(requestId, {});
    }
}

void PersistenceWorker::requestStatistics(quint64 requestId, const QString& deviceId,
                                          const QDateTime& from, const QDateTime& to)
{
    if (!m_open || !m_repository)
    {
        emit statisticsReady(requestId, {});
        return;
    }
    const Result<DeviceStatistics> result = m_repository->queryStatistics(deviceId, from, to);
    if (result.isOk())
    {
        emit statisticsReady(requestId, result.value());
    }
    else
    {
        emitError(QStringLiteral("Statistics query"), result.error().message);
        emit statisticsReady(requestId, {});
    }
}

void PersistenceWorker::requestHealthCheck(quint64 requestId)
{
    if (!m_database)
    {
        emit healthCheckReady(requestId, {});
        return;
    }
    m_health = m_database->healthCheck();
    emit healthCheckReady(requestId, m_health);
}

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

void PersistenceWorker::runRetention()
{
    if (!m_open || !m_database)
    {
        return;
    }

    const int days = m_config != nullptr ? m_config->database().retentionDays : 30;
    if (days <= 0)
    {
        SAIM_INFO(logcat::kDatabase, QStringLiteral("Retention disabled (retentionDays = 0)"));
        return;
    }

    // Flush first: the purge deletes by timestamp, and rows still sitting in this
    // worker's buffer have not been written yet - deleting around them would
    // leave the file with holes in the recent history.
    flushBuffer();

    const Result<qint64> purged = m_database->purgeOlderThan(days);
    if (purged.isOk())
    {
        SAIM_INFO(logcat::kDatabase,
                  QStringLiteral("Retention: removed %1 row(s) older than %2 day(s)")
                      .arg(purged.value())
                      .arg(days));
    }
    else
    {
        emitError(QStringLiteral("Retention purge"), purged.error().message);
    }
}

}   // namespace saim
