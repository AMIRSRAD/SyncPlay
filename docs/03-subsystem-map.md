# 03 — Subsystem Map

Module-by-module reference. Sizes are approximate.

## `src/main.cpp` (~4,300 lines)

The entry point and the entire per-frame loop. It is mostly straight-line ImGui
immediate-mode code plus in-place lambdas, not a set of standalone functions.

Notable contents:
- **`wWinMain`** (~line 691) — all init: DPI awareness, COM, icon loading
  (PNG→BGRA via WIC, rounded-corner mask, HICON), window class + borderless window
  (`WS_POPUP|WS_THICKFRAME`), custom chrome, D3D device, ImGui context/style/fonts,
  mpv handle + SW render context + render thread, `load_config`, the
  `MpvPlaybackController` + `SyncSession`, and the main loop.
- **`MpvPlaybackController`** (~641) — the `PlaybackController` over `mpv_handle*`.
- **Chat / timeline helpers** — `ChatTimestamp`, `FormatBytes`, `ChatStatusLabel`,
  `TrimChatHistory` (caps chat at 200), `AppendSystemTimelineChip` (deduped system
  events), and label generators (`TransportTimelineLabel`, `FileTimelineLabel`,
  `VoiceTimelineLabel`).
- **DirectWrite text rendering** — `RenderChatTextTexture` (~260) rasterizes
  complex-script/emoji chat text and the chat input to BGRA textures via D2D/DWrite/WIC
  with word-wrap, font fallback, and caret hit-testing; cached in `chatTextCache`
  (cap 400). `NeedsComplexText` decides ASCII fast-path vs the DWrite path.
- **Icon/image helpers** — `LoadPngBGRA`, `CreateTextureFromBGRA`,
  `CreateIconFromBGRA`, `LoadIconFromFile`, `ApplyRoundedMask`.
- **Shell integration** — `RegisterOpenWith` (~610) writes the per-user ProgId +
  `OpenWithProgids` for video extensions, then `SHChangeNotify`.
- **Loop body, part 2** (~2150–4512) — the bottom control/timeline bar, all panels
  (Session / Call / Chat+Files / Subs / Settings), global keyboard shortcuts, the
  right-click video context menu, toasts, Render+Present, debounced autosave (≥2s),
  and the careful shutdown order.

UI control flow uses `requestOnly()` (~1362) to decide whether a control mutates
mpv locally (+ `notifyLocalAction`) or sends a guest **intent**.

## `core/` — header-only utilities (+ one .cpp)

| File | Purpose |
|---|---|
| `playback_clock.h` | Monotonic position estimator on `steady_clock` (offset + running-anchor + speed); `syncTo()` aligns to a peer. Not thread-safe. |
| `playback_controller.h` | Abstract media-control interface (decouples sync from mpv). |
| `task_queue.h` | Mutex-guarded FIFO of `std::function<void()>`; `drain()` swaps out under lock and runs callbacks outside it. Used to marshal network callbacks to the UI thread. |
| `logging.h` | Thread-safe logger → `%APPDATA%/SyncPlay/syncplay.log` + `OutputDebugStringA`, gated by an atomic enable flag; RAII `LogLine` (`LogInfo/Warn/Debug`). |
| `sha1.h` | Self-contained SHA-1 + hex helpers. Used for the RFC 6455 WebSocket handshake and file-transfer integrity. |
| `base64.h` | RFC 4648 encode/decode. Used for the WS handshake and for binary payloads (voice frames, file chunks). |
| `string_utils.h` | `Trim`, `ToLower`, `StartsWith`, `EndsWith` (ASCII). |
| `utf.{h,cpp}` | `Utf8FromWide` / `WideFromUtf8` via Win32 (Windows-only). |
| `crash_dump.{h,cpp}` | `InstallCrashHandler` — `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` → `%APPDATA%/SyncPlay/crash-*.dmp` (static buffers only, no CRT/alloc in the handler). |
| `update_check.{h,cpp}` | Background WinHTTP call to the GitHub Releases API at startup; parses `tag_name`, compares to `SYNCPLAY_VERSION`, exposes the newer version to the UI (toast + About). Silent on any failure. Honours the app proxy. |
| `net_proxy.{h,cpp}` | `DetectSystemProxy` (reads the Windows/IE proxy via `WinHttpGetIEProxyConfigForCurrentUser`, picks the https/http entry) + `Set/GetAppProxy` (the proxy the app should use; "" = direct). The WinHTTP helpers read it per request. |

