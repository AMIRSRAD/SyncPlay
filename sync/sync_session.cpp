#include "sync_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <system_error>

#include "../core/logging.h"
#include "../core/string_utils.h"

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace {
std::string generateId() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 15);
    const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(32);
    for (int i = 0; i < 32; ++i)
        out[i] = hex[dist(rng)];
    return out;
}

std::filesystem::path downloadsDir() {
#ifdef _WIN32
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path) == S_OK && path) {
        std::filesystem::path dir(path);
        CoTaskMemFree(path);
        return dir;
    }
#endif
    if (const char* home = std::getenv("USERPROFILE"))
        return std::filesystem::path(home);
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home);
    return std::filesystem::current_path();
}

// Reduce a peer-supplied file name to a bare, safe filename so an incoming share
// can never escape the Downloads directory via ".." traversal, path separators,
// or an absolute/drive-qualified path. Never trust the name off the wire.
std::string sanitizeShareFileName(const std::string& raw) {
    std::string name = Trim(raw);
    const size_t sep = name.find_last_of("/\\");
    if (sep != std::string::npos)
        name = name.substr(sep + 1);
    if (name.size() >= 2 && name[1] == ':')  // strip a leftover drive prefix like "C:"
        name = name.substr(2);
    name = Trim(name);
    if (name == "." || name == "..")
        return {};
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out.push_back('_');
        else
            out.push_back(c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))  // Windows strips these
        out.pop_back();
    return out;
}
}

SyncSession::SyncSession(PlaybackController* player)
        : m_player(player),
          m_sync(player, SyncManager::Role::Host) {
    m_signalingClient.setRelayStateCallback([this](double time, bool playing, double speed,
                                                   uint64_t seq, double hostLatency) {
        onRelayState(time, playing, speed, seq, hostLatency);
    });
    m_signalingClient.setRelayChatCallback([this](const std::string& message) {
        dispatchChat(message);
    });
    m_signalingClient.setRelayReactionCallback([this](const std::string& emoji) {
        if (m_reactionCallback)
            m_reactionCallback(emoji);
    });
    m_signalingClient.setRelayIntentCallback([this](const std::string& action, double value) {
        onRelayIntent(action, value);
    });
    m_signalingClient.setPeerUpdateCallback([this](bool hostOnline, int guestCount) {
        onRelayPeerUpdate(hostOnline, guestCount);
    });
    m_signalingClient.setConnectionStateCallback([this]() {
        onConnectionStateChanged();
    });
    m_signalingClient.setRelayVoiceStartCallback([this]() {
        onRelayVoiceStartReceived();
    });
    m_signalingClient.setRelayVoiceStopCallback([this]() {
        onRelayVoiceStopReceived();
    });
    m_signalingClient.setRelayVoiceFrameCallback([this](const std::vector<uint8_t>& data) {
        onRelayVoiceFrameReceived(data);
    });
    m_signalingClient.setRelayFileCallback([this](int64_t size, double duration, const std::string& hash) {
        onRelayFileReceived(size, duration, hash);
    });
    m_signalingClient.setShareMetaCallback([this](const std::string& id, const std::string& name,
                                                  int64_t size, int totalChunks, const std::string& sender) {
        onShareMetaReceived(id, name, size, totalChunks, sender);
    });
    m_signalingClient.setShareChunkCallback([this](const std::string& id, int index, const std::vector<uint8_t>& data) {
        onShareChunkReceived(id, index, data);
    });
    m_signalingClient.setShareDoneCallback([this](const std::string& id, const std::string& sha1) {
        onShareDoneReceived(id, sha1);
    });

    m_relayVoice.setFrameCallback([this](const std::vector<uint8_t>& data) {
        if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
            return;
        if (!voiceAvailable())
            return;
        m_signalingClient.sendRelayVoiceFrame(data);
    });
    m_relayVoice.setConnectionStateCallback([this]() { onVoiceStateChanged(); });

    updateStatus();
    updateFileVerification();
}

void SyncSession::setNickname(const std::string& nickname) {
    m_nickname = Trim(nickname);
}

void SyncSession::setSignalingPort(int port) {
    m_signalingPort = port;
}

void SyncSession::setSessionPassword(const std::string& password) {
    m_sessionPassword = Trim(password);
}

void SyncSession::setSelectedInterfaceAddress(const std::string& address) {
    m_selectedInterfaceAddress = Trim(address);
    m_signalingServer.setPreferredInterface(m_selectedInterfaceAddress);
}

void SyncSession::setAutoPromote(bool enabled) {
    m_autoPromote = enabled;
}

