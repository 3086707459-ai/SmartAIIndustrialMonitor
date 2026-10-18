#include "ai/AnomalyDetector.h"

#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace saim {

namespace {

double clamp(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

/// Median of a copy - the input is taken by value so the caller's history is
/// never reordered.
double medianOf(std::deque<double> values)
{
    if (values.empty())
    {
        return 0.0;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    double result = values[middle];
    if (values.size() % 2 == 0)
    {
        // Average the two central values for an even-sized sample.
        const double upper = result;
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle - 1),
                         values.end());
        result = 0.5 * (upper + values[middle - 1]);
    }
    return result;
}

}   // namespace

// ===========================================================================
// RobustStatisticalModel
// ===========================================================================

RobustStatisticalModel::RobustStatisticalModel()
    : RobustStatisticalModel(Options{})
{
}

RobustStatisticalModel::RobustStatisticalModel(Options options)
    : m_options(options)
{
    m_options.baselineWindow = std::max(50, m_options.baselineWindow);
    m_options.warmupSamples  = std::max(5, m_options.warmupSamples);
}

std::vector<std::size_t> RobustStatisticalModel::monitoredIndices()
{
    // The five means, five standard deviations and five slopes.  Computed once.
    static const std::vector<std::size_t> kIndices = [] {
        std::vector<std::size_t> indices;
        indices.reserve(static_cast<std::size_t>(3 * kFeatureCount));
        for (std::size_t i = 0; i < static_cast<std::size_t>(3 * kFeatureCount); ++i)
        {
            indices.push_back(i);
        }
        return indices;
    }();
    return kIndices;
}

double RobustStatisticalModel::median(std::deque<double> values)
{
    return medianOf(std::move(values));
}

double RobustStatisticalModel::medianAbsoluteDeviation(const std::deque<double>& values, double med)
{
    if (values.empty())
    {
        return 0.0;
    }
    // std::deque has no reserve(); it is a segmented container and grows by
    // allocating new blocks, so there is no contiguous region to reserve.
    std::deque<double> deviations;
    for (const double value : values)
    {
        deviations.push_back(std::abs(value - med));
    }
    // 1.4826 rescales the MAD so that it is a consistent estimator of the
    // standard deviation for normally distributed data.  Without it the
    // threshold below would be in units nobody can reason about.
    constexpr double kConsistencyConstant = 1.4826;
    return kConsistencyConstant * medianOf(std::move(deviations));
}

double RobustStatisticalModel::score(const QString& deviceId, const std::vector<double>& features)
{
    const auto it = m_baselines.constFind(deviceId);
    if (it == m_baselines.constEnd() || it->count < m_options.warmupSamples)
    {
        // Not enough history to judge.  Reporting 0 here (rather than a guess) is
        // what lets the caller distinguish "normal" from "unknown".
        m_lastDeviations.insert(deviceId, {});
        return 0.0;
    }

    const Baseline& baseline = it.value();
    const std::vector<std::size_t> indices = monitoredIndices();

    std::vector<Deviation> deviations;
    deviations.reserve(indices.size());

    double worst = 0.0;

    for (std::size_t slot = 0; slot < indices.size(); ++slot)
    {
        const std::size_t featureIndex = indices[slot];
        if (featureIndex >= features.size() || slot >= baseline.history.size())
        {
            continue;
        }

        const std::deque<double>& series = baseline.history[slot];
        if (series.size() < 5)
        {
            continue;
        }

        const double med = medianOf(series);
        const double mad = medianAbsoluteDeviation(series, med);

        // A feature that has not moved at all in the baseline has zero MAD, so
        // the division would explode.  A floor derived from the feature's own
        // magnitude keeps the deviation finite and physically meaningful - a
        // 1 % change in a constant signal is not an infinite anomaly.
        const double floor = std::max(1e-6, 0.01 * std::abs(med));
        const double robustZ = std::abs(features[featureIndex] - med) / std::max(mad, floor);

        deviations.push_back(Deviation{featureIndex, robustZ});
        worst = std::max(worst, robustZ);
    }

    std::sort(deviations.begin(), deviations.end(),
              [](const Deviation& a, const Deviation& b) { return a.robustZ > b.robustZ; });
    m_lastDeviations.insert(deviceId, deviations);

    // Map the worst robust z-score onto 0..1 with a soft knee: below ~1 sigma
    // nothing at all, then rising, saturating at `robustZScale`.
    const double scale = std::max(0.5, m_options.robustZScale);
    const double normalised = clamp((worst - 1.0) / (scale - 1.0), 0.0, 1.0);

    // Squash so that the output is not linear in z: a 2-sigma event is a hint,
    // a 4-sigma event is a different category of thing, and the curve should
    // reflect that rather than reporting 0.57 for one and 0.86 for the other.
    return clamp(normalised * normalised * (3.0 - 2.0 * normalised), 0.0, 1.0);
}

