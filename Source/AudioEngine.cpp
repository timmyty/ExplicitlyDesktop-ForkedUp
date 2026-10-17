/*
  ==============================================================================

    AudioEngine.cpp
    Created: 9 Dec 2024
    Author: Explicitly Audio Systems

    PHASE 4: Whisper integration (no threading).

  ==============================================================================
*/

#include "AudioEngine.h"

// Undefine Windows macros that conflict with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <regex>
#include <thread>
#include <fstream>
#include <ctime>

// Helper function to clean Whisper transcript text
static std::string cleanTranscriptText(const std::string& text)
{
    std::string cleaned = text;
    
    // Remove parenthetical content like "( up beat music )", "(laughs)", etc.
    cleaned = std::regex_replace(cleaned, std::regex("\\([^)]*\\)"), "");
    
    // Fix Unicode quote characters: Î“Ã–Â¬ (U+00C3 U+0096 U+00AC) -> '
    cleaned = std::regex_replace(cleaned, std::regex("\xC3\x96\xAC"), "'");
    
    // Fix other common Unicode issues
    cleaned = std::regex_replace(cleaned, std::regex("\xE2\x80\x98"), "'");  // Left single quote
    cleaned = std::regex_replace(cleaned, std::regex("\xE2\x80\x99"), "'");  // Right single quote
    cleaned = std::regex_replace(cleaned, std::regex("\xE2\x80\x9C"), "\""); // Left double quote
    cleaned = std::regex_replace(cleaned, std::regex("\xE2\x80\x9D"), "\""); // Right double quote
    
    // Remove any remaining non-alphanumeric characters (except apostrophes and hyphens for contractions/compound words)
    // Keep: letters, numbers, apostrophes, hyphens, spaces
    std::string filtered;
    for (char c : cleaned)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '\'' || c == '-' || c == ' ')
        {
            filtered += c;
        }
    }
    cleaned = filtered;
    
    // Trim whitespace
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    
    return cleaned;
}

// Add this helper function
std::string mergeCommonSplits(const std::string& text)
{
    std::string merged = text;
    
    // Common profanity splits - merge them
    std::vector<std::pair<std::string, std::string>> replacements = {
        {"nig ga", "nigga"},
        {"nigg a", "nigga"},
        {"N igg", "Nigg"},
        {"b itch", "bitch"},
        {"B itch", "Bitch"},
        {"f uck", "fuck"},
        {"F uck", "Fuck"},
        {"f ucking", "fucking"},
        {"F ucking", "Fucking"},
        {"sh it", "shit"},
        {"Sh it", "Shit"}
    };
    
    for (const auto& [split, whole] : replacements)
    {
        size_t pos = 0;
        while ((pos = merged.find(split, pos)) != std::string::npos)
        {
            merged.replace(pos, split.length(), whole);
            std::cout << "[MERGE] Fixed split word: \"" << split << "\" â†’ \"" << whole << "\"" << std::endl;
            pos += whole.length();
        }
    }
    
    return merged;
}

AudioEngine::AudioEngine()
{
    // Leave a few cores for the audio thread and the rest of the PC
    whisperThreads = std::max(4, std::min(16, (int) std::thread::hardware_concurrency() - 4));
    if (auto* t = std::getenv("EXPLICITLY_WHISPER_THREADS"))
        whisperThreads = std::max(1, std::atoi(t));

    // Phase 4: Load profanity filter
    juce::File lexiconFile("lexicons/profanity_en.txt");
    if (!profanityFilter.loadLexicon(lexiconFile))
    {
        std::cout << "[Phase4] WARNING: Could not load profanity filter" << std::endl;
    }
    else
    {
        std::cout << "[Phase4] Profanity filter loaded" << std::endl;
    }
    
    lyricsGuide = std::make_unique<LyricsGuide>(profanityFilter);
    
    // The CUDA build of whisper/ggml loads its backends (CUDA, CPU variants) from
    // DLLs next to the exe; ask ggml to load them before creating any context
#ifdef _WIN32
    if (auto ggml = LoadLibraryA("ggml.dll"))
        if (auto loadAll = (void (*)()) GetProcAddress(ggml, "ggml_backend_load_all"))
            loadAll();
#endif

    // The model is loaded by start() (or loadWhisperModel); the UI picks which one.
    // EXPLICITLY_WHISPER_MODEL overrides the default for testing
    if (auto* m = std::getenv("EXPLICITLY_WHISPER_MODEL"))
        whisperModelName = m;
}

const std::vector<AudioEngine::ModelChoice>& AudioEngine::availableModels()
{
    // Each 2s slice must be transcribed within the ~1.5s left of the 3.5s delay.
    // Measured here: on the CPU only base.en keeps up; on an RTX 4090 all do
    static const std::vector<ModelChoice> models = {
        { "tiny.en",   "tiny.en - lightest load, least accurate (75 MB)" },
        { "base.en",   "base.en - light load, fine on CPU (148 MB)" },
        { "small.en",  "small.en - medium load, needs a GPU (488 MB)" },
        { "medium.en", "medium.en - heaviest load, most accurate, needs a GPU (1.5 GB)" },
    };
    return models;
}

void AudioEngine::setWhisperModel(const std::string& name)
{
    bool known = false;
    for (auto& m : availableModels())
        known = known || m.name == name;
    if (!known || name == whisperModelName)
        return;

    whisperModelName = name;
    if (!isRunning && whisperCtx != nullptr)
    {
        whisper_free(whisperCtx);   // Loaded on the next start()
        whisperCtx = nullptr;
    }
}

bool AudioEngine::loadWhisperModel()
{
    if (whisperCtx != nullptr)
        return true;

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;              // GPU if the CUDA backend loaded, otherwise CPU
    cparams.flash_attn = false;          // Flash attention turns DTW off
    cparams.dtw_token_timestamps = true; // Per-word timestamps for accurate muting

    const auto& name = whisperModelName;
    if (name == "tiny.en")        cparams.dtw_aheads_preset = WHISPER_AHEADS_TINY_EN;
    else if (name == "base.en")   cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE_EN;
    else if (name == "small.en")  cparams.dtw_aheads_preset = WHISPER_AHEADS_SMALL_EN;
    else                          cparams.dtw_aheads_preset = WHISPER_AHEADS_MEDIUM_EN;

    std::cout << "[Phase5] Loading Whisper model " << name << "..." << std::endl;
    whisperCtx = whisper_init_from_file_with_params(("Models/ggml-" + name + ".bin").c_str(), cparams);
    std::cout << "[Phase5] Whisper model " << name << (whisperCtx ? " loaded" : " FAILED to load") << std::endl;
    return whisperCtx != nullptr;
}

AudioEngine::~AudioEngine()
{
    *uiAlive = false;
    stop();
    if (whisperCtx != nullptr)
        whisper_free(whisperCtx);
}

