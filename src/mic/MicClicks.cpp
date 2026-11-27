#include "MicClicks.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <asp/fs/std.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "MicClicksUI.hpp"

using namespace geode::prelude;

namespace micclicks {

namespace {

std::filesystem::path configDir() {
    return Mod::get()->getConfigDir().toStdPath().unwrapOr(std::filesystem::path());
}

std::filesystem::path fingerprintFile() {
    return configDir() / "mic-clicks-fingerprint.json";
}

template <class T>
T setting(char const* key, T fallback) {
    (void)fallback;
    return Mod::get()->getSettingValue<T>(key);
}

// The settings store a device by name; "System default" means "let the OS pick".
constexpr char const* kDefaultDevice = "System default";

std::string readDevice(char const* key) {
    std::string name = setting<std::string>(key, kDefaultDevice);
    return name == kDefaultDevice ? std::string() : name;
}

void writeDevice(char const* key, std::string const& name) {
    Mod::get()->setSettingValue<std::string>(key, name.empty() ? std::string(kDefaultDevice) : name);
}

std::string joinValues(std::array<float, kBandCount> const& values) {
    std::string out;
    out.reserve(values.size() * 9);
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) {
            out.push_back(' ');
        }
        out += fmt::format("{:.4f}", values[i]);
    }
    return out;
}

