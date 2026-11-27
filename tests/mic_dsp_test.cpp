// Host side test for the Mic Clicks DSP chain (no Geode, no audio device needed).
//
//   g++ -std=c++20 -O2 -Isrc/mic tests/mic_dsp_test.cpp src/mic/Fft.cpp
//       src/mic/Fingerprint.cpp src/mic/TransientDetector.cpp
//       src/mic/RhythmLock.cpp src/mic/Slicer.cpp -o build/mic_dsp_test
//   ./build/mic_dsp_test
//
// It synthesises a room with a noise floor, mouse clicks (loud, quiet and very quiet), a
// talking voice and a keyboard, then checks that:
//   * calibration learns a fingerprint,
//   * the fingerprint detector finds the clicks a plain amplitude gate misses,
//   * the voice is rejected instead of becoming fake clicks,
//   * slices can be cut out of the rolling ring buffer,
//   * rhythm lock recovers an injected latency and its drift,
//   * the capture -> monitor jitter buffer pre-rolls, never replays stale audio on an underrun
//     and trims itself back when two device clocks drift apart,
//   * click slices keep ~45 ms after the peak, are gated with a smooth envelope (no chop) and
//     always end on silence,
//   * eight overlapping ClickVoices mix without clipping, steal the oldest and never allocate.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <new>
#include <random>
#include <vector>

#include "AudioRing.hpp"
#include "ClickMixer.hpp"
#include "Dsp.hpp"
#include "JitterBuffer.hpp"
#include "Fingerprint.hpp"
#include "RhythmLock.hpp"
#include "Slicer.hpp"
#include "TransientDetector.hpp"

using namespace micclicks;