bool AudioEngine::start(const juce::String& inputDeviceName,
                       const juce::String& outputDeviceName,
                       CensorMode mode)
{
    std::cout << "[Phase6] AudioEngine::start() called" << std::endl;
    
    if (isRunning)
    {
        std::cout << "[Phase6] Already running, stopping first" << std::endl;
        stop();
    }
    
    // Store censor mode
    currentCensorMode = mode;
    std::cout << "[Phase6] Censor mode: " << (mode == CensorMode::Mute ? "MUTE" : "REVERSE") << std::endl;
    
    // Phase 8: Start quality analysis session
    qualityAnalyzer.reset();
    qualityAnalyzer.startSession();
    
    // Setup audio device
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputDeviceName;
    setup.outputDeviceName = outputDeviceName;
    setup.sampleRate = 48000;
    setup.bufferSize = 512;
    setup.inputChannels.setRange(0, 2, true);   // Stereo input
    setup.outputChannels.setRange(0, 2, true);  // Stereo output
    
    std::cout << "[Phase1] Initializing audio device..." << std::endl;
    std::cout << "[Phase1]   Input: " << inputDeviceName << std::endl;
    std::cout << "[Phase1]   Output: " << outputDeviceName << std::endl;
    
    auto error = deviceManager.initialise(2, 2, nullptr, true, juce::String(), &setup);
    
    if (error.isNotEmpty())
    {
        std::cout << "[Phase1] ERROR: " << error << std::endl;
        lastError = "Device initialization failed: " + error;
        return false;
    }
    
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        std::cout << "[Phase1] ERROR: No audio device" << std::endl;
        lastError = "No audio device available after initialization";
        return false;
    }
    
    sampleRate = static_cast<int>(device->getCurrentSampleRate());
    numChannels = device->getActiveInputChannels().countNumberOfSetBits();
    int bufferSize = device->getCurrentBufferSizeSamples();
    int bitDepth = device->getCurrentBitDepth();
    
    std::cout << "[Phase2] ===== AUDIO DEVICE INFO =====" << std::endl;
    std::cout << "[Phase2] Sample rate: " << sampleRate << " Hz" << std::endl;
    std::cout << "[Phase2] Bit depth: " << bitDepth << " bits" << std::endl;
    std::cout << "[Phase2] Channels: " << numChannels << std::endl;
    std::cout << "[Phase2] Buffer size: " << bufferSize << " samples (" 
              << (bufferSize * 1000.0 / sampleRate) << " ms)" << std::endl;
    std::cout << "[Phase2] Input device: " << device->getInputChannelNames().joinIntoString(", ") << std::endl;
    std::cout << "[Phase2] Output device: " << device->getOutputChannelNames().joinIntoString(", ") << std::endl;
    std::cout << "[Phase2] ==============================" << std::endl;
    
    // Phase 5: Load the chosen Whisper model (kept loaded between starts)
    loadWhisperModel();
    if (whisperCtx == nullptr)
    {
        std::cout << "[Phase5] ERROR: Whisper model not loaded - was there an error at startup?" << std::endl;
        lastError = "Whisper model not loaded (check startup logs)";
        deviceManager.closeAudioDevice();
        return false;
    }
    std::cout << "[Phase5] Using pre-loaded Whisper model" << std::endl;
    
    // Allocate buffer for configurable chunk size
    int audioBufferSize = (int)(sampleRate * chunkSeconds);
    audioBuffer.resize(audioBufferSize, 0.0f);
    processingBuffer.resize(audioBufferSize, 0.0f);
    bufferWritePos = 0;
    transcriptionInterval = 0;
    
    // Initialize vocal filter
    vocalFilter.initialize(sampleRate);
    std::cout << "[Phase5] Vocal filter initialized" << std::endl;
    
    // Phase 6: Initialize delay buffer
    // Delay buffer capacity should be larger than initial delay to provide safety margin
    delayBufferSize = (int)(sampleRate * (initialDelaySeconds + 10.0));  // Initial delay + 10s safety margin
    delayBuffer.clear();
    delayBuffer.resize(2);  // Stereo
    for (auto& channel : delayBuffer)
        channel.resize(delayBufferSize, 0.0f);
    
    // Start with NO gap - readPos stays at 0 until we've buffered 10 seconds
    delayReadPos = 0;
    delayWritePos = 0;
    
    std::cout << "[Phase6] Delay buffer initialized: " << delayBufferSize << " samples total (" 
              << (delayBufferSize / sampleRate) << " seconds capacity)" << std::endl;
    std::cout << "[Phase6] Will buffer " << initialDelaySeconds << " seconds before starting playback" << std::endl;
    std::cout << "[Phase6] Initial positions: writePos=" << delayWritePos 
              << ", readPos=" << delayReadPos << " (playback paused until buffered)" << std::endl;
    
    // Debug recording of input/output for timing measurements
    debugRecordDir.clear();
    if (auto* dir = std::getenv("EXPLICITLY_DEBUG_RECORD"))
    {
        debugRecordDir = dir;
        // EXPLICITLY_DEBUG_ROLLING_SECONDS=N keeps the last N seconds instead, saved
        // on request (POST /debug/dump on the control port)
        int seconds = 60;
        debugRolling = false;
        if (auto* rolling = std::getenv("EXPLICITLY_DEBUG_ROLLING_SECONDS"))
        {
            seconds = juce::jlimit(10, 1800, std::atoi(rolling));
            debugRolling = true;
        }
        debugInput.assign((size_t) sampleRate * seconds, 0.0f);
        debugOutput.assign((size_t) sampleRate * seconds, 0.0f);
        debugRecordPos = 0;
        debugRecordFull.store(false);
        debugRecordWritten = false;
        std::cout << "[Debug] Recording " << seconds << "s of input/output"
                  << (debugRolling ? " (rolling)" : "") << " to " << debugRecordDir << std::endl;
    }
    
    // Phase 5: Start background Whisper thread
    shouldStopThread.store(false);
    hasNewBuffer.store(false);
    whisperThread = std::thread(&AudioEngine::whisperThreadFunction, this);
    std::cout << "[Phase5] Background Whisper thread started" << std::endl;
    
    // Idea 2 Phase 1: Try Windows Media Info first (instant, no buffering needed)
    // Note: This is done AFTER audio starts to avoid blocking startup
    std::cout << "[MediaInfo] Attempting Windows Media Control initialization..." << std::endl;
    
    try
    {
        if (windowsMediaInfo.initialize())
        {
            mediaInfoInitialized = true;
            std::cout << "[MediaInfo] Using Windows Media Control for song info" << std::endl;
            
            // Set callback for live updates
            windowsMediaInfo.setMediaChangedCallback([this](const WindowsMediaInfo::MediaInfo& info) {
                std::cout << "[MediaInfo] Media changed: " << info.artist << " - " << info.title << std::endl;
                
                // Update tracked song info
                lastSongTitle = info.title;
                lastSongArtist = info.artist;
                
                if (songInfoCallback && !info.title.empty())
                {
                    juce::MessageManager::callAsync([this, info]() {
                        songInfoCallback(juce::String(info.artist), 
                                       juce::String(info.title), 
                                       1.0f);  // 100% confidence (from system)
                    });
                }
                
                // Lyrics lookup and matching happen in the lyrics guide
                lyricsGuide->setSong(info.artist, info.title);
            });
            
            // Get initial media info immediately
            auto initialInfo = windowsMediaInfo.getCurrentMedia();
            if (!initialInfo.title.empty())
            {
                std::cout << "[MediaInfo] Initial song: " << initialInfo.artist << " - " << initialInfo.title << std::endl;
                
                // Track initial song for change detection
                lastSongTitle = initialInfo.title;
                lastSongArtist = initialInfo.artist;
                
                if (songInfoCallback)
                {
                    songInfoCallback(juce::String(initialInfo.artist), 
                                   juce::String(initialInfo.title), 
                                   1.0f);
                }
                
                lyricsGuide->setSong(initialInfo.artist, initialInfo.title);
            }
            
            // Disable audio fingerprinting since we have Windows Media Control
            songIdentificationAttempted = true;
            songIdentified = true;
        }
        else
        {
            throw std::runtime_error("Windows Media Control initialization returned false");
        }
    }
    catch (const std::exception& e)
    {
        std::cout << "[MediaInfo] Windows Media Control failed: " << e.what() << std::endl;
        std::cout << "[MediaInfo] Falling back to audio fingerprinting" << std::endl;
        
        mediaInfoInitialized = false;
        
        // Fallback: Initialize Chromaprint + AcoustID
        // AcoustID application key shipped with the original project (AcoustID
        // client keys identify an app and are meant to be embedded);
        // EXPLICITLY_ACOUSTID_KEY overrides it. Needs Chromaprint's fpcalc
        // (EXPLICITLY_FPCALC, or fpcalc.exe next to the app)
        const char* fpcalcEnv = std::getenv("EXPLICITLY_FPCALC");
        const char* acoustidEnv = std::getenv("EXPLICITLY_ACOUSTID_KEY");
        std::string fpcalcPath = fpcalcEnv != nullptr ? fpcalcEnv : "fpcalc.exe";
        std::string acoustidKey = acoustidEnv != nullptr ? acoustidEnv : "bNfeKNh59F";
        
        if (songRecognition.initialize(fpcalcPath, acoustidKey))
        {
            std::cout << "[SongRec] Song recognition enabled (Chromaprint + AcoustID)" << std::endl;
            songRecognition.setEnabled(true);
        }
        else
        {
            std::cout << "[SongRec] Song recognition disabled (initialization failed)" << std::endl;
        }
        
        songIdentificationAttempted = false;
        songIdentified = false;
        recognitionBuffer.clear();
        recognitionBuffer.reserve((int)(sampleRate * 10.0));  // Reserve 10s
        std::cout << "[SongRec] Will attempt song identification after 10s of audio" << std::endl;
    }
    
    // Add audio callback
    deviceManager.addAudioCallback(this);
    
    isRunning = true;
    
    std::cout << "[Phase5] Started successfully!" << std::endl;
    return true;
}

void AudioEngine::stop()
{
    if (!isRunning)
        return;
    
    // Write testing log before stopping if testing mode is active
    if (testingMode && !lastSongTitle.empty())
    {
        std::cout << "[Testing] Writing log on stop for: " << lastSongArtist << " - " << lastSongTitle << std::endl;
        writeTestingLog(lastSongArtist, lastSongTitle);
        currentSongPredictions.clear();
    }
    
    deviceManager.removeAudioCallback(this);
    deviceManager.closeAudioDevice();
    
    // Phase 5: Stop background thread
    shouldStopThread.store(true);
    bufferCV.notify_one();  // Wake up thread if waiting
    
    if (whisperThread.joinable())
    {
        std::cout << "[Phase5] Waiting for background thread to finish..." << std::endl;
        whisperThread.join();
        std::cout << "[Phase5] Background thread stopped" << std::endl;
    }
    
    // The Whisper model stays loaded so Start works again after Stop (it is freed
    // in the destructor, or when a different model is picked)
    
    isRunning = false;
    
    // Phase 8: End quality analysis session and print report
    qualityAnalyzer.endSession();
    std::cout << "\n" << qualityAnalyzer.generateReport() << std::endl;
    
    std::cout << "[Phase5] Stopped" << std::endl;
}

