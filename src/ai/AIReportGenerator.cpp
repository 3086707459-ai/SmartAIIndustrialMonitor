#include "ai/AIReportGenerator.h"

#include "ai/FaultPredictor.h"   // recommendedAction()
#include "common/ErrorCode.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QTextDocument>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace saim {

namespace {

/// Risk-score composition weights.  Sum to 1.0, so a device that is maximal on
/// every axis lands at exactly 100.  Named rather than inlined because these are
/// the numbers a maintenance manager would want to tune, and a reviewer will
/// reasonably ask where they came from: they are engineering judgement, chosen so
/// that a machine with no alarms and a clean health score lands near zero while
/// any single axis at maximum alone pushes the device into the "look at me"
/// range.
constexpr double kWeightHealth     = 0.40;   ///< 1 - healthScore/100
constexpr double kWeightState      = 0.25;   ///< rule-based severity
constexpr double kWeightAnomaly    = 0.20;   ///< high-severity anomaly density
constexpr double kWeightAlarms     = 0.15;   ///< alarm count, saturating

/// Alarm count at which the alarm axis is considered saturated.  Beyond ~10
/// alarms in a window, the eleventh adds no information about risk.
constexpr double kAlarmSaturationCount = 10.0;

/// Severity contribution of each rule-based state.
double stateSeverityFactor(DeviceState state)
{
    switch (state)
    {
    case DeviceState::Fault:   return 1.00;
    case DeviceState::Warning: return 0.55;
    case DeviceState::Normal:  return 0.05;
    case DeviceState::Offline: return 0.30;   // not a fault, but not information-free
    }
    return 0.0;
}

QString htmlEscape(QString text)
{
    text.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    text.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    text.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return text;
}

/// Trims a double to a fixed number of decimals for display, avoiding the
/// "74.30000000000001" that raw floating point produces in a printed report.
QString num(double value, int decimals = 1)
{
    return QString::number(value, 'f', decimals);
}

}   // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AIReportGenerator::AIReportGenerator() = default;

AIReportGenerator::AIReportGenerator(Options options)
    : m_options(std::move(options))
{
}

// ---------------------------------------------------------------------------
// Risk scoring
// ---------------------------------------------------------------------------

QString AIReportGenerator::riskLevelFor(double riskScore)
{
    // Named thresholds.  "Critical" is deliberately hard to reach: if everything
    // is critical, nothing is, and the label stops carrying information.
    if (riskScore >= 70.0)
    {
        return QStringLiteral("Critical");
    }
    if (riskScore >= 45.0)
    {
        return QStringLiteral("High");
    }
    if (riskScore >= 20.0)
    {
        return QStringLiteral("Medium");
    }
    return QStringLiteral("Low");
}

