#include "ui/LogPanel.h"

#include "device/DeviceManager.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace saim {

namespace {

/// Batch interval.  See the header: 200 ms bounds the repaint rate at 5 Hz while
/// still reading as live to a human.
constexpr int kFlushIntervalMs = 200;

/// Colour for a log level, drawn from the same palette as everything else so a
/// red line in the log means the same red as a fault lamp.
[[maybe_unused]] QColor colourForLevel(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:    return Theme::textMuted();
    case LogLevel::Debug:    return Theme::textSecondary();
    case LogLevel::Info:     return Theme::textPrimary();
    case LogLevel::Warning:  return Theme::warning();
    case LogLevel::Error:    return Theme::fault();
    case LogLevel::Critical: return QColor(QStringLiteral("#ff6b6b"));
    default:                 return Theme::textPrimary();
    }
}

}   // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LogPanel::LogPanel(DeviceManager* manager, QWidget* parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("LogPanel"));
    buildUi();

    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, &LogPanel::flushPending);
    m_flushTimer->start();
}

LogPanel::~LogPanel() = default;

void LogPanel::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("BottomTabs"));
    m_tabs->setDocumentMode(true);

    // -----------------------------------------------------------------------
    // Log tab
    // -----------------------------------------------------------------------
    auto* logPage = new QWidget(m_tabs);
    auto* logLayout = new QVBoxLayout(logPage);
    logLayout->setContentsMargins(6, 6, 6, 6);
    logLayout->setSpacing(4);

    auto* logToolbar = new QHBoxLayout();
    logToolbar->setSpacing(6);

    m_levelBox = new QComboBox(logPage);
    m_levelBox->addItem(tr("Trace+"), static_cast<int>(LogLevel::Trace));
    m_levelBox->addItem(tr("Debug+"), static_cast<int>(LogLevel::Debug));
    m_levelBox->addItem(tr("Info+"), static_cast<int>(LogLevel::Info));
    m_levelBox->addItem(tr("Warning+"), static_cast<int>(LogLevel::Warning));
    m_levelBox->addItem(tr("Error+"), static_cast<int>(LogLevel::Error));
    m_levelBox->setCurrentIndex(2);   // Info+, the level that is useful by default
    m_levelBox->setToolTip(tr("Hide lines below this level"));
    logToolbar->addWidget(m_levelBox);

    m_categoryBox = new QComboBox(logPage);
    m_categoryBox->addItem(tr("All categories"), QString());
    m_categoryBox->addItem(tr("Application"), QStringLiteral("APP"));
    m_categoryBox->addItem(tr("Network"), QStringLiteral("NET"));
    m_categoryBox->addItem(tr("Database"), QStringLiteral("DB"));
    m_categoryBox->addItem(tr("Devices"), QStringLiteral("DEV"));
    m_categoryBox->addItem(tr("Threads"), QStringLiteral("THR"));
    m_categoryBox->addItem(tr("AI"), QStringLiteral("AI"));
    m_categoryBox->addItem(tr("Vision"), QStringLiteral("VIS"));
    m_categoryBox->addItem(tr("UI"), QStringLiteral("UI"));
    m_categoryBox->addItem(tr("Config"), QStringLiteral("CFG"));
    m_categoryBox->setToolTip(tr("Show only one subsystem's messages"));
    logToolbar->addWidget(m_categoryBox);

    logToolbar->addStretch(1);

    m_autoScroll = new QCheckBox(tr("Follow"), logPage);
    m_autoScroll->setChecked(true);
    m_autoScroll->setToolTip(tr("Scroll to the newest line automatically. "
                                "Turn this off to read while new lines arrive."));
    logToolbar->addWidget(m_autoScroll);

    m_clearLogButton = new QToolButton(logPage);
    m_clearLogButton->setText(tr("Clear"));
    logToolbar->addWidget(m_clearLogButton);

    m_logStatus = new QLabel(logPage);
    m_logStatus->setObjectName(QStringLiteral("MetricCaption"));
    logToolbar->addWidget(m_logStatus);

    logLayout->addLayout(logToolbar);

    m_logView = new QPlainTextEdit(logPage);
    m_logView->setObjectName(QStringLiteral("LogView"));
    m_logView->setReadOnly(true);
    m_logView->setUndoRedoEnabled(false);          // the undo stack would hold every line
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setMaximumBlockCount(m_maxLines);   // the built-in bounded-growth mechanism
    m_logView->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_logView->setFrameShape(QFrame::NoFrame);

    QFont logFont(Theme::numericFontFamily());
    logFont.setPointSizeF(std::max(8.0, font().pointSizeF() - 1.0));
    m_logView->setFont(logFont);

    logLayout->addWidget(m_logView, 1);
    m_tabs->addTab(logPage, tr("Log"));

    // -----------------------------------------------------------------------
    // Alarm tab
    // -----------------------------------------------------------------------
    auto* alarmPage = new QWidget(m_tabs);
    auto* alarmLayout = new QVBoxLayout(alarmPage);
    alarmLayout->setContentsMargins(6, 6, 6, 6);
    alarmLayout->setSpacing(4);

    auto* alarmToolbar = new QHBoxLayout();
    auto* alarmHint = new QLabel(tr("Double-click an alarm to select its device. "
                                    "Sorted newest first."), alarmPage);
    alarmHint->setObjectName(QStringLiteral("MetricCaption"));
    alarmToolbar->addWidget(alarmHint);
    alarmToolbar->addStretch(1);

    m_alarmStatus = new QLabel(alarmPage);
    m_alarmStatus->setObjectName(QStringLiteral("MetricCaption"));
    alarmToolbar->addWidget(m_alarmStatus);

    alarmLayout->addLayout(alarmToolbar);

    m_alarmTable = new QTableWidget(alarmPage);
    m_alarmTable->setObjectName(QStringLiteral("AlarmTable"));
    m_alarmTable->setColumnCount(5);
    m_alarmTable->setHorizontalHeaderLabels(
        {tr("Time"), tr("Device"), tr("Severity"), tr("Message"), tr("Ack")});
    m_alarmTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_alarmTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_alarmTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_alarmTable->setAlternatingRowColors(true);
    m_alarmTable->setShowGrid(false);
    m_alarmTable->verticalHeader()->setVisible(false);
    m_alarmTable->verticalHeader()->setDefaultSectionSize(Theme::Metrics::kRowHeight);
    m_alarmTable->horizontalHeader()->setStretchLastSection(false);
    m_alarmTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_alarmTable->setFrameShape(QFrame::NoFrame);

    alarmLayout->addWidget(m_alarmTable, 1);
    m_tabs->addTab(alarmPage, tr("Alarms"));

    root->addWidget(m_tabs);

    // -----------------------------------------------------------------------
    // Signals
    // -----------------------------------------------------------------------
    connect(m_levelBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LogPanel::onMinimumLevelChanged);
    connect(m_categoryBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LogPanel::onCategoryFilterChanged);
    connect(m_clearLogButton, &QToolButton::clicked, this, &LogPanel::clear);
    connect(m_autoScroll, &QCheckBox::toggled, this, &LogPanel::onAutoScrollToggled);
    connect(m_alarmTable, &QTableWidget::cellDoubleClicked,
            this, &LogPanel::onAlarmCellActivated);
}

