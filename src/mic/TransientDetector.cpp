#include "TransientDetector.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace micclicks {

TransientDetector::TransientDetector() {
    configure(kSampleRate, kHopFrames);
    m_window.fill(0.f);
    m_analyzer.setStartupFloor(-75.f);
}

void TransientDetector::configure(int sampleRate, int hopFrames) {
    m_sampleRate = sampleRate > 0 ? sampleRate : kSampleRate;
    m_hop = std::clamp(hopFrames, 16, kFftSize);
    m_refractoryFrames = static_cast<uint64_t>(m_config.refractoryMs * 0.001f * static_cast<float>(m_sampleRate));
    m_voiceHoldFramesValue = static_cast<uint64_t>(m_config.voiceHoldFrames) * static_cast<uint64_t>(m_hop);
    reset();
}

void TransientDetector::setFingerprint(ClickFingerprint const& fingerprint) {
    m_fingerprint = fingerprint;
    if (fingerprint.valid) {
        m_analyzer.setFloor(fingerprint.floorDb, fingerprint.floorBroadDb);
    }
}

void TransientDetector::setGateScale(float scale) {
    m_gateScale = clampf(scale, 0.25f, 4.f);
}

void TransientDetector::setCalibrationMode(bool on) {
    m_calibration = on;
}

void TransientDetector::setNoiseFloor(std::array<float, kBandCount> const& floorDb, float floorBroadDb) {
    m_analyzer.setFloor(floorDb, floorBroadDb);
}

void TransientDetector::reset() {
    m_history.clear();
    m_filled = 0;
    m_haveHopStart = false;
    m_window.fill(0.f);
    m_refractoryUntil = 0;
    m_suppressUntil = 0;
    m_loudRun = 0;
    m_lastAcceptedPeakDb = -120.f;
    m_lastBroadDb = -120.f;
    m_lastPeakBandDb = -120.f;
    m_warmupLeft = static_cast<uint32_t>(std::max(0, m_config.warmupHops));
}

void TransientDetector::push(float const* samples, size_t frames, uint64_t startFrame, std::vector<Detection>& out) {
    if (!samples || frames == 0) {
        return;
    }
    if (!m_haveHopStart) {
        m_haveHopStart = true;
        m_hopStart = startFrame;
    }

    // Compact any gap (stream restarted, or the worker fell behind).
    if (startFrame != m_hopStart + m_filled) {
        m_filled = 0;
        m_hopStart = startFrame;
    }

    uint64_t position = startFrame;
    size_t index = 0;
    while (index < frames) {
        size_t room = static_cast<size_t>(m_hop) - m_filled;
        size_t take = std::min(room, frames - index);
        std::memmove(m_window.data(), m_window.data() + take, (m_window.size() - take) * sizeof(float));
        std::memcpy(m_window.data() + m_window.size() - take, samples + index, take * sizeof(float));
        m_filled += take;
        index += take;
        position += take;
        if (m_filled >= static_cast<size_t>(m_hop)) {
            m_filled = 0;
            m_hopStart = position;
            uint64_t analysedEnd = position > static_cast<uint64_t>(kAnalysisLag) ? position - static_cast<uint64_t>(kAnalysisLag) : 0;
            analyseHop(m_window.data(), analysedEnd, out);
        }
    }
}

void TransientDetector::measureAmbient(float const* samples, size_t frames) {
    // Same windowing as the detector, but only the floor is updated.
    if (!samples || frames < kFftSize) {
        return;
    }
    BandFeatures features;
    for (size_t offset = 0; offset + kFftSize <= frames; offset += static_cast<size_t>(m_hop)) {
        m_analyzer.analyze(samples + offset, features);
        m_analyzer.trackFloor(features);
        m_analyzer.trackFloor(features);
    }
    m_lastBroadDb = features.broadDb;
}

uint64_t TransientDetector::estimateOnset(uint64_t windowCenter) const {
    if (m_history.empty()) {
        return windowCenter;
    }
    float gate = m_config.gateDb / m_gateScale;
    float floorBroad = m_analyzer.floorBroadDb();
    uint64_t onset = m_history.back().endFrame;
    // Walk backwards while the level is still above half the gate: that is the start of
    // the attack rather than the peak we happened to trigger on.
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (it->broadDb < floorBroad + gate * 0.35f) {
            break;
        }
        onset = it->endFrame;
    }
    if (onset > static_cast<uint64_t>(kFftSize / 2)) {
        onset -= static_cast<uint64_t>(kFftSize / 2);
    }
    return onset < windowCenter ? onset : windowCenter;
}

