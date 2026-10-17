// The background's phone-streaming flow (BrowserExtension/background.js) with
// mocked extension APIs, fetch and WebSocket: offer -> host tab -> answer,
// PCM relay, loopback relay allowlist, hangup, tab gone.
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const core = require("../../BrowserExtension/remote-core.js");

// Objects from the vm context have its prototypes: compare as JSON
const same = (a, b) => assert.deepEqual(a === undefined ? a : JSON.parse(JSON.stringify(a)), b);

const SRC = fs.readFileSync(path.join(__dirname, "../../BrowserExtension/background.js"), "utf8");

function event() {
  const listeners = [];
  return { addListener: (f) => listeners.push(f), listeners };
}

function makePort(name, tabId) {
  const port = { name, sender: { tab: { id: tabId } }, sent: [], onMessage: event(), onDisconnect: event() };
  port.postMessage = (m) => port.sent.push(m);
  port.disconnect = () => { port.disconnected = true; };
  port.fromTab = (m) => port.onMessage.listeners.forEach((f) => f(m));
  port.close = () => port.onDisconnect.listeners.forEach((f) => f());
  return port;
}

function setup() {
  const calls = [];             // { method, path, body }
  const next = [];              // Queued /remote/next replies
  let waiting = null;
  const sockets = [];
  const created = [];
  const openTabs = new Set();
  const store = {};

  const fetch = async (url, init = {}) => {
    const p = new URL(url).pathname;
    const body = init.body ? JSON.parse(init.body) : undefined;
    calls.push({ method: init.method || "GET", path: p, body });
    const json = (v) => ({ ok: true, status: 200, json: async () => v });
    if (p === "/remote/next") {
      if (next.length) return json(next.shift());
      return new Promise((resolve) => { waiting = () => resolve(json(next.shift())); });
    }
    if (p === "/status") return json({ running: true, enabled: true, delayMs: 3500 });
    return json({ ok: true, path: p });
  };

  class WebSocket {
    constructor(url) { this.url = url; sockets.push(this); }
    close() { this.closed = true; }
  }

  const chrome = {
    runtime: { onMessage: event(), onConnect: event(), getPlatformInfo: async () => ({}) },
    storage: { session: { get: async (k) => ({ [k]: store[k] }), set: async (o) => Object.assign(store, o) } },
    tabs: {
      onRemoved: event(),
      get: async (id) => { if (!openTabs.has(id)) throw new Error("No tab"); return { id, url: "https://www.youtube.com/" }; },
      query: async () => [],
      sendMessage: async () => {},
    },
    windows: { create: async (opts) => { created.push(opts); const id = 100 + created.length; openTabs.add(id); return { id: 1, tabs: [{ id }] }; } },
    alarms: { create() {}, onAlarm: event() },
  };

  const ctx = {
    chrome, fetch, WebSocket, console, URL, AbortSignal, Date, Promise, Uint8Array, ArrayBuffer, JSON, Error, Map, Set,
    setTimeout, clearTimeout, setInterval: () => 0,
    importScripts: () => { ctx.ExplicitlyRemoteCore = core; },
  };
  ctx.globalThis = ctx;
  vm.createContext(ctx);
  vm.runInContext(SRC, ctx, { filename: "background.js" });

  return {
    chrome, calls, sockets, created, openTabs,
    push(msg) { next.push(msg); if (waiting) { const w = waiting; waiting = null; w(); } },
    message(msg, tabId) {
      return new Promise((resolve) => {
        const handled = chrome.runtime.onMessage.listeners[0](msg, { tab: { id: tabId } }, resolve);
        if (!handled) resolve(undefined);
      });
    },
    connect(tabId) {
      const port = makePort("explicitly-remote", tabId);
      chrome.runtime.onConnect.listeners[0](port);
      return port;
    },
  };
}

async function until(cond, ms = 3000) {
  const start = Date.now();
  while (!cond()) {
    if (Date.now() - start > ms) throw new Error("timed out");
    await new Promise((r) => setTimeout(r, 5));
  }
}

