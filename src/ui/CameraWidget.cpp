#include "ui/CameraWidget.h"

#include "thread/AIWorker.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace saim {

namespace {

/// Confidence options offered to the operator, and why a list rather than a
/// slider: these are the four numbers anyone doing vision work actually thinks
/// in.  A slider invites "0.37", which is a value nobody has a reason to pick
/// and which makes the filter impossible to describe in a report.
struct ConfidenceOption
{
    const char* label;
    float       value;
};

const ConfidenceOption kConfidenceOptions[] = {
    {"All detections",  0.00f},
    {"≥ 25 % (default)", 0.25f},
    {"≥ 50 %",          0.50f},
    {"≥ 75 %",          0.75f},
};

/// Frame-analysis rates.  Every one of these is *below* the camera's native
/// rate on purpose - see Options::analysisFps in VisionDetector.h.
const int kRateOptions[] = {2, 5, 10, 15};

/// The placeholder drawn when there is no frame at all.  A cross-hatched "no
/// signal" card rather than an empty black rectangle: an empty rectangle reads
/// as a rendering bug, and the first thing anyone does with a vision panel is
/// assume it is broken.
void paintNoSignal(QPainter& painter, const QRectF& rect, const QString& message)
{
    painter.save();

    QLinearGradient shade(rect.topLeft(), rect.bottomRight());
    shade.setColorAt(0.0, Theme::surface());
    shade.setColorAt(1.0, Theme::background());
    painter.fillRect(rect, shade);

    painter.setPen(QPen(Theme::border(), 1.0));
    painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));

    // Diagonals, drawn faintly.  Enough to read as "no signal" at a glance
    // without competing with the text.
    QColor hatch = Theme::border();
    hatch.setAlpha(70);
    painter.setPen(QPen(hatch, 1.0));
    const double step = 22.0;
    for (double offset = -rect.height(); offset < rect.width(); offset += step)
    {
        painter.drawLine(QPointF(rect.left() + offset, rect.bottom()),
                         QPointF(rect.left() + offset + rect.height(), rect.top()));
    }

    painter.setPen(Theme::textMuted());
    QFont font = painter.font();
    font.setPointSizeF(std::max(9.0, font.pointSizeF()));
    painter.setFont(font);
    painter.drawText(rect, Qt::AlignCenter, message);

    painter.restore();
}

}   // namespace

// ---------------------------------------------------------------------------

CameraWidget::CameraWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("CameraWidget"));
    buildUi();
    updateStatusLine();
}

CameraWidget::~CameraWidget() = default;

void CameraWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(Theme::Metrics::kPanelSpacing);

    // -- viewport -----------------------------------------------------------
    m_frameLabel = new QLabel(this);
    m_frameLabel->setObjectName(QStringLiteral("CameraViewport"));
    m_frameLabel->setAlignment(Qt::AlignCenter);
    m_frameLabel->setMinimumSize(240, 150);
    m_frameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_frameLabel->setText(QString());   // everything is painted, never laid out
    m_frameLabel->installEventFilter(this);
    root->addWidget(m_frameLabel, 1);

    // -- status line --------------------------------------------------------
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("CameraStatus"));
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);

    m_latencyLabel = new QLabel(this);
    m_latencyLabel->setObjectName(QStringLiteral("CameraLatency"));
    m_latencyLabel->setTextFormat(Qt::PlainText);
    m_latencyLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // The reason line only appears when something is missing, and it says which
    // thing.  Kept as its own label rather than appended to the status line so
    // that it can be styled as a warning without colouring the whole line.
    m_reasonLabel = new QLabel(this);
    m_reasonLabel->setObjectName(QStringLiteral("CameraReason"));
    m_reasonLabel->setTextFormat(Qt::PlainText);
    m_reasonLabel->setWordWrap(true);
    m_reasonLabel->hide();

    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(Theme::Metrics::kPanelSpacing);
    statusRow->addWidget(m_statusLabel, 1);
    statusRow->addWidget(m_latencyLabel, 0);

    root->addLayout(statusRow);
    root->addWidget(m_reasonLabel);

    // -- controls -----------------------------------------------------------
    m_captureButton = new QToolButton(this);
    m_captureButton->setText(tr("Grab frame"));
    m_captureButton->setToolTip(tr("Analyse one frame immediately, ignoring the analysis rate"));
    connect(m_captureButton, &QToolButton::clicked, this, &CameraWidget::onCaptureClicked);

    m_overlayToggle = new QCheckBox(tr("Overlay"), this);
    m_overlayToggle->setChecked(true);
    m_overlayToggle->setToolTip(tr("Draw the detection boxes over the frame"));
    connect(m_overlayToggle, &QCheckBox::toggled, this, &CameraWidget::onOverlayToggled);

    m_confidenceBox = new QComboBox(this);
    m_confidenceBox->setToolTip(tr("Hide detections below this confidence"));
    for (const ConfidenceOption& option : kConfidenceOptions)
    {
        m_confidenceBox->addItem(tr(option.label), option.value);
    }
    m_confidenceBox->setCurrentIndex(1);
    connect(m_confidenceBox, &QComboBox::currentIndexChanged,
            this, &CameraWidget::onConfidenceChanged);

    m_rateBox = new QComboBox(this);
    m_rateBox->setToolTip(tr("Frames analysed per second"));
    for (const int rate : kRateOptions)
    {
        m_rateBox->addItem(tr("%1 fps").arg(rate), rate);
    }
    m_rateBox->setCurrentIndex(1);   // 5 fps, the VisionDetector default
    connect(m_rateBox, &QComboBox::currentIndexChanged, this, [this](int index)
    {
        const int rate = m_rateBox->itemData(index).toInt();
        if (rate > 0)
        {
            emit captureRateChanged(rate);
        }
    });

    auto* controlRow = new QHBoxLayout();
    controlRow->setContentsMargins(0, 0, 0, 0);
    controlRow->setSpacing(Theme::Metrics::kPanelSpacing);
    controlRow->addWidget(m_captureButton, 0);
    controlRow->addWidget(m_overlayToggle, 0);
    controlRow->addWidget(m_confidenceBox, 1);
    controlRow->addWidget(m_rateBox, 0);
    controlRow->addStretch(1);
    root->addLayout(controlRow);

    // -- legend -------------------------------------------------------------
    m_legendLabel = new QLabel(this);
    m_legendLabel->setObjectName(QStringLiteral("CameraLegend"));
    m_legendLabel->setTextFormat(Qt::RichText);
    m_legendLabel->setWordWrap(true);
    root->addWidget(m_legendLabel);
}

// ---------------------------------------------------------------------------
// Detector status
// ---------------------------------------------------------------------------

void CameraWidget::setDetectorStatus(const QString& description, bool simulated,
                                     bool hasCamera, bool hasModel)
{
    m_statusDescription = description;
    m_hasCamera = hasCamera;
    m_hasModel  = hasModel;

    // The mode is derived, never supplied.  A caller passing "live" while
    // hasCamera is false would produce a panel that lies about its own state,
    // which is exactly the failure this widget exists to avoid.
    if (!simulated && hasCamera && hasModel)
    {
        m_mode = Mode::Live;
    }
    else if (simulated || hasCamera || hasModel)
    {
        m_mode = Mode::Simulated;
    }
    else
    {
        m_mode = Mode::Unavailable;
    }

    // Name what is missing, in the operator's terms rather than the code's.
    QStringList missing;
    if (!hasCamera)
    {
        missing << tr("no capture device");
    }
    if (!hasModel)
    {
        missing << tr("no ONNX model");
    }

    if (m_mode == Mode::Live)
    {
        m_reason.clear();
        m_reasonLabel->hide();
    }
    else
    {
        m_reason = missing.isEmpty()
                       ? tr("The detector reported no camera and no model, so frames "
                            "cannot be produced or analysed.")
                       : tr("%1. Detections below are synthetic, produced by the same "
                            "pipeline that the live path uses — they demonstrate the "
                            "plumbing, not the accuracy of a trained model.")
                             .arg(missing.join(QStringLiteral(", ")));
        m_reasonLabel->setText(m_reason);
        m_reasonLabel->show();
    }

    updateStatusLine();
    update();
}

