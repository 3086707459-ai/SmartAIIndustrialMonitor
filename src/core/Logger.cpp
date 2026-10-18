#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTextStream>
#include <QThread>

#include <cstdio>

namespace saim {

namespace {

/// Serialises every sink write.  A single global mutex is the right trade-off
/// here: log volume in this application is a few hundred lines per second at
/// peak, contention is negligible, and per-sink locking would not remove the
/// ordering problem between sinks anyway.
QMutex g_sinkIoMutex;

constexpr int kLevelWidth = 5;

QString padLevel(LogLevel level)
{
    return toString(level).leftJustified(kLevelWidth, QLatin1Char(' '));
}

bool stderrIsTty()
{
#ifdef _WIN32
    return false;   // Windows consoles need VT enabling; keep output plain.
#else
    return ::isatty(fileno(stderr)) != 0;
#endif
}

}   // namespace

// ---------------------------------------------------------------------------
// LogLevel
// ---------------------------------------------------------------------------

QString toString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:    return QStringLiteral("TRACE");
    case LogLevel::Debug:    return QStringLiteral("DEBUG");
    case LogLevel::Info:     return QStringLiteral("INFO");
    case LogLevel::Warning:  return QStringLiteral("WARN");
    case LogLevel::Error:    return QStringLiteral("ERROR");
    case LogLevel::Critical: return QStringLiteral("FATAL");
    }
    return QStringLiteral("?");
}

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------

void ConsoleSink::write(const LogRecord& record)
{
    if (static_cast<int>(record.level) < static_cast<int>(m_minimum))
    {
        return;
    }

    static const bool colour = stderrIsTty();

    const QString line = QStringLiteral("%1 [%2] [%3] %4")
                             .arg(record.timestamp.toString(QStringLiteral("HH:mm:ss.zzz")),
                                  padLevel(record.level),
                                  record.threadName.leftJustified(12, QLatin1Char(' ')),
                                  record.message);

    if (!colour)
    {
        std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
        return;
    }

    const char* tint = "";
    switch (record.level)
    {
    case LogLevel::Trace:
    case LogLevel::Debug:    tint = "\033[90m"; break;   // bright black
    case LogLevel::Info:     tint = "\033[36m"; break;   // cyan
    case LogLevel::Warning:  tint = "\033[33m"; break;   // yellow
    case LogLevel::Error:    tint = "\033[31m"; break;   // red
    case LogLevel::Critical: tint = "\033[1;41m"; break; // bold on red
    }
    std::fprintf(stderr, "%s%s\033[0m\n", tint, line.toLocal8Bit().constData());
}

// ---------------------------------------------------------------------------
// FileSink
// ---------------------------------------------------------------------------

FileSink::FileSink(QString filePath, qint64 maxBytes, int keepFiles)
    : m_path(std::move(filePath))
    , m_maxBytes(maxBytes)
    , m_keepFiles(keepFiles)
{
    ensureOpen();
}

FileSink::~FileSink()
{
    flush();
}

bool FileSink::ensureOpen()
{
    if (m_file.isOpen())
    {
        return true;
    }

    const QFileInfo info(m_path);
    QDir().mkpath(info.absolutePath());

    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        // Deliberately silent: logging to a broken file must not recurse into
        // logging.  The console sink still carries the message.
        return false;
    }
    m_written = m_file.size();
    return true;
}

void FileSink::rotateIfNeeded()
{
    if (m_maxBytes <= 0 || m_written < m_maxBytes)
    {
        return;
    }

    m_file.close();

    // smartai.log -> smartai.4.log, shifting the older ones along, then drop the
    // oldest.  Bounded disk usage without an external logrotate dependency.
    const QFileInfo info(m_path);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QStringLiteral("log") : info.suffix();
    const QString dir = info.absolutePath();

    QDir().remove(QStringLiteral("%1/%2.%3.%4").arg(dir, base).arg(m_keepFiles).arg(suffix));

    for (int i = m_keepFiles - 1; i >= 1; --i)
    {
        const QString from = QStringLiteral("%1/%2.%3.%4").arg(dir, base).arg(i).arg(suffix);
        const QString to = QStringLiteral("%1/%2.%3.%4").arg(dir, base).arg(i + 1).arg(suffix);
        if (QFile::exists(from))
        {
            QDir().rename(from, to);
        }
    }

    const QString first = QStringLiteral("%1/%2.1.%3").arg(dir, base, suffix);
    QDir().rename(m_path, first);

    m_written = 0;
    ensureOpen();
}

void FileSink::write(const LogRecord& record)
{
    QMutexLocker locker(&g_sinkIoMutex);

    rotateIfNeeded();
    if (!ensureOpen())
    {
        return;
    }

    // Machine-parsable, one record per line, fixed field order:
    //   ISO8601 | LEVEL | thread | category | file:line | message
    const QString line = QStringLiteral("%1 | %2 | %3 | %4 | %5:%6 | %7\n")
                             .arg(record.timestamp.toString(Qt::ISODateWithMs),
                                  padLevel(record.level),
                                  record.threadName,
                                  record.category,
                                  record.file,
                                  QString::number(record.line),
                                  record.message);

    const QByteArray utf8 = line.toUtf8();
    m_file.write(utf8);
    m_written += utf8.size();
}

