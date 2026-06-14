#include "platform/platform.h"
#include "render/render_sw.h"
#include "media/mpv_helpers.h"
#include "ui/app_state.h"
#include "ui/ui_helpers.h"
#include "ui/panel_utils.h"
#include "ui/panels.h"
#include "ui/chat_text.h"
#include "platform/file_dialog.h"
#include "core/utf.h"
#include "core/logging.h"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include <mpv/client.h>
#include <mpv/render.h>

#include <tchar.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sync/sync_session.h"
#include "../core/playback_controller.h"
#include "resource.h"

std::string ChatTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    return buf;
}

std::string FormatBytes(int64_t bytes) {
    if (bytes <= 0)
        return "Unknown size";
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32]{};
    if (unit == 0)
        std::snprintf(buf, sizeof(buf), "%lld %s", static_cast<long long>(bytes), units[unit]);
    else
        std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

const char* ChatStatusLabel(ChatLineStatus status) {
    switch (status) {
    case ChatLineStatus::Sending: return "sending";
    case ChatLineStatus::Receiving: return "receiving";
    case ChatLineStatus::Sent: return "sent";
    case ChatLineStatus::Failed: return "failed";
    case ChatLineStatus::Received: return "received";
    default: return "";
    }
}

static void TrimChatHistory(AppState& app) {
    while (app.chat.size() > 200)
        app.chat.pop_front();
}

static bool IsSystemChatLine(const ChatLine& line) {
    return line.kind == ChatLineKind::System || line.who == "System";
}

static void AppendSystemTimelineChip(AppState& app, const std::string& text) {
    if (text.empty())
        return;
    if (!app.chat.empty()) {
        const ChatLine& last = app.chat.back();
        if (IsSystemChatLine(last) && last.text == text)
            return;
    }

    ChatLine line;
    line.who = "System";
    line.text = text;
    line.time = ChatTimestamp();
    line.kind = ChatLineKind::System;
    line.status = ChatLineStatus::None;
    app.chat.push_back(std::move(line));
    TrimChatHistory(app);
    app.dirty = true;
}

static bool IsPeerTimelineAction(const std::string& message) {
    return message == "Guest joined" ||
           message == "Guest left" ||
           message == "Host joined" ||
           message == "Host left";
}

static bool IsInactiveVoiceState(const std::string& state) {
    return state.empty() || state == "Idle" || state == "Closed";
}

static std::string TransportTimelineLabel(const std::string& previous, const std::string& current) {
    if (current == previous)
        return {};
    if (current == "Connected")
        return "Connected";
    if (previous == "Connected" && current != "Connected")
        return "Disconnected";
    if (current == "Failed")
        return "Connection failed";
    return {};
}

static std::string FileTimelineLabel(const std::string& status) {
    if (status == "Files match")
        return "Files verified";
    if (status == "File mismatch")
        return "File mismatch";
    if (status == "Host file ready")
        return "Host file ready";
    return {};
}

static std::string VoiceTimelineLabel(const std::string& previous, const std::string& current) {
    if (current == previous)
        return {};
    if (current == "Connected")
        return "Voice started";
    if (current == "Failed")
        return "Voice failed";
    if (IsInactiveVoiceState(current) && !IsInactiveVoiceState(previous))
        return "Voice ended";
    return {};
}

static void DrawMutedMicSlash(bool enabled) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float buttonW = max.x - min.x;
    const float buttonH = max.y - min.y;
    const float s = std::min(buttonW, buttonH);
    ImVec4 slash = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
    slash.w = enabled ? 0.95f : 0.45f;
    const ImU32 slashCol = ImGui::GetColorU32(slash);
    const float thickness = std::max(1.25f, s * 0.07f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x + buttonW * 0.29f, min.y + buttonH * 0.24f),
                                        ImVec2(max.x - buttonW * 0.35f, max.y - buttonH * 0.30f),
                                        slashCol, thickness);
}

std::string FileNameFromPath(const std::string& path) {
    if (path.empty())
        return {};
    try {
        const std::filesystem::path fsPath(path);
        const std::string name = fsPath.filename().string();
        return name.empty() ? path : name;
    } catch (...) {
        return path;
    }
}

struct AppIconAssets {
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
    HICON iconBig = nullptr;
    HICON iconSmall = nullptr;
};

const char* kEmojiList[] = {
    "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\x85", "\xF0\x9F\x98\x89",
    "\xF0\x9F\x98\x8D", "\xF0\x9F\x98\xB2", "\xF0\x9F\x98\xB4", "\xF0\x9F\x98\xB7",
    "\xF0\x9F\x98\xA2", "\xF0\x9F\x98\xAD", "\xF0\x9F\x98\xA4", "\xF0\x9F\x98\xA1",
    "\xF0\x9F\x98\x8E", "\xF0\x9F\x98\xB1", "\xF0\x9F\x98\xB9", "\xF0\x9F\x91\x8D"
};
const int kEmojiListCount = static_cast<int>(sizeof(kEmojiList) / sizeof(kEmojiList[0]));
static const ImWchar kEmojiRanges[] = {
    0x2600, 0x27BF, // Misc symbols + dingbats
    0x1F300, 0x1F5FF, // Misc symbols and pictographs
    0x1F600, 0x1F64F, // Emoticons
    0x1F680, 0x1F6FF, // Transport and map
    0x1F900, 0x1F9FF, // Supplemental symbols and pictographs
    0
};
static const ImWchar kArabicRanges[] = {
    0x0600, 0x06FF, // Arabic
    0x0750, 0x077F, // Arabic Supplement
    0x08A0, 0x08FF, // Arabic Extended-A
    0xFB50, 0xFDFF, // Arabic Presentation Forms-A
    0xFE70, 0xFEFF, // Arabic Presentation Forms-B
    0
};

static bool CreateTextureFromBGRA(const uint8_t* pixels, UINT width, UINT height,
                                  ID3D11ShaderResourceView** out_srv);

int ChatInputCallback(ImGuiInputTextCallbackData* data) {
    if (!data || !data->UserData)
        return 0;
    ChatInputCursor* cursor = static_cast<ChatInputCursor*>(data->UserData);
    cursor->cursor = data->CursorPos;
    cursor->selectionStart = data->SelectionStart;
    cursor->selectionEnd = data->SelectionEnd;
    return 0;
}

bool NeedsComplexText(const char* utf8) {
    if (!utf8)
        return false;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(utf8); *c; ++c) {
        if (*c >= 0x80)
            return true;
    }
    return false;
}

void ReleaseChatTexture(ChatTextTexture& tex) {
    if (tex.srv) {
        tex.srv->Release();
        tex.srv = nullptr;
    }
}

bool RenderChatTextTexture(const std::wstring& text, float wrapWidth, float fontSize,
                           const ImVec4& color, ChatTextTexture& out,
                           DWRITE_WORD_WRAPPING wrapping,
                           float* outCaretX, float* outCaretY,
                           float* outCaretH, int caretPos) {
    using Microsoft::WRL::ComPtr;
    static ComPtr<IDWriteFactory> dwriteFactory;
    static ComPtr<ID2D1Factory> d2dFactory;
    static ComPtr<IWICImagingFactory> wicFactory;
    if (!dwriteFactory) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()))))
            return false;
    }
    if (!d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory), nullptr,
                                     reinterpret_cast<void**>(d2dFactory.GetAddressOf()))))
            return false;
    }
    if (!wicFactory) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&wicFactory))))
            return false;
    }

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr,
                                               DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               fontSize, L"", &format)))
        return false;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(wrapping);

    const float maxWidth = std::max(1.0f, wrapWidth);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory->CreateTextLayout(text.c_str(),
                                               static_cast<UINT32>(text.size()),
                                               format.Get(), maxWidth, 4096.0f, &layout)))
        return false;

    ComPtr<IDWriteFactory2> dwriteFactory2;
    if (SUCCEEDED(dwriteFactory.As(&dwriteFactory2))) {
        ComPtr<IDWriteTextFormat1> format1;
        if (SUCCEEDED(format.As(&format1))) {
            ComPtr<IDWriteFontFallback> fallback;
            if (SUCCEEDED(dwriteFactory2->GetSystemFontFallback(&fallback)) && fallback)
                format1->SetFontFallback(fallback.Get());
        }
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)))
        return false;
    DWRITE_OVERHANG_METRICS overhang{};
    if (FAILED(layout->GetOverhangMetrics(&overhang)))
        overhang = {};

    const float minX = std::min(0.0f, overhang.left);
    const float minY = std::min(0.0f, overhang.top);
    const float maxX = metrics.widthIncludingTrailingWhitespace + std::max(0.0f, overhang.right);
    const float maxY = metrics.height + std::max(0.0f, overhang.bottom);
    const float originX = -minX;
    const float originY = -minY;
    const UINT width = std::max<UINT>(1, static_cast<UINT>(std::ceil(maxX - minX)));
    const UINT height = std::max<UINT>(1, static_cast<UINT>(std::ceil(maxY - minY)));

    float caretX = 0.0f;
    float caretY = 0.0f;
    float caretH = fontSize;
    if (caretPos >= 0 && (outCaretX || outCaretY || outCaretH)) {
        const UINT32 textLen = static_cast<UINT32>(text.size());
        DWRITE_HIT_TEST_METRICS caretMetrics{};
        FLOAT hitX = 0.0f;
        FLOAT hitY = 0.0f;
        if (textLen > 0) {
            const UINT32 hitPos = caretPos >= static_cast<int>(textLen) ? textLen - 1 : static_cast<UINT32>(caretPos);
            const BOOL trailing = caretPos >= static_cast<int>(textLen);
            if (SUCCEEDED(layout->HitTestTextPosition(hitPos, trailing, &hitX, &hitY, &caretMetrics)))
                caretH = caretMetrics.height;
        }
        caretX = hitX + originX;
        caretY = hitY + originY;
    }

    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapCacheOnLoad, &wicBitmap)))
        return false;

    D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2dFactory->CreateWicBitmapRenderTarget(wicBitmap.Get(), props, &rt)))
        return false;
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(color.x, color.y, color.z, color.w), &brush)))
        return false;

    ComPtr<IDWriteRenderingParams> renderParams;
    if (SUCCEEDED(dwriteFactory->CreateCustomRenderingParams(
            1.8f, 0.0f, 1.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &renderParams))) {
        rt->SetTextRenderingParams(renderParams.Get());
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE;
#ifdef D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
    options = static_cast<D2D1_DRAW_TEXT_OPTIONS>(options | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
#endif
    rt->DrawTextLayout(D2D1::Point2F(originX, originY), layout.Get(), brush.Get(), options);
    if (FAILED(rt->EndDraw()))
        return false;

    std::vector<uint8_t> pixels;
    pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const UINT stride = width * 4;
    const UINT imageBytes = stride * height;
    WICRect rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    if (FAILED(wicBitmap->CopyPixels(&rect, stride, imageBytes, pixels.data())))
        return false;

    // D2D renders with premultiplied alpha, but ImGui's DX11 blend state expects
    // straight (non-premultiplied) alpha (SrcBlend=SRC_ALPHA, DestBlend=INV_SRC_ALPHA).
    // Uploading premultiplied data multiplies the colour by alpha a second time at
    // blend time, which darkens the antialiased glyph edges and makes the text look
    // jagged/fringed. Convert back to straight alpha so the edges blend cleanly.
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const uint32_t a = pixels[i + 3];
        if (a == 0) {
            pixels[i + 0] = pixels[i + 1] = pixels[i + 2] = 0;
        } else if (a < 255) {
            pixels[i + 0] = static_cast<uint8_t>(std::min<uint32_t>(255u, (pixels[i + 0] * 255u + a / 2u) / a));
            pixels[i + 1] = static_cast<uint8_t>(std::min<uint32_t>(255u, (pixels[i + 1] * 255u + a / 2u) / a));
            pixels[i + 2] = static_cast<uint8_t>(std::min<uint32_t>(255u, (pixels[i + 2] * 255u + a / 2u) / a));
        }
    }

    ReleaseChatTexture(out);
    if (!CreateTextureFromBGRA(pixels.data(), width, height, &out.srv))
        return false;
    out.size = ImVec2(static_cast<float>(width), static_cast<float>(height));
    if (outCaretX)
        *outCaretX = caretX;
    if (outCaretY)
        *outCaretY = caretY;
    if (outCaretH)
        *outCaretH = caretH;
    return true;
}

static bool LoadPngBGRA(const std::wstring& path, std::vector<uint8_t>& pixels, UINT& width, UINT& height,
                        UINT targetW = 0, UINT targetH = 0) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return false;

    UINT srcW = 0;
    UINT srcH = 0;
    if (FAILED(frame->GetSize(&srcW, &srcH)))
        return false;
    ComPtr<IWICBitmapSource> source = frame;
    if (targetW > 0 && targetH > 0 && (targetW != srcW || targetH != srcH)) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)))
            return false;
        if (FAILED(scaler->Initialize(frame.Get(), targetW, targetH, WICBitmapInterpolationModeFant)))
            return false;
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)))
        return false;
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    if (FAILED(converter->GetSize(&width, &height)))
        return false;
    if (width == 0 || height == 0)
        return false;
    pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const UINT stride = width * 4;
    const UINT imageBytes = stride * height;
    if (FAILED(converter->CopyPixels(nullptr, stride, imageBytes, pixels.data())))
        return false;
    return true;
}

static bool CreateTextureFromBGRA(const uint8_t* pixels, UINT width, UINT height,
                                  ID3D11ShaderResourceView** out_srv) {
    if (!g_pd3dDevice || !pixels || width == 0 || height == 0)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = pixels;
    sub.SysMemPitch = width * 4;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex)))
        return false;
    const HRESULT hr = g_pd3dDevice->CreateShaderResourceView(tex, nullptr, out_srv);
    tex->Release();
    return SUCCEEDED(hr);
}

static HICON CreateIconFromBGRA(const uint8_t* pixels, UINT width, UINT height) {
    if (!pixels || width == 0 || height == 0)
        return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = static_cast<LONG>(width);
    bi.bV5Height = -static_cast<LONG>(height);
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* dibPixels = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                        DIB_RGB_COLORS, &dibPixels, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!colorBmp || !dibPixels) {
        if (colorBmp)
            DeleteObject(colorBmp);
        return nullptr;
    }
    std::memcpy(dibPixels, pixels, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    HBITMAP maskBmp = CreateBitmap(width, height, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

static HICON LoadIconFromFile(const std::wstring& path, UINT size) {
    if (path.empty() || size == 0)
        return nullptr;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return nullptr;
    return reinterpret_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                                              static_cast<int>(size), static_cast<int>(size),
                                              LR_LOADFROMFILE | LR_DEFAULTCOLOR));
}

static void ApplyRoundedMask(std::vector<uint8_t>& pixels, UINT width, UINT height, float radius) {
    if (pixels.empty() || width == 0 || height == 0 || radius <= 0.0f)
        return;
    const float r = std::min(radius, std::min(width, height) * 0.5f);
    const float r2 = r * r;
    const float edge = std::max(0.0f, r - 1.0f);
    const float edge2 = edge * edge;
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    auto apply_corner = [&](int x, int y, float cx, float cy) {
        const float dx = static_cast<float>(x) - cx;
        const float dy = static_cast<float>(y) - cy;
        const float dist2 = dx * dx + dy * dy;
        if (dist2 <= r2)
            return;
        uint8_t& a = pixels[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4 + 3];
        if (dist2 >= edge2 + 1.0f) {
            a = 0;
            return;
        }
        const float dist = std::sqrt(dist2);
        const float t = std::clamp(r - dist, 0.0f, 1.0f);
        a = static_cast<uint8_t>(static_cast<float>(a) * t);
    };
    const float left = r - 1.0f;
    const float right = static_cast<float>(w) - r;
    const float top = r - 1.0f;
    const float bottom = static_cast<float>(h) - r;
    for (int y = 0; y < h; ++y) {
        const bool topCorner = y < static_cast<int>(r);
        const bool bottomCorner = y >= h - static_cast<int>(r);
        if (!topCorner && !bottomCorner)
            continue;
        for (int x = 0; x < w; ++x) {
            const bool leftCorner = x < static_cast<int>(r);
            const bool rightCorner = x >= w - static_cast<int>(r);
            if (!(leftCorner || rightCorner))
                continue;
            if (leftCorner && topCorner)
                apply_corner(x, y, left, top);
            else if (rightCorner && topCorner)
                apply_corner(x, y, right, top);
            else if (leftCorner && bottomCorner)
                apply_corner(x, y, left, bottom);
            else if (rightCorner && bottomCorner)
                apply_corner(x, y, right, bottom);
        }
    }
}

static std::wstring GetExecutableDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        path.resize(pos + 1);
    return path;
}

static AppIconAssets g_appIcon;

static std::wstring GetExecutablePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

