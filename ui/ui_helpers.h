#pragma once

#include <imgui.h>
#include <string>

std::string Utf8FromCodepoint(ImWchar c);

void DrawIconGlow(const ImVec2& min, const ImVec2& max, ImFont* font, const char* icon);
void StyledTooltip(const char* text);
void ShowDelayedTooltip(const char* text, float delaySeconds = 1.0f);

// Shared settings-panel anatomy so every tab reads the same way.
// PanelSection: accent pip + label + hairline running to the panel's right edge.
void PanelSection(const char* label);
// PanelRowLabel: label on the left, then positions the cursor so the following
// widget fills the rest of the row (label-left / control-right layout).
void PanelRowLabel(const char* label, float labelWidth);
void PushIconGlowOffset(const ImVec2& offset);
void PopIconGlowOffset();
bool IconButton(const char* id, const char* icon, const char* tooltip,
                ImFont* iconFont, ImVec2 size, bool glow = true);
bool IconButtonFont(const char* id, const char* icon, const char* tooltip,
                    ImFont* font, ImVec2 size, bool glow = true);
bool IconToggle(const char* id, const char* icon, const char* tooltip,
                bool active, ImFont* iconFont, ImVec2 size, bool glow = true);
