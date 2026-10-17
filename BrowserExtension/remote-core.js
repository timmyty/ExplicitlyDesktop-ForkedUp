// Pure helpers for phone streaming (docs/STREAMING_PROTOCOL.md), shared by
// background.js, remote.js and the Node tests. No DOM or extension APIs here.
// Loads as a classic script (content script, importScripts in the service
// worker, Firefox background script) or as a CommonJS module in Node.

(function (root, factory) {
  const core = factory();
  if (typeof module === "object" && module.exports) module.exports = core;
  else root.ExplicitlyRemoteCore = core;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  // ---- Sources (§4.1 load) ----------------------------------------------------
  const VIDEO_ID = /^[A-Za-z0-9_-]{11}$/;

  // Only the four forms the protocol lists, over https. Returns the desktop
  // watch URL the host tab opens, or null ("unsupported source").
  function parseYouTubeUrl(input) {
    if (typeof input !== "string" || input.length > 2048) return null;
    let u;
    try { u = new URL(input.trim()); } catch { return null; }
    if (u.protocol !== "https:" || u.username || u.password || u.port) return null;

    let id = null;
    if ((u.hostname === "www.youtube.com" || u.hostname === "m.youtube.com") && u.pathname === "/watch") {
      id = u.searchParams.get("v");
    } else if (u.hostname === "youtu.be") {
      id = u.pathname.slice(1);
    } else if (u.hostname === "www.youtube.com" && u.pathname.startsWith("/shorts/")) {
      id = u.pathname.slice("/shorts/".length).replace(/\/$/, "");
    }
    if (!id || !VIDEO_ID.test(id)) return null;

    // Keep a start time (t=90, t=90s, t=1m30s) so shared links resume where meant
    let start = 0;
    const t = u.searchParams.get("t") || u.searchParams.get("start");
    if (t) {
      const m = /^(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)s?)?$/.exec(t);
      if (m) start = (+m[1] || 0) * 3600 + (+m[2] || 0) * 60 + (+m[3] || 0);
    }
    const watchUrl = "https://www.youtube.com/watch?v=" + id + (start ? "&t=" + start + "s" : "");
    return { videoId: id, start, watchUrl };
  }

  // ---- PCM frames (§5.1) ------------------------------------------------------
  const PCM_HEADER = 28;
  const FLAG_DISCONTINUITY = 1, FLAG_FILTER = 2, FLAG_HELD = 4;

  function parsePcmFrame(data) {
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    if (bytes.length < PCM_HEADER) return null;
    if (bytes[0] !== 0x45 || bytes[1] !== 0x58 || bytes[2] !== 0x50 || bytes[3] !== 0x43) return null;   // "EXPC"
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
    const sampleRate = dv.getUint32(4, true);
    const channels = dv.getUint16(8, true);
    const flags = dv.getUint16(10, true);
    const startSample = Number(dv.getBigUint64(12, true));
    const frames = dv.getUint32(20, true);
    const delaySamples = dv.getUint32(24, true);
    if (!sampleRate || !channels || channels > 8) return null;
    if (bytes.length < PCM_HEADER + frames * channels * 2) return null;

    // Copy the samples out: the header leaves them 4-aligned only by luck of the
    // source buffer, and the copy can be transferred to the worklet
    const pcm = new Int16Array(frames * channels);
    new Uint8Array(pcm.buffer).set(bytes.subarray(PCM_HEADER, PCM_HEADER + pcm.byteLength));
    if (!littleEndian()) for (let i = 0; i < pcm.length; i++) pcm[i] = dv.getInt16(PCM_HEADER + 2 * i, true);

    return {
      sampleRate, channels, flags, startSample, frames, delaySamples, pcm,
      discontinuity: !!(flags & FLAG_DISCONTINUITY),
      filterActive: !!(flags & FLAG_FILTER),
      held: !!(flags & FLAG_HELD),
    };
  }

  // The app's side, for tests and the e2e fake app
  function buildPcmFrame({ sampleRate = 48000, channels = 2, flags = 0, startSample = 0, delaySamples = 0, pcm }) {
    const frames = pcm.length / channels;
    const out = new Uint8Array(PCM_HEADER + pcm.length * 2);
    const dv = new DataView(out.buffer);
    out.set([0x45, 0x58, 0x50, 0x43]);
    dv.setUint32(4, sampleRate, true);
    dv.setUint16(8, channels, true);
    dv.setUint16(10, flags, true);
    dv.setBigUint64(12, BigInt(startSample), true);
    dv.setUint32(20, frames, true);
    dv.setUint32(24, delaySamples, true);
    for (let i = 0; i < pcm.length; i++) dv.setInt16(PCM_HEADER + 2 * i, pcm[i], true);
    return out;
  }

  function littleEndian() {
    return new Uint8Array(new Uint16Array([1]).buffer)[0] === 1;
  }

  // ---- Base64 (PCM over the extension Port, which only carries JSON) -----------
  const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const B64_INDEX = new Uint8Array(128);
  for (let i = 0; i < B64.length; i++) B64_INDEX[B64.charCodeAt(i)] = i;

  function bytesToBase64(bytes) {
    if (typeof bytes.toBase64 === "function") return bytes.toBase64();
    if (typeof btoa === "function") {
      let s = "";
      for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
      return btoa(s);
    }
    return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.length).toString("base64");
  }

  function base64ToBytes(s) {
    if (typeof Uint8Array.fromBase64 === "function") return Uint8Array.fromBase64(s);
    // A table decode beats atob + charCodeAt by about 2x and has no string copy
    let len = s.length;
    while (len && s.charCodeAt(len - 1) === 61) len--;   // '='
    const out = new Uint8Array((len * 3) >> 2);
    let o = 0, i = 0;
    for (; i + 4 <= len; i += 4) {
      const n = (B64_INDEX[s.charCodeAt(i)] << 18) | (B64_INDEX[s.charCodeAt(i + 1)] << 12) |
                (B64_INDEX[s.charCodeAt(i + 2)] << 6) | B64_INDEX[s.charCodeAt(i + 3)];
      out[o++] = n >> 16; out[o++] = (n >> 8) & 255; out[o++] = n & 255;
    }
    if (len - i === 2) {
      out[o++] = ((B64_INDEX[s.charCodeAt(i)] << 2) | (B64_INDEX[s.charCodeAt(i + 1)] >> 4)) & 255;
    } else if (len - i === 3) {
      const n = (B64_INDEX[s.charCodeAt(i)] << 10) | (B64_INDEX[s.charCodeAt(i + 1)] << 4) | (B64_INDEX[s.charCodeAt(i + 2)] >> 2);
      out[o++] = (n >> 8) & 255; out[o++] = n & 255;
    }
    return out;
  }

  // ---- Timing (§7) ------------------------------------------------------------
  // "delayed": filter on, app delays. "caption": content.js mutes from captions,
  // app bypassed. "live": filter off or app not processing.
  function streamMode(status, captionOn) {
    if (!status || status.offline || !status.running || !status.enabled) return "live";
    return captionOn ? "caption" : "delayed";
  }

  // Audio on the phone path lags the tab's own output by the cable, the app,
  // the frame packing, the relay and the worklet's prebuffer
  function outputLatencyMs({ packMs = 20, relayMs = 0, ringMs = 0, cableMs = 30 } = {}) {
    return Math.max(0, packMs) + Math.max(0, relayMs) + Math.max(0, ringMs) + Math.max(0, cableMs);
  }

  // The video delay. The protocol says caption/live video goes out "undelayed",
  // but the audio still takes the relay path, so those modes get outputLatency
  // alone (about 0.1 s) rather than 0
  function videoDelayMs(mode, delayMs, outLatencyMs) {
    const out = Math.max(0, outLatencyMs || 0);
    return mode === "delayed" ? Math.max(0, delayMs || 0) + out : out;
  }

  // Smooths the measured delay so frame pacing doesn't jitter with every
  // relay sample; jumps (filter toggled, app delay changed) apply at once
  class DelaySmoother {
    constructor({ tauMs = 2000, jumpMs = 250 } = {}) {
      this.tauMs = tauMs;
      this.jumpMs = jumpMs;
      this.value = null;
      this.last = 0;
    }
    update(target, now) {
      if (this.value === null || Math.abs(target - this.value) >= this.jumpMs) {
        this.value = target;
      } else {
        const dt = Math.max(0, Math.min(1000, now - this.last));
        this.value += (target - this.value) * (1 - Math.exp(-dt / this.tauMs));
      }
      this.last = now;
      return this.value;
    }
  }

  // A clock that stops while the app holds (paused), so queued frames keep
  // their place relative to the held audio
  class QueueClock {
    constructor(now = () => performance.now()) {
      this.realNow = now;
      this.pausedTotal = 0;
      this.frozenAt = null;
    }
    now() {
      return (this.frozenAt ?? this.realNow()) - this.pausedTotal;
    }
    get frozen() { return this.frozenAt !== null; }
    freeze() { if (this.frozenAt === null) this.frozenAt = this.realNow(); }
    thaw() {
      if (this.frozenAt === null) return;
      this.pausedTotal += this.realNow() - this.frozenAt;
      this.frozenAt = null;
    }
    // Queue time -> real (performance) time, for frame timestamps
    toReal(t) { return t + this.pausedTotal; }
  }

  // Frames waiting for their time, oldest first
  class FrameQueue {
    constructor({ max = 1200, release = () => {} } = {}) {
      this.items = [];
      this.max = max;
      this.release = release;
      this.emitted = false;   // A frame went out since the last clear
    }
    get length() { return this.items.length; }
    push(t, item) {
      this.items.push({ t, item });
      while (this.items.length > this.max) this.release(this.items.shift().item);
    }
    // The newest frame that is due; older due ones are dropped
    takeDue(now, delay) {
      const due = now - delay;
      let idx = -1;
      for (let i = 0; i < this.items.length && this.items[i].t <= due; i++) idx = i;
      if (idx < 0) return null;
      for (let i = 0; i < idx; i++) this.release(this.items[i].item);
      const [entry] = this.items.splice(0, idx + 1).slice(-1);
      this.emitted = true;
      return entry;
    }
    clear() {
      for (const e of this.items) this.release(e.item);
      this.items = [];
      this.emitted = false;
    }
  }

  // §4.1 state.time: what the phone is showing
  function shownTime({ currentTime, mode, delayMs, seekAnchor = null, queueFilled = true }) {
    if (mode !== "delayed") return Math.max(0, currentTime || 0);
    // Right after a seek the phone still holds the last frame; report the target
    if (!queueFilled && seekAnchor !== null) return seekAnchor;
    return Math.max(0, (currentTime || 0) - (delayMs || 0) / 1000);
  }

  // ---- SDP --------------------------------------------------------------------
  const OPUS_PARAMS = { stereo: "1", "sprop-stereo": "1", maxaveragebitrate: "128000" };

  // Opus stereo at 128 kbps. Applied to the phone's offer (configures this
  // side's encoder) and to the answer (tells the phone what it gets)
  function mungeOpus(sdp, params = OPUS_PARAMS) {
    const eol = sdp.includes("\r\n") ? "\r\n" : "\n";
    const lines = sdp.split(eol);
    const pts = new Set();
    for (const l of lines) {
      const m = /^a=rtpmap:(\d+) opus\/48000/i.exec(l);
      if (m) pts.add(m[1]);
    }
    if (!pts.size) return sdp;
    const withFmtp = new Set();
    for (let i = 0; i < lines.length; i++) {
      const m = /^a=fmtp:(\d+) (.*)$/.exec(lines[i]);
      if (!m || !pts.has(m[1])) continue;
      withFmtp.add(m[1]);
      lines[i] = "a=fmtp:" + m[1] + " " + setFmtp(m[2], params);
    }
    for (const pt of pts) {
      if (withFmtp.has(pt)) continue;
      const at = lines.findIndex((l) => l.startsWith("a=rtpmap:" + pt + " "));
      lines.splice(at + 1, 0, "a=fmtp:" + pt + " " + setFmtp("", params));
    }
    return lines.join(eol);
  }

  function setFmtp(existing, params) {
    const parts = existing.split(";").map((p) => p.trim()).filter(Boolean);
    const keys = parts.map((p) => p.split("=")[0].trim().toLowerCase());
    for (const [k, v] of Object.entries(params)) {
      const i = keys.indexOf(k);
      if (i >= 0) parts[i] = k + "=" + v;
      else { parts.push(k + "=" + v); keys.push(k); }
    }
    return parts.join(";");
  }

  // Codec order for setCodecPreferences: H.264 (packetization-mode=1 and
  // constrained baseline first, what phones decode in hardware), then VP8,
  // then everything else as the browser listed it (RTX, RED, FEC stay in)
  function orderCodecs(codecs, kind) {
    const rank = (c) => {
      const mime = (c.mimeType || "").toLowerCase();
      const fmtp = (c.sdpFmtpLine || "").toLowerCase();
      if (kind === "audio") return mime === "audio/opus" ? 0 : 1;
      if (mime === "video/h264") {
        let r = 0;
        if (!/packetization-mode=1/.test(fmtp)) r += 2;
        if (!/profile-level-id=42e0/.test(fmtp)) r += 1;
        return r;
      }
      if (mime === "video/vp8") return 10;
      return 20;
    };
    return codecs.map((c, i) => ({ c, i, r: rank(c) }))
      .sort((a, b) => a.r - b.r || a.i - b.i)
      .map((x) => x.c);
  }

  // The DTLS fingerprint identifies the phone's RTCPeerConnection: an offer with
  // the same one is an ICE restart of the running session, not a new one
  function sdpFingerprint(sdp) {
    const m = /^a=fingerprint:(.+)$/m.exec(sdp || "");
    return m ? m[1].trim().toLowerCase() : null;
  }

  // ---- Search (§4.1 search) ---------------------------------------------------
  function searchUrl(q) {
    return "https://www.youtube.com/results?search_query=" + encodeURIComponent(String(q || "").slice(0, 200));
  }

  // ytInitialData is a JS object literal in a <script>; find its end by
  // matching braces outside strings
  function extractInitialData(html) {
    const m = /(?:var ytInitialData|window\["ytInitialData"\])\s*=\s*\{/.exec(html);
    if (!m) return null;
    const start = m.index + m[0].length - 1;
    let depth = 0, inStr = false, esc = false;
    for (let i = start; i < html.length; i++) {
      const c = html.charCodeAt(i);
      if (inStr) {
        if (esc) esc = false;
        else if (c === 92) esc = true;        // backslash
        else if (c === 34) inStr = false;     // "
        continue;
      }
      if (c === 34) inStr = true;
      else if (c === 123) depth++;
      else if (c === 125 && --depth === 0) {
        try { return JSON.parse(html.slice(start, i + 1)); } catch { return null; }
      }
    }
    return null;
  }

  const text = (t) => typeof t === "string" ? t : t?.simpleText ?? t?.runs?.map((r) => r.text).join("") ?? t?.content ?? "";
  const lastThumb = (list) => {
    const url = list?.length ? list[list.length - 1].url : "";
    return url.startsWith("//") ? "https:" + url : url;
  };

  function itemFromVideoRenderer(v) {
    if (!v?.videoId || !VIDEO_ID.test(v.videoId)) return null;
    const live = v.badges?.some((b) => b.metadataBadgeRenderer?.style === "BADGE_STYLE_TYPE_LIVE_NOW");
    return {
      url: "https://www.youtube.com/watch?v=" + v.videoId,
      title: text(v.title),
      channel: text(v.ownerText) || text(v.longBylineText) || text(v.shortBylineText),
      duration: text(v.lengthText) || (live ? "LIVE" : ""),
      thumb: lastThumb(v.thumbnail?.thumbnails),
    };
  }

  // Newer result layout
  function itemFromLockup(l) {
    if (l?.contentType !== "LOCKUP_CONTENT_TYPE_VIDEO" || !VIDEO_ID.test(l.contentId || "")) return null;
    const meta = l.metadata?.lockupMetadataViewModel;
    const rows = meta?.metadata?.contentMetadataViewModel?.metadataRows || [];
    const thumbVm = l.contentImage?.thumbnailViewModel;
    const badge = thumbVm?.overlays?.flatMap((o) => o.thumbnailOverlayBadgeViewModel?.thumbnailBadges || [])[0];
    return {
      url: "https://www.youtube.com/watch?v=" + l.contentId,
      title: text(meta?.title),
      channel: text(rows[0]?.metadataParts?.[0]?.text),
      duration: text(badge?.thumbnailBadgeViewModel?.text),
      thumb: lastThumb(thumbVm?.image?.sources),
    };
  }

  // Videos in page order (ads and promoted items use other renderers and are
  // not results), up to max
  function parseSearchResults(html, max = 20) {
    const data = typeof html === "string" ? extractInitialData(html) : html;
    const items = [];
    const seen = new Set();
    const stack = data ? [data] : [];
    while (stack.length && items.length < max) {
      const node = stack.pop();
      if (!node || typeof node !== "object") continue;
      let item = null;
      if (node.videoRenderer) item = itemFromVideoRenderer(node.videoRenderer);
      else if (node.lockupViewModel) item = itemFromLockup(node.lockupViewModel);
      if (item) {
        if (!seen.has(item.url)) { seen.add(item.url); items.push(item); }
        continue;
      }
      // Depth first, in document order
      const values = Array.isArray(node) ? node : Object.values(node);
      for (let i = values.length - 1; i >= 0; i--) if (values[i] && typeof values[i] === "object") stack.push(values[i]);
    }
    return items;
  }

  // ---- DataChannel messages (§4.1) --------------------------------------------
  const PHONE_TYPES = new Set(["load", "play", "pause", "seek", "volume", "filter", "search", "ping"]);

  // Parses and checks a phone message; null if it isn't one
  function parseControl(data) {
    let m;
    try { m = typeof data === "string" ? JSON.parse(data) : null; } catch { return null; }
    if (!m || typeof m !== "object" || !PHONE_TYPES.has(m.t)) return null;
    switch (m.t) {
      case "load": return typeof m.url === "string" ? m : null;
      case "seek": return Number.isFinite(m.time) && m.time >= 0 ? m : null;
      case "volume": return Number.isFinite(m.value) ? { ...m, value: Math.min(1, Math.max(0, m.value)) } : null;
      case "filter": return typeof m.on === "boolean" ? m : null;
      case "search": return typeof m.q === "string" ? m : null;
      default: return m;
    }
  }

  return {
    parseYouTubeUrl, parsePcmFrame, buildPcmFrame, bytesToBase64, base64ToBytes,
    streamMode, outputLatencyMs, videoDelayMs, DelaySmoother, QueueClock, FrameQueue, shownTime,
    mungeOpus, orderCodecs, sdpFingerprint, searchUrl, extractInitialData, parseSearchResults, parseControl,
    PCM_HEADER, FLAG_DISCONTINUITY, FLAG_FILTER, FLAG_HELD,
  };
});
