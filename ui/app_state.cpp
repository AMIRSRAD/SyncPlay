#include "app_state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <nlohmann/json.hpp>

#include "../core/sha1.h"
#include "../core/utf.h"

using nlohmann::json;

std::filesystem::path config_path() {
    char* appdata = nullptr;
    size_t len = 0;
    _dupenv_s(&appdata, &len, "APPDATA");
    std::filesystem::path base = appdata ? appdata : std::filesystem::current_path();
    if (appdata)
        free(appdata);
    std::filesystem::path dir = base / "SyncPlay";
    std::filesystem::create_directories(dir);
    return dir / "config.json";
}

static void get_str(const json& j, const char* key, char* dst, size_t dstSize) {
    if (!j.contains(key))
        return;
    const std::string value = j.value(key, "");
    std::snprintf(dst, dstSize, "%s", value.c_str());
}

static void get_color3(const json& j, const char* key, float out[3]) {
    if (!j.contains(key))
        return;
    const json& value = j.at(key);
    if (!value.is_array() || value.size() < 3)
        return;
    for (size_t i = 0; i < 3; ++i) {
        if (value[i].is_number()) {
            const float v = value[i].get<float>();
            out[i] = std::clamp(v, 0.0f, 1.0f);
        }
    }
}

static void get_str_array(const json& j, const char* key, std::vector<std::string>& out) {
    if (!j.contains(key))
        return;
    const json& value = j.at(key);
    if (!value.is_array())
        return;
    out.clear();
    for (const auto& item : value) {
        if (item.is_string())
            out.push_back(item.get<std::string>());
    }
}

