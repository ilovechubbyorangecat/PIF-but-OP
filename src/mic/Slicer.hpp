#pragma once
// Mic Clicks: click slice grooming.

#include <cstddef>

namespace micclicks {

// Normalises a detected click slice to a fixed peak level, gates whatever is left of the
// room tone after the click, and applies short fades so slicing cannot introduce a pop.
// Returns the usable length (0 when the slice turned out to be silent).
size_t gateAndNormalize(float* samples, size_t frames, int sampleRate);

} // namespace micclicks
