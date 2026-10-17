// Explicitly Remote: the phone client (docs/STREAMING_PROTOCOL.md).
//
// The first half of this file is DOM-free and exported so Node can test it (tests/js/).
// The UI only boots when a document exists. The app serves a fixed list of static files,
// so the protocol helpers live here and in crypto.js rather than in extra modules.

import {
  fromHex, toHex, newNonce, uuidv4, parsePairingCode, buildPairRequest, acceptPairResponse,
  signRequest, verifyResponse, base32Encode, randomBytes,
} from "./crypto.js";

// =======================================================================================
// Pure helpers (exported for tests)

export const STORAGE_KEY = "explicitly.remote.pairing";
export const DEVICE_ID_KEY = "explicitly.remote.deviceId";
export const NAME_KEY = "explicitly.remote.name";
export const RECENT_KEY = "explicitly.remote.recent";

/** Read `pair` and `host` from a location.hash such as "#pair=ABC…&host=xyz". */
export function parseFragment(hash) {
  const p = new URLSearchParams(String(hash || "").replace(/^#/, ""));
  return { pair: p.get("pair") || "", host: p.get("host") || "" };
}

/** Format a pairing code for display in groups of 4 (ABCD-EFGH-…). */
export function formatPairingCode(code) {
  const clean = String(code || "").toUpperCase().replace(/[^A-Z2-7]/g, "");
  return clean.replace(/(.{4})(?=.)/g, "$1-");
}

const SUPPORTED = [
  /^https:\/\/www\.youtube\.com\/watch\?(?:.*&)?v=[\w-]+/,
  /^https:\/\/m\.youtube\.com\/watch\?(?:.*&)?v=[\w-]+/,
  /^https:\/\/youtu\.be\/[\w-]+/,
  /^https:\/\/www\.youtube\.com\/shorts\/[\w-]+/,
];

/** True when the PC will accept this URL for {t:"load"} (protocol §4.1). */
export function isSupportedUrl(url) {
  return typeof url === "string" && SUPPORTED.some((re) => re.test(url));
}

/**
 * Turn what the user pasted into a URL the PC accepts, or null.
 * Adds https://, upgrades http, and maps youtube.com / music.youtube.com to www.
 */
export function normalizeVideoUrl(input) {
  let s = String(input || "").trim();
  if (!s) return null;
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(s)) s = "https://" + s;
  let u;
  try { u = new URL(s); } catch { return null; }
  if (u.protocol !== "https:" && u.protocol !== "http:") return null;
  u.protocol = "https:";
  const host = u.hostname.toLowerCase();
  if (host === "youtube.com" || host === "music.youtube.com") u.hostname = "www.youtube.com";
  else if (host === "www.youtu.be") u.hostname = "youtu.be";
  else u.hostname = host;
  u.port = "";
  const out = u.toString();
  return isSupportedUrl(out) ? out : null;
}

/** Pick the video URL from an Android share (url, text and title params). */
export function extractSharedUrl({ url, text, title } = {}) {
  for (const candidate of [url, text, title]) {
    if (!candidate) continue;
    const direct = normalizeVideoUrl(candidate);
    if (direct) return direct;
    const found = String(candidate).match(/https?:\/\/\S+/g) || [];
    for (const f of found) {
      const n = normalizeVideoUrl(f.replace(/[)\].,;!?'"]+$/, ""));
      if (n) return n;
    }
  }
  return null;
}

/** 75.4 → "1:15", 3725 → "1:02:05". */
export function formatTime(sec) {
  if (!Number.isFinite(sec) || sec < 0) sec = 0;
  const s = Math.floor(sec);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
  const ss = String(r).padStart(2, "0");
  return h > 0 ? `${h}:${String(m).padStart(2, "0")}:${ss}` : `${m}:${ss}`;
}

/** Results may send duration as seconds or as text ("4:13"). */
export function formatDuration(d) {
  if (typeof d === "number") return d > 0 ? formatTime(d) : "";
  return typeof d === "string" ? d.trim() : "";
}

/** The mode badge text for a state message. */
export function modeLabel(state) {
  if (!state || !state.mode) return "";
  if (state.mode === "delayed") {
    const ms = state.filter && Number.isFinite(state.filter.delayMs) ? state.filter.delayMs : 3500;
    return `Filtering · ${(ms / 1000).toFixed(1)} s delay`;
  }
  if (state.mode === "caption") return "Caption mute · no delay";
  if (state.mode === "live") return "Filter off";
  return "";
}

const ERROR_TEXT = {
  "protected": "This source is protected and can't be streamed.",
  "unsupported source": "Only YouTube links can be played for now (youtube.com/watch, youtu.be or Shorts).",
  "load failed": "The PC couldn't load that video.",
  "app offline": "Explicitly Desktop isn't running on your PC, so the video can't be filtered.",
};

/** Plain-language text for a {t:"error"} message. */
export function errorText(msg) {
  if (!msg) return "Something went wrong.";
  const base = ERROR_TEXT[msg.code];
  if (base && msg.code === "load failed" && msg.message) return `${base} (${msg.message})`;
  return base || msg.message || `Error: ${msg.code || "unknown"}`;
}

const ENDED_TEXT = {
  replaced: "Another phone started watching, so this one was disconnected.",
  revoked: "This phone was removed on the PC. Pair again to keep watching.",
  "host closed": "The PC closed the stream.",
};
export function endedText(reason) {
  return ENDED_TEXT[reason] || "The stream ended.";
}

/** A friendly default device name from the user agent. No "|" allowed (§1). */
export function defaultDeviceName(ua = "") {
  let name = "Phone";
  if (/iPad/.test(ua)) name = "iPad";
  else if (/iPhone/.test(ua)) name = "iPhone";
  else if (/Android/.test(ua)) {
    const m = ua.match(/Android [\d.]+; ([^;)]+?)(?: Build\/[^;)]*)?\)/);
    const model = m && m[1].trim();
    name = model && model !== "K" && !/^wv$/i.test(model) ? model : "Android phone";
  }
  return cleanName(name);
}