bool AudioEngine::setSongInfo(const std::string& artist, const std::string& title)
{
    std::cout << "[Lyrics] Setting song info: " << artist << " - " << title << std::endl;
    
    // Fetch lyrics from API
    SongInfo songInfo = LyricsAlignment::fetchLyrics(artist, title);
    
    if (songInfo.lyrics.empty())
    {
        std::cout << "[Lyrics] Failed to fetch lyrics" << std::endl;
        useLyricsAlignment = false;
        return false;
    }
    
    songLyrics = songInfo.lyrics;
    // useLyricsAlignment = true;  // DISABLED FOR TESTING
    
    std::cout << "[Lyrics] Lyrics loaded successfully (ALIGNMENT DISABLED) (" << songLyrics.length() << " chars)" << std::endl;
    return true;
}

void AudioEngine::setManualLyrics(const std::string& lyrics)
{
    std::cout << "[Lyrics] Setting manual lyrics (" << lyrics.length() << " chars)" << std::endl;
    songLyrics = lyrics;
    useLyricsAlignment = !lyrics.empty();
}

double AudioEngine::getCurrentLatency() const
{
    if (!isRunning)
        return -1.0;
    
    // Latency is constant at initialDelaySeconds (gap between input and output)
    return initialDelaySeconds * 1000.0;
}

double AudioEngine::getCurrentBufferSize() const
{
    if (!isRunning)
        return 0.0;
    
    // Calculate current delay buffer usage (gap between write and read positions)
    // Note: These are not atomic, but reading stale values is acceptable for display purposes
    int writePos = delayWritePos;
    int readPos = delayReadPos;
    int gap = (writePos - readPos + delayBufferSize) % delayBufferSize;
    
    // Convert samples to seconds
    return (double)gap / sampleRate;
}

void AudioEngine::setFilterEnabled(bool enabled)
{
    filterEnabled.store(enabled);
    std::cout << "[Filter] Filter Explicit Speech " << (enabled ? "ON" : "OFF") << std::endl;
}

AudioEngine::FilterStatus AudioEngine::getFilterStatus() const
{
    FilterStatus status;
    status.running = isRunning.load();
    status.enabled = filterEnabled.load();
    status.bypassed = status.enabled && isBypassed();

    if (status.running && status.enabled && !status.bypassed)
    {
        status.buffering = !playbackStarted.load() || outputPaused.load();
        // While buffering, output is silent and will start at the target delay
        status.delaySeconds = status.buffering ? initialDelaySeconds : getCurrentBufferSize();
    }

    return status;
}

bool AudioEngine::dumpDebugRecording()
{
    if (debugRecordDir.empty() || !debugRolling || debugInput.empty())
        return false;

    // Oldest sample first (the ring keeps being written meanwhile; fine for debugging)
    size_t size = debugInput.size();
    size_t pos = debugRecordPos;
    size_t count = std::min(pos, size);
    std::vector<float> in(count), out(count);
    for (size_t k = 0; k < count; ++k)
    {
        size_t at = (pos - count + k) % size;
        in[k] = debugInput[at];
        out[k] = debugOutput[at];
    }
    saveWavFile(debugRecordDir + "/debug_input.wav", in, sampleRate);
    saveWavFile(debugRecordDir + "/debug_output.wav", out, sampleRate);
    std::cout << "[Debug] Dumped last " << (double) count / sampleRate << "s of input/output" << std::endl;
    return true;
}

bool AudioEngine::isBufferUnderrun() const
{
    return bufferUnderrun.load();
}

void AudioEngine::setTestingMode(bool enabled)
{
    testingMode = enabled;
    std::cout << "[Testing] Testing mode " << (enabled ? "ENABLED" : "DISABLED") << std::endl;
    if (enabled)
    {
        std::cout << "[Testing] Will create log files for each song with profanity predictions" << std::endl;
    }
}

float AudioEngine::getCurrentInputLevel() const
{
    return currentInputLevel.load();
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    std::cout << "[Phase6] Audio device about to start: " << device->getName() << std::endl;
    bufferWritePos = 0;
    transcriptionInterval = 0;
    streamTime = 0.0;
    playbackStarted = false;  // Track when we start playing
    outputPaused = false;
    wasWaiting = false;
    debugCounter = 0;
    
    // Clear delay buffer (pre-filled with silence)
    for (auto& channel : delayBuffer)
        std::fill(channel.begin(), channel.end(), 0.0f);
    
    // Start with both positions at 0
    // Playback won't start until 10 seconds buffered
    delayReadPos = 0;
    delayWritePos = 0;
    
    std::cout << "[Phase6] Buffering " << initialDelaySeconds << " seconds before playback starts..." << std::endl;
}

