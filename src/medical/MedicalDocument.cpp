#include "medical/MedicalDocument.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

namespace saim::medical {
Result<MedicalDocument> loadMedicalDocument(const QString& directory, const QString& id)
{
    auto loaded = loadBIDMCRecord(directory, id);
    if (loaded.isError()) { return loaded.error(); }
    MedicalDocument document;
    document.record = std::move(loaded.value());
    document.report = respiratoryReport(document.record);
    document.report.insert(QStringLiteral("generated_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    return document;
}

namespace {
QString quoted(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

QString number(const QJsonObject& row, const QString& key)
{
    const auto value = row.value(key);
    return value.isDouble() && std::isfinite(value.toDouble()) ? QString::number(value.toDouble(), 'g', 17) : QString();
}
}

QString evaluationCsv(const QJsonObject& report)
{
    QString csv = QStringLiteral("record_id,start_seconds,end_seconds,estimate_bpm,ann1_bpm,ann2_bpm,reference_bpm,error_bpm,periodicity,scoring_status,signal_rejection,reference_rejection\n");
    const QJsonArray records = report.value(QStringLiteral("records")).isArray()
        ? report.value(QStringLiteral("records")).toArray() : QJsonArray{report};
    const QStringList numericKeys{QStringLiteral("start_seconds"), QStringLiteral("end_seconds"), QStringLiteral("estimate_bpm"),
        QStringLiteral("ann1_bpm"), QStringLiteral("ann2_bpm"), QStringLiteral("reference_bpm"),
        QStringLiteral("error_bpm"), QStringLiteral("periodicity")};
    for (const auto& item : records)
    {
        const auto record = item.toObject();
        for (const auto& window : record.value(QStringLiteral("windows")).toArray())
        {
            const auto row = window.toObject();
            QStringList fields{quoted(record.value(QStringLiteral("record_id")).toString())};
            for (const auto& key : numericKeys) { fields << number(row, key); }
            for (const auto& key : {"scoring_status", "signal_rejection", "reference_rejection"})
            { fields << quoted(row.value(QString::fromLatin1(key)).toString()); }
            csv += fields.join(QLatin1Char(',')) + QLatin1Char('\n');
        }
    }
    return csv;
}

Status saveEvaluationReport(const QJsonObject& report, const QString& path, EvaluationFormat format)
{
    if (!report.value(QStringLiteral("windows")).isArray() && !report.value(QStringLiteral("records")).isArray())
    { return Status::fail(1, QStringLiteral("No evaluation document is loaded")); }
    QSaveFile file(path);
    const auto bytes = format == EvaluationFormat::Csv ? evaluationCsv(report).toUtf8() : QJsonDocument(report).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    { return Status::fail(2, QStringLiteral("Could not save evaluation: %1").arg(file.errorString())); }
    return Status::ok();
}
} // namespace saim::medical