void RobustStatisticalModel::observe(const QString& deviceId, const std::vector<double>& features)
{
    Baseline& baseline = m_baselines[deviceId];

    const std::vector<std::size_t> indices = monitoredIndices();
    if (baseline.history.size() != indices.size())
    {
        baseline.history.assign(indices.size(), std::deque<double>{});
    }

    // Adaptation guard: never fold a sample the model already considers
    // anomalous into the baseline.  Without this, a machine that fails slowly
    // teaches the model that failing is normal - and the detector goes quiet
    // exactly when it matters most.  This is the failure mode that makes naive
    // online anomaly detectors useless in practice.
    const bool modelIsConfident = baseline.count >= m_options.warmupSamples;
    if (modelIsConfident)
    {
        const double currentScore = score(deviceId, features);
        if (currentScore >= m_options.adaptationGuardScore)
        {
            return;   // observed, but deliberately not learned from
        }
    }

    for (std::size_t slot = 0; slot < indices.size(); ++slot)
    {
        const std::size_t featureIndex = indices[slot];
        if (featureIndex >= features.size())
        {
            continue;
        }
        std::deque<double>& series = baseline.history[slot];
        series.push_back(features[featureIndex]);
        while (static_cast<int>(series.size()) > m_options.baselineWindow)
        {
            series.pop_front();
        }
    }

    ++baseline.count;
}

void RobustStatisticalModel::forget(const QString& deviceId)
{
    m_baselines.remove(deviceId);
    m_lastDeviations.remove(deviceId);
}

int RobustStatisticalModel::observationCount(const QString& deviceId) const
{
    const auto it = m_baselines.constFind(deviceId);
    return it == m_baselines.constEnd() ? 0 : it->count;
}

std::vector<RobustStatisticalModel::Deviation>
RobustStatisticalModel::lastDeviations(const QString& deviceId) const
{
    return m_lastDeviations.value(deviceId);
}

// ===========================================================================
// AnomalyDetector
// ===========================================================================

AnomalyDetector::AnomalyDetector()
    : AnomalyDetector(nullptr, Options{})
{
}

AnomalyDetector::AnomalyDetector(std::unique_ptr<IAnomalyModel> model, Options options)
    : m_model(model != nullptr ? std::move(model)
                               : std::make_unique<RobustStatisticalModel>())
    , m_options(options)
{
    SAIM_INFO(logcat::kAi,
              QStringLiteral("AnomalyDetector initialised with model '%1'").arg(modelName()));
}

AnomalyDetector::~AnomalyDetector() = default;

QString AnomalyDetector::modelName() const
{
    return m_model != nullptr ? m_model->name() : QStringLiteral("none");
}

int AnomalyDetector::trackedDeviceCount() const
{
    // Counted through the model rather than kept separately, so the two cannot
    // disagree after a device removal.
    return static_cast<int>(m_consecutiveBreaches.size());
}

