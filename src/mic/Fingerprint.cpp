#include "Fingerprint.hpp"

#include <algorithm>
#include <cmath>

namespace micclicks {

namespace {
constexpr float kRef = static_cast<float>((kFftSize / 2) * (kFftSize / 2));  // full scale reference
constexpr float kFloorMinDb = -110.f;
}

SpectrumAnalyzer::SpectrumAnalyzer(int sampleRate) {
    float binHz = static_cast<float>(sampleRate) / static_cast<float>(kFftSize);
    float low = 120.f;
    float high = std::min(15000.f, 0.45f * static_cast<float>(sampleRate));
    int previous = 1;
    for (int i = 0; i <= kBandCount; ++i) {
        float ratio = static_cast<float>(i) / static_cast<float>(kBandCount);
        float freq = low * std::pow(high / low, ratio);
        int edge = static_cast<int>(std::lround(freq / binHz));
        edge = std::clamp(edge, previous, kFftSize / 2);
        m_edges[static_cast<size_t>(i)] = edge;
        // every band gets at least one bin, so no band can end up empty
        previous = std::min(edge + 1, kFftSize / 2);
    }
    m_floorDb.fill(-85.f);
    m_lastBandDb.fill(-120.f);
    m_prevBandDb.fill(-120.f);
    m_blockSum.fill(0.f);
    for (auto& block : m_blocks) {
        block.fill(-85.f);
    }
    m_blockBroad.fill(-85.f);
    m_blockSumBroad = 0.f;
}

void SpectrumAnalyzer::setFloor(std::array<float, kBandCount> const& floorDb, float floorBroadDb) {
    m_floorDb = floorDb;
    m_floorBroadDb = floorBroadDb;
}

void SpectrumAnalyzer::setStartupFloor(float broadDb) {
    m_floorDb.fill(std::clamp(broadDb - 6.f, kFloorMinDb, -20.f));
    m_floorBroadDb = std::clamp(broadDb, kFloorMinDb, -20.f);
    m_blockSum.fill(0.f);
    m_blockSumBroad = 0.f;
    for (auto& block : m_blocks) {
        block = m_floorDb;
    }
    m_blockBroad.fill(m_floorBroadDb);
    m_blockFill = 0;
    m_blockNext = 0;
    m_blocksReady = false;
}

void SpectrumAnalyzer::analyze(float const* samples, BandFeatures& out) {
    std::array<float, kFftSize / 2> power {};
    m_fft.powerSpectrum(samples, power);

    float total = 0.f;
    float weightedHz = 0.f;
    float binHz = static_cast<float>(kSampleRate) / static_cast<float>(kFftSize);
    for (int b = 0; b < kBandCount; ++b) {
        int first = m_edges[b];
        int end = m_edges[b + 1];
        float sum = 0.f;
        for (int k = first; k < end; ++k) {
            sum += power[static_cast<size_t>(k)];
        }
        m_lastBandDb[static_cast<size_t>(b)] = energyToDb(sum / kRef);
        out.bandDb[static_cast<size_t>(b)] = m_lastBandDb[static_cast<size_t>(b)];
        total += sum;
        weightedHz += sum * binHz * static_cast<float>(first + end) * 0.5f;
    }

    out.broadDb = energyToDb(total / kRef);
    out.centroidHz = total > 1e-9f ? weightedHz / total : 0.f;

    // Positive spectral change: a click is a sudden broadband jump, a voice a slower one.
    float flux = 0.f;
    if (m_havePrev) {
        for (int b = 0; b < kBandCount; ++b) {
            float delta = out.bandDb[static_cast<size_t>(b)] - m_prevBandDb[static_cast<size_t>(b)];
            flux += clampf(delta, 0.f, 24.f);
        }
        flux /= static_cast<float>(kBandCount);
    }
    out.fluxDb = flux;
    m_prevBandDb = out.bandDb;
    m_havePrev = true;

    // Normalised shape: overall loudness removed, so a soft and a loud click of the same
    // switch match each other.
    float mean = 0.f;
    for (float v : out.bandDb) {
        mean += v;
    }
    mean /= static_cast<float>(kBandCount);
    float norm = 0.f;
    for (int b = 0; b < kBandCount; ++b) {
        float v = out.bandDb[static_cast<size_t>(b)] - mean;
        out.shape[static_cast<size_t>(b)] = v;
        norm += v * v;
    }
    norm = std::sqrt(norm);
    if (norm > 1e-6f) {
        for (float& v : out.shape) {
            v /= norm;
        }
    }
}

void SpectrumAnalyzer::trackFloor(BandFeatures const& features) {
    for (int b = 0; b < kBandCount; ++b) {
        m_blockSum[static_cast<size_t>(b)] += features.bandDb[static_cast<size_t>(b)];
    }
    m_blockSumBroad += features.broadDb;
    ++m_blockFill;
    if (m_blockFill >= kBlockFrames) {
        for (int b = 0; b < kBandCount; ++b) {
            m_blocks[static_cast<size_t>(m_blockNext)][static_cast<size_t>(b)] =
                m_blockSum[static_cast<size_t>(b)] / static_cast<float>(kBlockFrames);
        }
        m_blockBroad[static_cast<size_t>(m_blockNext)] = m_blockSumBroad / static_cast<float>(kBlockFrames);
        m_blockNext = (m_blockNext + 1) % kBlocks;
        if (m_blockNext == 0) {
            m_blocksReady = true;
        }
        m_blockSum.fill(0.f);
        m_blockSumBroad = 0.f;
        m_blockFill = 0;
    }

    // Quietest recent block, plus the block still being filled, so neither a voice nor a
    // click can raise the floor. 1e30 marks "nothing measured yet" (a negative sentinel
    // would win the min() and read as an impossibly quiet room).
    constexpr float kNone = 1.0e30f;
    int filled = m_blocksReady ? kBlocks : m_blockNext;
    for (int b = 0; b < kBandCount; ++b) {
        float value = kNone;
        if (m_blockFill > 0) {
            value = m_blockSum[static_cast<size_t>(b)] / static_cast<float>(m_blockFill);
        }
        for (int i = 0; i < filled; ++i) {
            value = std::min(value, m_blocks[static_cast<size_t>(i)][static_cast<size_t>(b)]);
        }
        if (value < kNone * 0.5f) {
            m_floorDb[static_cast<size_t>(b)] = std::clamp(value, kFloorMinDb, -20.f);
        }
    }
    float broad = kNone;
    if (m_blockFill > 0) {
        broad = m_blockSumBroad / static_cast<float>(m_blockFill);
    }
    for (int i = 0; i < filled; ++i) {
        broad = std::min(broad, m_blockBroad[static_cast<size_t>(i)]);
    }
    if (broad < kNone * 0.5f) {
        m_floorBroadDb = std::clamp(broad, kFloorMinDb, -20.f);
    }
}

float shapeSimilarity(std::array<float, kBandCount> const& a, std::array<float, kBandCount> const& b) {
    float dot = 0.f;
    float na = 0.f;
    float nb = 0.f;
    for (int i = 0; i < kBandCount; ++i) {
        dot += a[static_cast<size_t>(i)] * b[static_cast<size_t>(i)];
        na += a[static_cast<size_t>(i)] * a[static_cast<size_t>(i)];
        nb += b[static_cast<size_t>(i)] * b[static_cast<size_t>(i)];
    }
    if (na < 1e-9f || nb < 1e-9f) {
        return 0.f;
    }
    return dot / std::sqrt(na * nb);
}

float ClickFingerprint::shapeThreshold() const {
    // Tighter when the calibration clicks looked alike, looser when the user has an
    // unusual switch and the samples varied.
    float spread = clampf(1.f - consistency, 0.f, 1.f);
    return clampf(0.58f - 0.12f * spread, 0.42f, 0.58f);
}

float bandBrightness(std::array<float, kBandCount> const& bandDb) {
    float high = 0.f;
    float all = 0.f;
    for (int b = 0; b < kBandCount; ++b) {
        float energy = std::pow(10.f, bandDb[static_cast<size_t>(b)] / 10.f);
        all += energy;
        if (b >= kBrightBand) {
            high += energy;
        }
    }
    return all > 1e-9f ? high / all : 0.f;
}

ClickFingerprint buildFingerprint(std::vector<ClickCandidate> const& candidates, std::array<float, kBandCount> const& floorDb,
    float floorBroadDb, int sampleRate) {
    ClickFingerprint fp;
    fp.sampleRate = sampleRate;
    fp.floorDb = floorDb;
    fp.floorBroadDb = floorBroadDb;
    fp.clicks = static_cast<int>(candidates.size());
    if (candidates.size() < 3) {
        return fp;  // not enough clicks to learn a profile from
    }

    std::vector<float> scratch;
    scratch.reserve(candidates.size());
    std::array<float, kBandCount> median {};
    for (int b = 0; b < kBandCount; ++b) {
        scratch.clear();
        for (auto const& c : candidates) {
            scratch.push_back(c.features.bandDb[static_cast<size_t>(b)]);
        }
        median[static_cast<size_t>(b)] = medianInPlace(scratch.data(), static_cast<int>(scratch.size()));
    }

    float mean = 0.f;
    for (float v : median) {
        mean += v;
    }
    mean /= static_cast<float>(kBandCount);
    float norm = 0.f;
    for (int b = 0; b < kBandCount; ++b) {
        float v = median[static_cast<size_t>(b)] - mean;
        fp.shape[static_cast<size_t>(b)] = v;
        norm += v * v;
    }
    norm = std::sqrt(norm);
    if (norm < 1e-6f) {
        return fp;
    }
    for (float& v : fp.shape) {
        v /= norm;
    }

    // How alike were the samples? Also the median of the scalar features.
    float similaritySum = 0.f;
    for (auto const& c : candidates) {
        similaritySum += shapeSimilarity(c.features.shape, fp.shape);
    }
    fp.consistency = clampf(similaritySum / static_cast<float>(candidates.size()), 0.f, 1.f);

    std::vector<float> values;
    values.reserve(candidates.size());
    for (auto const& c : candidates) {
        values.push_back(bandBrightness(c.features.bandDb));
    }
    fp.brightness = medianInPlace(values.data(), static_cast<int>(values.size()));
    values.clear();
    for (auto const& c : candidates) {
        values.push_back(c.peakDb);
    }
    fp.peakDb = medianInPlace(values.data(), static_cast<int>(values.size()));
    values.clear();
    for (auto const& c : candidates) {
        values.push_back(c.attackDb);
    }
    fp.attackDb = medianInPlace(values.data(), static_cast<int>(values.size()));
    values.clear();
    for (auto const& c : candidates) {
        values.push_back(c.decayMs);
    }
    fp.decayMs = medianInPlace(values.data(), static_cast<int>(values.size()));
    values.clear();
    for (auto const& c : candidates) {
        values.push_back(c.features.centroidHz);
    }
    fp.centroidHz = medianInPlace(values.data(), static_cast<int>(values.size()));
    values.clear();
    for (auto const& c : candidates) {
        values.push_back(c.features.fluxDb);
    }
    fp.fluxDb = medianInPlace(values.data(), static_cast<int>(values.size()));

    // A profile that is flat (the room) or a pure low hum is not a click.
    fp.valid = fp.consistency > 0.45f && norm > 2.5f && fp.peakDb > fp.floorBroadDb + 4.f;
    return fp;
}

} // namespace micclicks
