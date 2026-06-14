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

## `media/` + `render/`

- `media/mpv_helpers.{h,cpp}` — typed mpv property accessors + track-list/playlist
  node parsers (`TrackInfo`, `PlaylistItem`). See
  [02-media-render-pipeline.md](02-media-render-pipeline.md).
- `render/render_sw.{h,cpp}` — `SwRenderState`, the render thread, and the D3D
  texture upload.

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
  callbacks (state/file/chat/intent/peer-update/voice-frame/share-*).
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
- **`file_dialog.{h,cpp}`** — `openFileDialog` using the modern COM
  `IFileOpenDialog`, centered on the owner via an `IFileDialogEvents` callback;
  parses a double-null-terminated filter into `COMDLG_FILTERSPEC`.

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
  `deque<ChatLine>` chat log, a `deque<EventToast>`). Plus `load_config`/`save_config`
  (JSON at `%APPDATA%/SyncPlay/config.json`), `computePartialHashHexUtf8` (SHA-1 of
  first 2 MB), and `format_time`.
  - `ChatLine` carries `who/text/time/kind/status/fileName/fileSize/fileTransferred/transferId/retryPath`.
  - `ChatLineKind { Text, System, File }`, `ChatLineStatus { None, Sending, Receiving, Sent, Failed, Received }`.
  - `EventToast { text, ttl }`.
- **`panel_utils.{h,cpp}`** — custom floating/dockable panel system: `PanelDragState`,
  `UpdatePanelDrag` (edge/corner hit-testing + clamp; `resizeAxis` bitmask
  1=right,2=bottom,4=left,3=corner), `BeginPanel`/`BeginPanelNoScroll`/`EndPanel`
  (shadowed ImGui child wrappers), `DockResizeHandle`.
- **`ui_helpers.{h,cpp}`** — `IconButton`/`IconButtonFont`/`IconToggle`,
  `DrawIconGlow` (4-direction accent glow), `ShowDelayedTooltip` (hover-dwell),
  `Utf8FromCodepoint`, and a glow-offset stack.
