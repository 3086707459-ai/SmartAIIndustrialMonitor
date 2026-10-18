// ---------------------------------------------------------------------------
// main.cpp - the standalone FakeDeviceServer.
//
// This is the same TcpServer the application embeds, exposed as a console tool.
// It exists for two situations the embedded one cannot cover:
//
//   * the plant and the monitor on two machines - which is the realistic
//     deployment, and the only way to demonstrate that the client actually
//     reconnects when the link drops, rather than merely that its state machine
//     says it would;
//   * a long-running feed with no interface attached, so that a database can be
//     accumulated for the report generator without leaving a window open.
//
// It deliberately does NOT link the UI or the workers.  A gateway that could
// paint a widget would be a gateway with opinions about presentation.
// ---------------------------------------------------------------------------

#include "core/ConfigManager.h"
#include <csignal>
#include "core/Logger.h"
#include "device/DeviceSimulator.h"
#include "network/TcpServer.h"

#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

namespace {

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

/// Ctrl+C has to be handled by hand in a Qt console application: an event loop
/// with no widget does not get a default handler, so without this the process
/// ignores the interrupt until it is killed, and the last log lines are lost.
/// The handler must be async-signal-safe, so it does nothing but set a flag and
/// let a timer do the work.
volatile std::sig_atomic_t g_interrupted = 0;

extern "C" void onSignal(int)
{
    g_interrupted = 1;
}

}   // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SmartAI"));
    QCoreApplication::setApplicationName(QStringLiteral("SmartAISimulator"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // ---- command line -----------------------------------------------------
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("FakeDeviceServer: streams simulated industrial telemetry over TCP."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOption({QStringLiteral("p"), QStringLiteral("port")},
                                  QStringLiteral("Listen on <port>. Default: 9100."),
                                  QStringLiteral("port"));
    QCommandLineOption intervalOption({QStringLiteral("i"), QStringLiteral("interval")},
                                      QStringLiteral("Sample interval in ms. Default: 500."),
                                      QStringLiteral("ms"));
    QCommandLineOption devicesOption({QStringLiteral("n"), QStringLiteral("devices")},
                                     QStringLiteral("Number of simulated devices (1-32). "
                                                    "Default: from config, else 6."),
                                     QStringLiteral("count"));
    QCommandLineOption faultOption(QStringLiteral("fault"),
                                   QStringLiteral("Fault <deviceId> after <seconds> seconds. "
                                                  "Repeatable."),
                                   QStringLiteral("deviceId:seconds"));
    QCommandLineOption wearOption(QStringLiteral("wear"),
                                  QStringLiteral("Degrade <deviceId> steadily to end of life "
                                                 "over the run. Repeatable."),
                                  QStringLiteral("deviceId"));
    QCommandLineOption quietOption(QStringLiteral("quiet"), QStringLiteral("Log warnings only."));
    QCommandLineOption burstOption(QStringLiteral("burst"),
                                   QStringLiteral("Emit a burst of <count> frames per device "
                                                  "every second, to exercise throughput."),
                                   QStringLiteral("count"));
    parser.addOption(portOption);
    parser.addOption(intervalOption);
    parser.addOption(devicesOption);
    parser.addOption(faultOption);
    parser.addOption(wearOption);
    parser.addOption(quietOption);
    parser.addOption(burstOption);
    parser.process(app);

    // ---- configuration ----------------------------------------------------
    saim::ConfigManager config;
    config.load();

    saim::Logger::Options logOptions;
    logOptions.minimumLevel   = parser.isSet(quietOption) ? saim::LogLevel::Warning
                                                          : saim::LogLevel::Info;
    logOptions.consoleEnabled = true;
    logOptions.logDirectory   = config.resolveDataPath(QStringLiteral("logs"));
    logOptions.fileName       = QStringLiteral("simulator.log");
    saim::Logger::instance().configure(logOptions);
    saim::Logger::instance().installQtMessageHandler();

    // ---- the fleet --------------------------------------------------------
    saim::TcpServer::Options options;
    options.listenAddress      = QStringLiteral("0.0.0.0");
    options.port               = static_cast<quint16>(
        parser.isSet(portOption) ? parser.value(portOption).toUShort() : config.network().listenPort);
    options.sampleIntervalMs   = parser.isSet(intervalOption)
                                     ? parser.value(intervalOption).toInt()
                                     : config.simulation().sampleIntervalMs;
    options.injectAnomalies    = config.simulation().injectAnomalies;
    options.anomalyRatePercent = config.simulation().anomalyRatePercent;

    // The device list defaults to the same one the monitor's config.json holds,
    // so the two halves of a two-machine setup describe the same plant without
    // anyone having to keep two lists in step.  -n overrides it with a generated
    // line, which is what to use when the point is load rather than realism.
    if (parser.isSet(devicesOption))
    {
        const int count = qBound(1, parser.value(devicesOption).toInt(), 32);
        QStringList generated;
        generated.reserve(count);
        for (int index = 1; index <= count; ++index)
        {
            generated << QStringLiteral("Motor-%1").arg(index, 3, 10, QLatin1Char('0'));
        }
        options.deviceIds = generated;
    }
    else
    {
        options.deviceIds = config.simulation().deviceIds;
    }

    saim::TcpServer server;
    server.setOptions(options);

    QObject::connect(&server, &saim::TcpServer::started, &app,
                     [](quint16 port)
                     {
                         out() << QStringLiteral("Listening on 0.0.0.0:%1").arg(port) << Qt::endl;
                     });

    QObject::connect(&server, &saim::TcpServer::clientConnected, &app,
                     [](const QString& peer)
                     {
                         out() << QStringLiteral("Client connected from %1").arg(peer) << Qt::endl;
                     });

    QObject::connect(&server, &saim::TcpServer::clientDisconnected, &app,
                     [](const QString& peer)
                     {
                         out() << QStringLiteral("Client %1 disconnected").arg(peer) << Qt::endl;
                     });

    QObject::connect(&server, &saim::TcpServer::errorOccurred, &app,
                     [](const QString& message)
                     {
                         out() << QStringLiteral("ERROR: %1").arg(message) << Qt::endl;
                     });

    // A once-a-second heartbeat line, so a terminal left running shows that the
    // feed is alive rather than merely that the process has not exited.  A
    // gateway that prints nothing is indistinguishable from a hung one.
    QTimer heartbeat;
    heartbeat.setInterval(1000);
    QObject::connect(&heartbeat, &QTimer::timeout, &app,
                     [&server]()
                     {
                         out() << QStringLiteral("[%1] %2 device(s)  %3 client(s)  %4")
                                      .arg(QDateTime::currentDateTime().toString(
                                               QStringLiteral("HH:mm:ss")),
                                           QString::number(server.simulatedDeviceIds().size()),
                                           QString::number(server.clientCount()),
                                           server.clientCount() == 0
                                               ? QStringLiteral("waiting for a client")
                                               : QStringLiteral("streaming"))
                               << Qt::endl;
                     });
    heartbeat.start();

    // ---- scheduled faults -------------------------------------------------
    // Both of these exist so that a demo of the *alarm path* does not depend on
    // waiting for random wear to accumulate.  They drive the same DeviceSimulator
    // the random path uses, so what the monitor sees is a genuine sample stream
    // in which one asset is genuinely degrading.
    const QStringList faultSpecs = parser.values(faultOption);
    for (const QString& spec : faultSpecs)
    {
        const QStringList parts = spec.split(QLatin1Char(':'));
        if (parts.size() != 2)
        {
            out() << QStringLiteral("Ignoring malformed --fault '%1' (expected id:seconds)")
                         .arg(spec) << Qt::endl;
            continue;
        }

        const QString deviceId = parts.at(0);
        const int seconds = parts.at(1).toInt();
        if (seconds < 0)
        {
            out() << QStringLiteral("Ignoring --fault '%1' (negative delay)").arg(spec) << Qt::endl;
            continue;
        }

        auto* timer = new QTimer(&app);
        timer->setSingleShot(true);
        timer->setInterval(seconds * 1000);
        QObject::connect(timer, &QTimer::timeout, &server,
                         [&server, deviceId]()
                         {
                             server.injectFault(deviceId, 90);
                             out() << QStringLiteral("Injected a fault on %1").arg(deviceId)
                                   << Qt::endl;
                         });
        timer->start();
    }

    for (const QString& deviceId : parser.values(wearOption))
    {
        // Wear reaches end of life over four minutes.  Slow enough that the
        // predictor's trend term has something to work with, fast enough to be
        // watchable.
        constexpr int kSteps = 480;
        auto* wearTimer = new QTimer(&app);
        wearTimer->setInterval(500);
        auto* step = new int(0);
        QObject::connect(wearTimer, &QTimer::timeout, &server,
                         [&server, deviceId, step]()
                         {
                             *step += 1;
                             const double fraction =
                                 qMin(1.0, static_cast<double>(*step) / kSteps);
                             server.setWearLevel(deviceId, fraction);
                         });
        wearTimer->start();
    }

    // ---- burst mode -------------------------------------------------------
    if (parser.isSet(burstOption))
    {
        const int count = qBound(1, parser.value(burstOption).toInt(), 100);
        auto* burstTimer = new QTimer(&app);
        burstTimer->setInterval(1000);
        QObject::connect(burstTimer, &QTimer::timeout, &server,
                         [&server, count]() { server.emitBurst(count); });
        burstTimer->start();
        out() << QStringLiteral("Burst mode: %1 extra frames per device per second").arg(count)
              << Qt::endl;
    }

    // ---- signal handling --------------------------------------------------
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    QTimer interruptPoller;
    interruptPoller.setInterval(200);
    QObject::connect(&interruptPoller, &QTimer::timeout, &app,
                     []()
                     {
                         if (g_interrupted != 0)
                         {
                             out() << QStringLiteral("Interrupted; shutting down.") << Qt::endl;
                             QCoreApplication::quit();
                         }
                     });
    interruptPoller.start();

    // ---- run --------------------------------------------------------------
    out() << QStringLiteral("SmartAISimulator %1 — %2 device(s), %3 ms interval")
                 .arg(QCoreApplication::applicationVersion())
                 .arg(options.deviceIds.isEmpty() ? 6 : options.deviceIds.size())
                 .arg(options.sampleIntervalMs)
          << Qt::endl;
    out() << QStringLiteral("Press Ctrl+C to stop.") << Qt::endl;

    server.start();

    const int exitCode = app.exec();
    server.stop();
    return exitCode;
}
