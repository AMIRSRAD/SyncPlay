#include "signaling_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include "../core/base64.h"
#include "../core/logging.h"
#include "../core/string_utils.h"

namespace {
double SteadyNowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool normalize_ws_url(const std::string& input, std::string& out) {
    std::string trimmed = Trim(input);
    if (trimmed.empty())
        return false;
    if (trimmed.find("://") == std::string::npos)
        trimmed = "ws://" + trimmed;

    const size_t scheme_end = trimmed.find("://");
    if (scheme_end == std::string::npos)
        return false;
    const size_t host_start = scheme_end + 3;
    if (host_start >= trimmed.size())
        return false;
    const size_t host_end = trimmed.find('/', host_start);
    const std::string host = trimmed.substr(host_start, host_end - host_start);
    if (host.empty())
        return false;
    out = trimmed;
    return true;
}
}

SignalingClient::SignalingClient() {
    setConnectionState("Idle");
}

void SignalingClient::connectToServer(const std::string& url) {
    std::string wsUrl;
    if (!normalize_ws_url(url, wsUrl)) {
        if (m_socket) {
            m_generation.fetch_add(1, std::memory_order_relaxed);
            m_socket->close();
            m_socket.reset();
        }
        setConnectionState("Invalid URL");
        LogWarn("signaling") << "Invalid URL" << std::endl;
        return;
    }

    m_lastUrl = wsUrl;
    LogInfo("signaling") << "Connecting to " << m_lastUrl << std::endl;

    const auto generation = m_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m_socket) {
        m_socket->close();
        m_socket.reset();
    }
    m_joined = false;
    m_socket = std::make_shared<rtc::WebSocket>();
    m_socket->onOpen([this, generation]() {
        if (generation != m_generation.load(std::memory_order_relaxed))
            return;
        LogInfo("signaling") << "WebSocket open " << m_lastUrl << std::endl;
        m_events.push([this]() { onConnected(); });
    });
    m_socket->onClosed([this, generation]() {
        if (generation != m_generation.load(std::memory_order_relaxed))
            return;
        LogInfo("signaling") << "WebSocket closed " << m_lastUrl << std::endl;
        m_events.push([this]() { onDisconnected(); });
    });
    m_socket->onError([this, generation](std::string error) {
        if (generation != m_generation.load(std::memory_order_relaxed))
            return;
        LogWarn("signaling") << "WebSocket error " << error << " " << m_lastUrl << std::endl;
        m_events.push([this]() { onDisconnected(); });
    });
    m_socket->onMessage({}, [this, generation](rtc::string text) {
        if (generation != m_generation.load(std::memory_order_relaxed))
            return;
        if (text.empty())
            return;
        const std::string payload = text;
        m_events.push([this, payload]() {
            LogDebug("signaling") << "WebSocket message " << payload.substr(0, 80) << std::endl;
            onTextMessage(payload);
        });
    });

    setConnectionState("Connecting");
    m_socket->open(wsUrl);
}

void SignalingClient::disconnect() {
    m_autoRejoin = false;
    m_reconnectPending = false;
    m_reconnectBackoff = std::chrono::milliseconds(1000);
    if (!m_lastUrl.empty())
        LogInfo("signaling") << "Disconnecting from " << m_lastUrl << std::endl;
    if (m_socket) {
        m_generation.fetch_add(1, std::memory_order_relaxed);
        m_socket->close();
        m_socket.reset();
    }
    m_pendingOutgoing.clear();
    m_pendingJoinCode.clear();
    m_pendingJoinRole.clear();
    m_pendingJoinMode.clear();
    m_sessionCode.clear();
    m_role.clear();
    m_joined = false;
    m_joinMode.clear();
    m_lastUrl.clear();
    setConnectionState("Disconnected");
}

