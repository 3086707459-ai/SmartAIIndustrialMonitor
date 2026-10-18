#include "ai/FaultPredictor.h"

#include "core/Logger.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>

namespace saim {

namespace {

/// Logistic squash - maps any real number onto (0, 1) without a discontinuity.
double sigmoid(double x)
{
    return 1.0 / (1.0 + std::exp(-x));
}

/// Clamps into [lo, hi].
double clamp(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

/// Maps a raw reading onto a 0..1 severity, where `warn` scores 0.5 and `high`
/// scores 1.0.  Values below `warn` fall linearly toward 0 as they approach a
/// nominal "healthy" reading, which is what makes a completely normal machine
/// score near zero rather than sitting at 0.5.
double severityScore(double value, double nominal, double warn, double high)
{
    if (value <= nominal)
    {
        return 0.0;
    }
    if (value <= warn)
    {
        const double span = std::max(1e-9, warn - nominal);
        return 0.5 * (value - nominal) / span;
    }
    if (high <= warn)
    {
        return 1.0;
    }
    return clamp(0.5 + 0.5 * (value - warn) / (high - warn), 0.0, 1.0);
}

/// Least-squares trend of a feature across the window, expressed per minute.
/// The slope is computed by FeatureExtractor; this only rescales it and clamps
/// so that a pathological window cannot push a score outside [0, 1].
double trendPerMinute(double slopePerSecond)
{
    return clamp(slopePerSecond * 60.0, -10.0, 10.0);
}

}   // namespace

// ---------------------------------------------------------------------------
// FaultClass helpers
// ---------------------------------------------------------------------------

QString toString(FaultClass faultClass)
{
    switch (faultClass)
    {
    case FaultClass::Healthy:         return QStringLiteral("None");
    case FaultClass::BearingFailure:  return QStringLiteral("Bearing Failure");
    case FaultClass::Overheating:     return QStringLiteral("Overheating");
    case FaultClass::ElectricalFault: return QStringLiteral("Electrical Fault");
    case FaultClass::Imbalance:       return QStringLiteral("Mechanical Imbalance");
    case FaultClass::LubricationLoss: return QStringLiteral("Lubrication Loss");
    case FaultClass::Count:           break;
    }
    return QStringLiteral("Unknown");
}

QString recommendedAction(FaultClass faultClass)
{
    switch (faultClass)
    {
    case FaultClass::Healthy:
        return QStringLiteral("No action required. Continue normal operation and scheduled "
                              "condition monitoring.");
    case FaultClass::BearingFailure:
        return QStringLiteral("Reduce load and schedule bearing replacement. Inspect shaft "
                              "alignment and re-grease before returning to full duty.");
    case FaultClass::Overheating:
        return QStringLiteral("Improve cooling: check the fan, clean the heat exchanger and verify "
                              "the ambient temperature. Reduce the duty cycle until the "
                              "temperature stabilises below the warning limit.");
    case FaultClass::ElectricalFault:
        return QStringLiteral("Inspect the supply and the winding insulation. Check terminal "
                              "torque and look for phase imbalance; an insulation test is "
                              "advisable before the next start.");
    case FaultClass::Imbalance:
        return QStringLiteral("Balance the rotating assembly and check the mounting bolts. Inspect "
                              "the coupling for wear or misalignment.");
    case FaultClass::LubricationLoss:
        return QStringLiteral("Top up or replace the lubricant and inspect for leakage. Rising "
                              "vibration together with rising current usually indicates "
                              "dry-running.");
    case FaultClass::Count:
        break;
    }
    return QStringLiteral("Inspect the asset and consult the maintenance manual.");
}

FaultClass faultClassFromString(const QString& text)
{
    // Compared case-insensitively against the exact strings toString() emits, so
    // the two functions form a matched pair.  Iterating rather than a static hash
    // keeps the mapping in one visible place: adding a class means adding its
    // toString() case and nothing else.
    const int count = static_cast<int>(FaultClass::Count);
    for (int index = 0; index < count; ++index)
    {
        const auto candidate = static_cast<FaultClass>(index);
        if (toString(candidate).compare(text, Qt::CaseInsensitive) == 0)
        {
            return candidate;
        }
    }
    return FaultClass::Healthy;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FaultPredictor::FaultPredictor(std::unique_ptr<IInferenceEngine> engine)
    : FaultPredictor(std::move(engine), Options{})
{
}

FaultPredictor::FaultPredictor(std::unique_ptr<IInferenceEngine> engine, Options options)
    : m_engine(std::move(engine))
    , m_extractor(options.minimumWindowSize > 0 ? std::max(10, options.minimumWindowSize) : 60)
    , m_options(options)
{
    if (hasNeuralModel())
    {
        SAIM_INFO(logcat::kAi,
                  QStringLiteral("FaultPredictor: %1 backend, model %2")
                      .arg(QString::fromLatin1(activeBackend()),
                           m_engine->metadata().filePath));
    }
    else
    {
        SAIM_INFO(logcat::kAi,
                  QStringLiteral("FaultPredictor: built-in physics model "
                                 "(no neural network available)"));
    }
}

FaultPredictor::~FaultPredictor() = default;

const char* FaultPredictor::activeBackend() const noexcept
{
    return hasNeuralModel() ? m_engine->backendName() : "heuristic";
}

const ModelMetadata& FaultPredictor::modelMetadata() const
{
    // A function-local static keeps the "no model" case returning a valid
    // reference rather than forcing every caller to check for null.
    static const ModelMetadata kEmpty;
    return hasNeuralModel() ? m_engine->metadata() : kEmpty;
}

void FaultPredictor::warmUp()
{
    if (hasNeuralModel())
    {
        m_engine->warmUp();
    }
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

FaultPrediction FaultPredictor::predict(const QString& deviceId,
                                        const std::vector<SensorSample>& window,
                                        const DeviceLimits& limits) const
{
    FaultPrediction prediction;
    prediction.deviceId  = deviceId;
    prediction.timestamp = QDateTime::currentDateTime();
    prediction.backend   = QString::fromLatin1(activeBackend());

    FeatureVector features;
    if (!m_extractor.extract(window, features))
    {
        // Not enough history yet.  faultType stays empty on purpose so the UI can
        // distinguish "no verdict" from "verdict: healthy" - showing a green
        // 100 % for a device that just powered on would be false reassurance.
        SAIM_TRACE(logcat::kAi,
                   QStringLiteral("No prediction for %1: window has %2 sample(s)")
                       .arg(deviceId)
                       .arg(window.size()));
        return prediction;
    }

    if (hasNeuralModel())
    {
        // Build the standardised float input the model was trained on.  The
        // standardisation constants must match scripts/train_fault_model.py; see
        // FeatureExtractor::standardise for why that is not a detail.
        std::vector<float> input(features.values.begin(), features.values.end());
        FeatureExtractor::standardise(input);

        const Result<std::vector<float>> output = m_engine->run(input);
        if (output.isOk())
        {
            prediction = interpretNeuralOutput(deviceId, output.value(), features, limits);
        }
        else
        {
            // A run-time failure (shape mismatch, allocation failure) degrades to
            // the fallback rather than surfacing an error to the operator every
            // 500 ms.  The backend field records that it happened.
            SAIM_WARN(logcat::kAi,
                      QStringLiteral("Neural inference failed for %1 (%2) - falling back "
                                     "for this cycle")
                          .arg(deviceId, output.error().message));
            prediction = predictHeuristic(deviceId, features, window, limits);
            prediction.backend = QStringLiteral("heuristic-fallback");
        }
    }
    else
    {
        prediction = predictHeuristic(deviceId, features, window, limits);
    }

    // Publish the explanation for the assistant.
    Explanation explanation;
    explanation.deviceId = deviceId;
    explanation.features = features;
    for (int i = 0; i < static_cast<int>(FaultClass::Count); ++i)
    {
        if (toString(static_cast<FaultClass>(i)) == prediction.faultType)
        {
            explanation.faultClass = static_cast<FaultClass>(i);
            break;
        }
    }
    explanation.dominantFactors = explainFactors(features, limits, explanation.faultClass);

    // shared_ptr swap: a reader either sees the previous object or the new one,
    // never a torn one, so the assistant's read needs no lock.
    m_lastExplanation = std::make_shared<const Explanation>(std::move(explanation));

    return prediction;
}

FaultPredictor::Explanation FaultPredictor::lastExplanation() const
{
    const std::shared_ptr<const Explanation> snapshot = m_lastExplanation;
    return snapshot != nullptr ? *snapshot : Explanation{};
}

// ---------------------------------------------------------------------------
// Neural output interpretation
// ---------------------------------------------------------------------------

FaultPrediction FaultPredictor::interpretNeuralOutput(const QString& deviceId,
                                                       const std::vector<float>& output,
                                                       const FeatureVector& features,
                                                       const DeviceLimits& limits) const
{
    FaultPrediction prediction;
    prediction.deviceId  = deviceId;
    prediction.timestamp = QDateTime::currentDateTime();
    prediction.backend   = QString::fromLatin1(activeBackend());

    // The exported model emits [health, failureProbability, classLogit0..N].
    // The layout is asserted by the export script and covered by a unit test,
    // because a silent layout change here would invert every verdict.
    constexpr std::size_t kMinimumOutputs = 2;
    if (output.size() < kMinimumOutputs)
    {
        SAIM_WARN(logcat::kAi,
                  QStringLiteral("Model returned %1 value(s), expected at least %2")
                      .arg(output.size())
                      .arg(kMinimumOutputs));
        return predictHeuristic(deviceId, features, {}, limits);
    }

    prediction.healthScore        = clamp(static_cast<double>(output[0]), 0.0, 100.0);
    prediction.failureProbability = clamp(static_cast<double>(output[1]), 0.0, 1.0);

    // Classification head: a softmax over the remaining logits.  Doing the
    // softmax here rather than inside the graph keeps the ONNX export at a lower
    // opset and makes the class probabilities inspectable in a debugger - worth
    // more than the microseconds it would save.
    const std::size_t classCount = std::min(output.size() - 2,
                                             static_cast<std::size_t>(FaultClass::Count));
    if (classCount > 0)
    {
        const auto logitsBegin = output.begin() + 2;
        const auto logitsEnd = logitsBegin + static_cast<std::ptrdiff_t>(classCount);

        // Subtract the maximum before exponentiating: the standard overflow guard.
        const double maximum = static_cast<double>(*std::max_element(logitsBegin, logitsEnd));
        std::vector<double> exponentials;
        exponentials.reserve(classCount);
        double sum = 0.0;
        for (auto it = logitsBegin; it != logitsEnd; ++it)
        {
            const double value = std::exp(static_cast<double>(*it) - maximum);
            exponentials.push_back(value);
            sum += value;
        }

        std::size_t bestIndex = 0;
        double bestProbability = 0.0;
        for (std::size_t i = 0; i < exponentials.size(); ++i)
        {
            const double probability = sum > 0.0 ? exponentials[i] / sum : 0.0;
            if (probability > bestProbability)
            {
                bestProbability = probability;
                bestIndex = i;
            }
        }

        prediction.confidence = bestProbability;
        prediction.faultType = toString(static_cast<FaultClass>(bestIndex));
    }
    else
    {
        // Regression-only model: derive the class from the features so the UI
        // still shows something actionable.
        prediction.confidence = 0.5;
        prediction.faultType = predictHeuristic(deviceId, features, {}, limits).faultType;
    }

    // If the model is confident the asset is healthy, do not report a fault type
    // from a low-confidence classification - that is how a dashboard ends up
    // showing "Bearing Failure" on a machine that is fine.
    if (prediction.failureProbability < 0.35 && prediction.healthScore > 75.0
        && prediction.confidence < 0.70)
    {
        prediction.faultType = toString(FaultClass::Healthy);
    }

    return prediction;
}

// ---------------------------------------------------------------------------
// Physics-informed fallback
// ---------------------------------------------------------------------------

FaultPrediction FaultPredictor::predictHeuristic(const QString& deviceId,
                                                  const FeatureVector& features,
                                                  const std::vector<SensorSample>& window,
                                                  const DeviceLimits& limits) const
{
    FaultPrediction prediction;
    prediction.deviceId  = deviceId;
    prediction.timestamp = QDateTime::currentDateTime();
    prediction.backend   = QStringLiteral("heuristic");

    const FeatureStatistics& temperature = features.statistics[0];
    const FeatureStatistics& voltage     = features.statistics[1];
    const FeatureStatistics& current     = features.statistics[2];
    const FeatureStatistics& vibration   = features.statistics[3];
    const FeatureStatistics& rpm         = features.statistics[4];

    // Nominal references.  A device's own nominal operating point is not in its
    // DeviceLimits (those are envelopes), so the window mean of the *healthy*
    // portion is used as the reference - approximated here by the minimum
    // observed value, which is the closest thing to "the machine at rest" the
    // window contains.
    const double nominalTemperature = std::min(40.0, temperature.minimum);
    const double nominalCurrent     = std::min(6.0, current.minimum);
    const double nominalVibration   = std::min(0.12, vibration.minimum);

    const double temperatureSeverity = severityScore(temperature.last, nominalTemperature,
                                                      limits.temperatureWarn, limits.temperatureHigh);
    const double currentSeverity = severityScore(current.last, nominalCurrent,
                                                  limits.currentWarn, limits.currentHigh);
    const double vibrationSeverity = severityScore(vibration.last, nominalVibration,
                                                    limits.vibrationWarn, limits.vibrationHigh);

    // Voltage is a two-sided envelope, so deviation from nominal is the symptom.
    constexpr double kNominalVoltage = 220.0;
    const double voltageSeverity = clamp(std::abs(voltage.last - kNominalVoltage) / 30.0, 0.0, 1.0);

    const double temperatureTrend = trendPerMinute(temperature.slopePerSecond);
    const double vibrationTrend   = trendPerMinute(vibration.slopePerSecond);
    const double currentTrend     = trendPerMinute(current.slopePerSecond);

    // ---- per-class scores -------------------------------------------------
    // Each score encodes a relationship that is physically motivated rather than
    // fitted.  An engineer reviewing this can argue with the reasoning, which is
    // exactly what a fallback model should allow.

    // Bearing failure: vibration is the leading indicator, and a *rising*
    // vibration matters more than a merely high one.
    const double bearingScore = clamp(0.60 * vibrationSeverity
                                          + 0.25 * clamp(vibrationTrend, 0.0, 1.0)
                                          + 0.15 * currentSeverity,
                                      0.0, 1.0);

    // Overheating: high absolute temperature plus a sustained positive slope.
    const double overheatScore = clamp(0.70 * temperatureSeverity
                                           + 0.30 * clamp(temperatureTrend, 0.0, 1.0)
                                           - 0.15 * vibrationSeverity,
                                       0.0, 1.0);

    // Electrical fault: a voltage excursion plus a current that does not track
    // the mechanical load - the signature of a winding or supply problem.
    const double electricalScore = clamp(0.55 * voltageSeverity
                                             + 0.30 * currentSeverity
                                             + 0.15 * clamp(currentTrend, 0.0, 1.0)
                                             + (voltageSeverity > 0.6 && currentSeverity > 0.6 ? 0.2 : 0.0),
                                         0.0, 1.0);

    // Imbalance: vibration high while the current stays near nominal - the load
    // is not rising, so the energy is going into a mechanical asymmetry.
    const double imbalanceScore = clamp(0.70 * vibrationSeverity
                                            + 0.30 * (1.0 - currentSeverity)
                                            - 0.20 * temperatureSeverity,
                                        0.0, 1.0);

    // Lubrication loss: vibration and current rising together with temperature
    // following - friction, not imbalance.
    const double lubricationScore = clamp(0.45 * vibrationSeverity
                                              + 0.35 * currentSeverity
                                              + 0.20 * temperatureSeverity
                                              - 0.15 * voltageSeverity,
                                          0.0, 1.0);

    // RPM droop is a corroborating symptom: a loaded machine slows down.
    const double rpmDroop = window.empty() || window.front().rpm <= 1.0
                                ? 0.0
                                : clamp((window.front().rpm - rpm.last) / window.front().rpm * 4.0,
                                        0.0, 1.0);

    struct ClassScore
    {
        FaultClass faultClass;
        double     score;
    };

    const double worstSymptom = std::max({bearingScore, overheatScore, electricalScore,
                                          imbalanceScore, lubricationScore});

    const ClassScore scores[] = {
        {FaultClass::Healthy,         clamp(1.0 - worstSymptom, 0.0, 1.0)},
        {FaultClass::BearingFailure,  bearingScore},
        {FaultClass::Overheating,     overheatScore},
        {FaultClass::ElectricalFault, electricalScore},
        {FaultClass::Imbalance,       imbalanceScore},
        {FaultClass::LubricationLoss, lubricationScore},
    };

    const ClassScore* best = &scores[0];
    for (const ClassScore& candidate : scores)
    {
        if (candidate.score > best->score)
        {
            best = &candidate;
        }
    }

    // Failure probability blends the worst symptom with the corroborating rpm
    // droop, squashed through a logistic so that it saturates near 1 rather than
    // clipping - a model that reports exactly 1.0 is a model nobody trusts.
    prediction.failureProbability = clamp(sigmoid((worstSymptom - 0.5) * 6.0) * 0.85
                                              + 0.15 * rpmDroop,
                                          0.0, 0.99);

    // Health score is the complement, weighted so that one severe symptom costs
    // more than several mild ones: a machine with a single critical fault is not
    // "mostly healthy".
    const double meanSeverity = (temperatureSeverity + currentSeverity + vibrationSeverity
                                 + voltageSeverity) / 4.0;
    const double combinedSeverity = std::max(worstSymptom,
                                             0.6 * worstSymptom + 0.4 * meanSeverity);
    prediction.healthScore = clamp(100.0 * (1.0 - combinedSeverity), 0.0, 100.0);

    prediction.confidence = clamp(0.55 + 0.40 * (best->score - scores[0].score), 0.0, 0.95);

    // Only claim a fault class when the evidence is clear; otherwise report None
    // and let the health score carry the message.
    if (best->faultClass == FaultClass::Healthy || best->score < 0.42)
    {
        prediction.faultType  = toString(FaultClass::Healthy);
        prediction.confidence = clamp(0.50 + (0.42 - best->score), 0.0, 0.95);
    }
    else
    {
        prediction.faultType = toString(best->faultClass);
    }

    return prediction;
}

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------

QStringList FaultPredictor::explainFactors(const FeatureVector& features,
                                            const DeviceLimits& limits,
                                            FaultClass faultClass) const
{
    QStringList factors;

    const FeatureStatistics& temperature = features.statistics[0];
    const FeatureStatistics& voltage     = features.statistics[1];
    const FeatureStatistics& current     = features.statistics[2];
    const FeatureStatistics& vibration   = features.statistics[3];

    // A factor is reported when it is *material*: either it breaches the device's
    // envelope, or its trend is steep enough to be the reason the score moved.
    if (temperature.last >= limits.temperatureWarn)
    {
        factors << QStringLiteral("Temperature is above the warning limit at %1 C (limit %2 C)")
                       .arg(temperature.last, 0, 'f', 1)
                       .arg(limits.temperatureWarn, 0, 'f', 1);
    }
    if (std::abs(temperature.slopePerSecond * 60.0) > 0.5)
    {
        factors << QStringLiteral("Temperature is trending %1 at %2 C per minute across the window")
                       .arg(temperature.slopePerSecond >= 0.0 ? QStringLiteral("up")
                                                              : QStringLiteral("down"),
                            QString::number(std::abs(temperature.slopePerSecond * 60.0), 'f', 2));
    }
    if (vibration.last >= limits.vibrationWarn)
    {
        factors << QStringLiteral("Vibration is elevated at %1 mm/s RMS (limit %2)")
                       .arg(vibration.last, 0, 'f', 3)
                       .arg(limits.vibrationWarn, 0, 'f', 2);
    }
    if (std::abs(vibration.slopePerSecond * 60.0) > 0.01)
    {
        factors << QStringLiteral("Vibration is climbing at %1 mm/s per minute - consistent with "
                                  "progressive mechanical wear")
                       .arg(vibration.slopePerSecond * 60.0, 0, 'f', 4);
    }
    if (current.last >= limits.currentWarn)
    {
        factors << QStringLiteral("Current draw is above the warning limit at %1 A (limit %2 A)")
                       .arg(current.last, 0, 'f', 2)
                       .arg(limits.currentWarn, 0, 'f', 1);
    }
    if (voltage.last <= limits.voltageWarnLow || voltage.last >= limits.voltageWarnHigh)
    {
        factors << QStringLiteral("Supply voltage is outside the nominal band at %1 V")
                       .arg(voltage.last, 0, 'f', 1);
    }

    // Variance is a symptom in its own right: an unstable machine is not a
    // healthy one even when every instantaneous reading is inside the envelope.
    if (temperature.stdDev > 3.0)
    {
        factors << QStringLiteral("Temperature is unstable (standard deviation %1 C over the window)")
                       .arg(temperature.stdDev, 0, 'f', 2);
    }
    if (vibration.stdDev > 0.08)
    {
        factors << QStringLiteral("Vibration is unsteady (standard deviation %1 mm/s)")
                       .arg(vibration.stdDev, 0, 'f', 3);
    }

    if (factors.isEmpty())
    {
        if (faultClass == FaultClass::Healthy)
        {
            factors << QStringLiteral("All monitored quantities are inside their normal operating "
                                      "envelopes with no significant trend");
        }
        else
        {
            factors << QStringLiteral("The pattern matches the '%1' signature, although no single "
                                      "quantity has crossed its limit yet")
                           .arg(toString(faultClass));
        }
    }

    return factors;
}

}   // namespace saim
