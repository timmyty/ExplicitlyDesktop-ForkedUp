// The manifest lists every new file, keeps permissions minimal, and the
// scripts parse as classic scripts (content scripts can't be modules)
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const DIR = path.join(__dirname, "../../BrowserExtension");
const manifest = JSON.parse(fs.readFileSync(path.join(DIR, "manifest.json"), "utf8"));

test("manifest: version, files, web-accessible worklet", () => {
  assert.equal(manifest.version, "1.3.0");
  const files = new Set([manifest.background.service_worker, ...manifest.background.scripts,
    ...manifest.content_scripts.flatMap((c) => c.js), ...manifest.web_accessible_resources.flatMap((w) => w.resources)]);
  for (const f of files) assert.ok(fs.existsSync(path.join(DIR, f)), f);
  const remote = manifest.content_scripts.find((c) => c.js.includes("remote.js"));
  assert.deepEqual(remote.js, ["remote-core.js", "pcm-worklet.js", "remote.js"]);
  assert.ok(remote.matches.every((m) => /youtube\.com/.test(m)));
  assert.ok(manifest.web_accessible_resources.some((w) => w.resources.includes("pcm-worklet.js")));
  assert.deepEqual(manifest.background.scripts, ["remote-core.js", "background.js"]);   // Firefox
});

test("manifest: loopback is the only non-site host permission", () => {
  const hosts = manifest.host_permissions;
  assert.ok(hosts.includes("http://127.0.0.1:8766/*"));
  for (const h of hosts) assert.match(h, /^(http:\/\/127\.0\.0\.1:8766\/\*|\*:\/\/(\*\.youtube\.com|store\.steampowered\.com|steamcommunity\.com|\*\.steamstatic\.com)\/\*)$/);
  assert.deepEqual(manifest.permissions.sort(), ["alarms", "storage"]);
});

test("scripts compile as classic scripts", () => {
  for (const f of ["background.js", "content.js", "remote.js", "remote-core.js", "pcm-worklet.js", "yt_main.js", "popup.js"]) {
    assert.doesNotThrow(() => new vm.Script(fs.readFileSync(path.join(DIR, f), "utf8"), { filename: f }), f);
  }
});

test("remote.js stays idle outside a host tab: no-op without WebRTC", () => {
  // Firefox-like or old context: missing pieces must not throw
  const ctx = { window: {}, globalThis: null, console };
  ctx.window.top = ctx.window;
  ctx.globalThis = ctx;
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(path.join(DIR, "remote.js"), "utf8"), ctx);
});
