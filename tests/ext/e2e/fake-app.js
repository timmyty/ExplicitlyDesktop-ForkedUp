// A stand-in for Explicitly Desktop's loopback port (127.0.0.1:8766) as the
// extension sees it: /status, /remote/next long poll, /remote/answer,
// /remote/state, /flush, /hold/*, /filter/*, and the /pcm WebSocket.
//
// The PCM is what the real app would tap: the host page's audio, delayMs late.
// The host page has no real audio path here, so the page reports its playback
// clock (anchors) and this server synthesizes that audio: a 50 ms 1 kHz beep at
// each whole second of media time (the clip flashes white there), placed where
// the app would output it: cable latency + delayMs after the page played it.

const http = require("node:http");
const crypto = require("node:crypto");
const core = require("../../../BrowserExtension/remote-core.js");

const RATE = 48000, FRAME = 960, CABLE_MS = 30, BEEP_MS = 50;

function startFakeApp({ port = 8766, delayMs = 3500 } = {}) {
  const log = [];                 // Every request: { t, method, path, body }
  const state = { running: true, enabled: true, held: false, delayMs };
  const offers = [];              // Waiting for /remote/next
  let waiter = null;              // A pending /remote/next response
  const answers = new Map();      // sessionId -> resolve
  const anchors = [];             // { wall, media, paused } from the host page
  let pcmSocket = null;

  const status = () => ({ running: state.running, enabled: state.enabled, buffering: false, bypassed: false,
    delayMs: state.enabled ? state.delayMs : 0, held: state.held, sampleRate: RATE });

  const server = http.createServer((req, res) => {
    let body = "";
    req.on("data", (c) => { body += c; });
    req.on("end", () => {
      const path = req.url.split("?")[0];
      let parsed;
      try { parsed = body ? JSON.parse(body) : undefined; } catch {}
      log.push({ t: Date.now(), method: req.method, path, body: parsed });
      const send = (code, obj) => { res.writeHead(code, { "Content-Type": "application/json" }); res.end(JSON.stringify(obj)); };
      if ((req.headers.origin || "").startsWith("http")) return send(403, { error: "forbidden" });

      if (req.method === "GET" && path === "/status") return send(200, status());
      if (req.method === "GET" && path === "/lexicon") return send(200, ["damn"]);
      if (req.method === "POST" && path === "/bypass") return send(200, status());
      if (req.method === "POST" && path.startsWith("/filter/")) {
        state.enabled = path.endsWith("/on") ? true : path.endsWith("/off") ? false : !state.enabled;
        return send(200, status());
      }
      if (req.method === "POST" && path === "/flush") return send(200, status());
      if (req.method === "POST" && (path === "/hold/on" || path === "/hold/off")) { state.held = path.endsWith("/on"); return send(200, status()); }
      if (req.method === "POST" && path === "/remote/state") { state.phone = parsed; return send(200, {}); }
      if (req.method === "POST" && path === "/remote/answer") {
        answers.get(parsed?.sessionId)?.(parsed);
        answers.delete(parsed?.sessionId);
        return send(200, {});
      }
      if (req.method === "GET" && path === "/remote/next") {
        if (offers.length) return send(200, offers.shift());
        const timer = setTimeout(() => { if (waiter?.res === res) { waiter = null; send(200, { type: "none" }); } }, 25000);
        waiter = { res, send: (m) => { clearTimeout(timer); send(200, m); } };
        return;
      }
      send(404, { error: "not found" });
    });
  });

  // ---- /pcm WebSocket (just enough of RFC 6455) ----
  server.on("upgrade", (req, socket) => {
    if (req.url !== "/pcm" || (req.headers.origin || "").startsWith("http")) { socket.destroy(); return; }
    const accept = crypto.createHash("sha1").update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
    socket.setNoDelay(true);
    pcmSocket?.destroy();
    pcmSocket = socket;
    log.push({ t: Date.now(), method: "WS", path: "/pcm" });
    socket.on("data", (d) => { if ((d[0] & 0x0f) === 8) socket.destroy(); });
    socket.on("close", () => { if (pcmSocket === socket) pcmSocket = null; });
    socket.on("error", () => {});
    pcm.discontinuity = true;
  });

  function wsSend(socket, payload) {
    const n = payload.length;
    const head = n < 126 ? Buffer.from([0x82, n]) : Buffer.from([0x82, 126, n >> 8, n & 255]);
    socket.write(Buffer.concat([head, Buffer.from(payload.buffer, payload.byteOffset, n)]));
  }

  // ---- The tapped audio ----
  // Media time the page was at (on screen) at wall time w, from its anchors
  function mediaAt(w) {
    let a = null;
    for (let i = anchors.length - 1; i >= 0; i--) if (anchors[i].wall <= w) { a = anchors[i]; break; }
    if (!a) return null;
    return a.paused ? a.media : a.media + (w - a.wall) / 1000;
  }

  const pcm = { t0: Date.now(), sent: 0, discontinuity: true };
  const ticker = setInterval(() => {
    const now = Date.now();
    while (pcm.t0 + (pcm.sent + FRAME) / RATE * 1000 <= now) {
      const samples = new Int16Array(FRAME * 2);
      if (!state.held) {
        for (let i = 0; i < FRAME; i++) {
          const wall = pcm.t0 + (pcm.sent + i) / RATE * 1000;   // When the app outputs this sample
          // The app outputs, delayMs later, what came in through the cable
          const m = mediaAt(wall - (state.enabled ? state.delayMs : 0) - CABLE_MS);
          if (m === null || m < 0) continue;
          const frac = m - Math.floor(m);
          if (frac < BEEP_MS / 1000) {
            const v = Math.round(Math.sin(2 * Math.PI * 1000 * (pcm.sent + i) / RATE) * 20000);
            samples[2 * i] = v; samples[2 * i + 1] = v;
          }
        }
      }
      if (pcmSocket) {
        const flags = (pcm.discontinuity ? 1 : 0) | (state.enabled ? 2 : 0) | (state.held ? 4 : 0);
        wsSend(pcmSocket, core.buildPcmFrame({ sampleRate: RATE, flags, startSample: pcm.sent, delaySamples: state.delayMs * 48, pcm: samples }));
        pcm.discontinuity = false;
      }
      pcm.sent += FRAME;
    }
  }, 2);

  return {
    log, state, anchors,
    listen: () => new Promise((resolve) => server.listen(port, "127.0.0.1", resolve)),
    close() { clearInterval(ticker); pcmSocket?.destroy(); server.closeAllConnections?.(); server.close(); },
    // The phone's offer, as the app's StreamHost would pass it on
    offer(sdp, sessionId = "s" + Date.now(), deviceId = "phone-1") {
      return new Promise((resolve) => {
        answers.set(sessionId, resolve);
        const msg = { type: "offer", sessionId, deviceId, deviceName: "Test phone", sdp };
        if (waiter) { const w = waiter; waiter = null; w.send(msg); } else offers.push(msg);
      });
    },
    anchor(a) {
      anchors.push(a);
      if (anchors.length > 2000) anchors.splice(0, 1000);
    },
  };
}

module.exports = { startFakeApp };
