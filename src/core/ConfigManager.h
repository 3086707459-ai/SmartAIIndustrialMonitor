#pragma once

// ---------------------------------------------------------------------------
// ConfigManager.h - layered, hot-reloadable application configuration.
//
// Precedence, lowest to highest:
//
//     1. compiled-in defaults (in this file)
//     2. config/config.json next to the executable  (or -DSAIM_CONFIG_DIR)
//     3. the user's per-machine overlay, QStandardPaths::AppConfigLocation
//     4. environment variables prefixed SAIM_
//
// Rationale: a plant deployment ships one read-only config.json with the line's
// device list and thresholds; an engineer overriding a threshold on the shop
// floor should not have to edit the shipped file, hence the per-user overlay.
//
// Thread safety: reads are lock-guarded and return copies, so a worker thread
// can sample a threshold without holding a lock across its work.  Writes are
// rare (UI settings dialog) and re-emit changed() for interested parties.
// ---------------------------------------------------------------------------

// LogLevel and logLevelFromString() live in Logger.h; the logging configuration
// section below is stored as text, so the parser needs the enum.  This is the
// one place where an include across two core-layer headers is the right call:
// duplicating the enum would let the two disagree about what "warning" means.
#include "core/Logger.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace saim {

class ConfigManager final : public QObject
{
    Q_OBJECT
public:
    /// Bacnet-ish "module.section.key" lookup path, e.g. "network.server.port".
    explicit ConfigManager(QObject* parent = nullptr);
    ~ConfigManager() override;

    // -- lifecycle ----------------------------------------------------------

    /// Loads defaults, then the files listed in `searchPaths` (later wins).
    /// Missing files are not an error; a malformed one is logged and skipped so
    /// that a typo in an optional overlay cannot stop the plant from starting.
    void load(const QStringList& searchPaths = defaultSearchPaths());

    /// Persists the current user-overridable subset to the user overlay path.
    bool saveUserOverlay() const;

    [[nodiscard]] static QStringList defaultSearchPaths();
    [[nodiscard]] static QString userOverlayPath();

    // -- typed access -------------------------------------------------------

    [[nodiscard]] bool    getBool  (const QString& path, bool    fallback = false) const;
    [[nodiscard]] int     getInt   (const QString& path, int     fallback = 0)     const;
    [[nodiscard]] qint64  getInt64 (const QString& path, qint64  fallback = 0)     const;
    [[nodiscard]] double  getDouble(const QString& path, double  fallback = 0.0)   const;
    [[nodiscard]] QString getString(const QString& path, const QString& fallback = {}) const;
    [[nodiscard]] QStringList getStringList(const QString& path) const;
    [[nodiscard]] QJsonObject getObject(const QString& path) const;
    [[nodiscard]] QJsonArray  getArray (const QString& path) const;

    void set(const QString& path, const QJsonValue& value);

    [[nodiscard]] bool contains(const QString& path) const;

    /// Full effective tree - useful for a diagnostics dump.
    [[nodiscard]] QJsonObject snapshot() const;

    // -- strongly-typed view of the sections this application actually uses ---
    //
    // Parsed once at load() and returned by value.  Worker threads read these
    // structs instead of doing string lookups on the hot path.
    // -----------------------------------------------------------------------

    struct NetworkConfig
    {
        QString listenAddress = QStringLiteral("0.0.0.0");
        quint16 listenPort    = 9100;
        QString remoteHost    = QStringLiteral("127.0.0.1");
        quint16 remotePort    = 9100;
        int     reconnectBaseMs = 1000;
        int     reconnectMaxMs  = 30000;
        int     heartbeatMs     = 5000;
        bool    autoConnect     = true;
    };

    struct DatabaseConfig
    {
        QString filePath = QStringLiteral("data/smartai.db");
        int     connectionPoolSize = 1;   ///< Qt SQL drivers are not thread safe
        int     retentionDays      = 30;  ///< older SensorData rows are purged
        int     flushIntervalMs    = 1000;
        int     batchSize          = 200;
    };

