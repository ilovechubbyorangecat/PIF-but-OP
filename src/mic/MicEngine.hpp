#pragma once
// Mic Clicks: the real-time engine.
//
// Threads and queues (nothing on the audio path takes a lock, and the game thread never
// waits for audio):
//
//   capture callback  --ring buffer-->  DSP worker  --click queue-->  render callback
//   game thread       --input queue-->  DSP worker
//   game thread       --delivery queue->  DSP worker
//   render callback   --clock atomics->  DSP worker
//
// The DSP worker owns everything that is expensive: FFT, fingerprint matching, slicing and
// latency estimation. The game thread only timestamps its inputs (a few dozen ns each).

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AudioBackend.hpp"
#include "AudioRing.hpp"
#include "ClickMixer.hpp"
#include "Dsp.hpp"
#include "Envelope.hpp"
#include "Fingerprint.hpp"
#include "JitterBuffer.hpp"
#include "RhythmLock.hpp"
#include "SpscQueue.hpp"
#include "TransientDetector.hpp"

namespace micclicks {

// The microphone and the click output have independent clocks, so the monitoring path runs
// through a jitter buffer: 12 ms of pre-roll absorbs the drift, anything above 40 ms of lag is
// trimmed back, and 300 ms is as much as we ever hold (if the output device stops draining).
inline constexpr int kMonitorPreRollMs = 12;
inline constexpr int kMonitorMaxLagMs = 40;
inline constexpr int kMonitorCapacityMs = 300;
// Smooth open/close for the monitored stream: no chop when it starts, stops or runs dry.
inline constexpr float kMonitorGateAttackMs = 8.f;
inline constexpr float kMonitorGateReleaseMs = 30.f;
// Frames mixed per inner chunk in the render callback (fixed, so nothing is allocated).
inline constexpr size_t kMixChunkFrames = 512;

enum class MonitorMode : int {
    ClicksOnly = 0,
    Everything = 1,
};

enum class AlignMode : int {
    GameTick = 0,      // follow the tick the game actually registered the input on
    PhysicalClick = 1, // follow the player's physical click
};

struct EngineConfig {
    // Devices are remembered by name: indices change whenever a USB device is plugged in.
    std::string captureName;  // empty = system default
    std::string renderName;
    float sensitivity = 1.f;
    float delayOverrideMs = 0.f;
    float clickGain = 1.f;
    float monitorGain = 0.8f;
    bool monitor = false;
    int monitorMode = 0;
    int alignMode = 0;
    float preRollMs = 12.f;
    float tailMs = 45.f;  // ~35-50 ms of click after the transient peak
};

struct EngineStatus {
    bool running = false;
    bool backendReady = false;
    bool calibrating = false;
    bool calibrated = false;
    bool fingerprintReady = false;
    int calibrationPhase = 0;  // 0 idle, 1 quiet, 2 clicks, 3 done, 4 failed
    float calibrationProgress = 0.f;
    float calibrationConfidence = 0.f;
    int calibrationClicks = 0;
    float inputLevelDb = -120.f;  // broadband level of the newest analysis frame
    float inputPeakDb = -120.f;   // peak of the last ~150 ms
    float noiseFloorDb = -85.f;
    float micDelayMs = 0.f;
    float delayOverrideMs = 0.f;
    float jitterMs = 0.f;
    float driftMsPerMinute = 0.f;
    float confidence = 0.f;
    int windowPairs = 0;
    int detections = 0;
    int accepted = 0;
    int rejectedGate = 0;
    int rejectedShape = 0;
    int rejectedRefractory = 0;
    int rejectedSustained = 0;
    int naiveMissed = 0;
    int scheduled = 0;
    int lateClicks = 0;
    int droppedClicks = 0;
    int unmatched = 0;
    int queueDepth = 0;
    // monitoring (jitter buffer) health and the negotiated render format
    int monitorUnderruns = 0;
    int monitorTrims = 0;
    int voices = 0;       // click voices playing in the last render block
    int voiceSteals = 0;  // how often a voice had to be stolen (all 8 busy)
    int monitorFillMs = 0;
    int renderSampleRate = 0;
    int renderPeriodFrames = 0;
    char captureDevice[96] = {};
    char renderDevice[96] = {};
    char backend[48] = {};
    char message[192] = {};
};

class MicEngine {
public:
    MicEngine();
    ~MicEngine();

