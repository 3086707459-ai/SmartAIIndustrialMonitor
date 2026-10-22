// ---------------------------------------------------------------------------
// test_device.cpp - the domain model: state machine, alarm suppression, history.
//
// Device is the component with the most *policy* in the whole application and
// the least I/O, which makes it the one that most deserves tests.  It decides
// when a machine is alarming, when an alarm is suppressed, and when a device is
// considered offline - and every one of those decisions is reachable from a
// constructed object with no socket and no database.
//
// A note on the timing in these tests
// -----------------------------------
// Alarm suppression and liveness are both time-based, and a test that sleeps for
// thirty seconds is a test nobody runs.  The approach here is to drive the
// time-dependent behaviour through the *public* entry point that takes a timeout
// parameter (checkLiveness) rather than to mock a clock.  Suppression itself is
// exercised by observing that a second alarm at the same severity does not
// re-emit - which is observable without waiting, because the emission is what is
// suppressed, not the state change.
// ---------------------------------------------------------------------------

#include "TestHarness.h"

#include "device/Device.h"

#include <QCoreApplication>
#include <QSignalSpy>

using namespace saim;

namespace {

SensorSample makeSample(const QString& deviceId, double temperature, double voltage,
                        double current, double vibration)
{
    SensorSample sample;
    sample.deviceId    = deviceId;
    sample.timestamp   = QDateTime::currentDateTime();
    sample.temperature = temperature;
    sample.voltage     = voltage;
    sample.current     = current;
    sample.vibration   = vibration;
    sample.rpm         = 1450.0;
    sample.valid       = true;
    return sample;
}

SensorSample normalSample(const QString& deviceId)
{
    return makeSample(deviceId, 45.0, 220.0, 8.0, 0.10);
}

}   // namespace

// ---------------------------------------------------------------------------
// Construction and identity
// ---------------------------------------------------------------------------

SAIM_TEST(a_new_device_is_offline_with_no_data)
{
    Device device(QStringLiteral("Motor-001"), QStringLiteral("Feed pump motor"));

    SAIM_CHECK(device.deviceId() == QStringLiteral("Motor-001"));
    SAIM_CHECK(device.deviceName() == QStringLiteral("Feed pump motor"));

    // Offline, not Normal.  A device that has never reported is not healthy -
    // it is unknown, and presenting unknown as healthy is how a dead sensor
    // goes unnoticed until something seizes.
    SAIM_CHECK(device.state() == DeviceState::Offline);
    SAIM_CHECK(!device.latest().valid);
    SAIM_CHECK(device.sampleCount() == 0);
    SAIM_CHECK(device.alarmCount() == 0);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

SAIM_TEST(a_normal_sample_brings_an_offline_device_online)
{
    Device device(QStringLiteral("M1"));

    QSignalSpy spy(&device, &Device::statusChanged);
    device.onSampleReceived(normalSample(QStringLiteral("M1")));

    SAIM_CHECK(device.state() == DeviceState::Normal);
    SAIM_CHECK(spy.count() == 1);
    SAIM_CHECK(device.latest().valid);
    SAIM_CHECK(device.sampleCount() == 1);
}

SAIM_TEST(status_changes_are_emitted_only_on_a_transition)
{
    // The signal drives the device list model, the tiles and the alarm path.  A
    // device sitting in Warning at 2 Hz for an hour must not emit 7200 identical
    // transitions - that is an event storm disguised as a signal.
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));

    QSignalSpy spy(&device, &Device::statusChanged);

    for (int index = 0; index < 5; ++index)
    {
        device.onSampleReceived(normalSample(QStringLiteral("M1")));
    }

    SAIM_CHECK(spy.count() == 0);
    SAIM_CHECK(device.sampleCount() == 6);
}

SAIM_TEST(a_warning_sample_moves_the_device_to_warning)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));

    // 76 C is above the warn threshold of 70 and below the high threshold of 85.
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 76.0, 220.0, 8.0, 0.10));

    SAIM_CHECK(device.state() == DeviceState::Warning);
}

SAIM_TEST(a_fault_sample_moves_the_device_to_fault)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 95.0, 220.0, 8.0, 0.10));

    SAIM_CHECK(device.state() == DeviceState::Fault);
}

SAIM_TEST(a_device_recovers_when_the_values_return_to_normal)
{
    // Recovery is as important as detection: a monitoring tool that latches a
    // warning forever teaches its operator to ignore warnings.
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 95.0, 220.0, 8.0, 0.10));
    SAIM_CHECK(device.state() == DeviceState::Fault);

    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    SAIM_CHECK(device.state() == DeviceState::Normal);
}

// ---------------------------------------------------------------------------
// Alarms
// ---------------------------------------------------------------------------

