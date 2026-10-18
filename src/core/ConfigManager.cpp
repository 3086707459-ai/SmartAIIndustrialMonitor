#include "core/ConfigManager.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace saim {

namespace {

/// Splits "a.b.c" into {"a","b","c"}.
QStringList splitPath(const QString& path)
{
    return path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
}

/// Deep merge: objects merge recursively, everything else is replaced wholesale.
/// Arrays replace rather than concatenate - a config overlay that half-appends
/// to a device list is a bug factory.
QJsonObject deepMerge(const QJsonObject& base, const QJsonObject& overlay)
{
    QJsonObject result = base;
    for (auto it = overlay.constBegin(); it != overlay.constEnd(); ++it)
    {
        const QJsonValue overlayValue = it.value();
        const QJsonValue baseValue = result.value(it.key());

        if (overlayValue.isObject() && baseValue.isObject())
        {
            result.insert(it.key(), deepMerge(baseValue.toObject(), overlayValue.toObject()));
        }
        else
        {
            result.insert(it.key(), overlayValue);
        }
    }
    return result;
}

LogLevel parseLogLevel(const QString& text)
{
    const QString lower = text.trimmed().toLower();
    if (lower == QLatin1String("trace"))    return LogLevel::Trace;
    if (lower == QLatin1String("debug"))    return LogLevel::Debug;
    if (lower == QLatin1String("info"))     return LogLevel::Info;
    if (lower == QLatin1String("warning"))  return LogLevel::Warning;
    if (lower == QLatin1String("warn"))     return LogLevel::Warning;
    if (lower == QLatin1String("error"))    return LogLevel::Error;
    if (lower == QLatin1String("critical")) return LogLevel::Critical;
    return LogLevel::Info;
}

}   // namespace

LogLevel logLevelFromString(const QString& text)
{
    return parseLogLevel(text);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ConfigManager::ConfigManager(QObject* parent)
    : QObject(parent)
{
    m_dataRoot = QCoreApplication::applicationDirPath();
}

ConfigManager::~ConfigManager() = default;

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

QStringList ConfigManager::defaultSearchPaths()
{
    QStringList paths;

    // 1. Explicit override from the build / launcher: -DSAIM_CONFIG_DIR=...
    const QString fromEnv = qEnvironmentVariable("SAIM_CONFIG_DIR");
    if (!fromEnv.isEmpty())
    {
        paths << QDir(fromEnv).filePath(QStringLiteral("config.json"));
    }

    // 2. Alongside the executable - this is what a deployment ships.
    paths << QDir(QCoreApplication::applicationDirPath())
                 .filePath(QStringLiteral("config/config.json"));

    // 3. Walk up a few levels so the app also finds the config when run from
    //    build/bin/ inside the source tree during development.
    QDir walker(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 4; ++depth)
    {
        if (!walker.cdUp())
        {
            break;
        }
        const QString candidate = walker.filePath(QStringLiteral("config/config.json"));
        if (QFile::exists(candidate))
        {
            paths << candidate;
            break;
        }
    }

    // 4. Per-user overlay - highest precedence.
    paths << userOverlayPath();

    return paths;
}

QString ConfigManager::userOverlayPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QStringLiteral("config.override.json"));
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void ConfigManager::load(const QStringList& searchPaths)
{
    {
        QMutexLocker locker(&m_mutex);
        m_root = QJsonObject{};
        m_overrides = QJsonObject{};
    }

    applyDefaults();

    for (const QString& path : searchPaths)
    {
        mergeFile(path);
    }

    applyEnvironmentOverrides();
    rebuildTypedViews();

    SAIM_INFO(logcat::kConfig,
              QStringLiteral("Configuration loaded from %1 source(s); data root = %2")
                  .arg(searchPaths.size())
                  .arg(m_dataRoot));

    emit reloaded();
}

void ConfigManager::mergeFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.exists())
    {
        SAIM_DEBUG(logcat::kConfig, QStringLiteral("Config file not present, skipping: %1").arg(filePath));
        return;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        SAIM_WARN(logcat::kConfig, QStringLiteral("Config file unreadable, skipping: %1").arg(filePath));
        return;
    }

    const QByteArray payload = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        // A broken overlay must never prevent start-up - log loudly and move on.
        SAIM_ERROR(logcat::kConfig,
                   QStringLiteral("Malformed JSON in %1 at offset %2 (%3) - ignored")
                       .arg(filePath)
                       .arg(parseError.offset)
                       .arg(parseError.errorString()));
        return;
    }

    QMutexLocker locker(&m_mutex);
    m_root = deepMerge(m_root, document.object());

    // The user overlay is the only file we ever write back.
    if (filePath == userOverlayPath())
    {
        m_overrides = deepMerge(m_overrides, document.object());
    }
}

