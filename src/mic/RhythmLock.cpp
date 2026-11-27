#include "RhythmLock.hpp"

#include <algorithm>
#include <cmath>

#include "Dsp.hpp"

namespace micclicks {

void RhythmLock::push(double deltaMs, int64_t inputNs, int64_t onsetNs) {
    m_ring[m_next] = Sample { deltaMs, inputNs, onsetNs };
    m_next = (m_next + 1) % kWindow;
    if (m_count < kWindow) {
        ++m_count;
    }
}

void RhythmLock::reset() {
    m_count = 0;
    m_next = 0;
    m_ring.fill(Sample {});
}

void RhythmLock::gather(double* out, size_t& count) const {
    count = m_count;
    for (size_t i = 0; i < m_count; ++i) {
        out[i] = at(i).deltaMs;
    }
}

double RhythmLock::medianMs() const {
    if (m_count == 0) {
        return 0.0;
    }
    std::array<double, kWindow> values {};
    size_t count = 0;
    gather(values.data(), count);
    std::sort(values.begin(), values.begin() + static_cast<long>(count));
    size_t middle = count / 2;
    if (count & 1) {
        return values[middle];
    }
    return 0.5 * (values[middle - 1] + values[middle]);
}

double RhythmLock::trimmedMeanMs() const {
    if (m_count == 0) {
        return 0.0;
    }
    std::array<double, kWindow> values {};
    size_t count = 0;
    gather(values.data(), count);
    std::sort(values.begin(), values.begin() + static_cast<long>(count));
    size_t drop = count / 10;  // 10% off each end
    if (count < 5) {
        drop = 0;
    }
    double sum = 0.0;
    size_t used = 0;
    for (size_t i = drop; i < count - drop; ++i) {
        sum += values[i];
        ++used;
    }
    return used ? sum / static_cast<double>(used) : 0.0;
}

double RhythmLock::jitterMs() const {
    if (m_count < 2) {
        return 0.0;
    }
    double centre = medianMs();
    std::array<double, kWindow> deviations {};
    for (size_t i = 0; i < m_count; ++i) {
        deviations[i] = std::abs(at(i).deltaMs - centre);
    }
    std::sort(deviations.begin(), deviations.begin() + static_cast<long>(m_count));
    size_t middle = m_count / 2;
    if (m_count & 1) {
        return deviations[middle];
    }
    return 0.5 * (deviations[middle - 1] + deviations[middle]);
}

double RhythmLock::driftMsPerMinute() const {
    if (m_count < 8) {
        return 0.0;
    }
    // Linear fit of delta against input time.
    double meanX = 0.0;
    double meanY = 0.0;
    int64_t first = at(0).inputNs;
    for (size_t i = 0; i < m_count; ++i) {
        meanX += static_cast<double>(at(i).inputNs - first);
        meanY += at(i).deltaMs;
    }
    meanX /= static_cast<double>(m_count);
    meanY /= static_cast<double>(m_count);
    double numerator = 0.0;
    double denominator = 0.0;
    for (size_t i = 0; i < m_count; ++i) {
        double x = static_cast<double>(at(i).inputNs - first) - meanX;
        double y = at(i).deltaMs - meanY;
        numerator += x * y;
        denominator += x * x;
    }
    double span = static_cast<double>(at(m_count - 1).inputNs - first);
    if (denominator < 1e-9 || span < 1e9) {
        return 0.0;  // under a second of data: no meaningful slope
    }
    double slopePerNs = numerator / denominator;
    return clampf(static_cast<float>(slopePerNs * 60.0e9), -120.f, 120.f);
}

double RhythmLock::confidence() const {
    if (m_count == 0) {
        return 0.0;
    }
    double fill = static_cast<double>(m_count) / static_cast<double>(kWindow);
    double spread = clampf(static_cast<float>(jitterMs() / 12.0), 0.f, 1.f);
    return clampf(static_cast<float>(fill * (1.0 - spread * 0.85)), 0.f, 1.f);
}

double RhythmLock::delayMs() const {
    return m_count ? trimmedMeanMs() : 0.0;
}

double RhythmLock::rateScale() const {
    // A latency slope of 1 ms/min means the capture clock is off by 1 ms per 60 s,
    // i.e. 1.7e-5. Keep the correction tiny so a few bad pairs cannot throw the mapping.
    double drift = driftMsPerMinute();
    return 1.0 + clampf(static_cast<float>(drift / 60000.0), -5e-4f, 5e-4f);
}

} // namespace micclicks