SAIM_TEST(an_alarm_is_raised_on_entering_a_warning_state)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    device.acknowledgeAlarm();

    QSignalSpy spy(&device, &Device::alarmTriggered);

    device.onSampleReceived(makeSample(QStringLiteral("M1"), 76.0, 220.0, 8.0, 0.10));

    SAIM_CHECK(spy.count() == 1);
    SAIM_CHECK(device.alarmCount() == 1);

    const auto alarm = spy.at(0).at(0).value<AlarmRecord>();
    SAIM_CHECK(alarm.deviceId == QStringLiteral("M1"));
    SAIM_CHECK(alarm.state == DeviceState::Warning);
    SAIM_CHECK(!alarm.message.isEmpty());
}

SAIM_TEST(a_device_stuck_in_warning_does_not_repeat_its_alarm)
{
    // The repeat-suppression window.  This is the difference between an alarm
    // system and a log file: 7200 identical alarms in an hour is how an operator
    // learns to close the alarm panel.
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 76.0, 220.0, 8.0, 0.10));

    QSignalSpy spy(&device, &Device::alarmTriggered);

    for (int index = 0; index < 10; ++index)
    {
        device.onSampleReceived(makeSample(QStringLiteral("M1"), 77.0, 220.0, 8.0, 0.10));
    }

    SAIM_CHECK(spy.count() == 0);
    SAIM_CHECK(device.alarmCount() == 1);   // still the one from the transition
}

SAIM_TEST(escalating_from_warning_to_fault_raises_a_second_alarm)
{
    // Suppression is per severity, not per device.  A device that gets *worse*
    // must say so immediately, or the suppression that stops the noise also
    // stops the escalation - which is the failure mode that actually hurts.
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 76.0, 220.0, 8.0, 0.10));

    QSignalSpy spy(&device, &Device::alarmTriggered);
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 95.0, 220.0, 8.0, 0.10));

    SAIM_CHECK(spy.count() == 1);
    const auto alarm = spy.at(0).at(0).value<AlarmRecord>();
    SAIM_CHECK(alarm.state == DeviceState::Fault);
    SAIM_CHECK(device.alarmCount() == 2);
}

SAIM_TEST(acknowledging_clears_the_latch_and_allows_a_new_alarm)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 76.0, 220.0, 8.0, 0.10));
    SAIM_CHECK(device.alarmCount() == 1);

    device.acknowledgeAlarm();
    device.onSampleReceived(normalSample(QStringLiteral("M1")));

    QSignalSpy spy(&device, &Device::alarmTriggered);
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 78.0, 220.0, 8.0, 0.10));

    SAIM_CHECK(spy.count() == 1);
}

// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------

SAIM_TEST(a_device_goes_offline_after_the_timeout)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    SAIM_CHECK(device.state() == DeviceState::Normal);

    // A zero timeout means "every device is now stale", which is the same code
    // path a real fifteen-second timeout takes.  Driving it through the public
    // method rather than sleeping keeps the test honest *and* instant.
    device.checkLiveness(0);

    SAIM_CHECK(device.state() == DeviceState::Offline);
}

SAIM_TEST(a_freshly_seen_device_stays_online)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));

    device.checkLiveness(60000);   // a minute: nothing can have gone stale yet

    SAIM_CHECK(device.state() == DeviceState::Normal);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

SAIM_TEST(history_keeps_the_most_recent_samples_and_bounds_memory)
{
    Device device(QStringLiteral("M1"));
    device.setHistoryCapacity(5);

    for (int index = 0; index < 20; ++index)
    {
        device.onSampleReceived(
            makeSample(QStringLiteral("M1"), 40.0 + static_cast<double>(index), 220.0, 8.0, 0.1));
    }

    const std::vector<SensorSample> history = device.history();

    // The cap is what makes the per-device memory cost a constant rather than a
    // function of uptime - the single most important property of this buffer.
    SAIM_CHECK(history.size() == 5);
    SAIM_CHECK(device.sampleCount() == 20);     // the counter is not capped
    SAIM_CHECK_NEAR(history.front().temperature, 55.0, 0.0);   // samples 15..19
    SAIM_CHECK_NEAR(history.back().temperature, 59.0, 0.0);
}

// ---------------------------------------------------------------------------
// Statistics - the input to the report generator
// ---------------------------------------------------------------------------

SAIM_TEST(statistics_summarise_the_retained_window)
{
    Device device(QStringLiteral("M1"));

    device.onSampleReceived(makeSample(QStringLiteral("M1"), 40.0, 220.0, 8.0, 0.10));
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 60.0, 220.0, 10.0, 0.30));
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 50.0, 220.0, 12.0, 0.20));

    const DeviceStatistics statistics = device.statistics();

    SAIM_CHECK(statistics.deviceId == QStringLiteral("M1"));
    SAIM_CHECK(statistics.sampleCount == 3);
    SAIM_CHECK_NEAR(statistics.maxTemperature, 60.0, 0.0);
    SAIM_CHECK_NEAR(statistics.avgTemperature, 50.0, 1e-9);
    SAIM_CHECK_NEAR(statistics.maxVibration, 0.30, 1e-9);
    SAIM_CHECK_NEAR(statistics.avgCurrent, 10.0, 1e-9);
}

