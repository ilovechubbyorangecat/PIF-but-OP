#include "MicEngine.hpp"

#include "Slicer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace micclicks {

namespace {
constexpr double kPairWindowMs = 120.0;      // how far apart an input and a click may be
constexpr double kDeliveryWindowMs = 250.0;  // a held back input is delivered within this
constexpr size_t kAmbientMax = kSampleRate * 3;

double msToNs(double ms) {
    return ms * 1.0e6;
}

void copyName(char* dst, size_t size, std::string const& src) {
    if (size == 0) {
        return;
    }
    size_t n = std::min(src.size(), size - 1);
    std::memcpy(dst, src.c_str(), n);
    dst[n] = '\0';
}
} // namespace

MicEngine::MicEngine() {
    m_detections.reserve(4);
    m_pendingInputs.clear();
    m_hopScratch.fill(0.f);
    m_sliceScratch.fill(0.f);
    m_ring.resize(kSampleRate * 4);  // 4 s of raw click history
    m_backend = audio::createBackend();
    if (m_backend) {
        m_backendName = m_backend->name();
    }
}

MicEngine::~MicEngine() {
    stop();
}

EngineConfig MicEngine::config() const {
    std::lock_guard<std::mutex> guard(m_configMutex);
    return m_config;
}

std::string MicEngine::backendName() const {
    return m_backend ? m_backend->name() : "none";
}

bool MicEngine::backendAvailable() const {
    return m_backend && m_backend->available();
}

std::string MicEngine::backendReason() const {
    return m_backend ? m_backend->unavailableReason() : "no backend";
}

std::vector<audio::DeviceInfo> MicEngine::captureDevices(std::string* error) {
    if (!m_backend) {
        if (error) {
            *error = "no backend";
        }
        return {};
    }
    return m_backend->enumerate(true, error);
}

std::vector<audio::DeviceInfo> MicEngine::renderDevices(std::string* error) {
    if (!m_backend) {
        if (error) {
            *error = "no backend";
        }
        return {};
    }
    return m_backend->enumerate(false, error);
}

int MicEngine::resolveDevice(std::vector<audio::DeviceInfo> const& devices, std::string const& name) {
    if (name.empty()) {
        return -1;
    }
    for (auto const& device : devices) {
        if (device.name == name) {
            return device.id;
        }
    }
    return -1;
}

bool MicEngine::startDevices(std::string& error) {
    if (!m_backend || !m_backend->available()) {
        error = backendReason();
        return false;
    }
    EngineConfig cfg = config();
    std::string enumError;

    auto captures = m_backend->enumerate(true, &enumError);
    auto renders = m_backend->enumerate(false, &enumError);
    int captureId = resolveDevice(captures, cfg.captureName);
    int renderId = resolveDevice(renders, cfg.renderName);

    audio::CaptureRequest capture;
    capture.deviceId = captureId;
    capture.sampleRate = audio::kBackendSampleRate;
    capture.periodMs = audio::kDefaultPeriodMs;
    capture.periods = audio::kDefaultPeriods;
    capture.sharedMode = true;
    capture.user = this;
    capture.sink = &MicEngine::captureCallback;
    m_captureStream = m_backend->openCapture(capture, enumError);
    if (!m_captureStream) {
        error = enumError.empty() ? "could not open the microphone" : enumError;
        return false;
    }

    audio::RenderRequest render;
    render.deviceId = renderId;
    render.sampleRate = audio::kBackendSampleRate;
    render.periodMs = audio::kDefaultPeriodMs;
    render.periods = audio::kDefaultPeriods;
    render.sharedMode = true;
    render.user = this;
    render.pull = &MicEngine::renderCallback;
    m_renderStream = m_backend->openRender(render, enumError);
    if (!m_renderStream) {
        error = enumError.empty() ? "could not open the output device" : enumError;
        m_captureStream.reset();
        return false;
    }

    m_captureName = cfg.captureName.empty() ? "Default microphone" : cfg.captureName;
    m_renderName = cfg.renderName.empty() ? "Default output" : cfg.renderName;
    for (auto const& device : captures) {
        if (device.id == captureId) {
            m_captureName = device.name;
        }
    }
    for (auto const& device : renders) {
        if (device.id == renderId) {
            m_renderName = device.name;
        }
    }

    if (!m_captureStream->start()) {
        error = "the microphone did not start";
        stopDevices();
        return false;
    }
    if (!m_renderStream->start()) {
        error = "the output device did not start";
        stopDevices();
        return false;
    }

    // A click can only be heard once the render buffer that holds it has been handed to the
    // device, so the scheduler has to aim at least one full device buffer ahead.
    audio::StreamInfo info = m_renderStream->info();
    uint32_t rate = info.sampleRate ? info.sampleRate : static_cast<uint32_t>(audio::kBackendSampleRate);
    double bufferMs = (info.periodFrames && info.periods)
        ? static_cast<double>(info.periodFrames) * static_cast<double>(info.periods) * 1000.0 / static_cast<double>(rate)
        : static_cast<double>(audio::kDefaultPeriodMs * audio::kDefaultPeriods);
    m_minLeadNs.store(static_cast<int64_t>((bufferMs + 2.0) * 1.0e6), std::memory_order_relaxed);
    m_renderSampleRate.store(rate, std::memory_order_relaxed);
    m_renderPeriodFrames.store(info.periodFrames, std::memory_order_relaxed);
    m_renderPeriods.store(info.periods, std::memory_order_relaxed);
    return true;
}

