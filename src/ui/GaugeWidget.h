#pragma once

// ---------------------------------------------------------------------------
// GaugeWidget.h - a 270-degree dial for one analogue quantity.
//
// Why a dial at all, when the dashboard is full of trend charts
// ------------------------------------------------------------
// A trend answers "where has this been".  A dial answers "where is it now,
// relative to its envelope" - and it answers it pre-attentively, without the
// operator reading a number or a time axis.  Plant HMI has used dials for sixty
// years for the same reason a car has a speedometer instead of a graph: the
// question "am I near the limit?" should not require arithmetic.
//
// The three dial decisions that matter
// ------------------------------------
//   * The *warning band is drawn on the face*, not implied by the needle colour.
//     An operator who has just walked up to the screen must be able to see where
//     the limits are before they see where the needle is.
//
//   * The needle carries the state colour and the band carries the limit colour,
//     so at a glance the dial reads "the needle is in the amber band" - the two
//     pieces of information the colour encodes independently reinforce each other
//     rather than duplicating the same signal.
//
//   * The numeric value is inside the dial, monospaced.  A dial alone gives
//     resolution of about 5 % of full scale; the number gives the exact value,
//     and putting it inside the ring means the eye does not have to travel.
// ---------------------------------------------------------------------------

#include <QWidget>

namespace saim {

/// Which end of the scale is the dangerous one.  Most process quantities are
/// upper-bounded (temperature, current, vibration); a few are lower-bounded
/// (insulation resistance, oil pressure); a handful have both (voltage).
enum class GaugeSense
{
    HigherIsWorse,
    LowerIsWorse,
    BothEnds         ///< warn band on both sides of a healthy middle
};

class GaugeWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit GaugeWidget(QWidget* parent = nullptr);
    ~GaugeWidget() override;

    // -- configuration ------------------------------------------------------

    void setCaption(const QString& caption);
    void setUnit(const QString& unit);
    void setDecimals(int decimals);
    void setSense(GaugeSense sense);

    /// The full-scale range of the dial face.
    void setRange(double minimum, double maximum);

    /// Envelope.  For HigherIsWorse/LowerIsWorse the two values are the warn and
    /// trip points in severity order; for BothEnds they are the low and high
    /// limits of the *healthy* band, and `setOuterLimits` supplies the trips.
    void setLimits(double warn, double trip);
    void setOuterLimits(double lowTrip, double highTrip);

    // -- live value ---------------------------------------------------------

    /// Sets the value and repaints.  Non-finite input is ignored.
    void setValue(double value);

    /// Clears the reading: the needle parks at the minimum and the face greys
    /// out.  A dial with no data must not sit at a plausible-looking mid-scale
    /// position, which would read as a real measurement.
    void clearValue();

    [[nodiscard]] double value() const noexcept { return m_value; }
    [[nodiscard]] bool hasValue() const noexcept { return m_hasValue; }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

    /// Height needed for a given width, so a layout can reserve the right space
    /// without the caller knowing the aspect ratio.
    [[nodiscard]] static int preferredHeightForWidth(int width);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override { return heightForWidthImpl(width); }

private:
    [[nodiscard]] int heightForWidthImpl(int width) const;
    /// Fraction of the scale (0..1) that a value occupies.
    [[nodiscard]] double fractionFor(double value) const noexcept;
    [[nodiscard]] QColor colourFor(double value) const;

    void drawFace(QPainter& painter, const QRectF& box) const;
    void drawBands(QPainter& painter, const QRectF& box) const;
    void drawNeedle(QPainter& painter, const QRectF& box) const;
    void drawReadout(QPainter& painter, const QRectF& box) const;

    QString m_caption = QStringLiteral("Value");
    QString m_unit;
    int     m_decimals = 1;

    GaugeSense m_sense = GaugeSense::HigherIsWorse;
    double m_minimum = 0.0;
    double m_maximum = 100.0;

    double m_warn = 70.0;
    double m_trip = 90.0;
    double m_lowTrip  = 0.0;
    double m_highTrip = 100.0;

    double m_value = 0.0;
    bool   m_hasValue = false;
};

}   // namespace saim
