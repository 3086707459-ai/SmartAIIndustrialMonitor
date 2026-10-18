#include "ui/DeviceDetailPanel.h"

#include "ai/FaultPredictor.h"
#include "device/Device.h"
#include "device/DeviceManager.h"
#include "ui/GaugeWidget.h"
#include "ui/MetricCard.h"
#include "ui/StateIndicator.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace saim {

namespace {

/// Wraps a widget in the standard panel frame.  Every band uses it, so the
/// borders, padding and corner radius are decided once.
QFrame* makePanel(const QString& objectName, QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setFrameShape(QFrame::NoFrame);
    return frame;
}

/// A caption/value pair, the atom of the AI band.
QWidget* makeField(const QString& caption, QLabel*& valueOut, QWidget* parent,
                   bool wrap = false)
{
    auto* host = new QWidget(parent);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* captionLabel = new QLabel(caption, host);
    captionLabel->setObjectName(QStringLiteral("MetricCaption"));
    layout->addWidget(captionLabel);

    valueOut = new QLabel(QStringLiteral("--"), host);
    valueOut->setObjectName(QStringLiteral("FieldValue"));
    valueOut->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (wrap)
    {
        valueOut->setWordWrap(true);
    }
    layout->addWidget(valueOut);

    return host;
}

}   // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DeviceDetailPanel::DeviceDetailPanel(DeviceManager* manager, QWidget* parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("DeviceDetailPanel"));
    buildUi();
    setDevice(QString());   // start on the empty page, never on a stale device
}

DeviceDetailPanel::~DeviceDetailPanel() = default;

void DeviceDetailPanel::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_stack = new QStackedWidget(this);
    m_emptyPage = buildEmptyPage();

    // The content lives inside a scroll area.  A detail panel has a fixed amount
    // of information but an unknown amount of screen: on a 1366x768 plant panel
    // the AI band would otherwise be cut off, and a monitoring screen that hides
    // the diagnosis is worse than one that scrolls.
    m_content = new QWidget();
    auto* contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(Theme::Metrics::kPanelPadding,
                                      Theme::Metrics::kPanelPadding,
                                      Theme::Metrics::kPanelPadding,
                                      Theme::Metrics::kPanelPadding);
    contentLayout->setSpacing(Theme::Metrics::kPanelSpacing);

    contentLayout->addWidget(buildHeaderBand());
    contentLayout->addWidget(buildGaugeBand());
    contentLayout->addWidget(buildAiBand());
    contentLayout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(m_content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_stack->addWidget(m_emptyPage);
    m_stack->addWidget(scroll);
    root->addWidget(m_stack);
}

