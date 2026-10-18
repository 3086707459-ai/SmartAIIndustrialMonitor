#include "ui/StateIndicator.h"

#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QToolTip>

namespace saim {

StateIndicator::StateIndicator(QWidget* parent)
    : QWidget(parent)
{
    // A fixed-height widget with the width driven by the lamp plus the optional
    // label.  QSizePolicy::Fixed horizontally too: a status lamp that stretches
    // looks like a progress bar.
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_label = new QLabel(this);
    m_label->setObjectName(QStringLiteral("StateLabel"));
    m_label->hide();   // shown by setShowText()
    layout->addWidget(m_label);

    refresh();
}

StateIndicator::~StateIndicator() = default;

void StateIndicator::setDiameter(int diameter)
{
    const int clamped = qBound(6, diameter, 48);
    if (clamped == m_diameter)
    {
        return;
    }
    m_diameter = clamped;
    updateGeometry();
    update();
}

void StateIndicator::setState(DeviceState state)
{
    if (state == m_state)
    {
        return;
    }
    m_state = state;
    refresh();
    update();
}

void StateIndicator::setShowText(bool show)
{
    if (show == m_showText)
    {
        return;
    }
    m_showText = show;
    m_label->setVisible(show);
    updateGeometry();
}

void StateIndicator::setLabelText(const QString& text)
{
    m_label->setText(text);
    // The label only has width when it is visible, so showing it later still
    // produces the right size hint.
    if (!text.isEmpty() && !m_showText)
    {
        setShowText(true);
    }
}

QSize StateIndicator::sizeHint() const
{
    if (!m_showText || m_label == nullptr)
    {
        return QSize(m_diameter, m_diameter);
    }

    const QFontMetrics metrics(m_label->font());
    return QSize(m_diameter + 6 + metrics.horizontalAdvance(m_label->text()),
                 qMax(m_diameter, metrics.height()));
}

QSize StateIndicator::minimumSizeHint() const
{
    return QSize(m_diameter, m_diameter);
}

void StateIndicator::refresh()
{
    // One place maps state -> colour and state -> words, and both come from
    // common/Types.cpp.  A widget that spelled "Fault" for itself would be the
    // fourth place in the code base holding a copy of that string.
    const QColor colour = Theme::stateColor(static_cast<int>(m_state));
    const QString text  = toString(m_state);

    if (m_label != nullptr)
    {
        // The text is set by setLabelText(); what belongs here is the colour, so
        // that a caller who only ever sets the state still gets a correctly
        // coloured word.
        if (m_label->text().isEmpty())
        {
            m_label->setText(text);
        }

        QPalette palette = m_label->palette();
        palette.setColor(QPalette::WindowText, colour);
        m_label->setPalette(palette);
    }

    setToolTip(QStringLiteral("%1").arg(text));
}

void StateIndicator::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor colour = Theme::stateColor(static_cast<int>(m_state));

    // Centre the lamp vertically and left-align it horizontally, so a taller row
    // (a wrapped label, a larger font) does not shift the lamp off the column the
    // eye has learned to scan down.
    const QRectF lampRect(0.0,
                          (static_cast<double>(height()) - static_cast<double>(m_diameter)) / 2.0,
                          static_cast<double>(m_diameter),
                          static_cast<double>(m_diameter));

    // The ring: a hairline in a lighter shade of the fill, which is what makes
    // the lamp read as a lit indicator rather than a flat colour swatch.
    QColor ring = colour.lighter(160);
    ring.setAlpha(200);

    painter.setPen(QPen(ring, 1.0));
    painter.setBrush(colour);
    painter.drawEllipse(lampRect.adjusted(0.5, 0.5, -0.5, -0.5));

    // Offline gets an extra cue: a hollow centre.  It is the one state that means
    // "no information" rather than "a value", and distinguishing it without
    // relying on colour alone matters most here, because grey is the hardest
    // colour to tell from a dimmed green under plant lighting.
    if (m_state == DeviceState::Offline)
    {
        painter.setBrush(Theme::background());
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(lampRect.adjusted(3.0, 3.0, -3.0, -3.0));
    }
}

}   // namespace saim