void SyncSession::setVoiceEnabled(bool enabled) {
    if (m_voiceEnabled == enabled)
        return;
    m_voiceEnabled = enabled;
    if (!m_voiceEnabled) {
        setVoiceMuted(true);
        if (m_relayVoice.active())
            stopVoiceCall();
    }
    dispatchStatus();
}

void SyncSession::setVoiceMuted(bool muted) {
    m_voiceMuted = muted;
    m_relayVoice.setInputMuted(m_voiceMuted);
    dispatchStatus();
}

void SyncSession::setVoiceVolume(float volume) {
    const float clamped = std::clamp(volume, 0.0f, 100.0f);
    m_relayVoice.setOutputGain(clamped / 100.0f);
}

void SyncSession::setVoiceInputThreshold(float threshold) {
    m_relayVoice.setInputThreshold(threshold);
}

void SyncSession::setVoiceCaptureDeviceIndex(int index) {
    m_relayVoice.setCaptureDeviceIndex(index);
    dispatchStatus();
}

void SyncSession::setLocalFileInfo(int64_t size, double duration, const std::string& hash) {
    if (size <= 0 || hash.empty())
        return;
    m_localFile.size = size;
    m_localFile.duration = duration;
    m_localFile.hash = hash;
    m_localFile.valid = true;
    updateFileVerification();
    sendFileInfoIfReady();
}

void SyncSession::setAllowGuestControl(bool enabled) {
    m_allowGuestControl = enabled;
}

void SyncSession::startHostSession() {
    LogInfo("session") << "Starting host session on port " << m_signalingPort
                       << " interface "
                       << (m_selectedInterfaceAddress.empty() ? std::string("auto") : m_selectedInterfaceAddress)
                       << std::endl;
    m_hostingSession = true;
    m_sessionActive = true;
    m_relayHostOnline = true;
    m_relayGuestCount = 0;
    m_lastRemoteSeq = 0;
    m_lastRemoteTime = 0.0;
    m_lastRemoteAt = {};
    m_localStateSeq = 0;
    m_remotePlayingKnown = false;
    m_remoteSpeedKnown = false;

    m_signalingServer.setPreferredInterface(m_selectedInterfaceAddress);
    if (!m_signalingServer.isRunning() &&
        !m_signalingServer.start(static_cast<uint16_t>(m_signalingPort))) {
        LogWarn("session") << "Host session failed; signaling server did not start" << std::endl;
        m_hostingSession = false;
        m_sessionActive = false;
        m_relayHostOnline = false;
        m_statusText = "Host failed";
        m_hintText = "Could not start host server. Check port/firewall/interface.";
        dispatchStatus();
        return;
    }
    m_sessionCode = m_signalingServer.createSession();
    if (!m_sessionPassword.empty())
        m_signalingServer.addAlias(m_sessionPassword, m_sessionCode);
    m_serverUrl = m_signalingServer.serverUrl();
    LogInfo("session") << "Host session created url " << m_serverUrl
                       << " code " << m_sessionCode << std::endl;

    m_signalingClient.connectToServer(m_serverUrl);
    m_signalingClient.joinSession(m_sessionCode, "host", "relay");
    m_sync.setRole(SyncManager::Role::Host);
    sendFileInfoIfReady();
    updateStatus();
}

void SyncSession::joinSession(const std::string& url, const std::string& code) {
    LogInfo("session") << "Joining session url " << Trim(url)
                       << " code " << Trim(code) << std::endl;
    m_hostingSession = false;
    m_sessionActive = true;
    m_relayHostOnline = false;
    m_relayGuestCount = 0;
    m_lastRemoteSeq = 0;
    m_lastRemoteTime = 0.0;
    m_lastRemoteAt = {};
    m_localStateSeq = 0;
    m_remotePlayingKnown = false;
    m_remoteSpeedKnown = false;

    const std::string trimmedUrl = Trim(url);
    const std::string trimmedCode = Trim(code);
    m_serverUrl = trimmedUrl;
    m_sessionCode = trimmedCode;
    m_signalingClient.connectToServer(trimmedUrl);
    m_signalingClient.joinSession(trimmedCode, "guest", "relay");
    m_sync.setRole(SyncManager::Role::Guest);
    sendFileInfoIfReady();
    updateStatus();
}

void SyncSession::disconnectSession() {
    m_signalingClient.disconnect();
    if (m_hostingSession && m_signalingServer.isRunning())
        m_signalingServer.stop();
    m_relayVoice.stop();
    m_hostingSession = false;
    m_sessionActive = false;
    m_sessionCode.clear();
    m_serverUrl.clear();
    m_relayHostOnline = false;
    m_relayGuestCount = 0;
    m_lastRemoteSeq = 0;
    m_lastRemoteTime = 0.0;
    m_lastRemoteAt = {};
    m_localStateSeq = 0;
    m_remotePlayingKnown = false;
    m_remoteSpeedKnown = false;
    m_remoteFile.valid = false;
    m_fileVerified = false;
    m_fileVerificationMessage = "Waiting for peer file info";
    m_outgoingQueue.clear();
    m_outgoingShares.clear();
    m_incomingShares.clear();
    m_shareSendActive = false;
    updateStatus();
}

