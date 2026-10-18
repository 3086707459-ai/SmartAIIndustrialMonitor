#include "ai/VisionDetector.h"

#include "core/Logger.h"

#include <QDateTime>
#include <QPainter>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <deque>

#ifdef SAIM_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif

namespace saim {

namespace {

/// Intersection over union - the standard overlap measure for NMS.
float intersectionOverUnion(const QRectF& a, const QRectF& b)
{
    const QRectF intersection = a.intersected(b);
    if (intersection.isEmpty())
    {
        return 0.0f;
    }
    const qreal intersectionArea = intersection.width() * intersection.height();
    const qreal unionArea = a.width() * a.height() + b.width() * b.height() - intersectionArea;
    if (unionArea <= 0.0)
    {
        return 0.0f;
    }
    return static_cast<float>(intersectionArea / unionArea);
}

/// Default labels for the demonstration model.  Loaded from config in a real
/// deployment so that a site-specific model needs no rebuild.
QStringList defaultClassNames()
{
    return {QStringLiteral("missing_part"),
            QStringLiteral("surface_scratch"),
            QStringLiteral("surface_dent"),
            QStringLiteral("foreign_object"),
            QStringLiteral("corrosion")};
}

}   // namespace

#ifdef SAIM_HAVE_OPENCV

// ===========================================================================
// OpenCV implementation
// ===========================================================================

struct VisionDetector::Impl
{
    cv::dnn::Net net;
    cv::VideoCapture camera;
    cv::Mat inputBlob;
    std::deque<double> inferenceLatenciesMs;
};

VisionDetector::VisionDetector()
    : VisionDetector(Options{})
{
}

VisionDetector::VisionDetector(Options options)
    : m_options(std::move(options))
    , m_impl(std::make_unique<Impl>())
{
    if (m_options.classNames.isEmpty())
    {
        m_options.classNames = defaultClassNames();
    }
}

VisionDetector::~VisionDetector() = default;

void VisionDetector::setOptions(const Options& options)
{
    m_options = options;
    if (m_options.classNames.isEmpty())
    {
        m_options.classNames = defaultClassNames();
    }
}

Status VisionDetector::initialise()
{
    // --- model ------------------------------------------------------------
    if (!m_options.modelPath.isEmpty())
    {
        try
        {
            m_impl->net = cv::dnn::readNetFromONNX(m_options.modelPath.toStdString());
            if (!m_impl->net.empty())
            {
                // CPU is the only backend guaranteed present.  A deployment with
                // a GPU would set DNN_BACKEND_CUDA / DNN_TARGET_CUDA here, and
                // the fact that it is a two-line change is the reason the
                // backend choice is not baked into the call sites.
                m_impl->net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                m_impl->net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                m_hasModel = true;
                SAIM_INFO(logcat::kVision,
                          QStringLiteral("Vision model loaded: %1").arg(m_options.modelPath));
            }
        }
        catch (const cv::Exception& exception)
        {
            // Not fatal: the detector degrades to simulation, and the UI says so.
            SAIM_WARN(logcat::kVision,
                      QStringLiteral("Cannot load vision model %1: %2 - running in simulation")
                          .arg(m_options.modelPath, QString::fromUtf8(exception.what())));
        }
    }

    // --- camera -----------------------------------------------------------
    if (m_impl->camera.open(m_options.cameraIndex, cv::CAP_DSHOW))
    {
        m_impl->camera.set(cv::CAP_PROP_FRAME_WIDTH, m_options.captureSize.width());
        m_impl->camera.set(cv::CAP_PROP_FRAME_HEIGHT, m_options.captureSize.height());
        // A one-frame buffer: without this the driver queues frames and the
        // displayed image lags the physical part by a second or more, which makes
        // the system useless for anything interactive.
        m_impl->camera.set(cv::CAP_PROP_BUFFERSIZE, 1);
        m_hasCamera = true;
        SAIM_INFO(logcat::kVision,
                  QStringLiteral("Camera %1 opened at %2x%3")
                      .arg(m_options.cameraIndex)
                      .arg(static_cast<int>(m_impl->camera.get(cv::CAP_PROP_FRAME_WIDTH)))
                      .arg(static_cast<int>(m_impl->camera.get(cv::CAP_PROP_FRAME_HEIGHT))));
    }
    else
    {
        SAIM_WARN(logcat::kVision,
                  QStringLiteral("No camera on index %1 - vision runs in simulation mode")
                      .arg(m_options.cameraIndex));
    }

    m_ready = m_hasCamera || m_hasModel;
    m_simulated = !m_hasCamera;

    // Build the status string the UI shows verbatim.  Operators deserve to know
    // exactly which part of the pipeline is missing, not just "vision offline".
    QStringList parts;
    parts << (m_hasModel ? QStringLiteral("model loaded") : QStringLiteral("no model"));
    parts << (m_hasCamera ? QStringLiteral("camera live") : QStringLiteral("no camera"));
    m_statusDescription = parts.join(QStringLiteral(", "));

    if (!m_hasModel)
    {
        m_simulated = true;
        m_statusDescription += QStringLiteral(" - detections are synthetic");
    }

    return Status::ok();
}

bool VisionDetector::preprocess(const QImage& source, LetterboxTransform& transform)
{
    if (source.isNull())
    {
        return false;
    }

    // QImage (RGB) -> cv::Mat (BGR): the conversion every Qt+OpenCV integration
    // has to get right.  QImage::Format_RGB888 is converted to BGR explicitly
    // rather than passing RGB to a model trained on BGR - the classic silent
    // accuracy loss, because a red/green swap still produces *plausible* boxes.
    const QImage converted = source.convertToFormat(QImage::Format_RGB888);
    cv::Mat rgb(converted.height(), converted.width(), CV_8UC3,
                const_cast<uchar*>(converted.constBits()),
                static_cast<size_t>(converted.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

    // Letterbox: scale to fit, then pad to the model's square input.  See the
    // header for why this is not a plain resize.
    const int targetWidth = std::max(32, m_options.inputWidth);
    const int targetHeight = std::max(32, m_options.inputHeight);

    const float scale = std::min(static_cast<float>(targetWidth) / static_cast<float>(bgr.cols),
                                 static_cast<float>(targetHeight) / static_cast<float>(bgr.rows));
    const int scaledWidth = static_cast<int>(std::round(static_cast<float>(bgr.cols) * scale));
    const int scaledHeight = static_cast<int>(std::round(static_cast<float>(bgr.rows) * scale));
    const int padX = (targetWidth - scaledWidth) / 2;
    const int padY = (targetHeight - scaledHeight) / 2;

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(scaledWidth, scaledHeight), 0, 0, cv::INTER_LINEAR);

    cv::Mat padded(targetHeight, targetWidth, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(padX, padY, scaledWidth, scaledHeight)));

    // blobFromImage performs the 1/255 scaling and the NCHW layout conversion.
    // swapRB = false because the input is already BGR.
    m_impl->inputBlob = cv::dnn::blobFromImage(padded, 1.0 / 255.0,
                                               cv::Size(targetWidth, targetHeight),
                                               cv::Scalar(), false, false);

    transform.scale = scale;
    transform.padX = padX;
    transform.padY = padY;
    return true;
}

QVector<Detection> VisionDetector::detect(const QImage& image, double* inferenceMs)
{
    QVector<Detection> detections;

    if (!m_hasModel || m_impl->net.empty())
    {
        return detections;   // caller falls back to synthesis
    }

    LetterboxTransform transform;
    if (!preprocess(image, transform))
    {
        return detections;
    }

    const auto start = std::chrono::steady_clock::now();

    m_impl->net.setInput(m_impl->inputBlob);
    cv::Mat output = m_impl->net.forward();

    const auto end = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
    if (inferenceMs != nullptr)
    {
        *inferenceMs = elapsedMs;
    }
    m_impl->inferenceLatenciesMs.push_back(elapsedMs);
    if (m_impl->inferenceLatenciesMs.size() > 32)
    {
        m_impl->inferenceLatenciesMs.pop_front();
    }

    // YOLOv5/v8 export as [1, N, 5 + classes] where the row is
    // (cx, cy, w, h, objectness, class scores...).  YOLOv8 omits objectness and
    // is [1, 4 + classes, N].  Both layouts are handled because a dropped-in
    // model should not require a code change to work.
    const int dimensions = output.dims;
    if (dimensions < 2)
    {
        return detections;
    }

    const bool channelsFirst = output.size[1] < output.size[2];   // [1, 4+C, N]

    const int numClasses = static_cast<int>(m_options.classNames.size());
    const int rows = channelsFirst ? output.size[2] : output.size[1];
    const int cols = channelsFirst ? output.size[1] : output.size[2];

    const bool hasObjectness = !channelsFirst && cols == numClasses + 5;
    const int attributeOffset = hasObjectness ? 5 : 4;

    const float* data = reinterpret_cast<const float*>(output.data);

    // Accessor that abstracts the two memory layouts.  Doing this once, here,
    // keeps the decode loop readable and means the layout question is answered in
    // exactly one place.
    const auto attribute = [&](int row, int attributeIndex) -> float {
        if (channelsFirst)
        {
            return data[attributeIndex * rows + row];
        }
        return data[row * cols + attributeIndex];
    };

    for (int row = 0; row < rows; ++row)
    {
        const float objectness = hasObjectness ? attribute(row, 4) : 1.0f;
        if (objectness < m_options.confidenceThreshold)
        {
            continue;
        }

        // Best class for this anchor.
        int bestClass = -1;
        float bestScore = 0.0f;
        for (int classIndex = 0; classIndex < numClasses; ++classIndex)
        {
            const float classScore = attribute(row, attributeOffset + classIndex);
            if (classScore > bestScore)
            {
                bestScore = classScore;
                bestClass = classIndex;
            }
        }

        const float confidence = objectness * bestScore;
        if (bestClass < 0 || confidence < m_options.confidenceThreshold)
        {
            continue;
        }

        // cx, cy, w, h are in letterboxed input pixels; undo the scale and the
        // padding to get back to normalised coordinates on the original frame.
        const float centreX = attribute(row, 0);
        const float centreY = attribute(row, 1);
        const float boxWidth = attribute(row, 2);
        const float boxHeight = attribute(row, 3);

        const float x = (centreX - boxWidth * 0.5f - static_cast<float>(transform.padX)) / transform.scale;
        const float y = (centreY - boxHeight * 0.5f - static_cast<float>(transform.padY)) / transform.scale;
        const float w = boxWidth / transform.scale;
        const float h = boxHeight / transform.scale;

        const qreal imageWidth = std::max(1, image.width());
        const qreal imageHeight = std::max(1, image.height());

        // Clamp into the frame: a box that runs off the edge would be drawn
        // outside the widget and would corrupt the normalised contract.
        const qreal nx = std::clamp(static_cast<qreal>(x) / imageWidth, 0.0, 1.0);
        const qreal ny = std::clamp(static_cast<qreal>(y) / imageHeight, 0.0, 1.0);
        const qreal nw = std::clamp(static_cast<qreal>(w) / imageWidth, 0.0, 1.0 - nx);
        const qreal nh = std::clamp(static_cast<qreal>(h) / imageHeight, 0.0, 1.0 - ny);

        Detection detection;
        detection.box = QRectF(nx, ny, nw, nh);
        detection.classId = bestClass;
        detection.className = bestClass < m_options.classNames.size()
                                  ? m_options.classNames.at(bestClass)
                                  : QStringLiteral("class_%1").arg(bestClass);
        detection.confidence = confidence;
        detections.append(detection);
    }

    return applyNms(std::move(detections), m_options.nmsThreshold);
}

#else   // !SAIM_HAVE_OPENCV

// ===========================================================================
// Simulation implementation - no OpenCV, no camera, still fully demonstrable.
// ===========================================================================

struct VisionDetector::Impl
{
    std::deque<double> inferenceLatenciesMs;
};

VisionDetector::VisionDetector()
    : VisionDetector(Options{})
{
}

VisionDetector::VisionDetector(Options options)
    : m_options(std::move(options))
    , m_impl(std::make_unique<Impl>())
{
    if (m_options.classNames.isEmpty())
    {
        m_options.classNames = defaultClassNames();
    }
}

VisionDetector::~VisionDetector() = default;

void VisionDetector::setOptions(const Options& options)
{
    m_options = options;
    if (m_options.classNames.isEmpty())
    {
        m_options.classNames = defaultClassNames();
    }
}

Status VisionDetector::initialise()
{
    m_simulated = true;
    m_ready = true;
    m_statusDescription = QStringLiteral(
        "OpenCV not compiled in - synthetic frames and detections. "
        "Rebuild with -DOpenCV_DIR=<path> for real inference.");
    SAIM_WARN(logcat::kVision, m_statusDescription);
    return Status::ok();
}

bool VisionDetector::preprocess(const QImage& source, LetterboxTransform& transform)
{
    Q_UNUSED(source)
    transform = LetterboxTransform{};
    return false;
}

QVector<Detection> VisionDetector::detect(const QImage& image, double* inferenceMs)
{
    Q_UNUSED(image)
    if (inferenceMs != nullptr)
    {
        *inferenceMs = 0.0;
    }
    return {};   // capture() falls back to synthesis
}

#endif   // SAIM_HAVE_OPENCV

// ===========================================================================
// Shared implementation
// ===========================================================================

QImage VisionDetector::synthesiseFrame(int frameIndex, QVector<Detection>& outDetections) const
{
    // A deterministic pseudo-surface: brushed metal with a moving highlight, plus
    // zero or more defects whose positions follow a fixed cycle.  Deterministic
    // rather than random so that a demo - and a screenshot in a README - is
    // reproducible, and so a test can assert on it.
    constexpr int kWidth = 640;
    constexpr int kHeight = 480;

    QImage image(kWidth, kHeight, QImage::Format_RGB888);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Base surface.
    painter.fillRect(image.rect(), QColor(58, 62, 68));

    // Horizontal brushing.
    for (int y = 0; y < kHeight; y += 3)
    {
        const int shade = 58 + static_cast<int>(6.0 * std::sin(static_cast<double>(y) * 0.35));
        painter.setPen(QColor(shade, shade + 3, shade + 7));
        painter.drawLine(0, y, kWidth, y);
    }

    // A moving specular highlight, so the frame visibly changes between captures
    // and a reviewer can tell the loop is live rather than a frozen image.
    const int highlightX = (frameIndex * 11) % (kWidth + 200) - 100;
    QLinearGradient highlight(highlightX - 90, 0, highlightX + 90, 0);
    highlight.setColorAt(0.0, QColor(255, 255, 255, 0));
    highlight.setColorAt(0.5, QColor(255, 255, 255, 26));
    highlight.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.fillRect(image.rect(), highlight);

    // Panel outline, to make the frame look like an inspected part rather than
    // a gradient.
    painter.setPen(QPen(QColor(96, 102, 110), 2));
    painter.drawRoundedRect(QRectF(24, 20, kWidth - 48, kHeight - 40), 8, 8);

    // Defects: one every few frames, cycling through the class list so every
    // label is exercised.
    outDetections.clear();
    const bool defectPresent = (frameIndex % 7) != 0;   // ~6 of 7 frames are clean
    if (defectPresent)
    {
        const int classCount = std::max(1, static_cast<int>(m_options.classNames.size()));
        const int classIndex = (frameIndex / 7) % classCount;
        const int slot = frameIndex % 4;

        static const QRectF kSlots[] = {
            QRectF(0.18, 0.22, 0.16, 0.18),
            QRectF(0.58, 0.30, 0.14, 0.16),
            QRectF(0.36, 0.58, 0.18, 0.14),
            QRectF(0.66, 0.62, 0.12, 0.12),
        };
        const QRectF normalised = kSlots[slot];
        const QRectF pixelBox(normalised.x() * kWidth, normalised.y() * kHeight,
                              normalised.width() * kWidth, normalised.height() * kHeight);

        const QColor defectColour = (classIndex % 3 == 0)   ? QColor(122, 82, 58)
                                    : (classIndex % 3 == 1) ? QColor(150, 150, 158)
                                                            : QColor(92, 108, 96);
        painter.setPen(Qt::NoPen);
        painter.setBrush(defectColour);
        painter.drawEllipse(pixelBox);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(defectColour.darker(160), 2));
        painter.drawEllipse(pixelBox.adjusted(-2, -2, 2, 2));

        Detection detection;
        detection.box = normalised;
        detection.classId = classIndex;
        detection.className = classIndex < m_options.classNames.size()
                                  ? m_options.classNames.at(classIndex)
                                  : QStringLiteral("class_%1").arg(classIndex);
        // A confidence that varies plausibly rather than sitting at a constant.
        detection.confidence = static_cast<float>(0.62 + 0.30 * ((frameIndex % 5) / 5.0));
        outDetections.append(detection);
    }

