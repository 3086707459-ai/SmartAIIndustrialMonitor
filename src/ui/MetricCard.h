#pragma once

// ---------------------------------------------------------------------------
// MetricCard.h - one large numeric readout with a label, a unit and a trend.
//
// Why a custom widget rather than a QLabel in a QGroupBox
// -------------------------------------------------------
// The dashboard's numbers are the application's primary output, and they have
// three requirements a plain label does not meet:
//
//   * Stability.  A value that updates ten times a second must not make the
//     layout twitch.  The digits are drawn in a monospaced face and the widget
//     reserves width for the widest plausible string, so "9.9" and "10.0" occupy
//     the same space.
//
//   * Context.  A bare "74.3" tells an operator nothing; "74.3 C, warning limit
//     70" tells them whether to act.  The card therefore carries a limit and
//     colours itself against it - which is exactly the rule the alarm engine
//     uses, so the card and the alarm can never disagree.
//
//   * Trend.  The direction of travel is usually more actionable than the value:
//     a machine at 68 C and rising is a different situation from one that has sat
//     at 68 C for a week.  The card shows the change over its own history window.
//
// The card holds a small history ring of its own rather than asking the Device
// for one: the dashboard may show a value the Device does not retain (current,
// for instance, is not in the AI window) and reaching back into the model for a
// display concern would couple the two.
// ---------------------------------------------------------------------------

#include "common/RingBuffer.h"

#include <QWidget>

#include <memory>

class QLabel;
class QPropertyAnimation;

namespace saim {

class MetricCard final : public QWidget
{
    Q_OBJECT
    /// Animated property for the value.  Animating the *displayed* number rather
    /// than the layout means the transition costs a repaint of one label and
    /// cannot disturb anything around it.
    Q_PROPERTY(double animatedValue READ animatedValue WRITE setAnimatedValue)

public:
    /// How the value is coloured.
    enum class Mode
    {
        /// Colour by the warn/high limits.  The default, and the right choice for
        /// anything with a datasheet envelope.
        Threshold,
        /// Higher is better: colour by a low limit.  Used for health score and
        /// uptime, where a falling number is the bad news.
        HigherIsBetter,
        /// Always the neutral text colour.  For values with no meaningful
        /// envelope - sample rate, device count.
        Neutral
    };

    explicit MetricCard(QWidget* parent = nullptr);
    ~MetricCard() override;

    void setCaption(const QString& caption);
    void setUnit(const QString& unit);
    void setDecimals(int decimals);
    void setMode(Mode mode);

    /// Thresholds interpreted according to the mode.  Setting them is what makes
    /// the card colour itself; without them it stays neutral.
    void setThresholds(double warn, double high);

    /// The value, plus the history it is trended against.
    void setValue(double value);

    /// Replaces the caption's companion line - used for "limit 70.0 C".
    void setHint(const QString& hint);

    /// Clears the value and shows a placeholder.  A device with no data must not
    /// display 0.0, which reads as a real measurement of zero.
    void setPlaceholder(const QString& text);

    [[nodiscard]] double value() const noexcept { return m_value; }
    [[nodiscard]] bool hasValue() const noexcept { return m_hasValue; }

    // -- Q_PROPERTY plumbing for the animation ------------------------------
    [[nodiscard]] double animatedValue() const noexcept { return m_displayedValue; }
    void setAnimatedValue(double value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void refreshColour();
    void refreshText();
    [[nodiscard]] QColor colourForValue(double value) const;

    QString m_caption = QStringLiteral("Value");
    QString m_unit;
    QString m_hint;
    QString m_placeholder = QStringLiteral("--");

    Mode   m_mode = Mode::Neutral;
    double m_warn = 0.0;
    double m_high = 0.0;
    bool   m_hasThresholds = false;

    int    m_decimals = 1;
    double m_value = 0.0;
    double m_displayedValue = 0.0;
    bool   m_hasValue = false;

    /// Trend over the retained window.  Small - 60 points is a couple of minutes
    /// at the dashboard's refresh rate, which is the horizon an operator reacts to.
    RingBuffer<double> m_history{60};

    QColor m_valueColour;

    std::unique_ptr<QPropertyAnimation> m_animation;
    bool m_hovered = false;
};

}   // namespace saim
