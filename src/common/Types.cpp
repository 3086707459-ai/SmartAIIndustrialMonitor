#include "common/Types.h"

#include "common/ErrorCode.h"

#include <QMetaType>

namespace saim {

// ---------------------------------------------------------------------------
// ErrorCode names
// ---------------------------------------------------------------------------

const char* toString(ErrorCode code)
{
    // A switch rather than a table so that adding a code without a name is a
    // compiler warning (-Wswitch) instead of a silent "Unknown" at run time.
    switch (code)
    {
    case ErrorCode::Ok:                    return "Ok";
    case ErrorCode::Unknown:               return "Unknown";
    case ErrorCode::InvalidArgument:       return "InvalidArgument";
    case ErrorCode::NotInitialised:        return "NotInitialised";
    case ErrorCode::NotSupported:          return "NotSupported";
    case ErrorCode::Timeout:               return "Timeout";
    case ErrorCode::IoError:               return "IoError";
    case ErrorCode::FileNotFound:          return "FileNotFound";
    case ErrorCode::PermissionDenied:      return "PermissionDenied";
    case ErrorCode::DatabaseOpenFailed:    return "DatabaseOpenFailed";
    case ErrorCode::DatabaseQueryFailed:   return "DatabaseQueryFailed";
    case ErrorCode::DatabaseMigrationFailed: return "DatabaseMigrationFailed";
    case ErrorCode::NetworkUnreachable:    return "NetworkUnreachable";
    case ErrorCode::ConnectionRefused:     return "ConnectionRefused";
    case ErrorCode::ConnectionLost:        return "ConnectionLost";
    case ErrorCode::ProtocolViolation:     return "ProtocolViolation";
    case ErrorCode::ModelNotFound:         return "ModelNotFound";
    case ErrorCode::ModelLoadFailed:       return "ModelLoadFailed";
    case ErrorCode::ModelInferenceFailed:  return "ModelInferenceFailed";
    case ErrorCode::ModelShapeMismatch:    return "ModelShapeMismatch";
    case ErrorCode::InsufficientData:      return "InsufficientData";
    case ErrorCode::BackendUnavailable:    return "BackendUnavailable";
    }
    return "Unrecognised";
}

// ---------------------------------------------------------------------------
// Enumeration names
// ---------------------------------------------------------------------------

QString toString(DeviceState state)
{
    switch (state)
    {
    case DeviceState::Offline: return QStringLiteral("OFFLINE");
    case DeviceState::Normal:  return QStringLiteral("NORMAL");
    case DeviceState::Warning: return QStringLiteral("WARNING");
    case DeviceState::Fault:   return QStringLiteral("FAULT");
    }
    return QStringLiteral("UNKNOWN");
}

QString toString(AnomalyLevel level)
{
    switch (level)
    {
    case AnomalyLevel::None:   return QStringLiteral("NONE");
    case AnomalyLevel::Low:    return QStringLiteral("LOW");
    case AnomalyLevel::Medium: return QStringLiteral("MEDIUM");
    case AnomalyLevel::High:   return QStringLiteral("HIGH");
    }
    return QStringLiteral("UNKNOWN");
}

QString toString(ConnectionState state)
{
    switch (state)
    {
    case ConnectionState::Disconnected: return QStringLiteral("DISCONNECTED");
    case ConnectionState::Connecting:   return QStringLiteral("CONNECTING");
    case ConnectionState::Connected:    return QStringLiteral("CONNECTED");
    case ConnectionState::Reconnecting: return QStringLiteral("RECONNECTING");
    case ConnectionState::Error:        return QStringLiteral("ERROR");
    }
    return QStringLiteral("UNKNOWN");
}

// ---------------------------------------------------------------------------
// HMI colours
//
// These are the conventional meanings on a plant floor, and they are *not*
// interchangeable with pretty palette choices:
//
//   green  = running normally
//   amber  = attention, schedule maintenance
//   red    = fault, stop the line
//   grey   = no information (offline) - deliberately NOT red, because "I cannot
//            see this machine" and "this machine has failed" call for different
//            responses, and colouring both red trains operators to ignore red.
//
// The hex values are tuned for a dark background (high value, moderate
// saturation) rather than taken from a light-theme palette, where the same hues
// read as muddy.
// ---------------------------------------------------------------------------

QString stateColor(DeviceState state)
{
    switch (state)
    {
    case DeviceState::Offline: return QStringLiteral("#6b7684");
    case DeviceState::Normal:  return QStringLiteral("#3fa46a");
    case DeviceState::Warning: return QStringLiteral("#d9a13c");
    case DeviceState::Fault:   return QStringLiteral("#d64545");
    }
    return QStringLiteral("#6b7684");
}

QString levelColor(AnomalyLevel level)
{
    switch (level)
    {
    case AnomalyLevel::None:   return QStringLiteral("#3fa46a");
    case AnomalyLevel::Low:    return QStringLiteral("#5b9bd5");
    case AnomalyLevel::Medium: return QStringLiteral("#d9a13c");
    case AnomalyLevel::High:   return QStringLiteral("#d64545");
    }
    return QStringLiteral("#6b7684");
}

// ---------------------------------------------------------------------------
// DeviceLimits
// ---------------------------------------------------------------------------

DeviceState DeviceLimits::evaluate(const SensorSample& s) const noexcept
{
    // Trip limits first: a single high reading is a Fault regardless of anything
    // else, and checking it first means the worst case is never masked by a
    // warning-level finding on a different channel.
    if (s.temperature >= temperatureHigh || s.vibration >= vibrationHigh
        || s.current >= currentHigh)
    {
        return DeviceState::Fault;
    }

    if (s.temperature >= temperatureWarn || s.vibration >= vibrationWarn
        || s.current >= currentWarn || s.voltage <= voltageWarnLow
        || s.voltage >= voltageWarnHigh)
    {
        return DeviceState::Warning;
    }

    return DeviceState::Normal;
}

// ---------------------------------------------------------------------------
// FaultPrediction
// ---------------------------------------------------------------------------

DeviceState FaultPrediction::impliedState() const noexcept
{
    // An empty faultType means "no verdict yet" - the window was too short.  That
    // is not a healthy verdict. Offline here means no information; Device uses
    // the worse online severity and will not downgrade live telemetry.
    if (faultType.isEmpty())
    {
        return DeviceState::Offline;
    }

    // The AI may escalate but never de-escalate: a model that is 60 % sure the
    // machine is fine does not overrule the physical envelope.  Device::
    // reevaluateState() takes the maximum of this and the rule-based verdict, so
    // returning Normal here is a floor, not an override.
    if (failureProbability >= 0.85 && healthScore < 40.0)
    {
        return DeviceState::Fault;
    }
    if (failureProbability >= 0.55 || healthScore < 65.0)
    {
        return DeviceState::Warning;
    }
    return DeviceState::Normal;
}

}   // namespace saim
