#include "Slicer.hpp"

#include <algorithm>
#include <cmath>

#include "Dsp.hpp"
#include "Envelope.hpp"

namespace micclicks {

namespace {

size_t msToFrames(float ms, int sampleRate) {
    if (ms <= 0.f || sampleRate <= 0) {
        return 0;
    }
    return static_cast<size_t>(ms * 0.001f * static_cast<float>(sampleRate) + 0.5f);
}

} // namespace

SliceShape groomClickSlice(float* samples, size_t frames, int sampleRate) {
    SliceShape shape;
    if (!samples || frames == 0 || sampleRate <= 0) {
        return shape;
    }

    // --- where is the click ------------------------------------------------------------
    float peak = 0.f;
    size_t peakIndex = 0;
    for (size_t i = 0; i < frames; ++i) {
        float magnitude = std::abs(samples[i]);
        if (magnitude > peak) {
            peak = magnitude;
            peakIndex = i;
        }
    }
    if (peak < 1e-4f) {
        return shape;  // nothing but room tone: not a click slice
    }

    // --- length: ~45 ms after the transient, never longer than what we captured ---------
    size_t tail = std::max<size_t>(msToFrames(kSliceTailMs, sampleRate), 1);
    size_t length = std::min(frames, peakIndex + tail);
    if (length <= peakIndex) {
        length = std::min(frames, peakIndex + 1);
    }

    // --- when does the click start and end? --------------------------------------------
    // Envelope follower with a fast attack and a ~20 ms release, compared with a threshold
    // 24 dB below the click. First pass: only the two edges; second pass: apply the gain.
    PeakFollower detector;
    detector.configure(sampleRate, 1.f, 20.f);
    detector.reset(0.f);
    float threshold = peak * 0.06f;

    size_t start = length;
    size_t end = 0;
    for (size_t i = 0; i < length; ++i) {
        if (detector.process(samples[i]) > threshold) {
            if (start == length) {
                start = i;
            }
            end = i;
        }
    }
    if (start == length) {
        // The follower never crossed the threshold (a very fast, very quiet click): treat the
        // peak itself as the click.
        start = peakIndex;
        end = peakIndex;
    }

    // Look ahead a little so the gate is fully open before the click's rise starts: a 6 ms
    // attack that starts *at* the onset would round off the attack of the click.
    size_t lookAhead = msToFrames(2.f, sampleRate);
    size_t openAt = start > lookAhead ? start - lookAhead : 0;
    size_t closeAt = std::min(length, end + 1);

    // --- normalise, but never amplify room noise into audibility -----------------------
    // Up to 8x (+18 dB) so a soft click lands at the same level as a hard one, and never more:
    // beyond that we would be amplifying the microphone's own hiss.
    float gain = std::clamp(0.5f / peak, 0.25f, 8.f);

    // --- smoothed gate ---------------------------------------------------------------
    // 0 .. openAt          raised-cosine 0 -> 1 (ducks the room tone *before* the click)
    // openAt .. closeAt    1 (the click, untouched)
    // closeAt .. +release  raised-cosine 1 -> 0 (30 ms, so the room tone fades instead of
    //                      being chopped off mid-waveform)
    size_t release = msToFrames(kSliceGateReleaseMs, sampleRate);
    for (size_t i = 0; i < openAt; ++i) {
        samples[i] *= gain * fadeInGain(i, std::max<size_t>(openAt, 1));
    }
    for (size_t i = openAt; i < closeAt; ++i) {
        samples[i] *= gain;
    }
    for (size_t i = closeAt; i < length; ++i) {
        size_t intoRelease = i - closeAt;
        float rampDown = intoRelease < release ? fadeOutGain(intoRelease, release) : 0.f;
        samples[i] *= gain * rampDown;
    }

    // --- fades -------------------------------------------------------------------------
    size_t fadeIn = std::min(length / 2, msToFrames(kSliceFadeInMs, sampleRate));
    for (size_t i = 0; i < fadeIn; ++i) {
        samples[i] *= fadeInGain(i, std::max<size_t>(fadeIn, 1));
    }

    // Whatever the gate did, the slice has to end on exact silence (the last sample is 0), so
    // mixing it can never produce a step - and "Clicks Only" monitoring passes no static.
    size_t fadeOut = std::min(length, msToFrames(kSliceFadeOutMs, sampleRate));
    for (size_t i = 0; i < fadeOut; ++i) {
        samples[length - 1 - i] *= fadeInGain(i, std::max<size_t>(fadeOut, 1));
    }

    shape.length = length;
    shape.peakOffset = peakIndex;
    return shape;
}

size_t gateAndNormalize(float* samples, size_t frames, int sampleRate) {
    return groomClickSlice(samples, frames, sampleRate).length;
}

} // namespace micclicks
