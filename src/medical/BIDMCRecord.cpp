#include "medical/BIDMCRecord.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <stdexcept>

static void initialiseBIDMCProvenance() { Q_INIT_RESOURCE(bidmc_provenance); }

namespace saim::medical {
namespace {
const QHash<QString, QString>& publisherHashes()
{
    static const auto hashes = [] {
        initialiseBIDMCProvenance();
        QFile manifest(QStringLiteral(":/medical/SHA256SUMS-bidmc-1.0.0.txt"));
        QHash<QString, QString> result;
        if (!manifest.open(QIODevice::ReadOnly)) { return result; }
        for (const auto& line : QString::fromUtf8(manifest.readAll()).split(QLatin1Char('\n')))
        {
            const auto fields = line.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (fields.size() == 2) { result.insert(fields[1], fields[0]); }
        }
        return result;
    }();
    return hashes;
}

struct Csv
{
    QStringList header;
    QVector<QStringList> rows;
    QString hash;
};

Csv readCsv(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error((path + ": " + file.errorString()).toStdString()); }
    constexpr qint64 maxBytes = 16 * 1024 * 1024;
    if (file.size() > maxBytes) { throw std::runtime_error("CSV exceeds 16 MiB import limit"); }
    const auto bytes = file.read(maxBytes + 1);
    if (file.error() != QFile::NoError || bytes.size() > maxBytes) { throw std::runtime_error("CSV read failed or limit exceeded"); }
    Csv csv;
    csv.hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const auto lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    for (QString line : lines)
    {
        line = line.trimmed();
        if (line.isEmpty()) { continue; }
        if (line.size() > 4096 || csv.rows.size() > 225001) { throw std::runtime_error("CSV row limit exceeded"); }
        auto fields = line.split(QLatin1Char(','), Qt::KeepEmptyParts);
        for (auto& field : fields) { field = field.trimmed(); }
        if (csv.header.isEmpty())
        {
            if (fields.front().startsWith(QChar(0xfeff))) { fields.front().remove(0, 1); }
            csv.header = fields;
            for (const auto& field : fields)
            {
                if (fields.count(field) != 1) { throw std::runtime_error("Duplicate CSV column"); }
            }
        }
        else
        {
            if (fields.size() != csv.header.size()) { throw std::runtime_error("CSV row width differs from header"); }
            csv.rows.push_back(std::move(fields));
        }
    }
    if (csv.rows.isEmpty()) { throw std::runtime_error("CSV contains no data rows"); }
    return csv;
}

qsizetype column(const Csv& csv, const QString& name)
{
    const auto index = csv.header.indexOf(name);
    if (index < 0) { throw std::runtime_error(("Missing CSV column: " + name).toStdString()); }
    return index;
}

double number(const QString& field, bool allowMissing)
{
    if (allowMissing && (field.isEmpty() || field.compare(QLatin1String("NaN"), Qt::CaseInsensitive) == 0))
    { return std::numeric_limits<double>::quiet_NaN(); }
    bool ok = false;
    const double value = field.toDouble(&ok);
    if (!ok || !std::isfinite(value)) { throw std::runtime_error(("Invalid finite CSV number: " + field).toStdString()); }
    return value;
}
} // namespace