void SyncSession::tick() {
    m_signalingServer.pumpEvents();
    m_signalingClient.pumpEvents();
    m_relayVoice.tick();

    const auto now = std::chrono::steady_clock::now();
    if (m_shareSendActive && now >= m_nextShareSend) {
        onShareSendTick();
        m_nextShareSend = now + m_shareSendInterval;
    }

    // Keep an RTT estimate warm on both sides while a session is up; it feeds
    // the latency compensation applied to incoming state.
    if (m_sessionActive && m_signalingClient.isConnected() &&
        now - m_lastPing >= m_pingInterval) {
        m_signalingClient.sendPing();
        m_lastPing = now;
    }

    if (m_hostingSession && transportConnected()) {
        if (m_player && m_player->isPlaying()) {
            if (now - m_lastSend >= m_sendInterval) {
                sendStateNow();
                m_lastSend = now;
            }
        }
    }
}

void SyncSession::notifyLocalAction() {
    sendStateNow();
}

void SyncSession::sendControlIntent(const std::string& action, double value) {
    if (action.empty())
        return;
    if (!m_sessionActive || m_hostingSession)
        return;
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return;
    m_signalingClient.sendRelayIntent(action, value);
}

void SyncSession::requestPlay() {
    sendControlIntent("play", 0.0);
}

void SyncSession::requestPause() {
    sendControlIntent("pause", 0.0);
}

void SyncSession::requestSeek(double time) {
    m_allowImmediateSeekUntil = std::chrono::steady_clock::now() + m_allowImmediateSeekWindow;
    sendControlIntent("seek", time);
}

void SyncSession::requestSeekDelta(double delta) {
    m_allowImmediateSeekUntil = std::chrono::steady_clock::now() + m_allowImmediateSeekWindow;
    sendControlIntent("seek_delta", delta);
}

void SyncSession::requestSpeed(double speed) {
    sendControlIntent("speed", speed);
}

bool SyncSession::sendChatMessage(const std::string& message) {
    const std::string trimmed = Trim(message);
    if (trimmed.empty())
        return false;
    const std::string prefix = m_nickname.empty() ? std::string("You") : m_nickname;
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return false;
    m_signalingClient.sendRelayChat(prefix + ": " + trimmed);
    return true;
}

bool SyncSession::sendSharedFile(const std::string& path, std::string* outShareId) {
    const std::string trimmed = Trim(path);
    if (trimmed.empty())
        return false;
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return false;

    std::filesystem::path filePath(trimmed);
    if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath))
        return false;
    std::error_code ec;
    const auto rawFileSize = std::filesystem::file_size(filePath, ec);
    if (ec || rawFileSize > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max()))
        return false;
    const int64_t fileSize = static_cast<int64_t>(rawFileSize);
    if (fileSize <= 0)
        return false;
    if (fileSize > m_shareMaxBytes)
        return false;
    const int64_t totalChunks64 = (fileSize + m_shareChunkSize - 1) / m_shareChunkSize;
    if (totalChunks64 <= 0 || totalChunks64 > std::numeric_limits<int>::max())
        return false;

    OutgoingShare share;
    share.file = std::make_shared<std::ifstream>(filePath, std::ios::binary);
    if (!share.file->is_open())
        return false;
    share.id = generateId();
    share.name = filePath.filename().string();
    const std::string trimmedName = Trim(m_nickname);
    share.sender = trimmedName.empty() ? std::string("User") : trimmedName;
    share.size = fileSize;
    share.totalChunks = static_cast<int>(totalChunks64);
    share.chunkIndex = 0;
    if (outShareId)
        *outShareId = share.id;

    m_outgoingShares[share.id] = share;
    m_outgoingQueue.push_back(share.id);
    m_signalingClient.sendShareMeta(share.id, share.name, share.size, share.totalChunks, share.sender);
    if (!m_shareSendActive) {
        m_shareSendActive = true;
        m_nextShareSend = std::chrono::steady_clock::now();
    }
    return true;
}

void SyncSession::startVoiceCall() {
    const std::string state = m_relayVoice.connectionState();
    if (state != "Idle" && state != "Closed" && state != "Failed")
        return;
    if (!voiceAvailable())
        return;
    m_relayVoice.setInputMuted(m_voiceMuted);
    if (m_relayVoice.start())
        m_signalingClient.sendRelayVoiceStart();
}

