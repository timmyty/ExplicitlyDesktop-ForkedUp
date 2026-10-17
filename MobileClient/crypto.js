// Cryptography for the phone client (docs/STREAMING_PROTOCOL.md §1–§3).
//
// The page is served over plain http://<lan-ip>:8767, which is not a secure context, so
// crypto.subtle is unavailable. This module implements SHA-256 and HMAC-SHA256 itself and
// uses crypto.getRandomValues (available on insecure pages) for nonces and ids.
//
// It also holds the protocol's MAC/wrap math, because the app serves only a fixed list of
// static files (index.html, app.js, crypto.js, style.css, manifest.json, icons, sw.js):
// a separate protocol.js would be a 404 on the phone. Everything here is DOM-free and is
// imported by the Node tests in tests/js/.

const encoder = new TextEncoder();

/** UTF-8 encode a string (Uint8Array passes through). */
export function utf8(s) {
  return typeof s === "string" ? encoder.encode(s) : s;
}

const HEX = Array.from({ length: 256 }, (_, i) => i.toString(16).padStart(2, "0"));

/** Lowercase hex of a byte array. */
export function toHex(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += HEX[bytes[i]];
  return s;
}

/** Bytes from a hex string (case-insensitive). Throws on odd length or bad digits. */
export function fromHex(hex) {
  if (typeof hex !== "string" || hex.length % 2 !== 0 || /[^0-9a-fA-F]/.test(hex)) {
    throw new Error("bad hex");
  }
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

// ---------------------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)

const K256 = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

/** SHA-256 of a string (UTF-8) or bytes. Returns 32 bytes. */
export function sha256(data) {
  const msg = utf8(data);
  const len = msg.length;
  const total = ((len + 9 + 63) >> 6) << 6; // padded length, multiple of 64
  const buf = new Uint8Array(total);
  buf.set(msg);
  buf[len] = 0x80;
  const view = new DataView(buf.buffer);
  // Bit length as a 64-bit big-endian integer.
  view.setUint32(total - 8, Math.floor(len / 0x20000000), false);
  view.setUint32(total - 4, (len << 3) >>> 0, false);

  let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
  let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
  const w = new Uint32Array(64);

  for (let off = 0; off < total; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = view.getUint32(off + i * 4, false);
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15], b = w[i - 2];
      const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
      const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const ch = (e & f) ^ (~e & g);
      const t1 = (h + S1 + ch + K256[i] + w[i]) | 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0;
      d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0;
    h4 = (h4 + e) | 0; h5 = (h5 + f) | 0; h6 = (h6 + g) | 0; h7 = (h7 + h) | 0;
  }

  const out = new Uint8Array(32);
  const ov = new DataView(out.buffer);
  [h0, h1, h2, h3, h4, h5, h6, h7].forEach((v, i) => ov.setUint32(i * 4, v >>> 0, false));
  return out;
}

/** SHA256hex(msg): lowercase hex. */
export function sha256Hex(data) {
  return toHex(sha256(data));
}

/** HMAC-SHA256(key, msg) as 32 bytes. key and msg may be strings (UTF-8) or bytes. */
export function hmacSha256(key, msg) {
  let k = utf8(key);
  if (k.length > 64) k = sha256(k);
  const block = new Uint8Array(64);
  block.set(k);
  const m = utf8(msg);
  const inner = new Uint8Array(64 + m.length);
  const outer = new Uint8Array(64 + 32);
  for (let i = 0; i < 64; i++) {
    inner[i] = block[i] ^ 0x36;
    outer[i] = block[i] ^ 0x5c;
  }
  inner.set(m, 64);
  outer.set(sha256(inner), 64);
  return sha256(outer);
}

/** HMAC(key, msg) per §1: lowercase hex, 64 chars. */
export function hmacHex(key, msg) {
  return toHex(hmacSha256(key, msg));
}

/** Constant-time comparison of two strings (for MACs). */
export function safeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

// ---------------------------------------------------------------------------------------
// Base32 (RFC 4648, A–Z 2–7, no padding)

const B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/**
 * Decode RFC 4648 base32. Case-insensitive; ignores dashes, spaces and trailing '='.
 * Throws on any other character. Leftover bits (< 8) are dropped.
 */
export function base32Decode(text) {
  const clean = String(text).toUpperCase().replace(/[\s-]/g, "").replace(/=+$/, "");
  const out = [];
  let bits = 0, value = 0;
  for (const ch of clean) {
    const v = B32.indexOf(ch);
    if (v < 0) throw new Error("bad base32 character: " + ch);
    value = (value << 5) | v;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out.push((value >>> bits) & 0xff);
    }
    value &= (1 << bits) - 1;
  }
  return new Uint8Array(out);
}

/** Encode bytes as base32 without padding (used by tests and the demo). */
export function base32Encode(bytes) {
  let out = "", bits = 0, value = 0;
  for (const b of bytes) {
    value = (value << 8) | b;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out += B32[(value >>> bits) & 31];
    }
    value &= (1 << bits) - 1;
  }
  if (bits > 0) out += B32[(value << (5 - bits)) & 31];
  return out;
}

// ---------------------------------------------------------------------------------------
// Randomness

