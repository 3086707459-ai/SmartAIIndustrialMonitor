#include "ui/DashboardWidget.h"

#include "device/Device.h"
#include "ui/ChartWidget.h"
#include "ui/MetricCard.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace saim {

namespace {

/// Tile geometry.  A tile is a fixed height so a row of them scans as one band,
/// and a minimum width so the name never truncates to nothing.
constexpr int kTileHeight = 76;
constexpr int kTileMinWidth = 168;
constexpr int kTileSpacing = Theme::Metrics::kPanelSpacing;

/// Tile width used to decide the column count.  Fixed rather than measured, so
/// the grid does not reshuffle itself while the window is being dragged.
constexpr int kTilePreferredWidth = 208;

}   // namespace

// ---------------------------------------------------------------------------
// DeviceTile
// ---------------------------------------------------------------------------

DeviceTile::DeviceTile(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(kTileHeight);
    setMinimumWidth(kTileMinWidth);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setToolTip(tr("Click to select this device"));
}

void DeviceTile::setDevice(const QString& deviceId, const QString& name, const QString& type)
{
    m_deviceId = deviceId;
    m_name = name;
    m_type = type;
    update();
}

void DeviceTile::updateState(DeviceState state, double temperature, double health, bool hasHealth)
{
    // Repaint only on a visible change.  At 6 devices x 10 Hz an unconditional
    // update() would repaint 60 tiles a second for values that mostly did not
    // move - measurable CPU for no visible difference.
    const bool changed = (state != m_state)
                         || !qFuzzyCompare(m_temperature + 1.0, temperature + 1.0)
                         || (hasHealth != m_hasHealth)
                         || (hasHealth && !qFuzzyCompare(m_health + 1.0, health + 1.0));

    m_state = state;
    m_temperature = temperature;
    m_health = health;
    m_hasHealth = hasHealth;

    if (changed)
    {
        update();
    }
}

void DeviceTile::setSelected(bool selected)
{
    if (selected == m_selected)
    {
        return;
    }
    m_selected = selected;
    update();
}

void DeviceTile::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QColor stateColour = Theme::stateColor(static_cast<int>(m_state));

    // -- surface ------------------------------------------------------------
    painter.setPen(QPen(m_selected ? Theme::accent() : Theme::border(), m_selected ? 1.6 : 1.0));
    painter.setBrush(m_hovered ? Theme::surfaceRaised() : Theme::surface());
    painter.drawRoundedRect(box, 4.0, 4.0);

    // -- state strip --------------------------------------------------------
    // Drawn on the left edge, same convention as the device list, so the two
    // views can be read with the same eye movement.
    QPainterPath strip;
    strip.addRoundedRect(QRectF(box.left(), box.top(), 4.0, box.height()), 2.0, 2.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(stateColour);
    painter.drawPath(strip);

    const QRectF content = box.adjusted(14.0, 8.0, -10.0, -8.0);

    // -- name ---------------------------------------------------------------
    QFont nameFont = font();
    nameFont.setBold(m_state >= DeviceState::Warning);
    painter.setFont(nameFont);
    painter.setPen(Theme::textPrimary());

    const QFontMetrics nameMetrics(nameFont);
    const QString shown = nameMetrics.elidedText(m_name, Qt::ElideRight,
                                                 static_cast<int>(content.width()));
    painter.drawText(QRectF(content.left(), content.top(), content.width(),
                            static_cast<double>(nameMetrics.height())),
                     Qt::AlignLeft | Qt::AlignVCenter, shown);

    // -- type ---------------------------------------------------------------
    QFont subFont = font();
    subFont.setPointSizeF(std::max(7.0, font().pointSizeF() - 1.5));
    painter.setFont(subFont);
    painter.setPen(Theme::textMuted());

    const QFontMetrics subMetrics(subFont);
    const double subTop = content.top() + nameMetrics.height();

    painter.drawText(QRectF(content.left(), subTop, content.width() * 0.6,
                            static_cast<double>(subMetrics.height())),
                     Qt::AlignLeft | Qt::AlignVCenter, m_type);

    // -- state word, right aligned ------------------------------------------
    // The word, not just the colour.  Colour alone is unreadable for a red/green
    // colour deficiency, which is roughly one in twelve men - see the note in
    // StateIndicator.h.
    painter.setPen(stateColour);
    painter.drawText(QRectF(content.left() + content.width() * 0.5, subTop,
                            content.width() * 0.5,
                            static_cast<double>(subMetrics.height())),
                     Qt::AlignRight | Qt::AlignVCenter, toString(m_state));

    // -- readouts -----------------------------------------------------------
    QFont valueFont(Theme::numericFontFamily());
    valueFont.setPointSizeF(std::max(9.5, font().pointSizeF() + 0.5));
    valueFont.setBold(true);

    const double valueTop = subTop + subMetrics.height() + 2.0;

    painter.setFont(valueFont);
    painter.setPen(Theme::textSecondary());

    const QFontMetrics valueMetrics(valueFont);

    // Temperature on the left, health on the right - the two numbers that decide
    // whether a maintenance engineer walks over.
    const QString temperatureText = QStringLiteral("%1 °C").arg(QString::number(m_temperature, 'f', 1));
    painter.drawText(QRectF(content.left(), valueTop, content.width() * 0.5,
                            static_cast<double>(valueMetrics.height())),
                     Qt::AlignLeft | Qt::AlignVCenter, temperatureText);

    if (m_hasHealth)
    {
        // Coloured against the same floors the detail panel uses, so the tile
        // and the panel agree.
        QColor healthColour = Theme::normal();
        if (m_health < 40.0)
        {
            healthColour = Theme::fault();
        }
        else if (m_health < 60.0)
        {
            healthColour = Theme::warning();
        }

        painter.setPen(healthColour);
        painter.drawText(QRectF(content.left() + content.width() * 0.5, valueTop,
                                content.width() * 0.5,
                                static_cast<double>(valueMetrics.height())),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1%").arg(QString::number(m_health, 'f', 0)));
    }
    else
    {
        // "no verdict" is not 100 % and must never be drawn as a number.
        painter.setPen(Theme::textMuted());
        painter.drawText(QRectF(content.left() + content.width() * 0.5, valueTop,
                                content.width() * 0.5,
                                static_cast<double>(valueMetrics.height())),
                         Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("--"));
    }
}

void DeviceTile::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        emit clicked(m_deviceId);
    }
    QWidget::mousePressEvent(event);
}

