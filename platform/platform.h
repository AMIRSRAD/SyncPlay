#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <atomic>

struct SwRenderState;

extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pd3dDeviceContext;
extern IDXGISwapChain* g_pSwapChain;
extern ID3D11RenderTargetView* g_mainRenderTargetView;

extern ID3D11Texture2D* g_videoTex;
extern ID3D11ShaderResourceView* g_videoSrv;
extern int g_videoTexW;
extern int g_videoTexH;

// Small downsampled+blurred copy of the current frame, used as a frosted-glass
// backdrop behind the overlay panels.
extern ID3D11Texture2D* g_blurTex;
extern ID3D11ShaderResourceView* g_blurSrv;
extern int g_blurTexW;
extern int g_blurTexH;
extern bool g_blurReady;
// When false, the per-frame blur downsample is skipped and panels render solid
// (saves CPU on low-end systems). Toggled from the Settings > Appearance panel.
extern bool g_glassEnabled;

// Saturation-weighted average colour of the current video frame (sRGB 0..1),
// sampled sparsely during the frame upload. Feeds the dynamic UI accent.
extern float g_videoAccentColor[3];
extern bool g_videoAccentValid;

extern HWND g_hWnd;
extern bool g_fullscreen;
extern WINDOWPLACEMENT g_wpPrev;
extern DWORD g_stylePrev;
extern DWORD g_exStylePrev;
extern bool g_pendingToggleFullscreen;
extern bool g_pendingTogglePlay;
// Set by taskbar thumbnail buttons and hardware media keys.
extern bool g_pendingPlaylistPrev;
extern bool g_pendingPlaylistNext;
extern bool g_pendingDrop;
extern bool g_pendingDpiChange;
extern unsigned int g_pendingDpiValue;
extern std::vector<std::wstring> g_dropPaths;
extern SwRenderState* g_renderState;
extern std::atomic<bool> g_requestExit;
extern std::atomic<bool> g_inSizing;
extern bool g_wasMaximized;

struct TitleBarHitTest {
    bool enabled = false;
    RECT dragRect{};
    RECT minRect{};
    RECT maxRect{};
    RECT closeRect{};
};

extern TitleBarHitTest g_titleBarHitTest;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void CleanupVideoTexture();
void EnsureVideoTexture(int w, int h);
void CleanupBlurTexture();
void EnsureBlurTexture(int w, int h);
void ToggleFullscreen(HWND hWnd);
void ApplyCustomWindowChrome(HWND hWnd);

// Taskbar integration: progress on the taskbar icon and prev/play-pause/next
// thumbnail buttons. Call UpdateTaskbar periodically (throttled by the caller);
// CleanupTaskbar on shutdown.
void UpdateTaskbar(HWND hWnd, double position, double duration, bool paused, bool hasMedia);
void CleanupTaskbar();
