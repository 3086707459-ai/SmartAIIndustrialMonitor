#include "medical/MedicalWindow.h"
#include <QChart>
#include <QChartView>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QFontDatabase>
#include <QIcon>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLegend>
#include <QLineSeries>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScatterSeries>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

static void initialiseMedicalResources() { Q_INIT_RESOURCE(medsignal_assets); }

namespace saim::medical {
MedicalWindow::MedicalWindow(QWidget* parent) : QMainWindow(parent)
{
    initialiseMedicalResources();
    // Explicitly select an installed text font: Windows' offscreen platform
    // can otherwise choose a symbol-only fallback and render every label as boxes.
    const auto families = QFontDatabase::families();
    for (const auto& family : {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Segoe UI"), QStringLiteral("Arial")})
    {
        if (families.contains(family)) { setFont(QFont(family, 9)); break; }
    }
    setWindowTitle(tr("MedSignal Workbench · 医疗生理信号回放与分析"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/medsignal.svg")));
    setObjectName(QStringLiteral("MedicalWindow"));
    resize(1380, 900);
    setMinimumSize(1050, 760);
    setStyleSheet(QStringLiteral(
        "QMainWindow,QWidget#MedicalRoot{background:#edf3f7;color:#183344;}"
        "QLabel{color:#183344;} QLabel#Brand{font-size:25px;font-weight:700;}"
        "QLabel#Subtitle{color:#597586;font-size:12px;}"
        "QFrame#Sidebar,QFrame#NumericCard{background:white;border:1px solid #d7e4eb;border-radius:10px;}"
        "QLabel#MetricValue{font-size:27px;font-weight:700;color:#087f8c;}"
        "QPushButton{background:white;border:1px solid #b4cbd6;border-radius:5px;padding:7px 12px;color:#183344;}"
        "QPushButton:disabled{color:#9babb5;} QPushButton#PlaybackButton{background:#087f8c;color:white;border:0;}"
        "QComboBox{background:white;color:#183344;padding:6px;border:1px solid #b4cbd6;border-radius:4px;}"
        "QSlider::groove:horizontal{height:5px;background:#cfdee6;}"
        "QSlider::handle:horizontal{background:#087f8c;width:13px;margin:-5px 0;border-radius:6px;}"));
    auto* root = new QWidget(this);
    root->setObjectName(QStringLiteral("MedicalRoot"));
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(20, 14, 20, 14);
    layout->setSpacing(10);
    auto* title = new QLabel(tr("MedSignal Workbench"), root);
    title->setObjectName(QStringLiteral("Brand"));
    layout->addWidget(title);
    auto* subtitle = new QLabel(tr("医疗生理信号回放与分析  /  PhysioNet BIDMC · retrospective research workspace"), root);
    subtitle->setObjectName(QStringLiteral("Subtitle"));
    layout->addWidget(subtitle);
    auto* content = new QHBoxLayout;
    content->setSpacing(16);
    auto* sidebar = new QFrame(root);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(250);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(16, 16, 16, 16);
    side->setSpacing(12);
    auto* sourceTitle = new QLabel(tr("REAL-WORLD DATA"), sidebar);
    side->addWidget(sourceTitle);
    auto* citation = new QLabel(tr("<b>BIDMC PPG and Respiration</b><br>PhysioNet · version 1.0.0<br>53 recordings · 8 min each<br>125 Hz waveforms / 1 Hz numerics<br><br>"
        "Pimentel et al., IEEE TBME<br>doi:10.1109/TBME.2016.2613124<br><a href='https://physionet.org/content/bidmc/1.0.0/'>Dataset &amp; attribution license</a>"), sidebar);
    citation->setWordWrap(true);
    citation->setOpenExternalLinks(true);
    side->addWidget(citation);
    m_open = new QPushButton(tr("Open BIDMC folder…"), sidebar);
    m_open->setObjectName(QStringLiteral("OpenDataset"));
    side->addWidget(m_open);
    m_records = new QComboBox(sidebar);
    m_records->setObjectName(QStringLiteral("RecordSelector"));
    side->addWidget(m_records);
    m_source = new QLabel(tr("Open a directory containing the three original CSV files per record."), sidebar);
    m_source->setWordWrap(true);
    m_source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    side->addWidget(m_source);
    side->addSpacing(16);
    auto* method = new QLabel(tr("<b>RESP BASELINE</b><br>30 s non-overlapping windows<br>Autocorrelation · no trained model<br>Reference: two manual annotators<br><br>"
        "ECG / PPG amplitudes retain source CSV units. Numerics come from the original monitor. Missing values display as —."), sidebar);
    method->setWordWrap(true);
    side->addWidget(method);
    m_export = new QPushButton(tr("Export evaluation JSON…"), sidebar);
    m_export->setEnabled(false);
    side->addWidget(m_export);
    side->addStretch();
    auto* scope = new QLabel(tr("RESEARCH REPLAY\nHistorical de-identified recordings.\nNot for live patient monitoring or clinical decisions."), sidebar);
    scope->setObjectName(QStringLiteral("ResearchScope"));
    scope->setWordWrap(true);
    side->addWidget(scope);
    content->addWidget(sidebar);
    auto* workspace = new QVBoxLayout;
    workspace->setSpacing(8);
    auto* metrics = new QHBoxLayout;
    const std::array<QString, 4> captions{tr("HEART RATE  ·  bpm"), tr("PULSE RATE  ·  bpm"), tr("RESP RATE  ·  /min"), tr("SpO₂  ·  %")};
    const std::array<QString, 4> names{QStringLiteral("NumericHR"), QStringLiteral("NumericPULSE"), QStringLiteral("NumericRESP"), QStringLiteral("NumericSpO2")};
    for (std::size_t i = 0; i < captions.size(); ++i)
    {
        auto* card = new QFrame(root);
        card->setObjectName(QStringLiteral("NumericCard"));
        auto* box = new QVBoxLayout(card);
        auto* caption = new QLabel(captions[i], card);
        caption->setObjectName(QStringLiteral("Subtitle"));
        box->addWidget(caption);
        auto* value = new QLabel(QStringLiteral("—"), card);
        value->setObjectName(names[i]);
        value->setStyleSheet(QStringLiteral("font-size:27px;font-weight:700;color:#087f8c;"));
        m_values[i] = value;
        box->addWidget(value);
        metrics->addWidget(card, 1);
    }
    workspace->addLayout(metrics);
    const std::array<QString, 3> chartNames{tr("ECG · lead II"), tr("PPG · PLETH"), tr("RESP · impedance respiration")};
    const std::array<QColor, 3> colors{QColor("#087f8c"), QColor("#386bc0"), QColor("#7259aa")};
    for (std::size_t i = 0; i < chartNames.size(); ++i)
    {
        auto* chart = new QChart;
        chart->setTitle(chartNames[i]);
        chart->setMargins(QMargins(6, 0, 6, 0));
        chart->setBackgroundRoundness(8);
        chart->legend()->setVisible(i == 2);
        chart->legend()->setAlignment(Qt::AlignBottom);
        auto* series = new QLineSeries(chart);
        series->setName(tr("RESP"));
        series->setPen(QPen(colors[i], 1.5));
        chart->addSeries(series);
        auto* x = new QValueAxis(chart);
        x->setRange(0, 10); x->setLabelFormat(QStringLiteral("%.1f s")); x->setTickCount(6);
        auto* y = new QValueAxis(chart);
        y->setRange(0, 1); y->setLabelFormat(QStringLiteral("%.2f")); y->setTickCount(3);
        chart->addAxis(x, Qt::AlignBottom); chart->addAxis(y, Qt::AlignLeft);
        series->attachAxis(x); series->attachAxis(y);
        m_lines[i] = series; m_timeAxes[i] = x; m_valueAxes[i] = y;
        if (i == 2)
        {
            for (std::size_t a = 0; a < m_annotations.size(); ++a)
            {
                auto* markers = new QScatterSeries(chart);
                markers->setName(tr("Annotator %1").arg(a + 1));
                markers->setMarkerSize(6);
                markers->setColor(a == 0 ? QColor("#d14878") : QColor("#d39428"));
                markers->setMarkerShape(a == 0 ? QScatterSeries::MarkerShapeCircle : QScatterSeries::MarkerShapeRectangle);
                chart->addSeries(markers); markers->attachAxis(x); markers->attachAxis(y);
                m_annotations[a] = markers;
            }
        }
        auto* view = new QChartView(chart, root);
        view->setObjectName(QStringLiteral("Waveform%1").arg(i));
        view->setRenderHint(QPainter::Antialiasing);
        view->setMinimumHeight(145);
        workspace->addWidget(view, 1);
    }
    m_analysis = new QLabel(tr("Import a record to evaluate the fixed respiratory baseline against manual annotations."), root);
    m_analysis->setObjectName(QStringLiteral("AnalysisSummary"));
    m_analysis->setWordWrap(true);
    workspace->addWidget(m_analysis);
    auto* controls = new QHBoxLayout;
    m_play = new QPushButton(tr("Play"), root);
    m_play->setObjectName(QStringLiteral("PlaybackButton"));
    m_play->setEnabled(false);
    controls->addWidget(m_play);
    m_speed = new QComboBox(root);
    for (int rate : {1, 5, 10}) { m_speed->addItem(QStringLiteral("%1×").arg(rate), rate); }
    controls->addWidget(m_speed);
    m_positionSlider = new QSlider(Qt::Horizontal, root);
    m_positionSlider->setObjectName(QStringLiteral("PlaybackPosition"));
    m_positionSlider->setEnabled(false);
    controls->addWidget(m_positionSlider, 1);
    m_time = new QLabel(tr("0.0 / 0.0 s"), root);
    controls->addWidget(m_time);
    workspace->addLayout(controls);
    m_status = new QLabel(tr("Ready · select a verified BIDMC dataset folder"), root);
    m_status->setObjectName(QStringLiteral("ImportStatus"));
    m_status->setWordWrap(true);
    workspace->addWidget(m_status);
    content->addLayout(workspace, 1);
    layout->addLayout(content, 1);
    setCentralWidget(root);
    m_timer = new QTimer(this);
    m_timer->setInterval(40);
    connect(m_timer, &QTimer::timeout, this, &MedicalWindow::tick);
    connect(m_open, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(this, tr("Original BIDMC CSV directory"), m_directory);
        if (!directory.isEmpty()) { openDirectory(directory); }
    });
    connect(m_records, &QComboBox::currentIndexChanged, this, [this] { loadSelected(); });
    connect(m_play, &QPushButton::clicked, this, [this] { setPlaying(!m_playing); });
    connect(m_positionSlider, &QSlider::valueChanged, this, [this](int value) { seek(value / 1000.0); });
    connect(m_speed, &QComboBox::currentIndexChanged, this, [this] {
        tick(); m_rate = m_speed->currentData().toDouble(); m_anchor = m_position; m_clock.restart();
    });
    connect(m_export, &QPushButton::clicked, this, &MedicalWindow::exportReport);
    connect(&m_loader, &QFutureWatcher<Result<BIDMCRecord>>::finished, this, [this] {
        const auto loaded = m_loader.result();
        m_open->setEnabled(true); m_records->setEnabled(true);
        if (loaded.isError())
        {
            m_status->setText(tr("Import failed: %1").arg(loaded.error().message));
            emit importFailed(loaded.error().message); return;
        }
        m_record = loaded.value();
        m_report = respiratoryReport(*m_record);
        m_positionSlider->setRange(0, static_cast<int>(std::round(m_record->duration() * 1000.0)));
        m_positionSlider->setEnabled(true); m_play->setEnabled(true); m_export->setEnabled(true);
        m_source->setText(tr("BIDMC %1\n%2 waveform samples\n%3 numeric samples\n%4 / %5 annotated breaths\n\nSHA-256 (Signals):\n%6")
            .arg(m_record->id).arg(m_record->waveforms.size()).arg(m_record->numerics.size())
            .arg(m_record->breaths[0].size()).arg(m_record->breaths[1].size()).arg(m_record->sha256[0].left(16) + QStringLiteral("…")));
        m_source->setToolTip(m_record->directory + QLatin1Char('\n') + m_record->sha256.join(QLatin1Char('\n')));
        const auto mae = m_report.value("mae_bpm");
        m_analysis->setText(tr("RESP baseline · %1/%2 windows evaluated · MAE %3 breaths/min vs manual counts · no trained model")
            .arg(m_report.value("accepted_windows").toInt()).arg(m_report.value("total_windows").toInt())
            .arg(mae.isDouble() ? QString::number(mae.toDouble(), 'f', 2) : QStringLiteral("—")));
        seek(0.0);
        emit recordLoaded(m_record->id);
    });
}

void MedicalWindow::openDirectory(const QString& directory)
{
    if (m_loader.isRunning()) { return; }
    setPlaying(false);
    m_directory = QDir(directory).absolutePath();
    const auto ids = availableBIDMCRecords(m_directory);
    const QSignalBlocker blocker(m_records);
    m_records->clear();
    for (const auto& id : ids) { m_records->addItem(tr("BIDMC record %1").arg(id), id); }
    if (ids.isEmpty())
    {
        m_record.reset(); m_report = {};
        for (auto* line : m_lines) { line->clear(); }
        for (auto* markers : m_annotations) { markers->clear(); }
        for (auto* value : m_values) { value->setText(QStringLiteral("—")); }
        m_source->setText(tr("No record loaded")); m_analysis->clear();
        m_position = 0.0; m_positionSlider->setEnabled(false);
        m_play->setEnabled(false); m_export->setEnabled(false);
        m_time->setText(tr("0.0 / 0.0 s"));
        m_status->setText(tr("No complete BIDMC CSV triplets found. Run scripts/fetch_bidmc.py first."));
        emit importFailed(m_status->text()); return;
    }
    m_records->setCurrentIndex(0);
    loadSelected();
}

void MedicalWindow::loadSelected()
{
    if (m_loader.isRunning() || m_records->currentIndex() < 0) { return; }
    setPlaying(false);
    m_record.reset(); m_report = {};
    for (auto* line : m_lines) { line->clear(); }
    for (auto* markers : m_annotations) { markers->clear(); }
    for (auto* value : m_values) { value->setText(QStringLiteral("—")); }
    m_source->setText(tr("Reading original source files…"));
    m_analysis->clear();
    m_positionSlider->setEnabled(false); m_play->setEnabled(false); m_export->setEnabled(false);
    m_open->setEnabled(false); m_records->setEnabled(false);
    m_status->setText(tr("Importing and validating timestamps in a worker thread…"));
    const QString directory = m_directory, id = m_records->currentData().toString();
    m_loader.setFuture(QtConcurrent::run([directory, id] { return loadBIDMCRecord(directory, id); }));
}

void MedicalWindow::seek(double seconds)
{
    if (!m_record || !std::isfinite(seconds)) { return; }
    m_position = std::clamp(seconds, 0.0, m_record->duration());
    m_anchor = m_position;
    if (m_playing) { m_clock.restart(); }
    render();
}

void MedicalWindow::setPlaying(bool playing)
{
    if (!m_record) { playing = false; }
    if (playing && m_position >= m_record->duration()) { seek(0.0); }
    m_playing = playing;
    m_play->setText(playing ? tr("Pause") : tr("Play"));
    if (playing) { m_anchor = m_position; m_clock.start(); m_timer->start(); }
    else { m_timer->stop(); }
}

void MedicalWindow::tick()
{
    if (!m_playing || !m_record) { return; }
    m_position = std::min(m_record->duration(), m_anchor + static_cast<double>(m_clock.elapsed()) / 1000.0 * m_rate);
    render();
    if (m_position >= m_record->duration()) { setPlaying(false); }
}

void MedicalWindow::render()
{
    if (!m_record) { return; }
    const auto& record = *m_record;
    const double start = std::max(0.0, m_position - 10.0);
    const double end = std::max(10.0, m_position);
    const auto first = std::lower_bound(record.waveforms.begin(), record.waveforms.end(), start,
        [](const WaveformSample& sample, double time) { return sample.seconds < time; });
    const auto last = std::upper_bound(first, record.waveforms.end(), m_position,
        [](double time, const WaveformSample& sample) { return time < sample.seconds; });
    int gaps = 0;
    for (std::size_t channel = 0; channel < m_lines.size(); ++channel)
    {
        QList<QPointF> points;
        bool missing = false;
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (auto it = first; it != last; ++it)
        {
            const double value = it->values[channel];
            if (!std::isfinite(value)) { missing = true; continue; }
            points.append(QPointF(it->seconds, value)); low = std::min(low, value); high = std::max(high, value);
        }
        if (missing) { points.clear(); ++gaps; } // never connect a trace across missing samples
        m_lines[channel]->replace(points);
        m_timeAxes[channel]->setRange(start, end);
        if (!points.isEmpty())
        {
            const double padding = std::max(0.01, (high - low) * 0.12);
            m_valueAxes[channel]->setRange(low - padding, high + padding);
        }
    }
    for (std::size_t i = 0; i < m_annotations.size(); ++i)
    {
        QList<QPointF> points;
        const auto& annotations = record.breaths[i];
        for (auto it = std::lower_bound(annotations.begin(), annotations.end(), start);
             it != annotations.end() && *it <= m_position; ++it)
        {
            const auto index = static_cast<qsizetype>(std::llround(*it * kSampleRate));
            const double value = record.waveforms[index].values[2];
            if (std::isfinite(value)) { points.append(QPointF(*it, value)); }
        }
        m_annotations[i]->replace(points);
    }
    const auto numeric = std::upper_bound(record.numerics.begin(), record.numerics.end(), m_position,
        [](double time, const NumericSample& sample) { return time < sample.seconds; });
    for (std::size_t i = 0; i < m_values.size(); ++i)
    {
        const double value = numeric == record.numerics.begin() ? std::numeric_limits<double>::quiet_NaN() : (numeric - 1)->values[i];
        const bool fresh = numeric != record.numerics.begin() && m_position - (numeric - 1)->seconds < 1.0;
        m_values[i]->setText(fresh && std::isfinite(value) ? QString::number(value, 'f', 0) : QStringLiteral("—"));
    }
    const QSignalBlocker blocker(m_positionSlider);
    m_positionSlider->setValue(static_cast<int>(std::round(m_position * 1000.0)));
    m_time->setText(tr("%1 / %2 s").arg(m_position, 0, 'f', 1).arg(record.duration(), 0, 'f', 1));
    m_status->setText(tr("BIDMC %1 · publisher SHA-256 verified · %2 missing numeric values in recording%3")
        .arg(record.id).arg(record.missingNumericValues)
        .arg(gaps > 0 ? tr(" · waveform gap: affected trace hidden") : QString()));
}

QString MedicalWindow::reportJson() const { return QString::fromUtf8(QJsonDocument(m_report).toJson()); }

void MedicalWindow::exportReport()
{
    if (!m_record) { return; }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export baseline evaluation"),
        QStringLiteral("bidmc_%1_evaluation.json").arg(m_record->id), tr("JSON files (*.json)"));
    if (path.isEmpty()) { return; }
    QSaveFile file(path);
    const auto bytes = reportJson().toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    { QMessageBox::warning(this, tr("Export failed"), file.errorString()); }
}
} // namespace saim::medical
