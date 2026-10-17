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
#include "AppRouter.h"
#include "PcmTap.h"
#include "RemoteSessions.h"
#include "PairedDevices.h"
#include "StreamHost.h"

/** "Actual Lyrics" panel. Plain text draws like a Label; four newline-separated
    lines (previous, current, next, after next) draw karaoke style with the
    current line highlighted. */
class LyricsPanel : public juce::Label
{
public:
    void paint (juce::Graphics& g) override
    {
        juce::StringArray lines;   // Split by hand so empty lines are kept
        auto text = getText();
        for (int start = 0;;)
        {
            int nl = text.indexOfChar (start, (juce::juce_wchar) 10);   // '\n'
            lines.add (text.substring (start, nl < 0 ? text.length() : nl));
            if (nl < 0)
                break;
            start = nl + 1;
        }
        if (lines.size() != 4)
        {
            juce::Label::paint (g);
            return;
        }

        g.fillAll (findColour (juce::Label::backgroundColourId));
        auto area = getLocalBounds().reduced (10, 4);
        float rowHeight = (float) area.getHeight() / 4.0f;

        for (int i = 0; i < 4; ++i)
        {
            bool current = (i == 1);
            g.setColour (current ? juce::Colours::white : juce::Colours::grey);
            g.setFont (juce::FontOptions (current ? 26.0f : 19.0f, current ? juce::Font::bold : juce::Font::plain));
            auto row = area.removeFromTop ((int) rowHeight);
            g.drawFittedText (lines[i], row, juce::Justification::centred, 1, 0.5f);
        }
    }
};

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
    
    // Phone streaming (docs/MOBILE_STREAMING.md)
    void setPhoneStreaming (bool on);
    void showPairingCode();
    void showPairApproval (const juce::String& deviceName, std::function<void (bool)> decide);
    void showPairedPhones();
    void updatePhoneStatus();
    
    // GUI Components - Controls
    juce::Label titleLabel;
    
    juce::Label inputDeviceLabel;
    juce::ComboBox inputDeviceCombo;
    
    juce::Label outputDeviceLabel;
    juce::ComboBox outputDeviceCombo;
    
    juce::Label censorModeLabel;
    juce::ComboBox censorModeCombo;
    
    juce::Label modelLabel;
    juce::ComboBox modelCombo;               // Whisper model, labelled by performance impact
    void applyModelChoice();

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
    LyricsPanel actualLyricsDisplay;
    
    // Song recognition display
    juce::Label songInfoLabel;
    juce::Label songInfoDisplay;
    
    juce::TextButton pasteLyricsButton;      // Paste lyrics for the song playing now
    juce::TextButton geniusKeyButton;        // Enter a Genius API key for lyrics lookups
    juce::TextButton previewLyricsButton;    // Shown when the song's full lyrics were found
    juce::Component::SafePointer<juce::DialogWindow> lyricsPreviewWindow;
    void showFullLyrics();
    std::unique_ptr<juce::AlertWindow> geniusKeyWindow;
    void showGeniusKey();
    std::unique_ptr<juce::AlertWindow> pasteLyricsWindow;
    std::unique_ptr<juce::TextEditor> pasteLyricsEditor;
    void showPasteLyrics();

    juce::ToggleButton showRawJsonToggle;
    juce::TextButton exportLogButton;
    
    // Testing mode toggle
    juce::ToggleButton testingModeToggle;
    
    // Phone streaming controls
    juce::ToggleButton phoneStreamingToggle;     // "Allow phone streaming" (off by default)
    juce::TextButton pairPhoneButton;            // Shows the QR code
    juce::TextButton phonesButton;               // Paired phones, with Revoke
    juce::ToggleButton alsoPlayHereToggle;       // "Also play on this PC" while a phone watches
    juce::Label phoneStatusLabel;
    juce::Component::SafePointer<juce::DialogWindow> pairingWindow;
    juce::String phoneAddress;                   // LAN address shown to phones
    int phoneTicks = 0;
    
    // Declared before the engine so they outlive its audio callback
    std::unique_ptr<PcmTap> pcmTap;              // Filtered output for the /pcm stream
    RemoteSessions remoteSessions;               // Phone offers <-> browser extension
    
    // Audio Engine
    std::unique_ptr<AudioEngine> audioEngine;
    bool outputOverridden = false;
    AppRouter appRouter;            // Apps to VB-CABLE while processing, back on Stop/exit
    int routerTicks = 0;
    bool snapshotTaken = false;     // EXPLICITLY_SNAPSHOT (docs)
    int snapshotCountdown = 0;   // EXPLICITLY_OUTPUT_DEVICE (testing): don't save it
    juce::ApplicationProperties appProperties;     // Remembers devices, mode and toggle
    std::unique_ptr<ControlServer> controlServer;   // Lets the browser extension sync video delay
    std::unique_ptr<PairedDevices> pairedDevices;   // Phones allowed to stream (devices.json)
    std::unique_ptr<StreamHost> streamHost;         // LAN server for phones (port 8767), off by default
    bool isProcessing = false;
    juce::String debugLog;
    juce::String recentLyrics;         // Store recent words for live display (Whisper)
    juce::String recentActualLyrics;   // Store recent words for actual lyrics display

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
