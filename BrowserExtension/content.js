// While Explicitly Desktop's Filter Explicit Speech is on, its audio reaches you
// a few seconds late. This draws the YouTube picture on a canvas over the real
// video, delayed by exactly the app's current audio delay, so they stay in sync.
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

  function findVideo() {
    return document.querySelector("#movie_player video.html5-main-video")
        || document.querySelector("video");
  }

  function clearFrames() {
    for (const f of frames) f.bmp.close?.();
    frames = [];
    shown = null;
  }

  function ensureCanvas() {
    if (canvas && canvas.isConnected && video && canvas.previousSibling === video) return;
    removeCanvas();
    canvas = document.createElement("canvas");
    canvas.id = "explicitly-delayed-video";
    // Sit right above the <video> but below YouTube's captions and controls
    canvas.style.cssText = "position:absolute;pointer-events:none;background:#000;";
    video.after(canvas);
    ctx = canvas.getContext("2d");
  }

  function removeCanvas() {
    canvas?.remove();
    canvas = null;
    ctx = null;
  }

  function placeCanvas() {
    // Match the video element's on-screen box (YouTube sets it inline)
    canvas.style.left = video.style.left || video.offsetLeft + "px";
    canvas.style.top = video.style.top || video.offsetTop + "px";
    canvas.style.width = video.clientWidth + "px";
    canvas.style.height = video.clientHeight + "px";
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
      // Protected (DRM) video can't be copied; leave the real video visible
      removeCanvas();
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
