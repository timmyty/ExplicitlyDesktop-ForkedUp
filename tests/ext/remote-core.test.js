// Unit tests for BrowserExtension/remote-core.js (node --test tests/ext)
const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const fs = require("node:fs");
const core = require("../../BrowserExtension/remote-core.js");

const vectors = JSON.parse(fs.readFileSync(path.join(__dirname, "../vectors/protocol_vectors.json"), "utf8"));

test("load: accepts exactly the four URL forms", () => {
  const ok = {
    "https://www.youtube.com/watch?v=dQw4w9WgXcQ": "dQw4w9WgXcQ",
    "https://m.youtube.com/watch?v=dQw4w9WgXcQ&feature=share": "dQw4w9WgXcQ",
    "https://youtu.be/dQw4w9WgXcQ": "dQw4w9WgXcQ",
    "https://youtu.be/dQw4w9WgXcQ?si=abc": "dQw4w9WgXcQ",
    "https://www.youtube.com/shorts/abc_DEF-123": "abc_DEF-123",
    "  https://www.youtube.com/watch?list=x&v=dQw4w9WgXcQ  ": "dQw4w9WgXcQ",
  };
  for (const [url, id] of Object.entries(ok)) {
    const p = core.parseYouTubeUrl(url);
    assert.ok(p, url);
    assert.equal(p.videoId, id);
    assert.equal(p.watchUrl, "https://www.youtube.com/watch?v=" + id);
  }
  const bad = [
    "http://www.youtube.com/watch?v=dQw4w9WgXcQ",          // not https
    "https://youtube.com/watch?v=dQw4w9WgXcQ",              // not listed
    "https://music.youtube.com/watch?v=dQw4w9WgXcQ",
    "https://www.youtube.com/watch?v=short",                // bad id
    "https://www.youtube.com/watch?v=dQw4w9WgXcQ1",
    "https://www.youtube.com/embed/dQw4w9WgXcQ",
    "https://m.youtube.com/shorts/dQw4w9WgXcQ",
    "https://www.youtube.com.evil.com/watch?v=dQw4w9WgXcQ",
    "https://user:pw@www.youtube.com/watch?v=dQw4w9WgXcQ",
    "https://www.youtube.com:444/watch?v=dQw4w9WgXcQ",
    "https://youtu.be/",
    "javascript:alert(1)",
    "",
    null,
    42,
  ];
  for (const url of bad) assert.equal(core.parseYouTubeUrl(url), null, String(url));
});

