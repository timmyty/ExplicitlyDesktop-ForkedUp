/*
  ==============================================================================

    AudioEngine.h
    Created: 9 Dec 2024
    Author: Explicitly Audio Systems

    Lock-free real-time audio processing engine.
    
    Responsibilities:
    - Capture audio from input device
    - Write to 300ms circular buffer
    - Push audio chunks to ASR thread via lock-free queue
    - Apply censorship based on ASR results
    - Output filtered audio to speakers
    
    Thread Safety:
    - audioDeviceIOCallback runs on real-time thread (no allocations, no locks)
    - Uses lock-free queues for ASR communication

  ==============================================================================
*/

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <whisper.h>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>
#include "QualityAnalyzer.h"
#include <mutex>
#include <condition_variable>
#include "ProfanityFilter.h"
#include "LyricsAlignment.h"
#include "VocalFilter.h"
#include "TimestampRefiner.h"
#include "SongRecognition.h"
#include "WindowsMediaInfo.h"
#include "LyricsGuide.h"
#include "Types.h"

class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    enum class CensorMode
    {
        Reverse,
        Mute
    };
    
    AudioEngine();
    ~AudioEngine() override;

    struct ModelChoice { std::string name; std::string label; };
    /** Whisper models in Models/, lightest first, labelled by performance impact. */
    static const std::vector<ModelChoice>& availableModels();
    /** Pick the model to use; takes effect on the next start(). */
    void setWhisperModel(const std::string& name);
    const std::string& getWhisperModel() const { return whisperModelName; }
    
    /**
        Start audio processing.
        
        @param inputDeviceName      Input device name (microphone, line-in)
        @param outputDeviceName     Output device name (speakers, headphones)
        @param mode                 Censorship mode (Reverse or Mute)
        @return                     true if started successfully
    */
    bool start(const juce::String& inputDeviceName,
               const juce::String& outputDeviceName,
               CensorMode mode);
    
    /**
        Stop audio processing.
    */
    void stop();
    
    /**
        Get current estimated latency in milliseconds.
        
        @return     Latency in ms, or -1.0 if not processing
    */
    double getCurrentLatency() const;
    
    /**
        Get current buffer capacity in seconds.
        
        Buffer starts at 5.1s and grows as processing accumulates (RTF < 1.0x).
        
        @return     Buffer size in seconds
    */
    double getCurrentBufferSize() const;
    
    /**
        Check if buffer is in underrun state (< 3 seconds).
        
        When buffer underruns, censorship is temporarily disabled to prevent glitches.
        
        @return     true if buffer is critically low
    */
    bool isBufferUnderrun() const;

    /** Debug: save the rolling input/output recording (EXPLICITLY_DEBUG_ROLLING_SECONDS). */
    bool dumpDebugRecording();

    /**
        Filter Explicit Speech toggle. Safe to call from any thread.

        When off, input passes straight to the output with no delay buffer and
        Whisper is idle. When turned back on, output is silent for
        initialDelaySeconds while the delay buffer refills.
    */
    void setFilterEnabled(bool enabled);
    bool isFilterEnabled() const { return filterEnabled.load(); }

    struct FilterStatus
    {
        bool running = false;
        bool enabled = false;
        bool buffering = false;      // Filter on but output silent while the delay refills
        double delaySeconds = 0.0;   // Delay between input and output (0 when filter is off)
    };

    /**
        Snapshot of the toggle and current delay, used by the UI and the
        local status server that keeps browser video in sync.
    */
    FilterStatus getFilterStatus() const;
    
    /** Lyrics guide state for the UI (song, where lyrics came from, following). */
    /** Save the user's Genius API key for lyrics lookups. */
    void setGeniusToken(const std::string& token)
    {
        if (lyricsGuide != nullptr)
            lyricsGuide->setGeniusToken(token);
    }

    /** Use pasted lyrics for the song playing now (saved as its local copy). */
    void setManualLyricsForCurrentSong(const std::string& text)
    {
        if (lyricsGuide != nullptr)
            lyricsGuide->setManualLyrics(text);
    }

    std::string getFullLyrics() const
    {
        return lyricsGuide != nullptr ? lyricsGuide->getFullLyrics() : std::string();
    }

    LyricsGuide::Status getLyricsStatus() const
    {
        return lyricsGuide != nullptr ? lyricsGuide->getStatus() : LyricsGuide::Status();
    }

    /**
        Enable/disable testing mode.
        
        When enabled, creates a log file for each song with all profanity predictions.
        
        @param enabled  true to enable testing mode
    */
    void setTestingMode(bool enabled);
    
    /**
        Phase 1: Get current input level (RMS).
        
        @return     RMS level 0.0-1.0
    */
    float getCurrentInputLevel() const;
    
    /**
        Get audio device manager (for device enumeration).
    */
    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    
    /**
        Get last error message.
    */
    juce::String getLastError() const { return lastError; }
    
    /**
        Phase 8: Get quality analyzer for statistics.
    */
    QualityAnalyzer& getQualityAnalyzer() { return qualityAnalyzer; }
    
    /**
        Set debug callback for UI updates.
    */
    void setDebugCallback(std::function<void(const juce::String&)> callback) 
    { 
        debugCallback = callback; 
    }
    
    /**
        Set lyrics callback for live display (Whisper transcription).
    */
    void setLyricsCallback(std::function<void(const juce::String&)> callback)
    {
        lyricsCallback = callback;
    }
    
    /**
        Set actual lyrics callback for displaying aligned/corrected lyrics.
    */
    void setActualLyricsCallback(std::function<void(const juce::String&)> callback)
    {
        actualLyricsCallback = callback;
    }
    
    /**
        Set song info callback for displaying detected song.
        
        @param callback Function called with (artist, title, confidence)
    */
    void setSongInfoCallback(std::function<void(const juce::String&, const juce::String&, float)> callback)
    {
        songInfoCallback = callback;
    }
    
    /**
        Set song info to fetch lyrics automatically.
        
        @param artist   Artist name
        @param title    Song title
        @return         true if lyrics fetched successfully
    */
    bool setSongInfo(const std::string& artist, const std::string& title);
    
    /**
        Set lyrics manually (skip API fetch).
        
        @param lyrics   Song lyrics text
    */
    void setManualLyrics(const std::string& lyrics);
    
    // AudioIODeviceCallback interface
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                         int numInputChannels,
                                         float* const* outputChannelData,
                                         int numOutputChannels,
                                         int numSamples,
                                         const juce::AudioIODeviceCallbackContext& context) override;
    
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    // Helper methods
    void whisperThreadFunction();
    std::vector<float> resampleTo16kHz(const std::vector<float>& input);
    void processTranscription(const std::vector<float>& buffer, double captureTime);
    
    // Audio device
    juce::AudioDeviceManager deviceManager;
    
    // Configuration variables (easy tuning)
    double chunkSeconds = 2.0;           // Audio chunk size to process (larger chunks = more context)
    double overlapSeconds = 1.0;         // Audio before each chunk also transcribed, to catch boundary words
    double initialDelaySeconds = 3.5;   // Delay while filtering: gives late catches (words found at the start of the next slice) time to land
    int whisperThreads = 8;              // CPU threads for Whisper (set in constructor)
    std::string whisperModelName = "medium.en";
    bool loadWhisperModel();
    
    // Simple level tracking for Phase 1-2
    std::atomic<float> currentInputLevel {0.0f};
    
    // Phase 5: Whisper integration with background thread
    whisper_context* whisperCtx = nullptr;
    std::vector<float> audioBuffer;          // Accumulation buffer (audio callback writes here)
    std::vector<float> processingBuffer;     // Copy for background thread to process
    std::vector<float> audioBuffer16k;
    int bufferWritePos = 0;
    int transcriptionInterval = 0;
    ProfanityFilter profanityFilter;
    VocalFilter vocalFilter;
    TimestampRefiner timestampRefiner;  // Phase 6: Accurate timestamp refinement
    LyricsAlignment lyricsAlignment;     // Phase 7: Lyrics alignment
    std::unique_ptr<LyricsGuide> lyricsGuide;  // Checks Whisper against the song's lyrics
    struct CensorRecord { int pos; int length; std::chrono::steady_clock::time_point when; };
    std::vector<CensorRecord> recentCensors;   // Stretches censored recently (windows overlap)
    int lastCuedLine = -1;          // Karaoke: last lyric line scheduled for display
    double lastCueTime = -1.0;      // ...and when in the stream it starts (seconds)
    int lastCueGeneration = -1;
    std::atomic<long long> samplesCaptured{0};   // Input samples since start (stream clock)
    long long bufferCaptureSamples = 0;          // samplesCaptured when the chunk was sent
    std::shared_ptr<bool> uiAlive = std::make_shared<bool>(true);  // Guards delayed UI updates
    double lastMediaPollTime = 0.0;
    
    // Phase 7 (Idea 2): Song Recognition & Lyrics Alignment
    WindowsMediaInfo windowsMediaInfo;
    bool mediaInfoInitialized = false;
    SongRecognition songRecognition;  // Fallback for when Windows Media Control unavailable
    std::atomic<bool> songIdentificationAttempted{false};
    std::vector<float> recognitionBuffer;
    const int recognitionBufferSize = 48000 * 10; // 10 seconds @ 48kHz
    
    // Song info
    bool songIdentified = false;
    SongRecognition::SongInfo currentSong;
    std::string songLyrics;
    bool useLyricsAlignment = false;
    std::string lastSongTitle;
    std::string lastSongArtist;
    double songElapsedTime = 0.0;  // Estimated position in song (seconds)
    
    // Background processing thread
    std::thread whisperThread;
    std::atomic<bool> shouldStop{false};
    std::mutex bufferMutex;
    std::condition_variable bufferCV;
    std::atomic<bool> hasNewBuffer{false};
    
    // Delay buffer for real-time processing with look-ahead
    std::vector<std::vector<float>> delayBuffer;
    int delayBufferSize = 0;
    int delayReadPos = 0;
    int delayWritePos = 0;
    
    // Runtime state
    std::atomic<bool> isRunning{false};
    int numChannels = 0;
    bool bufferReady = false;
    std::atomic<bool> shouldStopThread{false};
    CensorMode currentCensorMode = CensorMode::Reverse;
    
    // Playback timing
    double streamTime = 0.0;
    std::atomic<bool> playbackStarted{false};
    std::atomic<bool> outputPaused{false};    // Output silenced while the delay buffer rebuilds
    std::atomic<bool> filterEnabled{true};    // Filter Explicit Speech toggle
    
    // Debug: EXPLICITLY_DEBUG_RECORD=<folder> records the first 60s of input
    // and output (mono) to WAVs, for measuring censor timing
    std::string debugRecordDir;
    std::vector<float> debugInput, debugOutput;
    size_t debugRecordPos = 0;
    std::atomic<bool> debugRecordFull{false};
    bool debugRecordWritten = false;
    bool debugRolling = false;
    std::atomic<bool> wasWaiting{false};
    int debugCounter = 0;
    double bufferCaptureTime = 0.0;
    double captureStreamSeconds = 0.0;   // Stream time at the end of the chunk being transcribed
    
    // Buffer underrun handling
    std::atomic<bool> bufferUnderrun{false};
    double lastUnderrunWarningTime = 0.0;
    
    // Error handling
    juce::String lastError;
    
    // Callbacks
    std::function<void(const juce::String&)> debugCallback;
    std::function<void(const juce::String&)> lyricsCallback;           // Whisper transcription
    std::function<void(const juce::String&)> actualLyricsCallback;     // Aligned/corrected lyrics
    std::function<void(const juce::String&, const juce::String&, float)> songInfoCallback;
    
    // Quality analysis
    QualityAnalyzer qualityAnalyzer;
    
    // Testing mode
    struct ProfanityPrediction
    {
        std::string word;
        double timestamp;
        std::string censorMode;
        bool isMultiWord;
        
        ProfanityPrediction(const std::string& w, double t, const std::string& mode, bool multi = false)
            : word(w), timestamp(t), censorMode(mode), isMultiWord(multi) {}
    };
    
    bool testingMode = true;
    std::vector<ProfanityPrediction> currentSongPredictions;
    void writeTestingLog(const std::string& artist, const std::string& title);
    
    // Helper function to save debug WAV files
    void saveWavFile(const std::string& filename, const std::vector<float>& samples, int sampleRate);
    
    // Runtime state
    CensorMode censorMode = CensorMode::Reverse;
    int sampleRate = 48000;
    double totalProcessed = 0.0;
};
