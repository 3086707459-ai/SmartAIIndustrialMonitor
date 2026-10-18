#include "device/Device.h"

#include "core/Logger.h"

#include <QJsonObject>

#include <algorithm>
#include <numeric>

namespace saim {

namespace {
constexpr std::size_t kDefaultHistoryCapacity = 600;   // 5 min at 2 Hz
}

Device::Device(QString deviceId, QString deviceName, QObject* parent)
    : QObject(parent)
    , m_deviceId(std::move(deviceId))
    , m_deviceName(std::move(deviceName))
    , m_history(kDefaultHistoryCapacity)
{
    m_latest.valid = false;
    if (m_deviceName.isEmpty())
    {
        m_deviceName = m_deviceId;
    }
    // Makes the name show up in logs and in QThread::currentThread()->objectName()
    // style diagnostics instead of "Device(0x...)".
    setObjectName(QStringLiteral("Device[%1]").arg(m_deviceId));
}

Device::~Device() = default;

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

void Device::setDeviceName(const QString& name)
{
    if (name.isEmpty() || m_deviceName == name)
    {
        return;
    }
    m_deviceName = name;
    emit metadataChanged();
}

void Device::setDeviceType(const QString& type)
{
    if (type.isEmpty() || m_deviceType == type)
    {
        return;
    }
    m_deviceType = type;
    emit metadataChanged();
}

void Device::setEndpoint(const QString& ip, quint16 port)
{
    if (m_ipAddress == ip && m_port == port)
    {
        return;
    }
    m_ipAddress = ip;
    m_port = port;
    emit metadataChanged();
}

void Device::setLimits(const DeviceLimits& limits)
{
    m_limits = limits;
    emit metadataChanged();
}

void Device::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
    {
        return;
    }
    m_enabled = enabled;
    if (!enabled)
    {
        m_state = DeviceState::Offline;
        emit statusChanged(m_deviceId, m_state);
    }
    emit metadataChanged();
}

void Device::setHistoryCapacity(std::size_t capacity)
{
    if (capacity == 0 || capacity == m_history.capacity())
    {
        return;
    }
    // Rebuild rather than resize in place: RingBuffer's capacity is fixed at
    // construction so that the hot path never reallocates.
    const std::vector<SensorSample> preserved = m_history.toVector();
    m_history = RingBuffer<SensorSample>(capacity);
    const std::size_t keep = std::min(preserved.size(), capacity);
    for (std::size_t i = preserved.size() - keep; i < preserved.size(); ++i)
    {
        m_history.push(preserved[i]);
    }
}

// ---------------------------------------------------------------------------
// Telemetry ingress
// ---------------------------------------------------------------------------

void Device::onSampleReceived(const SensorSample& sample)
{
    if (!m_enabled || !sample.isValid())
    {
        return;
    }
    if (!sample.deviceId.isEmpty() && sample.deviceId != m_deviceId)
    {
        // Wrong routing is a wiring bug, not a data problem - make it visible.
        SAIM_WARN(logcat::kDevice,
                  QStringLiteral("Sample for '%1' delivered to device '%2' - dropped")
                      .arg(sample.deviceId, m_deviceId));
        return;
    }

    m_latest = sample;
    if (!m_latest.timestamp.isValid())
    {
        m_latest.timestamp = QDateTime::currentDateTime();
    }
    m_lastSeen = QDateTime::currentDateTime();
    ++m_sampleCount;
    m_history.push(m_latest);

    emit dataUpdated(m_latest);

    reevaluateState(/*fromTelemetry=*/true);
}

void Device::onPredictionReady(const FaultPrediction& prediction)
{
    m_prediction = prediction;
    emit predictionUpdated(m_prediction);
    reevaluateState(/*fromTelemetry=*/false);
}

void Device::onAnomalyDetected(const AnomalyEvent& event)
{
    m_lastAnomaly = event;
    emit anomalyDetected(m_lastAnomaly);

    if (event.isSignificant())
    {
        const DeviceState implied = event.level >= AnomalyLevel::High ? DeviceState::Fault
                                                                      : DeviceState::Warning;
        raiseAlarm(implied,
                   QStringLiteral("Anomaly [%1] %2 (score %3)")
                       .arg(toString(event.level), event.reason)
                       .arg(event.score, 0, 'f', 2),
                   0);   // anomaly events are already rate-limited upstream
    }
}

