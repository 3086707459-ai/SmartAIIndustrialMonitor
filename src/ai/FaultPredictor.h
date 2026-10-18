#pragma once

// ---------------------------------------------------------------------------
// FaultPredictor.h - device health scoring and fault-type classification.
//
// Two backends, one interface - and that is the design decision worth defending
// -------------------------------------------------------------------------
// The predictor owns an IInferenceEngine.  If a model loaded, every call is an
// ONNX Runtime inference.  If it did not (no model file, no ONNX Runtime, a
// corrupt export), the predictor falls back to a *physics-informed* model built
// from the same features.
//
// The fallback is not a placeholder.  It is a small, explainable model derived
// from the same relationships the simulator embodies - vibration grows
// super-linearly with bearing wear, temperature lags the load, current rises
// with mechanical resistance.  Three reasons this is the right engineering call
// rather than a cop-out:
//
//   1. The application must run on a reviewer's machine with no model file.
//      A system that shows "AI unavailable" in a demo has failed the demo.
//
//   2. It is the safety net in production too.  If a model file is corrupted
//      during a deployment, the plant keeps monitoring at reduced fidelity
//      instead of going dark - and the UI says which backend answered.
//
//   3. It makes the AI path testable.  Unit tests assert that the interface
//      behaves correctly without needing a 2 MB binary artefact in the repo.
//
// Both backends return the same FaultPrediction, and `backend` records which one
// produced the answer, so nothing downstream can be misled about provenance.
// ---------------------------------------------------------------------------

#include "ai/AIInferenceEngine.h"
#include "ai/FeatureExtractor.h"
#include "common/Types.h"

#include <QStringList>

#include <memory>
#include <vector>

namespace saim {

/// The fault classes the model can emit, in the index order the ONNX classifier
/// uses.  Kept in one place so the C++ and the training script cannot disagree
/// about what class 3 means - a mismatch that would silently relabel every
/// prediction.
enum class FaultClass : int
{
    Healthy = 0,
    BearingFailure = 1,
    Overheating = 2,
    ElectricalFault = 3,
    Imbalance = 4,
    LubricationLoss = 5,
    Count = 6
};

[[nodiscard]] QString toString(FaultClass faultClass);
/// Maintenance action for a class, in the operator's language.
[[nodiscard]] QString recommendedAction(FaultClass faultClass);

/// Inverse of toString().  Exists so that a fault type which has been persisted
/// as text (in the Prediction table, or in a report) can be turned back into its
/// class without a second, parallel mapping table.  Unknown text yields
/// FaultClass::Healthy rather than throwing: the value comes from a database row
/// written by an older version, and refusing to render a report because of it
/// would be the wrong trade-off.
[[nodiscard]] FaultClass faultClassFromString(const QString& text);

class FaultPredictor
{
public:
    struct Options
    {
        /// Below this many samples in the window, no prediction is produced.
        int minimumWindowSize = 10;
        double anomalyThreshold = 0.65;
    };

    /// `engine` may be null - that is the documented "no model available" case,
    /// not an error.  The predictor then answers from its built-in model.
    explicit FaultPredictor(std::unique_ptr<IInferenceEngine> engine,
                            Options options);
    /// Default-options overload; see the note in AIInferenceEngine.h on why the
    /// default argument is not written inline for a nested Options struct.
    explicit FaultPredictor(std::unique_ptr<IInferenceEngine> engine);
    ~FaultPredictor();

    FaultPredictor(const FaultPredictor&)            = delete;
    FaultPredictor& operator=(const FaultPredictor&) = delete;

    [[nodiscard]] bool hasNeuralModel() const noexcept { return m_engine != nullptr && m_engine->isReady(); }
    [[nodiscard]] const char* activeBackend() const noexcept;
    [[nodiscard]] const ModelMetadata& modelMetadata() const;

    /// Scores one device over its recent window.
    ///
    /// Returns a default-constructed prediction (with `faultType` empty) when
    /// the window is too short.  Callers must check `faultType.isEmpty()` and
    /// show "analysing" rather than treating the default as a healthy verdict.
    [[nodiscard]] FaultPrediction predict(const QString& deviceId,
                                          const std::vector<SensorSample>& window,
                                          const DeviceLimits& limits) const;

    /// The last feature vector that was computed, for the assistant's
    /// explanations.  Returned by value; callers on other threads get a copy.
    struct Explanation
    {
        QString deviceId;
        FeatureVector features;
        QStringList dominantFactors;   ///< human-readable, ordered by contribution
        FaultClass faultClass = FaultClass::Healthy;
    };
    [[nodiscard]] Explanation lastExplanation() const;

    void setOptions(const Options& options) { m_options = options; }

    /// Runs warm-up inferences so the first real call is not the slow one.
    void warmUp();

private:
    /// The physics-informed fallback.  Documented in the .cpp; the point is that
    /// it produces the same output type from the same inputs.
    [[nodiscard]] FaultPrediction predictHeuristic(const QString& deviceId,
                                                    const FeatureVector& features,
                                                    const std::vector<SensorSample>& window,
                                                    const DeviceLimits& limits) const;

    /// Identifies the dominant contributor to a poor score, for the explanation.
    [[nodiscard]] QStringList explainFactors(const FeatureVector& features,
                                              const DeviceLimits& limits,
                                              FaultClass faultClass) const;

    /// Maps the ONNX classifier's output vector onto a FaultPrediction.
    [[nodiscard]] FaultPrediction interpretNeuralOutput(const QString& deviceId,
                                                        const std::vector<float>& output,
                                                        const FeatureVector& features,
                                                        const DeviceLimits& limits) const;

    std::unique_ptr<IInferenceEngine> m_engine;
    FeatureExtractor m_extractor;
    Options m_options;

    /// Mutable because predict() is const and the explanation is a side channel
    /// for the UI.  Guarded because the AI worker writes it and the assistant
    /// may read it.  A shared_ptr swap keeps the read lock-free.
    mutable std::shared_ptr<const Explanation> m_lastExplanation;
};

}   // namespace saim
