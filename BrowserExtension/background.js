// Talks to Explicitly Desktop's local control port on behalf of the popup and
// YouTube and Steam tabs. Runs in the extension context, so the app can tell it apart
// from web pages (which it refuses).
//
// Phone streaming (docs/STREAMING_PROTOCOL.md): this side also long-polls the
// app for the phone's offers, keeps one YouTube "host tab" for them, and relays
// the app's filtered PCM to that tab. All loopback I/O happens here because a
// content script's requests carry the page's origin, which the app refuses.

const API = "http://127.0.0.1:8766";
const PCM_URL = "ws://127.0.0.1:8766/pcm";
const HOST_URL = "https://www.youtube.com/";
const api = globalThis.browser ?? globalThis.chrome;

if (typeof importScripts === "function" && !globalThis.ExplicitlyRemoteCore) importScripts("remote-core.js");
const core = globalThis.ExplicitlyRemoteCore;

const OFFLINE = { running: false, enabled: false, buffering: false, delayMs: 0, offline: true };

async function call(method, path, body) {
  try {
    const init = { method, cache: "no-store" };
    if (body !== undefined) {
      init.body = JSON.stringify(body);
      init.headers = { "Content-Type": "application/json" };
    }
    const res = await fetch(API + path, init);
    if (!res.ok) return OFFLINE;
    return await res.json();
  } catch {
    return OFFLINE;
  }
}

api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg?.type === "status") {
    call("GET", "/status").then(sendResponse);
    return true;
  }
  if (msg?.type === "toggle") {
    call("POST", "/filter/toggle").then(sendResponse);
    return true;
  }
  if (msg?.type === "bypass") {
    // A tab is muting a captioned video itself: keep the app's audio undelayed
    call("POST", "/bypass").then(sendResponse);
    return true;
  }
  if (msg?.type === "lexicon") {
    fetch(API + "/lexicon", { cache: "no-store" })
      .then((r) => (r.ok ? r.json() : []))
      .catch(() => [])
      .then(sendResponse);
    return true;
  }
  if (msg?.type === "remoteHello") {
    // Every YouTube tab's remote.js asks once; only the host tab goes active
    hostReady.then(() => sendResponse({ host: sender.tab?.id != null && sender.tab.id === host.tabId }));
    return true;
  }
  return false;
});

// ---- Phone streaming: host tab --------------------------------------------------
const host = {
  tabId: null,       // The tab this extension opened for the phone (kept across restarts)
  port: null,        // remote.js's Port while that tab is loaded
  waiters: [],       // Resolved when the port connects
  requests: new Map(),
  nextId: 1,
  session: null,     // { sessionId, deviceName } while the tab streams
};

const sessionStore = api.storage?.session;
const hostReady = (async () => {
  try { host.tabId = (await sessionStore?.get("hostTabId"))?.hostTabId ?? null; } catch {}
})();

function setHostTab(id) {
  host.tabId = id;
  sessionStore?.set({ hostTabId: id }).catch(() => {});
}

api.tabs?.onRemoved?.addListener((tabId) => {
  if (tabId === host.tabId) setHostTab(null);
});

async function ensureHostTab() {
  await hostReady;
  if (host.tabId != null) {
    try {
      const tab = await api.tabs.get(host.tabId);
      if (tab && /^https:\/\/(www|m)\.youtube\.com\//.test(tab.url || tab.pendingUrl || "")) return tab.id;
    } catch {}
    setHostTab(null);
  }
  // Its own window, behind the user's, so the phone's video doesn't take over
  // what they are doing. Not minimized: a hidden page may stop decoding video
  const opts = { url: HOST_URL, type: "normal", focused: false, state: "normal" };
  let win;
  try { win = await api.windows.create(opts); }
  catch { delete opts.focused; win = await api.windows.create(opts); }   // Firefox may refuse focused:false
  const tabId = win.tabs?.[0]?.id ?? (await api.tabs.query({ windowId: win.id }))[0]?.id;
  setHostTab(tabId);
  // In case its remote.js said hello before the id was stored
  setTimeout(() => api.tabs.sendMessage(tabId, { type: "remoteHost" }).catch(() => {}), 1500);
  return tabId;
}

function waitHostPort(ms) {
  if (host.port) return Promise.resolve(host.port);
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error("host tab not ready")), ms);
    host.waiters.push((port) => { clearTimeout(t); resolve(port); });
  });
}

function request(msg, ms) {
  return new Promise((resolve, reject) => {
    if (!host.port) return reject(new Error("host tab not ready"));
    const id = host.nextId++;
    const t = setTimeout(() => { host.requests.delete(id); reject(new Error("host tab timed out")); }, ms);
    host.requests.set(id, (reply) => { clearTimeout(t); resolve(reply); });
    host.port.postMessage({ ...msg, reqId: id });
  });
}

