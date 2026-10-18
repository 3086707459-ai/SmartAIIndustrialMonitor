#pragma once

// ---------------------------------------------------------------------------
// FeatureExtractor.h - turns a raw sample window into model-ready features.
//
// Why not feed raw samples to the model
// ------------------------------------
// A fault signature in industrial telemetry is almost never an absolute value;
// it is a *shape over time*: a rising trend, an increase in variance, vibration
// growing while the load stays flat.  A model given only the last sample cannot
// see any of that, and a model given 60 raw samples per feature spends most of
// its capacity learning that consecutive values are similar.
//
// So the window is summarised into a small, physically meaningful feature vector
// - the same features a maintenance engineer would look at.  Two consequences
// worth stating in an interview:
//
//   * The feature extractor is the model's real interface.  Retraining on a
//     different feature set means changing this file and the export script
//     together; the contract is documented in kFeatureCount and the ordering.
//
//   * Because the features are interpretable, the assistant can explain a
//     prediction in the engineer's own vocabulary ("temperature slope is +0.8
//     degC/min") instead of stating a probability with no provenance.
//
// Everything here is pure: no I/O, no allocation beyond the returned vector, and
// no dependency on Qt.  It is the most heavily unit-tested component in the
// project for exactly that reason.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QtGlobal>

#include <array>
#include <string>
#include <vector>

namespace saim {

/// Per-feature summary statistics for one window.
struct FeatureStatistics
{
    double mean = 0.0;
    double stdDev = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    /// Least-squares slope per second.  This is the trend the fault models key
    /// on: a bearing that is failing does not jump, it drifts.
    double slopePerSecond = 0.0;
    /// Newest value minus oldest, over the whole window.
    double totalDelta = 0.0;
    /// Newest value.
    double last = 0.0;
};

/// The complete input vector for the fault predictor and anomaly detector.
///
/// Layout (18 values, fixed order - the ONNX export must match exactly):
///
///   [0..4]   mean        of temperature, voltage, current, vibration, rpm
///   [5..9]   stdDev      of the same five
///   [10..14] slope/s     of the same five
///   [15]     window duration in seconds
///   [16]     sample count (normalised by the nominal window size)
///   [17]     fraction of the window spent above the warn threshold
struct FeatureVector
{
    static constexpr int kStatistics = 6;                 ///< per feature, in stats()
    static constexpr int kFeatures   = kFeatureCount;     ///< 5 channels
    static constexpr int kDimension  = 3 * kFeatureCount + 3;   ///< 18

    std::array<double, kDimension> values{};
    std::array<FeatureStatistics, kFeatureCount> statistics{};

    [[nodiscard]] const double* data() const noexcept { return values.data(); }
    [[nodiscard]] static constexpr int dimension() noexcept { return kDimension; }

    /// Names in `values` order - used by the assistant's explanations, and by
    /// the export script to check that the C++ and Python sides agree.
    [[nodiscard]] static std::vector<std::string> names();
};

/// Rolling-window feature extraction, plus the standardisation the model expects.
class FeatureExtractor
{
public:
    /// `nominalWindowSize` is only used to normalise the sample-count feature,
    /// so that a short window at start-up does not look like an anomaly.
    explicit FeatureExtractor(int nominalWindowSize = 60);

    void setNominalWindowSize(int size);
    [[nodiscard]] int nominalWindowSize() const noexcept { return m_nominalWindowSize; }

    /// Extracts features from a chronological window.
    ///
    /// Returns false when the window is too short to say anything meaningful
    /// (fewer than three samples).  Callers must treat that as "no prediction
    /// yet" rather than as "healthy" - the distinction matters, because
    /// reporting 100% health for a device that just came online is a lie the
    /// dashboard would show in green.
    [[nodiscard]] bool extract(const std::vector<SensorSample>& window,
                               FeatureVector& out) const;

    /// Applies the fixed standardisation the exported model was trained with.
    ///
    /// The mean/scale are constants in this file, not fitted at run time:
    /// they come from the training set in scripts/train_fault_model.py and must
    /// match it, or the model receives inputs from a different distribution than
    /// it learned - the single most common cause of a model that "works in
    /// Python and is useless in production".
    static void standardise(std::vector<float>& features);

    /// The training-set statistics, exposed so the export script and the tests
    /// can assert they have not drifted apart.
    struct Standardisation
    {
        static const std::array<double, FeatureVector::kDimension>& mean();
        static const std::array<double, FeatureVector::kDimension>& scale();
    };

private:
    int m_nominalWindowSize;
};

}   // namespace saim