    struct AiConfig
    {
        QString modelDirectory  = QStringLiteral("models");
        QString faultModelFile  = QStringLiteral("fault_predictor.onnx");
        QString visionModelFile = QStringLiteral("defect_yolo.onnx");
        bool    enableOnnx      = true;
        bool    enableVision    = true;
        int     windowSize      = 60;    ///< samples fed to the predictor
        int     inferenceHz     = 2;     ///< predictor cadence per device
        int     threadPoolSize  = 2;
        double  anomalyThreshold = 0.65;
    };

    struct UiConfig
    {
        QString theme          = QStringLiteral("industrial-dark");
        int     chartPoints    = 120;
        int     uiRefreshHz    = 10;     ///< coalescing rate for the dashboard
        bool    showGrid       = true;
        QString language       = QStringLiteral("zh_CN");
    };

    struct LoggingConfig
    {
        QString level      = QStringLiteral("debug");
        QString directory  = QStringLiteral("logs");
        qint64  maxFileBytes = 8LL * 1024 * 1024;
        int     keepFiles  = 5;
        bool    console    = true;
    };

    struct SimulationConfig
    {
        bool enabled          = true;
        int  deviceCount      = 6;
        int  sampleIntervalMs = 500;
        bool injectAnomalies  = true;
        int  anomalyRatePercent = 3;
        /// Populated from config so the simulated line matches the real one.
        QStringList deviceIds;
    };

    struct AlarmConfig
    {
        int  offlineTimeoutMs   = 15000;
        int  repeatSuppressionMs = 30000;  ///< do not re-alarm the same device
        bool persistAlarms      = true;
        bool soundEnabled       = false;
        int  historyLimit       = 500;
    };

    [[nodiscard]] NetworkConfig network() const { QMutexLocker lock(&m_mutex); return m_network; }
    [[nodiscard]] DatabaseConfig database() const { QMutexLocker lock(&m_mutex); return m_database; }
    [[nodiscard]] AiConfig ai() const { QMutexLocker lock(&m_mutex); return m_ai; }
    [[nodiscard]] UiConfig ui() const { QMutexLocker lock(&m_mutex); return m_ui; }
    [[nodiscard]] LoggingConfig logging() const { QMutexLocker lock(&m_mutex); return m_logging; }
    [[nodiscard]] SimulationConfig simulation() const { QMutexLocker lock(&m_mutex); return m_simulation; }
    [[nodiscard]] AlarmConfig alarms() const { QMutexLocker lock(&m_mutex); return m_alarms; }

    /// Resolves a possibly-relative path against the application data root.
    /// Nothing in the code base is allowed to hard-code a filesystem path; it
    /// goes through here so a deployment can relocate everything with one key.
    [[nodiscard]] QString resolvePath(const QString& maybeRelative) const;
    /// Output paths never search ancestor directories. This prevents a fresh
    /// database/log/report from silently escaping the configured data root.
    [[nodiscard]] QString resolveDataPath(const QString& maybeRelative) const;

    [[nodiscard]] QString dataRoot() const { QMutexLocker lock(&m_mutex); return m_dataRoot; }
    void setDataRoot(const QString& root);

signals:
    /// Emitted once per load(); the UI rebuilds its device list from this.
    void reloaded();
    void valueChanged(const QString& path, const QJsonValue& value);

private:
    void applyDefaults();
    void mergeFile(const QString& filePath);
    void applyEnvironmentOverrides();
    void rebuildTypedViews();
    void setValueAtPath(QJsonObject& root, const QString& path, const QJsonValue& value) const;
    [[nodiscard]] QJsonValue valueAtPath(const QString& path) const;

    mutable QMutex m_mutex;
    QJsonObject    m_root;       ///< the merged tree
    QJsonObject    m_overrides;  ///< only what the user changed (for saveUserOverlay)
    QString        m_dataRoot;

    NetworkConfig    m_network;
    DatabaseConfig   m_database;
    AiConfig         m_ai;
    UiConfig         m_ui;
    LoggingConfig    m_logging;
    SimulationConfig m_simulation;
    AlarmConfig      m_alarms;
};

/// Parses "trace"/"debug"/"info"/"warning"/"error"/"critical" (case-insensitive).
/// Unknown text yields LogLevel::Info rather than throwing, because the value
/// comes from a config file an operator may have hand-edited.
[[nodiscard]] LogLevel logLevelFromString(const QString& text);

}   // namespace saim
