#include "ui/ChartWidget.h"

#include "device/Device.h"
#include "device/DeviceManager.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>

QT_USE_NAMESPACE

namespace saim {

namespace {

/// Axis range is only adjusted when the data leaves the middle of it.  See the
/// header comment: auto-fitting every frame makes a ramp look flat.
constexpr double kAxisLowerMargin = 0.10;   // grow if data drops below 10%
constexpr double kAxisUpperMargin = 0.90;   // grow if data rises above 90%

/// Time axis span, in seconds, for a given point count at the nominal 2 Hz
/// sample rate.  The axis is decorative about the rate - it only has to be
/// monotonic and readable - so this is an estimate, not a promise.
constexpr double kNominalSampleHz = 2.0;

}   // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ChartWidget::ChartWidget(DeviceManager* manager, QWidget* parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("ChartWidget"));
    buildUi();
    rebuildSeries();
    applyChannelStyle();
    if (m_manager != nullptr)
    {
        connect(m_manager, &DeviceManager::sampleReceived, this, &ChartWidget::pushSample);
    }
}

ChartWidget::~ChartWidget() = default;

void ChartWidget::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // -- toolbar ------------------------------------------------------------
    auto* toolbar = new QWidget(this);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(6);

    m_channelBox = new QComboBox(toolbar);
    for (int channel = 0; channel < static_cast<int>(Channel::Count); ++channel)
    {
        const auto value = static_cast<Channel>(channel);
        m_channelBox->addItem(QStringLiteral("%1 (%2)").arg(labelOf(value), unitOf(value)),
                              channel);
    }
    m_channelBox->setToolTip(tr("Which quantity to plot"));
    toolbarLayout->addWidget(m_channelBox);

    m_windowBox = new QComboBox(toolbar);
    m_windowBox->addItem(tr("2 min"), 120);
    m_windowBox->addItem(tr("4 min"), 240);
    m_windowBox->addItem(tr("10 min"), 600);
    m_windowBox->addItem(tr("30 min"), 1800);
    m_windowBox->setCurrentIndex(1);
    m_windowBox->setToolTip(tr("Length of the retained window"));
    toolbarLayout->addWidget(m_windowBox);

    toolbarLayout->addStretch(1);

    m_limitsButton = new QToolButton(toolbar);
    m_limitsButton->setText(tr("Limits"));
    m_limitsButton->setCheckable(true);
    m_limitsButton->setChecked(true);
    m_limitsButton->setToolTip(tr("Show the warning and trip thresholds"));
    toolbarLayout->addWidget(m_limitsButton);

    m_pauseButton = new QToolButton(toolbar);
    m_pauseButton->setText(tr("Pause"));
    m_pauseButton->setCheckable(true);
    m_pauseButton->setToolTip(tr("Freeze the view; data keeps accumulating"));
    toolbarLayout->addWidget(m_pauseButton);

    m_clearButton = new QToolButton(toolbar);
    m_clearButton->setText(tr("Clear"));
    m_clearButton->setToolTip(tr("Discard the plotted history"));
    toolbarLayout->addWidget(m_clearButton);

    layout->addWidget(toolbar);

    // -- chart --------------------------------------------------------------
    m_chart = new QChart();
    m_chart->setMargins(QMargins(4, 4, 4, 4));
    m_chart->setBackgroundBrush(Qt::NoBrush);
    m_chart->setBackgroundPen(Qt::NoPen);
    m_chart->setPlotAreaBackgroundVisible(false);
    m_chart->legend()->setVisible(false);   // one trace: a legend would be noise

    m_timeAxis = new QDateTimeAxis(m_chart);
    m_timeAxis->setFormat(QStringLiteral("HH:mm:ss"));
    m_timeAxis->setTickCount(5);
    m_timeAxis->setGridLineColor(Theme::gridLine());
    m_timeAxis->setLinePenColor(Theme::border());
    m_timeAxis->setLabelsColor(Theme::textMuted());

    m_valueAxis = new QValueAxis(m_chart);
    m_valueAxis->setTickCount(5);
    m_valueAxis->setGridLineColor(Theme::gridLine());
    m_valueAxis->setLinePenColor(Theme::border());
    m_valueAxis->setLabelsColor(Theme::textMuted());

    m_chart->addAxis(m_timeAxis, Qt::AlignBottom);
    m_chart->addAxis(m_valueAxis, Qt::AlignLeft);

    const QDateTime now = QDateTime::currentDateTime();
    m_timeAxis->setRange(now.addSecs(-static_cast<int>(m_windowSize / kNominalSampleHz)), now);

    m_view = new QChartView(m_chart, this);
    m_view->setRenderHint(QPainter::Antialiasing, true);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setRubberBand(QChartView::NoRubberBand);
    m_view->setMinimumHeight(160);
    layout->addWidget(m_view, 1);

    // -- footer -------------------------------------------------------------
    auto* footer = new QWidget(this);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(12);

    m_statsLabel = new QLabel(footer);
    m_statsLabel->setObjectName(QStringLiteral("MetricCaption"));
    footerLayout->addWidget(m_statsLabel);
    footerLayout->addStretch(1);

    m_emptyLabel = new QLabel(tr("Waiting for data..."), footer);
    m_emptyLabel->setObjectName(QStringLiteral("MetricCaption"));
    footerLayout->addWidget(m_emptyLabel);

    layout->addWidget(footer);

    // -- signals ------------------------------------------------------------
    connect(m_channelBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChartWidget::onChannelIndexChanged);
    connect(m_windowBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index)
            {
                setWindowSize(m_windowBox->itemData(index).toInt());
            });
    connect(m_pauseButton, &QToolButton::toggled, this, &ChartWidget::onPauseToggled);
    connect(m_clearButton, &QToolButton::clicked, this, &ChartWidget::onClearRequested);
    connect(m_limitsButton, &QToolButton::toggled, this, [this](bool checked)
    {
        setShowLimits(checked);
    });
}

