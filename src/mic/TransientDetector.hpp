#pragma once
// Mic Clicks: transient (click) detection.
//
// Runs on the DSP worker only. Each hop it looks at the newest 256 samples, and accepts a
// detection only when all of these agree:
//   * the broadband level jumped above the learned noise floor (amplitude gate),
//   * the level is still rising (attack slope),
//   * a lot of bands changed at once (spectral flux),
//   * the band profile matches the calibrated click fingerprint,
//   * the sound did not stay loud for long (a voice or a fan is not a click).
// A plain amplitude threshold is the fallback before calibration and the baseline the
// statistics compare against.

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "Dsp.hpp"
#include "Fingerprint.hpp"

namespace micclicks {

struct DetectorConfig {
    float gateDb = 9.f;          // above the noise floor
    float fluxMinDb = 2.f;       // mean positive band change
    float riseMinDb = 3.f;       // how much louder than the recent minimum
    float refractoryMs = 35.f;   // one click per this window unless much louder
    int warmupHops = 150;        // learn the room before firing on it
    int voiceRunFrames = 42;     // hops above the gate before it is treated as voice/hum
    int voiceHoldFrames = 80;    // hops of suppression afterwards
};

struct DetectorStats {
    uint32_t candidates = 0;
    uint32_t accepted = 0;
    uint32_t rejectedGate = 0;
    uint32_t rejectedShape = 0;
    uint32_t rejectedRefractory = 0;
    uint32_t rejectedSustained = 0;
    uint32_t naiveMissed = 0;  // accepted, but below what a plain amplitude gate would need
};

struct Detection {
    uint64_t onsetFrame = 0;   // estimated start of the attack, in absolute captured frames
    uint64_t windowFrame = 0;  // centre of the analysis window that triggered
    float peakDb = -120.f;
    float snrDb = 0.f;
    float shapeScore = 0.f;
    float fluxDb = 0.f;
    float centroidHz = 0.f;
    float attackDb = 0.f;
    bool naiveMiss = false;
    BandFeatures features;
};

class TransientDetector {
public:
    TransientDetector();

    void configure(int sampleRate, int hopFrames);
    void setFingerprint(ClickFingerprint const& fingerprint);
    void setGateScale(float scale);  // mic sensitivity: > 1 lowers the gate
    void setCalibrationMode(bool on);
    void setNoiseFloor(std::array<float, kBandCount> const& floorDb, float floorBroadDb);
    void reset();

    // `samples` must be `frames` samples starting at absolute frame `startFrame`.
    // Frames are buffered and analysed as whole hops; detections append to `out`.
    void push(float const* samples, size_t frames, uint64_t startFrame, std::vector<Detection>& out);

    // Quiet room measurement (calibration phase 1): learns the floor without detecting.
    void measureAmbient(float const* samples, size_t frames);

    ClickFingerprint const& fingerprint() const {
        return m_fingerprint;
    }
    DetectorStats const& stats() const {
        return m_stats;
    }
    float floorBroadDb() const {
        return m_analyzer.floorBroadDb();
    }
    std::array<float, kBandCount> const& floorDb() const {
        return m_analyzer.floorDb();
    }
    int sampleRate() const {
        return m_sampleRate;
    }
    int hopFrames() const {
        return m_hop;
    }
    float lastBroadDb() const {
        return m_lastBroadDb;
    }
    float lastPeakBandDb() const {
        return m_lastPeakBandDb;
    }

private:
    struct FrameRecord {
        uint64_t endFrame = 0;
        float broadDb = -120.f;
        float fluxDb = 0.f;
        float centroidHz = 0.f;
        BandFeatures features;
    };

    void analyseHop(float const* hop, uint64_t endFrame, std::vector<Detection>& out);
    uint64_t estimateOnset(uint64_t windowCenter) const;

    Fft m_fft;
    SpectrumAnalyzer m_analyzer;
    ClickFingerprint m_fingerprint;
    DetectorConfig m_config;
    DetectorStats m_stats;

    // The analysis window trails the newest sample by half a window, so a click sits in the
    // middle of the Hann window instead of its tapering edge (worth ~12 dB of SNR).
    static constexpr int kAnalysisLag = kFftSize / 2;
    std::array<float, kFftSize + kAnalysisLag> m_window {};
    size_t m_filled = 0;
    uint64_t m_hopStart = 0;
    bool m_haveHopStart = false;

    // rolling frame history, used for the attack slope and to backdate the onset
    std::deque<FrameRecord> m_history;

    int m_sampleRate = kSampleRate;
    int m_hop = kHopFrames;
    float m_gateScale = 1.f;
    bool m_calibration = false;

    uint64_t m_refractoryUntil = 0;
    uint64_t m_suppressUntil = 0;
    uint32_t m_loudRun = 0;
    float m_lastAcceptedPeakDb = -120.f;
    float m_lastBroadDb = -120.f;
    float m_lastPeakBandDb = -120.f;
    uint64_t m_refractoryFrames = 0;
    uint64_t m_lastAcceptedFrame = 0;
    uint32_t m_warmupLeft = 0;
    uint64_t m_voiceHoldFramesValue = 0;
};

} // namespace micclicks
