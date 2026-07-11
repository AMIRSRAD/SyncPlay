# SyncPlay

SyncPlay is a Windows watch-party media player. Watch together in perfect sync —
from your own local files **or** a shared stream URL — with chat, floating emoji
reactions, file sharing, and optional voice, all carried over a lightweight
host-relay server.

> Status: Windows-only desktop app. Actively developed.

## Screenshots

<!-- Replace these with current screenshots. -->
<img width="1919" height="1029" alt="main" src="https://github.com/user-attachments/assets/fabf31b9-6d19-412a-979f-d4f8aaf26d17" />
<img width="963" height="472" alt="session" src="https://github.com/user-attachments/assets/23f760ca-ce32-47a9-939f-90cebe5f129d" />
<img width="1269" height="760" alt="settings" src="https://github.com/user-attachments/assets/fc0cb478-fd55-4828-ba43-467e9ced6453" />

## Features

**Playback**
- Local video through libmpv (MKV / MP4 / MOV / AVI / WebM), with **GPU decoding**.
- **Network streams by URL** — `Ctrl+U` or right-click → *Open URL…*: direct media
  links, HLS (`.m3u8`), DASH, internet radio — anything mpv/ffmpeg can play.
- Playlist / queue: drag-drop multiple files (or multi-select), next/prev, auto-advance.
- **Resume where you left off** — playback position is remembered per file.
- Subtitle & audio track controls; subtitle styling (font, size, colour, position, outline).
- **OpenSubtitles** online search & download (bring your own free API key).
- Video controls: brightness, contrast, saturation, gamma, hue, tone mapping, GLSL shaders.

**Watch together**
- Host-authoritative sync of play / pause / seek / speed, with drift correction
  **and network-latency compensation** so peers stay truly frame-aligned.
- **Auto-reconnect** with resync after a dropped connection.
- **Synced URL sessions** — the host opens a stream URL and everyone watches the
  same source (with a consent prompt), no file transfer required.
- Text chat with per-message status, sender avatars, and a session timeline.
- **Floating emoji reactions** that pop up over the video for everyone.
- File sharing (chunked, checksum-verified, saved to Downloads) + a Files view.
- Optional 1:1 voice call through the relay.
- **`syncplay://` invite links** — one click opens SyncPlay and joins the session.

**Interface**
- Dynamic UI accent that tints the whole app with the video's dominant colour.
- "Continue watching" cards on the idle screen.
- Polished timeline & volume sliders, animated toasts, and micro-animations throughout.
- **Taskbar integration** — playback progress on the icon, prev/play-pause/next
  thumbnail buttons, and hardware media-key support.
- Optional **system-proxy** routing for streams, sessions, and online search.
- Keyboard-shortcut overlay (**F1**), custom Win32 title bar, crash reporting.

## How It Works

Media is never routed through any SyncPlay server. There are two ways to share a source:

**Local files** — each person opens their own copy; only playback state crosses the wire.

**Stream URL** — the host opens a link and shares it; each guest streams the same URL directly.

Either way, the host starts an embedded WebSocket relay server:

```text
Host app  <---- embedded relay server ---->  Guest app
```

The relay carries playback state, control intents, chat, reactions, shared-URL
notices, file verification metadata, shared-file chunks, and optional voice frames.
The default path does not use STUN, TURN, or WebRTC P2P.

## Download / Run

