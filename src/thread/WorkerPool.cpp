#include "thread/WorkerPool.h"

#include "core/Logger.h"
#include "thread/AIWorker.h"
#include "thread/AcquisitionWorker.h"
#include "thread/PersistenceWorker.h"

namespace saim {

WorkerPool::WorkerPool(ConfigManager* config, DeviceManager* devices, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_devices(devices)
{
}

WorkerPool::~WorkerPool()
{
    stop();
    delete m_acquisition;
    delete m_ai;
    delete m_persistence;
}

void WorkerPool::create()
{
    if (m_created)
    {
        return;
    }

    // No parent for any of them.  Each worker is moved onto its own QThread, and
    // a QObject with a parent cannot be moved.  Ownership is by these raw
    // pointers, and the destructor order below is what makes that safe.
    m_acquisition = new AcquisitionWorker(m_config);
    m_persistence = new PersistenceWorker(m_config);
    m_ai          = new AIWorker(m_config);

    for (ThreadedWorker* worker : {static_cast<ThreadedWorker*>(m_acquisition),
                                   static_cast<ThreadedWorker*>(m_persistence),
                                   static_cast<ThreadedWorker*>(m_ai)})
    {
        connect(worker, &ThreadedWorker::started, this, [this, worker] {
            emit workerStarted(worker->threadName());
        });
    }

    m_created = true;
}

void WorkerPool::start()
{
    if (!m_created)
    {
        create();
    }

    // Start consumers before the producer so their initialisation is queued first.
    if (m_persistence != nullptr && !m_persistence->isRunning())
    {
        m_persistence->start();
    }

    if (m_ai != nullptr && !m_ai->isRunning())
    {
        m_ai->start();
    }
    if (m_acquisition != nullptr && !m_acquisition->isRunning())
    {
        m_acquisition->start();
    }
}

void WorkerPool::stop(const std::function<void()>& beforeStorageStop)
{
    // See the header for why this order and not the exact reverse of start().
    // The short version: drain the consumer last.
    if (m_acquisition != nullptr && m_acquisition->isRunning())
    {
        m_acquisition->stop();
        emit workerStopped(m_acquisition->threadName());
    }

    if (m_ai != nullptr && m_ai->isRunning())
    {
        m_ai->stop();
        emit workerStopped(m_ai->threadName());
    }

    if (beforeStorageStop) beforeStorageStop();
    if (m_persistence != nullptr && m_persistence->isRunning())
    {
        m_persistence->stop();
        emit workerStopped(m_persistence->threadName());
    }
}

bool WorkerPool::isRunning() const
{
    return (m_acquisition != nullptr && m_acquisition->isRunning())
           || (m_persistence != nullptr && m_persistence->isRunning())
           || (m_ai != nullptr && m_ai->isRunning());
}

}   // namespace saim
