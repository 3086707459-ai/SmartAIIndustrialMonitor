#pragma once

// ---------------------------------------------------------------------------
// WorkerPool.h - owns the lifetime of every background worker.
//
// Why this is a separate class rather than five members of the main window
// ---------------------------------------------------------------------------
// Worker lifetime is the part of a threaded application that is easiest to get
// subtly wrong: a thread started after the object that consumes its signals is
// destroyed, a worker never stopped so the process hangs on exit, a stop()
// called from the wrong thread.  None of that is UI code, and burying it in a
// window class makes it untestable.
//
// So ownership is centralised here, in a fixed order:
//
//   start:  persistence -> ai -> acquisition
//   stop:   acquisition -> ai -> persistence
//
// Stop order is the reverse of the data flow, and deliberately not the reverse
// of start order.  Acquisition stops first so no *new* samples enter the system;
// AI stops next so nothing new is queued for persistence; persistence stops last
// so it can drain and flush everything the other two produced.  Reversing that
// would throw away the final batch - the samples closest to the shutdown, which
// are the ones an investigator wants.
//
// The pool does not connect the workers to each other.  That wiring belongs to
// the composition root, where the whole data flow is visible in one place.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>

#include <memory>
#include <functional>

namespace saim {

class AIWorker;
class AcquisitionWorker;
class ConfigManager;
class DeviceManager;
class PersistenceWorker;

class WorkerPool final : public QObject
{
    Q_OBJECT
public:
    explicit WorkerPool(ConfigManager* config, DeviceManager* devices, QObject* parent = nullptr);
    ~WorkerPool() override;

    WorkerPool(const WorkerPool&)            = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Creates every worker.  Must be called before start().  Workers are
    /// created on the calling thread and moved onto their own by start().
    void create();

    /// Starts every worker, in the order documented above.
    void start();

    /// Stops every worker.  Idempotent, and safe from the destructor.
    void stop(const std::function<void()>& beforeStorageStop = {});

    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] AcquisitionWorker* acquisition() const noexcept { return m_acquisition; }
    [[nodiscard]] PersistenceWorker* persistence() const noexcept { return m_persistence; }
    [[nodiscard]] AIWorker*          ai() const noexcept          { return m_ai; }

signals:
    /// Emitted once per worker as it comes up, so the splash/status bar can name
    /// what is still starting.
    void workerStarted(const QString& workerName);
    void workerStopped(const QString& workerName);

private:
    ConfigManager* m_config  = nullptr;
    DeviceManager* m_devices = nullptr;

    AcquisitionWorker* m_acquisition = nullptr;
    PersistenceWorker* m_persistence = nullptr;
    AIWorker*          m_ai          = nullptr;

    bool m_created = false;
};

}   // namespace saim
