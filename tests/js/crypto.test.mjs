// SHA-256, HMAC-SHA256, hex and base32 from MobileClient/crypto.js, checked against
// Node's crypto module, RFC 4231 (HMAC-SHA256) and RFC 4648 (base32).
// Run: node --test tests/js/*.test.mjs  (or: cd tests/js && npm test)
import test from "node:test";
import assert from "node:assert/strict";
import nodeCrypto from "node:crypto";
import {
  sha256Hex, hmacHex, toHex, fromHex, base32Decode, base32Encode, randomBytes,
  uuidv4, newNonce, safeEqual, parsePairingCode,
} from "../../MobileClient/crypto.js";

const nodeSha = (b) => nodeCrypto.createHash("sha256").update(b).digest("hex");
const nodeHmac = (k, m) => nodeCrypto.createHmac("sha256", k).update(m).digest("hex");
const rnd = (n) => new Uint8Array(nodeCrypto.randomBytes(n));

test("SHA-256 known answers", () => {
  assert.equal(sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert.equal(sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert.equal(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  assert.equal(sha256Hex("a".repeat(1000000)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
});

test("SHA-256 matches Node for every length 0..300 and random sizes", () => {
  for (let n = 0; n <= 300; n++) {
    const b = rnd(n);
    assert.equal(sha256Hex(b), nodeSha(b), `length ${n}`);
  }
  for (let i = 0; i < 50; i++) {
    const b = rnd(1 + Math.floor(Math.random() * 20000));
    assert.equal(sha256Hex(b), nodeSha(b));
  }
});

test("SHA-256 hashes strings as UTF-8", () => {
  for (const s of ["Tim’s phone", "héllo wörld", "日本語", "emoji \u{1F600}", "a|b|c"]) {
    assert.equal(sha256Hex(s), nodeSha(Buffer.from(s, "utf8")));
  }
});

test("HMAC-SHA256 RFC 4231 test cases 1-7", () => {
  const rep = (b, n) => new Uint8Array(n).fill(b);
  const cases = [
    [rep(0x0b, 20), "Hi There", "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"],
    ["Jefe", "what do ya want for nothing?", "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"],
    [rep(0xaa, 20), rep(0xdd, 50), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"],
    [Uint8Array.from({ length: 25 }, (_, i) => i + 1), rep(0xcd, 50), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"],
    [rep(0x0c, 20), "Test With Truncation", "a3b6167473100ee06e0c796c2955552b"], // truncated to 128 bits
    [rep(0xaa, 131), "Test Using Larger Than Block-Size Key - Hash Key First", "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"],
    [rep(0xaa, 131), "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.", "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"],
  ];
  cases.forEach(([k, m, want], i) => {
    const got = hmacHex(k, m);
    assert.equal(got.slice(0, want.length), want, `RFC 4231 case ${i + 1}`);
  });
});

test("HMAC-SHA256 matches Node for random keys and messages", () => {
  for (let i = 0; i < 400; i++) {
    const k = rnd(Math.floor(Math.random() * 200));      // includes 0, <64, =64, >64
    const m = rnd(Math.floor(Math.random() * 1000));
    assert.equal(hmacHex(k, m), nodeHmac(k, m));
  }
  for (const n of [0, 63, 64, 65, 128]) {
    const k = rnd(n), m = rnd(n);
    assert.equal(hmacHex(k, m), nodeHmac(k, m), `key/msg length ${n}`);
  }
  assert.equal(hmacHex("", "Tim’s phone"), nodeHmac("", Buffer.from("Tim’s phone")));
});

test("hex round trip and validation", () => {
  for (let i = 0; i < 100; i++) {
    const b = rnd(i);
    const h = toHex(b);
    assert.equal(h, Buffer.from(b).toString("hex"));
    assert.deepEqual(fromHex(h), b);
    assert.deepEqual(fromHex(h.toUpperCase()), b);
  }
  assert.throws(() => fromHex("abc"));
  assert.throws(() => fromHex("zz"));
});

test("base32 RFC 4648 vectors", () => {
  const vec = { "": "", f: "MY", fo: "MZXQ", foo: "MZXW6", foob: "MZXW6YQ", fooba: "MZXW6YTB", foobar: "MZXW6YTBOI" };
  for (const [plain, enc] of Object.entries(vec)) {
    assert.equal(Buffer.from(base32Decode(enc)).toString(), plain);
    assert.equal(Buffer.from(base32Decode(enc.toLowerCase())).toString(), plain);
    assert.equal(base32Encode(Buffer.from(plain)), enc);
  }
  // Padding, dashes and spaces are tolerated.
  assert.equal(Buffer.from(base32Decode("MZXW6YTBOI======")).toString(), "foobar");
  assert.equal(Buffer.from(base32Decode("MZXW-6YTB OI")).toString(), "foobar");
  assert.throws(() => base32Decode("MZXW1"));   // 1 is not in the alphabet
  assert.throws(() => base32Decode("MZ!W"));
});

// Independent bit-string base32 encoder to cross-check random data.
function refB32(bytes) {
  const A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  let bits = [...bytes].map((b) => b.toString(2).padStart(8, "0")).join("");
  while (bits.length % 5) bits += "0";
  let s = "";
  for (let i = 0; i < bits.length; i += 5) s += A[parseInt(bits.slice(i, i + 5), 2)];
  return s;
}

test("base32 decode of random data (incl. 16-byte pairing secrets)", () => {
  for (let i = 0; i < 300; i++) {
    const b = rnd(i % 40);
    const enc = refB32(b);
    assert.deepEqual(base32Decode(enc), b);
    assert.equal(base32Encode(b), enc);
  }
  for (let i = 0; i < 100; i++) {
    const P = rnd(16);
    const enc = refB32(P);
    assert.equal(enc.length, 26);
    const dashed = enc.replace(/(.{4})(?=.)/g, "$1-");
    assert.deepEqual(parsePairingCode(dashed), P);
    assert.deepEqual(parsePairingCode(enc.toLowerCase()), P);
  }
  assert.throws(() => parsePairingCode("ABCD"));
  assert.throws(() => parsePairingCode("A".repeat(25) + "1"));
});

test("random bytes, nonces, uuids", () => {
  const a = randomBytes(32), b = randomBytes(32);
  assert.equal(a.length, 32);
  assert.notDeepEqual(a, b);
  const n = newNonce();
  assert.match(n, /^[0-9a-f]{32}$/);
  const seen = new Set();
  for (let i = 0; i < 200; i++) {
    const u = uuidv4();
    assert.match(u, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    seen.add(u);
  }
  assert.equal(seen.size, 200);
});

test("safeEqual", () => {
  assert.ok(safeEqual("abc", "abc"));
  assert.ok(!safeEqual("abc", "abd"));
  assert.ok(!safeEqual("abc", "ab"));
  assert.ok(!safeEqual(null, "ab"));
});
