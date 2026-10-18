#include "ui/GaugeWidget.h"

#include "ui/Theme.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace saim {

namespace {

/// Dial geometry.  The sweep starts at 225 degrees (lower-left) and runs 270
/// degrees clockwise to -45 degrees (lower-right), leaving a 90-degree gap at
/// the bottom.  That gap is not decoration: it is where the caption goes, and it
/// is what makes a row of dials read as instruments rather than as pie charts.
constexpr double kStartAngleDeg = 225.0;
constexpr double kSweepDeg      = 270.0;

/// Thickness of the coloured band ring, as a fraction of the dial radius.
constexpr double kBandThicknessRatio = 0.13;
/// Thickness of the tick ring outside the band.
constexpr double kTickLengthRatio = 0.07;

/// Converting degrees into Qt's 1/16th-degree, counter-clockwise arc units.
constexpr int kQtAngleScale = 16;

/// Pi, defined locally rather than taken from <cmath>'s M_PI.  M_PI is not part
/// of the C++ standard - it is a POSIX/implementation extension that MSVC only
/// exposes with _USE_MATH_DEFINES, so relying on it makes the file build on one
/// toolchain and fail on another.
constexpr double kPi = 3.14159265358979323846;

/// A dial is roughly square plus the caption line beneath it.
constexpr double kAspectRatio = 1.0;

int toQtAngle(double degrees)
{
    return static_cast<int>(std::lround(degrees * kQtAngleScale));
}

}   // namespace

GaugeWidget::GaugeWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("GaugeWidget"));
    setMinimumSize(120, 130);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

GaugeWidget::~GaugeWidget() = default;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void GaugeWidget::setCaption(const QString& caption)
{
    m_caption = caption;
    update();
}

void GaugeWidget::setUnit(const QString& unit)
{
    m_unit = unit;
    update();
}

void GaugeWidget::setDecimals(int decimals)
{
    m_decimals = qBound(0, decimals, 4);
    update();
}

void GaugeWidget::setSense(GaugeSense sense)
{
    m_sense = sense;
    update();
}

void GaugeWidget::setRange(double minimum, double maximum)
{
    // A zero-width scale would make every division below a division by zero.
    // Widening rather than rejecting keeps a mis-configured device visible on
    // screen instead of crashing the paint path.
    if (maximum <= minimum)
    {
        maximum = minimum + 1.0;
    }
    m_minimum = minimum;
    m_maximum = maximum;
    update();
}

void GaugeWidget::setLimits(double warn, double trip)
{
    m_warn = warn;
    m_trip = trip;
    update();
}

void GaugeWidget::setOuterLimits(double lowTrip, double highTrip)
{
    m_lowTrip  = lowTrip;
    m_highTrip = highTrip;
    update();
}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

void GaugeWidget::setValue(double value)
{
    // Dropped rather than drawn: painting "nan" or a needle at an arbitrary
    // angle would present a broken reading as a real one.
    if (!std::isfinite(value))
    {
        return;
    }

    m_value = value;
    m_hasValue = true;
    update();
}

void GaugeWidget::clearValue()
{
    m_hasValue = false;
    m_value = m_minimum;
    update();
}

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------

QSize GaugeWidget::sizeHint() const
{
    return QSize(180, 180 + 18);
}

QSize GaugeWidget::minimumSizeHint() const
{
    return QSize(110, 120);
}

int GaugeWidget::preferredHeightForWidth(int width)
{
    return static_cast<int>(std::lround(static_cast<double>(width) / kAspectRatio)) + 18;
}