// ---------------------------------------------------------------------------
// AI verdicts
// ---------------------------------------------------------------------------

SAIM_TEST(a_prediction_with_poor_health_escalates_the_state)
{
    // The rule-based envelope and the model are two independent opinions, and
    // the device state is the worse of the two.  A machine inside its electrical
    // limits whose bearing is failing is still a machine about to fail.
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    SAIM_CHECK(device.state() == DeviceState::Normal);

    FaultPrediction prediction;
    prediction.deviceId     = QStringLiteral("M1");
    prediction.timestamp    = QDateTime::currentDateTime();
    prediction.faultType    = QStringLiteral("Bearing Failure");
    prediction.healthScore  = 25.0;
    prediction.failureProbability = 0.9;
    prediction.confidence   = 0.9;
    prediction.backend      = QStringLiteral("heuristic");

    device.onPredictionReady(prediction);

    SAIM_CHECK(device.prediction().faultType == QStringLiteral("Bearing Failure"));
    SAIM_CHECK(device.state() == DeviceState::Fault);
}

SAIM_TEST(a_verdict_does_not_resurrect_an_offline_device)
{
    // Both directions of the "worst of the two" rule have to hold.  A stale
    // verdict from before a device dropped off the network must not bring it
    // back to life on the dashboard.
    Device device(QStringLiteral("M1"));

    FaultPrediction healthy;
    healthy.deviceId    = QStringLiteral("M1");
    healthy.faultType   = QStringLiteral("None");
    healthy.healthScore = 98.0;
    device.onPredictionReady(healthy);

    SAIM_CHECK(device.state() == DeviceState::Offline);
}

SAIM_TEST(statistics_count_matches_the_retained_window)
{
    Device device(QStringLiteral("M1"));
    device.setHistoryCapacity(3);
    for (int i = 0; i < 10; ++i) { device.onSampleReceived(normalSample(QStringLiteral("M1"))); }
    SAIM_CHECK(device.sampleCount() == 10);
    SAIM_CHECK(device.statistics().sampleCount == 3);
}

SAIM_TEST(late_analysis_cannot_revive_a_timed_out_device)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    device.checkLiveness(0);
    const auto alarms = device.alarmCount();
    QSignalSpy predictions(&device, &Device::predictionUpdated);
    QSignalSpy anomalies(&device, &Device::anomalyDetected);
    FaultPrediction prediction;
    prediction.deviceId = QStringLiteral("M1");
    prediction.timestamp = QDateTime::currentDateTime();
    prediction.faultType = QStringLiteral("None");
    device.onPredictionReady(prediction);
    AnomalyEvent anomaly;
    anomaly.deviceId = QStringLiteral("M1");
    anomaly.timestamp = prediction.timestamp;
    anomaly.level = AnomalyLevel::High;
    device.onAnomalyDetected(anomaly);
    SAIM_CHECK(device.state() == DeviceState::Offline);
    SAIM_CHECK(device.alarmCount() == alarms);
    SAIM_CHECK(predictions.isEmpty() && anomalies.isEmpty());
}

SAIM_TEST(disabled_devices_reject_diagnostics_until_new_telemetry)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    device.setEnabled(false);
    AnomalyEvent anomaly;
    anomaly.deviceId = QStringLiteral("M1");
    anomaly.timestamp = QDateTime::currentDateTime();
    anomaly.level = AnomalyLevel::High;
    device.onAnomalyDetected(anomaly);
    SAIM_CHECK(device.alarmCount() == 0);
    device.setEnabled(true);
    FaultPrediction prediction;
    prediction.deviceId = QStringLiteral("M1");
    prediction.timestamp = anomaly.timestamp;
    prediction.faultType = QStringLiteral("None");
    device.onPredictionReady(prediction);
    SAIM_CHECK(device.state() == DeviceState::Offline);
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    SAIM_CHECK(device.state() == DeviceState::Normal);
}

SAIM_TEST(anomaly_changes_state_immediately_and_does_not_repeat_alarms)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    AnomalyEvent anomaly;
    anomaly.deviceId = QStringLiteral("M1");
    anomaly.timestamp = QDateTime::currentDateTime();
    anomaly.level = AnomalyLevel::High;
    anomaly.reason = QStringLiteral("Vibration spike");
    QSignalSpy alarms(&device, &Device::alarmTriggered);
    device.onAnomalyDetected(anomaly);
    SAIM_CHECK(device.state() == DeviceState::Fault);
    SAIM_CHECK(alarms.count() == 1);
    SAIM_CHECK(alarms.at(0).at(0).value<AlarmRecord>().message.contains(anomaly.reason));
    device.onAnomalyDetected(anomaly);
    SAIM_CHECK(alarms.count() == 1);
    anomaly.level = AnomalyLevel::None;
    device.onAnomalyDetected(anomaly);
    SAIM_CHECK(device.state() == DeviceState::Normal);
}