static bool SetRegistryValueString(HKEY root, const std::wstring& subkey,
                                   const std::wstring& valueName, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t* data = value.c_str();
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG res = RegSetValueExW(key,
                                    valueName.empty() ? nullptr : valueName.c_str(),
                                    0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(key);
    return res == ERROR_SUCCESS;
}

static bool RegisterOpenWith(const std::wstring& exePath, const std::wstring& iconPath) {
    if (exePath.empty())
        return false;
    const std::wstring progId = L"SyncPlay.Video";
    const std::wstring base = L"Software\\Classes\\";
    const std::wstring command = L"\"" + exePath + L"\" \"%1\"";
    const std::wstring friendly = L"SyncPlay";
    const std::wstring description = L"SyncPlay media player";
    const std::wstring iconValue = iconPath.empty() ? exePath : iconPath;

    bool ok = true;
    ok &= SetRegistryValueString(HKEY_CURRENT_USER, base + progId, L"", L"SyncPlay Video");
    ok &= SetRegistryValueString(HKEY_CURRENT_USER, base + progId + L"\\DefaultIcon", L"", iconValue);
    ok &= SetRegistryValueString(HKEY_CURRENT_USER, base + progId + L"\\shell\\open\\command", L"", command);

    ok &= SetRegistryValueString(HKEY_CURRENT_USER, base + L"Applications\\SyncPlay.exe", L"FriendlyAppName", friendly);
    ok &= SetRegistryValueString(HKEY_CURRENT_USER, base + L"Applications\\SyncPlay.exe", L"ApplicationDescription", description);
    ok &= SetRegistryValueString(HKEY_CURRENT_USER, base + L"Applications\\SyncPlay.exe\\shell\\open\\command", L"", command);

    const wchar_t* exts[] = { L".mp4", L".mkv", L".mov", L".avi", L".webm" };
    for (const wchar_t* ext : exts) {
        const std::wstring openWithProgids = base + ext + L"\\OpenWithProgids";
        ok &= SetRegistryValueString(HKEY_CURRENT_USER, openWithProgids, progId, L"");
        const std::wstring supportedTypes = base + L"Applications\\SyncPlay.exe\\SupportedTypes";
        ok &= SetRegistryValueString(HKEY_CURRENT_USER, supportedTypes, ext, L"");
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

class MpvPlaybackController final : public PlaybackController {
public:
    explicit MpvPlaybackController(mpv_handle* mpv) : m_mpv(mpv) {}
    bool loadFile(const std::string& path) override {
        if (!m_mpv)
            return false;
        const char* cmd[] = { "loadfile", path.c_str(), nullptr };
        return mpv_command(m_mpv, cmd) >= 0;
    }

    void play() override {
        mpv_set_flag(m_mpv, "pause", false);
    }

    void pause() override {
        mpv_set_flag(m_mpv, "pause", true);
    }

    void seek(double time) override {
        if (!m_mpv)
            return;
        mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &time);
    }

    void setSpeed(double speed) override {
        if (!m_mpv)
            return;
        mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &speed);
    }

    bool isPlaying() const override {
        return m_mpv ? mpv_get_flag(m_mpv, "pause", false) == false : false;
    }

    double currentTime() const override {
        return mpv_get_double(m_mpv, "time-pos", 0.0);
    }

    double duration() const override {
        return mpv_get_double(m_mpv, "duration", 0.0);
    }

    double speed() const override {
        return mpv_get_double(m_mpv, "speed", 1.0);
    }

private:
    mpv_handle* m_mpv = nullptr;
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInited = (comHr == S_OK || comHr == S_FALSE);
    const auto loadIconSize = [&](UINT targetSize) -> HICON {
        if (targetSize == 0)
            return nullptr;
        HICON fromResource = reinterpret_cast<HICON>(
            LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_SYNCPLAY), IMAGE_ICON,
                       static_cast<int>(targetSize), static_cast<int>(targetSize),
                       LR_DEFAULTCOLOR));
        if (fromResource)
            return fromResource;
        std::wstring iconPath = GetExecutableDir() + L"assets\\SyncPlay.ico";
        HICON fromIco = LoadIconFromFile(iconPath, targetSize);
        if (!fromIco) {
            iconPath = L"assets\\SyncPlay.ico";
            fromIco = LoadIconFromFile(iconPath, targetSize);
        }
        if (fromIco)
            return fromIco;
        std::vector<uint8_t> iconPixels;
        UINT iconW = 0;
        UINT iconH = 0;
        std::wstring iconPathPng = GetExecutableDir() + L"assets\\SyncPlay.png";
        if (!LoadPngBGRA(iconPathPng, iconPixels, iconW, iconH, targetSize, targetSize)) {
            iconPathPng = L"assets\\SyncPlay.png";
            LoadPngBGRA(iconPathPng, iconPixels, iconW, iconH, targetSize, targetSize);
        }
        if (iconPixels.empty())
            return nullptr;
        ApplyRoundedMask(iconPixels, iconW, iconH, static_cast<float>(targetSize) * 0.32f);
        return CreateIconFromBGRA(iconPixels.data(), iconW, iconH);
    };
    g_appIcon.iconBig = loadIconSize(std::max<UINT>(32, GetSystemMetrics(SM_CXICON)));
    g_appIcon.iconSmall = loadIconSize(std::max<UINT>(16, GetSystemMetrics(SM_CXSMICON)));

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC | CS_DBLCLKS, WndProc, 0L, 0L, GetModuleHandle(nullptr),
                      g_appIcon.iconBig, nullptr, nullptr, nullptr, _T("SyncPlay"), g_appIcon.iconSmall };
    RegisterClassEx(&wc);
    const int desiredW = 1280;
    const int desiredH = 720;
    const DWORD windowStyle = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    const DWORD windowExStyle = WS_EX_APPWINDOW;
    RECT rect = {0, 0, desiredW, desiredH};
    AdjustWindowRectEx(&rect, windowStyle, FALSE, windowExStyle);
    const int winW = rect.right - rect.left;
    const int winH = rect.bottom - rect.top;
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int winX = std::max(0, (screenW - winW) / 2);
    const int winY = std::max(0, (screenH - winH) / 2);
    g_hWnd = CreateWindowEx(windowExStyle, wc.lpszClassName, _T("SyncPlay (Win32 + ImGui)"),
                            windowStyle, winX, winY, winW, winH,
                            nullptr, nullptr, wc.hInstance, nullptr);
    ApplyCustomWindowChrome(g_hWnd);

    DragAcceptFiles(g_hWnd, TRUE);

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    {
        std::vector<uint8_t> iconPixels;
        UINT iconW = 0;
        UINT iconH = 0;
        std::wstring iconPath = GetExecutableDir() + L"assets\\SyncPlay.png";
        if (!LoadPngBGRA(iconPath, iconPixels, iconW, iconH)) {
            iconPath = L"assets\\SyncPlay.png";
            LoadPngBGRA(iconPath, iconPixels, iconW, iconH);
        }
        if (!iconPixels.empty()) {
            ApplyRoundedMask(iconPixels, iconW, iconH, std::min(iconW, iconH) * 0.32f);
            ID3D11ShaderResourceView* srv = nullptr;
            if (CreateTextureFromBGRA(iconPixels.data(), iconW, iconH, &srv)) {
                g_appIcon.srv = srv;
                g_appIcon.width = static_cast<int>(iconW);
                g_appIcon.height = static_cast<int>(iconH);
            }
            if (g_appIcon.iconSmall)
                SendMessage(g_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon.iconSmall));
            if (g_appIcon.iconBig)
                SendMessage(g_hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon.iconBig));
            if (g_appIcon.iconSmall)
                SetClassLongPtr(g_hWnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(g_appIcon.iconSmall));
            if (g_appIcon.iconBig)
                SetClassLongPtr(g_hWnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(g_appIcon.iconBig));
        }
    }

    ShowWindow(g_hWnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string imguiIniPath = (config_path().parent_path() / "imgui.ini").string();
    io.IniFilename = imguiIniPath.c_str();
    // Keep arrow keys dedicated to playback shortcuts instead of UI focus navigation.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Style sizes are DPI-scaled; factored into a lambda so they can be
    // re-applied when the window moves to a different-DPI monitor (WM_DPICHANGED).
    auto applyStyleSizes = [](float scale) {
        ImGuiStyle& st = ImGui::GetStyle();
        const float U = 8.0f;
        st.WindowRounding = 12.0f;
        st.ChildRounding = 10.0f;
        st.FrameRounding = 10.0f;
        st.GrabRounding = 8.0f;
        st.PopupRounding = 10.0f;
        st.FrameBorderSize = 0.0f;
        st.WindowBorderSize = 0.0f;
        st.FramePadding = ImVec2(1.25f * U, 0.75f * U);
        st.WindowPadding = ImVec2(2.0f * U, 2.0f * U);
        st.ItemSpacing = ImVec2(1.0f * U, 1.0f * U);
        st.ItemInnerSpacing = ImVec2(0.75f * U, 0.75f * U);
        st.IndentSpacing = 2.0f * U;
        st.ScrollbarSize = 1.25f * U;
        st.ScrollbarRounding = 10.0f;
        st.ScaleAllSizes(scale);
    };

    ImVec4* colors = style.Colors;
    const ImVec4 bg = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    const ImVec4 panel = ImVec4(0.11f, 0.11f, 0.11f, 0.88f);
    const ImVec4 accent = ImVec4(0.40f, 0.58f, 0.98f, 1.00f);
    const ImVec4 accentSoft = ImVec4(0.40f, 0.58f, 0.98f, 0.70f);
    const ImVec4 text = ImVec4(0.94f, 0.95f, 0.97f, 1.00f);
    const ImVec4 mutedColor = ImVec4(0.66f, 0.69f, 0.74f, 1.00f);

    // Glass control palette: translucent fills that read well over the frosted
    // panels (and over the solid bars). Dark fills for editable fields (so text
    // keeps contrast over a bright blur); light fills for buttons/headers.
    const ImVec4 glassBorder       = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    const ImVec4 glassSeparator    = ImVec4(1.0f, 1.0f, 1.0f, 0.09f);
    const ImVec4 glassField        = ImVec4(0.0f, 0.0f, 0.0f, 0.24f);
    const ImVec4 glassFieldHover   = ImVec4(0.0f, 0.0f, 0.0f, 0.32f);
    const ImVec4 glassFieldActive  = ImVec4(0.0f, 0.0f, 0.0f, 0.40f);
    const ImVec4 glassButton       = ImVec4(1.0f, 1.0f, 1.0f, 0.09f);
    const ImVec4 glassButtonHover  = ImVec4(1.0f, 1.0f, 1.0f, 0.16f);
    const ImVec4 glassHeader       = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    const ImVec4 glassHeaderHover  = ImVec4(1.0f, 1.0f, 1.0f, 0.13f);
    const ImVec4 glassHeaderActive = ImVec4(1.0f, 1.0f, 1.0f, 0.18f);

    colors[ImGuiCol_WindowBg] = panel;
    colors[ImGuiCol_ChildBg] = panel;
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.12f, 0.975f);
    colors[ImGuiCol_Border] = glassBorder;
    colors[ImGuiCol_FrameBg] = glassField;
    colors[ImGuiCol_FrameBgHovered] = glassFieldHover;
    colors[ImGuiCol_FrameBgActive] = glassFieldActive;
    colors[ImGuiCol_TitleBg] = panel;
    colors[ImGuiCol_TitleBgActive] = panel;
    colors[ImGuiCol_Button] = glassButton;
    colors[ImGuiCol_ButtonHovered] = glassButtonHover;
    colors[ImGuiCol_ButtonActive] = accentSoft;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accentSoft;
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_Header] = glassHeader;
    colors[ImGuiCol_HeaderHovered] = glassHeaderHover;
    colors[ImGuiCol_HeaderActive] = glassHeaderActive;
    colors[ImGuiCol_Text] = text;
    colors[ImGuiCol_TextDisabled] = mutedColor;
    colors[ImGuiCol_Separator] = glassSeparator;
    colors[ImGuiCol_SeparatorHovered] = accentSoft;
    colors[ImGuiCol_SeparatorActive] = accent;

    float dpiScale = static_cast<float>(GetDpiForWindow(g_hWnd)) / 96.0f;
    ImFont* font10 = nullptr;
    ImFont* font12 = nullptr;
    ImFont* font14 = nullptr;
    ImFont* font16 = nullptr;
    ImFont* font18 = nullptr;
    ImFont* font22 = nullptr;
    ImFont* fontChat = nullptr;
    ImFont* fontIcons = nullptr;
    ImFont* fontIconsSmall = nullptr;
    ImFont* fontIconsLarge = nullptr;
    ImFont* fontIconsTiny = nullptr;

    // Rebuild every font at the current dpiScale. Re-callable so the UI can
    // rescale live when the window crosses to a different-DPI monitor.
    auto rebuildFonts = [&]() {
    io.Fonts->Clear();
    font10 = font12 = font14 = font16 = font18 = font22 = fontChat = nullptr;
    fontIcons = fontIconsSmall = fontIconsLarge = fontIconsTiny = nullptr;

    static const ImWchar iconRanges[] = {
            0xE11E, 0xE11E, // two page
            0xE146, 0xE146, // dock right
            0xE713, 0xE713, // settings
            0xE716, 0xE716, // session
            0xE717, 0xE717, // call
            0xE71A, 0xE71A, // stop
            0xE720, 0xE720, // microphone
            0xE72A, 0xE72B, // forward/rewind
            0xE73F, 0xE740, // window/fullscreen
            0xE74F, 0xE74F, // mute
            0xE767, 0xE769, // volume/play/pause
            0xE77A, 0xE77A, // unpin
            0xE7C2, 0xE7C2, // resize
            0xE823, 0xE823, // clock
            0xE840, 0xE840, // pin
            0xE8BC, 0xE8BC, // playlist
            0xE892, 0xE893, // prev/next
            0xE8B2, 0xE8B2, // subs
            0xE8B9, 0xE8B9, // stack
            0xE8BB, 0xE8BB, // close
            0xE8BD, 0xE8BD, // chat
            0xE8D6, 0xE8D6, // audio
            0xE8E5, 0xE8E5, // open
            0xE921, 0xE923, // minimize/maximize/restore
            0
    };

    bool iconsMerged10 = false;
    bool iconsMerged12 = false;
    bool iconsMerged14 = false;
    bool iconsMerged16 = false;
    bool iconsMerged22 = false;
    auto merge_icons = [&](float size) {
        ImFontConfig iconCfg;
        iconCfg.MergeMode = true;
        iconCfg.PixelSnapH = true;
        iconCfg.OversampleH = 2;
        iconCfg.OversampleV = 2;
        iconCfg.GlyphMinAdvanceX = size;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segmdl2.ttf", size, &iconCfg, iconRanges);
    };

    font10 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 10.0f * dpiScale);
    if (font10) {
        merge_icons(10.0f * dpiScale);
        iconsMerged10 = true;
    }
    font12 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 12.0f * dpiScale);
    if (font12) {
        merge_icons(12.0f * dpiScale);
        iconsMerged12 = true;
    }
    font14 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 14.0f * dpiScale);
    if (font14) {
        merge_icons(14.0f * dpiScale);
        iconsMerged14 = true;
    }
    font16 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f * dpiScale);
    if (font16) {
        merge_icons(16.0f * dpiScale);
        iconsMerged16 = true;
    }
    font18 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f * dpiScale);
    font22 = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 22.0f * dpiScale);
    if (font22) {
        merge_icons(22.0f * dpiScale);
        iconsMerged22 = true;
    }

    fontIcons = font16;
    fontIconsSmall = font14;
    fontIconsLarge = font22;
    fontIconsTiny = font10;
    if (!font16) {
        io.Fonts->Clear();
        font10 = nullptr;
        font12 = nullptr;
        font14 = nullptr;
        font18 = nullptr;
        font22 = nullptr;
        font16 = io.Fonts->AddFontDefault();
        if (font16 && !iconsMerged16) {
            merge_icons(font16->FontSize);
            iconsMerged16 = true;
        }
    }
    if (!font12)
        font12 = font14;
    if (!font10)
        font10 = font12;
    if (font10 && !iconsMerged10 && iconsMerged16)
        iconsMerged10 = true;
    if (font12 && !iconsMerged12 && iconsMerged16)
        iconsMerged12 = true;
    // Use the 16pt font for small labels too.
    font14 = font16 ? font16 : font14;
    if (font14 && !iconsMerged14 && iconsMerged16)
        iconsMerged14 = true;
    if (!font18)
        font18 = font16 ? font16 : font14;
    if (!font22)
        font22 = font16;
    if (font22 && !iconsMerged22 && iconsMerged16)
        iconsMerged22 = true;
    if (!fontIcons)
        fontIcons = font16;
    if (!fontIconsSmall)
        fontIconsSmall = fontIcons;
    if (!fontIconsLarge)
        fontIconsLarge = fontIcons;
    if (!fontIconsTiny)
        fontIconsTiny = font12 ? font12 : fontIconsSmall;

    ImFontGlyphRangesBuilder chatBuilder;
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesGreek());
    chatBuilder.AddRanges(kArabicRanges);
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesThai());
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesKorean());
    chatBuilder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    for (const char* emoji : kEmojiList)
        chatBuilder.AddText(emoji);
    static ImVector<ImWchar> chatRanges;
    chatRanges.clear();
    chatBuilder.BuildRanges(&chatRanges);

    ImFontConfig chatCfg;
    chatCfg.OversampleH = 2;
    chatCfg.OversampleV = 2;
    fontChat = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",
                                            16.0f * dpiScale, &chatCfg, chatRanges.Data);
    if (fontChat) {
        ImFontGlyphRangesBuilder emojiBuilder;
        emojiBuilder.AddRanges(kEmojiRanges);
        for (const char* emoji : kEmojiList)
            emojiBuilder.AddText(emoji);
        static ImVector<ImWchar> emojiRanges;
        emojiRanges.clear();
        emojiBuilder.BuildRanges(&emojiRanges);
        ImFontConfig emojiCfg;
        emojiCfg.MergeMode = true;
        emojiCfg.OversampleH = 2;
        emojiCfg.OversampleV = 2;
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguiemj.ttf",
                                          16.0f * dpiScale, &emojiCfg, emojiRanges.Data)) {
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf",
                                         16.0f * dpiScale, &emojiCfg, emojiRanges.Data);
        }
    } else {
        fontChat = font16;
    }
    io.FontDefault = font16;
    };

    applyStyleSizes(dpiScale);
    rebuildFonts();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    mpv_handle* mpv = mpv_create();
    if (!mpv) {
        CleanupDeviceD3D();
        return 1;
    }
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "vid", "auto");
    mpv_set_option_string(mpv, "hwdec", "auto-safe");
    mpv_set_option_string(mpv, "keep-open", "yes");
    mpv_set_option_string(mpv, "cache", "yes");
    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        CleanupDeviceD3D();
        return 1;
    }

    SwRenderState renderState;
    mpv_render_param render_params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&renderState.ctx, mpv, render_params) < 0) {
        mpv_terminate_destroy(mpv);
        CleanupDeviceD3D();
        return 1;
    }
    mpv_render_context_set_update_callback(renderState.ctx, mpv_render_update, &renderState);
    renderState.running.store(true, std::memory_order_relaxed);
    std::thread renderThread(render_thread, &renderState);
    g_renderState = &renderState;
    // Kick an initial frame so the video texture gets data even before mpv schedules updates.
    renderState.frameRequested.store(true, std::memory_order_relaxed);
    renderState.cv.notify_one();

    AppState app;
    float savedVolume = 100.0f;
    float savedSpeed = 1.0f;
    load_config(app, &savedVolume, &savedSpeed);
    {
        // Restore the persisted volume/speed onto mpv so they survive restarts;
        // the main loop reads these back from mpv into its local state.
        double initialVolume = std::clamp(static_cast<double>(savedVolume), 0.0, 100.0);
        mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &initialVolume);
        double initialSpeed = savedSpeed > 0.0f ? static_cast<double>(savedSpeed) : 1.0;
        mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &initialSpeed);
    }
    SyncPlayLog::SetEnabled(app.fileLoggingEnabled);
    {
        const std::wstring exePathW = GetExecutablePath();
        const std::string exePathUtf8 = Utf8FromWide(exePathW);
        const bool needsRegister = !app.openWithRegistered ||
                                   std::string(app.openWithExePath) != exePathUtf8;
        if (needsRegister) {
            std::wstring iconPath = GetExecutableDir() + L"assets\\SyncPlay.ico";
            if (GetFileAttributesW(iconPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                iconPath = exePathW;
            if (RegisterOpenWith(exePathW, iconPath)) {
                app.openWithRegistered = true;
                std::snprintf(app.openWithExePath, sizeof(app.openWithExePath), "%s",
                              exePathUtf8.c_str());
                app.dirty = true;
            }
        }
    }
    auto applyAccent = [&](const float* rgb) {
        ImVec4 accentColor(rgb[0], rgb[1], rgb[2], 1.0f);
        ImVec4 accentSoft(rgb[0], rgb[1], rgb[2], 0.70f);
        ImVec4 tabBase(rgb[0], rgb[1], rgb[2], 0.28f);
        ImVec4 tabHover(rgb[0], rgb[1], rgb[2], 0.50f);
        ImVec4 tabActive(rgb[0], rgb[1], rgb[2], 0.75f);
        ImVec4 tabUnfocused(rgb[0], rgb[1], rgb[2], 0.20f);
        ImVec4 tabUnfocusedActive(rgb[0], rgb[1], rgb[2], 0.55f);
        ImVec4* colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_CheckMark] = accentColor;
        colors[ImGuiCol_SliderGrab] = accentSoft;
        colors[ImGuiCol_SliderGrabActive] = accentColor;
        colors[ImGuiCol_ButtonActive] = accentSoft;
        colors[ImGuiCol_SeparatorHovered] = accentSoft;
        colors[ImGuiCol_SeparatorActive] = accentColor;
        colors[ImGuiCol_Tab] = tabBase;
        colors[ImGuiCol_TabHovered] = tabHover;
        colors[ImGuiCol_TabActive] = tabActive;
        colors[ImGuiCol_TabUnfocused] = tabUnfocused;
        colors[ImGuiCol_TabUnfocusedActive] = tabUnfocusedActive;
    };
    applyAccent(app.accentColor);
    MpvPlaybackController playback(mpv);
    SyncSession session(&playback);
    session.setNickname(std::string(app.nickname));
    session.setSignalingPort(app.signalingPort);
    session.setSessionPassword(std::string(app.sessionPassword));
    session.setSelectedInterfaceAddress(std::string(app.preferredInterface));
    session.setAutoPromote(app.autoPromote);
    session.setAllowGuestControl(app.allowGuestControl);
    session.setVoiceVolume(app.voiceVolume);
    session.setVoiceInputThreshold(app.voiceInputThreshold);
    session.setVoiceCaptureDeviceIndex(app.voiceCaptureDeviceIndex);
    session.setVoiceEnabled(app.voiceEnabled);
    session.setVoiceMuted(app.voiceMuted);

    std::string timelineTransportState = session.transportState();
    std::string timelineFileStatus = session.fileVerificationMessage();
    std::string timelineVoiceState = session.voiceState();

    session.setChatCallback([&app](const std::string& text) {
        const std::string stamp = ChatTimestamp();
        if (text.rfind("FILE|", 0) == 0) {
            std::string sender = "Peer";
            std::string name;
            std::string size;
            std::vector<std::string> parts;
            size_t start = 0;
            while (start < text.size()) {
                size_t pos = text.find('|', start);
                if (pos == std::string::npos) {
                    parts.push_back(text.substr(start));
                    break;
                }
                parts.push_back(text.substr(start, pos - start));
                start = pos + 1;
            }
            if (parts.size() >= 5) {
                sender = parts[1];
                name = parts[2];
                size = parts[4];
                ChatLine line;
                line.who = sender;
                line.text = "Received file";
                line.time = stamp;
                // Do not trust the peer-supplied path (parts[3]): it would be
                // handed to ShellExecute by the "Folder" button. The real local
                // download path is delivered separately by the share-progress
                // callback, which only ever writes inside the Downloads folder.
                line.kind = ChatLineKind::File;
                line.status = ChatLineStatus::Received;
                line.fileName = name.empty() ? "shared-file" : name;
                try {
                    line.fileSize = std::stoll(size);
                } catch (...) {
                    line.fileSize = 0;
                }
                app.chat.push_back(std::move(line));
            } else {
                app.chat.push_back({"Peer", text, stamp});
            }
        } else {
            app.chat.push_back({"Peer", text, stamp});
        }
        TrimChatHistory(app);
        app.dirty = true;
    });
    session.setShareProgressCallback([&app](const std::string& id,
                                            const std::string& name,
                                            const std::string& sender,
                                            const std::string& path,
                                            int64_t size,
                                            int64_t transferred,
                                            bool outgoing,
                                            bool done,
                                            bool failed) {
        if (id.empty())
            return;
        auto it = std::find_if(app.chat.begin(), app.chat.end(), [&](const ChatLine& line) {
            return line.transferId == id;
        });
        if (it == app.chat.end()) {
            ChatLine line;
            line.who = outgoing ? (app.nickname[0] == '\0' ? "You" : app.nickname)
                                : (sender.empty() ? "Peer" : sender);
            line.text = outgoing ? "Shared file" : "Receiving file";
            line.time = ChatTimestamp();
            line.kind = ChatLineKind::File;
            line.status = outgoing ? ChatLineStatus::Sending : ChatLineStatus::Receiving;
            line.fileName = name.empty() ? "shared-file" : name;
            line.filePath = path;
            line.retryPath = outgoing ? path : std::string();
            line.fileSize = size;
            line.fileTransferred = transferred;
            line.transferId = id;
            app.chat.push_back(std::move(line));
            TrimChatHistory(app);
            it = app.chat.empty() ? app.chat.end() : std::prev(app.chat.end());
        }
        if (it == app.chat.end())
            return;
        it->fileName = name.empty() ? it->fileName : name;
        if (!path.empty()) {
            it->filePath = path;
            if (outgoing)
                it->retryPath = path;
        }
        it->fileSize = size;
        it->fileTransferred = std::clamp<int64_t>(transferred, 0, size > 0 ? size : transferred);
        it->status = failed ? ChatLineStatus::Failed
                            : (done ? (outgoing ? ChatLineStatus::Sent : ChatLineStatus::Received)
                                    : (outgoing ? ChatLineStatus::Sending : ChatLineStatus::Receiving));
        it->text = failed ? "File share failed"
                          : (outgoing ? "Shared file"
                                      : (done ? "Received file" : "Receiving file"));
        app.dirty = true;
    });

    session.setActionCallback([&app](const std::string& message) {
        if (IsPeerTimelineAction(message)) {
            AppendSystemTimelineChip(app, message);
            return;
        }
        app.events.push_back({message, 1.5f});
    });
    session.setStatusCallback([&app, &session,
                               &timelineTransportState,
                               &timelineFileStatus,
                               &timelineVoiceState]() {
        app.sessionStatus = session.statusText();
        app.sessionHint = session.hintText();
        app.fileStatus = session.fileVerificationMessage();
        app.fileVerified = session.fileVerified();

        const std::string nextTransportState = session.transportState();
        AppendSystemTimelineChip(app, TransportTimelineLabel(timelineTransportState, nextTransportState));
        timelineTransportState = nextTransportState;

        if (app.fileStatus != timelineFileStatus)
            AppendSystemTimelineChip(app, FileTimelineLabel(app.fileStatus));
        timelineFileStatus = app.fileStatus;

        const std::string nextVoiceState = session.voiceState();
        AppendSystemTimelineChip(app, VoiceTimelineLabel(timelineVoiceState, nextVoiceState));
        timelineVoiceState = nextVoiceState;
    });
    app.sessionStatus = session.statusText();
    app.sessionHint = session.hintText();
    app.fileStatus = session.fileVerificationMessage();
    app.fileVerified = session.fileVerified();

    struct LocalFileInfo {
        std::string path;
        int64_t size = 0;
        std::string hash;
        bool valid = false;
        double lastDuration = 0.0;
        bool sent = false;
    } localFile;

    bool running = true;
    bool paused = false;
    bool scrubbing = false;
    float scrubPos = 0.0f;
    double lastScrubSyncValue = -1.0;
    auto lastScrubSyncSend = std::chrono::steady_clock::now();
    bool scrubSentDuringDrag = false;
    uint64_t frameCounter = 0;
    bool scrubPreviewSent = false;
    const uint64_t scrubPreviewStride = 3;
    uint64_t mpvAsyncId = 1;
    float lastVolume = 100.0f;
    bool muted = false;
    float volume = 100.0f;
    float speed = 1.0f;
    std::vector<float> speedOptions = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
    int64_t playlistCount = 0;
    int64_t playlistPos = -1;
    int playlistSelection = -1;
    bool fsTransition = false;
    int fsPhase = 0;
    float fsFade = 0.0f;

    auto resetScrubState = [&]() {
        scrubbing = false;
        scrubPos = 0.0f;
        lastScrubSyncValue = -1.0;
        lastScrubSyncSend = std::chrono::steady_clock::now();
        scrubSentDuringDrag = false;
        scrubPreviewSent = false;
    };

    auto setLocalFileWide = [&](const std::wstring& path) {
        localFile = {};
        const std::filesystem::path fsPath(path);
        if (!std::filesystem::exists(fsPath) || !std::filesystem::is_regular_file(fsPath))
            return;
        localFile.path = Utf8FromWide(path);
        localFile.size = static_cast<int64_t>(std::filesystem::file_size(fsPath));
        localFile.hash = computePartialHashHexUtf8(localFile.path);
        localFile.valid = !localFile.hash.empty();
    };
    auto setLocalFileUtf8 = [&](const std::string& path) {
        setLocalFileWide(WideFromUtf8(path));
    };

    auto updateLocalFileFromMpv = [&]() {
        char* path = mpv_get_property_string(mpv, "path");
        if (!path || !path[0]) {
            if (path)
                mpv_free(path);
            return;
        }
        std::string utf8 = path;
        mpv_free(path);
        if (utf8 != localFile.path)
            setLocalFileUtf8(utf8);
    };

    auto openVideo = [&]() {
        const std::wstring path = openFileDialog(g_hWnd,
                                                 L"Video Files\0*.mp4;*.mkv;*.avi;*.mov;*.webm\0All Files\0*.*\0",
                                                 L"Open Video");
        if (!path.empty()) {
            const std::string utf8 = Utf8FromWide(path);
            const char* cmd[] = { "loadfile", utf8.c_str(), nullptr };
            mpv_command(mpv, cmd);
            setLocalFileWide(path);
            resetScrubState();
            app.events.push_back({"Loaded file", 2.0f});
        }
    };

    auto openSubtitles = [&]() {
        const std::wstring path = openFileDialog(g_hWnd,
                                                 L"Subtitle Files\0*.srt;*.ass;*.vtt\0All Files\0*.*\0",
                                                 L"Open Subtitles");
        if (path.empty())
            return;
        const std::string utf8 = Utf8FromWide(path);
        const char* cmd[] = { "sub-add", utf8.c_str(), "select", nullptr };
        mpv_command(mpv, cmd);
        app.events.push_back({"Loaded subtitles", 2.0f});
    };

    auto openFolder = [&](const std::string& dirPath) {
        if (dirPath.empty())
            return;
        std::filesystem::path fsPath(dirPath);
        if (!std::filesystem::exists(fsPath))
            return;
        if (std::filesystem::is_regular_file(fsPath)) {
            const std::wstring args = L"/select,\"" + fsPath.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        const std::wstring wide = fsPath.wstring();
        ShellExecuteW(nullptr, L"open", L"explorer.exe", wide.c_str(), nullptr, SW_SHOWNORMAL);
    };

    auto browseShareFile = [&]() {
        const std::wstring path = openFileDialog(g_hWnd, L"All Files\0*.*\0", L"Share File");
        if (!path.empty()) {
            const std::string utf8 = Utf8FromWide(path);
            std::snprintf(app.filePath, sizeof(app.filePath), "%s", utf8.c_str());
        }
    };

    auto pushOsd = [&](const std::string& text, float ttl = 1.2f) {
        app.events.push_back({text, ttl});
    };

    auto requestOnly = [&]() {
        return session.sessionActive() && !session.isHost() &&
               session.transportConnected();
    };

    auto togglePlay = [&]() {
        if (requestOnly()) {
            if (paused) {
                session.requestPlay();
                pushOsd("Play requested");
            } else {
                session.requestPause();
                pushOsd("Pause requested");
            }
            return;
        }
        paused = !paused;
        mpv_set_flag(mpv, "pause", paused);
        session.notifyLocalAction();
        pushOsd(paused ? "Pause" : "Play");
    };

    auto applyVolume = [&](float newVolume, bool showToast = true) {
        volume = std::clamp(newVolume, 0.0f, 100.0f);
        if (volume > 0.0f)
            muted = false;
        double v = volume;
        mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &v);
        if (showToast) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Volume %.0f%%", volume);
            pushOsd(buf);
        }
    };

    auto toggleMute = [&]() {
        if (!muted && volume > 0.0f) {
            lastVolume = volume;
            volume = 0.0f;
            muted = true;
        } else {
            volume = (lastVolume > 0.0f) ? lastVolume : 100.0f;
            muted = false;
        }
        double v = volume;
        mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &v);
        if (muted) {
            pushOsd("Muted");
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Volume %.0f%%", volume);
            pushOsd(buf);
        }
    };

    auto applySpeed = [&](double next, bool showToast = true) {
        if (requestOnly()) {
            session.requestSpeed(next);
            if (showToast) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "Requested %.2fx", next);
                pushOsd(buf);
            }
            return;
        }
        mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &next);
        speed = static_cast<float>(next);
        session.notifyLocalAction();
        if (showToast) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2fx", next);
            pushOsd(buf);
        }
    };

    auto colorHex = [](const float rgb[3]) {
        auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
        const int r = static_cast<int>(clamp01(rgb[0]) * 255.0f + 0.5f);
        const int g = static_cast<int>(clamp01(rgb[1]) * 255.0f + 0.5f);
        const int b = static_cast<int>(clamp01(rgb[2]) * 255.0f + 0.5f);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
        return std::string(buf);
    };

    auto applyVideoColor = [&](bool markDirty = true) {
        double v = app.videoBrightness;
        mpv_set_property(mpv, "brightness", MPV_FORMAT_DOUBLE, &v);
        v = app.videoContrast;
        mpv_set_property(mpv, "contrast", MPV_FORMAT_DOUBLE, &v);
        v = app.videoSaturation;
        mpv_set_property(mpv, "saturation", MPV_FORMAT_DOUBLE, &v);
        v = app.videoGamma;
        mpv_set_property(mpv, "gamma", MPV_FORMAT_DOUBLE, &v);
        v = app.videoHue;
        mpv_set_property(mpv, "hue", MPV_FORMAT_DOUBLE, &v);
        if (markDirty)
            app.dirty = true;
    };

    auto applyToneMapping = [&](bool markDirty = true) {
        static const char* modes[] = {
            "auto", "clip", "linear", "gamma", "reinhard", "hable", "mobius", "bt.2390"
        };
        const int modeCount = static_cast<int>(sizeof(modes) / sizeof(modes[0]));
        app.videoToneMapping = std::clamp(app.videoToneMapping, 0, modeCount - 1);
        mpv_set_property_string(mpv, "tone-mapping", modes[app.videoToneMapping]);
        double param = app.videoToneMappingParam;
        mpv_set_property(mpv, "tone-mapping-param", MPV_FORMAT_DOUBLE, &param);
        double peak = app.videoTargetPeak;
        mpv_set_property(mpv, "target-peak", MPV_FORMAT_DOUBLE, &peak);
        if (markDirty)
            app.dirty = true;
    };

    auto applyVideoShaders = [&](bool markDirty = true) {
        const char* clearCmd[] = { "change-list", "glsl-shaders", "clear", nullptr };
        mpv_command(mpv, clearCmd);
        for (const auto& shader : app.videoShaders) {
            if (shader.empty())
                continue;
            const char* cmd[] = { "change-list", "glsl-shaders", "append", shader.c_str(), nullptr };
            mpv_command(mpv, cmd);
        }
        if (markDirty)
            app.dirty = true;
    };

    auto applySubtitleStyle = [&](bool markDirty = true) {
        const char* font = app.subtitleFont[0] ? app.subtitleFont : "sans-serif";
        mpv_set_property_string(mpv, "sub-font", font);
        double size = app.subtitleFontSize;
        mpv_set_property(mpv, "sub-font-size", MPV_FORMAT_DOUBLE, &size);
        int bold = app.subtitleBold ? 1 : 0;
        mpv_set_property(mpv, "sub-bold", MPV_FORMAT_FLAG, &bold);
        int italic = app.subtitleItalic ? 1 : 0;
        mpv_set_property(mpv, "sub-italic", MPV_FORMAT_FLAG, &italic);
        double spacing = app.subtitleSpacing;
        mpv_set_property(mpv, "sub-spacing", MPV_FORMAT_DOUBLE, &spacing);
        double border = app.subtitleBorderSize;
        mpv_set_property(mpv, "sub-border-size", MPV_FORMAT_DOUBLE, &border);
        double shadow = app.subtitleShadowOffset;
        mpv_set_property(mpv, "sub-shadow-offset", MPV_FORMAT_DOUBLE, &shadow);
        double pos = app.subtitlePos;
        mpv_set_property(mpv, "sub-pos", MPV_FORMAT_DOUBLE, &pos);
        double marginX = app.subtitleMarginX;
        mpv_set_property(mpv, "sub-margin-x", MPV_FORMAT_DOUBLE, &marginX);
        double marginY = app.subtitleMarginY;
        mpv_set_property(mpv, "sub-margin-y", MPV_FORMAT_DOUBLE, &marginY);
        int64_t alignX = static_cast<int64_t>(app.subtitleAlignX);
        mpv_set_property(mpv, "sub-align-x", MPV_FORMAT_INT64, &alignX);
        int64_t alignY = static_cast<int64_t>(app.subtitleAlignY);
        mpv_set_property(mpv, "sub-align-y", MPV_FORMAT_INT64, &alignY);
        std::string color = colorHex(app.subtitleColor);
        mpv_set_property_string(mpv, "sub-color", color.c_str());
        double opacity = std::clamp(app.subtitleOpacity, 0.0f, 1.0f);
        mpv_set_property(mpv, "sub-opacity", MPV_FORMAT_DOUBLE, &opacity);
        double delay = app.subtitleDelay;
        mpv_set_property(mpv, "sub-delay", MPV_FORMAT_DOUBLE, &delay);
        mpv_set_flag(mpv, "sub-visibility", app.subtitlesEnabled);
        if (markDirty)
            app.dirty = true;
    };

    applyVideoColor(false);
    applyToneMapping(false);
    applyVideoShaders(false);
    applySubtitleStyle(false);

    auto seekBy = [&](double delta) {
        if (requestOnly()) {
            session.requestSeekDelta(delta);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s%.0fs", (delta >= 0.0) ? "+" : "-", std::abs(delta));
            pushOsd(buf);
            return;
        }
        const double pos = mpv_get_double(mpv, "time-pos", 0.0);
        double seek = pos + delta;
        mpv_set_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &seek);
        session.notifyLocalAction();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.0fs", (delta >= 0.0) ? "+" : "-", std::abs(delta));
        pushOsd(buf);
    };

    int argcW = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    if (argvW && argcW > 1 && argvW[1] && argvW[1][0] != L'\0') {
        const std::wstring pathW = argvW[1];
        const std::string pathUtf8 = Utf8FromWide(pathW);
        if (!pathUtf8.empty()) {
            const char* cmd[] = { "loadfile", pathUtf8.c_str(), nullptr };
            mpv_command(mpv, cmd);
            setLocalFileWide(pathW);
            resetScrubState();
        }
    }
    if (argvW)
        LocalFree(argvW);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (running) {
        frameCounter++;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (g_requestExit.load(std::memory_order_relaxed))
            running = false;
        if (!running)
            break;

        session.tick();

        if (g_pendingToggleFullscreen && !fsTransition) {
            fsTransition = true;
            fsPhase = 0;
            fsFade = 0.0f;
        }
        g_pendingToggleFullscreen = false;
        if (g_pendingTogglePlay) {
            togglePlay();
            g_pendingTogglePlay = false;
        }

        if (g_pendingDpiChange) {
            g_pendingDpiChange = false;
            dpiScale = static_cast<float>(g_pendingDpiValue) / 96.0f;
            ImGui_ImplDX11_InvalidateDeviceObjects();
            rebuildFonts();
            applyStyleSizes(dpiScale);
        }

        if (g_pendingDrop) {
            const std::string utf8 = Utf8FromWide(g_dropPath);
            const char* cmd[] = { "loadfile", utf8.c_str(), nullptr };
            mpv_command(mpv, cmd);
            setLocalFileWide(g_dropPath);
            resetScrubState();
            app.events.push_back({"Loaded file", 2.0f});
            g_pendingDrop = false;
        }

        for (;;) {
            mpv_event* ev = mpv_wait_event(mpv, 0);
            if (!ev || ev->event_id == MPV_EVENT_NONE)
                break;
            if (ev->event_id == MPV_EVENT_FILE_LOADED) {
                resetScrubState();
                updateLocalFileFromMpv();
                applySubtitleStyle(false);
            }
        }

        paused = mpv_get_flag(mpv, "pause", paused);
        const double duration = mpv_get_double(mpv, "duration", 0.0);
        double position = mpv_get_double(mpv, "time-pos", 0.0);
        volume = static_cast<float>(mpv_get_double(mpv, "volume", volume));
        speed = static_cast<float>(mpv_get_double(mpv, "speed", speed));
        playlistCount = mpv_get_int64(mpv, "playlist-count", playlistCount);
        playlistPos = mpv_get_int64(mpv, "playlist-pos", playlistPos);
        const bool hasMedia = playlistCount > 0 || duration > 0.01;
        if (!hasMedia && app.showSubs)
            app.showSubs = false;
        if (!scrubbing) {
            scrubPos = static_cast<float>(position);
        }

        if (localFile.valid) {
            const double sendDuration = duration;
            const bool durationChanged =
                localFile.lastDuration <= 0.0 || std::abs(sendDuration - localFile.lastDuration) > 0.5;
            if (!localFile.sent || durationChanged) {
                localFile.lastDuration = sendDuration;
                localFile.sent = true;
                session.setLocalFileInfo(localFile.size, sendDuration, localFile.hash);
            }
        }

        RECT rc{};
        GetClientRect(g_hWnd, &rc);
        int ui_w = rc.right - rc.left;
        int ui_h = rc.bottom - rc.top;
        const bool uiMinimized = IsIconic(g_hWnd) != 0;
        if (uiMinimized && app.lastUiW > 0 && app.lastUiH > 0) {
            ui_w = app.lastUiW;
            ui_h = app.lastUiH;
        }
        const float edgePad = 8.0f;
        const float dockPad = 12.0f;
        const float dockMinW = std::max(160.0f, std::max(260.0f, static_cast<float>(ui_w) * 0.26f) - 100.0f);
        const float dockMaxW = std::max(dockMinW, static_cast<float>(ui_w) - dockPad - edgePad);
        if (app.dockPanelW <= 0.0f)
            app.dockPanelW = std::min(dockMaxW,
                                      std::max(dockMinW, static_cast<float>(ui_w) * 0.36f - 100.0f));
        const float dockPanelW = std::clamp(app.dockPanelW, dockMinW, dockMaxW);

        const ImGuiStyle& style = ImGui::GetStyle();
        const float lineH = ImGui::GetFrameHeight();
        const float tuneScale = dpiScale / 1.25f;
        const auto tune = [&](float v) { return v * tuneScale; };
        const float baseBarHeight = std::max(90.0f,
                                             lineH * 2.0f + style.WindowPadding.y * 2.0f +
                                             style.ItemSpacing.y * 2.0f + 3.0f);
        const float barHeightUi = std::max(tune(50.0f), baseBarHeight - tune(43.0f));
        const int barHeightFb = g_fullscreen ? 0 : static_cast<int>(barHeightUi);
        const int marginY = g_fullscreen ? 0 : barHeightFb;
        const bool anyRightOpen = app.showSession || app.showChat || app.showCall || app.showSubs || app.showSettings;
        const bool chatSidebarMode = app.showChat && app.sidePanels;
        const bool chatDockedSidebar = chatSidebarMode;
        const bool sideLayout = chatSidebarMode;
        const float viewW = sideLayout
                                ? std::max(1.0f, static_cast<float>(ui_w) - dockPanelW)
                                : static_cast<float>(ui_w);

        const int videoW = std::max(1, static_cast<int>(viewW));
        const int videoH = std::max(1, ui_h - marginY);

        ensure_sw_buffer(renderState, videoW, videoH);
        EnsureVideoTexture(videoW, videoH);
        if (g_inSizing.load(std::memory_order_relaxed)) {
            renderState.frameRequested.store(true, std::memory_order_relaxed);
            renderState.cv.notify_one();
        }
        // If the front buffer still has the old size after a resize, skip upload this frame.
        {
            std::lock_guard<std::mutex> lock(renderState.mutex);
            const int front = renderState.frontIndex;
            if (renderState.hasFrame.load(std::memory_order_relaxed) &&
                (renderState.widths[front] != g_videoTexW || renderState.heights[front] != g_videoTexH)) {
                renderState.frameRequested.store(true, std::memory_order_relaxed);
                renderState.cv.notify_one();
            }
        }
        // If we don't have a frame yet, keep requesting until we do.
        if (scrubbing || !renderState.hasFrame.load(std::memory_order_relaxed)) {
            renderState.frameRequested.store(true, std::memory_order_relaxed);
            renderState.cv.notify_one();
        }
        // Frosted-glass toggle: when off, skip the per-frame blur downsample (CPU)
        // and drop the frosted backdrop immediately, even while paused.
        g_glassEnabled = app.glassPanels;
        if (!g_glassEnabled)
            g_blurReady = false;
        static uint64_t lastUploaded = 0;
        const uint64_t frameId = renderState.frameCounter.load(std::memory_order_relaxed);
        if (frameId != lastUploaded) {
            update_video_texture(renderState);
            lastUploaded = frameId;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiIO& io = ImGui::GetIO();
        if (fsTransition) {
            const float speed = 6.0f;
            if (fsPhase == 0) {
                fsFade = std::min(1.0f, fsFade + io.DeltaTime * speed);
                if (fsFade >= 1.0f) {
                    ToggleFullscreen(g_hWnd);
                    fsPhase = 1;
                }
            } else {
                fsFade = std::max(0.0f, fsFade - io.DeltaTime * speed);
                if (fsFade <= 0.0f)
                    fsTransition = false;
            }
        }

        static bool chatInputActive = false;
        chatInputActive = false;

        if (g_videoSrv) {
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            bg->AddImage((ImTextureID)g_videoSrv, ImVec2(0, 0),
                         ImVec2(static_cast<float>(videoW), static_cast<float>(videoH)));
        }
        if (!hasMedia) {
            // Designed idle screen: a soft vertical gradient lifts the flat black,
            // with the app icon and a hint centered.
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            const float W = static_cast<float>(ui_w);
            const float H = static_cast<float>(ui_h);
            const ImU32 top = ImGui::GetColorU32(ImVec4(0.11f, 0.12f, 0.15f, 1.0f));
            const ImU32 bottom = ImGui::GetColorU32(ImVec4(0.04f, 0.04f, 0.06f, 1.0f));
            bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), top, top, bottom, bottom);
            const ImVec2 center(W * 0.5f, H * 0.5f - tune(24.0f));
            if (g_appIcon.srv && g_appIcon.width > 0 && g_appIcon.height > 0) {
                const float iconSz = tune(132.0f);
                bg->AddImage((ImTextureID)g_appIcon.srv,
                             ImVec2(center.x - iconSz * 0.5f, center.y - iconSz * 0.5f),
                             ImVec2(center.x + iconSz * 0.5f, center.y + iconSz * 0.5f),
                             ImVec2(0, 0), ImVec2(1, 1),
                             ImGui::GetColorU32(ImVec4(1, 1, 1, 0.92f)));
            }
            const char* hint = "Open a file  -  or drag it here";
            const ImVec2 hintSize = ImGui::CalcTextSize(hint);
            bg->AddText(ImVec2(center.x - hintSize.x * 0.5f, center.y + tune(86.0f)),
                        ImGui::GetColorU32(ImVec4(0.66f, 0.69f, 0.74f, 0.92f)), hint);
        }
        if (fsFade > 0.0f) {
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            const ImU32 fadeCol = ImGui::GetColorU32(ImVec4(0, 0, 0, fsFade));
            bg->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(ui_w), static_cast<float>(ui_h)), fadeCol);
        }

        const ImVec2 basePad = ImGui::GetStyle().WindowPadding;
        const ImVec2 popupPad(std::max(tune(8.0f), basePad.x * 0.6f),
                              std::max(tune(6.0f), basePad.y * 0.6f));
        const ImVec2 popupSpacing(std::max(tune(6.0f), basePad.x * 0.45f),
                                  std::max(tune(4.0f), basePad.y * 0.45f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(ui_w), static_cast<float>(ui_h)), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("Surface", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float uiFade = 1.0f - fsFade;
        static const std::string ICON_OPEN = Utf8FromCodepoint(0xE8E5);
        static const std::string ICON_SUBS = Utf8FromCodepoint(0xE8B2);
        static const std::string ICON_FULLSCREEN = Utf8FromCodepoint(0xE740);
        static const std::string ICON_WINDOW = Utf8FromCodepoint(0xE73F);
        static const std::string ICON_CHAT = Utf8FromCodepoint(0xE8BD);
        static const std::string ICON_CALL = Utf8FromCodepoint(0xE717);
        static const std::string ICON_MIC = Utf8FromCodepoint(0xE720);
        static const std::string ICON_SESSION = Utf8FromCodepoint(0xE716);
        static const std::string ICON_SETTINGS = Utf8FromCodepoint(0xE713);
        static const std::string ICON_OVERLAY = Utf8FromCodepoint(0xE8B9);
        static const std::string ICON_SIDEBAR = Utf8FromCodepoint(0xE146);
        static const std::string ICON_PIN = Utf8FromCodepoint(0xE840);
        static const std::string ICON_UNPIN = Utf8FromCodepoint(0xE77A);
        static const std::string ICON_SPEED = Utf8FromCodepoint(0xE823);
        static const std::string ICON_ASPECT = Utf8FromCodepoint(0xE7C2);
        static const std::string ICON_PLAYLIST = Utf8FromCodepoint(0xE8BC);
        static const std::string ICON_MINIMIZE = Utf8FromCodepoint(0xE921);
        static const std::string ICON_MAXIMIZE = Utf8FromCodepoint(0xE922);
        static const std::string ICON_RESTORE = Utf8FromCodepoint(0xE923);
        static const std::string ICON_CLOSE = Utf8FromCodepoint(0xE8BB);
        static const std::string ICON_PREV = Utf8FromCodepoint(0xE892);
        static const std::string ICON_NEXT = Utf8FromCodepoint(0xE893);
        static const std::string ICON_REW = Utf8FromCodepoint(0xE72B);
        static const std::string ICON_FWD = Utf8FromCodepoint(0xE72A);
        static const std::string ICON_PLAY = Utf8FromCodepoint(0xE768);
        static const std::string ICON_PAUSE = Utf8FromCodepoint(0xE769);
        static const std::string ICON_STOP = Utf8FromCodepoint(0xE71A);
        static const std::string ICON_VOL = Utf8FromCodepoint(0xE767);
        static const std::string ICON_MUTE = Utf8FromCodepoint(0xE74F);
        static const std::string ICON_AUDIO = Utf8FromCodepoint(0xE8D6);
        const ImGuiHoveredFlags hoverFlags = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
        bool uiHovered = false;
        bool panelRectHovered = false;
        ImVec2 nextPanelHeaderMin(0.0f, 0.0f);
        ImVec2 nextPanelHeaderMax(0.0f, 0.0f);
        bool nextPanelHeaderValid = false;
        const bool useCustomTitleBar = !g_fullscreen;
        const float titleBarH = useCustomTitleBar
                                    ? std::max(22.0f, ImGui::GetFrameHeight() - 6.0f) + 6.0f
                                    : 0.0f;

        const float iconFontSize = (fontIcons ? fontIcons->FontSize : ImGui::GetFontSize());
        auto round_px = [](float v) { return std::floor(v + 0.5f); };
        const bool topBarFullscreenStyle = true;
        const ImVec2 topPad = topBarFullscreenStyle ? basePad
                                                    : ImVec2(round_px(basePad.x * 0.55f),
                                                             round_px(basePad.y * 0.40f));
        const ImVec2 topItemSpacing = topBarFullscreenStyle ? ImGui::GetStyle().ItemSpacing
                                                            : ImVec2(round_px(ImGui::GetStyle().ItemSpacing.x * 0.55f),
                                                                     round_px(ImGui::GetStyle().ItemSpacing.y * 0.55f));
        const float iconBtnSize = std::max(ImGui::GetFrameHeight(),
                                           iconFontSize + basePad.y * 2.0f);
        const float topIconBtnSize = topBarFullscreenStyle
                                         ? iconBtnSize
                                         : round_px(std::max(ImGui::GetFrameHeight(),
                                                            iconFontSize + topPad.y * 2.0f));
        if (useCustomTitleBar) {
            const char* appName = "SyncPlay";
            const float leftPad = 7.0f;
            const float rightPad = 12.0f;
            const float titleGap = 10.0f;
            const float titleBtn = std::max(10.0f, titleBarH - 14.0f);
            const float btnY = (titleBarH - titleBtn) * 0.5f;
            const float btnGap = 8.0f;
            const float buttonsW = titleBtn * 3.0f + btnGap * 2.0f;
            ImFont* titleFont = font14 ? font14 : ImGui::GetFont();
            ImFont* appFont = font12 ? font12 : (font14 ? font14 : ImGui::GetFont());
            const ImVec2 appSize = appFont->CalcTextSizeA(appFont->FontSize, FLT_MAX, 0.0f, appName);
            const bool showAppIcon = (g_appIcon.srv != nullptr);
            const float appIconSize = showAppIcon ? std::min(titleBarH - 8.0f, iconFontSize) : 0.0f;
            const float appIconGap = showAppIcon ? 6.0f : 0.0f;
            const float buttonsX = static_cast<float>(ui_w) - rightPad - buttonsW;
            const float appX = leftPad + appIconSize + appIconGap;
            const float appRight = appX + appSize.x;
            const float dragRight = std::max(0.0f, buttonsX - 8.0f);
            g_titleBarHitTest.enabled = true;
            g_titleBarHitTest.dragRect = {0, 0, static_cast<LONG>(dragRight), static_cast<LONG>(titleBarH)};
            const float minX = buttonsX;
            const float maxX = minX + titleBtn + btnGap;
            const float closeX = maxX + titleBtn + btnGap;
            g_titleBarHitTest.minRect = {static_cast<LONG>(minX), static_cast<LONG>(btnY),
                                         static_cast<LONG>(minX + titleBtn), static_cast<LONG>(btnY + titleBtn)};
            g_titleBarHitTest.maxRect = {static_cast<LONG>(maxX), static_cast<LONG>(btnY),
                                         static_cast<LONG>(maxX + titleBtn), static_cast<LONG>(btnY + titleBtn)};
            g_titleBarHitTest.closeRect = {static_cast<LONG>(closeX), static_cast<LONG>(btnY),
                                           static_cast<LONG>(closeX + titleBtn), static_cast<LONG>(btnY + titleBtn)};

            ImVec4 titleBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            titleBg.w = std::min(1.0f, titleBg.w + 0.08f);
            ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, titleBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
            ImGui::BeginChild("TitleBar", ImVec2(static_cast<float>(ui_w), titleBarH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoSavedSettings);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 barMin = ImGui::GetWindowPos();
            const ImVec2 barMax = ImVec2(barMin.x + static_cast<float>(ui_w), barMin.y + titleBarH);
            const ImU32 borderCol = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Border]);
            dl->AddLine(ImVec2(barMin.x, barMax.y - 1.0f), ImVec2(barMax.x, barMax.y - 1.0f), borderCol);

            if (showAppIcon) {
                ImGui::SetCursorPos(ImVec2(leftPad, (titleBarH - appIconSize) * 0.5f));
                ImGui::Image(reinterpret_cast<ImTextureID>(g_appIcon.srv), ImVec2(appIconSize, appIconSize));
            }

            std::string mediaTitle = "No media";
            char* title = mpv_get_property_string(mpv, "media-title");
            if (title && title[0] != '\0')
                mediaTitle = title;
            if (title)
                mpv_free(title);
            const ImVec2 titleSize = titleFont->CalcTextSizeA(titleFont->FontSize, FLT_MAX, 0.0f,
                                                             mediaTitle.c_str());
            const float leftLimit = appRight + titleGap;
            const float rightLimit = std::max(leftLimit + 40.0f, buttonsX - 8.0f);
            const float titleX = (static_cast<float>(ui_w) - titleSize.x) * 0.5f;

            ImGui::PushClipRect(ImVec2(barMin.x + leftLimit, barMin.y),
                                ImVec2(barMin.x + rightLimit, barMin.y + titleBarH), true);
            const bool pushedTitleFont = (titleFont != ImGui::GetFont());
            if (pushedTitleFont)
                ImGui::PushFont(titleFont);
            ImGui::SetCursorPos(ImVec2(titleX, (titleBarH - titleSize.y) * 0.5f));
            ImGui::TextUnformatted(mediaTitle.c_str());
            if (pushedTitleFont)
                ImGui::PopFont();
            ImGui::PopClipRect();

            ImGui::SetCursorPos(ImVec2(appX, (titleBarH - appSize.y) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            const bool pushedAppFont = (appFont != ImGui::GetFont());
            if (pushedAppFont)
                ImGui::PushFont(appFont);
            ImGui::TextUnformatted(appName);
            if (pushedAppFont)
                ImGui::PopFont();
            ImGui::PopStyleColor();

            ImGui::SetCursorPos(ImVec2(minX, btnY));
            const bool isMax = IsZoomed(g_hWnd);
            const auto titleButton = [&](const char* id, const char* icon, const char* tooltip, float yOffset) {
                ImGui::PushID(id);
                ImGui::InvisibleButton("btn", ImVec2(titleBtn, titleBtn));
                const bool pressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                ImFont* iconFont = fontIconsTiny ? fontIconsTiny : ImGui::GetFont();
                const ImVec2 iconSize = iconFont->CalcTextSizeA(iconFont->FontSize, FLT_MAX, 0.0f, icon);
                const ImVec2 min = ImGui::GetItemRectMin();
                ImVec2 pos(min.x + (titleBtn - iconSize.x) * 0.5f,
                           min.y + (titleBtn - iconSize.y) * 0.5f + yOffset);
                pos.x = std::floor(pos.x);
                pos.y = std::floor(pos.y);
                const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                ImGui::GetWindowDrawList()->AddText(iconFont, iconFont->FontSize, pos, col, icon);
                if (tooltip)
                    ShowDelayedTooltip(tooltip);
                ImGui::PopID();
                return pressed;
            };
            if (titleButton("TitleMin", ICON_MINIMIZE.c_str(), "Minimize", 2.0f))
                ShowWindow(g_hWnd, SW_MINIMIZE);
            ImGui::SameLine(0.0f, btnGap);
            if (titleButton("TitleMax", isMax ? ICON_RESTORE.c_str() : ICON_MAXIMIZE.c_str(),
                            isMax ? "Restore" : "Maximize", 0.0f))
                ShowWindow(g_hWnd, isMax ? SW_RESTORE : SW_MAXIMIZE);
            ImGui::SameLine(0.0f, btnGap);
            if (titleButton("TitleClose", ICON_CLOSE.c_str(), "Close", 0.0f))
                PostMessage(g_hWnd, WM_CLOSE, 0, 0);

            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        } else {
            g_titleBarHitTest.enabled = false;
        }

        static bool lastPanelRectHovered = false;
        static ImVec2 lastPanelHeaderMin(0.0f, 0.0f);
        static ImVec2 lastPanelHeaderMax(0.0f, 0.0f);
        static bool lastPanelHeaderValid = false;
        float hoverMaxX = viewW;
        if (sideLayout) {
            const float dockLeftX = std::max(edgePad, static_cast<float>(ui_w) - dockPanelW);
            hoverMaxX = std::min(hoverMaxX, dockLeftX);
        }
        const bool hoverTopRegion = io.MousePos.y <= 90.0f && io.MousePos.x <= hoverMaxX;
        const bool hoverBottomRegion = io.MousePos.y >= static_cast<float>(ui_h) - barHeightUi - 8.0f &&
                                       io.MousePos.x <= hoverMaxX;
        const bool panelHeaderBlock = lastPanelHeaderValid &&
                                      io.MousePos.x >= lastPanelHeaderMin.x &&
                                      io.MousePos.x <= lastPanelHeaderMax.x &&
                                      io.MousePos.y >= lastPanelHeaderMin.y &&
                                      io.MousePos.y <= lastPanelHeaderMax.y;
        const bool panelHoverBlock = lastPanelRectHovered && !hoverTopRegion && !hoverBottomRegion;
        const bool hoverTop = !panelHoverBlock && !panelHeaderBlock && hoverTopRegion;
        const bool hoverBottom = !panelHoverBlock && hoverBottomRegion;
        static double lastHoverTop = 0.0;
        static double lastHoverBottom = 0.0;
        const double hoverNow = ImGui::GetTime();
        if (hoverTop)
            lastHoverTop = hoverNow;
        if (hoverBottom)
            lastHoverBottom = hoverNow;
        const bool keepTop = (hoverNow - lastHoverTop) <= 1.0;
        const bool keepBottom = (hoverNow - lastHoverBottom) <= 1.0;
        const bool anyHover = hoverTop || hoverBottom || keepTop || keepBottom;
        const bool anyPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool showTopBar = topBarFullscreenStyle ? (keepTop || hoverTop || keepBottom || hoverBottom)
                                                      : (!g_fullscreen || keepTop || hoverTop);
        const bool showControls = !g_fullscreen || anyHover || scrubbing || anyPopupOpen;
        if (anyPopupOpen)
            uiHovered = true;

        const float dt = io.DeltaTime;
        const auto smoothStep = [&](float current, float target, float inTime, float outTime) {
            const float tau = (target > current) ? inTime : outTime;
            const float ease = 1.0f - std::exp(-dt / std::max(0.001f, tau));
            return current + (target - current) * ease;
        };
        static float topAlpha = 0.0f;
        static float bottomAlpha = 0.0f;
        static float dockAlpha = 0.0f;
        const float topTarget = showTopBar ? 1.0f : 0.0f;
        const float bottomTarget = showControls ? 1.0f : 0.0f;
        const bool showDock = anyRightOpen;
        const float dockTarget = showDock ? 1.0f : 0.0f;
        topAlpha = smoothStep(topAlpha, topTarget, 0.06f, 0.025f);
        bottomAlpha = smoothStep(bottomAlpha, bottomTarget, 0.06f, 0.025f);
        dockAlpha = smoothStep(dockAlpha, dockTarget, 0.05f, 0.02f);

        const bool renderTopBar = topAlpha > 0.01f;
        const bool renderBottomBar = bottomAlpha > 0.01f;
        const bool renderDock = dockAlpha > 0.01f;

        enum PanelId { PanelNone = 0, PanelSession = 1, PanelChat = 2, PanelCall = 3, PanelSubs = 4, PanelSettings = 5 };
        static int lastPanel = PanelNone;
        if (app.showSession)
            lastPanel = PanelSession;
        if (app.showChat)
            lastPanel = PanelChat;
        if (app.showCall)
            lastPanel = PanelCall;
        if (app.showSubs)
            lastPanel = PanelSubs;
        if (app.showSettings)
            lastPanel = PanelSettings;
        if (!anyRightOpen && !renderDock)
            lastPanel = PanelNone;

        const bool sessionActive = app.showSession || (!anyRightOpen && renderDock && lastPanel == PanelSession);
        const bool chatActive = app.showChat || (!anyRightOpen && renderDock && lastPanel == PanelChat);
        const bool callActive = app.showCall || (!anyRightOpen && renderDock && lastPanel == PanelCall);
        const bool subsActive = app.showSubs || (!anyRightOpen && renderDock && lastPanel == PanelSubs);
        const bool settingsActive = app.showSettings || (!anyRightOpen && renderDock && lastPanel == PanelSettings);
        static bool chatSeenInitialized = false;
        static size_t chatSeenCount = 0;
        if (!chatSeenInitialized) {
            chatSeenCount = app.chat.size();
            chatSeenInitialized = true;
        }
        const size_t chatUnreadCount = app.chat.size() > chatSeenCount
                                           ? app.chat.size() - chatSeenCount
                                           : 0;

        if (renderTopBar) {
            const ImGuiStyle& topStyle = ImGui::GetStyle();
            const char* icons[] = {ICON_OPEN.c_str(), ICON_SUBS.c_str(),
                                   g_fullscreen ? ICON_WINDOW.c_str() : ICON_FULLSCREEN.c_str(),
                                   ICON_CALL.c_str(), ICON_CHAT.c_str(),
                                   ICON_SESSION.c_str(), ICON_SETTINGS.c_str()};
            const int iconCount = static_cast<int>(sizeof(icons) / sizeof(icons[0]));
            float topBarWidth = topPad.x * 2.0f +
                                topIconBtnSize * static_cast<float>(iconCount) +
                                topItemSpacing.x * static_cast<float>(std::max(0, iconCount - 1)) + 2.0f;
            const float maxTopBar = std::max(220.0f, viewW - 24.0f);
            if (topBarWidth > maxTopBar)
                topBarWidth = maxTopBar;
            const float rightEdge = viewW - 12.0f;
            float topBarX = std::max(12.0f, rightEdge - topBarWidth);
            if (topBarX + topBarWidth > rightEdge)
                topBarX = std::max(12.0f, rightEdge - topBarWidth);
            const float topBarH = topIconBtnSize + topPad.y * 2.0f;
            const float topBarY = 12.0f + titleBarH;

            const float topFade = (0.15f + 0.85f * topAlpha) * uiFade;
            ImVec4 topBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            const float topBgAlpha = topBarFullscreenStyle ? 0.45f : 0.90f;
            topBg.w = topBgAlpha * topFade;
            ImGui::SetCursorPos(ImVec2(topBarX, topBarY));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, topBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, topPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, topItemSpacing);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, topFade);
            ImGui::BeginChild("TopBar", ImVec2(topBarWidth, topBarH), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const bool topGlowShift = false;
            if (IconButton("Open", ICON_OPEN.c_str(), "Open (Ctrl+O)", fontIcons,
                           ImVec2(topIconBtnSize, topIconBtnSize)))
                openVideo();
            ImGui::SameLine();
            if (!hasMedia)
                ImGui::BeginDisabled();
            if (IconToggle("Subs", ICON_SUBS.c_str(), "Subtitles", app.showSubs, fontIcons,
                           ImVec2(topIconBtnSize, topIconBtnSize))) {
                app.showSubs = !app.showSubs;
                if (app.showSubs) {
                    app.showChat = false;
                    app.showCall = false;
                    app.showSession = false;
                    app.showSettings = false;
                }
            }
            if (!hasMedia)
                ImGui::EndDisabled();
            ImGui::SameLine();
            if (IconButton("Fullscreen", g_fullscreen ? ICON_WINDOW.c_str() : ICON_FULLSCREEN.c_str(),
                            g_fullscreen ? "Window (F11)" : "Fullscreen (F11)", fontIcons,
                            ImVec2(topIconBtnSize, topIconBtnSize)))
                g_pendingToggleFullscreen = true;
            ImGui::SameLine();
            const char* callTooltip = session.voiceActive() ? "Voice call active" : "Voice call";
            if (IconToggle("Call", ICON_CALL.c_str(), callTooltip,
                           app.showCall || session.voiceActive(), fontIcons,
                           ImVec2(topIconBtnSize, topIconBtnSize))) {
                app.showCall = !app.showCall;
                if (app.showCall) {
                    app.showChat = false;
                    app.showSubs = false;
                    app.showSession = false;
                    app.showSettings = false;
                }
            }
            ImGui::SameLine();
            if (IconToggle("Chat", ICON_CHAT.c_str(), "Chat", app.showChat, fontIcons,
                           ImVec2(topIconBtnSize, topIconBtnSize))) {
                app.showChat = !app.showChat;
                if (app.showChat) {
                    app.showCall = false;
                    app.showSubs = false;
                    app.showSession = false;
                    app.showSettings = false;
                }
            }
            if (chatUnreadCount > 0) {
                const ImVec2 itemMin = ImGui::GetItemRectMin();
                const ImVec2 itemMax = ImGui::GetItemRectMax();
                const float badgeR = std::max(5.0f, topIconBtnSize * 0.12f);
                const ImVec2 badgeCenter(itemMax.x - badgeR - 3.0f, itemMin.y + badgeR + 3.0f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 badgeCol = ImGui::GetColorU32(ImVec4(0.95f, 0.25f, 0.18f, topFade));
                dl->AddCircleFilled(badgeCenter, badgeR, badgeCol, 16);
                if (chatUnreadCount > 1) {
                    char badgeText[8]{};
                    if (chatUnreadCount > 9)
                        std::snprintf(badgeText, sizeof(badgeText), "9+");
                    else
                        std::snprintf(badgeText, sizeof(badgeText), "%zu", chatUnreadCount);
                    const ImVec2 textSize = ImGui::CalcTextSize(badgeText);
                    dl->AddText(ImVec2(badgeCenter.x - textSize.x * 0.5f,
                                       badgeCenter.y - textSize.y * 0.5f),
                                ImGui::GetColorU32(ImVec4(1, 1, 1, topFade)),
                                badgeText);
                }
            }
            ImGui::SameLine();
            if (IconToggle("Session", ICON_SESSION.c_str(), "Session", app.showSession, fontIcons,
                           ImVec2(topIconBtnSize, topIconBtnSize))) {
                app.showSession = !app.showSession;
                if (app.showSession) {
                    app.showSubs = false;
                    app.showChat = false;
                    app.showCall = false;
                    app.showSettings = false;
                }
            }
            ImGui::SameLine();
            if (IconToggle("Settings", ICON_SETTINGS.c_str(), "Settings", app.showSettings, fontIcons,
                           ImVec2(topIconBtnSize, topIconBtnSize))) {
                app.showSettings = !app.showSettings;
                if (app.showSettings) {
                    app.showSubs = false;
                    app.showChat = false;
                    app.showCall = false;
                    app.showSession = false;
                }
            }
            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        if (renderBottomBar) {
            const float bottomFade = (0.15f + 0.85f * bottomAlpha) * uiFade;
            ImVec4 ctrlBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            ctrlBg.w = (g_fullscreen ? 0.60f : 1.0f) * bottomFade;
            ImVec4 ctrlGrad = ctrlBg;
            ctrlGrad.x = std::min(1.0f, ctrlGrad.x + 0.04f);
            ctrlGrad.y = std::min(1.0f, ctrlGrad.y + 0.04f);
            ctrlGrad.z = std::min(1.0f, ctrlGrad.z + 0.05f);
            const float controlsY = g_fullscreen
                                        ? (static_cast<float>(ui_h) - barHeightUi)
                                        : std::floor(static_cast<float>(ui_h) - barHeightUi);
            // Windowed: extend the bar a hair past the client bottom (the Surface clips
            // the overshoot) so integer pixel rounding never leaves a dark clear-colour
            // strip between the bar and the window's bottom edge.
            const float controlsH = g_fullscreen
                                        ? barHeightUi
                                        : (static_cast<float>(ui_h) - controlsY + 1.0f);
            const float compactMinW = tune(640.0f);
            const float compactTargetW = std::min(viewW, std::max(compactMinW, viewW * 0.60f));
            const float controlsW = (g_fullscreen ? compactTargetW : viewW);
            const float controlsX = (g_fullscreen ? (viewW - controlsW) * 0.5f : 0.0f);
            if (!g_fullscreen) {
                // Soft scrim above the (full-width) control bar so it fades out of
                // the video instead of meeting it with a hard edge. Skipped in
                // fullscreen, where the bar is a narrow centered panel and the
                // scrim's side edges would show as a floating shadow.
                ImDrawList* bgd = ImGui::GetBackgroundDrawList();
                const float scrimTop = controlsY - tune(56.0f);
                const ImU32 clear = ImGui::GetColorU32(ImVec4(0, 0, 0, 0.0f));
                const ImU32 dark = ImGui::GetColorU32(ImVec4(0, 0, 0, 0.45f * bottomFade));
                bgd->AddRectFilledMultiColor(ImVec2(controlsX, scrimTop),
                                             ImVec2(controlsX + controlsW, controlsY),
                                             clear, clear, dark, dark);
            }
            ImGui::SetCursorPos(ImVec2(controlsX, controlsY));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ctrlBg);
            const float controlsPadX = std::max(tune(2.0f), basePad.x - tune(8.0f)) + tune(5.0f);
            const ImVec2 controlsPad(controlsPadX, 0.0f);
            const ImVec2 controlsSpacing(std::max(tune(2.0f), style.ItemSpacing.x - tune(4.0f)),
                                          std::max(tune(1.0f), style.ItemSpacing.y - tune(6.0f)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, controlsPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, controlsSpacing);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, bottomFade);
            // Windowed bar is flush to the window's rounded corners — square its own
            // corners so they meet the window rounding with no gap. Border is OFF: with
            // the bar flush to the window edges it showed as a hard white line on the
            // left/right/bottom. AlwaysUseWindowPadding keeps the content insets that a
            // borderless child would otherwise drop (so the timeline doesn't underlap).
            const float savedControlsRounding = ImGui::GetStyle().ChildRounding;
            if (!g_fullscreen)
                ImGui::GetStyle().ChildRounding = 0.0f;
            ImGui::BeginChild("Controls", ImVec2(controlsW, controlsH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_AlwaysUseWindowPadding);
            ImGui::GetStyle().ChildRounding = savedControlsRounding;

        const float bottomRowOffset = 3.0f;
        const float transportAdjust = std::clamp((1.2f - dpiScale) / 0.2f, 0.0f, 1.0f) * 5.0f;
        const float maxPos = duration > 0.0 ? static_cast<float>(duration) : 1.0f;
        const std::string timeLeft = format_time(position);
        const std::string timeRight = format_time(duration);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float timelineH = tune(16.0f);
        const float textH = ImGui::GetTextLineHeight();
        const float rowY = ImGui::GetCursorPosY() + bottomRowOffset;

        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), rowY));
        if (!hasMedia)
            ImGui::BeginDisabled();
        ImGui::InvisibleButton("##timeline", ImVec2(avail, timelineH));
        const bool sliderActive = hasMedia && ImGui::IsItemActive();
        const bool sliderDeactivated =
            hasMedia && (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsItemDeactivated());
        const bool sliderHovered = hasMedia && ImGui::IsItemHovered();
        ImVec2 tlMin = ImGui::GetItemRectMin();
        ImVec2 tlMax = ImGui::GetItemRectMax();
        if (!hasMedia)
            ImGui::EndDisabled();
        const float tlWidth = std::max(1.0f, tlMax.x - tlMin.x);
        const float centerY = (tlMin.y + tlMax.y) * 0.5f;
        const float trackThickness = std::max(3.0f, tune(5.0f));
        const float progress = std::clamp(scrubPos / maxPos, 0.0f, 1.0f);
        if (sliderActive) {
            if (io.KeyShift) {
                const float fineScale = 0.25f;
                const float delta = (io.MouseDelta.x / tlWidth) * maxPos * fineScale;
                scrubPos = std::clamp(scrubPos + delta, 0.0f, maxPos);
            } else {
                float t = (ImGui::GetIO().MousePos.x - tlMin.x) / tlWidth;
                t = std::clamp(t, 0.0f, 1.0f);
                scrubPos = t * maxPos;
            }
        }
            if (sliderHovered) {
                float t = (ImGui::GetIO().MousePos.x - tlMin.x) / tlWidth;
                t = std::clamp(t, 0.0f, 1.0f);
                const std::string hoverTime = format_time(t * maxPos);
                ShowDelayedTooltip(hoverTime.c_str());
            }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 trackCol = ImGui::GetColorU32(hasMedia
                                                      ? ImVec4(0.30f, 0.34f, 0.40f, 0.65f)
                                                      : ImVec4(0.24f, 0.27f, 0.32f, 0.34f));
        ImVec4 mediaFill = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
        if (!hasMedia)
            mediaFill = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        mediaFill.w *= hasMedia ? 1.0f : 0.55f;
        const ImU32 fillCol = ImGui::GetColorU32(mediaFill);
        const ImU32 knobCol = ImGui::GetColorU32(mediaFill);
        const float trackHalf = trackThickness * 0.5f;
        const float fillX = tlMin.x + tlWidth * progress;
        const bool showSyncDot = session.sessionActive() && session.transportConnected();
        if (sliderHovered) {
            const ImU32 glowCol = ImGui::GetColorU32(ImVec4(0.30f, 0.70f, 0.95f, 0.18f));
            dl->AddRectFilled(ImVec2(tlMin.x - tune(1.0f), centerY - (trackHalf + tune(2.0f))),
                              ImVec2(tlMax.x + tune(1.0f), centerY + (trackHalf + tune(2.0f))),
                              glowCol, trackHalf + tune(2.0f));
        }
        dl->AddRectFilled(ImVec2(tlMin.x, centerY - trackHalf),
                          ImVec2(tlMax.x, centerY + trackHalf),
                          trackCol, trackHalf);
        dl->AddRectFilled(ImVec2(tlMin.x, centerY - trackHalf),
                          ImVec2(fillX, centerY + trackHalf),
                          fillCol, trackHalf);
        const float knobR = tune(8.0f);
        dl->AddCircleFilled(ImVec2(fillX, centerY), knobR, knobCol, 20);
        if (showSyncDot) {
            dl->AddCircleFilled(ImVec2(fillX, centerY), std::max(2.5f, knobR * 0.50f),
                                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.75f)), 12);
        }

        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), rowY + timelineH + tune(2.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, mutedColor);
        if (font18)
            ImGui::PushFont(font18);
        ImGui::TextUnformatted(timeLeft.c_str());
        if (font18)
            ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const float timeRightW = ImGui::CalcTextSize(timeRight.c_str()).x;
        const float rightX = ImGui::GetWindowContentRegionMax().x - timeRightW;
        if (rightX > ImGui::GetCursorPosX())
            ImGui::SetCursorPosX(rightX);
        ImGui::PushStyleColor(ImGuiCol_Text, mutedColor);
        if (font18)
            ImGui::PushFont(font18);
        ImGui::TextUnformatted(timeRight.c_str());
        if (font18)
            ImGui::PopFont();
        ImGui::PopStyleColor();
        if (session.sessionActive()) {
            const std::string syncText = session.syncConfidenceText();
            const ImVec2 syncSize = ImGui::CalcTextSize(syncText.c_str());
            const float syncPadX = tune(8.0f);
            const float syncPadY = tune(2.0f);
            const float chipW = syncSize.x + syncPadX * 2.0f;
            const float chipH = syncSize.y + syncPadY * 2.0f;
            const float chipX = tlMin.x + (tlWidth - chipW) * 0.5f;
            const float chipY = tlMax.y + tune(1.0f);
            ImVec4 chipBg = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
            chipBg.w = 0.55f * bottomFade;
            ImVec4 syncCol = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
            if (syncText == "Synced" || syncText == "Sync host")
                syncCol = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
            else if (syncText.rfind("Resyncing", 0) == 0)
                syncCol = ImVec4(0.97f, 0.58f, 0.35f, bottomFade);
            syncCol.w *= bottomFade;
            dl->AddRectFilled(ImVec2(chipX, chipY), ImVec2(chipX + chipW, chipY + chipH),
                              ImGui::ColorConvertFloat4ToU32(chipBg), chipH * 0.5f);
            dl->AddText(ImVec2(chipX + syncPadX, chipY + syncPadY),
                        ImGui::ColorConvertFloat4ToU32(syncCol), syncText.c_str());
        }

        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), rowY + timelineH + textH));
        if (sliderActive) {
            if (!scrubbing)
                scrubPreviewSent = false;
            scrubbing = true;
            if (!requestOnly()) {
                const auto now = std::chrono::steady_clock::now();
                if (!scrubPreviewSent || (frameCounter % scrubPreviewStride) == 0) {
                    const std::string target = std::to_string(scrubPos);
                    const char* cmd[] = {"seek", target.c_str(), "absolute", nullptr};
                    mpv_command_async(mpv, mpvAsyncId++, cmd);
                    scrubPreviewSent = true;
                }
                if (std::abs(scrubPos - static_cast<float>(lastScrubSyncValue)) > 0.02f &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScrubSyncSend).count() >= 30) {
                    session.notifyLocalAction();
                    lastScrubSyncValue = scrubPos;
                    lastScrubSyncSend = now;
                    scrubSentDuringDrag = true;
                }
            }
        }
        if (sliderDeactivated) {
            scrubbing = false;
            const float delta = std::abs(scrubPos - static_cast<float>(lastScrubSyncValue));
            if (!requestOnly()) {
                if (!scrubSentDuringDrag || delta > 0.02f) {
                    const std::string target = std::to_string(scrubPos);
                    const char* cmd[] = {"seek", target.c_str(), "absolute+exact", nullptr};
                    mpv_command_async(mpv, mpvAsyncId++, cmd);
                    session.notifyLocalAction();
                    app.events.push_back({"Seeked", 1.5f});
                }
            } else {
                session.requestSeek(static_cast<double>(scrubPos));
                app.events.push_back({"Seek requested", 1.5f});
            }
            lastScrubSyncValue = scrubPos;
            lastScrubSyncSend = std::chrono::steady_clock::now();
            scrubSentDuringDrag = false;
            scrubPreviewSent = false;
        }
        if (!sliderActive && !sliderHovered)
            scrubbing = false;

        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        if (ImGui::BeginTable("ControlsRow", 3,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Center", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + bottomRowOffset);
            int speedIndex = 2;
            for (size_t i = 0; i < speedOptions.size(); ++i) {
                if (std::abs(speedOptions[i] - speed) < 0.001f) {
                    speedIndex = static_cast<int>(i);
                    break;
                }
            }
            const char* labels[] = {"0.5x","0.75x","1.0x","1.25x","1.5x","2.0x"};
            const float speedBtn = std::max(tune(18.0f), ImGui::GetFrameHeight());
            ImGui::AlignTextToFramePadding();
            if (!hasMedia)
                ImGui::BeginDisabled();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            if (IconButtonFont("SpeedPopupBtn", ICON_SPEED.c_str(), "Playback speed",
                               fontIconsSmall, ImVec2(speedBtn, speedBtn)))
                ImGui::OpenPopup("SpeedPopup");
            ImGui::PopStyleVar();
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(labels[speedIndex]);
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            if (IconButtonFont("AspectRatioBtn", ICON_ASPECT.c_str(), "Aspect ratio",
                               fontIconsSmall, ImVec2(speedBtn, speedBtn)))
                ImGui::OpenPopup("AspectPopup");
            ImGui::PopStyleVar();
            if (!hasMedia)
                ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            if (IconButtonFont("PlaylistBtn", ICON_PLAYLIST.c_str(),
                               "Playlist", fontIconsSmall, ImVec2(speedBtn, speedBtn)))
                ImGui::OpenPopup("PlaylistPopup");
            ImGui::PopStyleVar();
            const float popupRound = tune(4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, popupSpacing);
            if (ImGui::BeginPopup("SpeedPopup")) {
                for (size_t i = 0; i < speedOptions.size(); ++i) {
                    const bool selected = static_cast<int>(i) == speedIndex;
                    if (ImGui::MenuItem(labels[i], nullptr, selected)) {
                        double next = speedOptions[i];
                        applySpeed(next);
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(4);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, popupSpacing);
            if (ImGui::BeginPopup("AspectPopup")) {
                const double aspectOverride = mpv_get_double(mpv, "video-aspect-override", -1.0);
                auto aspectItem = [&](const char* label, double value) {
                    const bool selected = (value < 0.0)
                                              ? (aspectOverride < 0.0)
                                              : (std::abs(aspectOverride - value) < 0.01);
                    if (ImGui::MenuItem(label, nullptr, selected)) {
                        double next = value;
                        mpv_set_property(mpv, "video-aspect-override", MPV_FORMAT_DOUBLE, &next);
                    }
                };
                aspectItem("Auto", -1.0);
                aspectItem("16:9", 16.0 / 9.0);
                aspectItem("4:3", 4.0 / 3.0);
                aspectItem("21:9", 21.0 / 9.0);
                aspectItem("1:1", 1.0);
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(4);
            ImGui::SetNextWindowSize(ImVec2(tune(320.0f), tune(220.0f)), ImGuiCond_Appearing);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, popupSpacing);
            if (ImGui::BeginPopup("PlaylistPopup")) {
                if (ImGui::IsWindowAppearing())
                    playlistSelection = static_cast<int>(playlistPos);
                if (ImGui::Button("Add")) {
                    const std::wstring path = openFileDialog(
                        g_hWnd,
                        L"Video Files\0*.mp4;*.mkv;*.avi;*.mov;*.webm\0All Files\0*.*\0",
                        L"Add to Playlist");
                    if (!path.empty()) {
                        const std::string utf8 = Utf8FromWide(path);
                        if (playlistCount <= 0) {
                            const char* cmd[] = { "loadfile", utf8.c_str(), nullptr };
                            mpv_command(mpv, cmd);
                            setLocalFileWide(path);
                            resetScrubState();
                        } else {
                            const char* cmd[] = { "loadfile", utf8.c_str(), "append", nullptr };
                            mpv_command(mpv, cmd);
                        }
                        app.events.push_back({"Added to playlist", 1.5f});
                    }
                }
                ImGui::SameLine();
                const bool canRemove = playlistSelection >= 0 &&
                                       playlistSelection < static_cast<int>(playlistCount);
                if (!canRemove)
                    ImGui::BeginDisabled();
                if (ImGui::Button("Remove")) {
                    const std::string idx = std::to_string(playlistSelection);
                    const char* cmd[] = { "playlist-remove", idx.c_str(), nullptr };
                    mpv_command(mpv, cmd);
                    app.events.push_back({"Removed from playlist", 1.5f});
                    if (playlistSelection >= static_cast<int>(playlistCount) - 1)
                        playlistSelection = static_cast<int>(playlistCount) - 2;
                    if (playlistSelection < 0)
                        playlistSelection = -1;
                }
                if (!canRemove)
                    ImGui::EndDisabled();
                ImGui::SameLine();
                const bool hasItems = playlistCount > 0;
                if (!hasItems)
                    ImGui::BeginDisabled();
                if (ImGui::Button("Clear")) {
                    const char* cmd[] = { "playlist-clear", nullptr };
                    mpv_command(mpv, cmd);
                    playlistSelection = -1;
                    app.events.push_back({"Playlist cleared", 1.5f});
                }
                if (!hasItems)
                    ImGui::EndDisabled();

                ImGui::Separator();
                ImGui::BeginChild("PlaylistList", ImVec2(0.0f, tune(150.0f)), true);
                const auto playlistItems = mpv_read_playlist(mpv);
                if (playlistItems.empty()) {
                    ImGui::TextDisabled("Playlist is empty");
                } else {
                    for (const auto& item : playlistItems) {
                        std::string label = item.title;
                        if (label.empty()) {
                            const std::filesystem::path path(item.filename);
                            label = path.filename().string();
                        }
                        if (label.empty())
                            label = "Item " + std::to_string(item.index + 1);
                        if (item.current || item.index == playlistPos)
                            label = "> " + label;
                        const bool selected = item.index == playlistSelection;
                        if (ImGui::Selectable(label.c_str(), selected))
                            playlistSelection = item.index;
                        if (ImGui::IsItemHovered() &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            const std::string idx = std::to_string(item.index);
                            const char* cmd[] = { "playlist-play-index", idx.c_str(), nullptr };
                            mpv_command(mpv, cmd);
                            app.events.push_back({"Playing selected", 1.2f});
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(4);

            ImGui::TableSetColumnIndex(1);
            ImGuiStyle& rowStyle = ImGui::GetStyle();
            const ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
            const ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
            const float centerX = (contentMin.x + contentMax.x) * 0.5f;
            const float spacingX = rowStyle.ItemSpacing.x;
            const float buttonW = iconBtnSize;
            const float playW = iconBtnSize;
            const float leftGroupW = buttonW * 3.0f + spacingX * 2.0f;
            const float rightGroupW = buttonW * 2.0f + spacingX;
            float leftStartX = centerX - playW * 0.5f - spacingX - leftGroupW;
            float rightStartX = centerX + playW * 0.5f + spacingX;
            const float minX = contentMin.x;
            const float maxX = contentMax.x;
            if (leftStartX < minX)
                leftStartX = minX;
            if (rightStartX + rightGroupW > maxX)
                rightStartX = maxX - rightGroupW;

            if (!hasMedia)
                ImGui::BeginDisabled();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 19.0f + transportAdjust);
            ImGui::SetCursorPosX(leftStartX);
            ImGui::BeginGroup();
            if (IconButton("Stop", ICON_STOP.c_str(), "Stop (S)", fontIcons, ImVec2(iconBtnSize, iconBtnSize))) {
                const char* cmd[] = { "stop", nullptr };
                mpv_command(mpv, cmd);
            }
            ImGui::SameLine();
            if (IconButton("Prev", ICON_PREV.c_str(), "Previous item", fontIcons, ImVec2(iconBtnSize, iconBtnSize))) {
                if (playlistCount > 0 && playlistPos > 0) {
                    const char* cmd[] = { "playlist-prev", nullptr };
                    mpv_command(mpv, cmd);
                    app.events.push_back({"Previous item", 1.5f});
                } else {
                    app.events.push_back({"No previous item", 1.2f});
                }
            }
            ImGui::SameLine();
            if (IconButton("Rewind", ICON_REW.c_str(), "Back 10s (Left)", fontIcons, ImVec2(iconBtnSize, iconBtnSize))) {
                seekBy(-10.0);
            }
            ImGui::EndGroup();

            ImGui::SameLine(0.0f, 0.0f);
            const float playRowY = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(playRowY + 1.0f);
            ImGui::SetCursorPosX(centerX - playW * 0.5f + tune(3.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            bool playPressed = IconButtonFont("PlayPause", paused ? ICON_PLAY.c_str() : ICON_PAUSE.c_str(),
                                              paused ? "Play (Space)" : "Pause (Space)",
                                              fontIconsLarge, ImVec2(iconBtnSize, iconBtnSize));
            ImGui::PopStyleVar();
            ImGui::SetCursorPosY(playRowY);
            if (playPressed) {
                togglePlay();
            }

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetCursorPosX(rightStartX);
            ImGui::BeginGroup();
            if (IconButton("Forward", ICON_FWD.c_str(), "Forward 10s (Right)", fontIcons,
                            ImVec2(iconBtnSize, iconBtnSize))) {
                seekBy(10.0);
            }
            ImGui::SameLine();
            if (IconButton("Next", ICON_NEXT.c_str(), "Next item", fontIcons, ImVec2(iconBtnSize, iconBtnSize))) {
                if (playlistCount > 0 && playlistPos + 1 < playlistCount) {
                    const char* cmd[] = { "playlist-next", nullptr };
                    mpv_command(mpv, cmd);
                    app.events.push_back({"Next item", 1.5f});
                } else {
                    app.events.push_back({"No next item", 1.2f});
                }
            }
            ImGui::EndGroup();
            if (!hasMedia)
                ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + bottomRowOffset);
            const float sliderW2 = tune(150.0f);
            const float volumeBtn = ImGui::GetFrameHeight();
            const float trackBtn = volumeBtn;
            const float voiceBtn = volumeBtn;
            const float muteW = volumeBtn;
            const float totalWRight = trackBtn * 2.0f + voiceBtn + muteW + sliderW2 +
                                      rowStyle.ItemSpacing.x * 4.0f;
            const float availRight = ImGui::GetContentRegionAvail().x;
            if (availRight > totalWRight) {
                const float cursorX = ImGui::GetCursorPosX();
                ImGui::SetCursorPosX(cursorX + (availRight - totalWRight));
            }
            const float volumeH = tune(18.0f);
            const float textH = ImGui::GetTextLineHeight();
            const float lineH = std::max(textH, volumeBtn);
            const float rowY = ImGui::GetCursorPosY();
            const float buttonTop = rowY + (lineH - volumeBtn) * 0.5f;
            const float sliderTop = buttonTop + (volumeBtn - volumeH) * 0.5f;
            ImGui::SetCursorPosY(buttonTop);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            if (!hasMedia)
                ImGui::BeginDisabled();
            if (IconButtonFont("SubTrackBtn", ICON_SUBS.c_str(), "Subtitle Track", fontIconsSmall,
                               ImVec2(trackBtn, trackBtn)))
                ImGui::OpenPopup("SubTrackPopup");
            ImGui::SameLine();
            if (IconButtonFont("AudioTrackBtn", ICON_AUDIO.c_str(), "Audio Track", fontIconsSmall,
                               ImVec2(trackBtn, trackBtn)))
                ImGui::OpenPopup("AudioTrackPopup");
            if (!hasMedia)
                ImGui::EndDisabled();
            ImGui::SameLine();
            const char* voiceMuteTooltip = !app.voiceEnabled
                                               ? "Enable voice in Call"
                                               : (app.voiceMuted ? "Unmute microphone" : "Mute microphone");
            if (!app.voiceEnabled)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            bool voiceMutePressed = IconButtonFont("VoiceMuteBottom", ICON_MIC.c_str(),
                                                   voiceMuteTooltip, fontIconsSmall,
                                                   ImVec2(voiceBtn, voiceBtn));
            if (!app.voiceEnabled)
                ImGui::PopStyleColor();
            if (app.voiceMuted)
                DrawMutedMicSlash(app.voiceEnabled);
            ImGui::SameLine();
            if (!hasMedia)
                ImGui::BeginDisabled();
            bool mutePressed = IconButtonFont("Mute",
                                              (muted || volume <= 0.0f) ? ICON_MUTE.c_str() : ICON_VOL.c_str(),
                                              "Mute", fontIconsSmall, ImVec2(volumeBtn, volumeBtn));
            if (!hasMedia)
                ImGui::EndDisabled();
            ImGui::PopStyleVar();
            if (voiceMutePressed) {
                if (app.voiceEnabled) {
                    app.voiceMuted = !app.voiceMuted;
                    session.setVoiceMuted(app.voiceMuted);
                } else {
                    app.showCall = true;
                    app.showSession = false;
                    app.showChat = false;
                    app.showSubs = false;
                    app.showSettings = false;
                    app.events.push_back({"Enable voice in Call first", 1.5f});
                }
            }
            if (mutePressed) {
                toggleMute();
            }
            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX(), sliderTop));
            if (!hasMedia)
                ImGui::BeginDisabled();
            ImGui::InvisibleButton("##VolumeSlider", ImVec2(sliderW2, volumeH));
            const bool volActive = hasMedia && ImGui::IsItemActive();
            const bool volDeactivated = hasMedia && ImGui::IsItemDeactivatedAfterEdit();
            const bool volHovered = hasMedia && ImGui::IsItemHovered();
            ImVec2 sliderMin = ImGui::GetItemRectMin();
            ImVec2 sliderMax = ImGui::GetItemRectMax();
            if (!hasMedia)
                ImGui::EndDisabled();
            const float volWidth = std::max(1.0f, sliderMax.x - sliderMin.x);
            const float volCenterY = (sliderMin.y + sliderMax.y) * 0.5f;
            const float volTrack = std::max(3.0f, tune(5.0f));
            float volT = std::clamp(volume / 100.0f, 0.0f, 1.0f);
            if (volActive) {
                float t = (ImGui::GetIO().MousePos.x - sliderMin.x) / volWidth;
                t = std::clamp(t, 0.0f, 1.0f);
                applyVolume(t * 100.0f, false);
                volT = t;
            }
            if (volDeactivated)
                applyVolume(volume, true);
            if (volHovered) {
                char volTip[32];
                std::snprintf(volTip, sizeof(volTip), "Volume %.0f", volume);
                ShowDelayedTooltip(volTip);
            }
            ImDrawList* vdl = ImGui::GetWindowDrawList();
            const ImU32 vTrackCol = ImGui::GetColorU32(hasMedia
                                                           ? ImVec4(0.30f, 0.34f, 0.40f, 0.65f)
                                                           : ImVec4(0.24f, 0.27f, 0.32f, 0.34f));
            ImVec4 volumeFill = ImGui::GetStyle().Colors[hasMedia ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled];
            volumeFill.w *= hasMedia ? 1.0f : 0.55f;
            const ImU32 vFillCol = ImGui::GetColorU32(volumeFill);
            const ImU32 vKnobCol = ImGui::GetColorU32(volumeFill);
            const float vHalf = volTrack * 0.5f;
            const float vFillX = sliderMin.x + volWidth * volT;
            if (volHovered) {
                const ImU32 glowCol = ImGui::GetColorU32(ImVec4(0.30f, 0.70f, 0.95f, 0.18f));
                vdl->AddRectFilled(ImVec2(sliderMin.x - tune(1.0f), volCenterY - (vHalf + tune(2.0f))),
                                   ImVec2(sliderMax.x + tune(1.0f), volCenterY + (vHalf + tune(2.0f))),
                                   glowCol, vHalf + tune(2.0f));
            }
            vdl->AddRectFilled(ImVec2(sliderMin.x, volCenterY - vHalf),
                               ImVec2(sliderMax.x, volCenterY + vHalf),
                               vTrackCol, vHalf);
            vdl->AddRectFilled(ImVec2(sliderMin.x, volCenterY - vHalf),
                               ImVec2(vFillX, volCenterY + vHalf),
                               vFillCol, vHalf);
            const float knobEps = 0.001f;
            if (volT > knobEps && volT < 1.0f - knobEps)
                vdl->AddCircleFilled(ImVec2(vFillX, volCenterY), tune(8.0f), vKnobCol, 16);

            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, popupSpacing);
            if (ImGui::BeginPopup("SubTrackPopup")) {
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
                for (size_t i = 0; i < ids.size(); ++i) {
                    const bool selected = ids[i] == static_cast<int>(sid);
                    if (ImGui::Selectable(labels[i].c_str(), selected)) {
                        int chosen = ids[i];
                        if (chosen == 0) {
                            mpv_set_property_string(mpv, "sid", "no");
                        } else if (chosen == -1) {
                            mpv_set_property_string(mpv, "sid", "auto");
                        } else {
                            int64_t value = chosen;
                            mpv_set_property(mpv, "sid", MPV_FORMAT_INT64, &value);
                        }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(4);

            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, popupRound);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, popupSpacing);
            if (ImGui::BeginPopup("AudioTrackPopup")) {
                const int64_t aid = mpv_get_int64(mpv, "aid", 0);
                auto tracks = mpv_read_tracks(mpv, "audio");
                std::vector<std::string> labels;
                std::vector<int> ids;
                labels.emplace_back("Off");
                ids.push_back(0);
                labels.emplace_back("Auto");
                ids.push_back(-1);
                for (const auto& tr : tracks) {
                    std::string labelQ = tr.title;
                    if (labelQ.empty())
                        labelQ = tr.lang.empty() ? ("Audio " + std::to_string(tr.id)) : tr.lang;
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
                for (size_t i = 0; i < ids.size(); ++i) {
                    const bool selected = ids[i] == static_cast<int>(aid);
                    if (ImGui::Selectable(labels[i].c_str(), selected)) {
                        int chosen = ids[i];
                        if (chosen == 0) {
                            mpv_set_property_string(mpv, "aid", "no");
                        } else if (chosen == -1) {
                            mpv_set_property_string(mpv, "aid", "auto");
                        } else {
                            int64_t value = chosen;
                            mpv_set_property(mpv, "aid", MPV_FORMAT_INT64, &value);
                        }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(4);
            ImGui::EndTable();
        }
            if (ImGui::IsWindowHovered(hoverFlags))
                uiHovered = true;
            ImGui::EndChild();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }

        static PanelDragState chatDrag;
        app.lastUiW = ui_w;
        app.lastUiH = ui_h;

        const float panelHeaderH = ImGui::GetFrameHeight() + basePad.y * 2.0f;
        const float resizeGrip = 12.0f;

        const float panelFade = (0.15f + 0.85f * dockAlpha) * uiFade;

        const float topBarHeight = topIconBtnSize + topPad.y * 2.0f;
        const float dockTop = (renderTopBar ? (topBarHeight + 24.0f) : 16.0f) + titleBarH;
        const float dockBottom = renderBottomBar ? barHeightUi + 16.0f : 16.0f;
        const float dockHeight = std::max(220.0f, static_cast<float>(ui_h) - dockTop - dockBottom);
        const float sidebarRailTop = titleBarH + 12.0f;
        const float sidebarRailBottom = 12.0f;
        const float sidebarRailHeight = std::max(220.0f, static_cast<float>(ui_h) - sidebarRailTop - sidebarRailBottom);
        const float dockPanelX = sideLayout
                                     ? std::max(edgePad, static_cast<float>(ui_w) - dockPanelW)
                                     : std::max(edgePad, static_cast<float>(ui_w) - dockPanelW - dockPad);
        const float panelW = dockPanelW;
        const float panelH = std::max(280.0f, dockHeight * 0.45f);
        const float minDockH = dockHeight * 0.30f;
        const float maxDockH = dockHeight * 0.80f;
        const float dockPanelY = dockTop;
        const float panelAreaLeft = edgePad;
        const float panelAreaRight = static_cast<float>(ui_w) - edgePad;
        const float panelAreaTop = dockTop;
        const float panelAreaBottom = static_cast<float>(ui_h) - dockBottom;
        const float panelAreaW = std::max(1.0f, panelAreaRight - panelAreaLeft);
        const float panelAreaH = std::max(1.0f, panelAreaBottom - panelAreaTop);
        const float defaultPanelW = std::min(panelAreaW, std::max(260.0f, panelAreaW * 0.36f));
        const float defaultPanelH = std::min(panelAreaH, std::max(260.0f, panelAreaH * 0.58f));
        const float defaultChatW = std::min(defaultPanelW, std::max(220.0f, panelAreaW * 0.25f));
        const float defaultChatH = std::min(defaultPanelH, std::max(220.0f, panelAreaH * 0.45f));
        const float minOverlayW = std::min(panelAreaW, 220.0f);
        const float minOverlayH = std::min(panelAreaH, 200.0f);
        const float maxOverlayH = panelAreaH;
        const float minSidebarWBase = std::min(panelAreaW, std::max(240.0f, panelW * 0.70f));
        const auto centeredSheetSize = [&](float preferredW, float preferredH, float minW, float minH) {
            const float width = std::clamp(preferredW, std::min(panelAreaW, minW), panelAreaW);
            const float height = std::clamp(preferredH, std::min(panelAreaH, minH), panelAreaH);
            return ImVec2(width, height);
        };
        PanelContext panelCtx{
            app, session, mpv,
            uiHovered, panelRectHovered, nextPanelHeaderValid, nextPanelHeaderMin, nextPanelHeaderMax,
            panelAreaLeft, panelAreaTop, panelAreaW, panelAreaH,
            panelHeaderH, panelFade, basePad, hoverFlags,
            font10, font12, font14, font16, font18, font22, fontChat,
            fontIcons, fontIconsSmall, fontIconsLarge, fontIconsTiny,
            accent,
            centeredSheetSize, tune,
            applyVideoColor, applyToneMapping, applyVideoShaders, applyAccent,
            applySubtitleStyle, openSubtitles,
            chatUnreadCount, chatSeenCount, chatInputActive,
            openFolder, browseShareFile,
            ICON_OPEN, ICON_CHAT, ICON_OVERLAY, ICON_SIDEBAR,
        };
        auto clampPanelToArea = [&](float* pos, float* size, float minW, float minH, float maxW, float maxH) {
            size[0] = std::clamp(size[0], minW, maxW);
            size[1] = std::clamp(size[1], minH, maxH);
            const float maxX = std::max(panelAreaLeft, panelAreaRight - size[0]);
            const float maxY = std::max(panelAreaTop, panelAreaBottom - size[1]);
            pos[0] = std::clamp(pos[0], panelAreaLeft, maxX);
            pos[1] = std::clamp(pos[1], panelAreaTop, maxY);
        };
        static float prevPanelAreaTop = panelAreaTop;
        const float topDelta = panelAreaTop - prevPanelAreaTop;
        if (!app.sidePanels && std::abs(topDelta) > 0.01f) {
            const float snapThreshold = 4.0f;
            auto shiftIfAnchored = [&](float* pos, const PanelDragState& state) {
                if (state.dragging || state.resizing)
                    return;
                if (pos[1] <= prevPanelAreaTop + snapThreshold)
                    pos[1] += topDelta;
            };
            shiftIfAnchored(app.chatPos, chatDrag);
        }
        prevPanelAreaTop = panelAreaTop;
        const bool anyDocked = chatActive && chatDockedSidebar;
        const float activeDockTop = chatDockedSidebar ? sidebarRailTop : dockPanelY;
        const float activeDockHeight = chatDockedSidebar ? sidebarRailHeight : dockHeight;
        float dockResizeX = dockPanelX;

        if (anyDocked) {
            ImGui::SetCursorPos(ImVec2(dockResizeX - resizeGrip, activeDockTop));
            ImGui::InvisibleButton("DockResizeW", ImVec2(resizeGrip, activeDockHeight));
            if (ImGui::IsItemActive()) {
                const float nextDockW = std::clamp(app.dockPanelW - ImGui::GetIO().MouseDelta.x, dockMinW, dockMaxW);
                if (std::abs(nextDockW - app.dockPanelW) > 0.01f) {
                    app.dockPanelW = nextDockW;
                    app.dirty = true;
                }
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        if (sessionActive) {
            DrawSessionPanel(panelCtx);
        }

        if (callActive) {
            DrawCallPanel(panelCtx);
        }

        if (chatActive) {
            const float panelAlpha = g_fullscreen ? 0.65f : 0.94f;
            const float minPanelW = app.sidePanels ? minSidebarWBase : minOverlayW;
            const float minPanelH = app.sidePanels ? minDockH : minOverlayH;
            const float maxPanelW = panelAreaW;
            const float maxPanelH = app.sidePanels ? maxDockH : maxOverlayH;
            ImVec2 panelPos;
            ImVec2 panelSize;
            if (chatDockedSidebar) {
                panelPos = ImVec2(dockPanelX, sidebarRailTop);
                panelSize = ImVec2(panelW, sidebarRailHeight);
            } else {
                if (app.chatSize[0] <= 0.0f) {
                    app.chatPos[0] = dockPanelX;
                    app.chatPos[1] = dockPanelY;
                    app.chatSize[0] = defaultChatW;
                    app.chatSize[1] = defaultChatH;
                }
                float chatPos[2] = {app.chatPos[0], app.chatPos[1]};
                float chatSize[2] = {app.chatSize[0], app.chatSize[1]};
                clampPanelToArea(chatPos, chatSize, minPanelW, minPanelH, maxPanelW, maxPanelH);
                UpdatePanelDrag(chatDrag, chatPos, chatSize,
                                minPanelW, minPanelH, maxPanelW, maxPanelH,
                                panelHeaderH, resizeGrip, edgePad, ui_w, ui_h);
                clampPanelToArea(chatPos, chatSize, minPanelW, minPanelH, maxPanelW, maxPanelH);
                if (chatDrag.dragging || chatDrag.resizing) {
                    app.chatPos[0] = chatPos[0];
                    app.chatPos[1] = chatPos[1];
                    app.chatSize[0] = chatSize[0];
                    app.chatSize[1] = chatSize[1];
                    app.dirty = true;
                }
                panelPos = ImVec2(chatPos[0], chatPos[1]);
                panelSize = ImVec2(chatSize[0], chatSize[1]);
            }
            const ImVec2 mousePos = io.MousePos;
            if (mousePos.x >= panelPos.x && mousePos.x <= panelPos.x + panelSize.x &&
                mousePos.y >= panelPos.y && mousePos.y <= panelPos.y + panelSize.y) {
                panelRectHovered = true;
            }
            nextPanelHeaderValid = true;
            nextPanelHeaderMin = panelPos;
            nextPanelHeaderMax = ImVec2(panelPos.x + panelSize.x, panelPos.y + panelHeaderH);
            DrawChatPanel(panelCtx, panelPos, panelSize, chatDockedSidebar);
        }

        if (subsActive) {
            DrawSubsPanel(panelCtx);
        }

        if (settingsActive) {
            DrawSettingsPanel(panelCtx);
        }

        lastPanelRectHovered = panelRectHovered;
        lastPanelHeaderValid = nextPanelHeaderValid;
        if (nextPanelHeaderValid) {
            lastPanelHeaderMin = nextPanelHeaderMin;
            lastPanelHeaderMax = nextPanelHeaderMax;
        }

        const bool hasTextFocus = io.WantTextInput || chatInputActive;
        const bool widgetActive = ImGui::IsAnyItemActive();
        const bool widgetFocused = ImGui::IsAnyItemFocused();
        if (!hasTextFocus && !widgetActive && !widgetFocused && !anyPopupOpen) {
            const bool ctrlDown = io.KeyCtrl;
            if (hasMedia && ImGui::IsKeyPressed(ImGuiKey_Space))
                togglePlay();
            if (hasMedia && ImGui::IsKeyPressed(ImGuiKey_M))
                toggleMute();
            if (hasMedia && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                seekBy(ctrlDown ? -10.0 : -5.0);
            if (hasMedia && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                seekBy(ctrlDown ? 10.0 : 5.0);
            if (hasMedia && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                applyVolume(volume + (ctrlDown ? 10.0f : 5.0f));
            if (hasMedia && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                applyVolume(volume - (ctrlDown ? 10.0f : 5.0f));
        }

        const bool videoAreaHovered = !uiHovered && !panelRectHovered &&
                                      io.MousePos.x <= viewW && io.MousePos.y <= videoH;
        if (videoAreaHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            g_pendingToggleFullscreen = true;
        if (videoAreaHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            ImGui::OpenPopup("VideoContext");
        const float popupRound = tune(4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, popupRound);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, popupRound);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPad);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, popupSpacing);
        if (ImGui::BeginPopup("VideoContext")) {
            if (ImGui::MenuItem("Open"))
                openVideo();
            if (!hasMedia)
                ImGui::BeginDisabled();
            if (ImGui::MenuItem("Open Subtitles"))
                openSubtitles();
            ImGui::Separator();
            if (ImGui::MenuItem(paused ? "Play" : "Pause"))
                togglePlay();
            if (ImGui::MenuItem("Stop")) {
                const char* cmd[] = { "stop", nullptr };
                mpv_command(mpv, cmd);
            }
            if (ImGui::MenuItem("Seek -10s"))
                seekBy(-10.0);
            if (ImGui::MenuItem("Seek +10s"))
                seekBy(10.0);
            if (ImGui::BeginMenu("Playback Speed")) {
                for (const float option : speedOptions) {
                    char label[16];
                    std::snprintf(label, sizeof(label), "%.2fx", option);
                    const bool selected = std::abs(option - speed) < 0.001f;
                    if (ImGui::MenuItem(label, nullptr, selected))
                        applySpeed(option);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Aspect Ratio")) {
                const double aspectOverride = mpv_get_double(mpv, "video-aspect-override", -1.0);
                auto aspectItem = [&](const char* label, double value) {
                    const bool selected = (value < 0.0)
                                              ? (aspectOverride < 0.0)
                                              : (std::abs(aspectOverride - value) < 0.01);
                    if (ImGui::MenuItem(label, nullptr, selected)) {
                        double next = value;
                        mpv_set_property(mpv, "video-aspect-override", MPV_FORMAT_DOUBLE, &next);
                    }
                };
                aspectItem("Auto", -1.0);
                aspectItem("16:9", 16.0 / 9.0);
                aspectItem("4:3", 4.0 / 3.0);
                aspectItem("21:9", 21.0 / 9.0);
                aspectItem("1:1", 1.0);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Subtitles")) {
                bool subVisible = mpv_get_flag(mpv, "sub-visibility", true);
                if (ImGui::MenuItem("Enabled", nullptr, subVisible)) {
                    subVisible = !subVisible;
                    app.subtitlesEnabled = subVisible;
                    mpv_set_flag(mpv, "sub-visibility", subVisible);
                }
                if (ImGui::MenuItem("Open Subtitle File"))
                    openSubtitles();
                ImGui::EndMenu();
            }
            if (!hasMedia)
                ImGui::EndDisabled();
            if (ImGui::BeginMenu("Panels")) {
                if (ImGui::MenuItem("Chat", nullptr, app.showChat)) {
                    app.showChat = !app.showChat;
                    if (app.showChat) {
                        app.showCall = false;
                        app.showSubs = false;
                        app.showSession = false;
                        app.showSettings = false;
                    }
                }
                if (ImGui::MenuItem("Voice Call", nullptr, app.showCall)) {
                    app.showCall = !app.showCall;
                    if (app.showCall) {
                        app.showChat = false;
                        app.showSubs = false;
                        app.showSession = false;
                        app.showSettings = false;
                    }
                }
                if (ImGui::MenuItem("Session", nullptr, app.showSession)) {
                    app.showSession = !app.showSession;
                    if (app.showSession) {
                        app.showCall = false;
                        app.showSubs = false;
                        app.showChat = false;
                        app.showSettings = false;
                    }
                }
                if (!hasMedia)
                    ImGui::BeginDisabled();
                if (ImGui::MenuItem("Subtitles", nullptr, app.showSubs)) {
                    app.showSubs = !app.showSubs;
                    if (app.showSubs) {
                        app.showCall = false;
                        app.showChat = false;
                        app.showSession = false;
                        app.showSettings = false;
                    }
                }
                if (!hasMedia)
                    ImGui::EndDisabled();
                if (ImGui::MenuItem("Settings", nullptr, app.showSettings)) {
                    app.showSettings = !app.showSettings;
                    if (app.showSettings) {
                        app.showCall = false;
                        app.showSubs = false;
                        app.showChat = false;
                        app.showSession = false;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Chat Sidebar Mode", nullptr, app.sidePanels)) {
                    app.sidePanels = !app.sidePanels;
                    app.dirty = true;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(g_fullscreen ? "Windowed" : "Fullscreen"))
                g_pendingToggleFullscreen = true;
            if (ImGui::MenuItem("Copy Session Link")) {
                const std::string url = session.serverUrl();
                const std::string code = session.sessionCode();
                std::string link;
                if (!url.empty() && !code.empty())
                    link = url + " " + code;
                else if (!code.empty())
                    link = code;
                if (!link.empty()) {
                    ImGui::SetClipboardText(link.c_str());
                    pushOsd("Session link copied");
                } else {
                    pushOsd("No session link");
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);

        if (!app.events.empty()) {
            // Toast: symmetric padding with each line centred both axes. Size off the
            // glyph height (not line-with-spacing) and place lines at explicit y so
            // there's no stray trailing gap that would push the text upward.
            const float lineH = ImGui::GetTextLineHeight();
            const float gapY = ImGui::GetStyle().ItemSpacing.y;
            const ImVec2 toastPad(tune(16.0f), tune(11.0f));
            float maxToastW = 0.0f;
            for (const auto& e : app.events)
                maxToastW = std::max(maxToastW, ImGui::CalcTextSize(e.text.c_str()).x);
            const int toastN = static_cast<int>(app.events.size());
            const float toastW = maxToastW + toastPad.x * 2.0f;
            const float toastH = toastPad.y * 2.0f + lineH * toastN + gapY * (toastN - 1);
            ImVec4 toastBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            toastBg.w = 0.85f;
            ImGui::SetCursorPos(ImVec2(tune(20.0f), tune(60.0f)));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, toastBg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, toastPad);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tune(10.0f));
            ImGui::BeginChild("Toasts", ImVec2(toastW, toastH), true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const float availW = ImGui::GetContentRegionAvail().x;
            const float startY = ImGui::GetCursorPosY();
            int toastIdx = 0;
            for (auto& e : app.events) {
                const float textW = ImGui::CalcTextSize(e.text.c_str()).x;
                ImGui::SetCursorPos(ImVec2(toastPad.x + std::max(0.0f, (availW - textW) * 0.5f),
                                           startY + toastIdx * (lineH + gapY)));
                ImGui::TextUnformatted(e.text.c_str());
                ++toastIdx;
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            for (auto& e : app.events)
                e.ttl -= ImGui::GetIO().DeltaTime;
            while (!app.events.empty() && app.events.front().ttl <= 0.0f)
                app.events.pop_front();
        }

        ImGui::End();
        ImGui::PopStyleVar(2);

        ImGui::Render();
        const float clear_color[4] = {0.05f, 0.07f, 0.09f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);


        static auto lastSave = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (app.dirty && std::chrono::duration_cast<std::chrono::seconds>(now - lastSave).count() >= 2) {
            save_config(app, volume, speed);
            app.dirty = false;
            lastSave = now;
        }
    }

    save_config(app, volume, speed);

    mpv_render_context_set_update_callback(renderState.ctx, nullptr, nullptr);
    renderState.running.store(false, std::memory_order_relaxed);
    renderState.cv.notify_one();
    if (renderThread.joinable())
        renderThread.join();
    g_renderState = nullptr;
    if (renderState.ctx)
        mpv_render_context_free(renderState.ctx);
    mpv_terminate_destroy(mpv);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_appIcon.srv) {
        g_appIcon.srv->Release();
        g_appIcon.srv = nullptr;
    }
    if (g_appIcon.iconBig) {
        DestroyIcon(g_appIcon.iconBig);
        g_appIcon.iconBig = nullptr;
    }
    if (g_appIcon.iconSmall) {
        DestroyIcon(g_appIcon.iconSmall);
        g_appIcon.iconSmall = nullptr;
    }

    CleanupVideoTexture();
    CleanupBlurTexture();
    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    if (comInited)
        CoUninitialize();

    return 0;
}
