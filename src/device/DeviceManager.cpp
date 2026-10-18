#include "device/DeviceManager.h"

#include "core/ConfigManager.h"
#include "core/Logger.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QReadLocker>
#include <QWriteLocker>

#include <algorithm>

namespace saim {

namespace {
constexpr int kLivenessIntervalMs = 3000;
constexpr int kSummaryIntervalMs  = 500;
constexpr int kDefaultHistoryCapacity = 600;
}   // namespace

DeviceManager::DeviceManager(ConfigManager* config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    setObjectName(QStringLiteral("DeviceManager"));

    if (m_config != nullptr)
    {
        m_offlineTimeoutMs = m_config->alarms().offlineTimeoutMs;
        connect(m_config, &ConfigManager::reloaded, this, &DeviceManager::loadFromConfig);
    }

    // One timer for the whole fleet.  Checking liveness on a timer rather than
    // on a per-device QTimer keeps the wake-up count constant as the fleet
    // grows - on a 500-device line the naive design would be pathological.
    m_livenessTimer.setInterval(kLivenessIntervalMs);
    connect(&m_livenessTimer, &QTimer::timeout, this, &DeviceManager::onLivenessTick);
    m_livenessTimer.start();

    m_summaryTimer.setInterval(kSummaryIntervalMs);
    connect(&m_summaryTimer, &QTimer::timeout, this, &DeviceManager::refreshSummary);
    m_summaryTimer.start();
}

DeviceManager::~DeviceManager() = default;

// ---------------------------------------------------------------------------
// Population
// ---------------------------------------------------------------------------

Status DeviceManager::addDevice(const QString& deviceId, const QString& deviceName,
                                const QString& deviceType, const QString& ipAddress,
                                quint16 port)
{
    if (deviceId.trimmed().isEmpty())
    {
        return Status::fail(1, QStringLiteral("Device id must not be empty"));
    }

    QWriteLocker locker(&m_lock);
    if (m_devices.contains(deviceId))
    {
        return Status::fail(2, QStringLiteral("Device '%1' is already registered").arg(deviceId));
    }

    auto device = std::make_shared<Device>(deviceId, deviceName);
    device->setDeviceType(deviceType);
    device->setEndpoint(ipAddress, port);
    device->setHistoryCapacity(kDefaultHistoryCapacity);

    // A device created from config gets a type-appropriate envelope; one created
    // ad-hoc keeps the generic motor-like defaults.
    device->setLimits([](const QString& type) {
        DeviceLimits limits;
        if (type.compare(QLatin1String("pump"), Qt::CaseInsensitive) == 0)
        {
            limits.temperatureWarn = 65.0;
            limits.temperatureHigh = 80.0;
            limits.currentWarn     = 12.0;
            limits.currentHigh     = 16.0;
            limits.vibrationWarn   = 0.40;
            limits.vibrationHigh   = 0.70;
        }
        else if (type.compare(QLatin1String("spindle"), Qt::CaseInsensitive) == 0)
        {
            limits.temperatureWarn = 75.0;
            limits.temperatureHigh = 90.0;
            limits.currentWarn     = 18.0;
            limits.currentHigh     = 24.0;
            limits.vibrationWarn   = 0.50;
            limits.vibrationHigh   = 0.90;
        }
        return limits;
    }(deviceType));

    connectDeviceSignals(device);
    m_devices.insert(deviceId, device);
    locker.unlock();

    SAIM_INFO(logcat::kDevice,
              QStringLiteral("Device registered: %1 (%2) at %3:%4")
                  .arg(deviceId, deviceType, ipAddress)
                  .arg(port));

    emit deviceAdded(deviceId);
    refreshSummary();
    return Status::ok();
}

Status DeviceManager::removeDevice(const QString& deviceId)
{
    std::shared_ptr<Device> removed;
    {
        QWriteLocker locker(&m_lock);
        auto it = m_devices.find(deviceId);
        if (it == m_devices.end())
        {
            return Status::fail(1, QStringLiteral("Unknown device '%1'").arg(deviceId));
        }
        removed = it.value();
        m_devices.erase(it);
    }

    // The shared_ptr keeps the object alive until the last queued signal that
    // captured it has been delivered - so a sample in flight cannot resurrect a
    // dangling device.  This is the reason the registry stores shared_ptr rather
    // than raw pointers even though QObject parenting would also work.
    if (removed)
    {
        removed->disconnect(this);
    }

    SAIM_INFO(logcat::kDevice, QStringLiteral("Device removed: %1").arg(deviceId));
    emit deviceRemoved(deviceId);
    refreshSummary();
    return Status::ok();
}

Status DeviceManager::updateDevice(const QString& deviceId, const QString& deviceName,
                                   const QString& ipAddress, quint16 port)
{
    Device* device = findDevice(deviceId);
    if (device == nullptr)
    {
        return Status::fail(1, QStringLiteral("Unknown device '%1'").arg(deviceId));
    }
    device->setDeviceName(deviceName);
    device->setEndpoint(ipAddress, port);
    emit deviceMetadataChanged(deviceId);
    return Status::ok();
}

Status DeviceManager::setDeviceEnabled(const QString& deviceId, bool enabled)
{
    Device* device = findDevice(deviceId);
    if (device == nullptr)
    {
        return Status::fail(1, QStringLiteral("Unknown device '%1'").arg(deviceId));
    }
    device->setEnabled(enabled);
    emit deviceMetadataChanged(deviceId);
    return Status::ok();
}

Status DeviceManager::setDeviceLimits(const QString& deviceId, const DeviceLimits& limits)
{
    Device* device = findDevice(deviceId);
    if (device == nullptr)
    {
        return Status::fail(1, QStringLiteral("Unknown device '%1'").arg(deviceId));
    }
    device->setLimits(limits);
    emit deviceMetadataChanged(deviceId);
    return Status::ok();
}

void DeviceManager::loadFromConfig()
{
    if (m_config == nullptr)
    {
        return;
    }

    const QJsonArray configured = m_config->getArray(QStringLiteral("devices"));
    QStringList wantedIds;

    for (const QJsonValue& entry : configured)
    {
        if (!entry.isObject())
        {
            continue;
        }
        const QJsonObject object = entry.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
        {
            SAIM_WARN(logcat::kDevice, QStringLiteral("Skipping device entry without an id"));
            continue;
        }
        wantedIds << id;

        const QString name = object.value(QStringLiteral("name")).toString(id);
        const QString type = object.value(QStringLiteral("type")).toString(QStringLiteral("generic"));
        const QString ip   = object.value(QStringLiteral("ip")).toString();
        const quint16 port = static_cast<quint16>(object.value(QStringLiteral("port")).toInt(0));

        if (!contains(id))
        {
            const Status status = addDevice(id, name, type, ip, port);
            if (status.isError())
            {
                SAIM_WARN(logcat::kDevice,
                          QStringLiteral("Cannot add device %1: %2").arg(id, status.error().message));
                continue;
            }
        }
        else
        {
            // Reconcile in place: keep the accumulating history, refresh the
            // metadata.  Rebuilding would throw away the window the AI needs.
            updateDevice(id, name, ip, port);
        }

        findDevice(id)->setDeviceType(type);
        findDevice(id)->setEnabled(object.value(QStringLiteral("enabled")).toBool(true));
        // Per-device overrides from config win over the type defaults.
        const QJsonValue limitsValue = object.value(QStringLiteral("limits"));
        if (limitsValue.isObject())
        {
            const QJsonObject limitsObject = limitsValue.toObject();
            DeviceLimits limits = findDevice(id)->limits();
            const auto readLimit = [&limitsObject](const char* field, double current) {
                const QJsonValue v = limitsObject.value(QLatin1String(field));
                return v.isDouble() ? v.toDouble() : current;
            };
            limits.temperatureWarn = readLimit("temperatureWarn", limits.temperatureWarn);
            limits.temperatureHigh = readLimit("temperatureHigh", limits.temperatureHigh);
            limits.voltageWarnLow = readLimit("voltageWarnLow", limits.voltageWarnLow);
            limits.voltageWarnHigh = readLimit("voltageWarnHigh", limits.voltageWarnHigh);
            limits.currentWarn     = readLimit("currentWarn", limits.currentWarn);
            limits.currentHigh     = readLimit("currentHigh", limits.currentHigh);
            limits.vibrationWarn   = readLimit("vibrationWarn", limits.vibrationWarn);
            limits.vibrationHigh   = readLimit("vibrationHigh", limits.vibrationHigh);
            if (Device* device = findDevice(id))
            {
                device->setLimits(limits);
            }
        }
    }

    // An explicitly empty array retires the fleet; a missing or invalid key
    // does not silently erase dynamically registered devices.
    if (m_config->snapshot().value(QStringLiteral("devices")).isArray())
    {
        for (const QString& existing : deviceIds())
        {
            if (!wantedIds.contains(existing))
            {
                removeDevice(existing);
            }
        }
    }

    SAIM_INFO(logcat::kDevice,
              QStringLiteral("Fleet loaded from configuration: %1 device(s)").arg(deviceCount()));
}

void DeviceManager::clear()
{
    const QStringList ids = deviceIds();
    for (const QString& id : ids)
    {
        removeDevice(id);
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Device* DeviceManager::findDevice(const QString& deviceId) const
{
    QReadLocker locker(&m_lock);
    const auto it = m_devices.constFind(deviceId);
    return it == m_devices.constEnd() ? nullptr : it.value().get();
}

bool DeviceManager::contains(const QString& deviceId) const
{
    QReadLocker locker(&m_lock);
    return m_devices.contains(deviceId);
}

std::shared_ptr<Device> DeviceManager::device(const QString& deviceId) const
{
    QReadLocker locker(&m_lock);
    return m_devices.value(deviceId);
}

QList<std::shared_ptr<Device>> DeviceManager::devices() const
{
    QReadLocker locker(&m_lock);
    QList<std::shared_ptr<Device>> out;
    out.reserve(m_devices.size());
    for (const auto& entry : m_devices)
    {
        out.push_back(entry);
    }
    // Stable ordering matters: the UI list and the report must not reshuffle
    // between refreshes just because QHash iteration order is unspecified.
    std::sort(out.begin(), out.end(),
              [](const std::shared_ptr<Device>& a, const std::shared_ptr<Device>& b) {
                  return a->deviceId() < b->deviceId();
              });
    return out;
}

QStringList DeviceManager::deviceIds() const
{
    QReadLocker locker(&m_lock);
    QStringList ids = m_devices.keys();
    ids.sort();
    return ids;
}

int DeviceManager::deviceCount() const
{
    QReadLocker locker(&m_lock);
    return static_cast<int>(m_devices.size());
}

DeviceManager::FleetSummary DeviceManager::summary() const
{
    QReadLocker locker(&m_lock);
    return m_cachedSummary;
}

std::vector<SensorSample> DeviceManager::historySnapshot(const QString& deviceId, int maxSamples) const
{
    std::shared_ptr<Device> target;
    {
        QReadLocker locker(&m_lock);
        target = m_devices.value(deviceId);
    }
    if (!target)
    {
        return {};
    }

    // Device::history() copies the ring buffer, so once we hold the shared_ptr
    // the copy is safe even if the UI thread removes the device concurrently.
    std::vector<SensorSample> history = target->history();
    if (maxSamples > 0 && static_cast<int>(history.size()) > maxSamples)
    {
        history.erase(history.begin(), history.end() - maxSamples);
    }
    return history;
}

QJsonArray DeviceManager::snapshotJson() const
{
    QJsonArray array;
    for (const std::shared_ptr<Device>& entry : devices())
    {
        array.append(entry->toJson());
    }
    return array;
}

// ---------------------------------------------------------------------------
// Signal wiring
// ---------------------------------------------------------------------------

void DeviceManager::connectDeviceSignals(const std::shared_ptr<Device>& device)
{
    // Signature-compatible signals are piped straight through with a lambda
    // that only forwards - no logic here, so there is nothing to test.
    connect(device.get(), &Device::dataUpdated,
            this, [this](const SensorSample& sample) { emit sampleReceived(sample); });
    connect(device.get(), &Device::statusChanged, this, &DeviceManager::stateChanged);
    connect(device.get(), &Device::alarmTriggered, this, &DeviceManager::alarmRaised);
    connect(device.get(), &Device::predictionUpdated, this, &DeviceManager::predictionUpdated);
    connect(device.get(), &Device::anomalyDetected, this, &DeviceManager::anomalyDetected);
    connect(device.get(), &Device::metadataChanged, this, [this, id = device->deviceId()] {
        emit deviceMetadataChanged(id);
    });
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void DeviceManager::onSampleReceived(const SensorSample& sample)
{
    // Reject unusable frames at the boundary rather than letting a zero-filled
    // sample pollute the AI window and trip every threshold at once.
    if (!sample.isValid())
    {
        SAIM_DEBUG(logcat::kDevice,
                   QStringLiteral("Discarding invalid sample for '%1'").arg(sample.deviceId));
        return;
    }

    std::shared_ptr<Device> target;
    {
        QReadLocker locker(&m_lock);
        target = m_devices.value(sample.deviceId);
    }

    if (!target)
    {
        // Unknown device.  Auto-registration is tempting but wrong: a typo'd id
        // would silently create a phantom asset on the dashboard.  Log it and
        // let the operator decide.
        SAIM_WARN(logcat::kDevice,
                  QStringLiteral("Sample for unregistered device '%1' ignored")
                      .arg(sample.deviceId));
        return;
    }

    target->onSampleReceived(sample);
}

void DeviceManager::onLivenessTick()
{
    for (const std::shared_ptr<Device>& entry : devices())
    {
        entry->checkLiveness(m_offlineTimeoutMs);
    }
}

void DeviceManager::refreshSummary()
{
    const QList<std::shared_ptr<Device>> fleet = devices();

    FleetSummary updated;
    updated.total = static_cast<int>(fleet.size());

    double temperatureSum = 0.0;
    double healthSum      = 0.0;
    int    temperatureCount = 0;
    int    healthCount      = 0;

    for (const std::shared_ptr<Device>& entry : fleet)
    {
        switch (entry->state())
        {
        case DeviceState::Normal:
            ++updated.normal;
            ++updated.online;
            break;
        case DeviceState::Warning:
            ++updated.warning;
            ++updated.online;
            break;
        case DeviceState::Fault:
            ++updated.fault;
            ++updated.online;
            break;
        case DeviceState::Offline:
            ++updated.offline;
            break;
        }

        if (entry->latest().isValid())
        {
            temperatureSum += entry->latest().temperature;
            ++temperatureCount;
        }
        if (!entry->prediction().faultType.isEmpty() || entry->prediction().healthScore < 100.0)
        {
            healthSum += entry->prediction().healthScore;
            ++healthCount;
        }

        updated.totalSamples += entry->sampleCount();
        updated.totalAlarms  += entry->alarmCount();
    }

    if (temperatureCount > 0)
    {
        updated.averageTemperature = temperatureSum / temperatureCount;
    }
    // With no AI verdict yet, report a perfect score rather than 0 - an empty
    // fleet must not look like a catastrophic one.
    updated.averageHealth = healthCount > 0 ? healthSum / healthCount : 100.0;

    // Only publish on a real change: the summary drives a UI repaint and a
    // queued signal per 500 ms tick would be pure noise otherwise.
    const bool changed = updated.total != m_cachedSummary.total
                         || updated.normal != m_cachedSummary.normal
                         || updated.warning != m_cachedSummary.warning
                         || updated.fault != m_cachedSummary.fault
                         || updated.offline != m_cachedSummary.offline;
    const bool numbersChanged = std::abs(updated.averageTemperature - m_cachedSummary.averageTemperature) > 0.05
                                || std::abs(updated.averageHealth - m_cachedSummary.averageHealth) > 0.5;

    m_cachedSummary = updated;

    if (changed || numbersChanged)
    {
        emit summaryChanged(m_cachedSummary);
    }
}

}   // namespace saim
