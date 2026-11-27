#pragma once
// Mic Clicks: the rolling raw-microphone ring buffer.
//
// The capture callback is the only producer, the DSP worker the only consumer. It keeps
// the last N seconds of mic audio so a click that is only recognised a few milliseconds
// after it started can still be sliced with its full attack (pre-roll) and tail.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace micclicks {

class AudioRing {
public:
    explicit AudioRing(size_t frames = 0) {
        resize(frames);
    }

    void resize(size_t frames) {
        size_t cap = 1;
        while (cap < frames) {
            cap <<= 1;
        }
        m_data.assign(cap, 0.f);
        m_mask = cap - 1;
        m_written.store(0, std::memory_order_relaxed);
        m_read = 0;
    }

    size_t capacity() const {
        return m_mask + 1;
    }

    // Producer. Old samples are overwritten if the consumer fell behind.
    void write(float const* src, size_t frames) {
        uint64_t w = m_written.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) {
            m_data[static_cast<size_t>(w + i) & m_mask] = src[i];
        }
        m_written.store(w + frames, std::memory_order_release);
    }

    // Consumer.
    uint64_t written() const {
        return m_written.load(std::memory_order_acquire);
    }

    size_t available() const {
        uint64_t w = written();
        uint64_t pending = w - m_read;
        return static_cast<size_t>(pending > capacity() ? capacity() : pending);
    }

    size_t read(float* dst, size_t frames) {
        size_t n = available();
        if (n > frames) {
            n = frames;
        }
        for (size_t i = 0; i < n; ++i) {
            dst[i] = m_data[static_cast<size_t>(m_read + i) & m_mask];
        }
        m_read += n;
        return n;
    }

    // Consumer: peek without advancing (used for the analysis window).
    size_t peek(float* dst, size_t frames) const {
        uint64_t w = written();
        uint64_t pending = w - m_read;
        size_t n = static_cast<size_t>(pending < frames ? pending : frames);
        for (size_t i = 0; i < n; ++i) {
            dst[i] = m_data[static_cast<size_t>(m_read + i) & m_mask];
        }
        return n;
    }

    // Consumer: read absolute frames (history included). Fails when the requested span
    // has already been overwritten.
    bool readHistory(uint64_t firstFrame, float* dst, size_t frames) const {
        uint64_t w = written();
        if (firstFrame + frames > w) {
            return false;
        }
        if (frames > capacity() || w - firstFrame > capacity()) {
            return false;
        }
        for (size_t i = 0; i < frames; ++i) {
            dst[i] = m_data[static_cast<size_t>(firstFrame + i) & m_mask];
        }
        return true;
    }

    // Consumer: drop whatever is pending (used when a stream is (re)started, so the
    // analysis does not run over audio from before the restart).
    void skipToLatest() {
        m_read = written();
    }

    uint64_t readPosition() const {
        return m_read;
    }

private:
    std::vector<float> m_data;
    size_t m_mask = 0;
    alignas(64) std::atomic<uint64_t> m_written { 0 };
    alignas(64) uint64_t m_read = 0;  // consumer only
};

} // namespace micclicks