void FileSink::flush()
{
    QMutexLocker locker(&g_sinkIoMutex);
    if (m_file.isOpen())
    {
        m_file.flush();
    }
}

// ---------------------------------------------------------------------------
// SignalSink
// ---------------------------------------------------------------------------

void SignalSink::write(const LogRecord& record)
{
    emit recordLogged(record);
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger& Logger::instance()
{
    static Logger s_instance;
    return s_instance;
}

Logger::Logger()
    : m_recentCapacity(2000)
{
    m_uptime.start();
}

Logger::~Logger()
{
    flushSinks();
}

void Logger::configure(const Options& options)
{
    setMinimumLevel(options.minimumLevel);
    m_recentCapacity = options.recentCapacity > 0 ? options.recentCapacity : 1;

    {
        QMutexLocker locker(&m_sinkMutex);
        m_sinks.clear();

        if (options.consoleEnabled)
        {
            m_sinks.push_back(std::make_unique<ConsoleSink>(LogLevel::Trace));
        }
        if (!options.logDirectory.isEmpty())
        {
            const QString path = QDir(options.logDirectory).filePath(options.fileName);
            m_sinks.push_back(
                std::make_unique<FileSink>(path, options.maxFileBytes, options.keepFiles));
        }
    }

    {
        QMutexLocker locker(&m_historyMutex);
        m_recent.clear();
        m_recent.reserve(static_cast<int>(m_recentCapacity));
    }
}

void Logger::addSink(std::unique_ptr<LogSink> sink)
{
    if (!sink)
    {
        return;
    }
    QMutexLocker locker(&m_sinkMutex);
    m_sinks.push_back(std::move(sink));
}

LogSink* Logger::sink(const QString& name) const
{
    QMutexLocker locker(&m_sinkMutex);
    for (const auto& candidate : m_sinks)
    {
        if (candidate->name() == name)
        {
            return candidate.get();
        }
    }
    return nullptr;
}

void Logger::flushSinks()
{
    QMutexLocker locker(&m_sinkMutex);
    for (const auto& sink : m_sinks)
    {
        sink->flush();
    }
}

void Logger::log(LogLevel level, const QString& category, const QString& message,
                 const char* file, int line)
{
    if (static_cast<int>(level) < static_cast<int>(m_minimumLevel.load()))
    {
        return;
    }

    LogRecord record;
    record.timestamp  = QDateTime::currentDateTime();
    record.level      = level;
    record.category   = category;
    record.message    = message;
    record.threadName = QThread::currentThread() != nullptr
                            ? QThread::currentThread()->objectName()
                            : QStringLiteral("unknown");
    if (record.threadName.isEmpty())
    {
        record.threadName = QStringLiteral("unnamed");
    }
    record.file      = file != nullptr ? QString::fromUtf8(file) : QString();
    record.line      = line;
    record.elapsedMs = m_uptime.elapsed();

    writeToSinks(record);

    {
        QMutexLocker locker(&m_historyMutex);
        if (static_cast<std::size_t>(m_recent.size()) >= m_recentCapacity)
        {
            // Drop the oldest quarter at once rather than one per insert: it
            // turns an O(n) memmove per record into one amortised O(n)/n.
            const int dropCount = static_cast<int>(m_recentCapacity / 4) + 1;
            m_recent.remove(0, qMin(dropCount, m_recent.size()));
            m_dropped.fetch_add(dropCount);
        }
        m_recent.push_back(record);
    }

    emit recordAdded(record);
}

void Logger::writeToSinks(const LogRecord& record)
{
    // Snapshot the pointer list under the lock, then write outside it so a slow
    // sink (a blocking pipe, a full disk) cannot serialise the producer.
    std::vector<LogSink*> snapshot;
    {
        QMutexLocker locker(&m_sinkMutex);
        snapshot.reserve(m_sinks.size());
        for (const auto& sink : m_sinks)
        {
            snapshot.push_back(sink.get());
        }
    }
    for (LogSink* sink : snapshot)
    {
        sink->write(record);
    }
}

QVector<LogRecord> Logger::recent(int maxCount) const
{
    QMutexLocker locker(&m_historyMutex);
    if (maxCount <= 0 || m_recent.size() <= maxCount)
    {
        return m_recent;
    }
    return m_recent.mid(m_recent.size() - maxCount);
}

// ---------------------------------------------------------------------------
// Qt message handler
// ---------------------------------------------------------------------------

namespace {

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    LogLevel level = LogLevel::Debug;
    switch (type)
    {
    case QtDebugMsg:    level = LogLevel::Debug;    break;
    case QtInfoMsg:     level = LogLevel::Info;     break;
    case QtWarningMsg:  level = LogLevel::Warning;  break;
    case QtCriticalMsg: level = LogLevel::Error;    break;
    case QtFatalMsg:    level = LogLevel::Critical; break;
    }

    const QString category = context.category != nullptr ? QString::fromUtf8(context.category)
                                                         : QStringLiteral("qt");

    Logger::instance().log(level, category, message, context.file, context.line);

    if (type == QtFatalMsg)
    {
        Logger::instance().flushSinks();
        std::abort();
    }
}

}   // namespace

void Logger::installQtMessageHandler()
{
    qInstallMessageHandler(&qtMessageHandler);
}

}   // namespace saim