void MicEngine::stopDevices() {
    if (m_captureStream) {
        m_captureStream->stop();
        m_captureStream.reset();
    }
    if (m_renderStream) {
        m_renderStream->stop();
        m_renderStream.reset();
    }
}

bool MicEngine::start(EngineConfig const& config, std::string& error) {
    if (m_running.load(std::memory_order_acquire)) {
        return true;
    }
    {
        std::lock_guard<std::mutex> guard(m_configMutex);
        m_config = config;
    }
    m_sensitivity.store(config.sensitivity, std::memory_order_relaxed);
    m_delayOverrideMs.store(config.delayOverrideMs, std::memory_order_relaxed);
    m_clickGain.store(config.clickGain, std::memory_order_relaxed);
    m_monitorGain.store(config.monitorGain, std::memory_order_relaxed);
    m_monitor.store(config.monitor, std::memory_order_relaxed);
    m_monitorMode.store(config.monitorMode, std::memory_order_relaxed);
    m_alignMode.store(config.alignMode, std::memory_order_relaxed);

    m_ring.resize(kSampleRate * 4);
    configureMonitorBuffer();
    m_clickMixer.reset();
    m_monitorGate.configure(kSampleRate, kMonitorGateAttackMs, kMonitorGateReleaseMs);
    m_monitorGate.reset(0.f);
    m_voiceCount.store(0, std::memory_order_relaxed);
    m_voiceSteals.store(0, std::memory_order_relaxed);
    m_clickQueue.clear();
    m_inputQueue.clear();
    m_deliveryQueue.clear();
    m_lock.reset();
    m_statDetections = m_statScheduled = m_statLate = m_statDropped = m_statUnmatched = 0;
    m_leadEmaMs = 45.0;

    if (!startDevices(error)) {
        publish();
        return false;
    }
    m_running.store(true, std::memory_order_release);
    m_worker = std::thread([this] { workerLoop(); });
    publish();
    return true;
}

void MicEngine::stop() {
    bool wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
    stopDevices();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_clickMixer.reset();
    m_monitorGate.reset(0.f);
    m_voiceCount.store(0, std::memory_order_relaxed);
    m_monitorJitter.reset();
    m_calibrating.store(false);
    m_calibrationPhase.store(0);
    m_pendingInputs.clear();
    m_deliveries.clear();
    m_pendingClicks.clear();
    m_ambient.clear();
    m_candidates.clear();
    publish();
    (void)wasRunning;
}

void MicEngine::applyConfig(EngineConfig const& config, std::string* error) {
    EngineConfig previous = this->config();
    {
        std::lock_guard<std::mutex> guard(m_configMutex);
        m_config = config;
    }
    m_sensitivity.store(config.sensitivity, std::memory_order_relaxed);
    m_delayOverrideMs.store(config.delayOverrideMs, std::memory_order_relaxed);
    m_clickGain.store(config.clickGain, std::memory_order_relaxed);
    m_monitorGain.store(config.monitorGain, std::memory_order_relaxed);
    m_monitor.store(config.monitor, std::memory_order_relaxed);
    m_monitorMode.store(config.monitorMode, std::memory_order_relaxed);
    m_alignMode.store(config.alignMode, std::memory_order_relaxed);

    if (!running()) {
        return;
    }
    if (previous.monitor != config.monitor || previous.monitorMode != config.monitorMode) {
        // Switching the monitor on or off must not replay what was queued while it was off.
        m_monitorReset.store(true, std::memory_order_relaxed);
    }
    if (previous.captureName != config.captureName || previous.renderName != config.renderName) {
        // Devices are opened on the main thread only; the worker keeps running and simply
        // stops receiving audio for a moment.
        stopDevices();
        m_flushRequested.store(true, std::memory_order_relaxed);
        m_monitorReset.store(true, std::memory_order_relaxed);
        std::string local;
        if (!startDevices(local)) {
            if (error) {
                *error = local;
            }
            m_running.store(false);
            setMessage(local.empty() ? "audio device lost" : local);
            if (m_worker.joinable()) {
                m_worker.join();
            }
        }
        publish();
    }
}

