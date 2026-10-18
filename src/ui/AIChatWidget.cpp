#include "ui/AIChatWidget.h"

#include "ai/FaultPredictor.h"
#include "device/Device.h"
#include "device/DeviceManager.h"
#include "ui/Theme.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace saim {

namespace {

[[nodiscard]] bool mentions(const QString& haystack, const char* needle)
{
    return haystack.contains(QString::fromLatin1(needle), Qt::CaseInsensitive);
}

[[nodiscard]] QString humanAge(qint64 seconds)
{
    if (seconds < 60)
    {
        return QStringLiteral("%1 s").arg(seconds);
    }
    if (seconds < 3600)
    {
        return QStringLiteral("%1 min").arg(seconds / 60);
    }
    return QStringLiteral("%1 h").arg(seconds / 3600);
}

/// Sign of a slope, as a word.  Returned rather than inlined into a sentence
/// because "rising" and "falling" are the two words an operator scans for.
[[nodiscard]] QString trendWord(double slopePerSecond, double deadband)
{
    if (slopePerSecond > deadband)
    {
        return QStringLiteral("rising");
    }
    if (slopePerSecond < -deadband)
    {
        return QStringLiteral("falling");
    }
    return QStringLiteral("stable");
}

/// Formats a number with a fixed number of decimals, or "--" when there is no
/// reading.  Used everywhere in the answer builders so that no sentence can
/// accidentally print "0.00" for a value that was never measured.
[[nodiscard]] QString numberOrDash(bool hasValue, double value, int decimals)
{
    return hasValue ? QString::number(value, 'f', decimals) : QStringLiteral("--");
}

}   // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AIChatWidget::AIChatWidget(DeviceManager* manager, QWidget* parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("AIChatWidget"));
    buildUi();
    clearTranscript();
}

AIChatWidget::~AIChatWidget() = default;

void AIChatWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding,
                             Theme::Metrics::kPanelPadding, Theme::Metrics::kPanelPadding);
    root->setSpacing(Theme::Metrics::kPanelSpacing);

    // -- header -------------------------------------------------------------
    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(6);

    auto* title = new QLabel(tr("Maintenance assistant"), this);
    title->setObjectName(QStringLiteral("PanelTitle"));
    headerRow->addWidget(title);
    headerRow->addStretch(1);

    m_backendLabel = new QLabel(this);
    m_backendLabel->setObjectName(QStringLiteral("MetricCaption"));
    m_backendLabel->setToolTip(tr("Which inference backend the assistant's answers are based on"));
    headerRow->addWidget(m_backendLabel);

    m_clearButton = new QPushButton(tr("Clear"), this);
    m_clearButton->setToolTip(tr("Clear the conversation"));
    headerRow->addWidget(m_clearButton);

    root->addLayout(headerRow);

    // -- transcript ---------------------------------------------------------
    // Inside a scroll area whose widget is rebuilt on each append.  Appending a
    // widget to a QVBoxLayout and then forcing a layout pass is cheap for the
    // tens of messages a session produces; a QTextBrowser would be faster but
    // could not host the per-message frames, colours and device links that make
    // an answer readable.
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_transcriptHost = new QWidget(m_scroll);
    m_transcriptLayout = new QVBoxLayout(m_transcriptHost);
    m_transcriptLayout->setContentsMargins(0, 0, 6, 0);
    m_transcriptLayout->setSpacing(8);
    m_transcriptLayout->addStretch(1);

    m_scroll->setWidget(m_transcriptHost);
    root->addWidget(m_scroll, 1);

    // -- input --------------------------------------------------------------
    auto* inputRow = new QHBoxLayout();
    inputRow->setSpacing(6);

    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(tr("Ask about a device, e.g. \"why is Motor 001 alarming?\""));
    m_input->setFixedHeight(56);
    m_input->setTabChangesFocus(true);   // Tab must move focus, not indent
    inputRow->addWidget(m_input, 1);

    m_sendButton = new QPushButton(tr("Send"), this);
    m_sendButton->setObjectName(QStringLiteral("PrimaryButton"));
    m_sendButton->setFixedWidth(72);
    inputRow->addWidget(m_sendButton);

    root->addLayout(inputRow);

    // -- signals ------------------------------------------------------------
    connect(m_sendButton, &QPushButton::clicked, this, &AIChatWidget::onSubmit);
    connect(m_clearButton, &QPushButton::clicked, this, &AIChatWidget::clearTranscript);

    // Ctrl+Enter submits, because a plain Enter in a multi-line box must insert
    // a newline - an operator pasting a multi-line alarm message should not have
    // it sent half-finished.
    auto* submitShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), m_input);
    submitShortcut->setContext(Qt::WidgetShortcut);
    connect(submitShortcut, &QShortcut::activated, this, &AIChatWidget::onSubmit);
}

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

