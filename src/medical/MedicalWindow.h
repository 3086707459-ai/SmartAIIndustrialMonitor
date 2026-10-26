#pragma once
#include "medical/RespiratoryAnalysis.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QMainWindow>
#include <optional>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class QLineSeries;
class QScatterSeries;
class QValueAxis;

namespace saim::medical {
class MedicalWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MedicalWindow(QWidget* parent = nullptr);
    void openDirectory(const QString& directory);
    void seek(double seconds);
    void setPlaying(bool playing);
    [[nodiscard]] const BIDMCRecord* record() const { return m_record ? &*m_record : nullptr; }
    [[nodiscard]] double position() const { return m_position; }
    [[nodiscard]] bool isPlaying() const { return m_playing; }
    [[nodiscard]] bool isLoading() const { return m_loader.isRunning(); }
    [[nodiscard]] QString reportJson() const;
signals:
    void recordLoaded(const QString& id);
    void importFailed(const QString& message);
private:
    void loadSelected();
    void render();
    void exportReport();
    void tick();
    QString m_directory;
    std::optional<BIDMCRecord> m_record;
    QJsonObject m_report;
    QFutureWatcher<Result<BIDMCRecord>> m_loader;
    QComboBox* m_records = nullptr;
    QComboBox* m_speed = nullptr;
    QPushButton* m_open = nullptr;
    QPushButton* m_play = nullptr;
    QPushButton* m_export = nullptr;
    QSlider* m_positionSlider = nullptr;
    QLabel* m_source = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_time = nullptr;
    QLabel* m_analysis = nullptr;
    std::array<QLabel*, 4> m_values{};
    std::array<QLineSeries*, 3> m_lines{};
    std::array<QValueAxis*, 3> m_timeAxes{};
    std::array<QValueAxis*, 3> m_valueAxes{};
    std::array<QScatterSeries*, 2> m_annotations{};
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock;
    double m_position = 0.0;
    double m_anchor = 0.0;
    double m_rate = 1.0;
    bool m_playing = false;
};
} // namespace saim::medical
