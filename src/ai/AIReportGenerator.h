#pragma once

// ---------------------------------------------------------------------------
// AIReportGenerator.h - the daily / on-demand maintenance report.
//
// What this actually is, stated honestly
// -------------------------------------
// This is a *reporting* component, not a generative one.  It aggregates numbers
// that the system already measured - uptime, anomaly counts, peak temperature,
// alarm history, the AI verdicts - and writes them into a document with a
// narrative structure a maintenance engineer can act on.  Every sentence it
// emits is derived from a query result, and each one can be traced back to the
// row that produced it.
//
// That distinction is deliberate and worth defending.  "The AI writes the
// report" is easy to demo and impossible to trust: an operator cannot tell a
// hallucinated recommendation from a real one, and a maintenance decision made
// on a fabricated trend line is worse than no report at all.  A report that says
// exactly which numbers it used, and refuses to say anything the data does not
// support, is the version that survives contact with a plant.
//
// The structure is the same one a shift report has always had:
//
//   1. Fleet summary        - how many assets, how many in each state
//   2. Per-device section   - uptime, peaks, anomaly count, trend, risk
//   3. Findings             - the specific evidence, in severity order
//   4. Recommended actions  - derived from the fault class, not invented
//
// Two output formats, and why both
// --------------------------------
// Markdown is the one that gets pasted into a ticket or a chat.  HTML is what
// gets rendered, printed, or converted.  Generating both from one intermediate
// representation - rather than templating twice - means they cannot disagree.
//
// PDF is produced by handing the HTML to QTextDocument, which Qt renders and
// QPdfWriter writes.  That avoids a dependency on a headless browser or a
// LaTeX toolchain, both of which are a deployment problem on a plant PC.
// ---------------------------------------------------------------------------

#include "common/Result.h"
#include "common/Types.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace saim {

/// One device's contribution to the report.
struct DeviceReportSection
{
    QString deviceId;
    QString deviceName;
    QString deviceType;

    DeviceState state = DeviceState::Offline;
    QString     stateText;

    /// Health at the time of the report, and the class the predictor named.
    double  healthScore = 100.0;
    double  failureProbability = 0.0;
    QString faultType;
    QString backend;

    DeviceStatistics statistics;

    int     alarmCount = 0;
    int     highSeverityAnomalies = 0;
    QStringList findings;          ///< evidence, most severe first
    QStringList actions;           ///< recommended maintenance

    /// 0..100, higher is riskier - the sort key for the report.
    double riskScore = 0.0;
    QString riskLevel;             ///< "Low" | "Medium" | "High" | "Critical"
};

/// The whole report, before rendering.
struct ReportModel
{
    QString   title;
    QDateTime generatedAt;
    QDateTime windowFrom;
    QDateTime windowTo;

    /// Fleet roll-up.
    int deviceCount = 0;
    int normalCount = 0;
    int warningCount = 0;
    int faultCount = 0;
    int offlineCount = 0;

    qint64 totalSamples = 0;
    qint64 totalAlarms  = 0;

    QVector<DeviceReportSection> sections;   ///< sorted by riskScore, descending

    /// One-paragraph executive summary, built from the numbers above.
    QString executiveSummary;
    /// The single highest-risk finding, or empty when there is none.  Empty is a
    /// meaningful value here: "nothing needs attention" is the most common
    /// correct answer and the report should say so plainly.
    QString headline;
    QString overallRiskLevel;
};

/// The rendered report.
struct GeneratedReport
{
    ReportModel model;
    QString markdown;
    QString html;

    /// Absolute path when `exportToFile` was requested and the write succeeded.
    QString filePath;
    bool    exported = false;
    QString errorMessage;
};

class AIReportGenerator
{
public:
    struct Options
    {
        /// Directory for exported reports.  Empty means "the config data root".
        QString outputDirectory;
        QString sourceNote;
        /// Rows pulled per device when aggregating.  Bounded so that a month of
        /// 2 Hz data does not turn one button press into a 500 MB allocation -
        /// the heavy aggregates are computed in SQL, this only caps the detail.
        int historyLimit = 5000;
        /// Devices below this health score are listed even if their rule-based
        /// state is still Normal.  A machine at 88 % health with a rising trend
        /// is exactly what a shift report is for.
        double healthAttentionThreshold = 90.0;
    };

    AIReportGenerator();
    explicit AIReportGenerator(Options options);
    ~AIReportGenerator() = default;

    AIReportGenerator(const AIReportGenerator&)            = delete;
    AIReportGenerator& operator=(const AIReportGenerator&) = delete;

    [[nodiscard]] const Options& options() const noexcept { return m_options; }
    void setOptions(const Options& options) { m_options = options; }

    /// Builds the model and renders both formats.
    ///
    /// `sections` and the fleet counters are supplied by the caller, which is
    /// the only component that can query both the database and the live device
    /// registry.  Taking data as a parameter rather than reaching for a
    /// repository is what keeps this class pure - and therefore testable without
    /// a database file.
    [[nodiscard]] GeneratedReport generate(QVector<DeviceReportSection> sections,
                                           const QDateTime& from,
                                           const QDateTime& to,
                                           bool exportToFile);

    /// Renders an existing model.  Exposed so the UI can re-render at a
    /// different timestamp without re-running the queries.
    [[nodiscard]] QString renderMarkdown(const ReportModel& model) const;
    [[nodiscard]] QString renderHtml(const ReportModel& model) const;

    /// Writes `html` to a PDF at `path` using Qt's own document engine.
    /// Returns a Status - a report that failed to export must say so rather than
    /// silently leaving the operator with a file that does not exist.
    [[nodiscard]] Status exportPdf(const QString& html, const QString& path) const;

    /// Convenience: builds a section from one device's statistics and AI verdict.
    /// Pure, so the risk scoring can be unit-tested on constructed inputs.
    [[nodiscard]] static DeviceReportSection buildSection(const QString& deviceId,
                                                          const QString& deviceName,
                                                          const QString& deviceType,
                                                          DeviceState state,
                                                          const DeviceStatistics& statistics,
                                                          const FaultPrediction& prediction,
                                                          int alarmCount,
                                                          int highSeverityAnomalies,
                                                          const DeviceLimits& limits);

    /// Maps a 0..100 risk score onto its label.  Thresholds are named constants
    /// rather than magic numbers because the UI colours and the report text both
    /// key off them.
    [[nodiscard]] static QString riskLevelFor(double riskScore);

private:
    [[nodiscard]] QString buildExecutiveSummary(const ReportModel& model) const;
    [[nodiscard]] QString buildFindings(const DeviceReportSection& section) const;

    Options m_options;
};

}   // namespace saim
