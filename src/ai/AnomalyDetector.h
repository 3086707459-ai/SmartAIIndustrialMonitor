#pragma once

// ---------------------------------------------------------------------------
// AnomalyDetector.h - unsupervised, per-device anomaly detection.
//
// Why not reuse FaultPredictor
// ---------------------------
// The predictor is *supervised*: it answers "which known fault is this?" and can
// only ever name a class it was trained on.  The detector answers a different and
// strictly harder question - "is this unlike anything I have seen from this
// machine?" - and therefore catches the failure mode nobody labelled.
//
// The algorithm
// -------------
// A robust, per-device statistical model rather than a neural autoencoder, for
// reasons that are engineering decisions rather than compromises:
//
//   * It needs no training data, so it works on the first device a customer
//     connects.  An autoencoder needs a healthy baseline per asset class.
//
//   * It is per-device and *online*: mean and MAD are updated incrementally, so
//     a machine that legitimately drifts (a seasonal ambient temperature change)
//     adapts instead of alarming forever.
//
//   * Median/MAD instead of mean/standard deviation: the mean and standard
//     deviation are themselves corrupted by the outliers we are trying to find.
//     A single 10-sigma spike inflates the standard deviation enough to hide
//     itself; the median absolute deviation does not move at all.  This is the
//     single most important detail in the file.
//
// The same interface would accept an Isolation Forest or an autoencoder ONNX
// model - `IAnomalyModel` exists so that swapping the implementation is a
// one-line change in the factory, which is what "replaceable" should mean.
// ---------------------------------------------------------------------------

#include "ai/FeatureExtractor.h"
#include "common/Types.h"

#include <QDateTime>
#include <QHash>
#include <QString>

#include <deque>
#include <memory>
#include <vector>

namespace saim {

/// Pluggable detector backend, so the statistical model below can be replaced by
/// a learned one without touching any caller.
class IAnomalyModel
{
public:
    virtual ~IAnomalyModel() = default;

    /// Scores one feature vector for one device.  Returns a value where 0 means
    /// "entirely typical" and 1 means "maximally anomalous".
    [[nodiscard]] virtual double score(const QString& deviceId,
                                       const std::vector<double>& features) = 0;

    /// Lets the model observe a sample it should treat as normal.  Called for
    /// every sample, including anomalous ones - implementations decide how much
    /// to adapt (this one adapts slowly, and never from samples it flagged).
    virtual void observe(const QString& deviceId, const std::vector<double>& features) = 0;

    /// Forgets a device - called when it is removed from the fleet.
    virtual void forget(const QString& deviceId) = 0;

    [[nodiscard]] virtual QString name() const = 0;
    /// How many samples this device has contributed; below a threshold the model
    /// is not yet confident and must report Low at most.
    [[nodiscard]] virtual int observationCount(const QString& deviceId) const = 0;
};

/// Robust online statistical detector: per-device median/MAD with EWMA updates.
class RobustStatisticalModel final : public IAnomalyModel
{
public:
    struct Options
    {
        /// Samples needed before a device can raise anything above Low.  A model
        /// that has seen five samples does not know what "normal" is, and an
        /// alarm from it is noise an operator will learn to ignore.
        int warmupSamples = 30;
        /// The MAD multiplier that maps onto a 0..1 score.  3.5 corresponds
        /// roughly to a 3-sigma event for a normal distribution - the classical
        /// robust-outlier cut-off.
        double robustZScale = 3.5;
        /// EWMA factor for updating the running median estimate.  Small: the
        /// baseline should move on the scale of hours, not samples, so that a
        /// developing fault is not absorbed into "normal".
        double adaptationRate = 0.02;
        /// Retained observations per device per feature, used to keep a true
        /// median rather than only an EWMA estimate.
        int baselineWindow = 400;
        /// Score at or above which a sample is *not* folded into the baseline.
        ///
        /// This is the adaptation guard, and it is the difference between a
        /// detector that works and one that quietly stops working.  Deliberately
        /// the same value as AnomalyDetector::Options::reportThreshold: a sample
        /// the detector would report as anomalous must never be learned from, or
        /// a machine that degrades slowly teaches the model that degrading is
        /// normal - and the alarm goes quiet exactly when it matters.
        double adaptationGuardScore = 0.35;
    };

    explicit RobustStatisticalModel(Options options);
    RobustStatisticalModel();

