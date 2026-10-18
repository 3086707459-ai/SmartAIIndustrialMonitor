#pragma once

// ---------------------------------------------------------------------------
// DeviceListModel.h - the fleet as a list model.
//
// Why a model rather than filling a QListWidget by hand
// -----------------------------------------------------
// Three requirements make the hand-filled list the wrong tool:
//
//   * Sorting by severity.  An operator does not want devices in registration
//     order, they want the one that is on fire at the top.  With a model this is
//     a QSortFilterProxyModel and a sort role; with widgets it is a manual
//     reinsertion pass on every state change, which is where the flicker and the
//     selection loss come from.
//
//   * Filtering.  "Show me only the pumps" and "show me only what is alarming"
//     are the same proxy with a different predicate.
//
//   * Repaint cost.  A model tells the view *which* row changed.  Rebuilding a
//     widget list repaints and re-lays-out everything, and at 10 Hz that is
//     visible as a shudder every time a value ticks.
//
// The model reads the DeviceManager but never mutates it - a view is not allowed
// to own domain state, which is what keeps "the list shows something the rest of
// the application disagrees with" impossible.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QVector>

#include <memory>

namespace saim {

class DeviceManager;
class Device;

class DeviceListModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    /// Extra roles.  Start above Qt::UserRole so they cannot collide with the
    /// standard ones, and keep them together because the delegate and the proxy
    /// both key off this list.
    enum Roles
    {
        DeviceIdRole = Qt::UserRole + 1,
        DeviceNameRole,
        DeviceTypeRole,
        DeviceStateRole,
        TemperatureRole,
        HealthRole,
        VibrationRole,
        AlarmCountRole,
        LastSeenRole,
        EnabledRole
    };

    explicit DeviceListModel(DeviceManager* manager, QObject* parent = nullptr);
    ~DeviceListModel() override;

    DeviceListModel(const DeviceListModel&)            = delete;
    DeviceListModel& operator=(const DeviceListModel&) = delete;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

    /// Row for a device id, or an invalid index.  Used to restore the selection
    /// after a rebuild and to scroll to a device named from elsewhere.
    [[nodiscard]] QModelIndex indexOfDevice(const QString& deviceId) const;

    /// Re-reads the fleet from the manager.  Called on add/remove/metadata change;
    /// deliberately *not* on every sample, so a 20 Hz stream cannot drive a
    /// full-model reset twenty times a second.
    void reload();

    /// Re-emits data for every row.  Driven by the panel's coalescing timer, so
    /// live values refresh at the display rate rather than the sample rate.
    void refreshValues();

private slots:
    void onDeviceAdded(const QString& deviceId);
    void onDeviceRemoved(const QString& deviceId);
    void onDeviceMetadataChanged(const QString& deviceId);
    void onDeviceStateChanged(const QString& deviceId, saim::DeviceState state);
    void onDevicePrediction(const saim::FaultPrediction& prediction);

private:
    [[nodiscard]] QVariant valueFor(const Device& device, int role) const;
    void emitRowChanged(const QString& deviceId);

    DeviceManager* m_manager = nullptr;

    /// Stable ordering: registration order.  Displayed order comes from the
    /// proxy, so the model never has to guess at the user's sort.
    QVector<QString> m_ids;
};

// ---------------------------------------------------------------------------
// DeviceFilterProxy - search text and a "only what needs attention" switch.
// ---------------------------------------------------------------------------

class DeviceFilterProxy final : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit DeviceFilterProxy(QObject* parent = nullptr);

    void setSearchText(const QString& text);
    void setOnlyProblems(bool onlyProblems);
    void setTypeFilter(const QString& deviceType);   ///< empty -> all types

    [[nodiscard]] QString searchText() const { return m_search; }
    [[nodiscard]] bool onlyProblems() const noexcept { return m_onlyProblems; }

    /// Sorts by severity first and by name within a severity band.  This is the
    /// default, and it is the ordering the panel starts in.
    void sortBySeverity();

    /// Every other ordering goes through here.  Overriding sort() rather than
    /// leaving it to the base class is what clears the severity flag - without
    /// it, choosing "Name" after "Severity" would keep the severity comparator
    /// and the sort box would appear to do nothing.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    QString m_search;
    QString m_typeFilter;
    bool    m_onlyProblems = false;
    bool    m_severitySort = false;
};

}   // namespace saim