void DeviceTile::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void DeviceTile::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
// DashboardWidget
// ---------------------------------------------------------------------------

DashboardWidget::DashboardWidget(DeviceManager* manager, QWidget* parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("DashboardWidget"));
    buildUi();

    if (m_manager != nullptr)
    {
        connect(m_manager, &DeviceManager::deviceAdded, this, [this](const QString&)
        {
            rebuild();
        });
        connect(m_manager, &DeviceManager::deviceRemoved, this, [this](const QString&)
        {
            rebuild();
        });
        connect(m_manager, &DeviceManager::summaryChanged,
                this, &DashboardWidget::onSummaryChanged);
    }

    rebuild();
}

DashboardWidget::~DashboardWidget() = default;

void DashboardWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding,
                             Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding);
    root->setSpacing(Theme::Metrics::kPanelSpacing);

    // -- KPI row ------------------------------------------------------------
    auto* kpiRow = new QHBoxLayout();
    kpiRow->setSpacing(Theme::Metrics::kPanelSpacing);

    const auto makeKpi = [this](const QString& caption, MetricCard::Mode mode,
                                const QString& hint)
    {
        auto* card = new MetricCard(this);
        card->setCaption(caption);
        card->setDecimals(0);
        card->setMode(mode);
        card->setHint(hint);
        return card;
    };

    m_totalCard = makeKpi(tr("ASSETS"), MetricCard::Mode::Neutral, tr("registered"));
    m_healthyCard = makeKpi(tr("HEALTHY"), MetricCard::Mode::Neutral, tr("state: normal"));
    m_attentionCard = makeKpi(tr("NEEDS ATTENTION"), MetricCard::Mode::Threshold,
                              tr("warning or fault"));
    m_healthCard = makeKpi(tr("AVG HEALTH"), MetricCard::Mode::HigherIsBetter,
                           tr("fleet mean"));
    m_alarmCard = makeKpi(tr("ALARMS"), MetricCard::Mode::Neutral, tr("since start"));

    // A count of zero is healthy for "needs attention" and merely empty for the
    // others, so the thresholds are deliberately lopsided: one fault is already
    // worth amber, five is worth red.
    m_attentionCard->setThresholds(1.0, 4.0);
    m_healthCard->setThresholds(60.0, 40.0);

    for (MetricCard* card : {m_totalCard, m_healthyCard, m_attentionCard, m_healthCard, m_alarmCard})
    {
        kpiRow->addWidget(card, 1);
    }

    root->addLayout(kpiRow);

    // -- content: tiles on the left, trend on the right ---------------------
    auto* contentRow = new QHBoxLayout();
    contentRow->setSpacing(Theme::Metrics::kPanelSpacing);

    // tiles
    auto* tilePanel = new QFrame(this);
    tilePanel->setObjectName(QStringLiteral("Panel"));
    auto* tilePanelLayout = new QVBoxLayout(tilePanel);
    tilePanelLayout->setContentsMargins(Theme::Metrics::kPanelPadding,
                                        Theme::Metrics::kPanelPadding,
                                        Theme::Metrics::kPanelPadding,
                                        Theme::Metrics::kPanelPadding);
    tilePanelLayout->setSpacing(6);

    m_tileHeader = new QLabel(tr("Asset overview"), tilePanel);
    m_tileHeader->setObjectName(QStringLiteral("PanelTitle"));
    tilePanelLayout->addWidget(m_tileHeader);

    m_tileHost = new QWidget(tilePanel);
    m_tileGrid = new QGridLayout(m_tileHost);
    m_tileGrid->setContentsMargins(0, 0, 0, 0);
    m_tileGrid->setSpacing(kTileSpacing);
    tilePanelLayout->addWidget(m_tileHost);
    tilePanelLayout->addStretch(1);

    m_tileEmpty = new QLabel(tr("No devices are registered."), tilePanel);
    m_tileEmpty->setObjectName(QStringLiteral("EmptyState"));
    m_tileEmpty->setAlignment(Qt::AlignCenter);
    m_tileEmpty->hide();
    tilePanelLayout->addWidget(m_tileEmpty);

    auto* tileScroll = new QScrollArea(this);
    tileScroll->setWidget(tilePanel);
    tileScroll->setWidgetResizable(true);
    tileScroll->setFrameShape(QFrame::NoFrame);
    tileScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tileViewport = tileScroll->viewport();

    contentRow->addWidget(tileScroll, 3);

    // trend
    ensureChart();
    contentRow->addWidget(m_chart, 2);

    root->addLayout(contentRow, 1);
}

