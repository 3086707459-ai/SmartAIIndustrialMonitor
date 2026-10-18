#pragma once

// ---------------------------------------------------------------------------
// ChartWidget.h - the live trend strip.
//
// The decision that shapes this file: append, do not rebuild
// ---------------------------------------------------------
// The obvious implementation of a live chart is "clear all series, push the
// last N points, redraw" on every tick.  At 120 points x 4 series x 10 Hz that
// is 4800 point objects constructed and destroyed per second, and — more
// importantly — it throws away Qt Charts' internal geometry cache every frame,
// so the cost is O(n) per repaint rather than O(1).
//
// Instead each series is appended to and, once at capacity, has its oldest point
// removed.  Qt Charts handles `append` and `remove(0)` efficiently, the axis
// range is moved rather than recomputed, and the cost per tick becomes
// independent of the window length.  That is what lets the window be
// configurable from 60 to 2000 points without a performance cliff — which
// matters because a maintenance engineer wants the long view and an operator
// watching a ramp wants the short one.
//
// Why the y-axis rescales only on a margin breach
// ----------------------------------------------
// Auto-fitting the axis to the data on every frame makes the trace appear to
// stand still while the axis labels crawl — the classic "the line is flat but
// the numbers are changing" chart, which hides exactly the trend it is supposed
// to show.  The axis here holds a stable range and only grows or shrinks when
// the data leaves the middle 80 % of it, so a real ramp visibly ramps.
//
// Threshold lines are drawn as flat series rather than as a custom overlay, so
// they pan with the data for free and need no extra paint code.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QVector>
#include <QWidget>

#include <array>

class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;

QT_BEGIN_NAMESPACE
class QChart;
class QChartView;
class QDateTimeAxis;
class QLineSeries;
class QValueAxis;
QT_END_NAMESPACE

namespace saim {

class DeviceManager;

class ChartWidget final : public QWidget
{
    Q_OBJECT
public:
    /// The analogue channels the strip can plot.  Index order is the order of
    /// the combo box, and matches SensorSample's field order.
    enum class Channel
    {
        Temperature = 0,
        Current,
        Vibration,
        Voltage,
        Count
    };

    explicit ChartWidget(DeviceManager* manager, QWidget* parent = nullptr);
    ~ChartWidget() override;

    // -- configuration ------------------------------------------------------

    /// Retention window in samples.  Larger costs one QPointF per sample per
    /// channel — 2000 points is 32 kB, so the limit is the eye, not the memory.
    void setWindowSize(int points);
    [[nodiscard]] int windowSize() const noexcept { return m_windowSize; }

    void setChannel(Channel channel);
    [[nodiscard]] Channel channel() const noexcept { return m_channel; }

    /// Shows or hides the warn/high limit lines for the active channel.
    void setShowLimits(bool show);
    [[nodiscard]] bool showLimits() const noexcept { return m_showLimits; }

public slots:
    /// Charts this device.  An empty id clears the strip.
    void setDevice(const QString& deviceId);

    /// Appends the device's newest sample.  Safe to call faster than the refresh
    /// rate — points are buffered and published on the UI tick.
    void pushSample(const saim::SensorSample& sample);

    /// Publishes buffered points and advances the time axis.  Driven by the
    /// controller's coalescing timer, not by the sample stream.
    void refresh();

    /// Freezes the visible window.  Data continues to accumulate behind it, so
    /// resuming does not leave a gap.
    void setPaused(bool paused);
    [[nodiscard]] bool isPaused() const noexcept { return m_paused; }

    /// Discards all plotted points but keeps the configuration.
    void clear();

    /// Writes the current view to a PNG.  Returns false if the image could not
    /// be written; the caller surfaces that rather than assuming success.
    [[nodiscard]] bool exportImage(const QString& filePath) const;

signals:
    /// The active channel changed; the controller uses this to keep the detail
    /// gauges in step with the strip.
    void channelChanged(saim::ChartWidget::Channel channel);

private slots:
    void onChannelIndexChanged(int index);
    void onPauseToggled(bool paused);
    void onClearRequested();

private:
    void buildUi();
    void rebuildSeries();
    void updateLimitLines(const DeviceLimits& limits);
    void rescaleValueAxis(double newestValue, const DeviceLimits& limits);
    void applyChannelStyle();

    [[nodiscard]] double valueOf(const SensorSample& sample, Channel channel) const;
    [[nodiscard]] QString labelOf(Channel channel) const;
    [[nodiscard]] QString unitOf(Channel channel) const;

    DeviceManager* m_manager = nullptr;
    QString        m_deviceId;

    QChart*         m_chart = nullptr;
    QChartView*     m_view  = nullptr;
    QDateTimeAxis*  m_timeAxis = nullptr;
    QValueAxis*     m_valueAxis = nullptr;

    QLineSeries* m_series = nullptr;        ///< the data trace
    QLineSeries* m_warnLine = nullptr;      ///< warn threshold marker
    QLineSeries* m_tripLine = nullptr;      ///< trip threshold marker

    QComboBox*   m_channelBox = nullptr;
    QToolButton* m_pauseButton = nullptr;
    QToolButton* m_clearButton = nullptr;
    QToolButton* m_limitsButton = nullptr;
    QComboBox*   m_windowBox = nullptr;
    QLabel*      m_statsLabel = nullptr;
    QLabel*      m_emptyLabel = nullptr;

    /// Buffered points awaiting the next refresh.  A separate buffer from the
    /// plotted points because the inbound rate and the display rate are
    /// deliberately decoupled — see AcquisitionWorker's comment.
    QVector<QPointF> m_pending;

    Channel m_channel = Channel::Temperature;
    int     m_windowSize = 240;
    bool    m_showLimits = true;
    bool    m_paused = false;

    /// Running min/max/mean over the visible window, for the strip's footer.
    double m_minimum = 0.0;
    double m_maximum = 0.0;
    double m_sum = 0.0;
    qint64 m_valueCount = 0;

    /// True once the value axis has been fitted to real data, so the first
    /// sample does not fight the initial range.
    bool m_axisInitialised = false;
};

}   // namespace saim

Q_DECLARE_METATYPE(saim::ChartWidget::Channel)
