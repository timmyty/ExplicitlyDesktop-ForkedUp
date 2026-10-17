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
    
    // Fix Unicode quote characters: ΓÖ¬ (U+00C3 U+0096 U+00AC) -> '
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
            std::cout << "[MERGE] Fixed split word: \"" << split << "\" → \"" << whole << "\"" << std::endl;
            pos += whole.length();
        }
    }
    
    return merged;
}

AudioEngine::AudioEngine()
{
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
    
    // Phase 5: Load Whisper model at startup (faster "Start Processing" button response)
    std::cout << "[Phase5] Loading Whisper model at startup..." << std::endl;
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;  // CPU only for consistency
    cparams.dtw_token_timestamps = true;  // Enable DTW for better timestamp alignment
    cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE_EN;  // Use base.en alignment preset
    whisperCtx = whisper_init_from_file_with_params("Models/ggml-base.en.bin", cparams);
    
    if (whisperCtx == nullptr)
    {
        std::cout << "[Phase5] ERROR: Failed to load Whisper model at startup" << std::endl;
    }
    else
    {
        std::cout << "[Phase5] Whisper base.en model loaded successfully" << std::endl;
    }
}

AudioEngine::~AudioEngine()
{
    stop();
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
    
    // Phase 5: Check if Whisper model was loaded at startup
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
                
                // Fetch lyrics ASYNCHRONOUSLY - don't block audio thread
                if (!info.title.empty() && !info.artist.empty())
                {
                    std::cout << "[MediaInfo] Fetching lyrics in background..." << std::endl;
                    
                    // Disable alignment immediately and reset state (use raw Whisper for first ~2s)
                    useLyricsAlignment = false;
                    lyricsAlignment.reset();
                    songLyrics.clear();
                    
                    // Copy to local variables for thread capture
                    std::string artist = info.artist;
                    std::string title = info.title;
                    
                    // Launch background thread
                    std::thread([this, artist, title]() {
                        std::cout << "[LyricsFetch] Background fetch started for: " 
                                 << artist << " - " << title << std::endl;
                        
                        SongInfo lyricsInfo = LyricsAlignment::fetchLyrics(artist, title);
                        
                        // Post result back to message thread
                        juce::MessageManager::callAsync([this, lyricsInfo]() {
                            if (!lyricsInfo.lyrics.empty())
                            {
                                songLyrics = lyricsInfo.lyrics;
                                lyricsAlignment.reset();
                                lyricsAlignment.setLyrics(songLyrics);
                                // useLyricsAlignment = true;  // DISABLED FOR TESTING
                                
                                std::cout << "[LyricsFetch] ✓ Lyrics ready! Alignment DISABLED for testing (" 
                                         << songLyrics.length() << " chars)" << std::endl;
                            }
                            else
                            {
                                std::cout << "[LyricsFetch] ✗ No lyrics found - using raw Whisper" << std::endl;
                            }
                        });
                    }).detach();
                }
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
                
                // Fetch initial lyrics ASYNCHRONOUSLY
                std::cout << "[MediaInfo] Fetching initial lyrics in background..." << std::endl;
                useLyricsAlignment = false;  // Use raw Whisper initially
                
                // Copy to local variables for thread capture
                std::string artist = initialInfo.artist;
                std::string title = initialInfo.title;
                
                std::thread([this, artist, title]() {
                    std::cout << "[LyricsFetch] Background fetch started for: " 
                             << artist << " - " << title << std::endl;
                    
                    SongInfo lyricsInfo = LyricsAlignment::fetchLyrics(artist, title);
                    
                    juce::MessageManager::callAsync([this, lyricsInfo]() {
                        if (!lyricsInfo.lyrics.empty())
                        {
                            songLyrics = lyricsInfo.lyrics;
                            lyricsAlignment.reset();
                            lyricsAlignment.setLyrics(songLyrics);
                            // useLyricsAlignment = true;  // DISABLED FOR TESTING
                            
                            std::cout << "[LyricsFetch] ✓ Initial lyrics ready! Alignment DISABLED for testing (" 
                                     << songLyrics.length() << " chars)" << std::endl;
                        }
                        else
                        {
                            std::cout << "[LyricsFetch] ✗ No initial lyrics found" << std::endl;
                        }
                    });
                }).detach();
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
        std::string fpcalcPath = "C:\\\\Users\\\\andre\\\\Desktop\\\\Explicitly\\\\chromaprint-fpcalc-1.6.0-windows-x86_64\\\\fpcalc.exe";
        std::string acoustidKey = "bNfeKNh59F";
        
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
    
    // Cleanup Whisper
    if (whisperCtx)
    {
        whisper_free(whisperCtx);
        whisperCtx = nullptr;
    }
    
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

    if (status.running && status.enabled)
    {
        status.buffering = !playbackStarted.load() || outputPaused.load();
        // While buffering, output is silent and will start at the target delay
        status.delaySeconds = status.buffering ? initialDelaySeconds : getCurrentBufferSize();
    }

    return status;
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
    if (!filterEnabled.load())
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
    if (!filterEnabled.load())
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
                std::cout << "\n[Phase6] ✓ " << initialDelaySeconds << " SECONDS BUFFERED - PLAYBACK STARTING NOW!" << std::endl;
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
                std::cout << "\n[Phase6] ⚠ Buffer dropped to " << std::fixed << std::setprecision(2) 
                         << bufferSeconds << "s - PAUSING playback to rebuild buffer\n" << std::endl;
            }
            else if (bufferSeconds >= resumeThreshold && wasPaused)
            {
                wasPaused = false;
                std::cout << "\n[Phase6] ✓ Buffer recovered to " << std::fixed << std::setprecision(2) 
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
    
    std::cout << "[Testing] ✓ Log file created: " << filename << std::endl;
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
        int samplesToProcess = (int)(sampleRate * chunkSeconds);
        samplesToProcess = std::min(samplesToProcess, (int)buffer.size());
        std::vector<float> bufferCopy(buffer.begin(), buffer.begin() + samplesToProcess);
        
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
        
        std::cout << "[Phase5] Resampled " << samplesToProcess << " samples to " 
                  << audioBuffer16k.size() << " samples @ 16kHz" << std::endl;
        
        // Configure Whisper parameters - OPTIMIZED FOR SPEED
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_realtime = false;
        wparams.print_progress = false;
        wparams.print_timestamps = true;  // Enable for word-level timing
        wparams.print_special = false;
        wparams.translate = false;
        wparams.language = "en";
        wparams.n_threads = 8;  // Increase threads (use all CPU cores)
        wparams.single_segment = false;
        wparams.token_timestamps = false;
        wparams.max_len = 0;
        
        // Speed optimizations (valid parameters only)
        wparams.audio_ctx = 1500;  // Enable audio context for music disambiguation
        wparams.temperature = 0.0f;  // Start with greedy decoding
        wparams.temperature_inc = 0.2f;  // Try multiple decoding strategies (0, 0.2, 0.4, 0.6, 0.8, 1.0)
        wparams.entropy_thold = 5.0f;  // Don't skip uncertain segments (music has high entropy)
        wparams.logprob_thold = -1.0f;  // Accept lower probability tokens (faster)
        
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
        
        // Extract word-level segments using SEGMENT timestamps (more reliable than token timestamps)
        int numSegments = whisper_full_n_segments(whisperCtx);
        std::vector<WordSegment> transcribedWords;
        
        std::cout << "[Phase6] Using segment-level timestamps (token timestamps unreliable)" << std::endl;
        
        for (int i = 0; i < numSegments; ++i)
        {
            // Get segment-level timestamps (these are accurate!)
            int64_t segmentStart = whisper_full_get_segment_t0(whisperCtx, i);
            int64_t segmentEnd = whisper_full_get_segment_t1(whisperCtx, i);
            double segStartSec = segmentStart * 0.01;  // centiseconds to seconds
            double segEndSec = segmentEnd * 0.01;
            
            // Get all tokens in this segment
            int numTokens = whisper_full_n_tokens(whisperCtx, i);
            std::vector<std::string> segmentWords;
            
            for (int j = 0; j < numTokens; ++j)
            {
                whisper_token_data token = whisper_full_get_token_data(whisperCtx, i, j);
                
                // Skip special tokens
                if (token.id >= whisper_token_eot(whisperCtx))
                    continue;
                
                const char* tokenText = whisper_full_get_token_text(whisperCtx, i, j);
                std::string word = cleanTranscriptText(tokenText);
                
                if (!word.empty())
                    segmentWords.push_back(word);
            }
            
            // Distribute words evenly across segment duration
            if (!segmentWords.empty())
            {
                double segmentDuration = segEndSec - segStartSec;
                double wordDuration = segmentDuration / segmentWords.size();
                
                for (size_t k = 0; k < segmentWords.size(); ++k)
                {
                    double wordStart = segStartSec + (k * wordDuration);
                    double wordEnd = wordStart + wordDuration;
                    
                    // Clamp to 0-5 second range
                    wordStart = std::max(0.0, std::min(5.0, wordStart));
                    wordEnd = std::max(wordStart + 0.05, std::min(5.0, wordEnd));
                    
                    transcribedWords.emplace_back(
                        segmentWords[k],
                        wordStart,
                        wordEnd,
                        0.9f  // Confidence (not available at segment level)
                    );
                }
            }
        }
        
        std::cout << "[Phase5] Extracted " << transcribedWords.size() << " word segments" << std::endl;
        
        // Phase 6: Refine timestamps using audio energy analysis
        std::cout << "[Phase6] Refining timestamps..." << std::endl;
        for (auto& word : transcribedWords)
        {
            timestampRefiner.refineWordTimestamp(word, bufferCopy, sampleRate);
        }
        
        // Apply lyrics alignment if enabled (sliding window approach)
        std::vector<WordSegment> finalWords = transcribedWords;
        
        // Periodic song change detection (works with or without lyrics)
        static auto lastPeriodicCheck = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto timeSinceLastCheck = std::chrono::duration_cast<std::chrono::seconds>(now - lastPeriodicCheck).count();
        
        bool shouldCheckForNewSong = false;
        
        if (useLyricsAlignment && !songLyrics.empty())
        {
            std::cout << "[Phase5] Applying lyrics alignment with sliding window..." << std::endl;
            
            // Check if we're near end of lyrics (auto-queued song detection)
            if (lyricsAlignment.isReady())
            {
                int currentPos = lyricsAlignment.getCurrentPosition();
                int totalWords = lyricsAlignment.getTotalWords();
                
                // If we're in the last 10% of lyrics, check if a new song is playing
                if (totalWords > 0 && currentPos >= (totalWords * 0.90))
                {
                    // Check every 3 seconds when near end
                    if (timeSinceLastCheck >= 3 && mediaInfoInitialized)
                    {
                        std::cout << "[EndOfSong] Near lyrics end (" << currentPos << "/" << totalWords 
                                 << ") - checking for queued song..." << std::endl;
                        shouldCheckForNewSong = true;
                    }
                }
            }
        }
        else
        {
            // No lyrics loaded - check periodically (every 10 seconds)
            // This handles songs without lyrics or failed lyrics fetches
            if (timeSinceLastCheck >= 10 && mediaInfoInitialized)
            {
                std::cout << "[PeriodicCheck] No lyrics active - checking for song change..." << std::endl;
                shouldCheckForNewSong = true;
            }
        }
        
        // Execute song change detection if triggered
        if (shouldCheckForNewSong)
        {
            auto currentMedia = windowsMediaInfo.getCurrentMedia();
            if (!currentMedia.title.empty())
            {
                // Check if this is a different song (basic title comparison)
                if (currentMedia.title != lastSongTitle || currentMedia.artist != lastSongArtist)
                {
                    std::cout << "[SongChange] New song detected! " << currentMedia.artist 
                             << " - " << currentMedia.title << std::endl;
                    
                    // Testing mode: Write log file for previous song before switching
                    // Launch background lyrics fetch
                    std::string artist = currentMedia.artist;
                    std::string title = currentMedia.title;
                    
                    std::thread([this, artist, title]() {
                        SongInfo lyricsInfo = LyricsAlignment::fetchLyrics(artist, title);
                        
                        juce::MessageManager::callAsync([this, lyricsInfo]() {
                            if (!lyricsInfo.lyrics.empty())
                            {
                                songLyrics = lyricsInfo.lyrics;
                                lyricsAlignment.reset();
                                lyricsAlignment.setLyrics(songLyrics);
                                // useLyricsAlignment = true;  // DISABLED FOR TESTING
                                
                                std::cout << "[SongChange] ✓ New song lyrics loaded! (ALIGNMENT DISABLED)" << std::endl;
                            }
                            else
                            {
                                std::cout << "[SongChange] ✗ No lyrics found for new song - using raw Whisper" << std::endl;
                            }
                        });
                    }).detach();
                }
            }
            
            lastPeriodicCheck = now;
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
                std::cout << "[Phase5] ⚠ Alignment returned empty - falling back to raw Whisper" << std::endl;
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
        
        // Send raw Whisper transcription to UI (for comparison)
        if (lyricsCallback && !transcribedWords.empty())
        {
            juce::String whisperWords;
            for (const auto& wordSeg : transcribedWords)
            {
                whisperWords += juce::String(wordSeg.word) + " ";
            }
            
            // Call on message thread
            juce::MessageManager::callAsync([this, whisperWords]() {
                if (lyricsCallback)
                    lyricsCallback(whisperWords.trim());
            });
        }
        
        // Send corrected/aligned lyrics to UI (actual lyrics)
        if (actualLyricsCallback && !finalWords.empty())
        {
            juce::String correctedWords;
            for (const auto& wordSeg : finalWords)
            {
                correctedWords += juce::String(wordSeg.word) + " ";
            }
            
            // Call on message thread
            juce::MessageManager::callAsync([this, correctedWords]() {
                if (actualLyricsCallback)
                    actualLyricsCallback(correctedWords.trim());
            });
        }
        
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
            
            // Process the detected profanity (single or multi-word)
            if (foundProfanity)
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
                int chunkStartPos = (chunkEndPos - (int)(sampleRate * chunkSeconds) + delayBufferSize) % delayBufferSize;
                
                // Tiny model tends to timestamp late - use asymmetric padding
                double paddingBefore = 0.4;  // 400ms before word (catch early starts)
                double paddingAfter = 0.1;   // 100ms after word (tight end)
                
                int startSample = (int)((profanityStart - paddingBefore) * sampleRate);
                int endSample = (int)((profanityEnd + paddingAfter) * sampleRate);
                
                // Clamp to valid range (0 to chunkSeconds)
                int maxSample = (int)(sampleRate * chunkSeconds);
                startSample = std::max(0, std::min(startSample, maxSample));
                endSample = std::max(startSample, std::min(endSample, maxSample));
                
                // Calculate actual buffer positions we'll modify
                int actualStartPos = (chunkStartPos + startSample) % delayBufferSize;
                int actualEndPos = (chunkStartPos + endSample) % delayBufferSize;
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
                    std::cout << "[Phase6]     ⚠️ WARNING: Too close to readPos! Censorship may be late!" << std::endl;
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
                    std::cout << "[Phase6]     ✓ MUTED in delay buffer" << std::endl;
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
                    std::cout << "[Phase6]     ✓ REVERSED in delay buffer" << std::endl;
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
