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
2. Route the apps to filter: open them, play something in each, then run

       powershell -ExecutionPolicy Bypass -File Scripts\Route-AppsToFilter.ps1

   (Spotify, Firefox, Chrome and Brave by default; `-Apps name,...` for others, `-Restore` to undo.)
   Routed apps are silent while the app is closed.
3. Optional: **Genius key** for more lyrics (a client access token from genius.com/api-clients).

## Browser extension (YouTube video sync)
- **Chrome / Brave:** `chrome://extensions` (or `brave://extensions`), turn on Developer mode,
  Load unpacked, choose the `BrowserExtension` folder.
- **Firefox:** `about:debugging#/runtime/this-firefox`, Load Temporary Add-on, choose
  `BrowserExtension\manifest.json`, then allow its access to 127.0.0.1 and youtube.com in
  `about:addons`. Temporary add-ons are removed when Firefox restarts.

## Where things are kept
- Settings: `%APPDATA%\ExplicitlyDesktop\ExplicitlyDesktop.settings`
- Lyrics (temp local copies): `%APPDATA%\ExplicitlyDesktop\lyrics` (delete a file to look it up again)
- Genius key: `%APPDATA%\ExplicitlyDesktop\genius_token.txt`
- Song recognition by audio fingerprint (only used if Windows has no "now playing" info) needs
  `EXPLICITLY_ACOUSTID_KEY` and `EXPLICITLY_FPCALC` (Chromaprint's fpcalc.exe)
