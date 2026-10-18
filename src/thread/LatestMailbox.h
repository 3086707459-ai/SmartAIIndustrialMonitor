#pragma once
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QVector>
#include <algorithm>
#include <deque>

namespace saim {
// Bounded latest-value channel for result/image delivery. No per-result queued
// event; replacement is intentional and tracked independently from overflow.
template<class T> class LatestMailbox final
{
public:
    explicit LatestMailbox(int capacity) : m_capacity(std::max(1, capacity)) {}
    void put(const QString& key, const T& value)
    {
        QMutexLocker lock(&m_mutex);
        if (m_values.contains(key)) { ++m_replaced; m_values[key] = value; return; }
        if (m_values.size() == m_capacity)
        { m_values.remove(m_order.front()); m_order.pop_front(); ++m_evicted; }
        m_values.insert(key, value);
        m_order.push_back(key);
    }
    QVector<T> take(int limit)
    {
        QMutexLocker lock(&m_mutex);
        QVector<T> result;
        while (!m_order.empty() && result.size() < limit)
        { result.append(m_values.take(m_order.front())); m_order.pop_front(); }
        return result;
    }
    void clear() { QMutexLocker lock(&m_mutex); m_values.clear(); m_order.clear(); }
    quint64 evicted() const { QMutexLocker lock(&m_mutex); return m_evicted; }
    quint64 replaced() const { QMutexLocker lock(&m_mutex); return m_replaced; }
private:
    const int m_capacity;
    mutable QMutex m_mutex;
    QHash<QString, T> m_values;
    std::deque<QString> m_order;
    quint64 m_evicted = 0, m_replaced = 0;
};
} // namespace saim
