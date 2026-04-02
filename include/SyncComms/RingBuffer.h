#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SyncComms {

/// Lock-free single-producer single-consumer ring buffer for float samples.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 0)
        : m_readPos(0)
        , m_writePos(0)
    {
        Resize(capacity);
    }

    void Resize(size_t capacity) {
        // Round up to next power of 2
        size_t size = 1;
        while (size < capacity && size > 0) size <<= 1;
        m_buffer.resize(size, 0.0f);
        m_mask = size - 1;
        m_readPos.store(0, std::memory_order_relaxed);
        m_writePos.store(0, std::memory_order_relaxed);
    }

    size_t AvailableRead() const {
        size_t w = m_writePos.load(std::memory_order_acquire);
        size_t r = m_readPos.load(std::memory_order_relaxed);
        return w - r;
    }

    size_t AvailableWrite() const {
        return m_buffer.size() - AvailableRead();
    }

    /// Write samples into the buffer. Returns number of samples actually written.
    size_t Write(const float* data, size_t count) {
        size_t avail = AvailableWrite();
        if (count > avail) count = avail;
        if (count == 0) return 0;

        size_t pos = m_writePos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) {
            m_buffer[(pos + i) & m_mask] = data[i];
        }
        m_writePos.store(pos + count, std::memory_order_release);
        return count;
    }

    /// Read samples from the buffer. Returns number of samples actually read.
    size_t Read(float* data, size_t count) {
        size_t avail = AvailableRead();
        if (count > avail) count = avail;
        if (count == 0) return 0;

        size_t pos = m_readPos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) {
            data[i] = m_buffer[(pos + i) & m_mask];
        }
        m_readPos.store(pos + count, std::memory_order_release);
        return count;
    }

    void Clear() {
        m_readPos.store(0, std::memory_order_relaxed);
        m_writePos.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float>     m_buffer;
    size_t                 m_mask = 0;
    std::atomic<size_t>    m_readPos;
    std::atomic<size_t>    m_writePos;
};

} // namespace SyncComms
