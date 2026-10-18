#pragma once
#include "common/Types.h"
#include <QMutex>
#include <QMutexLocker>
#include <deque>
#include <variant>

namespace saim {
class RecordInbox final
{
public:
    using Record = std::variant<AlarmRecord, FaultPrediction, AnomalyEvent>;
    struct Statistics { quint64 offered = 0, taken = 0, rejected = 0; int depth = 0, highWater = 0; };
    bool push(Record record)
    {
        QMutexLocker lock(&m_mutex);
        ++m_stats.offered;
        if (m_closed || m_queue.size() >= 2048) { ++m_stats.rejected; return false; }
        m_queue.push_back(std::move(record));
        m_stats.highWater = std::max(m_stats.highWater, static_cast<int>(m_queue.size()));
        return true;
    }
    QVector<Record> take(int limit)
    {
        QMutexLocker lock(&m_mutex);
        QVector<Record> result;
        while (!m_queue.empty() && result.size() < limit)
        { result.append(std::move(m_queue.front())); m_queue.pop_front(); ++m_stats.taken; }
        return result;
    }
    void close() { QMutexLocker lock(&m_mutex); m_closed = true; }
    void reopen() { QMutexLocker lock(&m_mutex); m_closed = false; }
    Statistics statistics() const
    { QMutexLocker lock(&m_mutex); auto result = m_stats; result.depth = static_cast<int>(m_queue.size()); return result; }
private:
    mutable QMutex m_mutex;
    std::deque<Record> m_queue;
    Statistics m_stats;
    bool m_closed = false;
};
} // namespace saim