api.runtime.onConnect.addListener((port) => {
  if (port.name !== "explicitly-remote") return;
  if (port.sender?.tab?.id !== host.tabId) { port.disconnect(); return; }

  host.port?.disconnect();
  host.port = port;
  port.onMessage.addListener((msg) => onHostMessage(port, msg));
  port.onDisconnect.addListener(() => {
    if (host.port !== port) return;
    host.port = null;
    pcmStop();
    // The tab closed or reloaded mid-session: let the app unmute the speakers
    if (host.session) {
      call("POST", "/remote/state", { active: false, sessionId: host.session.sessionId });
      host.session = null;
    }
  });
  for (const w of host.waiters.splice(0)) w(port);
});

// Loopback calls the host tab may make for the phone
const RELAY_PATHS = new Set(["/filter/on", "/filter/off", "/flush", "/hold/on", "/hold/off", "/remote/state", "/bypass"]);

function onHostMessage(port, msg) {
  if (msg?.type === "reply") {
    host.requests.get(msg.reqId)?.(msg);
    host.requests.delete(msg.reqId);
  } else if (msg?.type === "api" && RELAY_PATHS.has(msg.path)) {
    call("POST", msg.path, msg.body).then((result) => port.postMessage({ type: "apiResult", id: msg.id, result }));
  } else if (msg?.type === "session") {
    host.session = msg.active ? { sessionId: msg.sessionId, deviceName: msg.deviceName } : null;
    if (msg.active) pcmStart(); else pcmStop();
  }
}

// ---- Phone streaming: signaling long poll ----------------------------------------
let polling = false;

async function remoteLoop() {
  if (polling) return;
  polling = true;
  try {
    for (;;) {
      let msg;
      try {
        const res = await fetch(API + "/remote/next", { cache: "no-store", signal: AbortSignal.timeout(35000) });
        if (!res.ok) throw new Error("http " + res.status);
        msg = await res.json();
        online = true;
      } catch {
        // App closed (or too old for streaming): try again in a while
        online = false;
        await new Promise((r) => setTimeout(r, 5000));
        continue;
      }
      if (msg?.type === "offer") handleOffer(msg);
      else if (msg?.type === "hangup") host.port?.postMessage({ type: "hangup", sessionId: msg.sessionId, reason: msg.reason });
    }
  } finally {
    polling = false;
  }
}

async function handleOffer(msg) {
  let reply;
  try {
    const tabId = await ensureHostTab();
    if (tabId == null) throw new Error("host browser not available");
    await waitHostPort(10000);
    reply = await request({ type: "offer", sessionId: msg.sessionId, deviceId: msg.deviceId, deviceName: msg.deviceName, sdp: msg.sdp }, 12000);
  } catch (e) {
    reply = { error: e.message || "host browser not available" };
  }
  const body = reply.sdp ? { sessionId: msg.sessionId, sdp: reply.sdp } : { sessionId: msg.sessionId, error: reply.error || "no answer" };
  await call("POST", "/remote/answer", body);
}

// The service worker stops after 30 s without extension events, which would
// end the long poll. While the app is up, a cheap API call every 20 s keeps it
// alive; an alarm restarts the loop if it stopped anyway
let online = false;
setInterval(() => { if (online || host.session) api.runtime.getPlatformInfo?.().catch?.(() => {}); }, 20000);
api.alarms?.create?.("explicitly-remote", { periodInMinutes: 0.5 });
api.alarms?.onAlarm?.addListener?.(() => remoteLoop());
remoteLoop();

// ---- Phone streaming: PCM relay ---------------------------------------------------
// Port messages are JSON in Chrome, so each frame goes as base64 (a 20 ms
// stereo frame is 3.9 kB; encoding takes ~0.02 ms). The arrival time lets the
// tab measure the relay delay
let pcmSocket = null;
let pcmWanted = false;

function pcmStart() {
  pcmWanted = true;
  if (pcmSocket) return;
  const ws = new WebSocket(PCM_URL);
  ws.binaryType = "arraybuffer";
  ws.onmessage = (e) => {
    if (!host.port || !(e.data instanceof ArrayBuffer)) return;
    const at = Date.now();
    host.port.postMessage({ type: "pcm", at, b64: core.bytesToBase64(new Uint8Array(e.data)) });
  };
  ws.onclose = () => {
    if (pcmSocket === ws) pcmSocket = null;
    if (pcmWanted) setTimeout(() => { if (pcmWanted) pcmStart(); }, 1000);
  };
  ws.onerror = () => {};
  pcmSocket = ws;
}

function pcmStop() {
  pcmWanted = false;
  const ws = pcmSocket;
  pcmSocket = null;
  ws?.close();
}
