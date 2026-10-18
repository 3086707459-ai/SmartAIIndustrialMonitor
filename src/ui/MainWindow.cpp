#include "ui/MainWindow.h"

#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "device/DeviceManager.h"
#include "thread/AIWorker.h"
#include "ui/AIChatWidget.h"
#include "ui/CameraWidget.h"
#include "ui/DashboardWidget.h"
#include "ui/DeviceDetailPanel.h"
#include "ui/DeviceListPanel.h"
#include "ui/LogPanel.h"
#include "ui/ReportDialog.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>

namespace saim {

namespace {

/// Object names for the docks, so saveState() can restore a layout across
/// versions.  A renamed dock silently loses its saved geometry, which is why
/// these are constants rather than string literals at the call site.
constexpr auto kLogDockName   = "dock.logs";
constexpr auto kRightDockName = "dock.assistant";

constexpr auto kSettingsGroup = "MainWindow";
constexpr auto kGeometryKey   = "geometry";
constexpr auto kWindowStateKey = "windowState";

/// A coloured status-bar lamp, built the same way every time.
///
/// The dot is only ever a *second* channel - the text next to it always says the
/// same thing in words, because roughly one man in twelve cannot reliably
/// separate the red from the green.
QString lamp(const QColor& colour, const QString& text)
{
    return QStringLiteral("<span style='color:%1'>&#9679;</span>&nbsp;%2")
        .arg(colour.name(), text.toHtmlEscaped());
}

}   // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MainWindow::MainWindow(ConfigManager* config, DeviceManager* devices, QWidget* parent)
    : QMainWindow(parent)
    , m_config(config)
    , m_devices(devices)
{
    Q_ASSERT(config != nullptr);
    Q_ASSERT(devices != nullptr);

    setObjectName(QStringLiteral("MainWindow"));
    setWindowIcon(Theme::icon(QStringLiteral("app")));
    setDockNestingEnabled(true);

    // Widgets are built before the actions, because a couple of the actions need
    // to reach a panel for their initial checked state.  The dependency is one
    // way and deliberate.
    buildCentralArea();
    buildDocks();
    buildActions();
    buildMenus();
    buildToolBar();
    buildStatusBar();

    m_transientTimer = new QTimer(this);
    m_transientTimer->setSingleShot(true);
    connect(m_transientTimer, &QTimer::timeout, this, &MainWindow::onClearTransientMessage);

    restoreLayout();
    const auto ids = m_devices->deviceIds();
    if (!ids.isEmpty()) { m_deviceList->selectDevice(ids.front()); }

    SAIM_INFO(logcat::kUi, QStringLiteral("Main window constructed"));
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// Central area
// ---------------------------------------------------------------------------

void MainWindow::buildCentralArea()
{
    // -- left: the device list ---------------------------------------------
    m_deviceList = new DeviceListPanel(m_devices, this);

    // -- centre: tiles over a chart ----------------------------------------
    m_dashboard = new DashboardWidget(m_devices, this);

    // -- right of centre: the detail page, in a tab so the chart is not
    //    permanently squeezed on a 1366x768 panel.
    m_details = new DeviceDetailPanel(m_devices, this);

    m_rightTabs = new QTabWidget(this);
    m_rightTabs->setObjectName(QStringLiteral("CentreTabs"));
    m_rightTabs->setDocumentMode(true);
    m_rightTabs->addTab(m_dashboard, Theme::icon(QStringLiteral("app")), tr("Overview"));
    m_rightTabs->addTab(m_details, Theme::icon(QStringLiteral("device-motor")), tr("Device"));

    // A splitter rather than a fixed layout: on a plant PC the operator wants
    // the list narrow and the detail wide, on a laptop the reverse.  Qt's
    // splitter state is part of saveState(), so the choice survives a restart.
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("CentralSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(m_deviceList);
    splitter->addWidget(m_rightTabs);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({Theme::Metrics::kSidebarMinWidth, 900});

    setCentralWidget(splitter);

    // Selecting a device in the list brings the detail tab forward.  This is the
    // one piece of navigation the window performs, and it is presentation, not
    // logic - the *meaning* of the selection is published as a signal.
    connect(m_deviceList, &DeviceListPanel::deviceSelected,
            m_dashboard, &DashboardWidget::setSelectedDevice);
    connect(m_deviceList, &DeviceListPanel::deviceActivated, this,
            [this](const QString& deviceId)
            {
                m_details->setDevice(deviceId);
                m_rightTabs->setCurrentWidget(m_details);
            });
    connect(m_deviceList, &DeviceListPanel::deviceSelected, this,
            [this](const QString& deviceId)
            {
                // Keep the detail page on the same device the list has selected,
                // even if the operator is currently looking at the overview.
                m_details->setDevice(deviceId);
            });

    connect(m_dashboard, &DashboardWidget::deviceActivated, this,
            [this](const QString& deviceId)
            {
                m_deviceList->selectDevice(deviceId);
                m_details->setDevice(deviceId);
                m_rightTabs->setCurrentWidget(m_details);
            });
    connect(m_dashboard, &DashboardWidget::deviceSelected,
            m_deviceList, &DeviceListPanel::selectDevice);
}

// ---------------------------------------------------------------------------
// Docks
// ---------------------------------------------------------------------------

void MainWindow::buildDocks()
{
    // -- assistant + camera -------------------------------------------------
    m_assistant = new AIChatWidget(m_devices, this);
    m_camera    = new CameraWidget(this);

    auto* rightTabs = new QTabWidget(this);
    rightTabs->setObjectName(QStringLiteral("AssistantTabs"));
    rightTabs->setDocumentMode(true);
    rightTabs->addTab(m_assistant, Theme::icon(QStringLiteral("report")), tr("Assistant"));
    rightTabs->addTab(m_camera, Theme::icon(QStringLiteral("device-fan")), tr("Vision"));

    m_rightDock = new QDockWidget(tr("AI analysis"), this);
    m_rightDock->setObjectName(QString::fromLatin1(kRightDockName));
    m_rightDock->setWidget(rightTabs);
    m_rightDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_rightDock->setMinimumWidth(Theme::Metrics::kRightPanelMinWidth);
    addDockWidget(Qt::RightDockWidgetArea, m_rightDock);

    // -- logs ---------------------------------------------------------------
    m_logPanel = new LogPanel(m_devices, this);
    m_logPanel->attachToLogger();

    m_logDock = new QDockWidget(tr("Logs & alarms"), this);
    m_logDock->setObjectName(QString::fromLatin1(kLogDockName));
    m_logDock->setWidget(m_logPanel);
    m_logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);

    // -- panel-to-panel relaying --------------------------------------------
    // Double-clicking an alarm selects its device.  The window relays; it does
    // not decide that an alarm means anything - note there is no acknowledge,
    // no state change and no query here.
    connect(m_logPanel, &LogPanel::alarmActivated, this,
            [this](const QString& deviceId)
            {
                m_deviceList->selectDevice(deviceId);
                m_details->setDevice(deviceId);
                m_rightTabs->setCurrentWidget(m_details);
            });

    // The assistant naming a device moves the rest of the interface to it, so a
    // conversation and the dashboard never drift apart.
    connect(m_assistant, &AIChatWidget::deviceReferenced, this,
            [this](const QString& deviceId)
            {
                m_deviceList->selectDevice(deviceId);
                m_details->setDevice(deviceId);
            });
    connect(m_deviceList, &DeviceListPanel::deviceSelected,
            m_assistant, &AIChatWidget::setContextDevice);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

QAction* MainWindow::makeAction(const QString& text, const QString& iconName,
                                const QKeySequence& shortcut, const QString& tip)
{
    auto* action = new QAction(Theme::icon(iconName), text, this);
    if (!shortcut.isEmpty())
    {
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::ApplicationShortcut);
    }
    if (!tip.isEmpty())
    {
        action->setToolTip(tip);
        action->setStatusTip(tip);
    }
    return action;
}

void MainWindow::buildActions()
{
    m_actStartStop = makeAction(tr("&Start acquisition"), QStringLiteral("app"),
                                QKeySequence(Qt::Key_F5),
                                tr("Start or stop the telemetry stream from the gateway"));
    // Checkable, but the *request* is what the toggle emits.  The button's state
    // is then published back by setAcquisitionRunning() from what actually
    // happened, so a connection that fails to open leaves the button showing
    // "stopped" rather than lying about a link that does not exist.
    m_actStartStop->setCheckable(true);

    m_actReconnect = makeAction(tr("&Reconnect"), QStringLiteral("device-fan"),
                                QKeySequence(Qt::CTRL | Qt::Key_R),
                                tr("Drop the current session and reconnect immediately"));

    m_actAnalyseAll = makeAction(tr("Analyse &all devices"), QStringLiteral("report"),
                                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
                                 tr("Run an immediate inference on every device, "
                                    "ignoring the scheduled cadence"));

    m_actReport = makeAction(tr("&Maintenance report…"), QStringLiteral("report"),
                             QKeySequence(Qt::CTRL | Qt::Key_E),
                             tr("Open the report dialog for the whole fleet"));

    m_actExport = makeAction(tr("&Export report to file"), QStringLiteral("report"),
                             QKeySequence(),
                             tr("Generate the fleet report and write the PDF and HTML"));

    m_actSimulate = makeAction(tr("&Simulate a fault"), QStringLiteral("alarm"),
                               QKeySequence(Qt::CTRL | Qt::Key_F),
                               tr("Drive a device into a sustained fault so the whole "
                                  "alarm path can be observed end to end"));

    m_actVision = makeAction(tr("&Vision analysis"), QStringLiteral("device-fan"),
                             QKeySequence(Qt::CTRL | Qt::Key_V),
                             tr("Start or stop periodic camera analysis"));
    m_actVision->setCheckable(true);

    m_actTheme = makeAction(tr("&Reload theme"), QStringLiteral("app"),
                            QKeySequence(Qt::Key_F6),
                            tr("Re-read the stylesheet from disk without restarting"));

    m_actSaveLayout = makeAction(tr("&Save window layout"), QStringLiteral("app"),
                                 QKeySequence(),
                                 tr("Remember the current dock and splitter arrangement"));

    m_actResetLayout = makeAction(tr("Reset window layout"), QStringLiteral("app"),
                                  QKeySequence(),
                                  tr("Return every panel to its default position"));

    // -- wiring: one line each, no bodies -----------------------------------
    connect(m_actStartStop,  &QAction::toggled, this, &MainWindow::onToggleAcquisition);
    connect(m_actReconnect,  &QAction::triggered, this,
            [this] { emit reconnectRequested(); });
    connect(m_actAnalyseAll, &QAction::triggered, this, &MainWindow::onAnalyseAllTriggered);
    connect(m_actReport,     &QAction::triggered, this, &MainWindow::onReportTriggered);
    connect(m_actExport,     &QAction::triggered, this, &MainWindow::onExportTriggered);
    connect(m_actSimulate,   &QAction::triggered, this, &MainWindow::onSimulateFaultFromMenu);
    connect(m_actVision,     &QAction::toggled, this, &MainWindow::onToggleVision);
    connect(m_actTheme,      &QAction::triggered, this, &MainWindow::onThemeTriggered);
    connect(m_actSaveLayout, &QAction::triggered, this, &MainWindow::onSaveLayoutTriggered);
    connect(m_actResetLayout,&QAction::triggered, this, &MainWindow::onResetLayoutTriggered);

    // -- panel actions that must surface in the menu bar --------------------
    connect(m_details, &DeviceDetailPanel::analyseRequested, this,
            [this](const QString& deviceId) { emit reportRequested(deviceId); });
    connect(m_details, &DeviceDetailPanel::simulateFaultRequested,
            this, &MainWindow::simulateFaultRequested);
    connect(m_details, &DeviceDetailPanel::reportRequested, this,
            [this](const QString& deviceId) { emit reportRequested(deviceId); });

    connect(m_camera, &CameraWidget::captureRequested,
            this, &MainWindow::visionCaptureRequested);
    connect(m_camera, &CameraWidget::captureRateChanged,
            this, &MainWindow::visionRateChanged);
}

void MainWindow::buildMenus()
{
    // -- File ---------------------------------------------------------------
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_actStartStop);
    fileMenu->addAction(m_actReconnect);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actReport);
    fileMenu->addAction(m_actExport);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actSaveLayout);
    fileMenu->addAction(m_actResetLayout);
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // -- Analysis -----------------------------------------------------------
    auto* analysisMenu = menuBar()->addMenu(tr("&Analysis"));
    analysisMenu->addAction(m_actAnalyseAll);
    analysisMenu->addAction(m_actVision);
    analysisMenu->addSeparator();
    analysisMenu->addAction(m_actSimulate);

    // -- View ---------------------------------------------------------------
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_actTheme);
    viewMenu->addSeparator();
    viewMenu->addAction(m_logDock->toggleViewAction());
    viewMenu->addAction(m_rightDock->toggleViewAction());

    // -- Help ---------------------------------------------------------------
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* aboutAction = helpMenu->addAction(tr("&About…"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutTriggered);
}

