#pragma once

// ---------------------------------------------------------------------------
// ThreadedWorker.h - base class for every long-lived worker in the project.
//
// The problem it solves
// ---------------------
// Qt gives you several ways to get work off the UI thread, and two of them are
// wrong for this application:
//
//   * Subclassing QThread and putting logic in run() means the *thread object*
//     lives on the creating thread while the work happens on another.  Any slot
//     connected to that object then executes on the wrong side, and the classic
//     "my worker's slots run on the UI thread" bug appears.  It also cannot use
//     an event loop, so it cannot receive queued calls at all.
//
//   * A raw std::thread loses Qt's event loop, timers, and queued connections,
//     which is most of what this codebase relies on.
//
// The correct pattern - used throughout here - is a plain QObject with
// moveToThread(), plus a controller that owns the QThread.  This class packages
// the boilerplate so every worker gets the same, correct lifecycle.
//
// Lifecycle contract
// ------------------
//   1. `ThreadedWorker worker;`            (created on the UI thread)
//   2. `worker.start()`                     creates the thread, moves itself onto
//                                            it, and invokes onStart() there
//   3. queued signals drive the worker      each slot runs on the worker thread
//   4. `worker.stop()`                      invokes onStop() ON the worker
//                                            thread, then quits and joins
//
// Step 4 is the part people get wrong.  Resources the worker owns - a socket, a
// database connection, a file handle - must be released on the thread that
// created them, so onStop() runs queued, not from the caller.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QThread>

namespace saim {

class ThreadedWorker : public QObject
{
    Q_OBJECT
public:
    explicit ThreadedWorker(QString threadName, QObject* parent = nullptr);
    ~ThreadedWorker() override;

    ThreadedWorker(const ThreadedWorker&)            = delete;
    ThreadedWorker& operator=(const ThreadedWorker&) = delete;

    /// Creates and starts the worker thread, then calls onStart() on it.
    /// Idempotent.  Returns false if the thread could not be started.
    bool start();

    /// Calls onStop() on the worker thread, then quits and joins it.
    /// Idempotent, and safe to call from the destructor.
    void stop();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QThread* thread() const noexcept { return m_thread; }
    [[nodiscard]] QString threadName() const { return m_threadName; }

    /// Runs `functor` on the worker thread and waits for it to finish.
    ///
    /// Use sparingly and never from a slot that the worker itself may call back
    /// into: a blocking invocation onto a thread that is waiting on you is a
    /// deadlock, and Qt will detect it and warn rather than hang forever only
    /// when the target thread is the caller's own.
    template <typename Functor>
    void invokeBlocking(Functor&& functor)
    {
        if (!isRunning() || QThread::currentThread() == m_thread)
        {
            // Not started: run inline so callers do not have to special-case
            // start-up ordering (a query issued before start() still works).
            functor();
            return;
        }
        QMetaObject::invokeMethod(this, std::forward<Functor>(functor),
                                  Qt::BlockingQueuedConnection);
    }

    /// Posts `functor` to the worker thread without waiting.
    template <typename Functor>
    void invokeAsync(Functor&& functor)
    {
        if (!isRunning())
        {
            return;
        }
        QMetaObject::invokeMethod(this, std::forward<Functor>(functor), Qt::QueuedConnection);
    }

signals:
    void started();
    void stopped();
    void failed(const QString& reason);

protected:
    /// Called on the worker thread.  Acquire resources here.
    virtual void onStart() {}

    /// Called on the worker thread just before the event loop exits.
    /// Release resources here - never in the destructor, which runs on the
    /// owning thread.
    virtual void onStop() {}

    /// Reports a fatal, non-recoverable problem.  Does not stop the worker:
    /// the decision to keep retrying belongs to the subclass.
    void reportFailure(const QString& reason);

private:
    QString m_threadName;
    QThread* m_thread = nullptr;
    QThread* m_ownerThread = nullptr;
    bool     m_ownsThread = false;
};

}   // namespace saim
