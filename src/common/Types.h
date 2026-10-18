#pragma once

// ---------------------------------------------------------------------------
// Types.h - domain-wide value types and enumerations.
//
// This header deliberately depends on nothing but Qt Core.  Every layer
// (device, network, database, ai, ui) speaks these types, which is what keeps
// the layers decoupled: the storage layer never sees a QWidget, and the UI
// never sees a QTcpSocket.
// ---------------------------------------------------------------------------

#include <QDateTime>
#include <QMetaType>
#include <QRectF>
#include <QString>
#include <QVector>

#include <array>

namespace saim {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Health state of a single device.  Ordered by severity so that comparisons
/// such as `state >= DeviceState::Warning` are meaningful when aggregating.
enum class DeviceState : int
{
    Offline = 0,   ///< no telemetry received within the offline timeout
    Normal = 1,    ///< all monitored quantities inside their envelopes
    Warning = 2,   ///< degraded - maintenance should be scheduled
    Fault = 3      ///< one or more quantities out of tolerance, stop the line
};

/// Severity of a single anomaly event.
enum class AnomalyLevel : int
{
    None = 0,
    Low = 1,
    Medium = 2,
    High = 3
};

/// Connection state of a transport (client side).
enum class ConnectionState : int
{
    Disconnected = 0,
    Connecting,
    Connected,
    Reconnecting,
    Error
};

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

[[nodiscard]] QString toString(DeviceState state);
[[nodiscard]] QString toString(AnomalyLevel level);
[[nodiscard]] QString toString(ConnectionState state);

/// Maps a state onto the industrial HMI colour convention
/// (green / amber / red / grey) as a "#RRGGBB" string.
[[nodiscard]] QString stateColor(DeviceState state);
[[nodiscard]] QString levelColor(AnomalyLevel level);

// ---------------------------------------------------------------------------
// SensorSample - one telemetry frame from one device.
//
// POD-like and cheap to copy; it is passed across thread boundaries through
// queued signal/slot connections thousands of times per second, which is why
// it holds no dynamic resources beyond the implicitly shared QString id.
// ---------------------------------------------------------------------------
struct SensorSample
{
    QString   deviceId;
    QDateTime timestamp;

    double temperature = 0.0;   ///< degrees Celsius
    double voltage     = 0.0;   ///< volts
    double current     = 0.0;   ///< amperes
    double vibration   = 0.0;   ///< mm/s RMS
    double rpm         = 0.0;   ///< revolutions per minute

    /// False when the frame arrived corrupted or the device reported a fault.
    bool valid = true;

    [[nodiscard]] bool isValid() const noexcept { return valid && !deviceId.isEmpty(); }

    /// The four monitored quantities in a fixed order; the order is part of the
    /// contract with the AI models and must not change without retraining.
    [[nodiscard]] std::array<double, 5> toFeatureVector() const noexcept
    {
        return {temperature, voltage, current, vibration, rpm};
    }
};

/// Names of the features in `SensorSample::toFeatureVector()` order.
inline constexpr std::array<const char*, 5> kFeatureNames{
    "temperature", "voltage", "current", "vibration", "rpm"};
inline constexpr int kFeatureCount = 5;

// ---------------------------------------------------------------------------
// DeviceLimits - the tolerance envelope used for rule-based alarming.
//
// In a real plant these come from the device datasheet / maintenance manual.
// They are configurable per device so that the platform is not hard-coded to
// one machine type.
// ---------------------------------------------------------------------------
struct DeviceLimits
{
    double temperatureWarn = 70.0;
    double temperatureHigh = 85.0;
    double voltageWarnLow  = 200.0;
    double voltageWarnHigh = 240.0;
    double currentWarn     = 15.0;
    double currentHigh     = 20.0;
    double vibrationWarn   = 0.45;
    double vibrationHigh   = 0.80;