void AIChatWidget::clearTranscript()
{
    m_messages.clear();

    // Remove every widget except the trailing stretch, which is what keeps the
    // messages pinned to the top of the scroll area rather than centred.
    while (m_transcriptLayout->count() > 1)
    {
        QLayoutItem* item = m_transcriptLayout->takeAt(0);
        if (item->widget() != nullptr)
        {
            item->widget()->deleteLater();
        }
        delete item;
    }

    ChatMessage greeting;
    greeting.role = ChatMessage::Role::Assistant;
    greeting.timestamp = QDateTime::currentDateTime();
    greeting.text = tr("I can explain this system's own measurements and AI assessments - "
                       "health scores, fault classes, anomalies and what maintenance they "
                       "imply. Ask about a device by name or id, or pick one of the "
                       "questions below.\n\n"
                       "I do not speculate: every number I quote comes from a reading this "
                       "application recorded, and if there is no data I will say so.");
    m_messages.append(greeting);

    ChatMessage capabilities;
    capabilities.role = ChatMessage::Role::System;
    capabilities.timestamp = QDateTime::currentDateTime();
    capabilities.text = describeCapabilities();
    m_messages.append(capabilities);

    rebuildTranscript();

    // -- example questions --------------------------------------------------
    // Built once here rather than in the transcript, because they are a property
    // of the input box, not of the conversation: they stay available after the
    // operator has asked something else.
    auto* suggestions = new QFrame(m_transcriptHost);
    suggestions->setObjectName(QStringLiteral("SuggestionRow"));
    auto* suggestionLayout = new QVBoxLayout(suggestions);
    suggestionLayout->setContentsMargins(0, 4, 0, 0);
    suggestionLayout->setSpacing(4);

    const QStringList shown{
        tr("Why is this device alarming?"),
        tr("What should I do about it?"),
        tr("Is the temperature trending up?"),
        tr("How healthy is the fleet?"),
    };
    for (const QString& text : shown)
    {
        auto* chip = new QPushButton(text, suggestions);
        chip->setObjectName(QStringLiteral("SuggestionChip"));
        chip->setFlat(true);
        chip->setCursor(Qt::PointingHandCursor);
        connect(chip, &QPushButton::clicked, this, &AIChatWidget::onSuggestionClicked);
        suggestionLayout->addWidget(chip, 0, Qt::AlignLeft);
    }

    m_transcriptLayout->insertWidget(m_transcriptLayout->count() - 1, suggestions);
}

void AIChatWidget::appendMessage(const ChatMessage& message)
{
    m_messages.append(message);

    // -- one message, as a framed block -------------------------------------
    auto* frame = new QFrame(m_transcriptHost);
    frame->setObjectName(QStringLiteral("ChatBubble"));
    frame->setProperty("role",
                       message.role == ChatMessage::Role::User ? QStringLiteral("user")
                       : message.role == ChatMessage::Role::System ? QStringLiteral("system")
                                                                   : QStringLiteral("assistant"));

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    // -- who and when -------------------------------------------------------
    auto* meta = new QLabel(frame);
    meta->setObjectName(QStringLiteral("MetricCaption"));
    meta->setText(QStringLiteral("%1  ·  %2")
                      .arg(message.role == ChatMessage::Role::User ? tr("You")
                           : message.role == ChatMessage::Role::System ? tr("System")
                                                                       : tr("Assistant"),
                           message.timestamp.toString(QStringLiteral("HH:mm:ss"))));
    layout->addWidget(meta);

    // -- body ---------------------------------------------------------------
    auto* body = new QLabel(frame);
    body->setObjectName(QStringLiteral("ChatBody"));
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // The text is plain, never rich.  Rendering untrusted-ish content as rich
    // text is how a device name containing markup turns into a broken layout;
    // Qt::PlainText keeps the widget honest.
    body->setTextFormat(Qt::PlainText);
    body->setText(message.text);
    layout->addWidget(body);

    // -- device link --------------------------------------------------------
    // An answer about a device offers a link to it.  This is the one "action"
    // the transcript has, and it exists so that reading a diagnosis and looking
    // at the machine are one click apart instead of two navigations apart.
    if (!message.deviceId.isEmpty())
    {
        auto* link = new QPushButton(tr("Show %1").arg(message.deviceId), frame);
        link->setObjectName(QStringLiteral("LinkButton"));
        link->setFlat(true);
        link->setCursor(Qt::PointingHandCursor);
        const QString deviceId = message.deviceId;
        connect(link, &QPushButton::clicked, this, [this, deviceId]()
        {
            emit deviceReferenced(deviceId);
        });
        layout->addWidget(link, 0, Qt::AlignLeft);
    }

    // Insert before the trailing stretch.
    m_transcriptLayout->insertWidget(m_transcriptLayout->count() - 1, frame);

    // Scroll to the bottom on the next event-loop turn, once the new widget has
    // been laid out and the scrollbar's range is correct.  Scrolling immediately
    // would use the *old* maximum and stop short.
    QMetaObject::invokeMethod(this, [this]()
    {
        m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum());
    }, Qt::QueuedConnection);
}

