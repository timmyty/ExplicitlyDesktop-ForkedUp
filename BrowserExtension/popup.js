const api = globalThis.browser ?? globalThis.chrome;
const button = document.getElementById("toggle");
const detail = document.getElementById("detail");

function show(s) {
  if (!s || s.offline) {
    button.disabled = true;
    button.className = "";
    button.textContent = "App not running";
    detail.textContent = "Open Explicitly Desktop to use the filter.";
    return;
  }
  button.disabled = false;
  button.className = s.enabled ? "on" : "off";
  button.textContent = "Filter Explicit Speech: " + (s.enabled ? "ON" : "OFF");
  if (!s.running) detail.textContent = "Press Start Processing in the app.";
  else if (!s.enabled) detail.textContent = "No delay. Audio plays live, uncensored.";
  else if (s.buffering) detail.textContent = "Buffering, audio resumes shortly…";
  else detail.textContent = `Audio and YouTube video delayed ${(s.delayMs / 1000).toFixed(1)}s.`;
}

const refresh = () => api.runtime.sendMessage({ type: "status" }).then(show);
button.addEventListener("click", () => api.runtime.sendMessage({ type: "toggle" }).then(show));
refresh();
setInterval(refresh, 500);
