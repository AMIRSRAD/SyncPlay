#include "platform.h"

#include <shellapi.h>
#include <dwmapi.h>
#include <tchar.h>
#include <cmath>
#include <windowsx.h>
#include <backends/imgui_impl_win32.h>

#include "render/render_sw.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef WM_NCUAHDRAWCAPTION
#define WM_NCUAHDRAWCAPTION 0x00AE
#endif
#ifndef WM_NCUAHDRAWFRAME
#define WM_NCUAHDRAWFRAME 0x00AF
#endif
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

ID3D11Texture2D* g_videoTex = nullptr;
ID3D11ShaderResourceView* g_videoSrv = nullptr;
int g_videoTexW = 0;
int g_videoTexH = 0;

ID3D11Texture2D* g_blurTex = nullptr;
ID3D11ShaderResourceView* g_blurSrv = nullptr;
int g_blurTexW = 0;
int g_blurTexH = 0;
bool g_blurReady = false;
bool g_glassEnabled = true;

HWND g_hWnd = nullptr;
bool g_fullscreen = false;
WINDOWPLACEMENT g_wpPrev = { sizeof(g_wpPrev) };
DWORD g_stylePrev = 0;
DWORD g_exStylePrev = 0;
bool g_pendingToggleFullscreen = false;
bool g_pendingTogglePlay = false;
bool g_pendingDrop = false;
bool g_pendingDpiChange = false;
unsigned int g_pendingDpiValue = 96;
std::vector<std::wstring> g_dropPaths;
SwRenderState* g_renderState = nullptr;
std::atomic<bool> g_requestExit{false};
std::atomic<bool> g_inSizing{false};
TitleBarHitTest g_titleBarHitTest{};
bool g_wasMaximized = false;
static int g_pendingResizeW = 0;
static int g_pendingResizeH = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ApplyCustomWindowChrome(HWND hWnd) {
    if (!hWnd)
        return;
    const int cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    const COLORREF borderColor = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
}