## `media/` + `render/`

- `media/mpv_helpers.{h,cpp}` — typed mpv property accessors + track-list/playlist
  node parsers (`TrackInfo`, `PlaylistItem`). See
  [02-media-render-pipeline.md](02-media-render-pipeline.md).
- `media/opensubtitles.{h,cpp}` — async OpenSubtitles REST client (WinHTTP). Computes
  the OpenSubtitles **moviehash** (filesize + 64 KiB head/tail sum), searches by
  hash + filename, downloads an SRT to a temp folder for `sub-add`. All work is on
  background threads with a generation guard; the UI polls `OsGetSnapshot()`.
- `render/render_sw.{h,cpp}` — `SwRenderState`, the render thread, the D3D texture
  upload, `produce_blur` (frosted-glass backdrop) and the dynamic-accent frame
  sample. See [02-media-render-pipeline.md](02-media-render-pipeline.md).

## `network/`

- **`signaling_server.{h,cpp}`** (~580 lines) — `rtc::WebSocketServer` host. Enumerates
  NICs via Windows IP Helper (`GetAdaptersAddresses`, prefers RFC1918 private IPv4,
  skips loopback/link-local), groups sockets into `Session`s by code, routes/relays
  messages with role+mode authorization, broadcasts `peer_update`, and generates
  6-char codes (`generateCode`) from an ambiguity-free alphabet.
  - `Session { host; vector<guests>; codes (incl aliases); mode (always "relay") }`
  - `PeerInfo { code; role; socket }`
  - `NetworkInterface { name; address }`
- **`signaling_client.{h,cpp}`** (~480 lines) — per-peer `rtc::WebSocket` wrapper.
  Normalizes the URL, wires `onOpen/onClosed/onError/onMessage` (all marshalled to a
  `TaskQueue` and guarded by a `m_generation` counter so stale-socket callbacks are
  dropped), JSON-encodes all outbound `send*` methods, and buffers outbound messages
  until joined (`queueOrSend`/`flushPending`). Inbound messages dispatch to typed
  callbacks (state/file/chat/**reaction**/**open_url**/intent/peer-update/voice-frame/share-*).
  - **Latency/RTT:** `sendPing`/`pong` round-trips maintain a smoothed `rttSeconds()`;
    `state` carries a `lat` (host one-way latency) field. See [04-sync-algorithm.md](04-sync-algorithm.md).
  - **Auto-reconnect:** an unexpected drop mid-session schedules a rejoin with
    exponential backoff (1 s → 10 s) and re-issues the last join; deliberate
    `disconnect()` clears the arm flag.
  - **Proxy:** `setProxy(url)` routes the WebSocket through an HTTP proxy; loopback
    targets always connect directly so self-hosting still works.
- **`relay_voice.{h,cpp}`** (~320 lines) — `RelayVoice` duplex audio over miniaudio
  (`MINIAUDIO_IMPLEMENTATION` defined here; s16/48kHz/mono). A realtime
  `audioCallback` enqueues captured PCM (cap 64 frames) and drains incoming frames
  to the output with gain; `tick()` (10 ms cadence) hands captured frames to the
  frame callback (→ `sendRelayVoiceFrame`); `pushIncomingFrame` enqueues received
  PCM. Mic is **muted by default**. `captureDevices()` enumerates inputs.

## `platform/`

