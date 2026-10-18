#pragma once
#include "common/Result.h"
#include "network/TcpClient.h"
#include "database/DatabaseManager.h"
#include "thread/AIWorker.h"

// ---------------------------------------------------------------------------
// ApplicationController.h - the composition root.
//
// The one place in the application where it is allowed to be obvious what
// depends on what.
//
// Why this is its own layer and not part of saim_thread
// ----------------------------------------------------
// It is genuinely tempting to put this in the thread layer - it does own
// threads.  It cannot go there, and the reason is a single link: the controller
// touches MainWindow, and MainWindow is UI.  The moment `saim_thread` links
// `saim_ui`, the dependency direction is broken for the whole project and the
// guarantee that "the networking layer cannot reach a widget" stops being
// enforced by the build and becomes a convention again.
//
// So this target sits *above* both, links both, and is linked by exactly one
// translation unit: main.cpp.  Nothing else in the tree may include this header,
// and that is checkable with a grep.
//
// Why a controller instead of a fat ApplicationManager
// ---------------------------------------------------
// "ApplicationManager" sounds like it should exist, and in most Qt projects it
// turns into the class that owns the config, the database, the socket, the
// workers *and* the main window - which is a second, undeclared god object
// sitting next to MainWindow.  This one is deliberately narrower: it does not
// implement anything.  It reads as a wiring diagram in code, and every
// interesting behaviour lives in the class that owns it.
//
// What it does own, and why those things and no others
// ---------------------------------------------------
//   * Worker and thread lifetime.  Nobody else can be allowed to start or stop
//     a worker, because the order matters and a stray start() is the bug that
//     produces a hang on exit.
//   * The UI-side coalescing timers.  Something has to decide that the status
//     bar updates twice a second rather than twenty times, and that decision
//     belongs next to the wiring, not inside a widget.
//   * The translation from "the operator asked for X" to "these worker calls
//     are made, in this order".  That is a policy, and it has exactly one
//     sensible home.
//
// What it deliberately does NOT do
// --------------------------------
// It paints nothing, queries nothing, and computes nothing.  There is no
// arithmetic in this file beyond counter increments.  If a future change needs
// arithmetic here, that is the signal that a class further down the stack is
// missing a method.
//
// Shutdown, spelled out
// ---------------------
// The destructor order is the whole reason this class exists:
//
//   1. stop the workers, in WorkerPool's documented order, and wait for the
//      threads to actually finish (not merely be asked to)
//   2. stop the in-process simulator
//   3. only then let the widgets be destroyed
//
// Reversing 1 and 3 - which is what happens when the main window owns the
// workers - means a queued `sampleAcquired` can be delivered to a Device whose
// DeviceManager is already gone.  That crash appears only under load, only on
// exit, and never on the developer's machine.
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "thread/WorkerPool.h"
#include "thread/LatestMailbox.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QTimer;

namespace saim {

class ConfigManager;
class DeviceManager;
class MainWindow;
class TcpServer;

class ApplicationController final : public QObject
{
    Q_OBJECT
public:
    /// Builds nothing.  Construction of the model and the workers happens in
    /// initialise(), so that a test can construct the controller without a
    /// database file, a socket or a window.
    explicit ApplicationController(ConfigManager* config, QObject* parent = nullptr);
    ~ApplicationController() override;

    ApplicationController(const ApplicationController&)            = delete;
    ApplicationController& operator=(const ApplicationController&) = delete;

    /// Creates the device registry, the worker pool and (when simulation is
    /// enabled) the in-process gateway.  Does not start any thread.
    [[nodiscard]] Status initialise();

    /// Creates the main window and connects it to everything.  Separate from
    /// initialise() so the window can be built after the workers exist - some of
    /// its initial state is read from them.
    void buildUi();

    /// Shows the window and starts the workers.  Returns immediately; the
    /// acquisition worker connects on its own thread.
    void start();

    /// Stops every worker and waits.  Idempotent - it is called both from the
    /// window's closeEvent and from the destructor.
    void stop();

