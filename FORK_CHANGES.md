# Explicitly Desktop: what this fork does

This fork of [AndrewMahran7/ExplicitlyDesktop](https://github.com/AndrewMahran7/ExplicitlyDesktop)
turns the original real-time profanity filter into something you can leave running while you
listen to Spotify or watch YouTube on Windows. It mutes explicit words in songs as they play,
keeps YouTube video in sync with the delayed audio, follows along with the song's lyrics, and
can be switched off instantly with no delay.

## What the app does

### Live filtering of any app's audio
- Apps you choose (for example Spotify, Firefox, Chrome and Brave) play into the VB-CABLE virtual
  audio device. Explicitly Desktop listens to that cable, holds the audio for about 3.5 seconds while
  it transcribes it, mutes the explicit words, and plays the result on your speakers or headphones.
- Only the apps you route are delayed. Games, calls and system sounds keep playing normally.
  `Scripts\Route-AppsToFilter.ps1` sets this per-app routing (the same setting as Windows'
  Volume mixer), and `-Restore` puts the apps back.
- The censor mode is Mute by default (Reverse is still available).

### Filter Explicit Speech toggle
- One button turns filtering on or off. **Off** passes audio straight through with no added delay.
  **On** holds about 3.5 seconds of audio again (heard as a short silence while it refills).
- The same toggle is in the browser extension's popup.
- The app remembers your devices, censor mode, toggle state and model, and starts filtering by
  itself when opened, so routed apps are never left silent.

### YouTube video kept in sync
- A small browser extension (`BrowserExtension/`, for Chrome, Brave and Firefox) delays the YouTube
  picture by exactly the app's current audio delay while the filter is on, so lips match the
  filtered sound. Pausing and seeking stay in sync because the picture follows the same buffered
  pipeline as the audio. With the filter off, the extension steps aside.
- The app tells the extension its state and delay over a control port on `127.0.0.1:8766`.
  Requests from web pages are refused, so a website can't switch the filter off.

### Speech recognition
- Whisper (via whisper.cpp) runs on the GPU when available. A model picker offers four English
  models labelled by performance impact:
  - tiny.en: lightest load, least accurate
  - base.en: light load, fine on a CPU
  - small.en: medium load, needs a GPU
  - medium.en: heaviest load, most accurate, needs a GPU (the default)
- Each 2-second slice is transcribed together with the second before it, so words cut by a slice
  boundary are heard whole. A word found twice is muted only once.
- Per-word timing comes from Whisper's DTW alignment, calibrated against recordings with known word
  positions. Mutes cover the word itself: the first consonant may come through, the next word is
  left alone.

### Song lyrics
- The app reads what's playing from Windows' "now playing" information (Spotify and browsers
  provide it), and prefers whichever app is actually playing.
- It looks the lyrics up on **LRCLIB**, then **Genius** (with your own API key), then
  **lyrics.ovh**. Spotify-style suffixes ("- Acoustic", "- From ...", "- Remastered") and multiple
  artists are handled, results must match the artist and title, and busy lookups are retried.
  Lyrics are kept as a temp local copy in `%APPDATA%\ExplicitlyDesktop\lyrics`, so each song is
  looked up once.
- **Paste lyrics** saves your own copy for the current song. **Preview Full Lyrics** shows the
  whole song (swears masked).
- The lyrics are used three ways:
  1. **Fewer false mutes:** if Whisper hears a swear where the lyrics have a similar-sounding
     clean word, it isn't muted.
  2. **Catching mishearings:** a swear in the lyrics that Whisper misheard or missed is still
     muted, timed from the words around it.
  3. **Better hearing:** Whisper is given the upcoming lyric lines as a hint, so it hears sung
     words instead of answering "(upbeat music)".
  Censored ("f**k") copies of lyrics never cancel a mute. With no lyrics, Whisper decides alone.
- **Actual Lyrics** shows karaoke-style lines: each line appears just before it is sung, with the
  next line underneath.

### Genius key
- The **Genius key** button stores your Genius API client access token in
  `%APPDATA%\ExplicitlyDesktop\genius_token.txt`. It is never written into the source or the repo.
  The app shows when Genius rejects a key.
- Genius's API finds the song; the lyrics text is read from the song's page, which Genius's terms
  do not allow. Use it for personal listening only, and expect it to break if Genius changes the page.

## Differences from the original

| Area | Original | This fork |
|---|---|---|
| Building on Windows | README described a JUCE submodule and a CUDA whisper build that the CMake file didn't match | Builds against JUCE 8 and a local whisper.cpp; uses the official whisper.cpp CUDA DLLs when present |
| Audio routing | Whole-system output to VB-CABLE | Per-app routing script; only chosen apps are delayed |
| On/off | Start/Stop only | Filter Explicit Speech toggle with zero-delay pass-through, also in the browser |
| Video | Audio only | Browser extension delays YouTube video to match |
| Model | base.en on the CPU, fixed | Picker from tiny.en to medium.en; GPU by default |
| Word timing | Words spread evenly across Whisper segments (DTW was switched off by a default) | DTW enabled and calibrated; overlapping windows; one mute per word |
| Lyrics | Fetched from lyrics.ovh but the matching was disabled | LRCLIB, Genius and lyrics.ovh; lyric-guided mute decisions; Whisper prompting; paste and preview |
| Lyrics display | Repeated Whisper's words | Karaoke-style matched lyric lines, swears masked |
| Settings | Chosen again on every launch | Remembered; auto-starts filtering |
| Stop then Start | Failed (the model was freed on Stop) | Works |
| Secrets | Personal fpcalc path hard-coded | AcoustID app key kept (overridable with `EXPLICITLY_ACOUSTID_KEY`); fpcalc from `EXPLICITLY_FPCALC` or next to the app; personal key files ignored |

Song recognition by audio fingerprint (AcoustID) is only a fallback for when Windows has no "now playing" info, and needs Chromaprint's `fpcalc.exe`.

## Testing tools
- `EXPLICITLY_DEBUG_RECORD=<folder>` records the app's input and output (60 s, or a rolling
  window with `EXPLICITLY_DEBUG_ROLLING_SECONDS=N`, saved by `POST /debug/dump` on the control port).
- `EXPLICITLY_DEBUG_SONG="Artist|Title"` pins the song for lyrics; `EXPLICITLY_OUTPUT_DEVICE`
  plays to another device without saving it; `EXPLICITLY_WHISPER_MODEL` overrides the model.

## Known limits
- Accuracy over loud music depends on Whisper; fast rap and screamed vocals are the hardest.
  Lyrics help a lot when they are found.
- Firefox only loads the extension temporarily unless it is signed through addons.mozilla.org.
- About 3.5 seconds of delay while filtering is inherent to checking words before you hear them.

See [SETUP_WINDOWS.md](SETUP_WINDOWS.md) for setup.
