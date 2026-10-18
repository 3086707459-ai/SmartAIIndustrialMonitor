#pragma once

// ---------------------------------------------------------------------------
// StateIndicator.h - the coloured status lamp, and its text form.
//
// A small widget with a disproportionate amount of thought behind it, because it
// is the single most-read element on the screen.  An operator scanning a wall of
// assets reads the lamps first and the numbers second, so the lamp has to be
// unambiguous at a glance and at a distance.
//
// Three decisions worth naming:
//
//   * The lamp is a *filled circle with a ring*, not a plain dot.  On a dark
//     background a bare dot at 10 px reads as a smudge; the ring gives it an edge
//     so its colour is legible without relying on brightness alone.
//
//   * Colour is never the only channel.  The optional text label spells the state
//     out, and the tooltip always does.  Around 8 % of men have a red/green
//     deficiency; an interface whose only fault indicator is "the dot is red" is
//     unreadable for them, which on a plant floor is a safety issue rather than an
//     accessibility nicety.
//
//   * There is no blink.  A blinking indicator is a strong attentional draw, and
//     a fault that blinks for six hours is a fault everybody has learned to
//     ignore.  Severity is carried by colour and by position in the list, both of
//     which are stable and readable at any moment.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QWidget>

class QLabel;

namespace saim {

class StateIndicator final : public QWidget
{
    Q_OBJECT
public:
    explicit StateIndicator(QWidget* parent = nullptr);
    ~StateIndicator() override;

    /// `diameter` is the lamp only; the widget's sizeHint adds the text label.
    void setDiameter(int diameter);

    void setState(DeviceState state);
    [[nodiscard]] DeviceState state() const noexcept { return m_state; }

    /// Shows the state name next to the lamp.  Off by default: in a dense list
    /// the column header says what the lamps mean, and repeating it sixty times
    /// is noise.  Switched on for the detail panel and the report preview.
    void setShowText(bool show);
    [[nodiscard]] bool showText() const noexcept { return m_showText; }

    /// Text shown instead of the state name.  Lets a caller label a lamp with the
    /// device name in the list, or with a metric in the detail panel.
    void setLabelText(const QString& text);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void refresh();

    DeviceState m_state = DeviceState::Offline;
    int  m_diameter = 12;
    bool m_showText = false;
    QLabel* m_label = nullptr;
};

}   // namespace saim