void SignalingClient::resetSession() {
    m_autoRejoin = false;
    m_reconnectPending = false;
    m_pendingOutgoing.clear();
    m_pendingJoinCode.clear();
    m_pendingJoinRole.clear();
    m_pendingJoinMode.clear();
    m_sessionCode.clear();
    m_role.clear();
    m_joined = false;
    m_joinMode.clear();
}

void SignalingClient::joinSession(const std::string& code, const std::string& role, const std::string& mode) {
    m_pendingJoinCode = Trim(code);
    m_pendingJoinRole = ToLower(Trim(role));
    m_pendingJoinMode = ToLower(Trim(mode));
    if (m_pendingJoinMode.empty())
        m_pendingJoinMode = "relay";
    LogInfo("signaling") << "Join requested " << m_pendingJoinRole << " " << m_pendingJoinCode
                          << " " << m_pendingJoinMode << std::endl;
    sendJoinIfReady();
}

void SignalingClient::sendRelayVoiceStart() {
    nlohmann::json j;
    j["type"] = "relay_voice_start";
    j["code"] = m_sessionCode;
    queueOrSend(j.dump());
}

void SignalingClient::sendRelayVoiceStop() {
    nlohmann::json j;
    j["type"] = "relay_voice_stop";
    j["code"] = m_sessionCode;
    queueOrSend(j.dump());
}

void SignalingClient::sendRelayVoiceFrame(const std::vector<uint8_t>& data) {
    if (data.empty())
        return;
    nlohmann::json j;
    j["type"] = "relay_voice_frame";
    j["code"] = m_sessionCode;
    j["data"] = Base64Encode(data);
    queueOrSend(j.dump());
}

std::string SignalingClient::connectionState() const {
    return m_connectionState;
}

bool SignalingClient::isConnected() const {
    return m_socket && m_socket->isOpen();
}

bool SignalingClient::isJoined() const {
    return m_joined;
}

std::string SignalingClient::sessionCode() const {
    return m_sessionCode;
}

void SignalingClient::setConnectionStateCallback(SimpleCallback cb) {
    m_connectionStateCb = std::move(cb);
}

void SignalingClient::setRelayVoiceStartCallback(SimpleCallback cb) {
    m_relayVoiceStartCb = std::move(cb);
}

void SignalingClient::setRelayVoiceStopCallback(SimpleCallback cb) {
    m_relayVoiceStopCb = std::move(cb);
}

void SignalingClient::setRelayVoiceFrameCallback(RelayVoiceFrameCallback cb) {
    m_relayVoiceFrameCb = std::move(cb);
}

void SignalingClient::setRelayStateCallback(RelayStateCallback cb) {
    m_relayStateCb = std::move(cb);
}

void SignalingClient::setRelayFileCallback(RelayFileCallback cb) {
    m_relayFileCb = std::move(cb);
}

void SignalingClient::setRelayChatCallback(RelayChatCallback cb) {
    m_relayChatCb = std::move(cb);
}

void SignalingClient::setRelayOpenUrlCallback(RelayOpenUrlCallback cb) {
    m_relayOpenUrlCb = std::move(cb);
}

void SignalingClient::setRelayReactionCallback(RelayReactionCallback cb) {
    m_relayReactionCb = std::move(cb);
}

void SignalingClient::setRelayIntentCallback(RelayIntentCallback cb) {
    m_relayIntentCb = std::move(cb);
}

void SignalingClient::setPeerUpdateCallback(PeerUpdateCallback cb) {
    m_peerUpdateCb = std::move(cb);
}

void SignalingClient::setShareMetaCallback(ShareMetaCallback cb) {
    m_shareMetaCb = std::move(cb);
}

void SignalingClient::setShareChunkCallback(ShareChunkCallback cb) {
    m_shareChunkCb = std::move(cb);
}

void SignalingClient::setShareDoneCallback(ShareDoneCallback cb) {
    m_shareDoneCb = std::move(cb);
}

void SignalingClient::pumpEvents() {
    m_events.drain();
    maintainReconnect();
}

