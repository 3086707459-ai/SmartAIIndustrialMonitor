#include "TestHarness.h"
#include "app/ApplicationController.h"
#include "core/ConfigManager.h"
#include "network/TcpServer.h"
#include "thread/ThreadedWorker.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <atomic>

using namespace saim;
namespace {
bool until(const std::function<bool()>& condition, int timeoutMs = 10000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeoutMs)
    { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); }
    return condition();
}
class Plant final : public ThreadedWorker
{
public:
    explicit Plant(int deviceCount = 3) : ThreadedWorker(QStringLiteral("overload-gateway")), m_count(deviceCount) {}
    ~Plant() override { stop(); }
    std::atomic<quint16> port{0};
    std::atomic<int> disconnected{0};
protected:
    void onStart() override
    {
        m_server = std::make_unique<TcpServer>();
        TcpServer::Options options;
        options.port = 0; options.listenAddress = QStringLiteral("127.0.0.1");
        options.sampleIntervalMs = 20; options.injectAnomalies = false;
        for (int i = 0; i < m_count; ++i) options.deviceIds.append(QStringLiteral("LOAD-%1").arg(i));
        m_server->setOptions(options);
        QObject::connect(m_server.get(), &TcpServer::started, m_server.get(), [this](quint16 value) { port = value; });
        QObject::connect(m_server.get(), &TcpServer::clientDisconnected, m_server.get(), [this] { ++disconnected; });
        m_server->start();
    }
    void onStop() override { m_server->stop(); m_server.reset(); }
private:
    int m_count;
    std::unique_ptr<TcpServer> m_server;
};
void configure(ConfigManager& config, quint16 port, const QString& database)
{
    config.load({});
    config.set(QStringLiteral("simulation.enabled"), false);
    config.set(QStringLiteral("network.client.remoteHost"), QStringLiteral("127.0.0.1"));
    config.set(QStringLiteral("network.client.remotePort"), port);
    config.set(QStringLiteral("database.filePath"), database);
    config.set(QStringLiteral("database.flushIntervalMs"), 20);
    config.set(QStringLiteral("database.batchSize"), 10);
    config.set(QStringLiteral("ai.enableOnnx"), false);
    config.set(QStringLiteral("ai.enableVision"), false);
    QJsonArray devices;
    for (int i = 0; i < 3; ++i)
        devices.append(QJsonObject{{"id", QStringLiteral("LOAD-%1").arg(i)}, {"name", "load"}, {"type", "motor"}});
    config.set(QStringLiteral("devices"), devices);
}
}

SAIM_TEST(real_tcp_sql_lock_rejects_explicitly_and_preserves_accounting)
{
    QTemporaryDir scratch; SAIM_CHECK(scratch.isValid());
    Plant plant; SAIM_CHECK(plant.start()); plant.invokeBlocking([] {});
    SAIM_CHECK(plant.port != 0);
    ConfigManager config;
    const auto path = scratch.filePath(QStringLiteral("overload.db"));
    configure(config, plant.port, path);
    config.set(QStringLiteral("pipeline.persistenceCapacity"), 32);
    ApplicationController controller(&config);
    SAIM_CHECK(controller.initialise().isOk()); controller.start();
    SAIM_CHECK(until([&] { return controller.pipelineStatistics().written >= 20; }));
    const auto connection = QStringLiteral("overload-readback");
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path); SAIM_CHECK(db.open());
        QSqlQuery query(db);
        SAIM_CHECK(query.exec(QStringLiteral("BEGIN IMMEDIATE")));
        const bool overflow = until([&] { return controller.pipelineStatistics().persistence.rejected > 10; }, 3000);
        SAIM_CHECK(query.exec(QStringLiteral("ROLLBACK")));
        SAIM_CHECK(overflow);
        SAIM_CHECK(until([&] { return controller.pipelineStatistics().written >= 50; }));
        controller.stop(); plant.stop();
        const auto s = controller.pipelineStatistics();
        SAIM_CHECK(s.persistence.highWater == 32 && s.persistence.depth == 0);
        SAIM_CHECK(s.persistence.waits > 0 && s.persistence.waitMs > 0);
        SAIM_CHECK(s.received == s.written + s.persistence.rejected + s.storageDropped);
        SAIM_CHECK(s.storageDropped == 0);
        SAIM_CHECK(query.exec(QStringLiteral("SELECT COUNT(*) FROM SensorData")) && query.next());
        SAIM_CHECK(query.value(0).toULongLong() == s.written);
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

SAIM_TEST(stalled_model_thread_does_not_accumulate_sample_events)
{
    QTemporaryDir scratch; SAIM_CHECK(scratch.isValid());
    Plant plant; SAIM_CHECK(plant.start()); plant.invokeBlocking([] {});
    ConfigManager config;
    configure(config, plant.port, scratch.filePath(QStringLiteral("ui-stall.db")));
    ApplicationController controller(&config);
    SAIM_CHECK(controller.initialise().isOk()); controller.start();
    SAIM_CHECK(until([&] { return controller.pipelineStatistics().written > 20; }));
    const auto before = controller.pipelineStatistics();
    // Gateway, acquisition and storage have separate event loops.
    QThread::msleep(1000);
    const auto during = controller.pipelineStatistics();
    SAIM_CHECK(during.received > before.received + 60 && during.written > before.written + 60);
    SAIM_CHECK(during.ui.depth == 3 && during.ui.highWater == 3);
    SAIM_CHECK(during.ui.coalesced > before.ui.coalesced + 60);
    controller.stop(); plant.stop();
    const auto after = controller.pipelineStatistics();
    SAIM_CHECK(after.written == after.received && after.ui.depth == 0);
    SAIM_CHECK(after.storageDropped == 0 && after.persistence.rejected == 0);
}

SAIM_TEST(slow_tcp_client_disconnect_does_not_invalidate_other_clients)
{
    Plant plant(512); SAIM_CHECK(plant.start()); plant.invokeBlocking([] {});
    QTcpSocket slow, fast;
    slow.setReadBufferSize(1);
    quint64 received = 0;
    QObject::connect(&fast, &QTcpSocket::readyRead, &fast, [&] { received += static_cast<quint64>(fast.readAll().size()); });
    slow.connectToHost(QHostAddress::LocalHost, plant.port);
    fast.connectToHost(QHostAddress::LocalHost, plant.port);
    SAIM_CHECK(until([&] { return received > 10000; }));
    SAIM_CHECK(until([&] { return plant.disconnected > 0; }, 20000));
    const auto before = received;
    SAIM_CHECK(until([&] { return received > before + 10000; }));
    SAIM_CHECK(fast.state() == QAbstractSocket::ConnectedState);
    fast.abort(); slow.abort(); plant.stop();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    return saim::test::runAll();
}
