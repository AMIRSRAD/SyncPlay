#include "panels.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <mpv/client.h>

#include "app_state.h"
#include "ui_helpers.h"
#include "panel_utils.h"
#include "chat_text.h"
#include "../core/net_proxy.h"
#include "../core/update_check.h"
#include "../media/opensubtitles.h"
#include "../media/mpv_helpers.h"
#include "../sync/sync_session.h"
#include "../network/relay_voice.h"
#include "../core/utf.h"
#include "../platform/platform.h"
#include "../platform/file_dialog.h"
#include "../core/utf.h"
#include "../core/logging.h"

// Each panel function aliases the context members back to the local names used by
// the original inline code in wWinMain, so the panel body below is verbatim.

namespace {
// Minimal ghost close button in the panel's top-right corner. Call right after
// BeginPanel* (cursor position is preserved). Returns true when clicked.
bool PanelCloseButton(const std::function<float(float)>& tune) {
    const float sz = tune(22.0f);
    const ImVec2 winSize = ImGui::GetWindowSize();
    const ImVec2 saved = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(winSize.x - sz - tune(12.0f), tune(12.0f)));
    const bool clicked = ImGui::InvisibleButton("##panelclose", ImVec2(sz, sz));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    if (hov)
        dl->AddCircleFilled(c, sz * 0.5f, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)), 20);
    const float k = sz * 0.20f;
    const ImU32 xc = ImGui::GetColorU32(ImVec4(1, 1, 1, hov ? 0.95f : 0.50f));
    const float th = std::max(1.2f, tune(1.4f));
    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), xc, th);
    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), xc, th);
    ImGui::SetCursorPos(saved);
    return clicked;
}

// Settings slider row: label-left layout with a small undo button that appears
// when the value differs from its default.
bool SliderRow(const char* label, const char* id, float* v, float minV, float maxV,
               const char* fmt, float defV, float rowLabelW) {
    static const std::string kUndoGlyph = Utf8FromCodepoint(0xE7A7);
    PanelRowLabel(label, rowLabelW);
    const float resetW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(resetW + ImGui::GetStyle().ItemSpacing.x));
    bool changed = ImGui::SliderFloat(id, v, minV, maxV, fmt);
    ImGui::SameLine();
    if (std::fabs(*v - defV) > 0.0001f) {
        // Custom-drawn button so the glyph is truly centred (the merged MDL2
        // glyph advance is wider than its ink, which skews Button's centring).
        ImGui::PushID(id);
        const bool clicked = ImGui::InvisibleButton("##reset", ImVec2(resetW, resetW));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiCol bgCol = held ? ImGuiCol_ButtonActive
                               : hovered ? ImGuiCol_ButtonHovered
                                         : ImGuiCol_Button;
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(bgCol),
                          ImGui::GetStyle().FrameRounding);
        // Centre on the glyph's actual ink, not its advance.
        ImFont* fnt = ImGui::GetFont();
        const char* gBegin = kUndoGlyph.c_str();
        const char* gEnd = gBegin + kUndoGlyph.size();
        if (const ImFontGlyph* glyph = fnt->FindGlyph(static_cast<ImWchar>(0xE7A7))) {
            const float gw = glyph->X1 - glyph->X0;
            const float gh = glyph->Y1 - glyph->Y0;
            const ImVec2 gpos(mn.x + ((mx.x - mn.x) - gw) * 0.5f - glyph->X0,
                              mn.y + ((mx.y - mn.y) - gh) * 0.5f - glyph->Y0);
            dl->AddText(gpos, ImGui::GetColorU32(ImGuiCol_Text), gBegin, gEnd);
        } else {
            const ImVec2 ts = ImGui::CalcTextSize(gBegin);
            dl->AddText(ImVec2(mn.x + ((mx.x - mn.x) - ts.x) * 0.5f,
                               mn.y + ((mx.y - mn.y) - ts.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_Text), gBegin, gEnd);
        }
        if (clicked) {
            *v = defV;
            changed = true;
        }
        if (hovered)
            StyledTooltip("Reset");
        ImGui::PopID();
    } else {
        ImGui::Dummy(ImVec2(resetW, resetW));
    }
    return changed;
}
} // namespace

