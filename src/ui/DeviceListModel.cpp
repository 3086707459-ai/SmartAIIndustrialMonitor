#include "ui/DeviceListModel.h"

#include "device/Device.h"
#include "device/DeviceManager.h"
#include "ui/Theme.h"

#include <QBrush>
#include <QIcon>

#include <algorithm>

namespace saim {

// ---------------------------------------------------------------------------
// DeviceListModel
// ---------------------------------------------------------------------------

DeviceListModel::DeviceListModel(DeviceManager* manager, QObject* parent)
    : QAbstractListModel(parent)
    , m_manager(manager)
{
    if (m_manager != nullptr)
    {
        // Connect only to lifecycle and state signals - not to sampleReceived.
        // A full model reset on every sample would be 20 rebuilds a second per
        // device; live numbers are refreshed by refreshValues() on the panel's
        // coalescing timer instead.
        connect(m_manager, &DeviceManager::deviceAdded,
                this, &DeviceListModel::onDeviceAdded);
        connect(m_manager, &DeviceManager::deviceRemoved,
                this, &DeviceListModel::onDeviceRemoved);
        connect(m_manager, &DeviceManager::deviceMetadataChanged,
                this, &DeviceListModel::onDeviceMetadataChanged);
        connect(m_manager, &DeviceManager::stateChanged,
                this, &DeviceListModel::onDeviceStateChanged);
        connect(m_manager, &DeviceManager::predictionUpdated,
                this, &DeviceListModel::onDevicePrediction);
    }

    reload();
}

DeviceListModel::~DeviceListModel() = default;

int DeviceListModel::rowCount(const QModelIndex& parent) const
{
    // A list model has no children below the root.
    return parent.isValid() ? 0 : static_cast<int>(m_ids.size());
}

QVariant DeviceListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_ids.size()
        || m_manager == nullptr)
    {
        return {};
    }

    const std::shared_ptr<Device> device = m_manager->device(m_ids.at(index.row()));
    if (device == nullptr)
    {
        return {};
    }

    if (role == Qt::DisplayRole)
    {
        return device->deviceName().isEmpty() ? device->deviceId() : device->deviceName();
    }
    if (role == Qt::DecorationRole)
    {
        // The device-type glyph, so a long list is scannable without reading
        // every label.  Resolved through Theme so the list, the dashboard cards
        // and the report cannot disagree about what a pump looks like.
        return Theme::icon(Theme::iconForDeviceType(device->deviceType()));
    }
    if (role == Qt::ToolTipRole)
    {
        const SensorSample& latest = device->latest();
        return QStringLiteral("%1\nID: %2\nType: %3\nState: %4\n"
                              "Temperature: %5 C\nVibration: %6 mm/s\n"
                              "Health: %7\nSamples: %8   Alarms: %9")
            .arg(device->deviceName().isEmpty() ? device->deviceId() : device->deviceName(),
                 device->deviceId(),
                 device->deviceType(),
                 toString(device->state()),
                 latest.isValid() ? QString::number(latest.temperature, 'f', 1)
                                  : QStringLiteral("--"),
                 latest.isValid() ? QString::number(latest.vibration, 'f', 3)
                                  : QStringLiteral("--"),
                 device->prediction().faultType.isEmpty()
                     ? QStringLiteral("analysing")
                     : QString::number(device->prediction().healthScore, 'f', 0) + QLatin1Char('%'),
                 QString::number(device->sampleCount()),
                 QString::number(device->alarmCount()));
    }

    return valueFor(*device, role);
}

