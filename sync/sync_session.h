#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/playback_controller.h"
#include "../sync/sync_manager.h"
#include "../sync/sync_state.h"
#include "../core/sha1.h"
#include "../network/relay_voice.h"
#include "../network/signaling_client.h"
#include "../network/signaling_server.h"

class SyncSession {
public:
    explicit SyncSession(PlaybackController* player);

    void setNickname(const std::string& nickname);
    // HTTP proxy for the signaling connection ("" = direct).
    void setNetworkProxy(const std::string& url);
    void setSignalingPort(int port);
    void setSessionPassword(const std::string& password);
    void setSelectedInterfaceAddress(const std::string& address);
    void setAutoPromote(bool enabled);
    void setVoiceEnabled(bool enabled);
    void setVoiceMuted(bool muted);
    void setVoiceVolume(float volume);
    void setVoiceInputThreshold(float threshold);
    void setVoiceCaptureDeviceIndex(int index);
    void setLocalFileInfo(int64_t size, double duration, const std::string& hash);
    void setAllowGuestControl(bool enabled);

    void startHostSession();
    void joinSession(const std::string& url, const std::string& code);
    void disconnectSession();

    void tick();
    void notifyLocalAction();
    void sendControlIntent(const std::string& action, double value);
    void requestPlay();
    void requestPause();
    void requestSeek(double time);
    void requestSeekDelta(double delta);
    void requestSpeed(double speed);
    bool sendChatMessage(const std::string& message);
    // Ephemeral emoji reaction, relayed to every peer in the session.
    bool sendReaction(const std::string& emoji);
    // Host shares a network stream URL with the session; guests get a consent
    // prompt via the open-url callback. Remembered and re-sent to late joiners.
    void shareOpenUrl(const std::string& url);
    bool sendSharedFile(const std::string& path, std::string* outShareId = nullptr);
    void startVoiceCall();
    void stopVoiceCall();
    bool voiceActive() const;
    bool voiceAvailable() const;
    bool voiceMuted() const;
    std::string voiceState() const;
    std::vector<VoiceCaptureDevice> voiceCaptureDevices() const;

    bool fileVerified() const;
    std::string fileVerificationMessage() const;
    std::vector<std::string> interfaceLabels() const;
    std::vector<std::string> interfaceAddresses() const;

    bool isHost() const;
    bool sessionActive() const;
    bool transportConnected() const;
    std::string transportState() const;
    double syncDriftSeconds() const;
    std::string syncConfidenceText() const;
    std::string signalingState() const;
    std::string sessionCode() const;
    std::string serverUrl() const;
    std::string statusText() const;
    std::string hintText() const;

    using ChatCallback = std::function<void(const std::string&)>;
    using ReactionCallback = std::function<void(const std::string&)>;
    using OpenUrlCallback = std::function<void(const std::string&)>;
    using ActionCallback = std::function<void(const std::string&)>;
    using StatusCallback = std::function<void()>;
    using ShareProgressCallback = std::function<void(const std::string& id,
                                                     const std::string& name,
                                                     const std::string& sender,
                                                     const std::string& path,
                                                     int64_t size,
                                                     int64_t transferred,
                                                     bool outgoing,
                                                     bool done,
                                                     bool failed)>;

    void setChatCallback(ChatCallback cb);
    void setReactionCallback(ReactionCallback cb);
    void setOpenUrlCallback(OpenUrlCallback cb);
    void setActionCallback(ActionCallback cb);
    void setStatusCallback(StatusCallback cb);
    void setShareProgressCallback(ShareProgressCallback cb);

private:
    void onRelayState(double time, bool playing, double speed, uint64_t seq, double hostLatency);
    void onRelayPeerUpdate(bool hostOnline, int guestCount);
    void onConnectionStateChanged();
    void onRelayIntent(const std::string& action, double value);
    void onVoiceStateChanged();
    void onRelayVoiceStartReceived();
    void onRelayVoiceStopReceived();
    void onRelayVoiceFrameReceived(const std::vector<uint8_t>& data);
    void onRelayFileReceived(int64_t size, double duration, const std::string& hash);
    void onShareMetaReceived(const std::string& id, const std::string& name, int64_t size, int totalChunks, const std::string& sender);
    void onShareChunkReceived(const std::string& id, int index, const std::vector<uint8_t>& data);
    void onShareDoneReceived(const std::string& id, const std::string& sha1);
    void onShareSendTick();
    void failIncomingShare(const std::string& id);

