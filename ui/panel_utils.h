#pragma once

#include <imgui.h>

struct PanelDragState {
    bool dragging = false;
    bool resizing = false;
    int resizeAxis = 0;
    ImVec2 grabOffset = ImVec2(0.0f, 0.0f);
    ImVec2 startPos = ImVec2(0.0f, 0.0f);
    ImVec2 startSize = ImVec2(0.0f, 0.0f);
};

void UpdatePanelDrag(PanelDragState& state, float* pos, float* size,
                     float minW, float minH, float maxW, float maxH,
                     float panelHeaderH, float resizeGrip, float edgePad,
                     int ui_w, int ui_h);

void BeginPanel(const char* id, const ImVec2& pos, const ImVec2& size,
                float alpha, float panelFade, const ImVec2& basePad, float padScale = 1.0f);
void BeginPanelNoScroll(const char* id, const ImVec2& pos, const ImVec2& size,
                        float alpha, float panelFade, const ImVec2& basePad, float padScale = 1.0f);
void EndPanel();

void DockResizeHandle(const char* id, float panelWidth, float panelHeight,
                      float& dockedH, float minH, float maxH, float resizeGrip);
