#include "TestHarness.h"
#include "ai/AnomalyDetector.h"
#include "ai/FaultPredictor.h"
#include <QCoreApplication>
#include <cmath>

using namespace saim;

namespace {
std::vector<SensorSample> window(double temperature)
{
    std::vector<SensorSample> samples(30);
    const auto start = QDateTime::fromString(QStringLiteral("2023-11-14T22:13:20.000Z"), Qt::ISODateWithMs);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        auto& s = samples[i];
        s.deviceId = QStringLiteral("motor");
        s.timestamp = start.addSecs(static_cast<qint64>(i));
        s.temperature = temperature; s.voltage = 220; s.current = 8;
        s.vibration = 0.2; s.rpm = 1500; s.valid = true;
    }
    return samples;
}
}

SAIM_TEST(feature_statistics_match_constant_signal)
{
    FeatureExtractor extractor(30);
    FeatureVector features;
    SAIM_CHECK(extractor.extract(window(50), features));
    SAIM_CHECK_NEAR(features.values[0], 50.0, 1e-12);
    SAIM_CHECK_NEAR(features.values[5], 0.0, 1e-12);
    SAIM_CHECK_NEAR(features.values[10], 0.0, 1e-12);
    SAIM_CHECK_NEAR(features.values[15], 29.0, 1e-12);
    SAIM_CHECK_NEAR(features.values[16], 1.0, 1e-12);
    SAIM_CHECK(FeatureVector::names().size() == FeatureVector::kDimension);
}

SAIM_TEST(trend_uses_elapsed_time_with_irregular_sampling)
{
    auto samples = window(50);
    const auto start = samples.front().timestamp;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto seconds = static_cast<qint64>(i * i);
        samples[i].timestamp = start.addSecs(seconds);
        samples[i].temperature = 50.0 + 0.1 * static_cast<double>(seconds);
    }
    FeatureVector features;
    SAIM_CHECK(FeatureExtractor().extract(samples, features));
    SAIM_CHECK_NEAR(features.values[10], 0.1, 1e-10);
}

SAIM_TEST(short_windows_do_not_claim_healthy_predictions)
{
    FaultPredictor predictor(nullptr);
    auto samples = window(50);
    samples.resize(2);
    FeatureVector features;
    SAIM_CHECK(!FeatureExtractor().extract(samples, features));
    SAIM_CHECK(predictor.predict(QStringLiteral("motor"), samples, {}).faultType.isEmpty());
}

SAIM_TEST(fallback_labels_its_backend_and_degrades_for_overheating)
{
    FaultPredictor predictor(nullptr);
    const auto healthy = predictor.predict(QStringLiteral("motor"), window(50), {});
    const auto hot = predictor.predict(QStringLiteral("motor"), window(120), {});
    SAIM_CHECK(!predictor.hasNeuralModel());
    SAIM_CHECK(!hot.backend.isEmpty());
    SAIM_CHECK(hot.backend != QStringLiteral("onnxruntime"));
    SAIM_CHECK(hot.healthScore < healthy.healthScore);
    SAIM_CHECK(hot.failureProbability >= healthy.failureProbability);
    for (const auto& p : {healthy, hot})
    {
        SAIM_CHECK(std::isfinite(p.healthScore));
        SAIM_CHECK(p.healthScore >= 0 && p.healthScore <= 100);
        SAIM_CHECK(p.failureProbability >= 0 && p.failureProbability <= 1);
    }
}

SAIM_TEST(anomaly_baselines_are_isolated_and_do_not_learn_spikes)
{
    RobustStatisticalModel model;
    std::vector<double> normal(18, 1.0);
    for (int i = 0; i < 40; ++i) { model.observe(QStringLiteral("a"), normal); }
    auto spike = normal;
    spike[0] = 1000;
    SAIM_CHECK(model.score(QStringLiteral("a"), spike) > 0.75);
    const int before = model.observationCount(QStringLiteral("a"));
    model.observe(QStringLiteral("a"), spike);
    SAIM_CHECK(model.observationCount(QStringLiteral("a")) == before);
    SAIM_CHECK(model.observationCount(QStringLiteral("b")) == 0);
    model.forget(QStringLiteral("a"));
    SAIM_CHECK(model.observationCount(QStringLiteral("a")) == 0);
}

int main()
{
    QCoreApplication app(saim::test::argcRef(), saim::test::argvRef());
    return saim::test::runAll();
}
