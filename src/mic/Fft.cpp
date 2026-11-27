#include "Fft.hpp"

#include <cmath>

namespace micclicks {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

Fft::Fft() {
    for (int i = 0; i < kFftSize; ++i) {
        m_hann[i] = 0.5f - 0.5f * std::cos(2.f * kPi * static_cast<float>(i) / static_cast<float>(kFftSize));
    }
    for (int i = 0; i < kFftSize / 2; ++i) {
        float angle = -2.f * kPi * static_cast<float>(i) / static_cast<float>(kFftSize);
        m_twiddle[i] = std::complex<float>(std::cos(angle), std::sin(angle));
    }
    int bits = 0;
    while ((1 << bits) < kFftSize) {
        ++bits;
    }
    for (int i = 0; i < kFftSize; ++i) {
        unsigned short rev = 0;
        for (int b = 0; b < bits; ++b) {
            if (i & (1 << b)) {
                rev |= static_cast<unsigned short>(1 << (bits - 1 - b));
            }
        }
        m_bitReverse[i] = rev;
    }
}

void Fft::transform(std::complex<float>* data) const {
    std::array<std::complex<float>, kFftSize> tmp {};
    for (int i = 0; i < kFftSize; ++i) {
        tmp[m_bitReverse[i]] = data[i];
    }
    for (int len = 2; len <= kFftSize; len <<= 1) {
        int half = len >> 1;
        int step = kFftSize / len;
        for (int start = 0; start < kFftSize; start += len) {
            for (int j = 0; j < half; ++j) {
                std::complex<float> w = m_twiddle[j * step];
                std::complex<float> u = tmp[start + j];
                std::complex<float> v = tmp[start + j + half] * w;
                tmp[start + j] = u + v;
                tmp[start + j + half] = u - v;
            }
        }
    }
    for (int i = 0; i < kFftSize; ++i) {
        data[i] = tmp[i];
    }
}

void Fft::powerSpectrum(float const* samples, std::array<float, kFftSize / 2>& power) const {
    std::array<std::complex<float>, kFftSize> buf {};
    for (int i = 0; i < kFftSize; ++i) {
        buf[i] = std::complex<float>(samples[i] * m_hann[i], 0.f);
    }
    transform(buf.data());
    for (int k = 0; k < kFftSize / 2; ++k) {
        power[k] = std::norm(buf[k]);
    }
}

} // namespace micclicks