void SyncSession::stopVoiceCall() {
    if (!m_relayVoice.active() && m_relayVoice.connectionState() == "Idle")
        return;
    if (m_signalingClient.isConnected() && m_signalingClient.isJoined())
        m_signalingClient.sendRelayVoiceStop();
    m_relayVoice.stop();
}

bool SyncSession::voiceActive() const {
    return m_relayVoice.active();
}

bool SyncSession::voiceAvailable() const {
    if (!m_voiceEnabled)
        return false;
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return false;
    if (!transportConnected())
        return false;
    return m_relayHostOnline && m_relayGuestCount == 1;
}

bool SyncSession::voiceMuted() const {
    return m_voiceMuted;
}

std::string SyncSession::voiceState() const {
    return m_relayVoice.connectionState();
}

std::vector<VoiceCaptureDevice> SyncSession::voiceCaptureDevices() const {
    return RelayVoice::captureDevices();
}

bool SyncSession::fileVerified() const {
    return m_fileVerified;
}

std::string SyncSession::fileVerificationMessage() const {
    return m_fileVerificationMessage;
}

std::vector<std::string> SyncSession::interfaceLabels() const {
    std::vector<std::string> labels;
    labels.push_back("Auto (best)");
    const auto interfaces = m_signalingServer.interfaces();
    for (const auto& iface : interfaces) {
        if (iface.name.empty())
            labels.push_back(iface.address);
        else
            labels.push_back(iface.name + " (" + iface.address + ")");
    }
    return labels;
}

std::vector<std::string> SyncSession::interfaceAddresses() const {
    std::vector<std::string> addresses;
    addresses.push_back(std::string());
    const auto interfaces = m_signalingServer.interfaces();
    for (const auto& iface : interfaces)
        addresses.push_back(iface.address);
    return addresses;
}

bool SyncSession::isHost() const {
    return m_hostingSession;
}

bool SyncSession::sessionActive() const {
    return m_sessionActive;
}

bool SyncSession::transportConnected() const {
    if (m_hostingSession)
        return m_relayGuestCount > 0;
    return m_relayHostOnline;
}

std::string SyncSession::transportState() const {
    return transportConnected() ? std::string("Connected") : std::string("Waiting");
}

double SyncSession::syncDriftSeconds() const {
    if (!m_player || m_hostingSession || m_lastRemoteSeq == 0)
        return 0.0;
    double remoteTime = m_lastRemoteTime;
    if (m_remotePlayingKnown && m_remotePlaying && m_lastRemoteAt.time_since_epoch().count() != 0) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - m_lastRemoteAt).count();
        remoteTime += elapsed * (m_remoteSpeedKnown ? m_remoteSpeed : 1.0);
    }
    return m_player->currentTime() - remoteTime;
}

std::string SyncSession::syncConfidenceText() const {
    if (!m_sessionActive)
        return "Sync idle";
    if (!transportConnected())
        return "Waiting for peer";
    if (m_hostingSession)
        return "Sync host";
    if (m_lastRemoteSeq == 0)
        return "Waiting for sync";
    if (m_remotePlayingKnown && !m_remotePlaying)
        return "Peer paused";

    const double drift = syncDriftSeconds();
    const double absDrift = std::abs(drift);
    if (absDrift < 0.12)
        return "Synced";

    char buf[64]{};
    if (absDrift < 0.75)
        std::snprintf(buf, sizeof(buf), "Drifting %.2fs", absDrift);
    else
        std::snprintf(buf, sizeof(buf), "Resyncing %.2fs", absDrift);
    return buf;
}

std::string SyncSession::signalingState() const {
    return m_signalingClient.connectionState();
}

std::string SyncSession::sessionCode() const {
    return m_sessionCode;
}

std::string SyncSession::serverUrl() const {
    return m_serverUrl;
}

std::string SyncSession::statusText() const {
    return m_statusText;
}

std::string SyncSession::hintText() const {
    return m_hintText;
}

void SyncSession::setChatCallback(ChatCallback cb) {
    m_chatCallback = std::move(cb);
}

void SyncSession::setReactionCallback(ReactionCallback cb) {
    m_reactionCallback = std::move(cb);
}

bool SyncSession::sendReaction(const std::string& emoji) {
    if (emoji.empty())
        return false;
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return false;
    m_signalingClient.sendRelayReaction(emoji);
    return true;
}

void SyncSession::setActionCallback(ActionCallback cb) {
    m_actionCallback = std::move(cb);
}

