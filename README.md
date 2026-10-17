<div align="center">

# 🎧 Explicitly Desktop

**Real-time profanity filter for the music you already play on Windows.**

Mutes explicit words in Spotify, YouTube and any other app *as they play*, keeps YouTube video in sync,
and follows along with the song's lyrics.

[![License: Proprietary](https://img.shields.io/badge/License-Proprietary-red.svg)](LICENSE)
![Platform: Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D6.svg)
![Built with JUCE](https://img.shields.io/badge/Built%20with-JUCE%208-8DC63F.svg)
![Whisper.cpp](https://img.shields.io/badge/ASR-whisper.cpp-555.svg)

[What it does](#-what-it-does) •
[How it works](#-how-it-works) •
[Quick start](#-quick-start) •
[Using it](#-using-it) •
[Troubleshooting](#-troubleshooting)

</div>

> [!NOTE]
> This is an expanded fork of [AndrewMahran7/ExplicitlyDesktop](https://github.com/AndrewMahran7/ExplicitlyDesktop).
> [FORK_CHANGES.md](FORK_CHANGES.md) describes everything it adds and how it differs from the original.

---

## ✨ What it does

| | Feature | Details |
|---|---|---|
| 🔇 | **Live muting** | Explicit words are muted about 3 seconds before you'd hear them, using a profanity word list. Mute or Reverse mode. |
| 🎚️ | **Per-app filtering** | Only the apps you choose (Spotify, Firefox, Chrome, Brave…) go through the filter. Games, calls and system sounds are untouched. |
| ⏯️ | **Instant on/off** | The **Filter Explicit Speech** toggle drops to zero delay when off, from the app or the browser. |
| 📺 | **YouTube in sync** | A browser extension delays the YouTube picture by exactly the audio delay, so lips match the filtered sound. |
| 🧠 | **Whisper on your GPU** | Pick a speech model, from *tiny.en* (lightest) to *medium.en* (most accurate). |
| 📝 | **Lyrics-guided** | Finds the song's lyrics (LRCLIB, Genius, lyrics.ovh) to catch misheard swears and skip false alarms. |
| 🎤 | **Karaoke lyrics** | Shows each lyric line just before it's sung, with the next line underneath, swears masked. |
| 💾 | **Set and forget** | Remembers your devices and settings, and starts filtering when it opens. |

## ⚙️ How it works

```
 Spotify / browsers ──► VB-CABLE ──► Explicitly Desktop ──► your speakers
                                        │
                                        ├─ holds ~3 s of audio
                                        ├─ Whisper transcribes each 2 s slice (+1 s overlap)
                                        ├─ checks words against the word list and the song's lyrics
                                        └─ mutes each explicit word before it plays
```

- **Word timing** comes from Whisper's per-word (DTW) alignment, calibrated on real songs, so a mute covers
  the word and leaves its neighbours alone.
- **Lyrics** are matched to what Whisper hears as the song plays. When they line up, a swear Whisper misheard
  is still muted, and a clean word Whisper mistook for a swear is left alone. Whisper is also given the coming
  lyric lines as a hint, so it hears sung words instead of answering "(upbeat music)".
- **The song** is read from Windows' "now playing" info, which Spotify and browsers provide.

## 🚀 Quick start

**You need:** Windows 10/11 x64 · Visual Studio 2022 (C++) · Git + Git LFS ·
[VB-CABLE](https://vb-audio.com/Cable/) · an NVIDIA GPU for the larger models

```powershell
# 1. This repo (Git LFS fetches the Whisper models)
git clone https://github.com/timmyty/ExplicitlyDesktop-ForkedUp.git
cd ExplicitlyDesktop-ForkedUp

# 2. JUCE 8
git clone --depth 1 --branch 8.0.9 https://github.com/juce-framework/JUCE.git C:\JUCE

# 3. whisper.cpp (headers + import library)
git clone --depth 1 --branch v1.9.5 https://github.com/ggml-org/whisper.cpp.git C:\whisper.cpp
cmake -S C:\whisper.cpp -B C:\whisper.cpp\build -A x64 -DBUILD_SHARED_LIBS=ON -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_TESTS=OFF
cmake --build C:\whisper.cpp\build --config Release

# 4. Build the app
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

> [!TIP]
> **For the GPU,** unzip `whisper-bin-win-cuda-12.4.0-x64.zip` from the whisper.cpp release built from the same
> commit (tag `b5454`) into `C:\whisper-cuda` *before* step 4. The build copies its DLLs next to the app.

Full instructions: [SETUP_WINDOWS.md](SETUP_WINDOWS.md)

## 🎛️ Using it

1. **Start the app** (`build\bin\Release\Explicitly Desktop.exe`). Pick **CABLE Output** as input, your
   speakers or headphones as output, and a Whisper model, then press **Start Processing**. Next time it
   starts on its own.
2. **Route your apps** to the filter. Play something in each, then run:
   ```powershell
   powershell -ExecutionPolicy Bypass -File Scripts\Route-AppsToFilter.ps1      # -Restore to undo
   ```
3. **Load the browser extension** for YouTube sync: open `chrome://extensions` (or `brave://extensions`),
   turn on Developer mode, choose **Load unpacked**, and select the `BrowserExtension` folder.
4. *(Optional)* Click **Genius key** and paste a Genius API client access token for more lyrics.

### Whisper models

| Model | Load | Accuracy | Runs on |
|---|---|---|---|
| tiny.en | ▁ lightest | ★☆☆☆ | CPU |
| base.en | ▃ light | ★★☆☆ | CPU |
| small.en | ▅ medium | ★★★☆ | GPU |
| medium.en | ▇ heaviest | ★★★★ | GPU *(default)* |

Each 2-second slice has to be transcribed within about a second, so only *base.en* and *tiny.en* keep up
without a GPU.

### Where things are kept

| What | Where |
|---|---|
| Settings | `%APPDATA%\ExplicitlyDesktop\ExplicitlyDesktop.settings` |
| Lyrics (temp local copies) | `%APPDATA%\ExplicitlyDesktop\lyrics\` |
| Genius key | `%APPDATA%\ExplicitlyDesktop\genius_token.txt` (never in the repo) |

## 🩺 Troubleshooting

<details>
<summary><b>No sound from Spotify or the browser</b></summary>

Routed apps play into VB-CABLE, so they're silent while Explicitly Desktop is closed. Start the app, or run
`Scripts\Route-AppsToFilter.ps1 -Restore`. If "CABLE Input" or "CABLE Output" is missing, check it isn't
disabled in Windows Sound settings.
</details>

<details>
<summary><b>A swear still gets through</b></summary>

Fast rap and screamed vocals are the hardest. Use *medium.en* on a GPU, and make sure lyrics were found
(**Detected Song** shows "Lyrics: following"). If none were found, add a Genius key or use **Paste lyrics**.
</details>

<details>
<summary><b>"Lyrics: none found"</b></summary>

The song isn't in the free lyrics databases. Add a Genius key, or paste the lyrics yourself with **Paste lyrics**.
If it says "Genius rejected the key", generate a *client access token* (not the client ID) at genius.com/api-clients.
</details>

<details>
<summary><b>YouTube picture and sound don't line up</b></summary>

Check that the extension is loaded and its popup shows the filter state. In Firefox, the extension is only
loaded until the browser restarts.
</details>

## 🙏 Credits & license

Original app by **Andrew Mahran (Explicitly Audio Systems)**. Built with [JUCE](https://juce.com/),
[whisper.cpp](https://github.com/ggml-org/whisper.cpp), [LRCLIB](https://lrclib.net/),
[Genius](https://genius.com/) and [lyrics.ovh](https://lyrics.ovh/).

Proprietary license. See [LICENSE](LICENSE): personal, non-commercial use only.
