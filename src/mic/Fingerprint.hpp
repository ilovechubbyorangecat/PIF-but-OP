#pragma once
// Mic Clicks: spectral analysis + the acoustic click fingerprint.
//
// A click is not just "loud": a mouse or keyboard switch has a very short broadband
// attack, a fast decay and a specific band profile. That profile (plus the learned noise
// floor) is what lets the detector tell a click from a voice, music or a fan.

#include <array>
#include <vector>

#include "Dsp.hpp"
#include "Fft.hpp"

namespace micclicks {

struct BandFeatures {
    std::array<float, kBandCount> bandDb {};
    std::array<float, kBandCount> shape {};  // mean removed, unit length
    float broadDb = -120.f;
    float fluxDb = 0.f;
    float centroidHz = 0.f;
};

// Turns raw samples into band energies and keeps the running noise floor.
class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(int sampleRate = kSampleRate);

    void setFloor(std::array<float, kBandCount> const& floorDb, float floorBroadDb);
    void setStartupFloor(float broadDb);
    std::array<float, kBandCount> const& floorDb() const {
        return m_floorDb;
    }
    float floorBroadDb() const {
        return m_floorBroadDb;
    }

    void analyze(float const* samples, BandFeatures& out);
    // Minimum statistics over roughly 2.5 s: the quietest recent moment per band is the
    // noise floor, so a click, a voice or music cannot lift it and hide the next click.
    void trackFloor(BandFeatures const& features);

    float bandEnergyDb(int band) const {
        return m_lastBandDb[band];
    }

private:
    Fft m_fft;
    std::array<int, kBandCount + 1> m_edges {};
    std::array<float, kBandCount> m_floorDb {};
    std::array<float, kBandCount> m_lastBandDb {};
    std::array<float, kBandCount> m_prevBandDb {};
    float m_floorBroadDb = -85.f;
    bool m_havePrev = false;

    // One block = the mean level over 0.32 s. The floor is the quietest of the last eight
    // blocks (~2.5 s), which is a low percentile rather than the absolute minimum: a
    // single quiet noise dip can then not make every band look like a click.
    static constexpr int kBlockFrames = 24;  // 0.32 s per block at 48 kHz / 64 hop
    static constexpr int kBlocks = 8;        // ~2.5 s of history
    std::array<float, kBandCount> m_blockSum {};
    float m_blockSumBroad = 0.f;
    int m_blockFill = 0;
    std::array<std::array<float, kBandCount>, kBlocks> m_blocks {};
    std::array<float, kBlocks> m_blockBroad {};
    int m_blockNext = 0;
    bool m_blocksReady = false;
};

float shapeSimilarity(std::array<float, kBandCount> const& a, std::array<float, kBandCount> const& b);

// Share of the energy above ~2 kHz. A mouse switch is bright and broadband; a voice, a
// keyboard thud or a desk knock is mostly low frequency.
inline constexpr int kBrightBand = kBandCount * 2 / 3;
float bandBrightness(std::array<float, kBandCount> const& bandDb);

// One candidate click measured during calibration.
struct ClickCandidate {
    BandFeatures features;
    float peakDb = -120.f;
    float attackDb = 0.f;
    float decayMs = 0.f;
};

struct ClickFingerprint {
    bool valid = false;
    int sampleRate = kSampleRate;
    std::array<float, kBandCount> shape {};   // unit length, mean removed
    std::array<float, kBandCount> floorDb {};
    float floorBroadDb = -85.f;
    float peakDb = -20.f;
    float attackDb = 12.f;
    float decayMs = 18.f;
    float centroidHz = 3000.f;
    float brightness = 0.f;
    float fluxDb = 4.f;
    float consistency = 0.f;
    int clicks = 0;
    // Shape gate derived from how consistent the calibration clicks were.
    float shapeThreshold() const;
};

// Combines the candidates into a template (component wise median, so one bad sample
// does not ruin it) and scores how consistent they were.
ClickFingerprint buildFingerprint(std::vector<ClickCandidate> const& candidates, std::array<float, kBandCount> const& floorDb,
    float floorBroadDb, int sampleRate);

} // namespace micclicks
