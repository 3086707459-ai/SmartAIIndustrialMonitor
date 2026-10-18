#include "TestHarness.h"
#include "thread/ThreadedWorker.h"
#include "thread/WorkerPool.h"
#include "thread/AcquisitionWorker.h"
#include "thread/AIWorker.h"
#include "thread/PersistenceWorker.h"
#include "core/ConfigManager.h"
#include "device/DeviceManager.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QTemporaryDir>
#include <atomic>
#include <thread>

using namespace saim;

namespace {
class ProbeWorker final : public ThreadedWorker
{
public:
    ProbeWorker() : ThreadedWorker(QStringLiteral("probe"))
    {
        timer.setParent(this);
        timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, this, [this] { ++ticks; });
    }
    ~ProbeWorker() override { stop(); }
    std::atomic<int> ticks{0};
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    QTimer timer;
protected:
    void onStart() override { ++starts; timer.start(); }
    void onStop() override { timer.stop(); ++stops; }
};
}

SAIM_TEST(worker_timers_and_blocking_calls_survive_restart)
{
    ProbeWorker worker;
    auto* owner = QThread::currentThread();
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        const int previous = worker.ticks.load();
        SAIM_CHECK(worker.start());
        bool nested = false;
        bool affinity = false;
        worker.invokeBlocking([&] {
            affinity = QThread::currentThread() == worker.thread()
                       && worker.timer.thread() == worker.thread();
            worker.invokeBlocking([&] { nested = true; });
        });
        QElapsedTimer deadline;
        deadline.start();
        while (worker.ticks.load() == previous && deadline.elapsed() < 2000)
        {
            QThread::msleep(5);
        }
        worker.stop();
        SAIM_CHECK(nested && affinity);
        SAIM_CHECK(worker.ticks.load() > previous);
        SAIM_CHECK(worker.QObject::thread() == owner);
        SAIM_CHECK(worker.timer.thread() == owner);
        SAIM_CHECK(!worker.isRunning());
    }
    worker.stop();
    SAIM_CHECK(worker.starts.load() == 3);
    SAIM_CHECK(worker.stops.load() == 3);
}

SAIM_TEST(worker_can_request_its_own_stop_without_self_join)
{
    ProbeWorker worker;
    SAIM_CHECK(worker.start());
    worker.invokeBlocking([&] { worker.stop(); });
    worker.stop();
    SAIM_CHECK(worker.stops.load() == 1);
    SAIM_CHECK(!worker.isRunning());
}

SAIM_TEST(pool_releases_all_owned_workers)
{
    ConfigManager config;
    config.load({});
    DeviceManager devices(&config);
    QPointer<AcquisitionWorker> acquisition;
    QPointer<AIWorker> ai;
    QPointer<PersistenceWorker> persistence;
    {
        WorkerPool pool(&config, &devices);
        pool.create();
        acquisition = pool.acquisition(); ai = pool.ai(); persistence = pool.persistence();
    }
    SAIM_CHECK(acquisition.isNull());
    SAIM_CHECK(ai.isNull());
    SAIM_CHECK(persistence.isNull());
}

SAIM_TEST(persistence_keeps_ingestion_state_across_threshold_updates)
{
    QTemporaryDir dir;
    SAIM_CHECK(dir.isValid());
    ConfigManager config;
    config.load({});
    config.set(QStringLiteral("database.filePath"), dir.filePath(QStringLiteral("state.db")));
    DeviceConfiguration device;
    device.id = QStringLiteral("motor"); device.name = device.id;
    device.type = QStringLiteral("motor");
    PersistenceWorker worker(&config);
    worker.setDevices({device});
    SAIM_CHECK(worker.start());
    SensorSample sample;
    sample.deviceId = device.id;
    sample.timestamp = QDateTime::currentDateTimeUtc();
    sample.temperature = 75; sample.voltage = 220;
    sample.current = 8; sample.vibration = 0.1; sample.rpm = 1500;
    worker.invokeBlocking([&] {
        worker.storeSample(sample); // Warning at the original threshold.
        device.limits.temperatureHigh = 60;
        device.limits.temperatureWarn = 50;
        worker.setDevices({device});
        sample.timestamp = sample.timestamp.addMSecs(1);
        worker.storeSample(sample); // Fault at the new threshold.
        worker.setDevices({});
        worker.storeSample(sample); // Removed device: ignored.
        device.id = QStringLiteral("new-device"); device.name = device.id;
        worker.setDevices({device});
        sample.deviceId = device.id;
        worker.storeSample(sample); // New foreign-key parent registered first.
    });
    worker.stop();
    DatabaseManager db(QStringLiteral("state-readback"));
    SAIM_CHECK(db.open(config.database().filePath).isOk());
    DataRepository repository(&db);
    const auto history = repository.queryHistory({});
    SAIM_CHECK(history.isOk());
    SAIM_CHECK(history.value().size() == 3);
    SAIM_CHECK(history.value()[0].state == DeviceState::Warning);
    SAIM_CHECK(history.value()[1].state == DeviceState::Fault);
    SAIM_CHECK(history.value()[2].deviceId == QStringLiteral("new-device"));
}