void SignalingClient::maintainReconnect() {
    if (!m_reconnectPending)
        return;
    if (std::chrono::steady_clock::now() < m_reconnectAt)
        return;
    m_reconnectPending = false;
    if (m_lastUrl.empty() || m_sessionCode.empty() || m_role.empty())
        return;
    LogInfo("signaling") << "Auto-reconnect attempt to " << m_lastUrl
                         << " as " << m_role << " " << m_sessionCode << std::endl;
    const std::string url = m_lastUrl;
    const std::string code = m_sessionCode;
    const std::string role = m_role;
    const std::string mode = m_joinMode;
    connectToServer(url);
    joinSession(code, role, mode);
}

void SignalingClient::onConnected() {
    m_reconnectBackoff = std::chrono::milliseconds(1000);
    m_reconnectPending = false;
    setConnectionState("Connected");
    sendJoinIfReady();
}

void SignalingClient::onDisconnected() {
    m_joined = false;
    // Unexpected drop mid-session: schedule an auto-rejoin with backoff so a
    // Wi-Fi blip doesn't end the watch party. deliberate disconnect() clears
    // m_autoRejoin before closing, so it never reaches here armed.
    if (m_autoRejoin && !m_sessionCode.empty() && !m_lastUrl.empty()) {
        m_reconnectPending = true;
        m_reconnectAt = std::chrono::steady_clock::now() + m_reconnectBackoff;
        LogInfo("signaling") << "Scheduling reconnect in "
                             << m_reconnectBackoff.count() << "ms" << std::endl;
        m_reconnectBackoff = std::min(m_reconnectBackoff * 2, std::chrono::milliseconds(10000));
        setConnectionState("Reconnecting");
        return;
    }
    setConnectionState("Disconnected");
}

void SignalingClient::onTextMessage(const std::string& message) {
    try {
        auto j = nlohmann::json::parse(message, nullptr, false);
        if (!j.is_object())
            return;

        std::string type = j.value("type", "");
        if (type == "relay_voice_start") {
            LogInfo("signaling") << "Relay voice start received" << std::endl;
            if (m_relayVoiceStartCb)
                m_relayVoiceStartCb();
        } else if (type == "relay_voice_stop") {
            LogInfo("signaling") << "Relay voice stop received" << std::endl;
            if (m_relayVoiceStopCb)
                m_relayVoiceStopCb();
        } else if (type == "relay_voice_frame") {
            std::string data = j.value("data", "");
            std::vector<uint8_t> bytes = Base64Decode(data);
            if (!bytes.empty() && m_relayVoiceFrameCb)
                m_relayVoiceFrameCb(bytes);
        } else if (type == "state") {
            double time = j.value("timestamp", 0.0);
            bool playing = j.value("playing", false);
            double speed = j.value("speed", 1.0);
            uint64_t seq = j.value("seq", 0ULL);
            double lat = j.value("lat", 0.0);
            if (m_relayStateCb)
                m_relayStateCb(time, playing, speed, seq, lat);
        } else if (type == "pong") {
            const double t0 = j.value("t", 0.0);
            if (t0 > 0.0) {
                const double rtt = (SteadyNowMs() - t0) / 1000.0;
                if (rtt >= 0.0 && rtt < 5.0) {
                    // Smooth with an EMA so one congested probe doesn't swing
                    // the compensation.
                    m_rttSeconds = (m_rttSeconds <= 0.0) ? rtt
                                                         : m_rttSeconds * 0.8 + rtt * 0.2;
                }
            }
        } else if (type == "file") {
            long long size = j.value("size", 0LL);
            double duration = j.value("duration", 0.0);
            std::string hash = j.value("hash", "");
            if (m_relayFileCb)
                m_relayFileCb(static_cast<int64_t>(size), duration, hash);
        } else if (type == "chat") {
            std::string text = j.value("text", "");
            if (!text.empty() && m_relayChatCb)
                m_relayChatCb(text);
        } else if (type == "reaction") {
            std::string emoji = j.value("emoji", "");
            if (!emoji.empty() && emoji.size() <= 16 && m_relayReactionCb)
                m_relayReactionCb(emoji);
        } else if (type == "open_url") {
            std::string url = j.value("url", "");
            if (!url.empty() && url.size() <= 2048 && m_relayOpenUrlCb)
                m_relayOpenUrlCb(url);
        } else if (type == "intent") {
            std::string action = j.value("action", "");
            double value = j.value("value", 0.0);
            if (!action.empty() && m_relayIntentCb)
                m_relayIntentCb(action, value);
        } else if (type == "share_meta") {
            std::string id = j.value("id", "");
            std::string name = j.value("name", "");
            std::string sender = j.value("sender", "");
            long long size = j.value("size", 0LL);
            int totalChunks = j.value("totalChunks", 0);
            if (m_shareMetaCb)
                m_shareMetaCb(id, name, static_cast<int64_t>(size), totalChunks, sender);
        } else if (type == "share_chunk") {
            std::string id = j.value("id", "");
            std::string data = j.value("data", "");
            int index = j.value("index", 0);
            std::vector<uint8_t> bytes = Base64Decode(data);
            if (m_shareChunkCb)
                m_shareChunkCb(id, index, bytes);
        } else if (type == "share_done") {
            std::string id = j.value("id", "");
            std::string sha1 = j.value("sha1", "");
            if (m_shareDoneCb)
                m_shareDoneCb(id, sha1);
        } else if (type == "peer_update") {
            bool hostOnline = j.value("host", false);
            int guestCount = j.value("guests", 0);
            if (m_peerUpdateCb)
                m_peerUpdateCb(hostOnline, guestCount);
        } else {
            LogDebug("signaling") << "Signal ignored type " << type << std::endl;
        }
    } catch (const std::exception& e) {
        LogWarn("signaling") << "Signal parse failed " << e.what() << std::endl;
    }
}

