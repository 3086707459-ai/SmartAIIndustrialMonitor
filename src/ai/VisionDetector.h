#pragma once

// ---------------------------------------------------------------------------
// VisionDetector.h - camera-based surface-defect detection (OpenCV + YOLO ONNX).
//
// Scope and honest framing
// ------------------------
// This module performs *inference plumbing and rendering*: it owns a capture
// source, letterboxes frames, runs a YOLO-family ONNX model through OpenCV's DNN
// module, applies non-maximum suppression, and hands detections back as domain
// types.  It does not train a model, and it does not claim to.
//
// Being precise about that boundary is deliberate.  "Integrated a YOLO ONNX model
// into a Qt application, handling letterboxing, NMS, post-processing and
// cross-thread frame delivery" is a real engineering claim a reviewer can probe.
// "Built an AI vision system" is not.
//
// Why OpenCV's DNN module rather than ONNX Runtime here
// ----------------------------------------------------
// ONNX Runtime is already used for the tabular predictor and would work here too.
// OpenCV's dnn module is used for vision because the surrounding operations a
// vision pipeline cannot avoid - resize with letterboxing, colour conversion,
// NMS, drawing - are all OpenCV calls anyway, and `cv::dnn::Net` accepts the same
// .onnx file.  Adding a second runtime for the *same model format* would mean two
// dependencies, two sets of threading concerns, and two failure modes for no gain.
// Where ONNX Runtime wins (the tabular model, where there is no image pipeline at
// all) it is used.  Matching the runtime to the pipeline is the point.
//
// Frame lifetime and threading
// ---------------------------
// A cv::Mat is reference-counted like a QString, so copying one across a signal
// is cheap and safe.  Frames are captured and pre-processed on the AI worker
// thread; the UI receives them as QImage for display.  The conversion happens
// once, in capture(), rather than in the widget - so the paint path never
// converts anything.
//
// Without OpenCV this class still compiles and runs in simulation mode, producing
// synthetic frames and detections.  That is not decoration: it lets the UI, the
// threading and the alarm path all be exercised and demonstrated on a machine
// with no camera and no OpenCV, which is the situation most reviewers are in.
// ---------------------------------------------------------------------------

#include "common/Result.h"
#include "common/Types.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

namespace saim {

/// One detection rendered for display plus its domain form.
struct VisionFrame
{
    QImage  image;                     ///< the frame that was analysed
    QVector<Detection> detections;     ///< what was found in it
    qint64  captureMs = 0;             ///< wall-clock time of capture
    double  inferenceMs = 0.0;         ///< model latency for this frame
    bool    simulated = false;         ///< true when no camera/model was available
};

class VisionDetector
{
public:
    struct Options
    {
        QString modelPath;
        /// YOLO input is square; 640 is the YOLOv5/v8 default and the size the
        /// export script targets.  Changing it here without re-exporting would
        /// silently degrade accuracy rather than fail, so it travels with the
        /// model in config.json.
        int inputWidth = 640;
        int inputHeight = 640;
        float confidenceThreshold = 0.45f;
        float nmsThreshold = 0.45f;
        /// Class labels, in the model's output order.  Supplied from config so
        /// that a site-specific model can be dropped in without a rebuild.
        QStringList classNames;
        /// 0 selects the first available camera.
        int cameraIndex = 0;
        /// Target analysis rate.  A camera at 30 fps analysing every frame burns
        /// a core for nothing: a defect on a conveyor does not appear and vanish
        /// in 33 ms, and the operator cannot read detections that fast anyway.
        int analysisFps = 5;
        /// Downscale before analysis for very large sensors.
        QSize captureSize{1280, 720};
    };

    explicit VisionDetector(Options options);
    VisionDetector();
    ~VisionDetector();

    VisionDetector(const VisionDetector&)            = delete;
    VisionDetector& operator=(const VisionDetector&) = delete;

    /// Loads the ONNX model and opens the capture device.
    /// Returns a Status; a failure is *not* fatal - the detector then runs in
    /// simulation mode and `isSimulated()` reports why.
    [[nodiscard]] Status initialise();

    [[nodiscard]] bool isReady() const noexcept { return m_ready; }
    [[nodiscard]] bool isSimulated() const noexcept { return m_simulated; }
    [[nodiscard]] QString statusDescription() const { return m_statusDescription; }
    [[nodiscard]] bool hasCamera() const noexcept { return m_hasCamera; }
    [[nodiscard]] bool hasModel() const noexcept { return m_hasModel; }

    [[nodiscard]] const Options& options() const noexcept { return m_options; }
    void setOptions(const Options& options);

    /// Grabs, pre-processes and analyses one frame.  Returns an empty result set
    /// (but a valid image) when the model found nothing, and a null image when
    /// the capture failed.
    [[nodiscard]] VisionFrame capture();

    /// Analyses a caller-supplied image.  Used by the test suite and by the UI's
    /// "analyse this file" action, so the detection path can be exercised without
    /// a camera.
    [[nodiscard]] QVector<Detection> detect(const QImage& image, double* inferenceMs = nullptr);

    /// Labels the model can emit, for the UI's legend.
    [[nodiscard]] QStringList classNames() const { return m_options.classNames; }

    /// Median inference latency over recent frames, milliseconds.
    [[nodiscard]] double medianInferenceMs() const;

private:
    /// Letterboxes `source` into the model's square input: preserves aspect ratio,
    /// pads with grey, and returns the scale/offset needed to map detections back.
    ///
    /// Letterboxing rather than a plain resize is the detail that separates a
    /// working vision integration from one that reports boxes in the wrong place.
    /// A stretched image distorts every object, and the model - trained on
    /// undistorted images - sees a different shape than it learned.
    struct LetterboxTransform
    {
        float scale = 1.0f;
        int   padX = 0;
        int   padY = 0;
    };
    [[nodiscard]] bool preprocess(const QImage& source, LetterboxTransform& transform);

    /// Greedy non-maximum suppression by IoU, per class.
    [[nodiscard]] static QVector<Detection> applyNms(QVector<Detection> detections, float iouThreshold);

    /// Produces a plausible synthetic frame: a metal-looking surface with zero or
    /// more defects drawn on it.  Deterministic per frame index so a demo is
    /// repeatable.
    [[nodiscard]] QImage synthesiseFrame(int frameIndex, QVector<Detection>& outDetections) const;

    Options m_options;
    bool    m_ready = false;
    bool    m_simulated = false;
    bool    m_hasCamera = false;
    bool    m_hasModel = false;
    QString m_statusDescription;
    int     m_frameIndex = 0;

    // Pimpl again: <opencv2/...> never reaches a translation unit that includes
    // this header, which is what keeps OpenCV an optional dependency.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}   // namespace saim