SAIM_TEST(ai_uses_copied_limits_and_forgets_removed_devices)
{
    ConfigManager config;
    config.load({});
    config.set(QStringLiteral("ai.enableOnnx"), false);
    config.set(QStringLiteral("ai.enableVision"), false);
    AIWorker worker(&config);
    DeviceConfiguration device;
    device.id = QStringLiteral("motor");
    device.limits.temperatureWarn = 150; device.limits.temperatureHigh = 200;
    worker.setDevices({device});
    device.limits.temperatureWarn = 70; device.limits.temperatureHigh = 85;
    QVector<AIAnalysisResult> results;
    QObject::connect(&worker, &AIWorker::analysisReady, &worker,
                     [&](const AIAnalysisResult& result) { results.append(result); }, Qt::DirectConnection);
    SAIM_CHECK(worker.start());
    worker.invokeBlocking([&] {
        SensorSample sample;
        sample.deviceId = device.id; sample.temperature = 100; sample.voltage = 220;
        sample.current = 8; sample.vibration = 0.1; sample.rpm = 1500;
        for (int i = 0; i < 20; ++i)
        {
            sample.timestamp = QDateTime::currentDateTimeUtc().addSecs(i);
            worker.ingestSample(sample);
        }
        worker.setDevices({device});
        worker.analyseDeviceNow(device.id);
        worker.setDevices({});
        worker.ingestSample(sample);
        worker.analyseDeviceNow(device.id);
    });
    worker.stop();
    SAIM_CHECK(results.size() == 2);
    SAIM_CHECK(results[0].prediction.healthScore > results[1].prediction.healthScore);
    SAIM_CHECK(results[0].analysisMs >= 0 && results[1].analysisMs >= 0);
}

SAIM_TEST(typed_configuration_reads_are_independent_snapshots)
{
    ConfigManager config;
    config.load({});
    config.set(QStringLiteral("ai.windowSize"), 40);
    const auto& before = config.ai();
    config.set(QStringLiteral("ai.windowSize"), 60);
    SAIM_CHECK(before.windowSize == 40);
    std::atomic<bool> stop{false};
    std::atomic<bool> valid{true};
    std::thread reader([&] {
        while (!stop.load())
        {
            const auto snapshot = config.ai();
            if (snapshot.windowSize < 40 || snapshot.windowSize > 60) { valid = false; }
        }
    });
    for (int i = 0; i < 500; ++i) { config.set(QStringLiteral("ai.windowSize"), 40 + i % 21); }
    stop = true;
    reader.join();
    SAIM_CHECK(valid.load());
}

SAIM_TEST(report_generation_owns_its_input_snapshot)
{
    ConfigManager config;
    config.load({});
    config.set(QStringLiteral("ai.enableOnnx"), false);
    config.set(QStringLiteral("ai.enableVision"), false);
    AIWorker worker(&config);
    QVector<DeviceReportSection> sections;
    DeviceStatistics statistics;
    statistics.sampleCount = 3;
    const auto from = QDateTime::currentDateTimeUtc().addSecs(-2);
    const auto to = from.addSecs(2);
    for (const auto& id : {QStringLiteral("a"), QStringLiteral("b")})
    {
        sections.append(AIReportGenerator::buildSection(id, id, QStringLiteral("motor"),
            DeviceState::Normal, statistics, {}, 0, 0, {}));
    }
    ReportResult result;
    QObject::connect(&worker, &AIWorker::reportReady, &worker,
                     [&](const ReportResult& report) { result = report; }, Qt::DirectConnection);
    SAIM_CHECK(worker.start());
    worker.invokeAsync([&worker, sections, from, to] { worker.generateReport(sections, from, to, false); });
    sections.clear();
    worker.invokeBlocking([] {});
    worker.stop();
    SAIM_CHECK(result.deviceCount == 2);
    SAIM_CHECK(result.markdown.contains(QStringLiteral("Telemetry samples | 6")));
    SAIM_CHECK(!result.html.isEmpty());
}

int main()
{
    QCoreApplication app(saim::test::argcRef(), saim::test::argvRef());
    return saim::test::runAll();
}