void MainWindow::buildToolBar()
{
    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName(QStringLiteral("MainToolBar"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    toolbar->addAction(m_actStartStop);
    toolbar->addAction(m_actReconnect);
    toolbar->addSeparator();
    toolbar->addAction(m_actAnalyseAll);
    toolbar->addAction(m_actReport);
    toolbar->addSeparator();
    toolbar->addAction(m_actVision);
    toolbar->addAction(m_actSimulate);
}

void MainWindow::buildStatusBar()
{
    // The status bar is built left to right in decreasing order of importance:
    // link, throughput, database, inference, alarms.  An operator glancing at it
    // gets the answer to "is the data flowing" before anything else.
    m_connectionLabel = new QLabel(this);
    m_connectionLabel->setTextFormat(Qt::RichText);
    m_connectionLabel->setMinimumWidth(180);

    m_throughputLabel = new QLabel(this);
    m_throughputLabel->setTextFormat(Qt::PlainText);
    m_throughputLabel->setMinimumWidth(190);

    m_databaseLabel = new QLabel(this);
    m_databaseLabel->setTextFormat(Qt::RichText);
    m_databaseLabel->setMinimumWidth(180);

    m_aiLabel = new QLabel(this);
    m_aiLabel->setTextFormat(Qt::PlainText);
    m_aiLabel->setMinimumWidth(220);

    m_alarmLabel = new QLabel(this);
    m_alarmLabel->setTextFormat(Qt::RichText);

    statusBar()->addWidget(m_connectionLabel);
    statusBar()->addWidget(m_throughputLabel);
    statusBar()->addWidget(m_databaseLabel);
    statusBar()->addWidget(m_aiLabel);
    statusBar()->addPermanentWidget(m_alarmLabel);

    m_transientLabel = new QLabel(this);
    m_transientLabel->setTextFormat(Qt::PlainText);
    statusBar()->addPermanentWidget(m_transientLabel, 1);

    // The window title names the data root because it is the directory the
    // database, the logs and every exported report land in - and "which install
    // am I looking at" is the first question asked when two copies are open.
    setWindowTitle(tr("SmartAI Industrial Monitor — %1").arg(m_config->dataRoot()));

    setConnectionState(ConnectionState::Disconnected, QString());
    setThroughput(0.0, 0);
    setDatabaseStatus(false, tr("not opened"));
    setAiStatus(tr("starting…"), false);
    setAlarmCount(0);
}

// ---------------------------------------------------------------------------
// Layout persistence
// ---------------------------------------------------------------------------

void MainWindow::restoreLayout()
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));

    const QByteArray geometry = settings.value(QString::fromLatin1(kGeometryKey)).toByteArray();
    if (!geometry.isEmpty() && restoreGeometry(geometry))
    {
        // Geometry restored; the size the user chose is the size they want.
    }
    else
    {
        resize(1600, 950);
        // Centre on the primary screen.  A window that opens half off-screen is
        // the first thing an operator notices and the last thing they forgive.
        if (QScreen* screen = QGuiApplication::primaryScreen())
        {
            const QRect available = screen->availableGeometry();
            move(available.center() - rect().center());
        }
    }

    const QByteArray windowState = settings.value(QString::fromLatin1(kWindowStateKey)).toByteArray();
    if (!windowState.isEmpty())
    {
        restoreState(windowState);
    }
    else
    {
        // A first run should look deliberate rather than like Qt's defaults:
        // logs get about a fifth of the height, which fits a screenful without
        // stealing the chart.
        resizeDocks({m_logDock}, {height() / 5}, Qt::Vertical);
    }
}