void CameraWidget::updateStatusLine()
{
    const QString modeText = [this]
    {
        switch (m_mode)
        {
        case Mode::Live:        return tr("LIVE");
        case Mode::Simulated:   return tr("SIMULATED");
        case Mode::Unavailable: return tr("UNAVAILABLE");
        }
        return tr("UNAVAILABLE");
    }();

    // A dot in the mode's own colour, then the mode word.  Colour is a second
    // channel, not the only one - the word is always there for a reader who
    // cannot distinguish red from green.
    const QColor modeColour = m_mode == Mode::Live        ? Theme::normal()
                              : m_mode == Mode::Simulated ? Theme::warning()
                                                          : Theme::offline();

    const QString description = m_statusDescription.isEmpty() ? tr("detector not started")
                                                              : m_statusDescription;

    m_statusLabel->setText(QStringLiteral("<span style='color:%1'>&#9679;</span> "
                                          "<b>%2</b> &nbsp; %3")
                               .arg(modeColour.name(), modeText, description.toHtmlEscaped()));
    m_statusLabel->setTextFormat(Qt::RichText);

    if (m_frame.isNull())
    {
        m_latencyLabel->setText(tr("no frames yet"));
        return;
    }

    m_latencyLabel->setText(tr("%1 ms  ·  frame %2")
                                .arg(m_inferenceMs, 0, 'f', 1)
                                .arg(m_frameCount));
}

void CameraWidget::rebuildLegend()
{
    if (m_detections.isEmpty())
    {
        m_legendLabel->clear();
        m_legendLabel->hide();
        return;
    }

    // One chip per distinct class present, with its count and the best
    // confidence seen.  Counting per class rather than per detection is what
    // makes the legend readable when a frame has nine boxes of two kinds.
    struct Aggregate
    {
        int   count = 0;
        float best  = 0.0f;
    };
    QHash<QString, Aggregate> byClass;
    QStringList order;   // preserve first-seen order, which follows confidence

    const auto accepted = [this](const Detection& detection)
    {
        return detection.confidence >= m_minimumConfidence;
    };

    for (const Detection& detection : m_detections)
    {
        if (!accepted(detection))
        {
            continue;
        }
        const QString key = detection.className.isEmpty()
                                ? tr("class %1").arg(detection.classId)
                                : detection.className;
        if (!byClass.contains(key))
        {
            order << key;
        }
        Aggregate& aggregate = byClass[key];
        ++aggregate.count;
        aggregate.best = std::max(aggregate.best, detection.confidence);
    }

    if (order.isEmpty())
    {
        m_legendLabel->clear();
        m_legendLabel->hide();
        return;
    }

    QStringList chips;
    for (const QString& key : order)
    {
        const Aggregate& aggregate = byClass.value(key);
        const QColor colour = Theme::riskColor(100.0 - static_cast<double>(aggregate.best) * 100.0);
        chips << QStringLiteral("<span style='color:%1'>&#9632;</span> %2 ×%3 (%4 %)")
                     .arg(colour.name(),
                          key.toHtmlEscaped(),
                          QString::number(aggregate.count),
                          QString::number(static_cast<int>(aggregate.best * 100.0f)));
    }

    m_legendLabel->setText(chips.join(QStringLiteral("&nbsp;&nbsp;&nbsp;")));
    m_legendLabel->show();
}

// ---------------------------------------------------------------------------
// Frame intake
// ---------------------------------------------------------------------------

