# 06 — Findings & Issues

Observations from reading the full tree. Each item is tagged with how strongly it's
confirmed:

- **[verified]** — confirmed by directly reading the relevant code in this review.
- **[observed]** — reported from a careful read; plausible and specific, but treat as
  "investigate / confirm" before acting.

---

## Design-vs-reality drift

### The design doc describes a product that wasn't built — [verified]
`SyncPlayDoc.txt` describes a WebRTC P2P mode with offer/answer signaling, STUN/TURN,
DTLS-encrypted data channels, and source files `network/p2p_peer.{h,cpp}` +
`network/voice_peer.{h,cpp}`. **None of those files exist.** The shipped network layer
is `signaling_client` + `signaling_server` + `relay_voice`, and the only libdatachannel
types used are `rtc::WebSocket` / `rtc::WebSocketServer` (confirmed by grep — no
`PeerConnection`/`DataChannel`). So libdatachannel is just a WebSocket library here.

**Implication:** the doc's security claims ("encrypted P2P channel", "no metadata on
signaling server") do **not** describe the running app. `README.md` is accurate (it
explicitly says the default path uses no STUN/TURN/WebRTC P2P); `SyncPlayDoc.txt` is
aspirational. Anyone using the doc as a spec will be misled.

---

## Security

### 1. Path traversal / arbitrary file write in file sharing — [verified] · HIGH · ✅ FIXED
> **Fixed (this session):** added `sanitizeShareFileName` in `sync/sync_session.cpp`
> (reduces the remote name to a bare filename, strips separators/drive/`..`, and
> replaces illegal characters); `resolveSharePath` now uses it. The companion
> `FILE|` chat open-vector is also closed (see item 1b).

`SyncSession::resolveSharePath` (`sync/sync_session.cpp:1028`) builds the download path
as `downloadsDir() / Trim(name)` where `name` is supplied by the remote peer
(`share_meta`). Only `Trim()` is applied.

- In C++ `std::filesystem`, `base / rhs` **replaces** `base` entirely when `rhs` is an
  absolute path (e.g. `C:\Windows\...`).
- `..\` components in a relative name escape the Downloads folder.

A malicious peer in a session can therefore write the `.part` file and the final file
**anywhere the user can write**. The chunk validation (in-order, size caps, SHA-1) is
otherwise solid, but the *destination* is unchecked.

**Fix sketch:** reduce to a bare filename (`std::filesystem::path(name).filename()`),
reject empty/`.`/`..`/separators/drive specifiers, and verify the resolved
`weakly_canonical` path stays under `weakly_canonical(downloadsDir())`.

### 1b. Untrusted path open via `FILE|` chat message — [verified] · HIGH · ✅ FIXED
A peer can send a relay `chat` message of the form `FILE|sender|name|path|size`; the
receiver's handler in `src/main.cpp` stored the peer-supplied `path` into
`ChatLine.filePath`, and the "Folder" button / file-card double-click passes it to
`ShellExecuteW("explorer.exe", <path>)` — i.e. a remote peer (plus one victim click)
could open an arbitrary path, UNC share, or executable.
> **Fixed (this session):** the `FILE|` handler no longer reads or stores the
> peer-supplied path. The legitimate local download path is delivered only by the
> share-progress callback, which writes inside the (now-sanitized) Downloads folder.

### 2. No transport encryption / weak auth — [verified] · trusted-LAN only
Plain `ws://`, no TLS. The only access control is knowing the 6-char session code
(32⁶ ≈ 1.07e9, no rate limiting). Chat, files, and voice travel in cleartext. Fine for
a trusted LAN; not safe over an untrusted network. (See
[05-signaling-protocol.md](05-signaling-protocol.md).)

### 3. Session password stored in plaintext — [observed]
`sessionPassword` is held in `AppState` and written verbatim to
`%APPDATA%/SyncPlay/config.json` (`ui/app_state.cpp:136`). The file is readable by the
user's profile; no obfuscation/encryption.

---

## Bugs

### 4. volume & speed are saved but never restored — [verified] · MEDIUM · ✅ FIXED
> **Fixed (this session):** `load_config` now takes optional `float* volume/speed`
> out-params and reads them back; `wWinMain` restores them onto mpv right after
> `load_config` so they survive restarts.

`save_config` writes `j["volume"]` and `j["speed"]` (`ui/app_state.cpp:192-193`), but
`load_config` never reads them back, and `src/main.cpp` (which calls `load_config` at
line 1053) only ever reads volume/speed *from mpv at runtime*. Net effect: a user's
volume and playback speed silently reset to defaults on every launch.

Several panel dock-geometry fields show the same asymmetry (written by `save_config`,
not read by `load_config`) — [observed].

### 5. Front-buffer use-after-free / tearing on resize — [verified] · MEDIUM · ✅ FIXED
`update_video_texture` (`render/render_sw.cpp`) snapshotted the front buffer pointer
under the lock but did the `memcpy` *after* releasing it. The render thread can swap
`frontIndex` and reallocate that (now back) buffer during a concurrent resize, so the
UI thread could read freed/torn memory.
> **Fixed (this session):** the lock is now held across the entire `Map`/copy/`Unmap`,
> so the render thread cannot swap or reallocate mid-copy. (`WRITE_DISCARD` keeps the
> map non-blocking, so the render thread only ever waits the brief copy duration.)

