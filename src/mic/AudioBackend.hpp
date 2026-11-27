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
    int sampleRate = kSampleRate;
    void* user = nullptr;
    CaptureSink sink = nullptr;
};

struct RenderRequest {
    int deviceId = -1;
    int sampleRate = kSampleRate;
    void* user = nullptr;
    RenderPull pull = nullptr;
};

class Stream {
public:
    virtual ~Stream() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
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
