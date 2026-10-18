#include "app/ApplicationController.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "device/DeviceProtocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <functional>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#endif

using namespace saim;

namespace {
// All sockets and timers are created in run(). UI-thread stalls do not pause
// the generator, and no queued Qt event is allocated for every emitted sample.
class Gateway final : public QThread
{
public:
    Gateway(int devices, int interval) : m_devices(devices), m_interval(interval) {}
    ~Gateway() override { requestInterruption(); wait(); }
    std::atomic<quint16> port{0};
    std::atomic<bool> failed{false}, feeding{false}, paused{true};
    std::atomic<quint64> sent{0}, socketErrors{0}, connections{0};
    std::atomic<qint64> peakPendingBytes{0};
protected:
    void run() override
    {
        QTcpServer server;
        QEventLoop loop;
        QTimer tick;
        QTcpSocket* client = nullptr;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections())
            {
                auto* socket = server.nextPendingConnection();
                if (client != nullptr) { socket->abort(); socket->deleteLater(); continue; }
                client = socket;
                ++connections;
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    socket->readAll(); // The live telemetry itself satisfies heartbeat liveness.
                });
                QObject::connect(socket, &QTcpSocket::disconnected, &server, [&] {
                    client->deleteLater(); client = nullptr;
                });
            }
        });
        if (!server.listen(QHostAddress::LocalHost, 0)) { failed = true; return; }
        port = server.serverPort();
        tick.setTimerType(Qt::PreciseTimer);
        QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
            if (isInterruptionRequested()) { loop.quit(); return; }
            if (!feeding) { paused = true; return; }
            paused = false;
            if (client == nullptr) return;
            if (client->bytesToWrite() > 4LL * 1024 * 1024)
            { ++socketErrors; feeding = false; return; }
            const auto timestamp = QDateTime::currentDateTimeUtc();
            for (int i = 0; i < m_devices; ++i)
            {
                SensorSample sample;
                sample.deviceId = QStringLiteral("STRESS-%1").arg(i, 3, 10, QLatin1Char('0'));
                sample.timestamp = timestamp;
                sample.temperature = 40.0 + static_cast<double>(sent.load() % 7) * 0.01;
                sample.voltage = 220.0;
                sample.current = 8.0;
                sample.vibration = 0.1;
                sample.rpm = 1450.0;
                const auto frame = ProtocolCodec::encodeSample(sample);
                if (client->write(frame) == frame.size()) { ++sent; }
                else { ++socketErrors; }
            }
            peakPendingBytes = std::max(peakPendingBytes.load(), client->bytesToWrite());
        });
        tick.start(m_interval);
        loop.exec();
        // Disconnect callbacks must not reference a destroyed local variable.
        if (client) { client->disconnect(&server); client->abort(); }
    }
private:
    int m_devices;
    int m_interval;
};

struct Memory
{
    quint64 working = 0, privateBytes = 0, peakWorking = 0;
};

Memory memoryUsage()
{
    Memory result;
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    {
        result.working = static_cast<quint64>(counters.WorkingSetSize);
        result.privateBytes = static_cast<quint64>(counters.PrivateUsage);
        result.peakWorking = static_cast<quint64>(counters.PeakWorkingSetSize);
    }
#elif defined(Q_OS_LINUX)
    QFile status(QStringLiteral("/proc/self/statm"));
    if (status.open(QIODevice::ReadOnly))
    {
        const auto values = status.readAll().simplified().split(' ');
        if (values.size() > 1)
            result.working = values[1].toULongLong() * static_cast<quint64>(sysconf(_SC_PAGESIZE));
        result.peakWorking = result.working;
    }
#endif
    return result;
}