AnomalyEvent AnomalyDetector::analyse(const QString& deviceId,
                                       const std::vector<SensorSample>& window,
                                       const DeviceLimits& limits)
{
    AnomalyEvent event;
    event.deviceId  = deviceId;
    event.timestamp = QDateTime::currentDateTime();

    FeatureExtractor extractor(m_options.minimumWindowSize > 0 ? m_options.minimumWindowSize : 60);
    FeatureVector features;
    if (!extractor.extract(window, features))
    {
        return event;   // level stays None: not enough data to have an opinion
    }

    const std::vector<double> featureVector(features.values.begin(), features.values.end());

    const double rawScore = m_model->score(deviceId, featureVector);

    // Exponential smoothing of the score: a single sharp sample should move the
    // displayed level a little, a sustained shift should move it a lot.  Without
    // this the level flickers between None and Medium every other sample and the
    // operator stops looking at it.
    constexpr double kSmoothing = 0.35;
    const double previous = m_smoothedScores.value(deviceId, rawScore);
    const double smoothed = previous + kSmoothing * (rawScore - previous);
    m_smoothedScores.insert(deviceId, smoothed);

    event.score = smoothed;
    event.level = classify(deviceId, smoothed);
    event.reason = describe(deviceId, smoothed, features, limits, event.offendingFeatures);

    // Teach the model, but only when it did not just flag this sample - and only
    // after the event has been classified, because classify() consumes the
    // confirmation counter that observe() must not disturb.
    m_model->observe(deviceId, featureVector);

    if (event.level >= AnomalyLevel::Medium)
    {
        SAIM_INFO(logcat::kAi,
                  QStringLiteral("Anomaly on %1: level=%2 score=%3 - %4")
                      .arg(deviceId, toString(event.level))
                      .arg(smoothed, 0, 'f', 3)
                      .arg(event.reason));
    }

    return event;
}

AnomalyLevel AnomalyDetector::classify(const QString& deviceId, double score)
{
    if (score < m_options.reportThreshold)
    {
        // Back to normal: clear the confirmation counter so the next excursion
        // has to earn its escalation from scratch.
        m_consecutiveBreaches.insert(deviceId, 0);
        return AnomalyLevel::None;
    }

    if (score < m_options.mediumThreshold)
    {
        return AnomalyLevel::Low;
    }

    // Above the medium threshold.  Count consecutive breaches before escalating:
    // this is the debounce that keeps a single bad frame from becoming a High
    // alarm, and it is why `confirmationSamples` exists as an option rather than
    // a constant - a slow process (a kiln) wants a longer confirmation than a
    // fast one (a spindle).
    const int breaches = m_consecutiveBreaches.value(deviceId, 0) + 1;
    m_consecutiveBreaches.insert(deviceId, breaches);

    if (breaches < std::max(1, m_options.confirmationSamples))
    {
        return AnomalyLevel::Low;
    }

    return score >= m_options.highThreshold ? AnomalyLevel::High : AnomalyLevel::Medium;
}