void DrawSettingsPanel(PanelContext& ctx) {
    AppState& app = ctx.app;
    SyncSession& session = ctx.session;
    bool& uiHovered = ctx.uiHovered;
    bool& panelRectHovered = ctx.panelRectHovered;
    bool& nextPanelHeaderValid = ctx.nextPanelHeaderValid;
    ImVec2& nextPanelHeaderMin = ctx.nextPanelHeaderMin;
    ImVec2& nextPanelHeaderMax = ctx.nextPanelHeaderMax;
    const float panelAreaLeft = ctx.panelAreaLeft;
    const float panelAreaTop = ctx.panelAreaTop;
    const float panelAreaW = ctx.panelAreaW;
    const float panelAreaH = ctx.panelAreaH;
    const float panelHeaderH = ctx.panelHeaderH;
    const float panelFade = ctx.panelFade;
    const ImVec2 basePad = ctx.basePad;
    const ImGuiHoveredFlags hoverFlags = ctx.hoverFlags;
    ImFont* font22 = ctx.font22;
    const ImVec4 accent = ctx.accent;
    auto& centeredSheetSize = ctx.centeredSheetSize;
    auto& tune = ctx.tune;
    auto& applyVideoColor = ctx.applyVideoColor;
    auto& applyToneMapping = ctx.applyToneMapping;
    auto& applyVideoShaders = ctx.applyVideoShaders;
    const ImGuiIO& io = ImGui::GetIO();

    {
            const float panelAlpha = g_fullscreen ? 0.70f : 0.94f;
            const ImVec2 panelSize = centeredSheetSize(940.0f, 700.0f, 700.0f, 520.0f);
            ImVec2 panelPos(panelAreaLeft + (panelAreaW - panelSize.x) * 0.5f,
                            panelAreaTop + (panelAreaH - panelSize.y) * 0.5f);
            const ImVec2 mousePos = io.MousePos;
            if (mousePos.x >= panelPos.x && mousePos.x <= panelPos.x + panelSize.x &&
                mousePos.y >= panelPos.y && mousePos.y <= panelPos.y + panelSize.y) {
                panelRectHovered = true;
            }
            nextPanelHeaderValid = true;
            nextPanelHeaderMin = panelPos;
            nextPanelHeaderMax = ImVec2(panelPos.x + panelSize.x, panelPos.y + panelHeaderH);
            BeginPanelNoScroll("SettingsPanel", panelPos, panelSize, panelAlpha, panelFade, basePad);
            if (PanelCloseButton(ctx.tune))
                app.showSettings = false;
            ImGui::AlignTextToFramePadding();
            if (font22)
                ImGui::PushFont(font22);
            ImGui::TextUnformatted("Settings");
            if (font22)
                ImGui::PopFont();
            ImGui::Separator();
            const float rowLabelW = ctx.tune(150.0f);
            if (ImGui::BeginTabBar("SettingsTabs")) {
                if (ImGui::BeginTabItem("Audio")) {
                    PanelSection("Output");
                    // mpv's audio device list; refreshed lazily and on demand.
                    static std::vector<AudioDeviceInfo> audioDevices;
                    static bool audioDevicesLoaded = false;
                    if (!audioDevicesLoaded) {
                        audioDevices = mpv_read_audio_devices(ctx.mpv);
                        audioDevicesLoaded = true;
                    }
                    char* curDevRaw = mpv_get_property_string(ctx.mpv, "audio-device");
                    const std::string curDev = curDevRaw ? curDevRaw : "auto";
                    if (curDevRaw)
                        mpv_free(curDevRaw);
                    int devIndex = 0;
                    std::vector<std::string> devLabels;
                    devLabels.reserve(audioDevices.size() + 1);
                    devLabels.push_back("Automatic");
                    for (size_t di = 0; di < audioDevices.size(); ++di) {
                        devLabels.push_back(audioDevices[di].description.empty()
                                                ? audioDevices[di].name
                                                : audioDevices[di].description);
                        if (audioDevices[di].name == curDev)
                            devIndex = static_cast<int>(di) + 1;
                    }
                    std::vector<const char*> devPtrs;
                    devPtrs.reserve(devLabels.size());
                    for (const auto& l : devLabels)
                        devPtrs.push_back(l.c_str());
                    PanelRowLabel("Device", rowLabelW);
                    if (ImGui::Combo("##audiodev", &devIndex, devPtrs.data(),
                                     static_cast<int>(devPtrs.size()))) {
                        const std::string chosen =
                            devIndex <= 0 ? "auto"
                                          : audioDevices[static_cast<size_t>(devIndex - 1)].name;
                        mpv_set_property_string(ctx.mpv, "audio-device", chosen.c_str());
                        app.events.push_back({"Audio output changed", 1.5f});
                    }
                    if (ImGui::SmallButton("Refresh devices")) {
                        audioDevices = mpv_read_audio_devices(ctx.mpv);
                        app.events.push_back({"Audio devices refreshed", 1.2f});
                    }
                    double audioDelay = mpv_get_double(ctx.mpv, "audio-delay", 0.0);
                    float audioDelayF = static_cast<float>(audioDelay);
                    if (SliderRow("Audio delay (s)", "##audiodelay", &audioDelayF, -2.0f, 2.0f,
                                  "%.2f", 0.0f, rowLabelW)) {
                        double v = audioDelayF;
                        mpv_set_property(ctx.mpv, "audio-delay", MPV_FORMAT_DOUBLE, &v);
                    }

                    PanelSection("Voice");
                    PanelRowLabel("Voice volume", rowLabelW);
                    if (ImGui::SliderFloat("##voicevol", &app.voiceVolume, 0.0f, 100.0f, "%.0f")) {
                        session.setVoiceVolume(app.voiceVolume);
                        app.dirty = true;
                    }
                    ImGui::Text("Voice: %s", session.voiceState().c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("- microphone %s", app.voiceMuted ? "muted" : "live");
                    if (!session.voiceActive() && !session.voiceAvailable()) {
                        if (!app.voiceEnabled)
                            ImGui::TextDisabled("Enable voice in the Call panel before connecting.");
                        else
                            ImGui::TextDisabled("Voice is available once exactly one peer is connected.");
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Video")) {
                    PanelSection("Color");
                    if (SliderRow("Brightness", "##vbright", &app.videoBrightness, -100.0f, 100.0f, "%.0f", 0.0f, rowLabelW))
                        applyVideoColor();
                    if (SliderRow("Contrast", "##vcontrast", &app.videoContrast, -100.0f, 100.0f, "%.0f", 0.0f, rowLabelW))
                        applyVideoColor();
                    if (SliderRow("Saturation", "##vsat", &app.videoSaturation, -100.0f, 100.0f, "%.0f", 0.0f, rowLabelW))
                        applyVideoColor();
                    if (SliderRow("Gamma", "##vgamma", &app.videoGamma, -100.0f, 100.0f, "%.0f", 0.0f, rowLabelW))
                        applyVideoColor();
                    if (SliderRow("Hue", "##vhue", &app.videoHue, -100.0f, 100.0f, "%.0f", 0.0f, rowLabelW))
                        applyVideoColor();
                    PanelSection("Tone Mapping");
                    const char* toneLabels[] = {
                        "Auto", "Clip", "Linear", "Gamma", "Reinhard", "Hable", "Mobius", "BT.2390"
                    };
                    PanelRowLabel("Mode", rowLabelW);
                    if (ImGui::Combo("##tonemode", &app.videoToneMapping, toneLabels,
                                     static_cast<int>(sizeof(toneLabels) / sizeof(toneLabels[0]))))
                        applyToneMapping();
                    if (SliderRow("Param", "##toneparam", &app.videoToneMappingParam, 0.0f, 1.0f, "%.2f", 0.0f, rowLabelW))
                        applyToneMapping();
                    if (SliderRow("Target peak (nits)", "##tonepeak", &app.videoTargetPeak, 100.0f, 2000.0f, "%.0f", 300.0f, rowLabelW))
                        applyToneMapping();
                    PanelSection("Shaders");
                    if (ImGui::Button("Add Shader")) {
                        const std::wstring path = openFileDialog(
                            g_hWnd,
                            L"Shader Files\0*.glsl;*.hook\0All Files\0*.*\0",
                            L"Add Shader");
                        if (!path.empty()) {
                            const std::string utf8 = Utf8FromWide(path);
                            app.videoShaders.push_back(utf8);
                            applyVideoShaders();
                            app.events.push_back({"Shader added", 1.5f});
                        }
                    }
                    ImGui::SameLine();
                    const bool hasShaders = !app.videoShaders.empty();
                    if (!hasShaders)
                        ImGui::BeginDisabled();
                    if (ImGui::Button("Clear Shaders")) {
                        app.videoShaders.clear();
                        applyVideoShaders();
                        app.events.push_back({"Shaders cleared", 1.5f});
                    }
                    if (!hasShaders)
                        ImGui::EndDisabled();
                    ImGui::BeginChild("ShaderList", ImVec2(0.0f, tune(140.0f)), false);
                    if (ImGui::BeginTable("ShaderTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Shader", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Remove", ImGuiTableColumnFlags_WidthFixed);
                        for (size_t i = 0; i < app.videoShaders.size(); ++i) {
                            const std::string& path = app.videoShaders[i];
                            std::string label = std::filesystem::path(path).filename().string();
                            if (label.empty())
                                label = path;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(label.c_str());
                            if (ImGui::IsItemHovered())
                                ShowDelayedTooltip(path.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::PushID(static_cast<int>(i));
                            if (ImGui::SmallButton("Remove")) {
                                app.videoShaders.erase(app.videoShaders.begin() +
                                                       static_cast<std::vector<std::string>::difference_type>(i));
                                applyVideoShaders();
                                app.events.push_back({"Shader removed", 1.5f});
                                ImGui::PopID();
                                break;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Appearance")) {
                    PanelSection("Theme");
                    // Accent is applied via an eased transition in the main loop
                    // (see curAccent), so we only update the stored colour here.
                    PanelRowLabel("Accent colour", rowLabelW);
                    if (ImGui::ColorEdit3("##accent", app.accentColor, ImGuiColorEditFlags_DisplayRGB))
                        app.dirty = true;
                    if (ImGui::Checkbox("Dynamic accent (match video)", &app.dynamicAccent))
                        app.dirty = true;
                    ImGui::TextDisabled("Tints the interface with the video's dominant colour.");
                    if (ImGui::SmallButton("Reset accent")) {
                        app.accentColor[0] = accent.x;
                        app.accentColor[1] = accent.y;
                        app.accentColor[2] = accent.z;
                        app.dirty = true;
                    }
                    PanelSection("Panels");
                    if (ImGui::Checkbox("Frosted glass (blur)", &app.glassPanels))
                        app.dirty = true;
                    ImGui::TextDisabled("Turn off to save CPU on lower-end systems.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Connection")) {
                    PanelSection("Network");
                    {
                        static std::string detectedProxy = DetectSystemProxy();
                        if (ImGui::Checkbox("Use system proxy", &app.useSystemProxy)) {
                            app.dirty = true;
                            detectedProxy = DetectSystemProxy();
                            const std::string proxy =
                                app.useSystemProxy ? detectedProxy : std::string();
                            SetAppProxy(proxy);
                            mpv_set_option_string(ctx.mpv, "http-proxy", proxy.c_str());
                            session.setNetworkProxy(proxy);
                        }
                        if (app.useSystemProxy) {
                            if (detectedProxy.empty())
                                ImGui::TextDisabled("No system proxy is configured in Windows.");
                            else
                                ImGui::TextDisabled("Proxy: %s", detectedProxy.c_str());
                            ImGui::TextDisabled("Applies to streams, sessions, and online search. "
                                                "Local connections stay direct.");
                        } else {
                            ImGui::TextDisabled("All connections are direct.");
                        }
                    }
                    PanelSection("Identity");
                    PanelRowLabel("Nickname", rowLabelW);
                    if (ImGui::InputText("##nickname", app.nickname, sizeof(app.nickname)))
                        app.dirty = true;
                    session.setNickname(std::string(app.nickname));

                    PanelSection("Signaling");
                    PanelRowLabel("Server URL", rowLabelW);
                    if (ImGui::InputText("##serverurl", app.serverUrl, sizeof(app.serverUrl)))
                        app.dirty = true;
                    PanelRowLabel("Session password", rowLabelW);
                    if (ImGui::InputText("##sesspass", app.sessionPassword, sizeof(app.sessionPassword),
                                         ImGuiInputTextFlags_Password))
                        app.dirty = true;
                    if (ImGui::Checkbox("Auto-promote host", &app.autoPromote))
                        app.dirty = true;
                    session.setSessionPassword(std::string(app.sessionPassword));
                    session.setAutoPromote(app.autoPromote);
                    {
                        auto ifaceLabels = session.interfaceLabels();
                        auto ifaceAddresses = session.interfaceAddresses();
                        int ifaceIndex = 0;
                        const std::string currentAddr = app.preferredInterface;
                        for (int i = 0; i < static_cast<int>(ifaceAddresses.size()); ++i) {
                            if (ifaceAddresses[i] == currentAddr) {
                                ifaceIndex = i;
                                break;
                            }
                        }
                        std::vector<std::string> ifaceLabelStorage;
                        ifaceLabelStorage.reserve(static_cast<size_t>(ifaceLabels.size()));
                        std::vector<const char*> ifaceLabelPtrs;
                        ifaceLabelPtrs.reserve(static_cast<size_t>(ifaceLabels.size()));
                        for (const auto& label : ifaceLabels) {
                            ifaceLabelStorage.push_back(label);
                            ifaceLabelPtrs.push_back(ifaceLabelStorage.back().c_str());
                        }
                        if (!ifaceLabelPtrs.empty()) {
                            PanelRowLabel("Interface", rowLabelW);
                            if (ImGui::Combo("##iface", &ifaceIndex, ifaceLabelPtrs.data(),
                                             static_cast<int>(ifaceLabelPtrs.size()))) {
                                const std::string addr = ifaceAddresses[static_cast<size_t>(ifaceIndex)];
                                std::snprintf(app.preferredInterface, sizeof(app.preferredInterface), "%s", addr.c_str());
                                session.setSelectedInterfaceAddress(addr);
                                app.dirty = true;
                            }
                        }
                    }
                    ImGui::TextDisabled("Transport: host relay server. Voice uses the same relay; "
                                        "TURN/STUN is not used.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Diagnostics")) {
                    PanelSection("Logging");
                    if (ImGui::Checkbox("Enable file logging", &app.fileLoggingEnabled)) {
                        SyncPlayLog::SetEnabled(app.fileLoggingEnabled);
                        app.dirty = true;
                        if (app.fileLoggingEnabled)
                            LogInfo("diagnostics") << "File logging enabled" << std::endl;
                    }
                    ImGui::TextDisabled("Log file: %s", SyncPlayLog::LogFilePath().string().c_str());
                    ImGui::TextWrapped("Keep this off unless debugging connection or media issues.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("About")) {
                    if (font22)
                        ImGui::PushFont(font22);
                    ImGui::TextUnformatted("SyncPlay");
                    if (font22)
                        ImGui::PopFont();
#ifdef SYNCPLAY_VERSION
                    ImGui::TextDisabled("Version " SYNCPLAY_VERSION);
#else
                    ImGui::TextDisabled("Version (dev)");
#endif
                    {
                        const std::string latestVersion = UpdateAvailableVersion();
                        if (!latestVersion.empty()) {
                            ImGui::TextColored(accent, "Update available: %s", latestVersion.c_str());
                            ImGui::SameLine();
                            if (ImGui::TextLink("Get it"))
                                ctx.openUrl("https://github.com/AMIRSRAD/SyncPlay/releases/latest");
                        }
                    }
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextDisabled("Created by");
                    ImGui::TextUnformatted("Amirsalar Saberi rad");
                    ImGui::Spacing();

                    ImGui::TextDisabled("Website");
                    ImGui::PushStyleColor(ImGuiCol_TextLink, accent);
                    if (ImGui::TextLink("https://amirsrad.ir"))
                        ctx.openUrl("https://amirsrad.ir");
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextWrapped("\xC2\xA9 2026 Amirsalar Saberi rad. All rights reserved.");
                    ImGui::TextWrapped("SyncPlay and all associated rights are owned by "
                                       "Amirsalar Saberi rad.");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            EndPanel();
    }
}

void DrawSessionPanel(PanelContext& ctx) {
    AppState& app = ctx.app;
    SyncSession& session = ctx.session;
    bool& uiHovered = ctx.uiHovered;
    bool& panelRectHovered = ctx.panelRectHovered;
    bool& nextPanelHeaderValid = ctx.nextPanelHeaderValid;
    ImVec2& nextPanelHeaderMin = ctx.nextPanelHeaderMin;
    ImVec2& nextPanelHeaderMax = ctx.nextPanelHeaderMax;
    const float panelAreaLeft = ctx.panelAreaLeft;
    const float panelAreaTop = ctx.panelAreaTop;
    const float panelAreaW = ctx.panelAreaW;
    const float panelAreaH = ctx.panelAreaH;
    const float panelHeaderH = ctx.panelHeaderH;
    const float panelFade = ctx.panelFade;
    const ImVec2 basePad = ctx.basePad;
    const ImGuiHoveredFlags hoverFlags = ctx.hoverFlags;
    ImFont* font14 = ctx.font14;
    ImFont* font22 = ctx.font22;
    auto& centeredSheetSize = ctx.centeredSheetSize;
    const ImGuiIO& io = ImGui::GetIO();

    {
            const float panelAlpha = g_fullscreen ? 0.70f : 0.94f;
            const ImVec2 panelSize = centeredSheetSize(840.0f, 580.0f, 560.0f, 420.0f);
            ImVec2 panelPos(panelAreaLeft + (panelAreaW - panelSize.x) * 0.5f,
                            panelAreaTop + (panelAreaH - panelSize.y) * 0.5f);
            const ImVec2 mousePos = io.MousePos;
            if (mousePos.x >= panelPos.x && mousePos.x <= panelPos.x + panelSize.x &&
                mousePos.y >= panelPos.y && mousePos.y <= panelPos.y + panelSize.y) {
                panelRectHovered = true;
            }
            nextPanelHeaderValid = true;
            nextPanelHeaderMin = panelPos;
            nextPanelHeaderMax = ImVec2(panelPos.x + panelSize.x, panelPos.y + panelHeaderH);
            BeginPanelNoScroll("SessionPanel", panelPos, panelSize, panelAlpha, panelFade, basePad);
            if (PanelCloseButton(ctx.tune))
                app.showSession = false;
            ImGui::AlignTextToFramePadding();
            if (font22)
                ImGui::PushFont(font22);
            ImGui::TextUnformatted("Session");
            if (font22)
                ImGui::PopFont();
            ImGui::Separator();

            const bool sessionLive = session.sessionActive();
            auto& tune = ctx.tune;
            const float contentW = ImGui::GetContentRegionAvail().x;

            if (sessionLive) {
                // ---- Live room view -------------------------------------------------
                // Status: coloured dot + short phrase.
                ImVec4 dotCol(0.97f, 0.78f, 0.42f, 1.0f); // waiting (amber)
                std::string phrase;
                const std::string sigState = session.signalingState();
                if (sigState == "Reconnecting") {
                    dotCol = ImVec4(0.95f, 0.36f, 0.30f, 1.0f);
                    phrase = "Reconnecting...";
                } else if (session.transportConnected()) {
                    dotCol = ImVec4(0.40f, 0.84f, 0.56f, 1.0f);
                    if (session.isHost()) {
                        const int guests = session.guestCount();
                        phrase = "Connected - " + std::to_string(guests + 1) + " watching";
                    } else {
                        phrase = "Connected to host";
                    }
                } else {
                    phrase = session.isHost() ? "Waiting for guests..." : "Waiting for host...";
                }
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float dotR = tune(5.0f);
                    const ImVec2 cur = ImGui::GetCursorScreenPos();
                    const float lineH = ImGui::GetTextLineHeight();
                    dl->AddCircleFilled(ImVec2(cur.x + dotR, cur.y + lineH * 0.55f), dotR,
                                        ImGui::ColorConvertFloat4ToU32(dotCol), 16);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dotR * 2.0f + tune(8.0f));
                    ImGui::TextUnformatted(phrase.c_str());
                }

                // Peer presence: you + guest avatars.
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float avD = tune(26.0f);
                    const float avGap = tune(6.0f);
                    ImVec2 cur = ImGui::GetCursorScreenPos();
                    cur.y += tune(4.0f);
                    const std::string self = app.nickname[0] ? app.nickname : "You";
                    // Stable hue from the nickname (same recipe as chat avatars).
                    uint32_t h = 2166136261u;
                    for (unsigned char c : self) { h ^= c; h *= 16777619u; }
                    float rr = 0, gg = 0, bb = 0;
                    ImGui::ColorConvertHSVtoRGB(static_cast<float>(h % 360u) / 360.0f, 0.55f, 0.85f,
                                                rr, gg, bb);
                    dl->AddCircleFilled(ImVec2(cur.x + avD * 0.5f, cur.y + avD * 0.5f), avD * 0.5f,
                                        ImGui::ColorConvertFloat4ToU32(ImVec4(rr, gg, bb, 0.95f)), 24);
                    char selfInitial[2] = {static_cast<char>(std::toupper(
                                               static_cast<unsigned char>(self[0]))),
                                           0};
                    const ImVec2 gs = ImGui::CalcTextSize(selfInitial);
                    dl->AddText(ImVec2(cur.x + (avD - gs.x) * 0.5f, cur.y + (avD - gs.y) * 0.5f),
                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.05f, 0.06f, 0.08f, 0.95f)),
                                selfInitial);
                    // Peer circles (generic; the roster doesn't carry names).
                    const int peers = session.isHost() ? session.guestCount()
                                                       : (session.hostOnline() ? 1 : 0);
                    for (int p = 0; p < std::min(peers, 8); ++p) {
                        const float x = cur.x + (avD + avGap) * static_cast<float>(p + 1);
                        dl->AddCircleFilled(ImVec2(x + avD * 0.5f, cur.y + avD * 0.5f), avD * 0.5f,
                                            ImGui::GetColorU32(ImVec4(1, 1, 1, 0.16f)), 24);
                        const char* glyph = session.isHost() ? "G" : "H";
                        const ImVec2 pg = ImGui::CalcTextSize(glyph);
                        dl->AddText(ImVec2(x + (avD - pg.x) * 0.5f, cur.y + (avD - pg.y) * 0.5f),
                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), glyph);
                    }
                    ImGui::Dummy(ImVec2(contentW, avD + tune(8.0f)));
                }

                // The session code, big and click-to-copy.
                const std::string activeCode = session.sessionCode();
                if (!activeCode.empty()) {
                    std::string spaced;
                    for (size_t ci = 0; ci < activeCode.size(); ++ci) {
                        if (ci) spaced += ' ';
                        spaced += activeCode[ci];
                    }
                    ImFont* codeFont = font22 ? font22 : ImGui::GetFont();
                    const ImVec2 codeSize = codeFont->CalcTextSizeA(codeFont->FontSize, FLT_MAX,
                                                                    0.0f, spaced.c_str());
                    const ImVec2 chipPad(tune(22.0f), tune(10.0f));
                    const float chipW = codeSize.x + chipPad.x * 2.0f;
                    const float chipH = codeSize.y + chipPad.y * 2.0f;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                         std::max(0.0f, (contentW - chipW) * 0.5f));
                    const bool codeClicked = ImGui::InvisibleButton("##sessioncode",
                                                                    ImVec2(chipW, chipH));
                    const bool codeHovered = ImGui::IsItemHovered();
                    if (codeHovered)
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 mn = ImGui::GetItemRectMin();
                    const ImVec2 mx = ImGui::GetItemRectMax();
                    dl->AddRectFilled(mn, mx,
                                      ImGui::GetColorU32(ImVec4(1, 1, 1, codeHovered ? 0.13f : 0.07f)),
                                      tune(10.0f));
                    dl->AddRect(mn, mx, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)), tune(10.0f));
                    dl->AddText(codeFont, codeFont->FontSize,
                                ImVec2(mn.x + chipPad.x, mn.y + chipPad.y),
                                ImGui::GetColorU32(ImGuiCol_CheckMark), spaced.c_str());
                    if (codeHovered)
                        StyledTooltip("Click to copy the session code");
                    if (codeClicked) {
                        ImGui::SetClipboardText(activeCode.c_str());
                        app.events.push_back({"Session code copied", 1.5f});
                    }
                }

                // Action row: invite link / URL / disconnect.
                {
                    const float btnW = (contentW - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
                    if (ImGui::Button("Copy invite link", ImVec2(btnW, 0.0f))) {
                        const std::string link = ctx.buildInviteLink ? ctx.buildInviteLink()
                                                                     : std::string();
                        if (!link.empty()) {
                            ImGui::SetClipboardText(link.c_str());
                            app.events.push_back({"Invite link copied", 1.5f});
                        } else {
                            app.events.push_back({"No session link", 1.5f});
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Copy URL", ImVec2(btnW, 0.0f))) {
                        const std::string url = session.serverUrl();
                        if (!url.empty()) {
                            ImGui::SetClipboardText(url.c_str());
                            app.events.push_back({"Server URL copied", 1.5f});
                        }
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.18f, 0.55f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.26f, 0.22f, 0.75f));
                    if (ImGui::Button("Disconnect", ImVec2(btnW, 0.0f))) {
                        session.disconnectSession();
                        app.sessionStatus = session.statusText();
                        app.sessionHint = session.hintText();
                    }
                    ImGui::PopStyleColor(2);
                }

                PanelSection("Details");
                const std::string activeUrl = session.serverUrl();
                if (!activeUrl.empty())
                    ImGui::TextDisabled("URL: %s", activeUrl.c_str());
                if (!app.fileStatus.empty()) {
                    const ImVec4 okColor = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                    const ImVec4 warnColor = ImVec4(0.97f, 0.78f, 0.42f, 1.0f);
                    ImGui::TextColored(app.fileVerified ? okColor : warnColor, "File: %s",
                                       app.fileStatus.c_str());
                }
                {
                    const std::string syncText = session.syncConfidenceText();
                    const double drift = std::abs(session.syncDriftSeconds());
                    ImVec4 syncColor = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                    if (syncText == "Synced" || syncText == "Sync host")
                        syncColor = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                    else if (syncText.rfind("Resyncing", 0) == 0)
                        syncColor = ImVec4(0.97f, 0.58f, 0.35f, 1.0f);
                    ImGui::TextColored(syncColor, "Playback sync: %s", syncText.c_str());
                    if (drift > 0.01 && !session.isHost())
                        ImGui::TextDisabled("Estimated drift: %.2fs", drift);
                }
            } else {
                // ---- Setup view: two action cards, then the matching form ----------
                static int sessionMode = 0; // 0 = host, 1 = join
                const float cardGap = ImGui::GetStyle().ItemSpacing.x;
                const float cardW = (contentW - cardGap) * 0.5f;
                const float cardH = tune(64.0f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const auto drawModeCard = [&](const char* id, const char* title,
                                              const char* subtitle, int mode) {
                    ImGui::PushID(id);
                    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
                    const bool hovered = ImGui::IsItemHovered();
                    const bool selected = sessionMode == mode;
                    if (hovered)
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    const ImVec2 mn = ImGui::GetItemRectMin();
                    const ImVec2 mx = ImGui::GetItemRectMax();
                    ImVec4 bg = selected ? ImGui::GetStyle().Colors[ImGuiCol_CheckMark]
                                         : ImVec4(1, 1, 1, hovered ? 0.12f : 0.06f);
                    if (selected)
                        bg.w = 0.22f;
                    dl->AddRectFilled(mn, mx, ImGui::ColorConvertFloat4ToU32(bg), tune(10.0f));
                    ImVec4 border = selected ? ImGui::GetStyle().Colors[ImGuiCol_CheckMark]
                                             : ImVec4(1, 1, 1, 0.12f);
                    if (selected)
                        border.w = 0.85f;
                    dl->AddRect(mn, mx, ImGui::ColorConvertFloat4ToU32(border), tune(10.0f), 0,
                                selected ? 2.0f : 1.0f);
                    const float padX = tune(14.0f);
                    dl->AddText(ImVec2(mn.x + padX, mn.y + tune(11.0f)),
                                ImGui::GetColorU32(ImGuiCol_Text), title);
                    dl->AddText(ImVec2(mn.x + padX,
                                       mn.y + tune(11.0f) + ImGui::GetTextLineHeight() + tune(3.0f)),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), subtitle);
                    ImGui::PopID();
                    if (clicked)
                        sessionMode = mode;
                };
                drawModeCard("hostcard", "Host a party", "Start a room on this PC", 0);
                ImGui::SameLine();
                drawModeCard("joincard", "Join a party", "Enter a code from a friend", 1);
                ImGui::Spacing();

                const float rowW = tune(150.0f);
                bool joinNow = false;
                if (sessionMode == 1) {
                    PanelRowLabel("Server URL", rowW);
                    ImGui::InputText("##joinurl", app.serverUrl, sizeof(app.serverUrl));
                    PanelRowLabel("Code", rowW);
                    if (ImGui::InputText("##joincode", app.joinCode, sizeof(app.joinCode),
                                         ImGuiInputTextFlags_CharsUppercase |
                                             ImGuiInputTextFlags_EnterReturnsTrue))
                        joinNow = true;
                } else {
                    PanelRowLabel("Port", rowW);
                    if (ImGui::InputInt("##hostport", &app.signalingPort))
                        app.dirty = true;
                    if (ImGui::Checkbox("Allow guest control", &app.allowGuestControl))
                        app.dirty = true;
                    session.setSignalingPort(app.signalingPort);
                    session.setAllowGuestControl(app.allowGuestControl);
                }
                if (ImGui::Checkbox("Enable voice", &app.voiceEnabled)) {
                    session.setVoiceEnabled(app.voiceEnabled);
                    if (!app.voiceEnabled) {
                        app.voiceMuted = true;
                        session.setVoiceMuted(true);
                    }
                    app.dirty = true;
                }
                ImGui::TextDisabled("Voice uses the host relay server. Microphone starts muted.");
                ImGui::Spacing();

                // Primary action: full-width accent button.
                ImVec4 actCol = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                actCol.w = 0.55f;
                ImVec4 actHov = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                actHov.w = 0.75f;
                ImGui::PushStyleColor(ImGuiCol_Button, actCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, actHov);
                if (sessionMode == 1) {
                    if (ImGui::Button("Join Session", ImVec2(contentW, tune(34.0f))))
                        joinNow = true;
                    if (joinNow) {
                        session.setNickname(std::string(app.nickname));
                        session.setSignalingPort(app.signalingPort);
                        session.setVoiceEnabled(app.voiceEnabled);
                        session.setVoiceMuted(app.voiceMuted);
                        session.joinSession(std::string(app.serverUrl), std::string(app.joinCode));
                        app.sessionStatus = session.statusText();
                        app.sessionHint = session.hintText();
                    }
                } else {
                    if (ImGui::Button("Create Session", ImVec2(contentW, tune(34.0f)))) {
                        session.setNickname(std::string(app.nickname));
                        session.setVoiceEnabled(app.voiceEnabled);
                        session.setVoiceMuted(app.voiceMuted);
                        session.startHostSession();
                        app.sessionStatus = session.statusText();
                        app.sessionHint = session.hintText();
                        const std::string serverUrl = session.serverUrl();
                        if (!serverUrl.empty())
                            std::snprintf(app.serverUrl, sizeof(app.serverUrl), "%s",
                                          serverUrl.c_str());
                    }
                }
                ImGui::PopStyleColor(2);
                if (!app.sessionHint.empty())
                    ImGui::TextDisabled("%s", app.sessionHint.c_str());
            }
            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            EndPanel();
    }
}

void DrawPlaylistPanel(PanelContext& ctx) {
    AppState& app = ctx.app;
    mpv_handle* mpv = ctx.mpv;
    bool& uiHovered = ctx.uiHovered;
    bool& panelRectHovered = ctx.panelRectHovered;
    bool& nextPanelHeaderValid = ctx.nextPanelHeaderValid;
    ImVec2& nextPanelHeaderMin = ctx.nextPanelHeaderMin;
    ImVec2& nextPanelHeaderMax = ctx.nextPanelHeaderMax;
    const float panelAreaLeft = ctx.panelAreaLeft;
    const float panelAreaTop = ctx.panelAreaTop;
    const float panelAreaW = ctx.panelAreaW;
    const float panelAreaH = ctx.panelAreaH;
    const float panelHeaderH = ctx.panelHeaderH;
    const float panelFade = ctx.panelFade;
    const ImVec2 basePad = ctx.basePad;
    const ImGuiHoveredFlags hoverFlags = ctx.hoverFlags;
    ImFont* font22 = ctx.font22;
    ImFont* fontChat = ctx.fontChat;
    auto& centeredSheetSize = ctx.centeredSheetSize;
    auto& tune = ctx.tune;
    const ImGuiIO& io = ImGui::GetIO();

    {
            const float panelAlpha = g_fullscreen ? 0.70f : 0.94f;
            const ImVec2 panelSize = centeredSheetSize(720.0f, 620.0f, 520.0f, 420.0f);
            ImVec2 panelPos(panelAreaLeft + (panelAreaW - panelSize.x) * 0.5f,
                            panelAreaTop + (panelAreaH - panelSize.y) * 0.5f);
            const ImVec2 mousePos = io.MousePos;
            if (mousePos.x >= panelPos.x && mousePos.x <= panelPos.x + panelSize.x &&
                mousePos.y >= panelPos.y && mousePos.y <= panelPos.y + panelSize.y) {
                panelRectHovered = true;
            }
            nextPanelHeaderValid = true;
            nextPanelHeaderMin = panelPos;
            nextPanelHeaderMax = ImVec2(panelPos.x + panelSize.x, panelPos.y + panelHeaderH);
            BeginPanelNoScroll("PlaylistPanel", panelPos, panelSize, panelAlpha, panelFade, basePad);
            if (PanelCloseButton(ctx.tune))
                app.showPlaylist = false;

            const auto playlistItems = mpv_read_playlist(mpv);
            const int64_t playlistPos = mpv_get_int64(mpv, "playlist-pos", -1);

            ImGui::AlignTextToFramePadding();
            if (font22)
                ImGui::PushFont(font22);
            ImGui::TextUnformatted("Playlist");
            if (font22)
                ImGui::PopFont();
            if (!playlistItems.empty()) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%d item%s", static_cast<int>(playlistItems.size()),
                                    playlistItems.size() == 1 ? "" : "s");
            }
            // Header actions, right-aligned.
            {
                const float addW = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                const float clrW = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                // Keep clear of the panel close button in the top-right corner.
                const float closeClearance = ctx.tune(34.0f);
                ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                addW - clrW - gap - closeClearance);
                if (ImGui::Button("Add")) {
                    const std::vector<std::wstring> paths = openFileDialogMulti(
                        g_hWnd,
                        L"Video Files\0*.mp4;*.mkv;*.avi;*.mov;*.webm\0All Files\0*.*\0",
                        L"Add to Playlist");
                    bool firstLoadsNow = playlistItems.empty();
                    for (const std::wstring& path : paths) {
                        if (firstLoadsNow) {
                            if (ctx.playLocalFile)
                                ctx.playLocalFile(path);
                            firstLoadsNow = false;
                        } else {
                            const std::string utf8 = Utf8FromWide(path);
                            const char* cmd[] = { "loadfile", utf8.c_str(), "append", nullptr };
                            mpv_command(mpv, cmd);
                        }
                    }
                    if (paths.size() > 1)
                        app.events.push_back({"Added " + std::to_string(paths.size()) + " to playlist", 1.5f});
                    else if (!paths.empty())
                        app.events.push_back({"Added to playlist", 1.5f});
                }
                ImGui::SameLine();
                if (playlistItems.empty())
                    ImGui::BeginDisabled();
                if (ImGui::Button("Clear")) {
                    const char* cmd[] = { "playlist-clear", nullptr };
                    mpv_command(mpv, cmd);
                    app.events.push_back({"Playlist cleared", 1.5f});
                }
                if (playlistItems.empty())
                    ImGui::EndDisabled();
            }
            ImGui::Separator();

            if (playlistItems.empty()) {
                // Empty state, centred.
                const char* line1 = "Queue is empty";
                const char* line2 = "Drop files on the window or click Add";
                const float availH = ImGui::GetContentRegionAvail().y;
                ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, availH * 0.4f - ImGui::GetTextLineHeight())));
                const float w1 = ImGui::CalcTextSize(line1).x;
                ImGui::SetCursorPosX((panelSize.x - w1) * 0.5f);
                ImGui::TextUnformatted(line1);
                const float w2 = ImGui::CalcTextSize(line2).x;
                ImGui::SetCursorPosX((panelSize.x - w2) * 0.5f);
                ImGui::TextDisabled("%s", line2);
            } else {
                ImGui::BeginChild("PlaylistRows", ImVec2(0.0f, 0.0f), false);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImFont* rowFont = fontChat ? fontChat : ImGui::GetFont();
                const float rowH = tune(40.0f);
                const float rounding = tune(8.0f);
                const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                for (const auto& item : playlistItems) {
                    ImGui::PushID(item.index);
                    const bool isCurrent = item.current || item.index == playlistPos;
                    const float rowW = ImGui::GetContentRegionAvail().x;
                    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                    const ImVec2 rowMax(rowMin.x + rowW, rowMin.y + rowH);
                    // Hover is rect-based (not item-based) because the action buttons
                    // must be SUBMITTED BEFORE the row's hit item — earlier items win
                    // mouse-press in ImGui, so buttons submitted after the row never
                    // received clicks (presses started a row drag instead).
                    const bool rowHovered = ImGui::IsWindowHovered() &&
                                            ImGui::IsMouseHoveringRect(rowMin, rowMax);

                    // Row visuals.
                    if (isCurrent) {
                        ImVec4 bg = accent;
                        bg.w = 0.16f;
                        dl->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(bg), rounding);
                        ImVec4 pip = accent;
                        pip.w = 0.9f;
                        dl->AddRectFilled(ImVec2(rowMin.x + tune(4.0f), rowMin.y + rowH * 0.25f),
                                          ImVec2(rowMin.x + tune(7.0f), rowMax.y - rowH * 0.25f),
                                          ImGui::ColorConvertFloat4ToU32(pip), 1.5f);
                    } else if (rowHovered) {
                        dl->AddRectFilled(rowMin, rowMax,
                                          ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f)), rounding);
                    }
                    // Index number.
                    char idxText[8];
                    std::snprintf(idxText, sizeof(idxText), "%d", item.index + 1);
                    const ImVec2 idxSize = ImGui::CalcTextSize(idxText);
                    dl->AddText(ImVec2(rowMin.x + tune(16.0f),
                                       rowMin.y + (rowH - idxSize.y) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), idxText);
                    // Title (chat font handles non-Latin names), clipped before the
                    // hover action buttons.
                    std::string label = item.title;
                    if (label.empty())
                        label = std::filesystem::path(WideFromUtf8(item.filename))
                                    .filename().string();
                    if (label.empty())
                        label = "Item " + std::string(idxText);
                    const float actionsW = rowHovered ? tune(84.0f) : tune(8.0f);
                    const float textX = rowMin.x + tune(42.0f);
                    dl->PushClipRect(ImVec2(textX, rowMin.y),
                                     ImVec2(rowMax.x - actionsW, rowMax.y), true);
                    dl->AddText(rowFont, rowFont->FontSize,
                                ImVec2(textX, rowMin.y + (rowH - rowFont->FontSize) * 0.5f),
                                ImGui::GetColorU32(isCurrent ? ImGuiCol_CheckMark : ImGuiCol_Text),
                                label.c_str());
                    dl->PopClipRect();

                    // Hover actions: play / remove as circular vector-drawn buttons —
                    // an accent play disc and a neutral disc that turns red on hover.
                    // Pure draw-list shapes, so they stay crisp at any DPI.
                    bool removedThis = false;
                    if (rowHovered) {
                        const float mbSz = tune(22.0f);
                        const float mbGap = tune(8.0f);
                        const float mbY = rowMin.y + (rowH - mbSz) * 0.5f;
                        const float mbX2 = rowMax.x - tune(12.0f) - mbSz;      // remove
                        const float mbX1 = mbX2 - mbGap - mbSz;                // play
                        ImDrawList* bdl = ImGui::GetWindowDrawList();

                        // Play: accent disc + dark triangle.
                        ImGui::SetCursorScreenPos(ImVec2(mbX1, mbY));
                        const bool playClick = ImGui::InvisibleButton("##rowplay", ImVec2(mbSz, mbSz));
                        {
                            const bool hov = ImGui::IsItemHovered();
                            const ImVec2 c(mbX1 + mbSz * 0.5f, mbY + mbSz * 0.5f);
                            const float r = mbSz * 0.5f;
                            ImVec4 acc = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                            acc.w = hov ? 1.0f : 0.82f;
                            bdl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(acc), 24);
                            const ImU32 dark = ImGui::ColorConvertFloat4ToU32(
                                ImVec4(0.05f, 0.06f, 0.08f, 0.95f));
                            bdl->AddTriangleFilled(
                                ImVec2(c.x - r * 0.26f, c.y - r * 0.42f),
                                ImVec2(c.x - r * 0.26f, c.y + r * 0.42f),
                                ImVec2(c.x + r * 0.48f, c.y), dark);
                        }
                        if (playClick) {
                            const std::string idx = std::to_string(item.index);
                            const char* cmd[] = { "playlist-play-index", idx.c_str(), nullptr };
                            mpv_command(mpv, cmd);
                        }

                        // Remove: quiet disc that turns red on hover, with a stroked X.
                        ImGui::SetCursorScreenPos(ImVec2(mbX2, mbY));
                        const bool removeClick = ImGui::InvisibleButton("##rowremove", ImVec2(mbSz, mbSz));
                        {
                            const bool hov = ImGui::IsItemHovered();
                            const ImVec2 c(mbX2 + mbSz * 0.5f, mbY + mbSz * 0.5f);
                            const float r = mbSz * 0.5f;
                            const ImU32 bg = ImGui::ColorConvertFloat4ToU32(
                                hov ? ImVec4(0.90f, 0.30f, 0.26f, 0.92f)
                                    : ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
                            bdl->AddCircleFilled(c, r, bg, 24);
                            const float k = r * 0.38f;
                            const ImU32 xc = ImGui::ColorConvertFloat4ToU32(
                                ImVec4(1.0f, 1.0f, 1.0f, hov ? 0.98f : 0.75f));
                            const float th = std::max(1.5f, tune(1.7f));
                            bdl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), xc, th);
                            bdl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), xc, th);
                        }
                        if (removeClick) {
                            const std::string idx = std::to_string(item.index);
                            const char* cmd[] = { "playlist-remove", idx.c_str(), nullptr };
                            mpv_command(mpv, cmd);
                            app.events.push_back({"Removed from playlist", 1.2f});
                            removedThis = true;
                        }
                    }

                    // Row hit item LAST: the buttons above already claimed any press
                    // on them, so this only sees clicks on the rest of the row.
                    ImGui::SetCursorScreenPos(rowMin);
                    ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
                    const bool rowClicked = ImGui::IsItemHovered() &&
                                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                    // Drag to reorder: the payload is the source playlist index.
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        ImGui::SetDragDropPayload("PLAYLIST_ROW", &item.index, sizeof(int));
                        std::string dragLabel = item.title;
                        if (dragLabel.empty())
                            dragLabel = std::filesystem::path(WideFromUtf8(item.filename))
                                            .filename().string();
                        ImGui::TextUnformatted(dragLabel.empty() ? "item" : dragLabel.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload("PLAYLIST_ROW")) {
                            const int from = *static_cast<const int*>(payload->Data);
                            int to = item.index;
                            if (from != to) {
                                // mpv playlist-move inserts before `to`; moving down
                                // needs the +1 because removal shifts indices.
                                if (from < to)
                                    to += 1;
                                const std::string a = std::to_string(from);
                                const std::string b = std::to_string(to);
                                const char* cmd[] = { "playlist-move", a.c_str(), b.c_str(), nullptr };
                                mpv_command(mpv, cmd);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (rowClicked && !removedThis) {
                        const std::string idx = std::to_string(item.index);
                        const char* cmd[] = { "playlist-play-index", idx.c_str(), nullptr };
                        mpv_command(mpv, cmd);
                    }
                    ImGui::PopID();
                    if (removedThis)
                        break; // indices shifted; redraw next frame
                }
                ImGui::EndChild();
            }
            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            EndPanel();
    }
}