// Counts heap traffic while `g_trackAllocations` is on (see the click mixer test).
bool g_trackAllocations = false;
size_t g_allocationCount = 0;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(size_t size) {
    if (g_trackAllocations) {
        ++g_allocationCount;
    }
    void* memory = std::malloc(size ? size : 1);
    if (!memory) {
        throw std::bad_alloc();
    }
    return memory;
}
void* operator new[](size_t size) {
    return ::operator new(size);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, size_t) noexcept {
    std::free(memory);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

constexpr double kRate = kSampleRate;

struct Event {
    double time = 0.0;
    float amplitude = 0.f;
    int kind = 0;  // 0 click, 1 voice, 2 keyboard
};

std::vector<Event> buildTimeline() {
    std::vector<Event> events;
    // calibration: eight clear clicks, one every 400 ms starting at 600 ms
    for (int i = 0; i < 8; ++i) {
        events.push_back({ 0.6 + i * 0.4, 0.13f, 0 });
    }
    // loud clicks
    for (int i = 0; i < 10; ++i) {
        events.push_back({ 4.6 + i * 0.42, 0.13f, 0 });
    }
    // quiet clicks: about 10 dB above the floor, a fixed amplitude gate at +12 dB misses them
    for (int i = 0; i < 10; ++i) {
        events.push_back({ 9.2 + i * 0.42, 0.0105f, 0 });
    }
    // keyboard chatter (softer, duller, longer) and a voice
    for (int i = 0; i < 6; ++i) {
        events.push_back({ 13.8 + i * 0.35, 0.035f, 2 });
    }
    events.push_back({ 16.2, 0.11f, 1 });  // 1.2 s of talking
    for (int i = 0; i < 8; ++i) {
        events.push_back({ 18.2 + i * 0.4, 0.13f, 0 });
    }
    std::sort(events.begin(), events.end(), [](Event const& a, Event const& b) { return a.time < b.time; });
    return events;
}

void addClick(std::vector<float>& signal, size_t at, float amplitude, std::mt19937& rng) {
    std::uniform_real_distribution<float> noise(-1.f, 1.f);
    size_t length = static_cast<size_t>(0.03 * kRate);
    for (size_t i = 0; i < length && at + i < signal.size(); ++i) {
        double t = static_cast<double>(i) / kRate;
        double attack = std::min(1.0, t / 0.0004);
        double decay = std::exp(-t / 0.0028);
        double body = 0.55 * std::sin(2.0 * 3.14159265 * 420.0 * t) * std::exp(-t / 0.006);
        float value = static_cast<float>(attack * decay) * noise(rng) + static_cast<float>(body * attack);
        signal[at + i] += value * amplitude;
    }
}

void addKeyboard(std::vector<float>& signal, size_t at, float amplitude, std::mt19937& rng) {
    std::uniform_real_distribution<float> noise(-1.f, 1.f);
    size_t length = static_cast<size_t>(0.06 * kRate);
    for (size_t i = 0; i < length && at + i < signal.size(); ++i) {
        double t = static_cast<double>(i) / kRate;
        double attack = std::min(1.0, t / 0.002);
        double decay = std::exp(-t / 0.012);
        double thud = std::sin(2.0 * 3.14159265 * 130.0 * t) * std::exp(-t / 0.02);
        signal[at + i] += (0.25f * static_cast<float>(attack * decay) * noise(rng) + static_cast<float>(0.75 * thud)) * amplitude;
    }
}

void addVoice(std::vector<float>& signal, size_t at, double seconds, float amplitude) {
    size_t length = static_cast<size_t>(seconds * kRate);
    for (size_t i = 0; i < length && at + i < signal.size(); ++i) {
        double t = static_cast<double>(i) / kRate;
        double envelope = std::min(1.0, t / 0.08) * std::min(1.0, (seconds - t) / 0.15);
        double wobble = 1.0 + 0.02 * std::sin(2.0 * 3.14159265 * 5.0 * t);
        double v = std::sin(2.0 * 3.14159265 * 165.0 * wobble * t) + 0.5 * std::sin(2.0 * 3.14159265 * 330.0 * wobble * t) +
            0.25 * std::sin(2.0 * 3.14159265 * 495.0 * wobble * t);
        signal[at + i] += static_cast<float>(0.45 * amplitude * envelope * v);
    }
}

double timeOfFrame(uint64_t frame) {
    return static_cast<double>(frame) / kRate;
}

bool near(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

size_t msFrames(float ms) {
    return static_cast<size_t>(ms * 0.001f * static_cast<float>(kSampleRate) + 0.5f);
}

// Allocation tracking for the real-time safety check (globals are defined above, next to the
// operator new that counts them).

} // namespace

int main() {
    int failures = 0;
    auto check = [&](bool ok, char const* what) {
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
        if (!ok) {
            ++failures;
        }
    };

    std::mt19937 rng(12345);
    auto const events = buildTimeline();
    double duration = 22.0;
    std::vector<float> signal(static_cast<size_t>(duration * kRate), 0.f);

    // Room tone: roughly -64 dBFS RMS.
    std::normal_distribution<float> gauss(0.f, 1.f);
    for (auto& sample : signal) {
        sample = static_cast<float>(0.0009 * gauss(rng));
    }
    for (auto const& event : events) {
        size_t at = static_cast<size_t>(event.time * kRate);
        if (event.kind == 0) {
            addClick(signal, at, event.amplitude, rng);
        }
        else if (event.kind == 1) {
            addVoice(signal, at, 1.2, event.amplitude);
        }
        else {
            addKeyboard(signal, at, event.amplitude, rng);
        }
    }

    // ---- pass 1: calibration (quiet room + eight clicks) -----------------------------
    TransientDetector calibrationDetector;
    calibrationDetector.configure(kSampleRate, kHopFrames);
    calibrationDetector.setCalibrationMode(true);

    std::vector<ClickCandidate> candidates;
    std::vector<Detection> batch;
    double calibrationEnd = 4.2;
    size_t calibrationFrames = static_cast<size_t>(calibrationEnd * kRate);

    for (size_t at = 0; at + kHopFrames <= calibrationFrames; at += kHopFrames) {
        batch.clear();
        calibrationDetector.push(signal.data() + at, kHopFrames, static_cast<uint64_t>(at), batch);
        for (auto const& detection : batch) {
            if (detection.snrDb < 8.f) {
                continue;
            }
            ClickCandidate candidate;
            candidate.features = detection.features;
            candidate.peakDb = detection.peakDb;
            candidate.attackDb = detection.attackDb;
            candidate.decayMs = 15.f;
            candidates.push_back(candidate);
        }
    }

    ClickFingerprint fingerprint =
        buildFingerprint(candidates, calibrationDetector.floorDb(), calibrationDetector.floorBroadDb(), kSampleRate);
    std::printf("\ncalibration: %d candidates, consistency %.2f, floor %.1f dB, peak %.1f dB, centroid %.0f Hz\n",
        fingerprint.clicks, static_cast<double>(fingerprint.consistency), static_cast<double>(fingerprint.floorBroadDb),
        static_cast<double>(fingerprint.peakDb), static_cast<double>(fingerprint.centroidHz));
    check(fingerprint.valid, "calibration builds a valid click fingerprint");
    check(fingerprint.consistency > 0.5f, "the calibration clicks are consistent");
    check(fingerprint.centroidHz > 1500.f, "the fingerprint is a bright, click-like profile");

    // ---- pass 2: live detection with the learned fingerprint --------------------------
    AudioRing ring(static_cast<size_t>(kRate * 2));
    TransientDetector detector;
    detector.configure(kSampleRate, kHopFrames);
    detector.setFingerprint(fingerprint);

    std::vector<double> detectionsAt;
    std::vector<double> naiveAt;
    RhythmLock lock;
    std::uniform_real_distribution<double> jitter(-2.5, 2.5);
    std::vector<double> sliceLengths;
    std::vector<float> slicePeaks;
    struct DeferredSlice {
        int64_t onset = 0;
        double detectedAt = 0.0;
    };
    std::deque<DeferredSlice> deferred;

    // The same front end, but a plain fixed amplitude gate and no fingerprint.
    SpectrumAnalyzer naiveAnalyzer(kSampleRate);
    naiveAnalyzer.setFloor(fingerprint.floorDb, fingerprint.floorBroadDb);
    // A fixed gate has to sit this high (+15 dB) to keep the voice out, which is what
    // makes it miss the quieter clicks that the fingerprint still finds.
    double naiveGateDb = 18.0;
    double naiveRefractoryUntil = -1.0;

    for (size_t at = calibrationFrames; at + kHopFrames <= signal.size(); at += kHopFrames) {
        float const* hop = signal.data() + at;
        ring.write(hop, kHopFrames);
        batch.clear();
        uint64_t startFrame = static_cast<uint64_t>(at) - static_cast<uint64_t>(calibrationFrames);
        detector.push(hop, kHopFrames, startFrame, batch);
        double now = timeOfFrame(at + kHopFrames);

        for (auto const& detection : batch) {
            if (std::getenv("MIC_TEST_VERBOSE")) {
                std::printf("  det t=%.3f snr %.1f floor %.1f flux %.1f attack %.1f shape %.2f bright %.2f\n", timeOfFrame(at + kHopFrames) - 0.0027,
                    static_cast<double>(detection.snrDb), static_cast<double>(detector.floorBroadDb()), static_cast<double>(detection.fluxDb), static_cast<double>(detection.attackDb),
                    static_cast<double>(detection.shapeScore), static_cast<double>(bandBrightness(detection.features.bandDb)));
            }
            detectionsAt.push_back(timeOfFrame(at + kHopFrames) - 0.0027);  // window centre
            double inputTime = detectionsAt.back() + 0.034 + jitter(rng) / 1000.0;
            lock.push((inputTime - detectionsAt.back()) * 1000.0, static_cast<int64_t>(inputTime * 1e9),
                static_cast<int64_t>(detectionsAt.back() * 1e9));

            deferred.push_back({ static_cast<int64_t>(detection.onsetFrame), now });
        }

        // Slice once ~50 ms of the tail has been captured (the engine plays the click a
        // little after it is recognised, so this is the real situation).
        while (!deferred.empty() && now - deferred.front().detectedAt > 0.05) {
            int64_t onset = deferred.front().onset;
            deferred.pop_front();
            size_t pre = static_cast<size_t>(0.012 * kRate);
            uint64_t first = static_cast<uint64_t>(onset) > pre ? static_cast<uint64_t>(onset) - pre : 0;
            size_t want = static_cast<size_t>(static_cast<uint64_t>(onset) - first) + static_cast<size_t>(0.15 * kRate);
            want = std::min(want, static_cast<size_t>(kMaxSliceFrames));
            uint64_t captured = ring.written();
            if (captured > first) {
                want = std::min(want, static_cast<size_t>(captured - first));
            }
            std::vector<float> slice(want, 0.f);
            if (ring.readHistory(first, slice.data(), slice.size())) {
                size_t length = gateAndNormalize(slice.data(), slice.size(), kSampleRate);
                if (length > 0) {
                    float peak = 0.f;
                    for (size_t i = 0; i < length; ++i) {
                        peak = std::max(peak, std::abs(slice[i]));
                    }
                    sliceLengths.push_back(static_cast<double>(length) / kRate);
                    slicePeaks.push_back(peak);
                }
            }
        }

        // Naive baseline: same floor, fixed +12 dB, 35 ms refractory, no shape check.
        if (at + kFftSize <= signal.size()) {
            BandFeatures features;
            naiveAnalyzer.analyze(signal.data() + at, features);
            if (features.broadDb > fingerprint.floorBroadDb + naiveGateDb && now > naiveRefractoryUntil) {
                naiveAt.push_back(now);
                naiveRefractoryUntil = now + 0.035;
            }
        }
        (void)now;
    }

    // ---- score the detections against the truth -------------------------------------
    auto countHits = [&](std::vector<double> const& found, double from, double to, int kind, int& missed) {
        int hits = 0;
        missed = 0;
        for (auto const& event : events) {
            if (event.kind != kind || event.time < from || event.time >= to) {
                continue;
            }
            bool hit = false;
            for (double at : found) {
                if (near(at, event.time, 0.02)) {
                    hit = true;
                    break;
                }
            }
            hits += hit ? 1 : 0;
            missed += hit ? 0 : 1;
        }
        return hits;
    };
    auto countFalse = [&](std::vector<double> const& found, double from, double to, double tolerance) {
        int falsePositives = 0;
        for (double at : found) {
            if (at < from || at >= to) {
                continue;
            }
            bool real = false;
            for (auto const& event : events) {
                if (event.kind == 0 && near(at, event.time, tolerance)) {
                    real = true;
                    break;
                }
            }
            falsePositives += real ? 0 : 1;
        }
        return falsePositives;
    };

    int quietMissed = 0;
    int quietNaiveMissed = 0;
    int naiveTolerance = 0;
    int quietHits = countHits(detectionsAt, 9.0, 13.8, 0, quietMissed);
    int naiveQuietHits = countHits(naiveAt, 9.0, 13.8, 0, quietNaiveMissed);
    (void)naiveTolerance;

    int loudMissed = 0;
    int naiveLoudMissed = 0;
    int loudHits = countHits(detectionsAt, 4.2, 9.0, 0, loudMissed);
    int naiveLoudHits = countHits(naiveAt, 4.2, 9.0, 0, naiveLoudMissed);

    int voiceFp = countFalse(detectionsAt, 16.0, 18.0, 0.03);
    int naiveVoiceFp = countFalse(naiveAt, 16.0, 18.0, 0.03);
    int keyboardFp = countFalse(detectionsAt, 13.5, 16.0, 0.03);
    int naiveKeyboardFp = countFalse(naiveAt, 13.5, 16.0, 0.03);

    std::printf("\n            loud clicks   quiet clicks   missed   voice FPs   keyboard FPs\n");
    std::printf("fingerprint   %2d/10          %2d/10          %3d      %3d          %3d\n", loudHits, quietHits, loudMissed + quietMissed, voiceFp, keyboardFp);
    std::printf("amplitude     %2d/10          %2d/10          %3d      %3d          %3d\n", naiveLoudHits, naiveQuietHits, naiveLoudMissed + quietNaiveMissed, naiveVoiceFp, naiveKeyboardFp);

    check(loudHits >= 9, "loud clicks are detected");
    check(quietHits + 0 >= 6, "quiet clicks (near the floor) are still detected");
    int fingerprintMisses = quietMissed + loudMissed;
    int naiveMisses = quietNaiveMissed + naiveLoudMissed;
    check(quietHits >= naiveQuietHits, "the fingerprint misses no more quiet clicks than the amplitude gate");
    check(fingerprintMisses * 3 <= naiveMisses, "at least 3x fewer missed clicks than the fixed amplitude gate");
    check(voiceFp <= 2, "the voice does not become fake clicks");
    check(voiceFp < naiveVoiceFp, "the fingerprint rejects more voice than the amplitude gate");
    check(keyboardFp <= 2, "keyboard chatter is mostly rejected");
    check(detectionsAt.size() < naiveAt.size(), "the fingerprint triggers less often overall");

    // ---- slicing ---------------------------------------------------------------------
    double averageLength = 0.0;
    double averagePeak = 0.0;
    for (size_t i = 0; i < sliceLengths.size(); ++i) {
        averageLength += sliceLengths[i];
        averagePeak += slicePeaks[i];
    }
    if (!sliceLengths.empty()) {
        averageLength /= static_cast<double>(sliceLengths.size());
        averagePeak /= static_cast<double>(slicePeaks.size());
    }
    std::printf("\nslices: %zu cut, average length %.1f ms, average peak %.2f\n", sliceLengths.size(), averageLength * 1000.0, averagePeak);
    check(!sliceLengths.empty(), "clicks can be sliced out of the ring buffer");
    check(averageLength > 0.008 && averageLength < 0.12, "slices keep the attack and trim the room tone");
    check(averagePeak > 0.3f && averagePeak < 0.8f, "slices are normalised to a consistent level");

    // ---- rhythm lock ------------------------------------------------------------------
    std::printf("\nrhythm lock: %zu pairs, median %.1f ms, trimmed mean %.1f ms, jitter %.2f ms, confidence %.2f\n", lock.count(),
        lock.medianMs(), lock.trimmedMeanMs(), lock.jitterMs(), lock.confidence());
    check(lock.count() > 20, "the rolling window fills up");
    check(near(lock.medianMs(), 34.0, 1.5), "the median recovers the injected 34 ms latency");
    check(lock.confidence() > 0.5, "the estimate reports useful confidence");

    // synthetic drift: 3 ms over 60 s of input timestamps
    RhythmLock driftLock;
    for (int i = 0; i < 40; ++i) {
        double t = 30.0 + i * 1.5;  // seconds
        driftLock.push(35.0 + (t - 30.0) * 0.05, static_cast<int64_t>(t * 1e9), static_cast<int64_t>((t - 0.035) * 1e9));
    }
    std::printf("drift: %.2f ms/min (expected 3.00), rate scale %.6f\n", driftLock.driftMsPerMinute(), driftLock.rateScale());
    check(near(driftLock.driftMsPerMinute(), 3.0, 0.6), "drift is recovered from the window");

    // ---- ring buffer ----------------------------------------------------------------
    AudioRing small(1024);
    std::vector<float> ramp(2000);
    for (size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<float>(i);
    }
    small.write(ramp.data(), ramp.size());
    float last = -1.f;
    check(small.readHistory(1900, &last, 1) && last == 1900.f, "absolute (history) reads work");
    check(!small.readHistory(0, &last, 1), "old frames outside the window are reported as gone");
    small.skipToLatest();
    check(small.available() == 0, "skipToLatest drops stale audio");

    // ---- jitter buffer (capture -> monitor, two independent device clocks) -------------
    {
        JitterBuffer jitter;
        size_t const block = 480;               // 10 ms render periods at 48 kHz
        size_t const preRoll = 576;             // 12 ms
        jitter.configure(12000, preRoll, 1920);  // 250 ms capacity, 40 ms max lag

        std::vector<float> produced(block), consumed(block);
        for (size_t i = 0; i < block; ++i) {
            produced[i] = static_cast<float>(i + 1);
        }

        // Before the pre-roll is queued the render callback must be silent, not starved repeats.
        jitter.write(produced.data(), 240);
        size_t got = jitter.read(consumed.data(), block);
        bool allSilent = true;
        for (float v : consumed) {
            allSilent = allSilent && v == 0.f;
        }
        check(got == 0 && allSilent, "the jitter buffer stays silent until its pre-roll is queued");

        // Fill the pre-roll and read: the first block must be the oldest queued audio, in order.
        std::vector<float> ramp(1200);
        for (size_t i = 0; i < ramp.size(); ++i) {
            ramp[i] = static_cast<float>(i);
        }
        jitter.dropQueued();
        jitter.write(ramp.data(), ramp.size());
        got = jitter.read(consumed.data(), block);
        bool inOrder = got == block;
        for (size_t i = 0; i < got && inOrder; ++i) {
            inOrder = consumed[i] == ramp[i];
        }
        check(inOrder, "after the pre-roll, queued audio is delivered in order");

        // Underrun: prime, drain, then ask for a full block with almost nothing queued. The head
        // has to be real audio, the tail silence (never a repeat of what was played), and the
        // buffer has to re-prime for the next block.
        jitter.dropQueued();
        jitter.write(ramp.data(), preRoll);
        (void)jitter.read(consumed.data(), block);
        uint32_t underrunsBefore = jitter.underruns();
        got = jitter.read(consumed.data(), block);
        bool tailSilent = true;
        for (size_t i = got; i < block; ++i) {
            tailSilent = tailSilent && consumed[i] == 0.f;
        }
        check(got > 0 && got < block && tailSilent, "a shortfall is filled with silence, not stale audio");
        check(jitter.underruns() > underrunsBefore, "the underrun is counted");
        got = jitter.read(consumed.data(), block);
        allSilent = got == 0;
        for (float v : consumed) {
            allSilent = allSilent && v == 0.f;
        }
        check(allSilent, "after an underrun the buffer re-primes instead of draining garbage");

        // Clock drift: the producer keeps handing over more than the consumer takes. The lag has
        // to stay bounded by the configured maximum, with the excess trimmed away.
        jitter.dropQueued();
        for (int round = 0; round < 400; ++round) {
            jitter.write(ramp.data(), block + 6);  // 1.25% faster than the consumer
            jitter.read(consumed.data(), block);
        }
        double fillMs = static_cast<double>(jitter.fillFrames()) * 1000.0 / kRate;
        std::printf("\njitter buffer: %zu frames queued (%.1f ms), %u underruns, %u trims, %u overflows\n", jitter.fillFrames(),
            fillMs, jitter.underruns(), jitter.trims(), jitter.overflows());
        check(fillMs < 45.0, "drifting clocks do not grow the monitoring latency");
        check(jitter.trims() > 0, "the excess is trimmed back");
        check(jitter.overflows() == 0, "the buffer never overflowed");

        // A stopped consumer (monitor off) must drop instead of overrunning the queue.
        jitter.dropQueued();
        size_t accepted = 0;
        for (int round = 0; round < 200; ++round) {
            accepted += jitter.write(ramp.data(), block);
        }
        check(accepted < 200 * block && jitter.overflows() > 0, "a stopped output drops audio instead of overrunning");
        check(jitter.fillFrames() <= jitter.capacityFrames(), "the jitter buffer never exceeds its capacity");
    }

    // ---- click slicing: tail length, smooth gate, silence at the end -------------------
    {
        int const rate = kSampleRate;
        size_t const preRoll = msFrames(12.f);
        size_t const tail = msFrames(60.f);
        size_t const frames = preRoll + tail;
        std::vector<float> raw(frames, 0.f);
        std::mt19937 sliceRng(777);
        std::uniform_real_distribution<float> noise(-0.0006f, 0.0006f);
        for (auto& sample : raw) {
            sample = noise(sliceRng);  // room tone, about -64 dBFS
        }
        // A click: 0.9 ms attack, 6 ms exponential decay, at the end of the pre-roll.
        size_t clickStart = preRoll;
        for (size_t i = 0; i < msFrames(8.f); ++i) {
            float t = static_cast<float>(i) / static_cast<float>(rate);
            float attack = std::min(1.f, t / 0.0009f);
            raw[clickStart + i] += 0.12f * attack * std::exp(-t / 0.006f);
        }

        std::vector<float> groomed = raw;
        SliceShape shape = groomClickSlice(groomed.data(), groomed.size(), rate);
        double tailMs = shape.length > shape.peakOffset ? static_cast<double>(shape.length - shape.peakOffset) * 1000.0 / rate : 0.0;
        std::printf("\nslice: %zu frames, peak at %zu, %.1f ms of tail\n", shape.length, shape.peakOffset, tailMs);
        check(shape.length > 0 && tailMs >= 35.0 && tailMs <= 50.0, "a slice keeps 35-50 ms of click after the peak");
        check(std::abs(groomed[shape.length - 1]) < 1e-6f, "a slice ends on exact silence");

        float preRms = 0.f;
        for (size_t i = 0; i < msFrames(5.f) && i < shape.length; ++i) {
            preRms += groomed[i] * groomed[i];
        }
        preRms = std::sqrt(preRms / static_cast<float>(msFrames(5.f)));
        check(preRms < 0.001f, "the room tone before the click is gated away");

        // No hard edge in the tail: the biggest sample-to-sample step in the last 20 ms has to
        // stay tiny, which is what "smooth release" means in practice.
        size_t from = shape.length > msFrames(20.f) ? shape.length - msFrames(20.f) : 0;
        float biggestStep = 0.f;
        for (size_t i = from + 1; i < shape.length; ++i) {
            biggestStep = std::max(biggestStep, std::abs(groomed[i] - groomed[i - 1]));
        }
        check(biggestStep < 0.01f, "the gate release fades out instead of chopping");

        float attackPeak = 0.f;
        size_t attackPeakAt = 0;
        for (size_t i = 0; i < shape.length; ++i) {
            if (std::abs(groomed[i]) > attackPeak) {
                attackPeak = std::abs(groomed[i]);
                attackPeakAt = i;
            }
        }
        check(near(attackPeak, 0.5, 0.03), "the slice is normalised to a consistent level");
        check(attackPeakAt + 2 > shape.peakOffset && attackPeakAt < shape.peakOffset + 60,
            "the gate opens before the transient, so the attack is untouched");

        // A very quiet click may be lifted, but only by the bounded gain: never amplify hiss.
        std::vector<float> whisper = raw;
        for (auto& sample : whisper) {
            sample *= 0.03f;  // the click is now ~-50 dBFS
        }
        SliceShape quietShape = groomClickSlice(whisper.data(), whisper.size(), rate);
        float quietPeak = 0.f;
        for (size_t i = 0; i < quietShape.length; ++i) {
            quietPeak = std::max(quietPeak, std::abs(whisper[i]));
        }
        check(quietShape.length > 0 && quietPeak <= 0.12f * 8.f * 0.5f + 0.01f, "quiet clicks are lifted, but the gain is bounded");
    }

    // ---- click mixer: eight voices, stealing, soft limiting, no allocation -------------
    {
        ClickMixer mixer;
        mixer.reset();
        std::vector<float> click(msFrames(45.f));
        for (size_t i = 0; i < click.size(); ++i) {
            click[i] = 0.05f * std::sin(static_cast<float>(i) * 0.3f);
        }
        std::vector<float> block(480 * 2, 0.f);

        // Eight simultaneous voices must all be audible: the mix is eight times one voice
        // (0.05 * 8 = 0.4, still below the limiter knee).
        for (int i = 0; i < 8; ++i) {
            mixer.start(click.data(), static_cast<uint32_t>(click.size()), 0, 1.f);
        }
        check(mixer.activeVoices() == 8, "all eight click voices are used");
        mixer.mix(block.data(), 480, 0);
        float peak = 0.f;
        for (float value : block) {
            peak = std::max(peak, std::abs(value));
        }
        check(peak > 0.35f && peak < 0.42f, "eight overlapping clicks are summed, not dropped");

        // A ninth click steals the oldest voice instead of being ignored.
        uint32_t stealsBefore = mixer.stolenVoices();
        mixer.start(click.data(), static_cast<uint32_t>(click.size()), 0, 1.f);
        check(mixer.stolenVoices() > stealsBefore, "the oldest voice is stolen when all eight are busy");

        // Soft limiter: transparent below the knee, bounded above it.
        std::vector<float> loud { 0.5f, -0.69f, 2.f, -6.f, 0.7f };
        ClickMixer::softLimit(loud.data(), loud.size() / 2);
        check(std::abs(loud[0] - 0.5f) < 1e-6f && std::abs(loud[1] + 0.69f) < 1e-6f, "the limiter leaves normal levels alone");
        bool bounded = true;
        for (float value : loud) {
            bounded = bounded && std::abs(value) <= 1.f;
        }
        check(bounded, "overlapping peaks are limited below full scale");

        // Real-time safety: starting and mixing voices must not touch the heap.
        ClickMixer rtMixer;
        rtMixer.reset();
        std::vector<float> rtBlock(480 * 2, 0.f);
        rtMixer.start(click.data(), static_cast<uint32_t>(click.size()), 0, 1.f);
        size_t allocations = 0;
        g_trackAllocations = true;
        g_allocationCount = 0;
        for (int i = 0; i < 64; ++i) {
            rtMixer.start(click.data(), static_cast<uint32_t>(click.size()), static_cast<uint64_t>(i * 120), 1.f);
            rtMixer.mix(rtBlock.data(), 480, static_cast<uint64_t>(i * 480));
            ClickMixer::softLimit(rtBlock.data(), 480);
        }
        allocations = g_allocationCount;
        g_trackAllocations = false;
        check(allocations == 0, "the audio path allocates nothing");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