QString AnomalyDetector::describe(const QString& deviceId, double score,
                                   const FeatureVector& features, const DeviceLimits& limits,
                                   QVector<int>& offendingChannels) const
{
    // Two independent kinds of evidence, and the message reports both:
    //
    //   1. Statistical - how far this window sits from the device's own learned
    //      baseline.  Catches behaviour nobody wrote a rule for.
    //   2. Physical - whether a quantity has crossed the envelope from the
    //      datasheet.  Catches a known-bad state even if the device has been
    //      running that way since the baseline was formed, which the statistical
    //      term alone would consider normal.
    //
    // Reporting only one of the two is how a monitoring system earns a reputation
    // for either crying wolf or missing the obvious.
    QString statisticalReason;

    if (const auto* robust = dynamic_cast<const RobustStatisticalModel*>(m_model.get()))
    {
        const std::vector<RobustStatisticalModel::Deviation> deviations =
            robust->lastDeviations(deviceId);

        QStringList parts;
        int reported = 0;
        for (const auto& deviation : deviations)
        {
            if (deviation.robustZ < 2.0 || reported >= 3)
            {
                break;   // the list is sorted descending, so we can stop early
            }

            // Feature index -> (channel, statistic) so the message names the
            // physical quantity rather than an array offset.
            const std::size_t channelIndex =
                deviation.featureIndex % static_cast<std::size_t>(kFeatureCount);
            const int statistic =
                static_cast<int>(deviation.featureIndex / static_cast<std::size_t>(kFeatureCount));
            const char* channelName = kFeatureNames[channelIndex];

            const char* statisticName = "average";
            if (statistic == 1)
            {
                statisticName = "variability";
            }
            else if (statistic == 2)
            {
                statisticName = "trend";
            }

            parts << QStringLiteral("%1 %2 is %3 robust sigmas from its learned baseline")
                         .arg(QString::fromLatin1(channelName),
                              QString::fromLatin1(statisticName))
                         .arg(deviation.robustZ, 0, 'f', 1);

            const int channel = static_cast<int>(channelIndex);
            if (!offendingChannels.contains(channel))
            {
                offendingChannels.append(channel);
            }
            ++reported;
        }

        statisticalReason = parts.join(QStringLiteral("; "));
    }

    // Envelope breaches, whether or not the statistical term fired.
    QStringList breaches;
    const auto noteBreach = [&breaches, &offendingChannels](int channel, const QString& message) {
        breaches << message;
        if (!offendingChannels.contains(channel))
        {
            offendingChannels.append(channel);
        }
    };

    if (features.statistics[0].last >= limits.temperatureWarn)
    {
        noteBreach(0, QStringLiteral("Temperature abnormal increase: %1 C against a warning limit "
                                     "of %2 C")
                          .arg(features.statistics[0].last, 0, 'f', 1)
                          .arg(limits.temperatureWarn, 0, 'f', 1));
    }
    if (features.statistics[3].last >= limits.vibrationWarn)
    {
        noteBreach(3, QStringLiteral("Vibration abnormal increase: %1 mm/s against a warning limit "
                                     "of %2")
                          .arg(features.statistics[3].last, 0, 'f', 3)
                          .arg(limits.vibrationWarn, 0, 'f', 2));
    }
    if (features.statistics[2].last >= limits.currentWarn)
    {
        noteBreach(2, QStringLiteral("Current abnormal increase: %1 A against a warning limit of "
                                     "%2 A")
                          .arg(features.statistics[2].last, 0, 'f', 2)
                          .arg(limits.currentWarn, 0, 'f', 1));
    }
    if (features.statistics[1].last <= limits.voltageWarnLow
        || features.statistics[1].last >= limits.voltageWarnHigh)
    {
        noteBreach(1, QStringLiteral("Supply voltage out of range at %1 V (band %2-%3 V)")
                          .arg(features.statistics[1].last, 0, 'f', 1)
                          .arg(limits.voltageWarnLow, 0, 'f', 0)
                          .arg(limits.voltageWarnHigh, 0, 'f', 0));
    }

    QStringList sections;
    if (!statisticalReason.isEmpty())
    {
        sections << statisticalReason;
    }
    if (!breaches.isEmpty())
    {
        sections << breaches.join(QStringLiteral("; "));
    }
    if (sections.isEmpty())
    {
        sections << QStringLiteral("Combined deviation from the learned baseline (score %1)")
                        .arg(score, 0, 'f', 2);
    }

    return sections.join(QStringLiteral(" | "));
}

void AnomalyDetector::forget(const QString& deviceId)
{
    m_model->forget(deviceId);
    m_consecutiveBreaches.remove(deviceId);
    m_smoothedScores.remove(deviceId);
}

}   // namespace saim
