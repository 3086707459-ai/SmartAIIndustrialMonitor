#pragma once

// ---------------------------------------------------------------------------
// Logger.h - structured, thread-safe, sink-based logging.
//
// Design notes (interview-relevant):
//
//  * Sinks, not ifdefs.  Console / file / UI are three implementations of one
//    LogSink interface.  Adding a syslog or network sink later is a new class,
//    not an edit to Logger (open/closed principle).
//
//  * Thread safety without a UI stall.  `log()` may be called from any of the
//    four worker threads.  The sinks are mutex-guarded, and the UI sink emits a
//    queued signal, so a log storm on the acquisition thread can never block a
//    paint.  The UI panel additionally coalesces bursts (see LogPanel).
//
//  * Bounded memory.  A ring buffer of the most recent records lets the UI show
//    history after it attaches, without the logger retaining an unbounded list.
//
//  * Qt integration.  installQtMessageHandler() routes qDebug/qWarning/qFatal
//    through the same pipeline, so third-party Qt warnings land in the project
//    log file with file/line info instead of going to a debugger that is not
//    attached in a release build.
// ---------------------------------------------------------------------------

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>
#include <vector>

namespace saim {

enum class LogLevel : int
{
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Critical
};

[[nodiscard]] QString toString(LogLevel level);

/// One log line, fully self-describing so a sink needs no other context.
struct LogRecord
{
    QDateTime   timestamp;
    LogLevel    level = LogLevel::Info;
    QString     category;      ///< "NET", "DB", "AI", "UI", ...
    QString     message;
    QString     threadName;    ///< owning QThread::objectName()
    QString     file;
    int         line = 0;
    qint64      elapsedMs = 0; ///< since Logger::start()
};

// ---------------------------------------------------------------------------
// LogSink - abstract output channel.
// ---------------------------------------------------------------------------
class LogSink
{
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
    [[nodiscard]] virtual QString name() const = 0;
};

/// Writes to stderr, colourised when the terminal supports it.
class ConsoleSink final : public LogSink
{
public:
    explicit ConsoleSink(LogLevel minimum = LogLevel::Debug) : m_minimum(minimum) {}
    void write(const LogRecord& record) override;
    [[nodiscard]] QString name() const override { return QStringLiteral("console"); }

private:
    LogLevel m_minimum;
};

/// Appends to a file, rotating when it exceeds `maxBytes` (keeps `keepFiles`).
class FileSink final : public LogSink
{
public:
    FileSink(QString filePath, qint64 maxBytes = 8LL * 1024 * 1024, int keepFiles = 5);
    ~FileSink() override;

    void write(const LogRecord& record) override;
    void flush() override;
    [[nodiscard]] QString name() const override { return QStringLiteral("file"); }

private:
    void rotateIfNeeded();
    bool ensureOpen();

    QString m_path;
    qint64  m_maxBytes;
    int     m_keepFiles;
    QFile   m_file;
    qint64  m_written = 0;
};

/// Emits every record as a Qt signal (queued to the UI thread).
///
/// The sink itself lives on the logger's thread; Qt::QueuedConnection in the
/// receiver's connect() call is what moves the payload across threads.
class SignalSink final : public QObject, public LogSink
{
    Q_OBJECT
public:
    explicit SignalSink(QObject* parent = nullptr) : QObject(parent) {}

    void write(const LogRecord& record) override;
    [[nodiscard]] QString name() const override { return QStringLiteral("signal"); }

signals:
    void recordLogged(const saim::LogRecord& record);
};

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
class Logger final : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        LogLevel minimumLevel = LogLevel::Debug;
        QString  logDirectory;             ///< empty -> file sink disabled
        QString  fileName = QStringLiteral("smartai.log");
        qint64   maxFileBytes = 8LL * 1024 * 1024;
        int      keepFiles = 5;
        bool     consoleEnabled = true;
        std::size_t recentCapacity = 2000;
    };

    /// Process-wide instance.  A function-local static: initialised on first use,
    /// destroyed at exit, and immune to the static-initialisation-order fiasco
    /// that a namespace-scope global would suffer.
    [[nodiscard]] static Logger& instance();

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&)                 = delete;
    Logger& operator=(Logger&&)      = delete;

    /// Replaces the sink set.  Safe to call once during start-up.
    void configure(const Options& options);

    void log(LogLevel level,
             const QString& category,
             const QString& message,
             const char* file = nullptr,
             int line = 0);

    void addSink(std::unique_ptr<LogSink> sink);

    /// Looks up an installed sink by its name().  Returns nullptr if absent.
    ///
    /// This exists so that a consumer which needs a *specific* sink - the log
    /// panel attaching to the signal sink, the persistence worker attaching a
    /// database sink - can find it without the Logger having to know about
    /// either of them.  The reverse arrangement, a Logger member per consumer,
    /// is what turns a logging facility into a hub that everything depends on.
    [[nodiscard]] LogSink* sink(const QString& name) const;

    /// Routes qDebug/qInfo/qWarning/qCritical through this logger.
    void installQtMessageHandler();

    [[nodiscard]] LogLevel minimumLevel() const noexcept { return m_minimumLevel; }
    void setMinimumLevel(LogLevel level) noexcept { m_minimumLevel = level; }

    /// Snapshot of the most recent records, newest last.
    [[nodiscard]] QVector<LogRecord> recent(int maxCount = 200) const;

    [[nodiscard]] qint64 elapsedMs() const noexcept { return m_uptime.elapsed(); }

    /// Records dropped because the recent-buffer was full, since start.
    [[nodiscard]] qint64 droppedCount() const noexcept { return m_dropped.load(); }

