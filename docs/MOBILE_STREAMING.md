# Mobile streaming: architecture change map

Goal: pick a video on a phone (Android or iOS). The Windows PC plays it and filters it
through Explicitly Desktop. The phone receives the video and the censored audio together,
in sync, and can control playback. Desktop filtering keeps working when this feature is off.

This is the plan from reading the current source. Each section lists what exists, what
changes, and what is new.

## 1. What exists today (and is kept)

| Piece | Where | Role in streaming |
|---|---|---|
| VB-CABLE routing | `AppRouter.*`, `Scripts/Route-AppsToFilter.ps1` | Unchanged. The browser that plays the phone's video is a routed app, so its audio is already filtered. |
| Filter and delay | `AudioEngine.cpp`, `audioDeviceIOCallbackWithContext` | Unchanged logic. Mutes and Reverse are written into `delayBuffer` before playback. **The filtered PCM is `outputChannelData` at the end of the callback.** That is the tap point. |
| Whisper, lyrics, GPU | `WhisperThread`, `LyricsGuide`, `LyricsAlignment` | Untouched. The remote session sees exactly what the speakers would play. |
| Filter toggle and bypass lease | `AudioEngine::setFilterEnabled`, `setBypassLease` | Reused for the phone's filter switch and for caption mode. |
| Control server | `ControlServer.*`, `127.0.0.1:8766` | Stays loopback only. It gains the endpoints that the PC browser extension needs (below). |
| Video delay | `BrowserExtension/content.js` | Already draws the YouTube picture on a canvas, delayed by the app's `delayMs`. **That canvas is the synced video source**, so the PC never has to match audio to video after the fact. |
| Caption mode | `BrowserExtension/content.js`, `yt_main.js` | Unchanged. In caption mode the app runs undelayed (bypass lease), the video is not delayed, and both stay in sync. |
| `WebSocketServer.*` | not in `CMakeLists.txt` | Dead code. It isn't used or extended; the new loopback socket is part of the new streaming module. |

## 2. Design choice: WebRTC runs in Chrome, not in the C++ app

Linking libwebrtc into the JUCE app would add a dependency of several hundred MB, a second
build system and new real-time risks. The PC already runs Chrome with our extension on the
playing tab, and Chrome ships WebRTC with H.264 and Opus, hardware encoders and DTLS-SRTP.
So the extension sends the media, and the C++ app supplies the filtered audio, pairing and
signaling.

```
 Phone (PWA)                         Windows PC
 ───────────                         ──────────────────────────────────────────────────────
 search / URL / controls ──DataChannel──► extension (host tab, routed to VB-CABLE)
                                            │   video.play / pause / seek / volume
                                            │
 <video> ◄═══ WebRTC (DTLS-SRTP) ═══════════┤  video track: delayed canvas.captureStream()
         H.264 + Opus, one MediaStream      │  audio track: filtered PCM ─► AudioWorklet
                                            │                ▲
                                            │   ws://127.0.0.1:8766/pcm (loopback only)
                                            │                │
                                  Explicitly Desktop  ─ PcmTap (lock-free ring, after filtering)
                                  StreamHost (LAN :8767): PWA files, pairing, signed signaling
```

Both tracks are already aligned when they leave the PC: the picture is delayed by the same
`delayMs` that the audio is. Because they share one `MediaStream`, the phone's WebRTC receiver
keeps them lip-synced with RTP/RTCP timestamps through jitter.

## 3. C++ changes (Windows host)

New files:

- `Source/PcmTap.h/.cpp`: a single-producer, single-consumer ring (reuses `LockFreeQueue.h`
  ideas) filled at the end of the audio callback with the final output samples (float, stereo,
  device rate). No locks or allocations on the audio thread. A writer thread reads it, packs
  20 ms frames as int16 with a sample counter, and serves them to the extension. When no
  client is connected, the tap does nothing.