int GaugeWidget::heightForWidthImpl(int width) const
{
    return preferredHeightForWidth(width);
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

double GaugeWidget::fractionFor(double value) const noexcept
{
    const double span = m_maximum - m_minimum;
    if (span <= 0.0)
    {
        return 0.0;
    }
    // Clamped, not wrapped: a value past full scale must pin the needle at the
    // end stop, the way a real gauge does.  Letting it run past would draw the
    // needle through the caption.
    return std::clamp((value - m_minimum) / span, 0.0, 1.0);
}

QColor GaugeWidget::colourFor(double value) const
{
    switch (m_sense)
    {
    case GaugeSense::LowerIsWorse:
        // warn and trip are read as floors, with trip the lower of the two.
        if (value <= m_trip)
        {
            return Theme::fault();
        }
        if (value <= m_warn)
        {
            return Theme::warning();
        }
        return Theme::normal();

    case GaugeSense::BothEnds:
        if (value <= m_lowTrip || value >= m_highTrip)
        {
            return Theme::fault();
        }
        if (value <= m_warn || value >= m_trip)
        {
            return Theme::warning();
        }
        return Theme::normal();

    case GaugeSense::HigherIsWorse:
    default:
        if (value >= m_trip)
        {
            return Theme::fault();
        }
        if (value >= m_warn)
        {
            return Theme::warning();
        }
        return Theme::normal();
    }
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void GaugeWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // The dial is a circle inscribed in the available square; the caption takes
    // the strip underneath so that two dials side by side align their faces even
    // when only one has a caption.
    const int captionBand = 18;
    const int side = std::min(width(), std::max(0, height() - captionBand));
    if (side < 24)
    {
        return;   // too small to draw anything legible; leave it blank
    }

    const QRectF box(static_cast<double>(width() - side) / 2.0, 0.0,
                     static_cast<double>(side), static_cast<double>(side));

    drawFace(painter, box);
    drawBands(painter, box);
    drawNeedle(painter, box);
    drawReadout(painter, box);

    // -- caption ------------------------------------------------------------
    QFont captionFont = font();
    captionFont.setPointSizeF(std::max(7.5, font().pointSizeF() - 1.0));
    captionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    painter.setFont(captionFont);
    painter.setPen(Theme::textSecondary());

    painter.drawText(QRectF(0.0, static_cast<double>(side), static_cast<double>(width()),
                            static_cast<double>(captionBand)),
                     Qt::AlignHCenter | Qt::AlignVCenter, m_caption);
}

void GaugeWidget::drawFace(QPainter& painter, const QRectF& box) const
{
    const double radius = box.width() / 2.0;
    const double bandThickness = radius * kBandThicknessRatio;
    const QRectF arcRect = box.adjusted(bandThickness / 2.0 + 1.0, bandThickness / 2.0 + 1.0,
                                        -bandThickness / 2.0 - 1.0, -bandThickness / 2.0 - 1.0);

    // -- unlit track --------------------------------------------------------
    QPen trackPen(Theme::surfaceRaised(), bandThickness, Qt::SolidLine, Qt::FlatCap);
    painter.setPen(trackPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(arcRect,
                    toQtAngle(kStartAngleDeg),
                    toQtAngle(-kSweepDeg));

    // -- ticks --------------------------------------------------------------
    // Eleven ticks, i.e. a mark every 10 % of scale.  Major ticks at the ends and
    // the middle are drawn longer; the middle one is the reference an operator
    // uses to judge "half scale" without reading the numbers.
    const int tickCount = 11;
    const double tickLength = radius * kTickLengthRatio;

    for (int index = 0; index < tickCount; ++index)
    {
        const double fraction = static_cast<double>(index) / static_cast<double>(tickCount - 1);
        const double degrees = kStartAngleDeg - fraction * kSweepDeg;
        const double radians = degrees * kPi / 180.0;

        const bool major = (index == 0 || index == tickCount - 1 || index == (tickCount - 1) / 2);
        const double inner = radius - bandThickness - 2.0;
        const double outer = inner + (major ? tickLength : tickLength * 0.55);

        const QPointF centre = box.center();
        const QPointF from(centre.x() + std::cos(radians) * inner,
                           centre.y() - std::sin(radians) * inner);
        const QPointF to(centre.x() + std::cos(radians) * outer,
                         centre.y() - std::sin(radians) * outer);

        painter.setPen(QPen(major ? Theme::textSecondary() : Theme::textMuted(),
                            major ? 1.4 : 1.0));
        painter.drawLine(from, to);
    }
}

void GaugeWidget::drawBands(QPainter& painter, const QRectF& box) const
{
    const double radius = box.width() / 2.0;
    const double bandThickness = radius * kBandThicknessRatio;
    const QRectF arcRect = box.adjusted(bandThickness / 2.0 + 1.0, bandThickness / 2.0 + 1.0,
                                        -bandThickness / 2.0 - 1.0, -bandThickness / 2.0 - 1.0);

    painter.setBrush(Qt::NoBrush);

    // Draws one band from `fromValue` to `toValue` in `colour`.  Each band is a
    // separate arc so the three-colour convention is visible on the face itself,
    // which is what lets an operator read the dial before reading the needle.
    const auto band = [&](double fromValue, double toValue, const QColor& colour)
    {
        const double fromFraction = fractionFor(fromValue);
        const double toFraction   = fractionFor(toValue);
        if (toFraction - fromFraction <= 1e-6)
        {
            return;   // zero-width band: a degenerate range, nothing to draw
        }

        const double startDeg = kStartAngleDeg - fromFraction * kSweepDeg;
        const double spanDeg  = -(toFraction - fromFraction) * kSweepDeg;

        QColor faded = colour;
        faded.setAlpha(210);
        painter.setPen(QPen(faded, bandThickness, Qt::SolidLine, Qt::FlatCap));
        painter.drawArc(arcRect, toQtAngle(startDeg), toQtAngle(spanDeg));
    };

    switch (m_sense)
    {
    case GaugeSense::LowerIsWorse:
        band(m_minimum, m_trip, Theme::fault());
        band(m_trip, m_warn, Theme::warning());
        band(m_warn, m_maximum, Theme::normal());
        break;

    case GaugeSense::BothEnds:
        band(m_minimum, m_lowTrip, Theme::fault());
        band(m_lowTrip, m_warn, Theme::warning());
        band(m_warn, m_trip, Theme::normal());
        band(m_trip, m_highTrip, Theme::warning());
        band(m_highTrip, m_maximum, Theme::fault());
        break;

    case GaugeSense::HigherIsWorse:
    default:
        band(m_minimum, m_warn, Theme::normal());
        band(m_warn, m_trip, Theme::warning());
        band(m_trip, m_maximum, Theme::fault());
        break;
    }
}

void GaugeWidget::drawNeedle(QPainter& painter, const QRectF& box) const
{
    if (!m_hasValue)
    {
        return;
    }

    const double radius = box.width() / 2.0;
    const double fraction = fractionFor(m_value);
    const double degrees = kStartAngleDeg - fraction * kSweepDeg;
    const double radians = degrees * kPi / 180.0;

    const QPointF centre = box.center();
    const QColor colour = colourFor(m_value);

    // A tapered needle drawn as a filled triangle rather than a thick line: the
    // taper is what makes the tip's exact position readable, which is the whole
    // point of a needle.
    const double tipLength = radius - radius * kBandThicknessRatio - radius * kTickLengthRatio - 4.0;
    const double tailLength = radius * 0.12;
    const double halfWidth = radius * 0.055;

    const QPointF tip(centre.x() + std::cos(radians) * tipLength,
                      centre.y() - std::sin(radians) * tipLength);
    const QPointF tail(centre.x() - std::cos(radians) * tailLength,
                       centre.y() + std::sin(radians) * tailLength);

    // Perpendicular unit vector, for the two base corners.
    const double perpX = -std::sin(radians);
    const double perpY = -std::cos(radians);

    QPainterPath needle;
    needle.moveTo(tip);
    needle.lineTo(centre.x() + perpX * halfWidth, centre.y() + perpY * halfWidth);
    needle.lineTo(tail);
    needle.lineTo(centre.x() - perpX * halfWidth, centre.y() - perpY * halfWidth);
    needle.closeSubpath();

    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);
    painter.drawPath(needle);

    // Hub: painted in the background colour with a ring in the needle's colour,
    // so the needle appears to pivot through it rather than to be stuck on top.
    const double hubRadius = radius * 0.075;
    painter.setBrush(Theme::surface());
    painter.setPen(QPen(colour, 1.4));
    painter.drawEllipse(centre, hubRadius, hubRadius);
}

void GaugeWidget::drawReadout(QPainter& painter, const QRectF& box) const
{
    const double radius = box.width() / 2.0;
    const QPointF centre = box.center();

    // Monospaced, like every other numeric readout in the application - see
    // MetricCard for the reasoning.
    QFont valueFont(Theme::numericFontFamily());
    valueFont.setPointSizeF(std::max(10.0, font().pointSizeF() + 3.0));
    valueFont.setBold(true);

    const QString text = m_hasValue ? QString::number(m_value, 'f', m_decimals)
                                    : QStringLiteral("--");
    const QColor colour = m_hasValue ? colourFor(m_value) : Theme::textMuted();

    painter.setFont(valueFont);
    painter.setPen(colour);

    const QFontMetrics valueMetrics(valueFont);
    const int valueHeight = valueMetrics.height();

    // Sits just below the hub.  Above it would collide with the needle's own
    // sweep on a dial whose zero is at the lower left.
    const QRectF valueRect(centre.x() - radius * 0.8,
                           centre.y() + radius * 0.12,
                           radius * 1.6,
                           static_cast<double>(valueHeight));

    painter.drawText(valueRect, Qt::AlignHCenter | Qt::AlignTop, text);

    if (!m_unit.isEmpty())
    {
        QFont unitFont = font();
        unitFont.setPointSizeF(std::max(7.5, font().pointSizeF() - 1.0));
        painter.setFont(unitFont);
        painter.setPen(Theme::textMuted());

        const QFontMetrics unitMetrics(unitFont);
        painter.drawText(QRectF(centre.x() - radius * 0.8,
                                valueRect.bottom() + 1.0,
                                radius * 1.6,
                                static_cast<double>(unitMetrics.height())),
                         Qt::AlignHCenter | Qt::AlignTop, m_unit);
    }
}

}   // namespace saim