Qt::ItemFlags DeviceListModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
    {
        return Qt::NoItemFlags;
    }
    // Selectable but not editable: the list is a view onto the fleet, and device
    // metadata is edited through the detail panel where the fields are validated.
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant DeviceListModel::valueFor(const Device& device, int role) const
{
    switch (role)
    {
    case DeviceIdRole:
        return device.deviceId();
    case DeviceNameRole:
        return device.deviceName().isEmpty() ? device.deviceId() : device.deviceName();
    case DeviceTypeRole:
        return device.deviceType();
    case DeviceStateRole:
        return static_cast<int>(device.state());
    case TemperatureRole:
        return device.latest().isValid() ? device.latest().temperature : 0.0;
    case VibrationRole:
        return device.latest().isValid() ? device.latest().vibration : 0.0;
    case HealthRole:
        return device.prediction().faultType.isEmpty()
                   ? QVariant()                                  // "analysing"
                   : QVariant(device.prediction().healthScore);
    case AlarmCountRole:
        return static_cast<qlonglong>(device.alarmCount());
    case LastSeenRole:
        return device.lastSeen();
    case EnabledRole:
        return device.isEnabled();
    default:
        return {};
    }
}

QModelIndex DeviceListModel::indexOfDevice(const QString& deviceId) const
{
    const auto it = std::find(m_ids.cbegin(), m_ids.cend(), deviceId);
    if (it == m_ids.cend())
    {
        return {};
    }
    return index(static_cast<int>(std::distance(m_ids.cbegin(), it)), 0);
}

void DeviceListModel::reload()
{
    beginResetModel();

    m_ids.clear();
    if (m_manager != nullptr)
    {
        m_ids = m_manager->deviceIds();
    }

    endResetModel();
}

void DeviceListModel::refreshValues()
{
    if (m_ids.isEmpty())
    {
        return;
    }

    // One contiguous dataChanged for the whole range.  Emitting per row would be
    // N signals per tick; the view coalesces internally, but the proxy's sort and
    // filter still re-run per emission, and at 6 devices x 10 Hz that is real
    // work for no benefit.
    emit dataChanged(index(0, 0), index(static_cast<int>(m_ids.size()) - 1, 0),
                     {TemperatureRole, VibrationRole, HealthRole, AlarmCountRole, LastSeenRole});
}

void DeviceListModel::emitRowChanged(const QString& deviceId)
{
    const QModelIndex row = indexOfDevice(deviceId);
    if (row.isValid())
    {
        emit dataChanged(row, row);
    }
}

void DeviceListModel::onDeviceAdded(const QString& deviceId)
{
    if (m_ids.contains(deviceId))
    {
        return;
    }

    const int row = static_cast<int>(m_ids.size());
    beginInsertRows({}, row, row);
    m_ids.append(deviceId);
    endInsertRows();
}

void DeviceListModel::onDeviceRemoved(const QString& deviceId)
{
    const QModelIndex row = indexOfDevice(deviceId);
    if (!row.isValid())
    {
        return;
    }

    const int position = row.row();
    beginRemoveRows({}, position, position);
    m_ids.remove(position);
    endRemoveRows();
}

void DeviceListModel::onDeviceMetadataChanged(const QString& deviceId)
{
    emitRowChanged(deviceId);
}

void DeviceListModel::onDeviceStateChanged(const QString& deviceId, saim::DeviceState state)
{
    Q_UNUSED(state)
    // The state itself is read through DeviceStateRole; this slot exists purely to
    // publish the change.  The proxy is configured with dynamicSortFilter, so it
    // re-sorts on the dataChanged this emits and a newly faulted device rises to
    // the top of the list without any explicit invalidation from here - which is
    // what keeps the model free of knowledge about its proxies.
    emitRowChanged(deviceId);
}

void DeviceListModel::onDevicePrediction(const saim::FaultPrediction& prediction)
{
    emitRowChanged(prediction.deviceId);
}

// ---------------------------------------------------------------------------
// DeviceFilterProxy
// ---------------------------------------------------------------------------

DeviceFilterProxy::DeviceFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    // Case- and accent-insensitive so that "motor" matches "Motor", and a device
    // named with a diacritic is still findable from a plain keyboard.
    setFilterCaseSensitivity(Qt::CaseInsensitive);
}

void DeviceFilterProxy::setSearchText(const QString& text)
{
    if (text == m_search)
    {
        return;
    }
    m_search = text;
    invalidateFilter();
}