/** Device names: trimmed, no "|", at most 40 chars. */
export function cleanName(s) {
  return String(s || "").replace(/\|/g, "/").replace(/\s+/g, " ").trim().slice(0, 40);
}

/** DataChannel messages, phone → PC (§4.1). */
export const msg = {
  load: (url) => ({ t: "load", url }),
  play: () => ({ t: "play" }),
  pause: () => ({ t: "pause" }),
  seek: (time) => ({ t: "seek", time: Math.max(0, Number(time) || 0) }),
  volume: (value) => ({ t: "volume", value: Math.min(1, Math.max(0, Number(value) || 0)) }),
  filter: (on) => ({ t: "filter", on: !!on }),
  search: (q, id) => ({ t: "search", q: String(q), id: String(id) }),
  ping: (id) => ({ t: "ping", id: String(id) }),
};

/** Parse an incoming DataChannel payload. Returns null for anything that isn't a typed object. */
export function parseMessage(data) {
  if (typeof data !== "string") return null;
  try {
    const m = JSON.parse(data);
    return m && typeof m === "object" && typeof m.t === "string" ? m : null;
  } catch {
    return null;
  }
}

/** Tracks ping ids and turns pongs into a round-trip time. */
export class PingTracker {
  constructor(now = () => Date.now()) {
    this.now = now;
    this.pending = new Map();
    this.seq = 0;
    this.rtt = null;
  }
  next() {
    const id = `p${++this.seq}`;
    this.pending.set(id, this.now());
    // Forget pings that never came back.
    for (const [k, t] of this.pending) if (this.now() - t > 10000) this.pending.delete(k);
    return msg.ping(id);
  }
  pong(id) {
    const sent = this.pending.get(String(id));
    if (sent === undefined) return null;
    this.pending.delete(String(id));
    this.rtt = this.now() - sent;
    return this.rtt;
  }
  reset() { this.pending.clear(); this.rtt = null; }
}

export class ApiError extends Error {
  constructor(kind, status, message) {
    super(message);
    this.kind = kind;     // "network", "unauthorized", "bad-mac", "http"
    this.status = status;
  }
}

/**
 * POST a signed request (§3) and verify the reply's X-Mac.
 * A 2xx reply must carry a valid MAC. An error reply with a MAC must also verify; an
 * error reply without one (e.g. 401 for an unknown device, which the app can't sign) is
 * reported as an error and its body is used only for the message text.
 */
export async function signedPost(fetchImpl, pairing, path, body = {}, opts = {}) {
  const raw = JSON.stringify(body);
  const key = fromHex(pairing.key);
  const { headers, nonce } = signRequest(key, pairing.deviceId, "POST", path, raw, opts.time, opts.nonce);
  let res;
  try {
    res = await fetchImpl(path, {
      method: "POST",
      headers: { ...headers, "Content-Type": "application/json" },
      body: raw,
      cache: "no-store",
      keepalive: !!opts.keepalive,
      signal: opts.signal,
    });
  } catch (e) {
    if (e && e.name === "AbortError") throw e;
    throw new ApiError("network", 0, "Can't reach your PC. Is it on the same Wi-Fi?");
  }
  const text = await res.text();
  const mac = res.headers.get("X-Mac");
  const ok = res.status >= 200 && res.status < 300;
  if (ok || mac) {
    if (!mac || !verifyResponse(key, path, nonce, text, mac)) {
      throw new ApiError("bad-mac", res.status, "The PC's reply failed verification and was ignored.");
    }
  }
  let data = {};
  if (text) {
    try { data = JSON.parse(text); } catch {
      if (ok) throw new ApiError("http", res.status, "The PC sent an unreadable reply.");
    }
  }
  if (res.status === 401) throw new ApiError("unauthorized", 401, data.error || "This PC no longer recognizes this phone.");
  if (!ok) {
    let message = data.error || `The PC answered ${res.status}.`;
    if (res.status === 409) message = "Your PC's browser isn't available. Open Chrome with the Explicitly Video Sync extension on the PC.";
    if (res.status === 504) message = "Your PC didn't answer in time.";
    throw new ApiError("http", res.status, message);
  }
  return data;
}

/**
 * Pairing request (§2). Returns the stored pairing record {hostId, hostName, origin, deviceId, key}.
 */
export async function pairWithHost(fetchImpl, { code, deviceId, name, origin, expectedHostId, signal, nonce = newNonce() }) {
  const P = parsePairingCode(code);
  const body = buildPairRequest(P, deviceId, cleanName(name) || "Phone", nonce);
  let res;
  try {
    res = await fetchImpl("/api/pair", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
      cache: "no-store",
      signal,
    });
  } catch (e) {
    if (e && e.name === "AbortError") throw e;
    throw new ApiError("network", 0, "Can't reach your PC. Check the address and that both are on the same Wi-Fi.");
  }
  const text = await res.text();
  let data = {};
  try { data = text ? JSON.parse(text) : {}; } catch { /* handled below */ }
  if (res.status === 403) {
    if (data.error === "denied") throw new ApiError("http", 403, "Pairing wasn't allowed on the PC (declined or timed out).");
    throw new ApiError("http", 403, "That code has expired or is wrong. Press Pair a phone on your PC to get a new one.");
  }
  if (res.status !== 200) throw new ApiError("http", res.status, data.error || `Pairing failed (${res.status}).`);
  const r = acceptPairResponse(P, deviceId, body.nonce, data);
  if (expectedHostId && r.hostId !== expectedHostId) {
    throw new ApiError("bad-mac", 200, "The PC that answered isn't the one in the QR code.");
  }
  P.fill(0);
  return { hostId: r.hostId, hostName: r.hostName, origin, deviceId, key: toHex(r.key) };
}

