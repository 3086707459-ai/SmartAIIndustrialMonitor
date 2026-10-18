#include "TestHarness.h"
#include "database/DataRepository.h"
#include <QCoreApplication>
#include <QTemporaryDir>

using namespace saim;

namespace {
void addDevice(DataRepository& repository)
{
    SAIM_CHECK(repository.upsertDevice(QStringLiteral("motor"), QStringLiteral("Motor"),
        QStringLiteral("motor"), QString(), 0, DeviceState::Normal, true).isOk());
}
SensorRecord record(int offset)
{
    SensorRecord r;
    r.deviceId = QStringLiteral("motor");
    r.timestamp = QDateTime::fromString(QStringLiteral("2023-11-14T22:13:20.000Z"), Qt::ISODateWithMs).addSecs(offset);
    r.temperature = offset; r.voltage = 220; r.current = 8; r.vibration = 0.2; r.rpm = 1500;
    return r;
}
}

SAIM_TEST(schema_reopens_without_losing_history)
{
    QTemporaryDir dir;
    SAIM_CHECK(dir.isValid());
    DatabaseManager db(QStringLiteral("reopen-test"));
    const auto path = dir.filePath(QStringLiteral("history.db"));
    SAIM_CHECK(db.open(path).isOk());
    DataRepository repository(&db);
    addDevice(repository);
    SAIM_CHECK(repository.insertSample(record(1)).isOk());
    const int version = db.schemaVersion();
    db.close();
    SAIM_CHECK(db.open(path).isOk());
    SAIM_CHECK(db.schemaVersion() == version);
    SAIM_CHECK(db.healthCheck().sensorRows == 1);
}

SAIM_TEST(batch_spans_sql_parameter_chunks_and_filters_history)
{
    DatabaseManager db(QStringLiteral("batch-test"));
    SAIM_CHECK(db.open(QStringLiteral(":memory:")).isOk());
    DataRepository repository(&db);
    addDevice(repository);
    QVector<SensorRecord> rows;
    for (int i = 0; i < 250; ++i) { rows.append(record(i)); }
    const auto inserted = repository.insertSamples(rows);
    SAIM_CHECK(inserted.isOk());
    SAIM_CHECK(inserted.value() == 250);
    HistoryQuery q;
    q.deviceId = QStringLiteral("motor");
    q.from = record(100).timestamp; q.to = record(109).timestamp;
    const auto history = repository.queryHistory(q);
    SAIM_CHECK(history.isOk());
    SAIM_CHECK(history.value().size() == 10);
    SAIM_CHECK_NEAR(history.value().front().temperature, 100.0, 1e-12);
}

SAIM_TEST(foreign_key_failure_rolls_back_all_chunks)
{
    DatabaseManager db(QStringLiteral("rollback-test"));
    SAIM_CHECK(db.open(QStringLiteral(":memory:")).isOk());
    DataRepository repository(&db);
    addDevice(repository);
    QVector<SensorRecord> rows;
    for (int i = 0; i < 130; ++i) { rows.append(record(i)); }
    rows.last().deviceId = QStringLiteral("unregistered-device");
    SAIM_CHECK(repository.insertSamples(rows).isError());
    SAIM_CHECK(db.healthCheck().sensorRows == 0);
}

SAIM_TEST(closed_database_reports_write_failure)
{
    DatabaseManager db(QStringLiteral("closed-test"));
    DataRepository repository(&db);
    SAIM_CHECK(repository.insertSample(record(1)).isError());
}

int main()
{
    QCoreApplication app(saim::test::argcRef(), saim::test::argvRef());
    return saim::test::runAll();
}
