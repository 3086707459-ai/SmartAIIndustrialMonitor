#include "TestHarness.h"
#include "thread/SampleInbox.h"
#include "thread/LatestMailbox.h"
#include "thread/RecordInbox.h"
#include "thread/PersistenceWorker.h"
#include "core/ConfigManager.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <atomic>
#include <future>
#include <thread>

using namespace saim;
namespace {
SensorSample sample(int sequence, const QString& id = QStringLiteral("motor"))
{
    SensorSample value;
    value.deviceId = id;
    value.timestamp = QDateTime::currentDateTimeUtc().addMSecs(sequence);
    value.temperature = sequence;
    value.voltage = 220; value.current = 8; value.vibration = 0.1; value.rpm = 1500;
    return value;
}
void conserved(const SampleInbox& inbox)
{
    const auto s = inbox.statistics();
    SAIM_CHECK(s.offered == s.taken + static_cast<quint64>(s.depth) + s.rejected
               + s.evicted + s.coalesced + s.cancelled);
    SAIM_CHECK(s.highWater <= s.capacity);
}
bool waitUntil(const std::function<bool()>& condition, int ms = 1000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < ms) QThread::msleep(1);
    return condition();
}
}

SAIM_TEST(full_storage_queue_rejects_with_deadline_and_preserves_fifo)
{
    SampleInbox inbox(2, SampleInbox::Policy::WaitThenReject, 5);
    SAIM_CHECK(inbox.push(sample(1)));
    SAIM_CHECK(inbox.push(sample(2)));
    QElapsedTimer clock; clock.start();
    SAIM_CHECK(!inbox.push(sample(3)));
    SAIM_CHECK(clock.elapsed() < 500);
    const auto values = inbox.take(10);
    SAIM_CHECK(values.size() == 2 && values[0].temperature == 1 && values[1].temperature == 2);
    SAIM_CHECK(inbox.statistics().waits == 1 && inbox.statistics().rejected == 1);
    conserved(inbox);
}

SAIM_TEST(waiting_producer_is_woken_by_consumer_and_close)
{
    SampleInbox inbox(1, SampleInbox::Policy::WaitThenReject, 50);
    inbox.push(sample(1));
    auto producer = std::async(std::launch::async, [&] { return inbox.push(sample(2)); });
    const bool waiting = waitUntil([&] { return inbox.statistics().waits == 1; });
    inbox.take(1);
    const bool accepted = producer.get();
    SAIM_CHECK(waiting && accepted);
    auto closingProducer = std::async(std::launch::async, [&] { return inbox.push(sample(3)); });
    const bool secondWaiting = waitUntil([&] { return inbox.statistics().waits == 2; });
    inbox.close();
    const bool acceptedAfterClose = closingProducer.get();
    SAIM_CHECK(secondWaiting && !acceptedAfterClose);
    inbox.discard();
    inbox.reopen();
    SAIM_CHECK(inbox.push(sample(4)));
    const auto values = inbox.take(10);
    SAIM_CHECK(values.size() == 1 && values[0].temperature == 4);
    conserved(inbox);
}

SAIM_TEST(ai_overflow_keeps_newest_samples_and_ui_coalesces_per_device)
{
    SampleInbox ai(2, SampleInbox::Policy::DropOldest);
    ai.push(sample(1)); ai.push(sample(2)); ai.push(sample(3));
    const auto analysis = ai.take(10);
    SAIM_CHECK(analysis.size() == 2 && analysis[0].temperature == 2 && analysis[1].temperature == 3);
    SAIM_CHECK(ai.statistics().evicted == 1);
    conserved(ai);
    SampleInbox ui(2, SampleInbox::Policy::LatestPerDevice);
    ui.push(sample(1, QStringLiteral("a")));
    ui.push(sample(2, QStringLiteral("b")));
    ui.push(sample(3, QStringLiteral("a")));
    SAIM_CHECK(ui.statistics().depth == 2 && ui.statistics().coalesced == 1);
    ui.push(sample(4, QStringLiteral("c")));
    const auto display = ui.take(10);
    SAIM_CHECK(display.size() == 2 && display[0].deviceId == QStringLiteral("b")
               && display[1].deviceId == QStringLiteral("c"));
    SAIM_CHECK(ui.statistics().evicted == 1);
    conserved(ui);
}

SAIM_TEST(concurrent_delivery_preserves_order_without_unbounded_events)
{
    SampleInbox inbox(16, SampleInbox::Policy::WaitThenReject, 50);
    std::atomic<bool> done{false};
    auto producer = std::async(std::launch::async, [&] {
        for (int i = 0; i < 2000; ++i) while (!inbox.push(sample(i))) {}
        done = true;
    });
    QVector<SensorSample> values;
    while (!done || inbox.statistics().depth != 0)
    {
        values.append(inbox.take(7));
        QThread::msleep(1);
    }
    producer.get();
    SAIM_CHECK(values.size() == 2000);
    for (int i = 0; i < values.size(); ++i) SAIM_CHECK(values[i].temperature == i);
    conserved(inbox);
}

