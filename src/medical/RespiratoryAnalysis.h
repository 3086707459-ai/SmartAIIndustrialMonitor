#pragma once
#include "medical/BIDMCRecord.h"
#include <QJsonObject>
#include <optional>

namespace saim::medical {
struct RespiratoryWindow
{
    double start = 0.0;
    double end = 0.0;
    std::optional<double> estimatedRate;
    std::array<double, 2> annotationRates{};
    double periodicity = 0.0;
    QString rejection;
    bool referenceValid = true;
};

// Fixed, untrained baseline: 30 s non-overlapping windows, 25 Hz block mean,
// normalized autocorrelation, 6..60 breaths/min, minimum periodicity 0.5.
// Estimates use only RESP waveform; annotations are consulted afterwards.
[[nodiscard]] QVector<RespiratoryWindow> analyseRespiration(const BIDMCRecord& record);
[[nodiscard]] QJsonObject respiratoryReport(const BIDMCRecord& record);
} // namespace saim::medical
