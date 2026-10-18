#include "ui/ReportDialog.h"

#include "core/Logger.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace saim {

namespace {

/// Report periods, in hours.  0 means "everything the retention policy keeps".
struct PeriodOption
{
    const char* label;
    int         hours;
};

const PeriodOption kPeriods[] = {
    {"Retained live window", 0},
};

/// The element id the report's own HTML uses; injected here so the operator
/// lands on the findings rather than on the title page.
constexpr auto kFindingsAnchor = "findings";

}   // namespace

ReportDialog::ReportDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("ReportDialog"));
    setWindowTitle(tr("Maintenance report"));
    setModal(false);           // non-modal: the operator can keep monitoring
    resize(980, 720);
    buildUi();
    updateButtonState();
}

ReportDialog::~ReportDialog() = default;

void ReportDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding,
                             Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding);
    root->setSpacing(Theme::Metrics::kPanelSpacing);

    // -- scope row ----------------------------------------------------------
    m_scopeBox = new QComboBox(this);
    m_scopeBox->setMinimumWidth(220);
    connect(m_scopeBox, &QComboBox::currentIndexChanged, this, &ReportDialog::onScopeChanged);

    m_periodBox = new QComboBox(this);
    for (const PeriodOption& period : kPeriods)
    {
        m_periodBox->addItem(tr(period.label), period.hours);
    }
    m_periodBox->setCurrentIndex(0);
    m_periodBox->setEnabled(false);
    m_periodBox->setToolTip(tr("Uses the current in-memory telemetry window; historical reports are not available."));

    m_generateButton = new QPushButton(tr("Generate"), this);
    m_generateButton->setObjectName(QStringLiteral("PrimaryButton"));
    m_generateButton->setDefault(true);
    connect(m_generateButton, &QPushButton::clicked, this, &ReportDialog::onGenerateClicked);

    auto* scopeRow = new QHBoxLayout();
    scopeRow->setSpacing(Theme::Metrics::kPanelSpacing);
    scopeRow->addWidget(new QLabel(tr("Scope:"), this), 0);
    scopeRow->addWidget(m_scopeBox, 1);
    scopeRow->addWidget(new QLabel(tr("Period:"), this), 0);
    scopeRow->addWidget(m_periodBox, 1);
    scopeRow->addWidget(m_generateButton, 0);
    root->addLayout(scopeRow);

    // -- headline -----------------------------------------------------------
    // The single highest-risk finding, pulled out of the document so it is
    // readable without scrolling.  A shift report whose important line is on
    // page two is a shift report nobody reads.
    m_headlineLabel = new QLabel(this);
    m_headlineLabel->setObjectName(QStringLiteral("ReportHeadline"));
    m_headlineLabel->setTextFormat(Qt::PlainText);
    m_headlineLabel->setWordWrap(true);
    m_headlineLabel->hide();
    root->addWidget(m_headlineLabel);

    // -- progress -----------------------------------------------------------
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);     // indeterminate: the query duration is unknown
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    m_progress->hide();
    root->addWidget(m_progress);

    // -- preview ------------------------------------------------------------
    m_preview = new QTextBrowser(this);
    m_preview->setObjectName(QStringLiteral("ReportPreview"));
    m_preview->setOpenExternalLinks(false);
    m_preview->setOpenLinks(false);   // the report's links are anchors, not URLs
    root->addWidget(m_preview, 1);

    // -- status -------------------------------------------------------------
    m_statusLabel = new QLabel(tr("Choose a scope and press Generate."), this);
    m_statusLabel->setObjectName(QStringLiteral("ReportStatus"));
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    // -- actions ------------------------------------------------------------
    m_copyButton = new QPushButton(tr("Copy Markdown"), this);
    connect(m_copyButton, &QPushButton::clicked, this, &ReportDialog::onCopyClicked);

    m_saveMarkdownButton = new QPushButton(tr("Save .md"), this);
    connect(m_saveMarkdownButton, &QPushButton::clicked,
            this, &ReportDialog::onSaveMarkdownClicked);

    m_saveHtmlButton = new QPushButton(tr("Save .html"), this);
    connect(m_saveHtmlButton, &QPushButton::clicked,
            this, &ReportDialog::onSaveHtmlClicked);

    m_exportButton = new QPushButton(tr("Export PDF && HTML"), this);
    m_exportButton->setObjectName(QStringLiteral("PrimaryButton"));
    connect(m_exportButton, &QPushButton::clicked, this, &ReportDialog::onExportFilesClicked);

    auto* closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(Theme::Metrics::kPanelSpacing);
    actionRow->addWidget(m_copyButton, 0);
    actionRow->addWidget(m_saveMarkdownButton, 0);
    actionRow->addWidget(m_saveHtmlButton, 0);
    actionRow->addStretch(1);
    actionRow->addWidget(m_exportButton, 0);
    actionRow->addWidget(closeButton, 0);
    root->addLayout(actionRow);
}

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

