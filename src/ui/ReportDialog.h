#pragma once

// ---------------------------------------------------------------------------
// ReportDialog.h - preview and export of the maintenance report.
//
// The design decision worth explaining
// ------------------------------------
// The report is *generated on the AI worker thread* and this dialog only ever
// renders what it is handed.  It would have been shorter to build the report
// here, on the UI thread, straight from the device registry - and it would have
// frozen the window for the duration of the aggregate queries.  A month of 2 Hz
// data across six devices is a few million rows; SQLite will answer that in a
// second or two, which is a second or two of a visibly hung interface.
//
// So the split is: this dialog owns *presentation and file saving of what it
// already holds*, the worker owns *generation*.  That also makes the export
// buttons that need no round trip - copying the Markdown, saving the HTML -
// genuinely instant, because the text is already in memory.
//
// PDF is the exception, and it deliberately round-trips through the worker:
// the generator owns the Markdown -> HTML -> PDF rendering path, and having the
// dialog re-implement part of it would mean two renderers that can disagree
// about what the report says.
// ---------------------------------------------------------------------------

#include "thread/AIWorker.h"

#include <QDateTime>
#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;

namespace saim {

class ReportDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit ReportDialog(QWidget* parent = nullptr);
    ~ReportDialog() override;

    /// Populates the scope selector.  The dialog does not reach into the device
    /// registry itself; the controller supplies the list, so this dialog has no
    /// dependency on the domain model at all.
    void setDeviceList(const QVector<QPair<QString, QString>>& devices);   // id, display name

    /// Preselects a device, e.g. when opened from a device's context menu.
    void selectDevice(const QString& deviceId);

public slots:
    /// A finished report.  Renders the HTML and re-enables the controls.
    void setReport(const saim::ReportResult& result);

    /// Shows or hides the busy state.  The generation is asynchronous, so the
    /// dialog must say so rather than appearing to have ignored the click.
    void setGenerating(bool generating);

    /// Reports a generation failure.  Kept distinct from a successful report so
    /// the preview area can show the error where the report would have been.
    void setError(const QString& message);

signals:
    /// Asks for a report.  `exportToFile` distinguishes a preview from an
    /// export: the worker writes files only in the latter case, so previewing
    /// never leaves stray documents in the output directory.
    void reportRequested(const QString& deviceId, const QDateTime& from,
                         const QDateTime& to, bool exportToFile);

private slots:
    void onGenerateClicked();
    void onExportFilesClicked();
    void onSaveHtmlClicked();
    void onSaveMarkdownClicked();
    void onCopyClicked();
    void onScopeChanged(int index);

private:
    void buildUi();
    void updateButtonState();
    [[nodiscard]] QDateTime windowFrom() const;
    [[nodiscard]] QDateTime windowTo() const;

    /// Maps the generator's risk *label* back onto a 0..100 score so the
    /// headline banner can be tinted with the same palette the report itself
    /// uses.  The label is the generator's vocabulary ("Low".."Critical") and
    /// this is deliberately the only place that translates it - the alternative,
    /// shipping both the score and the label, invites the two to disagree.
    [[nodiscard]] static double riskScoreHint(const saim::ReportResult& result);

    /// Writes `content` to a file the operator picks.  Returns silently when the
    /// dialog is cancelled; reports the failure in the status line otherwise.
    void saveTextToFile(const QString& content, const QString& suggestedName,
                        const QString& filter, const QString& description);

    QComboBox*    m_scopeBox = nullptr;
    QComboBox*    m_periodBox = nullptr;
    QPushButton*  m_generateButton = nullptr;
    QPushButton*  m_exportButton = nullptr;
    QPushButton*  m_saveHtmlButton = nullptr;
    QPushButton*  m_saveMarkdownButton = nullptr;
    QPushButton*  m_copyButton = nullptr;
    QTextBrowser* m_preview = nullptr;
    QLabel*       m_statusLabel = nullptr;
    QLabel*       m_headlineLabel = nullptr;
    QProgressBar* m_progress = nullptr;

    ReportResult m_result;
    bool m_hasReport = false;
    bool m_generating = false;
    QString m_preselectedDevice;
};

}   // namespace saim