void CameraWidget::applyVisionResult(const VisionResult& result)
{
    // A null image means the capture itself failed on the worker side.  Holding
    // the previous frame is the right call: blanking the panel on one dropped
    // frame would flicker, and the reader would lose the last good frame they
    // were looking at.
    if (!result.image.isNull())
    {
        m_frame = result.image;
        ++m_frameCount;
    }

    m_detections = result.detections;
    m_inferenceMs = result.inferenceMs;
    m_lastFrameMs = result.capturedAtMs;

    // A simulated result arriving while the detector reports live (or vice
    // versa) would mean the two sides disagree; trust the frame.
    if (result.simulated && m_mode == Mode::Live)
    {
        m_mode = Mode::Simulated;
    }

    rebuildLegend();
    updateStatusLine();
    update();
}

void CameraWidget::clear()
{
    m_frame = QImage();
    m_detections.clear();
    m_inferenceMs = 0.0;
    m_frameCount = 0;
    rebuildLegend();
    updateStatusLine();
    update();
}

// ---------------------------------------------------------------------------
// Capture gating
// ---------------------------------------------------------------------------

void CameraWidget::setCaptureEnabled(bool enabled)
{
    if (m_captureEnabled == enabled)
    {
        return;
    }
    m_captureEnabled = enabled;
    if (!enabled)
    {
        clear();
    }
    emit captureEnabledChanged(enabled);
}

void CameraWidget::onCaptureClicked()
{
    emit captureRequested();
}

void CameraWidget::onOverlayToggled(bool enabled)
{
    m_showOverlay = enabled;
    update();
}

void CameraWidget::onConfidenceChanged(int index)
{
    const float value = m_confidenceBox->itemData(index).toFloat();
    m_minimumConfidence = value;
    rebuildLegend();
    update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

bool CameraWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_frameLabel || event->type() != QEvent::Paint)
    {
        return QWidget::eventFilter(watched, event);
    }

    QPainter painter(m_frameLabel);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    paintFrame(painter);

    if (m_showOverlay && !m_detections.isEmpty())
    {
        paintOverlay(painter, frameTargetRect());
    }

    return true;   // handled: QLabel's own paint would draw nothing anyway
}

QRectF CameraWidget::frameTargetRect() const
{
    const QRectF viewport = m_frameLabel->rect();
    if (m_frame.isNull() || viewport.isEmpty())
    {
        return viewport;
    }

    const double frameAspect = static_cast<double>(m_frame.width()) /
                               static_cast<double>(std::max(1, m_frame.height()));
    const double viewAspect = viewport.width() / std::max(1.0, viewport.height());

    // Aspect-preserving fit.  Stretching a frame to fill the panel would distort
    // every detection box along with it - and a circular defect is by far the
    // easiest thing for a human to notice is no longer circular.
    double width  = viewport.width();
    double height = viewport.height();
    if (frameAspect > viewAspect)
    {
        height = width / frameAspect;
    }
    else
    {
        width = height * frameAspect;
    }

    return QRectF(viewport.left() + (viewport.width() - width) / 2.0,
                  viewport.top() + (viewport.height() - height) / 2.0,
                  width, height);
}