void AIChatWidget::rebuildTranscript()
{
    const QVector<ChatMessage> messages = m_messages;
    m_messages.clear();

    while (m_transcriptLayout->count() > 1)
    {
        QLayoutItem* item = m_transcriptLayout->takeAt(0);
        if (item->widget() != nullptr)
        {
            item->widget()->deleteLater();
        }
        delete item;
    }

    for (const ChatMessage& message : messages)
    {
        appendMessage(message);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void AIChatWidget::ask(const QString& question)
{
    const QString trimmed = question.trimmed();
    if (trimmed.isEmpty())
    {
        return;
    }

    ChatMessage userMessage;
    userMessage.role = ChatMessage::Role::User;
    userMessage.timestamp = QDateTime::currentDateTime();
    userMessage.text = trimmed;
    appendMessage(userMessage);

    ChatMessage answerMessage;
    answerMessage.role = ChatMessage::Role::Assistant;
    answerMessage.timestamp = QDateTime::currentDateTime();
    answerMessage.text = answer(trimmed);

    // Resolve the device the answer is about, so the transcript can offer a link
    // to it.  Done after the answer is composed, because composing it may have
    // failed to resolve a device - in which case there is nothing to link.
    if (Device* device = resolveDevice(trimmed))
    {
        answerMessage.deviceId = device->deviceId();
    }

    appendMessage(answerMessage);
}

void AIChatWidget::setContextDevice(const QString& deviceId)
{
    m_contextDeviceId = deviceId;
}

void AIChatWidget::applyAnalysis(const QString& deviceId, const FaultPrediction& prediction,
                                 const AnomalyEvent& anomaly)
{
    if (deviceId.isEmpty())
    {
        return;
    }
    m_recentPredictions.insert(deviceId, prediction);
    m_recentAnomalies.insert(deviceId, anomaly);
}

void AIChatWidget::setBackendDescription(const QString& description)
{
    m_backendDescription = description;
    if (m_backendLabel != nullptr)
    {
        m_backendLabel->setText(description);
    }
}

void AIChatWidget::onSubmit()
{
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty())
    {
        return;
    }

    m_input->clear();
    ask(text);
}

void AIChatWidget::onSuggestionClicked()
{
    auto* button = qobject_cast<QPushButton*>(sender());
    if (button == nullptr)
    {
        return;
    }
    ask(button->text());
}

// ---------------------------------------------------------------------------
// Device resolution
// ---------------------------------------------------------------------------

Device* AIChatWidget::resolveDevice(const QString& question, QString* matchedToken) const
{
    if (m_manager == nullptr)
    {
        return nullptr;
    }

    // 1. An explicit id or name in the question wins.  Longest id first, so that
    //    "Motor-001" is not shadowed by a device called "Motor-00" - a substring
    //    match in the wrong order would silently answer about the wrong machine,
    //    which in a diagnostic tool is the worst possible failure.
    QStringList ids = m_manager->deviceIds();
    std::sort(ids.begin(), ids.end(), [](const QString& left, const QString& right)
    {
        return left.size() > right.size();
    });

    for (const QString& id : ids)
    {
        if (mentions(question, id.toUtf8().constData()))
        {
            if (matchedToken != nullptr)
            {
                *matchedToken = id;
            }
            return m_manager->device(id).get();
        }

        const std::shared_ptr<Device> device = m_manager->device(id);
        if (device != nullptr && !device->deviceName().isEmpty()
            && mentions(question, device->deviceName().toUtf8().constData()))
        {
            if (matchedToken != nullptr)
            {
                *matchedToken = device->deviceName();
            }
            return device.get();
        }
    }

    // 2. A bare ordinal - "device 3".  Engineers number things, and making
    //    someone type the full id for a machine that is stencilled "3" on its
    //    housing would be a needless obstacle.
    for (const QString& id : ids)
    {
        // Trailing digit run of the id, e.g. "M-003" -> 3.
        qsizetype start = id.size();
        while (start > 0 && id.at(start - 1).isDigit())
        {
            --start;
        }
        if (start >= id.size())
        {
            continue;   // no trailing digits: nothing to match an ordinal against
        }

        const int numeric = id.mid(start).toInt();
        if (question.contains(QStringLiteral(" %1").arg(numeric))
            || question.contains(QStringLiteral("#%1").arg(numeric)))
        {
            if (matchedToken != nullptr)
            {
                *matchedToken = id;
            }
            return m_manager->device(id).get();
        }
    }

    // 3. Fall back to whatever the operator has selected.  A question with no
    //    device in it - "why is it alarming?" - is about what they are looking
    //    at, and that is the only sane interpretation.
    if (!m_contextDeviceId.isEmpty())
    {
        return m_manager->device(m_contextDeviceId).get();
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Intent matching and answering
//
// This is the whole "intelligence" of the widget, and it is deliberately
// readable.  The design principle: classify the question into a small set of
// intents, then answer each one from measurements.  Nothing is generated, so
// nothing can be fabricated - and when the intent is not recognised the widget
// says what it *can* answer instead of producing something plausible.
//
// The alternative - wiring an LLM to the device table - would answer more
// fluently and be strictly worse here, because a fluent wrong answer about a
// machine that is about to fail cannot be distinguished from a right one by the
// person reading it.  Being explicit about the intent list is what makes the
// answer checkable.
// ---------------------------------------------------------------------------

QString AIChatWidget::answer(const QString& question)
{
    const bool asksWhy     = mentions(question, "why") || mentions(question, "reason");
    const bool asksAction  = mentions(question, "what should") || mentions(question, "action")
                             || mentions(question, "fix") || mentions(question, "maintenance")
                             || mentions(question, "do about");
    const bool asksAnomaly = mentions(question, "anomal") || mentions(question, "unusual")
                             || mentions(question, "outlier") || mentions(question, "abnormal");
    const bool asksTrend   = mentions(question, "trend") || mentions(question, "rising")
                             || mentions(question, "falling") || mentions(question, "drift")
                             || mentions(question, "increasing");
    const bool asksHealth  = mentions(question, "health") || mentions(question, "score")
                             || mentions(question, "healthy") || mentions(question, "condition");
    const bool asksFleet   = mentions(question, "fleet") || mentions(question, "all device")
                             || mentions(question, "everything") || mentions(question, "overall")
                             || mentions(question, "plant");
    const bool asksHelp    = mentions(question, "what can you") || mentions(question, "help")
                             || mentions(question, "capab");

    if (asksHelp)
    {
        return describeCapabilities();
    }

    if (asksFleet)
    {
        return summariseFleet();
    }

    Device* device = resolveDevice(question);

    if (device == nullptr)
    {
        // No device named and none selected.  Saying so, and saying how to fix
        // it, is the only useful answer - guessing would be worse than admitting
        // the ambiguity, because the operator cannot tell a guess from a fact.
        const QString example = (m_manager != nullptr && m_manager->deviceCount() > 0)
                                    ? m_manager->deviceIds().first()
                                    : QStringLiteral("Motor001");
        return tr("I could not tell which device you mean. Name it explicitly - for "
                  "example \"why is %1 alarming?\" - or select it in the device list "
                  "before asking, and I will assume you mean the selected one.")
            .arg(example);
    }

    if (asksAction)
    {
        return recommendAction(*device);
    }
    if (asksWhy)
    {
        return explainAlarm(*device);
    }
    if (asksAnomaly)
    {
        return explainAnomaly(*device);
    }
    if (asksTrend)
    {
        return describeTrend(*device);
    }
    if (asksHealth)
    {
        return explainHealth(*device);
    }

    // No intent matched, but a device did.  Answering with the full picture is
    // more useful than asking again, and it is the same text a more specific
    // question would have produced.
    return explainHealth(*device) + QStringLiteral("\n\n") + recommendAction(*device);
}

// ---------------------------------------------------------------------------
// Answer builders
//
// Each of these quotes only values it read from the device, and each states the
// age of the evidence.  A diagnosis without a timestamp is a diagnosis the
// operator cannot judge.
// ---------------------------------------------------------------------------

QString AIChatWidget::explainAlarm(const Device& device) const
{
    const SensorSample& sample = device.latest();
    const FaultPrediction& prediction = device.prediction();
    const AnomalyEvent& anomaly = device.lastAnomaly();
    const DeviceLimits& limits = device.limits();

    if (!sample.isValid())
    {
        return tr("%1 has not reported any telemetry yet, so there is nothing to explain. "
                  "It will be assessed once a few seconds of data have arrived.")
            .arg(device.deviceId());
    }

    QStringList lines;

    lines << tr("%1 is in state %2. Here is what the system measured and concluded:")
                 .arg(device.deviceId(), toString(device.state()));

    // -- the rule-based reason ----------------------------------------------
    // This is the "why" for a threshold breach, and it is stated first because
    // it is the most concrete possible answer: a number against a limit.
    const DeviceState implied = limits.evaluate(sample);
    if (implied >= DeviceState::Warning)
    {
        QStringList breaches;

        if (sample.temperature >= limits.temperatureHigh)
        {
            breaches << tr("temperature %1 °C is at or above the trip limit of %2 °C")
                            .arg(sample.temperature, 0, 'f', 1).arg(limits.temperatureHigh, 0, 'f', 1);
        }
        else if (sample.temperature >= limits.temperatureWarn)
        {
            breaches << tr("temperature %1 °C is above the warning limit of %2 °C")
                            .arg(sample.temperature, 0, 'f', 1).arg(limits.temperatureWarn, 0, 'f', 1);
        }

        if (sample.current >= limits.currentHigh)
        {
            breaches << tr("current %1 A is at or above the trip limit of %2 A")
                            .arg(sample.current, 0, 'f', 2).arg(limits.currentHigh, 0, 'f', 1);
        }
        else if (sample.current >= limits.currentWarn)
        {
            breaches << tr("current %1 A is above the warning limit of %2 A")
                            .arg(sample.current, 0, 'f', 2).arg(limits.currentWarn, 0, 'f', 1);
        }

        if (sample.vibration >= limits.vibrationHigh)
        {
            breaches << tr("vibration %1 mm/s is at or above the trip limit of %2 mm/s")
                            .arg(sample.vibration, 0, 'f', 3).arg(limits.vibrationHigh, 0, 'f', 2);
        }
        else if (sample.vibration >= limits.vibrationWarn)
        {
            breaches << tr("vibration %1 mm/s is above the warning limit of %2 mm/s")
                            .arg(sample.vibration, 0, 'f', 3).arg(limits.vibrationWarn, 0, 'f', 2);
        }

        if (sample.voltage < limits.voltageWarnLow || sample.voltage > limits.voltageWarnHigh)
        {
            breaches << tr("voltage %1 V is outside the %2-%3 V supply window")
                            .arg(sample.voltage, 0, 'f', 1)
                            .arg(limits.voltageWarnLow, 0, 'f', 0)
                            .arg(limits.voltageWarnHigh, 0, 'f', 0);
        }

        if (!breaches.isEmpty())
        {
            lines << QString();
            lines << tr("Threshold breach:");
            for (const QString& breach : breaches)
            {
                lines << QStringLiteral("  - ") + breach;
            }
        }
    }

    // -- the AI verdict -----------------------------------------------------
    if (!prediction.faultType.isEmpty())
    {
        lines << QString();
        lines << tr("Model verdict: %1, health %2 %%, failure probability %3 %%, "
                    "confidence %4 %% (backend: %5).")
                     .arg(prediction.faultType)
                     .arg(prediction.healthScore, 0, 'f', 0)
                     .arg(prediction.failureProbability * 100.0, 0, 'f', 1)
                     .arg(prediction.confidence * 100.0, 0, 'f', 0)
                     .arg(prediction.backend);

        const FaultClass faultClass = faultClassFromString(prediction.faultType);
        if (faultClass != FaultClass::Healthy)
        {
            lines << tr("That class corresponds to: %1").arg(recommendedAction(faultClass));
        }
    }
    else
    {
        lines << QString();
        lines << tr("The model has not produced a verdict yet - it needs a full window "
                    "of samples before it will commit to one, and it deliberately "
                    "answers \"unknown\" rather than guessing.");
    }

    // -- the anomaly --------------------------------------------------------
    if (anomaly.level > AnomalyLevel::None)
    {
        lines << QString();
        lines << tr("Anomaly detector: %1 severity - %2 (score %3).")
                     .arg(toString(anomaly.level), anomaly.reason)
                     .arg(anomaly.score, 0, 'f', 2);
        lines << tr("That verdict compares this device against its own recent history, "
                    "not against a fixed limit - so it can flag a machine that is still "
                    "inside its envelope but behaving unlike itself.");
    }

    lines << QString();
    lines << tr("Evidence age: newest sample %1 old, %2 samples recorded, %3 alarms "
                "raised since start.")
                 .arg(humanAge(device.lastSeen().secsTo(QDateTime::currentDateTime())),
                      QString::number(device.sampleCount()),
                      QString::number(device.alarmCount()));

    return lines.join(QLatin1Char('\n'));
}

QString AIChatWidget::explainHealth(const Device& device) const
{
    // Prefer the most recent forced analysis over the scheduled one: if the
    // operator pressed "Analyse now", they are asking about *that* result.
    FaultPrediction prediction = device.prediction();
    if (m_recentPredictions.contains(device.deviceId()))
    {
        prediction = m_recentPredictions.value(device.deviceId());
    }

    if (prediction.faultType.isEmpty())
    {
        return tr("%1 has not been assessed yet. The predictor needs at least a few "
                  "seconds of samples before it will produce a health score - until "
                  "then the honest answer is \"unknown\", not a number.")
            .arg(device.deviceId());
    }

    QString assessment;
    if (prediction.healthScore >= 85.0)
    {
        assessment = tr("Healthy. Nothing in the recent window suggests developing damage.");
    }
    else if (prediction.healthScore >= 65.0)
    {
        assessment = tr("Slightly degraded. Worth watching, not yet worth a stoppage.");
    }
    else if (prediction.healthScore >= 40.0)
    {
        assessment = tr("Degraded. This is the band where planned maintenance pays for "
                        "itself - the trend is established and there is still time to "
                        "schedule the intervention.");
    }
    else
    {
        assessment = tr("Poor. The measurements are consistent with an established fault.");
    }

    const DeviceStatistics statistics = device.statistics();

    return tr("%1: health score %2 %%, failure probability %3 %%.\n%4\n\n"
              "Over the retained window: %5 samples, peak temperature %6 °C, "
              "average %7 °C, peak vibration %8 mm/s, average current %9 A.\n"
              "Fault class: %10. Backend: %11.\n\n"
              "A health score is a summary of the shape of the recent measurements - "
              "level, variability and trend across all five channels. It is not a "
              "remaining-life estimate and should not be read as one.")
        .arg(device.deviceId())
        .arg(prediction.healthScore, 0, 'f', 0)
        .arg(prediction.failureProbability * 100.0, 0, 'f', 1)
        .arg(assessment)
        .arg(statistics.sampleCount)
        .arg(statistics.maxTemperature, 0, 'f', 1)
        .arg(statistics.avgTemperature, 0, 'f', 1)
        .arg(statistics.maxVibration, 0, 'f', 3)
        .arg(statistics.avgCurrent, 0, 'f', 2)
        .arg(prediction.faultType, prediction.backend);
}

QString AIChatWidget::explainAnomaly(const Device& device) const
{
    AnomalyEvent anomaly = device.lastAnomaly();
    if (m_recentAnomalies.contains(device.deviceId()))
    {
        anomaly = m_recentAnomalies.value(device.deviceId());
    }

    if (anomaly.level == AnomalyLevel::None)
    {
        return tr("%1: the anomaly detector has found nothing significant. Recent "
                  "behaviour is consistent with what this device normally does.\n\n"
                  "Note the distinction: \"no anomaly\" means \"unlike nothing I have "
                  "seen from this machine\". It does not mean \"within specification\" - "
                  "a device can sit just inside its limits and still be flagged, or sit "
                  "outside them and be flagged by the threshold rules instead.")
            .arg(device.deviceId());
    }

    const SensorSample& sample = device.latest();

    QStringList lines;
    lines << tr("%1: %2-severity anomaly detected (score %3, where 1.0 is maximally "
                "unusual).")
                 .arg(device.deviceId(), toString(anomaly.level))
                 .arg(anomaly.score, 0, 'f', 2);
    lines << QString();
    lines << tr("What was found: %1").arg(anomaly.reason);

    if (!anomaly.offendingFeatures.isEmpty())
    {
        lines << QString();
        lines << tr("Channels implicated:");
        for (int index : anomaly.offendingFeatures)
        {
            if (index >= 0 && index < static_cast<int>(kFeatureNames.size()))
            {
                const QString channel = QString::fromLatin1(kFeatureNames[static_cast<std::size_t>(index)]);
                QString reading;
                if (channel == QStringLiteral("temperature"))
                {
                    reading = numberOrDash(sample.isValid(), sample.temperature, 1) + QStringLiteral(" °C");
                }
                else if (channel == QStringLiteral("voltage"))
                {
                    reading = numberOrDash(sample.isValid(), sample.voltage, 1) + QStringLiteral(" V");
                }
                else if (channel == QStringLiteral("current"))
                {
                    reading = numberOrDash(sample.isValid(), sample.current, 2) + QStringLiteral(" A");
                }
                else if (channel == QStringLiteral("vibration"))
                {
                    reading = numberOrDash(sample.isValid(), sample.vibration, 3) + QStringLiteral(" mm/s");
                }
                else if (channel == QStringLiteral("rpm"))
                {
                    reading = numberOrDash(sample.isValid(), sample.rpm, 0) + QStringLiteral(" rpm");
                }
                lines << QStringLiteral("  - %1, now %2").arg(channel, reading);
            }
        }
    }

    lines << QString();
    lines << tr("The detector compares each channel against a robust baseline learned "
                "from this device's own history. It uses median and median absolute "
                "deviation rather than mean and standard deviation, so a spike cannot "
                "inflate the baseline enough to hide itself - which is the failure mode "
                "that makes naive threshold detectors miss exactly the events they exist "
                "to catch.");

    return lines.join(QLatin1Char('\n'));
}

QString AIChatWidget::recommendAction(const Device& device) const
{
    FaultPrediction prediction = device.prediction();
    if (m_recentPredictions.contains(device.deviceId()))
    {
        prediction = m_recentPredictions.value(device.deviceId());
    }

    if (prediction.faultType.isEmpty())
    {
        return tr("%1 has not been assessed yet, so I have no basis for a "
                  "recommendation. Ask again once it has produced a health score, or "
                  "press \"Analyse now\" in the device panel.")
            .arg(device.deviceId());
    }

    const FaultClass faultClass = faultClassFromString(prediction.faultType);
    const QString action = recommendedAction(faultClass);

    QStringList lines;
    lines << tr("Recommendation for %1 (health %2 %%, failure probability %3 %%, "
                "class: %4):")
                 .arg(device.deviceId())
                 .arg(prediction.healthScore, 0, 'f', 0)
                 .arg(prediction.failureProbability * 100.0, 0, 'f', 1)
                 .arg(prediction.faultType);
    lines << QString();

    if (faultClass == FaultClass::Healthy)
    {
        lines << tr("No intervention indicated. Continue the current inspection "
                    "interval.");
    }
    else
    {
        lines << action;

        // Urgency, derived from the numbers rather than asserted.  The thresholds
        // are the same ones the report uses, so a spoken recommendation and a
        // printed one cannot disagree about how urgent the same machine is.
        lines << QString();
        if (prediction.failureProbability >= 0.75 || prediction.healthScore < 35.0)
        {
            lines << tr("Urgency: high. The measured probability of failure is in the "
                        "range where a planned stop this shift is cheaper than an "
                        "unplanned one.");
        }
        else if (prediction.failureProbability >= 0.45 || prediction.healthScore < 60.0)
        {
            lines << tr("Urgency: medium. Schedule the work within the current "
                        "maintenance window rather than immediately.");
        }
        else
        {
            lines << tr("Urgency: low. Note it for the next planned inspection.");
        }
    }

    lines << QString();
    lines << tr("This is the action associated with fault class \"%1\", not a "
                "generated suggestion - the same function produces the recommendation "
                "in the exported maintenance report, so the two always agree.")
                 .arg(prediction.faultType);

    return lines.join(QLatin1Char('\n'));
}

QString AIChatWidget::describeTrend(const Device& device) const
{
    const std::vector<SensorSample> window = device.history();
    if (window.size() < 5)
    {
        return tr("%1 does not have enough retained history to describe a trend yet "
                  "(%2 samples). A slope computed from three points is noise.")
            .arg(device.deviceId())
            .arg(window.size());
    }

    // Least-squares slope per second, computed here rather than taken from the
    // feature vector, because the operator may be asking about a channel the
    // model does not consume.
    const auto slopeOf = [&window](const std::function<double(const SensorSample&)>& pick)
    {
        const double n = static_cast<double>(window.size());
        const qint64 t0 = window.front().timestamp.toMSecsSinceEpoch();
        double sumX = 0.0;
        double sumY = 0.0;
        double sumXY = 0.0;
        double sumXX = 0.0;

        for (const SensorSample& sample : window)
        {
            const double x = static_cast<double>(sample.timestamp.toMSecsSinceEpoch() - t0) / 1000.0;
            const double y = pick(sample);
            sumX += x;
            sumY += y;
            sumXY += x * y;
            sumXX += x * x;
        }

        const double denominator = n * sumXX - sumX * sumX;
        if (std::abs(denominator) < 1e-9)
        {
            return 0.0;
        }
        return (n * sumXY - sumX * sumY) / denominator;
    };

    struct ChannelTrend
    {
        const char* name;
        double slope;
        double unit;
    };

    const std::vector<ChannelTrend> trends{
        {"Temperature", slopeOf([](const SensorSample& s) { return s.temperature; }), 1.0},
        {"Current",     slopeOf([](const SensorSample& s) { return s.current; }),     1.0},
        {"Vibration",   slopeOf([](const SensorSample& s) { return s.vibration; }),   1.0},
        {"Voltage",     slopeOf([](const SensorSample& s) { return s.voltage; }),     1.0},
    };

    const double windowSeconds = static_cast<double>(
        window.front().timestamp.secsTo(window.back().timestamp));

    QStringList lines;
    lines << tr("%1, over the last %2 (%3 samples):")
                 .arg(device.deviceId(), humanAge(static_cast<qint64>(windowSeconds)))
                 .arg(window.size());
    lines << QString();

    // Deadbands are per-channel because the channels have different natural
    // noise.  A 0.001 mm/s/s drift is meaningless; a 0.02 °C/s ramp is not.
    const std::array<double, 4> deadbands{0.002, 0.001, 0.00005, 0.0005};

    for (std::size_t index = 0; index < trends.size(); ++index)
    {
        const ChannelTrend& trend = trends[index];
        const double perMinute = trend.slope * 60.0;
        const QString word = trendWord(trend.slope, deadbands[index]);

        lines << tr("  %1: %2, %3 per minute")
                     .arg(QString::fromLatin1(trend.name), word)
                     .arg(perMinute, 0, 'f', 4);
    }

    lines << QString();

    // The interpretation, which is the part that makes the numbers useful.  It
    // names the mechanism rather than restating the slope.
    const double temperatureSlope = trends[0].slope;
    const double vibrationSlope = trends[2].slope;

    if (vibrationSlope > 0.00005 && temperatureSlope > 0.002)
    {
        lines << tr("Vibration and temperature are rising together. That combination is "
                    "the signature of mechanical wear - friction converting into heat - "
                    "rather than of a load or ambient change, which would move "
                    "temperature without moving vibration.");
    }
    else if (temperatureSlope > 0.002)
    {
        lines << tr("Temperature is rising while vibration is flat. That points at a "
                    "thermal or electrical cause - cooling, ambient or supply - rather "
                    "than a mechanical one.");
    }
    else if (vibrationSlope > 0.00005)
    {
        lines << tr("Vibration is rising while temperature is flat. That is an early "
                    "mechanical signature - imbalance, misalignment or early bearing "
                    "damage - and it is the case where catching it now is worth the "
                    "most.");
    }
    else
    {
        lines << tr("No channel shows a sustained trend. The device is operating at a "
                    "stable level.");
    }

    return lines.join(QLatin1Char('\n'));
}

QString AIChatWidget::summariseFleet() const
{
    if (m_manager == nullptr || m_manager->deviceCount() == 0)
    {
        return tr("No devices are registered yet, so there is nothing to summarise.");
    }

    const DeviceManager::FleetSummary summary = m_manager->summary();

    QStringList lines;
    lines << tr("Fleet: %1 devices, %2 online.")
                 .arg(summary.total)
                 .arg(summary.online);
    lines << QString();
    lines << tr("  Normal:  %1").arg(summary.normal);
    lines << tr("  Warning: %1").arg(summary.warning);
    lines << tr("  Fault:   %1").arg(summary.fault);
    lines << tr("  Offline: %1").arg(summary.offline);
    lines << QString();
    lines << tr("Average temperature: %1 °C. Average health: %2 %%. "
                "%3 samples and %4 alarms recorded since start.")
                 .arg(summary.averageTemperature, 0, 'f', 1)
                 .arg(summary.averageHealth, 0, 'f', 1)
                 .arg(summary.totalSamples)
                 .arg(summary.totalAlarms);

    // Name the worst offenders explicitly.  A count tells the operator whether to
    // look further; the names tell them where.
    const QList<std::shared_ptr<Device>> devices = m_manager->devices();
    QList<const Device*> ranked;
    ranked.reserve(devices.size());
    for (const std::shared_ptr<Device>& device : devices)
    {
        if (device != nullptr)
        {
            ranked.append(device.get());
        }
    }

    std::sort(ranked.begin(), ranked.end(), [](const Device* left, const Device* right)
    {
        if (left->state() != right->state())
        {
            return left->state() > right->state();
        }
        return left->prediction().healthScore < right->prediction().healthScore;
    });

    if (ranked.size() > 0)
    {
        lines << QString();
        lines << tr("Needing attention first:");
        const int shown = std::min<int>(3, static_cast<int>(ranked.size()));
        for (int index = 0; index < shown; ++index)
        {
            const Device* device = ranked[static_cast<std::size_t>(index)];
            const QString health = device->prediction().faultType.isEmpty()
                                       ? tr("not yet assessed")
                                       : tr("health %1 %%, %2")
                                             .arg(device->prediction().healthScore, 0, 'f', 0)
                                             .arg(device->prediction().faultType);
            lines << tr("  %1 - %2 (%3)").arg(device->deviceId(),
                                              toString(device->state()), health);
        }
    }

    return lines.join(QLatin1Char('\n'));
}

QString AIChatWidget::describeCapabilities() const
{
    // Stating the boundary is the feature.  An assistant that lets an operator
    // believe it can do more than it can is a liability; one that says exactly
    // what it answers is a tool they will actually use.
    return tr("What I can answer:\n"
              "  - why a device is in its current state, quoting the measurement that "
              "breached which limit\n"
              "  - what its health score and fault class mean\n"
              "  - what the anomaly detector found and on which channels\n"
              "  - the recommended maintenance action for the fault class, with an "
              "urgency derived from the measured probability\n"
              "  - the direction and rate of each channel's trend, and what the "
              "combination suggests\n"
              "  - a fleet-wide summary with the worst assets named\n\n"
              "What I do not do:\n"
              "  - I do not predict remaining useful life. Nothing in this system is "
              "trained for that, and a number invented for it would be worse than none.\n"
              "  - I do not answer questions unrelated to the monitored equipment.\n"
              "  - I do not speculate about causes the data does not show. If a "
              "channel has no reading, I say \"--\" rather than guessing.\n\n"
              "Ask about a device by its id, its name, or just select it first and ask "
              "\"why is it alarming?\"");
}

}   // namespace saim

