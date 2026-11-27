#pragma once
// Mic Clicks: audio device abstraction.
//
// The engine only talks to this interface: one capture stream (microphone) and one
// render stream (where the tick aligned clicks and the optional monitoring go). That
// keeps the rest of the feature testable, and means a platform without an audio backend
// still loads the mod (the panel then says so instead of crashing).

#include <memory>
#include <string>
#include <vector>

#include "Dsp.hpp"

namespace micclicks::audio {

// Every device in this feature runs at the same rate and sample format, so nothing has to be
// converted twice and the DSP (and the fingerprint) only ever sees 48 kHz float samples.
// miniaudio still converts internally if a device can only do something else (44.1 kHz USB
// microphones, a 44.1 kHz Windows mix format); its linear resampler runs with the maximum
// low-pass order for those cases.
inline constexpr int kBackendSampleRate = 48000;

// Tiny periods are what makes WASAPI shared mode crackle: the shared engine hands the buffer
// back in ~10 ms quanta, so asking for 1-5 ms just means the callback is starved on every
// glitch in the system. 10 ms periods, two of them, is the smallest stable configuration.
inline constexpr uint32_t kDefaultPeriodMs = 10;
inline constexpr uint32_t kDefaultPeriods = 2;

struct DeviceInfo {
    int id = -1;  // backend specific, only valid until the next enumeration
    std::string name;
    bool isDefault = false;
    bool isVirtual = false;  // VB-Cable, VoiceMeeter, BlackHole, ...
};

// Names that mean "this is a virtual cable, route to OBS/Discord".
bool looksVirtual(std::string const& name);

// Called on the audio thread with mono float32 at the requested sample rate.
using CaptureSink = void (*)(void* user, float const* mono, size_t frames);
// Called on the audio thread; fills `frames * 2` interleaved stereo floats and returns
// how many frames it wrote (anything left is silence).
using RenderPull = size_t (*)(void* user, float* stereo, size_t frames);

struct CaptureRequest {
    int deviceId = -1;
    int sampleRate = kBackendSampleRate;
    uint32_t periodMs = kDefaultPeriodMs;
    uint32_t periods = kDefaultPeriods;
    bool sharedMode = true;  // never take exclusive control of the device
    void* user = nullptr;
    CaptureSink sink = nullptr;
};

struct RenderRequest {
    int deviceId = -1;
    int sampleRate = kBackendSampleRate;
    uint32_t periodMs = kDefaultPeriodMs;
    uint32_t periods = kDefaultPeriods;
    bool sharedMode = true;
    void* user = nullptr;
    RenderPull pull = nullptr;
};

// What the device actually gave us. The callback always runs at `sampleRate` (that is what we
// asked for); `deviceSampleRate` is what the hardware is really clocked at, and
// `periodFrames * periods` is the buffer the backend handed out, which is the minimum lead
// the click scheduler has to aim for.
struct StreamInfo {
    uint32_t sampleRate = 0;
    uint32_t deviceSampleRate = 0;
    uint32_t periodFrames = 0;
    uint32_t periods = 0;
    bool sharedMode = true;
};

class Stream {
public:
    virtual ~Stream() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual StreamInfo info() const = 0;
};

class CaptureStream : public Stream {};
class RenderStream : public Stream {};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;
    virtual bool available() const = 0;
    virtual std::string unavailableReason() const = 0;
    virtual std::vector<DeviceInfo> enumerate(bool capture, std::string* error) = 0;
    virtual std::unique_ptr<CaptureStream> openCapture(CaptureRequest const& request, std::string& error) = 0;
    virtual std::unique_ptr<RenderStream> openRender(RenderRequest const& request, std::string& error) = 0;
};

// miniaudio backed implementation; a stub that reports "not available" in builds where
// the vendored header is missing.
std::unique_ptr<Backend> createBackend();

} // namespace micclicks::audio