void SyncSession::setStatusCallback(StatusCallback cb) {
    m_statusCallback = std::move(cb);
}

void SyncSession::setShareProgressCallback(ShareProgressCallback cb) {
    m_shareProgressCallback = std::move(cb);
}

void SyncSession::onRelayState(double time, bool playing, double speed, uint64_t seq,
                               double hostLatency) {
    if (m_hostingSession)
        return;
    if (seq <= m_lastRemoteSeq)
        return;
    // Latency compensation: the host captured `time` before the message crossed
    // host->server->us. While playing, its playhead has already advanced by that
    // transit time, so target the extrapolated position instead of the stale one.
    // (Clamp: a wild latency estimate must never inject a big artificial seek.)
    if (playing) {
        const double transit = std::clamp(hostLatency + m_signalingClient.rttSeconds() * 0.5,
                                          0.0, 1.0);
        time += transit * (speed > 0.0 ? speed : 1.0);
    }
    m_lastRemoteSeq = seq;
    m_lastRemoteTime = time;
    m_lastRemoteAt = std::chrono::steady_clock::now();
    notifyRemoteStateChange(playing, speed);
    SyncState s{time, playing, speed, seq};
    const auto now = std::chrono::steady_clock::now();
    const bool allowImmediate = (m_allowImmediateSeekUntil.time_since_epoch().count() != 0) &&
                                (now <= m_allowImmediateSeekUntil);
    m_sync.applyState(s, allowImmediate);
    if (allowImmediate)
        m_allowImmediateSeekUntil = std::chrono::steady_clock::time_point{};
}

void SyncSession::onRelayPeerUpdate(bool hostOnline, int guestCount) {
    const bool prevHost = m_relayHostOnline;
    const int prevGuest = m_relayGuestCount;
    m_relayHostOnline = hostOnline;
    m_relayGuestCount = guestCount;

    if (m_hostingSession) {
        if (guestCount > prevGuest)
            dispatchAction("Guest joined");
        if (guestCount < prevGuest)
            dispatchAction("Guest left");
    } else {
        if (hostOnline && !prevHost)
            dispatchAction("Host joined");
        if (!hostOnline && prevHost)
            dispatchAction("Host left");
    }

    if (m_hostingSession && m_relayGuestCount > 0) {
        sendFileInfoIfReady();
        if (guestCount > prevGuest)
            sendStateNow();
    }

    if (m_relayVoice.active() && !voiceAvailable())
        m_relayVoice.stop();

    updateStatus();
}

void SyncSession::onConnectionStateChanged() {
    if (m_relayVoice.active() && !voiceAvailable())
        m_relayVoice.stop();
    // After an auto-reconnect the guests need fresh state to resync; pushing it
    // on every host-side connect is cheap and idempotent (sendStateNow guards).
    if (m_hostingSession && m_signalingClient.isConnected() && m_signalingClient.isJoined()) {
        sendFileInfoIfReady();
        sendStateNow();
    }
    updateStatus();
}

void SyncSession::onRelayIntent(const std::string& action, double value) {
    applyControlIntent(action, value);
}

void SyncSession::onRelayFileReceived(int64_t size, double duration, const std::string& hash) {
    if (m_hostingSession)
        return;
    m_remoteFile.size = size;
    m_remoteFile.duration = duration;
    m_remoteFile.hash = hash;
    m_remoteFile.valid = true;
    updateFileVerification();
}

void SyncSession::onVoiceStateChanged() {
    dispatchStatus();
}

void SyncSession::onRelayVoiceStartReceived() {
    if (!voiceAvailable())
        return;
    if (!m_relayVoice.active()) {
        m_relayVoice.setInputMuted(m_voiceMuted);
        m_relayVoice.start();
    }
}

void SyncSession::onRelayVoiceStopReceived() {
    m_relayVoice.stop();
}

void SyncSession::onRelayVoiceFrameReceived(const std::vector<uint8_t>& data) {
    if (!voiceAvailable())
        return;
    m_relayVoice.pushIncomingFrame(data);
}

void SyncSession::onShareMetaReceived(const std::string& id, const std::string& name, int64_t size, int totalChunks, const std::string& sender) {
    if (id.empty() || name.empty() || size <= 0 || totalChunks <= 0)
        return;
    if (size > m_shareMaxBytes)
        return;
    if (m_incomingShares.find(id) != m_incomingShares.end())
        return;
    const std::filesystem::path finalPath = resolveSharePath(name);
    if (finalPath.empty())
        return;
    const std::filesystem::path tempPath = finalPath.string() + ".part";
    auto file = std::make_shared<std::ofstream>(tempPath, std::ios::binary);
    if (!file->is_open())
        return;
    IncomingShare share;
    share.file = file;
    share.tempPath = tempPath;
    share.finalPath = finalPath;
    share.name = name;
    share.sender = Trim(sender).empty() ? std::string("Peer") : Trim(sender);
    share.size = size;
    share.totalChunks = totalChunks;
    m_incomingShares[id] = share;
    if (m_shareProgressCallback) {
        const auto utf8 = finalPath.u8string();
        std::string path(utf8.begin(), utf8.end());
        m_shareProgressCallback(id, share.name, share.sender, path, share.size, 0,
                                false, false, false);
    }
}