/** Exponential backoff for reconnect attempt n (1-based): 1 s, 2 s, 4 s. */
export function backoffMs(attempt) {
  return 1000 * 2 ** Math.max(0, attempt - 1);
}

// =======================================================================================
// UI

if (typeof document !== "undefined" && typeof window !== "undefined") {
  boot();
}

function boot() {
  const $ = (id) => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  const DEMO = params.get("demo") === "1";

  const store = {
    get(k, fallback = null) {
      try { const v = localStorage.getItem(k); return v === null ? fallback : JSON.parse(v); } catch { return fallback; }
    },
    set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* private mode */ } },
    del(k) { try { localStorage.removeItem(k); } catch { /* ignore */ } },
  };

  // ---------- State ----------
  let pairing = DEMO ? null : store.get(STORAGE_KEY);
  let screen = "pair";
  let lastScreen = "browse";
  let conn = "offline";
  let pc = null, dc = null, stream = null;
  let sessionId = null;
  let gen = 0;                 // bumps on every new connection; stale callbacks check it
  let reconnecting = false;
  let disconnectTimer = null, pingTimer = null;
  let noRetryReason = null;    // set by {t:"ended"} so we don't auto-reconnect
  let pendingLoad = null;
  let state = null;            // last {t:"state"}
  let searchSeq = 0, searchId = null;
  let seekHoldUntil = 0, seekDragging = false, volDragging = false, volTimer = null;
  let pairAbort = null;
  let videoError = "";
  let demoRecent = [];
  const pings = new PingTracker();

  // ---------- Generic UI ----------
  function show(name) {
    if (name !== "pair") lastScreen = name;
    screen = name;
    for (const s of ["pair", "browse", "player"]) $(`${s}Screen`).hidden = s !== name;
    const paired = name !== "pair";
    $("connBadge").hidden = !paired || name === "player";
    $("menuBtn").hidden = !paired;
    $("backBtn").hidden = name !== "player";
    document.querySelector(".logo").hidden = name === "player";
    // Once paired, the PC's name is the title and the product name the subtitle.
    $("brandName").textContent = paired ? hostLabel() || "Your PC" : "Explicitly Remote";
    $("hostName").textContent = paired ? "Explicitly Remote" : "";
    if (name === "browse") renderBrowse();
    if (name === "player") renderPlayer();
  }

  function hostLabel() {
    if (DEMO) return "Living-room PC";
    return pairing ? pairing.hostName || "Your PC" : "";
  }

  function notice(text, kind = "error") {
    if (!text) { $("notice").hidden = true; return; }
    $("noticeText").textContent = text;
    $("notice").className = "notice" + (kind === "info" ? " info" : "");
    $("notice").hidden = false;
  }
  $("noticeClose").onclick = () => notice(null);

  function setConn(next) {
    conn = next;
    const labels = { connecting: "Connecting", connected: "Connected", reconnecting: "Reconnecting", offline: "Offline" };
    let text = labels[conn] || conn;
    if (conn === "connected" && pings.rtt !== null) text += ` · ${pings.rtt} ms`;
    for (const [b, t] of [["connBadge", "connText"], ["connBadge2", "connText2"]]) {
      $(b).dataset.state = conn;
      $(t).textContent = text;
    }
    renderPlayer();
  }

  // ---------- Pair screen ----------
  function showPair(message, fragment = {}) {
    stopSession();
    show("pair");
    $("pairWaiting").hidden = true;
    $("pairForm").hidden = false;
    $("pairHost").value = location.host;
    $("pairName").value = store.get(NAME_KEY) || defaultDeviceName(navigator.userAgent);
    if (fragment.pair) {
      $("pairCode").value = formatPairingCode(fragment.pair);
      $("pairLead").textContent = "Got the code from your PC. Check the name for this phone, then press Pair.";
    }
    $("pairForm").dataset.expectedHost = fragment.host || "";
    pairError(message || "");
  }

  function pairError(text) {
    $("pairError").textContent = text;
    $("pairError").hidden = !text;
  }

  $("pairCode").addEventListener("input", (e) => {
    const el = e.target;
    const atEnd = el.selectionStart === el.value.length;
    const f = formatPairingCode(el.value);
    if (atEnd && f !== el.value) el.value = f;
  });

  $("pairForm").addEventListener("submit", async (e) => {
    e.preventDefault();
    pairError("");
    const code = $("pairCode").value;
    try { parsePairingCode(code); } catch (err) { pairError(err.message); return; }
    const name = cleanName($("pairName").value) || defaultDeviceName(navigator.userAgent);

    // A different PC address means a different origin: go there and pair on that page.
    const typedHost = $("pairHost").value.trim().replace(/^https?:\/\//i, "").replace(/\/.*$/, "");
    if (typedHost && typedHost.toLowerCase() !== location.host.toLowerCase()) {
      const hostPart = /:\d+$/.test(typedHost) ? typedHost : `${typedHost}:8767`;
      if (!/^[\w.-]+(:\d+)?$/.test(hostPart) && !/^\[[0-9a-f:]+\](:\d+)?$/i.test(hostPart)) {
        pairError("That PC address doesn't look right (example: 192.168.1.20:8767).");
        return;
      }
      store.set(NAME_KEY, name);
      location.href = `http://${hostPart}/#pair=${encodeURIComponent(code.replace(/[\s-]/g, "").toUpperCase())}`;
      return;
    }

    store.set(NAME_KEY, name);
    $("pairForm").hidden = true;
    $("pairWaiting").hidden = false;
    if (DEMO) return;
    pairAbort = new AbortController();
    const timeout = setTimeout(() => pairAbort && pairAbort.abort(), 130000);
    try {
      let deviceId = store.get(DEVICE_ID_KEY);
      if (!deviceId) { deviceId = uuidv4(); store.set(DEVICE_ID_KEY, deviceId); }
      const rec = await pairWithHost(fetch.bind(window), {
        code, deviceId, name, origin: location.origin,
        expectedHostId: $("pairForm").dataset.expectedHost || "",
        signal: pairAbort.signal,
      });
      pairing = rec;
      store.set(STORAGE_KEY, rec);
      $("pairCode").value = "";
      notice(`Paired with ${rec.hostName || "your PC"}.`, "info");
      setTimeout(() => notice(null), 4000);
      show("browse");
      startSession();
    } catch (err) {
      $("pairForm").hidden = false;
      $("pairWaiting").hidden = true;
      pairError(err.name === "AbortError" ? "Pairing was cancelled or timed out." : err.message);
    } finally {
      clearTimeout(timeout);
      pairAbort = null;
    }
  });

  $("pairCancel").onclick = () => {
    if (pairAbort) pairAbort.abort();
    $("pairForm").hidden = false;
    $("pairWaiting").hidden = true;
  };

  // ---------- API ----------
  function api(path, body = {}, opts = {}) {
    return signedPost(fetch.bind(window), pairing, path, body, opts);
  }

  function forgetLocal(message) {
    pairing = null;
    store.del(STORAGE_KEY);
    showPair(message);
  }

  // ---------- WebRTC session ----------
  function waitIceGathering(peer, ms) {
    if (peer.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve) => {
      const done = () => { clearTimeout(t); peer.removeEventListener("icegatheringstatechange", check); resolve(); };
      const check = () => { if (peer.iceGatheringState === "complete") done(); };
      const t = setTimeout(done, ms);
      peer.addEventListener("icegatheringstatechange", check);
    });
  }

  function waitConnected(peer, ms) {
    return new Promise((resolve) => {
      if (peer.connectionState === "connected") return resolve(true);
      const t = setTimeout(() => { peer.removeEventListener("connectionstatechange", f); resolve(false); }, ms);
      const f = () => {
        if (peer.connectionState === "connected") { clearTimeout(t); peer.removeEventListener("connectionstatechange", f); resolve(true); }
        else if (peer.connectionState === "failed" || peer.connectionState === "closed") { clearTimeout(t); peer.removeEventListener("connectionstatechange", f); resolve(false); }
      };
      peer.addEventListener("connectionstatechange", f);
    });
  }

  async function negotiate(peer, myGen, iceRestart = false) {
    const offer = await peer.createOffer(iceRestart ? { iceRestart: true } : undefined);
    await peer.setLocalDescription(offer);
    await waitIceGathering(peer, 2000);
    if (myGen !== gen) throw new Error("stale");
    const ans = await api("/api/session/offer", { sdp: peer.localDescription.sdp });
    if (myGen !== gen) throw new Error("stale");
    if (!ans || typeof ans.sdp !== "string") throw new ApiError("http", 200, "The PC sent no answer.");
    sessionId = ans.sessionId || null;
    await peer.setRemoteDescription({ type: "answer", sdp: ans.sdp });
  }

  function closePeer() {
    clearInterval(pingTimer); pingTimer = null;
    clearTimeout(disconnectTimer); disconnectTimer = null;
    if (dc) { dc.onopen = dc.onclose = dc.onmessage = null; try { dc.close(); } catch { /* */ } }
    if (pc) { pc.onconnectionstatechange = pc.ontrack = null; try { pc.close(); } catch { /* */ } }
    dc = null; pc = null;
    pings.reset();
  }

  function newPeer() {
    const peer = new RTCPeerConnection({ iceServers: [] });
    peer.addTransceiver("video", { direction: "recvonly" });
    peer.addTransceiver("audio", { direction: "recvonly" });
    const channel = peer.createDataChannel("control", { ordered: true });
    peer.ontrack = (ev) => attachTrack(ev.track);
    peer.onconnectionstatechange = () => { if (peer === pc) onPeerState(peer); };
    wireChannel(channel);
    pc = peer; dc = channel;
    return peer;
  }

  function attachTrack(track) {
    const video = $("video");
    if (!stream) stream = new MediaStream();
    for (const old of stream.getTracks()) if (old.kind === track.kind && old !== track) stream.removeTrack(old);
    if (!stream.getTracks().includes(track)) stream.addTrack(track);
    if (video.srcObject !== stream) video.srcObject = stream;
    video.muted = false;
    tryPlay();
  }

  function tryPlay() {
    const video = $("video");
    if (!video.srcObject) return;
    const p = video.play();
    if (p && p.catch) {
      p.then(() => { $("tapStart").hidden = true; })
       .catch(() => { $("tapStart").hidden = false; });
    }
  }
  $("tapStart").onclick = () => {
    const video = $("video");
    video.muted = false;
    if (DEMO) { $("tapStart").hidden = true; return; }
    video.play().then(() => { $("tapStart").hidden = true; }).catch(() => { $("tapStart").hidden = false; });
  };
  $("video").addEventListener("pause", () => {
    // The OS can pause a remote stream (e.g. iOS interruptions); offer to restart it.
    if ($("video").srcObject && conn === "connected") $("tapStart").hidden = false;
  });

  function onPeerState(peer) {
    const s = peer.connectionState;
    if (s === "connected") {
      clearTimeout(disconnectTimer); disconnectTimer = null;
      if (dc && dc.readyState === "open") setConn("connected");
    } else if (s === "failed") {
      clearTimeout(disconnectTimer); disconnectTimer = null;
      scheduleReconnect();
    } else if (s === "disconnected") {
      if (!disconnectTimer) {
        disconnectTimer = setTimeout(() => {
          disconnectTimer = null;
          if (pc === peer && (peer.connectionState === "disconnected" || peer.connectionState === "failed")) scheduleReconnect();
        }, 3000);
      }
    }
  }

  function wireChannel(channel) {
    channel.onopen = () => {
      if (channel !== dc) return;
      setConn("connected");
      clearInterval(pingTimer);
      const ping = () => { if (channel.readyState === "open") send(pings.next()); };
      ping();
      pingTimer = setInterval(ping, 2000);
      if (pendingLoad) { const u = pendingLoad; pendingLoad = null; send(msg.load(u)); }
    };
    channel.onclose = () => {
      if (channel !== dc || noRetryReason) return;
      scheduleReconnect();
    };
    channel.onmessage = (ev) => { if (channel === dc) handleMessage(parseMessage(ev.data)); };
  }

  function send(m) {
    if (DEMO) return demoSend(m);
    if (!dc || dc.readyState !== "open") return false;
    try { dc.send(JSON.stringify(m)); return true; } catch { return false; }
  }

  async function startSession() {
    if (DEMO || !pairing) return;
    noRetryReason = null;
    reconnecting = false;
    const myGen = ++gen;
    closePeer();
    setConn("connecting");
    $("offlineBox").hidden = true;
    try {
      const peer = newPeer();
      await negotiate(peer, myGen);
      // ICE/DTLS that never completes (and never reports "failed") still gets retried.
      const ok = await waitConnected(peer, 15000);
      if (!ok && myGen === gen && pc === peer && conn !== "connected") scheduleReconnect();
    } catch (err) {
      if (myGen !== gen || err.message === "stale") return;
      handleSessionError(err);
    }
  }

  function handleSessionError(err) {
    if (err instanceof ApiError && err.kind === "unauthorized") {
      forgetLocal("This PC no longer recognizes this phone. Pair it again.");
      return;
    }
    goOffline(err.message || "Couldn't start the stream.");
  }

  function goOffline(text) {
    closePeer();
    reconnecting = false;
    setConn("offline");
    $("offlineText").textContent = text || "Can't reach your PC.";
    $("offlineBox").hidden = false;
    renderBrowse();
  }

  function scheduleReconnect() {
    if (reconnecting || noRetryReason || !pairing) return;
    reconnecting = true;
    reconnect(++gen);
  }

  async function reconnect(myGen) {
    setConn("reconnecting");
    for (let attempt = 1; attempt <= 3; attempt++) {
      await new Promise((r) => setTimeout(r, backoffMs(attempt)));
      if (myGen !== gen) return;
      try {
        let peer = pc;
        let ok = false;
        // Attempt 1: ICE restart on the existing connection. If the PC answered with a new
        // peer (new DTLS fingerprint), that fails and the later attempts start fresh.
        if (attempt === 1 && peer && peer.signalingState === "stable" && peer.connectionState !== "closed") {
          try {
            await negotiate(peer, myGen, true);
            ok = await waitConnected(peer, 6000) && !!dc && dc.readyState === "open";
          } catch (e) {
            if (e instanceof ApiError && (e.kind === "unauthorized" || e.kind === "bad-mac")) throw e;
            if (e.message === "stale") return;
            ok = false;
          }
        }
        if (!ok) {
          closePeer();
          peer = newPeer();
          await negotiate(peer, myGen);
          ok = await waitConnected(peer, 8000);
        }
        if (myGen !== gen) return;
        if (ok && dc && (dc.readyState === "closing" || dc.readyState === "closed")) ok = false;
        if (ok) {
          reconnecting = false;
          // A fresh channel may open just after the peer connects; its onopen sets "connected".
          if (dc && dc.readyState === "open") setConn("connected");
          return;
        }
      } catch (err) {
        if (myGen !== gen || err.message === "stale") return;
        if (err instanceof ApiError && err.kind === "unauthorized") { reconnecting = false; handleSessionError(err); return; }
        if (attempt === 3) { goOffline(err.message); return; }
      }
    }
    if (myGen === gen) goOffline("Lost the connection to your PC.");
  }

  function stopSession(hangup = true) {
    gen++;
    if (hangup && sessionId && pairing && !DEMO) {
      api("/api/session/hangup", { sessionId }, { keepalive: true }).catch(() => {});
    }
    sessionId = null;
    closePeer();
    reconnecting = false;
    setConn("offline");
  }

  // ---------- Messages from the PC ----------
  function handleMessage(m) {
    if (!m) return;
    switch (m.t) {
      case "state":
        onState(m);
        break;
      case "results":
        if (String(m.id) === searchId) renderResults(Array.isArray(m.items) ? m.items.slice(0, 20) : []);
        break;
      case "error":
        notice(errorText(m));
        if (m.code === "protected" || m.code === "unsupported source" || m.code === "load failed") {
          videoError = errorText(m);
          renderPlayer();
        }
        break;
      case "pong": {
        pings.pong(m.id);
        if (conn === "connected") setConn("connected");
        break;
      }
      case "ended":
        noRetryReason = m.reason || "ended";
        if (m.reason === "revoked") { forgetLocal(endedText("revoked")); return; }
        goOffline(endedText(m.reason));
        notice(endedText(m.reason), "info");
        break;
    }
  }

  function onState(s) {
    const prevUrl = state && state.url;
    state = s;
    if (s.url && s.url !== prevUrl) videoError = "";
    if (s.url && s.title) addRecent({ url: s.url, title: s.title, channel: s.channel || "", duration: s.duration });
    renderPlayer();
    renderNowBar();
  }

  // ---------- Browse ----------
  $("searchForm").addEventListener("submit", (e) => {
    e.preventDefault();
    const q = $("searchInput").value.trim();
    if (!q) return;
    $("searchInput").blur();
    searchId = `s${++searchSeq}`;
    $("resultsHead").hidden = false;
    $("resultsTitle").textContent = `Results for “${q}”`;
    $("resultsList").replaceChildren();
    if (!send(msg.search(q, searchId))) {
      resultsStatus("Connect to your PC to search.");
      return;
    }
    resultsStatus("Searching on your PC…");
  });

  function resultsStatus(text) {
    $("resultsStatus").textContent = text || "";
    $("resultsStatus").hidden = !text;
  }

  $("urlForm").addEventListener("submit", (e) => {
    e.preventDefault();
    const url = normalizeVideoUrl($("urlInput").value);
    if (!url) {
      $("urlError").textContent = "Paste a YouTube link (youtube.com/watch, youtu.be or Shorts).";
      $("urlError").hidden = false;
      return;
    }
    $("urlError").hidden = true;
    $("urlInput").value = "";
    $("urlInput").blur();
    playUrl(url);
  });
  $("urlInput").addEventListener("input", () => { $("urlError").hidden = true; });

  function playUrl(url, meta = {}) {
    videoError = "";
    if (!send(msg.load(url))) pendingLoad = url;
    if (meta.title) addRecent({ url, ...meta });
    if (!state || state.url !== url) {
      state = { ...(state || {}), url, title: meta.title || "Loading…", channel: meta.channel || "", time: 0, duration: 0, buffering: true };
    }
    show("player");
    tryPlay();
  }

  function itemRow(item, compact) {
    const li = document.createElement("li");
    const b = document.createElement("button");
    b.type = "button";
    b.className = "item";
    const th = document.createElement("div");
    th.className = "thumb";
    if (item.thumb && /^(https:|data:image\/)/.test(item.thumb)) {
      const img = document.createElement("img");
      img.alt = "";
      img.loading = "lazy";
      img.referrerPolicy = "no-referrer";
      img.src = item.thumb;
      img.onerror = () => img.remove();
      th.append(img);
    }
    const d = formatDuration(item.duration);
    if (d) { const s = document.createElement("span"); s.className = "dur"; s.textContent = d; th.append(s); }
    const tx = document.createElement("div");
    tx.className = "item-text";
    const t = document.createElement("div"); t.className = "item-title"; t.textContent = item.title || item.url;
    const c = document.createElement("div"); c.className = "item-sub"; c.textContent = item.channel || "";
    tx.append(t, c);
    b.append(th, tx);
    b.onclick = () => playUrl(item.url, { title: item.title, channel: item.channel, thumb: item.thumb, duration: item.duration });
    li.append(b);
    if (compact) li.dataset.compact = "1";
    return li;
  }

  function renderResults(items) {
    resultsStatus(items.length ? "" : "No results.");
    $("resultsList").replaceChildren(...items.filter((i) => i && i.url).map((i) => itemRow(i)));
    renderBrowseEmpty();
  }

  function getRecent() { const r = store.get(RECENT_KEY, []); return Array.isArray(r) ? r : []; }
  function addRecent(item) {
    if (DEMO) return;
    const list = getRecent().filter((r) => r.url !== item.url);
    const prev = getRecent().find((r) => r.url === item.url) || {};
    list.unshift({ url: item.url, title: item.title, channel: item.channel || prev.channel || "", thumb: item.thumb || prev.thumb || "", duration: item.duration || prev.duration || "" });
    store.set(RECENT_KEY, list.slice(0, 12));
    if (screen === "browse") renderRecent();
  }
  $("recentClear").onclick = () => { store.del(RECENT_KEY); renderRecent(); renderBrowseEmpty(); };

  function renderRecent() {
    const list = DEMO ? demoRecent : getRecent();
    $("recentHead").hidden = !list.length;
    $("recentList").replaceChildren(...list.map((i) => itemRow(i, true)));
  }

  function renderBrowseEmpty() {
    $("browseEmpty").hidden = !!($("resultsList").children.length || $("recentList").children.length || !$("resultsHead").hidden);
  }

  function renderNowBar() {
    const has = !!(state && state.url);
    $("nowBar").hidden = !has;
    if (has) $("nowTitle").textContent = state.title || state.url;
  }
  $("nowBar").onclick = () => show("player");
  $("nowBar").onkeydown = (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); show("player"); } };

  function renderBrowse() {
    renderRecent();
    renderNowBar();
    renderBrowseEmpty();
  }

  // ---------- Player ----------
  function setRangeFill(el) {
    const max = Number(el.max) || 0;
    const pct = max > 0 ? (Number(el.value) / max) * 100 : 0;
    el.style.setProperty("--pct", `${Math.min(100, Math.max(0, pct))}%`);
  }

  function renderPlayer() {
    if (screen !== "player") return;
    const s = state || {};
    const online = conn === "connected";
    $("playerTitle").textContent = s.title || (s.url ? s.url : "Nothing playing");
    $("playerChannel").textContent = s.channel || "";
    $("adLabel").hidden = !s.ad;

    const label = modeLabel(s);
    $("modeBadge").hidden = !label;
    $("modeBadge").textContent = label;
    $("modeBadge").dataset.mode = s.mode || "none";

    $("bufferBadge").hidden = !s.buffering;
    $("bufferSpin").hidden = !(s.buffering && online);

    const pb = $("playBtn");
    const paused = s.paused !== false;
    pb.dataset.icon = paused ? "play" : "pause";
    pb.setAttribute("aria-label", paused ? "Play" : "Pause");
    pb.querySelector("use").setAttribute("href", paused ? "#i-play" : "#i-pause");
    pb.disabled = !online || !s.url;

    const dur = Number(s.duration) || 0;
    const seek = $("seekBar");
    seek.disabled = !online || !(dur > 0);
    seek.max = String(dur);
    if (!seekDragging && Date.now() > seekHoldUntil) seek.value = String(Math.min(dur, Number(s.time) || 0));
    $("timeCur").textContent = formatTime(Number(seek.value));
    $("timeDur").textContent = formatTime(dur);
    setRangeFill(seek);

    const vol = $("volumeBar");
    vol.disabled = !online;
    if (!volDragging && Number.isFinite(s.volume)) vol.value = String(s.volume);
    setRangeFill(vol);

    const f = $("filterSwitch");
    f.disabled = !online;
    if (s.filter) f.checked = !!s.filter.enabled;
    $("filterSub").textContent = s.filter && s.filter.enabled === false
      ? "Off: you'll hear everything, with no delay"
      : (s.filter && s.filter.running === false ? "On, but Explicitly Desktop's filter isn't running" : "Mutes swearing before you hear it");

    let vm = videoError;
    if (!vm && !DEMO) {
      if (conn === "offline") vm = "Not connected";
      else if (conn !== "connected" && !$("video").srcObject) vm = "Connecting to your PC…";
      else if (!s.url) vm = "Pick a video to play";
    }
    $("videoMsg").textContent = vm;
    $("videoMsg").hidden = !vm;
    $("offlineBox").hidden = conn !== "offline" || $("offlineText").textContent === "";
  }

  $("playBtn").onclick = () => {
    const paused = !state || state.paused !== false;
    send(paused ? msg.play() : msg.pause());
    if (state) { state.paused = !paused; renderPlayer(); }
    tryPlay();
  };

  const seek = $("seekBar");
  seek.addEventListener("pointerdown", () => { seekDragging = true; });
  seek.addEventListener("input", () => {
    seekDragging = true;
    $("timeCur").textContent = formatTime(Number(seek.value));
    setRangeFill(seek);
  });
  seek.addEventListener("change", () => {
    seekDragging = false;
    seekHoldUntil = Date.now() + 1500;
    send(msg.seek(Number(seek.value)));
    if (state) { state.time = Number(seek.value); state.buffering = state.mode === "delayed"; renderPlayer(); }
  });
  seek.addEventListener("pointercancel", () => { seekDragging = false; });

  const vol = $("volumeBar");
  vol.addEventListener("pointerdown", () => { volDragging = true; });
  vol.addEventListener("input", () => {
    volDragging = true;
    setRangeFill(vol);
    if (!volTimer) volTimer = setTimeout(() => { volTimer = null; send(msg.volume(Number(vol.value))); }, 120);
  });
  vol.addEventListener("change", () => {
    volDragging = false;
    clearTimeout(volTimer); volTimer = null;
    send(msg.volume(Number(vol.value)));
    if (state) state.volume = Number(vol.value);
  });

  $("filterSwitch").addEventListener("change", (e) => {
    const on = e.target.checked;
    send(msg.filter(on));
    if (state) { state.filter = { ...(state.filter || {}), enabled: on }; renderPlayer(); }
  });

  $("fullBtn").onclick = async () => {
    const wrap = $("videoWrap"), video = $("video");
    const fsEl = document.fullscreenElement || document.webkitFullscreenElement;
    if (fsEl) { (document.exitFullscreen || document.webkitExitFullscreen).call(document); return; }
    const req = wrap.requestFullscreen || wrap.webkitRequestFullscreen;
    if (req && (document.fullscreenEnabled || document.webkitFullscreenEnabled)) {
      try {
        await req.call(wrap);
        const so = window.screen && window.screen.orientation;
        if (so && so.lock) so.lock("landscape").catch(() => {});
      } catch { /* ignore */ }
    } else if (video.webkitEnterFullscreen) {
      try { video.webkitEnterFullscreen(); } catch { /* iOS needs the video to have frames */ }
    }
  };

  $("backBtn").onclick = () => show("browse");
  $("retryBtn").onclick = () => { $("offlineText").textContent = ""; startSession(); };

  // ---------- Menu ----------
  $("menuBtn").onclick = () => {
    $("factHost").textContent = hostLabel() || "–";
    $("factOrigin").textContent = DEMO ? "192.168.1.20:8767" : (pairing && pairing.origin ? pairing.origin.replace(/^https?:\/\//, "") : location.host);
    $("factDevice").textContent = store.get(NAME_KEY) || defaultDeviceName(navigator.userAgent);
    $("sheet").hidden = false;
  };
  $("sheetClose").onclick = () => { $("sheet").hidden = true; };
  $("sheet").onclick = (e) => { if (e.target === $("sheet")) $("sheet").hidden = true; };
  $("reconnectBtn").onclick = () => { $("sheet").hidden = true; startSession(); };
  $("forgetBtn").onclick = async () => {
    $("sheet").hidden = true;
    if (DEMO) { showPair(); return; }
    const p = pairing;
    stopSession();
    if (p) { try { await signedPost(fetch.bind(window), p, "/api/unpair", {}); } catch { /* removed locally anyway */ } }
    store.del(RECENT_KEY);
    forgetLocal("");
    notice("This PC was forgotten.", "info");
  };

  // ---------- Lifecycle ----------
  window.addEventListener("pagehide", () => {
    if (sessionId && pairing && !DEMO) api("/api/session/hangup", { sessionId }, { keepalive: true }).catch(() => {});
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible" && pairing && !DEMO && conn === "offline" && !noRetryReason && screen !== "pair") {
      startSession();
    }
  });

  if (!DEMO && window.isSecureContext && "serviceWorker" in navigator) {
    navigator.serviceWorker.register("/sw.js").catch(() => { /* optional */ });
  }

  // ---------- Start ----------
  const fragment = parseFragment(location.hash);
  const shared = extractSharedUrl({ url: params.get("url"), text: params.get("text"), title: params.get("title") });
  if (fragment.pair || params.has("url") || params.has("text")) {
    // Never leave the pairing code in the address bar or history.
    history.replaceState(null, "", location.pathname + (DEMO ? location.search : ""));
  }

  if (DEMO) { startDemo(params.get("screen") || "browse"); return; }

  // A QR scanned while this page is already open only changes the fragment.
  window.addEventListener("hashchange", () => {
    const f = parseFragment(location.hash);
    if (!f.pair) return;
    history.replaceState(null, "", location.pathname);
    showPair("", f);
  });

  if (fragment.pair) {
    showPair("", fragment);
  } else if (!pairing) {
    showPair("");
  } else {
    if (shared) pendingLoad = shared;
    show(shared ? "player" : "browse");
    if (shared) state = { url: shared, title: "Loading…", buffering: true };
    setConn("connecting");
    api("/api/hello", {})
      .then((h) => {
        if (h && h.hostName && h.hostName !== pairing.hostName) {
          pairing.hostName = h.hostName;
          store.set(STORAGE_KEY, pairing);
          $("brandName").textContent = hostLabel();
        }
        startSession();
      })
      .catch((err) => handleSessionError(err));
  }

  // =====================================================================================
  // Demo mode (?demo=1&screen=pair|browse|player): sample data, no network.

  function demoThumb(hue, label) {
    const c = document.createElement("canvas");
    c.width = 320; c.height = 180;
    const g = c.getContext("2d");
    const grad = g.createLinearGradient(0, 0, 320, 180);
    grad.addColorStop(0, `hsl(${hue} 60% 38%)`);
    grad.addColorStop(1, `hsl(${(hue + 50) % 360} 55% 16%)`);
    g.fillStyle = grad; g.fillRect(0, 0, 320, 180);
    g.fillStyle = "rgba(255,255,255,.85)";
    g.font = "bold 30px system-ui, sans-serif";
    g.fillText(label, 20, 160);
    return c.toDataURL("image/png");
  }

  function demoPoster() {
    const c = document.createElement("canvas");
    c.width = 1280; c.height = 720;
    const g = c.getContext("2d");
    const grad = g.createLinearGradient(0, 0, 1280, 720);
    grad.addColorStop(0, "#14365f"); grad.addColorStop(1, "#3b1d0b");
    g.fillStyle = grad; g.fillRect(0, 0, 1280, 720);
    g.fillStyle = "rgba(251,140,0,.85)";
    g.beginPath(); g.arc(940, 260, 150, 0, Math.PI * 2); g.fill();
    g.fillStyle = "rgba(255,255,255,.08)";
    for (let i = 0; i < 9; i++) g.fillRect(0, 470 + i * 28, 1280, 12);
    g.fillStyle = "#fff"; g.font = "bold 64px system-ui, sans-serif";
    g.fillText("Mountain Bike Downhill", 70, 640);
    return c.toDataURL("image/jpeg", 0.85);
  }

  function demoSend(m) {
    if (!state) return true;
    if (m.t === "play") state.paused = false;
    if (m.t === "pause") state.paused = true;
    if (m.t === "seek") { state.time = m.time; state.buffering = state.mode === "delayed"; setTimeout(() => { state.buffering = false; renderPlayer(); }, 1500); }
    if (m.t === "volume") state.volume = m.value;
    if (m.t === "filter") {
      state.filter.enabled = m.on;
      state.mode = m.on ? "delayed" : "live";
    }
    if (m.t === "load") { state.url = m.url; }
    if (m.t === "search") setTimeout(() => renderResults(demoResults()), 300);
    renderPlayer();
    return true;
  }

  function demoResults() {
    return [
      { url: "https://www.youtube.com/watch?v=demo00001", title: "Top 10 Mountain Bike Downhill Runs of the Season (Unbelievable Crashes Included)", channel: "Trail Vision", duration: "12:48", thumb: demoThumb(205, "MTB") },
      { url: "https://www.youtube.com/watch?v=demo00002", title: "Stand-up special: full set, live in Chicago", channel: "Comedy Central Stand-Up", duration: "58:02", thumb: demoThumb(20, "LIVE") },
      { url: "https://www.youtube.com/watch?v=demo00003", title: "How to fix a squeaky bike brake in 5 minutes", channel: "Park Tool", duration: "5:31", thumb: demoThumb(140, "FIX") },
      { url: "https://www.youtube.com/watch?v=demo00004", title: "Lo-fi beats to study to", channel: "Chill Hop", duration: "1:02:11", thumb: demoThumb(280, "LOFI") },
    ];
  }

  function startDemo(which) {
    demoRecent = [
      { url: "https://youtu.be/demo00005", title: "Gordon reviews the worst restaurant in town", channel: "Kitchen Nightmares", duration: "43:10", thumb: demoThumb(0, "COOK") },
      { url: "https://www.youtube.com/shorts/demo00006", title: "When the goalie forgets the ball is in play", channel: "Football Shorts", duration: "0:41", thumb: demoThumb(100, "GOAL") },
    ];
    state = {
      t: "state", url: "https://www.youtube.com/watch?v=demo00001",
      title: "Top 10 Mountain Bike Downhill Runs of the Season (Unbelievable Crashes Included)",
      channel: "Trail Vision", paused: false, time: 187.4, duration: 768, volume: 0.8,
      buffering: false, mode: "delayed", filter: { running: true, enabled: true, delayMs: 3500 }, ad: false,
    };
    if (params.get("ad") === "1") state.ad = true;
    if (params.get("buffering") === "1") state.buffering = true;
    pings.rtt = 23;
    if (which === "pair") {
      showPair("", { pair: base32Encode(randomBytes(16)), host: "demo" });
      return;
    }
    show(which === "player" ? "player" : "browse");
    setConn(params.get("conn") || "connected");
    if (which === "player") {
      $("video").poster = demoPoster();
      if (params.get("tap") === "1") $("tapStart").hidden = false;
    } else {
      $("searchInput").value = "mountain bike";
      $("resultsHead").hidden = false;
      $("resultsTitle").textContent = "Results for “mountain bike”";
      renderResults(demoResults());
      renderRecent();
      renderBrowseEmpty();
    }
    if (params.get("error")) {
      const m = { t: "error", code: params.get("error") };
      notice(errorText(m));
      if (which === "player") { videoError = errorText(m); renderPlayer(); }
    }
  }
}

// Keep the uuid helper reachable for tests that import app.js only.
export { uuidv4 };
