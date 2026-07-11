# 02 — Media & Render Pipeline

## The pipeline

```
File → libmpv (demux + decode) → MPV_RENDER_API_TYPE_SW
     → CPU BGRA buffer (double-buffered) → D3D11 dynamic texture → ImGui background draw list
```

libmpv does all demuxing/decoding/AV-sync (local files **and** network URLs — its
ffmpeg backend speaks `http(s)`, HLS, DASH, etc.). Instead of letting mpv own a
window, SyncPlay uses mpv's **software render API**: mpv draws each frame into a
CPU-side BGRA bitmap that the app uploads to a Direct3D 11 texture and composites
under the ImGui UI. Configured in `src/main.cpp` at init:
- `vo=libmpv`, `vid=auto`, `keep-open=yes`, `cache=yes`, render type `MPV_RENDER_API_TYPE_SW`.
- `hwdec=auto-copy-safe` — decode on the GPU (D3D11VA/DXVA2) and copy the frame
  back to the SW buffer. Plain `auto-safe` would pick *native* zero-copy modes the
  SW render API can't consume, silently falling back to CPU decoding.
- `watch-later-dir` / `save-position-on-quit=yes` — resume support (position only).
- `http-proxy` — set when the system-proxy toggle is on (see
  [05-signaling-protocol.md](05-signaling-protocol.md) for the app-wide proxy).

## Threading model

Four actors touch video state:

1. **Main / UI thread** — runs `wWinMain`'s `while (running)` loop
   (`src/main.cpp` ~1566–4512). Per iteration:
   1. `PeekMessage` pump → `WndProc` (sets `g_pending*` globals, `g_dropPath`).
   2. Honor `g_requestExit` / `WM_QUIT`.
   3. `session.tick()` — pumps networking/sync/voice and drains the task queue.
   4. Apply pending fullscreen / play-toggle / dropped-file requests.
   5. `mpv_wait_event(0)` drain — on `MPV_EVENT_FILE_LOADED` reset scrub state,
      refresh `localFile` from mpv's `path`, reapply subtitle style.
   6. Poll mpv props (pause/duration/time-pos/volume/speed/playlist).
   7. Push `localFile` size/duration/hash to the session for peer verification.
   8. Upload the SW frame via `update_video_texture` (only when `frameCounter` changed).
   9. Build the ImGui frame; `Render` + `Present(1,0)`.

2. **Render thread** (`render/render_sw.cpp`, `render_thread`) — waits on a
   condition variable with a 16 ms timeout (≈60 fps), (re)allocates the back buffer
   with a 64-byte-aligned stride, calls `mpv_render_context_render` with SW params
   (`bgr0` format), then under the mutex swaps `frontIndex`, sets `hasFrame`, and
   increments `frameCounter`.

3. **libmpv internal thread** — invokes `mpv_render_update` (registered as mpv's
   update callback) when a new frame is ready; it sets `frameRequested` and notifies
   the render thread's condvar.

4. **miniaudio realtime thread** — voice capture/playback (see
   [03-subsystem-map.md](03-subsystem-map.md) → `network/relay_voice`).

All networking + sync + UI logic is otherwise **single-threaded** on the main loop.

## Double-buffering (`SwRenderState`)

Defined in `render/render_sw.h`. Shared blackboard between mpv, the render thread,
and the UI thread:

- `mpv_render_context* ctx`
- `buffers[2]`, `widths[2]`, `heights[2]`, `strides[2]`, `frontIndex` — the
  producer fills the back buffer, then publishes it as front under the mutex.
- atomics: `hasFrame`, `running`, `frameRequested`, `frameCounter`
- `mutex` + `condition_variable cv` — the wakeup protocol.

`ensure_sw_buffer(state, w, h)` (called on resize) updates `targetW/H`, sets
`sizeDirty`, and requests a frame so the render thread reallocates and re-renders.

## D3D texture upload

`update_video_texture` (`render/render_sw.cpp`) runs on the UI thread. It **holds
the state mutex across the whole map/copy** (fixing an earlier use-after-free where
a concurrent resize could reallocate the front buffer mid-copy), maps `g_videoTex`
with `D3D11_MAP_WRITE_DISCARD`, and copies each row while forcing opaque alpha in a
**single 32-bit pass** (`d[x] = s[x] | 0xFF000000u`, since mpv writes `bgr0` with an
undefined X byte). It silently drops the frame if the SW buffer size ≠
`g_videoTexW/H` (resize window until `EnsureVideoTexture` recreates it).

Two things ride on the same upload, so they update at frame cadence regardless of
the glass/accent toggles:
- **`produce_blur`** — a cheap box-average downsample (~240 px wide) into
  `g_blurTex`, used as the frosted-glass backdrop behind panels and the title bar.
  Skipped when the glass toggle is off (`g_glassEnabled`).
- **Dynamic-accent sampling** — a sparse, saturation-weighted average of the frame
  (`g_videoAccentColor`), which the UI loop normalises (HSV clamp) and eases into
  the theme accent so the interface takes on the video's palette.

The texture itself (`g_videoTex` + SRV `g_videoSrv`) is a `DYNAMIC`,
`CPU_ACCESS_WRITE`, `B8G8R8A8_UNORM` shader resource, created/resized by
`EnsureVideoTexture` in `platform/platform.cpp`.

## The playback adapter

`core/playback_controller.h` defines an abstract `PlaybackController`
(`loadFile/play/pause/seek/setSpeed/isPlaying/currentTime/duration/speed`).
`MpvPlaybackController` in `src/main.cpp` (~line 641) implements it over an
`mpv_handle*` using `mpv_command` / `mpv_get_property` / `mpv_set_property`.

This decouples the **sync layer from mpv**: `SyncSession` and `SyncManager` only
ever talk to the abstract interface, so the synchronization logic has no direct
dependency on libmpv.

## mpv helpers

`media/mpv_helpers.{h,cpp}` provide typed convenience accessors with fallbacks:
- `mpv_get_double / mpv_get_flag / mpv_set_flag / mpv_get_int64`
- `mpv_read_tracks(handle, typeFilter)` → `vector<TrackInfo>` (parses the
  `track-list` node tree: id/type/title/lang/selected).
- `mpv_read_playlist(handle)` → `vector<PlaylistItem>` (filename/title/current).

## Video color / subtitle pass-through

`src/main.cpp` (~1447–1529) mirrors `AppState` values into mpv properties:
- `applyVideoColor` → brightness/contrast/saturation/gamma/hue
- `applyToneMapping` → tone-mapping mode table
  (`auto/clip/linear/gamma/reinhard/hable/mobius/bt.2390`) + param + target-peak
- `applyVideoShaders` → `glsl-shaders` change-list (clear + append)
- `applySubtitleStyle` → font/size/color/opacity/outline/shadow/spacing/position/margins
