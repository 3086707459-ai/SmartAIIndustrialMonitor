#include "medical/MedicalWindow.h"
#include <QApplication>
#include <QChartView>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineSeries>
#include <QPainter>
#include <QSvgRenderer>
#include <QThread>
#include <QTextStream>
#include <QTableWidget>
#include <QComboBox>
#include <QTabWidget>
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
void require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
QJsonArray modules()
{
    QJsonArray result;
#ifdef Q_OS_WIN
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    require(snapshot != INVALID_HANDLE_VALUE, "Cannot enumerate runtime modules");
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do { result.append(QDir::fromNativeSeparators(QString::fromWCharArray(entry.szExePath))); }
        while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#endif
    return result;
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"evidence-dir", "Write deployment JSON and screenshot", "directory"});
    parser.process(app);
    if (!parser.isSet("evidence-dir")) { return 2; }
    const QDir evidence(QDir(parser.value("evidence-dir")).absolutePath());
    if (!QDir().mkpath(evidence.path())) { return 2; }
    QJsonObject result{{"schemaVersion", 2}, {"product", "MedSignalWorkbench"},
        {"platform", QApplication::platformName()}, {"qtVersion", QString::fromLatin1(qVersion())}};
    int exitCode = 1;
    try
    {
        require(QApplication::platformName() == QStringLiteral("windows"), "Native qwindows platform required");
        saim::medical::MedicalWindow window;
        window.show();
        window.openDirectory(QDir(app.applicationDirPath()).filePath(QStringLiteral("datasets/bidmc-demo")));
        QElapsedTimer deadline; deadline.start();
        while (window.record() == nullptr && deadline.elapsed() < 10000)
        { QApplication::processEvents(); QThread::msleep(5); }
        require(window.record() != nullptr, "Packaged original BIDMC record did not load");
        require(window.record()->sha256[0] == QStringLiteral("21495169cc36e82352d8e12cc6430927946d02d240b030715088d02396ea2d5c"), "Packaged data hash differs from publisher");
        require(window.record()->waveforms.size() == 60001, "Incomplete original waveform record");
        window.seek(111.0);
        require(window.findChild<QLabel*>(QStringLiteral("NumericSpO2"))->text() == QStringLiteral("—"), "Missing SpO2 was not preserved");
        window.seek(30.0);
        const auto views = window.findChildren<QChartView*>();
        require(views.size() == 3, "Three synchronized charts required");
        int points = 0;
        for (const auto* view : views)
        {
            const auto* line = qobject_cast<QLineSeries*>(view->chart()->series().first());
            require(line != nullptr && line->count() == 1251, "Chart lost original 125 Hz samples");
            points += static_cast<int>(line->count());
        }
        const auto report = QJsonDocument::fromJson(window.reportJson().toUtf8()).object();
        require(report.value("total_windows").toInt() == 16 && report.value("accepted_windows").toInt() > 0, "Packaged respiratory baseline failed");
        QFile evaluation(evidence.filePath(QStringLiteral("bidmc-demo-evaluation.json")));
        require(evaluation.open(QIODevice::WriteOnly), "Cannot write evaluation evidence");
        const auto json = window.reportJson().toUtf8();
        require(evaluation.write(json) == json.size(), "Cannot save evaluation evidence");
        require(saim::medical::saveEvaluationReport(report, evidence.filePath(QStringLiteral("bidmc-demo-evaluation.csv")),
            saim::medical::EvaluationFormat::Csv).isOk(), "Cannot save packaged CSV evaluation");
        auto* review = window.findChild<QTableWidget*>(QStringLiteral("WindowReview"));
        require(review && review->rowCount() == 16, "Window review table is incomplete");
        window.reviewWindow(1);
        const auto* reviewedLine = qobject_cast<QLineSeries*>(views[0]->chart()->series().first());
        require(reviewedLine && reviewedLine->count() == 3750 && reviewedLine->at(0).x() == 30.0,
            "Review navigation did not preserve the complete half-open interval");
        window.findChild<QComboBox*>(QStringLiteral("WaveformSpan"))->setCurrentIndex(0);
        window.seek(30.0);
        QSvgRenderer svg(QStringLiteral(":/icons/medsignal.svg"));
        require(svg.isValid(), "Packaged medical SVG asset unavailable");
        QImage icon(64, 64, QImage::Format_ARGB32); icon.fill(Qt::transparent);
        { QPainter painter(&icon); svg.render(&painter); }
        require(icon.pixelColor(32, 32).alpha() > 0, "SVG rendering failed");
        QApplication::processEvents();
        require(window.grab().save(evidence.filePath(QStringLiteral("native-window.png"))), "Cannot capture native medical window");
        auto* selector = window.findChild<QComboBox*>(QStringLiteral("RecordSelector"));
        require(selector && selector->count() == 2, "Packaged difficult record 05 is missing");
        selector->setCurrentIndex(1);
        deadline.restart();
        while (window.record() == nullptr && deadline.elapsed() < 10000)
        { QApplication::processEvents(); QThread::msleep(5); }
        require(window.record() && window.record()->id == QStringLiteral("05"), "Difficult record did not load");
        const auto difficult = QJsonDocument::fromJson(window.reportJson().toUtf8()).object();
        require(difficult.value("accepted_windows").toInt() == 2 && review->rowCount() == 16, "Difficult windows were hidden or incorrectly scored");
        window.findChild<QTabWidget*>(QStringLiteral("MedicalTabs"))->setCurrentIndex(1);
        QApplication::processEvents();
        require(window.grab().save(evidence.filePath(QStringLiteral("native-review.png"))), "Cannot capture window review");
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("guiVisible"), window.isVisible());
        result.insert(QStringLiteral("svgRendered"), true);
        result.insert(QStringLiteral("missingValuePreserved"), true);
        result.insert(QStringLiteral("waveformRows"), window.record()->waveforms.size());
        result.insert(QStringLiteral("numericRows"), window.record()->numerics.size());
        result.insert(QStringLiteral("chartPoints"), points);
        result.insert(QStringLiteral("reviewRows"), review->rowCount());
        result.insert(QStringLiteral("reviewNavigationPassed"), true);
        result.insert(QStringLiteral("csvExportPassed"), true);
        result.insert(QStringLiteral("difficultAcceptedWindows"), difficult.value("accepted_windows").toInt());
        result.insert(QStringLiteral("loadedModules"), modules());
        exitCode = 0;
    }
    catch (const std::exception& exception)
    { result.insert(QStringLiteral("ok"), false); result.insert(QStringLiteral("error"), QString::fromUtf8(exception.what())); }
    QFile output(evidence.filePath(QStringLiteral("probe-result.json")));
    if (!output.open(QIODevice::WriteOnly)) { return 3; }
    const auto bytes = QJsonDocument(result).toJson();
    if (output.write(bytes) != bytes.size()) { return 3; }
    QTextStream(stdout) << bytes;
    return exitCode;
}
