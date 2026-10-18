#pragma once

// ---------------------------------------------------------------------------
// AIChatWidget.h - the maintenance assistant.
//
// What this is, stated plainly
// ----------------------------
// This is an *explanation* interface, not a chat bot.  It answers a closed set of
// questions about the devices this application is monitoring - "why is device
// 001 alarming?", "what should I do about it?", "how has it behaved today?" -
// and it answers them from data the system actually measured.
//
// Every answer is assembled from a query result or an AI verdict, and each one
// carries its provenance: which numbers were used, which backend produced the
// inference, and how old the assessment is.  There is no language model behind
// this widget and the code does not pretend otherwise, for one reason worth
// stating in an interview:
//
//   A maintenance recommendation that cannot be traced to a measurement is not
//   a recommendation, it is a guess with confident phrasing.  An operator who
//   acts on a fabricated trend line costs the plant more than the assistant
//   saves - and they will have no way to know which answers were real.
//
// So the "reasoning" here is explicit and inspectable: the fault class names the
// mechanism, the feature vector supplies the evidence, the anomaly deviations
// supply the magnitude, and the recommendation comes from the fault class.  A
// reviewer can read the answer and check every clause of it against the database.
//
// The natural-language front end is a small intent matcher rather than an LLM.
// That is a deliberate engineering trade-off, and the honest framing is: it
// handles the questions an operator asks in a plant, it does so with zero
// latency and no network dependency, and it fails visibly ("I don't have data
// for that") instead of plausibly.
// ---------------------------------------------------------------------------

#include "common/Types.h"

#include <QHash>
#include <QVector>
#include <QWidget>

#include <memory>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace saim {

class DeviceManager;
class Device;

/// One exchange, kept so the transcript can be re-rendered or exported.
struct ChatMessage
{
    enum class Role
    {
        User,
        Assistant,
        System      ///< an error or a status note, styled differently
    };

    Role    role = Role::Assistant;
    QString text;
    QDateTime timestamp;
    /// Device the answer is about, so a click can select it.
    QString deviceId;
};

class AIChatWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit AIChatWidget(DeviceManager* manager, QWidget* parent = nullptr);
    ~AIChatWidget() override;

public slots:
    /// Answers a question posed from elsewhere in the UI (a context-menu item,
    /// a tile's "explain this" action).  Appends both the question and the
    /// answer to the transcript, exactly as if typed.
    void ask(const QString& question);

    /// Sets the device the assistant should assume when a question does not name
    /// one - "why is it alarming?" following a selection.
    void setContextDevice(const QString& deviceId);

    /// Drops in a freshly produced AI verdict so an answer can quote the numbers
    /// from a forced analysis rather than the last scheduled one.
    void applyAnalysis(const QString& deviceId, const saim::FaultPrediction& prediction,
                       const saim::AnomalyEvent& anomaly);

    /// Dims the input and explains why when no inference backend is available.
    void setBackendDescription(const QString& description);

    void clearTranscript();

signals:
    /// The assistant referenced a device; the controller selects it so the
    /// dashboard follows the conversation.
    void deviceReferenced(const QString& deviceId);

private slots:
    void onSubmit();
    void onSuggestionClicked();

private:
    void buildUi();
    void appendMessage(const ChatMessage& message);
    void rebuildTranscript();

    /// The whole "intelligence" of the widget: classifies the question into an
    /// intent, then composes the answer from data.  Documented at length in the
    /// .cpp, because the intent list *is* the specification of what this can
    /// answer - and being explicit about the boundary is the point.
    [[nodiscard]] QString answer(const QString& question);

    /// Individual answer builders.  Each one returns text derived only from
    /// measured values, and each is separately testable.
    [[nodiscard]] QString explainAlarm(const Device& device) const;
    [[nodiscard]] QString explainHealth(const Device& device) const;
    [[nodiscard]] QString explainAnomaly(const Device& device) const;
    [[nodiscard]] QString recommendAction(const Device& device) const;
    [[nodiscard]] QString describeTrend(const Device& device) const;
    [[nodiscard]] QString summariseFleet() const;
    [[nodiscard]] QString describeCapabilities() const;

    /// Resolves a device from the question text or the current context.
    /// Returns null when neither names a device that exists.
    [[nodiscard]] Device* resolveDevice(const QString& question, QString* matchedToken = nullptr) const;

    DeviceManager* m_manager = nullptr;

    QScrollArea*  m_scroll = nullptr;
    QWidget*      m_transcriptHost = nullptr;
    QVBoxLayout*  m_transcriptLayout = nullptr;
    QPlainTextEdit* m_input = nullptr;
    QPushButton*  m_sendButton = nullptr;
    QPushButton*  m_clearButton = nullptr;
    QLabel*       m_backendLabel = nullptr;

    QVector<ChatMessage> m_messages;
    QString m_contextDeviceId;
    QString m_backendDescription;

    /// Latest forced-analysis result per device, so an answer can cite the
    /// numbers the operator explicitly asked for rather than the scheduled ones.
    QHash<QString, FaultPrediction> m_recentPredictions;
    QHash<QString, AnomalyEvent>    m_recentAnomalies;
};

}   // namespace saim
