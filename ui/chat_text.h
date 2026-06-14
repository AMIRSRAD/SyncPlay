#pragma once

// Shared chat text-rendering helpers. Definitions live in src/main.cpp; declared
// here so the extracted chat panel (ui/panels.cpp) can use them too.

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <dwrite.h>

#include "imgui.h"
#include "app_state.h"

struct ChatTextTexture {
    ID3D11ShaderResourceView* srv = nullptr;
    ImVec2 size{0.0f, 0.0f};
};

struct ChatInputCursor {
    int cursor = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
};

int ChatInputCallback(ImGuiInputTextCallbackData* data);
bool NeedsComplexText(const char* utf8);
void ReleaseChatTexture(ChatTextTexture& tex);
bool RenderChatTextTexture(const std::wstring& text, float wrapWidth, float fontSize,
                           const ImVec4& color, ChatTextTexture& out,
                           DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_WRAP,
                           float* outCaretX = nullptr, float* outCaretY = nullptr,
                           float* outCaretH = nullptr, int caretPos = -1);

std::string ChatTimestamp();
std::string FormatBytes(int64_t bytes);
const char* ChatStatusLabel(ChatLineStatus status);
std::string FileNameFromPath(const std::string& path);

extern const char* kEmojiList[];
extern const int kEmojiListCount;
