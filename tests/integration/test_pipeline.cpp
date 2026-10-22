// ---------------------------------------------------------------------------
// test_pipeline.cpp - one sample, end to end, through the real stack.
//
// What this test is for
// ---------------------
// Every unit test in this suite proves that one component is correct in
// isolation.  None of them prove that the components are *connected* to each
// other.  Wiring is where this kind of application actually breaks: a signal
// connected to the wrong slot, a payload type never registered with the
// meta-object system, a timer that never starts, a worker started after its
// producer.  Each of those compiles, each passes every unit test, and each
// produces a system that runs and does nothing.
//
// So this test links saim::app - the entire stack minus main() - and drives it
// against the real in-process gateway, over a real loopback socket, through the
// real worker threads, into a real SQLite file.
//
// The layers it deliberately does NOT exercise
// --------------------------------------------
// MainWindow and every widget under it.  buildUi() is never called, so no
// QApplication is needed, no platform plugin is loaded, and this test runs on a
// headless CI machine with no display.  The widget layer has its own target;
// dragging it in here would make this test fail for reasons that have nothing
// to do with the pipeline.
//
// A note on the waiting
// ---------------------
// The assertions poll with a deadline rather than sleeping for a fixed time.  A
// fixed sleep is either too short (flaky on a loaded machine) or too long (every
// run pays for the worst case).  `waitFor` returns as soon as the condition
// holds and fails with the condition spelled out when it never does, which is
// what makes a failure here readable instead of mysterious.
// ---------------------------------------------------------------------------

#include "TestHarness.h"

#include "app/ApplicationController.h"
#include "common/Types.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "device/Device.h"
#include "device/DeviceManager.h"
#include "thread/AIWorker.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <memory>

using namespace saim;

