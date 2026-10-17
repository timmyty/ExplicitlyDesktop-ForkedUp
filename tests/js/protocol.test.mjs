// Protocol logic of the phone client (docs/STREAMING_PROTOCOL.md §2–§4), checked against
// an independent implementation with Node's crypto module, a simulated PC, and the shared
// interop vectors in tests/vectors/protocol_vectors.json (when present).
// Run: node --test tests/js/*.test.mjs  (or: cd tests/js && npm test)
import test from "node:test";
import assert from "node:assert/strict";
import nodeCrypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  pairMac, buildPairRequest, wrapPad, wrapKey, unwrapKey, pairOkMac, acceptPairResponse,
  requestMac, responseMac, signRequest, verifyResponse, toHex, fromHex, base32Encode, parsePairingCode,
} from "../../MobileClient/crypto.js";
import {
  signedPost, pairWithHost, ApiError, parseFragment, formatPairingCode, isSupportedUrl,
  normalizeVideoUrl, extractSharedUrl, modeLabel, errorText, msg, parseMessage, PingTracker,
  defaultDeviceName, cleanName, formatTime, formatDuration, backoffMs,
} from "../../MobileClient/app.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const H = (k, m) => nodeCrypto.createHmac("sha256", k).update(m, typeof m === "string" ? "utf8" : undefined).digest("hex");
const S = (m) => nodeCrypto.createHash("sha256").update(m, "utf8").digest("hex");
const rnd = (n) => new Uint8Array(nodeCrypto.randomBytes(n));

// ---------------------------------------------------------------------------------------
// §2 pairing math

test("pairing MAC matches HMAC(P, pair|deviceId|name|nonce)", () => {
  for (let i = 0; i < 50; i++) {
    const P = rnd(16);
    const deviceId = nodeCrypto.randomUUID();
    const name = ["Pixel 8", "Tim’s phone", "iPhone", "Ünïcødé 📱"][i % 4];
    const nonce = toHex(rnd(16));
    assert.equal(pairMac(P, deviceId, name, nonce), H(Buffer.from(P), `pair|${deviceId}|${name}|${nonce}`));
    const body = buildPairRequest(P, deviceId, name, nonce);
    assert.deepEqual(Object.keys(body), ["v", "deviceId", "name", "nonce", "mac"]);
    assert.equal(body.v, 1);
  }
  assert.throws(() => buildPairRequest(rnd(16), "id", "a|b", "00"));
});

test("wrap/unwrap K with pad = HMAC(P, wrap|nonce)", () => {
  for (let i = 0; i < 50; i++) {
    const P = rnd(16), K = rnd(32), nonce = toHex(rnd(16));
    const pad = Buffer.from(H(Buffer.from(P), `wrap|${nonce}`), "hex");
    const expected = Buffer.from(K.map((b, j) => b ^ pad[j])).toString("hex");
    assert.equal(toHex(wrapPad(P, nonce)), pad.toString("hex"));
    assert.equal(wrapKey(P, nonce, K), expected);
    assert.deepEqual(unwrapKey(P, nonce, expected), K);
    assert.deepEqual(unwrapKey(P, nonce, expected.toUpperCase()), K);
  }
  assert.throws(() => unwrapKey(rnd(16), "00", "abcd"));
});

/** A simulated PC (independent implementation with Node crypto). */
function makeServer({ P, hostId = "host-1", hostName = "Living-room PC", K = rnd(32), status = 200, tamper = null, error }) {
  const calls = [];
  const fetchImpl = async (url, init) => {
    calls.push({ url, init });
    const req = JSON.parse(init.body);
    if (status !== 200) return new Response(JSON.stringify({ error }), { status });
    const mac = H(Buffer.from(P), `pair|${req.deviceId}|${req.name}|${req.nonce}`);
    if (mac !== req.mac) return new Response(JSON.stringify({ error: "pairing code expired or wrong" }), { status: 403 });
    const pad = Buffer.from(H(Buffer.from(P), `wrap|${req.nonce}`), "hex");
    const wrapped = Buffer.from(K.map((b, j) => b ^ pad[j])).toString("hex");
    const reply = { v: 1, hostId, hostName, deviceId: req.deviceId, wrapped, mac: H(Buffer.from(P), `pair-ok|${req.deviceId}|${wrapped}`) };
    if (tamper) tamper(reply);
    return new Response(JSON.stringify(reply), { status: 200 });
  };
  return { fetchImpl, calls, K };
}

