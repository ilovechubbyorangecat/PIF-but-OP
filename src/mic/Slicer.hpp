#pragma once
// Mic Clicks: click slice grooming.
//
// A detected transient is cut out of the raw microphone ring buffer with a little pre-roll and
// a tail, then shaped here:
//
//   * normalised to a consistent peak level, so a soft click and a hard one sound alike,
//   * gated with a *smoothed* envelope (6 ms attack / 30 ms release), so the room tone around
//     the click fades away instead of being chopped off at a window boundary,
//   * cut ~45 ms after the transient peak and closed with a raised-cosine fade-out, so the
//     resonance of the click is kept but the slice always ends on silence (mixing many of them
//     can never click, and "Clicks Only" monitoring passes no room static between clicks).

#include <cstddef>

namespace micclicks {

struct SliceShape {
    size_t length = 0;      // usable frames at the front of the buffer
    size_t peakOffset = 0;  // frames from the start of the buffer to the transient peak
};

// Slices used to be cut with a hard window: `frames` is the raw capture window, `sampleRate` is
// the rate everything runs at.
inline constexpr float kSliceTailMs = 45.f;    // ~35-50 ms of click after the peak
inline constexpr float kSliceFadeOutMs = 10.f; // raised-cosine fade to silence at the end
inline constexpr float kSliceFadeInMs = 1.5f;  // kills the step at the very first sample
inline constexpr float kSliceGateAttackMs = 6.f;
inline constexpr float kSliceGateReleaseMs = 30.f;

SliceShape groomClickSlice(float* samples, size_t frames, int sampleRate);

// Thin wrapper kept for the existing call sites: returns just the usable length.
size_t gateAndNormalize(float* samples, size_t frames, int sampleRate);

} // namespace micclicks