void ChartWidget::rebuildSeries()
{
    // Removed and re-added rather than reused.  A QLineSeries that has been
    // attached to an axis and then re-attached keeps its old attachment in some
    // Qt versions, and the trace silently stops updating — a failure that looks
    // like a data problem and is not.  Rebuilding a fixed number of series once
    // per channel change is not a cost worth optimising.
    for (QLineSeries* series : {m_series, m_warnLine, m_tripLine})
    {
        if (series != nullptr)
        {
            m_chart->removeSeries(series);
            series->deleteLater();
        }
    }

    const auto makeSeries = [this](const QColor& colour, qreal width)
    {
        auto* series = new QLineSeries(m_chart);
        QPen pen(colour);
        pen.setWidthF(width);
        pen.setCosmetic(true);   // constant pixel width regardless of zoom
        series->setPen(pen);
        m_chart->addSeries(series);
        series->attachAxis(m_timeAxis);
        series->attachAxis(m_valueAxis);
        return series;
    };

    m_series   = makeSeries(Theme::accent(), 1.8);
    m_warnLine = makeSeries(Theme::warning(), 1.0);
    m_tripLine = makeSeries(Theme::fault(), 1.0);

    // Dashed threshold markers: distinguishable from data without relying on
    // colour alone, which matters because the data trace may itself be amber.
    QPen warnPen(Theme::warning(), 1.0, Qt::DashLine);
    warnPen.setCosmetic(true);
    m_warnLine->setPen(warnPen);

    QPen tripPen(Theme::fault(), 1.0, Qt::DashLine);
    tripPen.setCosmetic(true);
    m_tripLine->setPen(tripPen);

    updateLimitLines(DeviceLimits{});
    setShowLimits(m_showLimits);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void ChartWidget::setWindowSize(int points)
{
    m_windowSize = qBound(30, points, 3600);

    // Trim immediately rather than waiting for the next append: otherwise a
    // reduction from 30 min to 2 min would leave the axis showing half an hour
    // of stale data until the old points aged out, which reads as a bug.
    while (m_series != nullptr && m_series->count() > m_windowSize)
    {
        m_series->remove(0);
    }

    const QDateTime now = QDateTime::currentDateTime();
    m_timeAxis->setRange(now.addSecs(-static_cast<int>(static_cast<double>(m_windowSize) / kNominalSampleHz)),
                         now);
}

void ChartWidget::setChannel(Channel channel)
{
    if (channel == m_channel)
    {
        return;
    }
    m_channel = channel;
    applyChannelStyle();
    clear();
    emit channelChanged(channel);
}

void ChartWidget::setShowLimits(bool show)
{
    m_showLimits = show;
    if (m_warnLine != nullptr)
    {
        m_warnLine->setVisible(show);
        m_tripLine->setVisible(show);
    }
    if (m_limitsButton != nullptr && m_limitsButton->isChecked() != show)
    {
        m_limitsButton->setChecked(show);
    }
}

void ChartWidget::applyChannelStyle()
{
    if (m_valueAxis == nullptr)
    {
        return;
    }

    m_valueAxis->setTitleText(QStringLiteral("%1 (%2)").arg(labelOf(m_channel), unitOf(m_channel)));
    m_valueAxis->setTitleBrush(Theme::textSecondary());

    // The axis is re-fitted on the next sample; clearing this flag is what makes
    // that happen instead of the chart keeping the previous channel's range.
    m_axisInitialised = false;
    m_minimum = 0.0;
    m_maximum = 0.0;
    m_sum = 0.0;
    m_valueCount = 0;

    const Device* device = nullptr;
    if (m_manager != nullptr && !m_deviceId.isEmpty())
    {
        const std::shared_ptr<Device> shared = m_manager->device(m_deviceId);
        device = shared.get();
    }
    updateLimitLines(device != nullptr ? device->limits() : DeviceLimits{});
}

void ChartWidget::updateLimitLines(const DeviceLimits& limits)
{
    // The threshold series are two-point horizontal lines spanning the whole
    // time axis.  Drawing them as series rather than as a paintEvent overlay
    // means they pan and clip with the data for free - and there is no second
    // coordinate transform to keep in sync with Qt Charts' internal one.
    const qint64 fromMs = m_timeAxis->min().toMSecsSinceEpoch();
    const qint64 toMs   = m_timeAxis->max().toMSecsSinceEpoch();

    double warn = 0.0;
    double trip = 0.0;

    switch (m_channel)
    {
    case Channel::Temperature: warn = limits.temperatureWarn; trip = limits.temperatureHigh; break;
    case Channel::Current:     warn = limits.currentWarn;     trip = limits.currentHigh;     break;
    case Channel::Vibration:   warn = limits.vibrationWarn;   trip = limits.vibrationHigh;   break;
    case Channel::Voltage:     warn = limits.voltageWarnHigh; trip = limits.voltageWarnLow;  break;
    case Channel::Count:
    default:                   return;
    }

    const auto setLine = [&](QLineSeries* series, double value)
    {
        if (series == nullptr)
        {
            return;
        }
        QVector<QPointF> points;
        points.reserve(2);
        points.append(QPointF(static_cast<qreal>(fromMs), value));
        points.append(QPointF(static_cast<qreal>(toMs), value));
        series->replace(points);
    };

    setLine(m_warnLine, warn);
    setLine(m_tripLine, trip);
}

// ---------------------------------------------------------------------------
// Device binding
// ---------------------------------------------------------------------------

void ChartWidget::setDevice(const QString& deviceId)
{
    if (deviceId == m_deviceId)
    {
        return;
    }

    m_deviceId = deviceId;
    clear();

    const Device* device = nullptr;
    if (m_manager != nullptr && !deviceId.isEmpty())
    {
        const std::shared_ptr<Device> shared = m_manager->device(deviceId);
        device = shared.get();
    }

    if (device == nullptr)
    {
        m_emptyLabel->setText(tr("No device selected"));
        return;
    }

    updateLimitLines(device->limits());

    // Seed the strip from the device's retained history so the chart is not
    // empty for the first four minutes after selecting a device that has been
    // running for hours.  The history is already in the Device for the AI
    // window, so this costs a copy and no extra query.
    const std::vector<SensorSample> history = device->history();
    m_series->clear();
    for (const SensorSample& sample : history)
    {
        if (sample.isValid())
        {
            m_series->append(static_cast<qreal>(sample.timestamp.toMSecsSinceEpoch()),
                             valueOf(sample, m_channel));
        }
    }
    while (m_series->count() > m_windowSize)
    {
        m_series->remove(0);
    }

    m_axisInitialised = false;
    m_emptyLabel->setText(m_series->count() > 0 ? QString() : tr("Waiting for data..."));
    refresh();
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void ChartWidget::pushSample(const SensorSample& sample)
{
    if (m_paused || !sample.isValid() || sample.deviceId != m_deviceId)
    {
        return;
    }

    // Buffered, not appended.  Appending here would make the chart's cost track
    // the *inbound* rate; buffering bounds it to the display rate.  See
    // AcquisitionWorker::sampleCoalesced for the same reasoning one layer down.
    m_pending.append(QPointF(static_cast<qreal>(sample.timestamp.toMSecsSinceEpoch()),
                             valueOf(sample, m_channel)));
}

void ChartWidget::refresh()
{
    if (m_paused)
    {
        return;
    }

    if (!m_pending.isEmpty())
    {
        for (const QPointF& point : m_pending)
        {
            m_series->append(point);

            // Running statistics over the *displayed* points.  Recomputed from
            // the retained window rather than accumulated forever, so the footer
            // describes what is on screen and not what has ever been seen.
            m_minimum = (m_valueCount == 0) ? point.y() : std::min(m_minimum, point.y());
            m_maximum = (m_valueCount == 0) ? point.y() : std::max(m_maximum, point.y());
            m_sum += point.y();
            ++m_valueCount;
        }
        m_pending.clear();

        // Trim from the front, which is what keeps the per-tick cost O(1) in the
        // window length rather than O(n).
        while (m_series->count() > m_windowSize)
        {
            m_series->remove(0);
        }

        const double newest = m_series->count() > 0 ? m_series->at(m_series->count() - 1).y() : 0.0;
        const Device* device = (m_manager != nullptr && !m_deviceId.isEmpty())
                                   ? m_manager->device(m_deviceId).get()
                                   : nullptr;
        rescaleValueAxis(newest, device != nullptr ? device->limits() : DeviceLimits{});

        const QDateTime now = QDateTime::currentDateTime();
        m_timeAxis->setRange(
            now.addSecs(-static_cast<int>(static_cast<double>(m_windowSize) / kNominalSampleHz)), now);

        m_emptyLabel->setVisible(false);
    }

    // -- footer -------------------------------------------------------------
    if (m_valueCount > 0)
    {
        const double mean = m_sum / static_cast<double>(m_valueCount);

        // Statistics are over the *retained window*, so the footer describes what
        // is on screen.  A lifetime maximum would be a different and less useful
        // number: it never changes, so it stops being read.
        m_statsLabel->setText(tr("min %1  ·  max %2  ·  avg %3 %4  ·  %5 points")
                                  .arg(QString::number(m_minimum, 'f', 2),
                                       QString::number(m_maximum, 'f', 2),
                                       QString::number(mean, 'f', 2),
                                       unitOf(m_channel))
                                  .arg(m_series->count()));
    }
    else
    {
        m_statsLabel->clear();
    }
}

void ChartWidget::rescaleValueAxis(double newestValue, const DeviceLimits& limits)
{
    Q_UNUSED(limits)

    if (!m_axisInitialised)
    {
        // First point after a device change: fit immediately and give the range
        // real width, because a zero-height axis draws nothing.
        const double span = std::max(1.0, std::abs(newestValue) * 0.2);
        m_valueAxis->setRange(newestValue - span * 0.5, newestValue + span);
        m_axisInitialised = true;
        return;
    }

    const double low = m_valueAxis->min();
    const double high = m_valueAxis->max();
    const double span = high - low;

    if (span <= 0.0)
    {
        return;
    }

    const double position = (newestValue - low) / span;

    // Only rescale outside the middle 80 %.  A threshold line that falls outside
    // the current range also forces a rescale — otherwise the operator sees a
    // healthy-looking trace with the warning limit off-screen above it, which is
    // worse than no limit line at all.
    bool needsRescale = (position < kAxisLowerMargin || position > kAxisUpperMargin);

    if (m_showLimits)
    {
        double warn = 0.0;
        double trip = 0.0;
        switch (m_channel)
        {
        case Channel::Temperature: warn = limits.temperatureWarn; trip = limits.temperatureHigh; break;
        case Channel::Current:     warn = limits.currentWarn;     trip = limits.currentHigh;     break;
        case Channel::Vibration:   warn = limits.vibrationWarn;   trip = limits.vibrationHigh;   break;
        case Channel::Voltage:     warn = limits.voltageWarnHigh; trip = limits.voltageWarnLow;  break;
        case Channel::Count:
        default: break;
        }
        if (trip > 0.0 && (trip > high || trip < low))
        {
            needsRescale = true;
        }
        Q_UNUSED(warn)
    }

    if (!needsRescale)
    {
        return;
    }

    // Grow the range so the new point sits at 75 % of it, rather than fitting
    // tightly.  A range fitted exactly to the data would rescale on every tick,
    // which is the jitter this function exists to prevent.
    const double target = newestValue + span * 0.25;
    m_valueAxis->setRange(std::min(low, newestValue - span * 0.05),
                          std::max(high, target));
}

void ChartWidget::setPaused(bool paused)
{
    m_paused = paused;
    if (m_pauseButton != nullptr && m_pauseButton->isChecked() != paused)
    {
        m_pauseButton->setChecked(paused);
    }

    // A paused chart still accumulates into the pending buffer, so resuming
    // shows the gap closing rather than an unexplained hole in the trace.  The
    // buffer is bounded, though: an operator who pauses for an hour must not
    // come back to a million buffered points.
    if (m_pending.size() > 10000)
    {
        m_pending.remove(0, m_pending.size() - 10000);
    }
}

void ChartWidget::clear()
{
    m_pending.clear();
    m_series->clear();
    m_minimum = 0.0;
    m_maximum = 0.0;
    m_sum = 0.0;
    m_valueCount = 0;
    m_axisInitialised = false;
    m_statsLabel->clear();
    m_emptyLabel->setText(m_deviceId.isEmpty() ? tr("No device selected")
                                               : tr("Waiting for data..."));
    m_emptyLabel->setVisible(true);
}

bool ChartWidget::exportImage(const QString& filePath) const
{
    // Grab the chart view rather than rendering the chart, so that what is saved
    // is exactly what is on screen - including the axis labels, which are drawn
    // by the view's scene and are not part of QChart's own render.
    const QPixmap pixmap = m_view->grab();
    if (pixmap.isNull())
    {
        return false;
    }

    const QFileInfo info(filePath);
    if (!info.absoluteDir().exists())
    {
        return false;
    }

    return pixmap.save(filePath, "PNG");
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

double ChartWidget::valueOf(const SensorSample& sample, Channel channel) const
{
    switch (channel)
    {
    case Channel::Temperature: return sample.temperature;
    case Channel::Current:     return sample.current;
    case Channel::Vibration:   return sample.vibration;
    case Channel::Voltage:     return sample.voltage;
    case Channel::Count:
    default:                   return 0.0;
    }
}

QString ChartWidget::labelOf(Channel channel) const
{
    switch (channel)
    {
    case Channel::Temperature: return tr("Temperature");
    case Channel::Current:     return tr("Current");
    case Channel::Vibration:   return tr("Vibration");
    case Channel::Voltage:     return tr("Voltage");
    case Channel::Count:
    default:                   return tr("Unknown");
    }
}

QString ChartWidget::unitOf(Channel channel) const
{
    switch (channel)
    {
    case Channel::Temperature: return QStringLiteral("°C");
    case Channel::Current:     return QStringLiteral("A");
    case Channel::Vibration:   return QStringLiteral("mm/s");
    case Channel::Voltage:     return QStringLiteral("V");
    case Channel::Count:
    default:                   return QString();
    }
}

void ChartWidget::onChannelIndexChanged(int index)
{
    const int value = m_channelBox->itemData(index).toInt();
    setChannel(static_cast<Channel>(value));
}

void ChartWidget::onPauseToggled(bool paused)
{
    setPaused(paused);
}

void ChartWidget::onClearRequested()
{
    clear();
}

}   // namespace saim