void ConfigManager::applyEnvironmentOverrides()
{
    // SAIM_NETWORK__SERVER__PORT=9200 -> network.server.port
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QStringList keys = env.keys();
    const QString prefix = QStringLiteral("SAIM_");

    QMutexLocker locker(&m_mutex);
    for (const QString& key : keys)
    {
        if (!key.startsWith(prefix) || key == QLatin1String("SAIM_CONFIG_DIR"))
        {
            continue;
        }
        // Environment names are case-insensitive on Windows; resolve each
        // segment against the JSON schema to preserve camelCase field names.
        const QStringList segments = key.mid(prefix.size()).split(QStringLiteral("__"));
        QStringList resolved;
        QJsonObject object = m_root;
        for (const QString& segment : segments)
        {
            QString canonical = segment.toLower();
            for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            {
                if (it.key().compare(segment, Qt::CaseInsensitive) == 0)
                {
                    canonical = it.key();
                    break;
                }
            }
            resolved.append(canonical);
            object = object.value(canonical).toObject();
        }
        const QString path = resolved.join(QLatin1Char('.'));
        const QString raw = env.value(key);

        // Infer the JSON type so that getInt() keeps working after an override.
        QJsonValue value;
        bool ok = false;
        const int asInt = raw.toInt(&ok);
        if (ok)
        {
            value = asInt;
        }
        else
        {
            const double asDouble = raw.toDouble(&ok);
            if (ok)
            {
                value = asDouble;
            }
            else if (raw.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
            {
                value = true;
            }
            else if (raw.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0)
            {
                value = false;
            }
            else
            {
                value = raw;
            }
        }
        setValueAtPath(m_root, path, value);
    }
}

void ConfigManager::setValueAtPath(QJsonObject& root, const QString& path, const QJsonValue& value) const
{
    const QStringList parts = splitPath(path);
    if (parts.isEmpty())
    {
        return;
    }

    // Descend, creating intermediate objects as needed, then write at the leaf.
    QJsonObject* cursor = &root;
    for (int i = 0; i < parts.size() - 1; ++i)
    {
        QJsonValue child = cursor->value(parts.at(i));
        if (!child.isObject())
        {
            child = QJsonObject{};
            cursor->insert(parts.at(i), child);
        }
        // QJsonObject is copy-on-write and stores values by value, so we rebuild
        // the chain on the way back up instead of holding a pointer into it.
        QJsonObject nested = child.toObject();
        setValueAtPath(nested, parts.mid(i + 1).join(QLatin1Char('.')), value);
        cursor->insert(parts.at(i), nested);
        return;
    }
    cursor->insert(parts.last(), value);
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

void ConfigManager::applyDefaults()
{
    static const char* kDefaults = R"JSON(
{
  "application": {
    "name": "SmartAI Industrial Monitor",
    "organization": "SAIM",
    "version": "1.0.0"
  },
  "logging": {
    "level": "debug",
    "directory": "logs",
    "maxFileBytes": 8388608,
    "keepFiles": 5,
    "console": true
  },
  "network": {
    "server": {
      "listenAddress": "0.0.0.0",
      "listenPort": 9100
    },
    "client": {
      "remoteHost": "127.0.0.1",
      "remotePort": 9100,
      "autoConnect": true,
      "heartbeatMs": 5000
    },
    "reconnect": {
      "baseMs": 1000,
      "maxMs": 30000
    }
  },
  "database": {
    "filePath": "data/smartai.db",
    "retentionDays": 30,
    "flushIntervalMs": 1000,
    "batchSize": 200
  },
  "ai": {
    "modelDirectory": "models",
    "faultModelFile": "fault_predictor.onnx",
    "visionModelFile": "defect_yolo.onnx",
    "enableOnnx": true,
    "enableVision": true,
    "windowSize": 60,
    "inferenceHz": 2,
    "threadPoolSize": 2,
    "anomalyThreshold": 0.65
  },
  "ui": {
    "theme": "industrial-dark",
    "chartPoints": 120,
    "uiRefreshHz": 10,
    "showGrid": true,
    "language": "zh_CN"
  },
  "alarm": {
    "offlineTimeoutMs": 15000,
    "repeatSuppressionMs": 30000,
    "persistAlarms": true,
    "soundEnabled": false,
    "historyLimit": 500
  },
  "simulation": {
    "enabled": true,
    "deviceCount": 6,
    "sampleIntervalMs": 500,
    "injectAnomalies": true,
    "anomalyRatePercent": 3,
    "deviceIds": ["MOTOR-001", "MOTOR-002", "PUMP-001", "PUMP-002", "CNC-001", "FAN-001"]
  },
  "devices": [
    {
      "id": "MOTOR-001", "name": "Main Drive Motor", "type": "motor",
      "ip": "127.0.0.1", "port": 9100,
      "limits": { "temperatureWarn": 70, "temperatureHigh": 85,
                  "currentWarn": 15, "currentHigh": 20,
                  "vibrationWarn": 0.45, "vibrationHigh": 0.80 }
    },
    {
      "id": "PUMP-001", "name": "Coolant Pump", "type": "pump",
      "ip": "127.0.0.1", "port": 9100,
      "limits": { "temperatureWarn": 65, "temperatureHigh": 80,
                  "currentWarn": 12, "currentHigh": 16,
                  "vibrationWarn": 0.40, "vibrationHigh": 0.70 }
    },
    {
      "id": "CNC-001", "name": "CNC Spindle", "type": "spindle",
      "ip": "127.0.0.1", "port": 9100,
      "limits": { "temperatureWarn": 75, "temperatureHigh": 90,
                  "currentWarn": 18, "currentHigh": 24,
                  "vibrationWarn": 0.50, "vibrationHigh": 0.90 }
    }
  ]
}
)JSON";

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(kDefaults), &parseError);
    Q_ASSERT(parseError.error == QJsonParseError::NoError);

    QMutexLocker locker(&m_mutex);
    m_root = document.object();
}

