#include "panel_utils.h"

#include <algorithm>

#include "../platform/platform.h"

namespace {
void ClampPanel(float* pos, float* size, float minW, float minH, float maxW, float maxH,
                float edgePad, int ui_w, int ui_h) {
    size[0] = std::clamp(size[0], minW, maxW);
    size[1] = std::clamp(size[1], minH, maxH);
    pos[0] = std::clamp(pos[0], edgePad, static_cast<float>(ui_w) - size[0] - edgePad);
    pos[1] = std::clamp(pos[1], edgePad, static_cast<float>(ui_h) - size[1] - edgePad);
}
}

void UpdatePanelDrag(PanelDragState& state, float* pos, float* size,
                     float minW, float minH, float maxW, float maxH,
                     float panelHeaderH, float resizeGrip, float edgePad,
                     int ui_w, int ui_h) {
    ImVec2 mouse = ImGui::GetIO().MousePos;
    const float left = pos[0];
    const float top = pos[1];
    const float right = pos[0] + size[0];
    const float bottom = pos[1] + size[1];
    const float headerBottom = top + panelHeaderH;
    const float gripLeft = right - resizeGrip;
    const float gripTop = bottom - resizeGrip;
    const float gripRight = left + resizeGrip;
    auto hit = [&](float x0, float y0, float x1, float y1) {
        return mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1;
    };
    const bool onCorner = hit(gripLeft, gripTop, right, bottom);
    const bool onRight = hit(gripLeft, top, right, bottom);
    const bool onBottom = hit(left, gripTop, right, bottom);
    const bool onLeft = hit(left, top, gripRight, bottom);
    if (onCorner)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (onRight)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (onLeft)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (onBottom)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    else if (hit(left, top, right, headerBottom))
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (onCorner || onRight || onBottom || onLeft) {
            state.resizing = true;
            state.dragging = false;
            state.grabOffset = mouse;
            state.startPos = ImVec2(pos[0], pos[1]);
            state.startSize = ImVec2(size[0], size[1]);
            if (onCorner)
                state.resizeAxis = 3;
            else if (onRight)
                state.resizeAxis = 1;
            else if (onBottom)
                state.resizeAxis = 2;
            else
                state.resizeAxis = 4;
        } else if (hit(left, top, right, headerBottom)) {
            state.dragging = true;
            state.resizing = false;
            state.resizeAxis = 0;
            state.grabOffset = ImVec2(mouse.x - left, mouse.y - top);
        }
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.dragging = false;
        state.resizing = false;
        state.resizeAxis = 0;
    }

    if (state.dragging) {
        pos[0] = mouse.x - state.grabOffset.x;
        pos[1] = mouse.y - state.grabOffset.y;
    }
    if (state.resizing) {
        if (state.resizeAxis & 1)
            size[0] = state.startSize.x + (mouse.x - state.grabOffset.x);
        if (state.resizeAxis & 2)
            size[1] = state.startSize.y + (mouse.y - state.grabOffset.y);
        if (state.resizeAxis & 4) {
            const float delta = mouse.x - state.grabOffset.x;
            size[0] = state.startSize.x - delta;
            pos[0] = state.startPos.x + delta;
        }
    }
    ClampPanel(pos, size, minW, minH, maxW, maxH, edgePad, ui_w, ui_h);
}

static void BeginPanelImpl(const char* id, const ImVec2& pos, const ImVec2& size,
                           float alpha, float panelFade, const ImVec2& basePad,
                           ImGuiWindowFlags flags, float padScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().ChildRounding;
    const ImVec2 pMin = pos;
    const ImVec2 pMax(pos.x + size.x, pos.y + size.y);

    // Frosted-glass backdrop: draw the blurred frame behind the panel, UV-mapped
    // to this panel's on-screen region (the Surface window is at 0,0, so panel
    // coords are screen coords), then a translucent tint, a top highlight and a
    // light glass border.
    bool frosted = false;
    if (g_blurSrv && g_blurReady) {
        // The blur texture is a downsample of the video frame, which is drawn from
        // (0,0) to (g_videoTexW, g_videoTexH) — NOT the full window (the bottom bar and
        // any side dock are excluded). Map panel coords over the video rect, not the
        // DisplaySize, so the frosted backdrop lines up with what's actually behind it.
        const float vw = static_cast<float>(g_videoTexW);
        const float vh = static_cast<float>(g_videoTexH);
        if (vw > 1.0f && vh > 1.0f) {
            const ImVec2 uvMin(pMin.x / vw, pMin.y / vh);
            const ImVec2 uvMax(pMax.x / vw, pMax.y / vh);
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(g_blurSrv), pMin, pMax, uvMin, uvMax,
                                ImGui::GetColorU32(ImVec4(1, 1, 1, panelFade)), rounding);
            ImVec4 tint = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            tint.w = 0.52f * panelFade;
            dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(tint), rounding);
            dl->AddRect(pMin, pMax, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f * panelFade)),
                        rounding, 0, 1.0f);
            frosted = true;
        }
    }

    ImVec4 childBg(0.0f, 0.0f, 0.0f, 0.0f);
    if (!frosted) {
        childBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        childBg.w = alpha * panelFade;
    }
    // Generous inner padding so titles/content don't hug the panel corner.
    // Derive it from the font size (DPI-aware) rather than basePad/WindowPadding,
    // which the Surface window's zero-padding leaves at ~0 by the time we get here.
    const float fs = ImGui::GetFontSize();
    // padScale shrinks the whole padding (including the basePad floor) so a panel
    // can go tighter than the default WindowPadding when it asks for it (e.g. Chat).
    ImVec2 panelPad(std::max(basePad.x, fs * 1.6f), std::max(basePad.y, fs * 1.25f));
    panelPad.x *= padScale;
    panelPad.y *= padScale;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, childBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panelPad);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panelFade);
    ImGui::SetCursorPos(pos);
    // AlwaysUseWindowPadding: a borderless child otherwise ignores WindowPadding,
    // which made the frosted panels' content hug the top-left corner.
    ImGui::BeginChild(id, size, !frosted, flags | ImGuiWindowFlags_AlwaysUseWindowPadding);
}

void BeginPanel(const char* id, const ImVec2& pos, const ImVec2& size,
                float alpha, float panelFade, const ImVec2& basePad, float padScale) {
    BeginPanelImpl(id, pos, size, alpha, panelFade, basePad, ImGuiWindowFlags_None, padScale);
}

void BeginPanelNoScroll(const char* id, const ImVec2& pos, const ImVec2& size,
                        float alpha, float panelFade, const ImVec2& basePad, float padScale) {
    BeginPanelImpl(id, pos, size, alpha, panelFade, basePad,
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, padScale);
}

void EndPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DockResizeHandle(const char* id, float panelWidth, float panelHeight,
                      float& dockedH, float minH, float maxH, float resizeGrip) {
    ImGui::SetCursorPos(ImVec2(0.0f, panelHeight - resizeGrip));
    ImGui::InvisibleButton(id, ImVec2(panelWidth, resizeGrip));
    if (ImGui::IsItemActive())
        dockedH = std::clamp(dockedH + ImGui::GetIO().MouseDelta.y, minH, maxH);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
}