/** n cryptographically random bytes (crypto.getRandomValues works on insecure pages). */
export function randomBytes(n) {
  const out = new Uint8Array(n);
  globalThis.crypto.getRandomValues(out);
  return out;
}

/** 16 random bytes as hex: the protocol's nonce format. */
export function newNonce() {
  return toHex(randomBytes(16));
}

/** RFC 4122 version 4 uuid (crypto.randomUUID needs a secure context, so build it). */
export function uuidv4() {
  const b = randomBytes(16);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const h = toHex(b);
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

// ---------------------------------------------------------------------------------------
// Pairing (§2)

/** Normalize a typed or scanned pairing code: uppercase, no dashes/spaces. 26 chars → 16 bytes. */
export function parsePairingCode(text) {
  const clean = String(text || "").toUpperCase().replace(/[\s-]/g, "").replace(/=+$/, "");
  if (!/^[A-Z2-7]{26}$/.test(clean)) {
    throw new Error("The pairing code is 26 letters and digits (2–7).");
  }
  const p = base32Decode(clean);
  if (p.length !== 16) throw new Error("bad pairing code");
  return p;
}

/** mac = HMAC(P, "pair|" + deviceId + "|" + name + "|" + nonce) */
export function pairMac(P, deviceId, name, nonce) {
  return hmacHex(P, `pair|${deviceId}|${name}|${nonce}`);
}

/** The POST /api/pair body. */
export function buildPairRequest(P, deviceId, name, nonce) {
  for (const [field, v] of [["deviceId", deviceId], ["name", name], ["nonce", nonce]]) {
    if (typeof v !== "string" || v.includes("|")) throw new Error(`${field} must not contain "|"`);
  }
  return { v: 1, deviceId, name, nonce, mac: pairMac(P, deviceId, name, nonce) };
}

/** pad = bytes(HMAC(P, "wrap|" + nonce)), 32 bytes. */
export function wrapPad(P, nonce) {
  return hmacSha256(P, `wrap|${nonce}`);
}

/** wrapped = hex(K XOR pad). (The app does this; the phone uses it only in tests.) */
export function wrapKey(P, nonce, K) {
  const pad = wrapPad(P, nonce);
  if (K.length !== 32) throw new Error("K must be 32 bytes");
  return toHex(K.map((b, i) => b ^ pad[i]));
}

/** K = wrapped XOR pad. */
export function unwrapKey(P, nonce, wrappedHex) {
  const w = fromHex(wrappedHex);
  if (w.length !== 32) throw new Error("wrapped key must be 32 bytes");
  const pad = wrapPad(P, nonce);
  return w.map((b, i) => b ^ pad[i]);
}

/** mac = HMAC(P, "pair-ok|" + deviceId + "|" + wrapped) */
export function pairOkMac(P, deviceId, wrapped) {
  return hmacHex(P, `pair-ok|${deviceId}|${wrapped}`);
}

/**
 * Check a 200 /api/pair response and unwrap K.
 * Returns {hostId, hostName, deviceId, key: Uint8Array(32)}; throws if anything is off.
 */
export function acceptPairResponse(P, deviceId, nonce, resp) {
  if (!resp || resp.v !== 1) throw new Error("unexpected pairing reply (version)");
  if (resp.deviceId !== deviceId) throw new Error("pairing reply is for another device");
  if (typeof resp.wrapped !== "string" || !/^[0-9a-f]{64}$/i.test(resp.wrapped)) {
    throw new Error("pairing reply has no key");
  }
  if (!safeEqual(String(resp.mac || "").toLowerCase(), pairOkMac(P, deviceId, resp.wrapped))) {
    throw new Error("pairing reply failed verification");
  }
  return {
    hostId: resp.hostId,
    hostName: resp.hostName,
    deviceId,
    key: unwrapKey(P, nonce, resp.wrapped),
  };
}

// ---------------------------------------------------------------------------------------
// Signed requests (§3)

/** X-Mac = HMAC(K, method|path|deviceId|time|nonce|SHA256hex(rawBody)) */
export function requestMac(K, method, path, deviceId, time, nonce, rawBody) {
  return hmacHex(K, `${method}|${path}|${deviceId}|${time}|${nonce}|${sha256Hex(rawBody)}`);
}

/** Response X-Mac = HMAC(K, "resp|" + path + "|" + nonce + "|" + SHA256hex(rawResponseBody)) */
export function responseMac(K, path, nonce, rawBody) {
  return hmacHex(K, `resp|${path}|${nonce}|${sha256Hex(rawBody)}`);
}

/**
 * Headers for a signed request. `time` and `nonce` may be supplied (tests); otherwise now
 * and a fresh random nonce. Returns {headers, nonce, time}.
 */
export function signRequest(K, deviceId, method, path, rawBody, time = Date.now(), nonce = newNonce()) {
  const t = String(time);
  return {
    nonce,
    time: t,
    headers: {
      "X-Device": deviceId,
      "X-Time": t,
      "X-Nonce": nonce,
      "X-Mac": requestMac(K, method, path, deviceId, t, nonce, rawBody),
    },
  };
}

/** True when a response's X-Mac matches. */
export function verifyResponse(K, path, nonce, rawBody, macHeader) {
  return safeEqual(String(macHeader || "").toLowerCase(), responseMac(K, path, nonce, rawBody));
}