// ---------------------------------------------------------------------------
// Typed view rebuild
// ---------------------------------------------------------------------------

void ConfigManager::rebuildTypedViews()
{
    QMutexLocker locker(&m_mutex);

    const auto str = [this](const QString& path, const QString& fallback) {
        const QJsonValue v = valueAtPath(path);
        return v.isString() ? v.toString() : fallback;
    };
    const auto num = [this](const QString& path, double fallback) {
        const QJsonValue v = valueAtPath(path);
        return v.isDouble() ? v.toDouble() : fallback;
    };
    const auto flag = [this](const QString& path, bool fallback) {
        const QJsonValue v = valueAtPath(path);
        return v.isBool() ? v.toBool() : fallback;
    };

    NetworkConfig net;
    net.listenAddress   = str(QStringLiteral("network.server.listenAddress"), net.listenAddress);
    net.listenPort      = static_cast<quint16>(num(QStringLiteral("network.server.listenPort"), net.listenPort));
    net.remoteHost      = str(QStringLiteral("network.client.remoteHost"), net.remoteHost);
    net.remotePort      = static_cast<quint16>(num(QStringLiteral("network.client.remotePort"), net.remotePort));
    net.autoConnect     = flag(QStringLiteral("network.client.autoConnect"), net.autoConnect);
    net.heartbeatMs     = static_cast<int>(num(QStringLiteral("network.client.heartbeatMs"), net.heartbeatMs));
    net.reconnectBaseMs = static_cast<int>(num(QStringLiteral("network.reconnect.baseMs"), net.reconnectBaseMs));
    net.reconnectMaxMs  = static_cast<int>(num(QStringLiteral("network.reconnect.maxMs"), net.reconnectMaxMs));
    m_network = net;

    DatabaseConfig db;
    db.filePath      = str(QStringLiteral("database.filePath"), db.filePath);
    db.retentionDays = static_cast<int>(num(QStringLiteral("database.retentionDays"), db.retentionDays));
    db.flushIntervalMs = static_cast<int>(num(QStringLiteral("database.flushIntervalMs"), db.flushIntervalMs));
    db.batchSize     = static_cast<int>(num(QStringLiteral("database.batchSize"), db.batchSize));
    m_database = db;

    AiConfig ai;
    ai.modelDirectory   = str(QStringLiteral("ai.modelDirectory"), ai.modelDirectory);
    ai.faultModelFile   = str(QStringLiteral("ai.faultModelFile"), ai.faultModelFile);
    ai.visionModelFile  = str(QStringLiteral("ai.visionModelFile"), ai.visionModelFile);
    ai.enableOnnx       = flag(QStringLiteral("ai.enableOnnx"), ai.enableOnnx);
    ai.enableVision     = flag(QStringLiteral("ai.enableVision"), ai.enableVision);
    ai.windowSize       = static_cast<int>(num(QStringLiteral("ai.windowSize"), ai.windowSize));
    ai.inferenceHz      = static_cast<int>(num(QStringLiteral("ai.inferenceHz"), ai.inferenceHz));
    ai.threadPoolSize   = static_cast<int>(num(QStringLiteral("ai.threadPoolSize"), ai.threadPoolSize));
    ai.anomalyThreshold = num(QStringLiteral("ai.anomalyThreshold"), ai.anomalyThreshold);
    m_ai = ai;

    UiConfig uiCfg;
    uiCfg.theme       = str(QStringLiteral("ui.theme"), uiCfg.theme);
    uiCfg.chartPoints = static_cast<int>(num(QStringLiteral("ui.chartPoints"), uiCfg.chartPoints));
    uiCfg.uiRefreshHz = static_cast<int>(num(QStringLiteral("ui.uiRefreshHz"), uiCfg.uiRefreshHz));
    uiCfg.showGrid    = flag(QStringLiteral("ui.showGrid"), uiCfg.showGrid);
    uiCfg.language    = str(QStringLiteral("ui.language"), uiCfg.language);
    m_ui = uiCfg;

    LoggingConfig log;
    log.level        = str(QStringLiteral("logging.level"), log.level);
    log.directory    = str(QStringLiteral("logging.directory"), log.directory);
    log.maxFileBytes = static_cast<qint64>(num(QStringLiteral("logging.maxFileBytes"), static_cast<double>(log.maxFileBytes)));
    log.keepFiles    = static_cast<int>(num(QStringLiteral("logging.keepFiles"), log.keepFiles));
    log.console      = flag(QStringLiteral("logging.console"), log.console);
    m_logging = log;

    SimulationConfig sim;
    sim.enabled             = flag(QStringLiteral("simulation.enabled"), sim.enabled);
    sim.deviceCount         = static_cast<int>(num(QStringLiteral("simulation.deviceCount"), sim.deviceCount));
    sim.sampleIntervalMs    = static_cast<int>(num(QStringLiteral("simulation.sampleIntervalMs"), sim.sampleIntervalMs));
    sim.injectAnomalies     = flag(QStringLiteral("simulation.injectAnomalies"), sim.injectAnomalies);
    sim.anomalyRatePercent  = static_cast<int>(num(QStringLiteral("simulation.anomalyRatePercent"), sim.anomalyRatePercent));
    const QJsonValue ids = valueAtPath(QStringLiteral("simulation.deviceIds"));
    if (ids.isArray())
    {
        sim.deviceIds.clear();
        for (const QJsonValue& entry : ids.toArray())
        {
            if (entry.isString())
            {
                sim.deviceIds << entry.toString();
            }
        }
    }
    m_simulation = sim;

    AlarmConfig alarm;
    alarm.offlineTimeoutMs    = static_cast<int>(num(QStringLiteral("alarm.offlineTimeoutMs"), alarm.offlineTimeoutMs));
    alarm.repeatSuppressionMs = static_cast<int>(num(QStringLiteral("alarm.repeatSuppressionMs"), alarm.repeatSuppressionMs));
    alarm.persistAlarms       = flag(QStringLiteral("alarm.persistAlarms"), alarm.persistAlarms);
    alarm.soundEnabled        = flag(QStringLiteral("alarm.soundEnabled"), alarm.soundEnabled);
    alarm.historyLimit        = static_cast<int>(num(QStringLiteral("alarm.historyLimit"), alarm.historyLimit));
    m_alarms = alarm;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

QJsonValue ConfigManager::valueAtPath(const QString& path) const
{
    const QStringList parts = splitPath(path);
    QJsonValue cursor = m_root;
    for (const QString& part : parts)
    {
        if (!cursor.isObject())
        {
            return QJsonValue{};
        }
        cursor = cursor.toObject().value(part);
    }
    return cursor;
}

bool ConfigManager::getBool(const QString& path, bool fallback) const
{
    QMutexLocker locker(&m_mutex);
    const QJsonValue v = valueAtPath(path);
    return v.isBool() ? v.toBool() : fallback;
}

int ConfigManager::getInt(const QString& path, int fallback) const
{
    QMutexLocker locker(&m_mutex);
    const QJsonValue v = valueAtPath(path);
    return v.isDouble() ? v.toInt(fallback) : fallback;
}

qint64 ConfigManager::getInt64(const QString& path, qint64 fallback) const
{
    QMutexLocker locker(&m_mutex);
    const QJsonValue v = valueAtPath(path);
    return v.isDouble() ? static_cast<qint64>(v.toDouble()) : fallback;
}

double ConfigManager::getDouble(const QString& path, double fallback) const
{
    QMutexLocker locker(&m_mutex);
    const QJsonValue v = valueAtPath(path);
    return v.isDouble() ? v.toDouble() : fallback;
}

QString ConfigManager::getString(const QString& path, const QString& fallback) const
{
    QMutexLocker locker(&m_mutex);
    const QJsonValue v = valueAtPath(path);
    return v.isString() ? v.toString() : fallback;
}

QStringList ConfigManager::getStringList(const QString& path) const
{
    QMutexLocker locker(&m_mutex);
    QStringList out;
    const QJsonValue v = valueAtPath(path);
    if (v.isArray())
    {
        for (const QJsonValue& entry : v.toArray())
        {
            if (entry.isString())
            {
                out << entry.toString();
            }
        }
    }
    return out;
}

QJsonObject ConfigManager::getObject(const QString& path) const
{
    QMutexLocker locker(&m_mutex);
    return valueAtPath(path).toObject();
}

QJsonArray ConfigManager::getArray(const QString& path) const
{
    QMutexLocker locker(&m_mutex);
    return valueAtPath(path).toArray();
}

bool ConfigManager::contains(const QString& path) const
{
    QMutexLocker locker(&m_mutex);
    return !valueAtPath(path).isUndefined();
}

void ConfigManager::set(const QString& path, const QJsonValue& value)
{
    {
        QMutexLocker locker(&m_mutex);
        setValueAtPath(m_root, path, value);
        setValueAtPath(m_overrides, path, value);
    }
    rebuildTypedViews();
    emit valueChanged(path, value);
}

QJsonObject ConfigManager::snapshot() const
{
    QMutexLocker locker(&m_mutex);
    return m_root;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

bool ConfigManager::saveUserOverlay() const
{
    QJsonObject overrides;
    {
        QMutexLocker locker(&m_mutex);
        overrides = m_overrides;
    }

    const QString path = userOverlayPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    // QSaveFile writes to a temporary and renames on commit, so a crash mid-write
    // cannot leave a truncated config that fails to parse on next start-up.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        SAIM_ERROR(logcat::kConfig, QStringLiteral("Cannot write user config: %1").arg(path));
        return false;
    }
    file.write(QJsonDocument(overrides).toJson(QJsonDocument::Indented));
    if (!file.commit())
    {
        SAIM_ERROR(logcat::kConfig, QStringLiteral("Failed to commit user config: %1").arg(path));
        return false;
    }

    SAIM_INFO(logcat::kConfig, QStringLiteral("User configuration saved to %1").arg(path));
    return true;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

QString ConfigManager::resolveDataPath(const QString& maybeRelative) const
{
    QMutexLocker lock(&m_mutex);
    if (maybeRelative == QStringLiteral(":memory:")) return maybeRelative;
    return QDir::cleanPath(QDir(m_dataRoot).absoluteFilePath(maybeRelative));
}

QString ConfigManager::resolvePath(const QString& maybeRelative) const
{
    QMutexLocker lock(&m_mutex);
    if (maybeRelative.isEmpty())
    {
        return m_dataRoot;
    }
    const QFileInfo info(maybeRelative);
    if (info.isAbsolute())
    {
        return info.absoluteFilePath();
    }

    // Look next to the executable first (installed layout), then in the source
    // tree (development layout), so the same config works in both.
    const QString installed = QDir(m_dataRoot).filePath(maybeRelative);
    if (QFileInfo::exists(installed))
    {
        return QDir::cleanPath(installed);
    }

    QDir walker(m_dataRoot);
    for (int depth = 0; depth < 4; ++depth)
    {
        if (!walker.cdUp())
        {
            break;
        }
        const QString candidate = walker.filePath(maybeRelative);
        if (QFileInfo::exists(candidate) || QFileInfo::exists(QFileInfo(candidate).absolutePath()))
        {
            return QDir::cleanPath(candidate);
        }
    }
    return QDir::cleanPath(installed);
}

void ConfigManager::setDataRoot(const QString& root)
{
    QMutexLocker lock(&m_mutex);
    m_dataRoot = root;
}

}   // namespace saim
