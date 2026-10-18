#include "database/DatabaseManager.h"

#include "core/Logger.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

namespace saim {

namespace {

/// SQLite error codes we treat as expected rather than as faults.
constexpr int kSqliteErrorGeneric = -1;

}   // namespace

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

QStringList DatabaseManager::migrations()
{
    // Index N is the SQL that moves the schema *to* version N+1.  Never edit an
    // entry that has shipped: add a new one.  Editing history is how two
    // installations end up with structurally different databases.
    return QStringList{
        // ---- v1: core tables ------------------------------------------------
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS Device (
    id          TEXT    PRIMARY KEY,
    name        TEXT    NOT NULL,
    type        TEXT    NOT NULL DEFAULT 'generic',
    ip          TEXT    NOT NULL DEFAULT '',
    port        INTEGER NOT NULL DEFAULT 0,
    status      TEXT    NOT NULL DEFAULT 'OFFLINE',
    enabled     INTEGER NOT NULL DEFAULT 1,
    created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
    updated_at  TEXT    NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS SensorData (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT    NOT NULL,
    temperature REAL    NOT NULL,
    voltage     REAL    NOT NULL,
    current     REAL    NOT NULL,
    vibration   REAL    NOT NULL,
    rpm         REAL    NOT NULL DEFAULT 0,
    state       TEXT    NOT NULL DEFAULT 'NORMAL',
    timestamp   TEXT    NOT NULL,
    FOREIGN KEY (device_id) REFERENCES Device(id) ON DELETE CASCADE
);

-- The dashboard queries "last N samples for device X", and the retention job
-- deletes by time.  One composite index serves both orderings.
CREATE INDEX IF NOT EXISTS idx_sensor_device_time
    ON SensorData(device_id, timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_sensor_time
    ON SensorData(timestamp);

CREATE TABLE IF NOT EXISTS Alarm (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id    TEXT    NOT NULL,
    state        TEXT    NOT NULL,
    message      TEXT    NOT NULL,
    timestamp    TEXT    NOT NULL,
    acknowledged INTEGER NOT NULL DEFAULT 0,
    FOREIGN KEY (device_id) REFERENCES Device(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_alarm_time ON Alarm(timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_alarm_device ON Alarm(device_id, timestamp DESC);

CREATE TABLE IF NOT EXISTS SystemLog (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    level     TEXT    NOT NULL,
    category  TEXT    NOT NULL,
    thread    TEXT    NOT NULL DEFAULT '',
    message   TEXT    NOT NULL,
    file      TEXT    NOT NULL DEFAULT '',
    line      INTEGER NOT NULL DEFAULT 0,
    timestamp TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_log_time ON SystemLog(timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_log_level ON SystemLog(level, timestamp DESC);
)SQL"),

        // ---- v2: AI results -------------------------------------------------
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS Prediction (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id           TEXT    NOT NULL,
    health_score        REAL    NOT NULL,
    failure_probability REAL    NOT NULL,
    fault_type          TEXT    NOT NULL DEFAULT '',
    confidence          REAL    NOT NULL DEFAULT 0,
    backend             TEXT    NOT NULL DEFAULT '',
    timestamp           TEXT    NOT NULL,
    FOREIGN KEY (device_id) REFERENCES Device(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_prediction_device_time
    ON Prediction(device_id, timestamp DESC);

CREATE TABLE IF NOT EXISTS Anomaly (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT    NOT NULL,
    level       TEXT    NOT NULL,
    score       REAL    NOT NULL DEFAULT 0,
    reason      TEXT    NOT NULL DEFAULT '',
    features    TEXT    NOT NULL DEFAULT '',
    timestamp   TEXT    NOT NULL,
    FOREIGN KEY (device_id) REFERENCES Device(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_anomaly_device_time
    ON Anomaly(device_id, timestamp DESC);
)SQL"),

        // ---- v3: maintenance advice produced by the assistant --------------
        QStringLiteral(R"SQL(
CREATE TABLE IF NOT EXISTS MaintenanceAdvice (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT    NOT NULL,
    summary     TEXT    NOT NULL,
    root_cause  TEXT    NOT NULL DEFAULT '',
    action      TEXT    NOT NULL DEFAULT '',
    risk_level  TEXT    NOT NULL DEFAULT 'LOW',
    source      TEXT    NOT NULL DEFAULT 'assistant',
    timestamp   TEXT    NOT NULL,
    FOREIGN KEY (device_id) REFERENCES Device(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_advice_device_time
    ON MaintenanceAdvice(device_id, timestamp DESC);
)SQL"),
    };
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DatabaseManager::DatabaseManager(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

DatabaseManager::~DatabaseManager()
{
    close();
}

Status DatabaseManager::open(const QString& filePath)
{
    if (m_db.isOpen())
    {
        return Status::ok();
    }

    m_filePath = filePath;

    // Make sure the directory exists before SQLite tries to create the file -
    // otherwise the driver reports the unhelpful "unable to open database file".
    const QFileInfo info(filePath);
    if (filePath != QLatin1String(":memory:") && !QDir().mkpath(info.absolutePath()))
    {
        setError(QStringLiteral("open"), QStringLiteral("cannot create directory %1").arg(info.absolutePath()));
        return Status::fail(1, m_lastError);
    }

    // Reuse our own connection if this object already registered one; the
    // contains() check matters because addDatabase() with a duplicate name
    // *replaces* the existing entry, invalidating every query built from it.
    if (QSqlDatabase::contains(m_connectionName))
    {
        m_db = QSqlDatabase::database(m_connectionName, /*open=*/false);
    }
    else
    {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        m_ownsConnection = true;
    }

    m_db.setDatabaseName(filePath);

    // --- pragmas ----------------------------------------------------------
    // These must be issued on the connection, not stored in the file, so they
    // are re-applied on every open.
    //
    //   journal_mode = WAL   readers do not block the writer; the UI can query
    //                        history while the persistence worker is inserting.
    //   synchronous  = NORMAL WAL makes this durable-enough and much faster
    //                        than FULL; a power cut can lose the last few ms of
    //                        telemetry but never corrupts the database.
    //   foreign_keys = ON    SQLite ships with FK enforcement *off* by default.
    //                        Without this the ON DELETE CASCADE above is inert.
    //   busy_timeout         instead of failing immediately on a locked file,
    //                        wait - this is what stops "database is locked"
    //                        becoming a support ticket.
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));

    if (!m_db.open())
    {
        setError(QStringLiteral("open"), m_db.lastError().text());
        return Status::fail(2, m_lastError);
    }

    const QStringList pragmas{
        QStringLiteral("PRAGMA journal_mode=WAL;"),
        QStringLiteral("PRAGMA synchronous=NORMAL;"),
        QStringLiteral("PRAGMA foreign_keys=ON;"),
        QStringLiteral("PRAGMA busy_timeout=5000;"),
        QStringLiteral("PRAGMA temp_store=MEMORY;"),
        QStringLiteral("PRAGMA cache_size=-16000;"),   // ~16 MB page cache
    };
    for (const QString& pragma : pragmas)
    {
        const Status status = execute(pragma, QStringLiteral("pragma"));
        if (status.isError())
        {
            // cache_size/temp_store are optimisations: a driver that refuses one
            // should not stop the application from opening its database.
            SAIM_WARN(logcat::kDatabase, status.error().message);
        }
    }

    const Status migrated = applyMigrations();
    if (migrated.isError())
    {
        close();
        return migrated;
    }

    SAIM_INFO(logcat::kDatabase,
              QStringLiteral("Database opened: %1 (schema v%2, connection '%3')")
                  .arg(filePath)
                  .arg(m_schemaVersion)
                  .arg(m_connectionName));
    return Status::ok();
}

void DatabaseManager::close()
{
    if (!m_db.isValid())
    {
        return;
    }

    if (m_db.isOpen())
    {
        // Fold the WAL back into the main file so the database is a single
        // artefact an engineer can copy off the machine with a USB stick.
        QSqlQuery query(m_db);
        query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE);"));
        m_db.close();
        SAIM_INFO(logcat::kDatabase, QStringLiteral("Database closed: %1").arg(m_filePath));
    }

    m_db = QSqlDatabase();

    if (m_ownsConnection)
    {
        // removeDatabase() must not run while a QSqlDatabase copy is alive, or
        // Qt logs "connection is still in use".  m_db was just reset above.
        QSqlDatabase::removeDatabase(m_connectionName);
        m_ownsConnection = false;
    }
}

bool DatabaseManager::isOpen() const
{
    return m_db.isValid() && m_db.isOpen();
}

QSqlDatabase DatabaseManager::database() const
{
    return m_db;
}

// ---------------------------------------------------------------------------
// Migrations
// ---------------------------------------------------------------------------

Status DatabaseManager::applyMigrations()
{
    QSqlQuery query(m_db);

    if (!query.exec(QStringLiteral("PRAGMA user_version;")) || !query.next())
    {
        setError(QStringLiteral("migrate"), QStringLiteral("cannot read user_version"));
        return Status::fail(3, m_lastError);
    }
    const int currentVersion = query.value(0).toInt();

    const QStringList all = migrations();
    if (currentVersion >= all.size())
    {
        m_schemaVersion = currentVersion;
        return Status::ok();
    }

    SAIM_INFO(logcat::kDatabase,
              QStringLiteral("Migrating database schema from v%1 to v%2")
                  .arg(currentVersion)
                  .arg(all.size()));

    for (int version = currentVersion; version < all.size(); ++version)
    {
        // One transaction per migration step: a step that fails leaves the
        // database at the previous version rather than in a half-applied state.
        if (!m_db.transaction())
        {
            setError(QStringLiteral("migrate"),
                     QStringLiteral("cannot begin transaction: %1").arg(m_db.lastError().text()));
            return Status::fail(4, m_lastError);
        }

        QSqlQuery stepQuery(m_db);
        const QString script = all.at(version);

        // SQLite's driver can only execute one statement per exec(), so the
        // script is split on the semicolons that separate top-level statements.
        const QStringList statements = script.split(QLatin1Char(';'), Qt::SkipEmptyParts);

        bool stepOk = true;
        for (const QString& statement : statements)
        {
            const QString trimmed = statement.trimmed();
            if (trimmed.isEmpty())
            {
                continue;
            }
            if (!stepQuery.exec(trimmed))
            {
                setError(QStringLiteral("migrate"),
                         QStringLiteral("step v%1 failed: %2 (SQL: %3)")
                             .arg(version + 1)
                             .arg(stepQuery.lastError().text(), trimmed.left(120)));
                stepOk = false;
                break;
            }
        }

        if (!stepOk)
        {
            m_db.rollback();
            return Status::fail(5, m_lastError);
        }

        // The version can only be written once the step succeeded - and the
        // pragma cannot be parameterised, hence the string interpolation of an
        // int we control (never of user input).
        if (!stepQuery.exec(QStringLiteral("PRAGMA user_version=%1;").arg(version + 1)))
        {
            m_db.rollback();
            setError(QStringLiteral("migrate"),
                     QStringLiteral("cannot set user_version: %1").arg(stepQuery.lastError().text()));
            return Status::fail(6, m_lastError);
        }

        if (!m_db.commit())
        {
            m_db.rollback();
            setError(QStringLiteral("migrate"),
                     QStringLiteral("commit failed: %1").arg(m_db.lastError().text()));
            return Status::fail(7, m_lastError);
        }

        SAIM_INFO(logcat::kDatabase,
                  QStringLiteral("Schema migration to v%1 applied").arg(version + 1));
    }

    // Migrations are a handful of entries, but the conversion is stated rather
    // than implied: a silent narrowing is the kind of thing that is fine until
    // somebody generates a hundred migrations in a loop.
    m_schemaVersion = static_cast<int>(all.size());
    return Status::ok();
}

Status DatabaseManager::execute(const QString& sql, const QString& context)
{
    QSqlQuery query(m_db);
    if (!query.exec(sql))
    {
        setError(context, query.lastError().text());
        return Status::fail(kSqliteErrorGeneric, m_lastError);
    }
    return Status::ok();
}

void DatabaseManager::setError(const QString& context, const QString& detail)
{
    m_lastError = QStringLiteral("[%1] %2").arg(context, detail);
    SAIM_ERROR(logcat::kDatabase, m_lastError);
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

DatabaseManager::HealthReport DatabaseManager::healthCheck() const
{
    HealthReport report;
    report.schemaVersion = m_schemaVersion;
    report.fileSizeBytes = QFileInfo(m_filePath).size();

    if (!isOpen())
    {
        report.detail = QStringLiteral("database is not open");
        return report;
    }

    QSqlQuery query(m_db);

    if (query.exec(QStringLiteral("PRAGMA integrity_check;")) && query.next())
    {
        report.ok = query.value(0).toString().compare(QLatin1String("ok"), Qt::CaseInsensitive) == 0;
        if (!report.ok)
        {
            report.detail = query.value(0).toString();
        }
    }

    if (query.exec(QStringLiteral("PRAGMA journal_mode;")) && query.next())
    {
        report.journalMode = query.value(0).toString();
    }

    const auto countRows = [&query](const char* table) -> qint64 {
        if (query.exec(QStringLiteral("SELECT COUNT(*) FROM %1;").arg(QLatin1String(table))) && query.next())
        {
            return query.value(0).toLongLong();
        }
        return -1;
    };

    report.sensorRows = countRows("SensorData");
    report.alarmRows  = countRows("Alarm");
    report.deviceRows = countRows("Device");

    if (report.detail.isEmpty())
    {
        report.detail = QStringLiteral("%1 sensor rows, %2 alarms, %3 devices")
                            .arg(report.sensorRows)
                            .arg(report.alarmRows)
                            .arg(report.deviceRows);
    }
    return report;
}

Result<qint64> DatabaseManager::purgeOlderThan(int retentionDays)
{
    if (retentionDays <= 0)
    {
        return Result<qint64>::ok(0);   // retention disabled
    }
    if (!isOpen())
    {
        return Result<qint64>::fail(1, QStringLiteral("database is not open"));
    }

    const QString cutoff = QDateTime::currentDateTime()
                               .addDays(-retentionDays)
                               .toString(Qt::ISODateWithMs);

    qint64 removed = 0;

    // Deleting in bounded batches keeps each transaction short: a single
    // "DELETE ... WHERE timestamp < ?" over months of data would hold the write
    // lock for seconds and stall every insert behind it.
    constexpr int kBatchSize = 5000;
    while (true)
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "DELETE FROM SensorData WHERE id IN ("
            "  SELECT id FROM SensorData WHERE timestamp < :cutoff LIMIT :limit"
            ")"));
        query.bindValue(QStringLiteral(":cutoff"), cutoff);
        query.bindValue(QStringLiteral(":limit"), kBatchSize);

        if (!query.exec())
        {
            return Result<qint64>::fail(2, query.lastError().text());
        }
        const qint64 affected = query.numRowsAffected();
        removed += affected;

        if (affected < kBatchSize)
        {
            break;
        }
    }

    // Reclaiming the freed pages prevents the file from growing forever.
    QSqlQuery vacuum(m_db);
    vacuum.exec(QStringLiteral("PRAGMA incremental_vacuum;"));

    SAIM_INFO(logcat::kDatabase,
              QStringLiteral("Retention purge removed %1 sensor row(s) older than %2 day(s)")
                  .arg(removed)
                  .arg(retentionDays));

    return Result<qint64>::ok(removed);
}

}   // namespace saim
