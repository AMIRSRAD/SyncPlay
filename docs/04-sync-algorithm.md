# 04 — Synchronization Algorithm

Two layers:
- **`SyncManager`** (`sync/sync_manager.cpp`) — the pure drift-correction math.
- **`SyncSession`** (`sync/sync_session.cpp`) — orchestration: transport, roles,
  broadcast cadence, intents, file verification, chat, voice, file share.

## The wire payload

```cpp
struct SyncState {
    double   time;     // playback position in seconds
    bool     playing;  // play/pause
    double   speed;    // playback rate multiplier
    uint64_t seq;      // monotonic sequence number (ordering + dedup)
};
```

`SyncManager::captureState()` snapshots `{currentTime, isPlaying, speed, 0}` — it
always leaves `seq = 0`; the real sequence number is assigned by
`SyncSession::sendStateNow()` via `++m_localStateSeq`.

## Host broadcast path

`SyncSession::tick()` is called every frame from the main loop. It pumps the
server/client/voice and drives the file-share sender, then — **only if** hosting
**and** `transportConnected` **and** the local player `isPlaying()` — calls
`sendStateNow()` every `m_sendInterval = 1500 ms`.

`sendStateNow()` captures local state, stamps `seq`, and (gated on
`m_localFile.valid && guestCount > 0`) sends `sendRelayState(time, playing, speed, seq)`.

The host **also** sends immediately on any local control action
(`notifyLocalAction` → `sendStateNow`) and right after a guest joins, so changes
propagate without waiting for the 1500 ms timer.

> ⚠️ The host only broadcasts **while playing**. A pause is propagated by the single
> immediate send at the moment of pausing — there is no pause heartbeat/re-send, so a
> dropped pause packet can leave a guest still playing.

## Guest apply path

`onRelayState(time, playing, speed, seq)`:
1. Drop if `seq <= m_lastRemoteSeq` (dedup / ordering — no gap detection).
2. Record `m_lastRemoteTime` and `m_lastRemoteAt = steady_clock::now()`.
3. Update remote playing/speed bookkeeping (fires Play/Pause/Speed UI actions on change).
4. Decide `allowImmediate` (true only within the 1500 ms post-seek window).
5. Call `m_sync.applyState(state, allowImmediate)`.

## Drift correction (`SyncManager::applyState`)

Constants: `tinyDrift = 0.05s`, `moderateDrift = 0.25s`, `hardSeek = 1.0s`,
`maxRateAdjust = 0.03` (±3%).

First reconcile play/pause to match `s.playing`. Then `delta = s.time − localTime`
and pick **one** branch:

| Condition | Action |
|---|---|
| `allowImmediateSeek && \|delta\| > 0.05` | **seek** to `s.time`, reset correction, `setSpeed(s.speed)` |
| remote **paused** && `\|delta\| > 0.05` | **seek**, reset, `setSpeed` |
| `\|delta\| >= 1.0` (hardSeek) | **hard seek**, reset, `setSpeed` |
| `\|delta\| <= 0.05` (in sync) | reset correction, `setSpeed(s.speed)` |
| otherwise (`0.05 < \|delta\| < 1.0`) | **soft rate nudge** (below) |

**Soft rate nudge:**
```
normalized      = clamp(delta / 0.25, -1, 1)
target          = normalized * 0.03                 // max ±3%
m_speedCorrection = 0.6*m_speedCorrection + 0.4*target   // EMA, dead-zoned < 0.001
setSpeed(s.speed + m_speedCorrection)
```
Positive `delta` (remote ahead) → speed local playback up to catch up, converging,
then snap back to exact speed once within `tinyDrift`. `m_speedCorrection` is reset
to 0 in every non-soft branch, so it never runs away. Recovering a 0.25 s gap at full
±3% takes on the order of ~8 s.

## Guest control intents

Guest UI actions don't touch its own mpv. `requestPlay/Pause/Seek/SeekDelta/Speed`
→ `sendControlIntent` → `sendRelayIntent(action, value)`. Seeks first arm
`m_allowImmediateSeekUntil = now + 1500 ms` so the host's echoed state is hard-applied.

Host `onRelayIntent` → `applyControlIntent` (gated on `hosting && m_allowGuestControl
&& m_localFile.valid`): executes play/pause/seek/seek_delta/speed, dispatches a UI
action, then `sendStateNow()` to rebroadcast the authoritative result. Unknown
actions are ignored. `m_allowGuestControl` lets the host lock out guest control.

## Clock handling — important caveat

There is **no clock-offset / NTP-style synchronization and no latency compensation**
in the protocol. `SyncState.time` is the raw playback position at send time, so
transmission latency (plus up to the 1500 ms send interval) becomes drift directly.
The soft rate loop is the only mechanism that closes that gap, and only for drift
between 0.05 s and 1.0 s.

The only time extrapolation is display-only: `syncDriftSeconds()` advances
`m_lastRemoteTime` by `(now − m_lastRemoteAt) * remoteSpeed` while the remote is
playing, to estimate live drift for `syncConfidenceText()`:

| Estimated drift | Label |
|---|---|
| < 0.12 s | Synced |
| < 0.75 s | Drifting |
| otherwise | Resyncing |

This estimate drives the UI chip only — `applyState` uses the raw `s.time` at receive.

## File-identity verification

Before state flows, peers exchange `FileInfo { size, duration, hash }`. The host
sends via `sendRelayFile` when a guest is present; the guest verifies in
`updateFileVerification`: size match **and** non-empty hash match **and** duration
within 0.5 s. The hash is `computePartialHashHexUtf8` — SHA-1 of the **first 2 MB**.
Host requires only `m_localFile.valid` to broadcast.

## Two-peer assumptions

The model assumes exactly host + 1 guest: a single `m_lastRemoteSeq`, a single
remote file, and `voiceAvailable()` requires `m_relayHostOnline && m_relayGuestCount == 1`.
The relay server can hold multiple guests, but the sync/voice bookkeeping is built
for one.

## Dead / vestigial code

- `SyncManager::update()` only `captureState()`s and discards it (Host) or does
  nothing (Guest). Real broadcasting is in `SyncSession::tick()`/`sendStateNow()`.
- Guest-originated state is unreachable: `sendStateNow()` early-returns when
  `!m_hostingSession`.

## Tuning constants (in `sync_session.h`)

| Field | Value | Meaning |
|---|---|---|
| `m_sendInterval` | 1500 ms | host state broadcast period (while playing) |
| `m_allowImmediateSeekWindow` | 1500 ms | guest snap-seek window after a local seek |
| `m_shareChunkSize` | 128 KB | file-share chunk size |
| `m_shareMaxBytes` | 2 GB | max shared-file size |
| `m_shareSendInterval` | 2 ms | file-share send pacing |
