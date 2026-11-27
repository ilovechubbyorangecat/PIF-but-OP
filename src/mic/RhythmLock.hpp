#pragma once
// Mic Clicks: rhythm lock.
//
// Every detected mic click is paired with the game input it belongs to. The difference
// between when the game registered the input and when the click audio actually showed up
// in the capture stream is the microphone/driver latency. It is not constant (drivers
// resample, devices drift), so it is tracked over a rolling window of the last 40 pairs
// with a median / trimmed mean instead of a single calibration value.

#include <array>
#include <cstddef>
#include <cstdint>

namespace micclicks {

class RhythmLock {
public:
    static constexpr size_t kWindow = 40;  // the 40-click rolling window
    static constexpr size_t kMinimum = 6;  // enough pairs for a stable estimate

    struct Sample {
        double deltaMs = 0.0;  // input registration time - click arrival time
        int64_t inputNs = 0;
        int64_t onsetNs = 0;
    };

    void push(double deltaMs, int64_t inputNs, int64_t onsetNs);
    void reset();

    size_t count() const {
        return m_count;
    }
    bool ready() const {
        return m_count >= kMinimum;
    }
    // Oldest first.
    Sample const& at(size_t i) const {
        size_t start = m_count == kWindow ? m_next : 0;
        return m_ring[(start + i) % kWindow];
    }

    double medianMs() const;
    double trimmedMeanMs() const;
    double jitterMs() const;        // median absolute deviation
    double driftMsPerMinute() const;  // least squares slope over the window
    double confidence() const;      // 0..1
    double delayMs() const;         // trimmed mean, drift compensated
    double rateScale() const;       // capture clock correction (1.0 = exact)

private:
    void gather(double* out, size_t& count) const;

    std::array<Sample, kWindow> m_ring {};
    size_t m_count = 0;
    size_t m_next = 0;
};

} // namespace micclicks