namespace {

/// A per-run scratch tree.  QTemporaryDir removes itself in its destructor, so a
/// failing test does not leave a database behind for the next run to trip over -
/// which is how a suite starts failing for reasons nobody can reproduce.
std::unique_ptr<QTemporaryDir>& scratchOwnership()
{
    static std::unique_ptr<QTemporaryDir> dir;
    return dir;
}

QString& scratchRoot()
{
    static QString path;
    return path;
}

/// Ports come from a small scratch pool rather than being fixed, so that a
/// developer with the application already running does not get a spurious
/// "address in use" failure.  Tests that must share a port pass one explicitly.
quint16 nextPort()
{
    static quint16 port = 19310;
    return port++;
}

/// Pumps the event loop until `predicate` holds, or fails the test.
///
/// processEvents rather than QEventLoop::exec: the pipeline delivers across four
/// threads, and a nested event loop exited only by its own timer cannot observe
/// a condition that becomes true between two queued deliveries from elsewhere.
void waitFor(const std::function<bool()>& predicate, const char* what,
             int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();

    while (!predicate())
    {
        if (timer.elapsed() > timeoutMs)
        {
            throw ::saim::test::Failure{
                std::string("timed out after ") + std::to_string(timeoutMs)
                    + " ms waiting for: " + what,
                __FILE__, __LINE__};
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

/// Builds a ConfigManager pointing at a throwaway config tree.
///
/// The configuration is written as JSON to disk rather than driven in through
/// ConfigManager::set(), because the point is to exercise the path a real
/// deployment takes: a file, parsed at start-up, with env overrides on top.
std::unique_ptr<ConfigManager> makeConfig(quint16 port, bool withDevices)
{
    const QString configDir = scratchRoot() + QStringLiteral("/config");
    QDir().mkpath(configDir);

    const QString devicesJson = withDevices
        ? QStringLiteral(R"JSON(
  "devices": [
    { "id": "MOTOR-001", "name": "Main Drive Motor", "type": "motor",
      "ip": "127.0.0.1", "port": %1,
      "limits": { "temperatureWarn": 70, "temperatureHigh": 85,
                  "currentWarn": 15, "currentHigh": 20,
                  "vibrationWarn": 0.45, "vibrationHigh": 0.80 } },
    { "id": "PUMP-001", "name": "Coolant Pump", "type": "pump",
      "ip": "127.0.0.1", "port": %1,
      "limits": { "temperatureWarn": 65, "temperatureHigh": 80,
                  "currentWarn": 12, "currentHigh": 16,
                  "vibrationWarn": 0.40, "vibrationHigh": 0.70 } }
  ],)JSON").arg(port)
        : QString();

    const QString json = QStringLiteral(R"JSON(
{
  %1
  "logging": { "level": "warning", "directory": "%2/logs", "console": false },
  "network": {
    "server": { "listenAddress": "127.0.0.1", "listenPort": %3 },
    "client": { "remoteHost": "127.0.0.1", "remotePort": %3,
                "autoConnect": true, "heartbeatMs": 5000 },
    "reconnect": { "baseMs": 100, "maxMs": 500 }
  },
  "database": { "filePath": "%2/data/it.db", "retentionDays": 30,
                "flushIntervalMs": 100, "batchSize": 50 },
  "ai": { "modelDirectory": "%2/models", "enableOnnx": false, "enableVision": false,
          "windowSize": 20, "inferenceHz": 5, "anomalyThreshold": 0.35 },
  "alarm": { "offlineTimeoutMs": 15000, "repeatSuppressionMs": 30000 },
  "simulation": { "enabled": true, "deviceCount": 2,
                  "sampleIntervalMs": 40, "injectAnomalies": false,
                  "deviceIds": ["MOTOR-001", "PUMP-001"] }
}
)JSON")
                             .arg(devicesJson)
                             .arg(scratchRoot())
                             .arg(port);

    const QString configFile = configDir + QStringLiteral("/config.json");
    QFile file(configFile);
    SAIM_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(json.toUtf8());
    file.close();

    auto config = std::make_unique<ConfigManager>();
    // load() with an explicit list *replaces* the default search chain, so the
    // test can never accidentally pick up the developer's own
    // ../config/config.json and assert against the wrong fleet.
    config->load({configFile});
    return config;
}

/// A controller plus the configuration it owns.
///
/// The destructor order is the point: the controller must be torn down - which
/// joins the worker threads - before the ConfigManager it reads thresholds from
/// goes away.  Member declaration order already gives that, and the explicit
/// stop() makes it survivable even if someone reorders the members later.
struct Rig
{
    std::unique_ptr<ConfigManager>         config;
    std::unique_ptr<ApplicationController> controller;

    ~Rig()
    {
        if (controller)
        {
            controller->stop();
        }
    }

    [[nodiscard]] DeviceManager* devices() const { return controller->devices(); }
    [[nodiscard]] std::shared_ptr<Device> device(const QString& id) const
    {
        return devices()->device(id);
    }
};

/// Opens a second, independent connection to the integration database.
///
/// Deliberately a *different* connection name: Qt keys connections globally by
/// name, and the persistence worker already holds one for the same file.  Using
/// the worker's connection from the test thread would be reading a Qt object
/// across threads, which QSqlDatabase is explicitly not safe for.
class Readback
{
public:
    Readback(const QString& path, const QString& name)
        : m_name(name)
    {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_db.setDatabaseName(path);
        m_open = m_db.open();
    }

    ~Readback()
    {
        if (m_db.isOpen())
        {
            m_db.close();
        }
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }

    Readback(const Readback&)            = delete;
    Readback& operator=(const Readback&) = delete;

    [[nodiscard]] bool isOpen() const { return m_open; }

    [[nodiscard]] qint64 scalar(const QString& sql) const
    {
        if (!m_open)
        {
            return -1;
        }
        QSqlQuery query(m_db);
        if (!query.exec(sql) || !query.next())
        {
            return -1;
        }
        return query.value(0).toLongLong();
    }

    [[nodiscard]] QStringList column(const QString& sql) const
    {
        QStringList values;
        if (!m_open)
        {
            return values;
        }
        QSqlQuery query(m_db);
        if (!query.exec(sql))
        {
            return values;
        }
        while (query.next())
        {
            values << query.value(0).toString();
        }
        return values;
    }

private:
    QString       m_name;
    QSqlDatabase  m_db;
    bool          m_open = false;
};

}   // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

SAIM_TEST(config_is_read_from_the_search_path)
{
    const quint16 port = nextPort();
    const std::unique_ptr<ConfigManager> config = makeConfig(port, true);

    SAIM_CHECK(config->network().listenPort == port);
    SAIM_CHECK(config->network().remotePort == port);
    SAIM_CHECK(config->simulation().sampleIntervalMs == 40);
    SAIM_CHECK(config->alarms().offlineTimeoutMs == 15000);

    // The devices array survives the trip through JSON unmodified - including
    // the per-device limits, which are the exact numbers the rule engine will
    // compare against.
    const QJsonArray devices = config->getArray(QStringLiteral("devices"));
    SAIM_CHECK(devices.size() == 2);
    SAIM_CHECK(devices.at(0).toObject().value(QStringLiteral("id")).toString()
               == QStringLiteral("MOTOR-001"));
    SAIM_CHECK(devices.at(0).toObject().value(QStringLiteral("limits"))
                   .toObject().value(QStringLiteral("temperatureHigh")).toDouble()
               == 85.0);
}

SAIM_TEST(an_environment_override_beats_the_file)
{
    // The precedence chain is what lets an engineer change a threshold on the
    // shop floor without editing the shipped configuration.  Asserted explicitly
    // because it is the *later* source that must win, and a merge written the
    // wrong way round passes every test that only ever writes one file.
    const quint16 port = nextPort();
    const std::unique_ptr<ConfigManager> config = makeConfig(port, true);

    qputenv("SAIM_ALARM__OFFLINETIMEOUTMS", "4321");
    config->load({scratchRoot() + QStringLiteral("/config/config.json")});
    qunsetenv("SAIM_ALARM__OFFLINETIMEOUTMS");

    SAIM_CHECK(config->alarms().offlineTimeoutMs == 4321);
}

// ---------------------------------------------------------------------------
// The fleet
// ---------------------------------------------------------------------------

SAIM_TEST(the_device_registry_reconciles_rather_than_rebuilds)
{
    // A config reload must not throw away what a machine has accumulated.  A
    // device that already exists keeps its history, its counters and its state;
    // only metadata changes.  Rebuilding the fleet on every reload would blank
    // the charts and reset every counter, which on a hot-reloaded deployment
    // looks exactly like a crash.
    const quint16 port = nextPort();
    Rig rig;
    rig.config     = makeConfig(port, true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());

    SAIM_CHECK(rig.devices()->deviceCount() == 2);

    const std::shared_ptr<Device> motor = rig.device(QStringLiteral("MOTOR-001"));
    SAIM_CHECK(motor != nullptr);
    SAIM_CHECK(motor->deviceName() == QStringLiteral("Main Drive Motor"));

    SensorSample sample;
    sample.deviceId    = QStringLiteral("MOTOR-001");
    sample.timestamp   = QDateTime::currentDateTime();
    sample.temperature = 45.0;
    sample.voltage     = 220.0;
    sample.current     = 8.0;
    sample.vibration   = 0.10;
    motor->onSampleReceived(sample);
    SAIM_CHECK(motor->sampleCount() == 1);

    rig.devices()->loadFromConfig();

    SAIM_CHECK(rig.devices()->deviceCount() == 2);

    const std::shared_ptr<Device> reloaded = rig.device(QStringLiteral("MOTOR-001"));
    SAIM_CHECK(reloaded != nullptr);

    // The same object, not a new one with the same name - and the sample it
    // received before the reload is still in its history.
    SAIM_CHECK(reloaded.get() == motor.get());
    SAIM_CHECK(reloaded->sampleCount() == 1);
}

// ---------------------------------------------------------------------------
// The data path
// ---------------------------------------------------------------------------

SAIM_TEST(samples_travel_from_the_gateway_to_the_registry)
{
    // The single most valuable assertion in the suite.  It fails if the socket
    // never connected, if the frame codec is wrong, if a worker was never
    // started, if a signal is connected to the wrong slot, or if SensorSample
    // was never registered as a metatype.  Every one of those is invisible to a
    // unit test and fatal in production.
    const quint16 port = nextPort();
    Rig rig;
    rig.config     = makeConfig(port, true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());

    const std::shared_ptr<Device> motor = rig.device(QStringLiteral("MOTOR-001"));
    SAIM_CHECK(motor != nullptr);

    rig.controller->start();
    waitFor([&motor] { return motor->sampleCount() > 0; },
            "the first sample to reach MOTOR-001");

    SAIM_CHECK(motor->latest().deviceId == QStringLiteral("MOTOR-001"));
    SAIM_CHECK(motor->latest().valid);

    // Every channel lands in a physically sane band.  A decoder that swapped two
    // fields - temperature parsed as voltage, say - would still produce "a
    // sample arrived" and fails here instead.
    const SensorSample s = motor->latest();
    SAIM_CHECK(s.temperature > -40.0 && s.temperature < 200.0);
    SAIM_CHECK(s.voltage > 0.0 && s.voltage < 500.0);
    SAIM_CHECK(s.current >= 0.0 && s.current < 100.0);
    SAIM_CHECK(s.vibration >= 0.0 && s.vibration < 10.0);
    SAIM_CHECK(s.rpm > 0.0 && s.rpm < 10000.0);

    // The second device receives its own stream rather than a copy of the
    // first's - which is what proves the routing is by device id and not by
    // position in the list.
    const std::shared_ptr<Device> pump = rig.device(QStringLiteral("PUMP-001"));
    SAIM_CHECK(pump != nullptr);
    waitFor([&pump] { return pump->sampleCount() > 0; }, "a sample to reach PUMP-001");

    SAIM_CHECK(pump->latest().deviceId == QStringLiteral("PUMP-001"));

    // Both devices advance together: a stream that only ever fed the first
    // entry in the fleet would pass everything above.
    waitFor([&motor, &pump] { return motor->sampleCount() >= 3 && pump->sampleCount() >= 3; },
            "both devices to keep receiving");
}

SAIM_TEST(the_device_moves_into_normal_once_telemetry_arrives)
{
    // Offline -> Normal is the transition the whole interface hangs off: the
    // list row, the dashboard tile, the status indicator and the alarm counter
    // all key on statusChanged.  If it never fires, every panel shows a
    // dead-looking grey while the application is working perfectly.
    const quint16 port = nextPort();
    Rig rig;
    rig.config     = makeConfig(port, true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());

    const std::shared_ptr<Device> motor = rig.device(QStringLiteral("MOTOR-001"));
    SAIM_CHECK(motor->state() == DeviceState::Offline);

    rig.controller->start();
    waitFor([&motor] { return motor->state() == DeviceState::Normal; },
            "MOTOR-001 to come online");

    SAIM_CHECK(motor->state() == DeviceState::Normal);
    SAIM_CHECK(motor->lastSeen().isValid());

    // A device that has never reported is *unknown*, not healthy.  Asserted on
    // the second device while it is quiet, so this pins the ordering rule as
    // well as the transition.
    const std::shared_ptr<Device> pump = rig.device(QStringLiteral("PUMP-001"));
    waitFor([&pump] { return pump->state() != DeviceState::Offline; },
            "PUMP-001 to come online");
}

// ---------------------------------------------------------------------------
// The AI stage
// ---------------------------------------------------------------------------

SAIM_TEST(the_prediction_backend_is_honest_about_which_model_answered)
{
    // The scratch configuration sets enableOnnx=false and puts no model file in
    // the tree, so no neural network is in play - and the application must say
    // so rather than displaying "AI".  A system that shows a number without
    // recording which implementation produced it is a system whose numbers
    // cannot be audited, and this test is what stops a future change from
    // quietly relabelling the fallback backend.
    const quint16 port = nextPort();
    Rig rig;
    rig.config     = makeConfig(port, true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());

    // Constructed here rather than reached through the controller: nothing
    // outside the composition root may touch the worker pool, and the test
    // respects that by building the one worker it wants to interrogate.  No
    // thread is started, so this is just the object and its status strings.
    auto ai = std::make_unique<AIWorker>(rig.config.get());

    SAIM_CHECK(!ai->hasNeuralModel());
    SAIM_CHECK(!ai->predictorBackend().isEmpty());
    SAIM_CHECK(ai->predictorBackend() != QStringLiteral("onnxruntime"));

    // Vision was switched off in this config, so the worker must not claim a
    // camera it was told not to use.
    SAIM_CHECK(!ai->visionAvailable());
}

SAIM_TEST(analysis_runs_on_the_live_stream_and_produces_a_verdict)
{
    // The AI stage consumes the same socket stream the registry does.  This is
    // the connection that is easiest to get wrong - ingestSample is queued onto
    // another thread and a missed connection looks exactly like a model that has
    // nothing to say.
    const quint16 port = nextPort();
    Rig rig;
    rig.config     = makeConfig(port, true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());
    rig.controller->start();

    const std::shared_ptr<Device> motor = rig.device(QStringLiteral("MOTOR-001"));

    // The window is 20 samples at 5 Hz in this configuration, so a verdict is
    // due shortly after the device is warm.  Waiting on the registry first means
    // a failure here is about the AI path, not about the socket.
    waitFor([&motor] { return motor->sampleCount() >= 20; },
            "the AI window to fill");

    waitFor([&motor] { return !motor->prediction().faultType.isEmpty(); },
            "an AI verdict to reach the device");

    const FaultPrediction prediction = motor->prediction();
    SAIM_CHECK(!prediction.faultType.isEmpty());
    SAIM_CHECK(prediction.deviceId == QStringLiteral("MOTOR-001"));
    SAIM_CHECK(!prediction.backend.isEmpty());

    // healthScore is a percentage, and a value outside it means the two
    // backends disagree about their units - which the UI would render as a
    // gauge needle off its own scale.
    SAIM_CHECK(prediction.healthScore >= 0.0 && prediction.healthScore <= 100.0);
    SAIM_CHECK(prediction.failureProbability >= 0.0 && prediction.failureProbability <= 1.0);
    SAIM_CHECK(prediction.confidence >= 0.0 && prediction.confidence <= 1.0);

    // The verdict was routed to the right device, and the registry escalated
    // from it - the two halves of the "worst of rules and model" rule.
    SAIM_CHECK(motor->state() != DeviceState::Offline);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

SAIM_TEST(the_database_is_created_migrated_and_written)
{
    // Persistence is the subsystem that fails *silently* in the field: no socket
    // error, no exception, just a file that stops growing.  So the assertions
    // here are about the file's contents, not about a status code.
    const quint16 port = nextPort();
    Rig rig;
    rig.config     = makeConfig(port, true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());
    rig.controller->start();

    const QString dbPath = scratchRoot() + QStringLiteral("/data/it.db");
    waitFor([&dbPath] { return QFile::exists(dbPath); }, "the database file to appear");

    const std::shared_ptr<Device> motor = rig.device(QStringLiteral("MOTOR-001"));
    waitFor([&motor] { return motor->sampleCount() >= 10; }, "samples to accumulate");

    // A connection of the test's own - see Readback's comment on why it must not
    // borrow the worker's.
    const auto rows = [&dbPath] {
        Readback readback(dbPath, QStringLiteral("it_rows"));
        return readback.scalar(QStringLiteral("SELECT COUNT(*) FROM SensorData"));
    };

    // The batcher commits on an interval (100 ms here) and on batch size, so the
    // wait is on the rows appearing rather than on the clock.
    waitFor([&rows] { return rows() > 0; }, "rows to be committed to SensorData");

    SAIM_CHECK(rows() > 0);

    {
        Readback readback(dbPath, QStringLiteral("it_schema"));
        SAIM_CHECK(readback.isOpen());

        // The schema was applied, not merely the file touched.  A connection
        // that opens an empty file and never migrates produces a file, a WAL and
        // no tables - which looks identical to success from the outside.
        const QStringList tables = readback.column(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"));
        SAIM_CHECK(tables.contains(QStringLiteral("SensorData")));
        SAIM_CHECK(tables.contains(QStringLiteral("Alarm")));
        SAIM_CHECK(tables.contains(QStringLiteral("Device")));

        // user_version is how the migration chain knows where it stopped; a
        // database left at 0 would re-run migration 1 on every start-up.
        SAIM_CHECK(readback.scalar(QStringLiteral("PRAGMA user_version")) > 0);
    }

    // The sample count only ever moves forward.  A retention pass that ran with
    // a zero window, or a schema that dropped the table under load, would show
    // up as a count that resets - the failure that makes a historian useless.
    const qint64 before = rows();
    QThread::msleep(250);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    SAIM_CHECK(rows() >= before);
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

SAIM_TEST(shutdown_is_clean_and_idempotent)
{
    // The teardown path that matters is "stop twice": once from the window's
    // closeEvent, once from the controller's destructor.  A second stop that
    // touches a half-dismantled pool is a crash on exit - intermittent, only on
    // some machines, and the hardest bug in a project like this to reproduce.
    const quint16 port = nextPort();
    Rig rig;
    rig.config     = makeConfig(port, true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());
    rig.controller->start();

    const std::shared_ptr<Device> motor = rig.device(QStringLiteral("MOTOR-001"));
    waitFor([&motor] { return motor->sampleCount() > 0; }, "acquisition to be running");

    // stop() is documented idempotent; calling it twice is the contract.
    rig.controller->stop();
    rig.controller->stop();

    // After stopping, nothing new may arrive.  The counter is sampled, the event
    // loop pumped to drain anything already queued, then sampled again - so this
    // cannot pass merely because the loop was quiet when it was read.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
    const qint64 afterStop = motor->sampleCount();

    QThread::msleep(200);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 300);

    SAIM_CHECK(motor->sampleCount() == afterStop);
}

SAIM_TEST(a_second_controller_can_reuse_the_same_port)
{
    // The listening socket must genuinely be released.  A QObject deleted
    // without closing its QTcpServer leaves the port bound on some platforms,
    // and a suite that runs rig-after-rig then fails on the second one for a
    // reason that has nothing to do with the code under test.
    const quint16 port = nextPort();

    {
        Rig first;
        first.config     = makeConfig(port, true);
        first.controller = std::make_unique<ApplicationController>(first.config.get());
        SAIM_CHECK(first.controller->initialise().isOk());
        first.controller->start();

        const std::shared_ptr<Device> motor = first.device(QStringLiteral("MOTOR-001"));
        waitFor([&motor] { return motor->sampleCount() > 0; }, "the first rig to receive data");
    }

    // Same port, fresh objects.  If the first rig leaked the listener, this
    // start() cannot bind and no sample ever arrives.
    Rig second;
    second.config     = std::make_unique<ConfigManager>();
    second.config->load({scratchRoot() + QStringLiteral("/config/config.json")});
    second.controller = std::make_unique<ApplicationController>(second.config.get());
    SAIM_CHECK(second.controller->initialise().isOk());
    second.controller->start();

    const std::shared_ptr<Device> motor = second.device(QStringLiteral("MOTOR-001"));
    waitFor([&motor] { return motor->sampleCount() > 0; },
            "the second rig to bind the same port and receive data");
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

SAIM_TEST(live_configuration_updates_states_and_registers_new_devices)
{
    Rig rig;
    rig.config = makeConfig(nextPort(), true);
    const QString dbPath = scratchRoot() + QStringLiteral("/data/reload.db");
    rig.config->set(QStringLiteral("database.filePath"), dbPath);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());
    rig.controller->start();
    const auto count = [&dbPath](const QString& condition) {
        Readback db(dbPath, QStringLiteral("reload-readback"));
        return db.scalar(QStringLiteral("SELECT COUNT(*) FROM SensorData WHERE ") + condition);
    };
    waitFor([&] { return count(QStringLiteral("state='NORMAL'")) > 0; }, "initial normal rows");
    auto devices = rig.config->getArray(QStringLiteral("devices"));
    auto motor = devices[0].toObject();
    auto limits = motor.value(QStringLiteral("limits")).toObject();
    limits.insert(QStringLiteral("temperatureWarn"), 5);
    limits.insert(QStringLiteral("temperatureHigh"), 10);
    motor.insert(QStringLiteral("limits"), limits);
    devices[0] = motor;
    rig.config->set(QStringLiteral("devices"), devices);
    waitFor([&] { return count(QStringLiteral("device_id='MOTOR-001' AND state='FAULT'")) > 0; },
            "fault rows after live threshold change");
    motor.insert(QStringLiteral("id"), QStringLiteral("NEW-001"));
    rig.config->set(QStringLiteral("devices"), QJsonArray{motor});
    waitFor([&] { return count(QStringLiteral("device_id='NEW-001'")) > 0; },
            "new device samples and foreign-key parent after reload");
    SAIM_CHECK(!rig.devices()->contains(QStringLiteral("MOTOR-001")));
    SAIM_CHECK(rig.devices()->contains(QStringLiteral("NEW-001")));
    rig.config->set(QStringLiteral("devices"), QJsonArray{});
    SAIM_CHECK(rig.devices()->deviceCount() == 0);
    rig.controller->stop();
}

SAIM_TEST(controller_applies_complete_analysis_without_reviving_offline_devices)
{
    Rig rig;
    rig.config = makeConfig(nextPort(), true);
    rig.controller = std::make_unique<ApplicationController>(rig.config.get());
    SAIM_CHECK(rig.controller->initialise().isOk());
    const auto motor = rig.device(QStringLiteral("MOTOR-001"));
    SensorSample sample;
    sample.deviceId = motor->deviceId();
    sample.timestamp = QDateTime::currentDateTime();
    sample.temperature = 40; sample.voltage = 220; sample.current = 8;
    sample.vibration = 0.1; sample.valid = true;
    motor->onSampleReceived(sample);
    AIAnalysisResult result;
    result.prediction.deviceId = result.anomaly.deviceId = motor->deviceId();
    result.prediction.timestamp = result.anomaly.timestamp = sample.timestamp;
    result.prediction.faultType = QStringLiteral("Bearing Failure");
    result.prediction.healthScore = 25;
    result.prediction.failureProbability = 0.9;
    const auto deliver = [&] {
        SAIM_CHECK(QMetaObject::invokeMethod(rig.controller.get(), "onAnalysisReady",
            Qt::DirectConnection, Q_ARG(saim::AIAnalysisResult, result)));
    };
    deliver();
    SAIM_CHECK(motor->state() == DeviceState::Fault);
    int changes = 0;
    QObject observer;
    QObject::connect(motor.get(), &Device::statusChanged, &observer, [&] { ++changes; });
    const auto alarms = motor->alarmCount();
    result.prediction.healthScore = 100;
    result.prediction.failureProbability = 0;
    result.anomaly.level = AnomalyLevel::High;
    deliver();
    SAIM_CHECK(changes == 0 && motor->alarmCount() == alarms);
    motor->checkLiveness(0);
    const auto offlineAlarms = motor->alarmCount();
    deliver();
    SAIM_CHECK(motor->state() == DeviceState::Offline);
    SAIM_CHECK(motor->alarmCount() == offlineAlarms);
    SAIM_CHECK(motor->prediction().faultType.isEmpty());
}

int main()
{
    // QCoreApplication, not QGuiApplication: the pipeline has no widgets, and
    // asking for a platform plugin would make this test require a display.
    QCoreApplication app(::saim::test::argcRef(), saim::test::argvRef());
    QCoreApplication::setOrganizationName(QStringLiteral("SAIM-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SmartAIIndustrialMonitor-IT"));

    // One scratch tree per process, so two suites running in parallel on CI
    // cannot collide on a database file.
    scratchOwnership() = std::make_unique<QTemporaryDir>(
        QDir::temp().filePath(QStringLiteral("saim_it_XXXXXX")));
    if (!scratchOwnership()->isValid())
    {
        QTextStream(stdout) << "cannot create a scratch directory; aborting" << Qt::endl;
        return 1;
    }
    scratchRoot() = QDir::cleanPath(scratchOwnership()->path());

    // Quiet by default: the scratch config logs at warning level and the file
    // sink is off, so a passing run prints its results and nothing else.
    // SAIM_IT_VERBOSE=1 turns the console sink back on when something needs
    // diagnosing.
    Logger::Options options;
    options.minimumLevel   = LogLevel::Warning;
    options.consoleEnabled = qEnvironmentVariableIsSet("SAIM_IT_VERBOSE");
    options.logDirectory   = QString();
    options.recentCapacity = 500;
    Logger::instance().configure(options);

    const int result = saim::test::runAll();

    Logger::instance().flushSinks();
    return result;
}