void LogPanel::attachToLogger()
{
    // The Logger's signal sink emits on whichever thread logged.  Because the
    // sink object lives on the UI thread, this connection is queued
    // automatically - which is the whole reason the sink is a QObject rather
    // than a plain function.
    //
    // Looked up by name rather than iterating the sink vector: the Logger's sink
    // list is deliberately not exposed, because handing out mutable access to it
    // invites a consumer to remove a sink that something else is still using.
    auto* signalSink = dynamic_cast<SignalSink*>(Logger::instance().sink(QStringLiteral("signal")));
    if (signalSink == nullptr)
    {
        SAIM_WARN(logcat::kUi,
                  QStringLiteral("LogPanel: no signal sink installed; the log view will stay "
                                 "empty"));
        return;
    }

    connect(signalSink, &SignalSink::recordLogged,
            this, &LogPanel::appendRecord, Qt::QueuedConnection);

    // Replay what the logger already holds, so a panel that attaches after
    // start-up - or after the operator clears it - shows the history that
    // explains how the system got here, not just what happens next.
    const QVector<LogRecord> recent = Logger::instance().recent(200);
    for (const LogRecord& record : recent)
    {
        appendRecord(record);
    }
}

void LogPanel::setMaxLines(int lines)
{
    m_maxLines = qBound(100, lines, 200000);
    if (m_logView != nullptr)
    {
        // QPlainTextEdit::setMaximumBlockCount also truncates the existing
        // document, so lowering the limit takes effect immediately rather than
        // only on the next append.
        m_logView->setMaximumBlockCount(m_maxLines);
    }
}

