#include "thread/AIWorker.h"

#include "ai/AIInferenceEngine.h"
#include "ai/AIReportGenerator.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QMetaObject>

#include <algorithm>

namespace saim {

namespace {

/// Windows not touched for this long are dropped.  A device removed from the
/// fleet, or one that has gone permanently offline, should not hold its rolling
/// window and its learned anomaly baseline forever - and the anomaly detector's
/// baseline is per-device state that is meaningless once the device is gone.
constexpr qint64 kWindowIdleTimeoutMs = 10 * 60 * 1000;   // 10 minutes

/// Upper bound on tracked devices before the least recent is evicted.  Well above
/// any plausible fleet on one HMI, and low enough that a bug which leaks device
/// ids (a typo'd id per frame, say) cannot exhaust memory before it is noticed.
constexpr int kMaxTrackedDevices = 512;

}   // namespace

AIWorker::AIWorker(ConfigManager* config, QObject* parent)
    : ThreadedWorker(QStringLiteral("AIThread"), parent)
    , m_config(config)
    , m_inbox(config ? config->getInt(QStringLiteral("pipeline.aiCapacity"), 2048) : 2048,
              SampleInbox::Policy::DropOldest)
{
    m_inboxTimer.setParent(this);
    m_inboxTimer.setInterval(10);
    connect(&m_inboxTimer, &QTimer::timeout, this, &AIWorker::drainInbox);
    m_visionTimer.setParent(this);
    m_housekeepingTimer.setParent(this);
}

AIWorker::~AIWorker() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle - both of these run on AIThread
// ---------------------------------------------------------------------------

void AIWorker::onStart()
{
    m_uptime.start();
    m_inbox.reopen();
    m_inboxTimer.start();
    m_hasNeuralModel = false;
    m_visionAvailable = false;
    m_visionEnabled = false;

    const ConfigManager::AiConfig aiConfig =
        m_config != nullptr ? m_config->ai() : ConfigManager::AiConfig{};

    m_windowSize  = std::max(10, aiConfig.windowSize);
    m_inferenceHz = std::max(1, aiConfig.inferenceHz);
    m_analysisIntervalMs = 1000 / m_inferenceHz;

    // ---- predictor ---------------------------------------------------------
    //
    // The engine is created here, on this thread, and never leaves it.  An
    // Ort::Session is not thread safe; "one owner thread" is the property that
    // makes that safe, and constructing it anywhere else would quietly break it.
    std::unique_ptr<IInferenceEngine> engine;
    QString unavailableReason;

    if (aiConfig.enableOnnx)
    {
        OnnxInferenceEngine::Options engineOptions;
        engine = EngineFactory::createOnnx(engineOptions, &unavailableReason);
    }
    else
    {
        unavailableReason = QStringLiteral("disabled in configuration");
    }

    if (engine != nullptr && engine->isReady())
    {
        m_hasNeuralModel = true;
        SAIM_INFO(logcat::kAi,
                  QStringLiteral("AI worker: neural backend active (%1)")
                      .arg(QString::fromLatin1(engine->backendName())));
    }
    else
    {
        // Not an error.  The predictor's physics model answers instead, and the
        // UI reports which backend produced every verdict.
        SAIM_INFO(logcat::kAi,
                  QStringLiteral("AI worker: using the built-in physics model (%1)")
                      .arg(unavailableReason.isEmpty() ? QStringLiteral("no model loaded")
                                                       : unavailableReason));
    }

    FaultPredictor::Options predictorOptions;
    predictorOptions.minimumWindowSize = std::min(m_windowSize, 20);
    predictorOptions.anomalyThreshold  = aiConfig.anomalyThreshold;
    m_predictor = std::make_unique<FaultPredictor>(std::move(engine), predictorOptions);
    m_predictor->warmUp();

    // ---- anomaly detector --------------------------------------------------
    AnomalyDetector::Options detectorOptions;
    detectorOptions.minimumWindowSize = std::min(m_windowSize, 20);
    m_anomalyDetector = std::make_unique<AnomalyDetector>(nullptr, detectorOptions);

    // ---- report generator --------------------------------------------------
    AIReportGenerator::Options reportOptions;
    reportOptions.sourceNote = QStringLiteral(
        "Live snapshot: telemetry statistics cover each device's retained in-memory window. "
        "Alarm counts are since startup; this is not a historical database report.");
    reportOptions.outputDirectory = m_config != nullptr
                                        ? m_config->resolveDataPath(QStringLiteral("reports"))
                                        : QStringLiteral("reports");
    m_reporter = std::make_unique<AIReportGenerator>(reportOptions);

    // ---- vision ------------------------------------------------------------
    //
    // Constructed but not started.  Opening a camera is a side effect the
    // operator should ask for, not one that happens because the application
    // launched - a plant PC may have no camera at all, and a failed open at
    // start-up would put an error on screen before the user did anything.
    if (aiConfig.enableVision)
    {
        VisionDetector::Options visionOptions;
        visionOptions.modelPath = m_config != nullptr
                                      ? m_config->resolvePath(
                                            QDir(aiConfig.modelDirectory)
                                                .filePath(aiConfig.visionModelFile))
                                      : aiConfig.visionModelFile;
        visionOptions.analysisFps = m_visionFps;
        m_vision = std::make_unique<VisionDetector>(visionOptions);

        const Status status = m_vision->initialise();
        m_visionAvailable   = m_vision->isReady();
        m_visionDescription = m_vision->statusDescription();
        m_visionEnabled     = m_visionAvailable;

        if (!status.isOk())
        {
            SAIM_WARN(logcat::kVision,
                      QStringLiteral("Vision unavailable: %1").arg(status.error().message));
        }
    }
    else
    {
        m_visionDescription = QStringLiteral("disabled in configuration");
    }

    {
        QMutexLocker lock(&m_statusMutex);
        m_predictorBackend = QString::fromLatin1(m_predictor->activeBackend());
    }

    // ---- timers ------------------------------------------------------------
    m_visionTimer.setInterval(1000 / std::max(1, m_visionFps));
    m_visionTimer.setTimerType(Qt::CoarseTimer);   // vision latency is not critical
    connect(&m_visionTimer, &QTimer::timeout, this, &AIWorker::runVisionTick, Qt::UniqueConnection);
    if (visionRequested())
    {
        m_visionTimer.start();
    }

    // Housekeeping runs on a slow timer rather than in the sample path: evicting
    // a window is O(devices) and doing it per sample would put that cost on the
    // hot path for no reason.
    m_housekeepingTimer.setInterval(60000);
    connect(&m_housekeepingTimer, &QTimer::timeout, this, &AIWorker::evictIdleWindows,
            Qt::UniqueConnection);
    m_housekeepingTimer.start();

    emit aiStatusChanged(m_predictorBackend, m_hasNeuralModel, m_visionDescription);
    if (m_vision != nullptr)
    {
        emit visionStatusChanged(m_visionDescription, m_vision->isSimulated(),
                                 m_vision->hasCamera(), m_vision->hasModel());
    }

    SAIM_INFO(logcat::kAi,
              QStringLiteral("AI worker ready: window=%1 samples, %2 Hz per device, vision=%3")
                  .arg(m_windowSize)
                  .arg(m_inferenceHz)
                  .arg(m_visionDescription));
}

void AIWorker::onStop()
{
    m_inboxTimer.stop();
    m_inbox.close();
    // Shutdown prioritises durable storage; unanalysed work is counted.
    m_inbox.discard();
    m_visionTimer.stop();
    m_housekeepingTimer.stop();

    // Released here, on the thread that created them.  An Ort::Session and a
    // cv::VideoCapture must be destroyed by their owning thread; letting the
    // unique_ptr run in the destructor - which executes on the UI thread when the
    // owning QObject is deleted - would be a cross-thread release.
    m_vision.reset();
    m_predictor.reset();
    m_anomalyDetector.reset();
    m_reporter.reset();
    m_windows.clear();
    m_nextAnalysisMs.clear();

    SAIM_INFO(logcat::kAi,
              QStringLiteral("AI worker stopped after %1 inference(s)").arg(m_inferenceCount));
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void AIWorker::drainInbox()
{
    for (const auto& sample : m_inbox.take(128)) ingestSample(sample);
}

void AIWorker::applyConfiguration()
{
    if (m_config == nullptr)
    {
        return;
    }

    const ConfigManager::AiConfig aiConfig = m_config->ai();

    m_windowSize  = std::max(10, aiConfig.windowSize);
    m_inferenceHz = std::max(1, aiConfig.inferenceHz);
    m_analysisIntervalMs = 1000 / m_inferenceHz;

    if (m_predictor != nullptr)
    {
        FaultPredictor::Options options;
        options.minimumWindowSize = std::min(m_windowSize, 20);
        options.anomalyThreshold  = aiConfig.anomalyThreshold;
        m_predictor->setOptions(options);
    }

    // Windows longer than the new size are trimmed on the next sample; trimming
    // here as well would mean a config reload could leave a device with fewer
    // samples than it had, which reads as data loss to the person who just
    // changed an unrelated setting.
    SAIM_INFO(logcat::kAi,
              QStringLiteral("AI configuration applied: window=%1, %2 Hz")
                  .arg(m_windowSize)
                  .arg(m_inferenceHz));
}

void AIWorker::setVisionEnabled(bool enabled)
{
    m_visionEnabled = enabled && m_vision != nullptr;

    if (m_visionEnabled)
    {
        if (!m_visionTimer.isActive())
        {
            m_visionTimer.start();
        }
        // Grab one frame immediately: the operator just pressed the button and an
        // empty panel for 200 ms reads as a failure.
        captureVisionFrame();
    }
    else
    {
        m_visionTimer.stop();
    }

    SAIM_INFO(logcat::kVision,
              QStringLiteral("Vision %1").arg(m_visionEnabled ? QStringLiteral("enabled")
                                                              : QStringLiteral("disabled")));
}

// ---------------------------------------------------------------------------
// The sample path
// ---------------------------------------------------------------------------

void AIWorker::ingestSample(const SensorSample& sample)
{
    if (!sample.isValid() || !m_deviceLimits.contains(sample.deviceId))
    {
        return;
    }

    if (accumulate(sample))
    {
        analyseAndPublish(sample.deviceId, false);
    }
}

bool AIWorker::accumulate(const SensorSample& sample)
{
    auto it = m_windows.find(sample.deviceId);

    if (it == m_windows.end())
    {
        // A new device.  Evict before inserting so the cap is a hard bound rather
        // than one that can be exceeded by a burst of new ids.
        if (m_windows.size() >= kMaxTrackedDevices)
        {
            evictIdleWindows();
        }
        it = m_windows.insert(sample.deviceId, {});
    }

    std::deque<SensorSample>& window = it.value();
    window.push_back(sample);
    while (static_cast<int>(window.size()) > m_windowSize)
    {
        window.pop_front();
    }

    // Rate limit per device.  Checked after appending, so the window always holds
    // the newest samples even when the analysis is skipped.
    const qint64 now = m_uptime.elapsed();
    const qint64 dueAt = m_nextAnalysisMs.value(sample.deviceId, 0);
    if (now < dueAt)
    {
        return false;
    }

    // Not enough data to say anything.  Deliberately *not* an analysis: a
    // prediction from five samples is noise, and publishing one would put a
    // confident-looking number on screen for a device that just came online.
    if (static_cast<int>(window.size()) < std::min(m_windowSize, 20))
    {
        return false;
    }

    m_nextAnalysisMs.insert(sample.deviceId, now + m_analysisIntervalMs);
    return true;
}

void AIWorker::analyseDeviceNow(const QString& deviceId)
{
    analyseAndPublish(deviceId, true);
}

void AIWorker::analyseAndPublish(const QString& deviceId, bool force)
{
    if (m_predictor == nullptr)
    {
        return;
    }

    const std::vector<SensorSample> window = windowFor(deviceId);
    if (window.size() < static_cast<std::size_t>(std::min(m_windowSize, 20)))
    {
        // Nothing to analyse.  Reported as an error rather than silently ignored
        // *only* when the caller explicitly asked, because a button that does
        // nothing is worse than a button that explains why.
        if (force)
        {
            emit aiError(QStringLiteral("Not enough samples for %1 yet (%2 of %3 needed)")
                             .arg(deviceId)
                             .arg(window.size())
                             .arg(std::min(m_windowSize, 20)));
        }
        return;
    }

    if (!m_deviceLimits.contains(deviceId)) { return; }
    const DeviceLimits limits = m_deviceLimits.value(deviceId);

    QElapsedTimer timer;
    timer.start();

    AIAnalysisResult result;
    result.prediction = m_predictor->predict(deviceId, window, limits);
    result.anomaly    = m_anomalyDetector->analyse(deviceId, window, limits);
    result.features   = m_predictor->lastExplanation().features;
    result.analysisMs = timer.elapsed();

    ++m_inferenceCount;
    emit analysisReady(result);

    if (result.anomaly.isSignificant())
    {
        SAIM_INFO(logcat::kAi,
                  QStringLiteral("Analysis for %1: health %2, failure %3, anomaly %4 in %5 ms")
                      .arg(deviceId)
                      .arg(result.prediction.healthScore, 0, 'f', 1)
                      .arg(result.prediction.failureProbability)
                      .arg(toString(result.anomaly.level))
                      .arg(result.analysisMs));
    }
}

std::vector<SensorSample> AIWorker::windowFor(const QString& deviceId) const
{
    const auto it = m_windows.constFind(deviceId);
    if (it == m_windows.constEnd())
    {
        return {};
    }
    return std::vector<SensorSample>(it.value().begin(), it.value().end());
}

void AIWorker::forgetDevice(const QString& deviceId)
{
    m_windows.remove(deviceId);
    m_nextAnalysisMs.remove(deviceId);
    if (m_anomalyDetector != nullptr)
    {
        // The learned baseline is per-device state.  Keeping it for a device that
        // no longer exists would leak, and would also mean a device re-added under
        // the same id inherits a stale notion of "normal".
        m_anomalyDetector->forget(deviceId);
    }
}

void AIWorker::setDevices(const QVector<DeviceConfiguration>& devices)
{
    QHash<QString, DeviceLimits> next;
    for (const auto& device : devices)
    {
        if (device.enabled) { next.insert(device.id, device.limits); }
    }
    const auto previous = m_deviceLimits.keys();
    for (const auto& id : previous)
    {
        if (!next.contains(id)) { forgetDevice(id); }
    }
    m_deviceLimits = std::move(next);
}

void AIWorker::evictIdleWindows()
{
    if (m_windows.isEmpty())
    {
        return;
    }

    // "Idle" is judged from the newest sample in each window, not from a separate
    // timestamp map: the window already carries the information, and a second
    // source of truth is a second thing that can drift.
    const QDateTime cutoff = QDateTime::currentDateTime().addMSecs(-kWindowIdleTimeoutMs);

    QStringList stale;
    for (auto it = m_windows.constBegin(); it != m_windows.constEnd(); ++it)
    {
        if (!it.value().empty() && it.value().back().timestamp < cutoff)
        {
            stale << it.key();
        }
    }

    for (const QString& deviceId : stale)
    {
        SAIM_DEBUG(logcat::kAi,
                   QStringLiteral("Evicting idle AI window for %1").arg(deviceId));
        forgetDevice(deviceId);
    }
}

// ---------------------------------------------------------------------------
// Vision
// ---------------------------------------------------------------------------

void AIWorker::runVisionTick()
{
    if (!visionRequested())
    {
        return;
    }

    VisionFrame frame = m_vision->capture();

    if (frame.image.isNull() && ++m_visionFailures % 30 == 1)
    {
        // Logged on the first failure and then every thirtieth, so a camera that
        // has been unplugged produces a periodic reminder rather than 5 lines a
        // second for the rest of the shift.
        SAIM_WARN(logcat::kVision,
                  QStringLiteral("Frame capture returned no image (failure %1)").arg(m_visionFailures));
    }

    if (frame.image.isNull())
    {
        return;
    }

    VisionResult result;
    result.image       = std::move(frame.image);
    result.detections  = std::move(frame.detections);
    result.inferenceMs = frame.inferenceMs;
    result.simulated   = frame.simulated;
    result.capturedAtMs = QDateTime::currentMSecsSinceEpoch();

    emit visionReady(result);
}

void AIWorker::captureVisionFrame()
{
    runVisionTick();
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

void AIWorker::generateReport(QVector<DeviceReportSection> sections, const QDateTime& from,
                              const QDateTime& to, bool exportToFile)
{
    if (m_reporter == nullptr)
    {
        emit aiError(QStringLiteral("Report generator is not available"));
        return;
    }

    const GeneratedReport report = m_reporter->generate(std::move(sections), from, to, exportToFile);

    ReportResult result;
    result.markdown   = report.markdown;
    result.html       = report.html;
    result.filePath   = report.filePath;
    result.exported   = report.exported;
    result.headline   = report.model.headline;
    result.overallRiskLevel = report.model.overallRiskLevel;
    result.deviceCount      = report.model.deviceCount;

    emit reportReady(result);

    SAIM_INFO(logcat::kAi,
              QStringLiteral("Report generated for %1 device(s), overall risk %2 %3")
                  .arg(report.model.deviceCount)
                  .arg(report.model.overallRiskLevel)
                  .arg(report.exported ? QStringLiteral("-> ") + report.filePath
                                       : QStringLiteral("(not exported)")));
}

}   // namespace saim
