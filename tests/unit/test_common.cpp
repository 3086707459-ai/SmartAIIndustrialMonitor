// ---------------------------------------------------------------------------
// test_common.cpp - the value types and containers every layer is built on.
//
// These are the cheapest tests in the suite and the ones that would catch the
// most damage if they ever failed, because everything above them assumes these
// contracts hold.  A RingBuffer that returns the wrong element would show up as
// a subtly wrong chart, a subtly wrong feature vector and a subtly wrong
// prediction - three symptoms, one cause, and only if someone happened to look.
// ---------------------------------------------------------------------------

#include "TestHarness.h"

#include "common/RingBuffer.h"
#include "common/Result.h"
#include "common/Types.h"

#include <QDateTime>

using namespace saim;

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

SAIM_TEST(ring_buffer_returns_elements_in_chronological_order)
{
    RingBuffer<int> ring(4);

    ring.push(1);
    ring.push(2);
    ring.push(3);

    SAIM_CHECK(ring.size() == 3);
    SAIM_CHECK(ring.at(0) == 1);      // index 0 is always the oldest
    SAIM_CHECK(ring.at(2) == 3);
    SAIM_CHECK(ring.newest() == 3);
    SAIM_CHECK(ring.oldest() == 1);
    SAIM_CHECK(!ring.full());
}

SAIM_TEST(ring_buffer_overwrites_the_oldest_when_full)
{
    RingBuffer<int> ring(3);

    for (int value = 1; value <= 5; ++value)
    {
        ring.push(value);
    }

    SAIM_CHECK(ring.size() == 3);
    SAIM_CHECK(ring.full());
    SAIM_CHECK(ring.oldest() == 3);   // 1 and 2 were overwritten
    SAIM_CHECK(ring.newest() == 5);
    SAIM_CHECK(ring.at(0) == 3);

    const std::vector<int> all = ring.toVector();
    SAIM_CHECK(all.size() == 3);
    SAIM_CHECK(all.front() == 3);
    SAIM_CHECK(all.back() == 5);
}

SAIM_TEST(ring_buffer_with_zero_capacity_is_well_defined)
{
    // A degenerate configuration, but one that arises from a bad config value
    // rather than a programming error - so it must not divide by zero or read
    // past the end.  It is documented as well defined; this proves it.
    RingBuffer<int> ring(0);

    ring.push(42);

    SAIM_CHECK(ring.size() == 0);
    SAIM_CHECK(ring.empty());
    SAIM_CHECK(ring.toVector().empty());
}

SAIM_TEST(ring_buffer_clears)
{
    RingBuffer<int> ring(4);
    ring.push(7);
    ring.push(8);
    ring.clear();

    SAIM_CHECK(ring.empty());
    SAIM_CHECK(ring.size() == 0);

    ring.push(9);
    SAIM_CHECK(ring.size() == 1);
    SAIM_CHECK(ring.oldest() == 9);
}

// ---------------------------------------------------------------------------
// Result / Status
// ---------------------------------------------------------------------------

SAIM_TEST(result_reports_success_and_carries_a_value)
{
    const Result<int> ok(5);
    SAIM_CHECK(ok.isOk());
    SAIM_CHECK(!ok.isError());
    SAIM_CHECK(ok.value() == 5);
}

SAIM_TEST(result_error_carries_a_code_and_a_message)
{
    const Result<int> bad(Error{7, QStringLiteral("something went wrong")});

    SAIM_CHECK(bad.isError());
    SAIM_CHECK(bad.error().code == 7);
    SAIM_CHECK(bad.error().message == QStringLiteral("something went wrong"));

    // Accessing the value of an error must not be undefined: it returns the
    // caller's fallback.  The alternative - an assert, or a default-constructed
    // value - loses information at exactly the moment it is most needed.
    SAIM_CHECK(bad.valueOr(99) == 99);
}

SAIM_TEST(status_is_a_result_void)
{
    const Status good = Status::ok();
    SAIM_CHECK(good.isOk());

    const Status bad = Status::fail(3, QStringLiteral("no"));
    SAIM_CHECK(bad.isError());
    SAIM_CHECK(bad.error().code == 3);
}

// ---------------------------------------------------------------------------
// DeviceLimits - the rule-based envelope
// ---------------------------------------------------------------------------

namespace {

SensorSample sampleWith(double temperature, double voltage, double current, double vibration)
{
    SensorSample sample;
    sample.deviceId    = QStringLiteral("M1");
    sample.timestamp   = QDateTime::currentDateTime();
    sample.temperature = temperature;
    sample.voltage     = voltage;
    sample.current     = current;
    sample.vibration   = vibration;
    sample.valid       = true;
    return sample;
}

}   // namespace

