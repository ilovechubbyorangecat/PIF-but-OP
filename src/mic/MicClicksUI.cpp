#include "MicClicksUI.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "MicClicks.hpp"

using namespace geode::prelude;

namespace micclicks {

namespace {

constexpr float W = 440.f;
constexpr float H = 316.f;
constexpr ccColor3B kText { 236, 238, 255 };
constexpr ccColor3B kMuted { 168, 172, 190 };
constexpr ccColor3B kAccent { 120, 200, 255 };
constexpr ccColor3B kWarn { 255, 150, 120 };
constexpr ccColor3B kGood { 140, 235, 160 };

char const* kBodyFont = "chatFont.fnt";
char const* kHeadFont = "bigFont.fnt";

CCLabelBMFont* makeLabel(CCNode* parent, std::string const& text, float scale, ccColor3B colour, CCPoint position,
    CCPoint anchor = { 0.f, 0.5f }, char const* font = kBodyFont) {
    auto* label = CCLabelBMFont::create(text.c_str(), font);
    if (!label) {
        return nullptr;
    }
    label->setScale(scale);
    label->setColor(colour);
    label->setAnchorPoint(anchor);
    label->setPosition(position);
    parent->addChild(label, 3);
    return label;
}

void setLabel(CCLabelBMFont* label, std::string const& text) {
    if (label) {
        label->setString(text.c_str());
    }
}

// A horizontal bar drawn with a thick segment: keeps the panel free of sprite dependencies.
void drawBar(CCDrawNode* node, float width, float height, float fill, ccColor4F colour) {
    if (!node) {
        return;
    }
    node->clear();
    node->drawSegment({ 0.f, 0.f }, { width, 0.f }, height, { 0.f, 0.f, 0.f, 0.45f });
    float clamped = std::clamp(fill, 0.f, 1.f);
    if (clamped > 0.001f) {
        node->drawSegment({ 1.f, 0.f }, { 1.f + (width - 2.f) * clamped, 0.f }, height - 2.f, colour);
    }
}

int indexOf(std::vector<audio::DeviceInfo> const& devices, std::string const& name) {
    if (name.empty()) {
        return -1;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string deviceName(std::vector<audio::DeviceInfo> const& devices, std::string const& selected) {
    if (selected.empty()) {
        return "System default";
    }
    if (indexOf(devices, selected) < 0) {
        return selected + " (not found)";
    }
    return selected;
}

float levelToFill(float db) {
    // -70 dB (nothing) to -6 dB (clipping): the meter's useful range for a microphone.
    return std::clamp((db + 70.f) / 64.f, 0.f, 1.f);
}

} // namespace

MicSetupPopup::~MicSetupPopup() {
    if (s_open == this) {
        s_open = nullptr;
    }
}

MicSetupPopup* MicSetupPopup::create() {
    auto* popup = new MicSetupPopup();
    if (popup->init()) {
        popup->autorelease();
        return popup;
    }
    delete popup;
    return nullptr;
}

void MicSetupPopup::open() {
    if (s_open) {
        s_open->keyBackClicked();
        return;
    }
    if (auto* popup = create()) {
        popup->show();
    }
}

bool MicSetupPopup::init() {
    if (!Popup::init(W, H, "GJ_square01.png")) {
        return false;
    }
    s_open = this;
    buildRows();
    refresh();
    this->scheduleUpdate();
    return true;
}

void MicSetupPopup::buildRows() {
    auto* menu = CCMenu::create();
    menu->setPosition({ 0.f, 0.f });
    m_mainLayer->addChild(menu, 5);

    auto button = [&](char const* text, CCPoint at, SEL_MenuHandler handler, float scale, CCLabelBMFont** keep) {
        auto* label = CCLabelBMFont::create(text, kBodyFont);
        label->setScale(scale);
        auto* item = CCMenuItemLabel::create(label, this, handler);
        item->setPosition(at);
        menu->addChild(item);
        if (keep) {
            *keep = label;
        }
        return item;
    };

    float left = 16.f;
    float right = W - 16.f;

    makeLabel(m_mainLayer, "MIC CLICKS", 0.62f, kText, { left, H - 36.f }, { 0.f, 0.5f }, kHeadFont);
    m_status = makeLabel(m_mainLayer, "", 0.38f, kMuted, { left, H - 56.f });
    if (m_status) {
        m_status->limitLabelWidth(W - 32.f, 0.38f, 0.2f);
    }

    // --- microphone ---------------------------------------------------------------
    makeLabel(m_mainLayer, "MICROPHONE", 0.34f, kAccent, { left, H - 84.f });
    button("<", { left + 8.f, H - 106.f }, menu_selector(MicSetupPopup::onCapturePrev), 0.8f, nullptr);
    button(">", { right - 8.f, H - 106.f }, menu_selector(MicSetupPopup::onCaptureNext), 0.8f, nullptr);
    m_captureLabel = makeLabel(m_mainLayer, "", 0.45f, kText, { W / 2.f, H - 106.f }, { 0.5f, 0.5f });
    button("Refresh devices", { W / 2.f, H - 128.f }, menu_selector(MicSetupPopup::onRefreshDevices), 0.38f, nullptr);

    // --- calibration --------------------------------------------------------------
    makeLabel(m_mainLayer, "CLICK FINGERPRINT", 0.34f, kAccent, { left, H - 152.f });
    button("Calibrate", { left + 52.f, H - 172.f }, menu_selector(MicSetupPopup::onCalibrate), 0.42f, &m_calibrate);
    button("Cancel", { left + 130.f, H - 172.f }, menu_selector(MicSetupPopup::onCancelCalibration), 0.38f, nullptr);
    button("Clear", { left + 196.f, H - 172.f }, menu_selector(MicSetupPopup::onClearFingerprint), 0.38f, nullptr);
    m_calibration = makeLabel(m_mainLayer, "", 0.36f, kMuted, { right, H - 172.f }, { 1.f, 0.5f });

    m_meter = CCDrawNode::create();
    m_meter->setPosition({ left, H - 190.f });
    m_mainLayer->addChild(m_meter, 2);
    m_progress = CCDrawNode::create();
    m_progress->setPosition({ left, H - 198.f });
    m_mainLayer->addChild(m_progress, 2);

    // --- mic delay ----------------------------------------------------------------
    makeLabel(m_mainLayer, "MIC DELAY", 0.34f, kAccent, { left, H - 220.f });
    button("<", { right - 172.f, H - 220.f }, menu_selector(MicSetupPopup::onDelayDown), 0.6f, nullptr);
    button(">", { right - 16.f, H - 220.f }, menu_selector(MicSetupPopup::onDelayUp), 0.6f, nullptr);
    m_delay = makeLabel(m_mainLayer, "", 0.4f, kText, { right - 94.f, H - 220.f }, { 0.5f, 0.5f });
    if (m_delay) {
        m_delay->limitLabelWidth(140.f, 0.4f, 0.2f);
    }
    button("reset", { right - 94.f, H - 238.f }, menu_selector(MicSetupPopup::onDelayReset), 0.34f, nullptr);

    // --- monitoring ---------------------------------------------------------------
    makeLabel(m_mainLayer, "HEAR YOURSELF", 0.34f, kAccent, { left, H - 254.f });
    button("Off", { left + 46.f, H - 254.f }, menu_selector(MicSetupPopup::onMonitorToggle), 0.4f, &m_monitor);
    button("Clicks Only", { left + 148.f, H - 254.f }, menu_selector(MicSetupPopup::onMonitorMode), 0.36f, &m_mode);

    // --- output / routing ---------------------------------------------------------
    makeLabel(m_mainLayer, "OUTPUT (VB-CABLE FOR OBS / DISCORD)", 0.34f, kAccent, { left, H - 280.f });
    button("<", { left + 8.f, H - 298.f }, menu_selector(MicSetupPopup::onRenderPrev), 0.8f, nullptr);
    button(">", { right - 8.f, H - 298.f }, menu_selector(MicSetupPopup::onRenderNext), 0.8f, nullptr);
    m_renderLabel = makeLabel(m_mainLayer, "", 0.42f, kText, { W / 2.f, H - 298.f }, { 0.5f, 0.5f });
    m_virtualLabel = makeLabel(m_mainLayer, "", 0.32f, kGood, { W / 2.f + 150.f, H - 84.f }, { 1.f, 0.5f });

    // --- bottom row ---------------------------------------------------------------
    button("Enable", { left + 46.f, 14.f }, menu_selector(MicSetupPopup::onToggleEnabled), 0.42f, &m_enable);
    button("Click alignment: game tick", { W / 2.f, 14.f }, menu_selector(MicSetupPopup::onAlignMode), 0.36f, &m_align);
    if (m_align) {
        m_align->setColor(kMuted);
    }
    m_stats = makeLabel(m_mainLayer, "", 0.32f, kMuted, { right, 26.f }, { 1.f, 0.5f });
}

void MicSetupPopup::refresh() {
    auto& clicks = MicClicks::get();
    auto& engine = clicks.engine();
    EngineStatus status = engine.status();
    EngineConfig config = clicks.config();

    if (!engine.backendAvailable()) {
        setLabel(m_status, "No audio backend in this build: " + engine.backendReason());
    }
    else if (!status.running) {
        setLabel(m_status, clicks.enabled() ? "Engine stopped: " + engine.backendName() : "Mic Clicks is off (Enable below)");
    }
    else {
        int rate = status.renderSampleRate > 0 ? status.renderSampleRate : audio::kBackendSampleRate;
        int periodMs = (status.renderPeriodFrames > 0 && rate > 0) ? (status.renderPeriodFrames * 1000 + rate / 2) / rate
                                                                   : static_cast<int>(audio::kDefaultPeriodMs);
        setLabel(m_status, fmt::format("{}  |  {} kHz / {} ms  |  mic: {}  |  out: {}", engine.backendName(),
                            static_cast<double>(rate) / 1000.0, periodMs, status.captureDevice, status.renderDevice));
    }

    setLabel(m_captureLabel, deviceName(clicks.captureDevices(), config.captureName));
    setLabel(m_renderLabel, deviceName(clicks.renderDevices(), config.renderName));
    bool renderVirtual = audio::looksVirtual(config.renderName);
    setLabel(m_virtualLabel, renderVirtual ? "virtual cable" : "");

    setLabel(m_enable, clicks.enabled() ? "Disable" : "Enable");
    if (m_enable) {
        m_enable->setColor(clicks.enabled() ? kGood : kText);
    }

    setLabel(m_monitor, config.monitor ? "On" : "Off");
    if (m_monitor) {
        m_monitor->setColor(config.monitor ? kGood : kText);
    }
    setLabel(m_mode, config.monitorMode == static_cast<int>(MonitorMode::Everything) ? "Everything" : "Clicks Only");
    setLabel(m_align, config.alignMode == static_cast<int>(AlignMode::PhysicalClick) ? "Click alignment: physical" : "Click alignment: game tick");

    if (status.calibrating) {
        std::string phase = status.calibrationPhase == 1 ? "Stay quiet for a moment..." : "Click 8 times, about twice a second";
        setLabel(m_calibration, fmt::format("{}  {}%", phase, static_cast<int>(status.calibrationProgress * 100.f)));
        if (m_calibration) {
            m_calibration->setColor(kAccent);
        }
        drawBar(m_progress, W - 32.f, 4.f, status.calibrationProgress, { 0.47f, 0.78f, 1.f, 0.95f });
    }
    else if (status.calibrationPhase == 3) {
        setLabel(m_calibration, fmt::format("learned from {} clicks ({}% match)", status.calibrationClicks,
                                   static_cast<int>(status.calibrationConfidence * 100.f)));
        if (m_calibration) {
            m_calibration->setColor(kGood);
        }
        drawBar(m_progress, W - 32.f, 4.f, 0.f, { 0.f, 0.f, 0.f, 0.f });
    }
    else if (status.calibrationPhase == 4) {
        setLabel(m_calibration, "not enough distinct clicks: try again");
        if (m_calibration) {
            m_calibration->setColor(kWarn);
        }
        drawBar(m_progress, W - 32.f, 4.f, 0.f, { 0.f, 0.f, 0.f, 0.f });
    }
    else if (clicks.hasFingerprint()) {
        setLabel(m_calibration, "fingerprint loaded: click sounds are matched, not just loud");
        if (m_calibration) {
            m_calibration->setColor(kMuted);
        }
        drawBar(m_progress, W - 32.f, 4.f, 0.f, { 0.f, 0.f, 0.f, 0.f });
    }
    else {
        setLabel(m_calibration, "not calibrated: falling back to a loudness gate");
        if (m_calibration) {
            m_calibration->setColor(kWarn);
        }
        drawBar(m_progress, W - 32.f, 4.f, 0.f, { 0.f, 0.f, 0.f, 0.f });
    }

    if (status.windowPairs > 0) {
        setLabel(m_delay, fmt::format("auto {:.0f} ms  +{:.0f} ms", static_cast<double>(status.micDelayMs),
                            static_cast<double>(config.delayOverrideMs)));
    }
    else {
        setLabel(m_delay, fmt::format("auto -- ms  +{:.0f} ms", static_cast<double>(config.delayOverrideMs)));
    }

    // Level meter with the noise floor marked.
    if (m_meter) {
        m_meter->clear();
        float width = W - 32.f;
        float height = 6.f;
        m_meter->drawSegment({ 0.f, 0.f }, { width, 0.f }, height, { 0.f, 0.f, 0.f, 0.5f });
        float fill = levelToFill(status.inputLevelDb);
        if (fill > 0.002f) {
            ccColor4F colour = fill > 0.92f ? ccColor4F { 1.f, 0.45f, 0.35f, 0.95f } : ccColor4F { 0.5f, 0.9f, 0.6f, 0.95f };
            m_meter->drawSegment({ 1.f, 0.f }, { 1.f + (width - 2.f) * fill, 0.f }, height - 2.f, colour);
        }
        float peak = levelToFill(status.inputPeakDb);
        if (peak > 0.002f) {
            float x = 1.f + (width - 2.f) * peak;
            m_meter->drawSegment({ x, -2.f }, { x, 2.f }, 1.5f, { 1.f, 1.f, 1.f, 0.8f });
        }
        float floor_x = 1.f + (width - 2.f) * levelToFill(status.noiseFloorDb);
        m_meter->drawSegment({ floor_x, -3.f }, { floor_x, 3.f }, 1.f, { 1.f, 0.85f, 0.3f, 0.9f });
    }

    // "jit" is the capture->monitor jitter buffer: how much audio is queued, how often it ran
    // dry and how many frames were trimmed to keep the drift from growing the latency.
    setLabel(m_stats, fmt::format("clicks {}  paired {}  sched {}  late {}  drop {}  voice x{}  jit {}ms u{} t{}",
                        status.accepted, status.windowPairs, status.scheduled, status.lateClicks, status.droppedClicks,
                        status.rejectedSustained, status.monitorFillMs, status.monitorUnderruns, status.monitorTrims));

    if (m_calibrate) {
        m_calibrate->setColor(status.calibrating ? kMuted : kText);
    }
}

void MicSetupPopup::update(float dt) {
    m_refreshTimer += dt;
    if (m_refreshTimer >= 0.1f) {
        m_refreshTimer = 0.f;
        refresh();
    }
}

void MicSetupPopup::onToggleEnabled(CCObject*) {
    auto& clicks = MicClicks::get();
    clicks.setEnabled(!clicks.enabled());
    refresh();
}

void MicSetupPopup::onCalibrate(CCObject*) {
    auto& clicks = MicClicks::get();
    if (!clicks.enabled()) {
        clicks.setEnabled(true);
    }
    if (!clicks.engine().running()) {
        if (!clicks.lastMessage().empty()) {
            setLabel(m_status, clicks.lastMessage());
        }
        refresh();
        return;
    }
    clicks.engine().startCalibration();
    refresh();
}

void MicSetupPopup::onCancelCalibration(CCObject*) {
    MicClicks::get().engine().cancelCalibration();
    refresh();
}

void MicSetupPopup::onClearFingerprint(CCObject*) {
    MicClicks::get().clearFingerprint();
    refresh();
}

void MicSetupPopup::stepCapture(int direction) {
    auto& clicks = MicClicks::get();
    auto const& devices = clicks.captureDevices();
    if (devices.empty()) {
        return;
    }
    EngineConfig config = clicks.config();
    int index = indexOf(devices, config.captureName);
    int count = static_cast<int>(devices.size());
    index += direction;
    if (index < 0) {
        config.captureName.clear();  // system default first, then the list
    }
    else if (index >= count) {
        index = count - 1;
        config.captureName = devices[static_cast<size_t>(index)].name;
    }
    else {
        config.captureName = devices[static_cast<size_t>(index)].name;
    }
    clicks.setConfig(config);
    refresh();
}

void MicSetupPopup::stepRender(int direction) {
    auto& clicks = MicClicks::get();
    auto const& devices = clicks.renderDevices();
    if (devices.empty()) {
        return;
    }
    EngineConfig config = clicks.config();
    int index = indexOf(devices, config.renderName);
    int count = static_cast<int>(devices.size());
    index += direction;
    if (index < 0) {
        config.renderName.clear();
    }
    else {
        index = std::min(index, count - 1);
        config.renderName = devices[static_cast<size_t>(index)].name;
    }
    clicks.setConfig(config);
    refresh();
}

void MicSetupPopup::onCapturePrev(CCObject*) {
    stepCapture(-1);
}
void MicSetupPopup::onCaptureNext(CCObject*) {
    stepCapture(1);
}
void MicSetupPopup::onRenderPrev(CCObject*) {
    stepRender(-1);
}
void MicSetupPopup::onRenderNext(CCObject*) {
    stepRender(1);
}

void MicSetupPopup::onRefreshDevices(CCObject*) {
    MicClicks::get().refreshDevices();
    refresh();
}

void MicSetupPopup::onMonitorToggle(CCObject*) {
    auto& clicks = MicClicks::get();
    EngineConfig config = clicks.config();
    config.monitor = !config.monitor;
    clicks.setConfig(config);
    refresh();
}

void MicSetupPopup::onMonitorMode(CCObject*) {
    auto& clicks = MicClicks::get();
    EngineConfig config = clicks.config();
    config.monitorMode = config.monitorMode == static_cast<int>(MonitorMode::Everything) ? static_cast<int>(MonitorMode::ClicksOnly)
                                                                                        : static_cast<int>(MonitorMode::Everything);
    config.monitor = true;  // choosing a mode implies you want to hear it
    clicks.setConfig(config);
    refresh();
}

void MicSetupPopup::setDelayOffset(float value) {
    auto& clicks = MicClicks::get();
    EngineConfig config = clicks.config();
    config.delayOverrideMs = std::clamp(value, -60.f, 60.f);
    clicks.setConfig(config);
    refresh();
}

void MicSetupPopup::onDelayDown(CCObject*) {
    setDelayOffset(MicClicks::get().config().delayOverrideMs - 1.f);
}

void MicSetupPopup::onDelayUp(CCObject*) {
    setDelayOffset(MicClicks::get().config().delayOverrideMs + 1.f);
}

void MicSetupPopup::onDelayReset(CCObject*) {
    setDelayOffset(0.f);
}

void MicSetupPopup::onAlignMode(CCObject*) {
    auto& clicks = MicClicks::get();
    EngineConfig config = clicks.config();
    config.alignMode = config.alignMode == static_cast<int>(AlignMode::GameTick) ? static_cast<int>(AlignMode::PhysicalClick)
                                                                                : static_cast<int>(AlignMode::GameTick);
    clicks.setConfig(config);
    refresh();
}

} // namespace micclicks