void SignalingClient::setConnectionState(const std::string& state) {
    if (m_connectionState == state)
        return;
    LogInfo("signaling") << "Signaling state " << m_connectionState << " -> " << state << " " << m_lastUrl << std::endl;
    m_connectionState = state;
    if (m_connectionStateCb)
        m_connectionStateCb();
}

void SignalingClient::sendJoinIfReady() {
    if (!isConnected())
        return;
    if (m_pendingJoinCode.empty() || m_pendingJoinRole.empty())
        return;

    nlohmann::json j;
    j["type"] = "join";
    j["code"] = m_pendingJoinCode;
    j["role"] = m_pendingJoinRole;
    j["mode"] = m_pendingJoinMode;
    std::string payload = j.dump();
    LogInfo("signaling") << "Sending join " << m_pendingJoinRole << " " << m_pendingJoinCode << std::endl;
    m_socket->send(payload);

    m_sessionCode = m_pendingJoinCode;
    m_role = m_pendingJoinRole;
    m_joinMode = m_pendingJoinMode;
    m_pendingJoinCode.clear();
    m_pendingJoinRole.clear();
    m_pendingJoinMode.clear();
    m_joined = true;
    m_autoRejoin = true;
    flushPending();
}

void SignalingClient::sendRelayState(double time, bool playing, double speed, uint64_t seq,
                                     double latencySeconds) {
    nlohmann::json j;
    j["type"] = "state";
    j["code"] = m_sessionCode;
    j["timestamp"] = time;
    j["playing"] = playing;
    j["speed"] = speed;
    j["seq"] = seq;
    // Sender's one-way latency to the relay server; receivers add their own
    // half-RTT to estimate how stale `timestamp` is on arrival.
    j["lat"] = latencySeconds;
    queueOrSend(j.dump());
}

