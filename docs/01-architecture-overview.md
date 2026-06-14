# 01 — Architecture Overview

## What SyncPlay is

A Windows desktop "watch-party" media player. The core idea: **don't stream the
movie**. Every participant already has their own local copy of the same file; the
app only synchronizes *playback state* (position, play/pause, speed) plus chat,
file transfer, and optional voice. This keeps bandwidth tiny and avoids any media
infrastructure.

## The relay model

```
        Host machine                                    Guest machine(s)
 ┌──────────────────────────┐                      ┌─────────────────────────┐
 │ SignalingServer (ws://)   │◄──── WebSocket ─────►│ SignalingClient          │
 │  + SignalingClient("host")│  state / chat /      │  ("guest")               │
 │  mpv ► SW render ► D3D11   │  file chunks / voice │  mpv ► SW render ► D3D11  │
 └──────────────────────────┘  (all JSON text)      └─────────────────────────┘
```

- The **host** starts an embedded `SignalingServer` (libdatachannel
  `rtc::WebSocketServer`) on a TCP port (default **49152**) and *also* connects to
  it as a normal `SignalingClient` identified as `"host"`.
- **Guests** connect to `ws://<host-ip>:<port>` with a 6-character session **code**
  and identify as `"guest"`.
- The server is a pure **relay**: it groups sockets into a `Session` by code and
  forwards messages (host → guests, guest → host, fan-out for chat/state/shares).

There is **no WebRTC peer connection, no STUN/TURN, and no TLS** on the default
(and only) path. libdatachannel is linked but used *only* for its WebSocket
client/server — see [06-findings-and-issues.md](06-findings-and-issues.md).

## Host vs guest roles

| | Host | Guest |
|---|---|---|
| Authoritative clock | ✅ source of truth | ❌ follows host |
| Sends `state` packets | ✅ every 1500ms while playing (+ on actions) | ❌ never (early-returns) |
| Drives playback directly | ✅ | only if it *is* the host; otherwise sends **intents** |
| Sends control `intent` | ❌ | ✅ (host executes + re-broadcasts) |
| Sends `file` identity | ✅ | ❌ |

A connected guest's UI buttons do **not** mutate its own mpv. They round-trip
through the host: the guest sends an `intent`, the host applies it and broadcasts
the resulting authoritative `state`, which the guest then applies. This is gated by
`requestOnly()` in `src/main.cpp` (`sessionActive && !isHost && transportConnected`).

## Feature set

- Local video playback through libmpv (MKV/MP4/MOV/AVI/WebM).
- Host-authoritative sync of play/pause/seek/speed with guest drift correction.
- Text chat with per-message status + a "system timeline" of session events.
- Chunked file sharing (128 KB chunks, ≤2 GB) with progress, SHA-1 verification,
  saved to the Downloads folder.
- Combined "Files" view (active transfers + recent downloads).
- Optional 1:1 voice via the relay (miniaudio capture/playback).
- Subtitle + audio track controls; subtitle styling (font/size/color/position/
  outline/shadow/spacing).
- Video controls: brightness/contrast/saturation/gamma/hue, tone mapping, GLSL shaders.
- Playlist + custom timeline UI.
- Custom borderless Win32 window: rounded corners, custom title bar, app icon,
  per-user "Open With" registration for video extensions.
- Optional file logging (`%APPDATA%/SyncPlay/syncplay.log`).

## Technology stack

| Concern | Choice |
|---|---|
| Language | C++20 |
| Windowing | Win32 (custom borderless chrome) |
| GPU / present | Direct3D 11 + DXGI |
| Text rendering | Direct2D + DirectWrite (for chat / complex scripts / emoji) |
| Image decode | Windows Imaging Component (WIC) for PNG/ICO |
| UI | Dear ImGui (vendored in `third_party/imgui`, Win32+DX11 backends) |
| Media | libmpv, software render (`MPV_RENDER_API_TYPE_SW`) |
| Networking | libdatachannel (WebSocket only), nlohmann-json |
| Audio (voice) | miniaudio (header-only) |

## Build

- **Toolchain:** Windows, MSVC (Visual Studio C++), CMake 3.21+, Ninja, vcpkg.
- **vcpkg deps** (`vcpkg.json`): `libdatachannel`, `miniaudio`, `nlohmann-json`.
- **Not** vcpkg-managed: libmpv (external SDK at `MPV_ROOT`, default
  `C:/packages/mpv`) and ImGui (vendored).
- CRT forced to dynamic (`MultiThreadedDLL`); `RTC_STATIC` links libdatachannel statically.
- Output: `cmake-build-release/SyncPlay.exe` (GUI subsystem, no console).
- Post-build copies `assets/SyncPlay.png` + `.ico` next to the exe.

```powershell
cmake -S . -B cmake-build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg-master/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DMPV_ROOT=C:/packages/mpv
cmake --build cmake-build-release --target SyncPlay
```

Linked Windows system libs: `d3d11 dxgi d2d1 dwrite comdlg32 ole32 shell32 user32
gdi32 iphlpapi dwmapi windowscodecs`.

## Project layout

```
assets/       app icon + runtime image assets
core/         header-only helpers (clock, controller iface, sha1, base64, logging, task queue, utf)
media/        libmpv property/track/playlist helpers
network/      signaling server + client + relay voice
platform/     Win32 window chrome, D3D11 device, input, file dialog
render/        software render thread + D3D texture upload
scripts/      register_file_associations.ps1
src/          main.cpp — entry point + entire per-frame loop
sync/         session orchestration + drift-correction algorithm
third_party/  vendored ImGui + mpv dev package
ui/           AppState + config persistence + panel/widget helpers
```

## Configuration & data locations

- Config JSON: `%APPDATA%/SyncPlay/config.json`
- ImGui layout: `%APPDATA%/SyncPlay/imgui.ini`
- Log (opt-in): `%APPDATA%/SyncPlay/syncplay.log`
- Received shared files: user's **Downloads** folder (`FOLDERID_Downloads`).
- "Open With" registration: `HKCU\Software\Classes` (per-user, no admin), ProgId
  `SyncPlay.Video`, for `.mp4/.mkv/.mov/.avi/.webm`.
