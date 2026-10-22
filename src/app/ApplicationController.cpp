#include "app/ApplicationController.h"

#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "database/DatabaseManager.h"
#include "device/Device.h"
#include "device/DeviceManager.h"
#include "network/TcpServer.h"
#include "thread/AcquisitionWorker.h"
#include "thread/AIWorker.h"
#include "thread/PersistenceWorker.h"
#include "ui/AIChatWidget.h"
#include "ui/CameraWidget.h"
#include "ui/DashboardWidget.h"
#include "ui/DeviceDetailPanel.h"
#include "ui/DeviceListPanel.h"
#include "ui/LogPanel.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QTimer>

#include <algorithm>

namespace saim {

namespace {

/// How often the dashboard and the device list take a fresh snapshot.
///
/// 100 ms, i.e. 10 Hz.  Every number on that screen is derived from a 2 Hz
/// stream; repainting it faster than an eye can read costs CPU on a machine
/// whose real job includes talking to the line.  This one constant decides how
/// much of the event loop the interface is allowed to occupy.
constexpr int kUiRefreshIntervalMs = 100;

/// The status bar and the throughput readout update twice a second.  A number
/// that changes ten times a second is unreadable anyway - the digits move faster
/// than they can be read - so the display rate is deliberately below the data
/// rate rather than matched to it.
constexpr int kStatusIntervalMs = 500;

/// How quickly the displayed sample rate follows the real one.  An exponential
/// moving average rather than the raw value: an instantaneous rate measured over
/// one second jitters by tens of percent, and a readout oscillating between 11.8
/// and 12.4 looks broken even though both figures are true.
constexpr double kRateSmoothing = 0.3;

/// How long a "simulate fault" injection lasts.  Long enough for the operator to
/// move their eyes from the toolbar to the alarm panel, short enough that a demo
/// does not end with a permanently broken machine.
constexpr int kInjectedFaultSamples = 60;

}   // namespace

// ---------------------------------------------------------------------------
// Construction / teardown
// ---------------------------------------------------------------------------

ApplicationController::ApplicationController(ConfigManager* config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_uiInbox(config ? config->getInt(QStringLiteral("pipeline.uiCapacity"), 1024) : 1024,
                SampleInbox::Policy::LatestPerDevice)
{
    Q_ASSERT(config != nullptr);
}

ApplicationController::~ApplicationController()
{
    // Belt and braces: stop() is normally called from the window's closeEvent,
    // and this catches the paths that never reach it - a startup failure, a test
    // that destroys the controller directly, a SIGTERM handled elsewhere.
    stop();
    delete m_window;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Status ApplicationController::initialise()
{
    if (m_devices != nullptr)
    {
        return Status::ok();   // idempotent
    }

    createModel();
    createWorkers();
    createSimulator();

    // Timers are created last, and parented to this object, so they are
    // destroyed before the workers they drive - a timer firing during teardown
    // would otherwise touch a half-dismantled pool.
    m_uiRefreshTimer = new QTimer(this);
    m_uiRefreshTimer->setInterval(kUiRefreshIntervalMs);
    m_uiRefreshTimer->setTimerType(Qt::CoarseTimer);
    connect(m_uiRefreshTimer, &QTimer::timeout, this, &ApplicationController::onUiRefreshTick);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusIntervalMs);
    m_statusTimer->setTimerType(Qt::CoarseTimer);
    connect(m_statusTimer, &QTimer::timeout, this, &ApplicationController::onStatusTick);

    wireWorkers();
    publishDeviceSnapshotToWorkers();

    SAIM_INFO(logcat::kApp,
              QStringLiteral("Controller initialised: %1 device(s), simulation %2")
                  .arg(m_devices->deviceCount())
                  .arg(m_simulationEnabled ? QStringLiteral("on") : QStringLiteral("off")));

    return Status::ok();
}

void ApplicationController::createModel()
{
    m_devices = std::make_unique<DeviceManager>(m_config, nullptr);

    // Built from config.json, which is also where the device list lives, so the
    // registry and the simulated gateway describe the same plant.
    m_devices->loadFromConfig();

    if (m_devices->deviceCount() == 0)
    {
        // A missing "devices" key is not an error - a real deployment may add
        // them from a discovery service - but an empty fleet makes every panel
        // look broken, so it is logged at warning level rather than information.
        SAIM_WARN(logcat::kApp,
                  QStringLiteral("No devices are configured; the interface will show an "
                                 "empty fleet. Check the \"devices\" array in config.json."));
    }
}

void ApplicationController::createWorkers()
{
    m_workers = std::make_unique<WorkerPool>(m_config, m_devices.get(), nullptr);

    // Creating the workers does not start any thread; start() does that, in the
    // pool's documented order.
    m_workers->create();
}

void ApplicationController::createSimulator()
{
    const ConfigManager::SimulationConfig& simulation = m_config->simulation();
    m_simulationEnabled = simulation.enabled;

    if (!m_simulationEnabled)
    {
        SAIM_INFO(logcat::kApp,
                  QStringLiteral("Simulation disabled; expecting an external gateway at %1:%2")
                      .arg(m_config->network().remoteHost)
                      .arg(m_config->network().remotePort));
        return;
    }

    // ---- the in-process FakeDeviceServer ----------------------------------
    //
    // Deliberately in-process and not a separate tool.  A reviewer cloning the
    // repository has no PLC and no second terminal; if demonstrating the product
    // requires remembering to launch a helper first, the product looks broken on
    // the first run.  The standalone saim_simulator target exists as well, for
    // the case where the gateway should be on another machine - or where the
    // point being demonstrated is that the client survives a disconnect.
    TcpServer::Options options;
    options.listenAddress      = m_config->network().listenAddress;
    options.port               = m_config->network().listenPort;
    options.sampleIntervalMs   = simulation.sampleIntervalMs;
    options.injectAnomalies    = simulation.injectAnomalies;
    options.anomalyRatePercent = simulation.anomalyRatePercent;
    options.deviceIds          = m_devices->deviceIds();
    options.useDefaultFleetWhenEmpty = false;

    m_simulator = std::make_unique<TcpServer>(nullptr);
    m_simulator->setOptions(options);

    // The server runs on the calling (UI) thread.  Its timer at 2 Hz is nothing
    // next to the socket read loop, and keeping it here means a demo build has
    // one fewer thread to reason about.  The standalone tool does move it onto
    // its own thread, which is where that matters.
    connect(m_simulator.get(), &TcpServer::started, this,
            [](quint16 port)
            {
                SAIM_INFO(logcat::kNetwork,
                          QStringLiteral("In-process plant gateway listening on port %1").arg(port));
            });
    connect(m_simulator.get(), &TcpServer::errorOccurred, this,
            [](const QString& message)
            {
                SAIM_ERROR(logcat::kNetwork,
                           QStringLiteral("Plant gateway: %1").arg(message));
            });

    SAIM_INFO(logcat::kApp,
              QStringLiteral("Simulation enabled: %1 device(s), %2 ms interval")
                  .arg(options.deviceIds.size())
                  .arg(options.sampleIntervalMs));
}

void ApplicationController::buildUi()
{
    if (m_window != nullptr || m_devices == nullptr)
    {
        return;
    }

    m_window = new MainWindow(m_config, m_devices.get(), nullptr);
    wireUi();

    // The report dialog needs the fleet before it can offer a scope list, and
    // the fleet is known now rather than when the dialog opens.
    QVector<QPair<QString, QString>> scopes;
    const QList<std::shared_ptr<Device>> devices = m_devices->devices();
    scopes.reserve(devices.size());
    for (const std::shared_ptr<Device>& device : devices)
    {
        if (device != nullptr)
        {
            scopes.append({device->deviceId(),
                           device->deviceName().isEmpty() ? device->deviceId()
                                                          : device->deviceName()});
        }
    }
    m_window->setReportScope(scopes);

    // AI status is read from the worker *after* it has started, so the first
    // value the window shows is the real one rather than "starting…".  When the
    // worker has not started yet the default is correct and the real value
    // arrives on workerStarted().
    if (const AIWorker* ai = m_workers != nullptr ? m_workers->ai() : nullptr)
    {
        m_window->setAiStatus(ai->predictorBackend(),
                              ai->hasNeuralModel());
    }
}

void ApplicationController::start()
{
    if (m_started)
    {
        return;
    }
    m_started = true;
    m_uiInbox.reopen();

    // The plant gateway comes up *before* the client, and this ordering is the
    // whole reason start() does not simply delegate to the worker pool.  A
    // client that dials a socket nobody is listening on enters its reconnect
    // back-off and the first thing a reviewer sees is a Retrying… status on a
    // system that is working perfectly.  Listening first makes the first
    // attempt succeed, which is also what happens in a real deployment where
    // the gateway is always up.
    if (m_simulator != nullptr)
    {
        m_simulator->start();
    }

    if (m_workers != nullptr)
    {
        m_rowsAtDatabaseOpen = m_workers->persistence()->rowsWritten();
        m_workers->start();
    }

    // The panels are refreshed from a timer rather than from the sample stream.
    // See the constants at the top of this file for the reasoning; the short
    // version is that the data rate and the display rate are two different
    // decisions and binding them together is how a UI falls behind.
    m_uiRefreshTimer->start();
    m_statusTimer->start();

    // Acquisition is started explicitly rather than automatically by the pool,
    // so that the toolbar button reflects a state the operator asked for rather
    // than one that was assumed.
    if (m_config->network().autoConnect)
    {
        onAcquisitionToggleRequested(true);
    }

    if (m_window != nullptr)
    {
        m_window->show();
    }
}

void ApplicationController::stop()
{
    if (!m_started && m_workers == nullptr)
    {
        return;
    }
    m_started = false;

    if (m_uiRefreshTimer != nullptr)
    {
        m_uiRefreshTimer->stop();
    }
    if (m_statusTimer != nullptr)
    {
        m_statusTimer->stop();
    }

    // Order matters and is documented in WorkerPool: acquisition first so no new
    // samples enter, AI next so nothing new is queued for storage, persistence
    // last so it can drain and commit everything the other two produced.
    if (m_workers != nullptr)
    {
        m_workers->stop([this] {
            // Producers are joined. Apply the final latest-value snapshots on
            // the model's owner thread while record storage is still alive.
            m_uiInbox.close();
            while (m_uiInbox.statistics().depth > 0) onUiRefreshTick();
            for (const auto& result : m_analysisInbox.take(1024)) onAnalysisReady(result);
        });
    }
    m_uiInbox.close();
    m_uiInbox.discard();
    m_analysisInbox.clear();
    m_visionInbox.clear();

    if (m_simulator != nullptr)
    {
        m_simulator->stop();
    }

    SAIM_INFO(logcat::kApp, QStringLiteral("Controller stopped"));
}

// ---------------------------------------------------------------------------
// Wiring: the data path
// ---------------------------------------------------------------------------

void ApplicationController::wireWorkers()
{
    if (m_workers == nullptr || m_devices == nullptr)
    {
        return;
    }

    AcquisitionWorker* acquisition = m_workers->acquisition();
    PersistenceWorker* persistence = m_workers->persistence();
    AIWorker*          ai          = m_workers->ai();

    // ---- acquisition -> the world -----------------------------------------
    if (acquisition != nullptr)
    {
        // Direct connections call ONLY the thread-safe mailbox entry points.
        // They do not execute SQL, inference or model mutations on acquisition.
        // A full persistence mailbox waits briefly, slowing socket consumption.
        if (persistence != nullptr)
        {
            connect(acquisition, &AcquisitionWorker::sampleAcquired,
                    persistence, [persistence](const SensorSample& sample) {
                        persistence->enqueueSample(sample);
                    }, Qt::DirectConnection);
        }

        // Every sample also feeds the inference window.  The AI worker rate
        // limits itself per device, so passing it the full stream is correct -
        // it needs consecutive samples to build a window, and it decides for
        // itself when the window is worth analysing.
        if (ai != nullptr)
        {
            connect(acquisition, &AcquisitionWorker::sampleAcquired,
                    ai, [ai](const SensorSample& sample) {
                        ai->enqueueSample(sample);
                    }, Qt::DirectConnection);
        }

        // The *coalesced* stream is the one the interface sees: newest sample per
        // device, published at a bounded rate.  This is the single most important
        // connection in the file.  Feeding the widgets the raw stream works
        // perfectly on six devices at 2 Hz and falls over on five hundred at
        // 20 Hz, because the UI thread's event queue grows faster than it drains.
        connect(acquisition, &AcquisitionWorker::sampleAcquired,
                this, [this](const SensorSample& sample) { m_uiInbox.push(sample); },
                Qt::DirectConnection);

        connect(acquisition, &AcquisitionWorker::connectionStateChanged,
                this, &ApplicationController::onConnectionStateChanged);
        connect(acquisition, &AcquisitionWorker::gatewayError,
                this, &ApplicationController::onAcquisitionError);
    }

    // ---- the model -> persistence and AI -----------------------------------
    if (persistence != nullptr)
    {
        // Alarms, verdicts and anomalies are *derived* here and stored there.
        // They originate on the UI thread (Device::raiseAlarm runs where the
        // sample was delivered) and cross to the storage thread by queued
        // connection, which is also what makes the write safe.
        connect(m_devices.get(), &DeviceManager::alarmRaised,
                persistence, [persistence](const AlarmRecord& record) { persistence->enqueueRecord(record); },
                Qt::DirectConnection);
        connect(m_devices.get(), &DeviceManager::predictionUpdated,
                persistence, [persistence](const FaultPrediction& record) { persistence->enqueueRecord(record); },
                Qt::DirectConnection);
        connect(m_devices.get(), &DeviceManager::anomalyDetected,
                persistence, [persistence](const AnomalyEvent& record) {
                    if (record.level != AnomalyLevel::None) persistence->enqueueRecord(record);
                }, Qt::DirectConnection);

        connect(persistence, &PersistenceWorker::databaseOpened,
                this, &ApplicationController::onDatabaseOpened);
        connect(persistence, &PersistenceWorker::databaseFailed,
                this, &ApplicationController::onDatabaseFailed);
        connect(persistence, &PersistenceWorker::persistenceError,
                this, &ApplicationController::onPersistenceError);
    }

    // ---- AI -> the model and the UI ----------------------------------------
    if (ai != nullptr)
    {
        // A verdict is applied to the Device *on the UI thread* (Device is
        // UI-affine by contract), which is what lets the state machine and the
        // alarm path stay single-threaded.
        connect(ai, &AIWorker::analysisReady,
                this, [this](const AIAnalysisResult& result) {
                    m_analysisInbox.put(result.prediction.deviceId, result);
                }, Qt::DirectConnection);
        connect(ai, &AIWorker::visionReady,
                this, [this](const VisionResult& result) {
                    m_visionInbox.put(QStringLiteral("camera"), result);
                }, Qt::DirectConnection);
        connect(ai, &AIWorker::reportReady,
                this, &ApplicationController::onReportReady);
        connect(ai, &AIWorker::aiStatusChanged,
                this, &ApplicationController::onAiStatusChanged);
        connect(ai, &AIWorker::visionStatusChanged,
                this, &ApplicationController::onVisionStatusChanged);
        connect(ai, &AIWorker::aiError, this, &ApplicationController::onAiError);
    }

    // ---- config hot-reload -------------------------------------------------
    connect(m_config, &ConfigManager::reloaded,
            this, &ApplicationController::onConfigReloaded);
    connect(m_config, &ConfigManager::valueChanged, this, [this] {
        onConfigReloaded();
    });
    connect(m_devices.get(), &DeviceManager::deviceAdded,
            this, &ApplicationController::publishDeviceSnapshotToWorkers);
    connect(m_devices.get(), &DeviceManager::deviceRemoved,
            this, &ApplicationController::publishDeviceSnapshotToWorkers);
    connect(m_devices.get(), &DeviceManager::deviceMetadataChanged,
            this, &ApplicationController::publishDeviceSnapshotToWorkers);

    // ---- pool lifecycle ----------------------------------------------------
    connect(m_workers.get(), &WorkerPool::workerStarted, this,
            [this](const QString& name)
            {
                SAIM_DEBUG(logcat::kThread, QStringLiteral("Worker started: %1").arg(name));

                // The AI worker publishes its backend in onStart(); this is the
                // first moment it is readable from the control thread, so the
                // status bar is corrected here rather than left at its default.
                if (name.contains(QLatin1String("ai"), Qt::CaseInsensitive) &&
                    m_workers->ai() != nullptr && m_window != nullptr)
                {
                    m_window->setAiStatus(m_workers->ai()->predictorBackend(),
                                          m_workers->ai()->hasNeuralModel());
                }
            });
}

// ---------------------------------------------------------------------------
// Wiring: the interface
// ---------------------------------------------------------------------------

void ApplicationController::wireUi()
{
    if (m_window == nullptr)
    {
        return;
    }

    // -- the window's requests ----------------------------------------------
    // Every one of these is a signal from MainWindow, which implements none of
    // them.  The controller decides what each means; see the slot bodies below.
    connect(m_window, &MainWindow::acquisitionToggleRequested,
            this, &ApplicationController::onAcquisitionToggleRequested);
    connect(m_window, &MainWindow::reconnectRequested,
            this, &ApplicationController::onReconnectRequested);
    connect(m_window, &MainWindow::analyseAllRequested,
            this, &ApplicationController::onAnalyseAllRequested);
    connect(m_window, &MainWindow::reportRequested,
            this, &ApplicationController::onReportRequested);
    connect(m_window, &MainWindow::reportExportRequested,
            this, &ApplicationController::onReportExportRequested);
    connect(m_window, &MainWindow::visionToggleRequested,
            this, &ApplicationController::onVisionToggleRequested);
    connect(m_window, &MainWindow::visionCaptureRequested,
            this, &ApplicationController::onVisionCaptureRequested);
    connect(m_window, &MainWindow::visionRateChanged,
            this, &ApplicationController::onVisionRateChanged);
    connect(m_window, &MainWindow::simulateFaultRequested,
            this, &ApplicationController::onSimulateFaultRequested);
    connect(m_window, &MainWindow::shutdownRequested,
            this, &ApplicationController::onShutdownRequested);

    // -- the model's own transitions ----------------------------------------
    // Connected here rather than in the window: the alarm counter is application
    // state, and the window is told the *result*, not asked to keep the tally.
    connect(m_devices.get(), &DeviceManager::alarmRaised,
            this, &ApplicationController::onAlarmRaised);
    connect(m_devices.get(), &DeviceManager::stateChanged,
            this, &ApplicationController::onDeviceStateChanged);

    // -- the log panel's acknowledge action ---------------------------------
    if (LogPanel* logs = m_window->logPanel())
    {
        connect(logs, &LogPanel::acknowledgeRequested,
                this, &ApplicationController::onAlarmAcknowledged);
    }

    // -- the camera panel's capture gating ----------------------------------
    if (CameraWidget* camera = m_window->camera())
    {
        connect(camera, &CameraWidget::captureEnabledChanged, this,
                [this](bool enabled)
                {
                    if (m_workers != nullptr && m_workers->ai() != nullptr)
                    {
                        auto* ai = m_workers->ai();
                        ai->invokeAsync([ai, enabled] { ai->setVisionEnabled(enabled); });
                    }
                });
    }

    // -- the device list's own refresh --------------------------------------
    // The list repaints its rows on the same tick as everything else, so that
    // one frame never shows two different moments in time.
    connect(m_uiRefreshTimer, &QTimer::timeout,
            m_window->deviceList(), &DeviceListPanel::refreshValues);
    connect(m_uiRefreshTimer, &QTimer::timeout,
            m_window->dashboard(), &DashboardWidget::refresh);
    connect(m_uiRefreshTimer, &QTimer::timeout,
            m_window->details(), &DeviceDetailPanel::refresh);

    // -- the model's set changes --------------------------------------------
    // A device appearing or disappearing is rare (a config reload) so it is
    // connected to the event rather than polled on the tick.
    connect(m_devices.get(), &DeviceManager::deviceAdded, m_window->dashboard(),
            &DashboardWidget::rebuild);
    connect(m_devices.get(), &DeviceManager::deviceRemoved, m_window->dashboard(),
            &DashboardWidget::rebuild);

    m_window->setAcquisitionRunning(false);
    m_window->setVisionRunning(false);
}

// ---------------------------------------------------------------------------
// Window requests
// ---------------------------------------------------------------------------

void ApplicationController::onAcquisitionToggleRequested(bool start)
{
    QString endpoint;
    if (m_config != nullptr)
    {
        endpoint = QStringLiteral("%1:%2")
                       .arg(m_config->network().remoteHost)
                       .arg(m_config->network().remotePort);
    }

    if (m_workers == nullptr)
    {
        return;
    }

    if (start)
    {
        if (AcquisitionWorker* acquisition = m_workers->acquisition())
        {
            QMetaObject::invokeMethod(acquisition, &AcquisitionWorker::connectToGateway,
                                      Qt::QueuedConnection);
        }

        // The gateway may be a separate process that has not been started yet.
        // If the in-process simulator is the plant, it has to be listening
        // *before* the client dials, otherwise the first attempt fails and the
        // reviewer sees a reconnect loop on a system that was working fine.
        if (m_simulator != nullptr && !m_simulator->isListening())
        {
            m_simulator->start();
        }

        if (m_window != nullptr) { m_window->setConnectionState(ConnectionState::Connecting, endpoint); }
    }
    else
    {
        if (AcquisitionWorker* acquisition = m_workers->acquisition())
        {
            QMetaObject::invokeMethod(acquisition, &AcquisitionWorker::disconnectFromGateway,
                                      Qt::QueuedConnection);
        }
        if (m_window != nullptr) { m_window->setConnectionState(ConnectionState::Disconnected, endpoint); }
    }

    if (m_window != nullptr) { m_window->setAcquisitionRunning(start); }
}

void ApplicationController::onReconnectRequested()
{
    if (m_workers == nullptr)
    {
        return;
    }

    // A reconnect is a disconnect followed by a connect, issued in that order on
    // the worker's own queue.  Doing it as one "reconnect" method on the worker
    // would hide the fact that the link really does drop - and an operator who
    // presses this needs to see that it dropped.
    if (AcquisitionWorker* acquisition = m_workers->acquisition())
    {
        QMetaObject::invokeMethod(acquisition, &AcquisitionWorker::disconnectFromGateway,
                                  Qt::QueuedConnection);
        QMetaObject::invokeMethod(acquisition, &AcquisitionWorker::connectToGateway,
                                  Qt::QueuedConnection);
    }

    m_window->showTransientMessage(tr("Reconnecting to the gateway…"));
}

void ApplicationController::onAnalyseAllRequested()
{
    if (m_workers == nullptr || m_workers->ai() == nullptr || m_devices == nullptr)
    {
        return;
    }

    // Forced analysis ignores the per-device rate limit.  That is the point of
    // the button: the operator has decided they want a verdict *now*, and making
    // them wait for the scheduled cadence would read as a broken control.
    const QStringList ids = m_devices->deviceIds();
    for (const QString& id : ids)
    {
        QMetaObject::invokeMethod(m_workers->ai(), "analyseDeviceNow", Qt::QueuedConnection,
                                  Q_ARG(QString, id));
    }
}

void ApplicationController::onReportRequested(const QString& deviceId)
{
    onReportExportRequested(deviceId, false);
}

void ApplicationController::onReportExportRequested(const QString& deviceId, bool exportToFile)
{
    if (m_workers == nullptr || m_workers->ai() == nullptr || !m_workers->ai()->isRunning())
    {
        if (m_window) { m_window->showReportError(tr("The AI worker is not running, so no report can be produced.")); }
        return;
    }

    // Capture all live QObject state on its owning thread. Report generation
    // receives only values, even if a device is deleted while rendering.
    QVector<DeviceReportSection> sections;
    QDateTime from;
    QDateTime to;
    for (const auto& device : m_devices->devices())
    {
        if (!deviceId.isEmpty() && device->deviceId() != deviceId) { continue; }
        const auto statistics = device->statistics();
        if (statistics.from.isValid() && (!from.isValid() || statistics.from < from)) { from = statistics.from; }
        if (statistics.to.isValid() && (!to.isValid() || statistics.to > to)) { to = statistics.to; }
        sections.append(AIReportGenerator::buildSection(
            device->deviceId(), device->deviceName(), device->deviceType(), device->state(),
            statistics, device->prediction(), static_cast<int>(device->alarmCount()), 0, device->limits()));
    }
    if (sections.isEmpty())
    {
        if (m_window) { m_window->showReportError(tr("No devices match the report scope.")); }
        return;
    }
    if (m_window) { m_window->setReportGenerating(true); }
    auto* ai = m_workers->ai();
    ai->invokeAsync([ai, sections, from, to, exportToFile] {
        ai->generateReport(sections, from, to, exportToFile);
    });
}

void ApplicationController::onVisionToggleRequested(bool enabled)
{
    if (m_workers == nullptr || m_workers->ai() == nullptr)
    {
        return;
    }

    if (enabled && !m_workers->ai()->visionAvailable())
    {
        // Refused, and said so.  Silently leaving the toggle on would give the
        // operator a checked box and an empty panel - the worst of both.
        m_window->setVisionRunning(false);
        m_window->showTransientMessage(
            tr("Camera analysis is unavailable: no capture device and no ONNX model were found."));
        return;
    }

    QMetaObject::invokeMethod(m_workers->ai(), "setVisionEnabled", Qt::QueuedConnection,
                              Q_ARG(bool, enabled));

    if (CameraWidget* camera = m_window->camera())
    {
        // The panel owns the gating: when the toggle turns it on, the panel
        // starts asking for frames through its own signal, and the controller
        // forwards those.  One path, not two.
        camera->setCaptureEnabled(enabled);
    }
    m_window->setVisionRunning(enabled);
}

void ApplicationController::onVisionCaptureRequested()
{
    if (m_workers == nullptr || m_workers->ai() == nullptr)
    {
        return;
    }
    QMetaObject::invokeMethod(m_workers->ai(), "captureVisionFrame", Qt::QueuedConnection);
}

void ApplicationController::onVisionRateChanged(int fps)
{
    if (m_workers == nullptr || m_workers->ai() == nullptr)
    {
        return;
    }

    // The rate is applied by re-reading configuration rather than by a setter on
    // the worker, so there is one source of truth for "how fast is vision
    // running" and it is the config file the operator can also edit by hand.
    m_config->set(QStringLiteral("ai.visionFps"), fps);
    QMetaObject::invokeMethod(m_workers->ai(), "applyConfiguration", Qt::QueuedConnection);

    m_window->showTransientMessage(tr("Camera analysis rate set to %1 fps.").arg(fps));
}

void ApplicationController::onSimulateFaultRequested(const QString& deviceId)
{
    const QString resolved = resolveDeviceId(deviceId);
    if (resolved.isEmpty())
    {
        m_window->showTransientMessage(
            tr("Select a device first, or wait for the fleet to populate."));
        return;
    }

    // The simulator injects wear into the *plant*, so the degradation arrives
    // through the socket as ordinary telemetry.  Nothing downstream - framing,
    // the state machine, the anomaly detector, the predictor, the alarm path -
    // is bypassed or faked for the demo.  That is the difference between
    // demonstrating an alarm pipeline and demonstrating an alarm animation.
    if (m_simulator != nullptr)
    {
        QMetaObject::invokeMethod(m_simulator.get(), "injectFault", Qt::QueuedConnection,
                                  Q_ARG(QString, resolved),
                                  Q_ARG(int, kInjectedFaultSamples));
        m_window->showTransientMessage(
            tr("Simulated a fault on %1 for %2 samples. Watch the alarm and AI panels.")
                .arg(resolved)
                .arg(kInjectedFaultSamples));
        return;
    }

    // No simulator: the plant is real, so this control forwards the request to
    // the gateway and reports what it can and cannot promise.  Faking a fault
    // locally - injecting a synthetic sample and letting the pipeline react -
    // would produce a convincing alarm that no machine ever raised, which is
    // the one thing a monitoring product must never do.
    if (m_workers != nullptr && m_workers->acquisition() != nullptr)
    {
        QMetaObject::invokeMethod(m_workers->acquisition(), "sendCommand", Qt::QueuedConnection,
                                  Q_ARG(QString, resolved),
                                  Q_ARG(QString, QStringLiteral("simulate-fault")));
        m_window->showTransientMessage(
            tr("Asked the external gateway to fault %1. The result depends on the plant.")
                .arg(resolved));
        return;
    }

    m_window->showTransientMessage(tr("No simulator and no gateway: nothing to fault."));
}

void ApplicationController::onShutdownRequested()
{
    // Called from MainWindow::closeEvent, which is the last moment at which it
    // is still safe to wait for the workers.  Everything after this in the
    // process's life is destruction.
    stop();
}

// ---------------------------------------------------------------------------
// Worker events
// ---------------------------------------------------------------------------

void ApplicationController::onConfigReloaded()
{
    if (m_devices != nullptr)
    {
        // Reconciles rather than rebuilds: a device that is still in the file
        // keeps its history and its state, so reloading the config mid-shift
        // does not blank the charts.
        m_devices->loadFromConfig();
    }
    publishDeviceSnapshotToWorkers();
    if (m_workers && m_started)
    {
        auto* ai = m_workers->ai();
        ai->invokeAsync([ai] { ai->applyConfiguration(); });
        auto* persistence = m_workers->persistence();
        persistence->invokeAsync([persistence] { persistence->applyConfiguration(); });
    }

    if (m_simulator != nullptr && m_simulationEnabled)
    {
        TcpServer::Options options = m_simulator->options();
        options.sampleIntervalMs   = m_config->simulation().sampleIntervalMs;
        options.injectAnomalies    = m_config->simulation().injectAnomalies;
        options.anomalyRatePercent = m_config->simulation().anomalyRatePercent;
        options.deviceIds          = m_devices != nullptr ? m_devices->deviceIds() : QStringList{};
        m_simulator->setOptions(options);
    }

    if (m_window != nullptr)
    {
        m_window->showTransientMessage(tr("Configuration reloaded: %1 device(s).")
                                           .arg(m_devices != nullptr ? m_devices->deviceCount() : 0));
    }
}

void ApplicationController::onConnectionStateChanged(ConnectionState state)
{
    QString endpoint;
    if (m_config != nullptr)
    {
        endpoint = QStringLiteral("%1:%2")
                       .arg(m_config->network().remoteHost)
                       .arg(m_config->network().remotePort);
    }

    if (m_window != nullptr)
    {
        m_window->setConnectionState(state, endpoint);
    }
}

void ApplicationController::onAcquisitionStatistics(const TcpClient::Statistics& statistics)
{
    Q_UNUSED(statistics)

    // The rate comes from the acquisition worker's own counter, sampled on the
    // status tick, rather than from the byte counter here.  Two counters that
    // both claim to describe throughput is one counter too many.
}

void ApplicationController::onAcquisitionError(const QString& message)
{
    if (m_window != nullptr)
    {
        m_window->showTransientMessage(tr("Gateway: %1").arg(message), 8000);
    }
}

void ApplicationController::onPersistenceError(const QString& message)
{
    if (m_window != nullptr)
    {
        m_window->showTransientMessage(tr("Storage: %1").arg(message), 8000);
    }
}

void ApplicationController::onDatabaseOpened(const DatabaseManager::HealthReport& health)
{
    m_databaseHealth = health;
    if (m_window == nullptr)
    {
        return;
    }

    // The row count is shown rather than just "open", because "the database is
    // open" and "the database is receiving anything" are different facts and an
    // operator chasing a storage problem needs the second one.
    m_window->setDatabaseStatus(health.ok,
                                tr("%1 rows, schema v%2, %3")
                                    .arg(health.sensorRows)
                                    .arg(health.schemaVersion)
                                    .arg(health.journalMode));
}

void ApplicationController::onDatabaseFailed(const QString& reason)
{
    if (m_window != nullptr)
    {
        m_window->setDatabaseStatus(false, tr("unavailable — %1").arg(reason));
        m_window->showTransientMessage(
            tr("Storage is unavailable; telemetry will not be persisted. %1").arg(reason), 12000);
    }
}

void ApplicationController::onAiStatusChanged(const QString& backend, bool neural,
                                              const QString& visionDescription)
{
    Q_UNUSED(visionDescription)

    if (m_window != nullptr)
    {
        m_window->setAiStatus(backend, neural);
    }
}

void ApplicationController::onVisionStatusChanged(const QString& description, bool simulated,
                                                  bool hasCamera, bool hasModel)
{
    if (m_window == nullptr)
    {
        return;
    }

    if (CameraWidget* camera = m_window->camera())
    {
        camera->setDetectorStatus(description, simulated, hasCamera, hasModel);
    }

    // The toolbar toggle follows the detector's own verdict, not the operator's
    // click: if the camera turned out to be missing, the control shows off.
    m_window->setVisionRunning(!simulated || hasCamera || hasModel
                                   ? m_workers->ai() != nullptr && m_workers->ai()->visionAvailable()
                                   : false);
}

void ApplicationController::onVisionReady(const VisionResult& result)
{
    if (m_window == nullptr)
    {
        return;
    }

    if (CameraWidget* camera = m_window->camera())
    {
        camera->applyVisionResult(result);
    }
}

void ApplicationController::onAnalysisReady(const AIAnalysisResult& result)
{
    if (m_devices == nullptr)
    {
        return;
    }

    const std::shared_ptr<Device> device = m_devices->device(result.prediction.deviceId);
    if (device == nullptr)
    {
        // The device was removed between the analysis being queued and delivered.
        // Dropping it silently is correct: there is nothing left to attach it to,
        // and inventing a placeholder would put a ghost device on the dashboard.
        return;
    }

    // Applied on this thread, which is the UI thread.  Device is UI-affine by
    // contract - see Device.h - and routing the verdict through a direct call
    // rather than a connection keeps the state machine single-threaded.
    if (!device->applyAnalysis(result.prediction, result.anomaly)) { return; }

    if (m_window != nullptr)
    {
        // The assistant quotes the numbers from *this* run rather than the last
        // scheduled one, which is what makes "analyse now" then "why?" coherent.
        m_window->assistant()->applyAnalysis(result.prediction.deviceId,
                                             result.prediction, result.anomaly);
    }
}

void ApplicationController::onReportReady(const ReportResult& result)
{
    if (m_window == nullptr)
    {
        return;
    }

    m_window->setReportGenerating(false);
    m_window->showReport(result);
}

void ApplicationController::onAiError(const QString& message)
{
    if (m_window != nullptr)
    {
        m_window->setReportGenerating(false);
        m_window->showTransientMessage(tr("AI: %1").arg(message), 8000);
    }
}

// ---------------------------------------------------------------------------
// Alarms
// ---------------------------------------------------------------------------

void ApplicationController::onAlarmRaised(const AlarmRecord& alarm)
{
    m_unacknowledgedAlarms[alarm.deviceId] += 1;

    if (m_window == nullptr)
    {
        return;
    }

    m_window->logPanel()->appendAlarm(alarm);

    int total = 0;
    for (auto it = m_unacknowledgedAlarms.cbegin(); it != m_unacknowledgedAlarms.cend(); ++it)
    {
        total += static_cast<int>(it.value());
    }
    m_window->setAlarmCount(total);
}

void ApplicationController::onAlarmAcknowledged(const QString& deviceId)
{
    const QString resolved = resolveDeviceId(deviceId);
    if (resolved.isEmpty())
    {
        return;
    }

    // Acknowledging clears the device's own latch so it can alarm again after
    // maintenance, and clears the counter here.  Both are needed: the latch
    // suppresses *new* alarms, the counter is what the status bar shows.
    if (m_devices != nullptr)
    {
        if (const std::shared_ptr<Device> device = m_devices->device(resolved))
        {
            device->acknowledgeAlarm();
        }
    }

    m_unacknowledgedAlarms.remove(resolved);

    if (m_window != nullptr)
    {
        int total = 0;
        for (auto it = m_unacknowledgedAlarms.cbegin(); it != m_unacknowledgedAlarms.cend(); ++it)
        {
            total += static_cast<int>(it.value());
        }
        m_window->setAlarmCount(total);
        m_window->showTransientMessage(tr("Acknowledged the alarm on %1.").arg(resolved));
    }
}

void ApplicationController::onDeviceStateChanged(const QString& deviceId, DeviceState state)
{
    if (state == DeviceState::Normal || state == DeviceState::Offline)
    {
        // Recovering clears the badge.  An alarm that has gone away should not
        // leave a count sitting in the status bar demanding attention it no
        // longer needs - teaching an operator to ignore the counter is how a
        // real alarm gets missed.
        m_unacknowledgedAlarms.remove(deviceId);
    }
}

// ---------------------------------------------------------------------------
// Ticks
// ---------------------------------------------------------------------------

void ApplicationController::onUiRefreshTick()
{
    for (const auto& sample : m_uiInbox.take(256))
        m_devices->onSampleReceived(sample);
    for (const auto& result : m_analysisInbox.take(256)) onAnalysisReady(result);
    for (const auto& result : m_visionInbox.take(1)) onVisionReady(result);

    // Devices added or removed outside a config reload (a discovery service, a
    // test harness) still need their tiles to appear.
    if (m_devices != nullptr && m_window != nullptr)
    {
        if (m_window->dashboard()->isVisible() && m_devices->deviceCount() != 0)
        {
            // rebuild() is cheap when nothing changed - it walks the id set and
            // compares, which is O(devices) with no allocation.
            m_window->dashboard()->rebuild();
        }
    }
}

void ApplicationController::onStatusTick()
{
    const auto pipeline = pipelineStatistics();
    const quint64 drops = pipeline.persistence.rejected + pipeline.storageDropped
                          + pipeline.ai.evicted + pipeline.ui.evicted + pipeline.analysisEvicted
                          + pipeline.recordRejected + pipeline.recordFailed;
    if (drops != m_lastReportedDrops)
    {
        const QString message = tr("Pipeline loss: storage %1, AI %2, display %3, records %6; queue %4/%5")
            .arg(pipeline.persistence.rejected + pipeline.storageDropped)
            .arg(pipeline.ai.evicted).arg(pipeline.ui.evicted)
            .arg(pipeline.persistence.depth).arg(pipeline.persistence.capacity)
            .arg(pipeline.recordRejected + pipeline.recordFailed);
        SAIM_WARN(logcat::kThread, message);
        if (m_window) m_window->showTransientMessage(message, 8000);
        m_lastReportedDrops = drops;
    }
    if (m_window == nullptr || m_workers == nullptr || m_workers->acquisition() == nullptr)
    {
        return;
    }

    const AcquisitionWorker* acquisition = m_workers->acquisition();
    onAcquisitionStatistics(acquisition->statistics());
    const quint64 total = acquisition->receivedSampleCount();

    // Rate from the delta of the worker's own monotonic counter rather than from
    // a timer of its own: one counter, read twice, cannot drift from itself.
    if (m_lastSampleTotal != 0 && total >= m_lastSampleTotal)
    {
        const double intervalSeconds = static_cast<double>(kStatusIntervalMs) / 1000.0;
        const double instantaneous =
            static_cast<double>(total - m_lastSampleTotal) / intervalSeconds;
        m_smoothedRate = m_smoothedRate <= 0.0
                             ? instantaneous
                             : m_smoothedRate + kRateSmoothing * (instantaneous - m_smoothedRate);
    }
    m_lastSampleTotal = total;

    m_window->setThroughput(m_smoothedRate, total);
    if (m_databaseHealth.ok)
        m_window->setDatabaseStatus(true, tr("%1 rows, schema v%2, %3")
            .arg(m_databaseHealth.sensorRows + static_cast<qint64>(pipeline.written - m_rowsAtDatabaseOpen))
            .arg(m_databaseHealth.schemaVersion).arg(m_databaseHealth.journalMode));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

ApplicationController::PipelineStatistics ApplicationController::pipelineStatistics() const
{
    PipelineStatistics result;
    result.ui = m_uiInbox.statistics();
    result.analysisReplaced = m_analysisInbox.replaced();
    result.analysisEvicted = m_analysisInbox.evicted();
    result.visionReplaced = m_visionInbox.replaced();
    if (m_workers)
    {
        if (const auto* worker = m_workers->acquisition()) result.received = worker->receivedSampleCount();
        if (const auto* worker = m_workers->persistence())
        {
            result.persistence = worker->inboxStatistics();
            result.written = worker->rowsWritten();
            result.storageDropped = worker->droppedSamples();
            const auto records = worker->recordStatistics();
            result.recordRejected = records.rejected;
            result.recordFailed = worker->failedRecords();
            result.recordDepth = records.depth;
            result.recordHighWater = records.highWater;
        }
        if (const auto* worker = m_workers->ai()) result.ai = worker->inboxStatistics();
    }
    return result;
}

QString ApplicationController::resolveDeviceId(const QString& requested) const
{
    if (m_devices == nullptr)
    {
        return QString();
    }

    // An empty request means "whichever device the operator is looking at", and
    // the window already knows that - so an empty id is resolved to the first
    // device rather than to nothing, which keeps the toolbar actions usable
    // without a selection.  A named device that no longer exists resolves to
    // empty, and every caller treats empty as "do nothing and say so".
    if (requested.isEmpty())
    {
        const QStringList ids = m_devices->deviceIds();
        return ids.isEmpty() ? QString() : ids.first();
    }

    return m_devices->contains(requested) ? requested : QString();
}

void ApplicationController::publishDeviceSnapshotToWorkers()
{
    if (!m_devices || !m_workers) { return; }
    QVector<DeviceConfiguration> snapshot;
    for (const auto& device : m_devices->devices())
    {
        snapshot.append({device->deviceId(), device->deviceName(), device->deviceType(),
                         device->ipAddress(), device->port(), device->isEnabled(), device->limits()});
    }
    auto* ai = m_workers->ai();
    auto* persistence = m_workers->persistence();
    if (ai->isRunning()) { ai->invokeAsync([ai, snapshot] { ai->setDevices(snapshot); }); }
    else { ai->setDevices(snapshot); }
    if (persistence->isRunning())
    {
        persistence->invokeAsync([persistence, snapshot] { persistence->setDevices(snapshot); });
    }
    else { persistence->setDevices(snapshot); }
}

}   // namespace saim