QJsonObject inboxJson(const SampleInbox::Statistics& s)
{
    return {{"offered", static_cast<qint64>(s.offered)}, {"taken", static_cast<qint64>(s.taken)},
            {"rejected", static_cast<qint64>(s.rejected)}, {"evicted", static_cast<qint64>(s.evicted)},
            {"coalesced", static_cast<qint64>(s.coalesced)}, {"cancelled", static_cast<qint64>(s.cancelled)},
            {"waits", static_cast<qint64>(s.waits)}, {"wait_ms", s.waitMs},
            {"depth", s.depth}, {"capacity", s.capacity}, {"high_water", s.highWater},
            {"max_latency_ms", s.maxLatencyMs}, {"p95_latency_upper_ms", s.p95LatencyUpperMs}};
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition())
    {
        if (clock.elapsed() >= timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

int integerOption(const QCommandLineParser& parser, const QString& name, int minimum, int maximum)
{
    bool valid = false;
    const int value = parser.value(name).toInt(&valid);
    if (!valid || value < minimum || value > maximum)
        throw std::runtime_error(QStringLiteral("Invalid --%1; expected %2..%3")
                                     .arg(name).arg(minimum).arg(maximum).toStdString());
    return value;
}

QJsonObject verifyDatabase(const QString& path)
{
    QJsonObject result;
    const auto connection = QStringLiteral("stress-readback");
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        result["opened"] = db.open();
        if (db.isOpen())
        {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT COUNT(*) FROM SensorData")) && query.next())
                result["sensor_rows"] = query.value(0).toLongLong();
            if (query.exec(QStringLiteral("PRAGMA integrity_check")) && query.next())
                result["integrity_check"] = query.value(0).toString();
            if (query.exec(QStringLiteral("PRAGMA foreign_key_check")))
            {
                int violations = 0;
                while (query.next()) ++violations;
                result["foreign_key_violations"] = violations;
            }
            db.close();
        }
        else result["error"] = db.lastError().text();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SAIM-Pipeline-Stress"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Sustained real TCP / worker / SQLite acceptance workload"));
    parser.addHelpOption();
    parser.addOptions({{"duration", "Continuous workload duration in seconds.", "seconds", "1800"},
                       {"devices", "Number of configured devices.", "count", "16"},
                       {"interval-ms", "Gateway sampling interval per device.", "ms", "20"},
                       {"stall-every", "Main/controller thread stall interval; 0 disables.", "seconds", "30"},
                       {"stall-ms", "Main/controller thread stall duration.", "ms", "250"},
                       {"output", "New output directory (must not contain stress.db).", "directory", "stress-results"}});
    parser.process(app);
    try
    {
        const int duration = integerOption(parser, QStringLiteral("duration"), 1, 86400);
        const int deviceCount = integerOption(parser, QStringLiteral("devices"), 1, 256);
        const int interval = integerOption(parser, QStringLiteral("interval-ms"), 1, 10000);
        const int stallEvery = integerOption(parser, QStringLiteral("stall-every"), 0, 3600);
        const int stallMs = integerOption(parser, QStringLiteral("stall-ms"), 0, 1000);
        const auto output = QDir(parser.value(QStringLiteral("output"))).absolutePath();
        const auto databasePath = output + QStringLiteral("/stress.db");
        if (QFile::exists(databasePath) || !QDir().mkpath(output))
            throw std::runtime_error("Output directory unavailable or already contains stress.db; use a new run directory");

        Logger::Options logOptions;
        logOptions.minimumLevel = LogLevel::Warning;
        logOptions.consoleEnabled = true;
        logOptions.logDirectory = output + QStringLiteral("/logs");
        logOptions.recentCapacity = 128;
        Logger::instance().configure(logOptions);
        Gateway gateway(deviceCount, interval);
        gateway.start();
        if (!waitFor([&] { return gateway.port != 0 || gateway.failed; }, 10000) || gateway.failed)
            throw std::runtime_error("Gateway failed to start");

        ConfigManager config;
        config.setDataRoot(output);
        config.set(QStringLiteral("simulation.enabled"), false);
        config.set(QStringLiteral("network.client.remoteHost"), QStringLiteral("127.0.0.1"));
        config.set(QStringLiteral("network.client.remotePort"), gateway.port.load());
        config.set(QStringLiteral("network.client.autoConnect"), true);
        config.set(QStringLiteral("database.filePath"), databasePath);
        config.set(QStringLiteral("database.flushIntervalMs"), 100);
        config.set(QStringLiteral("database.batchSize"), 256);
        config.set(QStringLiteral("ai.enableOnnx"), false);
        config.set(QStringLiteral("ai.enableVision"), false);
        config.set(QStringLiteral("ai.windowSize"), 60);
        config.set(QStringLiteral("ai.inferenceHz"), 2);
        QJsonArray devices;
        for (int i = 0; i < deviceCount; ++i)
            devices.append(QJsonObject{{"id", QStringLiteral("STRESS-%1").arg(i, 3, 10, QLatin1Char('0'))},
                                       {"name", QStringLiteral("Stress device %1").arg(i)}, {"type", "motor"}});
        config.set(QStringLiteral("devices"), devices);
        QFile configuration(output + QStringLiteral("/effective-config.json"));
        if (!configuration.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot save effective configuration");
        configuration.write(QJsonDocument(config.snapshot()).toJson());
        configuration.close();
        ApplicationController controller(&config);
        const auto initialised = controller.initialise();
        if (!initialised.isOk()) throw std::runtime_error(initialised.error().message.toStdString());
        controller.start();
        if (!waitFor([&] { return gateway.connections > 0; }, 10000))
            throw std::runtime_error("Acquisition failed to connect to independent gateway");
        // The connected signal does not prove persistence startup; allow worker
        // initialization to settle before the measured stream starts.
        waitFor([&] { return controller.pipelineStatistics().persistence.capacity > 0; }, 1000);
        QElapsedTimer warmup;
        warmup.start();
        waitFor([&] { return warmup.elapsed() >= 250; }, 1000);

        QFile timeline(output + QStringLiteral("/timeline.jsonl"));
        if (!timeline.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot open timeline output");
        const auto startedAt = QDateTime::currentDateTimeUtc();
        QElapsedTimer elapsed;
        elapsed.start();
        gateway.feeding = true;
        qint64 nextReport = 0;
        qint64 nextStall = static_cast<qint64>(stallEvery) * 1000;
        int stalls = 0;
        Memory peak, baseline, finalMemory;
        bool baselineSet = false;
        while (elapsed.elapsed() < static_cast<qint64>(duration) * 1000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            const auto memory = memoryUsage();
            peak.working = std::max(peak.working, memory.working);
            peak.privateBytes = std::max(peak.privateBytes, memory.privateBytes);
            peak.peakWorking = std::max(peak.peakWorking, memory.peakWorking);
            if (!baselineSet && elapsed.elapsed() >= 30000) { baseline = memory; baselineSet = true; }
            finalMemory = memory;
            if (elapsed.elapsed() >= nextReport)
            {
                const auto stats = controller.pipelineStatistics();
                QJsonObject point{{"elapsed_ms", elapsed.elapsed()}, {"sent", static_cast<qint64>(gateway.sent.load())},
                                  {"received", static_cast<qint64>(stats.received)}, {"written", static_cast<qint64>(stats.written)},
                                  {"working_bytes", static_cast<qint64>(memory.working)},
                                  {"private_bytes", static_cast<qint64>(memory.privateBytes)},
                                  {"persistence", inboxJson(stats.persistence)}, {"ai", inboxJson(stats.ai)}, {"ui", inboxJson(stats.ui)}};
                timeline.write(QJsonDocument(point).toJson(QJsonDocument::Compact) + '\n');
                timeline.flush();
                QTextStream(stdout) << "elapsed=" << elapsed.elapsed() / 1000 << "s sent=" << gateway.sent.load()
                                    << " received=" << stats.received << " written=" << stats.written
                                    << " storage_drop=" << stats.storageDropped
                                    << " private_mib=" << memory.privateBytes / (1024 * 1024) << Qt::endl;
                nextReport += 10000;
            }
            if (stallEvery != 0 && elapsed.elapsed() >= nextStall)
            {
                QThread::msleep(static_cast<unsigned long>(stallMs));
                ++stalls;
                nextStall += static_cast<qint64>(stallEvery) * 1000;
            }
            if (gateway.socketErrors > 0) break;
            QThread::msleep(1);
        }
        const auto workloadMs = elapsed.elapsed();
        gateway.feeding = false;
        const bool producerPaused = waitFor([&] { return gateway.paused.load(); }, 2000);
        const auto emitted = gateway.sent.load();
        const bool drained = waitFor([&] {
            const auto s = controller.pipelineStatistics();
            return s.received == emitted && s.written == emitted
                   && s.persistence.depth == 0 && s.ai.depth == 0 && s.ui.depth == 0;
        }, 15000);
        QElapsedTimer stopTimer;
        stopTimer.start();
        controller.stop();
        const qint64 stopMs = stopTimer.elapsed();
        gateway.requestInterruption();
        gateway.wait();
        const auto stats = controller.pipelineStatistics();
        const auto database = verifyDatabase(databasePath);
        QJsonArray failures;
        const auto require = [&](bool condition, const QString& reason) { if (!condition) failures.append(reason); };
        require(workloadMs >= static_cast<qint64>(duration) * 1000, QStringLiteral("Workload terminated before requested duration"));
        require(producerPaused && drained, QStringLiteral("Pipeline failed to drain within 15 seconds"));
        require(gateway.socketErrors == 0 && gateway.connections == 1, QStringLiteral("Gateway socket failure or reconnect"));
        require(emitted > 0 && stats.received == emitted && stats.written == emitted,
                QStringLiteral("Sent / received / written counts differ"));
        require(stats.storageDropped == 0 && stats.persistence.rejected == 0 && stats.persistence.evicted == 0
                    && stats.persistence.cancelled == 0, QStringLiteral("Persistence lost samples"));
        require(stats.ai.rejected == 0 && stats.ai.evicted == 0 && stats.ai.cancelled == 0,
                QStringLiteral("AI lost samples under normal workload"));
        require(stats.ui.evicted == 0 && stats.ui.rejected == 0, QStringLiteral("UI mailbox exhausted capacity"));
        require(stats.recordRejected == 0 && stats.recordFailed == 0 && stats.analysisEvicted == 0,
                QStringLiteral("Derived record or analysis delivery failed"));
        require(stats.recordDepth == 0 && stats.recordHighWater <= 2048,
                QStringLiteral("Derived record mailbox not drained or exceeded capacity"));
        require(stats.persistence.highWater <= stats.persistence.capacity && stats.ai.highWater <= stats.ai.capacity
                    && stats.ui.highWater <= stats.ui.capacity, QStringLiteral("Queue exceeded its hard capacity"));
        require(database.value("sensor_rows").toVariant().toULongLong() == stats.written,
                QStringLiteral("Actual SQLite row count differs from committed counter"));
        require(database.value("integrity_check").toString() == QStringLiteral("ok")
                    && database.value("foreign_key_violations").toInt(-1) == 0, QStringLiteral("SQLite consistency check failed"));
        require(stopMs <= 5000, QStringLiteral("Graceful stop exceeded 5 seconds"));
        require(stats.persistence.p95LatencyUpperMs <= 1000, QStringLiteral("Persistence queue P95 upper bound exceeded 1 second"));
        require(peak.peakWorking <= 512ULL * 1024 * 1024, QStringLiteral("Working set exceeded 512 MiB"));
        const qint64 growth = baselineSet ? static_cast<qint64>(finalMemory.privateBytes) - static_cast<qint64>(baseline.privateBytes) : 0;
        require(!baselineSet || growth <= 64LL * 1024 * 1024, QStringLiteral("Private memory grew over 64 MiB after warmup"));
        const double actualRate = static_cast<double>(emitted) * 1000.0 / static_cast<double>(std::max<qint64>(1, workloadMs));
        const double requestedRate = static_cast<double>(deviceCount) * 1000.0 / static_cast<double>(interval);
        require(actualRate >= requestedRate * 0.8, QStringLiteral("Generator delivered less than 80% of requested rate"));
        QJsonObject report{{"schema_version", 1}, {"passed", failures.isEmpty()}, {"failures", failures},
                           {"started_at_utc", startedAt.toString(Qt::ISODateWithMs)}, {"duration_requested_seconds", duration},
                           {"workload_ms", workloadMs}, {"long_run", duration >= 1800},
                           {"qt_version", QString::fromLatin1(qVersion())}, {"os", QSysInfo::prettyProductName()},
                           {"cpu_arch", QSysInfo::currentCpuArchitecture()}, {"device_count", deviceCount},
                           {"interval_ms", interval}, {"requested_samples_per_second", requestedRate},
                           {"actual_samples_per_second", actualRate}, {"main_thread_stalls", stalls},
                           {"stall_ms", stallMs}, {"independent_gateway_thread", true}, {"widgets_rendered", false},
                           {"sent", static_cast<qint64>(emitted)}, {"received", static_cast<qint64>(stats.received)},
                           {"written", static_cast<qint64>(stats.written)}, {"storage_dropped", static_cast<qint64>(stats.storageDropped)},
                           {"record_rejected", static_cast<qint64>(stats.recordRejected)},
                           {"record_failed", static_cast<qint64>(stats.recordFailed)},
                           {"record_high_water", stats.recordHighWater},
                           {"analysis_replaced", static_cast<qint64>(stats.analysisReplaced)},
                           {"analysis_evicted", static_cast<qint64>(stats.analysisEvicted)},
                           {"gateway_peak_pending_bytes", gateway.peakPendingBytes.load()},
                           {"peak_working_set_bytes", static_cast<qint64>(peak.peakWorking)},
                           {"peak_private_bytes", static_cast<qint64>(peak.privateBytes)},
                           {"private_growth_after_30s_bytes", growth}, {"memory_warmup_completed", baselineSet},
                           {"stop_ms", stopMs}, {"drained", drained}, {"database", database},
                           {"persistence", inboxJson(stats.persistence)}, {"ai", inboxJson(stats.ai)}, {"ui", inboxJson(stats.ui)}};
        QSaveFile outputFile(output + QStringLiteral("/report.json"));
        if (!outputFile.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot save report");
        outputFile.write(QJsonDocument(report).toJson());
        if (!outputFile.commit()) throw std::runtime_error("Cannot commit report");
        QTextStream(stdout) << (failures.isEmpty() ? "PASS " : "FAIL ") << output + QStringLiteral("/report.json") << Qt::endl;
        return failures.isEmpty() ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        QTextStream(stderr) << "stress setup failed: " << error.what() << Qt::endl;
        return 2;
    }
}
