#pragma once

#include <deque>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class ChatLineKind {
    Text,
    System,
    File
};

enum class ChatLineStatus {
    None,
    Sending,
    Receiving,
    Sent,
    Failed,
    Received
};

struct ChatLine {
    std::string who;
    std::string text;
    std::string time;
    std::string filePath;
    ChatLineKind kind = ChatLineKind::Text;
    ChatLineStatus status = ChatLineStatus::None;
    std::string fileName;
    int64_t fileSize = 0;
    int64_t fileTransferred = 0;
    std::string transferId;
    std::string retryPath;
    // Runtime-only: when this line first appeared on screen (for the one-shot
    // entrance animation). Negative = not yet stamped by the chat draw loop.
    double appearAt = -1.0;
};

struct EventToast {
    std::string text;
    float ttl = 0.0f;
    // Runtime-only: stamped by the renderer for the slide-in animation.
    double addedAt = -1.0;
};

// One entry of the "Continue watching" list shown on the idle screen.
struct RecentMedia {
    std::string path;      // UTF-8 absolute path
    double position = 0.0; // last playback position (seconds)
    double duration = 0.0; // media duration if known (seconds)
    int64_t lastWatched = 0; // unix time of last playback
};

struct AppState {
    bool showSession = false;
    bool showChat = false;
    bool showCall = false;
    bool showSubs = false;
    bool showSettings = false;
    bool showEmoji = false;
    bool dirty = false;
    bool sessionDocked = true;
    bool chatDocked = true;
    bool subsDocked = true;
    bool settingsDocked = true;
    bool sidePanels = false;
    bool glassPanels = true;
    bool dynamicAccent = true;
    bool useSystemProxy = false;
    bool allowGuestControl = true;
    bool autoPromote = false;
    bool lanMode = false;
    bool showAdvancedSession = false;
    bool voiceEnabled = false;
    bool voiceMuted = true;
    bool openWithRegistered = false;
    bool fileLoggingEnabled = false;

    std::string sessionStatus = "Idle";
    std::string sessionHint;
    std::string fileStatus;
    bool fileVerified = false;

    char nickname[64]{};
    char sessionPassword[64]{};
    char joinCode[64]{};
    char serverUrl[256]{};
    char filePath[512]{};
    char chatInput[256]{};
    char preferredInterface[128]{};
    char openWithExePath[512]{};

    int signalingPort = 0;
    int subtitleTrack = 0;
    int audioTrack = 0;
    float subtitleOffset = 0.0f;
    float subtitleDelay = 0.0f;
    bool subtitlesEnabled = true;
    char subtitleFont[128]{};
    // OpenSubtitles online search (user-supplied API key from opensubtitles.com).
    char openSubsApiKey[128]{};
    char openSubsLangs[32] = "en";
    float subtitleFontSize = 36.0f;
    float subtitleColor[3]{1.0f, 1.0f, 1.0f};
    float subtitleOpacity = 1.0f;
    float subtitleBorderSize = 2.0f;
    float subtitleShadowOffset = 1.0f;
    float subtitleSpacing = 0.0f;
    float subtitlePos = 90.0f;
    float subtitleMarginX = 0.0f;
    float subtitleMarginY = 0.0f;
    int subtitleAlignX = 1;
    int subtitleAlignY = 2;
    bool subtitleBold = false;
    bool subtitleItalic = false;
    float voiceVolume = 70.0f;
    float voiceInputThreshold = 0.0f;
    int voiceCaptureDeviceIndex = -1;
    float accentColor[3]{0.40f, 0.58f, 0.98f};

    float videoBrightness = 0.0f;
    float videoContrast = 0.0f;
    float videoSaturation = 0.0f;
    float videoGamma = 0.0f;
    float videoHue = 0.0f;
    int videoToneMapping = 0;
    float videoToneMappingParam = 0.0f;
    float videoTargetPeak = 300.0f;
    std::vector<std::string> videoShaders;

    float sessionPos[2]{0.0f, 0.0f};
    float sessionSize[2]{0.0f, 0.0f};
    float chatPos[2]{0.0f, 0.0f};
    float chatSize[2]{0.0f, 0.0f};
    float subsPos[2]{0.0f, 0.0f};
    float subsSize[2]{0.0f, 0.0f};
    float settingsPos[2]{0.0f, 0.0f};
    float settingsSize[2]{0.0f, 0.0f};
    float sessionDockedH = 0.0f;
    float chatDockedH = 0.0f;
    float subsDockedH = 0.0f;
    float settingsDockedH = 0.0f;
    float dockPanelW = 0.0f;
    int lastUiW = 0;
    int lastUiH = 0;

    std::deque<ChatLine> chat;
    std::deque<EventToast> events;
    std::vector<RecentMedia> recentMedia; // most-recent first, capped at 8
};

std::filesystem::path config_path();
void load_config(AppState& app, float* volume = nullptr, float* speed = nullptr);
void save_config(const AppState& app, float volume, float speed);

std::string computePartialHashHexUtf8(const std::string& path);
std::string format_time(double seconds);
