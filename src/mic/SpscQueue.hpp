#pragma once
// Mic Clicks: lock-free single-producer / single-consumer queue.
//
// The whole feature is built out of these: the audio device thread never takes a lock,
// so a blocked game thread or a slow settings panel can never stall audio (and the
// other way round). Push is only ever called by one thread and pop by another.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace micclicks {

template <class T, size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "a queue needs at least two slots");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "queued items are copied byte for byte");

public:
    // Producer side. Returns false when the consumer is not keeping up.
    bool push(T const& item) {
        uint64_t head = m_head.load(std::memory_order_relaxed);
        uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            return false;
        }
        m_data[static_cast<size_t>(head) & (Capacity - 1)] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool pop(T& out) {
        uint64_t tail = m_tail.load(std::memory_order_relaxed);
        uint64_t head = m_head.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = m_data[static_cast<size_t>(tail) & (Capacity - 1)];
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        uint64_t head = m_head.load(std::memory_order_acquire);
        uint64_t tail = m_tail.load(std::memory_order_acquire);
        return head >= tail ? static_cast<size_t>(head - tail) : 0;
    }

    bool empty() const {
        return size() == 0;
    }

    // Consumer only.
    void clear() {
        uint64_t head = m_head.load(std::memory_order_acquire);
        m_tail.store(head, std::memory_order_release);
    }

private:
    std::array<T, Capacity> m_data {};
    alignas(64) std::atomic<uint64_t> m_head { 0 };
    alignas(64) std::atomic<uint64_t> m_tail { 0 };
};

} // namespace micclicks