void SyncSession::onShareChunkReceived(const std::string& id, int index, const std::vector<uint8_t>& data) {
    if (id.empty() || data.empty())
        return;
    auto it = m_incomingShares.find(id);
    if (it == m_incomingShares.end())
        return;
    IncomingShare& share = it->second;
    if (!share.file || !share.file->is_open())
        return;
    if (index != share.expectedChunk ||
        share.receivedChunks >= share.totalChunks ||
        share.received + static_cast<int64_t>(data.size()) > share.size) {
        failIncomingShare(id);
        return;
    }
    share.file->write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!*share.file) {
        failIncomingShare(id);
        return;
    }
    share.sha1.update(data.data(), data.size());
    share.received += static_cast<int64_t>(data.size());
    share.receivedChunks += 1;
    share.expectedChunk += 1;
    if (m_shareProgressCallback) {
        const auto utf8 = share.finalPath.u8string();
        std::string path(utf8.begin(), utf8.end());
        m_shareProgressCallback(id, share.name, share.sender, path, share.size, share.received,
                                false, false, false);
    }
}

void SyncSession::onShareDoneReceived(const std::string& id, const std::string& sha1) {
    if (id.empty())
        return;
    auto it = m_incomingShares.find(id);
    if (it == m_incomingShares.end())
        return;
    IncomingShare share = it->second;
    const bool complete = share.received == share.size &&
                          share.receivedChunks == share.totalChunks;
    const std::string computedSha1 = Sha1Hex(share.sha1.finalize());
    const bool hashOk = sha1.empty() || computedSha1 == sha1;
    if (!complete || !hashOk) {
        failIncomingShare(id);
        return;
    }
    m_incomingShares.erase(it);
    if (share.file && share.file->is_open())
        share.file->close();
    bool renamed = false;
    if (!share.tempPath.empty() && !share.finalPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(share.finalPath, ec);
        ec.clear();
        std::filesystem::rename(share.tempPath, share.finalPath, ec);
        renamed = !ec;
    }
    if (!renamed) {
        std::error_code ec;
        std::filesystem::remove(share.tempPath, ec);
        if (m_shareProgressCallback) {
            const auto utf8 = share.finalPath.u8string();
            std::string path(utf8.begin(), utf8.end());
            m_shareProgressCallback(id, share.name, share.sender, path, share.size, share.received,
                                    false, false, true);
        }
        return;
    }
    std::string filePath;
    if (!share.finalPath.empty()) {
        const auto utf8 = share.finalPath.u8string();
        filePath.assign(utf8.begin(), utf8.end());
    } else if (!share.tempPath.empty()) {
        const auto utf8 = share.tempPath.u8string();
        filePath.assign(utf8.begin(), utf8.end());
    }
    std::string note = "FILE|" + share.sender + "|" + share.name + "|" + filePath + "|" + std::to_string(share.size);
    if (m_shareProgressCallback) {
        m_shareProgressCallback(id, share.name, share.sender, filePath, share.size, share.size,
                                false, true, false);
    } else {
        dispatchChat(note);
    }
}

void SyncSession::failIncomingShare(const std::string& id) {
    auto it = m_incomingShares.find(id);
    if (it == m_incomingShares.end())
        return;

    IncomingShare share = it->second;
    m_incomingShares.erase(it);
    if (share.file && share.file->is_open())
        share.file->close();
    if (!share.tempPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(share.tempPath, ec);
    }
    if (m_shareProgressCallback) {
        std::string path;
        if (!share.finalPath.empty()) {
            const auto utf8 = share.finalPath.u8string();
            path.assign(utf8.begin(), utf8.end());
        }
        m_shareProgressCallback(id, share.name, share.sender, path, share.size, share.received,
                                false, false, true);
    }
}

