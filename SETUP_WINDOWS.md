# Setting up Explicitly Desktop on Windows

What the app does and how this fork differs from the original: [FORK_CHANGES.md](FORK_CHANGES.md).

## Requirements
- Windows 10/11 x64, Visual Studio 2022 with C++ (its bundled CMake 3.28 works), Git and Git LFS
- [VB-CABLE](https://vb-audio.com/Cable/). If "CABLE Input"/"CABLE Output" are missing from
  Sound settings, check they aren't disabled there.
- An NVIDIA GPU is recommended (small.en and medium.en need one to keep up)

## Build
1. Clone this repo with Git LFS (the Whisper models are in `Models/`).
2. JUCE 8 at `C:\JUCE`: `git clone --depth 1 --branch 8.0.9 https://github.com/juce-framework/JUCE.git C:\JUCE`
3. whisper.cpp v1.9.5 at `C:\whisper.cpp`, built once for its headers and import library:

       git clone --depth 1 --branch v1.9.5 https://github.com/ggml-org/whisper.cpp.git C:\whisper.cpp
       cmake -S C:\whisper.cpp -B C:\whisper.cpp\build -A x64 -DBUILD_SHARED_LIBS=ON -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_TESTS=OFF
       cmake --build C:\whisper.cpp\build --config Release

4. For the GPU: unzip `whisper-bin-win-cuda-12.4.0-x64.zip` from the whisper.cpp release built from
   the same commit (tag `b5454`) to `C:\whisper-cuda`. The build copies its DLLs next to the exe;
   without it the CPU DLLs are used.
5. Build the app:

       cmake -B build -G "Visual Studio 17 2022" -A x64
       cmake --build build --config Release

The app is `build\bin\Release\Explicitly Desktop.exe`.

## First run
1. In the app pick **Input Device** "CABLE Output (VB-Audio Virtual Cable)", your speakers or
   headphones as **Output Device**, a **Whisper Model**, and press **Start Processing**. These
   are remembered, and the app starts filtering by itself next time.
2. The app routes Spotify, Firefox, Chrome and Brave through the filter while processing, and back
   to your normal output on Stop Processing or when it closes. Other programs: add their exe names
   to `routedApps` in the settings file. `Scripts\Route-AppsToFilter.ps1` (and `-Restore`) does the
   same by hand.
3. Optional: **Genius key** for more lyrics (a client access token from genius.com/api-clients).

## Browser extension (YouTube and Steam store video sync)
- **Chrome / Brave:** `chrome://extensions` (or `brave://extensions`), turn on Developer mode,
  Load unpacked, choose the `BrowserExtension` folder.
- **Firefox:** `about:debugging#/runtime/this-firefox`, Load Temporary Add-on, choose
  `BrowserExtension\manifest.json`, then allow its access to 127.0.0.1 and youtube.com in
  `about:addons`. Temporary add-ons are removed when Firefox restarts.

## Phone streaming (optional, off by default)
Watch a video on your phone, played and filtered by this PC (see `docs/MOBILE_STREAMING.md`).
1. Load the browser extension in Chrome or Brave on this PC (above); it plays the video and
   sends it to the phone.
2. In the app, tick **Allow phone streaming**. Windows Firewall asks once about port 8767:
   allow it on **Private** networks only.
3. Press **Pair a phone** and scan the QR code with the phone's camera (same Wi-Fi as the PC),
   or open the address shown and type the code. It works once and expires after 2 minutes.
   Windows then asks whether to allow the phone.
4. **Phones (N)** lists paired phones; pick one to revoke it. While a phone is connected the
   PC's speakers are silent unless **Also play on this PC** is ticked.

The build copies `MobileClient\` next to the exe; the phone page is served from there.
Unit tests for the streaming code (no audio device needed):

    cmake --build build --config Release --target ExplicitlyTests
    build\bin\Release\ExplicitlyTests.exe

(`tests/cpp/build_and_run.sh` builds and runs them with g++ or clang++ on Linux/macOS.)

## Where things are kept
- Settings: `%APPDATA%\ExplicitlyDesktop\ExplicitlyDesktop.settings`
- Lyrics (temp local copies): `%APPDATA%\ExplicitlyDesktop\lyrics` (delete a file to look it up again)
- Genius key: `%APPDATA%\ExplicitlyDesktop\genius_token.txt`
- Paired phones: `%APPDATA%\ExplicitlyDesktop\devices.json` (device keys encrypted with Windows DPAPI
  for your user account)
- Song recognition by audio fingerprint (only used if Windows has no "now playing" info) uses the
  AcoustID key shipped with the app and needs Chromaprint's `fpcalc.exe` next to the app (or
  `EXPLICITLY_FPCALC`)