void load_config(AppState& app, float* volume, float* speed) {
    const auto path = config_path();
    if (!std::filesystem::exists(path))
        return;
    std::ifstream in(path);
    if (!in)
        return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    get_str(j, "nickname", app.nickname, sizeof(app.nickname));
    get_str(j, "sessionPassword", app.sessionPassword, sizeof(app.sessionPassword));
    get_str(j, "joinCode", app.joinCode, sizeof(app.joinCode));
    get_str(j, "serverUrl", app.serverUrl, sizeof(app.serverUrl));
    get_str(j, "preferredInterface", app.preferredInterface, sizeof(app.preferredInterface));
    get_str(j, "openWithExePath", app.openWithExePath, sizeof(app.openWithExePath));
    get_color3(j, "accentColor", app.accentColor);

    app.signalingPort = j.value("signalingPort", app.signalingPort);
    app.autoPromote = j.value("autoPromote", app.autoPromote);
    app.allowGuestControl = j.value("allowGuestControl", app.allowGuestControl);
    app.sidePanels = j.value("sidePanels", app.sidePanels);
    app.glassPanels = j.value("glassPanels", app.glassPanels);
    app.chatPos[0] = j.value("chatPosX", app.chatPos[0]);
    app.chatPos[1] = j.value("chatPosY", app.chatPos[1]);
    app.chatSize[0] = j.value("chatSizeW", app.chatSize[0]);
    app.chatSize[1] = j.value("chatSizeH", app.chatSize[1]);
    app.dockPanelW = j.value("dockPanelW", app.dockPanelW);
    app.showSubs = j.value("showSubs", app.showSubs);
    app.subsDocked = j.value("subsDocked", app.subsDocked);
    app.subsPos[0] = j.value("subsPosX", app.subsPos[0]);
    app.subsPos[1] = j.value("subsPosY", app.subsPos[1]);
    app.subsSize[0] = j.value("subsSizeW", app.subsSize[0]);
    app.subsSize[1] = j.value("subsSizeH", app.subsSize[1]);
    app.subsDockedH = j.value("subsDockedH", app.subsDockedH);
    app.subtitleTrack = j.value("subtitleTrack", app.subtitleTrack);
    app.audioTrack = j.value("audioTrack", app.audioTrack);
    app.subtitleOffset = j.value("subtitleOffset", app.subtitleOffset);
    app.subtitleDelay = j.value("subtitleDelay", app.subtitleDelay);
    app.subtitlesEnabled = j.value("subtitlesEnabled", app.subtitlesEnabled);
    get_str(j, "subtitleFont", app.subtitleFont, sizeof(app.subtitleFont));
    app.subtitleFontSize = j.value("subtitleFontSize", app.subtitleFontSize);
    get_color3(j, "subtitleColor", app.subtitleColor);
    app.subtitleOpacity = j.value("subtitleOpacity", app.subtitleOpacity);
    app.subtitleBorderSize = j.value("subtitleBorderSize", app.subtitleBorderSize);
    app.subtitleShadowOffset = j.value("subtitleShadowOffset", app.subtitleShadowOffset);
    app.subtitleSpacing = j.value("subtitleSpacing", app.subtitleSpacing);
    app.subtitlePos = j.value("subtitlePos", app.subtitlePos);
    app.subtitleMarginX = j.value("subtitleMarginX", app.subtitleMarginX);
    app.subtitleMarginY = j.value("subtitleMarginY", app.subtitleMarginY);
    app.subtitleAlignX = j.value("subtitleAlignX", app.subtitleAlignX);
    app.subtitleAlignY = j.value("subtitleAlignY", app.subtitleAlignY);
    app.subtitleBold = j.value("subtitleBold", app.subtitleBold);
    app.subtitleItalic = j.value("subtitleItalic", app.subtitleItalic);
    app.voiceVolume = j.value("voiceVolume", app.voiceVolume);
    app.voiceInputThreshold = j.value("voiceInputThreshold", app.voiceInputThreshold);
    app.voiceCaptureDeviceIndex = j.value("voiceCaptureDeviceIndex", app.voiceCaptureDeviceIndex);
    app.voiceEnabled = j.value("voiceEnabled", app.voiceEnabled);
    app.openWithRegistered = j.value("openWithRegistered", app.openWithRegistered);
    app.fileLoggingEnabled = j.value("fileLoggingEnabled", app.fileLoggingEnabled);
    app.videoBrightness = j.value("videoBrightness", app.videoBrightness);
    app.videoContrast = j.value("videoContrast", app.videoContrast);
    app.videoSaturation = j.value("videoSaturation", app.videoSaturation);
    app.videoGamma = j.value("videoGamma", app.videoGamma);
    app.videoHue = j.value("videoHue", app.videoHue);
    app.videoToneMapping = j.value("videoToneMapping", app.videoToneMapping);
    app.videoToneMappingParam = j.value("videoToneMappingParam", app.videoToneMappingParam);
    app.videoTargetPeak = j.value("videoTargetPeak", app.videoTargetPeak);
    get_str_array(j, "videoShaders", app.videoShaders);
    if (volume)
        *volume = j.value("volume", *volume);
    if (speed)
        *speed = j.value("speed", *speed);
}

