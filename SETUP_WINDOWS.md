# Running Explicitly Desktop on this PC

## What's where
- App: `build\bin\Release\Explicitly Desktop.exe` (Mute mode, input "CABLE Output", output "Speakers (JBL Clip 4)")
- Settings it remembers: `%APPDATA%\ExplicitlyDesktop\ExplicitlyDesktop.settings`
- Dependencies: JUCE 8.0.9 at `C:\JUCE`, whisper.cpp v1.9.5 (CPU build) at `C:\whisper.cpp\build`

## Daily use
1. Start **Explicitly Desktop**. It starts filtering by itself with the last devices used.
   Apps routed to VB-CABLE are silent while the app is closed.
2. **Filter Explicit Speech** button (in the app or the browser extension):
   - ON: explicit words muted, audio about 3 s behind (YouTube video delayed to match)
   - OFF: audio passes straight through with no added delay

## Routing apps to the filter
Spotify, Firefox, Chrome and Brave are sent to "CABLE Input" with Windows' per-app output setting.
Open the apps, play something in each, then run:

    powershell -ExecutionPolicy Bypass -File Scripts\Route-AppsToFilter.ps1

Undo with `-Restore`. Settings > System > Sound > Volume mixer shows the same setting.

## Browser extension (YouTube video sync)
- **Chrome / Brave:** `chrome://extensions` (or `brave://extensions`), turn on Developer mode,
  Load unpacked, choose the `BrowserExtension` folder.
- **Firefox:** `about:debugging#/runtime/this-firefox`, Load Temporary Add-on, choose
  `BrowserExtension\manifest.json`. Then in `about:addons` > Explicitly Video Sync > Permissions,
  allow access to 127.0.0.1 and youtube.com. Firefox removes temporary add-ons when it restarts;
  keeping it permanently needs the add-on signed through addons.mozilla.org (unlisted).

## Rebuild
    cmake -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