void DrawCallPanel(PanelContext& ctx) {
    AppState& app = ctx.app;
    SyncSession& session = ctx.session;
    bool& uiHovered = ctx.uiHovered;
    bool& panelRectHovered = ctx.panelRectHovered;
    bool& nextPanelHeaderValid = ctx.nextPanelHeaderValid;
    ImVec2& nextPanelHeaderMin = ctx.nextPanelHeaderMin;
    ImVec2& nextPanelHeaderMax = ctx.nextPanelHeaderMax;
    const float panelAreaLeft = ctx.panelAreaLeft;
    const float panelAreaTop = ctx.panelAreaTop;
    const float panelAreaW = ctx.panelAreaW;
    const float panelAreaH = ctx.panelAreaH;
    const float panelHeaderH = ctx.panelHeaderH;
    const float panelFade = ctx.panelFade;
    const ImVec2 basePad = ctx.basePad;
    const ImGuiHoveredFlags hoverFlags = ctx.hoverFlags;
    ImFont* font14 = ctx.font14;
    ImFont* font22 = ctx.font22;
    auto& centeredSheetSize = ctx.centeredSheetSize;
    const ImGuiIO& io = ImGui::GetIO();

    {
            const float panelAlpha = g_fullscreen ? 0.70f : 0.94f;
            const ImVec2 panelSize = centeredSheetSize(680.0f, 560.0f, 500.0f, 420.0f);
            ImVec2 panelPos(panelAreaLeft + (panelAreaW - panelSize.x) * 0.5f,
                            panelAreaTop + (panelAreaH - panelSize.y) * 0.5f);
            const ImVec2 mousePos = io.MousePos;
            if (mousePos.x >= panelPos.x && mousePos.x <= panelPos.x + panelSize.x &&
                mousePos.y >= panelPos.y && mousePos.y <= panelPos.y + panelSize.y) {
                panelRectHovered = true;
            }
            nextPanelHeaderValid = true;
            nextPanelHeaderMin = panelPos;
            nextPanelHeaderMax = ImVec2(panelPos.x + panelSize.x, panelPos.y + panelHeaderH);
            BeginPanelNoScroll("CallPanel", panelPos, panelSize, panelAlpha, panelFade, basePad);
            if (PanelCloseButton(ctx.tune))
                app.showCall = false;
            ImGui::AlignTextToFramePadding();
            if (font22)
                ImGui::PushFont(font22);
            ImGui::TextUnformatted("Voice Call");
            if (font22)
                ImGui::PopFont();
            ImGui::Separator();

            if (font14)
                ImGui::PushFont(font14);
            ImGui::TextUnformatted("Status");
            if (font14)
                ImGui::PopFont();
            ImGui::Text("Call: %s", session.voiceState().c_str());
            ImGui::Text("Session: %s", app.sessionStatus.c_str());
            ImGui::TextDisabled("Transport: host relay server");
            ImGui::Separator();

            if (ImGui::Checkbox("Enable voice for this session", &app.voiceEnabled)) {
                session.setVoiceEnabled(app.voiceEnabled);
                if (!app.voiceEnabled) {
                    session.stopVoiceCall();
                    app.voiceMuted = true;
                    session.setVoiceMuted(true);
                }
                app.dirty = true;
            }
            ImGui::TextDisabled("Voice becomes connectable after exactly one peer is connected.");

            const bool canToggleMic = app.voiceEnabled;
            if (!canToggleMic)
                ImGui::BeginDisabled();
            if (ImGui::Checkbox("Microphone muted", &app.voiceMuted)) {
                session.setVoiceMuted(app.voiceMuted);
                app.dirty = true;
            }
            if (!canToggleMic)
                ImGui::EndDisabled();

            ImGui::Separator();
            if (font14)
                ImGui::PushFont(font14);
            ImGui::TextUnformatted("Input");
            if (font14)
                ImGui::PopFont();

            static std::vector<VoiceCaptureDevice> voiceCaptureDevices;
            static bool voiceCaptureDevicesLoaded = false;
            if (!voiceCaptureDevicesLoaded) {
                voiceCaptureDevices = session.voiceCaptureDevices();
                voiceCaptureDevicesLoaded = true;
            }
            std::vector<std::string> micLabels;
            micLabels.reserve(voiceCaptureDevices.size() + 1);
            micLabels.push_back("Default microphone");
            for (const auto& device : voiceCaptureDevices)
                micLabels.push_back(device.name.empty() ? "Microphone" : device.name);
            std::vector<const char*> micLabelPtrs;
            micLabelPtrs.reserve(micLabels.size());
            for (const auto& label : micLabels)
                micLabelPtrs.push_back(label.c_str());

            int micComboIndex = std::clamp(app.voiceCaptureDeviceIndex + 1, 0,
                                           static_cast<int>(micLabelPtrs.size()) - 1);
            if (ImGui::Combo("Microphone", &micComboIndex, micLabelPtrs.data(),
                             static_cast<int>(micLabelPtrs.size()))) {
                app.voiceCaptureDeviceIndex = micComboIndex - 1;
                session.setVoiceCaptureDeviceIndex(app.voiceCaptureDeviceIndex);
                app.dirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                voiceCaptureDevices = session.voiceCaptureDevices();
                if (app.voiceCaptureDeviceIndex >= static_cast<int>(voiceCaptureDevices.size())) {
                    app.voiceCaptureDeviceIndex = -1;
                    session.setVoiceCaptureDeviceIndex(app.voiceCaptureDeviceIndex);
                    app.dirty = true;
                }
            }

            float thresholdPercent = app.voiceInputThreshold * 100.0f;
            if (ImGui::SliderFloat("Mic threshold", &thresholdPercent, 0.0f, 25.0f, "%.0f%%")) {
                app.voiceInputThreshold = std::clamp(thresholdPercent / 100.0f, 0.0f, 1.0f);
                session.setVoiceInputThreshold(app.voiceInputThreshold);
                app.dirty = true;
            }
            ImGui::TextDisabled("Threshold is a noise gate. 0%% sends all mic input.");

            // Live mic level meter: eased rise, slow decay, red zone near clipping,
            // and a tick showing where the noise gate sits. Live during a call.
            {
                const bool meterLive = session.voiceActive();
                static float meterLevel = 0.0f;
                const float rawLevel = meterLive ? std::clamp(session.voiceInputLevel(), 0.0f, 1.0f)
                                                 : 0.0f;
                const float dtMeter = ImGui::GetIO().DeltaTime;
                if (rawLevel > meterLevel)
                    meterLevel += (rawLevel - meterLevel) * std::min(1.0f, dtMeter * 30.0f);
                else
                    meterLevel += (rawLevel - meterLevel) * std::min(1.0f, dtMeter * 6.0f);

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Mic level");
                ImGui::SameLine();
                ImDrawList* mdl = ImGui::GetWindowDrawList();
                const float meterH = ImGui::GetTextLineHeight() * 0.6f;
                const float meterW = ImGui::GetContentRegionAvail().x - ctx.tune(6.0f);
                const ImVec2 mMin(ImGui::GetCursorScreenPos().x,
                                  ImGui::GetCursorScreenPos().y +
                                      (ImGui::GetFrameHeight() - meterH) * 0.5f);
                const ImVec2 mMax(mMin.x + meterW, mMin.y + meterH);
                ImGui::Dummy(ImVec2(meterW, ImGui::GetFrameHeight()));
                mdl->AddRectFilled(mMin, mMax, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)),
                                   meterH * 0.5f);
                if (meterLevel > 0.003f) {
                    const float fillW = meterW * meterLevel;
                    ImVec4 fillCol = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
                    if (meterLevel > 0.85f)
                        fillCol = ImVec4(0.95f, 0.36f, 0.30f, 1.0f); // clipping
                    mdl->AddRectFilled(mMin, ImVec2(mMin.x + fillW, mMax.y),
                                       ImGui::GetColorU32(fillCol), meterH * 0.5f);
                }
                if (app.voiceInputThreshold > 0.001f) {
                    const float tx = mMin.x + meterW * std::clamp(app.voiceInputThreshold, 0.0f, 1.0f);
                    mdl->AddLine(ImVec2(tx, mMin.y - 2.0f), ImVec2(tx, mMax.y + 2.0f),
                                 ImGui::GetColorU32(ImVec4(1, 1, 1, 0.45f)), 1.5f);
                }
                if (!meterLive)
                    ImGui::TextDisabled("Meter is live during a voice call.");
            }

            if (ImGui::SliderFloat("Voice Volume", &app.voiceVolume, 0.0f, 100.0f, "%.0f")) {
                session.setVoiceVolume(app.voiceVolume);
                app.dirty = true;
            }

            ImGui::Separator();
            const bool voiceReady = session.voiceAvailable();
            const bool callLive = session.voiceActive();
            const bool canPressCall = app.voiceEnabled && (callLive || voiceReady);
            if (!canPressCall)
                ImGui::BeginDisabled();
            if (ImGui::Button(callLive ? "End Call" : "Connect Voice")) {
                if (callLive)
                    session.stopVoiceCall();
                else
                    session.startVoiceCall();
            }
            if (!canPressCall)
                ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Open Session")) {
                app.showSession = true;
                app.showCall = false;
                app.showChat = false;
                app.showSubs = false;
                app.showSettings = false;
                app.showPlaylist = false;
            }

            if (!app.voiceEnabled)
                ImGui::TextDisabled("Enable voice here before connecting.");
            else if (!callLive && !voiceReady)
                ImGui::TextDisabled("Waiting for one connected peer.");
            else if (callLive)
                ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_CheckMark], "Voice is connected.");

            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            EndPanel();
    }
}

