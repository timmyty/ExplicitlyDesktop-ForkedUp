// Talks to Explicitly Desktop's local control port on behalf of the popup and
// YouTube and Steam tabs. Runs in the extension context, so the app can tell it apart
// from web pages (which it refuses).

const API = "http://127.0.0.1:8766";
const api = globalThis.browser ?? globalThis.chrome;

const OFFLINE = { running: false, enabled: false, buffering: false, delayMs: 0, offline: true };

async function call(method, path) {
  try {
    const res = await fetch(API + path, { method, cache: "no-store" });
    if (!res.ok) return OFFLINE;
    return await res.json();
  } catch {
    return OFFLINE;
  }
}

api.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
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
  return false;
});
