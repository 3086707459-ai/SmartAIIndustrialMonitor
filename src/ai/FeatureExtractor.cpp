#include "ai/FeatureExtractor.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace saim {

namespace {

/// Least-squares slope of y against t (seconds), for irregularly spaced samples.
///
/// The closed form assumes uniform spacing; telemetry from a real gateway is
/// *not* uniform (a dropped frame, a reconnect, a scheduling hiccup), so the
/// general least-squares form is used.  Using the uniform-spacing shortcut here
/// would make the slope estimate drift with the network jitter rather than with
/// the machine - a subtle bug that shows up as unexplained false warnings.
double leastSquaresSlope(const std::vector<double>& y, const std::vector<double>& t)
{
    const std::size_t n = y.size();
    if (n < 2)
    {
        return 0.0;
    }

    const double nAsDouble = static_cast<double>(n);
    const double sumT  = std::accumulate(t.begin(), t.end(), 0.0);
    const double sumY  = std::accumulate(y.begin(), y.end(), 0.0);

    double sumTT = 0.0;
    double sumTY = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        sumTT += t[i] * t[i];
        sumTY += t[i] * y[i];
    }

    const double denominator = nAsDouble * sumTT - sumT * sumT;
    if (std::abs(denominator) < 1e-12)
    {
        return 0.0;   // degenerate: every sample at the same instant
    }
    return (nAsDouble * sumTY - sumT * sumY) / denominator;
}

/// Population standard deviation.
double standardDeviation(const std::vector<double>& values, double mean)
{
    if (values.size() < 2)
    {
        return 0.0;
    }
    double accumulator = 0.0;
    for (const double value : values)
    {
        const double delta = value - mean;
        accumulator += delta * delta;
    }
    return std::sqrt(accumulator / static_cast<double>(values.size()));
}

}   // namespace

// ---------------------------------------------------------------------------
// FeatureVector
// ---------------------------------------------------------------------------

std::vector<std::string> FeatureVector::names()
{
    static const char* kChannels[] = {"temperature", "voltage", "current", "vibration", "rpm"};

    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(kDimension));

    for (const char* channel : kChannels)
    {
        names.emplace_back(std::string("mean_") + channel);
    }
    for (const char* channel : kChannels)
    {
        names.emplace_back(std::string("std_") + channel);
    }
    for (const char* channel : kChannels)
    {
        names.emplace_back(std::string("slope_") + channel);
    }
    names.emplace_back("window_seconds");
    names.emplace_back("sample_count_norm");
    names.emplace_back("above_warn_fraction");
    return names;
}

// ---------------------------------------------------------------------------
// FeatureExtractor
// ---------------------------------------------------------------------------

FeatureExtractor::FeatureExtractor(int nominalWindowSize)
    : m_nominalWindowSize(std::max(3, nominalWindowSize))
{
}

void FeatureExtractor::setNominalWindowSize(int size)
{
    m_nominalWindowSize = std::max(3, size);
}

