/*
  ==============================================================================

    MainComponent.h
    Created: 9 Dec 2024
    Author: Explicitly Audio Systems

    Main GUI component with controls and status display.
    
    Features:
    - Start/Stop processing button
    - Input/output device selection
    - Censor mode selector (Reverse/Mute)
    - Real-time latency indicator
    - Status text display

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "ControlServer.h"

class MainComponent  : public juce::Component,
                       public juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    
    void timerCallback() override;

private:
    void startProcessing();
    void stopProcessing();
    void updateLatencyDisplay();
    void updateDeviceList();
    void addDebugMessage(const juce::String& message, bool isProfanity = false);
    void exportDebugLog();
    void updateLiveLyrics(const juce::String& words);
    void updateActualLyrics(const juce::String& words);
    void updateFilterButton();
    void updateLyricsStatus();
    void restoreSettings();
    void saveSettings();
    
    // GUI Components - Controls
    juce::Label titleLabel;
    
    juce::Label inputDeviceLabel;
    juce::ComboBox inputDeviceCombo;
    
    juce::Label outputDeviceLabel;
    juce::ComboBox outputDeviceCombo;
    
    juce::Label censorModeLabel;
    juce::ComboBox censorModeCombo;
    
    juce::TextButton startStopButton;
    juce::TextButton filterToggleButton;   // Filter Explicit Speech on/off
    
    juce::Label statusLabel;
    juce::Label statusValueLabel;
    
    juce::Label latencyLabel;
    juce::Label latencyValueLabel;
    
    // Phase 1: Level meter
    juce::Label levelLabel;
    juce::Label levelValueLabel;
    
    // Lyrics input
    juce::Label lyricsLabel;
    juce::TextEditor lyricsInput;
    juce::TextButton fetchLyricsButton;
    juce::Label artistLabel;
    juce::TextEditor artistInput;
    juce::Label titleLabel2;
    juce::TextEditor titleInput;
    
    // GUI Components - Debug Display
    juce::Label transcriptLabel;
    juce::TextEditor transcriptDisplay;
    
    juce::Label dspDebugLabel;
    juce::TextEditor dspDebugDisplay;
    
    // Live lyrics display (Whisper transcription)
    juce::Label liveLyricsLabel;
    juce::Label liveLyricsDisplay;
    
    // Actual lyrics display (aligned/corrected)
    juce::Label actualLyricsLabel;
    juce::Label actualLyricsDisplay;
    
    // Song recognition display
    juce::Label songInfoLabel;
    juce::Label songInfoDisplay;
    
    juce::TextButton pasteLyricsButton;      // Paste lyrics for the song playing now
    juce::TextButton geniusKeyButton;        // Enter a Genius API key for lyrics lookups
    std::unique_ptr<juce::AlertWindow> geniusKeyWindow;
    void showGeniusKey();
    std::unique_ptr<juce::AlertWindow> pasteLyricsWindow;
    std::unique_ptr<juce::TextEditor> pasteLyricsEditor;
    void showPasteLyrics();

    juce::ToggleButton showRawJsonToggle;
    juce::TextButton exportLogButton;
    
    // Testing mode toggle
    juce::ToggleButton testingModeToggle;
    
    // Audio Engine
    std::unique_ptr<AudioEngine> audioEngine;
    juce::ApplicationProperties appProperties;     // Remembers devices, mode and toggle
    std::unique_ptr<ControlServer> controlServer;   // Lets the browser extension sync video delay
    bool isProcessing = false;
    juce::String debugLog;
    juce::String recentLyrics;         // Store recent words for live display (Whisper)
    juce::String recentActualLyrics;   // Store recent words for actual lyrics display

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