signals:
    void recordAdded(const saim::LogRecord& record);

private:
    Logger();
    ~Logger() override;

    void writeToSinks(const LogRecord& record);

public:
    /// Flushes every sink.  Public because the Qt message handler calls it on a
    /// fatal message, immediately before abort() - and at that point the process
    /// is already on the way down, so a private accessor would only force the
    /// free handler to be a friend.
    void flushSinks();

private:

    mutable QMutex                m_sinkMutex;
    std::vector<std::unique_ptr<LogSink>> m_sinks;

    mutable QMutex      m_historyMutex;
    QVector<LogRecord>  m_recent;
    std::size_t         m_recentCapacity;

    std::atomic<LogLevel>  m_minimumLevel{LogLevel::Debug};
    QElapsedTimer          m_uptime;
    std::atomic<qint64>    m_dropped{0};
};

// ---------------------------------------------------------------------------
// Convenience macros.
//
// They capture __FILE__/__LINE__ so that a log line points at the caller rather
// than at the logger, and they short-circuit before building the QString when
// the level is filtered out - so a Trace call in a per-frame path costs one
// atomic load, not a string allocation.
// ---------------------------------------------------------------------------

#define SAIM_LOG(level, category, message)                                                    \
    do {                                                                                      \
        if (static_cast<int>(level) >= static_cast<int>(::saim::Logger::instance().minimumLevel())) \
        {                                                                                     \
            ::saim::Logger::instance().log((level), (category), (message), __FILE__, __LINE__);\
        }                                                                                     \
    } while (false)

#define SAIM_TRACE(category, message)    SAIM_LOG(::saim::LogLevel::Trace,    category, message)
#define SAIM_DEBUG(category, message)    SAIM_LOG(::saim::LogLevel::Debug,    category, message)
#define SAIM_INFO(category, message)     SAIM_LOG(::saim::LogLevel::Info,     category, message)
#define SAIM_WARN(category, message)     SAIM_LOG(::saim::LogLevel::Warning,  category, message)
#define SAIM_ERROR(category, message)    SAIM_LOG(::saim::LogLevel::Error,    category, message)
#define SAIM_CRITICAL(category, message) SAIM_LOG(::saim::LogLevel::Critical, category, message)

/// Log categories, centralised so the UI filter list and the call sites cannot
/// drift apart.
namespace logcat {
inline constexpr const char* kApp      = "APP";
inline constexpr const char* kConfig   = "CFG";
inline constexpr const char* kDevice   = "DEV";
inline constexpr const char* kNetwork  = "NET";
inline constexpr const char* kDatabase = "DB";
inline constexpr const char* kThread   = "THR";
inline constexpr const char* kAi       = "AI";
inline constexpr const char* kVision   = "VIS";
inline constexpr const char* kUi       = "UI";
}   // namespace logcat

}   // namespace saim

// ---------------------------------------------------------------------------
// A LogRecord is emitted by SignalSink from whichever thread logged it and
// consumed by the UI panel and by the persistence worker, both of which live
// elsewhere.  Every one of those hops is a queued connection, and a queued
// connection to an unregistered type is dropped at run time with only a warning
// on stderr - so the log window would silently stay empty and the SystemLog
// table would silently stay empty with it.  Registering here means any
// translation unit that includes Logger.h is safe.
// ---------------------------------------------------------------------------
Q_DECLARE_METATYPE(saim::LogRecord)
Q_DECLARE_METATYPE(QVector<saim::LogRecord>)
