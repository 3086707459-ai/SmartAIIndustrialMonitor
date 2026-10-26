#include "medical/RespiratoryAnalysis.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("BIDMC RESP baseline evaluation against two manual annotators"));
    parser.addHelpOption();
    parser.addOption({"data", "Directory containing original BIDMC CSV files", "directory"});
    parser.addOption({"output", "Write machine-readable JSON evidence", "path"});
    parser.addOption({"require-records", "Fail unless exactly this many complete records are available", "count", "53"});
    parser.addOption({"manifest", "Publisher SHA256SUMS.txt snapshot", "path"});
    parser.process(app);
    bool countOk = false;
    const int required = parser.value("require-records").toInt(&countOk);
    if (!parser.isSet("data") || !parser.isSet("output") || !countOk || required < 1 || required > 53)
    { QTextStream(stderr) << "--data, --output and a valid --require-records (1..53) are required\n"; return 2; }
    const auto ids = saim::medical::availableBIDMCRecords(parser.value("data"));
    if (ids.size() != required)
    { QTextStream(stderr) << "Incomplete benchmark corpus: expected " << required << ", found " << ids.size() << '\n'; return 3; }
    QString manifestPath = parser.value("manifest");
    if (manifestPath.isEmpty())
    {
        const QString relative = QStringLiteral("datasets/SHA256SUMS-bidmc-1.0.0.txt");
        const QString packaged = QDir(app.applicationDirPath()).filePath(relative);
        manifestPath = QFile::exists(packaged) ? packaged : QDir::current().filePath(relative);
    }
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly)) { QTextStream(stderr) << "Publisher manifest is required\n"; return 8; }
    const auto manifestBytes = manifest.readAll();
    QHash<QString, QString> publisherHashes;
    for (const auto& line : QString::fromUtf8(manifestBytes).split(QLatin1Char('\n')))
    {
        const auto fields = line.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (fields.size() == 2) { publisherHashes.insert(fields[1], fields[0]); }
    }
    QJsonArray records;
    int total = 0, accepted = 0;
    double sumAbsolute = 0.0, sumSquared = 0.0, sumBias = 0.0;
    double recordMae = 0.0;
    int recordsWithEstimates = 0;
    for (const auto& id : ids)
    {
        const auto loaded = saim::medical::loadBIDMCRecord(parser.value("data"), id);
        if (loaded.isError()) { QTextStream(stderr) << id << ": " << loaded.error().message << '\n'; return 4; }
        auto report = saim::medical::respiratoryReport(loaded.value());
        const std::array<QString, 3> suffixes{QStringLiteral("Signals.csv"), QStringLiteral("Numerics.csv"), QStringLiteral("Breaths.csv")};
        for (std::size_t i = 0; i < suffixes.size(); ++i)
        {
            const QString name = QStringLiteral("bidmc_csv/bidmc_%1_%2").arg(id, suffixes[i]);
            if (publisherHashes.value(name) != loaded.value().sha256[static_cast<qsizetype>(i)])
            { QTextStream(stderr) << "Publisher SHA-256 mismatch: " << name << '\n'; return 9; }
        }
        if (loaded.value().waveforms.size() != 60001 || loaded.value().numerics.size() != 481)
        { QTextStream(stderr) << id << ": unexpected source row counts\n"; return 5; }
        total += report.value("total_windows").toInt();
        accepted += report.value("accepted_windows").toInt();
        for (const auto& item : report.value("windows").toArray())
        {
            const auto row = item.toObject();
            if (!row.value("error_bpm").isDouble()) { continue; }
            const double error = row.value("error_bpm").toDouble();
            sumAbsolute += std::abs(error); sumSquared += error * error; sumBias += error;
        }
        if (report.value("mae_bpm").isDouble()) { recordMae += report.value("mae_bpm").toDouble(); ++recordsWithEstimates; }
        QTextStream(stdout) << "BIDMC " << id << ": " << report.value("accepted_windows").toInt()
                            << "/" << report.value("total_windows").toInt() << " windows, MAE "
                            << report.value("mae_bpm").toDouble() << " breaths/min\n";
        records.append(report);
    }
    const auto ratio = [](double sum, int count) -> QJsonValue { return count > 0 ? QJsonValue(sum / count) : QJsonValue(QJsonValue::Null); };
    QJsonObject output{{"schema_version", 1}, {"dataset", "BIDMC 1.0.0"},
        {"evaluated_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"qt_version", QString::fromLatin1(qVersion())},
        {"publisher_hashes_verified", true},
        {"publisher_manifest_sha256", QString::fromLatin1(QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex())},
        {"source", QString::fromLatin1(saim::medical::kDatasetUrl)},
        {"method", "fixed RESP autocorrelation baseline v1; no fitted model or training split"},
        {"scope", "Retrospective baseline evaluation, not clinical validation; evaluates impedance RESP, not PPG-derived respiration"},
        {"record_count", ids.size()}, {"total_windows", total}, {"accepted_windows", accepted},
        {"coverage", total > 0 ? static_cast<double>(accepted) / total : 0.0},
        {"micro_mae_bpm", ratio(sumAbsolute, accepted)},
        {"micro_rmse_bpm", accepted > 0 ? QJsonValue(std::sqrt(sumSquared / accepted)) : QJsonValue(QJsonValue::Null)},
        {"micro_bias_bpm", ratio(sumBias, accepted)}, {"macro_mae_bpm", ratio(recordMae, recordsWithEstimates)},
        {"records", records}};
    QSaveFile file(parser.value("output"));
    const auto outputBytes = QJsonDocument(output).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(outputBytes) != outputBytes.size() || !file.commit())
    { QTextStream(stderr) << "Cannot save benchmark JSON\n"; return 6; }
    QTextStream(stdout) << "Evaluated " << ids.size() << " records; coverage " << output.value("coverage").toDouble()
                        << ", micro MAE " << output.value("micro_mae_bpm").toDouble() << " breaths/min\n";
    return accepted > 0 ? 0 : 7;
}