void DashboardWidget::ensureChart()
{
    if (m_chart != nullptr)
    {
        return;
    }

    m_chart = new ChartWidget(m_manager, this);
    connect(m_chart, &ChartWidget::channelChanged, this, [this](ChartWidget::Channel)
    {
        // The chart owns its own channel selection; nothing to do here, but the
        // connection documents that the dashboard is aware of the change and is
        // where a future "sync the detail gauges to the chart channel" hook goes.
    });
}

void DashboardWidget::rebuild()
{
    if (m_manager == nullptr)
    {
        return;
    }

    const QStringList ids = m_manager->deviceIds();

    // -- remove tiles for devices that are gone -----------------------------
    // Collected first, then erased: mutating the hash while iterating it is
    // undefined behaviour, and Qt's iterator invalidation here is silent.
    QStringList stale;
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it)
    {
        if (!ids.contains(it.key()))
        {
            stale.append(it.key());
        }
    }
    for (const QString& id : stale)
    {
        DeviceTile* tile = m_tiles.take(id);
        if (tile != nullptr)
        {
            m_tileGrid->removeWidget(tile);
            tile->deleteLater();
        }
    }

    // -- add tiles for new devices ------------------------------------------
    for (const QString& id : ids)
    {
        if (m_tiles.contains(id))
        {
            continue;
        }

        const std::shared_ptr<Device> device = m_manager->device(id);
        if (device == nullptr)
        {
            continue;
        }

        auto* tile = new DeviceTile(m_tileHost);
        tile->setDevice(id, tileName(*device), device->deviceType());
        connect(tile, &DeviceTile::clicked, this, &DashboardWidget::onTileClicked);

        m_tiles.insert(id, tile);
    }

    // -- re-lay the grid ----------------------------------------------------
    // Worst first, matching the device list's default ordering: a dashboard
    // whose tiles are in registration order makes the operator read all of them
    // to find the one that is red.
    QList<QString> ordered;
    ordered.reserve(m_tiles.size());
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it)
    {
        ordered.append(it.key());
    }

    std::sort(ordered.begin(), ordered.end(), [this](const QString& left, const QString& right)
    {
        const std::shared_ptr<Device> a = m_manager->device(left);
        const std::shared_ptr<Device> b = m_manager->device(right);
        const int aState = (a != nullptr) ? static_cast<int>(a->state()) : 0;
        const int bState = (b != nullptr) ? static_cast<int>(b->state()) : 0;
        if (aState != bState)
        {
            return aState > bState;
        }
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });

    const int available = m_tileViewport->width() - 2 * Theme::Metrics::kPanelPadding;
    m_columns = std::max(1, (available + kTileSpacing) / (kTilePreferredWidth + kTileSpacing));

    // Detach everything before re-adding, so the grid does not briefly contain a
    // widget at two positions during the reshuffle.
    for (DeviceTile* tile : m_tiles)
    {
        m_tileGrid->removeWidget(tile);
    }

    int row = 0;
    int column = 0;
    for (const QString& id : ordered)
    {
        DeviceTile* tile = m_tiles.value(id, nullptr);
        if (tile == nullptr)
        {
            continue;
        }
        m_tileGrid->addWidget(tile, row, column);
        if (++column >= m_columns)
        {
            column = 0;
            ++row;
        }
    }

    // Keep the last row from stretching its tiles to full width, which makes a
    // two-device fleet look like two enormous buttons.
    for (int index = 0; index < m_tileGrid->columnCount(); ++index)
    {
        m_tileGrid->setColumnStretch(index, 0);
    }
    for (int index = 0; index < m_columns; ++index)
    {
        m_tileGrid->setColumnStretch(index, 1);
    }

    m_tileEmpty->setVisible(m_tiles.isEmpty());
    m_tileHost->setVisible(!m_tiles.isEmpty());

    refresh();
}