void SyncSession::onShareSendTick() {
    if (m_outgoingQueue.empty()) {
        m_shareSendActive = false;
        return;
    }
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return;
    const std::string id = m_outgoingQueue.front();
    m_outgoingQueue.pop_front();
    auto it = m_outgoingShares.find(id);
    if (it == m_outgoingShares.end())
        return;
    OutgoingShare& share = it->second;
    if (!share.file || !share.file->is_open()) {
        if (m_shareProgressCallback)
            m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                    share.size, share.sent, true, false, true);
        m_outgoingShares.erase(it);
        return;
    }
    if (share.sent >= share.size || share.chunkIndex >= share.totalChunks) {
        const bool complete = share.sent == share.size &&
                              share.chunkIndex == share.totalChunks;
        share.file->close();
        if (complete) {
            const std::string sha1 = Sha1Hex(share.sha1.finalize());
            m_signalingClient.sendShareDone(share.id, sha1);
            if (m_shareProgressCallback)
                m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                        share.size, share.size, true, true, false);
        } else if (m_shareProgressCallback) {
            m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                    share.size, std::min<int64_t>(share.sent, share.size),
                                    true, false, true);
        }
        m_outgoingShares.erase(it);
        return;
    }

    std::vector<uint8_t> chunk;
    chunk.resize(static_cast<size_t>(m_shareChunkSize));
    share.file->read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize readBytes = share.file->gcount();
    if (readBytes <= 0) {
        const bool complete = share.sent == share.size &&
                              share.chunkIndex == share.totalChunks &&
                              share.file->eof();
        share.file->close();
        if (complete) {
            m_signalingClient.sendShareDone(share.id);
            if (m_shareProgressCallback)
                m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                        share.size, share.size, true, true, false);
        } else if (m_shareProgressCallback) {
            m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                    share.size, share.sent, true, false, true);
        }
        m_outgoingShares.erase(it);
        return;
    }
    const int64_t nextSent = share.sent + static_cast<int64_t>(readBytes);
    if (nextSent > share.size || share.chunkIndex >= share.totalChunks) {
        share.file->close();
        if (m_shareProgressCallback)
            m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                    share.size, std::min<int64_t>(nextSent, share.size),
                                    true, false, true);
        m_outgoingShares.erase(it);
        return;
    }
    chunk.resize(static_cast<size_t>(readBytes));
    share.sha1.update(chunk.data(), chunk.size());
    m_signalingClient.sendShareChunk(share.id, share.chunkIndex, chunk);
    share.sent = nextSent;
    if (m_shareProgressCallback)
        m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                share.size, share.sent, true, false, false);
    share.chunkIndex += 1;
    if (share.file->eof()) {
        share.file->close();
        const std::string sha1 = Sha1Hex(share.sha1.finalize());
        m_signalingClient.sendShareDone(share.id, sha1);
        if (m_shareProgressCallback)
            m_shareProgressCallback(share.id, share.name, share.sender, std::string(),
                                    share.size, share.size, true, true, false);
        m_outgoingShares.erase(it);
        return;
    }
    m_outgoingQueue.push_back(id);
}

void SyncSession::updateStatus() {
    const std::string conn = m_signalingClient.connectionState();
    if (!m_sessionActive) {
        m_statusText = "Idle";
        m_hintText.clear();
    } else if (conn == "Invalid URL") {
        m_statusText = "Idle";
        m_hintText = "Invalid server URL.";
    } else if (conn == "Connecting") {
        m_statusText = "Connecting";
        m_hintText = "Connecting to server...";
    } else if (conn == "Disconnected") {
        m_statusText = "Disconnected";
        m_hintText = "Could not reach server.";
    } else if (!m_signalingClient.isJoined()) {
        m_statusText = "Joining";
        m_hintText = "Joining session...";
    } else if (m_hostingSession) {
        m_statusText = m_relayGuestCount > 0 ? std::string("Hosting - Connected")
                                             : std::string("Hosting - Waiting");
        m_hintText = m_relayGuestCount > 0 ? std::string() : std::string("Waiting for guest...");
    } else {
        m_statusText = transportConnected() ? std::string("Connected")
                                            : std::string("Waiting");
        m_hintText = transportConnected() ? std::string() : std::string("Waiting for host...");
    }
    dispatchStatus();
}

void SyncSession::sendStateNow() {
    if (!m_player)
        return;
    if (!m_hostingSession)
        return;
    SyncState s = m_sync.captureState();
    s.seq = ++m_localStateSeq;
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return;
    if (m_hostingSession) {
        if (!m_localFile.valid || m_relayGuestCount <= 0)
            return;
    } else {
        if (!m_fileVerified || !m_relayHostOnline)
            return;
    }
    m_signalingClient.sendRelayState(s.time, s.playing, s.speed, s.seq,
                                     m_signalingClient.rttSeconds() * 0.5);
}

