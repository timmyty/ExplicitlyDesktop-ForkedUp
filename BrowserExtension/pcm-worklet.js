// The phone's audio track: the app's filtered PCM (from /pcm, relayed by the
// background) played into a MediaStreamAudioDestinationNode. Frames arrive in
// bursts, so they wait in a jitter ring with a small prebuffer. The app's sound
// card and this AudioContext run on different clocks; the ring reads a touch
// faster or slower (resampling, so no clicks) to hold its fill at the target.
//
// Loaded three ways: as the AudioWorklet module (registers the processor), as
// a content script before remote.js (the ScriptProcessor fallback uses the same
// ring), and from Node for the tests.

(() => {
  class JitterRing {
    constructor({ sampleRate = 48000, targetMs = 60, maxMs = 320, capacityMs = 2000 } = {}) {
      this.setRate(sampleRate);
      this.targetMs = targetMs;
      this.maxMs = maxMs;
      this.capacityMs = capacityMs;
      this.alloc();
      this.underruns = 0;
      this.drops = 0;
    }

    setRate(rate) {
      this.rate = rate;
    }

    alloc() {
      this.capacity = Math.ceil(this.rate * this.capacityMs / 1000);
      this.buf = new Float32Array(this.capacity * 2);   // Stereo, interleaved
      this.w = 0;           // Frames written (ever)
      this.r = 0;           // Frames consumed (ever)
      this.frac = 0;        // Fractional read position past r
      this.priming = true;  // Silent until the prebuffer is there
      this.avg = 0;         // Smoothed fill, frames
    }

    get fill() { return this.w - this.r; }
    get fillMs() { return this.fill / this.rate * 1000; }
    get target() { return Math.round(this.rate * this.targetMs / 1000); }

    reset() {
      this.r = this.w;
      this.frac = 0;
      this.priming = true;
    }

    // Interleaved int16; mono is duplicated, extra channels are ignored
    push(pcm, channels = 2, rate = this.rate, discontinuity = false) {
      if (rate !== this.rate) { this.setRate(rate); this.alloc(); }
      else if (discontinuity) this.reset();

      const frames = (pcm.length / channels) | 0;
      const right = channels > 1 ? 1 : 0;
      for (let i = 0; i < frames; i++) {
        const at = ((this.w + i) % this.capacity) * 2;
        this.buf[at] = pcm[i * channels] / 32768;
        this.buf[at + 1] = pcm[i * channels + right] / 32768;
      }
      this.w += frames;

      if (this.fill > this.capacity) this.r = this.w - this.capacity;
      // A backlog (the tab stalled, then a burst arrived) would add latency
      // for good; skip ahead to the target instead
      if (this.fill > this.rate * this.maxMs / 1000) {
        this.r = this.w - this.target;
        this.frac = 0;
        this.drops++;
      }
    }

    // Speed correction from the smoothed fill, none within 5 ms. Two sound
    // cards differ by well under 0.1%; the ±2% limit is for clocks that are
    // really off (a context without a real output device runs ~0.3-1% slow)
    correction() {
      const err = this.avg - this.target;
      if (Math.abs(err) < this.rate * 0.005) return 0;
      return Math.max(-0.02, Math.min(0.02, 0.02 * err / this.target));
    }

    // Fills n samples of left/right at outRate
    pull(left, right, n, outRate = this.rate) {
      if (this.priming) {
        if (this.fill < this.target) { left.fill(0, 0, n); right.fill(0, 0, n); return false; }
        this.priming = false;
        this.avg = this.fill;
      }
      this.avg += (this.fill - this.avg) * 0.003;

      const step = (this.rate / outRate) * (1 + this.correction());
      const avail = this.fill;
      let i = 0;
      for (; i < n; i++) {
        const pos = this.frac + i * step;
        const idx = Math.floor(pos);
        if (idx + 1 >= avail) break;
        const f = pos - idx;
        const a = ((this.r + idx) % this.capacity) * 2;
        const b = ((this.r + idx + 1) % this.capacity) * 2;
        left[i] = this.buf[a] + (this.buf[b] - this.buf[a]) * f;
        right[i] = this.buf[a + 1] + (this.buf[b + 1] - this.buf[a + 1]) * f;
      }
      if (i < n) {
        // Ran dry: play silence and prebuffer again
        left.fill(0, i, n); right.fill(0, i, n);
        this.r = this.w;
        this.frac = 0;
        this.priming = true;
        this.underruns++;
        return false;
      }
      const end = this.frac + n * step;
      const used = Math.floor(end);
      this.r += used;
      this.frac = end - used;
      return true;
    }
  }

  if (typeof registerProcessor === "function" && typeof AudioWorkletProcessor === "function") {
    class PcmProcessor extends AudioWorkletProcessor {
      constructor(options) {
        super();
        const o = options?.processorOptions || {};
        this.ring = new JitterRing({ sampleRate: o.sampleRate || sampleRate, targetMs: o.targetMs || 60 });
        this.quanta = 0;
        this.port.onmessage = (e) => {
          const m = e.data;
          if (m.pcm) this.ring.push(m.pcm, m.channels, m.rate, m.discontinuity);
          else if (m.reset) this.ring.reset();
        };
      }
      process(_inputs, outputs) {
        const out = outputs[0];
        const left = out[0], right = out[1] || out[0];
        this.ring.pull(left, right, left.length, sampleRate);
        // Every ~10 ms: the fill (for the video delay) doubles as remote.js's
        // frame clock, which keeps time in a background window
        if (++this.quanta % 4 === 0) {
          this.port.postMessage({ fillMs: this.ring.fillMs, priming: this.ring.priming, underruns: this.ring.underruns, drops: this.ring.drops });
        }
        return true;
      }
    }
    registerProcessor("explicitly-pcm", PcmProcessor);
    return;
  }

  if (typeof module === "object" && module.exports) module.exports = { JitterRing };
  else globalThis.ExplicitlyJitterRing = JitterRing;
})();
