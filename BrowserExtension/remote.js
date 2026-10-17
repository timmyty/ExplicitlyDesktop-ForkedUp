// Phone streaming, PC side (docs/STREAMING_PROTOCOL.md §4). Runs in every
// YouTube tab but stays idle unless the background says this is the host tab,
// the one it opened for the phone. There it answers the phone's WebRTC offer
// with one MediaStream:
//
// - video: the page's <video>, captured and delayed by the app's delay plus the
//   audio relay's latency, so it leaves this tab in step with the audio;
// - audio: the app's filtered output (PCM over /pcm, relayed by the
//   background), played through an AudioWorklet into the stream only. Nothing
//   is played on this PC from here.
//
// The phone's commands arrive on the "control" DataChannel. Ads play as they
// come; state.ad only lets the phone label them.

(() => {
  const api = globalThis.browser ?? globalThis.chrome;
  const core = globalThis.ExplicitlyRemoteCore;
  const JitterRing = globalThis.ExplicitlyJitterRing;
  if (window.top !== window || !core || !api?.runtime?.connect) return;
  if (typeof RTCPeerConnection !== "function" || typeof AudioContext !== "function") return;   // Can't host here

  const shared = (globalThis.__explicitlySync ??= {});   // Also used by content.js
  const TARGET_MS = 60;           // Audio prebuffer
  const MAX_FRAMES = 1200;        // Same cap as content.js
  const PROTECTED_MS = 3000;      // Playing with no captured frames this long -> protected
  const RECOVER_MS = 12000;       // Time the phone gets to restart ICE before we give up

  let port = null;
  let status = { running: false, enabled: false, buffering: false, delayMs: 0, offline: true };
  let pc = null, dc = null, session = null;
  let media = null;               // Tracks, built on the first offer and kept
  let navigating = false;
  let failTimer = null;
  let timer = null;
  let lastTick = 0, lastStatus = 0, lastWatch = 0, lastState = 0, lastResume = 0;
  let lastStateJson = "";
  let appOfflineSent = false;

  const audio = { ctx: null, node: null, ring: null, track: null, fillAvg: TARGET_MS, relayMs: 1, frameMs: 20, extraMs: 3, decodeMs: 0 };
  const vid = {
    track: null, writer: null, canvas: null, cctx: null,
    el: null, src: null, gen: 0, capturing: false, lastCapture: 0,
    clock: new core.QueueClock(), queue: null, smoother: new core.DelaySmoother(), delay: 0,
    lastFrameAt: 0, playingSince: 0, protected: false, held: false, seekAnchor: null, lastTs: 0,
  };
  vid.queue = new core.FrameQueue({ max: MAX_FRAMES, release: (f) => f.bmp.close?.() });

  const findVideo = () => document.querySelector("#movie_player video.html5-main-video") || document.querySelector("#movie_player video");
  const currentVideoId = () => { try { return new URL(location.href).searchParams.get("v"); } catch { return null; } };
  const isAd = () => !!document.querySelector("#movie_player.ad-showing");
  const mode = () => core.streamMode(status, !!shared.captionOn?.());

  // ---- Host tab handshake ------------------------------------------------------
  function hello() {
    api.runtime.sendMessage({ type: "remoteHello" }).then((r) => { if (r?.host) becomeHost(); }).catch(() => {});
  }

  api.runtime.onMessage.addListener((msg) => {
    if (msg?.type === "remoteHost") becomeHost();
  });

  function becomeHost() {
    if (port) return;
    port = api.runtime.connect({ name: "explicitly-remote" });
    port.onMessage.addListener(onPortMessage);
    port.onDisconnect.addListener(() => {
      port = null;
      for (const resolve of pending.values()) resolve(null);
      pending.clear();
      // The service worker restarted: connect again and pick the relay back up
      setTimeout(hello, 1000);
    });
    if (session) port.postMessage({ type: "session", active: true, sessionId: session.sessionId, deviceName: session.deviceName });
    if (!timer) timer = setInterval(tick, 16);   // Backup clock; the worklet's is the main one
    tick();
  }

  function onPortMessage(msg) {
    if (msg.type === "pcm") onPcm(msg);
    else if (msg.type === "apiResult") { pending.get(msg.id)?.(msg.result); pending.delete(msg.id); }
    else if (msg.type === "offer") {
      handleOffer(msg)
        .catch((e) => ({ error: "answer failed: " + (e?.message || e) }))
        .then((r) => port?.postMessage({ type: "reply", reqId: msg.reqId, ...r }));
    } else if (msg.type === "hangup") {
      if (session && (!msg.sessionId || msg.sessionId === session.sessionId)) endSession(null, { pause: true });
    }
  }

  // Loopback calls go through the background (see background.js)
  const pending = new Map();
  let nextApi = 1;
  function apiCall(path, body) {
    return new Promise((resolve) => {
      if (!port) return resolve(null);
      const id = nextApi++;
      pending.set(id, resolve);
      port.postMessage({ type: "api", id, path, body });
      setTimeout(() => { if (pending.delete(id)) resolve(null); }, 5000);
    });
  }

  // ---- Session -----------------------------------------------------------------
  async function handleOffer(msg) {
    const fingerprint = core.sdpFingerprint(msg.sdp);
    const offer = { type: "offer", sdp: core.mungeOpus(msg.sdp) };   // Our encoder: stereo, 128 kbps

    // Same phone connection (same DTLS fingerprint): an ICE restart
    if (pc && session && session.deviceId === msg.deviceId && session.fingerprint === fingerprint &&
        pc.signalingState === "stable" && pc.connectionState !== "closed") {
      await pc.setRemoteDescription(offer);
      const answer = await pc.createAnswer();
      await pc.setLocalDescription({ type: "answer", sdp: core.mungeOpus(answer.sdp) });
      await gathered(pc, 2000);
      session.sessionId = msg.sessionId;
      return { sdp: pc.localDescription.sdp };
    }

    await ensureMedia();
    const peer = new RTCPeerConnection({ iceServers: [] });
    peer.ondatachannel = (e) => {
      if (e.channel.label !== "control") return;
      const ch = e.channel;
      if (peer === pc) dc = ch;
      ch.onopen = () => { if (ch === dc) sendState(true); };
      ch.onmessage = (ev) => { if (ch === dc) onControl(ev.data); };
      ch.onclose = () => { if (ch === dc && !navigating) endSession(null, { pause: true }); };
    };
    peer.onconnectionstatechange = () => {
      if (peer !== pc) return;
      const s = peer.connectionState;
      if (s === "connected") { clearTimeout(failTimer); failTimer = null; }
      else if ((s === "failed" || s === "disconnected") && !failTimer) {
        failTimer = setTimeout(() => {
          failTimer = null;
          if (peer === pc && peer.connectionState !== "connected") endSession(null, { pause: true });
        }, RECOVER_MS);
      }
    };

    await peer.setRemoteDescription(offer);
    // The offer's recvonly transceivers (video, then audio) take our tracks
    const vs = peer.addTrack(media.video, media.stream);
    const as = peer.addTrack(media.audio, media.stream);
    for (const t of peer.getTransceivers()) {
      const kind = t.receiver.track?.kind;
      const caps = kind && (RTCRtpReceiver.getCapabilities?.(kind) || RTCRtpSender.getCapabilities?.(kind));
      if (caps?.codecs && t.setCodecPreferences) {
        try { t.setCodecPreferences(core.orderCodecs(caps.codecs, kind)); } catch {}
      }
    }
    const answer = await peer.createAnswer();
    await peer.setLocalDescription({ type: "answer", sdp: core.mungeOpus(answer.sdp) });
    maxBitrate(as, 128000);
    maxBitrate(vs, 6000000);
    await gathered(peer, 2000);

    // A new phone ends the old one's session; the same phone just reconnected
    if (session) endSession(session.deviceId === msg.deviceId ? null : "replaced", { pause: false, replacing: true });
    pc = peer;
    session = { sessionId: msg.sessionId, deviceId: msg.deviceId, deviceName: msg.deviceName, fingerprint };
    shared.remoteHost = true;   // content.js: no overlay here, the phone gets the delayed picture
    appOfflineSent = false;
    apiCall("/remote/state", { active: true, sessionId: session.sessionId, deviceName: session.deviceName });
    port?.postMessage({ type: "session", active: true, sessionId: session.sessionId, deviceName: session.deviceName });
    resumeAudio(true);
    return { sdp: peer.localDescription.sdp };
  }

  function gathered(peer, ms) {
    if (peer.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve) => {
      const done = () => { clearTimeout(t); peer.removeEventListener("icegatheringstatechange", check); resolve(); };
      const check = () => { if (peer.iceGatheringState === "complete") done(); };
      const t = setTimeout(done, ms);
      peer.addEventListener("icegatheringstatechange", check);
    });
  }

  function maxBitrate(sender, bps) {
    try {
      const p = sender.getParameters();
      if (!p.encodings?.length) return;
      p.encodings[0].maxBitrate = bps;
      sender.setParameters(p).catch(() => {});
    } catch {}
  }

  function endSession(reason, { pause = true, replacing = false } = {}) {
    if (!session) return;
    const old = pc, oldDc = dc, s = session;
    if (reason && oldDc?.readyState === "open") { try { oldDc.send(JSON.stringify({ t: "ended", reason })); } catch {} }
    pc = null; dc = null; session = null;
    clearTimeout(failTimer); failTimer = null;
    if (oldDc) oldDc.onclose = null;
    setTimeout(() => old?.close(), reason ? 300 : 0);   // Let "ended" go out first
    if (replacing) return;

    if (pause) findVideo()?.pause();
    // Back to plain desktop filtering: the app plays out its buffer, the PC
    // window's overlay comes back
    if (vid.held) setHold(false);
    shared.remoteHost = false;
    apiCall("/remote/state", { active: false, sessionId: s.sessionId });
    port?.postMessage({ type: "session", active: false });
  }

  function send(m) {
    if (dc?.readyState !== "open") return;
    try { dc.send(JSON.stringify(m)); } catch {}
  }

  // ---- Tracks --------------------------------------------------------------------
  async function ensureMedia() {
    if (media) return media;
    const audioTrack = await createAudio();
    const videoTrack = createVideo();
    const stream = new MediaStream([videoTrack, audioTrack]);
    media = { stream, video: videoTrack, audio: audioTrack };
    return media;
  }

  async function createAudio() {
    const rate = status.sampleRate || 48000;
    const ctx = new AudioContext({ sampleRate: rate, latencyHint: "interactive" });
    const dest = ctx.createMediaStreamDestination();
    dest.channelCount = 2;
    let node = null;
    if (ctx.audioWorklet && typeof AudioWorkletNode === "function") {
      try {
        await ctx.audioWorklet.addModule(api.runtime.getURL("pcm-worklet.js"));
        node = new AudioWorkletNode(ctx, "explicitly-pcm", {
          numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
          processorOptions: { targetMs: TARGET_MS, sampleRate: rate },
        });
        node.port.onmessage = (e) => { audio.stats = e.data; audio.fillAvg += (e.data.fillMs - audio.fillAvg) * 0.02; tick(); };
        audio.extraMs = 128 / rate * 1000;
      } catch (e) {
        console.debug("[Explicitly] AudioWorklet unavailable, using ScriptProcessor:", e?.message || e);
        node = null;
      }
    }
    if (!node) {
      // The page's CSP can refuse the worklet module: same ring on this thread
      const ring = audio.ring = new JitterRing({ sampleRate: rate, targetMs: TARGET_MS });
      node = ctx.createScriptProcessor(1024, 1, 2);
      node.onaudioprocess = (e) => {
        const out = e.outputBuffer;
        ring.pull(out.getChannelData(0), out.getChannelData(1), out.length, ctx.sampleRate);
        audio.fillAvg += (ring.fillMs - audio.fillAvg) * 0.1;
      };
      audio.extraMs = 1024 / rate * 1000;
    }
    node.connect(dest);   // Only into the stream: never ctx.destination (the PC speakers)
    audio.ctx = ctx;
    audio.node = node;
    audio.track = dest.stream.getAudioTracks()[0];
    resumeAudio(true);
    return audio.track;
  }

  // Autoplay rules can start the context suspended; YouTube playing sound in
  // this tab means it may run, so keep asking
  function resumeAudio(now) {
    const t = performance.now();
    if (!audio.ctx || audio.ctx.state !== "suspended" || (!now && t - lastResume < 1000)) return;
    lastResume = t;
    audio.ctx.resume().catch(() => {});
  }

  function onPcm(msg) {
    if (!audio.ctx) return;
    const t0 = performance.now();
    const frame = core.parsePcmFrame(core.base64ToBytes(msg.b64));
    if (!frame) return;
    // Relay delay (background -> here); Date.now because the two contexts'
    // performance clocks differ
    const relay = Math.min(500, Math.max(0, Date.now() - msg.at));
    audio.relayMs += (relay - audio.relayMs) * 0.05;
    audio.frameMs = frame.frames / frame.sampleRate * 1000;
    if (audio.ring) audio.ring.push(frame.pcm, frame.channels, frame.sampleRate, frame.discontinuity);
    else audio.node.port.postMessage({ pcm: frame.pcm, channels: frame.channels, rate: frame.sampleRate, discontinuity: frame.discontinuity }, [frame.pcm.buffer]);
    audio.decodeMs += (performance.now() - t0 - audio.decodeMs) * 0.05;
  }

  // Output latency of the phone's audio past the app's output (§7). A sample
  // waits in the ring for the fill there was when its frame arrived; the
  // smoothed fill sits half a frame below that
  function outputLatency() {
    return core.outputLatencyMs({
      packMs: audio.frameMs / 2,
      relayMs: audio.relayMs,
      ringMs: audio.fillAvg + audio.extraMs,
      cableMs: status.cableLatencyMs ?? 30,
    });
  }

  function createVideo() {
    if (typeof MediaStreamTrackGenerator === "function" && typeof MediaStreamTrackProcessor === "function" && typeof VideoFrame === "function") {
      const gen = new MediaStreamTrackGenerator({ kind: "video" });
      vid.writer = gen.writable.getWriter();
      vid.track = gen;
    } else {
      // Fallback: frames drawn on a canvas that is captured on each draw
      vid.canvas = document.createElement("canvas");
      vid.canvas.width = 1280;
      vid.canvas.height = 720;
      vid.cctx = vid.canvas.getContext("2d");
      vid.track = vid.canvas.captureStream(0).getVideoTracks()[0];
    }
    return vid.track;
  }

  function captureSize(w, h) {
    // Keep GPU memory reasonable for long delays
    const maxW = status.delayMs > 6000 ? 854 : 1280;
    const cw = Math.min(w, maxW);
    return { w: cw, h: Math.max(2, Math.round(h * (cw / w))) };
  }

  // Follows YouTube's <video>; it can be swapped, or change source (new
  // captured track), on navigation
  function watchVideo() {
    const el = findVideo();
    if (!el) return;
    if (el !== vid.el) {
      vid.el = el;
      vid.src?.stop();
      vid.src = null;
      vid.gen++;
      vid.protected = false;
    }
    if (vid.protected) return;
    if (el.mediaKeys) return reportProtected();
    if (!vid.writer || (vid.src && vid.src.readyState === "live")) return;

    let stream;
    try { stream = el.captureStream(); } catch { return reportProtected(); }
    const track = stream.getVideoTracks()[0];
    for (const t of stream.getAudioTracks()) t.stop();   // Only the picture; the audio comes from the app
    if (!track) return;   // Nothing loaded yet; try again next time
    vid.src = track;
    vid.lastFrameAt = performance.now();
    readFrames(new MediaStreamTrackProcessor({ track }).readable.getReader(), ++vid.gen);
  }

  async function readFrames(reader, gen) {
    for (;;) {
      let r;
      try { r = await reader.read(); } catch { break; }
      if (r.done) break;
      const frame = r.value;
      if (gen !== vid.gen) { frame.close(); break; }
      const t = vid.clock.now();
      vid.lastFrameAt = performance.now();
      const mediaTime = vid.el?.currentTime ?? 0;
      let bmp = null;
      try {
        const { w, h } = captureSize(frame.displayWidth, frame.displayHeight);
        bmp = await createImageBitmap(frame, { resizeWidth: w, resizeHeight: h, resizeQuality: "low" });
      } catch {}
      frame.close();   // Promptly: the decoder only has a few frames to lend
      if (bmp && gen === vid.gen && session && !vid.protected) vid.queue.push(t, { bmp, mediaTime });
      else bmp?.close();
    }
    reader.cancel?.().catch?.(() => {});
    if (gen === vid.gen) vid.src = null;
  }

  // Canvas fallback: copy the <video> on the clock instead
  async function captureFallback(now) {
    const el = vid.el;
    if (vid.capturing || !el || now - vid.lastCapture < 33 || el.readyState < 2 || !el.videoWidth || vid.protected || !session) return;
    if (el.paused && vid.queue.length) return;
    vid.capturing = true;
    vid.lastCapture = now;
    const t = vid.clock.now(), mediaTime = el.currentTime;
    try {
      const { w, h } = captureSize(el.videoWidth, el.videoHeight);
      const bmp = await createImageBitmap(el, { resizeWidth: w, resizeHeight: h, resizeQuality: "low" });
      vid.lastFrameAt = performance.now();
      vid.queue.push(t, { bmp, mediaTime });
    } catch {
      reportProtected();   // A tainted (cross-origin or DRM) video can't be copied
    }
    vid.capturing = false;
  }

  function emit({ t, item }) {
    if (vid.writer) {
      // Timestamps on the original capture spacing, so WebRTC paces them evenly
      const ts = Math.max(vid.lastTs + 1, Math.round(Math.min(performance.now(), vid.clock.toReal(t + vid.delay)) * 1000));
      vid.lastTs = ts;
      let frame;
      try { frame = new VideoFrame(item.bmp, { timestamp: ts }); } catch { item.bmp.close(); return; }
      item.bmp.close();
      vid.writer.write(frame).catch(() => frame.close());
    } else {
      if (vid.canvas.width !== item.bmp.width) vid.canvas.width = item.bmp.width;
      if (vid.canvas.height !== item.bmp.height) vid.canvas.height = item.bmp.height;
      vid.cctx.drawImage(item.bmp, 0, 0);
      item.bmp.close();
      vid.track.requestFrame?.();
    }
  }

  function reportProtected() {
    if (vid.protected) return;
    vid.protected = true;
    vid.queue.clear();
    send({ t: "error", code: "protected", message: "This source is protected and can't be streamed." });
  }

  // Playing, visible (a hidden page may legitimately stop decoding video) and
  // still no captured frames: protected
  function checkProtected(now) {
    const el = vid.el;
    if (!el || vid.protected || !session) return;
    const playing = !el.paused && el.readyState >= 3 && document.visibilityState === "visible";
    if (!playing) { vid.playingSince = 0; return; }
    if (!vid.playingSince) vid.playingSince = now;
    if (now - Math.max(vid.playingSince, vid.lastFrameAt) > PROTECTED_MS) reportProtected();
  }

  // ---- The clock -------------------------------------------------------------------
  // Driven by the worklet's messages (audio thread, keeps time in a background
  // or covered window) with a timer as backup; rAF and video-frame callbacks
  // stop when the window isn't drawn
  function tick() {
    const now = performance.now();
    if (now - lastTick < 4) return;
    lastTick = now;

    if (now - lastStatus > 500) { lastStatus = now; pollStatus(); }
    if (!session) return;
    if (now - lastWatch > 500) { lastWatch = now; watchVideo(); resumeAudio(false); }

    vid.delay = vid.smoother.update(core.videoDelayMs(mode(), status.delayMs, outputLatency()), now);
    if (!vid.writer) captureFallback(now);
    if (!vid.clock.frozen) {
      const entry = vid.queue.takeDue(vid.clock.now(), vid.delay);
      if (entry) emit(entry);
    }
    checkProtected(now);
    if (now - lastState >= 1000) sendState(true);
  }

  function pollStatus() {
    api.runtime.sendMessage({ type: "status" }).then((s) => {
      if (!s) return;
      const changed = s.enabled !== status.enabled || s.running !== status.running || s.offline !== status.offline;
      status = s;
      if (session && s.offline && !appOfflineSent) {
        appOfflineSent = true;
        send({ t: "error", code: "app offline", message: "Explicitly Desktop isn't running on the PC." });
      } else if (!s.offline) appOfflineSent = false;
      if (changed) sendState();
    }).catch(() => {});
  }

  // ---- Pause, seek ---------------------------------------------------------------
  // The app holds its delay line while paused and the frame clock stops with
  // it, so resuming continues both from the same point
  function setHold(on) {
    if (vid.held === on) return;
    vid.held = on;
    if (on) vid.clock.freeze(); else vid.clock.thaw();
    apiCall(on ? "/hold/on" : "/hold/off");
  }

  const isMain = (el) => el instanceof HTMLVideoElement && el === findVideo();

  document.addEventListener("pause", (e) => {
    // The end of a video isn't a hold: its last seconds still have to play out
    if (isMain(e.target) && session && !e.target.ended) setHold(true);
  }, true);
  document.addEventListener("playing", (e) => {
    if (isMain(e.target) && vid.held) setHold(false);
  }, true);
  for (const type of ["play", "pause", "volumechange", "durationchange", "loadedmetadata", "waiting", "playing", "seeked"]) {
    document.addEventListener(type, (e) => { if (isMain(e.target)) sendState(); }, true);
  }

  // ---- Phone commands (§4.1) -------------------------------------------------------
  async function onControl(raw) {
    const m = core.parseControl(raw);
    if (!m) return;
    const el = vid.el || findVideo();
    switch (m.t) {
      case "ping": send({ t: "pong", id: m.id }); break;
      case "play": el?.play().catch(() => {}); break;
      case "pause": el?.pause(); break;
      case "seek":
        // Not during an ad: that would skip it. Seeks made on the PC itself
        // aren't flushed; both sides play out what was buffered, as content.js does
        if (!el || isAd()) break;
        el.currentTime = Number.isFinite(el.duration) ? Math.min(m.time, el.duration) : m.time;
        vid.queue.clear();
        vid.seekAnchor = el.currentTime;
        apiCall("/flush");
        sendState(true);
        break;
      case "volume":
        if (el) { el.volume = m.value; if (m.value > 0) el.muted = false; }
        window.postMessage({ source: "explicitly-content", type: "setVolume", value: m.value }, "*");
        break;
      case "filter": {
        const s = await apiCall(m.on ? "/filter/on" : "/filter/off");
        if (s && !s.offline) status = s;
        sendState();
        break;
      }
      case "search": search(m); break;
      case "load": load(m.url); break;
    }
  }

  async function search(m) {
    let items = [];
    try {
      // Same origin as this page, so the content script may fetch it
      const r = await fetch(core.searchUrl(m.q), { credentials: "include" });
      if (r.ok) items = core.parseSearchResults(await r.text(), 20);
    } catch {}
    send({ t: "results", id: m.id, items });
  }

  // Loads go through YouTube's own navigation (yt_main.js), so this page, its
  // peer connection and the phone's session survive. If YouTube doesn't take
  // it, the page is loaded for real; the background keeps this tab as host and
  // the phone's reconnect logic starts a new session with the new page
  async function load(url) {
    const p = core.parseYouTubeUrl(url);
    if (!p) return send({ t: "error", code: "unsupported source", message: "Only YouTube videos can be played." });
    vid.queue.clear();
    vid.protected = false;
    vid.seekAnchor = p.start;

    const el = findVideo();
    if (currentVideoId() === p.videoId && el) {
      el.currentTime = p.start;
      el.play().catch(() => {});
      return;
    }
    apiCall("/flush");
    window.postMessage({ source: "explicitly-content", type: "navigate", videoId: p.videoId, start: p.start }, "*");
    const ok = await until(() => currentVideoId() === p.videoId, 4000);
    if (!ok) {
      navigating = true;
      location.assign(p.watchUrl);
      return;
    }
    sendState(true);
    await until(() => { const v = findVideo(); return v && (v.readyState >= 2 || v.error); }, 15000);
    const v = findVideo();
    if (currentVideoId() === p.videoId && (!v || v.error || v.readyState < 2)) {
      send({ t: "error", code: "load failed", message: "The video didn't load on the PC." });
    }
  }

  function until(test, ms) {
    return new Promise((resolve) => {
      const start = performance.now();
      const check = () => {
        if (test()) return resolve(true);
        if (performance.now() - start > ms) return resolve(false);
        setTimeout(check, 100);
      };
      check();
    });
  }

  // ---- State (§4.1) --------------------------------------------------------------
  function buildState() {
    const el = vid.el || findVideo();
    const m = mode();
    const playing = el && !el.paused;
    const title = document.querySelector("#above-the-fold #title h1, h1.ytd-watch-metadata")?.textContent?.trim() ||
                  document.title.replace(/ - YouTube$/, "");
    const channel = document.querySelector("#above-the-fold #owner #channel-name a, ytd-video-owner-renderer #channel-name a")?.textContent?.trim() || "";
    return {
      t: "state",
      url: currentVideoId() ? "https://www.youtube.com/watch?v=" + currentVideoId() : location.href,
      title, channel,
      paused: !playing,
      time: Math.round(core.shownTime({ currentTime: el?.currentTime, mode: m, delayMs: vid.delay, seekAnchor: vid.seekAnchor, queueFilled: vid.queue.emitted }) * 1000) / 1000,
      duration: el && Number.isFinite(el.duration) ? el.duration : 0,
      volume: el ? (el.muted && !shared.captionOn?.() ? 0 : el.volume) : 1,
      buffering: !!(status.buffering || (playing && (el.readyState < 3 || (m === "delayed" && !vid.queue.emitted)))),
      mode: m,
      filter: { running: !!status.running, enabled: !!status.enabled, delayMs: status.delayMs || 0 },
      ad: isAd(),
    };
  }

  // On change (anything but the clock) and at least every second
  function sendState(force) {
    if (dc?.readyState !== "open") return;
    const s = buildState();
    const json = JSON.stringify({ ...s, time: 0 });
    if (!force && json === lastStateJson) return;
    lastStateJson = json;
    lastState = performance.now();
    send(s);
  }

  // Debug numbers for the A/V offset (also read by tests/ext/e2e)
  setInterval(() => {
    if (!session) return;
    console.debug(`[Explicitly] remote: delay ${vid.delay.toFixed(1)} ms (app ${status.delayMs}, relay ${audio.relayMs.toFixed(1)}, ring ${audio.fillAvg.toFixed(1)}), pcm decode ${audio.decodeMs.toFixed(3)} ms, ring underruns ${audio.stats?.underruns ?? audio.ring?.underruns} drops ${audio.stats?.drops ?? audio.ring?.drops}, queue ${vid.queue.length}`);
  }, 10000);

  hello();
})();
