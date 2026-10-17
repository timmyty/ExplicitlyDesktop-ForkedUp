// While Explicitly Desktop's Filter Explicit Speech is on, its audio reaches you
// a few seconds late. This draws the video's picture (YouTube, Steam store
// trailers) on a canvas over the real video, delayed by exactly the app's
// current audio delay, so they stay in sync.
// Pausing and seeking stay in sync too: both the audio and this picture keep
// playing out what was already buffered, then follow.

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
    if (!active() || !video || video.readyState < 2 || !video.videoWidth) return;
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
    if (gen !== generation || !active()) return;
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

  function poll() {
    api.runtime.sendMessage({ type: "status" }).then((s) => {
      const wasActive = active();
      status = s ?? status;
      if (active()) {
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
