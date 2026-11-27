#pragma once
// Mic Clicks: the capture -> playback jitter buffer.
//
// The microphone and the click output (headphones, or a virtual cable for OBS/Discord) are
// two separate pieces of hardware with their own clocks. The capture callback produces
// samples at (nominally) 48000 Hz, the render callback consumes them at its own 48000 Hz,
// and the parts-per-million difference accumulates: after a minute the render callback has
// either starved (a click, a gap) or queued up a second of delay.
//
// So the smooth path between them is not a plain ring buffer but a jitter buffer:
//
//   * a pre-roll (10-15 ms) is queued before the first real read, and that slack is what
//     absorbs the two clocks wandering against each other,
//   * an underrun never replays old audio: the missing frames are silence, and the buffer
//     re-primes so the next block starts with a full pre-roll again,
//   * when the queue grows past a maximum lag, the oldest frames are trimmed a few at a
//     time until the latency is back where it started (monitoring stays in sync forever).
//
// Lock-free single producer (the DSP worker) / single consumer (the audio callback); no
// allocation, no locks, no syscalls on either side.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace micclicks {

class JitterBuffer {
public:
    // capacityFrames: total size, rounded up to a power of two.
    // preRollFrames: how much audio has to be queued before reads return anything.
    // maxLagFrames: above this the oldest frames are trimmed, so latency can't creep up.
    void configure(size_t capacityFrames, size_t preRollFrames, size_t maxLagFrames) {
        size_t capacity = 64;
        while (capacity < capacityFrames) {
            capacity <<= 1;
        }
        m_storage.assign(capacity, 0.f);
        m_capacity = capacity;
        m_mask = capacity - 1;
        m_preRoll = std::min(preRollFrames, capacity / 2);
        m_maxLag = std::max(maxLagFrames, m_preRoll + 1);
        reset();
    }

    bool configured() const {
        return m_capacity > 0;
    }

    // --- producer (DSP worker) ---------------------------------------------------------
    // Returns how many frames were accepted. A short write means the output device stopped
    // draining; the newest audio is dropped rather than overwriting what is still queued.
    size_t write(float const* frames, size_t count) {
        if (!frames || count == 0 || m_capacity == 0) {
            return 0;
        }
        uint64_t write = m_write.load(std::memory_order_relaxed);
        uint64_t read = m_read.load(std::memory_order_acquire);
        size_t fill = static_cast<size_t>(write - read);
        size_t room = m_capacity - fill;
        size_t accepted = std::min(count, room);
        copyIn(write, frames, accepted);
        m_write.store(write + accepted, std::memory_order_release);
        if (accepted < count) {
            m_overflows.fetch_add(1, std::memory_order_relaxed);
        }
        return accepted;
    }

    // Drops everything queued and re-primes. Called from the producer side only.
    void dropQueued() {
        uint64_t read = m_read.load(std::memory_order_acquire);
        m_write.store(read, std::memory_order_release);
        m_priming.store(true, std::memory_order_release);
    }

    // --- consumer (audio callback) -----------------------------------------------------
    // Always fills exactly `count` frames: what is missing is silence, never stale audio.
    // Returns how many frames came from the microphone.
    size_t read(float* out, size_t count) {
        if (!out || count == 0 || m_capacity == 0) {
            return 0;
        }
        uint64_t read = m_read.load(std::memory_order_relaxed);
        uint64_t write = m_write.load(std::memory_order_acquire);
        size_t fill = static_cast<size_t>(write - read);

        if (m_priming.load(std::memory_order_relaxed)) {
            if (fill < m_preRoll) {
                // Still building the pre-roll: stay silent instead of draining a buffer
                // that is about to run dry again.
                std::memset(out, 0, count * sizeof(float));
                return 0;
            }
            m_priming.store(false, std::memory_order_relaxed);
        }

        size_t taken = std::min(count, fill);
        copyOut(read, out, taken);
        if (taken < count) {
            std::memset(out + taken, 0, (count - taken) * sizeof(float));
            m_underruns.fetch_add(1, std::memory_order_relaxed);
            m_priming.store(true, std::memory_order_release);
        }

        uint64_t next = read + taken;
        if (taken == count) {
            // Trim the oldest frames, a small slice per callback, when the queue is longer
            // than the maximum lag. Skipping a few frames is inaudible; a growing delay is
            // not.
            size_t left = static_cast<size_t>(write - next);
            if (left > m_maxLag) {
                size_t excess = left - m_maxLag;
                size_t trim = std::min(excess, std::max<size_t>(1, count / 64));
                next += trim;
                m_trims.fetch_add(static_cast<uint32_t>(trim), std::memory_order_relaxed);
            }
        }
        m_read.store(next, std::memory_order_release);
        return taken;
    }

    // Consumer side, only while the stream is stopped.
    void reset() {
        m_read.store(0, std::memory_order_relaxed);
        m_write.store(0, std::memory_order_relaxed);
        m_priming.store(true, std::memory_order_relaxed);
        m_underruns.store(0, std::memory_order_relaxed);
        m_overflows.store(0, std::memory_order_relaxed);
        m_trims.store(0, std::memory_order_relaxed);
    }

    size_t fillFrames() const {
        uint64_t write = m_write.load(std::memory_order_acquire);
        uint64_t read = m_read.load(std::memory_order_acquire);
        return write > read ? static_cast<size_t>(write - read) : 0;
    }
    size_t preRollFrames() const {
        return m_preRoll;
    }
    size_t maxLagFrames() const {
        return m_maxLag;
    }
    size_t capacityFrames() const {
        return m_capacity;
    }
    uint32_t underruns() const {
        return m_underruns.load(std::memory_order_relaxed);
    }
    uint32_t overflows() const {
        return m_overflows.load(std::memory_order_relaxed);
    }
    uint32_t trims() const {
        return m_trims.load(std::memory_order_relaxed);
    }

private:
    void copyIn(uint64_t write, float const* src, size_t count) {
        size_t start = static_cast<size_t>(write) & m_mask;
        size_t first = std::min(count, m_capacity - start);
        std::memcpy(m_storage.data() + start, src, first * sizeof(float));
        if (count > first) {
            std::memcpy(m_storage.data(), src + first, (count - first) * sizeof(float));
        }
    }

    void copyOut(uint64_t read, float* dst, size_t count) {
        size_t start = static_cast<size_t>(read) & m_mask;
        size_t first = std::min(count, m_capacity - start);
        std::memcpy(dst, m_storage.data() + start, first * sizeof(float));
        if (count > first) {
            std::memcpy(dst + first, m_storage.data(), (count - first) * sizeof(float));
        }
    }

    std::vector<float> m_storage;
    size_t m_capacity = 0;
    size_t m_mask = 0;
    size_t m_preRoll = 0;
    size_t m_maxLag = 0;

    alignas(64) std::atomic<uint64_t> m_write { 0 };
    alignas(64) std::atomic<uint64_t> m_read { 0 };
    std::atomic<bool> m_priming { true };
    std::atomic<uint32_t> m_underruns { 0 };
    std::atomic<uint32_t> m_overflows { 0 };
    std::atomic<uint32_t> m_trims { 0 };
};

} // namespace micclicks
