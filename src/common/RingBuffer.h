#pragma once

// ---------------------------------------------------------------------------
// RingBuffer.h - fixed-capacity circular buffer used for rolling sensor windows.
//
// Why not QQueue / std::deque: the AI feature extractor reads a sliding window
// of the last N samples on every frame (20 Hz x N devices).  A ring buffer keeps
// a contiguous backing store with zero allocation after construction, so the
// inference path never touches the allocator - which matters on the worker
// thread where a malloc stall shows up directly as dropped frames.
//
// Not thread safe by design: each DeviceWorker owns its own buffer, and the
// window snapshot handed to the AI layer is a copy taken on that one thread.
// ---------------------------------------------------------------------------

#include <QVector>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace saim {

template <typename T>
class RingBuffer
{
public:
    explicit RingBuffer(std::size_t capacity)
        : m_data(capacity)
    {
        m_data.reserve(0);   // capacity already fixed; reserve is a no-op guard
    }

    void push(const T& value)
    {
        if (m_data.empty())
        {
            return;   // capacity 0 - degenerate but well defined
        }
        if (m_size < m_data.size())
        {
            m_data[m_size] = value;
            ++m_size;
        }
        else
        {
            m_data[m_head] = value;
            m_head = (m_head + 1) % m_data.size();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_data.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] bool full() const noexcept { return m_size == m_data.size(); }

    /// Oldest element.  Precondition: !empty().
    [[nodiscard]] const T& oldest() const { return m_data[m_head]; }

    /// Newest element.  Precondition: !empty().
    [[nodiscard]] const T& newest() const
    {
        const std::size_t idx = (m_head + m_size - 1) % m_data.size();
        return m_data[idx];
    }

    /// Index 0 = oldest, size()-1 = newest.
    [[nodiscard]] const T& at(std::size_t chronologicalIndex) const
    {
        return m_data[(m_head + chronologicalIndex) % m_data.size()];
    }

    /// Copy out the window in chronological order - what the model consumes.
    [[nodiscard]] std::vector<T> toVector() const
    {
        std::vector<T> out;
        out.reserve(m_size);
        for (std::size_t i = 0; i < m_size; ++i)
        {
            out.push_back(at(i));
        }
        return out;
    }

    void clear() noexcept
    {
        m_head = 0;
        m_size = 0;
    }

private:
    std::vector<T> m_data;
    std::size_t    m_head = 0;   ///< index of the oldest element
    std::size_t    m_size = 0;   ///< number of valid elements
};

}   // namespace saim