void DashboardWidget::refresh()
{
    if (m_manager == nullptr)
    {
        return;
    }

    const DeviceManager::FleetSummary summary = m_manager->summary();

    // -- KPIs ---------------------------------------------------------------
    m_totalCard->setValue(static_cast<double>(summary.total));
    m_healthyCard->setValue(static_cast<double>(summary.normal));
    m_attentionCard->setValue(static_cast<double>(summary.warning + summary.fault));
    m_alarmCard->setValue(static_cast<double>(summary.totalAlarms));

    if (summary.online > 0)
    {
        m_healthCard->setValue(summary.averageHealth);
    }
    else
    {
        // No device has reported yet, so there is no fleet mean.  Showing 0 %
        // would read as "everything is critically unhealthy", which is the
        // opposite of "we do not know yet".
        m_healthCard->setPlaceholder(QStringLiteral("--"));
    }

    // -- tiles --------------------------------------------------------------
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it)
    {
        DeviceTile* tile = it.value();
        const std::shared_ptr<Device> device = m_manager->device(it.key());
        if (tile == nullptr || device == nullptr)
        {
            continue;
        }

        const SensorSample& sample = device->latest();
        const FaultPrediction& prediction = device->prediction();

        tile->updateState(device->state(),
                          sample.isValid() ? sample.temperature : 0.0,
                          prediction.healthScore,
                          !prediction.faultType.isEmpty());
    }

    // -- trend --------------------------------------------------------------
    if (m_chart != nullptr)
    {
        m_chart->refresh();
    }
}

void DashboardWidget::setSelectedDevice(const QString& deviceId)
{
    m_selectedDeviceId = deviceId;

    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it)
    {
        if (it.value() != nullptr)
        {
            it.value()->setSelected(it.key() == deviceId);
        }
    }

    if (m_chart != nullptr)
    {
        m_chart->setDevice(deviceId);
    }
}

void DashboardWidget::onSummaryChanged(const saim::DeviceManager::FleetSummary& summary)
{
    m_summary = summary;
    refresh();
}

void DashboardWidget::onTileClicked(const QString& deviceId)
{
    // Clicking a tile both selects the device and shows its trend, and — on a
    // double click — asks the shell to switch to the detail page.  The widget
    // does not decide which: it publishes the click and the selection, and the
    // shell wires those to a navigation step.
    setSelectedDevice(deviceId);
    emit deviceSelected(deviceId);
    emit deviceActivated(deviceId);
}

QString DashboardWidget::healthText(const Device& device) const
{
    const FaultPrediction& prediction = device.prediction();
    return prediction.faultType.isEmpty()
               ? QStringLiteral("--")
               : QStringLiteral("%1%").arg(QString::number(prediction.healthScore, 'f', 0));
}

QString DashboardWidget::tileName(const Device& device) const
{
    return device.deviceName().isEmpty() ? device.deviceId() : device.deviceName();
}

}   // namespace saim