    painter.end();
    return image;
}

VisionFrame VisionDetector::capture()
{
    VisionFrame frame;
    frame.captureMs = QDateTime::currentMSecsSinceEpoch();

#ifdef SAIM_HAVE_OPENCV
    if (m_hasCamera)
    {
        cv::Mat raw;
        if (m_impl->camera.read(raw) && !raw.empty())
        {
            cv::Mat rgb;
            cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);

            frame.image = QImage(rgb.data, rgb.cols, rgb.rows,
                                 static_cast<int>(rgb.step), QImage::Format_RGB888)
                              .copy();   // detach from the cv::Mat's buffer

            if (m_hasModel)
            {
                frame.detections = detect(frame.image, &frame.inferenceMs);
            }
        }
    }
#endif

    ++m_frameIndex;

    if (frame.image.isNull())
    {
        // No camera, or the grab failed.  Synthesise so the UI still shows a live
        // picture and the whole alarm path can still be demonstrated.
        QVector<Detection> synthetic;
        frame.image = synthesiseFrame(m_frameIndex, synthetic);
        frame.detections = synthetic;
        frame.simulated = true;

        m_impl->inferenceLatenciesMs.push_back(1.5);   // a nominal figure
        if (m_impl->inferenceLatenciesMs.size() > 32)
        {
            m_impl->inferenceLatenciesMs.pop_front();
        }
    }
    else
    {
        // A real frame.  An empty detection list here is a genuine "clean part"
        // verdict from the model, which must not be confused with the synthetic
        // path - the UI labels them differently.
        frame.simulated = false;
    }

    if (!frame.detections.isEmpty())
    {
        // One log line per frame with a finding, at debug level: a conveyor at
        // 5 fps would otherwise fill the log with thousands of records.
        SAIM_DEBUG(logcat::kVision,
                   QStringLiteral("Vision: %1 detection(s), top='%2' (%3)")
                       .arg(frame.detections.size())
                       .arg(frame.detections.first().className)
                       .arg(static_cast<double>(frame.detections.first().confidence), 0, 'f', 2));
    }

    return frame;
}

