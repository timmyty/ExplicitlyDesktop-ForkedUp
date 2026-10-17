// Unit tests for the jitter ring in BrowserExtension/pcm-worklet.js
const test = require("node:test");
const assert = require("node:assert/strict");
const { JitterRing } = require("../../BrowserExtension/pcm-worklet.js");

const Q = 128;   // Render quantum

function frame(n, startSample, rate = 48000, freq = 1000) {
  const pcm = new Int16Array(n * 2);
  for (let i = 0; i < n; i++) {
    const v = Math.round(Math.sin(2 * Math.PI * freq * (startSample + i) / rate) * 16000);
    pcm[2 * i] = v; pcm[2 * i + 1] = -v;
  }
  return pcm;
}

// Feeds 20 ms frames on the source clock and pulls quanta on the output clock
function run(ring, { seconds, srcRate = 48000, outRate = 48000, srcSpeed = 1, onQuantum }) {
  const L = new Float32Array(Q), R = new Float32Array(Q);
  const frameN = srcRate / 50;
  let sent = 0, t = 0, nextFrame = 0;
  const dt = Q / outRate;
  while (t < seconds) {
    while (nextFrame <= t) {
      ring.push(frame(frameN, sent, srcRate), 2, srcRate, false);
      sent += frameN;
      nextFrame += frameN / (srcRate * srcSpeed);
    }
    ring.pull(L, R, Q, outRate);
    onQuantum?.(L, R, t);
    t += dt;
  }
}

test("prebuffers 60 ms of silence, then plays", () => {
  const ring = new JitterRing({ sampleRate: 48000, targetMs: 60 });
  const L = new Float32Array(Q), R = new Float32Array(Q);
  ring.push(frame(960, 0), 2, 48000);
  assert.equal(ring.pull(L, R, Q), false);
  assert.ok(L.every((x) => x === 0));
  ring.push(frame(960, 960), 2, 48000);
  ring.push(frame(960, 1920), 2, 48000);   // 60 ms now
  assert.equal(ring.pull(L, R, Q), true);
  assert.ok(Math.abs(L[10] - Math.round(Math.sin(2 * Math.PI * 1000 * 10 / 48000) * 16000) / 32768) < 1e-6);
  assert.ok(Math.abs(R[10] + L[10]) < 1e-6);
  assert.equal(ring.fill, 2880 - 128);
});

test("steady state holds the fill near the target and never underruns", () => {
  const ring = new JitterRing({ sampleRate: 48000, targetMs: 60 });
  run(ring, { seconds: 10 });
  assert.equal(ring.underruns, 0);
  assert.ok(Math.abs(ring.avg / 48 - 60) < 12, `avg fill ${ring.avg / 48} ms`);
});

test("clock drift is absorbed without underruns or drops", () => {
  for (const speed of [1.002, 0.998, 1.01, 0.99]) {   // ±2000 ppm is far worse than real sound cards; ±1% is a fake output device
    const ring = new JitterRing({ sampleRate: 48000, targetMs: 60 });
    const fills = [];
    run(ring, { seconds: 60, srcSpeed: speed, onQuantum: (_l, _r, t) => { if (t > 30) fills.push(ring.fillMs); } });
    const mean = fills.reduce((a, b) => a + b, 0) / fills.length;
    assert.equal(ring.underruns, 0, `speed ${speed}`);
    assert.equal(ring.drops, 0, `speed ${speed}`);
    assert.ok(Math.abs(mean - 60) < 15, `speed ${speed}: mean fill ${mean} ms`);
  }
});

test("discontinuity resets to a fresh prebuffer", () => {
  const ring = new JitterRing({ sampleRate: 48000, targetMs: 60 });
  run(ring, { seconds: 1 });
  ring.push(frame(960, 0), 2, 48000, true);
  assert.equal(ring.fill, 960);
  assert.equal(ring.priming, true);
});

test("a backlog is cut back to the target", () => {
  const ring = new JitterRing({ sampleRate: 48000, targetMs: 60, maxMs: 320 });
  for (let i = 0; i < 20; i++) ring.push(frame(960, i * 960), 2, 48000);   // 400 ms burst
  assert.equal(ring.drops, 1);
  assert.equal(ring.fillMs, 120);   // Cut to 60 ms at 340 ms, then three more frames
});

test("underrun plays silence and prebuffers again", () => {
  const ring = new JitterRing({ sampleRate: 48000, targetMs: 60 });
  const L = new Float32Array(Q), R = new Float32Array(Q);
  for (let i = 0; i < 3; i++) ring.push(frame(960, i * 960), 2, 48000);
  for (let i = 0; i < 30; i++) ring.pull(L, R, Q);
  assert.equal(ring.underruns, 1);
  assert.equal(ring.priming, true);
  assert.ok(L.every((x) => x === 0));
});

test("44.1 kHz source resampled to 48 kHz stays a clean sine", () => {
  const ring = new JitterRing({ sampleRate: 44100, targetMs: 60 });
  const out = [];
  run(ring, { seconds: 2, srcRate: 44100, outRate: 48000, onQuantum: (L, _r, t) => { if (t > 1) out.push(...L); } });
  assert.equal(ring.underruns, 0);
  // A 1 kHz sine at 48 kHz: x[n+1] + x[n-1] = 2 cos(w) x[n]
  const c = 2 * Math.cos(2 * Math.PI * 1000 / 48000);
  let worst = 0;
  for (let i = 1; i < out.length - 1; i++) worst = Math.max(worst, Math.abs(out[i + 1] + out[i - 1] - c * out[i]));
  assert.ok(worst < 0.01, `distortion ${worst}`);
});

test("mono source is duplicated", () => {
  const ring = new JitterRing({ sampleRate: 48000, targetMs: 1 });
  ring.push(Int16Array.from({ length: 200 }, (_, i) => i * 100), 1, 48000);
  const L = new Float32Array(Q), R = new Float32Array(Q);
  ring.pull(L, R, Q);
  assert.deepEqual(Array.from(L), Array.from(R));
  assert.ok(L[5] > 0);
});