    void dispatchChat(const std::string& message);
    void dispatchAction(const std::string& message);
    void dispatchStatus();
    void updateStatus();
    void sendStateNow();
    void applyControlIntent(const std::string& action, double value);
    void notifyRemoteStateChange(bool playing, double speed);
    std::filesystem::path resolveSharePath(const std::string& name) const;
    std::string formatBytes(int64_t bytes) const;
    void updateFileVerification();
    void sendFileInfoIfReady();

    struct FileInfo {
        int64_t size = 0;
        double duration = 0.0;
        std::string hash;
        bool valid = false;
    };

    struct IncomingShare {
        std::shared_ptr<std::ofstream> file;
        std::filesystem::path tempPath;
        std::filesystem::path finalPath;
        std::string name;
        std::string sender;
        int64_t size = 0;
        int64_t received = 0;
        int totalChunks = 0;
        int receivedChunks = 0;
        int expectedChunk = 0;
        Sha1 sha1;
    };

    struct OutgoingShare {
        std::shared_ptr<std::ifstream> file;
        std::string id;
        std::string name;
        std::string sender;
        int64_t size = 0;
        int64_t sent = 0;
        int totalChunks = 0;
        int chunkIndex = 0;
        Sha1 sha1;
    };

    PlaybackController* m_player = nullptr;
    SyncManager m_sync;

    SignalingServer m_signalingServer;
    SignalingClient m_signalingClient;
    RelayVoice m_relayVoice;

    bool m_hostingSession = false;
    bool m_sessionActive = false;

    std::string m_sessionCode;
    std::string m_serverUrl;
    std::string m_sessionPassword;
    std::string m_selectedInterfaceAddress;
    std::string m_nickname;
    int m_signalingPort = 49152;
    bool m_autoPromote = false;

    FileInfo m_localFile;
    FileInfo m_remoteFile;
    bool m_fileVerified = false;
    std::string m_fileVerificationMessage;
    bool m_allowGuestControl = true;
    bool m_voiceEnabled = false;
    bool m_voiceMuted = true;

    std::unordered_map<std::string, IncomingShare> m_incomingShares;
    std::unordered_map<std::string, OutgoingShare> m_outgoingShares;
    std::deque<std::string> m_outgoingQueue;
    bool m_shareSendActive = false;
    int m_shareChunkSize = 128 * 1024;
    int64_t m_shareMaxBytes = 2LL * 1024 * 1024 * 1024;
    std::chrono::steady_clock::time_point m_nextShareSend{};
    std::chrono::milliseconds m_shareSendInterval{2};

    bool m_relayHostOnline = false;
    int m_relayGuestCount = 0;
    uint64_t m_localStateSeq = 0;
    uint64_t m_lastRemoteSeq = 0;
    double m_lastRemoteTime = 0.0;
    std::chrono::steady_clock::time_point m_lastRemoteAt{};
    bool m_remotePlayingKnown = false;
    bool m_remotePlaying = false;
    bool m_remoteSpeedKnown = false;
    double m_remoteSpeed = 0.0;

    std::chrono::steady_clock::time_point m_lastSend{};
    std::chrono::milliseconds m_sendInterval{1500};
    std::chrono::steady_clock::time_point m_lastPing{};
    std::chrono::milliseconds m_pingInterval{2000};
    std::chrono::steady_clock::time_point m_allowImmediateSeekUntil{};
    std::chrono::milliseconds m_allowImmediateSeekWindow{1500};

    std::string m_statusText;
    std::string m_hintText;

    // Network URL currently shared with the session (host side); re-sent to
    // guests that join later. Cleared when a local file takes over.
    std::string m_currentShareUrl;

    ChatCallback m_chatCallback;
    ReactionCallback m_reactionCallback;
    OpenUrlCallback m_openUrlCallback;
    ActionCallback m_actionCallback;
    StatusCallback m_statusCallback;
    ShareProgressCallback m_shareProgressCallback;
};