QVector<Detection> VisionDetector::applyNms(QVector<Detection> detections, float iouThreshold)
{
    if (detections.size() < 2)
    {
        return detections;
    }

    // Sort by confidence descending, then greedily keep a box if it does not
    // overlap a kept box of the same class too much.  Suppression is per class:
    // a scratch inside a dented region is two findings, not one.
    std::sort(detections.begin(), detections.end(),
              [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });

    QVector<Detection> kept;
    QVector<bool> suppressed(detections.size(), false);

    for (int i = 0; i < detections.size(); ++i)
    {
        if (suppressed[i])
        {
            continue;
        }
        kept.append(detections[i]);

        for (int j = i + 1; j < detections.size(); ++j)
        {
            if (suppressed[j] || detections[j].classId != detections[i].classId)
            {
                continue;
            }
            if (intersectionOverUnion(detections[i].box, detections[j].box) > iouThreshold)
            {
                suppressed[j] = true;
            }
        }
    }

    return kept;
}

double VisionDetector::medianInferenceMs() const
{
    if (m_impl->inferenceLatenciesMs.empty())
    {
        return 0.0;
    }
    std::vector<double> sorted(m_impl->inferenceLatenciesMs.begin(),
                               m_impl->inferenceLatenciesMs.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
}

}   // namespace saim