void SignalingClient::sendPing() {
    if (!m_socket || !m_socket->isOpen())
        return; // stale pings are useless; never queue them
    nlohmann::json j;
    j["type"] = "ping";
    j["t"] = SteadyNowMs();
    m_socket->send(j.dump());
}

double SignalingClient::rttSeconds() const {
    return m_rttSeconds;
}

void SignalingClient::sendRelayFile(int64_t size, double duration, const std::string& hash) {
    nlohmann::json j;
    j["type"] = "file";
    j["code"] = m_sessionCode;
    j["size"] = static_cast<long long>(size);
    j["duration"] = duration;
    j["hash"] = hash;
    queueOrSend(j.dump());
}

void SignalingClient::sendRelayChat(const std::string& text) {
    nlohmann::json j;
    j["type"] = "chat";
    j["code"] = m_sessionCode;
    j["text"] = text;
    queueOrSend(j.dump());
}

void SignalingClient::sendRelayReaction(const std::string& emoji) {
    if (emoji.empty() || emoji.size() > 16)
        return;
    nlohmann::json j;
    j["type"] = "reaction";
    j["code"] = m_sessionCode;
    j["emoji"] = emoji;
    // Reactions are ephemeral; drop rather than queue when disconnected.
    queueOrSend(j.dump());
}

void SignalingClient::sendRelayOpenUrl(const std::string& url) {
    if (url.empty() || url.size() > 2048)
        return;
    nlohmann::json j;
    j["type"] = "open_url";
    j["code"] = m_sessionCode;
    j["url"] = url;
    queueOrSend(j.dump());
}

void SignalingClient::sendRelayIntent(const std::string& action, double value) {
    if (action.empty())
        return;
    nlohmann::json j;
    j["type"] = "intent";
    j["code"] = m_sessionCode;
    j["action"] = action;
    j["value"] = value;
    queueOrSend(j.dump());
}

void SignalingClient::sendShareMeta(const std::string& id, const std::string& name, int64_t size, int totalChunks, const std::string& sender) {
    nlohmann::json j;
    j["type"] = "share_meta";
    j["code"] = m_sessionCode;
    j["id"] = id;
    j["name"] = name;
    j["size"] = static_cast<long long>(size);
    j["totalChunks"] = totalChunks;
    j["sender"] = sender;
    queueOrSend(j.dump());
}

void SignalingClient::sendShareChunk(const std::string& id, int index, const std::vector<uint8_t>& data) {
    nlohmann::json j;
    j["type"] = "share_chunk";
    j["code"] = m_sessionCode;
    j["id"] = id;
    j["index"] = index;
    j["data"] = Base64Encode(data);
    queueOrSend(j.dump());
}

void SignalingClient::sendShareDone(const std::string& id, const std::string& sha1) {
    nlohmann::json j;
    j["type"] = "share_done";
    j["code"] = m_sessionCode;
    j["id"] = id;
    if (!sha1.empty())
        j["sha1"] = sha1;
    queueOrSend(j.dump());
}

void SignalingClient::queueOrSend(const std::string& payload) {
    if (!isConnected() || !m_joined || m_sessionCode.empty()) {
        m_pendingOutgoing.push_back(payload);
        LogDebug("signaling") << "Queueing signal; connected "
                               << (m_socket && m_socket->isOpen())
                               << " joined " << m_joined
                               << " code " << m_sessionCode
                               << " pending " << m_pendingOutgoing.size() << std::endl;
        return;
    }
    m_socket->send(payload);
}

void SignalingClient::flushPending() {
    if (!isConnected() || !m_joined || m_sessionCode.empty())
        return;
    for (const auto& payload : m_pendingOutgoing)
        m_socket->send(payload);
    if (!m_pendingOutgoing.empty())
        LogDebug("signaling") << "Flushed pending messages " << m_pendingOutgoing.size() << std::endl;
    m_pendingOutgoing.clear();
}
