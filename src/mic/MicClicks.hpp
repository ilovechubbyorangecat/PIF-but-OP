#pragma once
// Mic Clicks: the game side.
//
// Keeps the Geode/mod settings, the physics tick clock and the fingerprint file out of the
// DSP code, and gives the mod's hooks a tiny, allocation free API:
//
//   micclicks::onGameTick(tick, dt)     every physics step (P1 only)
//   micclicks::onUserInput(down)        the player's physical press/release
//   micclicks::onDeliveredInput(down)   the input as the game finally receives it
//
// The engine turns the pair of timestamps into "which tick did this click belong to" and
// schedules the sliced audio for it.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "AudioBackend.hpp"
#include "MicEngine.hpp"

namespace micclicks {

class MicClicks {
public:
    static MicClicks& get();

    // $on_mod(Loaded)
    void init();
    void shutdown();

    // Re-reads the mod settings; cheap enough to poll once a second.
    void reloadSettings(bool force = false);
    bool enabled() const {
        return m_enabled;
    }

    // --- hooks -------------------------------------------------------------------
    void onGameTick(int64_t tick, double dt);
    void onUserInput(bool down);
    void onDeliveredInput(bool down);
    void onLevelStart();
    void onLevelEnd();

    // --- engine access (UI) ------------------------------------------------------
    MicEngine& engine() {
        return m_engine;
    }
    MicEngine const& engine() const {
        return m_engine;
    }
    void startEngine(std::string& error);
    void stopEngine();
    void setEnabled(bool on);
    EngineConfig const& config() const {
        return m_config;
    }
    // Applies a changed configuration (devices, monitor mode, offset) and persists it.
    void setConfig(EngineConfig const& config);
    void refreshDevices();
    std::vector<audio::DeviceInfo> const& captureDevices() const {
        return m_captures;
    }
    std::vector<audio::DeviceInfo> const& renderDevices() const {
        return m_renders;
    }
    std::string const& deviceError() const {
        return m_deviceError;
    }
    // Writes the engine config back into the mod settings (device choice, gains, ...).
    void persistConfig();
    // Called from the tick loop: saves a freshly learned fingerprint once.
    void pollCalibration();

    bool hasFingerprint() const {
        return m_engine.hasFingerprint();
    }
    void clearFingerprint();
    std::string const& lastMessage() const {
        return m_message;
    }

private:
    MicClicks() = default;
    void loadFingerprint();
    void saveFingerprint(ClickFingerprint const& fingerprint);
    EngineConfig buildConfig() const;
    int64_t snapToTick(int64_t timeNs) const;

    MicEngine m_engine;
    EngineConfig m_config;
    bool m_enabled = false;
    bool m_initialised = false;
    int64_t m_lastPollNs = 0;
    std::string m_message;

    std::vector<audio::DeviceInfo> m_captures;
    std::vector<audio::DeviceInfo> m_renders;
    std::string m_deviceError;

    // tick clock (P1 update): used to snap input timestamps onto the 240 Hz grid
    std::atomic<int64_t> m_tick { 0 };
    std::atomic<int64_t> m_tickStartNs { 0 };
    std::atomic<int64_t> m_tickPeriodNs { 0 };
    std::atomic<bool> m_snapToTick { true };
};

// Free functions so the hooks in main.cpp stay one line each.
void init();
void onGameTick(int64_t tick, double dt);
void onUserInput(bool down);
void onDeliveredInput(bool down);
void onLevelStart();
void onLevelEnd();
void openSetup();

} // namespace micclicks
