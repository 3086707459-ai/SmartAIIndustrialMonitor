#pragma once

// ---------------------------------------------------------------------------
// DeviceSimulator.h - physics-plausible industrial device emulator.
//
// The simulator is a first-class part of the deliverable, not a throwaway mock:
// without hardware, it is the only way a reviewer can reproduce the AI path.
// It therefore models more than "random numbers in range":
//
//   * A slow thermal mass so temperature follows current with a time constant -
//     so the trend features the model consumes are actually meaningful.
//   * Bearing-wear degradation that accumulates, makes vibration and temperature
//     drift upward over minutes, and stays until reset.  This is what produces a
//     genuine (not injected) progression toward a Fault state.
//   * Optional stochastic anomaly injection (a spike lasting a few samples).
//
// Everything is driven by an injectable seed, so a failing scenario found in
// testing can be replayed exactly - a property worth mentioning in an interview.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QString>

#include <random>

namespace saim {

/// Per-device operating profile.  A pump and a spindle do not share a duty
/// cycle, so the simulator is parameterised rather than hard-coded.
struct DeviceProfile
{
    double nominalTemperature = 45.0;
    double nominalVoltage     = 220.0;
    double nominalCurrent     = 8.0;
    double nominalVibration   = 0.15;
    double nominalRpm         = 1450.0;

    // Noise magnitudes (standard deviation).
    double temperatureNoise = 0.6;
    double voltageNoise     = 1.5;
    double currentNoise     = 0.35;
    double vibrationNoise   = 0.02;
    double rpmNoise         = 8.0;

    // Coupling: how much extra vibration a worn bearing produces per step, and
    // how much of the vibration energy is dissipated as heat.
    double wearRatePerStep     = 1.0e-4;
    double vibrationHeatGain   = 12.0;
    double thermalTimeConstant = 25.0;   ///< samples to reach ~63% of a step change

    /// Reasonable defaults for the machine types named in config.json.
    [[nodiscard]] static DeviceProfile forType(const QString& type);
};

class DeviceSimulator
{
public:
    explicit DeviceSimulator(QString deviceId, quint32 seed = 0);

    [[nodiscard]] const QString& deviceId() const noexcept { return m_deviceId; }

    void setProfile(const DeviceProfile& profile) { m_profile = profile; }
    [[nodiscard]] const DeviceProfile& profile() const noexcept { return m_profile; }

    /// Advances the model by one sampling interval and returns the new state.
    [[nodiscard]] SensorSample step();

    /// Progresses the simulated bearing wear by `fraction` of its full range.
    /// 0 = healthy, 1 = end of life.  Exposed so the UI can drive a "degrade
    /// this asset" action and a reviewer can watch the AI react on demand.
    void setWearLevel(double fraction);
    [[nodiscard]] double wearLevel() const noexcept { return m_wear; }

    /// Forces a transient fault lasting `samples` steps (a spike, not wear).
    void injectTransientFault(int samples, double magnitude = 3.0);

    /// Forces the next `samples` steps to report a plausible failure state.
    void injectFailure(int samples);

    void reset();

    [[nodiscard]] quint64 stepCount() const noexcept { return m_stepCount; }

private:
    [[nodiscard]] double gaussian(double sigma);

    QString        m_deviceId;
    DeviceProfile  m_profile;
    std::mt19937   m_rng;

    // Model state.  Zero-initialised here so that a default-constructed
    // simulator is in a well-defined state even before reset() runs.
    double  m_temperature = 0.0;
    double  m_current     = 0.0;
    double  m_vibration   = 0.0;
    double  m_rpm         = 0.0;
    double  m_wear        = 0.0;   ///< 0..1
    quint64 m_stepCount   = 0;

    int     m_transientSamplesLeft = 0;
    double  m_transientMagnitude   = 0.0;
    int     m_failureSamplesLeft   = 0;
    bool    m_initialised          = false;
};

}   // namespace saim
