// End-to-end check of the PC-side sender (BrowserExtension/) in Chromium:
//   fake app (127.0.0.1:8766)  <->  extension (unpacked, host tab on a fake
//   www.youtube.com)  <-WebRTC->  receiver page (the phone)
// The clip flashes white at every whole second; the fake app's PCM beeps at
// the same media times, delayed like the real app. The receiver measures
// beep time minus flash time. Also exercises the §4.1 controls.
//
//   node tests/ext/e2e/run.js            (needs root for port 443, or E2E_HTTPS_PORT + a matching rule)
// Env: PLAYWRIGHT_BROWSERS_PATH (default /opt/pw-browsers), PLAYWRIGHT_MODULE,
//      CHROMIUM (executable), E2E_SECONDS (measure time, default 15), E2E_HEADED=1

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const http = require("node:http");
const { execFileSync, spawn } = require("node:child_process");
const { startFakeApp } = require("./fake-app.js");
const { startFakeYouTube } = require("./fake-youtube.js");

const EXT = path.resolve(__dirname, "../../../BrowserExtension");
const BROWSERS = process.env.PLAYWRIGHT_BROWSERS_PATH || "/opt/pw-browsers";
const SECONDS = +(process.env.E2E_SECONDS || 15);
const DELAY_MS = 3500;
const HTTPS_PORT = +(process.env.E2E_HTTPS_PORT || 443);
const TMP = path.join(os.tmpdir(), "explicitly-e2e");

function loadPlaywright() {
  const tries = [process.env.PLAYWRIGHT_MODULE, "playwright", "/opt/node22/lib/node_modules/playwright"].filter(Boolean);
  for (const t of tries) { try { return require(t); } catch {} }
  throw new Error("playwright not found; set PLAYWRIGHT_MODULE");
}

function findChromium() {
  if (process.env.CHROMIUM) return process.env.CHROMIUM;
  const dir = fs.readdirSync(BROWSERS).filter((d) => /^chromium-\d+$/.test(d)).sort().pop();
  return path.join(BROWSERS, dir, "chrome-linux", "chrome");
}

function findFfmpeg() {
  const dir = fs.readdirSync(BROWSERS).filter((d) => /^ffmpeg-\d+$/.test(d)).sort().pop();
  return path.join(BROWSERS, dir, "ffmpeg-linux");
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function until(fn, ms, what) {
  const start = Date.now();
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() - start > ms) throw new Error("timed out: " + what);
    await sleep(100);
  }
}

// Self-signed certificate for www.youtube.com
function makeCert() {
  const key = path.join(TMP, "key.pem"), cert = path.join(TMP, "cert.pem");
  if (!fs.existsSync(cert)) {
    execFileSync("openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "30",
      "-subj", "/CN=www.youtube.com", "-addext", "subjectAltName=DNS:www.youtube.com"], { stdio: "ignore" });
  }
  return { key: fs.readFileSync(key), cert: fs.readFileSync(cert) };
}

// A 60 s VP8 clip at 30 fps: white for 2 frames at each whole second, dark
// otherwise (with a moving bar). Chromium draws the JPEGs, ffmpeg encodes
async function makeClip(chromium, exe) {
  const out = path.join(TMP, "clip.webm");
  if (fs.existsSync(out)) return out;
  const browser = await chromium.launch({ executablePath: exe, headless: true });
  const page = await browser.newPage();
  const jpegs = await page.evaluate(() => {
    const c = document.createElement("canvas");
    c.width = 640; c.height = 360;
    const g = c.getContext("2d");
    const list = [];
    for (let k = 0; k < 30; k++) {
      g.fillStyle = k < 2 ? "#fff" : "#141414";
      g.fillRect(0, 0, 640, 360);
      if (k >= 2) { g.fillStyle = "#555"; g.fillRect(k * 20, 150, 40, 60); }
      list.push(c.toDataURL("image/jpeg", 0.9).split(",")[1]);
    }
    return list;
  });
  await browser.close();
  // This ffmpeg build has no pipe protocol: concatenated JPEGs in a file
  const frames = jpegs.map((b) => Buffer.from(b, "base64"));
  const mjpeg = path.join(TMP, "frames.mjpeg");
  fs.writeFileSync(mjpeg, Buffer.concat(Array.from({ length: 60 * 30 }, (_, i) => frames[i % 30])));
  const ff = spawn(findFfmpeg(), ["-y", "-loglevel", "error", "-f", "image2pipe", "-framerate", "30", "-c:v", "mjpeg", "-i", mjpeg,
    "-c:v", "libvpx", "-b:v", "1M", "-g", "30", "-deadline", "realtime", "-cpu-used", "8", out], { stdio: "inherit" });
  await new Promise((resolve, reject) => ff.on("exit", (code) => (code === 0 ? resolve() : reject(new Error("ffmpeg " + code)))));
  return out;
}

