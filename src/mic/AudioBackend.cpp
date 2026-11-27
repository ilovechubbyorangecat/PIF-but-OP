// Mic Clicks: audio device backend.
//
// One capture stream (the microphone) and one render stream (the monitoring / VB-Cable
// output). They are deliberately two independent devices: the processed click stream has
// to be routable to a virtual cable while the game keeps playing on the normal output.
//
// miniaudio (public domain / MIT-0, vendored in vendor/miniaudio.h) does the platform
// work: WASAPI on Windows, CoreAudio on macOS/iOS, AAudio/OpenSL on Android. It could not
// be reached on a platform without an audio device, in which case the mod still builds
// and the Mic Setup panel explains that the backend is missing.

#include "AudioBackend.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace micclicks::audio {

bool looksVirtual(std::string const& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    static char const* kMarkers[] = { "cable", "vb-audio", "vb audio", "voicemeeter", "virtual", "blackhole", "loopback",
        "soundflower", "aggregate", "vac ", "dante" };
    for (char const* marker : kMarkers) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace micclicks::audio

#if __has_include("vendor/miniaudio.h")

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "vendor/miniaudio.h"

namespace micclicks::audio {

namespace {

class MiniCapture final : public CaptureStream {
public:
    MiniCapture(ma_context* context, ma_device_id const* deviceId, int sampleRate, CaptureSink sink, void* user)
        : m_sink(sink), m_user(user) {
        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        if (deviceId) {
            m_deviceId = *deviceId;
            m_haveDeviceId = true;
            config.capture.pDeviceID = &m_deviceId;
        }
        config.sampleRate = static_cast<ma_uint32>(sampleRate);
        config.periodSizeInMilliseconds = 5;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.dataCallback = &MiniCapture::onData;
        config.pUserData = this;
        m_result = ma_device_init(context, &config, &m_device);
        m_ready = m_result == MA_SUCCESS;
    }

    ~MiniCapture() override {
        if (m_ready) {
            ma_device_uninit(&m_device);
            m_ready = false;
        }
    }

    bool ready() const {
        return m_ready;
    }
    ma_result result() const {
        return m_result;
    }

    bool start() override {
        if (!m_ready) {
            return false;
        }
        return ma_device_start(&m_device) == MA_SUCCESS;
    }

    void stop() override {
        if (m_ready) {
            ma_device_stop(&m_device);
        }
    }

private:
    static void onData(ma_device* device, void* output, void const* input, ma_uint32 frameCount) {
        (void)output;
        auto* self = static_cast<MiniCapture*>(device->pUserData);
        if (!self || !self->m_sink || !input || frameCount == 0) {
            return;
        }
        self->m_sink(self->m_user, static_cast<float const*>(input), static_cast<size_t>(frameCount));
    }

    ma_device m_device {};
    ma_device_id m_deviceId {};
    ma_result m_result = MA_ERROR;
    bool m_ready = false;
    bool m_haveDeviceId = false;
    CaptureSink m_sink = nullptr;
    void* m_user = nullptr;
};

class MiniRender final : public RenderStream {
public:
    MiniRender(ma_context* context, ma_device_id const* deviceId, int sampleRate, RenderPull pull, void* user)
        : m_pull(pull), m_user(user) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        if (deviceId) {
            m_deviceId = *deviceId;
            m_haveDeviceId = true;
            config.playback.pDeviceID = &m_deviceId;
        }
        config.sampleRate = static_cast<ma_uint32>(sampleRate);
        config.periodSizeInMilliseconds = 5;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.dataCallback = &MiniRender::onData;
        config.pUserData = this;
        m_result = ma_device_init(context, &config, &m_device);
        m_ready = m_result == MA_SUCCESS;
    }

    ~MiniRender() override {
        if (m_ready) {
            ma_device_uninit(&m_device);
            m_ready = false;
        }
    }

    bool ready() const {
        return m_ready;
    }
    ma_result result() const {
        return m_result;
    }

    bool start() override {
        if (!m_ready) {
            return false;
        }
        return ma_device_start(&m_device) == MA_SUCCESS;
    }

    void stop() override {
        if (m_ready) {
            ma_device_stop(&m_device);
        }
    }

private:
    static void onData(ma_device* device, void* output, void const* input, ma_uint32 frameCount) {
        (void)input;
        auto* self = static_cast<MiniRender*>(device->pUserData);
        if (!output || frameCount == 0) {
            return;
        }
        float* out = static_cast<float*>(output);
        if (self && self->m_pull) {
            size_t written = self->m_pull(self->m_user, out, static_cast<size_t>(frameCount));
            if (written < frameCount) {
                std::memset(out + written * 2, 0, (frameCount - written) * 2 * sizeof(float));
            }
        }
        else {
            std::memset(out, 0, static_cast<size_t>(frameCount) * 2 * sizeof(float));
        }
    }

    ma_device m_device {};
    ma_device_id m_deviceId {};
    ma_result m_result = MA_ERROR;
    bool m_ready = false;
    bool m_haveDeviceId = false;
    RenderPull m_pull = nullptr;
    void* m_user = nullptr;
};

class MiniBackend final : public Backend {
public:
    MiniBackend() {
        ma_context_config config = ma_context_config_init();
        m_result = ma_context_init(nullptr, 0, &config, &m_context);
        m_ok = m_result == MA_SUCCESS;
        if (!m_ok) {
            m_reason = std::string("miniaudio context: ") + ma_result_description(m_result);
        }
    }

    ~MiniBackend() override {
        if (m_ok) {
            ma_context_uninit(&m_context);
            m_ok = false;
        }
    }

    std::string name() const override {
        return "miniaudio";
    }
    bool available() const override {
        return m_ok;
    }
    std::string unavailableReason() const override {
        return m_reason;
    }

    std::vector<DeviceInfo> enumerate(bool capture, std::string* error) override {
        std::vector<DeviceInfo> devices;
        if (!m_ok) {
            if (error) {
                *error = m_reason;
            }
            return devices;
        }
        ma_device_info* playbackInfos = nullptr;
        ma_uint32 playbackCount = 0;
        ma_device_info* captureInfos = nullptr;
        ma_uint32 captureCount = 0;
        ma_result result = ma_context_get_devices(&m_context, &playbackInfos, &playbackCount, &captureInfos, &captureCount);
        if (result != MA_SUCCESS) {
            if (error) {
                *error = ma_result_description(result);
            }
            return devices;
        }
        ma_device_info* infos = capture ? captureInfos : playbackInfos;
        ma_uint32 count = capture ? captureCount : playbackCount;
        auto& ids = capture ? m_captureIds : m_renderIds;
        ids.clear();
        devices.reserve(count);
        for (ma_uint32 i = 0; i < count; ++i) {
            DeviceInfo info;
            info.id = static_cast<int>(ids.size());
            info.name = infos[i].name;
            info.isDefault = infos[i].isDefault != MA_FALSE;
            info.isVirtual = looksVirtual(info.name);
            ids.push_back(infos[i].id);
            devices.push_back(std::move(info));
        }
        return devices;
    }

    std::unique_ptr<CaptureStream> openCapture(CaptureRequest const& request, std::string& error) override {
        if (!m_ok) {
            error = m_reason;
            return nullptr;
        }
        ma_device_id const* id = lookup(m_captureIds, request.deviceId);
        auto stream = std::make_unique<MiniCapture>(&m_context, id, request.sampleRate, request.sink, request.user);
        if (!stream->ready()) {
            error = std::string("capture device: ") + ma_result_description(stream->result());
            return nullptr;
        }
        return stream;
    }

    std::unique_ptr<RenderStream> openRender(RenderRequest const& request, std::string& error) override {
        if (!m_ok) {
            error = m_reason;
            return nullptr;
        }
        ma_device_id const* id = lookup(m_renderIds, request.deviceId);
        auto stream = std::make_unique<MiniRender>(&m_context, id, request.sampleRate, request.pull, request.user);
        if (!stream->ready()) {
            error = std::string("output device: ") + ma_result_description(stream->result());
            return nullptr;
        }
        return stream;
    }

private:
    static ma_device_id const* lookup(std::vector<ma_device_id> const& ids, int index) {
        if (index < 0 || index >= static_cast<int>(ids.size())) {
            return nullptr;  // default device
        }
        return &ids[static_cast<size_t>(index)];
    }

    ma_context m_context {};
    ma_result m_result = MA_ERROR;
    bool m_ok = false;
    std::string m_reason;
    std::vector<ma_device_id> m_captureIds;
    std::vector<ma_device_id> m_renderIds;
};

} // namespace

std::unique_ptr<Backend> createBackend() {
    return std::make_unique<MiniBackend>();
}

} // namespace micclicks::audio

#else

namespace micclicks::audio {

namespace {
class UnavailableBackend final : public Backend {
public:
    std::string name() const override {
        return "none";
    }
    bool available() const override {
        return false;
    }
    std::string unavailableReason() const override {
        return "This build has no audio backend (vendor/miniaudio.h is missing).";
    }
    std::vector<DeviceInfo> enumerate(bool, std::string* error) override {
        if (error) {
            *error = unavailableReason();
        }
        return {};
    }
    std::unique_ptr<CaptureStream> openCapture(CaptureRequest const&, std::string& error) override {
        error = unavailableReason();
        return nullptr;
    }
    std::unique_ptr<RenderStream> openRender(RenderRequest const&, std::string& error) override {
        error = unavailableReason();
        return nullptr;
    }
};
} // namespace

std::unique_ptr<Backend> createBackend() {
    return std::make_unique<UnavailableBackend>();
}

} // namespace micclicks::audio

#endif