    MicEngine(MicEngine const&) = delete;
    MicEngine& operator=(MicEngine const&) = delete;

    bool start(EngineConfig const& config, std::string& error);
    void stop();
    void applyConfig(EngineConfig const& config, std::string* error = nullptr);
    bool running() const {
        return m_running.load(std::memory_order_acquire);
    }
    EngineConfig config() const;

    std::vector<audio::DeviceInfo> captureDevices(std::string* error = nullptr);
    std::vector<audio::DeviceInfo> renderDevices(std::string* error = nullptr);
    std::string backendName() const;
    bool backendAvailable() const;
    std::string backendReason() const;

    // Game thread. `delivered` = this call is the one the game's own input handler runs
    // for (a click the buffer held back is delivered later than it was pressed).
    void noteInput(bool down, bool delivered, int64_t timeNs, int64_t tick);
    void flush();

    // Calibration (main thread; the worker does the work).
    void startCalibration();
    void cancelCalibration();
    // Hands the freshly learned fingerprint to the caller, once.
    bool takeFingerprint(ClickFingerprint& out);
    void setFingerprint(ClickFingerprint const& fingerprint);
    bool hasFingerprint() const;

    EngineStatus status() const;
    double micDelayMs() const;
    double confidence() const;

private:
    struct ClickSlice {
        uint64_t startFrame = 0;
        uint32_t length = 0;
        uint32_t onsetOffset = 0;
        std::array<float, kMaxSliceFrames> samples {};
    };

    struct InputMessage {
        int64_t timeNs = 0;
        int64_t tick = 0;
        uint8_t down = 0;
        uint8_t delivered = 0;
    };

    struct DeliveryMessage {
        int64_t timeNs = 0;
        int64_t tick = 0;
        uint8_t down = 0;
    };

    struct InputRecord {
        double timeMs = 0.0;
        int64_t timeNs = 0;
        bool down = false;
        bool used = false;
    };

    struct DeliveryRecord {
        double timeMs = 0.0;
        int64_t timeNs = 0;
        bool down = false;
    };

    struct PendingClick {
        bool down = false;
        int64_t physicalNs = 0;
        uint64_t onsetFrame = 0;
        double createdMs = 0.0;
    };

    static void captureCallback(void* user, float const* mono, size_t frames);
    static size_t renderCallback(void* user, float* stereo, size_t frames);

    void onCapture(float const* mono, size_t frames);
    size_t onRender(float* stereo, size_t frames);
    void workerLoop();
    void drainInputs(double nowMs);
    void drainDeliveries();
    void processDetection(Detection const& detection, int64_t arrivalNs, double nowMs);
    void processCalibration(Detection const& detection, double nowMs);
    void tryMatchPending(double nowMs);
    void scheduleClick(bool down, int64_t targetNs, uint64_t onsetFrame, double nowMs);
    void finishCalibration();
    void applyPendingFingerprint();
    void publish();
    void setMessage(std::string const& text);
    void stopDevices();
    bool startDevices(std::string& error);
    static int resolveDevice(std::vector<audio::DeviceInfo> const& devices, std::string const& name);
    void configureMonitorBuffer();
    int64_t leadNs() const;
    int64_t minAudibleLeadNs() const;
    int64_t overrideNs() const;

