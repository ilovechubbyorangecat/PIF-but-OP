#pragma once
// Mic Clicks: the click voice mixer.
//
// Every click is a short slice (~12 ms of pre-roll + ~45 ms of click). Fast tapping makes those
// slices overlap, and the old render callback kept only six of them, cut a voice off whenever a
// seventh arrived and mixed the rest raw - so overlapping clicks clipped and the last tap of a
// burst could disappear.
//
// This mixer owns eight fixed voices (no allocation, no locks: it lives inside the engine and is
// driven from the audio callback only). When all eight are busy the *oldest* voice is stolen,
// which by definition is the one closest to the end of its slice. The summed block then goes
// through a tanh soft-knee limiter, so eight overlapping clicks stay below full scale instead of
// clipping.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Dsp.hpp"

namespace micclicks {

// One playing click. The slice is copied in when it starts, so nothing on the audio thread ever
// touches the ring buffer, the DSP worker or the heap.
struct ClickVoice {
    std::array<float, kMaxSliceFrames> samples {};
    uint64_t startFrame = 0;  // render frame the first sample belongs to
    uint32_t length = 0;      // frames in `samples`
    float gain = 1.f;
    uint32_t serial = 0;  // arrival order, used to find the oldest voice
    bool active = false;
};

class ClickMixer {
public:
    static constexpr size_t kMaxVoices = 8;
    // Below this the limiter is a no-op, so single clicks keep their exact level.
    static constexpr float kLimitKnee = 0.7f;

    void reset() {
        for (auto& voice : m_voices) {
            voice.active = false;
            voice.length = 0;
            voice.serial = 0;
        }
        m_serial = 0;
        m_stolen = 0;
        m_started = 0;
    }

    // Starts a click. When all eight voices are busy the oldest is reused.
    void start(float const* samples, uint32_t length, uint64_t startFrame, float gain) {
        if (!samples || length == 0) {
            return;
        }
        length = std::min<uint32_t>(length, static_cast<uint32_t>(kMaxSliceFrames));

        ClickVoice* voice = nullptr;
        for (auto& candidate : m_voices) {
            if (!candidate.active) {
                voice = &candidate;
                break;
            }
        }
        if (!voice) {
            voice = &m_voices[0];
            for (auto& candidate : m_voices) {
                if (candidate.serial < voice->serial) {
                    voice = &candidate;  // oldest
                }
            }
            ++m_stolen;
        }

        voice->startFrame = startFrame;
        voice->length = length;
        voice->gain = gain;
        voice->serial = ++m_serial;
        voice->active = true;
        std::memcpy(voice->samples.data(), samples, static_cast<size_t>(length) * sizeof(float));
        ++m_started;
    }

    // Adds every active voice into an interleaved stereo block (left = right = the mono click).
    // Returns how many voices contributed. Voices that finished are released here.
    size_t mix(float* stereo, size_t frames, uint64_t blockStart) {
        if (!stereo || frames == 0) {
            return 0;
        }
        size_t played = 0;
        for (auto& voice : m_voices) {
            if (!voice.active) {
                continue;
            }
            int64_t relative = static_cast<int64_t>(voice.startFrame) - static_cast<int64_t>(blockStart);
            int64_t end = relative + static_cast<int64_t>(voice.length);
            if (end <= 0) {
                voice.active = false;  // finished before this block started
                voice.length = 0;
                continue;
            }
            ++played;
            size_t from = relative < 0 ? static_cast<size_t>(-relative) : 0;
            for (size_t j = from; j < voice.length; ++j) {
                int64_t at = relative + static_cast<int64_t>(j);
                if (at >= static_cast<int64_t>(frames)) {
                    break;
                }
                if (at < 0) {
                    continue;
                }
                float value = voice.samples[j] * voice.gain;
                stereo[static_cast<size_t>(at) * 2] += value;
                stereo[static_cast<size_t>(at) * 2 + 1] += value;
            }
            if (end <= static_cast<int64_t>(frames)) {
                voice.active = false;  // this block was its last one
                voice.length = 0;
            }
        }
        return played;
    }

    // tanh soft-knee limiter for an interleaved stereo block: transparent below `kLimitKnee`,
    // asymptotic to full scale above it, so overlapping clicks compress instead of clipping.
    static void softLimit(float* stereo, size_t frames) {
        if (!stereo) {
            return;
        }
        size_t samples = frames * 2;
        for (size_t i = 0; i < samples; ++i) {
            float value = stereo[i];
            float magnitude = std::abs(value);
            if (magnitude <= kLimitKnee) {
                continue;
            }
            float over = (magnitude - kLimitKnee) / (1.f - kLimitKnee);
            float limited = kLimitKnee + (1.f - kLimitKnee) * std::tanh(over);
            stereo[i] = std::copysign(limited, value);
        }
    }

    size_t activeVoices() const {
        size_t active = 0;
        for (auto const& voice : m_voices) {
            if (voice.active) {
                ++active;
            }
        }
        return active;
    }
    uint32_t stolenVoices() const {
        return m_stolen;
    }
    uint32_t startedVoices() const {
        return m_started;
    }

private:
    std::array<ClickVoice, kMaxVoices> m_voices {};
    uint32_t m_serial = 0;
    uint32_t m_stolen = 0;
    uint32_t m_started = 0;
};

} // namespace micclicks