void ReportDialog::setDeviceList(const QVector<QPair<QString, QString>>& devices)
{
    const QString previous = m_scopeBox->currentData().toString();

    m_scopeBox->blockSignals(true);
    m_scopeBox->clear();
    m_scopeBox->addItem(tr("Whole fleet"), QString());
    for (const auto& device : devices)
    {
        m_scopeBox->addItem(device.second, device.first);
    }

    const int restored = m_scopeBox->findData(previous);
    m_scopeBox->setCurrentIndex(restored >= 0 ? restored : 0);
    m_scopeBox->blockSignals(false);

    if (!m_preselectedDevice.isEmpty())
    {
        selectDevice(m_preselectedDevice);
    }
}

void ReportDialog::selectDevice(const QString& deviceId)
{
    m_preselectedDevice = deviceId;
    const int index = m_scopeBox->findData(deviceId);
    if (index >= 0)
    {
        m_scopeBox->setCurrentIndex(index);
    }
}

void ReportDialog::onScopeChanged(int index)
{
    Q_UNUSED(index)
    // Switching scope does not clear the preview.  The previous report is still
    // a valid document for the scope it was generated for, and the status line
    // already names that scope - throwing it away would lose something the
    // operator may still be reading.
    updateButtonState();
}

QDateTime ReportDialog::windowTo() const
{
    return QDateTime::currentDateTime();
}

QDateTime ReportDialog::windowFrom() const
{
    const int hours = m_periodBox->currentData().toInt();
    if (hours <= 0)
    {
        // A fixed, obviously-ancient instant rather than QDateTime() - an invalid
        // datetime reaching a SQL BETWEEN clause is a silently empty result, and
        // "no data" is indistinguishable from "no devices" in the UI.
        static const QDateTime kRetentionFloor(QDate(2000, 1, 1), QTime(0, 0));
        return kRetentionFloor;
    }
    return windowTo().addSecs(-static_cast<qint64>(hours) * 3600);
}

// ---------------------------------------------------------------------------
// Generation lifecycle
// ---------------------------------------------------------------------------

void ReportDialog::onGenerateClicked()
{
    if (m_generating)
    {
        return;
    }
    setGenerating(true);

    const QString deviceId = m_scopeBox->currentData().toString();
    emit reportRequested(deviceId, windowFrom(), windowTo(), false);
}

void ReportDialog::onExportFilesClicked()
{
    if (m_generating)
    {
        return;
    }
    setGenerating(true);

    const QString deviceId = m_scopeBox->currentData().toString();
    emit reportRequested(deviceId, windowFrom(), windowTo(), true);
}

void ReportDialog::setGenerating(bool generating)
{
    m_generating = generating;
    m_generateButton->setEnabled(!generating);
    m_exportButton->setEnabled(!generating && m_hasReport);
    m_progress->setVisible(generating);

    if (generating)
    {
        m_statusLabel->setText(tr("Aggregating history and running the predictor…"));
    }
}

