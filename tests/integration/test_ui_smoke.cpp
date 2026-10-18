#include "TestHarness.h"
#include "app/ApplicationController.h"
#include "core/ConfigManager.h"
#include "device/Device.h"
#include "device/DeviceManager.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/DashboardWidget.h"
#include "ui/ChartWidget.h"
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

using namespace saim;

SAIM_TEST(window_renders_live_telemetry_and_releases_on_shutdown)
{
    QTemporaryDir scratch;
    SAIM_CHECK(scratch.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    QTcpServer reservation;
    SAIM_CHECK(reservation.listen(QHostAddress::LocalHost, 0));
    const int port = reservation.serverPort();
    reservation.close();

    ConfigManager config;
    config.load({});
    config.setDataRoot(scratch.path());
    config.set(QStringLiteral("network.server.listenAddress"), QStringLiteral("127.0.0.1"));
    config.set(QStringLiteral("network.server.listenPort"), port);
    config.set(QStringLiteral("network.client.remotePort"), port);
    config.set(QStringLiteral("simulation.sampleIntervalMs"), 50);
    config.set(QStringLiteral("simulation.injectAnomalies"), false);
    config.set(QStringLiteral("ai.enableOnnx"), false);
    config.set(QStringLiteral("ai.enableVision"), false);

    QPointer<MainWindow> window;
    {
        ApplicationController controller(&config);
        SAIM_CHECK(controller.initialise().isOk());
        controller.buildUi();
        window = controller.window();
        SAIM_CHECK(window);
        controller.start();
        QElapsedTimer deadline;
        deadline.start();
        auto motor = controller.devices()->device(QStringLiteral("MOTOR-001"));
        SAIM_CHECK(motor);
        while ((motor->sampleCount() < 10 || motor->prediction().faultType.isEmpty())
               && deadline.elapsed() < 10000)
        {
            QApplication::processEvents();
            QThread::msleep(10);
        }
        SAIM_CHECK(motor->sampleCount() >= 10);
        SAIM_CHECK(!motor->prediction().faultType.isEmpty());
        SAIM_CHECK(window->isVisible());
        const auto chart = window->dashboard()->findChild<ChartWidget*>();
        SAIM_CHECK(chart);
        chart->refresh();
        const auto view = chart->findChild<QChartView*>();
        SAIM_CHECK(view);
        bool hasTelemetry = false;
        for (auto* series : view->chart()->series())
        {
            const auto* line = qobject_cast<QLineSeries*>(series);
            if (line != nullptr && line->count() > 2) { hasTelemetry = true; }
        }
        SAIM_CHECK(hasTelemetry);
        // Summary publication is throttled independently of device updates.
        // Capture one consistent UI snapshot after the first inference.
        controller.devices()->refreshSummary();
        window->dashboard()->refresh();
        QElapsedTimer animationTime;
        animationTime.start();
        while (animationTime.elapsed() < 250)
        {
            QApplication::processEvents();
            QThread::msleep(10);
        }
        const auto capture = window->grab();
        SAIM_CHECK(!capture.isNull());
        const QString path = qEnvironmentVariable("SAIM_SMOKE_SCREENSHOT");
        if (!path.isEmpty()) { SAIM_CHECK(capture.save(path)); }
        controller.stop();
    }
    SAIM_CHECK(window.isNull());
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("SAIM-UI-Test"));
    app.setApplicationName(QStringLiteral("SAIM-UI-Smoke"));
    Theme::apply(app);
    return saim::test::runAll();
}