void AudioEngine::audioDeviceStopped()
{
    std::cout << "[Phase5] Audio device stopped" << std::endl;
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                  int numInputChannels,
                                                  float* const* outputChannelData,
                                                  int numOutputChannels,
                                                  int numSamples,
                                                  const juce::AudioIODeviceCallbackContext& context)
{
    static std::atomic<int> callbackCount{0};
    int currentCount = callbackCount.fetch_add(1);
    
    if (currentCount == 0)
    {
        std::cout << "[Phase5] *** FIRST AUDIO CALLBACK *** " << numSamples << " samples" << std::endl;
    }
    
    // Calculate RMS level from first input channel
    float rmsSum = 0.0f;
    if (numInputChannels > 0 && inputChannelData[0] != nullptr)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float sample = inputChannelData[0][i];
            rmsSum += sample * sample;
        }
        float rms = std::sqrt(rmsSum / numSamples);
        currentInputLevel.store(rms);
    }
    
    // Phase 5: Accumulate audio into buffer (mono downmix)
    if (numInputChannels > 0 && inputChannelData[0] != nullptr)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            // Downmix stereo to mono
            float monoSample = inputChannelData[0][i];
            if (numInputChannels > 1 && inputChannelData[1] != nullptr)
                monoSample = (monoSample + inputChannelData[1][i]) * 0.5f;
            
            // Add to buffer
            if (bufferWritePos < audioBuffer.size())
            {
                audioBuffer[bufferWritePos] = monoSample;
                bufferWritePos++;
            }
            
            // Idea 2 Phase 1: Accumulate audio for song recognition (first 10 seconds)
            if (!songIdentificationAttempted && recognitionBuffer.size() < (size_t)(sampleRate * 10.0))
            {
                recognitionBuffer.push_back(monoSample);
            }
        }
    }
    
    // Idea 2 Phase 1: Attempt song identification after accumulating 10 seconds
    if (!songIdentificationAttempted && recognitionBuffer.size() >= (size_t)(sampleRate * 10.0))
    {
        songIdentificationAttempted = true;
        
        std::cout << "[SongRec] Attempting song identification with " 
                  << (recognitionBuffer.size() / sampleRate) << " seconds of audio..." << std::endl;
        
        // Try to identify the song
        currentSong = songRecognition.identifySong(recognitionBuffer.data(), 
                                                    (int)recognitionBuffer.size(), 
                                                    sampleRate);
        
        if (currentSong.identified)
        {
            songIdentified = true;
            std::cout << "[SongRec] *** SONG IDENTIFIED ***" << std::endl;
            std::cout << "[SongRec] Artist: " << currentSong.artist << std::endl;
            std::cout << "[SongRec] Title: " << currentSong.title << std::endl;
            std::cout << "[SongRec] Album: " << currentSong.album << std::endl;
            std::cout << "[SongRec] Confidence: " << (currentSong.confidence * 100.0f) << "%" << std::endl;
            
            // Notify UI of song identification
            if (songInfoCallback)
            {
                songInfoCallback(juce::String(currentSong.artist), 
                                juce::String(currentSong.title), 
                                currentSong.confidence);
            }
            
            // Fetch lyrics if not already included
            if (currentSong.lyrics.empty())
            {
                std::cout << "[SongRec] Fetching lyrics..." << std::endl;
                currentSong.lyrics = songRecognition.fetchLyrics(currentSong.artist, currentSong.title);
                
                if (!currentSong.lyrics.empty())
                {
                    std::cout << "[SongRec] Lyrics fetched successfully (" 
                              << currentSong.lyrics.length() << " chars)" << std::endl;
                    // TODO Phase 1b: Align lyrics with Whisper word timestamps
                }
                else
                {
                    std::cout << "[SongRec] WARNING: Could not fetch lyrics" << std::endl;
                }
            }
        }
        else
        {
            std::cout << "[SongRec] Song not identified - will continue with Whisper-only mode" << std::endl;
            
            // Notify UI that song could not be identified
            if (songInfoCallback)
            {
                songInfoCallback(juce::String("Unknown"), 
                                juce::String("Song not recognized"), 
                                0.0f);
            }
        }
        
        // Free recognition buffer memory (no longer needed)
        recognitionBuffer.clear();
        recognitionBuffer.shrink_to_fit();
    }
    
    // Check if we've accumulated chunkSeconds of audio AND Whisper is ready
    // Send chunks with configurable overlap between them
    transcriptionInterval += numSamples;
    
    // Filter off: don't spend GPU time transcribing audio that plays uncensored
    if (!filterActive())
    {
        bufferWritePos = 0;
        transcriptionInterval = 0;
    }
    // Send to Whisper if: (1) we have chunkSeconds of audio, AND (2) Whisper is ready for more
    else if (transcriptionInterval >= (sampleRate * chunkSeconds) && !hasNewBuffer.load())
    {
        // Phase 5: Signal background thread (it's ready for next chunk)
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            
            // Copy buffer for background processing (chunkSeconds of audio)
            int samplesToProcess = std::min(bufferWritePos, (int)(sampleRate * chunkSeconds));
            std::copy(audioBuffer.begin(), audioBuffer.begin() + samplesToProcess, 
                     processingBuffer.begin());
            
            // Store the CURRENT writePos when we send this chunk
            // This marks where the END of the chunk is in the delay buffer
            // The chunk starts chunkSeconds before this position
            bufferCaptureTime = (double)delayWritePos;  // Store END position of chunk
            bufferCaptureSamples = samplesCaptured.load();
            
            int chunkStartPos = (delayWritePos - (int)(sampleRate * chunkSeconds) + delayBufferSize) % delayBufferSize;
            std::cout << "[CAPTURE] Sending chunk to Whisper | chunkStart=" << chunkStartPos 
                      << ", chunkEnd(writePos)=" << delayWritePos << ", readPos=" << delayReadPos << std::endl;
            
            if (wasWaiting)
            {
                std::cout << "[FLOW] Whisper finished! Sending next chunk immediately (buffer growing)" << std::endl;
                wasWaiting = false;
            }
            
            hasNewBuffer.store(true);
            bufferCV.notify_one();
        }
        
        // Reset buffer for next chunk (no overlap)
        bufferWritePos = 0;
        transcriptionInterval = 0;
    }
    else if (transcriptionInterval >= (sampleRate * chunkSeconds) && hasNewBuffer.load())
    {
        // We have chunkSeconds of audio but Whisper is still busy - buffer is growing!
        if (++debugCounter % 100 == 0)  // Log every ~1 second
        {
            double extraTime = (transcriptionInterval - (sampleRate * chunkSeconds)) / sampleRate;
            std::cout << "[FLOW] Waiting for Whisper to finish... (accumulated " 
                      << std::fixed << std::setprecision(2) << extraTime << "s extra audio)" << std::endl;
            wasWaiting = true;
        }
    }
    
    // Phase 6: Monitor buffer health (only after playback has started)
    double currentBufferSize = getCurrentBufferSize();
    
    // Only check for underrun AFTER we've started playing
    if (playbackStarted.load())
    {
        // Critical threshold: buffer below minimum safe level (chunkSeconds + 0.5s margin)
        double minBufferSize = chunkSeconds + 0.5;
        double recoveryBufferSize = initialDelaySeconds;  // Recover to initial delay level
        
        if (currentBufferSize < minBufferSize && !bufferUnderrun.load())
        {
            bufferUnderrun.store(true);
            std::cout << "\n[BUFFER UNDERRUN] Buffer dropped to " << currentBufferSize 
                      << "s (min: " << minBufferSize << "s) - DISABLING CENSORSHIP to prevent glitches!\n" << std::endl;
            lastUnderrunWarningTime = streamTime;
            
            // Phase 8: Record underrun event
            qualityAnalyzer.recordBufferUnderrun();
        }
        // Recovery threshold: buffer recovered to initial delay level
        else if (currentBufferSize > recoveryBufferSize && bufferUnderrun.load())
        {
            bufferUnderrun.store(false);
            std::cout << "\n[BUFFER RECOVERED] Buffer restored to " << currentBufferSize 
                      << "s - Re-enabling censorship\n" << std::endl;
        }
    }
    // Periodic warning if still in underrun
    else if (bufferUnderrun.load() && (streamTime - lastUnderrunWarningTime) > 5.0)
    {
        std::cout << "[WARNING] Buffer still low: " << currentBufferSize << "s" << std::endl;
        lastUnderrunWarningTime = streamTime;
    }
    
    // Filter Explicit Speech toggle: when off, pass input straight through with no
    // delay. The read position tracks the write position so that turning the filter
    // back on starts a fresh initialDelaySeconds fill (heard as a short silence).
    if (!filterActive())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < std::min(2, numInputChannels); ++ch)
                if (inputChannelData[ch] != nullptr)
                    delayBuffer[ch][delayWritePos] = inputChannelData[ch][i];

            for (int ch = 0; ch < numOutputChannels; ++ch)
            {
                if (outputChannelData[ch] == nullptr)
                    continue;
                int inCh = std::min(ch, numInputChannels - 1);
                outputChannelData[ch][i] = (inCh >= 0 && inputChannelData[inCh] != nullptr)
                                               ? inputChannelData[inCh][i] : 0.0f;
            }

            delayWritePos = (delayWritePos + 1) % delayBufferSize;
        }

        delayReadPos = delayWritePos;
        samplesCaptured += numSamples;
        playbackStarted.store(false);
        outputPaused.store(false);
        bufferUnderrun.store(false);
        streamTime += (double)numSamples / sampleRate;
        return;
    }

    // Phase 6: Write input to delay buffer, read from 10 seconds ago
    for (int i = 0; i < numSamples; ++i)
    {
        // Write current input to delay buffer
        for (int ch = 0; ch < std::min(2, numInputChannels); ++ch)
        {
            if (inputChannelData[ch] != nullptr)
            {
                delayBuffer[ch][delayWritePos] = inputChannelData[ch][i];
            }
        }
        
        // Dynamic buffer management: pause playback if buffer too low, resume when filled
        int currentGap = (delayWritePos - delayReadPos + delayBufferSize) % delayBufferSize;
        double bufferSeconds = (double)currentGap / sampleRate;
        
        // Start playback when buffer reaches 10 seconds
        // Pause playback if buffer drops below 8 seconds (RTF > 1.0 catching up)
        // Resume playback when buffer recovers to 10 seconds
        bool canPlay;
        if (!playbackStarted.load())
        {
            // Initial buffering: need initialDelaySeconds to start
            canPlay = (bufferSeconds >= initialDelaySeconds);
            if (canPlay)
            {
                playbackStarted.store(true);
                std::cout << "\n[Phase6] âœ“ " << initialDelaySeconds << " SECONDS BUFFERED - PLAYBACK STARTING NOW!" << std::endl;
                std::cout << "[Phase6] Censored audio will now be audible\n" << std::endl;
            }
        }
        else
        {
            // Dynamic buffering: pause if too low, resume when recovered
            bool wasPaused = outputPaused.load();

            double pauseThreshold = initialDelaySeconds - 2.0;  // Pause at initialDelaySeconds - 2s
            double resumeThreshold = initialDelaySeconds;       // Resume at initialDelaySeconds
            
            if (bufferSeconds < pauseThreshold && !wasPaused)
            {
                wasPaused = true;
                std::cout << "\n[Phase6] âš  Buffer dropped to " << std::fixed << std::setprecision(2) 
                         << bufferSeconds << "s - PAUSING playback to rebuild buffer\n" << std::endl;
            }
            else if (bufferSeconds >= resumeThreshold && wasPaused)
            {
                wasPaused = false;
                std::cout << "\n[Phase6] âœ“ Buffer recovered to " << std::fixed << std::setprecision(2) 
                         << bufferSeconds << "s - RESUMING playback\n" << std::endl;
            }
            
            outputPaused.store(wasPaused);
            canPlay = !wasPaused;
        }
        
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] != nullptr)
            {
                if (canPlay)
                {
                    int readCh = std::min(ch, 1);  // Stereo only
                    outputChannelData[ch][i] = delayBuffer[readCh][delayReadPos];
                }
                else
                {
                    // Output silence while buffering
                    outputChannelData[ch][i] = 0.0f;
                }
            }
        }
        
        if (!debugRecordDir.empty() && (debugRolling || debugRecordPos < debugInput.size()))
        {
            size_t at = debugRecordPos % debugInput.size();
            debugInput[at] = inputChannelData[0] != nullptr ? inputChannelData[0][i] : 0.0f;
            debugOutput[at] = outputChannelData[0] != nullptr ? outputChannelData[0][i] : 0.0f;
            if (++debugRecordPos == debugInput.size() && !debugRolling)
                debugRecordFull.store(true);
        }
        
        // Always advance write position
        delayWritePos = (delayWritePos + 1) % delayBufferSize;
        
        // Only advance read position if we're playing
        if (canPlay)
        {
            delayReadPos = (delayReadPos + 1) % delayBufferSize;
        }
    }
    
    // Update stream time (tracks the DELAYED output time)
    streamTime += (double)numSamples / sampleRate;
    samplesCaptured += numSamples;
    
    if (currentCount == 0)
        std::cout << "[Phase6] Audio passthrough + censorship active" << std::endl;
}

