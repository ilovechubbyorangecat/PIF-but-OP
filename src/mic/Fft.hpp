#pragma once
// Mic Clicks: small radix-2 FFT (256 points) used for the spectral click fingerprint.

#include <array>
#include <complex>

#include "Dsp.hpp"

namespace micclicks {

class Fft {
public:
    Fft();

    // `samples` must hold kFftSize raw samples; the Hann window is applied here.
    // `power` receives |X_k|^2 for the first kFftSize/2 bins.
    void powerSpectrum(float const* samples, std::array<float, kFftSize / 2>& power) const;

    float hann(size_t i) const {
        return m_hann[i];
    }

    static constexpr float windowEnergy() {
        // sum(hann^2) / N for a periodic Hann window
        return 0.375f;
    }

private:
    void transform(std::complex<float>* data) const;

    std::array<float, kFftSize> m_hann {};
    std::array<std::complex<float>, kFftSize / 2> m_twiddle {};
    std::array<unsigned short, kFftSize> m_bitReverse {};
};

} // namespace micclicks