    [[nodiscard]] DeviceManager* devices() const noexcept { return m_devices.get(); }
    [[nodiscard]] MainWindow*    window()  const noexcept { return m_window; }
    [[nodiscard]] bool isSimulationEnabled() const noexcept { return m_simulationEnabled; }
    struct PipelineStatistics
    {
        quint64 received = 0, written = 0, storageDropped = 0;
        quint64 analysisReplaced = 0, analysisEvicted = 0, visionReplaced = 0;
        quint64 recordRejected = 0, recordFailed = 0;
        int recordDepth = 0, recordHighWater = 0;
        SampleInbox::Statistics persistence, ai, ui;
    };
    [[nodiscard]] PipelineStatistics pipelineStatistics() const;

private slots:
    // -- window requests ----------------------------------------------------
    void onAcquisitionToggleRequested(bool start);
    void onReconnectRequested();
    void onAnalyseAllRequested();
    void onReportRequested(const QString& deviceId);
    void onReportExportRequested(const QString& deviceId, bool exportToFile);
    void onVisionToggleRequested(bool enabled);
    void onVisionCaptureRequested();
    void onVisionRateChanged(int fps);
    void onSimulateFaultRequested(const QString& deviceId);
    void onShutdownRequested();

    // -- worker events ------------------------------------------------------
    void onConfigReloaded();
    void onConnectionStateChanged(saim::ConnectionState state);
    void onAcquisitionStatistics(const saim::TcpClient::Statistics& statistics);
    void onAcquisitionError(const QString& message);
    void onPersistenceError(const QString& message);
    void onDatabaseOpened(const saim::DatabaseManager::HealthReport& health);
    void onDatabaseFailed(const QString& reason);
    void onAiStatusChanged(const QString& backend, bool neural, const QString& visionDescription);
    void onVisionStatusChanged(const QString& description, bool simulated,
                               bool hasCamera, bool hasModel);
    void onVisionReady(const saim::VisionResult& result);
    void onAnalysisReady(const saim::AIAnalysisResult& result);
    void onReportReady(const saim::ReportResult& result);
    void onAiError(const QString& message);

    void onAlarmRaised(const saim::AlarmRecord& alarm);
    void onAlarmAcknowledged(const QString& deviceId);
    void onDeviceStateChanged(const QString& deviceId, saim::DeviceState state);

    // -- coalescing ticks ---------------------------------------------------
    void onUiRefreshTick();
    void onStatusTick();

private:
    void createModel();
    void createWorkers();
    void createSimulator();
    void wireWorkers();
    void wireUi();
    void publishDeviceSnapshotToWorkers();

    [[nodiscard]] QString resolveDeviceId(const QString& requested) const;

    ConfigManager* m_config = nullptr;
    SampleInbox m_uiInbox;
    LatestMailbox<AIAnalysisResult> m_analysisInbox{1024};
    LatestMailbox<VisionResult> m_visionInbox{1};
    quint64 m_lastReportedDrops = 0;

    std::unique_ptr<DeviceManager> m_devices;
    std::unique_ptr<WorkerPool>    m_workers;
    std::unique_ptr<TcpServer>     m_simulator;

    MainWindow* m_window = nullptr;

    // Timers live here rather than in the widgets: the *rate* at which the
    // interface updates is a system-wide policy, and two widgets choosing two
    // different rates is how a UI ends up doing four times the work it needs to.
    QTimer* m_uiRefreshTimer = nullptr;
    QTimer* m_statusTimer    = nullptr;

    /// Samples acknowledged per device, for the alarm badge.  Maintained here
    /// rather than queried from the registry, which would be an O(devices) walk
    /// on every alarm.
    QHash<QString, qint64> m_unacknowledgedAlarms;

    quint64 m_lastSampleTotal = 0;
    DatabaseManager::HealthReport m_databaseHealth;
    quint64 m_rowsAtDatabaseOpen = 0;
    double  m_smoothedRate    = 0.0;
    bool    m_started         = false;
    bool    m_simulationEnabled = false;
};

}   // namespace saim