bool parseValues(std::string const& text, std::array<float, kBandCount>& out) {
    std::istringstream stream(text);
    for (size_t i = 0; i < out.size(); ++i) {
        if (!(stream >> out[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

MicClicks& MicClicks::get() {
    static MicClicks instance;
    return instance;
}

void MicClicks::init() {
    if (m_initialised) {
        return;
    }
    m_initialised = true;
    m_config = buildConfig();
    m_snapToTick.store(Mod::get()->getSettingValue<bool>("mic-snap-tick"));
    loadFingerprint();
    m_enabled = Mod::get()->getSettingValue<bool>("mic-clicks");
    if (m_enabled) {
        std::string error;
        startEngine(error);
        if (!error.empty()) {
            log::warn("Mic Clicks: {}", error);
            m_message = error;
        }
    }
    // Device enumeration spins up miniaudio's context, so it happens when the engine starts or
    // when the setup panel opens; the module load path stays free of audio work.
    log::info("Mic Clicks: {} (backend {}, {} click fingerprint)", m_enabled ? "enabled" : "off", m_engine.backendName(),
        m_engine.hasFingerprint() ? "with a" : "no");
}

void MicClicks::shutdown() {
    m_enabled = false;
    stopEngine();
}

EngineConfig MicClicks::buildConfig() const {
    EngineConfig config;
    config.captureName = readDevice("mic-device");
    config.renderName = readDevice("mic-output");
    config.monitor = setting<bool>("mic-monitor", false);
    config.monitorMode = setting<std::string>("mic-monitor-mode", "Clicks Only") == "Everything" ? static_cast<int>(MonitorMode::Everything)
                                                                                               : static_cast<int>(MonitorMode::ClicksOnly);
    config.monitorGain = static_cast<float>(setting<double>("mic-monitor-gain", 0.8));
    config.clickGain = static_cast<float>(setting<double>("mic-click-gain", 1.0));
    config.sensitivity = static_cast<float>(setting<double>("mic-sensitivity", 1.0));
    config.delayOverrideMs = static_cast<float>(setting<double>("mic-delay-offset", 0.0));
    config.alignMode = setting<std::string>("mic-align-mode", "Game tick") == "Physical click" ? static_cast<int>(AlignMode::PhysicalClick)
                                                                                              : static_cast<int>(AlignMode::GameTick);
    return config;
}

void MicClicks::reloadSettings(bool force) {
    if (!m_initialised) {
        return;
    }
    bool enabled = Mod::get()->getSettingValue<bool>("mic-clicks");
    EngineConfig config = buildConfig();
    m_snapToTick.store(Mod::get()->getSettingValue<bool>("mic-snap-tick"));

    bool configChanged = force || config.captureName != m_config.captureName || config.renderName != m_config.renderName ||
        config.monitor != m_config.monitor || config.monitorMode != m_config.monitorMode || config.monitorGain != m_config.monitorGain ||
        config.clickGain != m_config.clickGain || config.sensitivity != m_config.sensitivity ||
        config.delayOverrideMs != m_config.delayOverrideMs || config.alignMode != m_config.alignMode;
    m_config = config;

    if (enabled != m_enabled) {
        m_enabled = enabled;
        if (m_enabled) {
            std::string error;
            startEngine(error);
            if (!error.empty()) {
                m_message = error;
            }
        }
        else {
            stopEngine();
        }
        return;
    }
    if (m_enabled && configChanged) {
        std::string error;
        m_engine.applyConfig(m_config, &error);
        if (!error.empty()) {
            m_message = error;
            m_enabled = false;
        }
    }
}

void MicClicks::startEngine(std::string& error) {
    error.clear();
    if (m_engine.running()) {
        return;
    }
    refreshDevices();
    if (!m_engine.start(m_config, error)) {
        return;
    }
    loadFingerprint();  // the engine keeps the fingerprint across restarts, but be explicit
    m_message = "Mic Clicks running";
}

void MicClicks::stopEngine() {
    if (m_engine.running()) {
        m_engine.stop();
        m_message = "Mic Clicks stopped";
    }
}

void MicClicks::setEnabled(bool on) {
    Mod::get()->setSettingValue<bool>("mic-clicks", on);
    reloadSettings(true);
}

void MicClicks::setConfig(EngineConfig const& config) {
    m_config = config;
    persistConfig();
    if (m_enabled && m_engine.running()) {
        std::string error;
        m_engine.applyConfig(m_config, &error);
        if (!error.empty()) {
            m_message = error;
            log::warn("Mic Clicks: {}", error);
        }
    }
}

void MicClicks::refreshDevices() {
    std::string error;
    m_captures = m_engine.captureDevices(&error);
    if (!error.empty()) {
        m_deviceError = error;
    }
    m_renders = m_engine.renderDevices(&error);
    if (!error.empty()) {
        m_deviceError = error;
    }
    if (!error.empty() && m_captures.empty()) {
        m_message = error;
    }
}

void MicClicks::persistConfig() {
    writeDevice("mic-device", m_config.captureName);
    writeDevice("mic-output", m_config.renderName);
    Mod::get()->setSettingValue<bool>("mic-monitor", m_config.monitor);
    Mod::get()->setSettingValue<std::string>("mic-monitor-mode",
        m_config.monitorMode == static_cast<int>(MonitorMode::Everything) ? "Everything" : "Clicks Only");
    Mod::get()->setSettingValue<double>("mic-monitor-gain", static_cast<double>(m_config.monitorGain));
    Mod::get()->setSettingValue<double>("mic-click-gain", static_cast<double>(m_config.clickGain));
    Mod::get()->setSettingValue<double>("mic-sensitivity", static_cast<double>(m_config.sensitivity));
    Mod::get()->setSettingValue<double>("mic-delay-offset", static_cast<double>(m_config.delayOverrideMs));
    Mod::get()->setSettingValue<std::string>("mic-align-mode",
        m_config.alignMode == static_cast<int>(AlignMode::PhysicalClick) ? "Physical click" : "Game tick");
}

int64_t MicClicks::snapToTick(int64_t timeNs) const {
    if (!m_snapToTick.load(std::memory_order_relaxed)) {
        return timeNs;
    }
    int64_t period = m_tickPeriodNs.load(std::memory_order_relaxed);
    if (period <= 0) {
        return timeNs;
    }
    int64_t start = m_tickStartNs.load(std::memory_order_relaxed);
    if (start <= 0) {
        return timeNs;
    }
    int64_t delta = timeNs - start;
    int64_t ticks = delta >= 0 ? (delta + period / 2) / period : (delta - period / 2) / period;
    // The tick clock is at most a frame old; never snap an input further than that.
    ticks = std::clamp<int64_t>(ticks, -2, 2);
    return start + ticks * period;
}

void MicClicks::onGameTick(int64_t tick, double dt) {
    int64_t now = nowNs();
    m_tick.store(tick, std::memory_order_relaxed);
    m_tickStartNs.store(now, std::memory_order_relaxed);
    m_tickPeriodNs.store(dt > 0.0 ? static_cast<int64_t>(dt * 1.0e9) : 0, std::memory_order_relaxed);

    if (now - m_lastPollNs < 1'000'000'000) {
        return;
    }
    m_lastPollNs = now;
    reloadSettings();
    pollCalibration();
}

void MicClicks::onUserInput(bool down) {
    if (!m_enabled || !m_engine.running()) {
        return;
    }
    m_engine.noteInput(down, false, snapToTick(nowNs()), m_tick.load(std::memory_order_relaxed));
}

void MicClicks::onDeliveredInput(bool down) {
    if (!m_enabled || !m_engine.running()) {
        return;
    }
    m_engine.noteInput(down, true, snapToTick(nowNs()), m_tick.load(std::memory_order_relaxed));
}

void MicClicks::onLevelStart() {
    if (m_enabled) {
        m_engine.flush();
    }
}

void MicClicks::onLevelEnd() {
    if (m_enabled) {
        m_engine.flush();
        pollCalibration();
    }
}

void MicClicks::pollCalibration() {
    ClickFingerprint fingerprint;
    if (m_engine.takeFingerprint(fingerprint)) {
        saveFingerprint(fingerprint);
        m_message = fmt::format("Click fingerprint saved ({} clicks, {}% match)", fingerprint.clicks,
            static_cast<int>(fingerprint.consistency * 100.f));
        log::info("Mic Clicks: {}", m_message);
    }
}

void MicClicks::saveFingerprint(ClickFingerprint const& fingerprint) {
    std::error_code ec;
    std::filesystem::create_directories(configDir(), ec);
    matjson::Value value = matjson::Value::object();
    value["version"] = 1;
    value["valid"] = fingerprint.valid;
    value["sampleRate"] = fingerprint.sampleRate;
    value["clicks"] = fingerprint.clicks;
    value["consistency"] = static_cast<double>(fingerprint.consistency);
    value["peakDb"] = static_cast<double>(fingerprint.peakDb);
    value["floorBroadDb"] = static_cast<double>(fingerprint.floorBroadDb);
    value["attackDb"] = static_cast<double>(fingerprint.attackDb);
    value["decayMs"] = static_cast<double>(fingerprint.decayMs);
    value["centroidHz"] = static_cast<double>(fingerprint.centroidHz);
    value["brightness"] = static_cast<double>(fingerprint.brightness);
    value["fluxDb"] = static_cast<double>(fingerprint.fluxDb);
    value["shape"] = joinValues(fingerprint.shape);
    value["floor"] = joinValues(fingerprint.floorDb);

    std::ofstream out(fingerprintFile(), std::ios::trunc);
    if (!out) {
        log::warn("Mic Clicks: could not write {}", fingerprintFile().string());
        return;
    }
    out << value.dump();
}

void MicClicks::loadFingerprint() {
    std::ifstream in(fingerprintFile());
    if (!in) {
        return;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto parsed = matjson::parse(buffer.str());
    if (parsed.isErr()) {
        log::warn("Mic Clicks: damaged fingerprint file");
        return;
    }
    matjson::Value value = parsed.unwrap();
    if (!value.isObject() || !value.contains("shape") || !value.contains("floor")) {
        return;
    }
    ClickFingerprint fingerprint;
    if (!parseValues(value["shape"].asString().unwrapOr(""), fingerprint.shape) ||
        !parseValues(value["floor"].asString().unwrapOr(""), fingerprint.floorDb)) {
        return;
    }
    fingerprint.sampleRate = static_cast<int>(value["sampleRate"].asInt().unwrapOr(kSampleRate));
    fingerprint.clicks = static_cast<int>(value["clicks"].asInt().unwrapOr(0));
    fingerprint.consistency = static_cast<float>(value["consistency"].asDouble().unwrapOr(0.0));
    fingerprint.peakDb = static_cast<float>(value["peakDb"].asDouble().unwrapOr(-30.0));
    fingerprint.floorBroadDb = static_cast<float>(value["floorBroadDb"].asDouble().unwrapOr(-70.0));
    fingerprint.attackDb = static_cast<float>(value["attackDb"].asDouble().unwrapOr(12.0));
    fingerprint.decayMs = static_cast<float>(value["decayMs"].asDouble().unwrapOr(18.0));
    fingerprint.centroidHz = static_cast<float>(value["centroidHz"].asDouble().unwrapOr(3000.0));
    fingerprint.brightness = static_cast<float>(value["brightness"].asDouble().unwrapOr(0.4));
    fingerprint.fluxDb = static_cast<float>(value["fluxDb"].asDouble().unwrapOr(4.0));
    fingerprint.valid = value["valid"].asBool().unwrapOr(true);
    if (!fingerprint.valid) {
        return;
    }
    m_engine.setFingerprint(fingerprint);
    log::info("Mic Clicks: loaded click fingerprint from {}", fingerprintFile().string());
}

void MicClicks::clearFingerprint() {
    std::error_code ec;
    std::filesystem::remove(fingerprintFile(), ec);
    m_engine.setFingerprint(ClickFingerprint {});
    m_message = "Click fingerprint removed";
}

// --- free functions used by the hooks ------------------------------------------------

void init() {
    MicClicks::get().init();
}

void onGameTick(int64_t tick, double dt) {
    MicClicks::get().onGameTick(tick, dt);
}

void onUserInput(bool down) {
    MicClicks::get().onUserInput(down);
}

void onDeliveredInput(bool down) {
    MicClicks::get().onDeliveredInput(down);
}

void onLevelStart() {
    MicClicks::get().onLevelStart();
}

void onLevelEnd() {
    MicClicks::get().onLevelEnd();
}

void openSetup() {
    MicClicks::get().reloadSettings(true);
    MicClicks::get().refreshDevices();
    MicSetupPopup::open();
}

} // namespace micclicks