DeviceReportSection AIReportGenerator::buildSection(const QString& deviceId,
                                                    const QString& deviceName,
                                                    const QString& deviceType,
                                                    DeviceState state,
                                                    const DeviceStatistics& statistics,
                                                    const FaultPrediction& prediction,
                                                    int alarmCount,
                                                    int highSeverityAnomalies,
                                                    const DeviceLimits& limits)
{
    DeviceReportSection section;
    section.deviceId   = deviceId;
    section.deviceName = deviceName.isEmpty() ? deviceId : deviceName;
    section.deviceType = deviceType;
    section.state      = state;
    section.stateText  = toString(state);
    section.statistics = statistics;
    section.alarmCount = alarmCount;
    section.highSeverityAnomalies = highSeverityAnomalies;

    section.healthScore        = prediction.healthScore;
    section.failureProbability = prediction.failureProbability;
    section.faultType          = prediction.faultType;
    section.backend            = prediction.backend;

    // -- findings: the evidence, most severe first --------------------------
    //
    // Each finding is a statement about a measured number.  Nothing is inferred
    // without the number being printed next to it, which is what makes the
    // report auditable rather than merely plausible.

    if (statistics.maxTemperature >= limits.temperatureHigh)
    {
        section.findings << QStringLiteral("Peak temperature %1 C exceeded the trip limit of %2 C")
                                .arg(num(statistics.maxTemperature)).arg(num(limits.temperatureHigh));
    }
    else if (statistics.maxTemperature >= limits.temperatureWarn)
    {
        section.findings << QStringLiteral("Peak temperature %1 C exceeded the warning limit of %2 C")
                                .arg(num(statistics.maxTemperature)).arg(num(limits.temperatureWarn));
    }

    if (statistics.maxVibration >= limits.vibrationHigh)
    {
        section.findings << QStringLiteral("Peak vibration %1 mm/s exceeded the trip limit of %2")
                                .arg(num(statistics.maxVibration, 3)).arg(num(limits.vibrationHigh, 2));
    }
    else if (statistics.maxVibration >= limits.vibrationWarn)
    {
        section.findings << QStringLiteral("Peak vibration %1 mm/s exceeded the warning limit of %2")
                                .arg(num(statistics.maxVibration, 3)).arg(num(limits.vibrationWarn, 2));
    }

    if (statistics.avgCurrent >= limits.currentWarn)
    {
        section.findings << QStringLiteral("Mean current draw %1 A is above the nominal %2 A - "
                                           "consistent with increased mechanical resistance")
                                .arg(num(statistics.avgCurrent, 2)).arg(num(limits.currentWarn));
    }

    if (highSeverityAnomalies > 0)
    {
        section.findings << QStringLiteral("%1 high-severity anomaly event(s) recorded")
                                .arg(highSeverityAnomalies);
    }

    if (alarmCount > 0)
    {
        section.findings << QStringLiteral("%1 alarm(s) raised in the reporting window")
                                .arg(alarmCount);
    }

    if (statistics.uptimeRatio > 0.0 && statistics.uptimeRatio < 0.98)
    {
        section.findings << QStringLiteral("Availability %1 %% - %2 %% of the window was spent "
                                           "outside the normal state")
                                .arg(num(statistics.uptimeRatio * 100.0))
                                .arg(num((1.0 - statistics.uptimeRatio) * 100.0));
    }

    // The predictor's verdict, when it has one.  `faultType` is empty until the
    // window has filled, and an empty verdict must not be reported as "healthy".
    if (!prediction.faultType.isEmpty()
        && prediction.faultType.compare(QStringLiteral("None"), Qt::CaseInsensitive) != 0)
    {
        section.findings << QStringLiteral("Model verdict: %1 (confidence %2 %%, failure "
                                           "probability %3 %%)")
                                .arg(prediction.faultType)
                                .arg(num(prediction.confidence * 100.0))
                                .arg(num(prediction.failureProbability * 100.0));
    }

    if (section.findings.isEmpty())
    {
        section.findings << QStringLiteral("No threshold breach, alarm or anomaly recorded in "
                                           "the reporting window");
    }

    // -- actions: from the fault class, never invented ----------------------

    if (!prediction.faultType.isEmpty())
    {
        // recommendedAction() is the single source of truth for the maintenance
        // text - the same string the assistant's chat panel shows, so the report
        // and the live UI cannot give contradictory advice.
        const FaultClass faultClass = faultClassFromString(prediction.faultType);
        section.actions << recommendedAction(faultClass);
    }

    if (statistics.maxTemperature >= limits.temperatureWarn)
    {
        section.actions << QStringLiteral("Verify cooling airflow and heat-sink condition; "
                                          "check for blocked filters");
    }
    if (statistics.maxVibration >= limits.vibrationWarn)
    {
        section.actions << QStringLiteral("Check bearing play and alignment; re-torque mounting "
                                          "bolts; schedule vibration re-measurement");
    }
    if (statistics.avgCurrent >= limits.currentWarn)
    {
        section.actions << QStringLiteral("Measure supply balance across phases and inspect for "
                                          "mechanical binding");
    }
    if (state == DeviceState::Offline && statistics.sampleCount > 0)
    {
        section.actions << QStringLiteral("Investigate the telemetry gap - verify gateway link "
                                          "and device power");
    }
    if (section.actions.isEmpty())
    {
        section.actions << QStringLiteral("No action required; continue routine monitoring");
    }

    section.actions.removeDuplicates();

    // -- risk score ---------------------------------------------------------

    const double healthRisk = std::max(0.0, 100.0 - prediction.healthScore) / 100.0;

    // Anomaly density rather than a raw count: three high-severity events in a
    // ten-sample window is a different situation from three in ten thousand, and
    // the score should reflect the rate.
    const double sampleBase = std::max<double>(1.0, static_cast<double>(statistics.sampleCount));
    const double anomalyDensity =
        std::min(1.0, static_cast<double>(highSeverityAnomalies) / std::max(1.0, sampleBase / 100.0));

    const double alarmFactor = std::min(1.0, static_cast<double>(alarmCount) / kAlarmSaturationCount);

    const double risk = kWeightHealth  * healthRisk
                        + kWeightState   * stateSeverityFactor(state)
                        + kWeightAnomaly * anomalyDensity
                        + kWeightAlarms  * alarmFactor;

    section.riskScore = std::round(std::clamp(risk * 100.0, 0.0, 100.0) * 10.0) / 10.0;
    section.riskLevel = riskLevelFor(section.riskScore);

    return section;
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

GeneratedReport AIReportGenerator::generate(QVector<DeviceReportSection> sections,
                                            const QDateTime& from,
                                            const QDateTime& to,
                                            bool exportToFile)
{
    GeneratedReport report;
    ReportModel& model = report.model;

    model.generatedAt = QDateTime::currentDateTime();
    model.windowFrom  = from;
    model.windowTo    = to;
    model.title       = QStringLiteral("SmartAI Industrial Monitor - Maintenance Report");

    // Highest risk first.  A report is read top-down and often only the top is
    // read, so the ordering *is* the prioritisation.
    std::sort(sections.begin(), sections.end(),
              [](const DeviceReportSection& a, const DeviceReportSection& b) {
                  if (std::abs(a.riskScore - b.riskScore) > 1e-9)
                  {
                      return a.riskScore > b.riskScore;
                  }
                  return a.deviceId < b.deviceId;   // stable, deterministic order
              });

    model.sections     = std::move(sections);
    model.deviceCount  = static_cast<int>(model.sections.size());

    for (const DeviceReportSection& section : model.sections)
    {
        switch (section.state)
        {
        case DeviceState::Normal:  ++model.normalCount;  break;
        case DeviceState::Warning: ++model.warningCount; break;
        case DeviceState::Fault:   ++model.faultCount;   break;
        case DeviceState::Offline: ++model.offlineCount; break;
        }
        model.totalSamples += section.statistics.sampleCount;
        model.totalAlarms  += section.alarmCount;
    }

    // Overall risk is the worst single asset, not the mean.  A line with five
    // healthy machines and one about to seize is not a 17 %-risk line.
    double worstRisk = 0.0;
    for (const DeviceReportSection& section : model.sections)
    {
        if (section.riskScore > worstRisk)
        {
            worstRisk = section.riskScore;
            model.headline = QStringLiteral("%1 (%2) - %3")
                                 .arg(section.deviceName, section.deviceId, section.riskLevel);
        }
    }
    model.overallRiskLevel = riskLevelFor(worstRisk);

    model.executiveSummary = buildExecutiveSummary(model);

    report.markdown = renderMarkdown(model);
    report.html     = renderHtml(model);

    if (exportToFile)
    {
        QString directory = m_options.outputDirectory;
        if (directory.isEmpty())
        {
            directory = QDir::current().filePath(QStringLiteral("reports"));
        }

        QDir().mkpath(directory);

        const QString baseName = QStringLiteral("maintenance_report_%1")
                                     .arg(model.generatedAt.toString(QStringLiteral("yyyyMMdd_HHmmss")));
        const QString markdownPath = QDir(directory).filePath(baseName + QStringLiteral(".md"));

        QFile file(markdownPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream stream(&file);
            // UTF-8 explicitly: the report contains the same characters the rest
            // of the application logs, and the platform default on Windows is not
            // UTF-8.
            stream.setEncoding(QStringConverter::Utf8);
            stream << report.markdown;
            file.close();
            report.filePath = markdownPath;
            report.exported = true;

            SAIM_INFO(logcat::kAi,
                      QStringLiteral("Report exported: %1 (%2 device(s), overall risk %3)")
                          .arg(markdownPath)
                          .arg(model.deviceCount)
                          .arg(model.overallRiskLevel));
        }
        else
        {
            report.errorMessage =
                QStringLiteral("Could not write %1: %2").arg(markdownPath, file.errorString());
            SAIM_WARN(logcat::kAi, report.errorMessage);
        }
    }

    return report;
}

QString AIReportGenerator::buildExecutiveSummary(const ReportModel& model) const
{
    QStringList sentences;

    sentences << QStringLiteral("%1 asset(s) were monitored between %2 and %3.")
                     .arg(model.deviceCount)
                     .arg(model.windowFrom.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                     .arg(model.windowTo.toString(QStringLiteral("yyyy-MM-dd HH:mm")));

    // Report the non-normal states explicitly, and say so when there are none -
    // an empty section reads as missing data, not as good news.
    QStringList stateParts;
    if (model.faultCount > 0)
    {
        stateParts << QStringLiteral("%1 in fault").arg(model.faultCount);
    }
    if (model.warningCount > 0)
    {
        stateParts << QStringLiteral("%1 in warning").arg(model.warningCount);
    }
    if (model.offlineCount > 0)
    {
        stateParts << QStringLiteral("%1 offline").arg(model.offlineCount);
    }

    if (stateParts.isEmpty())
    {
        sentences << QStringLiteral("All assets reported normal throughout the window.");
    }
    else
    {
        sentences << QStringLiteral("Current state: %1, %2 normal.")
                         .arg(stateParts.join(QStringLiteral(", ")))
                         .arg(model.normalCount);
    }

    sentences << QStringLiteral("%1 telemetry sample(s) and %2 alarm(s) were recorded.")
                     .arg(model.totalSamples)
                     .arg(model.totalAlarms);

    // The recommendation sentence, derived from the overall risk label rather
    // than written per report.  Deterministic text for a deterministic input is
    // the whole point - the same data always produces the same report.
    const QString risk = model.overallRiskLevel;
    if (risk == QLatin1String("Critical"))
    {
        sentences << QStringLiteral("Overall risk is CRITICAL: stop-and-inspect is recommended for "
                                    "the asset listed first before the next production shift.");
    }
    else if (risk == QLatin1String("High"))
    {
        sentences << QStringLiteral("Overall risk is HIGH: schedule maintenance for the leading "
                                    "asset within the current maintenance window.");
    }
    else if (risk == QLatin1String("Medium"))
    {
        sentences << QStringLiteral("Overall risk is MEDIUM: no immediate action required, but "
                                    "trend the leading asset over the coming shifts.");
    }
    else
    {
        sentences << QStringLiteral("Overall risk is LOW: continue routine monitoring.");
    }

    return sentences.join(QLatin1Char(' '));
}

QString AIReportGenerator::buildFindings(const DeviceReportSection& section) const
{
    return section.findings.join(QStringLiteral("; "));
}

// ---------------------------------------------------------------------------
// Markdown rendering
// ---------------------------------------------------------------------------

QString AIReportGenerator::renderMarkdown(const ReportModel& model) const
{
    QString out;
    QTextStream stream(&out);

    stream << "# " << model.title << "\n\n";
    stream << "**Generated:** " << model.generatedAt.toString(Qt::ISODate) << "  \n";
    stream << "**Window:** " << model.windowFrom.toString(Qt::ISODate) << " -> "
           << model.windowTo.toString(Qt::ISODate) << "  \n";
    stream << "**Overall risk:** " << model.overallRiskLevel << "\n\n";

    if (!m_options.sourceNote.isEmpty()) { stream << "> " << m_options.sourceNote << "\n\n"; }
    stream << "## Executive summary\n\n" << model.executiveSummary << "\n\n";

    stream << "## Fleet overview\n\n";
    stream << "| Metric | Value |\n|---|---|\n";
    stream << "| Devices monitored | " << model.deviceCount << " |\n";
    stream << "| Normal | " << model.normalCount << " |\n";
    stream << "| Warning | " << model.warningCount << " |\n";
    stream << "| Fault | " << model.faultCount << " |\n";
    stream << "| Offline | " << model.offlineCount << " |\n";
    stream << "| Telemetry samples | " << model.totalSamples << " |\n";
    stream << "| Alarms | " << model.totalAlarms << " |\n\n";

    stream << "## Risk ranking\n\n";
    stream << "| # | Device | State | Risk | Health | Failure prob. | Verdict |\n";
    stream << "|---|---|---|---|---|---|---|\n";
    int rank = 1;
    for (const DeviceReportSection& section : model.sections)
    {
        stream << "| " << rank++ << " | " << section.deviceName << " (" << section.deviceId << ") | "
               << section.stateText << " | " << section.riskLevel << " (" << num(section.riskScore)
               << ") | " << num(section.healthScore) << " | "
               << num(section.failureProbability * 100.0) << " % | "
               << (section.faultType.isEmpty() ? QStringLiteral("-") : section.faultType) << " |\n";
    }
    stream << "\n";

    stream << "## Per-device detail\n\n";
    for (const DeviceReportSection& section : model.sections)
    {
        stream << "### " << section.deviceName << " (" << section.deviceId << ")\n\n";
        stream << "- **Type:** " << section.deviceType << "\n";
        stream << "- **State:** " << section.stateText << "\n";
        stream << "- **Risk:** " << section.riskLevel << " (" << num(section.riskScore) << "/100)\n";
        stream << "- **Health score:** " << num(section.healthScore) << "/100";
        if (!section.backend.isEmpty())
        {
            stream << " (backend: " << section.backend << ")";
        }
        stream << "\n";
        stream << "- **Samples in window:** " << section.statistics.sampleCount << "\n";
        stream << "- **Peak temperature:** " << num(section.statistics.maxTemperature) << " C"
               << " (mean " << num(section.statistics.avgTemperature) << " C)\n";
        stream << "- **Peak vibration:** " << num(section.statistics.maxVibration, 3) << " mm/s\n";
        stream << "- **Mean current:** " << num(section.statistics.avgCurrent, 2) << " A\n";
        stream << "- **Availability:** " << num(section.statistics.uptimeRatio * 100.0) << " %\n";
        stream << "- **Alarms:** " << section.alarmCount
               << " | **High-severity anomalies:** " << section.highSeverityAnomalies << "\n\n";

        stream << "**Findings**\n\n";
        for (const QString& finding : section.findings)
        {
            stream << "- " << finding << "\n";
        }
        stream << "\n**Recommended actions**\n\n";
        for (const QString& action : section.actions)
        {
            stream << "- [ ] " << action << "\n";
        }
        stream << "\n";
    }

    stream << "---\n\n";
    stream << "_Generated by " << QStringLiteral("SmartAI Industrial Monitor")
           << ". All figures are derived from recorded telemetry; no value in this report is "
              "estimated or inferred beyond the model verdicts, which are labelled with their "
              "backend._\n";

    stream.flush();
    return out;
}

// ---------------------------------------------------------------------------
// HTML rendering
// ---------------------------------------------------------------------------

QString AIReportGenerator::renderHtml(const ReportModel& model) const
{
    // Palette matches the application's industrial-dark theme so that a printed
    // report and the screen it came from look like the same product.
    const QString background = QStringLiteral("#12161c");
    const QString surface    = QStringLiteral("#1a2029");
    const QString border     = QStringLiteral("#2b3442");
    const QString text       = QStringLiteral("#d6dde6");
    const QString muted      = QStringLiteral("#8b97a6");

    const auto riskColor = [](const QString& level) {
        if (level == QLatin1String("Critical")) { return QStringLiteral("#d64545"); }
        if (level == QLatin1String("High"))     { return QStringLiteral("#e07b39"); }
        if (level == QLatin1String("Medium"))   { return QStringLiteral("#d9b23c"); }
        return QStringLiteral("#3fa46a");
    };

    QString out;
    QTextStream stream(&out);

    stream << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
    stream << "<title>" << htmlEscape(model.title) << "</title>";
    stream << "<style>"
              "body{background:" << background << ";color:" << text
           << ";font-family:'Segoe UI',Roboto,Arial,sans-serif;font-size:11pt;"
              "margin:0;padding:28px;}"
              "h1{font-size:20pt;margin:0 0 4px 0;letter-spacing:0.5px;}"
              "h2{font-size:13pt;margin:26px 0 8px 0;padding-bottom:6px;border-bottom:1px solid "
           << border << ";color:#e8eef5;}"
              "h3{font-size:11.5pt;margin:20px 0 6px 0;color:#e8eef5;}"
              ".meta{color:" << muted << ";font-size:9.5pt;margin-bottom:18px;}"
              ".summary{background:" << surface << ";border-left:3px solid #3d7fd6;"
              "padding:12px 14px;margin:14px 0;line-height:1.55;}"
              "table{border-collapse:collapse;width:100%;margin:10px 0 18px 0;}"
              "th{background:" << surface << ";text-align:left;padding:7px 9px;font-size:9.5pt;"
              "color:" << muted << ";border-bottom:1px solid " << border << ";}"
              "td{padding:6px 9px;border-bottom:1px solid " << border << ";font-size:10pt;}"
              ".badge{display:inline-block;padding:2px 9px;border-radius:10px;font-size:9pt;"
              "font-weight:600;color:#0d1117;}"
              ".device{background:" << surface << ";border:1px solid " << border
           << ";border-radius:6px;padding:14px 16px;margin:12px 0;}"
              ".kv{color:" << muted << ";}"
              "ul{margin:6px 0 12px 18px;padding:0;}"
              "li{margin:3px 0;line-height:1.5;}"
              ".actions li{list-style:none;margin-left:-14px;}"
              ".actions li:before{content:'\\2610  ';color:" << muted << ";}"
              ".risk{" << "font-weight:700;}"
              "footer{margin-top:26px;padding-top:12px;border-top:1px solid " << border
           << ";color:" << muted << ";font-size:8.5pt;line-height:1.5;}"
              "</style></head><body>";

    stream << "<h1>" << htmlEscape(model.title) << "</h1>";
    stream << "<div class=\"meta\">Generated "
           << htmlEscape(model.generatedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
           << " &nbsp;|&nbsp; Window "
           << htmlEscape(model.windowFrom.toString(QStringLiteral("yyyy-MM-dd HH:mm"))) << " &rarr; "
           << htmlEscape(model.windowTo.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
           << " &nbsp;|&nbsp; Overall risk <span class=\"risk\" style=\"color:"
           << riskColor(model.overallRiskLevel) << "\">" << htmlEscape(model.overallRiskLevel)
           << "</span></div>";

    if (!m_options.sourceNote.isEmpty())
    {
        stream << "<p class=\"meta\">" << htmlEscape(m_options.sourceNote) << "</p>";
    }
    stream << "<div class=\"summary\">" << htmlEscape(model.executiveSummary) << "</div>";

    stream << "<h2>Fleet overview</h2><table><tr>"
              "<th>Devices</th><th>Normal</th><th>Warning</th><th>Fault</th><th>Offline</th>"
              "<th>Samples</th><th>Alarms</th></tr><tr>";
    stream << "<td>" << model.deviceCount << "</td><td>" << model.normalCount << "</td><td>"
           << model.warningCount << "</td><td>" << model.faultCount << "</td><td>"
           << model.offlineCount << "</td><td>" << model.totalSamples << "</td><td>"
           << model.totalAlarms << "</td></tr></table>";

    stream << "<h2>Risk ranking</h2><table><tr><th>#</th><th>Device</th><th>State</th>"
              "<th>Risk</th><th>Health</th><th>Failure prob.</th><th>Verdict</th></tr>";
    int rank = 1;
    for (const DeviceReportSection& section : model.sections)
    {
        stream << "<tr><td>" << rank++ << "</td><td>" << htmlEscape(section.deviceName) << " ("
               << htmlEscape(section.deviceId) << ")</td><td>" << htmlEscape(section.stateText)
               << "</td><td><span class=\"badge\" style=\"background:"
               << riskColor(section.riskLevel) << "\">" << htmlEscape(section.riskLevel)
               << "</span> " << num(section.riskScore) << "</td><td>" << num(section.healthScore)
               << "</td><td>" << num(section.failureProbability * 100.0) << " %</td><td>"
               << (section.faultType.isEmpty() ? QStringLiteral("&mdash;")
                                               : htmlEscape(section.faultType))
               << "</td></tr>";
    }
    stream << "</table>";

    stream << "<h2>Per-device detail</h2>";
    for (const DeviceReportSection& section : model.sections)
    {
        stream << "<div class=\"device\"><h3>" << htmlEscape(section.deviceName) << " &middot; "
               << htmlEscape(section.deviceId) << "</h3>";

        stream << "<div class=\"kv\">Type " << htmlEscape(section.deviceType) << " &nbsp;|&nbsp; State "
               << htmlEscape(section.stateText) << " &nbsp;|&nbsp; Risk <span class=\"risk\" style=\"color:"
               << riskColor(section.riskLevel) << "\">" << htmlEscape(section.riskLevel) << "</span>"
               << " &nbsp;|&nbsp; Health " << num(section.healthScore) << "/100"
               << (section.backend.isEmpty() ? QString()
                                             : QStringLiteral(" (") + htmlEscape(section.backend)
                                                   + QStringLiteral(")"))
               << "</div>";

        stream << "<table><tr><th>Peak temp.</th><th>Mean temp.</th><th>Peak vibration</th>"
                  "<th>Mean current</th><th>Availability</th><th>Samples</th><th>Alarms</th></tr><tr>";
        stream << "<td>" << num(section.statistics.maxTemperature) << " C</td><td>"
               << num(section.statistics.avgTemperature) << " C</td><td>"
               << num(section.statistics.maxVibration, 3) << " mm/s</td><td>"
               << num(section.statistics.avgCurrent, 2) << " A</td><td>"
               << num(section.statistics.uptimeRatio * 100.0) << " %</td><td>"
               << section.statistics.sampleCount << "</td><td>" << section.alarmCount
               << "</td></tr></table>";

        stream << "<b>Findings</b><ul>";
        for (const QString& finding : section.findings)
        {
            stream << "<li>" << htmlEscape(finding) << "</li>";
        }
        stream << "</ul>";

        stream << "<b>Recommended actions</b><ul class=\"actions\">";
        for (const QString& action : section.actions)
        {
            stream << "<li>" << htmlEscape(action) << "</li>";
        }
        stream << "</ul></div>";
    }

    stream << "<footer>Generated by SmartAI Industrial Monitor. All figures are derived from "
              "recorded telemetry; no value in this report is estimated or inferred beyond the "
              "model verdicts, which are labelled with their backend.</footer>";

    stream << "</body></html>";
    stream.flush();
    return out;
}

// ---------------------------------------------------------------------------
// PDF export
// ---------------------------------------------------------------------------

Status AIReportGenerator::exportPdf(const QString& html, const QString& path) const
{
    if (html.isEmpty())
    {
        return Status::fail(static_cast<int>(ErrorCode::InvalidArgument),
                            QStringLiteral("Refusing to export an empty report"));
    }

    QFileInfo info(path);
    if (!info.dir().exists() && !QDir().mkpath(info.absolutePath()))
    {
        return Status::fail(static_cast<int>(ErrorCode::IoError),
                            QStringLiteral("Cannot create directory %1").arg(info.absolutePath()));
    }

    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    writer.setTitle(QStringLiteral("SmartAI Maintenance Report"));
    writer.setResolution(300);   // print quality; 96 renders visibly soft on paper

    QTextDocument document;
    document.setHtml(html);

    // QTextDocument lays out in device-independent pixels, while QPdfWriter's
    // paint device is in its own resolution.  Setting the document's page width
    // to the writer's paintable width is what stops the content overflowing the
    // right margin - the classic "my PDF is cut off" bug.
    document.setPageSize(QSizeF(writer.width(), writer.height()));
    document.print(&writer);

    if (!QFileInfo::exists(path))
    {
        return Status::fail(static_cast<int>(ErrorCode::IoError),
                            QStringLiteral("PDF writer produced no file at %1").arg(path));
    }

    SAIM_INFO(logcat::kAi, QStringLiteral("Report PDF exported: %1").arg(path));
    return Status::ok();
}

}   // namespace saim