void ToggleFullscreen(HWND hWnd) {
    if (!g_fullscreen) {
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
        const int cornerPref = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
        ShowWindow(hWnd, SW_HIDE);
        g_wasMaximized = IsZoomed(hWnd) != FALSE;
        g_stylePrev = GetWindowLong(hWnd, GWL_STYLE);
        g_exStylePrev = GetWindowLong(hWnd, GWL_EXSTYLE);
        GetWindowPlacement(hWnd, &g_wpPrev);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLong(hWnd, GWL_STYLE, g_stylePrev & ~WS_OVERLAPPEDWINDOW);
            SetWindowLong(hWnd, GWL_EXSTYLE, g_exStylePrev & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                                             WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
            SetWindowPos(hWnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        ShowWindow(hWnd, SW_SHOWNA);
        g_fullscreen = true;
    } else {
        if (g_pSwapChain) {
            g_pSwapChain->SetFullscreenState(FALSE, nullptr);
        }
        BOOL enableTransitions = FALSE;
        DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &enableTransitions, sizeof(enableTransitions));
        ShowWindow(hWnd, SW_HIDE);
        SetWindowLong(hWnd, GWL_STYLE, g_stylePrev);
        SetWindowLong(hWnd, GWL_EXSTYLE, g_exStylePrev);
        SetWindowPlacement(hWnd, &g_wpPrev);
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
        ApplyCustomWindowChrome(hWnd);
        if (g_wasMaximized) {
            HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(monitor, &mi)) {
                const int w = mi.rcWork.right - mi.rcWork.left;
                const int h = mi.rcWork.bottom - mi.rcWork.top;
                SetWindowPos(hWnd, nullptr, mi.rcWork.left, mi.rcWork.top, w, h,
                             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            }
            ShowWindow(hWnd, SW_MAXIMIZE);
        } else {
            ShowWindow(hWnd, SW_SHOWNA);
        }
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool imguiHandled = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

    switch (msg) {
    case WM_NCACTIVATE:
        if (!g_fullscreen)
            return 1;
        break;
    case WM_NCCALCSIZE:
        if (wParam && !g_fullscreen)
            return 0;
        break;
    case WM_NCPAINT:
        if (!g_fullscreen)
            return 0;
        break;
    case WM_NCUAHDRAWCAPTION:
    case WM_NCUAHDRAWFRAME:
        if (!g_fullscreen)
            return 0;
        break;
    case WM_GETMINMAXINFO: {
        if (!g_fullscreen) {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(monitor, &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
                return 0;
            }
        }
        break;
    }
    case WM_DPICHANGED: {
        // Window moved to a monitor with a different DPI. Resize/reposition to the
        // OS-suggested rect and ask the app loop to rebuild fonts at the new scale.
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hWnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_pendingDpiValue = HIWORD(wParam);
        g_pendingDpiChange = true;
        return 0;
    }
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && g_pSwapChain != nullptr && wParam != SIZE_MINIMIZED) {
            const int w = (int)LOWORD(lParam);
            const int h = (int)HIWORD(lParam);
            if (g_inSizing.load(std::memory_order_relaxed)) {
                g_pendingResizeW = w;
                g_pendingResizeH = h;
                return 0;
            }
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        g_inSizing.store(true, std::memory_order_relaxed);
        return 0;
    case WM_EXITSIZEMOVE:
        g_inSizing.store(false, std::memory_order_relaxed);
        if (g_pd3dDevice != nullptr && g_pSwapChain != nullptr &&
            g_pendingResizeW > 0 && g_pendingResizeH > 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)g_pendingResizeW, (UINT)g_pendingResizeH,
                                        DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        g_pendingResizeW = 0;
        g_pendingResizeH = 0;
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_NCHITTEST: {
        if (g_fullscreen)
            break;
        RECT winRect{};
        GetWindowRect(hWnd, &winRect);
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const UINT dpi = GetDpiForWindow(hWnd);
        const int frameX = GetSystemMetricsForDpi(SM_CXFRAME, dpi);
        const int frameY = GetSystemMetricsForDpi(SM_CYFRAME, dpi);
        const int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int borderX = frameX + padding;
        const int borderY = frameY + padding;
        const bool onLeft = pt.x >= winRect.left && pt.x < winRect.left + borderX;
        const bool onRight = pt.x <= winRect.right && pt.x > winRect.right - borderX;
        const bool onTop = pt.y >= winRect.top && pt.y < winRect.top + borderY;
        const bool onBottom = pt.y <= winRect.bottom && pt.y > winRect.bottom - borderY;
        if (onTop && onLeft)
            return HTTOPLEFT;
        if (onTop && onRight)
            return HTTOPRIGHT;
        if (onBottom && onLeft)
            return HTBOTTOMLEFT;
        if (onBottom && onRight)
            return HTBOTTOMRIGHT;
        if (onLeft)
            return HTLEFT;
        if (onRight)
            return HTRIGHT;
        if (onTop)
            return HTTOP;
        if (onBottom)
            return HTBOTTOM;

        if (g_titleBarHitTest.enabled) {
            POINT clientPt = pt;
            ScreenToClient(hWnd, &clientPt);
            if (PtInRect(&g_titleBarHitTest.minRect, clientPt) ||
                PtInRect(&g_titleBarHitTest.maxRect, clientPt) ||
                PtInRect(&g_titleBarHitTest.closeRect, clientPt))
                return HTCLIENT;
            if (PtInRect(&g_titleBarHitTest.dragRect, clientPt))
                return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_DESTROY:
        if (g_renderState) {
            g_renderState->running.store(false, std::memory_order_relaxed);
            g_renderState->cv.notify_one();
        }
        g_requestExit.store(true, std::memory_order_relaxed);
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        if (g_renderState) {
            g_renderState->running.store(false, std::memory_order_relaxed);
            g_renderState->cv.notify_one();
        }
        g_requestExit.store(true, std::memory_order_relaxed);
        DestroyWindow(hWnd);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        g_dropPaths.clear();
        for (UINT i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH] = {};
            if (DragQueryFileW(drop, i, path, MAX_PATH))
                g_dropPaths.emplace_back(path);
        }
        if (!g_dropPaths.empty())
            g_pendingDrop = true;
        DragFinish(drop);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_F11 || (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)))
            g_pendingToggleFullscreen = true;
        else if (wParam == VK_ESCAPE && g_fullscreen)
            g_pendingToggleFullscreen = true;
        return 0;
    }
    if (imguiHandled)
        return true;
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                      featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                      &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (!pBackBuffer)
        return;
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void CleanupVideoTexture() {
    if (g_videoSrv) { g_videoSrv->Release(); g_videoSrv = nullptr; }
    if (g_videoTex) { g_videoTex->Release(); g_videoTex = nullptr; }
    g_videoTexW = 0;
    g_videoTexH = 0;
}

void EnsureVideoTexture(int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (g_videoTexW == w && g_videoTexH == h && g_videoTex && g_videoSrv)
        return;
    CleanupVideoTexture();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_videoTex))) {
        g_pd3dDevice->CreateShaderResourceView(g_videoTex, nullptr, &g_videoSrv);
        g_videoTexW = w;
        g_videoTexH = h;
    }
}

void CleanupBlurTexture() {
    if (g_blurSrv) { g_blurSrv->Release(); g_blurSrv = nullptr; }
    if (g_blurTex) { g_blurTex->Release(); g_blurTex = nullptr; }
    g_blurTexW = 0;
    g_blurTexH = 0;
    g_blurReady = false;
}

void EnsureBlurTexture(int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (g_blurTexW == w && g_blurTexH == h && g_blurTex && g_blurSrv)
        return;
    CleanupBlurTexture();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_blurTex))) {
        g_pd3dDevice->CreateShaderResourceView(g_blurTex, nullptr, &g_blurSrv);
        g_blurTexW = w;
        g_blurTexH = h;
    }
}