// The receiver ("phone") page and its way to hand an offer to the fake app
function startReceiverServer(app) {
  const server = http.createServer((req, res) => {
    if (req.url === "/receiver.html") {
      res.writeHead(200, { "Content-Type": "text/html" });
      return res.end(fs.readFileSync(path.join(__dirname, "receiver.html")));
    }
    if (req.url === "/test/offer" && req.method === "POST") {
      let b = "";
      req.on("data", (c) => { b += c; });
      req.on("end", async () => {
        const { sdp } = JSON.parse(b);
        const timeout = sleep(20000).then(() => ({ error: "timeout" }));
        const reply = await Promise.race([app.offer(sdp), timeout]);
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify(reply));
      });
      return;
    }
    res.writeHead(404); res.end();
  });
  return new Promise((resolve) => server.listen(8799, "127.0.0.1", () => resolve(server)));
}

function pairOffsets(flashes, beeps) {
  const out = [];
  for (const f of flashes) {
    let best = null;
    for (const b of beeps) if (best === null || Math.abs(b - f) < Math.abs(best - f)) best = b;
    if (best !== null && Math.abs(best - f) < 400) out.push(best - f);
  }
  return out;
}

function summarize(xs) {
  if (!xs.length) return { n: 0 };
  const s = [...xs].sort((a, b) => a - b);
  const med = s[Math.floor(s.length / 2)];
  return { n: s.length, median: +med.toFixed(1), min: +s[0].toFixed(1), max: +s[s.length - 1].toFixed(1),
    within45: s.filter((x) => Math.abs(x) <= 45).length };
}