void AudioEngine::whisperThreadFunction()
{
    std::cout << "[Phase5] Whisper background thread running" << std::endl;
    
    // Local buffer for processing (avoids race condition)
    std::vector<float> localBuffer(sampleRate * chunkSeconds);
    
    while (!shouldStopThread.load())
    {
        std::unique_lock<std::mutex> lock(bufferMutex);
        
        // Wait for new buffer or stop signal
        bufferCV.wait(lock, [this] { 
            return hasNewBuffer.load() || shouldStopThread.load(); 
        });
        
        if (shouldStopThread.load())
            break;
        
        if (hasNewBuffer.load())
        {
            std::cout << "[Phase5] Processing " << chunkSeconds << "-second buffer in background..." << std::endl;
            
            // Copy to local buffer BEFORE releasing lock
            std::copy(processingBuffer.begin(), processingBuffer.end(), localBuffer.begin());
            double captureTime = bufferCaptureTime;  // Copy timestamp
            captureStreamSeconds = (double) bufferCaptureSamples / sampleRate;
            hasNewBuffer.store(false);
            
            // NOW release the lock - audio callback can write new data safely
            lock.unlock();
            
            // Process the LOCAL buffer with its capture timestamp
            processTranscription(localBuffer, captureTime);
        }
    }
    
    std::cout << "[Phase5] Whisper background thread exiting" << std::endl;
}

// Write testing log file for current song
void AudioEngine::writeTestingLog(const std::string& artist, const std::string& title)
{
    if (!testingMode)
        return;
    
    // Get current timestamp for filename
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_s(&tm_now, &time_t_now);
    
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &tm_now);
    
    // Create safe filename (remove invalid characters)
    std::string safeArtist = artist;
    std::string safeTitle = title;
    
    // Handle empty artist/title
    if (safeArtist.empty()) safeArtist = "Unknown_Artist";
    if (safeTitle.empty()) safeTitle = "Unknown_Title";
    
    auto sanitize = [](std::string& str) {
        for (char& c : str)
        {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || 
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            {
                c = '_';
            }
        }
    };
    
    sanitize(safeArtist);
    sanitize(safeTitle);
    
    // Create TestLogs directory if it doesn't exist
    juce::File logsDir = juce::File::getCurrentWorkingDirectory().getChildFile("TestLogs");
    if (!logsDir.exists())
    {
        logsDir.createDirectory();
        std::cout << "[Testing] Created TestLogs directory: " << logsDir.getFullPathName() << std::endl;
    }
    
    std::string filename = logsDir.getChildFile(safeArtist + " - " + safeTitle + " - " + timestamp + ".txt").getFullPathName().toStdString();
    
    // Write log file
    std::ofstream logFile(filename);
    if (!logFile.is_open())
    {
        std::cout << "[Testing] ERROR: Failed to create log file: " << filename << std::endl;
        return;
    }
    
    logFile << "=================================================\n";
    logFile << "Explicitly Desktop - Profanity Detection Log\n";
    logFile << "=================================================\n";
    logFile << "Artist: " << artist << "\n";
    logFile << "Title: " << title << "\n";
    logFile << "Date: " << timestamp << "\n";
    logFile << "Total Predictions: " << currentSongPredictions.size() << "\n";
    logFile << "=================================================\n\n";
    
    for (size_t i = 0; i < currentSongPredictions.size(); ++i)
    {
        const auto& pred = currentSongPredictions[i];
        logFile << "[" << (i + 1) << "] ";
        logFile << "\"" << pred.word << "\" ";
        logFile << "at " << std::fixed << std::setprecision(2) << pred.timestamp << "s ";
        logFile << "(" << pred.censorMode << ")";
        if (pred.isMultiWord)
            logFile << " [MULTI-WORD]";
        logFile << "\n";
    }
    
    logFile << "\n=================================================\n";
    logFile << "End of Log\n";
    logFile << "=================================================\n";
    
    logFile.close();
    
    std::cout << "[Testing] âœ“ Log file created: " << filename << std::endl;
    std::cout << "[Testing]   Predictions logged: " << currentSongPredictions.size() << std::endl;
    std::cout << "[Testing]   Full path: " << filename << std::endl;
}

std::vector<float> AudioEngine::resampleTo16kHz(const std::vector<float>& input)
{
    if (sampleRate == 16000)
        return input;
    
    // Simple linear interpolation resample
    double ratio = (double)sampleRate / 16000.0;
    size_t outputSize = (size_t)(input.size() / ratio);
    std::vector<float> output(outputSize);
    
    for (size_t i = 0; i < outputSize; ++i)
    {
        double srcPos = i * ratio;
        size_t srcIndex = (size_t)srcPos;
        double frac = srcPos - srcIndex;
        
        if (srcIndex + 1 < input.size())
            output[i] = input[srcIndex] * (1.0f - frac) + input[srcIndex + 1] * frac;
        else
            output[i] = input[srcIndex];
    }
    
    return output;
}

