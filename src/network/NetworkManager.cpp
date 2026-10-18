#include "network/NetworkManager.h"

#include "core/ConfigManager.h"
#include "core/Logger.h"

#include <QJsonObject>
#include <QMetaObject>

namespace saim {

NetworkManager::NetworkManager(ConfigManager* config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    setObjectName(QStringLiteral("NetworkManager"));

    // The thread and the client are built once and live for the whole process.
    // Recreating them per connection would mean a thread start/stop per retry,
    // which is far more expensive than the reconnect itself.
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("NetworkThread"));

    m_client = new TcpClient;              // no parent: it belongs to m_thread
    m_client->moveToThread(m_thread);

    // The client must be destroyed on its own thread, otherwise Qt asserts on
    // "QObject: Cannot create children for a parent that is in a different
    // thread" during teardown.  Connecting finished() -> deleteLater is the
    // documented idiom and is what makes the destructor sequence below safe.
    connect(m_thread, &QThread::finished, m_client, &QObject::deleteLater);

    wireClientSignals();

    if (m_config != nullptr)
    {
        connect(m_config, &ConfigManager::reloaded, this, &NetworkManager::applyConfiguration);
    }
}

NetworkManager::~NetworkManager()
{
    stop();

    if (m_thread != nullptr)
    {
        // deleteLater on the QThread: deleting a QThread from the thread it
        // manages is undefined, and deleting it from the creator while it may
        // still be running is worse.  After wait() returned it is stopped for
        // good, so the deferred delete is safe.
        m_thread->deleteLater();
        m_thread = nullptr;
    }
    m_client = nullptr;   // owned by the thread's finished() handler
}

// ---------------------------------------------------------------------------
// Signal wiring
// ---------------------------------------------------------------------------

void NetworkManager::wireClientSignals()
{
    // Every connection here is implicitly queued: the client lives on
    // NetworkThread, the manager lives on the UI thread, so Qt picks
    // Qt::QueuedConnection and marshals the payload through the event loop.
    // That is why SensorSample must be a registered metatype.

    connect(m_client, &TcpClient::connected, this, [this] {
        SAIM_INFO(logcat::kNetwork, QStringLiteral("Acquisition link established"));
        emit connected();
    });

    connect(m_client, &TcpClient::disconnected, this, [this] {
        SAIM_WARN(logcat::kNetwork, QStringLiteral("Acquisition link lost"));
        emit disconnected();
    });

    connect(m_client, &TcpClient::stateChanged, this, [this](ConnectionState state) {
        m_state = state;
        emit stateChanged(state);
    });

    connect(m_client, &TcpClient::sampleReceived, this, &NetworkManager::sampleReceived);

    connect(m_client, &TcpClient::errorOccurred, this, &NetworkManager::errorOccurred);

    connect(m_client, &TcpClient::statisticsChanged, this,
            [this](const TcpClient::Statistics& statistics) {
                m_statistics = statistics;
                emit statisticsChanged(statistics);
            });
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void NetworkManager::start()
{
    if (!m_thread->isRunning())
    {
        m_thread->start();
        SAIM_INFO(logcat::kThread,
                  QStringLiteral("Network thread started (%1)").arg(m_thread->objectName()));
    }

    applyConfiguration();

    // Queued: the slot runs on NetworkThread, so TcpClient::start() and every
    // subsequent socket operation happen on one thread and never on ours.
    QMetaObject::invokeMethod(m_client, "start", Qt::QueuedConnection);
    m_started = true;
}

void NetworkManager::stop()
{
    if (m_client != nullptr)
    {
        QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
    }
    m_started = false;

    if (m_thread != nullptr && m_thread->isRunning())
    {
        m_thread->quit();
        // Blocking join.  A timeout here would mean a slot is wedged on the
        // worker; better to hang visibly in shutdown than to exit with a live
        // thread still holding a socket.
        if (!m_thread->wait(5000))
        {
            SAIM_ERROR(logcat::kThread,
                       QStringLiteral("Network thread did not stop within 5 s - terminating"));
            m_thread->terminate();
            m_thread->wait(1000);
        }
        SAIM_INFO(logcat::kThread, QStringLiteral("Network thread stopped"));
    }
}

void NetworkManager::applyConfiguration()
{
    if (m_config == nullptr || m_client == nullptr)
    {
        return;
    }

    const ConfigManager::NetworkConfig& network = m_config->network();

    TcpClient::Options options;
    options.host           = network.remoteHost;
    options.port           = network.remotePort;
    options.reconnectBaseMs = network.reconnectBaseMs;
    options.reconnectMaxMs  = network.reconnectMaxMs;
    options.heartbeatMs     = network.heartbeatMs;

    // Options is a plain value type; invoking the setter through the meta-object
    // system would require Q_DECLARE_METATYPE on it, so instead we post a lambda
    // that captures a copy and runs on the worker thread.  Qt 5.10+ supports
    // this, and it keeps the client's API free of Qt meta-type boilerplate.
    QMetaObject::invokeMethod(
        m_client,
        [client = m_client, options] { client->setOptions(options); },
        Qt::QueuedConnection);

    SAIM_DEBUG(logcat::kNetwork,
               QStringLiteral("Network endpoints configured: %1:%2")
                   .arg(options.host)
                   .arg(options.port));
}

void NetworkManager::sendCommand(const QString& deviceId, const QString& command)
{
    QMetaObject::invokeMethod(
        m_client,
        [client = m_client, deviceId, command] {
            QJsonObject payload;
            payload.insert(QStringLiteral("id"), deviceId);
            payload.insert(QStringLiteral("command"), command);
            client->send(MessageType::Command, payload);
        },
        Qt::QueuedConnection);
}

QString NetworkManager::endpointDescription() const
{
    if (m_config == nullptr)
    {
        return QStringLiteral("unconfigured");
    }
    const ConfigManager::NetworkConfig& network = m_config->network();
    return QStringLiteral("%1:%2").arg(network.remoteHost).arg(network.remotePort);
}

}   // namespace saim
