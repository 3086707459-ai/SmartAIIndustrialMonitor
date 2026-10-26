#include "medical/RespiratoryAnalysis.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace saim::medical {
namespace {
constexpr int kWindowSamples = 3750;
constexpr int kDecimation = 5;
constexpr double kAnalysisHz = 25.0;
constexpr int kMinLag = 25;
constexpr int kMaxLag = 250;

void estimate(const BIDMCRecord& record, qsizetype first, RespiratoryWindow& window)
{
    std::vector<double> values;
    values.reserve(kWindowSamples / kDecimation);
    for (int i = 0; i < kWindowSamples; i += kDecimation)
    {
        double sum = 0.0;
        for (int j = 0; j < kDecimation; ++j)
        {
            const double value = record.waveforms[first + i + j].values[2];
            if (!std::isfinite(value)) { window.rejection = QStringLiteral("missing RESP waveform"); return; }
            sum += value;
        }
        values.push_back(sum / kDecimation);
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    for (auto& value : values) { value -= mean; }
    const double energy = std::inner_product(values.begin(), values.end(), values.begin(), 0.0);
    if (energy < 1e-12) { window.rejection = QStringLiteral("flat RESP waveform"); return; }
    std::array<double, kMaxLag + 2> correlations{};
    for (int lag = kMinLag - 1; lag <= kMaxLag + 1; ++lag)
    {
        double product = 0.0, left = 0.0, right = 0.0;
        for (std::size_t i = static_cast<std::size_t>(lag); i < values.size(); ++i)
        {
            const double a = values[i], b = values[i - static_cast<std::size_t>(lag)];
            product += a * b; left += a * a; right += b * b;
        }
        correlations[static_cast<std::size_t>(lag)] = left * right > 1e-20 ? product / std::sqrt(left * right) : 0.0;
    }
    double best = 0.0;
    for (int lag = kMinLag; lag <= kMaxLag; ++lag)
    {
        const auto i = static_cast<std::size_t>(lag);
        if (correlations[i] >= correlations[i - 1] && correlations[i] > correlations[i + 1])
        { best = std::max(best, correlations[i]); }
    }
    window.periodicity = best;
    if (best < 0.5) { window.rejection = QStringLiteral("insufficient periodicity"); return; }
    for (int lag = kMinLag; lag <= kMaxLag; ++lag)
    {
        const auto i = static_cast<std::size_t>(lag);
        if (correlations[i] >= 0.9 * best && correlations[i] >= correlations[i - 1]
            && correlations[i] > correlations[i + 1])
        {
            window.estimatedRate = 60.0 * kAnalysisHz / lag;
            return;
        }
    }
}
} // namespace

QVector<RespiratoryWindow> analyseRespiration(const BIDMCRecord& record)
{
    QVector<RespiratoryWindow> windows;
    // The final endpoint sample at 480 s belongs to no complete following window.
    for (qsizetype first = 0; first + kWindowSamples <= record.waveforms.size(); first += kWindowSamples)
    {
        RespiratoryWindow window;
        window.start = static_cast<double>(first) / kSampleRate;
        window.end = window.start + 30.0;
        estimate(record, first, window);
        for (double time : record.ambiguousAnnotationTimes)
        {
            if (time >= window.start && time < window.end)
            {
                window.referenceValid = false;
                window.rejection = QStringLiteral("duplicate manual breath annotation in reference window");
            }
        }
        for (std::size_t i = 0; i < record.breaths.size(); ++i)
        {
            const auto& annotations = record.breaths[i];
            const auto begin = std::lower_bound(annotations.begin(), annotations.end(), window.start);
            const auto end = std::lower_bound(annotations.begin(), annotations.end(), window.end);
            window.annotationRates[i] = static_cast<double>(end - begin) * 2.0;
        }
        windows.push_back(window);
    }
    return windows;
}

QJsonObject respiratoryReport(const BIDMCRecord& record)
{
    const auto windows = analyseRespiration(record);
    QJsonArray rows;
    double absolute = 0.0, squared = 0.0, bias = 0.0, agreement = 0.0;
    int accepted = 0;
    for (const auto& window : windows)
    {
        const double reference = (window.annotationRates[0] + window.annotationRates[1]) / 2.0;
        agreement += std::abs(window.annotationRates[0] - window.annotationRates[1]);
        QJsonObject row{{"start_seconds", window.start}, {"end_seconds", window.end},
            {"ann1_bpm", window.annotationRates[0]}, {"ann2_bpm", window.annotationRates[1]},
            {"reference_bpm", reference}, {"periodicity", window.periodicity}, {"rejection", window.rejection},
            {"reference_valid", window.referenceValid}};
        if (window.estimatedRate)
        {
            row.insert(QStringLiteral("estimate_bpm"), *window.estimatedRate);
            if (window.referenceValid)
            {
                const double error = *window.estimatedRate - reference;
                row.insert(QStringLiteral("error_bpm"), error);
                absolute += std::abs(error); squared += error * error; bias += error; ++accepted;
            }
        }
        else { row.insert(QStringLiteral("estimate_bpm"), QJsonValue::Null); }
        rows.append(row);
    }
    const auto metric = [accepted](double total) -> QJsonValue {
        return accepted > 0 ? QJsonValue(total / accepted) : QJsonValue(QJsonValue::Null);
    };
    return {{"schema_version", 1}, {"dataset", "BIDMC 1.0.0"}, {"record_id", record.id},
        {"source", QString::fromLatin1(kDatasetUrl)}, {"signal_rows", record.waveforms.size()},
        {"numeric_rows", record.numerics.size()}, {"duration_seconds", record.duration()},
        {"numeric_start_seconds", record.numerics.front().seconds},
        {"missing_numeric_values", record.missingNumericValues}, {"missing_waveform_values", record.missingWaveformValues},
        {"sha256", QJsonArray::fromStringList(record.sha256)},
        {"annotation_order_inversions", QJsonArray{record.annotationOrderInversions[0], record.annotationOrderInversions[1]}},
        {"duplicate_annotations", QJsonArray{record.duplicateAnnotations[0], record.duplicateAnnotations[1]}},
        {"annotation_policy", "sort annotation timestamps; deduplicate exact sample numbers for display; exclude duplicate-containing windows from error metrics"},
        {"method", "RESP autocorrelation baseline v1; fixed parameters; no training"},
        {"reference", "mean of two independent manual breath counts per [start,end) 30 s window, multiplied by 2"},
        {"timebase", "sample index / 125 Hz; original CSV timestamps validated within their five-significant-digit serialization precision"},
        {"total_windows", windows.size()}, {"accepted_windows", accepted},
        {"coverage", windows.isEmpty() ? 0.0 : static_cast<double>(accepted) / static_cast<double>(windows.size())},
        {"mae_bpm", metric(absolute)}, {"rmse_bpm", accepted > 0 ? QJsonValue(std::sqrt(squared / accepted)) : QJsonValue(QJsonValue::Null)},
        {"bias_bpm", metric(bias)}, {"annotator_mae_bpm", windows.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(agreement / static_cast<double>(windows.size()))},
        {"windows", rows}};
}
} // namespace saim::medical