SAIM_TEST(result_and_record_channels_are_bounded)
{
    LatestMailbox<int> results(2);
    results.put(QStringLiteral("a"), 1); results.put(QStringLiteral("a"), 2);
    results.put(QStringLiteral("b"), 3); results.put(QStringLiteral("c"), 4);
    SAIM_CHECK(results.replaced() == 1 && results.evicted() == 1);
    SAIM_CHECK(results.take(4) == QVector<int>({3, 4}));
    RecordInbox records;
    for (int i = 0; i < 2048; ++i) SAIM_CHECK(records.push(FaultPrediction{}));
    SAIM_CHECK(!records.push(AlarmRecord{}));
    records.close();
    SAIM_CHECK(!records.push(AlarmRecord{}));
    SAIM_CHECK(records.take(4096).size() == 2048);
    const auto stats = records.statistics();
    SAIM_CHECK(stats.rejected == 2 && stats.depth == 0 && stats.highWater == 2048);
}

SAIM_TEST(storage_drains_full_mailbox_and_restarts_without_stale_samples)
{
    QTemporaryDir scratch;
    SAIM_CHECK(scratch.isValid());
    ConfigManager config; config.load({});
    config.set(QStringLiteral("database.filePath"), scratch.filePath(QStringLiteral("restart.db")));
    config.set(QStringLiteral("pipeline.persistenceCapacity"), 8);
    PersistenceWorker worker(&config);
    DeviceConfiguration device; device.id = QStringLiteral("motor"); device.name = device.id;
    device.type = QStringLiteral("motor");
    worker.setDevices({device});
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        SAIM_CHECK(worker.start());
        worker.invokeBlocking([] {});
        // Execute the producer while the worker cannot run its drain timer.
        worker.invokeBlocking([&] {
            for (int i = 0; i < 12; ++i) worker.enqueueSample(sample(i));
        });
        worker.stop();
        SAIM_CHECK(worker.rowsWritten() == static_cast<quint64>((cycle + 1) * 8));
        const auto stats = worker.inboxStatistics();
        SAIM_CHECK(stats.depth == 0 && stats.rejected == static_cast<quint64>((cycle + 1) * 4));
        SAIM_CHECK(worker.droppedSamples() == 0);
    }
}

SAIM_TEST(locked_database_shutdown_counts_loss_without_retrying_every_batch)
{
    QTemporaryDir scratch;
    SAIM_CHECK(scratch.isValid());
    ConfigManager config; config.load({});
    const auto path = scratch.filePath(QStringLiteral("locked.db"));
    config.set(QStringLiteral("database.filePath"), path);
    config.set(QStringLiteral("database.batchSize"), 200);
    config.set(QStringLiteral("pipeline.persistenceCapacity"), 1024);
    PersistenceWorker worker(&config);
    DeviceConfiguration device; device.id = QStringLiteral("motor"); device.name = device.id;
    device.type = QStringLiteral("motor");
    worker.setDevices({device});
    SAIM_CHECK(worker.start());
    worker.invokeBlocking([] {});
    const auto connection = QStringLiteral("backpressure-lock");
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        SAIM_CHECK(db.open());
        QSqlQuery query(db);
        SAIM_CHECK(query.exec(QStringLiteral("SELECT COUNT(*) FROM Device")) && query.next());
        SAIM_CHECK(query.value(0).toInt() == 1);
        SAIM_CHECK(query.exec(QStringLiteral("BEGIN IMMEDIATE")));
        // Block timer execution until all 1024 entries have been enqueued.
        worker.invokeBlocking([&] { for (int i = 0; i < 1024; ++i) worker.enqueueSample(sample(i)); });
        QElapsedTimer elapsed; elapsed.start();
        worker.stop();
        // At most the active transaction plus one final attempt, not 6 batches
        // times the busy timeout. Allow scheduling margin on loaded hosts.
        SAIM_CHECK(elapsed.elapsed() < 13000);
        SAIM_CHECK(worker.inboxStatistics().depth == 0);
        SAIM_CHECK(worker.rowsWritten() == 0 && worker.droppedSamples() == 1024);
        SAIM_CHECK(query.exec(QStringLiteral("ROLLBACK")));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

SAIM_TEST(fresh_database_stays_in_explicit_data_root)
{
    QTemporaryDir parent;
    SAIM_CHECK(parent.isValid());
    const auto root = parent.filePath(QStringLiteral("chosen-root"));
    SAIM_CHECK(QDir().mkpath(root));
    ConfigManager config; config.load({}); config.setDataRoot(root);
    config.set(QStringLiteral("database.filePath"), QStringLiteral("fresh.db"));
    SAIM_CHECK(config.resolveDataPath(QStringLiteral("fresh.db")) == root + QStringLiteral("/fresh.db"));
    SAIM_CHECK(config.resolveDataPath(QStringLiteral(":memory:")) == QStringLiteral(":memory:"));
    PersistenceWorker worker(&config);
    SAIM_CHECK(worker.start()); worker.invokeBlocking([] {}); worker.stop();
    SAIM_CHECK(QFileInfo::exists(root + QStringLiteral("/fresh.db")));
    SAIM_CHECK(!QFileInfo::exists(parent.filePath(QStringLiteral("fresh.db"))));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    return saim::test::runAll();
}