test("pairWithHost: full handshake against a simulated PC", async () => {
  const P = rnd(16);
  const code = formatPairingCode(base32Encode(P));
  const srv = makeServer({ P });
  const deviceId = nodeCrypto.randomUUID();
  const rec = await pairWithHost(srv.fetchImpl, { code, deviceId, name: "Pixel 8", origin: "http://192.168.1.20:8767", expectedHostId: "host-1" });
  assert.deepEqual(rec, { hostId: "host-1", hostName: "Living-room PC", origin: "http://192.168.1.20:8767", deviceId, key: Buffer.from(srv.K).toString("hex") });
  assert.equal(srv.calls[0].url, "/api/pair");
  assert.equal(srv.calls[0].init.method, "POST");
  const body = JSON.parse(srv.calls[0].init.body);
  assert.match(body.nonce, /^[0-9a-f]{32}$/);
  assert.equal(body.name, "Pixel 8");
});

test("pairWithHost rejects a tampered or mismatched reply", async () => {
  const P = rnd(16), code = base32Encode(P), deviceId = nodeCrypto.randomUUID();
  const flip = (r) => { r.wrapped = (r.wrapped[0] === "0" ? "1" : "0") + r.wrapped.slice(1); };
  await assert.rejects(pairWithHost(makeServer({ P, tamper: flip }).fetchImpl, { code, deviceId, name: "x" }), /verification/);
  await assert.rejects(pairWithHost(makeServer({ P, tamper: (r) => { r.deviceId = "other"; } }).fetchImpl, { code, deviceId, name: "x" }), /another device/);
  await assert.rejects(pairWithHost(makeServer({ P }).fetchImpl, { code, deviceId, name: "x", expectedHostId: "different" }), /QR code/);
  // Wrong code → the server answers 403 "expired or wrong".
  await assert.rejects(pairWithHost(makeServer({ P: rnd(16) }).fetchImpl, { code, deviceId, name: "x" }), /expired or is wrong/);
  await assert.rejects(pairWithHost(makeServer({ P, status: 403, error: "denied" }).fetchImpl, { code, deviceId, name: "x" }), /wasn't allowed/);
  await assert.rejects(pairWithHost(makeServer({ P }).fetchImpl, { code: "TOO-SHORT", deviceId, name: "x" }), /26 letters/);
});

// ---------------------------------------------------------------------------------------
// §3 signed requests

test("signRequest headers and MACs match the spec", () => {
  for (let i = 0; i < 30; i++) {
    const K = rnd(32), deviceId = nodeCrypto.randomUUID();
    const body = JSON.stringify({ sdp: `v=0\r\nrandom ${i} ’` });
    const r = signRequest(K, deviceId, "POST", "/api/session/offer", body);
    assert.match(r.nonce, /^[0-9a-f]{32}$/);
    assert.ok(Math.abs(Number(r.time) - Date.now()) < 5000);
    assert.deepEqual(Object.keys(r.headers), ["X-Device", "X-Time", "X-Nonce", "X-Mac"]);
    assert.equal(r.headers["X-Device"], deviceId);
    const want = H(Buffer.from(K), `POST|/api/session/offer|${deviceId}|${r.time}|${r.nonce}|${S(body)}`);
    assert.equal(r.headers["X-Mac"], want);
    assert.equal(requestMac(K, "POST", "/api/session/offer", deviceId, r.time, r.nonce, body), want);
  }
  const K = rnd(32);
  const fixed = signRequest(K, "d", "POST", "/api/hello", "{}", 1760000000000, "ab".repeat(16));
  assert.equal(fixed.headers["X-Time"], "1760000000000");
  assert.equal(fixed.headers["X-Nonce"], "ab".repeat(16));
});

test("response MAC verification", () => {
  const K = rnd(32), nonce = toHex(rnd(16)), body = '{"sessionId":"s","sdp":"v=0"}';
  const mac = H(Buffer.from(K), `resp|/api/session/offer|${nonce}|${S(body)}`);
  assert.equal(responseMac(K, "/api/session/offer", nonce, body), mac);
  assert.ok(verifyResponse(K, "/api/session/offer", nonce, body, mac));
  assert.ok(verifyResponse(K, "/api/session/offer", nonce, body, mac.toUpperCase()));
  assert.ok(!verifyResponse(K, "/api/session/offer", nonce, body + " ", mac));
  assert.ok(!verifyResponse(K, "/api/hello", nonce, body, mac));
  assert.ok(!verifyResponse(rnd(32), "/api/session/offer", nonce, body, mac));
  assert.ok(!verifyResponse(K, "/api/session/offer", nonce, body, null));
});

/** Simulated signed endpoint: verifies the request like the app, signs the reply. */
function signedServer(K, deviceId, handler) {
  const seen = new Set();
  const calls = [];
  const fetchImpl = async (url, init) => {
    const h = init.headers;
    calls.push({ url, init });
    const mac = H(Buffer.from(K), `${init.method}|${url}|${h["X-Device"]}|${h["X-Time"]}|${h["X-Nonce"]}|${S(init.body)}`);
    if (h["X-Device"] !== deviceId || mac !== h["X-Mac"] || seen.has(h["X-Nonce"]) || Math.abs(Number(h["X-Time"]) - Date.now()) > 120000) {
      return new Response(JSON.stringify({ error: "unauthorized" }), { status: 401 });
    }
    seen.add(h["X-Nonce"]);
    const { status = 200, body, badMac = false, noMac = false } = handler(url, JSON.parse(init.body));
    const text = JSON.stringify(body);
    const headers = noMac ? {} : { "X-Mac": badMac ? "0".repeat(64) : H(Buffer.from(K), `resp|${url}|${h["X-Nonce"]}|${S(text)}`) };
    return new Response(text, { status, headers });
  };
  return { fetchImpl, calls };
}

test("signedPost: round trip through a verifying PC", async () => {
  const K = rnd(32), deviceId = nodeCrypto.randomUUID();
  const pairing = { deviceId, key: toHex(K), hostId: "h", hostName: "PC", origin: "http://x" };
  const srv = signedServer(K, deviceId, (url, body) => {
    if (url === "/api/hello") return { body: { hostName: "PC", appVersion: "1.0", filter: { status: "running" } } };
    if (url === "/api/session/offer") return { body: { sessionId: "s-1", sdp: "answer:" + body.sdp } };
    return { status: 404, body: { error: "nope" } };
  });
  assert.equal((await signedPost(srv.fetchImpl, pairing, "/api/hello", {})).hostName, "PC");
  const ans = await signedPost(srv.fetchImpl, pairing, "/api/session/offer", { sdp: "v=0\r\n" });
  assert.deepEqual(ans, { sessionId: "s-1", sdp: "answer:v=0\r\n" });
  assert.equal(srv.calls[0].init.body, "{}");
  assert.equal(srv.calls[0].init.method, "POST");
  assert.equal(srv.calls[0].init.headers["Content-Type"], "application/json");
});

test("signedPost rejects a missing or wrong response MAC", async () => {
  const K = rnd(32), deviceId = nodeCrypto.randomUUID();
  const pairing = { deviceId, key: toHex(K) };
  const bad = signedServer(K, deviceId, () => ({ body: { sessionId: "evil", sdp: "x" }, badMac: true }));
  await assert.rejects(signedPost(bad.fetchImpl, pairing, "/api/session/offer", { sdp: "o" }), (e) => e instanceof ApiError && e.kind === "bad-mac");
  const none = signedServer(K, deviceId, () => ({ body: { sessionId: "evil", sdp: "x" }, noMac: true }));
  await assert.rejects(signedPost(none.fetchImpl, pairing, "/api/session/offer", { sdp: "o" }), (e) => e.kind === "bad-mac");
  // Error reply carrying a bad MAC is also rejected as tampering.
  const badErr = signedServer(K, deviceId, () => ({ status: 409, body: { error: "x" }, badMac: true }));
  await assert.rejects(signedPost(badErr.fetchImpl, pairing, "/api/session/offer", {}), (e) => e.kind === "bad-mac");
});

test("signedPost maps 401, 409 and 504", async () => {
  const K = rnd(32), deviceId = nodeCrypto.randomUUID();
  // Unknown device (wrong key): the app answers 401 without a MAC.
  const srv = signedServer(rnd(32), deviceId, () => ({ body: {} }));
  await assert.rejects(signedPost(srv.fetchImpl, { deviceId, key: toHex(K) }, "/api/hello", {}), (e) => e.kind === "unauthorized" && e.status === 401);
  const busy = signedServer(K, deviceId, () => ({ status: 409, body: { error: "host browser not available" } }));
  await assert.rejects(signedPost(busy.fetchImpl, { deviceId, key: toHex(K) }, "/api/session/offer", {}), (e) => e.status === 409 && /browser/.test(e.message));
  const slow = signedServer(K, deviceId, () => ({ status: 504, body: {} }));
  await assert.rejects(signedPost(slow.fetchImpl, { deviceId, key: toHex(K) }, "/api/session/offer", {}), (e) => e.status === 504);
  const down = async () => { throw new TypeError("Failed to fetch"); };
  await assert.rejects(signedPost(down, { deviceId, key: toHex(K) }, "/api/hello", {}), (e) => e.kind === "network");
});

// ---------------------------------------------------------------------------------------
// Shared interop vectors (written by the C++ side)

const vecPath = path.join(here, "..", "vectors", "protocol_vectors.json");
const vectors = fs.existsSync(vecPath) ? JSON.parse(fs.readFileSync(vecPath, "utf8")) : null;

test("interop vectors: pairing", { skip: !vectors && "tests/vectors/protocol_vectors.json not found" }, async () => {
  const v = vectors.pairing;
  const P = fromHex(v.P_hex);
  assert.equal(base32Encode(P), v.P_b32);
  assert.deepEqual(parsePairingCode(v.P_b32), P);
  assert.deepEqual(parsePairingCode(v.typedCode), P);
  assert.equal(formatPairingCode(v.P_b32), v.typedCode);
  const frag = parseFragment(new URL(v.qrUrl).hash);
  assert.deepEqual(frag, { pair: v.P_b32, host: v.hostId });
  assert.equal(pairMac(P, v.deviceId, v.name, v.nonce), v.pairMac);
  assert.equal(toHex(wrapPad(P, v.nonce)), v.pad_hex);
  assert.equal(wrapKey(P, v.nonce, fromHex(v.K_hex)), v.wrapped);
  assert.equal(pairOkMac(P, v.deviceId, v.wrapped), v.pairOkMac);
  const r = acceptPairResponse(P, v.deviceId, v.nonce, { v: 1, hostId: v.hostId, hostName: "PC", deviceId: v.deviceId, wrapped: v.wrapped, mac: v.pairOkMac });
  assert.equal(toHex(r.key), v.K_hex);

  // The same exchange through pairWithHost, with the vector's nonce and a canned reply.
  const fetchImpl = async (url, init) => {
    assert.deepEqual(JSON.parse(init.body), { v: 1, deviceId: v.deviceId, name: v.name, nonce: v.nonce, mac: v.pairMac });
    return new Response(JSON.stringify({ v: 1, hostId: v.hostId, hostName: "PC", deviceId: v.deviceId, wrapped: v.wrapped, mac: v.pairOkMac }), { status: 200 });
  };
  const rec = await pairWithHost(fetchImpl, { code: v.typedCode, deviceId: v.deviceId, name: v.name, nonce: v.nonce, origin: "http://192.168.1.20:8767", expectedHostId: v.hostId });
  assert.equal(rec.key, v.K_hex);
});

test("interop vectors: signed request and response", { skip: !vectors && "tests/vectors/protocol_vectors.json not found" }, async () => {
  const v = vectors.signedRequest;
  const K = fromHex(v.K_hex);
  assert.equal(nodeCrypto.createHash("sha256").update(v.body).digest("hex"), v.bodySha256);
  const r = signRequest(K, v.deviceId, v.method, v.path, v.body, v.time, v.nonce);
  assert.equal(r.headers["X-Mac"], v.mac);
  assert.equal(requestMac(K, v.method, v.path, v.deviceId, v.time, v.nonce, v.body), v.mac);
  assert.equal(responseMac(K, v.path, v.nonce, v.responseBody), v.responseMac);
  assert.ok(verifyResponse(K, v.path, v.nonce, v.responseBody, v.responseMac));

  // signedPost with the vector's time/nonce sends exactly the vector's MAC and accepts its reply.
  const fetchImpl = async (url, init) => {
    assert.equal(url, v.path);
    assert.equal(init.headers["X-Mac"], v.mac);
    return new Response(v.responseBody, { status: 200, headers: { "X-Mac": v.responseMac } });
  };
  const out = await signedPost(fetchImpl, { deviceId: v.deviceId, key: v.K_hex }, v.path, JSON.parse(v.body), { time: v.time, nonce: v.nonce });
  assert.deepEqual(out, JSON.parse(v.responseBody));
  assert.equal(JSON.stringify(JSON.parse(v.body)), v.body, "vector body is in JSON.stringify form");
});

test("interop vectors: primitives", { skip: !vectors && "tests/vectors/protocol_vectors.json not found" }, async () => {
  const { sha256Hex, hmacHex, base32Decode } = await import("../../MobileClient/crypto.js");
  const p = vectors.primitives;
  assert.equal(sha256Hex(""), p.sha256_empty);
  assert.equal(sha256Hex("abc"), p.sha256_abc);
  assert.equal(hmacHex(p.hmac_rfc4231_tc2_key, p.hmac_rfc4231_tc2_data), p.hmac_rfc4231_tc2);
  assert.equal(Buffer.from(base32Decode(p.base32_foobar)).toString(), "foobar");
  assert.equal(Buffer.from(base32Decode(p.base32_f)).toString(), "f");
});

// ---------------------------------------------------------------------------------------
// §4.1 control messages and UI helpers

test("DataChannel message builders", () => {
  assert.deepEqual(msg.load("https://youtu.be/x"), { t: "load", url: "https://youtu.be/x" });
  assert.deepEqual(msg.play(), { t: "play" });
  assert.deepEqual(msg.pause(), { t: "pause" });
  assert.deepEqual(msg.seek(12.5), { t: "seek", time: 12.5 });
  assert.deepEqual(msg.seek(-3), { t: "seek", time: 0 });
  assert.deepEqual(msg.volume(1.7), { t: "volume", value: 1 });
  assert.deepEqual(msg.volume(0.25), { t: "volume", value: 0.25 });
  assert.deepEqual(msg.filter(0), { t: "filter", on: false });
  assert.deepEqual(msg.search("cats", 3), { t: "search", q: "cats", id: "3" });
  assert.deepEqual(msg.ping(7), { t: "ping", id: "7" });
  assert.equal(parseMessage('{"t":"pong","id":"p1"}').t, "pong");
  assert.equal(parseMessage("not json"), null);
  assert.equal(parseMessage('{"x":1}'), null);
  assert.equal(parseMessage(new ArrayBuffer(2)), null);
});

test("PingTracker measures round trips", () => {
  let now = 1000;
  const p = new PingTracker(() => now);
  const m = p.next();
  assert.equal(m.t, "ping");
  now += 37;
  assert.equal(p.pong(m.id), 37);
  assert.equal(p.rtt, 37);
  assert.equal(p.pong(m.id), null, "a pong only counts once");
  assert.equal(p.pong("unknown"), null);
});

test("supported URLs and normalization (§4.1 load)", () => {
  for (const u of ["https://www.youtube.com/watch?v=dQw4w9WgXcQ", "https://m.youtube.com/watch?v=dQw4w9WgXcQ&t=10",
    "https://youtu.be/dQw4w9WgXcQ", "https://www.youtube.com/shorts/abc_DEF-1", "https://www.youtube.com/watch?feature=x&v=abc"]) {
    assert.ok(isSupportedUrl(u), u);
  }
  for (const u of ["http://www.youtube.com/watch?v=x", "https://vimeo.com/1", "https://www.youtube.com/", "https://evil.com/?https://youtu.be/x", "javascript:alert(1)"]) {
    assert.ok(!isSupportedUrl(u), u);
  }
  assert.equal(normalizeVideoUrl("youtube.com/watch?v=abc"), "https://www.youtube.com/watch?v=abc");
  assert.equal(normalizeVideoUrl("http://youtu.be/abc?si=1"), "https://youtu.be/abc?si=1");
  assert.equal(normalizeVideoUrl("  https://music.youtube.com/watch?v=abc "), "https://www.youtube.com/watch?v=abc");
  assert.equal(normalizeVideoUrl("https://vimeo.com/123"), null);
  assert.equal(normalizeVideoUrl(""), null);
  assert.equal(normalizeVideoUrl("ftp://youtu.be/x"), null);
});

test("Android share target picks the URL", () => {
  assert.equal(extractSharedUrl({ url: "https://youtu.be/abc" }), "https://youtu.be/abc");
  assert.equal(extractSharedUrl({ text: "Watch this! https://youtu.be/abc?si=xyz." }), "https://youtu.be/abc?si=xyz");
  assert.equal(extractSharedUrl({ title: "Funny", text: "Funny video https://www.youtube.com/watch?v=q1 via app" }), "https://www.youtube.com/watch?v=q1");
  assert.equal(extractSharedUrl({ text: "no link here" }), null);
  assert.equal(extractSharedUrl({}), null);
});

test("fragment, code formatting, labels", () => {
  assert.deepEqual(parseFragment("#pair=ABC&host=h1"), { pair: "ABC", host: "h1" });
  assert.deepEqual(parseFragment(""), { pair: "", host: "" });
  assert.equal(formatPairingCode("abcdefghijklmnopqrstuvwxyz"), "ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YZ");
  assert.equal(modeLabel({ mode: "delayed", filter: { delayMs: 3500 } }), "Filtering · 3.5 s delay");
  assert.equal(modeLabel({ mode: "delayed", filter: { delayMs: 3720 } }), "Filtering · 3.7 s delay");
  assert.equal(modeLabel({ mode: "caption" }), "Caption mute · no delay");
  assert.equal(modeLabel({ mode: "live" }), "Filter off");
  assert.equal(errorText({ code: "protected", message: "x" }), "This source is protected and can't be streamed.");
  assert.equal(errorText({ code: "weird", message: "Plain text" }), "Plain text");
  assert.equal(formatTime(75.4), "1:15");
  assert.equal(formatTime(3725), "1:02:05");
  assert.equal(formatTime(NaN), "0:00");
  assert.equal(formatDuration(61), "1:01");
  assert.equal(formatDuration("4:13"), "4:13");
  assert.deepEqual([1, 2, 3].map(backoffMs), [1000, 2000, 4000]);
});

test("device names", () => {
  assert.equal(defaultDeviceName("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X)"), "iPhone");
  assert.equal(defaultDeviceName("Mozilla/5.0 (Linux; Android 14; Pixel 8 Build/UD1A) AppleWebKit"), "Pixel 8");
  assert.equal(defaultDeviceName("Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36"), "Android phone");
  assert.equal(cleanName("  a|b  "), "a/b");
  assert.equal(cleanName("x".repeat(80)).length, 40);
});
