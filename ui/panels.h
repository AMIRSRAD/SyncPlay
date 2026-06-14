#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include "imgui.h"

struct mpv_handle;
struct AppState;
class SyncSession;

// Per-frame bundle of everything the right-side panels need from the main loop.
// Constructed fresh each frame (reference members bind to the current loop locals).
// Each Draw*Panel function aliases these back to local names so the panel body
// can be moved out of wWinMain verbatim.
struct PanelContext {
    AppState& app;
    SyncSession& session;
    mpv_handle* mpv;

    // In/out chrome feedback (written by panels, read later in the frame).
    bool& uiHovered;
    bool& panelRectHovered;
    bool& nextPanelHeaderValid;
    ImVec2& nextPanelHeaderMin;
    ImVec2& nextPanelHeaderMax;

    // Per-frame geometry.
    float panelAreaLeft;
    float panelAreaTop;
    float panelAreaW;
    float panelAreaH;
    float panelHeaderH;
    float panelFade;
    ImVec2 basePad;
    ImGuiHoveredFlags hoverFlags;

    // Fonts (may change on a live DPI rebuild).
    ImFont* font10;
    ImFont* font12;
    ImFont* font14;
    ImFont* font16;
    ImFont* font18;
    ImFont* font22;
    ImFont* fontChat;
    ImFont* fontIcons;
    ImFont* fontIconsSmall;
    ImFont* fontIconsLarge;
    ImFont* fontIconsTiny;

    ImVec4 accent;

    // Callbacks into main-loop logic.
    std::function<ImVec2(float, float, float, float)> centeredSheetSize;
    std::function<float(float)> tune;
    std::function<void()> applyVideoColor;
    std::function<void()> applyToneMapping;
    std::function<void()> applyVideoShaders;
    std::function<void(const float*)> applyAccent;
    std::function<void()> applySubtitleStyle;
    std::function<void()> openSubtitles;

    // Chat-panel extras.
    std::size_t chatUnreadCount;
    std::size_t& chatSeenCount;
    bool& chatInputActive;
    std::function<void(const std::string&)> openFolder;
    std::function<void()> browseShareFile;
    std::string iconOpen;
    std::string iconChat;
    std::string iconOverlay;
    std::string iconSidebar;
};

void DrawSettingsPanel(PanelContext& ctx);
void DrawSessionPanel(PanelContext& ctx);
void DrawCallPanel(PanelContext& ctx);
void DrawSubsPanel(PanelContext& ctx);
void DrawChatPanel(PanelContext& ctx, const ImVec2& panelPos, const ImVec2& panelSize,
                   bool chatDockedSidebar);