QWidget* DeviceDetailPanel::buildEmptyPage()
{
    auto* page = new QWidget(this);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(8);
    layout->addStretch(1);

    auto* title = new QLabel(tr("No device selected"), page);
    title->setObjectName(QStringLiteral("EmptyStateTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto* hint = new QLabel(tr("Choose a device from the list to see its live values "
                               "and the AI assessment."), page);
    hint->setObjectName(QStringLiteral("EmptyState"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    layout->addStretch(1);
    return page;
}

QWidget* DeviceDetailPanel::buildHeaderBand()
{
    QFrame* band = makePanel(QStringLiteral("Panel"), this);

    auto* layout = new QHBoxLayout(band);
    layout->setContentsMargins(Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding,
                               Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding);
    layout->setSpacing(12);

    m_lamp = new StateIndicator(band);
    m_lamp->setDiameter(16);
    m_lamp->setShowText(false);
    layout->addWidget(m_lamp, 0, Qt::AlignTop);

    // -- identity -----------------------------------------------------------
    auto* identity = new QWidget(band);
    auto* identityLayout = new QVBoxLayout(identity);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(2);

    m_nameLabel = new QLabel(identity);
    m_nameLabel->setObjectName(QStringLiteral("DeviceTitle"));
    identityLayout->addWidget(m_nameLabel);

    m_idLabel = new QLabel(identity);
    m_idLabel->setObjectName(QStringLiteral("DeviceSubtitle"));
    m_idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identityLayout->addWidget(m_idLabel);

    layout->addWidget(identity, 1);

    // -- live counters ------------------------------------------------------
    auto* counters = new QWidget(band);
    auto* counterLayout = new QGridLayout(counters);
    counterLayout->setContentsMargins(0, 0, 0, 0);
    counterLayout->setHorizontalSpacing(18);
    counterLayout->setVerticalSpacing(2);

    QLabel* endpointValue = nullptr;
    QLabel* lastSeenValue = nullptr;
    QLabel* samplesValue  = nullptr;

    counterLayout->addWidget(makeField(tr("Endpoint"), endpointValue, counters), 0, 0);
    counterLayout->addWidget(makeField(tr("Last frame"), lastSeenValue, counters), 0, 1);
    counterLayout->addWidget(makeField(tr("Samples"), samplesValue, counters), 0, 2);

    // The fields are constructed by makeField(), which hands the label back
    // through an out-parameter; storing them in members lets refresh() update
    // them without re-walking the widget tree.
    m_endpointLabel   = endpointValue;
    m_lastSeenLabel   = lastSeenValue;
    m_sampleCountLabel = samplesValue;

    layout->addWidget(counters, 0);

    // -- actions ------------------------------------------------------------
    auto* actions = new QWidget(band);
    auto* actionLayout = new QVBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(4);

    m_acknowledgeButton = new QPushButton(tr("Acknowledge"), actions);
    m_acknowledgeButton->setToolTip(tr("Clear this device's alarm latch so it can alarm again "
                                       "after maintenance"));
    actionLayout->addWidget(m_acknowledgeButton);

    m_simulateButton = new QPushButton(tr("Simulate fault"), actions);
    m_simulateButton->setObjectName(QStringLiteral("DangerButton"));
    m_simulateButton->setToolTip(tr("Drive the simulator into a sustained fault to exercise "
                                    "the alarm and AI pipeline"));
    actionLayout->addWidget(m_simulateButton);

    layout->addWidget(actions, 0);

    connect(m_acknowledgeButton, &QPushButton::clicked, this, &DeviceDetailPanel::onAcknowledgeClicked);
    connect(m_simulateButton, &QPushButton::clicked, this, &DeviceDetailPanel::onSimulateClicked);

    return band;
}

QWidget* DeviceDetailPanel::buildGaugeBand()
{
    QFrame* band = makePanel(QStringLiteral("Panel"), this);

    auto* layout = new QHBoxLayout(band);
    layout->setContentsMargins(Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding,
                               Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding);
    layout->setSpacing(Theme::Metrics::kPanelSpacing);

    // Four dials, one per analogue channel.  Sense and limits are set from the
    // device's own envelope in updateGauges(), so a device with a custom
    // datasheet gets a correctly scaled dial without any code change here.
    m_temperatureGauge = new GaugeWidget(band);
    m_temperatureGauge->setCaption(tr("Temperature"));
    m_temperatureGauge->setUnit(QStringLiteral("°C"));
    m_temperatureGauge->setRange(0.0, 120.0);

    m_currentGauge = new GaugeWidget(band);
    m_currentGauge->setCaption(tr("Current"));
    m_currentGauge->setUnit(QStringLiteral("A"));
    m_currentGauge->setRange(0.0, 30.0);
    m_currentGauge->setDecimals(2);

    m_vibrationGauge = new GaugeWidget(band);
    m_vibrationGauge->setCaption(tr("Vibration"));
    m_vibrationGauge->setUnit(QStringLiteral("mm/s"));
    m_vibrationGauge->setRange(0.0, 1.5);
    m_vibrationGauge->setDecimals(3);

    m_voltageGauge = new GaugeWidget(band);
    m_voltageGauge->setCaption(tr("Voltage"));
    m_voltageGauge->setUnit(QStringLiteral("V"));
    m_voltageGauge->setRange(160.0, 280.0);
    m_voltageGauge->setDecimals(1);
    // Voltage is the one channel where both ends are bad: a sag and a surge are
    // both faults, and a dial that only flags the high end would hide half the
    // failures an electrician is looking for.
    m_voltageGauge->setSense(GaugeSense::BothEnds);

    layout->addWidget(m_temperatureGauge, 1);
    layout->addWidget(m_currentGauge, 1);
    layout->addWidget(m_vibrationGauge, 1);
    layout->addWidget(m_voltageGauge, 1);

    return band;
}

QWidget* DeviceDetailPanel::buildAiBand()
{
    QFrame* band = makePanel(QStringLiteral("Panel"), this);

    auto* layout = new QVBoxLayout(band);
    layout->setContentsMargins(Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding,
                               Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding);
    layout->setSpacing(Theme::Metrics::kPanelSpacing);

    // -- title row ----------------------------------------------------------
    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(8);

    auto* title = new QLabel(tr("AI assessment"), band);
    title->setObjectName(QStringLiteral("PanelTitle"));
    titleRow->addWidget(title);
    titleRow->addStretch(1);

    m_backendLabel = new QLabel(band);
    m_backendLabel->setObjectName(QStringLiteral("MetricCaption"));
    m_backendLabel->setToolTip(tr("Which inference backend produced the assessment. "
                                  "'onnxruntime' means a trained neural model answered; "
                                  "'heuristic' means the built-in physics model did."));
    titleRow->addWidget(m_backendLabel);

    m_reportButton = new QPushButton(tr("Report"), band);
    m_reportButton->setToolTip(tr("Generate a maintenance report for this device"));
    titleRow->addWidget(m_reportButton);

    m_analyseButton = new QPushButton(tr("Analyse now"), band);
    m_analyseButton->setObjectName(QStringLiteral("PrimaryButton"));
    m_analyseButton->setToolTip(tr("Run an immediate inference instead of waiting for the "
                                   "next scheduled analysis"));
    titleRow->addWidget(m_analyseButton);

    layout->addLayout(titleRow);

    // -- cards --------------------------------------------------------------
    auto* cardRow = new QHBoxLayout();
    cardRow->setSpacing(Theme::Metrics::kPanelSpacing);

    m_healthCard = new MetricCard(band);
    m_healthCard->setCaption(tr("HEALTH SCORE"));
    m_healthCard->setUnit(QStringLiteral("%"));
    m_healthCard->setDecimals(0);
    m_healthCard->setMode(MetricCard::Mode::HigherIsBetter);
    m_healthCard->setThresholds(60.0, 40.0);   // warn floor, critical floor
    m_healthCard->setPlaceholder(QStringLiteral("--"));
    cardRow->addWidget(m_healthCard, 1);

    m_failureCard = new MetricCard(band);
    m_failureCard->setCaption(tr("FAILURE RISK"));
    m_failureCard->setUnit(QStringLiteral("%"));
    m_failureCard->setDecimals(1);
    m_failureCard->setMode(MetricCard::Mode::Threshold);
    m_failureCard->setThresholds(55.0, 85.0);
    m_failureCard->setPlaceholder(QStringLiteral("--"));
    cardRow->addWidget(m_failureCard, 1);

    layout->addLayout(cardRow);

    // -- health bar ---------------------------------------------------------
    // The card shows the number; the bar shows it against the whole scale at a
    // width the eye can compare across devices.  Both, because they answer
    // different questions and neither is redundant.
    m_healthBar = new QProgressBar(band);
    m_healthBar->setRange(0, 100);
    m_healthBar->setTextVisible(false);
    m_healthBar->setFixedHeight(6);
    layout->addWidget(m_healthBar);

    // -- fields -------------------------------------------------------------
    auto* fieldRow = new QHBoxLayout();
    fieldRow->setSpacing(18);
    fieldRow->addWidget(makeField(tr("Fault class"), m_faultClassLabel, band), 1);
    fieldRow->addWidget(makeField(tr("Anomaly"), m_anomalyLabel, band), 2);
    layout->addLayout(fieldRow);

    auto* adviceHost = makeField(tr("Recommended action"), m_adviceLabel, band, true);
    layout->addWidget(adviceHost);

    connect(m_analyseButton, &QPushButton::clicked, this, &DeviceDetailPanel::onAnalyseClicked);
    connect(m_reportButton, &QPushButton::clicked, this, &DeviceDetailPanel::onReportClicked);

    return band;
}

// ---------------------------------------------------------------------------
// Device selection
// ---------------------------------------------------------------------------

Device* DeviceDetailPanel::currentDevice() const
{
    if (m_manager == nullptr || m_deviceId.isEmpty())
    {
        return nullptr;
    }
    const std::shared_ptr<Device> device = m_manager->device(m_deviceId);
    return device.get();
}

void DeviceDetailPanel::setDevice(const QString& deviceId)
{
    m_deviceId = deviceId;

    const bool haveDevice = (currentDevice() != nullptr);
    m_stack->setCurrentIndex(haveDevice ? 1 : 0);

    if (haveDevice)
    {
        refresh();
    }
}

void DeviceDetailPanel::refresh()
{
    const Device* device = currentDevice();
    if (device == nullptr)
    {
        // The device was removed while it was selected.  Falling back to the
        // empty page is the only honest thing to do - continuing to show its last
        // known values would present a decommissioned machine as live.
        m_stack->setCurrentIndex(0);
        return;
    }

    updateHeader(*device);
    updateGauges(*device);
    updateAiBand(*device);
}

// ---------------------------------------------------------------------------
// Band updates
// ---------------------------------------------------------------------------

void DeviceDetailPanel::updateHeader(const Device& device)
{
    m_lamp->setState(device.state());

    m_nameLabel->setText(device.deviceName().isEmpty() ? device.deviceId()
                                                       : device.deviceName());
    m_idLabel->setText(QStringLiteral("%1  ·  %2  ·  %3")
                           .arg(device.deviceId(), device.deviceType(), toString(device.state())));

    m_endpointLabel->setText(device.ipAddress().isEmpty()
                                 ? tr("simulated")
                                 : QStringLiteral("%1:%2").arg(device.ipAddress())
                                       .arg(device.port()));

    if (device.lastSeen().isValid())
    {
        // Relative age, not an absolute timestamp.  "4 s ago" is what an operator
        // needs; "2026-10-09 14:32:07" requires them to subtract, and they will
        // get it wrong under pressure.
        const qint64 ageSeconds = device.lastSeen().secsTo(QDateTime::currentDateTime());
        m_lastSeenLabel->setText(ageSeconds < 1 ? tr("just now")
                                                : tr("%1 s ago").arg(ageSeconds));
    }
    else
    {
        m_lastSeenLabel->setText(tr("never"));
    }

    m_sampleCountLabel->setText(QString::number(device.sampleCount()));
}

void DeviceDetailPanel::updateGauges(const Device& device)
{
    const SensorSample& sample = device.latest();
    const DeviceLimits& limits = device.limits();

    // Limits come from the device, not from constants in this file: a customer
    // with a 55 C bearing and a customer with a 105 C motor must both get a dial
    // that shows them where their own limit is.
    m_temperatureGauge->setLimits(limits.temperatureWarn, limits.temperatureHigh);
    m_temperatureGauge->setRange(0.0, std::max(120.0, limits.temperatureHigh * 1.3));

    m_currentGauge->setLimits(limits.currentWarn, limits.currentHigh);
    m_currentGauge->setRange(0.0, std::max(30.0, limits.currentHigh * 1.4));

    m_vibrationGauge->setLimits(limits.vibrationWarn, limits.vibrationHigh);
    m_vibrationGauge->setRange(0.0, std::max(1.5, limits.vibrationHigh * 1.5));

    m_voltageGauge->setLimits(limits.voltageWarnLow, limits.voltageWarnHigh);
    m_voltageGauge->setOuterLimits(limits.voltageWarnLow - 25.0, limits.voltageWarnHigh + 25.0);
    m_voltageGauge->setRange(std::max(0.0, limits.voltageWarnLow - 60.0),
                             limits.voltageWarnHigh + 40.0);

    if (sample.isValid())
    {
        m_temperatureGauge->setValue(sample.temperature);
        m_currentGauge->setValue(sample.current);
        m_vibrationGauge->setValue(sample.vibration);
        m_voltageGauge->setValue(sample.voltage);
    }
    else
    {
        m_temperatureGauge->clearValue();
        m_currentGauge->clearValue();
        m_vibrationGauge->clearValue();
        m_voltageGauge->clearValue();
    }
}

void DeviceDetailPanel::updateAiBand(const Device& device)
{
    const FaultPrediction& prediction = device.prediction();
    const AnomalyEvent& anomaly = device.lastAnomaly();

    // -- backend provenance --------------------------------------------------
    if (prediction.backend.isEmpty())
    {
        m_backendLabel->setText(tr("waiting for first analysis"));
    }
    else
    {
        const bool neural = prediction.backend.startsWith(QStringLiteral("onnx"));
        m_backendLabel->setText(neural ? tr("backend: ONNX Runtime")
                                       : tr("backend: built-in model"));
        m_backendLabel->setProperty("neural", neural);
    }

    // -- cards ---------------------------------------------------------------
    if (prediction.faultType.isEmpty())
    {
        // No verdict yet.  Both cards are put back to their placeholder rather
        // than left showing the previous device's numbers - a stale green 96 %
        // on a machine that has just been swapped in is exactly the kind of lie
        // this panel exists to avoid.
        m_healthCard->setPlaceholder(tr("--"));
        m_failureCard->setPlaceholder(tr("--"));
        m_healthBar->setValue(0);
        m_faultClassLabel->setText(tr("Analysing..."));
        m_faultClassLabel->setObjectName(QStringLiteral("FieldValue"));
    }
    else
    {
        m_healthCard->setValue(prediction.healthScore);
        m_failureCard->setValue(prediction.failureProbability * 100.0);
        m_healthBar->setValue(static_cast<int>(std::lround(prediction.healthScore)));

        m_faultClassLabel->setText(prediction.faultType);
        m_faultClassLabel->setObjectName(
            prediction.faultType == QStringLiteral("None")
                ? QStringLiteral("StateNormal")
                : QStringLiteral("StateWarning"));
        m_faultClassLabel->style()->unpolish(m_faultClassLabel);
        m_faultClassLabel->style()->polish(m_faultClassLabel);
    }

    // -- anomaly -------------------------------------------------------------
    if (anomaly.level == AnomalyLevel::None)
    {
        m_anomalyLabel->setText(tr("None detected"));
    }
    else
    {
        m_anomalyLabel->setText(QStringLiteral("%1 - %2")
                                    .arg(toString(anomaly.level), anomaly.reason));
    }

    // -- advice --------------------------------------------------------------
    // The advice is derived from the fault class through the same function the
    // report uses, so a recommendation cannot differ between the screen and the
    // printed document.
    if (prediction.faultType.isEmpty())
    {
        m_adviceLabel->setText(tr("Waiting for enough samples to assess this device."));
    }
    else
    {
        const FaultClass faultClass = faultClassFromString(prediction.faultType);
        const QString action = recommendedAction(faultClass);
        m_adviceLabel->setText(action.isEmpty() ? tr("No action required.")
                                                : action);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void DeviceDetailPanel::onAcknowledgeClicked()
{
    if (!m_deviceId.isEmpty())
    {
        emit acknowledgeRequested(m_deviceId);
    }
}

void DeviceDetailPanel::onAnalyseClicked()
{
    if (!m_deviceId.isEmpty())
    {
        emit analyseRequested(m_deviceId);
    }
}

void DeviceDetailPanel::onReportClicked()
{
    if (!m_deviceId.isEmpty())
    {
        emit reportRequested(m_deviceId);
    }
}

void DeviceDetailPanel::onSimulateClicked()
{
    if (!m_deviceId.isEmpty())
    {
        emit simulateFaultRequested(m_deviceId);
    }
}

}   // namespace saim
