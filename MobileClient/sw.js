// Service worker: offline shell for the phone client.
// The app serves this page over plain http on the LAN, where browsers refuse service
// workers, so app.js registers it only in a secure context (https or localhost).
// API calls (/api/*) always go to the network and are never cached.

const CACHE = "explicitly-remote-v1";
const SHELL = [
  "/", "/index.html", "/app.js", "/crypto.js", "/style.css",
  "/manifest.json", "/icon-192.png", "/icon-512.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

self.addEventListener("fetch", (event) => {
  const req = event.request;
  const url = new URL(req.url);
  if (req.method !== "GET" || url.origin !== self.location.origin || url.pathname.startsWith("/api/")) return;
  // Network first (the PC is the source of truth), falling back to the cached shell.
  event.respondWith(
    fetch(req)
      .then((res) => {
        if (res.ok && SHELL.includes(url.pathname)) {
          const copy = res.clone();
          caches.open(CACHE).then((c) => c.put(url.pathname, copy));
        }
        return res;
      })
      .catch(async () => {
        const cached = await caches.match(url.pathname, { ignoreSearch: true });
        return cached || (req.mode === "navigate" ? caches.match("/index.html") : Response.error());
      }),
  );
});
