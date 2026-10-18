#pragma once

// ---------------------------------------------------------------------------
// DashboardWidget.h - the fleet at a glance: KPIs, a device grid and a trend.
//
// What a dashboard is for, and what that rules out
// ------------------------------------------------
// A dashboard is not a smaller version of the detail view.  It answers one
// question - "is anything wrong, and where?" - and it has to answer it in the
// two seconds an operator spends walking past the screen.  Everything that does
// not serve that question is noise, however interesting it is.
//
// So this panel is deliberately three things and no more:
//
//   1. A KPI row: how many assets, how many are healthy, how many are alarming,
//      average health.  These are the *counts*, because a count is what tells
//      you whether to look further.
//
//   2. A device grid of state tiles - one per asset, colour-coded, ordered worst
//      first.  This is the "where?" - the one thing a table of numbers cannot
//      answer quickly, because a table makes you read while a grid lets you scan.
//
//   3. A trend strip for the selected device, so the dashboard can confirm a
//      suspicion without a navigation step.
//
// The tiles are rebuilt only when the *set* of devices changes; their colours
// and values update in place on the refresh tick.  Rebuilding them on every
// sample would destroy and recreate a dozen widgets ten times a second, which is
// both a leak waiting to happen and visibly flickery.
// ---------------------------------------------------------------------------

#include "common/Types.h"
#include "device/DeviceManager.h"

#include <QHash>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QLabel;

namespace saim {

class ChartWidget;
class MetricCard;
class StateIndicator;

/// One tile in the device grid.  A small widget rather than a styled QLabel so
/// it can own its own click behaviour and hover state.
class DeviceTile final : public QWidget
{
    Q_OBJECT
public:
    explicit DeviceTile(QWidget* parent = nullptr);

    void setDevice(const QString& deviceId, const QString& name, const QString& type);
    void updateState(DeviceState state, double temperature, double health, bool hasHealth);
    void setSelected(bool selected);

    [[nodiscard]] QString deviceId() const noexcept { return m_deviceId; }

signals:
    void clicked(const QString& deviceId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QString m_deviceId;
    QString m_name;
    QString m_type;

    DeviceState m_state = DeviceState::Offline;
    double m_temperature = 0.0;
    double m_health = 0.0;
    bool   m_hasHealth = false;
    bool   m_selected = false;
    bool   m_hovered = false;
};

class DashboardWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit DashboardWidget(DeviceManager* manager, QWidget* parent = nullptr);
    ~DashboardWidget() override;

signals:
    /// A tile was clicked; the controller selects that device everywhere.
    void deviceSelected(const QString& deviceId);
    void deviceActivated(const QString& deviceId);
    /// A KPI card was clicked with a severity filter - "show me the 2 faults".
    void severityFilterRequested(int minimumState);

public slots:
    /// Re-reads the fleet: adds and removes tiles as needed.
    void rebuild();

    /// Pushes fresh numbers into the tiles and KPIs.  Driven by the controller's
    /// coalescing timer.
    void refresh();

    /// Highlights a tile without emitting deviceSelected (which would loop).
    void setSelectedDevice(const QString& deviceId);

private slots:
    void onSummaryChanged(const saim::DeviceManager::FleetSummary& summary);
    void onTileClicked(const QString& deviceId);

private:
    void buildUi();
    void ensureChart();
    [[nodiscard]] QString healthText(const Device& device) const;
    [[nodiscard]] QString tileName(const Device& device) const;

    DeviceManager* m_manager = nullptr;

    MetricCard* m_totalCard = nullptr;
    MetricCard* m_healthyCard = nullptr;
    MetricCard* m_attentionCard = nullptr;
    MetricCard* m_healthCard = nullptr;
    MetricCard* m_alarmCard = nullptr;

    QWidget*     m_tileHost = nullptr;
    QWidget*     m_tileViewport = nullptr;
    QGridLayout* m_tileGrid = nullptr;
    QLabel*      m_tileHeader = nullptr;
    QLabel*      m_tileEmpty = nullptr;

    QHash<QString, DeviceTile*> m_tiles;

    ChartWidget* m_chart = nullptr;
    QString      m_selectedDeviceId;

    DeviceManager::FleetSummary m_summary;

    /// Tiles per row.  Recomputed on resize so the grid fills the width rather
    /// than leaving a ragged column at the right edge.
    int m_columns = 3;
};

}   // namespace saim
