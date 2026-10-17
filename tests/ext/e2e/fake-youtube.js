// www.youtube.com for the e2e run (Chromium maps the name to 127.0.0.1): just
// enough of the DOM for the extension (ytd-app, #movie_player, the main
// <video>), YouTube-style SPA navigation on "yt-navigate", a results page with
// ytInitialData, and the test clip. The page reports its playback clock to the
// fake app, which plays the role of the real audio path.

const https = require("node:https");
const fs = require("node:fs");

const PAGE = `<!doctype html><html><head><meta charset="utf-8"><title>YouTube</title></head>
<body style="background:#000;color:#fff"><ytd-app id="app"></ytd-app><script src="/fake.js"></script></body></html>`;

const FAKE_JS = `
const app = document.getElementById("app");
function render() {
  const id = new URL(location.href).searchParams.get("v");
  if (location.pathname !== "/watch" || !id) return;
  let p = document.getElementById("movie_player");
  if (!p) {
    p = document.createElement("div");
    p.id = "movie_player";
    p.className = "html5-video-player";
    p.innerHTML = '<video class="html5-main-video" style="width:640px;height:360px" playsinline></video>';
    app.appendChild(p);
    const h = document.createElement("h1");
    h.className = "ytd-watch-metadata";
    app.appendChild(h);
  }
  document.querySelector("h1.ytd-watch-metadata").textContent = "Test clip " + id;
  document.title = "Test clip " + id + " - YouTube";
  const v = p.querySelector("video");
  v.src = "/clip.webm?v=" + id;
  v.play().catch((e) => console.log("play failed", e.message));
}
// What YouTube's app does for a link click: same page, new URL
app.addEventListener("yt-navigate", (e) => {
  history.pushState({}, "", e.detail.endpoint.commandMetadata.webCommandMetadata.url);
  render();
});
render();

// The playback clock, for the fake app's synthetic audio
let last = 0;
function report(wall, media, paused) {
  fetch("/__anchor", { method: "POST", body: JSON.stringify({ wall, media, paused }) }).catch(() => {});
}
function onFrame(_now, md) {
  const v = document.querySelector("#movie_player video");
  if (md.expectedDisplayTime - last > 200) { last = md.expectedDisplayTime; report(performance.timeOrigin + md.expectedDisplayTime, md.mediaTime, false); }
  v.requestVideoFrameCallback(onFrame);
}
const wait = setInterval(() => {
  const v = document.querySelector("#movie_player video");
  if (!v) return;
  clearInterval(wait);
  v.requestVideoFrameCallback(onFrame);
  for (const t of ["pause", "seeked", "playing"]) v.addEventListener(t, () => report(performance.timeOrigin + performance.now(), v.currentTime, v.paused));
  measureCapture(v);
}, 50);

// For the report: when a flash reaches captureStream() (what remote.js gets)
// versus when it is shown (what the anchors, and so the beeps, follow)
function measureCapture(v) {
  if (typeof MediaStreamTrackProcessor !== "function") return;
  const shown = [], captured = [];
  const c = new OffscreenCanvas(16, 9), g = c.getContext("2d", { willReadFrequently: true });
  const lum = (src) => { g.drawImage(src, 0, 0, 16, 9); const d = g.getImageData(0, 0, 16, 9).data; let s = 0; for (let i = 0; i < d.length; i += 4) s += d[i]; return s / (d.length / 4); };
  let wasShown = false;
  const onShown = (_n, md) => {
    if (md.mediaTime % 1 < 0.02 && !wasShown) shown.push(performance.timeOrigin + md.expectedDisplayTime);
    wasShown = md.mediaTime % 1 < 0.02;
    v.requestVideoFrameCallback(onShown);
  };
  v.requestVideoFrameCallback(onShown);
  let wasBright = false;
  (async () => {
    let track;
    while (!(track = v.captureStream().getVideoTracks()[0])) await new Promise((r) => setTimeout(r, 200));
    const reader = new MediaStreamTrackProcessor({ track }).readable.getReader();
    for (;;) {
      const { value: f, done } = await reader.read();
      if (done) return;
      const at = performance.timeOrigin + performance.now();
      const bright = lum(f) > 128;
      f.close();
      if (bright && !wasBright) captured.push(at);
      wasBright = bright;
    }
  })();
  setInterval(() => fetch("/__capture", { method: "POST", body: JSON.stringify({ shown, captured }) }).catch(() => {}), 2000);
}
`;

function resultsHtml(q) {
  const vr = (id, title) => ({ videoRenderer: { videoId: id, title: { runs: [{ text: title }] }, ownerText: { runs: [{ text: "Test channel" }] },
    lengthText: { simpleText: "1:00" }, thumbnail: { thumbnails: [{ url: "https://i.ytimg.com/vi/" + id + "/hq.jpg" }] } } });
  const data = { contents: { twoColumnSearchResultsRenderer: { primaryContents: { sectionListRenderer: { contents: [{ itemSectionRenderer: { contents: [
    vr("TESTCLIP01A", q + " one"), { adSlotRenderer: {} }, vr("TESTCLIP02B", q + " two"), vr("TESTCLIP03C", q + " three"),
  ] } }] } } } } };
  return `<!doctype html><html><body><script>var ytInitialData = ${JSON.stringify(data)};</script></body></html>`;
}

function startFakeYouTube({ key, cert, clip, onAnchor, onCapture, port = 443 }) {
  const server = https.createServer({ key, cert }, (req, res) => {
    const u = new URL(req.url, "https://www.youtube.com");
    if (u.pathname === "/fake.js") { res.writeHead(200, { "Content-Type": "text/javascript" }); return res.end(FAKE_JS); }
    if (u.pathname === "/__anchor") {
      let b = "";
      req.on("data", (c) => { b += c; });
      req.on("end", () => { try { onAnchor(JSON.parse(b)); } catch {} res.end(); });
      return;
    }
    if (u.pathname === "/__capture") {
      let b = "";
      req.on("data", (c) => { b += c; });
      req.on("end", () => { try { onCapture?.(JSON.parse(b)); } catch {} res.end(); });
      return;
    }
    if (u.pathname === "/results") { res.writeHead(200, { "Content-Type": "text/html" }); return res.end(resultsHtml(u.searchParams.get("search_query") || "")); }
    if (u.pathname === "/clip.webm") {
      // Ranges, so the clip can be seeked
      const size = fs.statSync(clip).size;
      const m = /bytes=(\d*)-(\d*)/.exec(req.headers.range || "");
      if (m) {
        const start = m[1] ? +m[1] : 0, end = m[2] ? +m[2] : size - 1;
        res.writeHead(206, { "Content-Type": "video/webm", "Content-Range": `bytes ${start}-${end}/${size}`, "Content-Length": end - start + 1, "Accept-Ranges": "bytes" });
        return fs.createReadStream(clip, { start, end }).pipe(res);
      }
      res.writeHead(200, { "Content-Type": "video/webm", "Content-Length": size, "Accept-Ranges": "bytes" });
      return fs.createReadStream(clip).pipe(res);
    }
    if (u.pathname === "/" || u.pathname === "/watch") { res.writeHead(200, { "Content-Type": "text/html" }); return res.end(PAGE); }
    res.writeHead(404); res.end();
  });
  return new Promise((resolve) => server.listen(port, "127.0.0.1", () => resolve(server)));
}

module.exports = { startFakeYouTube };