    /// Returns the worst state implied by a single sample.
    [[nodiscard]] DeviceState evaluate(const SensorSample& s) const noexcept;
};

// ---------------------------------------------------------------------------
// FaultPrediction - output of FaultPredictor.
// ---------------------------------------------------------------------------
/// Value-only device configuration crossing worker boundaries.
struct DeviceConfiguration
{
    QString id;
    QString name;
    QString type;
    QString ip;
    quint16 port = 0;
    bool enabled = true;
    DeviceLimits limits;
};

struct FaultPrediction
{
    QString   deviceId;
    QDateTime timestamp;

    double healthScore        = 100.0;  ///< 0..100, higher is healthier
    double failureProbability = 0.0;    ///< 0..1
    QString faultType;                  ///< e.g. "Bearing Failure", "None"
    double confidence         = 0.0;    ///< 0..1, model confidence

    /// Which backend produced this result - surfaced in the UI so a reviewer can
    /// tell whether ONNX Runtime or the built-in fallback answered.
    QString backend;                    ///< "onnxruntime" | "heuristic"

    [[nodiscard]] DeviceState impliedState() const noexcept;
};

// ---------------------------------------------------------------------------
// AnomalyEvent - output of AnomalyDetector.
// ---------------------------------------------------------------------------
struct AnomalyEvent
{
    QString      deviceId;
    QDateTime    timestamp;
    AnomalyLevel level = AnomalyLevel::None;
    QString      reason;                    ///< human readable, e.g. "Temperature abnormal increase"
    QVector<int> offendingFeatures;         ///< indices into kFeatureNames
    double       score = 0.0;               ///< normalised 0..1 anomaly score

    [[nodiscard]] bool isSignificant() const noexcept
    {
        return level >= AnomalyLevel::Medium;
    }
};

// ---------------------------------------------------------------------------
// Detection - one bounding box produced by VisionDetector.
// ---------------------------------------------------------------------------
struct Detection
{
    QRectF  box;                        ///< normalised [0,1] coordinates
    int     classId   = -1;
    QString className;
    float   confidence = 0.0f;
};

// ---------------------------------------------------------------------------
// AlarmRecord - a persisted alarm row.
// ---------------------------------------------------------------------------
struct AlarmRecord
{
    qint64       id = -1;
    QString      deviceId;
    QDateTime    timestamp;
    DeviceState  state = DeviceState::Normal;
    QString      message;
    bool         acknowledged = false;
};

// ---------------------------------------------------------------------------
// Aggregate statistics for the AI report / dashboard header.
// ---------------------------------------------------------------------------
struct DeviceStatistics
{
    QString   deviceId;
    QDateTime from;
    QDateTime to;

    qint64 sampleCount    = 0;
    qint64 anomalyCount   = 0;
    double maxTemperature = 0.0;
    double avgTemperature = 0.0;
    double maxVibration   = 0.0;
    double avgCurrent     = 0.0;
    double uptimeRatio    = 0.0;   ///< fraction of the window spent in Normal
};

}   // namespace saim

// ---------------------------------------------------------------------------
// Qt metatype registration.
//
// Without these, `QObject::connect` with Qt::QueuedConnection cannot marshal the
// payload across threads and fails silently at run time with
// "Cannot queue arguments of type ...".  Registering here means every
// translation unit that includes Types.h gets correct cross-thread behaviour.
// ---------------------------------------------------------------------------
Q_DECLARE_METATYPE(saim::SensorSample)
Q_DECLARE_METATYPE(saim::FaultPrediction)
Q_DECLARE_METATYPE(saim::AnomalyEvent)
Q_DECLARE_METATYPE(saim::Detection)
Q_DECLARE_METATYPE(QVector<saim::Detection>)
Q_DECLARE_METATYPE(saim::DeviceState)
Q_DECLARE_METATYPE(saim::AnomalyLevel)
Q_DECLARE_METATYPE(saim::ConnectionState)
Q_DECLARE_METATYPE(saim::AlarmRecord)
Q_DECLARE_METATYPE(saim::DeviceStatistics)