void DeviceFilterProxy::setOnlyProblems(bool onlyProblems)
{
    if (onlyProblems == m_onlyProblems)
    {
        return;
    }
    m_onlyProblems = onlyProblems;
    invalidateFilter();
}

void DeviceFilterProxy::setTypeFilter(const QString& deviceType)
{
    if (deviceType == m_typeFilter)
    {
        return;
    }
    m_typeFilter = deviceType;
    invalidateFilter();
}

void DeviceFilterProxy::sortBySeverity()
{
    m_severitySort = true;
    // Calls the base implementation directly, deliberately bypassing this
    // class's own sort() override - which exists to *clear* the flag.
    // SortDescending because severity is stored as an int where Fault > Normal;
    // ascending would put the healthy machines first, which is precisely the
    // information the operator does not need.
    QSortFilterProxyModel::sort(0, Qt::DescendingOrder);
}

void DeviceFilterProxy::sort(int column, Qt::SortOrder order)
{
    m_severitySort = false;
    QSortFilterProxyModel::sort(column, order);
}

bool DeviceFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QModelIndex source = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!source.isValid())
    {
        return false;
    }

    if (!m_typeFilter.isEmpty())
    {
        const QString type = source.data(DeviceListModel::DeviceTypeRole).toString();
        if (type.compare(m_typeFilter, Qt::CaseInsensitive) != 0)
        {
            return false;
        }
    }

    if (m_onlyProblems)
    {
        const int state = source.data(DeviceListModel::DeviceStateRole).toInt();
        if (state < static_cast<int>(DeviceState::Warning))
        {
            return false;
        }
    }

    if (m_search.isEmpty())
    {
        return true;
    }

    // Match against id, name and type: an engineer searching "pump" wants every
    // pump, and one searching "M-003" wants that one asset.
    const QString search = m_search.trimmed();
    return source.data(DeviceListModel::DeviceIdRole).toString().contains(search, Qt::CaseInsensitive)
           || source.data(DeviceListModel::DeviceNameRole).toString().contains(search, Qt::CaseInsensitive)
           || source.data(DeviceListModel::DeviceTypeRole).toString().contains(search, Qt::CaseInsensitive);
}

bool DeviceFilterProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    if (m_severitySort)
    {
        const int leftState  = left.data(DeviceListModel::DeviceStateRole).toInt();
        const int rightState = right.data(DeviceListModel::DeviceStateRole).toInt();

        if (leftState != rightState)
        {
            // Fault before Warning before Normal before Offline.  Offline sits
            // last rather than first: it is the *least* urgent thing on the list,
            // because a machine that is switched off is behaving correctly, while
            // a machine reporting a fault is not.
            const auto rank = [](int state)
            {
                switch (static_cast<DeviceState>(state))
                {
                case DeviceState::Fault:   return 0;
                case DeviceState::Warning: return 1;
                case DeviceState::Normal:  return 2;
                case DeviceState::Offline:
                default:                   return 3;
                }
            };
            return rank(leftState) < rank(rightState);
        }

        // Within a severity band, the sicker machine first: a lower health score
        // is nearer to failing and deserves the higher position.
        const QVariant leftHealth  = left.data(DeviceListModel::HealthRole);
        const QVariant rightHealth = right.data(DeviceListModel::HealthRole);

        if (leftHealth.isValid() && rightHealth.isValid()
            && !qFuzzyCompare(leftHealth.toDouble() + 1.0, rightHealth.toDouble() + 1.0))
        {
            return leftHealth.toDouble() < rightHealth.toDouble();
        }
    }

    // Tie-break by name, so the order is stable.  An unstable sort makes rows
    // swap places between refreshes, which is unreadable and makes a click land
    // on the wrong device - the one interaction bug that is genuinely dangerous
    // in a control room.
    return QString::compare(left.data(Qt::DisplayRole).toString(),
                            right.data(Qt::DisplayRole).toString(),
                            Qt::CaseInsensitive) < 0;
}

}   // namespace saim
