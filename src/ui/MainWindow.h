#pragma once

// ---------------------------------------------------------------------------
// MainWindow.h - the shell: menus, toolbars, docks, status bar.  Nothing else.
//
// The rule this file exists to satisfy
// ------------------------------------
// "No logic in MainWindow.cpp" is one of those rules that is easy to state and
// easy to violate accidentally - every feature starts as "just one more slot in
// the window class".  So the split here is mechanical and easy to check:
//
//   * MainWindow *builds* widgets and *relays* signals.  That is all it does.
//   * It never queries a device, opens a socket, runs a query or calls a model.
//   * Every action it offers is re-emitted as a signal.  The composition root
//     (ApplicationController) decides what that signal means.
//
// You can verify the rule by reading this file's signal list: it is exactly the
// set of things the operator can ask for, and not one of them is implemented
// here.  A reviewer asking "where does the acquisition actually start?" gets
// pointed at the controller rather than at a 2000-line window class.
//
// Collaborators are injected, not created
// ---------------------------------------
// The panels need a DeviceManager and a ConfigManager.  Both are handed to the
// constructor, which only forwards them to the panels that need them.  A window
// that constructs its own dependencies cannot be opened twice, cannot be tested
// with a stub registry, and quietly becomes the owner of the application's state.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QKeySequence>
#include <QMainWindow>
#include <QString>
#include <QVector>

class QAction;
class QDockWidget;
class QLabel;
class QTabWidget;
class QTimer;