test("load: start time is kept", () => {
  assert.equal(core.parseYouTubeUrl("https://youtu.be/dQw4w9WgXcQ?t=90").start, 90);
  assert.equal(core.parseYouTubeUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=1m30s").start, 90);
  assert.equal(core.parseYouTubeUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=1h2m3s").watchUrl,
    "https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=3723s");
  assert.equal(core.parseYouTubeUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=nonsense").start, 0);
});

test("PCM frame: protocol vector parses (§5.1)", () => {
  const v = vectors.pcmFrame;
  const bytes = Buffer.from(v.frame_hex, "hex");
  const f = core.parsePcmFrame(new Uint8Array(bytes));
  assert.equal(f.sampleRate, +v.sampleRate);
  assert.equal(f.channels, +v.channels);
  assert.equal(f.flags, +v.flags);
  assert.equal(f.startSample, +v.startSample);
  assert.equal(f.frames, +v.frames);
  assert.equal(f.delaySamples, +v.delaySamples);
  assert.deepEqual(Array.from(f.pcm), v.int16Samples.split(",").map(Number));
  assert.equal(f.discontinuity, true);
  assert.equal(f.filterActive, true);
  assert.equal(f.held, false);
});

test("PCM frame: build round trip, odd byte offset, rejects bad input", () => {
  const pcm = Int16Array.from({ length: 960 * 2 }, (_, i) => (i * 37) % 65536 - 32768);
  const bytes = core.buildPcmFrame({ sampleRate: 44100, flags: 4, startSample: 2 ** 40 + 5, delaySamples: 154350, pcm });
  assert.equal(bytes.length, 28 + 3840);
  // From a view at an odd offset (as a sliced buffer would be)
  const shifted = new Uint8Array(bytes.length + 1);
  shifted.set(bytes, 1);
  const f = core.parsePcmFrame(shifted.subarray(1));
  assert.equal(f.sampleRate, 44100);
  assert.equal(f.startSample, 2 ** 40 + 5);
  assert.equal(f.frames, 960);
  assert.equal(f.held, true);
  assert.deepEqual(Array.from(f.pcm), Array.from(pcm));
  assert.equal(core.parsePcmFrame(new Uint8Array(10)), null);
  const bad = Uint8Array.from(bytes); bad[0] = 0x46;
  assert.equal(core.parsePcmFrame(bad), null);
  assert.equal(core.parsePcmFrame(bytes.subarray(0, 100)), null);   // truncated samples
  assert.equal(core.parsePcmFrame(bytes.buffer).frames, 960);   // ArrayBuffer accepted
});

test("base64 relay: round trip and cost per 20 ms frame", () => {
  for (const n of [0, 1, 2, 3, 4, 5, 100, 3868]) {
    const b = Uint8Array.from({ length: n }, (_, i) => (i * 131 + 7) & 255);
    const s = core.bytesToBase64(b);
    assert.equal(s, Buffer.from(b).toString("base64"));
    assert.deepEqual(core.base64ToBytes(s), b);
  }
  // A real frame: encode (background) + decode + parse (tab) must stay well under 1 ms
  const pcm = Int16Array.from({ length: 1920 }, () => Math.round((Math.random() * 2 - 1) * 30000));
  const frame = core.buildPcmFrame({ pcm });
  const N = 2000;
  const t0 = performance.now();
  for (let i = 0; i < N; i++) core.parsePcmFrame(core.base64ToBytes(core.bytesToBase64(frame)));
  const per = (performance.now() - t0) / N;
  console.log(`  base64 relay: ${(per * 1000).toFixed(1)} µs per frame (encode+decode+parse), ${core.bytesToBase64(frame).length} chars`);
  assert.ok(per < 1, `too slow: ${per} ms`);

  // For comparison: a plain number array through JSON (what a Port would carry)
  const t1 = performance.now();
  for (let i = 0; i < 200; i++) JSON.parse(JSON.stringify(Array.from(frame)));
  console.log(`  plain array JSON: ${((performance.now() - t1) / 200 * 1000).toFixed(1)} µs per frame`);
});

test("mode and video delay (§7)", () => {
  const on = { running: true, enabled: true, delayMs: 3500 };
  assert.equal(core.streamMode(on, false), "delayed");
  assert.equal(core.streamMode(on, true), "caption");
  assert.equal(core.streamMode({ ...on, enabled: false }, false), "live");
  assert.equal(core.streamMode({ ...on, running: false }, true), "live");
  assert.equal(core.streamMode({ ...on, offline: true }, false), "live");
  assert.equal(core.streamMode(null, false), "live");

  const out = core.outputLatencyMs({ packMs: 10, relayMs: 1.5, ringMs: 62, cableMs: 30 });
  assert.equal(out, 103.5);
  assert.equal(core.outputLatencyMs({ packMs: 10, relayMs: -5, ringMs: 0, cableMs: 30 }), 40);
  assert.equal(core.videoDelayMs("delayed", 3500, out), 3603.5);
  assert.equal(core.videoDelayMs("caption", 3500, out), 103.5);
  assert.equal(core.videoDelayMs("live", 3500, out), 103.5);
});

test("DelaySmoother: jumps apply at once, small changes settle smoothly", () => {
  const s = new core.DelaySmoother({ tauMs: 1000, jumpMs: 250 });
  assert.equal(s.update(3600, 0), 3600);
  const v = s.update(3620, 1000);
  assert.ok(v > 3610 && v < 3620, String(v));   // ~63% of the way after one tau
  assert.equal(s.update(100, 1100), 100);        // filter off: jump
  let x;
  for (let t = 1100; t <= 20000; t += 16) x = s.update(110, t);
  assert.ok(Math.abs(x - 110) < 0.01);
});

test("QueueClock freezes while held", () => {
  let real = 1000;
  const c = new core.QueueClock(() => real);
  assert.equal(c.now(), 1000);
  c.freeze(); real = 1500;
  assert.equal(c.now(), 1000);
  c.freeze(); real = 1700;   // second freeze is a no-op
  c.thaw();
  assert.equal(c.now(), 1000);
  real = 1800;
  assert.equal(c.now(), 1100);
  assert.equal(c.toReal(1100), 1800);
});

test("FrameQueue: newest due frame, older dropped, clear releases", () => {
  const released = [];
  const q = new core.FrameQueue({ max: 4, release: (x) => released.push(x) });
  for (let i = 0; i < 6; i++) q.push(i * 10, "f" + i);   // f0, f1 dropped by the cap
  assert.deepEqual(released, ["f0", "f1"]);
  assert.equal(q.takeDue(100, 95), null);              // due before 5: nothing
  assert.equal(q.emitted, false);
  const e = q.takeDue(140, 100);                       // due <= 40: f2 f3 f4, keep newest
  assert.equal(e.item, "f4");
  assert.equal(e.t, 40);
  assert.deepEqual(released, ["f0", "f1", "f2", "f3"]);
  assert.equal(q.emitted, true);
  q.clear();
  assert.deepEqual(released, ["f0", "f1", "f2", "f3", "f5"]);
  assert.equal(q.length, 0);
  assert.equal(q.emitted, false);
});

test("state.time is what the phone shows", () => {
  assert.equal(core.shownTime({ currentTime: 100, mode: "delayed", delayMs: 3600 }), 96.4);
  assert.equal(core.shownTime({ currentTime: 2, mode: "delayed", delayMs: 3600 }), 0);
  assert.equal(core.shownTime({ currentTime: 100, mode: "caption", delayMs: 100 }), 100);
  // Right after a seek, before the delayed picture catches up
  assert.equal(core.shownTime({ currentTime: 61, mode: "delayed", delayMs: 3600, seekAnchor: 60, queueFilled: false }), 60);
  assert.equal(core.shownTime({ currentTime: 65, mode: "delayed", delayMs: 3600, seekAnchor: 60, queueFilled: true }), 61.4);
});

const OFFER_AUDIO = [
  "v=0", "o=- 1 2 IN IP4 127.0.0.1", "s=-", "t=0 0",
  "a=fingerprint:sha-256 AB:CD:EF",
  "m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9",
  "a=rtpmap:111 opus/48000/2",
  "a=fmtp:111 minptime=10;useinbandfec=1;stereo=0",
  "a=rtpmap:63 red/48000/2",
  "a=fmtp:63 111/111",
  "a=rtpmap:9 G722/8000",
  "",
].join("\r\n");

test("SDP: Opus stereo 128 kbps fmtp", () => {
  const out = core.mungeOpus(OFFER_AUDIO);
  assert.match(out, /^a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1;maxaveragebitrate=128000$/m);
  assert.match(out, /^a=fmtp:63 111\/111$/m);            // RED untouched
  assert.ok(out.endsWith("\r\n") && !/[^\r]\n/.test(out)); // line endings kept
  assert.equal(core.mungeOpus(out), out);                  // idempotent

  // No fmtp line yet: one is added after the rtpmap
  const bare = "m=audio 9 RTP/SAVPF 109\na=rtpmap:109 opus/48000/2\na=sendrecv\n";
  assert.equal(core.mungeOpus(bare), "m=audio 9 RTP/SAVPF 109\na=rtpmap:109 opus/48000/2\na=fmtp:109 stereo=1;sprop-stereo=1;maxaveragebitrate=128000\na=sendrecv\n");
  assert.equal(core.mungeOpus("v=0\r\nm=video 9 RTP 96\r\n"), "v=0\r\nm=video 9 RTP 96\r\n");
});

test("SDP: fingerprint identifies the phone's connection", () => {
  assert.equal(core.sdpFingerprint(OFFER_AUDIO), "sha-256 ab:cd:ef");
  assert.equal(core.sdpFingerprint("v=0\r\n"), null);
});

test("codec order: H.264 (pm=1, baseline) then VP8, rest kept", () => {
  const codecs = [
    { mimeType: "video/VP8" },
    { mimeType: "video/rtx" },
    { mimeType: "video/VP9", sdpFmtpLine: "profile-id=0" },
    { mimeType: "video/H264", sdpFmtpLine: "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f" },
    { mimeType: "video/H264", sdpFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c1f" },
    { mimeType: "video/H264", sdpFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f" },
    { mimeType: "video/red" },
  ];
  const order = core.orderCodecs(codecs, "video").map((c) => c.mimeType + " " + (c.sdpFmtpLine || ""));
  assert.deepEqual(order, [
    "video/H264 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
    "video/H264 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c1f",
    "video/H264 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f",
    "video/VP8 ",
    "video/rtx ",
    "video/VP9 profile-id=0",
    "video/red ",
  ]);
  // Without H.264 (e.g. open-source Chromium): VP8 first
  assert.equal(core.orderCodecs(codecs.filter((c) => c.mimeType !== "video/H264"), "video")[0].mimeType, "video/VP8");
  const a = core.orderCodecs([{ mimeType: "audio/red" }, { mimeType: "audio/opus" }, { mimeType: "audio/G722" }], "audio");
  assert.deepEqual(a.map((c) => c.mimeType), ["audio/opus", "audio/red", "audio/G722"]);
});

function resultsPage(data) {
  return `<html><script nonce="x">var ytInitialData = ${JSON.stringify(data)};</script><script>var other = {"a":"}"};</script></html>`;
}

test("search: results from ytInitialData, both layouts, max 20, no ads", () => {
  const vr = (i, extra = {}) => ({ videoRenderer: {
    videoId: ("vid" + i).padEnd(11, "x"),
    title: { runs: [{ text: "Title " }, { text: String(i) }] },
    ownerText: { runs: [{ text: "Chan " + i }] },
    lengthText: { simpleText: "3:0" + (i % 10) },
    thumbnail: { thumbnails: [{ url: "https://i.ytimg.com/s.jpg" }, { url: "//i.ytimg.com/vi/" + i + "/hq.jpg" }] },
    ...extra,
  } });
  const lockup = { lockupViewModel: {
    contentType: "LOCKUP_CONTENT_TYPE_VIDEO", contentId: "lockupVid01",
    metadata: { lockupMetadataViewModel: { title: { content: "New \"layout\" } title" },
      metadata: { contentMetadataViewModel: { metadataRows: [{ metadataParts: [{ text: { content: "Lock Chan" } }] }] } } } },
    contentImage: { thumbnailViewModel: { image: { sources: [{ url: "https://i.ytimg.com/l.jpg" }] },
      overlays: [{ thumbnailOverlayBadgeViewModel: { thumbnailBadges: [{ thumbnailBadgeViewModel: { text: "12:34" } }] } }] } },
  } };
  const data = { contents: { twoColumnSearchResultsRenderer: { primaryContents: { sectionListRenderer: { contents: [
    { itemSectionRenderer: { contents: [
      { adSlotRenderer: { videoId: "adadadadada" } },
      vr(0), lockup, vr(1, { badges: [{ metadataBadgeRenderer: { style: "BADGE_STYLE_TYPE_LIVE_NOW" } }], lengthText: undefined }),
      vr(0),   // duplicate
      { shelfRenderer: { content: { verticalListRenderer: { items: Array.from({ length: 30 }, (_, i) => vr(i + 2)) } } } },
    ] } },
  ] } } } } };
  const items = core.parseSearchResults(resultsPage(data));
  assert.equal(items.length, 20);
  assert.deepEqual(items[0], {
    url: "https://www.youtube.com/watch?v=vid0xxxxxxx",
    title: "Title 0", channel: "Chan 0", duration: "3:00", thumb: "https://i.ytimg.com/vi/0/hq.jpg",
  });
  assert.deepEqual(items[1], {
    url: "https://www.youtube.com/watch?v=lockupVid01",
    title: "New \"layout\" } title", channel: "Lock Chan", duration: "12:34", thumb: "https://i.ytimg.com/l.jpg",
  });
  assert.equal(items[2].duration, "LIVE");
  assert.equal(items[3].title, "Title 2");
  assert.ok(!items.some((i) => i.url.includes("adadadadada")));
  assert.deepEqual(core.parseSearchResults("<html>no data</html>"), []);
  assert.deepEqual(core.parseSearchResults("var ytInitialData = {broken"), []);
  assert.equal(core.searchUrl("a b&c"), "https://www.youtube.com/results?search_query=a%20b%26c");
});

test("control messages: parsed and checked (§4.1)", () => {
  assert.deepEqual(core.parseControl('{"t":"ping","id":3}'), { t: "ping", id: 3 });
  assert.deepEqual(core.parseControl('{"t":"volume","value":1.5}'), { t: "volume", value: 1 });
  assert.equal(core.parseControl('{"t":"volume","value":"x"}'), null);
  assert.equal(core.parseControl('{"t":"seek","time":-1}'), null);
  assert.equal(core.parseControl('{"t":"seek","time":12.5}').time, 12.5);
  assert.equal(core.parseControl('{"t":"filter","on":"yes"}'), null);
  assert.equal(core.parseControl('{"t":"load"}'), null);
  assert.equal(core.parseControl('{"t":"eval","code":"x"}'), null);
  assert.equal(core.parseControl("not json"), null);
  assert.equal(core.parseControl({ t: "play" }), null);   // only strings come off the channel
  for (const t of ["play", "pause"]) assert.equal(core.parseControl(JSON.stringify({ t })).t, t);
});