void AudioEngine::processTranscription(const std::vector<float>& buffer, double captureTime)
{
    if (!whisperCtx)
        return;
    
    if (debugRecordFull.load() && !debugRecordWritten)
    {
        debugRecordWritten = true;
        saveWavFile(debugRecordDir + "/debug_input.wav", debugInput, sampleRate);
        saveWavFile(debugRecordDir + "/debug_output.wav", debugOutput, sampleRate);
        std::cout << "[Debug] Wrote debug_input.wav and debug_output.wav" << std::endl;
    }
    
    try
    {
        // Calculate delay duration for timestamp calculation
        double delaySeconds = (double)delayBufferSize / sampleRate;
        
        // Start timing
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Log current buffer size with detailed analysis
        double currentBufferSize = getCurrentBufferSize();
        int rawGap = delayWritePos - delayReadPos;
        int actualGap = (delayWritePos - delayReadPos + delayBufferSize) % delayBufferSize;
        
        std::cout << "[BUFFER] Size: " << std::fixed << std::setprecision(2) << currentBufferSize << "s";
        std::cout << " | writePos=" << delayWritePos << ", readPos=" << delayReadPos;
        std::cout << " | raw gap=" << rawGap << ", actual gap=" << actualGap << " samples";
        std::cout << " | bufSize=" << delayBufferSize << std::endl;
        
        // Phase 8: Record buffer health
        qualityAnalyzer.recordBufferSize(currentBufferSize);
        
        // Phase 5: Process the buffer passed as parameter (already a local copy in thread)
        // Transcribe the new chunk plus overlapSeconds of the audio before it, read
        // straight from the delay buffer, so words cut by a chunk boundary are
        // heard whole in the next window
        const double windowSeconds = chunkSeconds + overlapSeconds;
        
        // DTW marks each token near where it finishes, so a word's first-token time
        // is late for its start, and its last-token time is just before its end.
        // Measured on medium.en with a test clip of known word positions
        // (EXPLICITLY_DEBUG_RECORD): starts +0.24..+0.33s late; ending words at the
        // next word's start instead clipped the following word in songs.
        const double startBias = -0.25;   // Lets the first consonant through
        const double dtwEndOffset = 0.13;   // Word end after its last token's DTW time
        const int windowSamples = (int)(sampleRate * windowSeconds);
        const int windowStartPos = ((int)captureTime - windowSamples + delayBufferSize) % delayBufferSize;
        std::vector<float> bufferCopy((size_t) windowSamples);
        for (int i = 0; i < windowSamples; ++i)
        {
            int pos = (windowStartPos + i) % delayBufferSize;
            bufferCopy[(size_t) i] = 0.5f * (delayBuffer[0][pos] + delayBuffer[1][pos]);
        }
        juce::ignoreUnused(buffer);
        
        // DISABLED: Vocal filtering may be degrading audio quality for Whisper
        // vocalFilter.processBuffer(bufferCopy);
        
        audioBuffer16k = resampleTo16kHz(bufferCopy);
        
        // DEBUG: Save first 10 chunks to WAV for quality inspection
        static int chunkCounter = 0;
        if (chunkCounter < 10)
        {
            // Create DebugAudio directory if it doesn't exist
            juce::File debugDir = juce::File::getCurrentWorkingDirectory().getChildFile("DebugAudio");
            if (!debugDir.exists())
            {
                debugDir.createDirectory();
                std::cout << "[DEBUG] Created DebugAudio directory: " << debugDir.getFullPathName() << std::endl;
            }
            
            std::string filename = debugDir.getChildFile("debug_chunk_" + juce::String(chunkCounter++) + ".wav").getFullPathName().toStdString();
            saveWavFile(filename, audioBuffer16k, 16000);
            std::cout << "[DEBUG] Saved " << filename << " for inspection" << std::endl;
        }
        
        std::cout << "[Phase5] Resampled " << windowSamples << " samples to " 
                  << audioBuffer16k.size() << " samples @ 16kHz" << std::endl;
        
        // Configure Whisper parameters - OPTIMIZED FOR SPEED
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_realtime = false;
        wparams.print_progress = false;
        wparams.print_timestamps = true;  // Enable for word-level timing
        wparams.print_special = false;
        wparams.translate = false;
        wparams.language = "en";
        wparams.n_threads = whisperThreads;
        wparams.single_segment = false;
        wparams.token_timestamps = true;   // Second timing estimate besides DTW
        wparams.max_len = 0;
        
        // Speed optimizations (valid parameters only)
        wparams.audio_ctx = 1500;  // Enable audio context for music disambiguation
        wparams.temperature = 0.0f;  // Start with greedy decoding
        wparams.temperature_inc = 0.2f;  // Try multiple decoding strategies (0, 0.2, 0.4, 0.6, 0.8, 1.0)
        wparams.entropy_thold = 5.0f;  // Don't skip uncertain segments (music has high entropy)
        wparams.logprob_thold = -1.0f;  // Accept lower probability tokens (faster)
        
        // Over loud music Whisper often answers "(upbeat music)". Suppress those
        // non-speech tokens, and when the song's lyrics are known, prompt with the
        // lines around where we are so it hears the sung words
        wparams.suppress_nst = true;
        std::string lyricsPrompt = lyricsGuide->getPrompt();
        if (!lyricsPrompt.empty())
            wparams.initial_prompt = lyricsPrompt.c_str();
        
        // Run transcription
        int result = whisper_full(whisperCtx, wparams, audioBuffer16k.data(), (int)audioBuffer16k.size());
        
        if (result != 0)
        {
            std::cout << "[Phase5] Whisper transcription failed with code " << result << std::endl;
            return;
        }
        
        // CRITICAL: Reset Whisper state to prevent memory accumulation
        // Without this, KV cache grows indefinitely and RTF increases over time
        whisper_reset_timings(whisperCtx);
        
        // Extract words with per-token DTW timestamps (enabled on the context via
        // dtw_token_timestamps). Tokens are sub-words, so tokens are grouped into
        // words at leading spaces. A word runs from its first token's DTW time to
        // the next word's first token. Falls back to spreading words evenly over
        // the segment if DTW times are unavailable.
        int numSegments = whisper_full_n_segments(whisperCtx);
        std::vector<WordSegment> transcribedWords;
        bool allDtw = true;
        
        struct PendingWord { std::string text; double start; double last; double ts0; double ts1; };
        std::vector<std::pair<double, double>> tokenTimes;   // Per word: token-timestamp start/end
        
        for (int i = 0; i < numSegments; ++i)
        {
            double segStartSec = whisper_full_get_segment_t0(whisperCtx, i) * 0.01;
            double segEndSec = whisper_full_get_segment_t1(whisperCtx, i) * 0.01;
            
            std::vector<PendingWord> words;
            bool haveDtw = true;
            
            int numTokens = whisper_full_n_tokens(whisperCtx, i);
            for (int j = 0; j < numTokens; ++j)
            {
                whisper_token_data token = whisper_full_get_token_data(whisperCtx, i, j);
                if (token.id >= whisper_token_eot(whisperCtx))
                    continue;  // Special tokens
                
                std::string raw = whisper_full_get_token_text(whisperCtx, i, j);
                bool startsWord = words.empty() || (! raw.empty() && raw[0] == ' ');
                std::string piece = cleanTranscriptText(raw);
                if (piece.empty())
                    continue;
                
                if (token.t_dtw < 0)
                    haveDtw = false;
                
                if (startsWord)
                    words.push_back({ piece, token.t_dtw * 0.01, token.t_dtw * 0.01, token.t0 * 0.01, token.t1 * 0.01 });
                else
                {
                    words.back().text += piece;
                    words.back().last = token.t_dtw * 0.01;
                    words.back().ts1 = token.t1 * 0.01;
                }
            }
            
            allDtw = allDtw && haveDtw;
            for (size_t k = 0; k < words.size(); ++k)
            {
                double wordStart, wordEnd;
                if (haveDtw)
                {
                    // DTW marks each token near where it finishes: the word ends just
                    // after its last token's time (never past the next word's start)
                    wordStart = words[k].start;
                    wordEnd = words[k].last + dtwEndOffset;
                    if (k + 1 < words.size())
                        wordEnd = std::min(wordEnd, words[k + 1].start);
                }
                else
                {
                    double wordDuration = (segEndSec - segStartSec) / words.size();
                    wordStart = segStartSec + k * wordDuration;
                    wordEnd = wordStart + wordDuration;
                }
                
                wordStart = std::max(0.0, std::min(windowSeconds, wordStart));
                wordEnd = std::max(wordStart + 0.05, std::min(windowSeconds, wordEnd));
                transcribedWords.emplace_back(words[k].text, wordStart, wordEnd, 0.9f);
                tokenTimes.push_back({ words[k].ts0, words[k].ts1 });
            }
        }
        
        std::cout << "[Phase5] Extracted " << transcribedWords.size() << " words ("
                  << (allDtw ? "DTW timestamps" : "spread over segment") << ")" << std::endl;
        
        // Apply lyrics alignment if enabled (sliding window approach)
        std::vector<WordSegment> finalWords = transcribedWords;
        
        // Follow what's playing (Spotify, browsers) every few seconds; the lyrics
        // guide ignores repeats and looks up lyrics for new songs in the background
        static auto lastMediaPoll = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        auto now = std::chrono::steady_clock::now();
        // EXPLICITLY_DEBUG_SONG="Artist|Title" pins the song (for testing with clips
        // that don't show up in Windows' now-playing info)
        static const char* debugSong = std::getenv("EXPLICITLY_DEBUG_SONG");
        if (debugSong != nullptr)
        {
            juce::String pinned(debugSong);
            lyricsGuide->setSong(pinned.upToFirstOccurrenceOf("|", false, false).toStdString(),
                                 pinned.fromFirstOccurrenceOf("|", false, false).toStdString());
        }
        else if (mediaInfoInitialized && now - lastMediaPoll >= std::chrono::seconds(3))
        {
            lastMediaPoll = now;
            auto media = windowsMediaInfo.getCurrentMedia();
            lyricsGuide->setSong(media.artist, media.title);
        }
        
        if (useLyricsAlignment && !songLyrics.empty())
        {
            finalWords = lyricsAlignment.alignChunk(transcribedWords, songElapsedTime);
            
            // Only increment time if we actually had transcribed words (audio was playing)
            if (!transcribedWords.empty())
            {
                songElapsedTime += chunkSeconds;
            }
            
            // BUGFIX: If alignment returns empty (no match), fall back to raw Whisper
            if (finalWords.empty() && !transcribedWords.empty())
            {
                std::cout << "[Phase5] âš  Alignment returned empty - falling back to raw Whisper" << std::endl;
                finalWords = transcribedWords;
            }
            
            // NEW: If Whisper heard NOTHING but we have lyrics loaded, predict next words
            if (finalWords.empty() && transcribedWords.empty() && lyricsAlignment.isReady())
            {
                std::cout << "[Phase5] Whisper heard nothing - PREDICTING next lyrics words" << std::endl;
                finalWords = lyricsAlignment.predictNextWords(chunkSeconds);
                
                if (!finalWords.empty())
                {
                    std::cout << "[Phase5] Predicted " << finalWords.size() << " words from lyrics position " 
                             << lyricsAlignment.getCurrentPosition() << std::endl;
                }
            }
        }
        
        // If we STILL have no words (no lyrics loaded or prediction failed), skip censorship
        if (finalWords.empty())
        {
            std::cout << "[Phase5] No words to censor - skipping" << std::endl;
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            double seconds = duration.count() / 1000.0;
            double realTimeFactor = seconds / chunkSeconds;
            
            std::cout << "[TIMING] Processed " << chunkSeconds << "s audio in " << seconds 
                      << "s (RTF: " << std::fixed << std::setprecision(2) << realTimeFactor << "x)" << std::endl;
            
            qualityAnalyzer.recordRTF(realTimeFactor);
            qualityAnalyzer.updateSessionDuration(streamTime);
            return;
        }
        
        // Print transcript and check profanity (including multi-word patterns)
        std::cout << "[Phase5] ========== TRANSCRIPT (" << finalWords.size() << " words) ==========" << std::endl;
        
        std::string fullTranscript;
        std::vector<std::string> detectedWords;
        
        // Check Whisper's words against the song's lyrics, when we have them
        LyricsGuide::Decision guide;
        if (!useLyricsAlignment)
            guide = lyricsGuide->analyze(finalWords, windowSeconds, startBias,
                                         captureStreamSeconds - windowSeconds);
        
        struct Hit { std::string text; double start; double end; bool multi; bool finalTimes = false; };
        std::vector<Hit> hits;
        
        // Check for single-word AND multi-word profanity
        std::vector<bool> wordAlreadyCensored(finalWords.size(), false);  // Track which words are already handled
        
        for (size_t idx = 0; idx < finalWords.size(); ++idx)
        {
            // Skip if this word was already part of a multi-word profanity
            if (wordAlreadyCensored[idx])
                continue;
            
            const auto& wordSeg = finalWords[idx];
            fullTranscript += wordSeg.word + " ";
            
            bool foundProfanity = false;
            std::string profanityText;
            double profanityStart, profanityEnd;
            bool isMultiWord = false;
            
            // FIRST: Check multi-word profanity patterns (prioritize longer matches)
            if (idx + 1 < finalWords.size())
            {
                const auto& nextWord = finalWords[idx + 1];
                std::string combined = LyricsAlignment::normalizeText(wordSeg.word + nextWord.word);
                
                if (profanityFilter.isProfane(combined))
                {
                    foundProfanity = true;
                    profanityText = wordSeg.word + " " + nextWord.word;
                    profanityStart = wordSeg.start;
                    profanityEnd = nextWord.end;
                    isMultiWord = true;
                    
                    // Mark both words as handled
                    wordAlreadyCensored[idx] = true;
                    wordAlreadyCensored[idx + 1] = true;
                }
            }
            
            // SECOND: If no multi-word match, check single word profanity
            if (!foundProfanity)
            {
                std::string normalizedWord = LyricsAlignment::normalizeText(wordSeg.word);
                
                if (profanityFilter.isProfane(normalizedWord))
                {
                    foundProfanity = true;
                    profanityText = wordSeg.word;
                    profanityStart = wordSeg.start;
                    profanityEnd = wordSeg.end;
                    isMultiWord = false;
                    
                    wordAlreadyCensored[idx] = true;
                }
            }
            
            // Lyrics say a clean word is sung here: Whisper misheard, don't mute
            if (foundProfanity && guide.aligned && guide.lyricsSayClean[idx]
                && (!isMultiWord || guide.lyricsSayClean[idx + 1]))
            {
                std::cout << "[LyricsGuide] Not muting \"" << profanityText
                          << "\": lyrics have a clean word here" << std::endl;
                continue;
            }
            
            if (foundProfanity)
            {
                if (idx < tokenTimes.size())
                    std::cout << "[Timing] \"" << profanityText << "\" dtw " << profanityStart << "-" << profanityEnd
                              << " token-ts " << tokenTimes[idx].first << "-"
                              << tokenTimes[std::min(tokenTimes.size() - 1, idx + (isMultiWord ? 1 : 0))].second
                              << " window-start-abs "
                              << (double)((long long) debugRecordPos - (long long)((delayWritePos - windowStartPos + delayBufferSize) % delayBufferSize)) / sampleRate
                              << std::endl;
                hits.push_back({ profanityText, profanityStart, profanityEnd, isMultiWord });
            }
        }
        
        // Profane lyrics words that Whisper misheard or missed
        for (size_t k = 0; k < guide.extraMutes.size(); ++k)
        {
            std::cout << "[LyricsGuide] Muting from lyrics: " << guide.extraMuteWords[k] << std::endl;
            hits.push_back({ guide.extraMuteWords[k], guide.extraMutes[k].first, guide.extraMutes[k].second, false,
                             guide.extraMuteIsFinal[k] });
        }
        
        // Displays. Audio at offset t in this window is heard (gap - (window - t))
        // after capture; transcription has already used `elapsed` of that.
        {
            double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - startTime).count();
            double gap = getCurrentBufferSize();
            auto msUntilHeard = [&](double t) {
                return juce::jmax(0, (int)((gap - (windowSeconds - t) - elapsed) * 1000.0));
            };
            auto alive = uiAlive;
            
            // "Whisper Heard": what was heard in the new part of the window, shown
            // as its middle plays
            juce::String heard;
            for (const auto& wordSeg : transcribedWords)
                if (wordSeg.start >= overlapSeconds)
                    heard += juce::String(wordSeg.word) + " ";
            heard = heard.trim();
            if (heard.removeCharacters(" ").equalsIgnoreCase("BLANKAUDIO"))   // Whisper's [BLANK_AUDIO]
                heard = {};
            
            if (heard.isNotEmpty())
            {
                int waitMs = msUntilHeard(overlapSeconds + chunkSeconds / 2.0);
                juce::MessageManager::callAsync([this, alive, heard, waitMs]() {
                    juce::Timer::callAfterDelay(waitMs, [this, alive, heard]() {
                        if (*alive && lyricsCallback)
                            lyricsCallback(heard);
                    });
                });
            }
            
            // "Actual Lyrics", karaoke style: each lyric line appears a moment before
            // it is sung, with the previous line above and the next two below
            const double karaokeLeadSeconds = 0.6;
            if (guide.songGeneration != lastCueGeneration)
            {
                lastCueGeneration = guide.songGeneration;
                lastCuedLine = -1;
                lastCueTime = -1.0;
            }
            const double windowStreamStart = captureStreamSeconds - windowSeconds;
            
            for (const auto& cue : guide.lineCues)
            {
                // In order of when they are heard: the overlap's repeats are
                // dropped, but a corrected match (even to an earlier line) shows
                double cueTime = windowStreamStart + cue.start;
                if (cue.line == lastCuedLine || cueTime < lastCueTime + 0.5)
                    continue;
                lastCuedLine = cue.line;
                lastCueTime = cueTime;
                
                // Four lines: previous, current, next, the one after (the panel
                // highlights the second)
                juce::String text;
                text << cue.previous << "\n" << cue.text << "\n" << cue.next << "\n" << cue.afterNext;
                
                int waitMs = juce::jmax(0, msUntilHeard(cue.start) - (int)(karaokeLeadSeconds * 1000.0));
                std::cout << "[Karaoke] line " << cue.line << " in " << waitMs << "ms" << std::endl;
                int line = cue.line;
                juce::MessageManager::callAsync([this, alive, text, waitMs, line]() {
                    juce::Timer::callAfterDelay(waitMs, [this, alive, text, line]() {
                        if (!*alive)
                            return;
                        if (!debugRecordDir.empty())
                            std::cout << "[Karaoke] showing line " << line << " at output "
                                      << (double) debugRecordPos / sampleRate << "s" << std::endl;
                        if (actualLyricsCallback)
                            actualLyricsCallback(text);
                    });
                });
            }
            
            // No lyrics for this song: show Whisper's words there instead
            if (!lyricsGuide->getStatus().hasLyrics && heard.isNotEmpty())
            {
                int waitMs = msUntilHeard(overlapSeconds);
                juce::MessageManager::callAsync([this, alive, heard, waitMs]() {
                    juce::Timer::callAfterDelay(waitMs, [this, alive, heard]() {
                        if (*alive && actualLyricsCallback)
                            actualLyricsCallback(heard);
                    });
                });
            }
        }
        
        // Process the detected profanity (single or multi-word)
        for (const auto& hit : hits)
        {
            const std::string& profanityText = hit.text;
            double profanityStart = hit.start;
            double profanityEnd = hit.end;
            bool isMultiWord = hit.multi;
            {
                // Skip censorship if buffer is critically low (emergency bypass)
                if (bufferUnderrun.load())
                {
                    std::cout << "[Phase6] Profanity \"" << profanityText
                              << "\" detected but SKIPPING (buffer underrun)" << std::endl;
                    
                    // Phase 8: Record skipped word
                    qualityAnalyzer.recordCensorshipEvent(profanityText, profanityStart, false, "SKIPPED", isMultiWord);
                    continue;
                }
                
                detectedWords.push_back(profanityText);
                
                // Phase 8: Record censorship event
                std::string modeStr = (currentCensorMode == CensorMode::Reverse) ? "REVERSE" : "MUTE";
                qualityAnalyzer.recordCensorshipEvent(profanityText, profanityStart, true, modeStr, isMultiWord);
                
                // Testing mode: Track this prediction
                if (testingMode)
                {
                    currentSongPredictions.emplace_back(profanityText, profanityStart, modeStr, isMultiWord);
                }
                
                // Phase 6: Calculate position in delay buffer
                // captureTime stores where the chunk ENDS (writePos when captured)
                // wordSeg.start/end are offsets from CHUNK START (0-chunkSeconds)
                // So: chunkStart = captureTime - chunkSeconds, profanityPos = chunkStart + offset
                int chunkEndPos = (int)captureTime;
                int chunkStartPos = windowStartPos;
                juce::ignoreUnused(chunkEndPos);
                
                // Correct Whisper's late word starts (startBias, above) and pad a little
                // Keep mutes tight: a leading "f" sound is fine, a muted neighbour
                // word is not
                double paddingBefore = 0.0;
                double paddingAfter = 0.08;
                
                // Words the lyrics placed for us are already in real time
                double bias = hit.finalTimes ? 0.0 : startBias;
                double padBefore = hit.finalTimes ? 0.0 : paddingBefore;
                double padAfter = hit.finalTimes ? 0.0 : paddingAfter;
                
                int startSample = (int)((profanityStart + bias - padBefore) * sampleRate);
                int endSample = (int)((profanityEnd + padAfter) * sampleRate);
                
                // Clamp to the chunk, allowing a little past its end for the padding
                // (that audio has arrived by the time transcription finishes)
                int maxSample = windowSamples + (int)(sampleRate * 0.2);
                startSample = std::max(0, std::min(startSample, maxSample));
                endSample = std::max(startSample, std::min(endSample, maxSample));
                
                // Calculate actual buffer positions we'll modify
                int actualStartPos = (chunkStartPos + startSample) % delayBufferSize;
                int actualEndPos = (chunkStartPos + endSample) % delayBufferSize;
                
                // Windows overlap, so the same word can be found twice. Censor each
                // stretch once (reversing twice would play it forwards again)
                {
                    int newLen = endSample - startSample;
                    bool alreadyDone = false;
                    
                    // Only the previous window can repeat a word; older entries would
                    // alias once the circular delay buffer wraps
                    auto nowTime = std::chrono::steady_clock::now();
                    recentCensors.erase(std::remove_if(recentCensors.begin(), recentCensors.end(),
                        [nowTime](const CensorRecord& r) { return nowTime - r.when > std::chrono::seconds(4); }),
                        recentCensors.end());
                    
                    for (const auto& done : recentCensors)
                    {
                        int offset = (actualStartPos - done.pos + delayBufferSize) % delayBufferSize;
                        int back = (done.pos - actualStartPos + delayBufferSize) % delayBufferSize;
                        int overlap = offset < done.length ? std::min(newLen, done.length - offset)
                                    : (back < newLen ? std::min(done.length, newLen - back) : 0);
                        if (overlap > newLen * 3 / 10)
                            alreadyDone = true;
                    }
                    // The next window often finds the same word with a slightly
                    // different time; muting both made one word's mute longer
                    if (alreadyDone)
                    {
                        std::cout << "[Phase6] \"" << profanityText << "\" already censored (overlap)" << std::endl;
                        continue;
                    }
                    recentCensors.push_back({ actualStartPos, newLen, nowTime });
                }
                int currentReadPos = delayReadPos;  // Snapshot current read position
                
                // Calculate how far ahead of readPos we are
                int distanceFromRead = (actualStartPos - currentReadPos + delayBufferSize) % delayBufferSize;
                double secondsAhead = (double)distanceFromRead / sampleRate;
                
                std::string profanityTypeLabel = isMultiWord ? "MULTI-WORD PROFANITY" : "PROFANITY";
                std::cout << "[Phase6] *** " << profanityTypeLabel << ": \"" << profanityText << "\" ***" << std::endl;
                std::cout << "[Phase6]     Whisper timestamp: " << profanityStart << "s - " << profanityEnd << "s" << std::endl;
                std::cout << "[Phase6]     With padding: " << (profanityStart - paddingBefore) << "s - " 
                         << (profanityEnd + paddingAfter) << "s" << std::endl;
                std::cout << "[Phase6]     Sample range in chunk: " << startSample << " - " << endSample 
                         << " (" << (endSample - startSample) << " samples)" << std::endl;
                std::cout << "[Phase6]     Buffer positions: chunkEnd=" << chunkEndPos << ", chunkStart=" << chunkStartPos 
                         << ", profanityStart=" << actualStartPos << ", profanityEnd=" << actualEndPos << std::endl;
                std::cout << "[Phase6]     Current readPos=" << currentReadPos 
                         << ", distance ahead=" << distanceFromRead << " samples (" 
                         << std::fixed << std::setprecision(2) << secondsAhead << "s)" << std::endl;
                
                if (secondsAhead < 1.0)
                {
                    std::cout << "[Phase6]     âš ï¸ WARNING: Too close to readPos! Censorship may be late!" << std::endl;
                }
                
                // Apply censorship based on mode
                int numSamplesToCensor = endSample - startSample;
                int fadeSamples = std::min(480, numSamplesToCensor / 4);  // 10ms fade @ 48kHz
                
                if (currentCensorMode == CensorMode::Mute)
                {
                    // MUTE: Zero out the samples
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        for (int i = startSample; i < endSample; ++i)
                        {
                            int delayPos = (chunkStartPos + i) % delayBufferSize;
                            delayBuffer[ch][delayPos] = 0.0f;
                        }
                    }
                    std::cout << "[Phase6]     âœ“ MUTED in delay buffer" << std::endl;
                }
                else if (currentCensorMode == CensorMode::Reverse)
                {
                    // REVERSE: Flip audio backwards with fade
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        // First, copy the region to temporary buffer
                        std::vector<float> tempBuffer(numSamplesToCensor);
                        for (int i = 0; i < numSamplesToCensor; ++i)
                        {
                            int delayPos = (chunkStartPos + startSample + i) % delayBufferSize;
                            tempBuffer[i] = delayBuffer[ch][delayPos];
                        }
                        
                        // Reverse it
                        std::reverse(tempBuffer.begin(), tempBuffer.end());
                        
                        // Apply fade and volume reduction, then write back
                        for (int i = 0; i < numSamplesToCensor; ++i)
                        {
                            float sample = tempBuffer[i];
                            float volumeReduction = 0.5f;  // 50% volume
                            
                            // Fade at edges to prevent clicks
                            if (i < fadeSamples)
                            {
                                float fadeGain = (float)i / fadeSamples;
                                sample *= fadeGain * volumeReduction;
                            }
                            else if (i >= numSamplesToCensor - fadeSamples)
                            {
                                float fadeGain = (float)(numSamplesToCensor - i) / fadeSamples;
                                sample *= fadeGain * volumeReduction;
                            }
                            else
                            {
                                sample *= volumeReduction;
                            }
                            
                            int delayPos = (chunkStartPos + startSample + i) % delayBufferSize;
                            delayBuffer[ch][delayPos] = sample;
                        }
                    }
                    std::cout << "[Phase6]     âœ“ REVERSED in delay buffer" << std::endl;
                }
            }
        }
        
        std::cout << "[Phase6] \"" << fullTranscript << "\"" << std::endl;
        
        if (!detectedWords.empty())
        {
            std::cout << "[Phase6] *** PROFANITY DETECTED: ";
            for (const auto& w : detectedWords)
                std::cout << "\"" << w << "\" ";
            std::cout << "***" << std::endl;
        }
        
        // Reset Whisper state to prevent memory/performance degradation
        whisper_reset_timings(whisperCtx);
        
        // End timing
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        double seconds = duration.count() / 1000.0;
        double realTimeFactor = seconds / chunkSeconds;
        
        std::cout << "[Phase6] ================================================" << std::endl;
        std::cout << "[TIMING] Processed " << chunkSeconds << "s audio in " << seconds << "s (RTF: " 
                  << std::fixed << std::setprecision(2) << realTimeFactor << "x)";
        
        if (realTimeFactor > 1.0)
            std::cout << " [WARNING: Processing slower than real-time!]";
        
        std::cout << std::endl;
        
        // Phase 8: Record RTF and update session duration
        qualityAnalyzer.recordRTF(realTimeFactor);
        qualityAnalyzer.updateSessionDuration(streamTime);
    }
    catch (const std::exception& e)
    {
        std::cout << "[Phase6] Exception in processTranscription: " << e.what() << std::endl;
    }
}