test("offer opens a host tab, forwards the offer and posts the answer", async () => {
  const bg = setup();
  bg.push({ type: "offer", sessionId: "s1", deviceId: "d1", deviceName: "Tim's phone", sdp: "v=0 offer" });
  await until(() => bg.created.length === 1);
  same(bg.created[0], { url: "https://www.youtube.com/", type: "normal", focused: false, state: "normal" });
  const tabId = 101;

  // Only the host tab is told it is the host
  same(await bg.message({ type: "remoteHello" }, 55), { host: false });
  same(await bg.message({ type: "remoteHello" }, tabId), { host: true });

  // A port from another tab is refused
  const stranger = bg.connect(55);
  assert.equal(stranger.disconnected, true);

  const port = bg.connect(tabId);
  await until(() => port.sent.some((m) => m.type === "offer"));
  const offer = port.sent.find((m) => m.type === "offer");
  assert.equal(offer.sdp, "v=0 offer");
  assert.equal(offer.sessionId, "s1");
  port.fromTab({ type: "reply", reqId: offer.reqId, sdp: "v=0 answer" });
  await until(() => bg.calls.some((c) => c.path === "/remote/answer"));
  same(bg.calls.find((c) => c.path === "/remote/answer").body, { sessionId: "s1", sdp: "v=0 answer" });

  // Session: PCM relay starts and frames reach the tab as base64
  port.fromTab({ type: "session", active: true, sessionId: "s1", deviceName: "Tim's phone" });
  assert.equal(bg.sockets.length, 1);
  const ws = bg.sockets[0];
  assert.equal(ws.url, "ws://127.0.0.1:8766/pcm");
  assert.equal(ws.binaryType, "arraybuffer");
  const frame = core.buildPcmFrame({ pcm: Int16Array.from({ length: 1920 }, (_, i) => i - 960) });
  ws.onmessage({ data: frame.buffer });
  const pcm = port.sent.find((m) => m.type === "pcm");
  assert.ok(Math.abs(pcm.at - Date.now()) < 1000);
  assert.ok(Buffer.from(core.base64ToBytes(pcm.b64)).equals(Buffer.from(frame)));

  // Loopback relay: allowed paths only
  port.fromTab({ type: "api", id: 9, path: "/hold/on" });
  port.fromTab({ type: "api", id: 10, path: "/debug/dump" });
  port.fromTab({ type: "api", id: 11, path: "/remote/state", body: { active: true, sessionId: "s1" } });
  await until(() => port.sent.filter((m) => m.type === "apiResult").length === 2);
  assert.ok(bg.calls.some((c) => c.method === "POST" && c.path === "/hold/on"));
  assert.ok(!bg.calls.some((c) => c.path === "/debug/dump"));
  same(bg.calls.find((c) => c.path === "/remote/state").body, { active: true, sessionId: "s1" });

  // Hangup is passed to the tab
  bg.push({ type: "hangup", sessionId: "s1", reason: "phone" });
  await until(() => port.sent.some((m) => m.type === "hangup"));

  // A second offer reuses the open tab
  bg.push({ type: "offer", sessionId: "s2", deviceId: "d1", deviceName: "Tim's phone", sdp: "v=0 offer2" });
  await until(() => port.sent.filter((m) => m.type === "offer").length === 2);
  assert.equal(bg.created.length, 1);
  const offer2 = port.sent.filter((m) => m.type === "offer")[1];
  port.fromTab({ type: "reply", reqId: offer2.reqId, error: "answer failed: x" });
  await until(() => bg.calls.filter((c) => c.path === "/remote/answer").length === 2);
  same(bg.calls.filter((c) => c.path === "/remote/answer")[1].body, { sessionId: "s2", error: "answer failed: x" });

  // The tab goes away mid-session: relay stops, app told the phone is gone
  port.close();
  assert.equal(ws.closed, true);
  await until(() => bg.calls.some((c) => c.path === "/remote/state" && c.body.active === false));
});

test("closed host tab: the next offer opens a new window", async () => {
  const bg = setup();
  bg.push({ type: "offer", sessionId: "a", deviceId: "d", sdp: "x" });
  await until(() => bg.created.length === 1);
  const port = bg.connect(101);
  await until(() => port.sent.some((m) => m.type === "offer"));
  port.fromTab({ type: "reply", reqId: port.sent[0].reqId, sdp: "y" });
  port.close();
  bg.openTabs.delete(101);
  bg.chrome.tabs.onRemoved.listeners[0](101);
  bg.push({ type: "offer", sessionId: "b", deviceId: "d", sdp: "x" });
  await until(() => bg.created.length === 2);
  same(await bg.message({ type: "remoteHello" }, 102), { host: true });
  same(await bg.message({ type: "remoteHello" }, 101), { host: false });
});

test("existing messages still work", async () => {
  const bg = setup();
  same(await bg.message({ type: "status" }, 1), { running: true, enabled: true, delayMs: 3500 });
  await bg.message({ type: "toggle" }, 1);
  await bg.message({ type: "bypass" }, 1);
  assert.ok(bg.calls.some((c) => c.method === "POST" && c.path === "/filter/toggle"));
  assert.ok(bg.calls.some((c) => c.method === "POST" && c.path === "/bypass"));
  assert.equal(await bg.message({ type: "unknown" }, 1), undefined);
});