void MicEngine::configureMonitorBuffer() {
    auto frames = [](int ms) { return static_cast<size_t>(ms) * static_cast<size_t>(kSampleRate) / 1000; };
    m_monitorJitter.configure(frames(kMonitorCapacityMs), frames(kMonitorPreRollMs), frames(kMonitorMaxLagMs));
    m_monitorReset.store(false, std::memory_order_relaxed);
}

void MicEngine::setMessage(std::string const& text) {
    std::lock_guard<std::mutex> guard(m_statusMutex);
    copyName(m_status.message, sizeof(m_status.message), text);
}

void MicEngine::captureCallback(void* user, float const* mono, size_t frames) {
    static_cast<MicEngine*>(user)->onCapture(mono, frames);
}

size_t MicEngine::renderCallback(void* user, float* stereo, size_t frames) {
    return static_cast<MicEngine*>(user)->onRender(stereo, frames);
}

void MicEngine::onCapture(float const* mono, size_t frames) {
    if (!mono || frames == 0) {
        return;
    }
    m_ring.write(mono, frames);
    float peak = 0.f;
    for (size_t i = 0; i < frames; ++i) {
        float v = std::abs(mono[i]);
        if (v > peak) {
            peak = v;
        }
    }
    float db = energyToDb(peak * peak);
    float previous = m_capturePeakDb.load(std::memory_order_relaxed);
    if (db > previous) {
        m_capturePeakDb.store(db, std::memory_order_relaxed);
    }
}

size_t MicEngine::onRender(float* stereo, size_t frames) {
    uint64_t blockStart = m_renderFrame.load(std::memory_order_relaxed);
    m_renderTimeNs.store(nowNs(), std::memory_order_relaxed);
    m_renderFrame.store(blockStart + frames, std::memory_order_relaxed);
    if (!stereo || frames == 0) {
        return 0;
    }

    // ---- real-time safety ------------------------------------------------------------
    // Everything below runs on the audio thread: no allocation (all buffers are members),
    // no mutex, no logging, no blocking call. The only cross-thread handovers are the SPSC
    // click queue and the SPSC jitter buffer, both wait-free.
    if (m_clearOutput.exchange(false, std::memory_order_acq_rel)) {
        m_clickMixer.reset();
        m_monitorGate.reset(0.f);
        m_voiceCount.store(0, std::memory_order_relaxed);
    }

    // Take whatever the DSP thread scheduled since the last callback and hand it to a voice.
    float clickGain = m_clickGain.load(std::memory_order_relaxed);
    while (m_clickQueue.pop(m_popScratch)) {
        m_clickMixer.start(m_popScratch.samples.data(), m_popScratch.length, m_popScratch.startFrame, clickGain);
    }
    m_voiceSteals.store(static_cast<int>(m_clickMixer.stolenVoices()), std::memory_order_relaxed);

    bool monitorEverything = m_monitor.load(std::memory_order_relaxed) &&
        m_monitorMode.load(std::memory_order_relaxed) == static_cast<int>(MonitorMode::Everything);
    bool monitorReady = monitorEverything && m_monitorJitter.configured();
    float monitorGain = m_monitorGain.load(std::memory_order_relaxed);
    if (!monitorReady) {
        // Nothing continuous is playing: keep the gate shut so switching it on fades in
        // instead of starting with a step.
        m_monitorGate.reset(0.f);
    }

    int voicesThisBlock = 0;
    size_t offset = 0;
    while (offset < frames) {
        size_t chunk = std::min(frames - offset, kMixChunkFrames);
        float* mix = m_mixScratch.data();
        std::memset(mix, 0, chunk * 2 * sizeof(float));

        voicesThisBlock = std::max(voicesThisBlock, static_cast<int>(m_clickMixer.mix(mix, chunk, blockStart + offset)));

        // The jitter buffer returns exactly `chunk` mono frames: silence where the microphone has
        // not delivered yet (underrun), never a replay of audio we already played. It is drained
        // even while the monitor is off, so switching it back on starts on fresh audio instead of
        // a backlog of stale samples.
        size_t taken = m_monitorJitter.configured()
            ? m_monitorJitter.read(m_monitorScratch.data(), chunk)
            : 0;
        if (monitorReady) {
            // The gate ramps over the underrun boundary (8 ms attack / 30 ms release), so a
            // re-prime is a fade instead of a chop.
            for (size_t i = 0; i < chunk; ++i) {
                float gate = m_monitorGate.next(i < taken);
                float value = m_monitorScratch[i] * monitorGain * gate;
                mix[i * 2] += value;
                mix[i * 2 + 1] += value;
            }
        }

        // Clicks + monitor together, soft limited: eight overlapping clicks compress smoothly
        // instead of clipping, and the monitor feed can never push the sum over full scale.
        ClickMixer::softLimit(mix, chunk);

        std::memcpy(stereo + offset * 2, mix, chunk * 2 * sizeof(float));
        offset += chunk;
    }

    m_voiceCount.store(voicesThisBlock, std::memory_order_relaxed);
    return frames;
}