void DrawSubsPanel(PanelContext& ctx) {
    AppState& app = ctx.app;
    mpv_handle* mpv = ctx.mpv;
    bool& uiHovered = ctx.uiHovered;
    bool& panelRectHovered = ctx.panelRectHovered;
    bool& nextPanelHeaderValid = ctx.nextPanelHeaderValid;
    ImVec2& nextPanelHeaderMin = ctx.nextPanelHeaderMin;
    ImVec2& nextPanelHeaderMax = ctx.nextPanelHeaderMax;
    const float panelAreaLeft = ctx.panelAreaLeft;
    const float panelAreaTop = ctx.panelAreaTop;
    const float panelAreaW = ctx.panelAreaW;
    const float panelAreaH = ctx.panelAreaH;
    const float panelHeaderH = ctx.panelHeaderH;
    const float panelFade = ctx.panelFade;
    const ImVec2 basePad = ctx.basePad;
    const ImGuiHoveredFlags hoverFlags = ctx.hoverFlags;
    ImFont* font22 = ctx.font22;
    auto& centeredSheetSize = ctx.centeredSheetSize;
    auto& applySubtitleStyle = ctx.applySubtitleStyle;
    auto& openSubtitles = ctx.openSubtitles;
    const ImGuiIO& io = ImGui::GetIO();

    {
            const float panelAlpha = g_fullscreen ? 0.70f : 0.94f;
            const ImVec2 panelSize = centeredSheetSize(980.0f, 720.0f, 680.0f, 520.0f);
            ImVec2 panelPos(panelAreaLeft + (panelAreaW - panelSize.x) * 0.5f,
                            panelAreaTop + (panelAreaH - panelSize.y) * 0.5f);
            const ImVec2 mousePos = io.MousePos;
            if (mousePos.x >= panelPos.x && mousePos.x <= panelPos.x + panelSize.x &&
                mousePos.y >= panelPos.y && mousePos.y <= panelPos.y + panelSize.y) {
                panelRectHovered = true;
            }
            nextPanelHeaderValid = true;
            nextPanelHeaderMin = panelPos;
            nextPanelHeaderMax = ImVec2(panelPos.x + panelSize.x, panelPos.y + panelHeaderH);
            BeginPanelNoScroll("SubsPanel", panelPos, panelSize, panelAlpha, panelFade, basePad);
            if (PanelCloseButton(ctx.tune))
                app.showSubs = false;
            ImGui::AlignTextToFramePadding();
            if (font22)
                ImGui::PushFont(font22);
            ImGui::TextUnformatted("Subtitles");
            if (font22)
                ImGui::PopFont();
            ImGui::Separator();
            const float rowLabelW = ctx.tune(150.0f);
            // Scroll region so long content (search results + style + preview)
            // can never clip past the panel's bottom edge.
            ImGui::BeginChild("SubsScroll", ImVec2(0.0f, 0.0f), false);

            PanelSection("Track");
            bool subVisible = mpv_get_flag(mpv, "sub-visibility", true);
            app.subtitlesEnabled = subVisible;
            if (ImGui::Checkbox("Subtitles enabled", &subVisible)) {
                app.subtitlesEnabled = subVisible;
                mpv_set_flag(mpv, "sub-visibility", subVisible);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Open subtitle file..."))
                openSubtitles();
            if (SliderRow("Delay (s)", "##subdelay", &app.subtitleDelay, -10.0f, 10.0f, "%.1f",
                          0.0f, rowLabelW)) {
                double delay = app.subtitleDelay;
                mpv_set_property(mpv, "sub-delay", MPV_FORMAT_DOUBLE, &delay);
                app.dirty = true;
            }

            const int64_t sid = mpv_get_int64(mpv, "sid", 0);
            auto subs = mpv_read_tracks(mpv, "sub");
            std::vector<std::string> labels;
            std::vector<int> ids;
            labels.emplace_back("Off");
            ids.push_back(0);
            labels.emplace_back("Auto");
            ids.push_back(-1);
            for (const auto& tr : subs) {
                std::string labelQ = tr.title;
                if (labelQ.empty())
                    labelQ = tr.lang.empty() ? ("Subtitle " + std::to_string(tr.id)) : tr.lang;
                if (!tr.lang.empty()) {
                    std::string labelLower = labelQ;
                    std::string langLower = tr.lang;
                    std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(),
                                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                    std::transform(langLower.begin(), langLower.end(), langLower.begin(),
                                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                    if (labelLower.find(langLower) == std::string::npos)
                        labelQ += " (" + tr.lang + ")";
                }
                labels.push_back(labelQ);
                ids.push_back(tr.id);
            }
            int currentIndex = 0;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == static_cast<int>(sid)) {
                    currentIndex = static_cast<int>(i);
                    break;
                }
            }
            std::vector<const char*> cstr;
            cstr.reserve(labels.size());
            for (auto& l : labels)
                cstr.push_back(l.c_str());
            PanelRowLabel("Track", rowLabelW);
            if (ImGui::Combo("##subtrack", &currentIndex, cstr.data(), static_cast<int>(cstr.size()))) {
                int selected = ids[static_cast<size_t>(currentIndex)];
                if (selected == 0) {
                    mpv_set_property_string(mpv, "sid", "no");
                } else if (selected == -1) {
                    mpv_set_property_string(mpv, "sid", "auto");
                } else {
                    int64_t value = selected;
                    mpv_set_property(mpv, "sid", MPV_FORMAT_INT64, &value);
                }
            }

            PanelSection("Online Search");
            {
                const OsSnapshot os = OsGetSnapshot();
                char* mpvPath = mpv_get_property_string(mpv, "path");
                const std::string mediaPath = mpvPath ? mpvPath : "";
                if (mpvPath)
                    mpv_free(mpvPath);

                PanelRowLabel("API key", rowLabelW);
                if (ImGui::InputText("##oskey", app.openSubsApiKey, sizeof(app.openSubsApiKey),
                                     ImGuiInputTextFlags_Password))
                    app.dirty = true;
                if (app.openSubsApiKey[0] == '\0') {
                    ImGui::SetCursorPosX(rowLabelW);
                    if (ImGui::TextLink("Get a free key from opensubtitles.com"))
                        ctx.openUrl("https://www.opensubtitles.com/consumers");
                }
                PanelRowLabel("Languages", rowLabelW);
                ImGui::SetNextItemWidth(ctx.tune(120.0f));
                if (ImGui::InputText("##oslangs", app.openSubsLangs, sizeof(app.openSubsLangs)))
                    app.dirty = true;
                if (ImGui::IsItemHovered())
                    ShowDelayedTooltip("Comma-separated codes, e.g. en,fa");

                const bool busy = os.phase == OsPhase::Searching || os.phase == OsPhase::Downloading;
                const bool canSearch = !busy && app.openSubsApiKey[0] != '\0' && !mediaPath.empty();
                if (!canSearch)
                    ImGui::BeginDisabled();
                if (ImGui::Button("Search Online"))
                    OsStartSearch(app.openSubsApiKey, mediaPath, app.openSubsLangs);
                if (!canSearch)
                    ImGui::EndDisabled();
                if (app.openSubsApiKey[0] == '\0') {
                    ImGui::SameLine();
                    ImGui::TextDisabled("API key required");
                } else if (mediaPath.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Load a video first");
                }

                if (!os.message.empty() && os.phase != OsPhase::Idle) {
                    const bool isError = os.phase == OsPhase::Error;
                    std::string osMsg = os.message;
                    if (busy) {
                        // Animated working dots while the background thread runs.
                        const int dots = static_cast<int>(ImGui::GetTime() * 2.5) % 4;
                        while (!osMsg.empty() && osMsg.back() == '.')
                            osMsg.pop_back();
                        osMsg.append(static_cast<size_t>(dots), '.');
                    }
                    ImGui::TextColored(isError ? ImVec4(0.95f, 0.34f, 0.28f, 1.0f)
                                               : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled],
                                       "%s", osMsg.c_str());
                }
                if (os.phase == OsPhase::Downloaded && !os.downloadedPath.empty()) {
                    const char* cmd[] = {"sub-add", os.downloadedPath.c_str(), "select", nullptr};
                    mpv_command(mpv, cmd);
                    app.events.push_back({"Subtitles loaded", 2.0f});
                    OsAcknowledgeDownload();
                }
                if ((os.phase == OsPhase::Results || os.phase == OsPhase::Downloading) &&
                    !os.results.empty()) {
                    ImGui::BeginChild("OsResults", ImVec2(0.0f, ctx.tune(120.0f)), false);
                    if (ImGui::BeginTable("OsResultsTable", 3,
                                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("Lang", ImGuiTableColumnFlags_WidthFixed, ctx.tune(38.0f));
                        ImGui::TableSetupColumn("Release", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Get", ImGuiTableColumnFlags_WidthFixed, ctx.tune(64.0f));
                        int osRow = 0;
                        for (const auto& r : os.results) {
                            ImGui::PushID(osRow++);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(r.language.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(r.release.c_str());
                            if (ImGui::IsItemHovered() && !r.release.empty())
                                ShowDelayedTooltip(r.release.c_str());
                            ImGui::TableSetColumnIndex(2);
                            if (busy)
                                ImGui::BeginDisabled();
                            if (ImGui::SmallButton("Download"))
                                OsStartDownload(app.openSubsApiKey, r);
                            if (busy)
                                ImGui::EndDisabled();
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndChild();
                }
            }

            PanelSection("Style");
            if (SliderRow("Size", "##subsize", &app.subtitleFontSize, 12.0f, 72.0f, "%.0f",
                          36.0f, rowLabelW))
                applySubtitleStyle();
            float subCol4[4] = {app.subtitleColor[0], app.subtitleColor[1],
                                app.subtitleColor[2], app.subtitleOpacity};
            PanelRowLabel("Colour", rowLabelW);
            if (ImGui::ColorEdit4("##subcolor", subCol4,
                                  ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_AlphaBar)) {
                app.subtitleColor[0] = subCol4[0];
                app.subtitleColor[1] = subCol4[1];
                app.subtitleColor[2] = subCol4[2];
                app.subtitleOpacity = subCol4[3];
                applySubtitleStyle();
            }
            if (ImGui::Checkbox("Bold", &app.subtitleBold))
                applySubtitleStyle();
            ImGui::SameLine();
            if (ImGui::Checkbox("Italic", &app.subtitleItalic))
                applySubtitleStyle();
            if (SliderRow("Outline", "##suboutline", &app.subtitleBorderSize, 0.0f, 8.0f, "%.1f",
                          2.0f, rowLabelW))
                applySubtitleStyle();
            if (SliderRow("Position (%)", "##subpos", &app.subtitlePos, 0.0f, 100.0f, "%.0f",
                          90.0f, rowLabelW))
                applySubtitleStyle();

            // Live preview: a dark stand-in for the video with the sample line
            // rendered at a proportional size, with colour/outline/position applied.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float pvW = ImGui::GetContentRegionAvail().x;
                const float pvH = ctx.tune(72.0f);
                const ImVec2 pvMin = ImGui::GetCursorScreenPos();
                const ImVec2 pvMax(pvMin.x + pvW, pvMin.y + pvH);
                ImGui::Dummy(ImVec2(pvW, pvH));
                dl->AddRectFilled(pvMin, pvMax,
                                  ImGui::ColorConvertFloat4ToU32(ImVec4(0.03f, 0.04f, 0.05f, 1.0f)),
                                  ctx.tune(8.0f));
                dl->AddRect(pvMin, pvMax, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)), ctx.tune(8.0f));
                const char* sample = "The quick brown fox jumps";
                ImFont* pvFont = ImGui::GetFont();
                const float pvSize = std::clamp(app.subtitleFontSize * 0.55f, 10.0f, pvH * 0.42f);
                const ImVec2 ts = pvFont->CalcTextSizeA(pvSize, FLT_MAX, 0.0f, sample);
                const float tx = pvMin.x + (pvW - ts.x) * 0.5f;
                // subtitlePos: 0 = top of frame, 100 = bottom.
                const float usable = pvH - ts.y - ctx.tune(10.0f);
                const float ty = pvMin.y + ctx.tune(5.0f) +
                                 usable * std::clamp(app.subtitlePos / 100.0f, 0.0f, 1.0f);
                const float ol = app.subtitleBorderSize * 0.6f;
                if (ol > 0.05f) {
                    const ImU32 olCol = IM_COL32(0, 0, 0, 230);
                    const float o = std::max(1.0f, ol);
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox)
                            if (ox || oy)
                                dl->AddText(pvFont, pvSize, ImVec2(tx + ox * o, ty + oy * o),
                                            olCol, sample);
                }
                dl->AddText(pvFont, pvSize, ImVec2(tx, ty),
                            ImGui::ColorConvertFloat4ToU32(
                                ImVec4(app.subtitleColor[0], app.subtitleColor[1],
                                       app.subtitleColor[2], app.subtitleOpacity)),
                            sample);
            }

            if (ImGui::SmallButton("Reset style")) {
                app.subtitleFont[0] = '\0';
                app.subtitleFontSize = 36.0f;
                app.subtitleColor[0] = 1.0f;
                app.subtitleColor[1] = 1.0f;
                app.subtitleColor[2] = 1.0f;
                app.subtitleOpacity = 1.0f;
                app.subtitleBorderSize = 2.0f;
                app.subtitleShadowOffset = 1.0f;
                app.subtitleSpacing = 0.0f;
                app.subtitlePos = 90.0f;
                app.subtitleMarginX = 0.0f;
                app.subtitleMarginY = 0.0f;
                app.subtitleAlignX = 1;
                app.subtitleAlignY = 2;
                app.subtitleBold = false;
                app.subtitleItalic = false;
                applySubtitleStyle();
            }
            ImGui::EndChild();

            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            EndPanel();
    }
}

