#pragma once

// ---------------------------------------------------------------------------
// DeviceDetailPanel.h - everything known about one device, in three bands.
//
// Layout rationale
// ----------------
//     +--------------------------------------------------+
//     |  header:  name, id, type, lamp, last-seen        |   identity
//     +--------------------------------------------------+
//     |  gauges:  temperature / current / vibration      |   live analogue
//     +--------------------------------------------------+
//     |  AI:      health, fault class, anomaly, advice   |   the verdict
//     +--------------------------------------------------+
//
// The three bands answer the three questions an operator actually asks, in the
// order they ask them: "which machine is this?", "what is it doing right now?"
// and "what does the system think is wrong?".  A single flat grid of twenty
// readouts - the usual shape of a detail panel - makes the third question as hard
// to answer as the first.
//
// The AI band is the part that has to be honest.  It distinguishes three states
// that are easy to conflate and dangerous to conflate:
//
//   * no verdict yet   -> "Analysing"   (grey)
//   * a healthy verdict -> "Normal"     (green)
//   * no AI available   -> the backend is named, so the operator knows whether a
//                          neural model or the built-in fallback answered
//
// Showing green for an unanalysed device is the single most misleading thing a
// monitoring dashboard can do, and it is why the model exposes the raw verdict
// rather than a pre-coloured state.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QProgressBar;
class QStackedWidget;

namespace saim {

class Device;
class DeviceManager;
class GaugeWidget;
class MetricCard;
class StateIndicator;

class DeviceDetailPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit DeviceDetailPanel(DeviceManager* manager, QWidget* parent = nullptr);
    ~DeviceDetailPanel() override;

public slots:
    /// Shows this device.  An unknown id shows the "no selection" page rather
    /// than the last device's data - which is the failure mode where a panel
    /// keeps displaying a machine that has been removed.
    void setDevice(const QString& deviceId);

    /// Pushes fresh values into the gauges and the AI band.  Called on the UI
    /// refresh tick by the controller; also callable directly after an AI result.
    void refresh();

signals:
    /// The operator asked for a live AI analysis of this device.
    void analyseRequested(const QString& deviceId);

    /// The operator asked for a maintenance report for this device.
    void reportRequested(const QString& deviceId);

    /// The operator acknowledged the device's alarms.
    void acknowledgeRequested(const QString& deviceId);

    /// A simulated fault was requested (only enabled when the simulator is the
    /// data source).  Kept on this panel because it is a per-device action.
    void simulateFaultRequested(const QString& deviceId);

private slots:
    void onAcknowledgeClicked();
    void onAnalyseClicked();
    void onReportClicked();
    void onSimulateClicked();

private:
    void buildUi();
    QWidget* buildHeaderBand();
    QWidget* buildGaugeBand();
    QWidget* buildAiBand();

    /// The shared "nothing selected" page.  Built once so switching devices does
    /// not rebuild it.
    QWidget* buildEmptyPage();

    [[nodiscard]] Device* currentDevice() const;

    void updateHeader(const Device& device);
    void updateGauges(const Device& device);
    void updateAiBand(const Device& device);

    DeviceManager* m_manager = nullptr;
    QString        m_deviceId;

    QStackedWidget* m_stack = nullptr;
    QWidget*        m_content = nullptr;
    QWidget*        m_emptyPage = nullptr;

    // header band
    StateIndicator* m_lamp = nullptr;
    QLabel* m_nameLabel = nullptr;
    QLabel* m_idLabel = nullptr;
    QLabel* m_endpointLabel = nullptr;
    QLabel* m_lastSeenLabel = nullptr;
    QLabel* m_sampleCountLabel = nullptr;

    // gauge band
    GaugeWidget* m_temperatureGauge = nullptr;
    GaugeWidget* m_currentGauge = nullptr;
    GaugeWidget* m_vibrationGauge = nullptr;
    GaugeWidget* m_voltageGauge = nullptr;

    // AI band
    MetricCard*  m_healthCard = nullptr;
    MetricCard*  m_failureCard = nullptr;
    QLabel*      m_faultClassLabel = nullptr;
    QLabel*      m_backendLabel = nullptr;
    QLabel*      m_anomalyLabel = nullptr;
    QLabel*      m_adviceLabel = nullptr;
    QProgressBar* m_healthBar = nullptr;

    // actions
    QPushButton* m_analyseButton = nullptr;
    QPushButton* m_reportButton = nullptr;
    QPushButton* m_acknowledgeButton = nullptr;
    QPushButton* m_simulateButton = nullptr;
};

}   // namespace saim