void MainWindow::onSaveLayoutTriggered()
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    settings.setValue(QString::fromLatin1(kGeometryKey), saveGeometry());
    settings.setValue(QString::fromLatin1(kWindowStateKey), saveState());
    settings.endGroup();

    showTransientMessage(tr("Window layout saved."));
}

void MainWindow::onResetLayoutTriggered()
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    settings.remove(QString());   // the whole group: geometry and state together
    settings.endGroup();

    // Restoring from an empty store is exactly the first-run path, so resetting
    // and starting fresh cannot drift apart.
    restoreLayout();
    showTransientMessage(tr("Window layout reset to the default arrangement."));
}

// ---------------------------------------------------------------------------
// Slots: inbound display state
// ---------------------------------------------------------------------------

void MainWindow::setConnectionState(ConnectionState state, const QString& endpoint)
{
    const QColor colour = state == ConnectionState::Connected     ? Theme::normal()
                          : state == ConnectionState::Connecting  ? Theme::warning()
                          : state == ConnectionState::Reconnecting ? Theme::warning()
                                                                   : Theme::offline();
    const QString text = state == ConnectionState::Connected     ? tr("Connected")
                         : state == ConnectionState::Connecting  ? tr("Connecting")
                         : state == ConnectionState::Reconnecting ? tr("Reconnecting")
                                                                  : tr("Disconnected");

    const QString suffix = endpoint.isEmpty() ? QString() : QStringLiteral("  ") + endpoint;
    m_connectionLabel->setText(lamp(colour, text + suffix));
}