### 6. Pause has no heartbeat — [verified] · LOW/MEDIUM
The host only broadcasts `state` *while playing* (`SyncSession::tick()` guards on
`isPlaying()`). A pause is sent once at the moment it happens; if that single packet is
lost, a guest keeps playing with no re-send. (See
[04-sync-algorithm.md](04-sync-algorithm.md).)

### 7. "Preferred interface" bind is non-functional — [observed] · LOW
`SignalingServer::start` always sets `bindAddress = "0.0.0.0"` and never honors the
preferred interface for binding — it only affects the advertised `serverUrl()`.

### 8. Dead / vestigial code — [observed] · ✅ MOSTLY FIXED
- ✅ `SyncManager::update()` — removed (was captured-and-discarded / no-op; verified
  no callers anywhere).
- ✅ `Session::pendingForHost` / `pendingForGuests` (`signaling_server`) — fields and
  the dead clear/flush block removed (nothing ever populated them).
- ✅ `example.html` (stock IANA "Example Domain" page) — deleted.
- Guest `sendStateNow()` early-returns (`!m_hostingSession`), making the guest
  state-verification branch unreachable — *left as-is* (harmless guard, removing it
  risks changing the host/guest gating).

---

## Performance

### 9. Voice is raw PCM over base64 JSON — [verified]
s16/48 kHz/mono, base64'd in `relay_voice_frame` (~768 kbps + ~33% base64 inflation),
no Opus/codec, no jitter buffer beyond a 64-frame drop cap. Very heavy compared to a
real WebRTC/Opus path.

### 10. Realtime audio callback takes a mutex — [observed]
`RelayVoice::audioCallback` runs on the miniaudio realtime thread and locks
`std::mutex`es (`m_incomingMutex`/`m_outgoingMutex`) — risks priority inversion /
glitches under contention. Common but technically unsafe for hard-realtime audio.

### 11. Per-pixel alpha fill every frame — [observed] · ✅ IMPROVED
`update_video_texture` rewrote each pixel's alpha to `0xFF` in a *second* pass over the
destination after the row `memcpy` (mpv writes `bgr0`).
> **Improved (this session):** copy + opaque-alpha are now a single 32-bit pass
> (`d[x] = s[x] | 0xFF000000u`), halving destination memory traffic. (Eliminating the
> write entirely would require an opaque blend/texture-format change in the ImGui draw
> path — left as-is.)

---

## Minor / latent — [observed]

- ✅ **FIXED** — `openFileDialog` `COMDLG_FILTERSPEC` dangling-pointer risk: the parser
  now builds the whole `owned` string vector first, then takes `c_str()` pointers in a
  second pass, so a vector reallocation can't dangle a captured pointer.
- ✅ **FIXED** — Settings → Connection tab no longer sets `app.dirty = true` every
  frame; the individual controls already mark dirty on change, so autosave churn is
  gone.
- ✅ **FIXED** — `computePartialHashHexUtf8` now opens via a wide path
  (`WideFromUtf8(path).c_str()`) on Windows, so non-ASCII media paths hash correctly
  (they previously returned an empty hash, breaking file verification for those files).
- ✅ **FIXED** (quality) — `BeginPanel` / `BeginPanelNoScroll` deduplicated behind a
  shared `BeginPanelImpl` in `ui/panel_utils.cpp`.
- `RenderChatTextTexture` / chat texture caches hold D3D SRVs; release paths *appear*
  balanced but any early-return that skips a release would leak. *Left as-is.*
- `dpiScale` is computed once at startup; fonts are baked at that scale and not
  rebaked when the window moves to a different-DPI monitor. *Needs a build to do
  safely (see below).*
- Several session setters are called every frame a panel is visible rather than on
  change (idempotent, but wasteful).

---

## Suggested priorities

✅ Done so far: **#1 path traversal**, **#1b `FILE|` open-vector**,
**#4 volume/speed persistence**, **#5 render use-after-free**, **#11 alpha-fill pass**,
**#8 dead code** (`SyncManager::update`, server pending-vectors, `example.html`),
**autosave churn**, **non-ASCII partial-hash**, **file-dialog dangling filter pointers**,
**`BeginPanel` dedup**.

### Deferred — need a compiler in the loop (too risky to do blind)
- **Extract the panels** out of `main.cpp` into `ui/panels_*.cpp`. Big maintainability
  win but a large mechanical refactor of tightly-coupled by-reference lambdas; must be
  compiled/run to verify nothing detached.
- **Runtime DPI re-scale** on `WM_DPICHANGED` — requires rebuilding the ImGui font
  atlas (`ImGui_ImplDX11_InvalidateDeviceObjects` + re-bake) mid-loop; needs build +
  multi-monitor testing.

### Remaining (design decisions, not bugs)
- **#9/#10 voice** — Opus + a lock-free audio queue, if voice quality/CPU matters.
- **#3 plaintext `sessionPassword`** in `config.json` — omit or obfuscate (currently a
  deliberate LAN-convenience choice).
- **#7 preferred-interface bind** — binding `0.0.0.0` works; honoring the preferred
  interface for the *bind* (not just the advertised URL) is a behavior change.
- **Reconcile `SyncPlayDoc.txt`** with reality (it describes an unbuilt WebRTC P2P design).
