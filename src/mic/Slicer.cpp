#include "Slicer.hpp"

#include <algorithm>
#include <cmath>

namespace micclicks {

size_t gateAndNormalize(float* samples, size_t frames, int sampleRate) {
    if (!samples || frames == 0) {
        return 0;
    }
    float peak = 0.f;
    for (size_t i = 0; i < frames; ++i) {
        peak = std::max(peak, std::abs(samples[i]));
    }
    if (peak < 1e-4f) {
        return 0;
    }

    size_t window = static_cast<size_t>(sampleRate / 200);  // 5 ms
    if (window == 0) {
        window = 1;
    }
    float threshold = peak * 0.06f;  // -24 dB below the click
    size_t lastLoud = 0;
    for (size_t start = 0; start < frames; start += window) {
        size_t end = std::min(frames, start + window);
        float localPeak = 0.f;
        for (size_t i = start; i < end; ++i) {
            localPeak = std::max(localPeak, std::abs(samples[i]));
        }
        if (localPeak > threshold) {
            lastLoud = end;
        }
    }

    size_t fadeOut = std::max<size_t>(1, static_cast<size_t>(sampleRate * 0.004f));
    size_t length = std::min(frames, lastLoud + fadeOut);
    if (length == 0) {
        return 0;
    }

    float gain = std::min(3.f, 0.5f / peak);
    for (size_t i = 0; i < length; ++i) {
        samples[i] *= gain;
    }

    size_t fadeIn = std::min<size_t>(length / 2, std::max<size_t>(1, static_cast<size_t>(sampleRate * 0.0015f)));
    for (size_t i = 0; i < fadeIn; ++i) {
        samples[i] *= static_cast<float>(i) / static_cast<float>(fadeIn);
    }
    size_t tail = std::min(length, fadeOut);
    for (size_t i = 0; i < tail; ++i) {
        samples[length - 1 - i] *= static_cast<float>(i) / static_cast<float>(tail);
    }
    return length;
}

} // namespace micclicks
