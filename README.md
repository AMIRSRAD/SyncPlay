# SyncPlay

SyncPlay is a Windows watch-party media player. Everyone plays their own local media file, while a host-authoritative session keeps playback, chat, file sharing, and optional voice connected through a simple host relay server.

> Status: Windows-only desktop app. Actively changing.

## Screenshots


<img width="1919" height="1029" alt="main" src="https://github.com/user-attachments/assets/fabf31b9-6d19-412a-979f-d4f8aaf26d17" />
<img width="963" height="472" alt="session" src="https://github.com/user-attachments/assets/23f760ca-ce32-47a9-939f-90cebe5f129d" />
<img width="1269" height="760" alt="settings" src="https://github.com/user-attachments/assets/fc0cb478-fd55-4828-ba43-467e9ced6453" />


## Features

- Local video playback through libmpv.
- Host-authoritative sync for play, pause, seek, and playback speed.
- Guest playback correction with drift handling.
- Session hosting through an embedded WebSocket relay server.
- Text chat with local message status and system timeline chips.
- File sharing with chunked transfer, progress, retry, checksum verification, and Downloads-folder output.
- Combined Files view for active transfers and recently downloaded files.
- Optional one-to-one voice call through the host relay server.
- Subtitle and audio track controls.
- Subtitle styling: font, size, color, position, outline, shadow, spacing.
- Video controls: brightness, contrast, saturation, gamma, hue, tone mapping, shaders.
- Playlist controls and custom timeline UI.
- Custom Win32 title bar, rounded corners, app icon, and Open With registration.
- Optional file logging for diagnostics.

## How It Works

SyncPlay does not stream the movie/video. Each user opens a local copy of the media file.

The host starts an embedded WebSocket relay server:

```text
Host app  <---- embedded relay server ---->  Guest app
```

The relay carries:

- playback state
- guest control intents
- chat messages
- file verification metadata
- shared-file chunks
- optional voice frames

The current default path does not use STUN, TURN, Google servers, or WebRTC P2P.

## Download / Run

https://github.com/AMIRSRAD/SyncPlay/releases/tag/Release



On first run, SyncPlay may register itself as an Open With option for common video formats. This is per-user and does not take over default file associations.

## Starting A Session

Host:

1. Open SyncPlay.
2. Open the Session panel.
3. Choose the network interface if needed, or keep Auto.
4. Click Create Session.
5. Share the shown URL and code.

Guest:

1. Open SyncPlay.
2. Open the Session panel.
3. Enter the host URL and code.
4. Click Join Session.

LAN example:

```text
ws://192.168.1.25:49152
```

Do not use `localhost` or `127.0.0.1` from the guest machine.

## Firewall / Network Notes

The host must be reachable by the guest.

- Same LAN: allow inbound TCP to the host app/port.
- Different networks: port forwarding, VPN, or another reachable route is required.
- Windows Firewall rules are path-specific. If you move/copy `SyncPlay.exe`, the new path may need a new allow rule.

Default port:

```text
49152
```

Example administrator command:

```powershell
netsh advfirewall firewall add rule name="SyncPlay TCP" dir=in action=allow program="C:\Path\To\SyncPlay.exe" enable=yes profile=any protocol=TCP localport=49152
```

## Build From Source

Tested target:

- Windows
- Visual Studio C++ toolchain
- CMake 3.21+
- Ninja
- vcpkg

Dependencies:

- libdatachannel
- miniaudio
- nlohmann-json
- libmpv SDK
- Dear ImGui sources in `third_party/imgui`

The project expects the mpv SDK here by default:

```text
C:/packages/mpv/include/mpv/client.h
C:/packages/mpv/lib/mpv.lib
C:/packages/mpv/bin/libmpv-2.dll
```

Configure:

```powershell
cmake -S . -B cmake-build-release `
  -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg-master/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DMPV_ROOT=C:/packages/mpv
```

Build:

```powershell
cmake --build cmake-build-release --target SyncPlay
```

Output:

```text
cmake-build-release/SyncPlay.exe
```

## mpv SDK Note

This repository includes an mpv dev package under `third_party/mpv-dev-*`.

If your SDK only has `libmpv-2.dll` and headers but not `mpv.lib`, generate an MSVC import library from the DLL exports or install an mpv package that already provides `mpv.lib`.

## Diagnostics

File logging is disabled by default.

Enable it from:

```text
Settings > Diagnostics > Enable file logging
```

Log output:

```text
%APPDATA%/SyncPlay/syncplay.log
```

Use this for connection, relay, voice, and file-transfer debugging.

## Known Limitations

- Windows only.
- No media streaming. Users need their own local media file.
- Current transport is host relay server, not WebRTC P2P.
- The host must be reachable from guests.
- Voice is one-to-one.
- File sharing uses the relay path and is not intended as a high-speed file-transfer replacement.
- No packaged installer/release flow is documented yet.

## Roadmap Ideas

- Packaged releases.
- In-app firewall/help diagnostics.
- LAN host discovery.
- Better file-transfer history.
- Optional P2P/WebRTC transport for high-bandwidth features.
- Screen sharing.
- More polished onboarding.

## Project Layout

```text
assets/       app icon and runtime image assets
core/         shared helpers
media/        mpv helpers
network/      signaling relay client/server and relay voice
platform/     Win32, D3D11, window chrome, input
render/       software render upload path
scripts/      utility scripts
src/          main application loop
sync/         session and playback synchronization
third_party/  vendored dependencies
ui/           app state and UI helpers
```

## License

No license file is included yet. Add a license before publishing if you want others to use, modify, or redistribute the project.