void ReportDialog::setReport(const ReportResult& result)
{
    setGenerating(false);

    m_result = result;
    m_hasReport = true;

    m_preview->setHtml(result.html);
    m_preview->scrollToAnchor(QString::fromLatin1(kFindingsAnchor));

    // The headline comes from the generator's model, which is the component
    // that decided what the most important finding was.  A report whose headline
    // is "nothing needs attention" is a *success*, not an empty result - so the
    // banner says so explicitly rather than simply not appearing.
    const QString headline = result.headline.isEmpty()
                                 ? tr("No findings need attention for this scope and period.")
                                 : result.headline;

    const QColor tone = result.headline.isEmpty() ? Theme::normal()
                                                  : Theme::riskColor(100.0 - riskScoreHint(result));
    m_headlineLabel->setText(QStringLiteral("%1  %2")
                                 .arg(result.overallRiskLevel.isEmpty()
                                          ? tr("Overall:")
                                          : result.overallRiskLevel + QStringLiteral(":"),
                                      headline));
    m_headlineLabel->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600; padding: 2px 0;").arg(tone.name()));
    m_headlineLabel->show();

    if (result.exported && !result.filePath.isEmpty())
    {
        m_statusLabel->setText(tr("Report for %1 device(s) generated and exported to %2")
                                   .arg(result.deviceCount)
                                   .arg(QDir::toNativeSeparators(result.filePath)));
    }
    else
    {
        m_statusLabel->setText(tr("Report for %1 device(s) generated. "
                                  "Use Export to write the PDF and HTML.")
                                   .arg(result.deviceCount));
    }

    updateButtonState();
}

void ReportDialog::setError(const QString& message)
{
    setGenerating(false);
    m_statusLabel->setText(message);

    // The error goes into the preview too, replacing whatever was there.  A
    // status line at the bottom of a long dialog is easy to miss, and after a
    // failure the preview is showing a stale report for a scope the operator
    // has since changed - which is worse than showing the error.
    m_preview->setHtml(QStringLiteral(
                           "<div style='color:#e8a13c;font-family:sans-serif;padding:24px'>"
                           "<h3>Report generation failed</h3><p>%1</p></div>")
                           .arg(message.toHtmlEscaped()));
    m_headlineLabel->hide();
    updateButtonState();
}

void ReportDialog::updateButtonState()
{
    const bool ready = m_hasReport && !m_generating;
    m_copyButton->setEnabled(ready);
    m_saveHtmlButton->setEnabled(ready);
    m_saveMarkdownButton->setEnabled(ready);
    m_exportButton->setEnabled(ready);
}

double ReportDialog::riskScoreHint(const ReportResult& result)
{
    // Matches AIReportGenerator::riskLevelFor's bands.  Expressed as a lookup on
    // the label rather than a duplicated set of thresholds, so the two can only
    // disagree by the label being unrecognised - which lands on Medium, the
    // neutral answer.
    const QString level = result.overallRiskLevel.toLower();
    if (level == QLatin1String("critical"))
    {
        return 92.0;
    }
    if (level == QLatin1String("high"))
    {
        return 75.0;
    }
    if (level == QLatin1String("low"))
    {
        return 12.0;
    }
    return 45.0;
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

void ReportDialog::saveTextToFile(const QString& content, const QString& suggestedName,
                                  const QString& filter, const QString& description)
{
    if (content.isEmpty())
    {
        m_statusLabel->setText(tr("Nothing to save."));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(this, description, suggestedName, filter);
    if (path.isEmpty())
    {
        return;   // cancelled; not an error, and saying so would be noise
    }

    // QSaveFile writes to a temporary and renames on commit, so a crash or a
    // full disk during the write cannot leave a half-written report where the
    // previous one was.  For a document someone may file as a maintenance
    // record, that matters.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        const QString reason = file.errorString();
        m_statusLabel->setText(tr("Could not write %1: %2").arg(path, reason));
        SAIM_WARN("report", QStringLiteral("save failed: %1 (%2)").arg(path, reason));
        return;
    }

    file.write(content.toUtf8());
    if (!file.commit())
    {
        const QString reason = file.errorString();
        m_statusLabel->setText(tr("Could not write %1: %2").arg(path, reason));
        return;
    }

    m_statusLabel->setText(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
}

void ReportDialog::onSaveMarkdownClicked()
{
    saveTextToFile(m_result.markdown,
                   QStringLiteral("smartai-report.md"),
                   tr("Markdown (*.md);;Text (*.txt);;All files (*)"),
                   tr("Save the report as Markdown"));
}

void ReportDialog::onSaveHtmlClicked()
{
    saveTextToFile(m_result.html,
                   QStringLiteral("smartai-report.html"),
                   tr("HTML (*.html);;All files (*)"),
                   tr("Save the report as HTML"));
}

void ReportDialog::onCopyClicked()
{
    QApplication::clipboard()->setText(m_result.markdown);
    m_statusLabel->setText(tr("Markdown copied to the clipboard (%1 characters).")
                               .arg(m_result.markdown.size()));
}

}   // namespace saim
