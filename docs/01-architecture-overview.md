# 01 — Architecture Overview

## What SyncPlay is

A Windows desktop "watch-party" media player. The core idea: the app synchronizes
*playback state* (position, play/pause, speed) plus chat, reactions, file transfer,
and optional voice — the media itself is not routed through any SyncPlay
infrastructure.

Two ways to share a source:

1. **Local files** — every participant already has their own copy of the same
   file; only playback state crosses the wire. Tiny bandwidth, no media servers.
2. **Network streams** — the host opens a URL (direct `http(s)` media, HLS
   `.m3u8`, DASH — anything libmpv/ffmpeg speaks) and shares it with the session;
   guests are prompted, then each streams the *same source URL* directly. No file
   transfer, and everyone stays in sync exactly as with local files.

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

**Playback**
- Local video through libmpv (MKV/MP4/MOV/AVI/WebM), GPU decode (`hwdec=auto-copy-safe`).
- Network streams by URL (`Ctrl+U` / context menu): direct media, HLS, DASH.
- Playlist/queue: multi-file drag-drop + multi-select, next/prev, auto-advance.
- Resume where you left off (mpv watch-later, saved every 5 s and on quit).
- Subtitle + audio track controls; subtitle styling (font/size/color/position/outline).
- OpenSubtitles online search + download (user API key; moviehash + filename).
- Video controls: brightness/contrast/saturation/gamma/hue, tone mapping, GLSL shaders.

**Watch-party (session)**
- Host-authoritative sync of play/pause/seek/speed with guest drift correction
  **and RTT-based latency compensation**.
- Auto-reconnect with backoff + state resync after a dropped connection.
- Synced URL sessions (host shares a stream URL; guests consent-prompt then join).
- Text chat with per-message status, sender avatars, and a "system timeline".
- Floating **emoji reactions** relayed to everyone over the video.
- Chunked file sharing (128 KB chunks, ≤2 GB, SHA-1, → Downloads) + combined Files view.
- Optional 1:1 voice via the relay (miniaudio capture/playback).
- `syncplay://` invite links (registered per-user + by installer) that auto-join.

**Interface & platform**
- Dynamic UI accent sampled from the video's dominant colour (toggle in Settings).
- "Continue watching" resume cards on the idle screen.
- Timeline/volume glow-up, animated toast cards, micro-animations throughout.
- Taskbar integration: playback progress, prev/play-pause/next thumb buttons, media keys.
- Custom borderless Win32 window: rounded corners, custom title bar, app icon,
  per-user "Open With" registration for video extensions.
- Optional system-proxy routing (Settings → Connection), keyboard-shortcut overlay (F1).
- Crash minidumps + optional file logging (`%APPDATA%/SyncPlay/`).
- Startup update check against GitHub Releases.

## Technology stack

| Concern | Choice |
|---|---|
| Language | C++20 |
| Windowing | Win32 (custom borderless chrome) |
| GPU / present | Direct3D 11 + DXGI |
| Text rendering | Direct2D + DirectWrite (for chat / complex scripts / emoji) |
| Image decode | Windows Imaging Component (WIC) for PNG/ICO |
| UI | Dear ImGui (vendored in `third_party/imgui`, Win32+DX11 backends) |
| Media | libmpv, software render (`MPV_RENDER_API_TYPE_SW`), GPU decode via `hwdec` |
| Networking | libdatachannel (WebSocket only), nlohmann-json |
| HTTP client | WinHTTP (update check, OpenSubtitles, proxy detection) |
| Audio (voice) | miniaudio (header-only) |
| Crash capture | DbgHelp `MiniDumpWriteDump` |

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
core/         helpers: clock, controller iface, sha1, base64, logging, task queue,
              utf, crash_dump, update_check, net_proxy
media/        libmpv helpers + OpenSubtitles client
network/      signaling server + client + relay voice
packaging/    Inno Setup script + build_installer.ps1
platform/     Win32 window chrome, D3D11 device, input, file dialog, taskbar
render/       software render thread + D3D texture upload + blur/accent sampling
scripts/      register_file_associations.ps1
src/          main.cpp — entry point + entire per-frame loop
sync/         session orchestration + drift-correction algorithm
third_party/  vendored ImGui + mpv dev package
ui/           AppState + config persistence + extracted panels + widget helpers
.github/      release CI workflow
```

## Configuration & data locations

- Config JSON: `%APPDATA%/SyncPlay/config.json` (settings, recent-media list,
  proxy toggle, accent, etc.)
- ImGui layout: `%APPDATA%/SyncPlay/imgui.ini`
- Watch-later (resume) positions: `%APPDATA%/SyncPlay/watch_later/`
- Log (opt-in): `%APPDATA%/SyncPlay/syncplay.log`
- Crash dumps: `%APPDATA%/SyncPlay/crash-YYYYMMDD-HHMMSS.dmp`
- Received shared files: user's **Downloads** folder (`FOLDERID_Downloads`).
- Registrations under `HKCU\Software\Classes` (per-user, no admin):
  - "Open With" ProgId `SyncPlay.Video` for `.mp4/.mkv/.mov/.avi/.webm`.
  - `syncplay://` URL protocol for invite links.

## Versioning & releases

The app version lives in **one place** — `project(SyncPlay VERSION x.y.z)` in
`CMakeLists.txt`. It is passed to the app as the `SYNCPLAY_VERSION` compile
definition (About tab, update check) and parsed by `packaging/build_installer.ps1`.
The GitHub Actions workflow (`.github/workflows/release.yml`) builds and attaches
the Inno Setup installer to a Release when a `v*` tag is pushed.

## License

Proprietary — see `LICENSE`. Copyright © Amirsalar Saberi rad, all rights reserved.
Bundled third-party components (libmpv, Dear ImGui, libdatachannel, nlohmann-json,
miniaudio, …) retain their own licenses.