void AudioEngine::saveWavFile(const std::string& filename, const std::vector<float>& samples, int sampleRate)
{
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        std::cerr << "[ERROR] Could not open " << filename << " for writing" << std::endl;
        return;
    }
    
    // WAV header (44 bytes)
    file.write("RIFF", 4);
    int32_t fileSize = 36 + samples.size() * 2;
    file.write(reinterpret_cast<char*>(&fileSize), 4);
    file.write("WAVE", 4);
    
    // fmt chunk
    file.write("fmt ", 4);
    int32_t fmtSize = 16;
    file.write(reinterpret_cast<char*>(&fmtSize), 4);
    int16_t audioFormat = 1; // PCM
    file.write(reinterpret_cast<char*>(&audioFormat), 2);
    int16_t numChannels = 1; // Mono
    file.write(reinterpret_cast<char*>(&numChannels), 2);
    file.write(reinterpret_cast<char*>(&sampleRate), 4);
    int32_t byteRate = sampleRate * 2; // 16-bit = 2 bytes per sample
    file.write(reinterpret_cast<char*>(&byteRate), 4);
    int16_t blockAlign = 2;
    file.write(reinterpret_cast<char*>(&blockAlign), 2);
    int16_t bitsPerSample = 16;
    file.write(reinterpret_cast<char*>(&bitsPerSample), 2);
    
    // data chunk
    file.write("data", 4);
    int32_t dataSize = samples.size() * 2;
    file.write(reinterpret_cast<char*>(&dataSize), 4);
    
    // Convert float [-1.0, 1.0] to int16 [-32768, 32767]
    for (float sample : samples)
    {
        // Clamp to prevent overflow
        float clamped = std::max(-1.0f, std::min(1.0f, sample));
        int16_t intSample = static_cast<int16_t>(clamped * 32767.0f);
        file.write(reinterpret_cast<char*>(&intSample), 2);
    }
    
    file.close();
    std::cout << "[DEBUG] Saved " << samples.size() << " samples to " << filename << std::endl;
}