async function main() {
  fs.mkdirSync(TMP, { recursive: true });
  const { chromium } = loadPlaywright();
  const exe = findChromium();
  const checks = [];
  const check = (name, ok, detail = "") => { checks.push({ name, ok: !!ok, detail }); console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail ? "  " + detail : ""}`); };

  const clip = await makeClip(chromium, exe);
  const app = startFakeApp({ delayMs: DELAY_MS });
  await app.listen();
  let capture = { shown: [], captured: [] };
  const yt = await startFakeYouTube({ ...makeCert(), clip, onAnchor: (a) => app.anchor(a), onCapture: (c) => { capture = c; }, port: HTTPS_PORT });
  const rx = await startReceiverServer(app);

  const profile = fs.mkdtempSync(path.join(TMP, "profile-"));
  const headed = process.env.E2E_HEADED === "1";
  const context = await chromium.launchPersistentContext(profile, {
    executablePath: exe,
    headless: false,
    ignoreHTTPSErrors: true,
    args: [
      ...(headed ? [] : ["--headless=new"]),
      `--disable-extensions-except=${EXT}`,
      `--load-extension=${EXT}`,
      `--host-resolver-rules=MAP www.youtube.com 127.0.0.1${HTTPS_PORT === 443 ? "" : ":" + HTTPS_PORT}`,
      "--ignore-certificate-errors",
      "--autoplay-policy=no-user-gesture-required",
      "--no-sandbox",
      "--no-proxy-server",   // An https_proxy in the environment would get www.youtube.com
    ],
  });
  const logs = [];
  const watch = (page) => page.on("console", (m) => logs.push(`[${page.url().slice(0, 40)}] ${m.text()}`));
  context.on("page", watch);
  context.pages().forEach(watch);

  let result = {};
  try {
    const sw = context.serviceWorkers()[0] || await context.waitForEvent("serviceworker", { timeout: 10000 });
    check("extension service worker loaded", sw, sw.url());

    const phone = await context.newPage();
    await phone.goto("http://127.0.0.1:8799/receiver.html");
    await until(() => phone.evaluate(() => window.R.open || (window.R.errors.length && window.R.errors)), 25000, "data channel open");
    const errs = await phone.evaluate(() => window.R.errors.filter((e) => !e.startsWith("play:")));
    check("offer answered, DataChannel open", errs.length === 0, errs.join("; "));
    const answer = await phone.evaluate(() => window.R.answer);
    check("answer has Opus stereo 128 kbps", /stereo=1/.test(answer) && /maxaveragebitrate=128000/.test(answer));
    const hostPage = context.pages().find((p) => p.url().startsWith("https://www.youtube.com"));
    check("host tab opened on www.youtube.com", hostPage, hostPage?.url());
    check("app told session is active", app.state.phone?.active === true, JSON.stringify(app.state.phone));

    // Load through the phone: YouTube-style navigation, same page
    await phone.evaluate(() => window.send({ t: "load", url: "https://youtu.be/TESTCLIP01A" }));
    await until(() => phone.evaluate(() => window.R.state?.url?.includes("TESTCLIP01A") && !window.R.state.paused), 10000, "loaded");
    check("load via SPA navigation (no reload)", hostPage.url().includes("/watch?v=TESTCLIP01A") && app.log.filter((l) => l.path === "/remote/answer").length === 1);

    // Delay fill, then measure
    await sleep(DELAY_MS + 2000);
    await phone.evaluate(() => { window.R.flashes.length = 0; window.R.beeps.length = 0; window.R.tFlashes.length = 0; window.R.tBeeps.length = 0; });
    await sleep(SECONDS * 1000);
    let r = await phone.evaluate(() => ({ flashes: [...window.R.flashes], beeps: [...window.R.beeps], tFlashes: [...window.R.tFlashes], tBeeps: [...window.R.tBeeps], state: window.R.state }));
    const first = summarize(pairOffsets(r.flashes, r.beeps));
    console.log("  track level:", summarize(pairOffsets(r.tFlashes, r.tBeeps)));
    if (process.env.E2E_VERBOSE) console.log("  per beep (track level):", pairOffsets(r.tFlashes, r.tBeeps).map((x) => x.toFixed(0)).join(" "), "\n  per beep (played):", pairOffsets(r.flashes, r.beeps).map((x) => x.toFixed(0)).join(" "));
    console.log(`flashes ${r.flashes.length}, beeps ${r.beeps.length}, offset (beep - flash) ms:`, first);
    check("flashes and beeps arrive", r.flashes.length >= SECONDS - 2 && r.beeps.length >= SECONDS - 2, `${r.flashes.length}/${r.beeps.length}`);
    check("A/V offset within ±45 ms (median)", first.n && Math.abs(first.median) <= 45, JSON.stringify(first));
    check("state: delayed mode, time is behind by the delay", r.state.mode === "delayed" && r.state.filter.delayMs === DELAY_MS, `time ${r.state.time}`);
    result.offset = first;

    // Pause: hold on, clock frozen; play: hold off, sync kept
    await phone.evaluate(() => window.send({ t: "pause" }));
    await until(() => app.state.held, 3000, "hold on");
    await until(() => phone.evaluate(() => window.R.state?.paused), 3000, "paused state");
    check("pause -> POST /hold/on, state.paused", true);
    await sleep(2000);
    await phone.evaluate(() => window.send({ t: "play" }));
    await until(() => !app.state.held, 3000, "hold off");
    check("play -> POST /hold/off", true);
    await sleep(1500);
    await phone.evaluate(() => { window.R.flashes.length = 0; window.R.beeps.length = 0; window.R.tFlashes.length = 0; window.R.tBeeps.length = 0; });
    await sleep(8000);
    r = await phone.evaluate(() => ({ flashes: [...window.R.flashes], beeps: [...window.R.beeps], tFlashes: [...window.R.tFlashes], tBeeps: [...window.R.tBeeps] }));
    const after = summarize(pairOffsets(r.flashes, r.beeps));
    console.log("  track level:", summarize(pairOffsets(r.tFlashes, r.tBeeps)));
    console.log("offset after pause/resume:", after);
    check("A/V offset after pause/resume within ±45 ms", after.n && Math.abs(after.median) <= 45, JSON.stringify(after));
    result.afterPause = after;

    // Seek: frame queue cleared, /flush, phone shows the target while refilling
    const flushes = app.log.filter((l) => l.path === "/flush").length;
    await phone.evaluate(() => window.send({ t: "seek", time: 30 }));
    await until(() => app.log.filter((l) => l.path === "/flush").length > flushes, 3000, "flush");
    await sleep(300);
    const st = await phone.evaluate(() => window.R.state);
    check("seek -> POST /flush, state shows target while buffering", st.buffering && Math.abs(st.time - 30) < 0.5, `time ${st.time} buffering ${st.buffering}`);
    await sleep(DELAY_MS + 2000);
    await phone.evaluate(() => { window.R.flashes.length = 0; window.R.beeps.length = 0; window.R.tFlashes.length = 0; window.R.tBeeps.length = 0; });
    await sleep(6000);
    r = await phone.evaluate(() => ({ flashes: [...window.R.flashes], beeps: [...window.R.beeps], tFlashes: [...window.R.tFlashes], tBeeps: [...window.R.tBeeps] }));
    const afterSeek = summarize(pairOffsets(r.flashes, r.beeps));
    console.log("  track level:", summarize(pairOffsets(r.tFlashes, r.tBeeps)));
    console.log("offset after seek:", afterSeek);
    check("A/V offset after seek within ±45 ms", afterSeek.n && Math.abs(afterSeek.median) <= 45, JSON.stringify(afterSeek));
    result.afterSeek = afterSeek;

    await phone.evaluate(() => window.send({ t: "volume", value: 0.5 }));
    await until(() => phone.evaluate(() => window.R.state?.volume === 0.5), 3000, "volume");
    check("volume -> state.volume", true);

    await phone.evaluate(() => window.send({ t: "filter", on: false }));
    await until(() => phone.evaluate(() => window.R.state?.mode === "live"), 3000, "filter off");
    check("filter off -> POST /filter/off, mode live", app.log.some((l) => l.path === "/filter/off"));
    await phone.evaluate(() => window.send({ t: "filter", on: true }));

    await phone.evaluate(() => window.send({ t: "search", q: "cats", id: 7 }));
    const res = await until(() => phone.evaluate(() => window.R.msgs.find((m) => m.t === "results")), 5000, "results");
    check("search -> results from ytInitialData", res.id === 7 && res.items.length === 3 && res.items[0].title === "cats one", JSON.stringify(res.items[0]));

    await phone.evaluate(() => window.send({ t: "ping", id: 42 }));
    await until(() => phone.evaluate(() => window.R.msgs.some((m) => m.t === "pong" && m.id === 42)), 3000, "pong");
    check("ping -> pong", true);

    await phone.evaluate(() => window.send({ t: "load", url: "https://vimeo.com/123" }));
    const err = await until(() => phone.evaluate(() => window.R.msgs.find((m) => m.t === "error")), 3000, "error");
    check("unsupported load -> error", err.code === "unsupported source");

    const capBias = summarize(pairOffsets(capture.shown, capture.captured));
    if (!capBias.n) console.log("capture lists", capture.shown.length, capture.captured.length, capture.shown.slice(-3), capture.captured.slice(-3));
    console.log("host page: flash captured minus shown, ms:", capBias);
    result.captureMinusShown = capBias;
    const sync = summarize(await phone.evaluate(() => window.R.sync.slice(-40)));
    console.log("receiver playout sync (video - audio capture time being played, ms):", sync);
    result.receiverSync = sync;
    const st2 = await phone.evaluate(() => window.stats());
    console.log("receiver stats:", JSON.stringify(st2));
    result.stats = st2;

    // Phone leaves (closes its connection): host pauses, app told
    await phone.evaluate(() => window.pc.close());
    await phone.close();
    await until(() => app.state.phone?.active === false, 15000, "session inactive");
    const paused = await hostPage.evaluate(() => document.querySelector("#movie_player video").paused);
    check("phone gone -> host paused, /remote/state inactive", paused);
  } catch (e) {
    check("run", false, e.message);
  } finally {
    const relay = logs.filter((l) => l.includes("[Explicitly]"));
    console.log("extension logs:\n  " + relay.slice(-6).join("\n  "));
    if (process.env.E2E_VERBOSE) console.log("app log: " + app.log.filter((l) => l.path !== "/status" && l.path !== "/__anchor").map((l) => l.method + " " + l.path).join(", "));
    if (process.env.E2E_VERBOSE) console.log("pages: " + context.pages().map((p) => p.url()).join(", ") + "\n" + logs.join("\n"));
    await context.close().catch(() => {});
    app.close(); yt.close(); rx.close();
    fs.rmSync(profile, { recursive: true, force: true });
  }
  const failed = checks.filter((c) => !c.ok);
  console.log(JSON.stringify({ ...result, passed: checks.length - failed.length, failed: failed.length }));
  process.exit(failed.length ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });
