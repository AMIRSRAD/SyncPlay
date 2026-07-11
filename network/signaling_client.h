#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <rtc/websocket.hpp>

#include "../core/task_queue.h"

class SignalingClient {
public:
    using SimpleCallback = std::function<void()>;
    using RelayStateCallback = std::function<void(double time, bool playing, double speed,
                                                  uint64_t seq, double hostLatency)>;
    using RelayFileCallback = std::function<void(int64_t size, double duration, const std::string& hash)>;
    using RelayChatCallback = std::function<void(const std::string& text)>;
    using RelayReactionCallback = std::function<void(const std::string& emoji)>;
    using RelayOpenUrlCallback = std::function<void(const std::string& url)>;
    using RelayIntentCallback = std::function<void(const std::string& action, double value)>;
    using PeerUpdateCallback = std::function<void(bool hostOnline, int guestCount)>;
    using RelayVoiceFrameCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using ShareMetaCallback = std::function<void(const std::string& id,
                                                 const std::string& name,
                                                 int64_t size,
                                                 int totalChunks,
                                                 const std::string& sender)>;
    using ShareChunkCallback = std::function<void(const std::string& id,
                                                  int index,
                                                  const std::vector<uint8_t>& data)>;
    using ShareDoneCallback = std::function<void(const std::string& id,
                                                 const std::string& sha1)>;

    SignalingClient();

    void connectToServer(const std::string& url);
    // HTTP proxy for the WebSocket connection ("" = direct). Loopback targets
    // always connect directly, so hosting a local relay keeps working.
    void setProxy(const std::string& url);
    void disconnect();
    void resetSession();
    void joinSession(const std::string& code, const std::string& role, const std::string& mode = std::string());
    void sendRelayVoiceStart();
    void sendRelayVoiceStop();
    void sendRelayVoiceFrame(const std::vector<uint8_t>& data);
    void sendRelayState(double time, bool playing, double speed, uint64_t seq,
                        double latencySeconds = 0.0);
    // Round-trip-time probe to the relay server (echoed back as "pong").
    void sendPing();
    // Smoothed RTT to the relay server in seconds; 0 until the first pong.
    double rttSeconds() const;
    void sendRelayFile(int64_t size, double duration, const std::string& hash);
    void sendRelayChat(const std::string& text);
    void sendRelayReaction(const std::string& emoji);
    void sendRelayOpenUrl(const std::string& url);
    void sendRelayIntent(const std::string& action, double value);
    void sendShareMeta(const std::string& id, const std::string& name, int64_t size, int totalChunks, const std::string& sender);
    void sendShareChunk(const std::string& id, int index, const std::vector<uint8_t>& data);
    void sendShareDone(const std::string& id, const std::string& sha1 = std::string());

    std::string connectionState() const;
    bool isConnected() const;
    bool isJoined() const;
    std::string sessionCode() const;

    void setConnectionStateCallback(SimpleCallback cb);
    void setRelayVoiceStartCallback(SimpleCallback cb);
    void setRelayVoiceStopCallback(SimpleCallback cb);
    void setRelayVoiceFrameCallback(RelayVoiceFrameCallback cb);
    void setRelayStateCallback(RelayStateCallback cb);
    void setRelayFileCallback(RelayFileCallback cb);
    void setRelayChatCallback(RelayChatCallback cb);
    void setRelayReactionCallback(RelayReactionCallback cb);
    void setRelayOpenUrlCallback(RelayOpenUrlCallback cb);
    void setRelayIntentCallback(RelayIntentCallback cb);
    void setPeerUpdateCallback(PeerUpdateCallback cb);
    void setShareMetaCallback(ShareMetaCallback cb);
    void setShareChunkCallback(ShareChunkCallback cb);
    void setShareDoneCallback(ShareDoneCallback cb);

    void pumpEvents();

private:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const std::string& message);

    void setConnectionState(const std::string& state);
    void sendJoinIfReady();
    void queueOrSend(const std::string& payload);
    void flushPending();
    void maintainReconnect();

    std::shared_ptr<rtc::WebSocket> m_socket;
    std::string m_connectionState;
    std::string m_sessionCode;
    std::string m_role;
    std::string m_pendingJoinCode;
    std::string m_pendingJoinRole;
    std::string m_pendingJoinMode;
    std::string m_joinMode;
    std::string m_lastUrl;
    std::string m_proxyUrl;
    bool m_joined = false;
    std::vector<std::string> m_pendingOutgoing;
    std::atomic<uint64_t> m_generation{0};

    double m_rttSeconds = 0.0;

    // Auto-rejoin: after an unexpected disconnect mid-session, reconnect with
    // exponential backoff and re-issue the last join. Cleared by disconnect().
    bool m_autoRejoin = false;
    bool m_reconnectPending = false;
    std::chrono::steady_clock::time_point m_reconnectAt{};
    std::chrono::milliseconds m_reconnectBackoff{1000};

    SimpleCallback m_connectionStateCb;
    SimpleCallback m_relayVoiceStartCb;
    SimpleCallback m_relayVoiceStopCb;
    RelayVoiceFrameCallback m_relayVoiceFrameCb;
    RelayStateCallback m_relayStateCb;
    RelayFileCallback m_relayFileCb;
    RelayChatCallback m_relayChatCb;
    RelayReactionCallback m_relayReactionCb;
    RelayOpenUrlCallback m_relayOpenUrlCb;
    RelayIntentCallback m_relayIntentCb;
    PeerUpdateCallback m_peerUpdateCb;
    ShareMetaCallback m_shareMetaCb;
    ShareChunkCallback m_shareChunkCb;
    ShareDoneCallback m_shareDoneCb;

    TaskQueue m_events;
};
