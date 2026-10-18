#pragma once

// ---------------------------------------------------------------------------
// DatabaseManager.h - SQLite connection ownership and schema migration.
//
// The single most important fact about Qt SQL
// -------------------------------------------
// QSqlDatabase is **thread affine**.  A QSqlDatabase instance, and any QSqlQuery
// derived from it, may only be used from the thread that created it.  Qt will
// emit "QSqlDatabasePrivate::database: requested database does not belong to the
// calling thread" and silently return invalid results otherwise - a failure mode
// that is easy to miss because the code path still "works" until the data turns
// out to be empty.
//
// The design here therefore gives the persistence worker its *own* named
// connection, created on that thread.  Connection names are per-thread keys, so
// two threads each call addDatabase() with a different name and neither can
// reach the other's handle.
//
// Migrations
// ----------
// The schema is versioned in SQLite's own `user_version` pragma.  On open we
// apply every migration whose version is higher than the file's, inside a
// transaction, so an interrupted upgrade rolls back rather than leaving a
// half-migrated database.  Adding a column later is then a new entry in
// `migrations()` and nothing else.
// ---------------------------------------------------------------------------

#include "common/Result.h"
#include "common/Types.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <memory>

namespace saim {

class ConfigManager;

class DatabaseManager
{
public:
    /// `connectionName` must be unique per thread - see the class comment.
    explicit DatabaseManager(QString connectionName);
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager&)            = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    /// Opens (creating if needed), enables WAL, applies migrations.
    /// Must be called from the thread that will use the database.
    [[nodiscard]] Status open(const QString& filePath);
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString filePath() const { return m_filePath; }
    [[nodiscard]] QString connectionName() const { return m_connectionName; }
    [[nodiscard]] int schemaVersion() const noexcept { return m_schemaVersion; }

    /// The handle for this thread.  Callers must not store it beyond the
    /// lifetime of this manager, and must not hand it to another thread.
    [[nodiscard]] QSqlDatabase database() const;

    /// Runs `PRAGMA integrity_check` and `PRAGMA foreign_key_check`, plus a
    /// row-count sanity pass.  Exposed because "the plant database is fine" is a
    /// claim that should be checkable from the UI, not an assumption.
    struct HealthReport
    {
        bool   ok = false;
        int    schemaVersion = 0;
        qint64 sensorRows = 0;
        qint64 alarmRows  = 0;
        qint64 deviceRows = 0;
        qint64 fileSizeBytes = 0;
        QString journalMode;
        QString detail;
    };
    [[nodiscard]] HealthReport healthCheck() const;

    /// Deletes SensorData rows older than the retention window and compacts.
    /// Returns the number of rows removed.
    [[nodiscard]] Result<qint64> purgeOlderThan(int retentionDays);

    /// Last error text from the driver, for the UI's diagnostics page.
    [[nodiscard]] QString lastError() const { return m_lastError; }

    /// The schema, as SQL - exposed so that the migration list is inspectable.
    [[nodiscard]] static QStringList migrations();

private:
    [[nodiscard]] Status applyMigrations();
    [[nodiscard]] Status execute(const QString& sql, const QString& context);
    void setError(const QString& context, const QString& detail);

    QString       m_connectionName;
    QString       m_filePath;
    QString       m_lastError;
    int           m_schemaVersion = 0;
    bool          m_ownsConnection = false;
    mutable QSqlDatabase m_db;
};

}   // namespace saim
