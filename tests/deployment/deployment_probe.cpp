#include "app/ApplicationController.h"
#include "core/ConfigManager.h"
#include "database/DatabaseManager.h"
#include "device/Device.h"
#include "device/DeviceManager.h"
#include "ui/ChartWidget.h"
#include "ui/DashboardWidget.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineSeries>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QSqlDatabase>
#include <QSvgRenderer>
#include <QTcpServer>
#include <QTextStream>
#include <QThread>
#include <QtCharts/QChartView>

#include <algorithm>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {
void require(bool condition, const char* description)
{
    if (!condition) { throw std::runtime_error(description); }
}

QJsonArray loadedModules()
{
    QJsonArray modules;
#ifdef Q_OS_WIN
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    require(snapshot != INVALID_HANDLE_VALUE, "Cannot enumerate loaded Windows modules");
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            modules.append(QDir::fromNativeSeparators(QString::fromWCharArray(entry.szExePath)));
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#endif
    return modules;
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("SAIM-Acceptance"));
    app.setApplicationName(QStringLiteral("SAIM-Deployment-Probe"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("evidence-dir"), QStringLiteral("Write JSON, SQLite and screenshot evidence here"), QStringLiteral("dir")});
    parser.process(app);
    const QString evidence = QDir(parser.value(QStringLiteral("evidence-dir"))).absolutePath();
    if (!parser.isSet(QStringLiteral("evidence-dir")) || !QDir().mkpath(evidence)) { return 2; }

    QJsonObject result;
    result.insert(QStringLiteral("platform"), QApplication::platformName());
    result.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
    result.insert(QStringLiteral("executable"), QCoreApplication::applicationFilePath());
    result.insert(QStringLiteral("libraryPaths"), QJsonArray::fromStringList(QCoreApplication::libraryPaths()));
    result.insert(QStringLiteral("sqlDrivers"), QJsonArray::fromStringList(QSqlDatabase::drivers()));
    QElapsedTimer elapsed;
    elapsed.start();
    int exitCode = 1;
    try
    {
        require(QApplication::platformName() == QStringLiteral("windows"), "Acceptance requires native qwindows platform");
        require(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")), "Packaged QSQLITE driver unavailable");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, evidence);
        QTcpServer reservation;
        require(reservation.listen(QHostAddress::LocalHost, 0), "Cannot reserve a loopback port");
        const int port = reservation.serverPort();
        reservation.close();

        saim::ConfigManager config;
        const auto configPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config/config.json"));
        QFile shippedConfig(configPath);
        require(shippedConfig.open(QIODevice::ReadOnly), "Shipped configuration is missing");
        require(QJsonDocument::fromJson(shippedConfig.readAll()).isObject(), "Shipped configuration is invalid JSON");
        config.load({configPath});
        config.setDataRoot(evidence);
        config.set(QStringLiteral("database.filePath"), QStringLiteral("acceptance.db"));
        config.set(QStringLiteral("database.flushIntervalMs"), 100);
        config.set(QStringLiteral("network.server.listenAddress"), QStringLiteral("127.0.0.1"));
        config.set(QStringLiteral("network.server.listenPort"), port);
        config.set(QStringLiteral("network.client.remotePort"), port);
        config.set(QStringLiteral("simulation.sampleIntervalMs"), 20);
        config.set(QStringLiteral("simulation.injectAnomalies"), false);
        config.set(QStringLiteral("ai.enableOnnx"), false);
        config.set(QStringLiteral("ai.enableVision"), false);
        saim::Theme::apply(app);
        QPointer<saim::MainWindow> window;
        {
            saim::ApplicationController controller(&config);
            require(controller.initialise().isOk(), "Controller initialisation failed");
            controller.buildUi();
            window = controller.window();
            require(!window.isNull(), "Main window was not created");
            controller.start();
            const auto motor = controller.devices()->device(QStringLiteral("MOTOR-001"));
            require(motor != nullptr, "Default motor missing");
            while ((motor->sampleCount() < 30 || motor->prediction().faultType.isEmpty()) && elapsed.elapsed() < 20000)
            {
                QApplication::processEvents();
                QThread::msleep(10);
            }
            require(motor->sampleCount() >= 30, "Loopback TCP telemetry did not reach the device model");
            require(!motor->prediction().faultType.isEmpty(), "Worker inference did not complete");
            require(window->isVisible(), "Native GUI is not visible");
            auto* chart = window->dashboard()->findChild<saim::ChartWidget*>();
            require(chart != nullptr, "Chart widget missing");
            chart->refresh();
            auto* view = chart->findChild<QChartView*>();
            require(view != nullptr, "Qt Charts view missing");
            int points = 0;
            for (auto* series : view->chart()->series())
            {
                const auto* line = qobject_cast<QLineSeries*>(series);
                if (line != nullptr) { points = std::max(points, static_cast<int>(line->count())); }
            }
            require(points >= 3, "Chart did not render live telemetry");
            controller.devices()->refreshSummary();
            window->dashboard()->refresh();
            QApplication::processEvents();
            require(window->grab().save(QDir(evidence).filePath(QStringLiteral("native-window.png"))), "GUI screenshot write failed");
            result.insert(QStringLiteral("motorSamples"), static_cast<qint64>(motor->sampleCount()));
            result.insert(QStringLiteral("chartPoints"), points);
            result.insert(QStringLiteral("guiVisible"), true);
            result.insert(QStringLiteral("loopbackPort"), port);
            controller.stop();
        }
        require(window.isNull(), "Main window was not destroyed after shutdown");
        saim::DatabaseManager database(QStringLiteral("deployment-inspection"));
        require(database.open(QDir(evidence).filePath(QStringLiteral("acceptance.db"))).isOk(), "Cannot reopen persisted database");
        const auto health = database.healthCheck();
        require(health.ok && health.sensorRows >= 30 && health.deviceRows >= 3, "Persistence or SQLite integrity check failed");
        result.insert(QStringLiteral("sensorRows"), health.sensorRows);
        result.insert(QStringLiteral("deviceRows"), health.deviceRows);
        result.insert(QStringLiteral("schemaVersion"), health.schemaVersion);
        result.insert(QStringLiteral("journalMode"), health.journalMode);
        result.insert(QStringLiteral("databaseIntegrity"), health.ok);

        QSvgRenderer svg(QByteArrayLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><rect width='8' height='8' fill='#00cc88'/></svg>"));
        require(svg.isValid(), "SVG renderer unavailable");
        QImage icon(8, 8, QImage::Format_ARGB32);
        icon.fill(Qt::transparent);
        QPainter painter(&icon);
        svg.render(&painter);
        painter.end();
        require(icon.pixelColor(4, 4).alpha() == 255, "SVG render produced no pixels");
        result.insert(QStringLiteral("svgRendered"), true);
        result.insert(QStringLiteral("loadedModules"), loadedModules());
        result.insert(QStringLiteral("ok"), true);
        exitCode = 0;
    }
    catch (const std::exception& error)
    {
        result.insert(QStringLiteral("ok"), false);
        result.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
    }
    result.insert(QStringLiteral("elapsedMs"), elapsed.elapsed());
    QFile output(QDir(evidence).filePath(QStringLiteral("probe-result.json")));
    if (!output.open(QIODevice::WriteOnly)) { return 3; }
    output.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return exitCode;
}