bool FeatureExtractor::extract(const std::vector<SensorSample>& window, FeatureVector& out) const
{
    // Three samples is the minimum for a trend to mean anything; two points
    // always fit a line exactly, which would report a confident slope from noise.
    constexpr std::size_t kMinimumSamples = 3;
    if (window.size() < kMinimumSamples)
    {
        return false;
    }

    const std::size_t count = window.size();

    // Time base in seconds, relative to the first sample.  QDateTime would be
    // fine; milliseconds-since-epoch keeps the arithmetic in doubles and avoids
    // any timezone handling in a path that must stay pure.
    std::vector<double> seconds;
    seconds.reserve(count);
    const qint64 firstMs = window.front().timestamp.toMSecsSinceEpoch();
    double durationSeconds = 0.0;
    for (const SensorSample& sample : window)
    {
        const double offset = static_cast<double>(sample.timestamp.toMSecsSinceEpoch() - firstMs) / 1000.0;
        seconds.push_back(offset);
        durationSeconds = std::max(durationSeconds, offset);
    }

    out = FeatureVector{};

    for (int channel = 0; channel < kFeatureCount; ++channel)
    {
        std::vector<double> series;
        series.reserve(count);
        for (const SensorSample& sample : window)
        {
            series.push_back(sample.toFeatureVector()[static_cast<std::size_t>(channel)]);
        }

        FeatureStatistics stats;
        stats.mean = std::accumulate(series.begin(), series.end(), 0.0) / static_cast<double>(count);
        stats.stdDev = standardDeviation(series, stats.mean);
        stats.minimum = *std::min_element(series.begin(), series.end());
        stats.maximum = *std::max_element(series.begin(), series.end());
        stats.slopePerSecond = leastSquaresSlope(series, seconds);
        stats.last = series.back();
        stats.totalDelta = series.back() - series.front();

        out.statistics[static_cast<std::size_t>(channel)] = stats;

        out.values[static_cast<std::size_t>(channel)] = stats.mean;
        out.values[static_cast<std::size_t>(kFeatureCount + channel)] = stats.stdDev;
        out.values[static_cast<std::size_t>(2 * kFeatureCount + channel)] = stats.slopePerSecond;
    }

    out.values[static_cast<std::size_t>(3 * kFeatureCount + 0)] = durationSeconds;
    out.values[static_cast<std::size_t>(3 * kFeatureCount + 1)] =
        static_cast<double>(count) / static_cast<double>(m_nominalWindowSize);

    // Fraction of the window spent above the temperature warn threshold.  This
    // is the one feature that is not a moment of the distribution: a device that
    // oscillates across a limit is qualitatively different from one that sits
    // comfortably below it, and no mean/var pair distinguishes them.
    const DeviceLimits defaultLimits;
    int aboveWarn = 0;
    for (const SensorSample& sample : window)
    {
        if (sample.temperature >= defaultLimits.temperatureWarn
            || sample.vibration >= defaultLimits.vibrationWarn)
        {
            ++aboveWarn;
        }
    }
    out.values[static_cast<std::size_t>(3 * kFeatureCount + 2)] =
        static_cast<double>(aboveWarn) / static_cast<double>(count);

    return true;
}

// ---------------------------------------------------------------------------
// Standardisation
// ---------------------------------------------------------------------------

const std::array<double, FeatureVector::kDimension>& FeatureExtractor::Standardisation::mean()
{
    // Derived from the synthetic training set produced by
    // scripts/train_fault_model.py (10 000 windows, healthy + degraded + failing).
    // Regenerating that script's output means regenerating these two arrays -
    // and the script prints them in exactly this format to make that mechanical.
    static const std::array<double, FeatureVector::kDimension> kMean{
        // mean: temperature, voltage, current, vibration, rpm
        52.31, 220.04, 8.62, 0.214, 3180.5,
        // stdDev
        4.87, 1.52, 1.31, 0.093, 512.4,
        // slope per second
        0.0041, -0.0002, 0.0018, 0.0007, -0.31,
        // window seconds, count norm, above-warn fraction
        29.7, 1.01, 0.14};
    return kMean;
}

const std::array<double, FeatureVector::kDimension>& FeatureExtractor::Standardisation::scale()
{
    static const std::array<double, FeatureVector::kDimension> kScale{
        11.42, 3.11, 3.94, 0.187, 1980.0,   // mean
        5.63, 1.04, 1.12, 0.121, 402.7,     // stdDev
        0.0132, 0.0011, 0.0064, 0.0019, 0.87,   // slope
        4.1, 0.02, 0.21};                   // window, count, fraction
    return kScale;
}

void FeatureExtractor::standardise(std::vector<float>& features)
{
    const auto& mean = Standardisation::mean();
    const auto& scale = Standardisation::scale();

    const std::size_t count = std::min(features.size(), static_cast<std::size_t>(FeatureVector::kDimension));
    for (std::size_t i = 0; i < count; ++i)
    {
        const double divisor = std::abs(scale[i]) < 1e-9 ? 1.0 : scale[i];
        features[i] = static_cast<float>((static_cast<double>(features[i]) - mean[i]) / divisor);
    }
}

}   // namespace saim