void MainWindow::setThroughput(double samplesPerSecond, quint64 totalSamples)
{
    m_throughputLabel->setText(tr("%1 samples/s   %2 total")
                                   .arg(samplesPerSecond, 0, 'f', 1)
                                   .arg(totalSamples));
}

void MainWindow::setDatabaseStatus(bool open, const QString& description)
{
    m_databaseLabel->setText(lamp(open ? Theme::normal() : Theme::offline(),
                                  tr("Database: %1").arg(description)));
}

void MainWindow::setAiStatus(const QString& backend, bool neural)
{
    // The distinction that matters is "a model produced this" versus "a
    // heuristic produced this".  It is stated in words, in every mode.
    const QString kind = neural ? tr("neural (ONNX)") : tr("heuristic fallback");
    m_aiLabel->setText(tr("Inference: %1 · %2").arg(backend, kind));
    m_aiLabel->setStyleSheet(QStringLiteral("color: %1;")
                                 .arg((neural ? Theme::textPrimary()
                                              : Theme::textSecondary()).name()));
}

void MainWindow::setAlarmCount(int unacknowledged)
{
    if (unacknowledged <= 0)
    {
        m_alarmLabel->setText(lamp(Theme::normal(), tr("No active alarms")));
        return;
    }
    m_alarmLabel->setText(lamp(Theme::fault(), tr("%1 unacknowledged").arg(unacknowledged)));
}

