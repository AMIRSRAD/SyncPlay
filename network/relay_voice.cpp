#include "relay_voice.h"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/logging.h"

RelayVoice::RelayVoice() {
    setConnectionState("Idle", true);
}

RelayVoice::~RelayVoice() {
    stop();
}

bool RelayVoice::start() {
    if (m_device) {
        m_active.store(true, std::memory_order_relaxed);
        setConnectionState("Connected", true);
        return true;
    }

    m_device = new ma_device();
    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format = ma_format_s16;
    config.capture.channels = m_channels;
    config.playback.format = ma_format_s16;
    config.playback.channels = m_channels;
    config.sampleRate = m_sampleRate;
    config.dataCallback = RelayVoice::audioCallback;
    config.pUserData = this;

    ma_device_id selectedCaptureId{};
    bool hasSelectedCaptureId = false;
    if (m_captureDeviceIndex >= 0) {
        ma_context context;
        if (ma_context_init(nullptr, 0, nullptr, &context) == MA_SUCCESS) {
            ma_device_info* playbackInfos = nullptr;
            ma_uint32 playbackCount = 0;
            ma_device_info* captureInfos = nullptr;
            ma_uint32 captureCount = 0;
            if (ma_context_get_devices(&context, &playbackInfos, &playbackCount,
                                       &captureInfos, &captureCount) == MA_SUCCESS &&
                m_captureDeviceIndex < static_cast<int>(captureCount)) {
                selectedCaptureId = captureInfos[m_captureDeviceIndex].id;
                config.capture.pDeviceID = &selectedCaptureId;
                hasSelectedCaptureId = true;
            }
            ma_context_uninit(&context);
        }
        if (!hasSelectedCaptureId)
            LogWarn("relay_voice") << "Selected capture device not available, using default" << std::endl;
    }

    ma_result result = ma_device_init(nullptr, &config, m_device);
    if (result != MA_SUCCESS) {
        LogWarn("relay_voice") << "Audio device init failed" << std::endl;
        delete m_device;
        m_device = nullptr;
        setConnectionState("Failed", true);
        return false;
    }

    result = ma_device_start(m_device);
    if (result != MA_SUCCESS) {
        LogWarn("relay_voice") << "Audio device start failed" << std::endl;
        ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
        setConnectionState("Failed", true);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_incomingMutex);
        m_incoming.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        m_outgoing.clear();
    }

    m_active.store(true, std::memory_order_relaxed);
    m_lastSend = std::chrono::steady_clock::now();
    setConnectionState("Connected", true);
    LogInfo("relay_voice") << "Relay voice audio started" << std::endl;
    return true;
}

void RelayVoice::stop() {
    if (m_device) {
        ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
        LogInfo("relay_voice") << "Relay voice audio stopped" << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(m_incomingMutex);
        m_incoming.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        m_outgoing.clear();
    }

    m_active.store(false, std::memory_order_relaxed);
    setConnectionState("Idle", true);
}

void RelayVoice::tick() {
    if (!m_active.load(std::memory_order_relaxed) || !m_frameCb)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_lastSend.time_since_epoch().count() == 0)
        m_lastSend = now;
    if (now - m_lastSend < m_sendInterval)
        return;
    m_lastSend = now;

    std::deque<std::vector<uint8_t>> frames;
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        frames.swap(m_outgoing);
    }
    for (const auto& frame : frames) {
        if (!frame.empty())
            m_frameCb(frame);
    }
}

void RelayVoice::pushIncomingFrame(const std::vector<uint8_t>& data) {
    if (!m_active.load(std::memory_order_relaxed) || data.empty())
        return;

    std::lock_guard<std::mutex> lock(m_incomingMutex);
    if (m_incoming.size() > 64)
        return;
    AudioChunk chunk;
    chunk.data = data;
    m_incoming.push_back(std::move(chunk));
}

void RelayVoice::setOutputGain(float gain) {
    if (gain < 0.0f)
        gain = 0.0f;
    m_outputGain.store(gain, std::memory_order_relaxed);
}

void RelayVoice::setInputMuted(bool muted) {
    m_inputMuted.store(muted, std::memory_order_relaxed);
    if (muted) {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        m_outgoing.clear();
    }
}

void RelayVoice::setInputThreshold(float threshold) {
    m_inputThreshold.store(std::clamp(threshold, 0.0f, 1.0f), std::memory_order_relaxed);
}

void RelayVoice::setCaptureDeviceIndex(int index) {
    const int next = std::max(-1, index);
    if (m_captureDeviceIndex == next)
        return;
    const bool wasActive = active();
    if (wasActive)
        stop();
    m_captureDeviceIndex = next;
    if (wasActive)
        start();
}