void MicEngine::noteInput(bool down, bool delivered, int64_t timeNs, int64_t tick) {
    if (!running()) {
        return;
    }
    if (delivered) {
        DeliveryMessage message;
        message.timeNs = timeNs;
        message.tick = tick;
        message.down = down ? 1 : 0;
        if (!m_deliveryQueue.push(message)) {
            m_inputOverflow.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else {
        InputMessage message;
        message.timeNs = timeNs;
        message.tick = tick;
        message.down = down ? 1 : 0;
        message.delivered = 0;
        if (!m_inputQueue.push(message)) {
            m_inputOverflow.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void MicEngine::flush() {
    m_flushRequested.store(true, std::memory_order_relaxed);
    m_monitorReset.store(true, std::memory_order_relaxed);
}

void MicEngine::startCalibration() {
    m_calibrationRequest.store(1, std::memory_order_release);
}

void MicEngine::cancelCalibration() {
    m_calibrationRequest.store(2, std::memory_order_release);
}

bool MicEngine::hasFingerprint() const {
    std::lock_guard<std::mutex> guard(m_fingerprintMutex);
    return m_fingerprint.valid;
}

void MicEngine::setFingerprint(ClickFingerprint const& fingerprint) {
    std::lock_guard<std::mutex> guard(m_fingerprintMutex);
    m_fingerprint = fingerprint;
    m_fingerprintApply.store(true, std::memory_order_release);
}

bool MicEngine::takeFingerprint(ClickFingerprint& out) {
    std::lock_guard<std::mutex> guard(m_fingerprintMutex);
    if (!m_fingerprintDirty) {
        return false;
    }
    out = m_fingerprint;
    m_fingerprintDirty = false;
    return true;
}

double MicEngine::micDelayMs() const {
    return m_lock.delayMs();
}

double MicEngine::confidence() const {
    return m_lock.confidence();
}

EngineStatus MicEngine::status() const {
    std::lock_guard<std::mutex> guard(m_statusMutex);
    return m_status;
}

void MicEngine::drainInputs(double nowMs) {
    InputMessage message;
    while (m_inputQueue.pop(message)) {
        InputRecord record;
        record.timeNs = message.timeNs;
        record.timeMs = static_cast<double>(message.timeNs) / 1.0e6;
        record.down = message.down != 0;
        record.used = false;
        m_pendingInputs.push_back(record);
    }
    // Keep enough history for a late click, drop the rest.
    while (!m_pendingInputs.empty() && (nowMs - m_pendingInputs.front().timeMs > 600.0 || m_pendingInputs.size() > 128)) {
        m_pendingInputs.pop_front();
    }
}

void MicEngine::drainDeliveries() {
    DeliveryMessage message;
    while (m_deliveryQueue.pop(message)) {
        DeliveryRecord record;
        record.timeNs = message.timeNs;
        record.timeMs = static_cast<double>(message.timeNs) / 1.0e6;
        record.down = message.down != 0;
        m_deliveries.push_back(record);
    }
    while (m_deliveries.size() > 64) {
        m_deliveries.pop_front();
    }
}

void MicEngine::workerLoop() {
    m_detector.configure(kSampleRate, kHopFrames);
    m_detector.setGateScale(m_sensitivity.load(std::memory_order_relaxed));
    m_ring.skipToLatest();
    int64_t lastPublish = nowNs();

    while (m_running.load(std::memory_order_acquire)) {
        if (m_flushRequested.exchange(false, std::memory_order_acq_rel)) {
            m_pendingInputs.clear();
            m_deliveries.clear();
            m_pendingClicks.clear();
            m_clearOutput.store(true, std::memory_order_release);
        }
        if (m_fingerprintApply.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> guard(m_fingerprintMutex);
            m_detector.setFingerprint(m_fingerprint);
        }
        m_detector.setGateScale(m_sensitivity.load(std::memory_order_relaxed));

        if (m_monitorReset.exchange(false, std::memory_order_acq_rel)) {
            m_monitorJitter.dropQueued();
        }

        bool monitorEverything = m_monitor.load(std::memory_order_relaxed) &&
            m_monitorMode.load(std::memory_order_relaxed) == static_cast<int>(MonitorMode::Everything);
        if (!monitorEverything) {
            m_monitorJitter.dropQueued();
        }

        double nowMs = static_cast<double>(nowNs()) / 1.0e6;
        drainInputs(nowMs);
        drainDeliveries();

        // Calibration requests
        int request = m_calibrationRequest.exchange(0, std::memory_order_acq_rel);
        if (request == 1) {
            m_detector.reset();
            m_detector.setCalibrationMode(true);
            m_calibrating.store(true, std::memory_order_release);
            m_calibrationPhase.store(1, std::memory_order_release);
            m_calibrationProgress.store(0.f, std::memory_order_relaxed);
            m_calibrationClicks.store(0, std::memory_order_relaxed);
            m_calibrationConfidence.store(0.f, std::memory_order_relaxed);
            m_calibrationStartMs = nowMs;
            m_ambient.clear();
            m_candidates.clear();
            setMessage("Calibrating: stay quiet for a moment");
        }
        else if (request == 2) {
            m_calibrating.store(false, std::memory_order_release);
            m_calibrationPhase.store(0, std::memory_order_release);
            m_detector.setCalibrationMode(false);
            setMessage("Calibration cancelled");
        }

        if (m_calibrationPhase.load(std::memory_order_acquire) == 1) {
            double elapsed = nowMs - m_calibrationStartMs;
            if (elapsed >= 1500.0) {
                if (m_ambient.size() >= static_cast<size_t>(kFftSize)) {
                    m_detector.measureAmbient(m_ambient.data(), m_ambient.size());
                }
                m_ambient.clear();
                m_ambient.shrink_to_fit();
                m_calibrationPhase.store(2, std::memory_order_release);
                m_calibrationStartMs = nowMs;
                setMessage("Now click the mouse 8 times, about twice a second");
            }
        }
        else if (m_calibrationPhase.load(std::memory_order_acquire) == 2) {
            double elapsed = nowMs - m_calibrationStartMs;
            int clicks = m_calibrationClicks.load(std::memory_order_relaxed);
            float progress = clampf(0.35f + 0.65f * static_cast<float>(elapsed / 6000.0), 0.35f, 1.f);
            m_calibrationProgress.store(progress, std::memory_order_relaxed);
            if (elapsed >= 6000.0 || clicks >= 12) {
                finishCalibration();
            }
        }

        bool didWork = false;
        int budget = 64;
        while (budget-- > 0 && m_running.load(std::memory_order_relaxed)) {
            if (m_ring.available() < static_cast<size_t>(kHopFrames)) {
                break;
            }
            uint64_t firstFrame = m_ring.readPosition();
            size_t got = m_ring.read(m_hopScratch.data(), static_cast<size_t>(kHopFrames));
            if (got == 0) {
                break;
            }
            didWork = true;

            int phase = m_calibrationPhase.load(std::memory_order_acquire);
            if (phase == 1 && m_ambient.size() + got <= kAmbientMax) {
                m_ambient.insert(m_ambient.end(), m_hopScratch.begin(), m_hopScratch.begin() + static_cast<long>(got));
            }

            m_detections.clear();
            m_detector.push(m_hopScratch.data(), got, firstFrame, m_detections);

            if (phase == 2 || m_calibrating.load(std::memory_order_relaxed)) {
                for (auto const& detection : m_detections) {
                    processCalibration(detection, nowMs);
                }
            }
            else if (!m_detections.empty()) {
                uint64_t written = m_ring.written();
                int64_t now = nowNs();
                double rateScale = m_lock.rateScale();
                for (auto const& detection : m_detections) {
                    double framesBack = static_cast<double>(written - detection.onsetFrame);
                    double secondsBack = framesBack / (static_cast<double>(kSampleRate) * rateScale);
                    int64_t arrivalNs = now - static_cast<int64_t>(secondsBack * 1.0e9);
                    processDetection(detection, arrivalNs, nowMs);
                }
            }

            if (monitorEverything && got > 0) {
                // Straight into the jitter buffer: it owns the pre-roll and the trimming, so
                // nothing is lost at hop boundaries the way the old block queue lost the tail
                // of a block whenever the render callback asked for fewer frames than it had.
                m_monitorJitter.write(m_hopScratch.data(), got);
            }
        }

        tryMatchPending(nowMs);

        if (!didWork) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (nowNs() - lastPublish > 50'000'000) {
            lastPublish = nowNs();
            float peak = m_capturePeakDb.load(std::memory_order_relaxed);
            m_capturePeakDb.store(std::max(peak - 3.f, -120.f), std::memory_order_relaxed);
            publish();
        }
    }
}

void MicEngine::processDetection(Detection const& detection, int64_t arrivalNs, double nowMs) {
    ++m_statDetections;
    double arrivalMs = static_cast<double>(arrivalNs) / 1.0e6;

    InputRecord* best = nullptr;
    double bestDistance = kPairWindowMs;
    for (auto& input : m_pendingInputs) {
        if (input.used) {
            continue;
        }
        double distance = std::abs(input.timeMs - arrivalMs);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &input;
        }
    }
    if (!best) {
        ++m_statUnmatched;
        return;
    }
    best->used = true;
    bool down = best->down;
    int64_t physicalNs = best->timeNs;

    m_lock.push(best->timeMs - arrivalMs, best->timeNs, arrivalNs);

    if (m_alignMode.load(std::memory_order_relaxed) == static_cast<int>(AlignMode::PhysicalClick)) {
        scheduleClick(down, physicalNs, detection.onsetFrame, nowMs);
        return;
    }
    PendingClick pending;
    pending.down = down;
    pending.physicalNs = physicalNs;
    pending.onsetFrame = detection.onsetFrame;
    pending.createdMs = nowMs;
    m_pendingClicks.push_back(pending);
    tryMatchPending(nowMs);
}

void MicEngine::processCalibration(Detection const& detection, double nowMs) {
    (void)nowMs;
    if (detection.snrDb < 8.f) {
        return;
    }
    ClickCandidate candidate;
    candidate.features = detection.features;
    candidate.peakDb = detection.peakDb;
    candidate.attackDb = detection.attackDb;
    candidate.decayMs = 18.f;  // measured over the tail at the end of calibration instead
    m_candidates.push_back(candidate);
    m_calibrationClicks.store(static_cast<int>(m_candidates.size()), std::memory_order_relaxed);
    if (!m_candidates.empty()) {
        // Live confidence while the user clicks: how alike the samples already are.
        ClickFingerprint probe = buildFingerprint(m_candidates, m_detector.floorDb(), m_detector.floorBroadDb(), kSampleRate);
        m_calibrationConfidence.store(probe.clicks >= 3 ? probe.consistency : 0.f, std::memory_order_relaxed);
    }
}

void MicEngine::finishCalibration() {
    ClickFingerprint fingerprint = buildFingerprint(m_candidates, m_detector.floorDb(), m_detector.floorBroadDb(), kSampleRate);
    m_detector.setCalibrationMode(false);
    if (fingerprint.valid) {
        m_detector.setFingerprint(fingerprint);
        {
            std::lock_guard<std::mutex> guard(m_fingerprintMutex);
            m_fingerprint = fingerprint;
            m_fingerprintDirty = true;
        }
        m_calibrationPhase.store(3, std::memory_order_release);
        setMessage("Click fingerprint learned");
    }
    else {
        m_calibrationPhase.store(4, std::memory_order_release);
        setMessage("Not enough distinct clicks: try again, a little louder");
    }
    m_calibrationConfidence.store(fingerprint.consistency, std::memory_order_relaxed);
    m_calibrationProgress.store(1.f, std::memory_order_relaxed);
    m_calibrating.store(false, std::memory_order_release);
    publish();
}

void MicEngine::tryMatchPending(double nowMs) {
    for (auto it = m_pendingClicks.begin(); it != m_pendingClicks.end();) {
        auto match = m_deliveries.end();
        for (auto delivery = m_deliveries.begin(); delivery != m_deliveries.end(); ++delivery) {
            if (delivery->down != it->down) {
                continue;
            }
            double deltaMs = (delivery->timeNs - it->physicalNs) / 1.0e6;
            if (deltaMs < -3.0 || deltaMs > kDeliveryWindowMs) {
                continue;
            }
            match = delivery;
            break;
        }
        if (match != m_deliveries.end()) {
            scheduleClick(it->down, match->timeNs, it->onsetFrame, nowMs);
            m_deliveries.erase(match);
            it = m_pendingClicks.erase(it);
            continue;
        }
        if (nowMs - it->createdMs > kDeliveryWindowMs) {
            // The buffer dropped or never delivered this input: keep the sound where the
            // player made it rather than losing the click.
            scheduleClick(it->down, it->physicalNs, it->onsetFrame, nowMs);
            ++m_statUnmatched;
            it = m_pendingClicks.erase(it);
            continue;
        }
        ++it;
    }

    while (!m_deliveries.empty() && nowMs - m_deliveries.front().timeMs > 800.0) {
        m_deliveries.pop_front();
    }
}

int64_t MicEngine::minAudibleLeadNs() const {
    // One full device buffer (periods x period, 20 ms with the 10 ms / 2 period setup) plus a
    // couple of milliseconds of slack: a click written into the buffer must land in audio the
    // device has not started playing yet.
    return m_minLeadNs.load(std::memory_order_relaxed);
}

int64_t MicEngine::overrideNs() const {
    return static_cast<int64_t>(msToNs(m_delayOverrideMs.load(std::memory_order_relaxed)));
}

int64_t MicEngine::leadNs() const {
    // The pipeline estimate already covers capture + analysis + a render period of slack,
    // so every click gets the same small head start and they stay evenly spaced.
    return static_cast<int64_t>(msToNs(m_leadEmaMs));
}


void MicEngine::scheduleClick(bool down, int64_t targetNs, uint64_t onsetFrame, double nowMs) {
    (void)down;
    EngineConfig cfg;
    {
        std::lock_guard<std::mutex> guard(m_configMutex);
        cfg = m_config;
    }

    int64_t now = nowNs();
    double observedMs = static_cast<double>(now - targetNs) / 1.0e6;
    double neededMs = observedMs + static_cast<double>(minAudibleLeadNs()) / 1.0e6 + 2.0;
    if (neededMs > m_leadEmaMs) {
        m_leadEmaMs = neededMs;
    }
    else {
        m_leadEmaMs = std::max(neededMs, m_leadEmaMs * 0.999);
    }
    m_leadEmaMs = std::clamp(m_leadEmaMs, 8.0, 150.0);

    int64_t want = targetNs + overrideNs() + leadNs();
    int64_t earliest = now + minAudibleLeadNs();
    int64_t audible = std::max(want, earliest);
    if (earliest > want) {
        ++m_statLate;
    }

    size_t pre = static_cast<size_t>(cfg.preRollMs * 0.001f * static_cast<float>(kSampleRate));
    uint64_t first = onsetFrame > pre ? onsetFrame - pre : 0;
    size_t onsetOffset = static_cast<size_t>(onsetFrame - first);
    size_t tail = static_cast<size_t>(cfg.tailMs * 0.001f * static_cast<float>(kSampleRate));
    size_t length = std::min(onsetOffset + tail, static_cast<size_t>(kMaxSliceFrames));
    // The click is played back a few milliseconds after it was recognised, so the end of the
    // tail may not have been captured yet: keep whatever exists and let the slicer fade it.
    uint64_t written = m_ring.written();
    if (first < written) {
        length = std::min(length, static_cast<size_t>(written - first));
    }
    else {
        length = 0;
    }
    if (length <= onsetOffset) {
        ++m_statDropped;
        return;
    }
    if (!m_ring.readHistory(first, m_sliceScratch.data(), length)) {
        ++m_statDropped;
        return;
    }
    length = groomClickSlice(m_sliceScratch.data(), length, kSampleRate).length;
    if (length == 0) {
        ++m_statDropped;
        return;
    }

    uint64_t baseFrame = m_renderFrame.load(std::memory_order_acquire);
    int64_t baseTime = m_renderTimeNs.load(std::memory_order_acquire);
    double framesAhead = static_cast<double>(audible - baseTime) * 1.0e-9 * static_cast<double>(kSampleRate);
    int64_t startFrame = static_cast<int64_t>(baseFrame) + static_cast<int64_t>(std::llround(framesAhead));
    startFrame -= static_cast<int64_t>(onsetOffset);
    if (startFrame < static_cast<int64_t>(baseFrame)) {
        startFrame = static_cast<int64_t>(baseFrame);
    }

    ClickSlice& slice = m_sliceMessage;
    slice.startFrame = static_cast<uint64_t>(startFrame);
    slice.length = static_cast<uint32_t>(length);
    slice.onsetOffset = static_cast<uint32_t>(onsetOffset);
    std::memcpy(slice.samples.data(), m_sliceScratch.data(), length * sizeof(float));
    if (!m_clickQueue.push(slice)) {
        ++m_statDropped;
        return;
    }
    ++m_statScheduled;
    (void)nowMs;
}

void MicEngine::publish() {
    EngineStatus next;
    next.running = m_running.load(std::memory_order_acquire);
    next.backendReady = backendAvailable();
    next.calibrating = m_calibrating.load(std::memory_order_acquire);
    next.calibrationPhase = m_calibrationPhase.load(std::memory_order_acquire);
    next.calibrationProgress = m_calibrationProgress.load(std::memory_order_relaxed);
    next.calibrationConfidence = m_calibrationConfidence.load(std::memory_order_relaxed);
    next.calibrationClicks = m_calibrationClicks.load(std::memory_order_relaxed);
    next.inputLevelDb = m_detector.lastBroadDb();
    next.inputPeakDb = m_capturePeakDb.load(std::memory_order_relaxed);
    next.noiseFloorDb = m_detector.floorBroadDb();
    next.micDelayMs = static_cast<float>(m_lock.delayMs());
    next.delayOverrideMs = m_delayOverrideMs.load(std::memory_order_relaxed);
    next.jitterMs = static_cast<float>(m_lock.jitterMs());
    next.driftMsPerMinute = static_cast<float>(m_lock.driftMsPerMinute());
    next.confidence = static_cast<float>(m_lock.confidence());
    next.windowPairs = static_cast<int>(m_lock.count());
    DetectorStats const& stats = m_detector.stats();
    next.accepted = static_cast<int>(stats.accepted);
    next.detections = static_cast<int>(m_statDetections);
    next.rejectedGate = static_cast<int>(stats.rejectedGate);
    next.rejectedShape = static_cast<int>(stats.rejectedShape);
    next.rejectedRefractory = static_cast<int>(stats.rejectedRefractory);
    next.rejectedSustained = static_cast<int>(stats.rejectedSustained);
    next.naiveMissed = static_cast<int>(stats.naiveMissed);
    next.scheduled = static_cast<int>(m_statScheduled);
    next.lateClicks = static_cast<int>(m_statLate);
    next.droppedClicks = static_cast<int>(m_statDropped);
    next.unmatched = static_cast<int>(m_statUnmatched);
    next.queueDepth = static_cast<int>(m_clickQueue.size());
    next.voices = m_voiceCount.load(std::memory_order_relaxed);
    next.voiceSteals = m_voiceSteals.load(std::memory_order_relaxed);
    next.monitorUnderruns = static_cast<int>(m_monitorJitter.underruns());
    next.monitorTrims = static_cast<int>(m_monitorJitter.trims());
    {
        uint32_t rate = m_renderSampleRate.load(std::memory_order_relaxed);
        if (rate == 0) {
            rate = static_cast<uint32_t>(kSampleRate);
        }
        next.monitorFillMs = static_cast<int>(m_monitorJitter.fillFrames() * 1000 / rate);
        next.renderSampleRate = static_cast<int>(rate);
        next.renderPeriodFrames = static_cast<int>(m_renderPeriodFrames.load(std::memory_order_relaxed));
    }
    {
        std::lock_guard<std::mutex> guard(m_fingerprintMutex);
        next.calibrated = m_fingerprint.valid;
        next.fingerprintReady = m_fingerprint.valid;
    }
    copyName(next.captureDevice, sizeof(next.captureDevice), m_captureName);
    copyName(next.renderDevice, sizeof(next.renderDevice), m_renderName);
    copyName(next.backend, sizeof(next.backend), backendName());

    std::lock_guard<std::mutex> guard(m_statusMutex);
    std::string message = m_status.message;
    copyName(next.message, sizeof(next.message), message);
    m_status = next;
}

} // namespace micclicks
