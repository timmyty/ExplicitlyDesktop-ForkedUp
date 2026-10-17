# Explicitly Desktop - Real-Time Profanity Filter

> **This is an expanded fork.** It adds per-app routing, a zero-delay Filter Explicit Speech
> toggle, YouTube video sync, a Whisper model picker with GPU support, lyrics-guided muting
> (LRCLIB, Genius, lyrics.ovh) and karaoke-style lyrics. See [FORK_CHANGES.md](FORK_CHANGES.md)
> for what it does and how it differs, and [SETUP_WINDOWS.md](SETUP_WINDOWS.md) to build and run it.


[![License: Proprietary](https://img.shields.io/badge/License-Proprietary-red.svg)](LICENSE)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

**Real-time profanity filtering for music and audio** • Built with JUCE, Whisper.cpp & Vocal Isolation

[Features](#features) • [Getting Started](#getting-started) • [Setup Guide](#setup-guide) • [Contributing](#contributing)

---

## Features

🎵 **Music Filtering** - Filters profanity from music with vocals  
🎤 **Whisper ASR** - Accurate speech recognition using Whisper tiny.en model  
🎶 **Vocal Isolation** - Separates vocals from instrumentals for better accuracy  
🔇 **Smart Censorship** - Automatic profanity detection with reverse/mute modes  
⏱️ **3-Second Latency** - Buffered processing for reliable filtering  
🌐 **WebSocket API** - Remote control and monitoring capabilities  

## Getting Started

### Prerequisites

- Windows 10/11 (64-bit)
- Visual Studio 2019/2022 with C++ Desktop Development
- CMake 3.20+
- **Git LFS** (for downloading the Whisper model) - [Install here](https://git-lfs.github.com/)
- **VB-CABLE Virtual Audio Device** (required - see [Setup Guide](#setup-guide))

### Quick Build

```powershell
# Clone the repository (Git LFS will automatically download the model)
git clone https://github.com/AndrewMahran7/ExplicitlyDesktop.git
cd ExplicitlyDesktop

# Initialize JUCE submodules
git submodule update --init --recursive

# Configure with CMake
mkdir build
cd build
cmake ..

# Build
cmake --build . --config Release

# Run
.\bin\Release\ExplicitlyDesktop.exe
```

## Setup Guide

### 1. Install VB-CABLE Virtual Audio Device

Explicitly Desktop requires a virtual audio cable to route audio through the filter.

1. **Download VB-CABLE** from [https://vb-audio.com/Cable/](https://vb-audio.com/Cable/)
2. **Install** the driver (requires admin rights)
3. **Restart** your computer

### 2. Configure Audio Routing

To filter music from Spotify, YouTube, or other applications:

#### Windows Audio Settings:
1. Open **Settings** → **System** → **Sound**
2. Set **Output device** to **CABLE Input (VB-Audio Virtual Cable)**
3. This routes all system audio through the virtual cable

#### Explicitly Desktop Settings:
1. Launch **Explicitly Desktop**
2. Set **Input Device** to **CABLE Output (VB-Audio Virtual Cable)**
3. Set **Output Device** to your **physical speakers/headphones**
4. Choose **Censor Mode** (Reverse or Mute)
5. Click **Start Processing**

#### Audio Flow Diagram:
```
Spotify/YouTube/etc.
        ↓
System Audio Output → VB-CABLE Input
        ↓
VB-CABLE Output → Explicitly Desktop (Input)
        ↓
[Whisper ASR + Profanity Filter]
        ↓
Explicitly Desktop (Output) → Your Speakers/Headphones
        ↓
Filtered Audio (Profanity Removed)
```

### 3. Verify Models

The required files are already included in the repository via Git LFS:

- **Whisper Model**: `Models/ggml-tiny.en.bin` (included via Git LFS)
- **Profanity Lexicon**: `lexicons/profanity_en.txt` (included in repository)

**Note**: If you cloned the repo without Git LFS installed, the model file will be a pointer. Install Git LFS and run:
```powershell
git lfs install
git lfs pull
```

### Usage

1. **Configure audio routing** (see above)
2. **Launch Explicitly Desktop**
3. Select input device: **CABLE Output**
4. Select output device: **Your speakers**
5. Choose censor mode: **Reverse** (reverses profanity) or **Mute** (silences it)
6. Click **Start Processing**
7. Play music - profanity will be automatically filtered with 3-second latency

## Architecture Overview

### System Design

Explicitly Desktop uses a **delayed playback architecture** with vocal isolation for accurate music filtering:

```
┌──────────────────────────────────────────────────────────────┐
│  Audio Input (VB-CABLE Output)                              │
│  ↓ Music from Spotify/YouTube/etc.                          │
└──────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────┐
│  Explicitly Desktop Audio Engine                            │
│                                                              │
│  ┌────────────────────────────────────────────────────┐    │
│  │ Audio Capture Thread                               │    │
│  │ - Captures audio from virtual cable                │    │
│  │ - Stores in 13-second delay buffer                 │    │
│  │ - Accumulates chunks for batch processing          │    │
│  └────────────────────────────────────────────────────┘    │
│                            ↓                                 │
│  ┌────────────────────────────────────────────────────┐    │
│  │ Processing Thread (Every 3 seconds)                │    │
│  │ 1. Vocal Isolation (separate vocals from music)    │    │
│  │ 2. Whisper ASR (transcribe isolated vocals)        │    │
│  │ 3. Profanity Detection (check against lexicon)     │    │
│  │ 4. Generate censor timestamps                      │    │
│  └────────────────────────────────────────────────────┘    │
│                            ↓                                 │
│  ┌────────────────────────────────────────────────────┐    │
│  │ Audio Playback Thread                              │    │
│  │ - Reads from delay buffer (3 seconds behind)       │    │
│  │ - Applies censorship (reverse/mute profanity)      │    │
│  │ - Outputs filtered audio                           │    │
│  └────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────┐
│  Audio Output (Your Speakers/Headphones)                    │
│  ↓ Filtered music (profanity censored)                      │
└──────────────────────────────────────────────────────────────┘
```

### Latency Budget

```
Audio Capture:          10ms    (512 samples @ 48kHz)
   ↓
Delay Buffer:           3000ms  (allows time for processing)
   ↓
Processing Pipeline:
  - Vocal Isolation:    ~500ms
  - Whisper ASR:        ~1500ms (tiny.en model)
  - Profanity Check:    <1ms
   ↓
Censorship DSP:         <1ms    (reverse/mute samples)
   ↓
Audio Output:           10ms    (512 samples @ 48kHz)
──────────────────────────────────
TOTAL LATENCY:          ~3.0s    (configurable)
```

### Key Design Decisions

1. **Delayed Playback (3 seconds)**
   - Provides time for vocal isolation and Whisper processing
   - Ensures reliable, accurate filtering
   - Configurable delay based on processing needs

2. **Vocal Isolation**
   - Separates vocals from instrumentals
   - Improves ASR accuracy on music with heavy background
   - Reduces false positives from instrumental sounds

3. **Whisper ASR (tiny.en model)**
   - High-quality speech recognition
   - Word-level timestamps for precise censorship
   - Offline processing (no cloud dependency)

4. **Batch Processing**
   - Processes audio in 3-second chunks
   - Efficient use of Whisper inference
   - Reduces CPU overhead

5. **Lexicon-Based Detection**
   - Simple string matching against profanity list
   - Multi-token support (e.g., "what the hell")
   - Easily customizable word list

## Technology Stack

### Audio Framework
- **JUCE 7.x**: Cross-platform C++ audio framework
- **Windows Audio**: WASAPI for audio I/O
- **Sample Rate**: 48 kHz
- **Buffer Size**: 512 samples (10.67ms)

### ASR Engine
- **Whisper.cpp**: Optimized C++ implementation of OpenAI Whisper
- **Model**: tiny.en (English-only, ~75MB)
- **Inference Time**: ~1.5 seconds for 3-second audio chunk
- **Word-level timestamps** via DTW alignment

### Vocal Isolation
- Custom vocal filter implementation
- Separates vocals from instrumentals
- Improves ASR accuracy on music

### Virtual Audio Device
- **VB-CABLE**: Virtual audio cable for routing
- Routes system audio through the filter
- Enables filtering of any audio source

## Project Structure

```
desktop/
├── README.md                       # This file
├── CONTRIBUTING.md                 # Contribution guidelines
├── CODE_OF_CONDUCT.md             # Community guidelines
├── LICENSE                         # Proprietary license
├── CMakeLists.txt                  # CMake build configuration
│
├── Source/                         # C++ source code
│   ├── Main.cpp                    # Application entry point
│   ├── MainComponent.h/cpp         # GUI and controls
│   ├── AudioEngine.h/cpp           # Core audio processing
│   ├── WhisperThread.h/cpp         # Whisper ASR integration
│   ├── VocalFilter.h/cpp           # Vocal isolation
│   ├── ProfanityFilter.h           # Lexicon-based detection
│   ├── CensorshipEngine.h          # Reverse/mute DSP
│   ├── CircularBuffer.h            # Audio buffer
│   ├── LockFreeQueue.h             # Thread-safe queue
│   ├── Types.h                     # Data structures
│   └── WebSocketServer.h/cpp       # WebSocket API
│
├── Models/                         # ASR models
│   └── ggml-tiny.en.bin           # Whisper tiny.en model
│
├── lexicons/                       # Profanity word lists
│   └── profanity_en.txt           # English profanity lexicon
│
└── .github/                        # GitHub configuration
    ├── ISSUE_TEMPLATE/            # Issue templates
    └── PULL_REQUEST_TEMPLATE.md   # PR template
```

## Contributing

We welcome contributions! Whether you're fixing bugs, adding features, or improving documentation, your help is appreciated.

- 📖 Read our [Contributing Guide](CONTRIBUTING.md)
- 🐛 [Report a Bug](https://github.com/AndrewMahran7/ExplicitlyDesktop/issues/new?template=bug_report.md)
- 💡 [Request a Feature](https://github.com/AndrewMahran7/ExplicitlyDesktop/issues/new?template=feature_request.md)
- 👥 Check out [Good First Issues](https://github.com/AndrewMahran7/ExplicitlyDesktop/labels/good%20first%20issue)

### Quick Contribution Checklist

1. Fork the repo and create a feature branch
2. Follow our [coding standards](CONTRIBUTING.md#coding-standards)
3. Test your changes thoroughly with VB-CABLE
4. Submit a PR with a clear description
5. Respond to review feedback

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community guidelines.

## License

Proprietary - See [LICENSE](LICENSE) for details.

**TL;DR**: Personal use and contributions are welcome. Redistribution and commercial use are prohibited without permission.

## Troubleshooting

### No Audio Output
- Verify VB-CABLE is installed and working
- Check that input device is set to "CABLE Output"
- Check that output device is set to your physical speakers
- Ensure system audio is routed to "CABLE Input"

### Profanity Not Being Filtered
- Wait for 3-second delay buffer to fill
- Check that Whisper model is loaded (see console output)
- Verify profanity lexicon file exists in `lexicons/profanity_en.txt`
- Try increasing audio input level

### High CPU Usage
- Expected during processing (Whisper inference is CPU-intensive)
- Close other applications
- Consider using a more powerful CPU

### Model Not Found Error
- Ensure `ggml-tiny.en.bin` is in the `Models/` directory
- Check file path in console output
- Re-download model if corrupted

## Future Enhancements

- [ ] Faster Whisper models (distil-whisper)
- [ ] GPU acceleration for Whisper inference
- [ ] Multi-language support
- [ ] Custom profanity lexicon editor
- [ ] Real-time waveform visualization with censor markers
- [ ] Adjustable latency settings
- [ ] Multiple censor modes (bleep, pitch shift, scramble)
- [ ] Statistics dashboard (words censored, accuracy metrics)

## Support

For questions, bug reports, or feature requests, please open an issue on GitHub:
https://github.com/AndrewMahran7/ExplicitlyDesktop/issues

---

**Made with ❤️ by Andrew Mahran (Explicitly Audio Systems)**
#   E x p l i c i t l y D e s k t o p 
 
 