void save_config(const AppState& app, float volume, float speed) {
    json j;
    j["nickname"] = app.nickname;
    j["sessionPassword"] = app.sessionPassword;
    j["joinCode"] = app.joinCode;
    j["serverUrl"] = app.serverUrl;
    j["preferredInterface"] = app.preferredInterface;
    j["openWithExePath"] = app.openWithExePath;
    j["signalingPort"] = app.signalingPort;
    j["autoPromote"] = app.autoPromote;
    j["allowGuestControl"] = app.allowGuestControl;
    j["sidePanels"] = app.sidePanels;
    j["glassPanels"] = app.glassPanels;
    j["chatPosX"] = app.chatPos[0];
    j["chatPosY"] = app.chatPos[1];
    j["chatSizeW"] = app.chatSize[0];
    j["chatSizeH"] = app.chatSize[1];
    j["dockPanelW"] = app.dockPanelW;
    j["showSubs"] = app.showSubs;
    j["subsDocked"] = app.subsDocked;
    j["subsPosX"] = app.subsPos[0];
    j["subsPosY"] = app.subsPos[1];
    j["subsSizeW"] = app.subsSize[0];
    j["subsSizeH"] = app.subsSize[1];
    j["subsDockedH"] = app.subsDockedH;
    j["subtitleTrack"] = app.subtitleTrack;
    j["audioTrack"] = app.audioTrack;
    j["subtitleOffset"] = app.subtitleOffset;
    j["subtitleDelay"] = app.subtitleDelay;
    j["subtitlesEnabled"] = app.subtitlesEnabled;
    j["subtitleFont"] = app.subtitleFont;
    j["subtitleFontSize"] = app.subtitleFontSize;
    j["subtitleColor"] = {app.subtitleColor[0], app.subtitleColor[1], app.subtitleColor[2]};
    j["subtitleOpacity"] = app.subtitleOpacity;
    j["subtitleBorderSize"] = app.subtitleBorderSize;
    j["subtitleShadowOffset"] = app.subtitleShadowOffset;
    j["subtitleSpacing"] = app.subtitleSpacing;
    j["subtitlePos"] = app.subtitlePos;
    j["subtitleMarginX"] = app.subtitleMarginX;
    j["subtitleMarginY"] = app.subtitleMarginY;
    j["subtitleAlignX"] = app.subtitleAlignX;
    j["subtitleAlignY"] = app.subtitleAlignY;
    j["subtitleBold"] = app.subtitleBold;
    j["subtitleItalic"] = app.subtitleItalic;
    j["voiceVolume"] = app.voiceVolume;
    j["voiceInputThreshold"] = app.voiceInputThreshold;
    j["voiceCaptureDeviceIndex"] = app.voiceCaptureDeviceIndex;
    j["voiceEnabled"] = app.voiceEnabled;
    j["openWithRegistered"] = app.openWithRegistered;
    j["fileLoggingEnabled"] = app.fileLoggingEnabled;
    j["videoBrightness"] = app.videoBrightness;
    j["videoContrast"] = app.videoContrast;
    j["videoSaturation"] = app.videoSaturation;
    j["videoGamma"] = app.videoGamma;
    j["videoHue"] = app.videoHue;
    j["videoToneMapping"] = app.videoToneMapping;
    j["videoToneMappingParam"] = app.videoToneMappingParam;
    j["videoTargetPeak"] = app.videoTargetPeak;
    j["videoShaders"] = app.videoShaders;
    j["accentColor"] = {app.accentColor[0], app.accentColor[1], app.accentColor[2]};
    j["volume"] = volume;
    j["speed"] = speed;

    const auto path = config_path();
    std::ofstream out(path);
    if (!out)
        return;
    out << j.dump(2);
}

std::string computePartialHashHexUtf8(const std::string& path) {
#ifdef _WIN32
    // Open via a wide path so UTF-8 (non-ASCII) media paths hash correctly;
    // a narrow std::ifstream interprets the bytes in the active code page.
    std::ifstream file(WideFromUtf8(path).c_str(), std::ios::binary);
#else
    std::ifstream file(path, std::ios::binary);
#endif
    if (!file)
        return {};
    constexpr size_t kRead = 2 * 1024 * 1024;
    std::vector<uint8_t> data;
    data.resize(kRead);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    const std::streamsize readBytes = file.gcount();
    if (readBytes <= 0)
        return {};
    data.resize(static_cast<size_t>(readBytes));
    return Sha1Hex(data);
}

std::string format_time(double seconds) {
    int s = static_cast<int>(std::max(0.0, seconds));
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    auto pad = [](int v) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d", v);
        return std::string(buf);
    };
    if (h > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d:%s:%s", h, pad(m).c_str(), pad(sec).c_str());
        return buf;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%s", m, pad(sec).c_str());
    return buf;
}
