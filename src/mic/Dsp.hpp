#pragma once
// Mic Clicks: shared DSP constants and small helpers.
//
// Everything in this folder is plain C++ with no Geode/cocos dependency, so the whole
// analysis chain can be built and unit tested outside the game (see tests/mic_dsp_test.cpp).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace micclicks {

inline constexpr int kBandCount = 24;      // log spaced bands used for the click fingerprint
inline constexpr int kFftSize = 256;       // 5.3 ms at 48 kHz
inline constexpr int kSampleRate = 48000;  // capture and render rate (the backend converts)
inline constexpr int kHopFrames = 64;      // 1.3 ms analysis hop
inline constexpr int kMaxSliceFrames = 4096;  // 85 ms of click audio (12 ms pre-roll + 45 ms tail)

inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline float energyToDb(float energy) {
    return 10.f * std::log10(std::max(energy, 1e-12f));
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// `values` is reordered in place on purpose: these are small (<= 40) and only run on the
// DSP thread, where allocating on every click would be worse than copying.
inline float medianInPlace(float* values, int count) {
    if (count <= 0) {
        return 0.f;
    }
    std::sort(values, values + count);
    if (count & 1) {
        return values[count / 2];
    }
    return 0.5f * (values[count / 2 - 1] + values[count / 2]);
}

inline float trimmedMeanInPlace(float* values, int count, float trimFraction) {
    if (count <= 0) {
        return 0.f;
    }
    std::sort(values, values + count);
    int drop = static_cast<int>(count * clampf(trimFraction, 0.f, 0.45f));
    int first = drop;
    int last = count - drop;
    if (last <= first) {
        first = 0;
        last = count;
    }
    double sum = 0.0;
    for (int i = first; i < last; ++i) {
        sum += values[i];
    }
    return static_cast<float>(sum / (last - first));
}

} // namespace micclicks
