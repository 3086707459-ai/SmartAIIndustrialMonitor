#pragma once

#include "common/Result.h"
#include <QVector>
#include <QStringList>
#include <array>
#include <limits>

namespace saim::medical {

inline constexpr double kSampleRate = 125.0;
inline constexpr auto kDatasetUrl = "https://physionet.org/content/bidmc/1.0.0/";

struct WaveformSample
{
    double seconds = 0.0;
    // Original CSV amplitudes, without rescaling or inventing physical units.
    std::array<double, 3> values{}; // II ECG, PLETH PPG, impedance RESP
};

struct NumericSample
{
    double seconds = 0.0;
    std::array<double, 4> values{}; // HR, PULSE, RESP, SpO2; NaN stays missing
};

struct BIDMCRecord
{
    QString id;
    QString directory;
    QVector<WaveformSample> waveforms;
    QVector<NumericSample> numerics;
    std::array<QVector<double>, 2> breaths; // seconds; independent annotators
    std::array<int, 2> annotationOrderInversions{};
    std::array<int, 2> duplicateAnnotations{};
    QVector<double> ambiguousAnnotationTimes; // duplicate-event windows excluded from scoring
    QStringList sha256; // Signals, Numerics, Breaths, in that order
    int missingWaveformValues = 0;
    int missingNumericValues = 0;

    [[nodiscard]] double duration() const { return waveforms.isEmpty() ? 0.0 : waveforms.back().seconds; }
};

// Loads the three original CSV files transactionally. No synthetic fallback.
// A malformed field is an error; an explicitly missing NaN/empty field remains NaN.
[[nodiscard]] Result<BIDMCRecord> loadBIDMCRecord(const QString& directory, const QString& id);
[[nodiscard]] QStringList availableBIDMCRecords(const QString& directory);

} // namespace saim::medical
