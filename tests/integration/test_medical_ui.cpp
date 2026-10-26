#include "TestHarness.h"
#include "medical/MedicalWindow.h"
#include <QApplication>
#include <QChartView>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLabel>
#include <QLineSeries>
#include <QThread>
#include <QTemporaryDir>

using namespace saim::medical;
namespace {
void waitLoaded(MedicalWindow& window)
{
    QElapsedTimer deadline; deadline.start();
    while (window.record() == nullptr && deadline.elapsed() < 10000)
    { QApplication::processEvents(); QThread::msleep(5); }
    SAIM_CHECK(window.record());
}
}

SAIM_TEST(real_medical_waveforms_annotations_and_missing_numerics_render)
{
    MedicalWindow window;
    window.show();
    window.openDirectory(QStringLiteral(SAIM_MEDICAL_FIXTURE));
    waitLoaded(window);
    SAIM_CHECK(window.windowTitle().contains(QStringLiteral("MedSignal")));
    window.seek(10.0);
    const auto views = window.findChildren<QChartView*>();
    SAIM_CHECK(views.size() == 3);
    for (const auto* view : views)
    {
        const auto* line = qobject_cast<QLineSeries*>(view->chart()->series().first());
        SAIM_CHECK(line && line->count() == 1251);
    }
    SAIM_CHECK(views[2]->chart()->series()[1]->isVisible());
    window.seek(111.0);
    SAIM_CHECK(window.findChild<QLabel*>(QStringLiteral("NumericHR"))->text() == QStringLiteral("92"));
    SAIM_CHECK(window.findChild<QLabel*>(QStringLiteral("NumericPULSE"))->text() == QStringLiteral("—"));
    SAIM_CHECK(window.findChild<QLabel*>(QStringLiteral("NumericSpO2"))->text() == QStringLiteral("—"));
    window.setPlaying(true);
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < 120) { QApplication::processEvents(); QThread::msleep(5); }
    window.setPlaying(false);
    SAIM_CHECK(window.position() > 111.0);
    window.seek(999.0);
    SAIM_CHECK_NEAR(window.position(), 480.0, 1e-9);
    window.setPlaying(true); SAIM_CHECK(window.position() < 1.0); window.setPlaying(false);
    const auto report = QJsonDocument::fromJson(window.reportJson().toUtf8()).object();
    SAIM_CHECK(report.value("record_id").toString() == QStringLiteral("01"));
    SAIM_CHECK(report.value("total_windows").toInt() == 16);
    window.seek(30.0);
    QApplication::processEvents();
    const auto image = window.grab(); SAIM_CHECK(!image.isNull());
    const QString path = qEnvironmentVariable("SAIM_MEDICAL_SCREENSHOT");
    if (!path.isEmpty()) { SAIM_CHECK(image.save(path)); }
}

SAIM_TEST(empty_import_clears_previous_record_and_stops_playback)
{
    MedicalWindow window;
    window.openDirectory(QStringLiteral(SAIM_MEDICAL_FIXTURE));
    waitLoaded(window);
    window.seek(111.0); window.setPlaying(true);
    QTemporaryDir empty; SAIM_CHECK(empty.isValid());
    window.openDirectory(empty.path());
    SAIM_CHECK(!window.record());
    SAIM_CHECK(!window.isPlaying());
    SAIM_CHECK(window.findChild<QLabel*>(QStringLiteral("NumericHR"))->text() == QStringLiteral("—"));
    SAIM_CHECK(QJsonDocument::fromJson(window.reportJson().toUtf8()).object().isEmpty());
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    return saim::test::runAll();
}