namespace saim {

class AIChatWidget;
class CameraWidget;
class ConfigManager;
class DashboardWidget;
class DeviceDetailPanel;
class DeviceListPanel;
class DeviceManager;
class LogPanel;
class ReportDialog;

struct ReportResult;
struct AnomalyEvent;
struct FaultPrediction;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(ConfigManager* config, DeviceManager* devices,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // -- panel access --------------------------------------------------------
    //
    // The controller drives the panels through these.  Exposing the panels is
    // the lesser evil compared to adding a forwarding method here for every slot
    // on every widget: the forwarders would be the beginning of the logic this
    // class is supposed to not have.

    [[nodiscard]] DeviceListPanel*   deviceList() const noexcept { return m_deviceList; }
    [[nodiscard]] DashboardWidget*   dashboard()  const noexcept { return m_dashboard; }
    [[nodiscard]] DeviceDetailPanel* details()    const noexcept { return m_details; }
    [[nodiscard]] AIChatWidget*      assistant()  const noexcept { return m_assistant; }
    [[nodiscard]] CameraWidget*      camera()     const noexcept { return m_camera; }
    [[nodiscard]] LogPanel*          logPanel()   const noexcept { return m_logPanel; }

public slots:
    // -- inbound state, all push-only ---------------------------------------
    //
    // Every one of these is a *display* update.  None of them reads anything
    // back, which is what keeps the window from becoming a place where state
    // lives and therefore a place where decisions get made.

    /// Connection lamp and endpoint text in the status bar.
    void setConnectionState(saim::ConnectionState state, const QString& endpoint);

    /// Throughput readout, driven by the acquisition worker's statistics.
    void setThroughput(double samplesPerSecond, quint64 totalSamples);

    /// Database lamp and the path/row count the persistence worker reports.
    void setDatabaseStatus(bool open, const QString& description);

    /// Which inference backend is live, so the operator knows whether a verdict
    /// came from a model or from the heuristic fallback.
    void setAiStatus(const QString& backend, bool neural);

    /// Unacknowledged alarm count in the status bar.
    void setAlarmCount(int unacknowledged);

    /// One-line transient note in the status bar (a save that succeeded, a
    /// device that was acknowledged).  Auto-clears, so it cannot go stale.
    void showTransientMessage(const QString& message, int milliseconds = 4000);

    /// Routes a finished report into the report dialog, opening it if needed.
    void showReport(const saim::ReportResult& result);
    void showReportError(const QString& message);
    void setReportGenerating(bool generating);

    /// Populates the report dialog's scope selector.
    void setReportScope(const QVector<QPair<QString, QString>>& devices);

    /// Reflects the acquisition state onto the menu/toolbar actions, which is
    /// the only stateful thing this class does - and it is stateful about its
    /// own widgets, not about the application.
    void setAcquisitionRunning(bool running);
    void setVisionRunning(bool running);

signals:
    // -- operator intent, all of it implemented elsewhere -------------------

    void acquisitionToggleRequested(bool start);
    void reconnectRequested();

    /// Fleet-wide report, and one for a named device (empty id = whole fleet).
    void reportRequested(const QString& deviceId);
    void reportExportRequested(const QString& deviceId, bool exportToFile);

    /// "Analyse every device now", as opposed to the per-device button.
    void analyseAllRequested();

    void visionToggleRequested(bool enabled);

    /// Camera panel wants a frame or a different analysis rate.
    void visionCaptureRequested();
    void visionRateChanged(int fps);

    /// Simulator controls, so a reviewer can drive the plant from the UI.
    void simulateFaultRequested(const QString& deviceId);
    void clearWearRequested(const QString& deviceId);

    void themeReloadRequested();
    void aboutRequested();

    /// The window is closing; the controller must stop every worker before the
    /// widgets go away, otherwise a queued signal arrives at a deleted receiver.
    void shutdownRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onAboutTriggered();
    void onThemeTriggered();
    void onToggleAcquisition();
    void onToggleVision();
    void onReportTriggered();
    void onExportTriggered();
    void onAnalyseAllTriggered();
    void onSimulateFaultFromMenu();
    void onSaveLayoutTriggered();
    void onResetLayoutTriggered();
    void onClearTransientMessage();

private:
    void buildCentralArea();
    void buildDocks();
    void buildActions();
    void buildMenus();
    void buildToolBar();
    void buildStatusBar();
    void restoreLayout();

    /// The rule for every action in one place: one QAction, one signal, no body.
    /// Written as a helper so that adding an action cannot accidentally add a
    /// second place where behaviour lives.
    [[nodiscard]] QAction* makeAction(const QString& text, const QString& iconName,
                                      const QKeySequence& shortcut, const QString& tip);

    ConfigManager* m_config  = nullptr;
    DeviceManager* m_devices = nullptr;

    // -- central area -------------------------------------------------------
    DeviceListPanel*   m_deviceList = nullptr;
    DashboardWidget*   m_dashboard  = nullptr;
    DeviceDetailPanel* m_details    = nullptr;
    QTabWidget*        m_rightTabs  = nullptr;

    // -- docks --------------------------------------------------------------
    AIChatWidget* m_assistant = nullptr;
    CameraWidget* m_camera    = nullptr;
    LogPanel*     m_logPanel  = nullptr;

    QDockWidget* m_logDock    = nullptr;
    QDockWidget* m_rightDock  = nullptr;

    // -- actions ------------------------------------------------------------
    QAction* m_actStartStop   = nullptr;
    QAction* m_actReconnect   = nullptr;
    QAction* m_actReport      = nullptr;
    QAction* m_actExport      = nullptr;
    QAction* m_actAnalyseAll  = nullptr;
    QAction* m_actSimulate    = nullptr;
    QAction* m_actVision      = nullptr;
    QAction* m_actTheme       = nullptr;
    QAction* m_actSaveLayout  = nullptr;
    QAction* m_actResetLayout = nullptr;

    // -- status bar ---------------------------------------------------------
    QLabel* m_connectionLabel = nullptr;
    QLabel* m_throughputLabel = nullptr;
    QLabel* m_databaseLabel   = nullptr;
    QLabel* m_aiLabel         = nullptr;
    QLabel* m_alarmLabel      = nullptr;
    QLabel* m_transientLabel  = nullptr;
    QTimer* m_transientTimer  = nullptr;

    ReportDialog* m_reportDialog = nullptr;

    bool m_acquisitionRunning = false;
    bool m_shuttingDown       = false;
};

}   // namespace saim