Grab the latest installer from **[Releases](https://github.com/AMIRSRAD/SyncPlay/releases)**.

On first run, SyncPlay registers (per-user, no admin) as an *Open With* option for
common video formats and as the handler for `syncplay://` invite links. It does not
take over your default file associations.

## Starting A Session

**Host:** open the Session panel → pick the network interface (or keep Auto) →
**Create Session** → share the URL and code, or right-click the video →
**Copy Session Link** for a one-click `syncplay://` invite.

**Guest:** open the Session panel → enter the host URL and code → **Join Session**
(or just click an invite link).

LAN example:

```text
ws://192.168.1.25:49152
```

Do not use `localhost` or `127.0.0.1` from the guest machine.

## Watch A Stream Together

1. Host: **Ctrl+U** → paste a direct media / HLS / DASH URL → **Open**.
2. In a session, guests get a "watch together" prompt for that URL — accept to join in.
3. Everyone streams the same source; sync works exactly as with local files.

## Firewall / Network Notes

The host must be reachable by the guest.

- Same LAN: allow inbound TCP to the host app/port.
- Different networks: port forwarding, VPN, or another reachable route is required.
- Windows Firewall rules are path-specific; a moved/copied `SyncPlay.exe` may need a new rule.

Default port: `49152`

```powershell
netsh advfirewall firewall add rule name="SyncPlay TCP" dir=in action=allow program="C:\Path\To\SyncPlay.exe" enable=yes profile=any protocol=TCP localport=49152
```

If you're behind a proxy, enable **Settings → Connection → Use system proxy**.

## Build From Source

Toolchain: Windows, MSVC (Visual Studio C++), CMake 3.21+, Ninja, vcpkg.

vcpkg deps: `libdatachannel`, `miniaudio`, `nlohmann-json`. Not vcpkg-managed:
the libmpv SDK (external, at `MPV_ROOT`) and Dear ImGui (vendored in `third_party/imgui`).

The project expects the mpv SDK here by default:

```text
C:/packages/mpv/include/mpv/client.h
C:/packages/mpv/lib/mpv.lib
C:/packages/mpv/bin/libmpv-2.dll
```

Configure & build:

```powershell
cmake -S . -B cmake-build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DMPV_ROOT=C:/packages/mpv

cmake --build cmake-build-release --target SyncPlay
```

Output: `cmake-build-release/SyncPlay.exe`.

### Packaging & releases

`packaging/build_installer.ps1` stages the exe + runtime DLLs + VC++ runtime + assets
and compiles the Inno Setup installer (version read from `CMakeLists.txt`). Pushing a
`v*` tag runs `.github/workflows/release.yml`, which builds and attaches the installer
to a GitHub Release.

### mpv SDK note

If your SDK has `libmpv-2.dll` + headers but no `mpv.lib`, generate an MSVC import
library from the DLL exports — the `.def` **must** start with `LIBRARY libmpv-2.dll`,
or the exe will try to import a non-existent `mpv.dll` and fail to launch.

## Diagnostics

- File logging (off by default): **Settings → Diagnostics → Enable file logging** →
  `%APPDATA%/SyncPlay/syncplay.log`.
- Crashes write a minidump to `%APPDATA%/SyncPlay/crash-*.dmp` automatically.

## Documentation

Deeper technical docs live in [`docs/`](docs/): architecture, media/render pipeline,
subsystem map, sync algorithm, and the signaling protocol.

## Known Limitations

- Windows only.
- Transport is a host relay server (not WebRTC P2P); the host must be reachable by guests.
- Session security is trusted-LAN-oriented: plain `ws://`, access gated only by the
  6-char code. Don't expose it on an untrusted network.
- Voice is one-to-one and uncompressed (fine on LAN, heavy over the internet).
- Clicking an invite link while SyncPlay is already open starts a second instance.

## Project Layout

```text
assets/       app icon and runtime image assets
core/         shared helpers (logging, sha1, crash dump, update check, proxy, ...)
media/        mpv helpers + OpenSubtitles client
network/      signaling relay client/server and relay voice
packaging/    Inno Setup script + installer build script
platform/     Win32, D3D11, window chrome, input, taskbar
render/        software render + blur/accent sampling
src/          main application loop
sync/         session and playback synchronization
third_party/  vendored dependencies
ui/           app state, panels, and UI helpers
```

## License

Proprietary — see [`LICENSE`](LICENSE). Copyright © 2026 Amirsalar Saberi rad. All
rights reserved. Bundled third-party components retain their own licenses.
