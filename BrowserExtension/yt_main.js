// Runs in YouTube's own page (MAIN world) from the start, so it sees the
// player's caption requests. YouTube only serves captions to its player (the
// request carries a token), so this reads the caption responses the player
// receives and passes them to content.js. Nothing is changed or blocked.
// content.js can also ask for captions; the player then loads them itself.

(() => {
  const SOURCE = "explicitly-yt";
  const seen = new Map();   // videoId -> caption messages already received

  function report(url, text) {
    try {
      const u = new URL(url, location.href);
      if (u.searchParams.get("fmt") !== "json3") return;
      const msg = {
        source: SOURCE, type: "captions",
        videoId: u.searchParams.get("v"),
        kind: u.searchParams.get("kind") || "manual",
        lang: u.searchParams.get("lang"),
        url: u.href,
        json: text,
      };
      if (!seen.has(msg.videoId)) seen.set(msg.videoId, []);
      seen.get(msg.videoId).push(msg);
      window.postMessage(msg, "*");
    } catch {}
  }

  // Observe XHR (what the player uses) and fetch
  const open = XMLHttpRequest.prototype.open;
  const send = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (method, url) {
    this.__explicitlyUrl = String(url);
    return open.apply(this, arguments);
  };
  XMLHttpRequest.prototype.send = function () {
    if (this.__explicitlyUrl && this.__explicitlyUrl.includes("/api/timedtext")) {
      this.addEventListener("load", () => {
        if (typeof this.responseText === "string" && this.responseText.length) report(this.__explicitlyUrl, this.responseText);
      });
    }
    return send.apply(this, arguments);
  };
  const origFetch = window.fetch;
  window.fetch = async function (input) {
    const response = await origFetch.apply(this, arguments);
    try {
      const url = typeof input === "string" ? input : input.url;
      if (url && url.includes("/api/timedtext")) response.clone().text().then((t) => { if (t) report(url, t); });
    } catch {}
    return response;
  };

  // Requests from content.js
  window.addEventListener("message", async (e) => {
    if (e.source !== window || e.data?.source !== "explicitly-content") return;
    const player = document.getElementById("movie_player");
    if (!player) return;

    if (e.data.type === "loadCaptions") {
      // Captions the player already loaded (e.g. captions were on) count too
      const vid = player.getVideoData?.().video_id;
      for (const msg of seen.get(vid) || []) window.postMessage(msg, "*");

      // Tell the player to load English captions (auto-generated preferred).
      // If captions were off, they are switched off again once received
      const tracks = player.getPlayerResponse?.()?.captions?.playerCaptionsTracklistRenderer?.captionTracks || [];
      if (!tracks.length) {
        window.postMessage({ source: SOURCE, type: "noCaptions", videoId: player.getVideoData?.().video_id }, "*");
        return;
      }
      const asr = tracks.find((t) => t.kind === "asr" && t.languageCode?.startsWith("en"));
      const en = asr || tracks.find((t) => t.languageCode?.startsWith("en")) || tracks[0];
      const wasOn = !!player.getOption?.("captions", "track")?.languageCode;
      player.loadModule?.("captions");
      player.setOption?.("captions", "track", { languageCode: en.languageCode, kind: en.kind || "" });
      if (!wasOn) setTimeout(() => player.unloadModule?.("captions"), 2500);
    }

    if (e.data.type === "fetchVariant" && e.data.url) {
      // Same caption request the player made (with its token), for the
      // auto-generated track, which has per-word timing
      try {
        const r = await origFetch(e.data.url, { credentials: "include" });
        const t = await r.text();
        if (t) report(e.data.url, t);
      } catch {}
    }
  });
})();