    // configuration (main thread writes, everything reads through atomics/copies)
    mutable std::mutex m_configMutex;
    EngineConfig m_config;
    std::atomic<float> m_sensitivity { 1.f };
    std::atomic<float> m_delayOverrideMs { 0.f };
    std::atomic<float> m_clickGain { 1.f };
    std::atomic<float> m_monitorGain { 0.8f };
    std::atomic<bool> m_monitor { false };
    std::atomic<int> m_monitorMode { 0 };
    std::atomic<int> m_alignMode { 0 };

    std::atomic<bool> m_running { false };
    std::atomic<bool> m_flushRequested { false };
    std::atomic<bool> m_clearOutput { false };

    // audio devices (main thread only)
    std::unique_ptr<audio::Backend> m_backend;
    std::unique_ptr<audio::CaptureStream> m_captureStream;
    std::unique_ptr<audio::RenderStream> m_renderStream;
    std::string m_captureName;
    std::string m_renderName;
    std::string m_backendName;

    // queues
    AudioRing m_ring;
    SpscQueue<ClickSlice, 8> m_clickQueue;
    JitterBuffer m_monitorJitter;
    std::array<float, kMixChunkFrames> m_monitorScratch {};
    std::array<float, kMixChunkFrames * 2> m_mixScratch {};
    std::atomic<bool> m_monitorReset { false };
    GateEnvelope m_monitorGate;
    SpscQueue<InputMessage, 256> m_inputQueue;
    SpscQueue<DeliveryMessage, 256> m_deliveryQueue;
    std::atomic<uint32_t> m_inputOverflow { 0 };

    // DSP worker
    std::thread m_worker;
    TransientDetector m_detector;
    RhythmLock m_lock;
    std::vector<Detection> m_detections;
    std::deque<InputRecord> m_pendingInputs;
    std::deque<DeliveryRecord> m_deliveries;
    std::deque<PendingClick> m_pendingClicks;
    std::array<float, kMaxSliceFrames> m_sliceScratch {};
    std::array<float, kHopFrames> m_hopScratch {};
    double m_leadEmaMs = 14.0;
    int64_t m_lastDetectionNs = 0;

    // render clock and the buffer the backend gave us (main thread writes, worker reads)
    std::atomic<uint64_t> m_renderFrame { 0 };
    std::atomic<int64_t> m_minLeadNs { 11'000'000 };
    std::atomic<uint32_t> m_renderSampleRate { 0 };
    std::atomic<uint32_t> m_renderPeriodFrames { 0 };
    std::atomic<uint32_t> m_renderPeriods { 0 };
    std::atomic<int64_t> m_renderTimeNs { 0 };
    std::atomic<float> m_capturePeakDb { -120.f };
    ClickMixer m_clickMixer;
    ClickSlice m_popScratch;    // one queue slot, reused so the callback never allocates
    ClickSlice m_sliceMessage;  // the worker's outgoing slice (heap-free, 16 KB)
    std::atomic<int> m_voiceCount { 0 };
    std::atomic<int> m_voiceSteals { 0 };

    // calibration
    std::atomic<int> m_calibrationRequest { 0 };
    std::atomic<bool> m_calibrating { false };
    std::atomic<int> m_calibrationPhase { 0 };
    std::atomic<float> m_calibrationProgress { 0.f };
    std::atomic<float> m_calibrationConfidence { 0.f };
    std::atomic<int> m_calibrationClicks { 0 };
    double m_calibrationStartMs = 0.0;
    std::vector<float> m_ambient;
    std::vector<ClickCandidate> m_candidates;

    mutable std::mutex m_fingerprintMutex;
    ClickFingerprint m_fingerprint;
    bool m_fingerprintDirty = false;
    std::atomic<bool> m_fingerprintApply { false };

    // statistics (worker writes, snapshot copies)
    uint32_t m_statDetections = 0;
    uint32_t m_statScheduled = 0;
    uint32_t m_statLate = 0;
    uint32_t m_statDropped = 0;
    uint32_t m_statUnmatched = 0;

    mutable std::mutex m_statusMutex;
    EngineStatus m_status;
};

} // namespace micclicks