void Device::checkLiveness(int timeoutMs)
{
    if (!m_enabled || m_state == DeviceState::Offline)
    {
        return;
    }
    if (!m_lastSeen.isValid())
    {
        return;   // never seen - remains Offline until the first frame
    }
    if (m_lastSeen.msecsTo(QDateTime::currentDateTime()) >= timeoutMs)
    {
        SAIM_WARN(logcat::kDevice,
                  QStringLiteral("Device %1 went offline (no data for %2 ms)")
                      .arg(m_deviceId)
                      .arg(timeoutMs));
        m_state = DeviceState::Offline;
        m_alarmLatched = false;
        emit statusChanged(m_deviceId, m_state);
        raiseAlarm(DeviceState::Offline,
                   QStringLiteral("Telemetry timeout - device unreachable"),
                   timeoutMs);
    }
}

void Device::acknowledgeAlarm()
{
    m_alarmLatched = false;
    SAIM_INFO(logcat::kDevice, QStringLiteral("Alarm acknowledged for %1").arg(m_deviceId));
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void Device::reevaluateState(bool fromTelemetry)
{
    DeviceState next = m_state;

    if (m_enabled && m_latest.isValid())
    {
        // Rule-based envelope first - deterministic, explainable, and it is what
        // a maintenance engineer will trust over a neural network.
        next = m_limits.evaluate(m_latest);

        // The AI verdict may only *escalate* severity, never de-escalate it.
        // Rationale: an operator must not be told a machine is healthy because a
        // model is uncertain; the physics rules remain the safety floor.
        const DeviceState aiState = m_prediction.impliedState();
        if (aiState > next)
        {
            next = aiState;
        }
        const DeviceState anomalyState = m_lastAnomaly.level >= AnomalyLevel::High
                                             ? DeviceState::Fault
                                             : (m_lastAnomaly.level >= AnomalyLevel::Medium
                                                    ? DeviceState::Warning
                                                    : DeviceState::Normal);
        if (anomalyState > next)
        {
            next = anomalyState;
        }
    }
    else if (m_enabled)
    {
        next = DeviceState::Offline;
    }

    if (next == m_state)
    {
        return;
    }

    const DeviceState previous = m_state;
    m_state = next;
    emit statusChanged(m_deviceId, m_state);

    // Escalation raises an alarm; recovery clears the latch so a future
    // escalation is reported again.
    //
    // `std::max` rather than `next > previous`, and it is not a cosmetic choice.
    // DeviceState is ordered Offline(0) < Normal(1) < Warning(2) < Fault(3) for
    // *display* sorting, so the raw comparison puts Normal above Offline - a
    // device that comes back online would then read as an escalation and raise
    // a spurious alarm at exactly the moment the operator is already relieved.
    // Online is a lateral move from Offline in severity terms, and this says so.
    if (next > std::max(previous, DeviceState::Normal) && next != DeviceState::Offline)
    {
        raiseAlarm(next, describeAnomaly(m_latest), 30000);
    }
    else if (next == DeviceState::Normal)
    {
        m_alarmLatched = false;
        SAIM_INFO(logcat::kDevice,
                  QStringLiteral("Device %1 recovered to NORMAL").arg(m_deviceId));
        if (fromTelemetry)
        {
            // Informational record so the log shows the recovery, without
            // incrementing the alarm counter.
            emit anomalyDetected(AnomalyEvent{m_deviceId, QDateTime::currentDateTime(),
                                              AnomalyLevel::None,
                                              QStringLiteral("Recovered to normal operating range"),
                                              {}, 0.0});
        }
    }
}

QString Device::describeAnomaly(const SensorSample& sample) const
{
    QStringList reasons;

    if (sample.temperature >= m_limits.temperatureHigh)
    {
        reasons << QStringLiteral("Temperature critical (%1 C >= %2 C)")
                       .arg(sample.temperature, 0, 'f', 1)
                       .arg(m_limits.temperatureHigh, 0, 'f', 1);
    }
    else if (sample.temperature >= m_limits.temperatureWarn)
    {
        reasons << QStringLiteral("Temperature abnormal increase (%1 C)")
                       .arg(sample.temperature, 0, 'f', 1);
    }

    if (sample.current >= m_limits.currentHigh)
    {
        reasons << QStringLiteral("Overcurrent (%1 A >= %2 A)")
                       .arg(sample.current, 0, 'f', 2)
                       .arg(m_limits.currentHigh, 0, 'f', 1);
    }
    else if (sample.current >= m_limits.currentWarn)
    {
        reasons << QStringLiteral("Current elevated (%1 A)").arg(sample.current, 0, 'f', 2);
    }

    if (sample.vibration >= m_limits.vibrationHigh)
    {
        reasons << QStringLiteral("Vibration critical (%1 mm/s >= %2)")
                       .arg(sample.vibration, 0, 'f', 3)
                       .arg(m_limits.vibrationHigh, 0, 'f', 2);
    }
    else if (sample.vibration >= m_limits.vibrationWarn)
    {
        reasons << QStringLiteral("Vibration elevated (%1 mm/s)").arg(sample.vibration, 0, 'f', 3);
    }

    if (sample.voltage <= m_limits.voltageWarnLow || sample.voltage >= m_limits.voltageWarnHigh)
    {
        reasons << QStringLiteral("Supply voltage out of range (%1 V)").arg(sample.voltage, 0, 'f', 1);
    }

    if (reasons.isEmpty())
    {
        return QStringLiteral("Health score degraded below threshold");
    }
    return reasons.join(QStringLiteral("; "));
}

void Device::raiseAlarm(DeviceState state, const QString& message, int suppressionMs)
{
    const QDateTime now = QDateTime::currentDateTime();

    // Repeat suppression: while a device stays in the same severity band, emit
    // at most one alarm per window.  Without this a device oscillating across
    // the threshold floods both the alarm table and the operator's screen.
    if (m_alarmLatched && state == m_lastAlarmedState
        && suppressionMs > 0 && m_lastAlarmTime.isValid()
        && m_lastAlarmTime.msecsTo(now) < suppressionMs)
    {
        return;
    }

    m_lastAlarmTime = now;
    m_lastAlarmedState = state;
    m_alarmLatched = true;
    ++m_alarmCount;

    AlarmRecord alarm;
    alarm.deviceId  = m_deviceId;
    alarm.timestamp = now;
    alarm.state     = state;
    alarm.message   = message;

    SAIM_WARN(logcat::kDevice,
              QStringLiteral("ALARM %1 [%2] %3").arg(m_deviceId, toString(state), message));

    emit alarmTriggered(alarm);
}

// ---------------------------------------------------------------------------
// History / statistics
// ---------------------------------------------------------------------------

std::vector<SensorSample> Device::history() const
{
    return m_history.toVector();
}

DeviceStatistics Device::statistics() const
{
    DeviceStatistics stats;
    stats.deviceId = m_deviceId;
    stats.sampleCount = static_cast<qint64>(m_history.size());
    stats.anomalyCount = m_alarmCount;

    const std::size_t count = m_history.size();
    if (count == 0)
    {
        return stats;
    }

    stats.from = m_history.oldest().timestamp;
    stats.to   = m_history.newest().timestamp;

    double temperatureSum = 0.0;
    double currentSum     = 0.0;
    qint64 normalCount    = 0;

    for (std::size_t i = 0; i < count; ++i)
    {
        const SensorSample& s = m_history.at(i);
        temperatureSum += s.temperature;
        currentSum     += s.current;
        stats.maxTemperature = std::max(stats.maxTemperature, s.temperature);
        stats.maxVibration   = std::max(stats.maxVibration, s.vibration);
        if (m_limits.evaluate(s) == DeviceState::Normal)
        {
            ++normalCount;
        }
    }

    const double n = static_cast<double>(count);
    stats.avgTemperature = temperatureSum / n;
    stats.avgCurrent     = currentSum / n;
    stats.uptimeRatio    = static_cast<double>(normalCount) / n;
    return stats;
}

QJsonObject Device::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("id"), m_deviceId);
    json.insert(QStringLiteral("name"), m_deviceName);
    json.insert(QStringLiteral("type"), m_deviceType);
    json.insert(QStringLiteral("ip"), m_ipAddress);
    json.insert(QStringLiteral("port"), static_cast<int>(m_port));
    json.insert(QStringLiteral("state"), toString(m_state));
    json.insert(QStringLiteral("enabled"), m_enabled);
    json.insert(QStringLiteral("sampleCount"), static_cast<double>(m_sampleCount));
    json.insert(QStringLiteral("alarmCount"), static_cast<double>(m_alarmCount));
    json.insert(QStringLiteral("lastSeen"), m_lastSeen.toString(Qt::ISODateWithMs));

    if (m_latest.isValid())
    {
        QJsonObject telemetry;
        telemetry.insert(QStringLiteral("temperature"), m_latest.temperature);
        telemetry.insert(QStringLiteral("voltage"), m_latest.voltage);
        telemetry.insert(QStringLiteral("current"), m_latest.current);
        telemetry.insert(QStringLiteral("vibration"), m_latest.vibration);
        telemetry.insert(QStringLiteral("rpm"), m_latest.rpm);
        json.insert(QStringLiteral("telemetry"), telemetry);
    }

    if (!m_prediction.faultType.isEmpty())
    {
        QJsonObject prediction;
        prediction.insert(QStringLiteral("health"), m_prediction.healthScore);
        prediction.insert(QStringLiteral("probability"), m_prediction.failureProbability);
        prediction.insert(QStringLiteral("fault"), m_prediction.faultType);
        prediction.insert(QStringLiteral("backend"), m_prediction.backend);
        json.insert(QStringLiteral("prediction"), prediction);
    }
    return json;
}

}   // namespace saim
