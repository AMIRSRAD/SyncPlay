# 05 — Signaling / Relay Protocol

## Transport

- **WebSocket only**, over plain `ws://` (no `wss://`/TLS). Implemented with
  libdatachannel's `rtc::WebSocket` (client) and `rtc::WebSocketServer` (host).
  No `rtc::PeerConnection` / `rtc::DataChannel` anywhere — libdatachannel is used
  *purely* as a WebSocket library.
- Every message is a **JSON text frame** with a `"type"` field and a `"code"`
  field (the server stamps/overwrites `code` when relaying). Binary payloads
  (voice PCM, file chunks) are **base64-encoded inside the JSON**.
- Default port **49152**. The server always binds `0.0.0.0` (the "preferred
  interface" setting only affects the advertised URL, not the bind — see
  [06-findings-and-issues.md](06-findings-and-issues.md)).

## Roles & sessions

- A `Session` is a room keyed by one or more **codes** (aliases share one Session),
  holding one **host** socket and N **guest** sockets, plus a `mode` (always coerced
  to `"relay"`).
- **Codes** are 6 uppercase chars from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (omits
  `0/O/1/I`), generated with a `thread_local mt19937` in `generateCode()`.
- A guest joins by sending `join` with the code; the first peer claiming `role:"host"`
  on a code becomes the authority. If a later real host joins an existing code, the
  server closes the previous host socket.

## Message types

| `type` | Direction | Fields (besides `type`,`code`) | Meaning |
|---|---|---|---|
| `join` | client → server | `role`, `mode` | Join/identify on a session |
| `state` | host → guests | `timestamp`, `playing`, `speed`, `seq` | Authoritative playback state |
| `file` | host → guests | `size`, `duration`, `hash` | Local-file identity for verification |
| `chat` | any → others | `text` | Chat message |
| `intent` | guest → host | `action`, `value` | Control request (play/pause/seek/seek_delta/speed) |
| `share_meta` | sender → others | `id`, `name`, `size`, `totalChunks`, `sender` | Start of a file transfer |
| `share_chunk` | sender → others | `id`, `index`, `data` (base64) | One file chunk |
| `share_done` | sender → others | `id`, `sha1?` | End of transfer (+ optional checksum) |
| `relay_voice_start` | peer → peer | — | Begin voice |
| `relay_voice_stop` | peer → peer | — | End voice |
| `relay_voice_frame` | peer → peer | `data` (base64 PCM) | One audio frame |
| `peer_update` | server → clients | `host` (bool), `guests` (int) | Presence update |

> The internal field is `timestamp` on the wire; conceptually it is the playback
> position in seconds (see `SyncState.time` in [04-sync-algorithm.md](04-sync-algorithm.md)).

## Server authorization rules (`onTextMessage`)

- Only **guests** may send `intent`.
- Only the **host** may send `file`.
- `state` / `file` / `chat` are rejected outside `relay` mode (the only mode).
- `relay_voice_*` requires **≤ 1 guest** (point-to-point voice) and routes
  host ↔ the single guest.
- Fan-out: `state`/`file` host → all guests; `chat`/`share_*` host → all guests and
  guest → host + other guests.

## Client robustness

- All libdatachannel callbacks (`onOpen/onClosed/onError/onMessage`) are pushed onto
  a `core/TaskQueue` and processed on the owning (UI) thread via `pumpEvents()`,
  so message handling and session-map mutation are single-threaded.
- A monotonic **`m_generation`** counter is captured per connection; callbacks from a
  superseded socket (after reconnect) are dropped.
- Outbound messages are buffered (`queueOrSend`/`flushPending`) until the socket is
  connected **and** joined **and** has a code.
- `onTextMessage` parses with `nlohmann::json::parse(..., false)` and dispatches by
  `"type"`, base64-decoding voice/share payloads.

## Voice frames

Raw **s16 / 48 kHz / mono PCM**, base64-encoded into `relay_voice_frame` — **not**
Opus or any codec, and **not** SRTP. This is bandwidth-heavy (~768 kbps before
base64's ~33% inflation). No jitter buffer beyond a 64-frame hard cap in both
directions (overflow silently drops frames). See `relay_voice` in
[03-subsystem-map.md](03-subsystem-map.md).

## File share

`share_meta` → N × `share_chunk` → `share_done`. The receiver enforces:
in-order chunks (`index == expectedChunk`), `receivedChunks < totalChunks`,
`received + size <= declared size`, and the 2 GB cap. SHA-1 integrity is checked
when `share_done` carries a `sha1` (optional — treated as OK when empty). The output
path is chosen by `resolveSharePath` (Downloads + de-dup).

> ⚠️ `resolveSharePath` does **not** sanitize the remote-supplied name → path
> traversal / arbitrary write. See [06-findings-and-issues.md](06-findings-and-issues.md).

## Security model (as implemented)

- No TLS, no authentication beyond knowing the 6-char code (code space 32⁶ ≈ 1.07e9,
  no rate limiting).
- Anyone who can reach the port and present a valid code joins. The host must expose
  an inbound TCP port (firewall rule).
- Treat it as **trusted-LAN-only**. The design doc's "encrypted channel" / "no
  metadata on server" claims do not hold for this relay path.
