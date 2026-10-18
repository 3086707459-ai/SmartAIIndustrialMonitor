#include "thread/AcquisitionWorker.h"

#include "core/ConfigManager.h"
#include "core/Logger.h"

#include <QHash>
#include <QJsonObject>
#include <QTimer>

namespace saim {

namespace {
/// Rolling window used to estimate the inbound sample rate.
constexpr qint64 kRateWindowMs = 2000;
}   // namespace

AcquisitionWorker::AcquisitionWorker(ConfigManager* config, QObject* parent)
    : ThreadedWorker(QStringLiteral("AcquisitionThread"), parent)
    , m_config(config)
{
}

AcquisitionWorker::~AcquisitionWorker() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle - both of these run on the worker thread
// ---------------------------------------------------------------------------

void AcquisitionWorker::onStart()
{
    m_uptime.start();

    // Constructed here rather than in the constructor: this function is already
    // executing on AcquisitionThread, so the socket is *born* with the correct
    // thread affinity and never has to be moved.
    m_client = new TcpClient(this);

    connect(m_client, &TcpClient::sampleReceived, this, [this](const SensorSample& sample) {
        ++m_receivedSamples;
        updateRateEstimate();
        emit sampleAcquired(sample);

    });

    connect(m_client, &TcpClient::stateChanged, this, [this](ConnectionState state) {
        m_connectionState = state;
        emit connectionStateChanged(state);
    });

    connect(m_client, &TcpClient::statisticsChanged, this,
            [this](const TcpClient::Statistics& statistics) {
                QMutexLocker lock(&m_statisticsMutex);
                m_statistics = statistics;
            });

    connect(m_client, &TcpClient::errorOccurred, this, &AcquisitionWorker::gatewayError);

    applyOptionsFromConfig();

    if (m_config != nullptr && m_config->network().autoConnect)
    {
        SAIM_INFO(logcat::kNetwork, QStringLiteral("Acquisition worker: auto-connecting"));
        m_client->start();
    }
}

void AcquisitionWorker::onStop()
{
    if (m_client != nullptr)
    {
        // onStop() is invoked with a blocking queued call, so this executes on
        // the worker thread and the socket is closed by its own thread.
        m_client->stop();
        // Destroy socket and member timers before the worker moves back to its
        // owner thread. A deferred child deletion could otherwise migrate only
        // the client, leaving its unparented value members on the old thread.
        delete m_client;
        m_client = nullptr;
    }

    SAIM_INFO(logcat::kNetwork, QStringLiteral("Acquisition worker: link closed"));
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void AcquisitionWorker::applyOptionsFromConfig()
{
    if (m_config == nullptr)
    {
        return;
    }

    const ConfigManager::NetworkConfig& network = m_config->network();
    if (m_client == nullptr)
    {
        return;
    }

    TcpClient::Options options;
    options.host            = network.remoteHost;
    options.port            = network.remotePort;
    options.reconnectBaseMs = network.reconnectBaseMs;
    options.reconnectMaxMs  = network.reconnectMaxMs;
    options.heartbeatMs     = network.heartbeatMs;

    m_client->setOptions(options);
    SAIM_INFO(logcat::kNetwork,
              QStringLiteral("Acquisition target configured: %1:%2")
                  .arg(options.host)
                  .arg(options.port));
}

// ---------------------------------------------------------------------------
// Rate estimation / diagnostics
// ---------------------------------------------------------------------------

void AcquisitionWorker::updateRateEstimate()
{
    const qint64 now = m_uptime.elapsed();
    if (m_rateWindowStartMs == 0)
    {
        m_rateWindowStartMs = now;
        m_rateWindowCount = 0;
    }

    ++m_rateWindowCount;

    const qint64 elapsed = now - m_rateWindowStartMs;
    if (elapsed >= kRateWindowMs)
    {
        m_samplesPerSecond = static_cast<double>(m_rateWindowCount) * 1000.0
                             / static_cast<double>(elapsed);
        m_rateWindowStartMs = now;
        m_rateWindowCount = 0;
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void AcquisitionWorker::connectToGateway()
{
    if (m_client == nullptr)
    {
        // start() was not called, or onStop() already ran.
        reportFailure(QStringLiteral("connectToGateway() before the worker started"));
        return;
    }
    applyOptionsFromConfig();
    m_client->start();
}

void AcquisitionWorker::disconnectFromGateway()
{
    if (m_client != nullptr)
    {
        m_client->stop();
    }
}

void AcquisitionWorker::sendCommand(const QString& deviceId, const QString& command)
{
    if (m_client == nullptr)
    {
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), deviceId);
    payload.insert(QStringLiteral("command"), command);
    m_client->send(MessageType::Command, payload);
}

}   // namespace saim