SAIM_TEST(limits_pass_a_sample_inside_the_envelope)
{
    const DeviceLimits limits;

    const SensorSample normal = sampleWith(45.0, 220.0, 8.0, 0.10);
    SAIM_CHECK(limits.evaluate(normal) == DeviceState::Normal);
}

SAIM_TEST(limits_warn_between_the_warn_and_high_thresholds)
{
    const DeviceLimits limits;

    // 75 C is above temperatureWarn (70) and below temperatureHigh (85).
    const SensorSample warm = sampleWith(75.0, 220.0, 8.0, 0.10);
    SAIM_CHECK(limits.evaluate(warm) == DeviceState::Warning);
}

SAIM_TEST(limits_fault_at_or_above_the_high_threshold)
{
    const DeviceLimits limits;

    const SensorSample hot = sampleWith(90.0, 220.0, 8.0, 0.10);
    SAIM_CHECK(limits.evaluate(hot) == DeviceState::Fault);
}

SAIM_TEST(limits_take_the_worst_of_every_channel)
{
    // The point of this test is the *combination*: one channel at Fault must
    // dominate three channels at Normal, regardless of order.  A loop that
    // returned early on the first Normal reading would pass the previous three
    // tests and fail here.
    const DeviceLimits limits;

    const SensorSample mixed = sampleWith(45.0, 220.0, 8.0, 0.95);   // vibration High
    SAIM_CHECK(limits.evaluate(mixed) == DeviceState::Fault);

    const SensorSample alsoMixed = sampleWith(72.0, 190.0, 8.0, 0.10);   // temp Warn, volts Warn
    SAIM_CHECK(limits.evaluate(alsoMixed) == DeviceState::Warning);
}

SAIM_TEST(limits_treat_voltage_as_a_two_sided_envelope)
{
    // Voltage is the one channel that is wrong at *both* ends, which is why the
    // limits struct has a low and a high rather than a single warn/high pair.
    // A machine fed at 180 V is damaged by the same mechanism as one fed at
    // 250 V: undervoltage raises current for the same load.
    const DeviceLimits limits;

    const SensorSample brownout = sampleWith(45.0, 180.0, 8.0, 0.10);
    SAIM_CHECK(limits.evaluate(brownout) == DeviceState::Warning);

    const SensorSample surge = sampleWith(45.0, 260.0, 8.0, 0.10);
    // Voltage has warning bounds only; no voltage trip threshold is configured.
    SAIM_CHECK(limits.evaluate(surge) == DeviceState::Warning);
}

// ---------------------------------------------------------------------------
// SensorSample::toFeatureVector
// ---------------------------------------------------------------------------

SAIM_TEST(feature_vector_has_the_declared_dimension)
{
    // The dimension is a contract with the model: a 5-wide vector here and an
    // 18-wide input layer there is a runtime crash inside ONNX Runtime, so the
    // declared count and the produced count are both asserted.
    const SensorSample sample = sampleWith(60.0, 220.0, 10.0, 0.3);

    const auto features = sample.toFeatureVector();

    SAIM_CHECK(features.size() == static_cast<std::size_t>(kFeatureCount));
    SAIM_CHECK(kFeatureNames.size() == kFeatureCount);
    SAIM_CHECK_NEAR(features[0], 60.0, 0.0);
    SAIM_CHECK_NEAR(features[1], 220.0, 0.0);
    SAIM_CHECK_NEAR(features[2], 10.0, 0.0);
    SAIM_CHECK_NEAR(features[3], 0.3, 0.0);
}

// ---------------------------------------------------------------------------
// Derived state
// ---------------------------------------------------------------------------

SAIM_TEST(prediction_implies_a_state_from_its_health_score)
{
    FaultPrediction prediction;
    prediction.faultType = QStringLiteral("Bearing Failure");

    prediction.healthScore = 95.0;
    SAIM_CHECK(prediction.impliedState() == DeviceState::Normal);

    prediction.healthScore = 60.0;
    SAIM_CHECK(prediction.impliedState() == DeviceState::Warning);

    prediction.healthScore = 20.0;
    prediction.failureProbability = 0.9;
    SAIM_CHECK(prediction.impliedState() == DeviceState::Fault);
}

SAIM_TEST(a_prediction_with_no_fault_type_implies_nothing)
{
    // This is the "no verdict yet" case and it matters: the default
    // FaultPrediction has healthScore 100, and treating that as a real verdict
    // would paint a green 100 % on a device that has never been analysed.  The
    // empty faultType is the discriminator, and this test pins that contract.
    const FaultPrediction unanalysed;

    SAIM_CHECK(unanalysed.faultType.isEmpty());
    SAIM_CHECK(unanalysed.impliedState() == DeviceState::Offline);
}

int main()
{
    return saim::test::runAll();
}