- `Source/StreamHost.h/.cpp`: the LAN-facing server, off by default (setting "Allow phone
  streaming"). It serves:
  - the PWA files (`MobileClient/`, copied next to the exe like `Models/`);
  - `POST /pair`: the pairing handshake, with a Windows approval dialog;
  - `POST /signal`: relays SDP offer/answer and ICE between the phone and the extension.
    Every message carries an HMAC-SHA256 with the device key, so nobody on the LAN can
    replace the DTLS fingerprints (no man in the middle).
  - Nothing else: filter, debug and lexicon endpoints stay on `127.0.0.1:8766`.
- `Source/PairedDevices.h/.cpp`: device list in `%APPDATA%\ExplicitlyDesktop\devices.json`
  (id, name, key hash, last seen), with revoke from the UI.

Changed:

- `AudioEngine`: `setPcmTap(PcmTap*)`, plus one `push` call after the output loop.
  `flushDelay()`, called on remote seek, so the 3.5 s of old audio isn't played after a seek.
  `setLocalOutputMuted(bool)`: while a phone is watching, the speakers get silence after the
  tap ("Also play on this PC" can turn this off). This prevents double playback and echo.
- `ControlServer`: `GET /pcm` (WebSocket upgrade, loopback, extension origin only),
  `POST /flush`, and a long-poll `GET /remote/next` that hands signaling and commands to the
  extension. These all keep the existing rule that web-page origins get 403.
- `MainComponent`: a "Phone" section with the on/off switch, the QR code, the paired device
  list with Revoke, and connection status.
- `CMakeLists.txt`: the new sources, `bcrypt.lib` for HMAC/random, and copying `MobileClient/`.

## 4. Extension changes (PC side)

- New `remote.js` (only on the host tab): opens a dedicated window with the requested URL,
  builds the `RTCPeerConnection`, adds the delayed canvas track (or the live
  `video.captureStream()` track when the filter is off or caption mode is active), and plays
  `/pcm` through an `AudioWorklet` into a `MediaStreamAudioDestinationNode` for the audio track.
- It carries out phone commands on the `<video>` (play, pause, seek, volume, rate). On seek it
  calls `POST /flush` and clears the canvas frame queue, so both sides restart together.
- Remote browse: it loads YouTube search results in the host tab and returns titles, channels,
  durations and thumbnails from the page (no API key and no downloads).
- DRM check: if `video.mediaKeys` is set, or `captureStream` gives no frames, it tells the
  phone "This source is protected and can't be streamed" and sends nothing.
- `manifest.json`: adds `remote.js` and keeps `host_permissions` to YouTube and the
  loopback port.

## 5. Phone client (PWA)

`MobileClient/` holds plain HTML, CSS and JS with no build step, and works in Safari on iOS
16.4+ and Chrome on Android:

- Pair screen: scan the QR code with the camera. It opens `http://<pc-ip>:8767/#k=<key>`.
  The key is in the URL fragment, so it never crosses the network. A typed code is the
  fallback.
- Browse: a search box (results come from the PC), a URL box, and a share target on Android.
- Player: the remote `<video>`, play/pause, seek bar, volume, fullscreen, filter on/off
  (`/filter/*` via the PC), connection, buffering and "filtering active" badges.
- Recovery: automatic ICE restart, then reconnect with the stored key; it shows "Reconnecting…".

## 6. Pairing and security

1. In the app, the user turns on "Allow phone streaming". The app shows a QR code with its LAN
   address and a fresh 128-bit pairing secret, valid for 2 minutes.
2. The phone scans it, opens the PWA and sends `/pair` with a device name and proof of the
   secret.
3. Windows shows "Allow *Tim's phone* to control Explicitly Desktop?". Only **Allow** stores the
   device; it gets a long-term key, kept in the phone's storage and as a hash on the PC.
4. Every later signaling message is HMAC-signed. Media and control then run over WebRTC
   (DTLS-SRTP and the DataChannel), so all of it is encrypted.
5. Revoke deletes the key and closes any live session.

The LAN port only does pairing, signaling and static files. Everything privileged stays on
`127.0.0.1`.

**Discovery:** browsers can't do mDNS or broadcast, so the QR code carries the address and the
PWA remembers it. If the PC's IP changes, scanning again fixes it. Optional mDNS can be added
for native wrappers later.

**Plain HTTP on the LAN:** a page served from the PC over HTTP isn't installable as a PWA on
Android (it's still a home-screen bookmark on iOS). Media and commands are encrypted anyway
(WebRTC), and signaling is signed. A self-signed HTTPS option can come later.

## 7. Sync and errors

- Delay: about 3.5 s while filtering (`initialDelaySeconds`), as now. The canvas uses the
  app's live `delayMs` plus the measured `/pcm` transport delay (ring depth plus worklet
  buffer), so the two tracks leave the PC aligned to within one audio frame.
- Seek, pause, resume, filter toggle: flush the delay buffer and the canvas queue, then
  restart both. The phone shows "Buffering" during the 3.5 s refill.
- Jitter and lost frames: these are handled by WebRTC's jitter buffer and A/V sync. The PWA
  shows a weak-connection badge when packet loss goes above 5%.
- Source fails, phone disconnects, PC sleeps: the host tab reports state on the DataChannel;
  on disconnect the PC pauses playback and unmutes its speakers.
- DRM or not allowed: the session is refused with a clear message, never worked around.
- Memory: PCM and frames live only in RAM rings. Nothing is recorded unless the existing
  `EXPLICITLY_DEBUG_RECORD` is set.

## 8. Tests

- C++ (new `ExplicitlyTests` target, no audio device needed): PcmTap ring (overrun, frame
  counter continuity), HMAC signing and verification, device store and revoke, flush.
- JS (Node): signaling message signing, the command protocol, and the delay compensation
  math.
- End-to-end sync (Playwright, Chromium): a test page plays an authorized clip with a flash
  and a beep every second through a fake PCM source. The receiver page measures the
  flash-to-beep offset over WebRTC, which must stay within ±45 ms, and checks the controls.

## 9. License (needs the owner's decision before anything is published)

`LICENSE` allows personal, non-commercial use only. Redistributing modified versions,
commercial use and derivative works for distribution are prohibited without permission from
Andrew Mahran. Building and using this feature on your own PC and phone is fine. Publishing
binaries, an app-store app or a paid product built on this code needs a license from the
copyright holder. Third-party parts: JUCE 8 (AGPLv3 or commercial), whisper.cpp (MIT),
VB-CABLE (donationware; redistribution needs VB-Audio's permission). Content: only
non-DRM sources the user is allowed to watch, with YouTube ads left untouched.

## 10. Order of work

1. This map.
2. PcmTap, the `/pcm` socket, and a phone page that plays filtered audio.
3. Synced video and WebRTC A/V from the extension.
4. Remote browse, URL selection and controls.
5. QR pairing, approval, revoke and reconnect.
6. Tests, tuning and docs (build and setup steps added to `SETUP_WINDOWS.md`).
