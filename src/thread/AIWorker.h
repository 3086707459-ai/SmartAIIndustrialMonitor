#pragma once

// ---------------------------------------------------------------------------
// AIWorker.h - runs inference off the UI thread, one device window at a time.
//
// The rule this class exists to enforce
// ------------------------------------
// Inference is the single most expensive thing this application does, and the
// naive design - call the predictor from the slot that received the sample - puts
// it on whichever thread delivered the signal.  If that is the UI thread, the
// interface stutters by exactly the inference latency, every time.  If it is the
// acquisition thread, the socket read loop stalls and the gateway times out.
//
// So: the AI layer gets its own thread, and it owns its engines exclusively.
// That matters beyond latency.  An `Ort::Session` is not thread safe, and neither
// is a `cv::dnn::Net`.  Owning them outright - one thread, one owner, no sharing
// - is what makes that safe rather than merely likely to work.
//
// Two inputs, one thread
// ----------------------
// The worker consumes two independent streams:
//
//   * ingestSample   -> appended to a per-device rolling window; when the window
//     is full and the device's inference interval has elapsed, a prediction and
//     an anomaly analysis are produced.
//   * vision ticks   -> one camera frame analysed by the vision detector.
//
// Both arrive as queued calls on this thread and are processed in order.  A slow
// vision frame delays a prediction by one frame - bounded, and far better than
// the alternative of a second thread competing for the same CPU.
//
// Back-pressure
// -------------
// Analyses are rate-limited *per device* (`inferenceHz`) rather than per call.
// A device streaming at 20 Hz must not trigger 20 inferences a second: the
// verdict would not change between them, and the CPU spent producing it is
// stolen from the threads that keep the UI responsive.
//
// Why the windows live here and not in Device
// -------------------------------------------
// Device already keeps a history ring for the chart.  The AI window is a
// *different* thing: fixed-length, consumed by a model, and owned by the thread
// that runs the model.  Sharing one buffer between a paint path and an inference
// path would mean locking it on every sample - and the lock would be held across
// an inference, which is precisely the stall this design avoids.
// ---------------------------------------------------------------------------

#include "ai/AnomalyDetector.h"
#include "ai/FaultPredictor.h"
#include "ai/VisionDetector.h"
#include "common/Types.h"
#include "ai/AIReportGenerator.h"
#include "thread/ThreadedWorker.h"
#include "thread/SampleInbox.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QTimer>
#include <QMutex>
#include <QMutexLocker>
#include <atomic>

#include <deque>
#include <memory>
#include <vector>

namespace saim {

class ConfigManager;
class AIReportGenerator;

/// Everything the UI needs to render one device's AI panel.
struct AIAnalysisResult
{
    FaultPrediction prediction;
    AnomalyEvent    anomaly;
    /// Feature vector behind the two, so the assistant can explain them.
    FeatureVector   features;
    qint64          analysisMs = 0;
};

/// A finished vision analysis, ready for the camera widget.
struct VisionResult
{
    QImage  image;
    QVector<Detection> detections;
    double  inferenceMs = 0.0;
    bool    simulated = false;
    qint64  capturedAtMs = 0;
};

/// A generated maintenance report, in both forms.
struct ReportResult
{
    QString markdown;
    QString html;
    QString filePath;
    bool    exported = false;

    /// The generator's own one-line conclusion, and its overall risk band.
    /// Carried here rather than re-derived by the dialog: the component that
    /// wrote the document is the component that decided what mattered most in
    /// it, and a second opinion computed elsewhere could disagree with the text
    /// the operator is about to read.
    QString headline;
    QString overallRiskLevel;

    /// Devices covered, so the dialog can say "nothing needs attention" with
    /// confidence rather than because the preview came out empty.
    int deviceCount = 0;
};

class AIWorker final : public ThreadedWorker
{
    Q_OBJECT
public:
    explicit AIWorker(ConfigManager* config, QObject* parent = nullptr);
    ~AIWorker() override;
    bool enqueueSample(const SensorSample& sample) { return m_inbox.push(sample); }
    [[nodiscard]] SampleInbox::Statistics inboxStatistics() const { return m_inbox.statistics(); }

