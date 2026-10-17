# Streaming and control protocol

Version 1. Three parties:

- **App**: Explicitly Desktop (C++/JUCE) on the Windows PC.
- **Extension**: the "Explicitly Video Sync" browser extension on the same PC (background
  service worker, plus `remote.js` in the host tab that plays the phone's video).
- **Phone**: the PWA in `MobileClient/`, served by the app.

```
Phone ──HTTP (LAN :8767, HMAC-signed)──► App ◄──HTTP/WS (127.0.0.1:8766, extension origin)── Extension
  ▲                                                                                              │
  └──────────────── WebRTC: H.264 video + Opus audio + DataChannel "control" (DTLS) ─────────────┘
```

All binary integers are little-endian. "b64u" is base64url without padding. "B32" is RFC 4648
base32 (A–Z, 2–7) without padding. Times are milliseconds unless named `…Sec`.

## 1. Cryptography

- `HMAC(key, msg)` = HMAC-SHA256, output as lowercase hex (64 chars).
- `SHA256hex(msg)` = lowercase hex.
- Strings are UTF-8. A `|` joins fields; no field may contain `|` (ids are b64u/hex/uuid).
- The phone page runs on plain `http://<lan-ip>` (not a secure context), so `crypto.subtle`
  is unavailable there: the PWA uses its own SHA-256/HMAC (`MobileClient/crypto.js`).
  `crypto.getRandomValues` is available and used for nonces and ids.

## 2. Pairing (phone ↔ app, LAN)

1. The user turns on **Allow phone streaming** and presses **Pair a phone**. The app creates a
   pairing secret `P` = 16 random bytes, valid for 120 s and usable once. It shows:
   - a QR code for `http://<lan-ip>:8767/#pair=<B32(P)>&host=<hostId>`
   - the same code as text, `B32(P)` in groups of 4 (`ABCD-EFGH-…`, 26 chars), for typing.
   The fragment is never sent over the network.
2. The phone opens that URL (the phone camera's QR reader does this) and reads `pair`.
   It creates `deviceId` (uuid v4, kept in `localStorage`), `name` (editable, default from the
   user agent), and `nonce` (16 random bytes, hex), then sends:

   `POST /api/pair`  `{"v":1,"deviceId","name","nonce","mac"}`
   with `mac = HMAC(P, "pair|" + deviceId + "|" + name + "|" + nonce)`.

3. The app checks `mac` against the live `P` (expired, used or wrong → `403
   {"error":"pairing code expired or wrong"}`). It then asks on Windows: *Allow "<name>" to
   play videos and control Explicitly Desktop?* The HTTP request stays open for up to 120 s.
   - Deny or timeout → `403 {"error":"denied"}`. `P` is used up either way.
   - Allow → the app creates the device key `K` = 32 random bytes, stores the device (§6) and replies
     `200 {"v":1,"hostId","hostName","deviceId","wrapped","mac"}` where
     - `wrapped = hex(K XOR pad)`, `pad = bytes(HMAC(P, "wrap|" + nonce))` (32 bytes),
     - `mac = HMAC(P, "pair-ok|" + deviceId + "|" + wrapped)`.
4. The phone checks `mac`, unwraps `K`, and stores `{hostId, hostName, origin, deviceId, K}` in
   `localStorage`. `P` is then discarded.

Someone on the LAN can see the messages but not `P` (it only exists in the QR/typed code), so
they can't learn `K` or forge requests.

## 3. Signed requests (phone → app, LAN)

Every request after pairing is `POST` with a JSON body and these headers:

```
X-Device: <deviceId>
X-Time:   <unix ms>
X-Nonce:  <16 random bytes hex>
X-Mac:    HMAC(K, method + "|" + path + "|" + deviceId + "|" + time + "|" + nonce + "|" + SHA256hex(rawBody))
```

The app rejects: an unknown or revoked device (401), `|X-Time − now| > 120 s` (401), a nonce
already seen in the last 5 minutes (401), or a bad MAC (401). Each response carries
`X-Mac: HMAC(K, "resp|" + path + "|" + nonce + "|" + SHA256hex(rawResponseBody))`, and the phone
rejects a response whose MAC is wrong (this protects the SDP answer and its DTLS
fingerprint).

| Path | Body | Response |
|---|---|---|
| `/api/hello` | `{}` | `{"hostName","appVersion","filter":{status}}` (used to check pairing still works) |
| `/api/session/offer` | `{"sdp": "<offer>"}` | `{"sessionId","sdp":"<answer>"}`, or `409 {"error":"host browser not available"}`, `504` on timeout (15 s) |
| `/api/session/hangup` | `{"sessionId"}` | `{}` |
| `/api/unpair` | `{}` | `{}` (removes this device) |

Static files (`GET /`, `/index.html`, `/app.js`, `/crypto.js`, `/style.css`, `/manifest.json`,
`/icon-192.png`, `/icon-512.png`, `/sw.js`) are public and not signed. Anything else returns 404.
The LAN port serves nothing privileged without a valid MAC.

## 4. WebRTC session

- The **phone makes the offer**: `recvonly` video and audio transceivers, in that order, plus a
  DataChannel `"control"` (ordered). `iceServers: []` (LAN only). ICE is not trickled: the phone
  waits for gathering to finish (or 2 s) before sending, and the answer is complete too.
- The **extension answers** in the host tab, attaching one video track and one audio track that
  share one `MediaStream` (so the phone keeps them lip-synced). Codec preference is H.264
  (then VP8) for video and Opus 48 kHz stereo for audio (`stereo=1; maxaveragebitrate=128000`).
- A new offer from the same device replaces its old session. Only one session runs at a time;
  an offer from a second device ends the first one with a `{"t":"ended","reason":"replaced"}`
  message on its DataChannel.

### 4.1 DataChannel `"control"` messages (JSON, one per message, field `t` = type)

Phone → PC:

| t | Fields | Meaning |
|---|---|---|
| `load` | `url` | Open this URL in the host tab. Only `https://www.youtube.com/watch?v=…`, `https://m.youtube.com/watch?v=…`, `https://youtu.be/…` and `https://www.youtube.com/shorts/…` are accepted in v1, otherwise `error` `"unsupported source"`. |
| `play` / `pause` | — | |
| `seek` | `time` (seconds) | |
| `volume` | `value` 0–1 | Sets the PC player's volume (it is what gets filtered and streamed). |
| `filter` | `on` (bool) | Filter Explicit Speech on/off (same as the app's toggle). |
| `search` | `q`, `id` | Search YouTube on the PC; answered with `results`. |
| `ping` | `id` | Answered with `pong` (round trip shown as connection quality). |

PC → phone:

| t | Fields |
|---|---|
| `state` | `url`, `title`, `channel`, `paused`, `time`, `duration`, `volume`, `buffering` (bool), `mode` (`"delayed"`, `"caption"`, `"live"`), `filter` (`{running, enabled, delayMs}`), `ad` (bool). Sent on every change and at least every 1 s. `time` is the position the phone is *showing* (player time minus the delay while delayed). |
| `results` | `id`, `items`: `[{url, title, channel, duration, thumb}]` (max 20) |
| `error` | `code` (`"unsupported source"`, `"protected"`, `"load failed"`, `"app offline"`), `message` |
| `pong` | `id` |
| `ended` | `reason` (`"replaced"`, `"revoked"`, `"host closed"`) |

Ads play as they come (they are never skipped, hidden or altered); `state.ad` only lets the
phone label them.

## 5. Loopback API (extension ↔ app, `127.0.0.1:8766`)

As today, requests with an `Origin` starting with `http` get 403, so only the extension and
local tools can use these. New endpoints:

| Method, path | Body | Response |
|---|---|---|
| `GET /remote/next` | — | Long poll, up to 25 s. `{"type":"offer","sessionId","deviceId","deviceName","sdp"}`, `{"type":"hangup","sessionId","reason"}`, or `{"type":"none"}` |
| `POST /remote/answer` | `{"sessionId","sdp"}` or `{"sessionId","error"}` | `{}` |
| `POST /remote/state` | `{"active":bool,"sessionId","deviceName"}` | `{}`. Active → the app shows "Phone connected" and (unless **Also play on this PC** is on) silences its speakers *after* the PCM tap. Inactive → speakers back on. |
| `POST /flush` | — | Status. Drops the audio waiting in the delay buffer and refills it (heard as `initialDelaySeconds` of silence), used after a seek. |
| `POST /hold/on`, `POST /hold/off` | — | Status. Hold freezes the delay pipeline (no write, no read, silent output) so pause/resume keeps audio and video aligned with no gap and no replay. |
| `GET /pcm` (WebSocket upgrade) | — | Binary frames of filtered audio, below. One client; a new one replaces the old. |

`/status` gains `"held"`, `"localMuted"`, `"phone"` (`{"active","deviceName"}`), and
`"sampleRate"`.

### 5.1 PCM frames on `/pcm`

Each WebSocket binary message is one frame of the app's final output (after muting/Reverse,
before the speaker mute):

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | `"EXPC"` |
| 4 | `uint32` | `sampleRate` (device rate, usually 48000) |
| 8 | `uint16` | `channels` (2) |
| 10 | `uint16` | `flags` (bit0: discontinuity before this frame, bit1: filter active, bit2: held) |
| 12 | `uint64` | `startSample`: index of the first sample frame since the stream started |
| 20 | `uint32` | `frames` (samples per channel in this message; normally 20 ms) |
| 24 | `uint32` | `delaySamples`: current input-to-output delay |
| 28 | `int16[frames*channels]` | interleaved PCM |

Header size 28 bytes. The app sends while a client is connected and doesn't buffer more than
1 s for a slow client (older audio is dropped and the next frame has the discontinuity flag).
The app never writes PCM to disk.

## 6. Paired devices (app)

`%APPDATA%\ExplicitlyDesktop\devices.json`:
`{"hostId","devices":[{"deviceId","name","key":"<hex K>","paired":"ISO date","lastSeen":"ISO date"}]}`.
`K` has to stay on the PC to verify MACs. It is protected with Windows DPAPI
(`CryptProtectData`, current user) and stored as `"keyProtected":"<b64>"`. **Revoke** removes
the entry and ends that device's session (`ended`, `"revoked"`).

## 7. Timing and sync

- Video is delayed by `delayMs + outputLatencyMs` before it reaches the WebRTC video track.
  `delayMs` comes from `/status` (the app's real input-to-output gap). `outputLatencyMs` is the
  extension's measured `/pcm` path (relay plus its audio prebuffer) plus `cableLatencyMs`
  (default 30, the VB-CABLE input buffer).
- Mode `caption` (captioned YouTube, app bypassed) and `live` (filter off) send video undelayed.
- Seek: the extension clears its frame queue and calls `/flush`. Both sides refill, and the phone
  shows "Buffering" for about 3.5 s.
- Pause and resume: `/hold/on` and `/hold/off` plus freezing the frame queue's clock.
- The phone shows `state.time` as its position.
