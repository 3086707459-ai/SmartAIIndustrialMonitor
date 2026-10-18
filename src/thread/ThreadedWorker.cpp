#include "thread/ThreadedWorker.h"

#include "core/Logger.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

namespace saim {

ThreadedWorker::ThreadedWorker(QString threadName, QObject* parent)
    : QObject(parent)
    , m_threadName(std::move(threadName))
    , m_ownerThread(QThread::currentThread())
{
    // Give the *worker* a name too: Logger reads QThread::currentThread()
    // ->objectName(), and that name is what makes a log line attributable to the
    // thread that produced it.
    setObjectName(m_threadName);
}

ThreadedWorker::~ThreadedWorker()
{
    // stop() joins the worker thread, so by the time the QThread object is
    // destroyed nothing can still be executing on it.
    stop();

    if (m_ownsThread && m_thread != nullptr)
    {
        delete m_thread;
        m_thread = nullptr;
    }
}

bool ThreadedWorker::start()
{
    if (isRunning())
    {
        return true;
    }

    if (m_thread == nullptr)
    {
        // No parent: the thread must outlive the caller's QObject tree during
        // shutdown, and parenting it to this object would make Qt try to delete
        // it from the wrong thread.  Ownership is explicit instead.
        m_thread = new QThread;
        m_thread->setObjectName(m_threadName);
        m_ownsThread = true;
    }

    // Order matters: moveToThread() before start() means the worker's event loop
    // exists by the time the first queued call arrives, so nothing posted
    // between the two is lost.
    moveToThread(m_thread);
    m_thread->start();

    // onStart() must execute on the worker thread, hence the queued invocation
    // rather than a direct call.  QThread::start() has already been called, so
    // the loop is about to spin and will pick this up.
    QMetaObject::invokeMethod(
        this,
        [this] {
            SAIM_INFO(logcat::kThread,
                      QStringLiteral("Worker '%1' starting on thread 0x%2")
                          .arg(m_threadName)
                          .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));
            onStart();
            emit started();
        },
        Qt::QueuedConnection);

    return true;
}

void ThreadedWorker::stop()
{
    if (m_thread == nullptr || !m_thread->isRunning())
    {
        return;
    }

    // Release worker-owned resources on the worker thread.  Blocking, so that
    // when this returns the socket/database is closed and nothing else can be
    // posted to a half-torn-down object.
    //
    // Guard: if stop() is somehow invoked *from* the worker thread, a blocking
    // invocation would deadlock.  Detect it and call directly instead.
    if (QThread::currentThread() == m_thread)
    {
        onStop();
        moveToThread(m_ownerThread);
        m_thread->quit();
        emit stopped();
        return; // A thread cannot join itself; its owner joins on stop().
    }
    else if (QObject::thread() == m_thread)
    {
        QMetaObject::invokeMethod(
            this,
            [this] {
                SAIM_INFO(logcat::kThread, QStringLiteral("Worker '%1' stopping").arg(m_threadName));
                onStop();
                moveToThread(m_ownerThread);
            },
            Qt::BlockingQueuedConnection);
    }

    m_thread->quit();

    // Never terminate a thread while it might own a SQL transaction or mutex.
    m_thread->wait();

    emit stopped();
}

bool ThreadedWorker::isRunning() const
{
    return m_thread != nullptr && m_thread->isRunning();
}

void ThreadedWorker::reportFailure(const QString& reason)
{
    SAIM_ERROR(logcat::kThread,
               QStringLiteral("Worker '%1' failure: %2").arg(m_threadName, reason));
    emit failed(reason);
}

}   // namespace saim
