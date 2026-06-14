#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
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

extern HWND g_hWnd;
extern bool g_fullscreen;
extern WINDOWPLACEMENT g_wpPrev;
extern DWORD g_stylePrev;
extern DWORD g_exStylePrev;
extern bool g_pendingToggleFullscreen;
extern bool g_pendingTogglePlay;
extern bool g_pendingDrop;
extern bool g_pendingDpiChange;
extern unsigned int g_pendingDpiValue;
extern std::wstring g_dropPath;
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
void ToggleFullscreen(HWND hWnd);
void ApplyCustomWindowChrome(HWND hWnd);
