#pragma once

#include <imgui.h>
#include <string>

std::string Utf8FromCodepoint(ImWchar c);

void DrawIconGlow(const ImVec2& min, const ImVec2& max, ImFont* font, const char* icon);
void StyledTooltip(const char* text);
void ShowDelayedTooltip(const char* text, float delaySeconds = 1.0f);
void PushIconGlowOffset(const ImVec2& offset);
void PopIconGlowOffset();
bool IconButton(const char* id, const char* icon, const char* tooltip,
                ImFont* iconFont, ImVec2 size, bool glow = true);
bool IconButtonFont(const char* id, const char* icon, const char* tooltip,
                    ImFont* font, ImVec2 size, bool glow = true);
bool IconToggle(const char* id, const char* icon, const char* tooltip,
                bool active, ImFont* iconFont, ImVec2 size, bool glow = true);
