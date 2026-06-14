#include "panel_utils.h"

#include <algorithm>

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
                           ImGuiWindowFlags flags) {
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    bg.w = alpha * panelFade;
    ImGui::SetCursorPos(pos);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().ChildRounding;
    const ImU32 shadowCol = ImGui::GetColorU32(ImVec4(0, 0, 0, 0.25f * panelFade));
    dl->AddRectFilled(ImVec2(pos.x, pos.y + 2.0f),
                      ImVec2(pos.x + size.x, pos.y + size.y + 2.0f),
                      shadowCol, rounding + 2.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, basePad);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panelFade);
    ImGui::BeginChild(id, size, true, flags);
}

void BeginPanel(const char* id, const ImVec2& pos, const ImVec2& size,
                float alpha, float panelFade, const ImVec2& basePad) {
    BeginPanelImpl(id, pos, size, alpha, panelFade, basePad, ImGuiWindowFlags_None);
}

void BeginPanelNoScroll(const char* id, const ImVec2& pos, const ImVec2& size,
                        float alpha, float panelFade, const ImVec2& basePad) {
    BeginPanelImpl(id, pos, size, alpha, panelFade, basePad,
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
