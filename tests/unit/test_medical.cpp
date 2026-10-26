#include "TestHarness.h"
#include "medical/RespiratoryAnalysis.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>

using namespace saim::medical;
namespace {
QString source() { return QStringLiteral(SAIM_MEDICAL_FIXTURE); }
BIDMCRecord realRecord()
{
    const auto loaded = loadBIDMCRecord(source(), QStringLiteral("01"));
    SAIM_CHECK_MSG(loaded.isOk(), loaded.isError() ? loaded.error().message.toStdString() : "");
    return loaded.value();
}
void copyRecord(const QString& destination)
{
    for (const auto& suffix : {"Signals.csv", "Numerics.csv", "Breaths.csv"})
    {
        const QString name = QStringLiteral("bidmc_01_%1").arg(QString::fromLatin1(suffix));
        SAIM_CHECK(QFile::copy(QDir(source()).filePath(name), QDir(destination).filePath(name)));
    }
}
void replaceText(const QString& path, const QByteArray& before, const QByteArray& after)
{
    QFile file(path);
    SAIM_CHECK(file.open(QIODevice::ReadOnly));
    auto bytes = file.readAll(); file.close();
    SAIM_CHECK(bytes.contains(before)); bytes.replace(before, after);
    SAIM_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    SAIM_CHECK(file.write(bytes) == bytes.size());
}
}

SAIM_TEST(original_bidmc_record_has_publisher_hashes_and_exact_timeline)
{
    const auto record = realRecord();
    SAIM_CHECK(record.waveforms.size() == 60001);
    SAIM_CHECK(record.numerics.size() == 481);
    SAIM_CHECK(record.breaths[0].size() == 170);
    SAIM_CHECK(record.breaths[1].size() == 170);
    SAIM_CHECK_NEAR(record.duration(), 480.0, 1e-12);
    SAIM_CHECK(record.sha256[0] == QStringLiteral("21495169cc36e82352d8e12cc6430927946d02d240b030715088d02396ea2d5c"));
    SAIM_CHECK(record.sha256[1] == QStringLiteral("27d43794cf2e7150db10e8bfe61d2bfdb26873a8f1629ffc5fbee5c9a4eb6ef5"));
    SAIM_CHECK(record.sha256[2] == QStringLiteral("698846517e27e7789d855875bae9774db3c189900efae8e486c300ed5ce58037"));
    SAIM_CHECK_NEAR(record.waveforms[0].values[0], 0.72549, 1e-12);
    SAIM_CHECK_NEAR(record.waveforms[0].values[1], 0.43597, 1e-12);
    SAIM_CHECK_NEAR(record.waveforms[0].values[2], 0.35386, 1e-12);
}

SAIM_TEST(real_missing_monitor_values_stay_missing)
{
    const auto record = realRecord();
    SAIM_CHECK_NEAR(record.numerics[111].values[0], 92.0, 1e-12);
    SAIM_CHECK(std::isnan(record.numerics[111].values[1]));
    SAIM_CHECK(std::isnan(record.numerics[111].values[3]));
    SAIM_CHECK(record.missingNumericValues > 0);
    SAIM_CHECK(record.missingWaveformValues == 0);
}

SAIM_TEST(manual_annotations_use_one_based_samples_and_separate_streams)
{
    const auto record = realRecord();
    // Original CSV: 189 / 187. WFDB counterpart: 188 / 186, zero-based.
    SAIM_CHECK_NEAR(record.breaths[0][0], 188.0 / 125.0, 1e-12);
    SAIM_CHECK_NEAR(record.breaths[1][0], 186.0 / 125.0, 1e-12);
    const auto report = respiratoryReport(record);
    SAIM_CHECK(report.value("total_windows").toInt() == 16);
    const auto first = report.value("windows").toArray().first().toObject();
    SAIM_CHECK_NEAR(first.value("ann1_bpm").toDouble(), 24.0, 1e-12);
    SAIM_CHECK_NEAR(first.value("ann2_bpm").toDouble(), 24.0, 1e-12);
}

