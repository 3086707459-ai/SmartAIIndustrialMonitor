#pragma once

#include "common/Types.h"
#include <QDeadlineTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>

namespace saim {

// No QObject events per sample. Producers touch only this locked mailbox;
// consumers drain bounded batches from a timer on their own thread.
class SampleInbox final
{
public:
    enum class Policy { WaitThenReject, DropOldest, LatestPerDevice };
    struct Statistics
    {
        quint64 offered = 0, taken = 0, rejected = 0, evicted = 0;
        quint64 coalesced = 0, cancelled = 0, waits = 0;
        int depth = 0, capacity = 0, highWater = 0;
        double waitMs = 0, maxLatencyMs = 0, p95LatencyUpperMs = 0;
    };

    explicit SampleInbox(int capacity, Policy policy, int waitMs = 5)
        : m_capacity(std::clamp(capacity, 1, 65536)), m_policy(policy),
          m_waitMs(std::clamp(waitMs, 0, 50)) {}

    bool push(const SensorSample& sample)
    {
        QMutexLocker lock(&m_mutex);
        ++m_stats.offered;
        if (m_closed) { ++m_stats.rejected; return false; }
        if (m_policy == Policy::LatestPerDevice)
        {
            const auto found = std::find_if(m_queue.begin(), m_queue.end(),
                [&sample](const Entry& entry) { return entry.sample.deviceId == sample.deviceId; });
            if (found != m_queue.end())
            {
                *found = Entry{sample, Clock::now()};
                ++m_stats.coalesced;
                return true;
            }
        }
        if (static_cast<int>(m_queue.size()) == m_capacity && m_policy == Policy::WaitThenReject)
        {
            ++m_stats.waits;
            const auto start = Clock::now();
            QDeadlineTimer deadline(m_waitMs);
            while (!m_closed && static_cast<int>(m_queue.size()) == m_capacity && !deadline.hasExpired())
                m_space.wait(&m_mutex, deadline);
            m_stats.waitMs += milliseconds(Clock::now() - start);
        }
        if (m_closed) { ++m_stats.rejected; return false; }
        if (static_cast<int>(m_queue.size()) == m_capacity)
        {
            if (m_policy == Policy::WaitThenReject) { ++m_stats.rejected; return false; }
            m_queue.pop_front();
            ++m_stats.evicted;
        }
        m_queue.push_back(Entry{sample, Clock::now()});
        m_stats.highWater = std::max(m_stats.highWater, static_cast<int>(m_queue.size()));
        return true;
    }

    QVector<SensorSample> take(int limit)
    {
        QMutexLocker lock(&m_mutex);
        QVector<SensorSample> result;
        const int count = std::min(std::max(0, limit), static_cast<int>(m_queue.size()));
        result.reserve(count);
        for (int i = 0; i < count; ++i)
        {
            const double latency = milliseconds(Clock::now() - m_queue.front().queuedAt);
            m_stats.maxLatencyMs = std::max(m_stats.maxLatencyMs, latency);
            const auto bucket = std::lower_bound(m_bounds.begin(), m_bounds.end(), latency);
            ++m_latency[static_cast<size_t>(bucket - m_bounds.begin())];
            result.append(std::move(m_queue.front().sample));
            m_queue.pop_front();
        }
        m_stats.taken += static_cast<quint64>(count);
        if (count != 0) m_space.wakeAll();
        return result;
    }

    // Owner calls reopen only after the previous consumer has stopped. Counters
    // are lifetime totals; queued samples never silently survive a restart.
    void reopen() { QMutexLocker lock(&m_mutex); m_closed = false; }
    void close() { QMutexLocker lock(&m_mutex); m_closed = true; m_space.wakeAll(); }
    void discard()
    {
        QMutexLocker lock(&m_mutex);
        m_stats.cancelled += static_cast<quint64>(m_queue.size());
        m_queue.clear();
        m_space.wakeAll();
    }
    Statistics statistics() const
    {
        QMutexLocker lock(&m_mutex);
        auto result = m_stats;
        result.depth = static_cast<int>(m_queue.size());
        result.capacity = m_capacity;
        quint64 cumulative = 0;
        for (size_t i = 0; i < m_latency.size() && result.taken != 0; ++i)
        {
            cumulative += m_latency[i];
            if (static_cast<double>(cumulative) >= static_cast<double>(result.taken) * 0.95)
            {
                result.p95LatencyUpperMs = i < m_bounds.size() ? m_bounds[i] : result.maxLatencyMs;
                break;
            }
        }
        return result;
    }

private:
    using Clock = std::chrono::steady_clock;
    static double milliseconds(Clock::duration value)
    { return std::chrono::duration<double, std::milli>(value).count(); }
    struct Entry { SensorSample sample; Clock::time_point queuedAt; };
    const int m_capacity;
    const Policy m_policy;
    const int m_waitMs;
    mutable QMutex m_mutex;
    QWaitCondition m_space;
    std::deque<Entry> m_queue;
    bool m_closed = false;
    Statistics m_stats;
    const std::array<double, 12> m_bounds{1, 2, 5, 10, 20, 50, 100, 250, 500, 1000, 5000, 30000};
    std::array<quint64, 13> m_latency{};
};
} // namespace saim