- **`platform.{h,cpp}`** — owns the D3D11 globals (`g_pd3dDevice`,
  `g_pd3dDeviceContext`, `g_pSwapChain`, `g_mainRenderTargetView`, `g_videoTex`,
  `g_videoSrv`, `g_videoTexW/H`), the window handle `g_hWnd`, fullscreen save/restore
  state, and the `g_pending*` command flags. Implements `WndProc` (forwards to ImGui
  first, then custom non-client handling: `WM_NCCALCSIZE`→0 removes the frame,
  `WM_NCHITTEST` re-implements resize borders + title-bar buttons via
  `g_titleBarHitTest`, `WM_DROPFILES` reads the first dropped file, resize
  coalescing during interactive sizing), `CreateDeviceD3D`/`CreateRenderTarget`/
  `EnsureVideoTexture`, `ToggleFullscreen` (fake/borderless), and
  `ApplyCustomWindowChrome` (rounded corners + no border via `DwmSetWindowAttribute`).
  Also: **`WM_DPICHANGED`** rebakes the ImGui font atlas at the new scale, multi-file
  **`WM_DROPFILES`** fills `g_dropPaths`, and **taskbar integration** — `ITaskbarList3`
  progress on the icon, a prev/play-pause/next thumbnail toolbar (glyph icons rendered
  from Segoe MDL2), and `WM_APPCOMMAND` hardware media keys (`UpdateTaskbar` /
  `CleanupTaskbar`, driven via `g_pendingPlaylistPrev/Next`).
- **`file_dialog.{h,cpp}`** — `openFileDialog` / `openFileDialogMulti` (multi-select
  via `FOS_ALLOWMULTISELECT`) using the modern COM `IFileOpenDialog`, centered on the
  owner via an `IFileDialogEvents` callback; parses a double-null-terminated filter
  into `COMDLG_FILTERSPEC`.

The platform layer is decoupled from the rest via globals + one-shot `g_pending*`
flags that the main loop polls and clears each frame.

## `sync/`

The heart of the app — see [04-sync-algorithm.md](04-sync-algorithm.md).

- `sync_state.h` — `struct SyncState { double time; bool playing; double speed; uint64_t seq; }`.
- `sync_manager.{h,cpp}` — `SyncManager`: the drift-correction state machine
  (`applyState`) + `Role { Host, Guest }`.
- `sync_session.{h,cpp}` (~1,160 lines) — `SyncSession`: owns the `SyncManager`,
  `SignalingServer`, `SignalingClient`, and `RelayVoice`; handles lifecycle,
  broadcast cadence, control intents, file verification, chat, voice gating, and
  chunked file transfer.

## `ui/`

- **`app_state.{h,cpp}`** — the `AppState` "god struct" (panel visibility/dock flags,
  fixed-size `char[]` input buffers, subtitle/voice/video params, panel geometry, a
  `deque<ChatLine>` chat log, a `deque<EventToast>`, a `vector<RecentMedia>`
  continue-watching list, and setting flags: `glassPanels`, `dynamicAccent`,
  `useSystemProxy`, OpenSubtitles key/langs). Plus `load_config`/`save_config`
  (JSON at `%APPDATA%/SyncPlay/config.json`), `computePartialHashHexUtf8` (SHA-1 of
  first 2 MB), and `format_time`.
  - `ChatLine` carries who/text/time/kind/status/file fields + runtime-only
    `appearAt` (entrance animation).
  - `EventToast { text, ttl, addedAt }`; `RecentMedia { path, position, duration, lastWatched }`.
- **`panels.{h,cpp}`** — the right-side panels (Settings / Session / Call / Chat+Files
  / Subs) extracted out of `main.cpp` into standalone `Draw*Panel(PanelContext&)`
  functions. `PanelContext` is a per-frame bundle of references + callbacks the main
  loop passes in (fonts, geometry, `applyAccent`, `openUrl`, chat/reaction extras, …).
- **`chat_text.h`** — declarations for the shared DirectWrite chat/emoji texture
  helpers (`RenderChatTextTexture` with an emoji `cropTight` option, caret hit-test,
  `NeedsComplexText`); definitions live in `main.cpp`.
- **`panel_utils.{h,cpp}`** — custom floating/dockable panel system: `PanelDragState`,
  `UpdatePanelDrag` (edge/corner hit-testing + clamp; `resizeAxis` bitmask
  1=right,2=bottom,4=left,3=corner), `BeginPanel`/`BeginPanelNoScroll`/`EndPanel`
  (frosted-glass child wrappers with the slide-in appear animation + `padScale`),
  `DockResizeHandle`.
- **`ui_helpers.{h,cpp}`** — `IconButton`/`IconButtonFont`/`IconToggle` (with eased
  press-scale), `DrawIconGlow` (4-direction accent glow), `StyledTooltip` /
  `ShowDelayedTooltip` (themed hover-dwell), `Utf8FromCodepoint`, glow-offset stack.