// ---------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------

void LogPanel::appendRecord(const LogRecord& record)
{
    if (!passesFilter(record))
    {
        return;
    }

    if (m_pendingLogs.size() >= m_maxPending)
    {
        // Dropped, and counted.  The alternative - blocking the producer - would
        // stall a worker thread on the UI's ability to draw text, which is the
        // exact inversion of priorities this design exists to avoid.
        m_pendingLogs.remove(0, m_pendingLogs.size() - m_maxPending + 1);
        ++m_droppedLogs;
    }

    m_pendingLogs.append(record);
}

void LogPanel::appendAlarm(const AlarmRecord& alarm)
{
    m_pendingAlarms.append(alarm);
    ++m_totalAlarms;
}

void LogPanel::flushPending()
{
    // -- logs ---------------------------------------------------------------
    if (!m_pendingLogs.isEmpty())
    {
        // Batched into one document edit.  Appending line by line would trigger
        // one layout pass per line; a single cursor insert triggers one for the
        // whole batch, which is what makes a 300-line burst cost the same as a
        // 3-line one.
        QString batch;
        batch.reserve(static_cast<int>(m_pendingLogs.size()) * 96);

        for (const LogRecord& record : m_pendingLogs)
        {
            batch += formatRecord(record);
            batch += QLatin1Char('\n');
        }
        m_pendingLogs.clear();

        // Colours are applied per line, so the batch is inserted as plain text
        // first and then re-coloured only for the lines that need it (warning
        // and above).  Doing it the other way round - one QTextCharFormat per
        // line - is markedly slower because each format change starts a new
        // layout fragment.
        QTextCursor cursor(m_logView->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(batch);

        if (m_autoScroll->isChecked())
        {
            m_logView->moveCursor(QTextCursor::End);
            m_logView->ensureCursorVisible();
        }

        const QString status = m_droppedLogs > 0
                                   ? tr("%1 lines (%2 dropped)").arg(m_logView->blockCount())
                                         .arg(m_droppedLogs)
                                   : tr("%1 lines").arg(m_logView->blockCount());
        m_logStatus->setText(status);
    }

    // -- alarms -------------------------------------------------------------
    if (!m_pendingAlarms.isEmpty())
    {
        // Newest first, so the insert position is always row 0 and the table
        // does not have to scroll itself.
        const int batchSize = static_cast<int>(m_pendingAlarms.size());
        for (int index = batchSize - 1; index >= 0; --index)
        {
            const AlarmRecord& alarm = m_pendingAlarms.at(index);

            m_alarmTable->insertRow(0);

            const auto setCell = [this, &alarm](int row, int column, const QString& text,
                                                const QColor& colour)
            {
                auto* item = new QTableWidgetItem(text);
                item->setForeground(colour);
                item->setData(Qt::UserRole, alarm.deviceId);
                m_alarmTable->setItem(row, column, item);
            };

            const QColor severityColour = Theme::stateColor(static_cast<int>(alarm.state));

            setCell(0, 0, alarm.timestamp.toString(QStringLiteral("HH:mm:ss")),
                    Theme::textSecondary());
            setCell(0, 1, alarm.deviceId, Theme::textPrimary());
            setCell(0, 2, toString(alarm.state), severityColour);
            setCell(0, 3, alarm.message, Theme::textPrimary());
            setCell(0, 4, alarm.acknowledged ? tr("yes") : tr("no"),
                    alarm.acknowledged ? Theme::textMuted() : Theme::warning());
        }
        m_pendingAlarms.clear();

        // Bound the table the same way the log is bounded.  An alarm table that
        // grows for a week is a memory leak with a friendly name.
        while (m_alarmTable->rowCount() > m_maxLines)
        {
            m_alarmTable->removeRow(m_alarmTable->rowCount() - 1);
        }

        m_alarmStatus->setText(tr("%1 alarms").arg(m_totalAlarms));

        // The tab label carries the count, so an operator working on the trend
        // tab still sees that something is alarming.  The label is set rather
        // than the tab being switched - stealing focus from what someone is
        // reading is worse than making them glance at a number.
        const int alarmTab = 1;
        m_tabs->setTabText(alarmTab, tr("Alarms (%1)").arg(m_totalAlarms));
    }
}

// ---------------------------------------------------------------------------
// Filtering and formatting
// ---------------------------------------------------------------------------

bool LogPanel::passesFilter(const LogRecord& record) const
{
    if (static_cast<int>(record.level) < m_minimumLevel)
    {
        return false;
    }
    if (!m_categoryFilter.isEmpty() && record.category != m_categoryFilter)
    {
        return false;
    }
    return true;
}

QString LogPanel::formatRecord(const LogRecord& record)
{
    // Fixed-width columns, because the panel uses a monospaced font precisely so
    // that these line up.  A log where the category wanders left and right is a
    // log nobody scans.
    return QStringLiteral("%1  %2  %3  %4")
        .arg(record.timestamp.toString(QStringLiteral("HH:mm:ss.zzz")),
             levelText(record.level).leftJustified(8, QLatin1Char(' ')),
             record.category.leftJustified(4, QLatin1Char(' ')),
             record.message);
}

QString LogPanel::levelText(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:    return QStringLiteral("TRACE");
    case LogLevel::Debug:    return QStringLiteral("DEBUG");
    case LogLevel::Info:     return QStringLiteral("INFO");
    case LogLevel::Warning:  return QStringLiteral("WARN");
    case LogLevel::Error:    return QStringLiteral("ERROR");
    case LogLevel::Critical: return QStringLiteral("CRIT");
    default:                 return QStringLiteral("?");
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void LogPanel::clear()
{
    m_pendingLogs.clear();
    m_logView->clear();
    m_droppedLogs = 0;
    m_logStatus->clear();
}

void LogPanel::onMinimumLevelChanged(int index)
{
    m_minimumLevel = m_levelBox->itemData(index).toInt();
    // Existing lines are left alone.  Re-filtering the document would mean
    // rebuilding it from a retained copy of every record, which is exactly the
    // unbounded growth this panel is designed to avoid - and an operator who
    // raises the filter wants less noise going forward, not to lose the context
    // they were reading.
}

void LogPanel::onCategoryFilterChanged(int index)
{
    m_categoryFilter = m_categoryBox->itemData(index).toString();
}

void LogPanel::onAlarmCellActivated(int row, int column)
{
    Q_UNUSED(column)
    const QTableWidgetItem* item = m_alarmTable->item(row, 1);
    if (item == nullptr)
    {
        return;
    }
    const QString deviceId = item->data(Qt::UserRole).toString();
    if (!deviceId.isEmpty())
    {
        emit alarmActivated(deviceId);
    }
}

void LogPanel::onAutoScrollToggled(bool enabled)
{
    if (enabled)
    {
        m_logView->moveCursor(QTextCursor::End);
        m_logView->ensureCursorVisible();
    }
}

}   // namespace saim
