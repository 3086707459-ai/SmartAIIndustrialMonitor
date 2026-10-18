#include "device/DeviceSimulator.h"

#include "core/Logger.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <random>

namespace saim {

namespace {

/// Clamps into [lo, hi] - used so that no model term can run away and produce a
/// physically impossible value that the alarm rules would then trip on.
double clamp(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

}   // namespace

// ---------------------------------------------------------------------------
// DeviceProfile
// ---------------------------------------------------------------------------

DeviceProfile DeviceProfile::forType(const QString& type)
{
    DeviceProfile p;
    const QString lower = type.toLower();

    if (lower == QLatin1String("pump"))
    {
        p.nominalTemperature  = 42.0;
        p.nominalCurrent      = 6.5;
        p.nominalVibration    = 0.12;
        p.nominalRpm          = 2900.0;
        p.thermalTimeConstant = 18.0;
        p.vibrationHeatGain   = 15.0;
    }
    else if (lower == QLatin1String("spindle"))
    {
        p.nominalTemperature  = 52.0;
        p.nominalCurrent      = 11.0;
        p.nominalVibration    = 0.20;
        p.nominalRpm          = 8000.0;
        p.rpmNoise            = 25.0;
        p.thermalTimeConstant = 12.0;   // spindles heat fast
        p.vibrationHeatGain   = 9.0;
        p.wearRatePerStep     = 1.6e-4;
    }
    else if (lower == QLatin1String("fan"))
    {
        p.nominalTemperature  = 35.0;
        p.nominalCurrent      = 3.2;
        p.nominalVibration    = 0.09;
        p.nominalRpm          = 1200.0;
        p.thermalTimeConstant = 30.0;
    }
    // "motor" and anything unknown keep the defaults, which are motor-like.

    return p;
}

// ---------------------------------------------------------------------------
// DeviceSimulator
// ---------------------------------------------------------------------------

DeviceSimulator::DeviceSimulator(QString deviceId, quint32 seed)
    : m_deviceId(std::move(deviceId))
    , m_rng(seed != 0 ? seed : std::random_device{}())
{
}

double DeviceSimulator::gaussian(double sigma)
{
    // std::normal_distribution is not required to be reproducible across
    // standard-library implementations, but for a simulator that is fine: the
    // injected seed makes a run repeatable on one machine, which is what a bug
    // report actually needs.
    std::normal_distribution<double> distribution(0.0, sigma);
    return distribution(m_rng);
}

void DeviceSimulator::reset()
{
    m_temperature          = m_profile.nominalTemperature;
    m_current              = m_profile.nominalCurrent;
    m_vibration            = m_profile.nominalVibration;
    m_rpm                  = m_profile.nominalRpm;
    m_wear                 = 0.0;
    m_stepCount            = 0;
    m_transientSamplesLeft = 0;
    m_failureSamplesLeft   = 0;
    m_initialised          = true;
}

void DeviceSimulator::setWearLevel(double fraction)
{
    m_wear = clamp(fraction, 0.0, 1.0);
    SAIM_INFO(logcat::kDevice,
              QStringLiteral("Simulator %1 wear level set to %2%")
                  .arg(m_deviceId)
                  .arg(static_cast<int>(m_wear * 100.0)));
}

void DeviceSimulator::injectTransientFault(int samples, double magnitude)
{
    m_transientSamplesLeft = std::max(1, samples);
    m_transientMagnitude   = magnitude;
    SAIM_WARN(logcat::kDevice,
              QStringLiteral("Simulator %1 injecting transient fault for %2 samples")
                  .arg(m_deviceId)
                  .arg(samples));
}

void DeviceSimulator::injectFailure(int samples)
{
    m_failureSamplesLeft = std::max(1, samples);
    SAIM_WARN(logcat::kDevice,
              QStringLiteral("Simulator %1 entering sustained failure mode for %2 samples")
                  .arg(m_deviceId)
                  .arg(samples));
}

SensorSample DeviceSimulator::step()
{
    if (!m_initialised)
    {
        reset();
    }

    ++m_stepCount;

    // --- wear model --------------------------------------------------------
    // Bearing wear accumulates monotonically rather than randomly: it is this
    // slow drift that makes a trend-based model able to see a failure coming.
    if (m_wear < 1.0)
    {
        m_wear = clamp(m_wear + m_profile.wearRatePerStep, 0.0, 1.0);
    }
    // A worn bearing vibrates super-linearly - it gets worse fast near end of life.
    const double wearVibration = m_profile.nominalVibration * (1.0 + 6.0 * m_wear * m_wear);

    // --- vibration ---------------------------------------------------------
    m_vibration = clamp(wearVibration + gaussian(m_profile.vibrationNoise), 0.01, 5.0);

    // --- current -----------------------------------------------------------
    // Load rises with the mechanical resistance that the wear introduces.
    m_current = clamp(m_profile.nominalCurrent * (1.0 + 0.35 * m_wear)
                          + gaussian(m_profile.currentNoise),
                      0.1, 60.0);

    // --- temperature: first-order lag toward a load-dependent equilibrium ---
    //   T_eq = T_nominal + k*(I - I_nominal) + heat(vibration) + wear offset
    //   T   += (T_eq - T) / tau
    // The lag is what makes temperature a *lagging* indicator - physically
    // correct, and it is why the model must look at a window, not one sample.
    const double heatFromVibration = std::max(0.0, m_vibration - m_profile.nominalVibration)
                                     * m_profile.vibrationHeatGain;
    const double equilibrium = m_profile.nominalTemperature
                               + 1.8 * (m_current - m_profile.nominalCurrent)
                               + heatFromVibration
                               + 25.0 * m_wear;
    m_temperature += (equilibrium - m_temperature) / m_profile.thermalTimeConstant;
    m_temperature = clamp(m_temperature + gaussian(m_profile.temperatureNoise), -20.0, 200.0);

    // --- rpm: droops slightly under load -----------------------------------
    m_rpm = clamp(m_profile.nominalRpm * (1.0 - 0.05 * m_wear) + gaussian(m_profile.rpmNoise),
                  0.0, 30000.0);

    // --- voltage -----------------------------------------------------------
    const double voltage = clamp(m_profile.nominalVoltage + gaussian(m_profile.voltageNoise),
                                 150.0, 300.0);

    // --- transient spike ---------------------------------------------------
    if (m_transientSamplesLeft > 0)
    {
        --m_transientSamplesLeft;
        // A spike is a fast excursion, not a new equilibrium: vibration and
        // current take the brunt, temperature only gets a modest bump.
        m_vibration *= m_transientMagnitude;
        m_current   *= (1.0 + (m_transientMagnitude - 1.0) * 0.5);
        m_temperature += 8.0 * (m_transientMagnitude - 1.0);
    }

    // --- sustained failure -------------------------------------------------
    if (m_failureSamplesLeft > 0)
    {
        --m_failureSamplesLeft;
        m_temperature = std::max(m_temperature, 92.0 + gaussian(1.5));
        m_current     = std::max(m_current, 22.0 + gaussian(0.8));
        m_vibration   = std::max(m_vibration, 0.95 + gaussian(0.05));
    }

    SensorSample sample;
    sample.deviceId    = m_deviceId;
    sample.timestamp   = QDateTime::currentDateTime();
    sample.temperature = std::round(m_temperature * 100.0) / 100.0;
    sample.voltage     = std::round(voltage * 100.0) / 100.0;
    sample.current     = std::round(m_current * 100.0) / 100.0;
    sample.vibration   = std::round(m_vibration * 1000.0) / 1000.0;
    sample.rpm         = std::round(m_rpm * 10.0) / 10.0;
    sample.valid       = true;
    return sample;
}

}   // namespace saim
