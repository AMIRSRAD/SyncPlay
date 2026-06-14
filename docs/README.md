# SyncPlay — Developer Understanding

This folder documents how SyncPlay actually works, reconstructed by reading the
entire source tree. It reflects the **shipped code**, which in places differs
from `SyncPlayDoc.txt` (see [06-findings-and-issues.md](06-findings-and-issues.md)).

> **One-line summary:** A Windows-only "watch-party" media player. Everyone plays
> their *own local copy* of a file (no media streaming); a host-authoritative
> session keeps playback (play/pause/seek/speed), chat, file sharing, and optional
> 1:1 voice in sync over a single plain-`ws://` relay the host runs.

## Contents

| Doc | What it covers |
|---|---|
| [01-architecture-overview.md](01-architecture-overview.md) | What it is, the relay model, host/guest roles, build, dependencies, project layout |
| [02-media-render-pipeline.md](02-media-render-pipeline.md) | libmpv software-render path, the threading model, D3D11 texture upload, the playback adapter |
| [03-subsystem-map.md](03-subsystem-map.md) | Module-by-module reference: every directory, its key files and symbols |
| [04-sync-algorithm.md](04-sync-algorithm.md) | `SyncState`, the drift-correction tiers, broadcast cadence, control intents, file verification |
| [05-signaling-protocol.md](05-signaling-protocol.md) | The JSON-over-WebSocket wire protocol: message types, fields, authorization, codes |
| [06-findings-and-issues.md](06-findings-and-issues.md) | Design-vs-reality drift, security issues, bugs, and performance notes |

## At a glance

- **Stack:** C++20, Win32 + Direct3D 11 (+ Direct2D/DirectWrite for text), Dear ImGui (vendored), libmpv (software render), libdatachannel (used *only* as a WebSocket impl), nlohmann-json, miniaudio.
- **Size:** ~9,300 lines across 41 source files; ~4,300 of them in `src/main.cpp`.
- **Transport:** host-run embedded WebSocket relay, default port **49152**, no TLS.
- **Model:** effectively two-peer (host + 1 guest); voice and sync bookkeeping assume exactly one guest.
- **Platform:** Windows only — everything is gated behind `if(WIN32)` and uses Win32/D3D/DWM/WIC APIs.