void MainWindow::showTransientMessage(const QString& message, int milliseconds)
{
    m_transientLabel->setText(message);
    m_transientTimer->start(milliseconds);
}

void MainWindow::onClearTransientMessage()
{
    m_transientLabel->clear();
}

// ---------------------------------------------------------------------------
// Slots: reporting
// ---------------------------------------------------------------------------

void MainWindow::setReportScope(const QVector<QPair<QString, QString>>& devices)
{
    if (m_reportDialog == nullptr)
    {
        m_reportDialog = new ReportDialog(this);

        // The dialog's request goes out as the window's signal; the window does
        // not run the query and does not know where the answer comes from.
        connect(m_reportDialog, &ReportDialog::reportRequested, this,
                [this](const QString& deviceId, const QDateTime& from, const QDateTime& to,
                       bool exportToFile)
                {
                    Q_UNUSED(from)
                    Q_UNUSED(to)
                    if (exportToFile)
                    {
                        emit reportExportRequested(deviceId, true);
                    }
                    else
                    {
                        emit reportRequested(deviceId);
                    }
                });
    }

    m_reportDialog->setDeviceList(devices);
}

void MainWindow::onReportTriggered()
{
    if (m_reportDialog == nullptr)
    {
        setReportScope({});
    }
    m_reportDialog->selectDevice(QString());   // whole fleet
    m_reportDialog->show();
    m_reportDialog->raise();
    m_reportDialog->activateWindow();
}

void MainWindow::onExportTriggered()
{
    if (m_reportDialog == nullptr)
    {
        setReportScope({});
    }
    m_reportDialog->selectDevice(m_deviceList != nullptr ? m_deviceList->selectedDeviceId()
                                                         : QString());
    m_reportDialog->show();
    m_reportDialog->raise();
    m_reportDialog->activateWindow();
}