void SyncSession::applyControlIntent(const std::string& action, double value) {
    if (!m_player)
        return;
    if (!m_hostingSession)
        return;
    if (!m_allowGuestControl)
        return;
    if (!m_localFile.valid)
        return;

    const std::string act = ToLower(Trim(action));
    if (act == "play") {
        m_player->play();
        dispatchAction("Play");
    } else if (act == "pause") {
        m_player->pause();
        dispatchAction("Pause");
    } else if (act == "seek") {
        m_player->seek(value);
    } else if (act == "seek_delta") {
        m_player->seek(m_player->currentTime() + value);
    } else if (act == "speed") {
        m_player->setSpeed(value);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Speed %.2fx", value);
        dispatchAction(buf);
    } else {
        return;
    }
    sendStateNow();
}

void SyncSession::updateFileVerification() {
    if (m_hostingSession) {
        if (!m_localFile.valid) {
            m_fileVerified = false;
            m_fileVerificationMessage = "Local file not loaded";
        } else {
            m_fileVerified = true;
            m_fileVerificationMessage = "Host file ready";
        }
        dispatchStatus();
        return;
    }
    if (!m_localFile.valid) {
        m_fileVerified = false;
        m_fileVerificationMessage = "Local file not loaded";
    } else if (!m_remoteFile.valid) {
        m_fileVerified = false;
        m_fileVerificationMessage = "Waiting for peer file info";
    } else {
        const bool sizeMatch = m_localFile.size == m_remoteFile.size;
        const bool hashMatch = !m_localFile.hash.empty() && m_localFile.hash == m_remoteFile.hash;
        bool durationMatch = true;
        if (m_localFile.duration > 0.0 && m_remoteFile.duration > 0.0)
            durationMatch = std::abs(m_localFile.duration - m_remoteFile.duration) < 0.5;
        if (sizeMatch && hashMatch && durationMatch) {
            m_fileVerified = true;
            m_fileVerificationMessage = "Files match";
        } else {
            m_fileVerified = false;
            m_fileVerificationMessage = "File mismatch";
        }
    }
    dispatchStatus();
}

void SyncSession::sendFileInfoIfReady() {
    if (!m_localFile.valid)
        return;
    if (!m_signalingClient.isConnected() || !m_signalingClient.isJoined())
        return;
    if (m_hostingSession && m_relayGuestCount > 0) {
        m_signalingClient.sendRelayFile(m_localFile.size, m_localFile.duration, m_localFile.hash);
    }
}

void SyncSession::dispatchChat(const std::string& message) {
    if (m_chatCallback)
        m_chatCallback(message);
}

void SyncSession::dispatchAction(const std::string& message) {
    if (m_actionCallback)
        m_actionCallback(message);
}

void SyncSession::dispatchStatus() {
    if (m_statusCallback)
        m_statusCallback();
}

void SyncSession::notifyRemoteStateChange(bool playing, double speed) {
    if (!m_remotePlayingKnown) {
        m_remotePlayingKnown = true;
        m_remotePlaying = playing;
    } else if (playing != m_remotePlaying) {
        dispatchAction(playing ? "Play" : "Pause");
        m_remotePlaying = playing;
    }

    if (!m_remoteSpeedKnown) {
        m_remoteSpeedKnown = true;
        m_remoteSpeed = speed;
    } else if (std::abs(speed - m_remoteSpeed) > 0.001) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Speed %.2fx", speed);
        dispatchAction(buf);
        m_remoteSpeed = speed;
    }
}

std::filesystem::path SyncSession::resolveSharePath(const std::string& name) const {
    std::filesystem::path baseDir = downloadsDir();
    if (baseDir.empty())
        return {};
    std::string safeName = sanitizeShareFileName(name);
    if (safeName.empty())
        safeName = "shared-file";
    std::filesystem::path candidate = baseDir / safeName;
    if (!std::filesystem::exists(candidate))
        return candidate;

    std::filesystem::path namePath(safeName);
    std::string baseName = namePath.stem().string();
    if (baseName.empty())
        baseName = safeName;
    const std::string suffix = namePath.extension().string();
    for (int i = 1; i < 1000; ++i) {
        std::string numbered = baseName + " (" + std::to_string(i) + ")";
        if (!suffix.empty())
            numbered += suffix;
        candidate = baseDir / numbered;
        if (!std::filesystem::exists(candidate))
            return candidate;
    }
    return baseDir / generateId();
}

std::string SyncSession::formatBytes(int64_t bytes) const {
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    char buf[64];
    if (bytes >= static_cast<int64_t>(gb)) {
        std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / gb);
        return buf;
    }
    if (bytes >= static_cast<int64_t>(mb)) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / mb);
        return buf;
    }
    if (bytes >= static_cast<int64_t>(kb)) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / kb);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    return buf;
}
