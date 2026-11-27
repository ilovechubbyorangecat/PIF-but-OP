#pragma once
// Mic Clicks: smoothed gain envelopes.
//
// A noise gate that switches its gain in a single sample is audible as a "chop": the room
// tone - or the tail of the click itself - gets cut off mid-waveform, which is exactly the
// stutter/chuff that shows up between quick taps and in the "Everything" monitor.
//
// So every gate in this feature moves its gain along an attack/release ramp instead:
// 5-10 ms to open (fast enough to keep the click's attack intact) and 25-40 ms to close
// (slow enough that the noise between clicks fades away instead of being chopped off).
// Everything here is allocation free and per-sample, so it is safe inside an audio callback.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace micclicks {

// Exponential attack/release ramp for a 0..1 gain.
class GateEnvelope {
public:
    void configure(int sampleRate, float attackMs, float releaseMs) {
        m_attack = coefficient(sampleRate, attackMs);
        m_release = coefficient(sampleRate, releaseMs);
    }

    void reset(float value = 0.f) {
        m_gain = clamp01(value);
    }
    float value() const {
        return m_gain;
    }

    // One sample step toward open (1) or closed (0). Returns the gain for this sample.
    float next(bool open) {
        float target = open ? 1.f : 0.f;
        float coefficient = target > m_gain ? m_attack : m_release;
        m_gain += (target - m_gain) * coefficient;
        if (m_gain < 1e-5f) {
            m_gain = 0.f;  // also keeps denormals out of the mix
        }
        else if (m_gain > 0.99999f) {
            m_gain = 1.f;
        }
        return m_gain;
    }

    // Multiplies a mono block by the ramp.
    void apply(float* samples, size_t frames, bool open) {
        for (size_t i = 0; i < frames; ++i) {
            samples[i] *= next(open);
        }
    }

    // One step of a linear time constant: 63 % of the way after `ms` milliseconds.
    static float coefficient(int sampleRate, float ms) {
        if (sampleRate <= 0 || ms <= 0.f) {
            return 1.f;
        }
        float samples = std::max(1.f, static_cast<float>(sampleRate) * ms * 0.001f);
        return 1.f - std::exp(-1.f / samples);
    }

private:
    static float clamp01(float v) {
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    float m_attack = 1.f;
    float m_release = 1.f;
    float m_gain = 0.f;
};

// Envelope follower used to *decide* whether a sample belongs to the click: instant on the way
// up (so the attack is never missed) and slow on the way down.
class PeakFollower {
public:
    void configure(int sampleRate, float attackMs, float releaseMs) {
        m_attack = GateEnvelope::coefficient(sampleRate, attackMs);
        m_release = GateEnvelope::coefficient(sampleRate, releaseMs);
    }

    void reset(float value = 0.f) {
        m_value = value;
    }
    float value() const {
        return m_value;
    }

    float process(float sample) {
        float rectified = std::abs(sample);
        float coefficient = rectified > m_value ? m_attack : m_release;
        m_value += (rectified - m_value) * coefficient;
        return m_value;
    }

private:
    float m_attack = 1.f;
    float m_release = 0.01f;
    float m_value = 0.f;
};

// Raised-cosine fade helpers: `fadeIn` goes 0 -> 1, `fadeOut` goes 1 -> 0, both smoother than a
// straight line (no slope discontinuity at either end).
inline float fadeInGain(size_t index, size_t length) {
    if (length == 0 || index >= length) {
        return 1.f;
    }
    float t = static_cast<float>(index) / static_cast<float>(length);
    return 0.5f - 0.5f * std::cos(t * 3.14159265358979323846f);
}

inline float fadeOutGain(size_t index, size_t length) {
    return 1.f - fadeInGain(index, length);
}

} // namespace micclicks