    [[nodiscard]] bool hasNeuralModel() const noexcept { return m_hasNeuralModel; }
    [[nodiscard]] QString predictorBackend() const { QMutexLocker lock(&m_statusMutex); return m_predictorBackend; }
    [[nodiscard]] bool visionAvailable() const noexcept { return m_visionAvailable; }
    [[nodiscard]] quint64 inferenceCount() const noexcept { return m_inferenceCount; }

public slots:
    /// Feeds one sample into its device's window, and analyses it if due.
    void ingestSample(const saim::SensorSample& sample);

    /// Builds the window and analyses immediately, ignoring the rate limit.
    /// Used by the assistant's "explain this device" action, where the operator
    /// is waiting and a half-second delay reads as a broken button.
    void analyseDeviceNow(const QString& deviceId);

    /// Re-reads limits, window sizes and model paths after a config reload.
    void applyConfiguration();
    void setDevices(const QVector<saim::DeviceConfiguration>& devices);

    /// Enables or disables the camera path at run time.
    void setVisionEnabled(bool enabled);

    /// Grabs and analyses one frame.
    void captureVisionFrame();

    /// Renders a value snapshot captured on the device registry's owner thread.
    void generateReport(QVector<saim::DeviceReportSection> sections, const QDateTime& from,
                        const QDateTime& to, bool exportToFile);

    /// Drops a removed device's window and learned baseline.
    void forgetDevice(const QString& deviceId);

signals:
    void analysisReady(const saim::AIAnalysisResult& result);
    void visionReady(const saim::VisionResult& result);
    void reportReady(const saim::ReportResult& result);

    void visionStatusChanged(const QString& description, bool simulated,
                             bool hasCamera, bool hasModel);
    void aiStatusChanged(const QString& predictorBackend, bool neural,
                         const QString& visionDescription);
    void aiError(const QString& message);

protected:
    void onStart() override;
    void onStop() override;

private slots:
    void drainInbox();
    void runVisionTick();
    void evictIdleWindows();

private:
    /// Appends to the window and trims it.  Returns true when the device is due
    /// for analysis.
    [[nodiscard]] bool accumulate(const SensorSample& sample);

    /// The full analysis for one device: features -> prediction -> anomaly.
    void analyseAndPublish(const QString& deviceId, bool force);

    [[nodiscard]] std::vector<SensorSample> windowFor(const QString& deviceId) const;
    [[nodiscard]] bool visionRequested() const noexcept { return m_visionEnabled && m_vision != nullptr; }

    ConfigManager* m_config  = nullptr;
    SampleInbox m_inbox;
    QTimer m_inboxTimer;
    QHash<QString, DeviceLimits> m_deviceLimits;

    std::unique_ptr<FaultPredictor>    m_predictor;
    std::unique_ptr<AnomalyDetector>   m_anomalyDetector;
    std::unique_ptr<VisionDetector>    m_vision;
    std::unique_ptr<AIReportGenerator> m_reporter;

    /// Rolling windows keyed by device id.  A deque rather than a QVector: the
    /// hot operation is pop_front, which is O(1) here and O(n) there.
    QHash<QString, std::deque<SensorSample>> m_windows;

    /// Next permitted analysis time per device, from `inferenceHz`.
    QHash<QString, qint64> m_nextAnalysisMs;

    QTimer        m_visionTimer;
    QTimer        m_housekeepingTimer;
    QElapsedTimer m_uptime;

    mutable QMutex m_statusMutex;
    std::atomic<bool> m_hasNeuralModel{false};
    std::atomic<bool> m_visionAvailable{false};
    bool    m_visionEnabled     = false;
    QString m_predictorBackend  = QStringLiteral("uninitialised");
    QString m_visionDescription = QStringLiteral("not started");

    std::atomic<quint64> m_inferenceCount{0};
    quint64 m_visionFailures = 0;

    int m_windowSize  = 60;
    int m_inferenceHz = 2;
    int m_visionFps   = 5;

    /// ms between analyses for one device, derived from m_inferenceHz.
    qint64 m_analysisIntervalMs = 500;
};

}   // namespace saim

Q_DECLARE_METATYPE(saim::AIAnalysisResult)
Q_DECLARE_METATYPE(saim::VisionResult)
Q_DECLARE_METATYPE(saim::ReportResult)
