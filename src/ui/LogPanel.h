#pragma once

// ---------------------------------------------------------------------------
// LogPanel.h - the bottom status strip: live log lines and raised alarms.
//
// The problem this panel exists to solve is not "display logs"
// -----------------------------------------------------------
// It is that a log view is the easiest thing in a Qt application to make
// catastrophically slow.  The naive version - one QString appended to a
// QPlainTextEdit per record - has three distinct failure modes:
//
//   1. Every append re-lays-out the document.  A log storm (a reconnect loop
//      retrying twice a second, a database error on every sample) turns that
//      into the single largest CPU consumer in the process, and it is on the
//      UI thread.
//
//   2. Unbounded growth.  A QString in a text document is copied on every
//      layout pass, so an hour of logging is not "a lot of text", it is O(n)
//      work per line for the rest of the session.
//
//   3. Cross-thread delivery.  Log records are produced on four worker threads.
//      Emitting one queued signal per record means the UI event queue becomes
//      the bottleneck, and the queue growing faster than it drains is how a Qt
//      application dies with no crash and no error.
//
// So this panel does three things a naive one does not:
//
//   * It **coalesces**: incoming records go into a bounded ring, and a 200 ms
//     timer publishes them in one batch.  200 ms is below the threshold at which
//     a human reads a log as "live", and it bounds the repaint rate to 5 Hz no
//     matter how loud the source is.
//
//   * It **keeps the drop count visible**.  When the ring overflows, the panel
//     says how many lines were dropped rather than silently lying about being
//     complete.  A log view that quietly discards half its input is worse than
//     no log view, because it makes a post-mortem wrong.
//
//   * It uses a **QPlainTextEdit**, not a QTextEdit.  QPlainTextEdit exists
//     precisely for append-heavy, line-oriented text: it lays out only the
//     visible blocks, so appending is O(1) in the document length rather than
//     O(n).
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "core/Logger.h"

#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTableWidget;
class QTimer;
class QToolButton;

namespace saim {

class DeviceManager;

class LogPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit LogPanel(DeviceManager* manager, QWidget* parent = nullptr);
    ~LogPanel() override;

    /// Subscribes to the process-wide Logger's signal sink.  Separate from the
    /// constructor so a test can build the panel without a logger.
    void attachToLogger();

    /// Caps the retained line count.  Older lines are removed from the top, so
    /// the document cannot grow without bound across a long shift.
    void setMaxLines(int lines);

public slots:
    /// Queues a record for the next batch.
    void appendRecord(const saim::LogRecord& record);

    /// Queues an alarm for the alarm tab and flashes the tab label.
    void appendAlarm(const saim::AlarmRecord& alarm);

    /// Publishes everything queued so far.  Called by the internal timer; public
    /// so a test can drive it deterministically without waiting on wall-clock.
    void flushPending();

    void clear();

signals:
    /// The operator clicked an alarm row; the controller selects that device.
    void alarmActivated(const QString& deviceId);

    /// The operator asked to acknowledge every alarm for a device.
    void acknowledgeRequested(const QString& deviceId);

private slots:
    void onMinimumLevelChanged(int index);
    void onCategoryFilterChanged(int index);
    void onAlarmCellActivated(int row, int column);
    void onAutoScrollToggled(bool enabled);

private:
    void buildUi();
    [[nodiscard]] bool passesFilter(const LogRecord& record) const;
    [[nodiscard]] static QString formatRecord(const LogRecord& record);
    [[nodiscard]] static QString levelText(LogLevel level);

    DeviceManager* m_manager = nullptr;

    QTabWidget*     m_tabs = nullptr;

    // -- log tab ------------------------------------------------------------
    QPlainTextEdit* m_logView = nullptr;
    QComboBox*      m_levelBox = nullptr;
    QComboBox*      m_categoryBox = nullptr;
    QCheckBox*      m_autoScroll = nullptr;
    QToolButton*    m_clearLogButton = nullptr;
    QLabel*         m_logStatus = nullptr;

    // -- alarm tab ----------------------------------------------------------
    QTableWidget* m_alarmTable = nullptr;
    QLabel*       m_alarmStatus = nullptr;

    QTimer* m_flushTimer = nullptr;

    /// Bounded staging buffers.  A ring would be better for the log, but the
    /// volume that matters here is a burst, not a steady stream, and a vector
    /// with an explicit cap handles that with far less code.
    QVector<LogRecord>   m_pendingLogs;
    QVector<AlarmRecord> m_pendingAlarms;

    int m_maxLines = 5000;
    int m_maxPending = 2000;
    qint64 m_droppedLogs = 0;
    qint64 m_totalAlarms = 0;
    int m_minimumLevel = static_cast<int>(LogLevel::Info);
    QString m_categoryFilter;
};

}   // namespace saim