Result<BIDMCRecord> loadBIDMCRecord(const QString& directory, const QString& id)
{
    if (!QRegularExpression(QStringLiteral("^[0-9]{2}$")).match(id).hasMatch()
        || id.toInt() < 1 || id.toInt() > 53)
    { return Result<BIDMCRecord>::fail(1, QStringLiteral("BIDMC record id must be 01 through 53")); }
    try
    {
        const QDir folder(directory);
        const QString prefix = QStringLiteral("bidmc_%1_").arg(id);
        const Csv waveCsv = readCsv(folder.filePath(prefix + QStringLiteral("Signals.csv")));
        const Csv numerics = readCsv(folder.filePath(prefix + QStringLiteral("Numerics.csv")));
        const Csv breaths = readCsv(folder.filePath(prefix + QStringLiteral("Breaths.csv")));
        BIDMCRecord record;
        record.id = id;
        record.directory = folder.absolutePath();
        record.sha256 = {waveCsv.hash, numerics.hash, breaths.hash};
        const auto timeColumn = column(waveCsv, QStringLiteral("Time [s]"));
        const std::array<qsizetype, 3> channels{column(waveCsv, QStringLiteral("II")),
            column(waveCsv, QStringLiteral("PLETH")), column(waveCsv, QStringLiteral("RESP"))};
        for (const auto& row : waveCsv.rows)
        {
            WaveformSample sample;
            const double csvTime = number(row[timeColumn], false);
            const double expected = static_cast<double>(record.waveforms.size()) / kSampleRate;
            // Publisher CSV rounds times to five significant digits: e.g.
            // sample 12501 is serialized as 100.01 instead of 100.008. Check
            // that documented clock within its serialization precision, then
            // reconstruct exact 125 Hz time from the sample index, not rounded
            // differences (which can even be zero at late timestamps).
            const double tolerance = expected > 0.0
                ? std::max(1e-9, 0.500001 * std::pow(10.0, std::floor(std::log10(expected)) - 4.0)) : 1e-9;
            if (std::abs(csvTime - expected) > tolerance)
            { throw std::runtime_error("Waveform timeline is not a contiguous 125 Hz record starting at zero"); }
            sample.seconds = expected;
            for (std::size_t i = 0; i < channels.size(); ++i)
            {
                sample.values[i] = number(row[channels[i]], true);
                if (!std::isfinite(sample.values[i])) { ++record.missingWaveformValues; }
            }
            record.waveforms.push_back(sample);
        }
        if (record.waveforms.size() < 2) { throw std::runtime_error("Waveform record is too short"); }
        const auto numericTime = column(numerics, QStringLiteral("Time [s]"));
        const std::array<qsizetype, 4> numericChannels{column(numerics, QStringLiteral("HR")),
            column(numerics, QStringLiteral("PULSE")), column(numerics, QStringLiteral("RESP")), column(numerics, QStringLiteral("SpO2"))};
        const double numericStart = number(numerics.rows.front()[numericTime], false);
        // Original record 23 starts at -1 s and ends at 479 s. Preserve its
        // offset; shifting it to zero would misalign the monitor values.
        if (std::floor(numericStart) != numericStart || numericStart < -1.0 || numericStart > 1.0)
        { throw std::runtime_error("Unsupported numeric time origin"); }
        for (const auto& row : numerics.rows)
        {
            NumericSample sample;
            sample.seconds = number(row[numericTime], false);
            if (std::abs(sample.seconds - (numericStart + static_cast<double>(record.numerics.size()))) > 0.00005
                || sample.seconds > record.duration() + 0.00005)
            { throw std::runtime_error("Numeric timeline is not aligned contiguous 1 Hz data"); }
            for (std::size_t i = 0; i < numericChannels.size(); ++i)
            {
                sample.values[i] = number(row[numericChannels[i]], true);
                if (!std::isfinite(sample.values[i])) { ++record.missingNumericValues; }
            }
            record.numerics.push_back(sample);
        }
        const std::array<qsizetype, 2> annotatorColumns{
            column(breaths, QStringLiteral("breaths ann1 [signal sample no]")),
            column(breaths, QStringLiteral("breaths ann2 [signal sample no]"))};
        for (const auto& row : breaths.rows)
        {
            for (std::size_t i = 0; i < annotatorColumns.size(); ++i)
            {
                const double index = number(row[annotatorColumns[i]], true);
                if (!std::isfinite(index)) { continue; }
                if (index < 1.0 || std::floor(index) != index || index > static_cast<double>(record.waveforms.size()))
                { throw std::runtime_error("Breath annotation sample index outside recording"); }
                // CSV uses MATLAB one-based signal sample numbers.
                const double seconds = (index - 1.0) / kSampleRate;
                if (!record.breaths[i].isEmpty() && seconds < record.breaths[i].back())
                { ++record.annotationOrderInversions[i]; }
                record.breaths[i].push_back(seconds);
            }
        }
        if (record.breaths[0].isEmpty() || record.breaths[1].isEmpty())
        { throw std::runtime_error("Both independent breath annotation streams are required"); }
        for (std::size_t i = 0; i < record.breaths.size(); ++i)
        {
            auto& annotations = record.breaths[i];
            std::sort(annotations.begin(), annotations.end());
            for (qsizetype j = 1; j < annotations.size(); ++j)
            {
                if (annotations[j] == annotations[j - 1])
                {
                    ++record.duplicateAnnotations[i];
                    record.ambiguousAnnotationTimes.push_back(annotations[j]);
                }
            }
            annotations.erase(std::unique(annotations.begin(), annotations.end()), annotations.end());
        }
        const std::array<QString, 3> suffixes{QStringLiteral("Signals.csv"), QStringLiteral("Numerics.csv"), QStringLiteral("Breaths.csv")};
        for (std::size_t i = 0; i < suffixes.size(); ++i)
        {
            const QString name = QStringLiteral("bidmc_csv/%1%2").arg(prefix, suffixes[i]);
            if (publisherHashes().value(name) != record.sha256[static_cast<qsizetype>(i)])
            { throw std::runtime_error(("Publisher SHA-256 mismatch: " + name).toStdString()); }
        }
        return record;
    }
    catch (const std::exception& exception)
    { return Result<BIDMCRecord>::fail(2, QString::fromUtf8(exception.what())); }
}

QStringList availableBIDMCRecords(const QString& directory)
{
    const QDir folder(directory);
    QStringList ids;
    for (int i = 1; i <= 53; ++i)
    {
        const QString id = QStringLiteral("%1").arg(i, 2, 10, QLatin1Char('0'));
        const QString prefix = QStringLiteral("bidmc_%1_").arg(id);
        if (folder.exists(prefix + QStringLiteral("Signals.csv"))
            && folder.exists(prefix + QStringLiteral("Numerics.csv"))
            && folder.exists(prefix + QStringLiteral("Breaths.csv"))) { ids << id; }
    }
    return ids;
}
} // namespace saim::medical
