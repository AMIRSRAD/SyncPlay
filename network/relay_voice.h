#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct ma_device;

struct VoiceCaptureDevice {
    std::string name;
};

class RelayVoice {
public:
    using SimpleCallback = std::function<void()>;
    using FrameCallback = std::function<void(const std::vector<uint8_t>& data)>;

    RelayVoice();
    ~RelayVoice();

    bool start();
    void stop();
    void tick();
    void pushIncomingFrame(const std::vector<uint8_t>& data);
    void setOutputGain(float gain);
    void setInputMuted(bool muted);
    void setInputThreshold(float threshold);
    void setCaptureDeviceIndex(int index);
    bool inputMuted() const;
    float inputThreshold() const;
    // Peak of the last captured buffer, 0..1 (updates while the device runs,
    // even when muted, so a level meter can confirm the mic works).
    float inputLevel() const;
    int captureDeviceIndex() const;
    static std::vector<VoiceCaptureDevice> captureDevices();

    bool active() const;
    std::string connectionState() const;

    void setFrameCallback(FrameCallback cb);
    void setConnectionStateCallback(SimpleCallback cb);

private:
    struct AudioChunk {
        std::vector<uint8_t> data;
        size_t offset = 0;
    };

    void setConnectionState(const std::string& state, bool forceEmit = false);
    static void audioCallback(ma_device* device, void* output, const void* input, unsigned int frameCount);

    ma_device* m_device = nullptr;
    int m_sampleRate = 48000;
    int m_channels = 1;
    int m_captureDeviceIndex = -1;
    std::atomic<float> m_outputGain{1.0f};
    std::atomic<float> m_inputThreshold{0.0f};
    std::atomic<float> m_inputLevel{0.0f};
    std::atomic_bool m_inputMuted{true};
    std::atomic_bool m_active{false};
    std::string m_connectionState;

    std::mutex m_incomingMutex;
    std::deque<AudioChunk> m_incoming;
    std::mutex m_outgoingMutex;
    std::deque<std::vector<uint8_t>> m_outgoing;

    std::chrono::steady_clock::time_point m_lastSend{};
    std::chrono::milliseconds m_sendInterval{10};

    FrameCallback m_frameCb;
    SimpleCallback m_connectionStateCb;
};