void CameraWidget::paintFrame(QPainter& painter) const
{
    const QRectF viewport = m_frameLabel->rect();

    if (m_frame.isNull())
    {
        const QString message = m_mode == Mode::Unavailable
                                    ? tr("No camera and no model available")
                                    : tr("Waiting for the first frame…");
        paintNoSignal(painter, viewport.adjusted(0.5, 0.5, -0.5, -0.5), message);
        return;
    }

    const QRectF target = frameTargetRect();

    // Letterbox bars, so the frame boundary is unambiguous when the panel and
    // the sensor disagree about aspect ratio.
    painter.fillRect(viewport, Theme::background());
    painter.drawImage(target, m_frame, m_frame.rect());

    painter.setPen(QPen(Theme::border(), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(target.adjusted(0.5, 0.5, -0.5, -0.5));
}

void CameraWidget::paintOverlay(QPainter& painter, const QRectF& target) const
{
    painter.save();

    // Boxes are drawn in *decreasing* confidence order so that the strongest
    // detection ends up on top.  With overlapping boxes - which is the normal
    // case when a defect is partially occluded - the one the operator should
    // trust is the one they can read.
    QVector<Detection> ordered;
    ordered.reserve(m_detections.size());
    for (const Detection& detection : m_detections)
    {
        if (detection.confidence >= m_minimumConfidence)
        {
            ordered.append(detection);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Detection& lhs, const Detection& rhs)
              {
                  return lhs.confidence > rhs.confidence;
              });

    QFont labelFont(Theme::numericFontFamily());
    labelFont.setPointSizeF(std::max(7.5, painter.font().pointSizeF() - 0.5));
    labelFont.setBold(true);
    painter.setFont(labelFont);

    for (const Detection& detection : ordered)
    {
        // Normalised top-left/w-h mapped through the *same* rectangle the image
        // was drawn into.  See frameTargetRect().
        const QRectF box(target.left() + detection.box.x() * target.width(),
                         target.top() + detection.box.y() * target.height(),
                         detection.box.width() * target.width(),
                         detection.box.height() * target.height());

        // Colour by confidence, not by class: in a defect-detection context the
        // question the operator is asking is "how sure is it", and the class
        // name is already in the label.  Theme::riskColor gives the same
        // green-amber-red convention used everywhere else in the application.
        const QColor colour = Theme::riskColor(100.0 - static_cast<double>(detection.confidence) * 100.0);

        QColor fill = colour;
        fill.setAlpha(38);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRect(box);

        painter.setPen(QPen(colour, 1.6));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);

        // Corner ticks.  They make a box legible over a busy surface, where a
        // one-pixel outline disappears into the texture of the part.
        const double tick = std::min(12.0, std::min(box.width(), box.height()) * 0.28);
        painter.setPen(QPen(colour, 2.4));
        painter.drawLine(box.topLeft(), box.topLeft() + QPointF(tick, 0));
        painter.drawLine(box.topLeft(), box.topLeft() + QPointF(0, tick));
        painter.drawLine(box.topRight(), box.topRight() + QPointF(-tick, 0));
        painter.drawLine(box.topRight(), box.topRight() + QPointF(0, tick));
        painter.drawLine(box.bottomLeft(), box.bottomLeft() + QPointF(tick, 0));
        painter.drawLine(box.bottomLeft(), box.bottomLeft() + QPointF(0, -tick));
        painter.drawLine(box.bottomRight(), box.bottomRight() + QPointF(-tick, 0));
        painter.drawLine(box.bottomRight(), box.bottomRight() + QPointF(0, -tick));

        // -- label ----------------------------------------------------------
        const QString text = QStringLiteral("%1  %2 %")
                                 .arg(detection.className.isEmpty()
                                          ? tr("class %1").arg(detection.classId)
                                          : detection.className)
                                 .arg(static_cast<int>(detection.confidence * 100.0f));

        const QFontMetricsF metrics(labelFont);
        const double textWidth = metrics.horizontalAdvance(text) + 8.0;
        const double textHeight = metrics.height() + 2.0;

        // Above the box normally, below it when there is no room above - which
        // happens for every detection touching the top edge, i.e. most of them
        // on a camera aimed at a conveyor.
        QRectF labelRect(box.left(), box.top() - textHeight, textWidth, textHeight);
        if (labelRect.top() < target.top())
        {
            labelRect.moveTop(box.bottom());
        }
        labelRect.moveLeft(std::min(labelRect.left(),
                                    target.right() - labelRect.width()));

        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawRect(labelRect);

        // Black or white on the chip, whichever contrasts.  A fixed colour would
        // be unreadable on at least one of the three risk colours.
        painter.setPen(colour.lightness() > 150 ? QColor(20, 20, 20) : QColor(240, 240, 240));
        painter.drawText(labelRect, Qt::AlignCenter, text);
    }

    painter.restore();
}

}   // namespace saim