void MainWindow::showReport(const ReportResult& result)
{
    if (m_reportDialog == nullptr)
    {
        // A report can arrive before anyone opened the dialog - the export path
        // is reachable from the toolbar.  Creating it here means the result is
        // never dropped on the floor.
        setReportScope({});
    }
    m_reportDialog->setReport(result);
    m_reportDialog->show();
    m_reportDialog->raise();
    m_reportDialog->activateWindow();
}

void MainWindow::showReportError(const QString& message)
{
    if (m_reportDialog == nullptr || !m_reportDialog->isVisible())
    {
        // No dialog open: a modal box is the appropriate way to say the operator
        // asked for something and it failed.
        QMessageBox::warning(this, tr("Report generation failed"), message);
        return;
    }
    m_reportDialog->setError(message);
}

void MainWindow::setReportGenerating(bool generating)
{
    if (m_reportDialog != nullptr)
    {
        m_reportDialog->setGenerating(generating);
    }
}

// ---------------------------------------------------------------------------
// Slots: operator actions
// ---------------------------------------------------------------------------

void MainWindow::onToggleAcquisition()
{
    emit acquisitionToggleRequested(m_actStartStop->isChecked());
}

void MainWindow::onToggleVision()
{
    // The action is the source of truth for the *request*; setVisionRunning()
    // is what reflects what actually happened.  The two can disagree briefly,
    // which is correct - the worker may refuse to start the camera.
    emit visionToggleRequested(m_actVision->isChecked());
}

void MainWindow::onAnalyseAllTriggered()
{
    emit analyseAllRequested();
    showTransientMessage(tr("Requested an immediate analysis of every device."));
}

void MainWindow::onSimulateFaultFromMenu()
{
    // An empty id means "whichever device the operator is looking at", and the
    // controller resolves it.  Doing that resolution here would require the
    // window to know about selection semantics, which is exactly the kind of
    // logic this class is supposed to be free of.
    emit simulateFaultRequested(m_deviceList != nullptr ? m_deviceList->selectedDeviceId()
                                                        : QString());
    showTransientMessage(tr("Simulated fault injected; watch the alarm and AI panels."));
}

void MainWindow::onThemeTriggered()
{
    if (Theme::reload(*qApp))
    {
        showTransientMessage(tr("Stylesheet reloaded."));
        return;
    }
    showTransientMessage(tr("Could not reload the stylesheet; the current one is unchanged."));
}

void MainWindow::onAboutTriggered()
{
    QMessageBox::about(
        this, tr("About SmartAI Industrial Monitor"),
        tr("<h3>SmartAI Industrial Monitor %1</h3>"
           "<p>An industrial device monitoring and AI fault-diagnosis platform built on "
           "Qt %2 and C++17.</p>"
           "<p><b>Data path:</b> TCP gateway &rarr; framing &rarr; domain model &rarr; "
           "SQLite and ONNX inference, each stage on its own thread.</p>"
           "<p><b>Where the numbers come from:</b> every verdict shows which backend "
           "produced it. A neural model and the heuristic fallback are never presented "
           "as if they were the same thing.</p>")
            .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}

// ---------------------------------------------------------------------------
// State reflection
// ---------------------------------------------------------------------------

void MainWindow::setAcquisitionRunning(bool running)
{
    m_acquisitionRunning = running;

    // setChecked() emits toggled(), which would emit the request straight back.
    // Blocking the signal here is what breaks that loop; the alternative - a
    // guard flag in the slot - is the same trick with more state to keep right.
    QSignalBlocker blocker(m_actStartStop);
    m_actStartStop->setChecked(running);
    m_actStartStop->setText(running ? tr("&Stop acquisition") : tr("&Start acquisition"));

    m_actReconnect->setEnabled(running);
}

void MainWindow::setVisionRunning(bool running)
{
    QSignalBlocker blocker(m_actVision);
    m_actVision->setChecked(running);
    m_actVision->setText(running ? tr("&Vision analysis (on)") : tr("&Vision analysis"));
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_shuttingDown)
    {
        event->accept();
        return;
    }

    // The layout is saved on close rather than only from the menu item: an
    // operator who arranges the panels and then quits has expressed the same
    // intent as one who presses Save layout, and should not have to discover
    // that there is a menu item for it.
    onSaveLayoutTriggered();

    m_shuttingDown = true;

    // The controller stops every worker synchronously before it lets the window
    // go.  Letting closeEvent return before that has happened would leave queued
    // signals in flight towards widgets that are already being destroyed.
    emit shutdownRequested();

    event->accept();
}

}   // namespace saim
