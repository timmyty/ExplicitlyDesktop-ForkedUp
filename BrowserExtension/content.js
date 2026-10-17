// Two ways to keep video and filtered audio together while Explicitly Desktop's
// Filter Explicit Speech is on:
//
// 1. Caption mode (YouTube videos with captions): no delay at all. The video's
//    own captions (auto-generated ones have per-word timing and mark swears as
//    "[ __ ]") give the mute times in advance, and the video is muted at those
//    moments by its playback time. The app is told to pass this audio through
//    undelayed while this runs.
// 2. Delayed picture (no captions, and Steam store trailers): the app's audio
//    reaches you a few seconds late, so the picture is drawn on a canvas over the
//    real video, delayed by exactly the app's current delay. Pausing and seeking
//    stay in sync too: both keep playing out what was already buffered.

(() => {
  const api = globalThis.browser ?? globalThis.chrome;
  const POLL_MS = 250;
  const MAX_FRAMES = 1200;  // Hard cap so a stuck clock can't eat GPU memory

  let status = { running: false, enabled: false, delayMs: 0 };
  let video = null;
  let canvas = null;
  let ctx = null;
  let frames = [];          // { t, bmp } oldest first
  let generation = 0;       // Bumped on stop so stale capture/render loops exit
  let running = false;
  let shown = null;

  const active = () => status.running && status.enabled && !status.offline;
  // Delayed picture only when caption mode isn't handling this video
  const overlayActive = () => active() && !caption.on;
  const isYouTube = location.hostname.endsWith("youtube.com");

  function findVideo() {
    if (isYouTube)
      return document.querySelector("#movie_player video.html5-main-video") || document.querySelector("video");

    // Other sites (Steam store trailers): the biggest visible video, preferring
    // one that is playing
    let best = null, bestScore = 0;
    for (const v of document.querySelectorAll("video")) {
      const r = v.getBoundingClientRect();
      const area = Math.max(0, r.width) * Math.max(0, r.height);
      if (area < 100 * 60) continue;
      const score = area * (v.paused ? 1 : 10);
      if (score > bestScore) { best = v; bestScore = score; }
    }
    return best;
  }

  function clearFrames() {
    for (const f of frames) f.bmp.close?.();
    frames = [];
    shown = null;
  }

  function ensureCanvas() {
    if (canvas && canvas.isConnected && video && (!isYouTube || canvas.previousSibling === video)) return;
    removeCanvas();
    canvas = document.createElement("canvas");
    canvas.id = "explicitly-delayed-video";
    if (isYouTube) {
      // Sit right above the <video> but below YouTube's captions and controls
      canvas.style.cssText = "position:absolute;pointer-events:none;background:#000;";
      video.after(canvas);
    } else {
      // Elsewhere, float over the video's on-screen box; clicks pass through
      canvas.style.cssText = "position:fixed;pointer-events:none;background:#000;z-index:2147483646;";
      document.documentElement.appendChild(canvas);
    }
    ctx = canvas.getContext("2d");
  }

  function removeCanvas() {
    canvas?.remove();
    canvas = null;
    ctx = null;
  }

  function placeCanvas() {
    if (isYouTube) {
      // Match the video element's box (YouTube sets it inline)
      canvas.style.left = video.style.left || video.offsetLeft + "px";
      canvas.style.top = video.style.top || video.offsetTop + "px";
      canvas.style.width = video.clientWidth + "px";
      canvas.style.height = video.clientHeight + "px";
      return;
    }
    const r = video.getBoundingClientRect();
    const hidden = r.width < 1 || r.height < 1 || document.fullscreenElement && !document.fullscreenElement.contains(video);
    canvas.style.display = hidden ? "none" : "block";
    canvas.style.left = r.left + "px";
    canvas.style.top = r.top + "px";
    canvas.style.width = r.width + "px";
    canvas.style.height = r.height + "px";
  }

  function captureSize() {
    // Keep GPU memory reasonable for long delays
    const maxW = status.delayMs > 6000 ? 854 : 1280;
    const w = Math.min(video.videoWidth, maxW);
    return { w, h: Math.round(video.videoHeight * (w / video.videoWidth)) };
  }

  async function captureFrame() {
    if (!overlayActive() || !video || video.readyState < 2 || !video.videoWidth) return;
    const { w, h } = captureSize();
    try {
      const bmp = await createImageBitmap(video, { resizeWidth: w, resizeHeight: h, resizeQuality: "low" });
      frames.push({ t: performance.now(), bmp });
      if (frames.length > MAX_FRAMES) frames.shift().bmp.close?.();
    } catch {
      // A video from another site (Steam's CDN) can't be copied unless it was
      // loaded with CORS. Its CDN allows that, so reload it that way once,
      // keeping the position; otherwise (DRM) leave the real video visible
      if (!video.crossOrigin && video.currentSrc && !video.currentSrc.startsWith("blob:")) {
        const t = video.currentTime, playing = !video.paused;
        video.crossOrigin = "anonymous";
        video.load();
        video.currentTime = t;
        if (playing) video.play().catch(() => {});
      } else {
        removeCanvas();
      }
    }
  }

  function scheduleCapture(gen) {
    if (gen !== generation || !video) return;
    const next = () => { if (gen === generation) { captureFrame(); scheduleCapture(gen); } };
    if (video.requestVideoFrameCallback) video.requestVideoFrameCallback(next);
    else requestAnimationFrame(next);
  }

  function render(gen) {
    if (gen !== generation || !overlayActive()) return;
    requestAnimationFrame(() => render(gen));
    if (!canvas || !video) return;

    placeCanvas();
    const due = performance.now() - status.delayMs;

    // Newest frame that is at least delayMs old; drop anything older than it
    let idx = -1;
    for (let i = 0; i < frames.length && frames[i].t <= due; i++) idx = i;
    while (idx > 0) { frames.shift().bmp.close?.(); idx--; }

    // Until the first frame is old enough (filter just turned on), hold the
    // oldest frame, matching the app's silence while its buffer fills
    const frame = frames[0] ?? null;
    if (!frame || frame === shown) return;

    if (canvas.width !== frame.bmp.width) canvas.width = frame.bmp.width;
    if (canvas.height !== frame.bmp.height) canvas.height = frame.bmp.height;
    ctx.drawImage(frame.bmp, 0, 0);
    shown = frame;
  }

  function start() {
    video = findVideo();
    if (!video) return;
    ensureCanvas();
    if (!running) {
      running = true;
      const gen = generation;
      scheduleCapture(gen);
      requestAnimationFrame(() => render(gen));
    }
  }

  function stop() {
    running = false;
    generation++;
    clearFrames();
    removeCanvas();
  }

  // ---- Caption mode (YouTube) -------------------------------------------------
  const caption = {
    videoId: null,
    cues: null,            // [{ eventId, mediaId, timestamp: { startMs, endMs }, action: "MUTE" }]
    kind: null,            // "asr" (per-word timing) or "manual"
    requestedAt: 0,
    gaveUp: false,
    on: false,             // Caption mode is muting this video
    mutedByUs: false,
    lastBypass: 0,
  };
  let lexicon = null;
  const MUTE_LEAD_MS = 60, MUTE_TAIL_MS = 60;   // Small margin around each word
  const norm = (w) => w.toLowerCase().replace(/[^a-z0-9]/g, "");

  function currentVideoId() {
    try { return new URL(location.href).searchParams.get("v"); } catch { return null; }
  }

  function resetCaptions(videoId) {
    if (caption.mutedByUs && video) video.muted = false;
    Object.assign(caption, { videoId, cues: null, kind: null, requestedAt: 0, gaveUp: false, on: false, mutedByUs: false });
  }

  // json3 caption events -> words with times
  function wordsFrom(json) {
    const words = [];
    for (const ev of json.events || []) {
      if (!ev.segs || ev.tStartMs == null) continue;
      const evEnd = ev.tStartMs + (ev.dDurationMs || 0);
      const segs = ev.segs.filter((sg) => sg.utf8 && sg.utf8.trim());
      const timed = segs.some((sg) => sg.tOffsetMs != null);
      if (timed) {
        // Auto-generated: each segment is a word with its own start
        segs.forEach((sg, k) => {
          const start = ev.tStartMs + (sg.tOffsetMs || 0);
          const next = k + 1 < segs.length ? ev.tStartMs + (segs[k + 1].tOffsetMs || 0) : evEnd;
          words.push({ text: sg.utf8.trim(), start, end: Math.min(next, start + 1200) });
        });
      } else {
        // Manual captions: a line per event; spread its words by length
        const parts = segs.map((sg) => sg.utf8).join(" ").split(/\s+/).filter(Boolean);
        const total = parts.reduce((n, w) => n + w.length + 1, 0) || 1;
        let at = ev.tStartMs;
        for (const w of parts) {
          const len = (ev.dDurationMs || 0) * (w.length + 1) / total;
          words.push({ text: w, start: at, end: at + len });
          at += len;
        }
      }
    }
    return words;
  }

  function cuesFrom(words, videoId) {
    const cues = [];
    const add = (start, end) => cues.push({
      eventId: `evt_${videoId}_${cues.length}`,
      mediaId: `youtube:${videoId}`,
      timestamp: { startMs: Math.round(start - MUTE_LEAD_MS), endMs: Math.round(end + MUTE_TAIL_MS) },
      action: "MUTE",
    });
    for (let i = 0; i < words.length; i++) {
      const w = words[i];
      const n = norm(w.text);
      const masked = w.text.includes("__") || /\*/.test(w.text);   // YouTube writes swears as [ __ ]
      if (masked || (n && lexicon.has(n))) { add(w.start, w.end); continue; }
      if (i + 1 < words.length && lexicon.has(n + norm(words[i + 1].text))) {
        add(w.start, words[i + 1].end);
        i++;
      }
    }
    return cues;
  }

  window.addEventListener("message", (e) => {
    if (e.source !== window || e.data?.source !== "explicitly-yt") return;
    const msg = e.data;
    if (msg.videoId && msg.videoId !== currentVideoId()) return;

    if (msg.type === "noCaptions") { caption.gaveUp = true; return; }
    if (msg.type !== "captions" || !lexicon) return;

    let json;
    try { json = JSON.parse(msg.json); } catch { return; }

    // Prefer auto-generated captions (per-word timing). If the player loaded a
    // manual track, ask for the auto-generated one too, with the same request
    if (msg.kind !== "asr" && caption.kind !== "asr" && !msg.url.includes("kind=asr")) {
      const u = new URL(msg.url);
      u.searchParams.set("kind", "asr");
      u.searchParams.set("lang", "en");
      window.postMessage({ source: "explicitly-content", type: "fetchVariant", url: u.href }, "*");
    }
    if (caption.kind === "asr" && msg.kind !== "asr") return;   // Keep the better one

    caption.cues = cuesFrom(wordsFrom(json), currentVideoId());
    caption.kind = msg.kind;
    console.debug(`[Explicitly] ${caption.cues.length} mute cues from ${msg.kind} captions`);
  });

  function muteLoop() {
    requestAnimationFrame(muteLoop);
    if (!caption.on || !caption.cues || !video) return;
    const t = video.currentTime * 1000;
    const inCue = caption.cues.some((c) => t >= c.timestamp.startMs && t <= c.timestamp.endMs);
    if (inCue && !video.muted) { video.muted = true; caption.mutedByUs = true; }
    else if (!inCue && caption.mutedByUs) { video.muted = false; caption.mutedByUs = false; }
  }
  if (isYouTube) requestAnimationFrame(muteLoop);

  function updateCaptionMode() {
    if (!isYouTube) return;
    const vid = currentVideoId();
    if (vid !== caption.videoId) resetCaptions(vid);

    if (!active() || !vid) {
      if (caption.on) { caption.on = false; if (caption.mutedByUs && video) { video.muted = false; caption.mutedByUs = false; } }
      return;
    }

    if (!lexicon) {
      api.runtime.sendMessage({ type: "lexicon" }).then((list) => { if (Array.isArray(list) && list.length) lexicon = new Set(list); }).catch(() => {});
      return;
    }

    if (!caption.cues && !caption.gaveUp) {
      const now = performance.now();
      if (!caption.requestedAt) {
        caption.requestedAt = now;
        window.postMessage({ source: "explicitly-content", type: "loadCaptions" }, "*");
      } else if (now - caption.requestedAt > 6000) {
        caption.gaveUp = true;   // No captions arrived: use the delayed picture
      }
    }

    const wasOn = caption.on;
    caption.on = !!caption.cues;
    if (caption.on) {
      if (!wasOn) stop();   // Drop the delayed picture
      const now = performance.now();
      if (now - caption.lastBypass > 1000) {
        caption.lastBypass = now;
        api.runtime.sendMessage({ type: "bypass" }).catch(() => {});
      }
    }
  }

  function poll() {
    api.runtime.sendMessage({ type: "status" }).then((s) => {
      const wasActive = overlayActive();
      status = s ?? status;
      video = video || findVideo();
      updateCaptionMode();
      if (overlayActive()) {
        // YouTube swaps the <video> on navigation; follow it
        const current = findVideo();
        if (current !== video) { stop(); }
        start();
      } else if (wasActive) {
        stop();
      }
    }).catch(() => {});
  }

  setInterval(poll, POLL_MS);
  poll();
})();