SAIM_TEST(complete_analysis_has_no_intermediate_recovery_or_duplicate_alarm)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    FaultPrediction prediction;
    prediction.deviceId = QStringLiteral("M1");
    prediction.timestamp = QDateTime::currentDateTime();
    prediction.faultType = QStringLiteral("Bearing Failure");
    prediction.backend = QStringLiteral("heuristic");
    prediction.healthScore = 25;
    prediction.failureProbability = 0.9;
    AnomalyEvent anomaly;
    anomaly.deviceId = prediction.deviceId;
    anomaly.timestamp = prediction.timestamp;
    SAIM_CHECK(device.applyAnalysis(prediction, anomaly));
    SAIM_CHECK(device.state() == DeviceState::Fault);
    const auto count = device.alarmCount();
    QSignalSpy states(&device, &Device::statusChanged);
    prediction.healthScore = 100;
    prediction.failureProbability = 0;
    prediction.faultType = QStringLiteral("None");
    anomaly.level = AnomalyLevel::High;
    SAIM_CHECK(device.applyAnalysis(prediction, anomaly));
    SAIM_CHECK(device.state() == DeviceState::Fault);
    SAIM_CHECK(states.isEmpty());
    SAIM_CHECK(device.alarmCount() == count);
    anomaly.level = AnomalyLevel::None;
    SAIM_CHECK(device.applyAnalysis(prediction, anomaly));
    SAIM_CHECK(device.state() == DeviceState::Normal);
    SAIM_CHECK(states.count() == 1);
    device.onSampleReceived(makeSample(QStringLiteral("M1"), 95, 220, 8, 0.1));
    SAIM_CHECK(device.applyAnalysis(prediction, anomaly));
    SAIM_CHECK(device.state() == DeviceState::Fault); // healthy AI cannot override physical limits
}

SAIM_TEST(misrouted_or_undated_analysis_is_rejected_without_partial_update)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    FaultPrediction prediction;
    prediction.deviceId = QStringLiteral("M1");
    prediction.timestamp = QDateTime::currentDateTime();
    prediction.faultType = QStringLiteral("None");
    AnomalyEvent anomaly;
    anomaly.deviceId = QStringLiteral("M2");
    anomaly.timestamp = prediction.timestamp;
    anomaly.level = AnomalyLevel::High;
    QSignalSpy predictions(&device, &Device::predictionUpdated);
    SAIM_CHECK(!device.applyAnalysis(prediction, anomaly));
    SAIM_CHECK(device.prediction().faultType.isEmpty() && predictions.isEmpty());
    device.onAnomalyDetected(anomaly);
    prediction.deviceId = QStringLiteral("M2");
    device.onPredictionReady(prediction);
    SAIM_CHECK(predictions.isEmpty() && device.alarmCount() == 0);
    prediction.deviceId = anomaly.deviceId = QStringLiteral("M1");
    anomaly.timestamp = {};
    SAIM_CHECK(!device.applyAnalysis(prediction, anomaly));
    SAIM_CHECK(device.state() == DeviceState::Normal);
}

SAIM_TEST(new_telemetry_session_does_not_inherit_old_diagnostics)
{
    Device device(QStringLiteral("M1"));
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    FaultPrediction prediction;
    prediction.deviceId = QStringLiteral("M1");
    prediction.timestamp = QDateTime::currentDateTime();
    prediction.faultType = QStringLiteral("Bearing Failure");
    prediction.healthScore = 25;
    prediction.failureProbability = 0.9;
    device.onPredictionReady(prediction);
    SAIM_CHECK(device.state() == DeviceState::Fault);
    device.checkLiveness(0);
    SAIM_CHECK(device.prediction().faultType.isEmpty());
    const auto alarms = device.alarmCount();
    device.onSampleReceived(normalSample(QStringLiteral("M1")));
    SAIM_CHECK(device.state() == DeviceState::Normal);
    SAIM_CHECK(device.alarmCount() == alarms);
    SAIM_CHECK(device.latest().isValid() && device.historySize() == 2);
}

int main()
{
    // A QCoreApplication is needed because Device uses QDateTime (which is
    // fine without one) and because QSignalSpy requires the Qt meta-object
    // system to be initialised.  No event loop is run: every signal in this
    // suite is a direct connection, which is the point - Device is UI-affine.
    QCoreApplication app(::saim::test::argcRef(), saim::test::argvRef());
    return saim::test::runAll();
}