bool RelayVoice::inputMuted() const {
    return m_inputMuted.load(std::memory_order_relaxed);
}

float RelayVoice::inputThreshold() const {
    return m_inputThreshold.load(std::memory_order_relaxed);
}

float RelayVoice::inputLevel() const {
    return m_inputLevel.load(std::memory_order_relaxed);
}

int RelayVoice::captureDeviceIndex() const {
    return m_captureDeviceIndex;
}

std::vector<VoiceCaptureDevice> RelayVoice::captureDevices() {
    std::vector<VoiceCaptureDevice> devices;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
        return devices;

    ma_device_info* playbackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* captureInfos = nullptr;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&context, &playbackInfos, &playbackCount,
                               &captureInfos, &captureCount) == MA_SUCCESS) {
        devices.reserve(captureCount);
        for (ma_uint32 i = 0; i < captureCount; ++i) {
            VoiceCaptureDevice device;
            device.name = captureInfos[i].name[0] ? captureInfos[i].name : "Microphone";
            devices.push_back(std::move(device));
        }
    }

    ma_context_uninit(&context);
    return devices;
}

bool RelayVoice::active() const {
    return m_active.load(std::memory_order_relaxed);
}

std::string RelayVoice::connectionState() const {
    return m_connectionState;
}

void RelayVoice::setFrameCallback(FrameCallback cb) {
    m_frameCb = std::move(cb);
}

void RelayVoice::setConnectionStateCallback(SimpleCallback cb) {
    m_connectionStateCb = std::move(cb);
}

void RelayVoice::setConnectionState(const std::string& state, bool forceEmit) {
    if (!forceEmit && m_connectionState == state)
        return;
    m_connectionState = state;
    if (m_connectionStateCb)
        m_connectionStateCb();
}

void RelayVoice::audioCallback(ma_device* device, void* output, const void* input, unsigned int frameCount) {
    auto* self = static_cast<RelayVoice*>(device ? device->pUserData : nullptr);
    if (!self)
        return;

    const size_t bytes = static_cast<size_t>(frameCount) * self->m_channels * sizeof(int16_t);
    if (output && bytes > 0)
        std::memset(output, 0, bytes);

    // Capture peak for the UI level meter — tracked even while muted so the user
    // can confirm the mic works before unmuting.
    float normalizedPeak = 0.0f;
    if (input && bytes > 0) {
        const auto* samples = static_cast<const int16_t*>(input);
        const size_t count = bytes / sizeof(int16_t);
        int peak = 0;
        for (size_t i = 0; i < count; ++i) {
            const int sample = std::abs(static_cast<int>(samples[i]));
            if (sample > peak)
                peak = sample;
        }
        normalizedPeak = static_cast<float>(peak) / 32767.0f;
        self->m_inputLevel.store(normalizedPeak, std::memory_order_relaxed);
    }

    if (input && bytes > 0 &&
        self->m_active.load(std::memory_order_relaxed) &&
        !self->m_inputMuted.load(std::memory_order_relaxed)) {
        const auto* src = static_cast<const uint8_t*>(input);
        bool shouldSend = true;
        const float threshold = self->m_inputThreshold.load(std::memory_order_relaxed);
        if (threshold > 0.0f && normalizedPeak < threshold)
            shouldSend = false;
        if (shouldSend) {
            std::lock_guard<std::mutex> lock(self->m_outgoingMutex);
            if (self->m_outgoing.size() < 64)
                self->m_outgoing.emplace_back(src, src + bytes);
        }
    }

    if (!output || bytes == 0)
        return;

    auto* dst = static_cast<uint8_t*>(output);
    size_t remaining = bytes;
    std::lock_guard<std::mutex> lock(self->m_incomingMutex);
    while (remaining > 0 && !self->m_incoming.empty()) {
        auto& front = self->m_incoming.front();
        const size_t avail = front.data.size() - front.offset;
        const size_t toCopy = std::min(remaining, avail);
        std::memcpy(dst, front.data.data() + front.offset, toCopy);
        front.offset += toCopy;
        dst += toCopy;
        remaining -= toCopy;
        if (front.offset >= front.data.size())
            self->m_incoming.pop_front();
    }

    const float gain = self->m_outputGain.load(std::memory_order_relaxed);
    if (gain != 1.0f) {
        auto* samples = reinterpret_cast<int16_t*>(output);
        const size_t count = bytes / sizeof(int16_t);
        for (size_t i = 0; i < count; ++i) {
            const float scaled = static_cast<float>(samples[i]) * gain;
            const float clamped = std::clamp(scaled, -32768.0f, 32767.0f);
            samples[i] = static_cast<int16_t>(clamped);
        }
    }
}