SAIM_TEST(baseline_estimates_are_independent_of_reference_annotations)
{
    auto record = realRecord();
    const auto before = analyseRespiration(record);
    record.breaths = {};
    const auto after = analyseRespiration(record);
    SAIM_CHECK(before.size() == after.size());
    int assessed = 0;
    for (qsizetype i = 0; i < before.size(); ++i)
    {
        SAIM_CHECK(before[i].estimatedRate == after[i].estimatedRate);
        if (before[i].estimatedRate) { ++assessed; }
        SAIM_CHECK(after[i].annotationRates[0] == 0.0);
    }
    SAIM_CHECK(assessed > 0);
}

SAIM_TEST(missing_and_flat_resp_windows_abstain_instead_of_guessing)
{
    auto record = realRecord();
    record.waveforms[5].values[2] = std::numeric_limits<double>::quiet_NaN();
    for (qsizetype i = 3750; i < 7500; ++i) { record.waveforms[i].values[2] = 0.25; }
    const auto windows = analyseRespiration(record);
    SAIM_CHECK(!windows[0].estimatedRate);
    SAIM_CHECK(windows[0].rejection == QStringLiteral("missing RESP waveform"));
    SAIM_CHECK(!windows[1].estimatedRate);
    SAIM_CHECK(windows[1].rejection == QStringLiteral("flat RESP waveform"));
    const auto report = respiratoryReport(record);
    SAIM_CHECK(report.value("coverage").toDouble() <= 14.0 / 16.0);
    SAIM_CHECK(report.value("windows").toArray()[0].toObject().value("estimate_bpm").isNull());
}

SAIM_TEST(import_rejects_broken_time_axis_without_partial_record)
{
    QTemporaryDir scratch; SAIM_CHECK(scratch.isValid()); copyRecord(scratch.path());
    replaceText(QDir(scratch.path()).filePath(QStringLiteral("bidmc_01_Signals.csv")), "0.008,", "0.009,");
    const auto loaded = loadBIDMCRecord(scratch.path(), QStringLiteral("01"));
    SAIM_CHECK(loaded.isError());
    SAIM_CHECK(loaded.error().message.contains(QStringLiteral("125 Hz")));
}

SAIM_TEST(import_rejects_malformed_fields_instead_of_coercing_to_zero)
{
    QTemporaryDir scratch; SAIM_CHECK(scratch.isValid()); copyRecord(scratch.path());
    replaceText(QDir(scratch.path()).filePath(QStringLiteral("bidmc_01_Numerics.csv")), "0,94,93,25,97", "0,garbage,93,25,97");
    SAIM_CHECK(loadBIDMCRecord(scratch.path(), QStringLiteral("01")).isError());
}

SAIM_TEST(import_requires_all_three_files_and_safe_record_identifier)
{
    QTemporaryDir scratch; SAIM_CHECK(scratch.isValid());
    SAIM_CHECK(availableBIDMCRecords(scratch.path()).isEmpty());
    SAIM_CHECK(loadBIDMCRecord(scratch.path(), QStringLiteral("01")).isError());
    SAIM_CHECK(loadBIDMCRecord(source(), QStringLiteral("../01")).isError());
    SAIM_CHECK(loadBIDMCRecord(source(), QStringLiteral("54")).isError());
    SAIM_CHECK(availableBIDMCRecords(source()) == QStringList{QStringLiteral("01")});
}

SAIM_TEST(import_rejects_modified_but_well_formed_source_by_publisher_hash)
{
    QTemporaryDir scratch; SAIM_CHECK(scratch.isValid()); copyRecord(scratch.path());
    replaceText(QDir(scratch.path()).filePath(QStringLiteral("bidmc_01_Signals.csv")), "0.72549", "0.72548");
    const auto loaded = loadBIDMCRecord(scratch.path(), QStringLiteral("01"));
    SAIM_CHECK(loaded.isError());
    SAIM_CHECK(loaded.error().message.contains(QStringLiteral("Publisher SHA-256 mismatch")));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    return saim::test::runAll();
}
