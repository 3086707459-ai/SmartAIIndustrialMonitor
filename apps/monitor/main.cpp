// ---------------------------------------------------------------------------
// main.cpp - process entry point.  Thirteen lines of decisions, then Qt's loop.
//
// Everything that could be described as "the application" lives elsewhere.  This
// file's entire job is to establish the four things that must exist before any
// object does, in the order they must exist:
//
//   1. QApplication          - Qt needs it before any widget, timer or event
//   2. logging               - so that a failure in step 3 or 4 is *recorded*
//                              rather than dying silently
//   3. configuration         - because everything below reads it
//   4. the controller        - which builds the model, the workers and the window
//
// The order of 2 and 3 matters more than it looks.  ConfigManager logs a warning
// when an optional overlay is missing and an error when a file is malformed; if
// logging were configured after it, those messages would be discarded and a
// hand-edited config file with a trailing comma would fail invisibly.
//
// Why the controller is heap-allocated and destroyed explicitly
// ------------------------------------------------------------
// Workers must be stopped while QApplication still exists - tearing down a
// QThread during QApplication's own destruction is where "the process hangs on
// exit" comes from.  Deleting the controller before app.exec() returns to main
// makes that ordering explicit rather than dependent on declaration order.
// ---------------------------------------------------------------------------

#include "app/ApplicationController.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

#include <memory>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    // Identity before anything reads a path: QStandardPaths derives every
    // per-user location from these two strings, and ConfigManager uses one of
    // them for the user's config overlay and the log directory.
    QCoreApplication::setOrganizationName(QStringLiteral("SmartAI"));
    QCoreApplication::setApplicationName(QStringLiteral("SmartAIIndustrialMonitor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // -- command line -------------------------------------------------------
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Industrial device monitoring and AI fault diagnosis."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption configOption(
        QStringList{QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Read configuration from <dir>/config.json."),
        QStringLiteral("dir"));
    const QCommandLineOption dataOption(
        QStringList{QStringLiteral("d"), QStringLiteral("data-dir")},
        QStringLiteral("Put the database, logs and exports under <dir>."),
        QStringLiteral("dir"));
    const QCommandLineOption noSimulationOption(
        QStringLiteral("no-simulation"),
        QStringLiteral("Do not start the in-process plant gateway; expect a real one."));
    const QCommandLineOption logLevelOption(
        QStringLiteral("log-level"),
        QStringLiteral("trace | debug | info | warning | error  (default: from config)"),
        QStringLiteral("level"));
    parser.addOption(configOption);
    parser.addOption(dataOption);
    parser.addOption(noSimulationOption);
    parser.addOption(logLevelOption);
    parser.process(app);

    // -- configuration ------------------------------------------------------
    auto config = std::make_unique<saim::ConfigManager>();

    QStringList searchPaths = saim::ConfigManager::defaultSearchPaths();
    if (parser.isSet(configOption))
    {
        // An explicit -c wins outright rather than being appended.  Appending it
        // would leave the shipped config.json in the chain, and an operator who
        // names a file expects that file to be the one in force.
        searchPaths = QStringList{QDir(parser.value(configOption))
                                      .filePath(QStringLiteral("config.json"))};
    }
    config->load(searchPaths);

    if (parser.isSet(dataOption))
    {
        config->setDataRoot(parser.value(dataOption));
    }

    if (parser.isSet(noSimulationOption))
    {
        config->set(QStringLiteral("simulation.enabled"), false);
    }

    // -- logging ------------------------------------------------------------
    {
        const saim::ConfigManager::LoggingConfig& logging = config->logging();

        saim::Logger::Options options;
        options.minimumLevel   = saim::logLevelFromString(logging.level);
        options.consoleEnabled = logging.console;
        options.maxFileBytes   = logging.maxFileBytes;
        options.keepFiles      = logging.keepFiles;
        options.recentCapacity = 4000;   // 2000 is the default; the log panel reads this

        // The file sink is enabled only when a directory resolves. resolveDataPath()
        // returns the data root for an empty string, so this guards against the
        // configuration asking for no file logging at all.
        if (!logging.directory.isEmpty())
        {
            options.logDirectory = config->resolveDataPath(logging.directory);
        }

        if (parser.isSet(logLevelOption))
        {
            options.minimumLevel = saim::logLevelFromString(parser.value(logLevelOption));
        }

        saim::Logger::instance().configure(options);

        // Installed *after* configure() so that Qt's own warnings - a missing
        // stylesheet, an unregistered metatype, a bad SQL driver - land in the
        // same file as the application's own messages.  Those warnings are
        // usually the first clue to a real problem, and having them in a
        // separate stream from the app's log is how they get ignored.
        saim::Logger::instance().installQtMessageHandler();
    }

    SAIM_INFO(saim::logcat::kApp, QStringLiteral("%1 %2 starting")
                                      .arg(QCoreApplication::applicationName(),
                                           QCoreApplication::applicationVersion()));
    SAIM_INFO(saim::logcat::kApp, QStringLiteral("Data root: %1").arg(config->dataRoot()));

    // -- theme --------------------------------------------------------------
    // Applied before the first widget is constructed: restyling after widgets
    // exist triggers a full re-polish and a visible flicker on start-up.
    saim::Theme::apply(app);

    // -- the application ----------------------------------------------------
    auto controller = std::make_unique<saim::ApplicationController>(config.get());

    const saim::Status initialised = controller->initialise();
    if (initialised.isError())
    {
        // Fatal, and said out loud rather than logged and exited.  A user who
        // double-clicks an icon and sees nothing happen has no way to tell a
        // crash from a slow start.
        const QString message = initialised.error().message;
        SAIM_CRITICAL(saim::logcat::kApp, QStringLiteral("Start-up failed: %1").arg(message));
        QMessageBox::critical(nullptr, QObject::tr("SmartAI Industrial Monitor"),
                              QObject::tr("The application could not start.\n\n%1").arg(message));
        return 1;
    }

    controller->buildUi();
    controller->start();

    const int exitCode = app.exec();

    // Explicit, ordered teardown *before* QApplication goes away.  See the note
    // at the top of this file.
    controller->stop();
    controller.reset();

    SAIM_INFO(saim::logcat::kApp, QStringLiteral("Exiting with code %1").arg(exitCode));

    // Flush the file sink while the logger is still alive.  A Qt message handler
    // installed above stays installed until the process ends, and a queued log
    // record arriving after the sinks are gone would be written nowhere.
    saim::Logger::instance().flushSinks();

    return exitCode;
}
