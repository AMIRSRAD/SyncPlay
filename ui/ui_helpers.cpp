#include "ui_helpers.h"

#include <imgui_internal.h>

#include <cmath>
#include <unordered_map>
#include <vector>

namespace {
std::vector<ImVec2> g_glowOffsetStack;

// Eased press-scale for icon buttons: the icon shrinks slightly while held and
// springs back on release. Keyed by the button's ImGui id.
float IconPressScale(ImGuiID id, bool held) {
    static std::unordered_map<ImGuiID, float> s_scale;
    auto it = s_scale.find(id);
    float v = (it == s_scale.end()) ? 1.0f : it->second;
    const float target = held ? 0.86f : 1.0f;
    const float dt = ImGui::GetIO().DeltaTime;
    v += (target - v) * (1.0f - std::exp(-dt / 0.05f));
    s_scale[id] = v;
    return v;
}

ImVec2 CurrentGlowOffset() {
    if (g_glowOffsetStack.empty())
        return ImVec2(0.0f, 0.0f);
    return g_glowOffsetStack.back();
}
} // namespace

std::string Utf8FromCodepoint(ImWchar c) {
    char buf[5] = {0};
    const char* out = ImTextCharToUtf8(buf, static_cast<unsigned int>(c));
    if (!out || out[0] == '\0')
        return std::string();
    return std::string(out);
}

void DrawIconGlow(const ImVec2& min, const ImVec2& max, ImFont* font, const char* icon) {
    if (!font || !icon || !icon[0])
        return;
    const float fontSize = font->FontSize;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, icon);
    const ImVec2 align = ImGui::GetStyle().ButtonTextAlign;
    ImVec2 pos = min;
    if (align.x > 0.0f)
        pos.x = ImMax(pos.x, pos.x + (max.x - pos.x - textSize.x) * align.x);
    if (align.y > 0.0f)
        pos.y = ImMax(pos.y, pos.y + (max.y - pos.y - textSize.y) * align.y);
    pos.x = IM_FLOOR(pos.x);
    pos.y = IM_FLOOR(pos.y);
    const ImVec2 offset = CurrentGlowOffset();
    pos.x += offset.x;
    pos.y += offset.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 glow = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
    glow.w *= 0.35f;
    const ImU32 glowCol = ImGui::GetColorU32(glow);
    const float g = 1.0f;
    dl->AddText(font, fontSize, ImVec2(pos.x - g, pos.y), glowCol, icon);
    dl->AddText(font, fontSize, ImVec2(pos.x + g, pos.y), glowCol, icon);
    dl->AddText(font, fontSize, ImVec2(pos.x, pos.y - g), glowCol, icon);
    dl->AddText(font, fontSize, ImVec2(pos.x, pos.y + g), glowCol, icon);
}

void PushIconGlowOffset(const ImVec2& offset) {
    g_glowOffsetStack.push_back(offset);
}

void PopIconGlowOffset() {
    if (!g_glowOffsetStack.empty())
        g_glowOffsetStack.pop_back();
}

void StyledTooltip(const char* text) {
    if (!text || !text[0])
        return;
    // Tooltips inherit the Surface window's pushed WindowRounding(0)/WindowPadding(0)
    // for the rest of the frame, so style them explicitly to match the glass theme:
    // a softly rounded, padded, translucent-dark popup with a faint light border.
    const float fs = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, fs * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.7f, fs * 0.45f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.10f, 0.13f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
    if (ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void ShowDelayedTooltip(const char* text, float delaySeconds) {
    if (!text || !text[0])
        return;
    ImGuiID id = ImGui::GetItemID();
    static ImGuiID s_hoverId = 0;
    static double s_hoverStart = 0.0;
    if (ImGui::IsItemHovered()) {
        const double now = ImGui::GetTime();
        if (id != s_hoverId) {
            s_hoverId = id;
            s_hoverStart = now;
        }
        if ((now - s_hoverStart) >= delaySeconds)
            StyledTooltip(text);
    } else if (id == s_hoverId) {
        s_hoverId = 0;
        s_hoverStart = 0.0;
    }
}

bool IconButton(const char* id, const char* icon, const char* tooltip,
                ImFont* iconFont, ImVec2 size, bool glow) {
    ImGui::PushID(id);
    if (iconFont)
        ImGui::PushFont(iconFont);
    const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, transparent);
    const ImGuiID btnId = ImGui::GetID(icon);
    ImGui::SetWindowFontScale(IconPressScale(btnId, ImGui::GetActiveID() == btnId));
    bool pressed = ImGui::Button(icon, size);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor(3);
    if (iconFont)
        ImGui::PopFont();
    if (glow && ImGui::IsItemHovered()) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        DrawIconGlow(ImVec2(min.x + pad.x, min.y + pad.y),
                     ImVec2(max.x - pad.x, max.y - pad.y),
                     iconFont ? iconFont : ImGui::GetFont(), icon);
    }
    if (tooltip)
        ShowDelayedTooltip(tooltip);
    ImGui::PopID();
    return pressed;
}

bool IconButtonFont(const char* id, const char* icon, const char* tooltip,
                    ImFont* font, ImVec2 size, bool glow) {
    ImGui::PushID(id);
    if (font)
        ImGui::PushFont(font);
    const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, transparent);
    const ImGuiID btnId = ImGui::GetID(icon);
    ImGui::SetWindowFontScale(IconPressScale(btnId, ImGui::GetActiveID() == btnId));
    bool pressed = ImGui::Button(icon, size);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor(3);
    if (font)
        ImGui::PopFont();
    if (glow && ImGui::IsItemHovered()) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        DrawIconGlow(ImVec2(min.x + pad.x, min.y + pad.y),
                     ImVec2(max.x - pad.x, max.y - pad.y),
                     font, icon);
    }
    if (tooltip)
        ShowDelayedTooltip(tooltip);
    ImGui::PopID();
    return pressed;
}

bool IconToggle(const char* id, const char* icon, const char* tooltip,
                bool active, ImFont* iconFont, ImVec2 size, bool glow) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    }
    bool pressed = IconButton(id, icon, tooltip, iconFont, size, glow);
    if (active)
        ImGui::PopStyleColor(2);
    return pressed;
}