void DrawChatPanel(PanelContext& ctx, const ImVec2& panelPos, const ImVec2& panelSize,
                   bool chatDockedSidebar) {
    AppState& app = ctx.app;
    SyncSession& session = ctx.session;
    bool& uiHovered = ctx.uiHovered;
    const float panelFade = ctx.panelFade;
    const ImVec2 basePad = ctx.basePad;
    const ImGuiHoveredFlags hoverFlags = ctx.hoverFlags;
    ImFont* font22 = ctx.font22;
    ImFont* fontChat = ctx.fontChat;
    ImFont* fontIconsSmall = ctx.fontIconsSmall;
    auto& tune = ctx.tune;
    const std::size_t chatUnreadCount = ctx.chatUnreadCount;
    std::size_t& chatSeenCount = ctx.chatSeenCount;
    bool& chatInputActive = ctx.chatInputActive;
    auto& openFolder = ctx.openFolder;
    auto& browseShareFile = ctx.browseShareFile;
    const std::string ICON_OPEN = ctx.iconOpen;
    const std::string ICON_CHAT = ctx.iconChat;
    const std::string ICON_OVERLAY = ctx.iconOverlay;
    const std::string ICON_SIDEBAR = ctx.iconSidebar;
    const std::string ICON_EMOJI = ctx.iconEmoji;
    const float panelAlpha = g_fullscreen ? 0.65f : 0.94f;

    {
            const float chatU = ImGui::GetStyle().ItemSpacing.x;
            if (chatDockedSidebar) {
                ImDrawList* parentDraw = ImGui::GetWindowDrawList();
                parentDraw->AddLine(ImVec2(panelPos.x, panelPos.y),
                                    ImVec2(panelPos.x, panelPos.y + panelSize.y),
                                    ImGui::GetColorU32(ImGuiCol_Border));
                ImVec4 railBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
                railBg.w = std::min(1.0f, panelAlpha * panelFade);
                ImGui::SetCursorPos(panelPos);
                // Same padding as the overlay chat (padScale 0.6). A borderless
                // child ignores WindowPadding unless AlwaysUseWindowPadding is set,
                // which left the sidebar content hugging the top/bottom edges.
                const float railFs = ImGui::GetFontSize();
                const ImVec2 railPad(std::max(basePad.x, railFs * 1.6f) * 0.6f,
                                     std::max(basePad.y, railFs * 1.25f) * 0.6f);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, railBg);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, railPad);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panelFade);
                ImGui::BeginChild("ChatPanel", panelSize, false,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                  ImGuiWindowFlags_AlwaysUseWindowPadding);
            } else {
                BeginPanelNoScroll("ChatPanel", panelPos, panelSize, panelAlpha, panelFade, basePad, 0.6f);
            }
            ImGui::AlignTextToFramePadding();
            if (font22)
                ImGui::PushFont(font22);
            ImGui::TextUnformatted("Chat");
            if (font22)
                ImGui::PopFont();
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float headerBtn = std::max(18.0f, ImGui::GetFrameHeight());
            const ImVec2 headerBtnSize(headerBtn, headerBtn);
            static size_t lastChatCount = 0;
            static bool forceChatScroll = false;
            static bool focusChatInput = false;
            static int pendingChatScrollFrames = 0;
            static int chatSubView = 0; // 0 chat, 1 files
            const float rightW = headerBtn * 4.0f + gap * 3.0f;
            const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - rightW;
            if (rightX > ImGui::GetCursorPosX())
                ImGui::SameLine(rightX);
            if (IconToggle("ChatFilesView", ICON_OPEN.c_str(), "Files and transfers",
                           chatSubView == 1, fontIconsSmall, headerBtnSize)) {
                chatSubView = chatSubView == 1 ? 0 : 1;
            }
            ImGui::SameLine();
            const char* overlayIcon = app.sidePanels ? ICON_OVERLAY.c_str() : ICON_SIDEBAR.c_str();
            if (IconToggle("ChatOverlay", overlayIcon,
                           app.sidePanels ? "Chat overlay" : "Chat sidebar",
                           app.sidePanels, fontIconsSmall, headerBtnSize)) {
                app.sidePanels = !app.sidePanels;
                app.dirty = true;
                chatSubView = 0;
            }
            ImGui::SameLine();
            if (IconButtonFont("ChatEmoji", ICON_EMOJI.c_str(), "Emoji",
                               fontIconsSmall, headerBtnSize))
                app.showEmoji = !app.showEmoji;
            ImGui::SameLine();
            if (IconButtonFont("ChatFile", ICON_OPEN.c_str(), "Share File",
                               fontIconsSmall, headerBtnSize)) {
                browseShareFile();
                if (app.filePath[0] != '\0') {
                    const std::string path = app.filePath;
                    ChatLine line;
                    line.who = app.nickname[0] == '\0' ? "You" : app.nickname;
                    line.text = "Shared file";
                    line.time = ChatTimestamp();
                    line.kind = ChatLineKind::File;
                    line.fileName = FileNameFromPath(path);
                    line.retryPath = path;
                    try {
                        line.fileSize = static_cast<int64_t>(std::filesystem::file_size(std::filesystem::path(path)));
                    } catch (...) {
                        line.fileSize = 0;
                    }
                    line.status = ChatLineStatus::Sending;
                    std::string shareId;
                    if (session.sendSharedFile(path, &shareId)) {
                        line.transferId = shareId;
                        line.fileTransferred = 0;
                    } else {
                        line.status = ChatLineStatus::Failed;
                        line.text = "File share failed";
                    }
                    app.chat.push_back(std::move(line));
                    if (app.chat.size() > 200)
                        app.chat.pop_front();
                    app.filePath[0] = '\0';
                    app.dirty = true;
                    forceChatScroll = true;
                }
            }
            ImGui::Separator();
            std::vector<int> transferRows;
            std::vector<int> downloadedRows;
            transferRows.reserve(app.chat.size());
            downloadedRows.reserve(app.chat.size());
            int activeTransferCount = 0;
            int failedTransferCount = 0;
            for (int i = 0; i < static_cast<int>(app.chat.size()); ++i) {
                const ChatLine& line = app.chat[static_cast<size_t>(i)];
                const bool isFile = line.kind == ChatLineKind::File ||
                                    !line.fileName.empty() || !line.filePath.empty();
                if (!isFile)
                    continue;
                transferRows.push_back(i);
                const bool inProgress = line.status == ChatLineStatus::Sending ||
                                        line.status == ChatLineStatus::Receiving;
                if (inProgress)
                    ++activeTransferCount;
                if (line.status == ChatLineStatus::Failed)
                    ++failedTransferCount;
                const bool incomingDownload = line.status == ChatLineStatus::Received &&
                                              !line.filePath.empty() &&
                                              line.retryPath.empty();
                if (incomingDownload)
                    downloadedRows.push_back(i);
            }
            auto renderFilesView = [&]() {
                const float viewH = std::max(80.0f, ImGui::GetContentRegionAvail().y);
                ImGui::BeginChild("FilesView", ImVec2(0.0f, viewH), false);

                ImGui::TextDisabled("Transfers: %d active, %d failed", activeTransferCount, failedTransferCount);
                if (transferRows.empty()) {
                    ImGui::TextDisabled("No file transfers yet.");
                } else if (ImGui::BeginTable("FilesTransfersTable", 4,
                                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, tune(76.0f));
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, tune(96.0f));
                    for (int row = 0; row < static_cast<int>(transferRows.size()); ++row) {
                        const int chatIndex = transferRows[transferRows.size() - 1 - static_cast<size_t>(row)];
                        ChatLine& line = app.chat[static_cast<size_t>(chatIndex)];
                        const std::string fileName = line.fileName.empty()
                                                         ? (line.filePath.empty() ? line.text : FileNameFromPath(line.filePath))
                                                         : line.fileName;
                        ImGui::PushID(chatIndex);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(fileName.c_str());
                        if (ImGui::IsItemHovered())
                            ShowDelayedTooltip(fileName.c_str());

                        ImGui::TableSetColumnIndex(1);
                        float progress = 1.0f;
                        if (line.fileSize > 0)
                            progress = std::clamp(static_cast<float>(line.fileTransferred) /
                                                      static_cast<float>(line.fileSize),
                                                  0.0f, 1.0f);
                        const bool inProgress = line.status == ChatLineStatus::Sending ||
                                                line.status == ChatLineStatus::Receiving;
                        if (inProgress)
                            progress = std::clamp(progress, 0.02f, 0.98f);
                        else if (line.status == ChatLineStatus::Failed)
                            progress = 0.0f;
                        else
                            progress = 1.0f;
                        ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, tune(5.0f)), "");
                        const std::string sizeText = line.fileSize > 0 && line.fileTransferred > 0 &&
                                                     line.fileTransferred < line.fileSize
                                                         ? FormatBytes(line.fileTransferred) + " / " + FormatBytes(line.fileSize)
                                                         : FormatBytes(line.fileSize);
                        if (ImGui::IsItemHovered())
                            ShowDelayedTooltip(sizeText.c_str());

                        ImGui::TableSetColumnIndex(2);
                        const ImVec4 stateColor = line.status == ChatLineStatus::Failed
                                                      ? ImVec4(0.95f, 0.34f, 0.28f, 1.0f)
                                                      : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                        ImGui::TextColored(stateColor, "%s", ChatStatusLabel(line.status));

                        ImGui::TableSetColumnIndex(3);
                        const std::string openPath = !line.filePath.empty() ? line.filePath : line.retryPath;
                        if (line.status == ChatLineStatus::Failed && !line.retryPath.empty()) {
                            if (ImGui::SmallButton("Retry")) {
                                line.status = ChatLineStatus::Sending;
                                line.fileTransferred = 0;
                                std::string shareId;
                                if (session.sendSharedFile(line.retryPath, &shareId))
                                    line.transferId = shareId;
                                else
                                    line.status = ChatLineStatus::Failed;
                                app.dirty = true;
                            }
                        } else if (!openPath.empty() && !inProgress) {
                            if (ImGui::SmallButton("Folder"))
                                openFolder(openPath);
                        } else {
                            ImGui::TextDisabled("-");
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                ImGui::Separator();
                ImGui::TextDisabled("Recently downloaded");
                if (downloadedRows.empty()) {
                    ImGui::TextDisabled("No downloaded files yet.");
                } else if (ImGui::BeginTable("FilesDownloadsTable", 3,
                                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, tune(84.0f));
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, tune(92.0f));
                    for (int row = 0; row < static_cast<int>(downloadedRows.size()); ++row) {
                        const int chatIndex = downloadedRows[downloadedRows.size() - 1 - static_cast<size_t>(row)];
                        ChatLine& line = app.chat[static_cast<size_t>(chatIndex)];
                        const std::string fileName = line.fileName.empty() ? FileNameFromPath(line.filePath) : line.fileName;
                        ImGui::PushID(chatIndex);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(fileName.c_str());
                        if (ImGui::IsItemHovered())
                            ShowDelayedTooltip(line.filePath.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("%s", FormatBytes(line.fileSize).c_str());
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::SmallButton("Folder"))
                            openFolder(line.filePath);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
            };
            if (chatSubView == 1) {
                renderFilesView();
            } else {
            const bool chatAppearing = ImGui::IsWindowAppearing();
            const float inputRowH = ImGui::GetFrameHeight();
            const float bottomReserve = inputRowH + ImGui::GetStyle().ItemSpacing.y + tune(6.0f);
            if (chatAppearing) {
                focusChatInput = true;
                forceChatScroll = true;
            }
            ImGui::BeginChild("ChatScroll", ImVec2(0, -bottomReserve), false);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 chatScrollScreenPos = ImGui::GetWindowPos();
            const ImVec2 chatScrollSize = ImGui::GetWindowSize();
            const float contentW = ImGui::GetContentRegionAvail().x;
            const float maxBubbleW = std::max(120.0f, contentW * 0.70f);
            const ImVec2 bubblePad(1.0f * chatU, 0.60f * chatU);
            const float bubbleRounding = 12.0f;
            const float bubbleSpacing = 0.60f * chatU;
            const float scrollSlack = std::max(4.0f, chatU * 0.5f);
            const bool wasAtBottom =
                ImGui::GetScrollY() >= (ImGui::GetScrollMaxY() - scrollSlack);
            bool scrollToBottom = false;
            if (forceChatScroll) {
                scrollToBottom = true;
                forceChatScroll = false;
            } else if (app.chat.size() != lastChatCount && wasAtBottom) {
                scrollToBottom = true;
            }
            ImFont* textFont = fontChat ? fontChat : ImGui::GetFont();
            const float wrapBase = std::max(40.0f, maxBubbleW - bubblePad.x * 2.0f);
            const int wrapKey = static_cast<int>(std::round(wrapBase));
            const int fontKey = static_cast<int>(std::round(textFont->FontSize));
            static int lastWrapKey = 0;
            static int lastFontKey = 0;
            static std::unordered_map<std::string, ChatTextTexture> chatTextCache;
            if (wrapKey != lastWrapKey || fontKey != lastFontKey) {
                for (auto& entry : chatTextCache)
                    ReleaseChatTexture(entry.second);
                chatTextCache.clear();
                lastWrapKey = wrapKey;
                lastFontKey = fontKey;
            } else if (chatTextCache.size() > 400) {
                for (auto& entry : chatTextCache)
                    ReleaseChatTexture(entry.second);
                chatTextCache.clear();
            }
            ImVec4 peerCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
            peerCol.w = std::min(0.85f, peerCol.w + 0.2f);
            ImVec4 mineCol = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
            mineCol.w = std::min(0.9f, mineCol.w + 0.2f);
            ImVec4 systemCol = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            systemCol.w = std::min(0.6f, systemCol.w + 0.1f);
            auto chatStatusText = [](ChatLineStatus status) -> const char* {
                switch (status) {
                case ChatLineStatus::Sending: return "sending";
                case ChatLineStatus::Receiving: return "receiving";
                case ChatLineStatus::Sent: return "sent";
                case ChatLineStatus::Failed: return "failed";
                case ChatLineStatus::Received: return "received";
                default: return "";
                }
            };
            auto chatStatusColor = [&]() {
                return ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
            };
            auto failedStatusColor = []() {
                return ImVec4(0.95f, 0.34f, 0.28f, 1.0f);
            };
            const auto roundPixel = [](float v) { return std::floor(v + 0.5f); };
            const auto isSystemLine = [](const ChatLine& l) {
                return l.kind == ChatLineKind::System || l.who == "System";
            };
            const int chatN = static_cast<int>(app.chat.size());
            // Group consecutive messages from the same sender (a system line breaks a
            // group). Group start -> show the peer's name; group end -> draw the tail.
            std::vector<char> groupStartFlag(static_cast<size_t>(chatN), 1);
            std::vector<char> groupEndFlag(static_cast<size_t>(chatN), 1);
            for (int gi = 0; gi < chatN; ++gi) {
                if (isSystemLine(app.chat[static_cast<size_t>(gi)]))
                    continue;
                const std::string& who = app.chat[static_cast<size_t>(gi)].who;
                if (gi > 0) {
                    const ChatLine& prv = app.chat[static_cast<size_t>(gi - 1)];
                    if (!isSystemLine(prv) && prv.who == who)
                        groupStartFlag[static_cast<size_t>(gi)] = 0;
                }
                if (gi + 1 < chatN) {
                    const ChatLine& nxt = app.chat[static_cast<size_t>(gi + 1)];
                    if (!isSystemLine(nxt) && nxt.who == who)
                        groupEndFlag[static_cast<size_t>(gi)] = 0;
                }
            }
            const float intraSpacing = roundPixel(bubbleSpacing * 0.45f);
            const float groupSpacing = roundPixel(bubbleSpacing * 1.6f);
            const float nameLineH = ImGui::GetTextLineHeight();
            const float nameGapH = nameLineH + roundPixel(chatU * 0.15f);
            const auto bubbleCorners = [](bool mine, bool atEnd) -> ImDrawFlags {
                if (!atEnd)
                    return ImDrawFlags_RoundCornersAll;
                return mine ? (ImDrawFlags_RoundCornersAll & ~ImDrawFlags_RoundCornersBottomRight)
                            : (ImDrawFlags_RoundCornersAll & ~ImDrawFlags_RoundCornersBottomLeft);
            };
            // Stable per-sender avatar colour (FNV hash of the name -> hue).
            const auto senderColor = [](const std::string& who) {
                uint32_t h = 2166136261u;
                for (unsigned char c : who) {
                    h ^= c;
                    h *= 16777619u;
                }
                float r = 0.0f, g = 0.0f, b = 0.0f;
                ImGui::ColorConvertHSVtoRGB(static_cast<float>(h % 360u) / 360.0f, 0.55f, 0.85f,
                                            r, g, b);
                return ImVec4(r, g, b, 1.0f);
            };
            // First UTF-8 codepoint of the name, uppercased when ASCII.
            const auto firstGlyph = [](const std::string& s) -> std::string {
                if (s.empty())
                    return "?";
                const unsigned char c = static_cast<unsigned char>(s[0]);
                size_t len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                len = std::min(len, s.size());
                std::string glyph = s.substr(0, len);
                if (glyph.size() == 1)
                    glyph[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(glyph[0])));
                return glyph;
            };
            // Lines present on the very first drawn frame are history: stamp them
            // with 0 so only genuinely new messages play the entrance animation.
            static bool chatHistoryStamped = false;
            if (chatN == 0) {
                // Friendly empty state, centred in the scroll region.
                const char* hi1 = "No messages yet";
                const char* hi2 = "Say hi to your watch party";
                const float availH = ImGui::GetContentRegionAvail().y;
                const float availW = ImGui::GetContentRegionAvail().x;
                ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, availH * 0.42f - ImGui::GetTextLineHeight())));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(0.0f, (availW - ImGui::CalcTextSize(hi1).x) * 0.5f));
                ImGui::TextUnformatted(hi1);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(0.0f, (availW - ImGui::CalcTextSize(hi2).x) * 0.5f));
                ImGui::TextDisabled("%s", hi2);
            }
            for (int i = 0; i < chatN; ++i) {
                ChatLine& line = app.chat[static_cast<size_t>(i)];
                ImGui::PushID(i);
                const bool isMine = (line.who == "You") ||
                                    (app.nickname[0] != '\0' && line.who == app.nickname);
                const bool isSystem = isSystemLine(line);
                const bool isFile = line.kind == ChatLineKind::File || !line.fileName.empty() || !line.filePath.empty();
                const bool groupStart = groupStartFlag[static_cast<size_t>(i)] != 0;
                const bool groupEnd = groupEndFlag[static_cast<size_t>(i)] != 0;
                const float endSpacing = groupEnd ? groupSpacing : intraSpacing;
                const bool showName = !isMine && !isSystem && groupStart && !line.who.empty();
                const ImVec2 startPos = ImGui::GetCursorPos();

                // One-shot entrance: new lines rise ~14px while fading in.
                if (line.appearAt < 0.0)
                    line.appearAt = chatHistoryStamped ? ImGui::GetTime() : 0.0;
                float appear = 1.0f;
                if (line.appearAt > 0.0) {
                    const float t = static_cast<float>((ImGui::GetTime() - line.appearAt) / 0.22);
                    appear = std::clamp(t, 0.0f, 1.0f);
                    appear = appear * appear * (3.0f - 2.0f * appear);
                }
                const float rise = (1.0f - appear) * tune(14.0f);
                const bool fading = appear < 0.999f;
                if (fading)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * appear);

                float headerH = 0.0f;
                if (showName) {
                    const ImVec2 nameScreen = ImGui::GetCursorScreenPos();
                    const float avatarD = nameLineH;
                    const float avatarR = avatarD * 0.5f;
                    ImVec4 avCol = senderColor(line.who);
                    avCol.w = 0.92f * appear;
                    const ImVec2 avCenter(roundPixel(nameScreen.x + avatarR),
                                          roundPixel(nameScreen.y + rise + avatarR));
                    drawList->AddCircleFilled(avCenter, avatarR,
                                              ImGui::ColorConvertFloat4ToU32(avCol), 24);
                    const std::string initial = firstGlyph(line.who);
                    const float avFontSize = avatarD * 0.62f;
                    const ImVec2 gs = textFont->CalcTextSizeA(avFontSize, FLT_MAX, 0.0f,
                                                              initial.c_str());
                    drawList->AddText(textFont, avFontSize,
                                      ImVec2(avCenter.x - gs.x * 0.5f, avCenter.y - gs.y * 0.5f),
                                      ImGui::ColorConvertFloat4ToU32(
                                          ImVec4(0.05f, 0.06f, 0.08f, 0.95f * appear)),
                                      initial.c_str());
                    drawList->AddText(ImVec2(roundPixel(nameScreen.x + avatarD + chatU * 0.5f),
                                             roundPixel(nameScreen.y + rise)),
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled), line.who.c_str());
                    headerH = nameGapH;
                }

                if (isSystem) {
                    const std::string label = line.text.empty() ? line.who : line.text;
                    const ImVec2 chipPad(chatU * 0.95f, chatU * 0.45f);
                    const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
                    const ImVec2 chipSize(std::min(contentW, labelSize.x + chipPad.x * 2.0f),
                                          labelSize.y + chipPad.y * 2.0f);
                    const float chipX = std::max(0.0f, (contentW - chipSize.x) * 0.5f);
                    ImGui::SetCursorPos(ImVec2(startPos.x + chipX, startPos.y));
                    const ImVec2 chipScreen = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("SystemChip", chipSize);
                    ImVec4 chipCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                    chipCol.w = std::min(0.62f, chipCol.w + 0.12f) * appear;
                    drawList->AddRectFilled(ImVec2(chipScreen.x, chipScreen.y + rise),
                                            ImVec2(chipScreen.x + chipSize.x,
                                                   chipScreen.y + rise + chipSize.y),
                                            ImGui::ColorConvertFloat4ToU32(chipCol),
                                            chipSize.y * 0.5f);
                    drawList->AddText(ImVec2(chipScreen.x + chipPad.x, chipScreen.y + rise + chipPad.y),
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
                    ImGui::SetCursorPos(ImVec2(startPos.x, roundPixel(startPos.y + chipSize.y + endSpacing)));
                    if (fading)
                        ImGui::PopStyleVar();
                    ImGui::PopID();
                    continue;
                }

                if (isFile) {
                    const bool inProgress = line.status == ChatLineStatus::Sending ||
                                            line.status == ChatLineStatus::Receiving;
                    const std::string openPath = !line.filePath.empty() ? line.filePath : line.retryPath;
                    const bool canOpenFileCard = !openPath.empty() && !inProgress &&
                                                 line.status != ChatLineStatus::Failed;
                    const float cardW = std::min(contentW, std::max(170.0f, contentW * 0.76f));
                    const float cardH = std::max(58.0f, ImGui::GetFrameHeight() * 2.15f);
                    const float cardX = isMine ? std::max(0.0f, contentW - cardW) : 0.0f;
                    ImGui::SetCursorPos(ImVec2(startPos.x + cardX, startPos.y + headerH));
                    const ImVec2 cardMin = ImGui::GetCursorScreenPos();
                    const ImVec2 cardSize(cardW, cardH);
                    ImGui::InvisibleButton("FileCardHit", cardSize);
                    const bool cardHovered = ImGui::IsItemHovered();
                    if (canOpenFileCard && cardHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        openFolder(openPath);
                    const ImVec2 cardDrawMin(cardMin.x, cardMin.y + rise);
                    ImVec4 cardCol = isMine ? ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]
                                            : ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
                    cardCol.w = std::min(0.86f, cardCol.w + 0.16f) * appear;
                    drawList->AddRectFilled(cardDrawMin,
                                            ImVec2(cardDrawMin.x + cardSize.x, cardDrawMin.y + cardSize.y),
                                            ImGui::ColorConvertFloat4ToU32(cardCol),
                                            bubbleRounding, bubbleCorners(isMine, groupEnd));
                    drawList->AddRect(cardDrawMin,
                                      ImVec2(cardDrawMin.x + cardSize.x, cardDrawMin.y + cardSize.y),
                                      ImGui::GetColorU32(ImGuiCol_Border), bubbleRounding,
                                      bubbleCorners(isMine, groupEnd));

                    const ImVec2 innerPad(chatU * 0.75f, chatU * 0.55f);
                    const float innerX = cardDrawMin.x + innerPad.x;
                    float innerY = cardDrawMin.y + innerPad.y;
                    const std::string fileName = line.fileName.empty()
                                                     ? (line.filePath.empty() ? line.text : FileNameFromPath(line.filePath))
                                                     : line.fileName;
                    drawList->AddText(ImVec2(innerX, innerY), ImGui::GetColorU32(ImGuiCol_Text), fileName.c_str());
                    innerY += ImGui::GetTextLineHeightWithSpacing();
                    const bool hasProgress = line.fileSize > 0 && line.fileTransferred > 0 &&
                                             line.fileTransferred < line.fileSize;
                    const std::string meta = hasProgress
                                                 ? (FormatBytes(line.fileTransferred) + " / " +
                                                    FormatBytes(line.fileSize) + " - " +
                                                    chatStatusText(line.status))
                                                 : (FormatBytes(line.fileSize) + " - " +
                                                    chatStatusText(line.status));
                    ImVec4 statusCol = line.status == ChatLineStatus::Failed ? failedStatusColor() : chatStatusColor();
                    statusCol.w *= appear;
                    drawList->AddText(ImVec2(innerX, innerY), ImGui::ColorConvertFloat4ToU32(statusCol), meta.c_str());

                    const float progressW = std::max(40.0f, cardW - innerPad.x * 2.0f);
                    float progress = 1.0f;
                    if (line.fileSize > 0)
                        progress = std::clamp(static_cast<float>(line.fileTransferred) /
                                                  static_cast<float>(line.fileSize),
                                              0.0f, 1.0f);
                    if (inProgress)
                        progress = std::clamp(progress, 0.02f, 0.98f);
                    else if (line.status == ChatLineStatus::Failed)
                        progress = 0.0f;
                    else
                        progress = 1.0f;
                    const float progressY = cardDrawMin.y + cardH - innerPad.y - 4.0f;
                    ImGui::SetCursorScreenPos(ImVec2(innerX, progressY));
                    ImGui::ProgressBar(progress, ImVec2(progressW, 4.0f), "");

                    if (cardHovered) {
                        if (canOpenFileCard)
                            StyledTooltip("Double-click to open folder");
                        else if (!line.time.empty())
                            StyledTooltip(line.time.c_str());
                    }
                    ImGui::SetCursorPos(ImVec2(startPos.x, roundPixel(startPos.y + headerH + cardH + endSpacing)));
                    if (fading)
                        ImGui::PopStyleVar();
                    ImGui::PopID();
                    continue;
                }

                const ImVec4 bubbleCol = isMine ? mineCol : peerCol;
                const float wrapWidth = wrapBase;
                const std::string cacheKey =
                    std::to_string(wrapKey) + "|" + std::to_string(fontKey) + "|" + line.text;
                ChatTextTexture* textTex = nullptr;
                auto it = chatTextCache.find(cacheKey);
                if (it == chatTextCache.end()) {
                    ChatTextTexture tex;
                    const std::wstring wide = WideFromUtf8(line.text);
                    const ImVec4 textCol = ImGui::GetStyle().Colors[ImGuiCol_Text];
                    if (!RenderChatTextTexture(wide, wrapWidth, textFont->FontSize, textCol, tex)) {
                        tex.size = textFont->CalcTextSizeA(textFont->FontSize, FLT_MAX,
                                                           wrapWidth, line.text.c_str());
                    }
                    it = chatTextCache.emplace(cacheKey, std::move(tex)).first;
                }
                textTex = &it->second;
                ImVec2 textSize = textTex->size;
                if (textSize.x <= 0.0f || textSize.y <= 0.0f) {
                    textSize = textFont->CalcTextSizeA(textFont->FontSize, FLT_MAX,
                                                       wrapWidth, line.text.c_str());
                }
                textSize.x = std::ceil(textSize.x);
                textSize.y = std::ceil(textSize.y);
                const ImVec2 bubbleSize(std::ceil(textSize.x + bubblePad.x * 2.0f),
                                        std::ceil(textSize.y + bubblePad.y * 2.0f));
                float bubbleX = isMine ? (contentW - bubbleSize.x) : 0.0f;
                if (bubbleX < 0.0f)
                    bubbleX = 0.0f;
                bubbleX = roundPixel(bubbleX);
                ImVec2 bubblePos(startPos.x + bubbleX, startPos.y + headerH);
                bubblePos.x = roundPixel(bubblePos.x);
                bubblePos.y = roundPixel(bubblePos.y);
                const ImVec2 cursorScreen = ImGui::GetCursorScreenPos();
                ImVec2 bubbleMin(cursorScreen.x + bubbleX, cursorScreen.y + headerH + rise);
                bubbleMin.x = roundPixel(bubbleMin.x);
                bubbleMin.y = roundPixel(bubbleMin.y);
                const ImVec2 bubbleMax(bubbleMin.x + bubbleSize.x, bubbleMin.y + bubbleSize.y);

                ImGui::SetCursorPos(bubblePos);
                ImGui::InvisibleButton("ChatBubble", bubbleSize);
                const bool bubbleHovered = ImGui::IsItemHovered();
                if (bubbleHovered && !line.time.empty())
                    StyledTooltip(line.time.c_str());

                ImVec4 fillCol = bubbleCol;
                fillCol.w *= appear;
                drawList->AddRectFilled(bubbleMin, bubbleMax,
                                        ImGui::ColorConvertFloat4ToU32(fillCol),
                                        bubbleRounding, bubbleCorners(isMine, groupEnd));
                if (isMine && bubbleSize.x > bubbleRounding * 2.0f + 2.0f) {
                    // Subtle two-tone sheen: a soft highlight that fades down the top
                    // half. Inset by the corner radius so it never crosses the rounding.
                    const ImU32 sheenTop = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(1.0f, 1.0f, 1.0f, 0.10f * appear));
                    const ImU32 sheenBot = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(1.0f, 1.0f, 1.0f, 0.0f));
                    drawList->AddRectFilledMultiColor(
                        ImVec2(bubbleMin.x + bubbleRounding, bubbleMin.y + 1.0f),
                        ImVec2(bubbleMax.x - bubbleRounding,
                               bubbleMin.y + bubbleSize.y * 0.55f),
                        sheenTop, sheenTop, sheenBot, sheenBot);
                }

                const ImVec2 textMin(roundPixel(bubbleMin.x + bubblePad.x),
                                     roundPixel(bubbleMin.y + bubblePad.y));
                const ImVec2 textMax(textMin.x + textSize.x, textMin.y + textSize.y);
                if (textTex && textTex->srv) {
                    drawList->AddImage(reinterpret_cast<ImTextureID>(textTex->srv),
                                       textMin, textMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                       IM_COL32(255, 255, 255,
                                                static_cast<int>(appear * 255.0f)));
                } else {
                    ImGui::SetCursorPos(ImVec2(bubblePos.x + bubblePad.x, bubblePos.y + bubblePad.y));
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
                    if (fontChat)
                        ImGui::PushFont(fontChat);
                    ImGui::TextUnformatted(line.text.c_str());
                    if (fontChat)
                        ImGui::PopFont();
                    ImGui::PopTextWrapPos();
                }

                float nextY = roundPixel(startPos.y + headerH + bubbleSize.y + endSpacing);
                if (isMine && line.status == ChatLineStatus::Failed) {
                    const char* statusText = chatStatusText(line.status);
                    if (statusText[0] != '\0') {
                        ImVec4 statusCol = line.status == ChatLineStatus::Failed ? failedStatusColor() : chatStatusColor();
                        statusCol.w *= appear;
                        const ImVec2 statusSize = ImGui::CalcTextSize(statusText);
                        const float statusX = std::max(0.0f, contentW - statusSize.x);
                        drawList->AddText(ImVec2(cursorScreen.x + statusX,
                                                 cursorScreen.y + rise + nextY - startPos.y),
                                          ImGui::ColorConvertFloat4ToU32(statusCol), statusText);
                        nextY += statusSize.y;
                    }
                }
                ImGui::SetCursorPos(ImVec2(startPos.x, nextY));
                if (fading)
                    ImGui::PopStyleVar();
                ImGui::PopID();
            }
            chatHistoryStamped = true;
            const bool showNewChatIndicator =
                chatUnreadCount > 0 && !scrollToBottom && pendingChatScrollFrames <= 0 && !wasAtBottom;
            if (showNewChatIndicator) {
                char label[32]{};
                if (chatUnreadCount > 99)
                    std::snprintf(label, sizeof(label), "99+ new messages");
                else
                    std::snprintf(label, sizeof(label), "%zu new message%s",
                                  chatUnreadCount, chatUnreadCount == 1 ? "" : "s");
                const ImVec2 labelSize = ImGui::CalcTextSize(label);
                const ImVec2 pad = ImGui::GetStyle().FramePadding;
                const ImVec2 btnSize(labelSize.x + pad.x * 2.0f, ImGui::GetFrameHeight());
                const ImVec2 btnPos(chatScrollScreenPos.x + chatScrollSize.x - btnSize.x - chatU,
                                    chatScrollScreenPos.y + chatScrollSize.y - btnSize.y - chatU);
                ImGui::SetCursorScreenPos(btnPos);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.25f, 0.18f, 0.92f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.34f, 0.25f, 0.96f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.18f, 0.14f, 0.98f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, btnSize.y * 0.5f);
                if (ImGui::Button(label, btnSize)) {
                    scrollToBottom = true;
                    pendingChatScrollFrames = 2;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
            }
            if (scrollToBottom)
                pendingChatScrollFrames = std::max(pendingChatScrollFrames, 2);
            if (pendingChatScrollFrames > 0) {
                ImGui::SetScrollY(ImGui::GetScrollMaxY());
                --pendingChatScrollFrames;
                chatSeenCount = app.chat.size();
            } else if (ImGui::GetScrollY() >= (ImGui::GetScrollMaxY() - scrollSlack)) {
                chatSeenCount = app.chat.size();
            }
            ImGui::EndChild();
            lastChatCount = app.chat.size();

            const float sendW = ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float inputW = std::max(80.0f, ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x);
            ImGui::SetNextItemWidth(inputW);
            if (focusChatInput) {
                ImGui::SetKeyboardFocusHere();
                focusChatInput = false;
            }
            static ChatInputCursor chatInputCursor;
            static ChatTextTexture chatInputTex;
            static std::string chatInputCached;
            static float chatInputFontSize = 0.0f;
            static ImVec4 chatInputColor{0.0f, 0.0f, 0.0f, 0.0f};
            static float chatInputCaretX = 0.0f;
            static float chatInputCaretY = 0.0f;
            static float chatInputCaretH = 0.0f;

            const ImVec4 inputTextColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const bool inputNeedsShaping = NeedsComplexText(app.chatInput) && app.chatInput[0] != '\0';
            if (inputNeedsShaping) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(inputTextColor.x, inputTextColor.y, inputTextColor.z, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0, 0, 0, 0));
            }
            if (fontChat)
                ImGui::PushFont(fontChat);
            const ImGuiInputTextFlags inputFlags =
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways;
            const bool sendOnEnter = ImGui::InputTextWithHint("##ChatInput", "Type a message...",
                                                              app.chatInput, sizeof(app.chatInput),
                                                              inputFlags, ChatInputCallback,
                                                              &chatInputCursor);
            if (fontChat)
                ImGui::PopFont();
            if (inputNeedsShaping)
                ImGui::PopStyleColor(2);
            const bool inputActive = ImGui::IsItemActive() || ImGui::IsItemFocused();
            if (inputActive)
                chatInputActive = true;
            if (inputNeedsShaping) {
                const ImVec2 inputMin = ImGui::GetItemRectMin();
                const ImVec2 inputMax = ImGui::GetItemRectMax();
                const ImVec2 pad = ImGui::GetStyle().FramePadding;
                const float innerW = std::max(1.0f, (inputMax.x - inputMin.x) - pad.x * 2.0f);
                const float innerH = std::max(1.0f, (inputMax.y - inputMin.y) - pad.y * 2.0f);
                const float targetInputH = innerH + std::max(1.0f, pad.y * 0.8f);
                const ImVec2 textMin(inputMin.x + pad.x, inputMin.y + pad.y);
                float inputFontSize = textFont->FontSize * 0.60f;

                const bool rebuild = chatInputCached != app.chatInput ||
                                     chatInputFontSize != inputFontSize ||
                                     chatInputColor.x != inputTextColor.x ||
                                     chatInputColor.y != inputTextColor.y ||
                                     chatInputColor.z != inputTextColor.z ||
                                     chatInputColor.w != inputTextColor.w;
                if (rebuild) {
                    std::wstring wide = WideFromUtf8(std::string(app.chatInput));
                    ChatTextTexture next;
                    float caretX = 0.0f;
                    float caretY = 0.0f;
                    float caretH = inputFontSize;
                    bool rendered = RenderChatTextTexture(wide, std::max(4096.0f, innerW), inputFontSize,
                                                          inputTextColor, next, DWRITE_WORD_WRAPPING_NO_WRAP,
                                                          &caretX, &caretY, &caretH, chatInputCursor.cursor);
                    if (rendered && next.size.y > targetInputH && inputFontSize > 1.0f) {
                        const float minScale = 0.85f;
                        const float scale = std::max(minScale,
                                                     (targetInputH - 1.0f) / next.size.y);
                        const float scaledFont = std::max(1.0f, inputFontSize * scale);
                        ChatTextTexture scaled;
                        float caretX2 = 0.0f;
                        float caretY2 = 0.0f;
                        float caretH2 = scaledFont;
                        if (RenderChatTextTexture(wide, std::max(4096.0f, innerW), scaledFont,
                                                  inputTextColor, scaled, DWRITE_WORD_WRAPPING_NO_WRAP,
                                                  &caretX2, &caretY2, &caretH2, chatInputCursor.cursor)) {
                            ReleaseChatTexture(next);
                            next = scaled;
                            inputFontSize = scaledFont;
                            caretX = caretX2;
                            caretY = caretY2;
                            caretH = caretH2;
                        } else {
                            ReleaseChatTexture(scaled);
                        }
                    }
                    if (rendered) {
                        ReleaseChatTexture(chatInputTex);
                        chatInputTex = next;
                        chatInputCached = app.chatInput;
                        chatInputFontSize = inputFontSize;
                        chatInputColor = inputTextColor;
                        chatInputCaretX = caretX;
                        chatInputCaretY = caretY;
                        chatInputCaretH = caretH;
                    } else {
                        ReleaseChatTexture(chatInputTex);
                        chatInputCached.clear();
                    }
                }

                ImDrawList* draw = ImGui::GetWindowDrawList();
                if (chatInputTex.srv) {
                    float scrollX = 0.0f;
                    if (chatInputTex.size.x > innerW) {
                        const float maxScroll = chatInputTex.size.x - innerW;
                        scrollX = std::min(maxScroll, std::max(0.0f, chatInputCaretX - innerW + 1.0f));
                    }
                    const float textY = textMin.y + std::max(0.0f, (innerH - chatInputTex.size.y) * 0.5f);
                    ImGui::PushClipRect(textMin, ImVec2(textMin.x + innerW, textMin.y + innerH), true);
                    const ImVec2 texMin(textMin.x - scrollX, textY);
                    const ImVec2 texMax(texMin.x + chatInputTex.size.x, texMin.y + chatInputTex.size.y);
                    draw->AddImage((ImTextureID)chatInputTex.srv, texMin, texMax);
                    if (inputActive) {
                        const float caretX = textMin.x + chatInputCaretX - scrollX;
                        const float caretY = textY + chatInputCaretY;
                        const float caretH = std::max(chatInputCaretH, ImGui::GetTextLineHeight());
                        const ImU32 caretCol = ImGui::GetColorU32(ImGuiCol_Text);
                        draw->AddLine(ImVec2(caretX, caretY), ImVec2(caretX, caretY + caretH), caretCol, 1.0f);
                    }
                    ImGui::PopClipRect();
                } else {
                    draw->AddText(textMin, ImGui::GetColorU32(ImGuiCol_Text), app.chatInput);
                }
            } else if (chatInputTex.srv) {
                ReleaseChatTexture(chatInputTex);
                chatInputCached.clear();
            }
            ImGui::SameLine();
            bool sendNow = sendOnEnter;
            if (ImGui::Button("Send"))
                sendNow = true;
            if (sendNow) {
                if (app.chatInput[0] != '\0') {
                    const std::string who = app.nickname[0] == '\0' ? "You" : app.nickname;
                    const std::string textToSend = app.chatInput;
                    ChatLine line;
                    line.who = who;
                    line.text = textToSend;
                    line.time = ChatTimestamp();
                    line.kind = ChatLineKind::Text;
                    line.status = ChatLineStatus::Sending;
                    const bool sent = session.sendChatMessage(textToSend);
                    line.status = sent ? ChatLineStatus::Sent : ChatLineStatus::Failed;
                    app.chat.push_back(std::move(line));
                    if (app.chat.size() > 200)
                        app.chat.pop_front();
                    app.chatInput[0] = '\0';
                    app.dirty = true;
                    forceChatScroll = true;
                }
                focusChatInput = true;
            }
            }
            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            if (chatDockedSidebar) {
                ImGui::EndChild();
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor();
            } else {
                EndPanel();
            }

            if (app.showEmoji) {
                // Full-colour emoji picker: each emoji is a D2D-rendered colour
                // texture, tight-cropped and centred in a fixed square cell.
                static std::unordered_map<int, ChatTextTexture> emojiTexCache;
                const float emojiCell = tune(30.0f);
                const float emojiPad = tune(8.0f);
                const int emojiPerRow = 8;
                const int emojiRows = (kEmojiListCount + emojiPerRow - 1) / emojiPerRow;
                const float itemGap = ImGui::GetStyle().ItemSpacing.x;
                const float emojiW = emojiPad * 2.0f + emojiPerRow * emojiCell +
                                     (emojiPerRow - 1) * itemGap;
                const float emojiH = emojiPad * 2.0f + emojiRows * emojiCell +
                                     (emojiRows - 1) * ImGui::GetStyle().ItemSpacing.y;
                ImGui::SetNextWindowBgAlpha(0.96f);
                ImGui::SetNextWindowPos(ImVec2(panelPos.x + panelSize.x - emojiW - tune(4.0f),
                                               panelPos.y + panelSize.y - emojiH - tune(4.0f)),
                                        ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(emojiW, emojiH), ImGuiCond_Always);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(emojiPad, emojiPad));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, tune(10.0f));
                ImGui::Begin("Emoji", &app.showEmoji,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize);
                if (ImGui::IsWindowHovered(hoverFlags))
                    uiHovered = true;
                ImDrawList* edl = ImGui::GetWindowDrawList();
                for (int i = 0; i < kEmojiListCount; ++i) {
                    auto texIt = emojiTexCache.find(i);
                    if (texIt == emojiTexCache.end()) {
                        ChatTextTexture tex;
                        RenderChatTextTexture(WideFromUtf8(kEmojiList[i]), 240.0f,
                                              fontChat ? fontChat->FontSize : 24.0f,
                                              ImVec4(1, 1, 1, 1), tex,
                                              DWRITE_WORD_WRAPPING_WRAP,
                                              nullptr, nullptr, nullptr, -1,
                                              /*cropTight=*/true);
                        texIt = emojiTexCache.emplace(i, std::move(tex)).first;
                    }
                    const ChatTextTexture& tex = texIt->second;
                    ImGui::PushID(i);
                    bool emojiClicked = false;
                    if (tex.srv && tex.size.x > 0.0f && tex.size.y > 0.0f) {
                        emojiClicked = ImGui::InvisibleButton("##emoji", ImVec2(emojiCell, emojiCell));
                        const ImVec2 cMin = ImGui::GetItemRectMin();
                        const ImVec2 cMax = ImGui::GetItemRectMax();
                        if (ImGui::IsItemHovered())
                            edl->AddRectFilled(cMin, cMax,
                                               ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)),
                                               tune(6.0f));
                        const float fit = std::min((emojiCell * 0.84f) / tex.size.x,
                                                   (emojiCell * 0.84f) / tex.size.y);
                        const ImVec2 half(tex.size.x * fit * 0.5f, tex.size.y * fit * 0.5f);
                        const ImVec2 c((cMin.x + cMax.x) * 0.5f, (cMin.y + cMax.y) * 0.5f);
                        edl->AddImage(reinterpret_cast<ImTextureID>(tex.srv),
                                      ImVec2(c.x - half.x, c.y - half.y),
                                      ImVec2(c.x + half.x, c.y + half.y));
                    } else {
                        if (fontChat)
                            ImGui::PushFont(fontChat);
                        emojiClicked = ImGui::Button(kEmojiList[i], ImVec2(emojiCell, emojiCell));
                        if (fontChat)
                            ImGui::PopFont();
                    }
                    if (emojiClicked) {
                        std::strncat(app.chatInput, kEmojiList[i],
                                     sizeof(app.chatInput) - std::strlen(app.chatInput) - 1);
                        app.showEmoji = false;
                    }
                    ImGui::PopID();
                    if ((i + 1) % emojiPerRow != 0)
                        ImGui::SameLine();
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
            }
    }
}
