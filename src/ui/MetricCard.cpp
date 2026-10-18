#include "ui/MetricCard.h"

#include "ui/Theme.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPropertyAnimation>

#include <algorithm>
#include <cmath>

namespace saim {

namespace {

/// Animation duration for a value change.  180 ms: long enough to read as a
/// transition, short enough that the number is never behind the data.  A longer
/// animation would make a fast alarm look like a lag.
constexpr int kAnimationMs = 180;

/// Sparkline height as a fraction of the card.
constexpr double kSparklineHeightRatio = 0.26;

}   // namespace

MetricCard::MetricCard(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MetricCard"));
    setMinimumSize(150, 88);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // A card is decoration plus text; the tooltip is where the exact value and
    // the trend live for anyone who wants them.
    setAttribute(Qt::WA_Hover, true);

    m_valueColour = Theme::textPrimary();
}

MetricCard::~MetricCard() = default;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void MetricCard::setCaption(const QString& caption)
{
    m_caption = caption;
    update();
}

void MetricCard::setUnit(const QString& unit)
{
    m_unit = unit;
    update();
}

void MetricCard::setDecimals(int decimals)
{
    m_decimals = qBound(0, decimals, 4);
    refreshText();
}

void MetricCard::setMode(Mode mode)
{
    m_mode = mode;
    refreshColour();
    update();
}

void MetricCard::setThresholds(double warn, double high)
{
    m_warn = warn;
    m_high = high;
    m_hasThresholds = true;
    refreshColour();
    update();
}

void MetricCard::setHint(const QString& hint)
{
    m_hint = hint;
    update();
}

void MetricCard::setPlaceholder(const QString& text)
{
    m_placeholder = text;
    m_hasValue = false;
    m_history.clear();
    update();
}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

void MetricCard::setValue(double value)
{
    // A non-finite value is a bug upstream, but painting "nan" onto an industrial
    // dashboard is a worse outcome than ignoring the frame - so it is dropped here
    // rather than propagated into the display.
    if (!std::isfinite(value))
    {
        return;
    }

    const bool firstValue = !m_hasValue;

    m_value = value;
    m_hasValue = true;
    m_history.push(value);

    if (firstValue)
    {
        // No animation on the first sample: sliding up from zero on the first
        // reading looks like the value started at zero, which is a lie about a
        // machine that was simply not being watched a moment ago.
        m_displayedValue = value;
        refreshColour();
        refreshText();
        update();
        return;
    }

    if (m_animation)
    {
        m_animation->stop();
    }
    m_animation = std::make_unique<QPropertyAnimation>(this, "animatedValue");
    m_animation->setDuration(kAnimationMs);
    m_animation->setStartValue(m_displayedValue);
    m_animation->setEndValue(value);
    // OutCubic: fast at the start, settling gently.  A linear ramp reads as
    // mechanical; this reads as the number catching up.
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    m_animation->start();

    refreshColour();
    update();
}

void MetricCard::setAnimatedValue(double value)
{
    m_displayedValue = value;
    refreshText();
    update();
}

// ---------------------------------------------------------------------------
// Colour and text
// ---------------------------------------------------------------------------

QColor MetricCard::colourForValue(double value) const
{
    if (m_mode == Mode::Neutral || !m_hasThresholds)
    {
        return Theme::textPrimary();
    }

    if (m_mode == Mode::HigherIsBetter)
    {
        // warn and high are reinterpreted as low and critical floors.  Named
        // through this comment rather than through a second set of setters,
        // because a card that can be told "low is bad" via setThresholds(60, 40)
        // would be a trap for the next reader.
        const double criticalFloor = m_high;   // e.g. 40
        const double warnFloor     = m_warn;   // e.g. 60

        if (value <= criticalFloor)
        {
            return Theme::fault();
        }
        if (value <= warnFloor)
        {
            return Theme::warning();
        }
        return Theme::normal();
    }

    // Threshold mode: warn and high are upper bounds.
    if (value >= m_high)
    {
        return Theme::fault();
    }
    if (value >= m_warn)
    {
        return Theme::warning();
    }
    return Theme::normal();
}

void MetricCard::refreshColour()
{
    m_valueColour = m_hasValue ? colourForValue(m_value) : Theme::textMuted();
}

void MetricCard::refreshText()
{
    const QString shown = m_hasValue ? QString::number(m_displayedValue, 'f', m_decimals)
                                     : m_placeholder;
    const QString unitPart = m_unit.isEmpty() ? QString() : QStringLiteral(" %1").arg(m_unit);

    setToolTip(QStringLiteral("%1: %2%3\n%4")
                   .arg(m_caption, shown, unitPart,
                        m_hasValue ? QStringLiteral("History: %1 sample(s)").arg(m_history.size())
                                   : QStringLiteral("No data received yet")));
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void MetricCard::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    // -- surface ------------------------------------------------------------
    // The hover state raises the surface and brightens the border rather than
    // changing the fill colour, so hovering can never be mistaken for a status
    // change - the one thing on this card that must stay unambiguous.
    painter.setPen(QPen(m_hovered ? Theme::accent() : Theme::border(), 1.0));
    painter.setBrush(m_hovered ? Theme::surfaceRaised() : Theme::surface());
    painter.drawRoundedRect(card, 4.0, 4.0);

    const QRectF content = card.adjusted(Theme::Metrics::kPanelPadding, 8.0,
                                         -Theme::Metrics::kPanelPadding, -8.0);

    // -- caption ------------------------------------------------------------
    QFont captionFont = font();
    captionFont.setPointSizeF(std::max(7.5, font().pointSizeF() - 1.0));
    captionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    painter.setFont(captionFont);
    painter.setPen(Theme::textSecondary());

    const QFontMetrics captionMetrics(captionFont);
    const int captionHeight = captionMetrics.height();
    painter.drawText(QRectF(content.left(), content.top(), content.width(), captionHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, m_caption);

    // -- value --------------------------------------------------------------
    // Monospaced digits: without them, a value crossing a digit boundary ("9.9"
    // to "10.0") shifts every glyph and the readout visibly jitters at 10 Hz.
    QFont valueFont(Theme::numericFontFamily());
    valueFont.setPointSizeF(std::max(13.0, font().pointSizeF() + 7.0));
    valueFont.setBold(true);

    const QString shown = m_hasValue ? QString::number(m_displayedValue, 'f', m_decimals)
                                     : m_placeholder;
    const QString unitPart = m_unit.isEmpty() ? QString() : QStringLiteral(" %1").arg(m_unit);

    painter.setFont(valueFont);
    painter.setPen(m_valueColour);

    const QFontMetrics valueMetrics(valueFont);
    const int valueHeight = valueMetrics.height();
    const double valueTop = content.top() + captionHeight + 2.0;

    painter.drawText(QRectF(content.left(), valueTop, content.width(), valueHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, shown);

    // -- unit ---------------------------------------------------------------
    // Drawn separately, in a smaller muted face, so the number itself stays the
    // largest thing on the card.  Measured from the value's actual width rather
    // than from a fixed offset, so a wide number does not overlap its unit.
    if (!unitPart.isEmpty())
    {
        QFont unitFont = font();
        unitFont.setPointSizeF(std::max(8.0, font().pointSizeF() - 0.5));
        painter.setFont(unitFont);
        painter.setPen(Theme::textMuted());

        const int valueWidth = valueMetrics.horizontalAdvance(shown);

        painter.drawText(QRectF(content.left() + valueWidth + 4.0,
                                valueTop, content.width() - valueWidth - 4.0,
                                static_cast<double>(valueHeight)),
                         Qt::AlignLeft | Qt::AlignBottom, unitPart.trimmed());
    }

    // -- hint ---------------------------------------------------------------
    if (!m_hint.isEmpty())
    {
        QFont hintFont = font();
        hintFont.setPointSizeF(std::max(7.0, font().pointSizeF() - 1.5));
        painter.setFont(hintFont);
        painter.setPen(Theme::textMuted());

        const QFontMetrics hintMetrics(hintFont);
        painter.drawText(QRectF(content.left(),
                                content.bottom() - hintMetrics.height(),
                                content.width(),
                                hintMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, m_hint);
    }

    // -- sparkline ----------------------------------------------------------
    //
    // Only drawn once there is a shape worth drawing.  Three points make a
    // triangle, not a trend, and a zigzag on the card would imply information
    // that is not there.
    if (m_history.size() >= 8)
    {
        const double sparkHeight = content.height() * kSparklineHeightRatio;
        const QRectF spark(content.left(),
                           content.bottom() - sparkHeight,
                           content.width(),
                           sparkHeight);

        double minimum = m_history.newest();
        double maximum = minimum;
        for (std::size_t index = 0; index < m_history.size(); ++index)
        {
            const double sample = m_history.at(index);
            minimum = std::min(minimum, sample);
            maximum = std::max(maximum, sample);
        }

        // A flat series has zero range; drawing it against a zero span would
        // divide by zero, and drawing it against a tiny span would exaggerate
        // sensor noise into a dramatic mountain range.  Flattening it to the
        // middle says "nothing is happening", which is the truth.
        const double span = maximum - minimum;
        const double flatValue = spark.center().y();

        QPainterPath path;
        const std::size_t count = m_history.size();
        const double step = spark.width() / static_cast<double>(count - 1);

        for (std::size_t index = 0; index < count; ++index)
        {
            const double sample = m_history.at(index);
            const double normalised = span > 1e-9 ? (sample - minimum) / span : 0.5;
            const double x = spark.left() + step * static_cast<double>(index);
            const double y = span > 1e-9
                                 ? spark.bottom() - normalised * spark.height()
                                 : flatValue;

            if (index == 0)
            {
                path.moveTo(x, y);
            }
            else
            {
                path.lineTo(x, y);
            }
        }

        QColor sparkColour = m_valueColour;
        sparkColour.setAlpha(m_hovered ? 200 : 120);
        painter.setPen(QPen(sparkColour, 1.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
}

void MetricCard::mousePressEvent(QMouseEvent* event)
{
    // Emitting a signal on click is the caller's job; the card's own contribution
    // is to not swallow the event, so a parent list or scroll area still works.
    QWidget::mousePressEvent(event);
}

void MetricCard::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void MetricCard::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}

}   // namespace saim