void TransientDetector::analyseHop(float const* window, uint64_t endFrame, std::vector<Detection>& out) {
    FrameRecord record;
    record.endFrame = endFrame;
    m_analyzer.analyze(window, record.features);
    record.broadDb = record.features.broadDb;
    record.fluxDb = record.features.fluxDb;
    record.centroidHz = record.features.centroidHz;

    uint64_t windowCenter = endFrame > static_cast<uint64_t>(kFftSize / 2) ? endFrame - static_cast<uint64_t>(kFftSize / 2) : 0;

    // Attack: how far above the quietest of the last few frames we are right now.
    float recentMin = record.broadDb;
    for (auto const& past : m_history) {
        recentMin = std::min(recentMin, past.broadDb);
    }
    float attackDb = record.broadDb - recentMin;

    m_lastBroadDb = record.broadDb;
    float peakBandSnr = -200.f;
    for (int b = 0; b < kBandCount; ++b) {
        float snr = record.features.bandDb[static_cast<size_t>(b)] - m_analyzer.floorDb()[static_cast<size_t>(b)];
        peakBandSnr = std::max(peakBandSnr, snr);
    }
    m_lastPeakBandDb = peakBandSnr;

    m_history.push_back(record);
    while (m_history.size() > 8) {
        m_history.pop_front();
    }

    float gate = m_config.gateDb / m_gateScale;
    float floorBroad = m_analyzer.floorBroadDb();
    float snr = record.broadDb - floorBroad;
    bool loud = snr > gate || peakBandSnr > gate + 8.f;

    // Right after start (or a device change) the floor is still unknown: only learn it.
    if (m_warmupLeft > 0) {
        --m_warmupLeft;
        m_analyzer.trackFloor(record.features);
        return;
    }

    // A voice, music or a fan: stays loud for a long time. Suppress until it is quiet
    // again, so a talker next to the mic cannot fire clicks.
    if (loud) {
        ++m_loudRun;
        // Still loud while suppressed (a voice does not stop talking mid-sentence): keep rolling
        // the suppression forward instead of reopening the gate every hold period.
        bool extend = endFrame < m_suppressUntil;
        if (m_loudRun > static_cast<uint32_t>(m_config.voiceRunFrames) || extend) {
            if (!extend) {
                ++m_stats.rejectedSustained;
            }
            m_suppressUntil = endFrame + m_voiceHoldFramesValue;
            if (m_loudRun > static_cast<uint32_t>(m_config.voiceRunFrames)) {
                m_loudRun = static_cast<uint32_t>(m_config.voiceRunFrames);
            }
        }
    }
    else {
        m_loudRun = 0;  // the run has to be contiguous to count as voice
        if (m_suppressUntil > endFrame + m_voiceHoldFramesValue) {
            m_suppressUntil = endFrame + m_voiceHoldFramesValue;
        }
    }

    if (endFrame < m_suppressUntil) {
        m_analyzer.trackFloor(record.features);
        return;
    }

    float fluxMin = m_calibration ? 1.2f : m_config.fluxMinDb;
    float riseMin = m_calibration ? 2.0f : m_config.riseMinDb;
    bool candidate = loud && record.fluxDb > fluxMin && attackDb > riseMin;
    if (!candidate) {
        if (loud) {
            ++m_stats.rejectedGate;
        }
        m_analyzer.trackFloor(record.features);
        return;
    }
    ++m_stats.candidates;

    // Spectral shape: the fingerprint decides between "click" and "something else loud".
    float shapeScore = 0.f;
    bool shapeOk = true;
    if (m_fingerprint.valid && !m_calibration) {
        // A dull hit (keyboard thud, desk knock, voice) is not the finger's switch.
        float brightness = bandBrightness(record.features.bandDb);
        if (brightness < m_fingerprint.brightness * 0.45f) {
            ++m_stats.rejectedShape;
            m_analyzer.trackFloor(record.features);
            return;
        }
        shapeScore = shapeSimilarity(record.features.shape, m_fingerprint.shape);
        shapeOk = shapeScore >= m_fingerprint.shapeThreshold();
        if (!shapeOk) {
            float strong = snr > gate + 12.f;
            shapeOk = strong && shapeScore >= 0.30f;
        }
        // Low, dull and unlike a click: almost always a voice.
        if (!shapeOk && record.centroidHz < 1200.f) {
            shapeOk = false;
        }
    }
    if (!shapeOk) {
        ++m_stats.rejectedShape;
        m_analyzer.trackFloor(record.features);
        return;
    }

    float peakDb = record.broadDb;
    // A click right after a click is the same click. Only a much louder hit, and not
    // before half the refractory window, counts as a new one (a keyboard and a mouse
    // pressed together).
    bool retrigger = peakDb > m_lastAcceptedPeakDb + 12.f &&
        endFrame - m_lastAcceptedFrame > m_refractoryFrames / 2;
    if (endFrame < m_refractoryUntil && !retrigger) {
        ++m_stats.rejectedRefractory;
        m_analyzer.trackFloor(record.features);
        return;
    }

    Detection detection;
    detection.windowFrame = windowCenter;
    detection.onsetFrame = estimateOnset(windowCenter);
    detection.peakDb = peakDb;
    detection.snrDb = snr;
    detection.shapeScore = shapeScore;
    detection.fluxDb = record.fluxDb;
    detection.centroidHz = record.centroidHz;
    detection.attackDb = attackDb;
    detection.naiveMiss = snr < 6.f;  // a fixed floor+6 dB gate would not have seen this
    detection.features = record.features;
    out.push_back(detection);

    ++m_stats.accepted;
    if (detection.naiveMiss) {
        ++m_stats.naiveMissed;
    }
    m_lastAcceptedPeakDb = peakDb;
    m_lastAcceptedFrame = endFrame;
    m_refractoryUntil = endFrame + m_refractoryFrames;
    m_analyzer.trackFloor(record.features);
}

} // namespace micclicks
