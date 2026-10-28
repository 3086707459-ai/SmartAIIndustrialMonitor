#pragma once
#include "medical/RespiratoryAnalysis.h"

namespace saim::medical {
// Loaded and evaluated as one value in a worker; the GUI only publishes a
// complete document, so metadata, metrics and plots cannot refer to two records.
struct MedicalDocument
{
    BIDMCRecord record;
    QJsonObject report;
};

[[nodiscard]] Result<MedicalDocument> loadMedicalDocument(const QString& directory, const QString& id);

enum class EvaluationFormat { Json, Csv };

// Handles a single record report or the benchmark's aggregate "records" array.
// Missing numeric values are empty CSV fields, never fabricated zero values.
[[nodiscard]] QString evaluationCsv(const QJsonObject& report);
[[nodiscard]] Status saveEvaluationReport(const QJsonObject& report, const QString& path, EvaluationFormat format);
} // namespace saim::medical
