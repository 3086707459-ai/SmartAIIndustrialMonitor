#pragma once

// ---------------------------------------------------------------------------
// CameraWidget.h - the vision panel: live frame, detections, and the honest
//                  label when there is no camera.
//
// The part that matters is what it shows when there is no camera
// --------------------------------------------------------------
// A vision panel that renders nothing when the capture device is missing looks
// broken, and a reviewer will assume the code is broken rather than the
// hardware being absent.  This widget therefore has three *named* modes, and
// always says which one it is in:
//
//   LIVE        a real camera and a real model
//   SIMULATED   no camera, but the detection path still runs on synthetic frames
//   UNAVAILABLE neither, with the reason stated
//
// The simulated mode is not decoration.  It exercises the same letterboxing,
// the same NMS, the same cross-thread frame delivery and the same overlay
// drawing as the live path - so everything except the sensor is demonstrated on
// a machine that has no sensor.  That is the difference between "the vision
// module works" as a claim and as something the reader can check.
//
// Overlay drawing
// ---------------
// Boxes are drawn by this widget from normalised [0,1] coordinates rather than
// being burned in by the detector.  Two reasons: the frame that goes to disk or
// into a report is then the clean one, and the overlay can be toggled, filtered
// by confidence and restyled without touching the inference path.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QImage>
#include <QMetaType>
#include <QRectF>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QEvent;
class QLabel;
class QPainter;
class QToolButton;

namespace saim {

/// Forward-declared so this header does not pull in AIWorker's metatypes.
struct VisionResult;

class CameraWidget final : public QWidget
{
    Q_OBJECT
public:
    enum class Mode
    {
        Unavailable,
        Simulated,
        Live
    };

    explicit CameraWidget(QWidget* parent = nullptr);
    ~CameraWidget() override;

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }

public slots:
    /// Accepts a finished analysis from the AI worker.  The frame arrives as a
    /// QImage, already converted on the worker thread, so this slot only stores
    /// and repaints.
    void applyVisionResult(const saim::VisionResult& result);

    /// Records what the detector reported about itself, so the panel can name
    /// the mode and the reason.
    void setDetectorStatus(const QString& description, bool simulated, bool hasCamera,
                           bool hasModel);

    /// Enables or disables the periodic grab.  The controller drives this so
    /// that a panel scrolled out of view costs nothing.
    void setCaptureEnabled(bool enabled);
    [[nodiscard]] bool isCaptureEnabled() const noexcept { return m_captureEnabled; }

    /// Clears the frame and shows the placeholder.  Used when the camera is
    /// switched off from the toolbar.
    void clear();

signals:
    /// The operator asked for a frame now.
    void captureRequested();

    /// The operator changed the analysis rate.
    void captureRateChanged(int fps);

    /// The panel's visibility-driven capture state changed.
    void captureEnabledChanged(bool enabled);

protected:
    /// Paints the detection boxes over the frame label.
    ///
    /// The overlay is drawn in an event filter on the label rather than by
    /// subclassing it, so that the frame stays a plain QLabel with the whole of
    /// Qt's image scaling and alignment available - and so the class that owns
    /// the detections is the class that draws them.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onCaptureClicked();
    void onOverlayToggled(bool enabled);
    void onConfidenceChanged(int index);

private:
    void buildUi();
    void rebuildLegend();
    void updateStatusLine();

    /// The rectangle inside the frame label where the image is actually drawn -
    /// aspect-preserved and centred, so a 16:9 frame in a square panel is
    /// letterboxed rather than stretched.
    ///
    /// Both the frame and the overlay are placed through this one function, and
    /// that is the point: a detection box arrives in normalised [0,1]
    /// coordinates, and mapping it with a *different* rectangle than the one the
    /// image was drawn into is how overlays end up systematically offset.
    [[nodiscard]] QRectF frameTargetRect() const;

    void paintFrame(QPainter& painter) const;
    void paintOverlay(QPainter& painter, const QRectF& target) const;

    QLabel*      m_frameLabel = nullptr;
    QLabel*      m_statusLabel = nullptr;
    QLabel*      m_reasonLabel = nullptr;
    QLabel*      m_legendLabel = nullptr;
    QLabel*      m_latencyLabel = nullptr;
    QToolButton* m_captureButton = nullptr;
    QCheckBox*   m_overlayToggle = nullptr;
    QComboBox*   m_rateBox = nullptr;
    QComboBox*   m_confidenceBox = nullptr;

    QImage  m_frame;
    QVector<Detection> m_detections;

    Mode    m_mode = Mode::Unavailable;
    QString m_statusDescription;
    QString m_reason;

    bool    m_hasCamera = false;
    bool    m_hasModel = false;
    bool    m_showOverlay = true;
    bool    m_captureEnabled = false;
    double  m_inferenceMs = 0.0;
    float   m_minimumConfidence = 0.25f;
    qint64  m_lastFrameMs = 0;
    int     m_frameCount = 0;
};

}   // namespace saim