    [[nodiscard]] double score(const QString& deviceId,
                               const std::vector<double>& features) override;
    void observe(const QString& deviceId, const std::vector<double>& features) override;
    void forget(const QString& deviceId) override;
    [[nodiscard]] QString name() const override { return QStringLiteral("robust-median-mad"); }
    [[nodiscard]] int observationCount(const QString& deviceId) const override;

    /// The per-feature deviation that drove the score, for the explanation.
    /// Returns a list of (featureIndex, robustZ) sorted by magnitude, descending.
    struct Deviation
    {
        std::size_t featureIndex = 0;
        double      robustZ = 0.0;
    };
    [[nodiscard]] std::vector<Deviation> lastDeviations(const QString& deviceId) const;

private:
    struct Baseline
    {
        /// Only the features that are actually monitored (means, stdDevs and
        /// slopes) are baselined - tracking all 18 wastes memory and would let a
        /// near-constant feature dominate through numerical noise.
        std::vector<std::deque<double>> history;
        int count = 0;
    };

    /// Feature indices worth baselining: the five means, five stdDevs and five
    /// slopes.  The last three features (window duration, sample count, fraction
    /// above warn) are structural, not machine behaviour.
    [[nodiscard]] static std::vector<std::size_t> monitoredIndices();

    [[nodiscard]] static double median(std::deque<double> values);
    [[nodiscard]] static double medianAbsoluteDeviation(const std::deque<double>& values, double med);

    Options m_options;
    QHash<QString, Baseline> m_baselines;
    mutable QHash<QString, std::vector<Deviation>> m_lastDeviations;
};

// ---------------------------------------------------------------------------
// AnomalyDetector - the layer the rest of the application talks to.
// ---------------------------------------------------------------------------

class AnomalyDetector
{
public:
    struct Options
    {
        /// Score above which an event is reported at all.
        double reportThreshold = 0.35;
        /// Score -> level boundaries.  Deliberately asymmetric: reaching High
        /// should require real evidence, because High means "stop the line".
        double mediumThreshold = 0.55;
        double highThreshold   = 0.75;
        /// Sustained-breach policy: an excursion must persist for this many
        /// consecutive samples before it escalates past Low.  This is what stops
        /// a single dropped frame or a sensor glitch from raising a High alarm -
        /// debouncing, applied to alarms rather than to buttons.
        int confirmationSamples = 3;
        int minimumWindowSize = 10;
    };

    /// `model` may be null, in which case the robust statistical model is used.
    explicit AnomalyDetector(std::unique_ptr<IAnomalyModel> model,
                             Options options);
    AnomalyDetector();
    ~AnomalyDetector();

    AnomalyDetector(const AnomalyDetector&)            = delete;
    AnomalyDetector& operator=(const AnomalyDetector&) = delete;

    [[nodiscard]] QString modelName() const;

    /// Analyses one device's window.  Always returns an event; `level` is None
    /// when nothing significant was found, so callers can use it as a log record
    /// as well as an alarm.
    ///
    /// The detector both *scores* and *adapts*: it is the only component that
    /// decides what this device's normal looks like, which is why the two
    /// operations are one call - splitting them would let a caller score without
    /// ever teaching the model, and the baseline would never form.
    [[nodiscard]] AnomalyEvent analyse(const QString& deviceId,
                                       const std::vector<SensorSample>& window,
                                       const DeviceLimits& limits);

    void forget(const QString& deviceId);

    /// Number of devices currently being modelled - shown in diagnostics.
    [[nodiscard]] int trackedDeviceCount() const;

private:
    /// Converts a raw score into a level, applying the confirmation policy.
    [[nodiscard]] AnomalyLevel classify(const QString& deviceId, double score);

    /// Builds the human-readable reason from the deviations that drove the score.
    [[nodiscard]] QString describe(const QString& deviceId, double score,
                                   const FeatureVector& features,
                                   const DeviceLimits& limits,
                                   QVector<int>& offendingChannels) const;

    std::unique_ptr<IAnomalyModel> m_model;
    Options m_options;

    /// Consecutive samples above the medium threshold, per device.
    QHash<QString, int> m_consecutiveBreaches;
    /// Smoothed score per device, so that a single sharp sample cannot flip the
    /// displayed level back and forth.
    QHash<QString, double> m_smoothedScores;
};

}   // namespace saim
