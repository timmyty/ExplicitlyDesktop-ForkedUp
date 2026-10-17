/*
  ==============================================================================

    MainComponent.cpp
    Created: 9 Dec 2024
    Author: Explicitly Audio Systems

    Main GUI implementation.

  ==============================================================================
*/

#include "MainComponent.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

MainComponent::MainComponent()
{
    setSize (900, 700);
    
    // Title
    titleLabel.setText ("Explicitly Desktop - Real-Time Filter", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (24.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (titleLabel);
    
    // Input Device
    inputDeviceLabel.setText ("Input Device:", juce::dontSendNotification);
    addAndMakeVisible (inputDeviceLabel);
    
    inputDeviceCombo.setTextWhenNoChoicesAvailable ("No devices available");
    inputDeviceCombo.setTextWhenNothingSelected ("Select input device");
    addAndMakeVisible (inputDeviceCombo);
    
    // Output Device
    outputDeviceLabel.setText ("Output Device:", juce::dontSendNotification);
    addAndMakeVisible (outputDeviceLabel);
    
    outputDeviceCombo.setTextWhenNoChoicesAvailable ("No devices available");
    outputDeviceCombo.setTextWhenNothingSelected ("Select output device");
    addAndMakeVisible (outputDeviceCombo);
    
    // Censor Mode
    censorModeLabel.setText ("Censor Mode:", juce::dontSendNotification);
    addAndMakeVisible (censorModeLabel);
    
    censorModeCombo.addItem ("Reverse", 1);
    censorModeCombo.addItem ("Mute", 2);
    censorModeCombo.setSelectedId (2);  // Default: Mute
    addAndMakeVisible (censorModeCombo);
    
    // Start/Stop Button
    startStopButton.setButtonText ("Start Processing");
    startStopButton.onClick = [this] 
    {
        if (!isProcessing)
            startProcessing();
        else
            stopProcessing();
    };
    addAndMakeVisible (startStopButton);
    
    // Filter Explicit Speech toggle (off = no delay, audio passes straight through)
    filterToggleButton.setClickingTogglesState (false);
    filterToggleButton.onClick = [this]
    {
        audioEngine->setFilterEnabled (! audioEngine->isFilterEnabled());
        updateFilterButton();
        saveSettings();
    };
    addAndMakeVisible (filterToggleButton);
    
    // Status
    statusLabel.setText ("Status:", juce::dontSendNotification);
    addAndMakeVisible (statusLabel);
    
    statusValueLabel.setText ("Idle", juce::dontSendNotification);
    statusValueLabel.setFont (juce::Font (16.0f, juce::Font::bold));
    addAndMakeVisible (statusValueLabel);
    
    // Buffer
    latencyLabel.setText ("Buffer:", juce::dontSendNotification);
    addAndMakeVisible (latencyLabel);
    
    latencyValueLabel.setText ("-- ms", juce::dontSendNotification);
    latencyValueLabel.setFont (juce::Font (16.0f, juce::Font::bold));
    addAndMakeVisible (latencyValueLabel);
    
    // Phase 1: Level meter
    levelLabel.setText ("Input Level:", juce::dontSendNotification);
    addAndMakeVisible (levelLabel);
    
    levelValueLabel.setText ("0.000", juce::dontSendNotification);
    levelValueLabel.setFont (juce::Font (16.0f, juce::Font::bold));
    addAndMakeVisible (levelValueLabel);
    
    // Debug Display - Transcript
    transcriptLabel.setText ("ASR Transcript:", juce::dontSendNotification);
    addAndMakeVisible (transcriptLabel);
    
    transcriptDisplay.setMultiLine (true);
    transcriptDisplay.setReadOnly (true);
    transcriptDisplay.setScrollbarsShown (true);
    transcriptDisplay.setCaretVisible (false);
    transcriptDisplay.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (transcriptDisplay);
    
    // Debug Display - DSP
    dspDebugLabel.setText ("DSP Debug:", juce::dontSendNotification);
    addAndMakeVisible (dspDebugLabel);
    
    dspDebugDisplay.setMultiLine (true);
    dspDebugDisplay.setReadOnly (true);
    dspDebugDisplay.setScrollbarsShown (true);
    dspDebugDisplay.setCaretVisible (false);
    dspDebugDisplay.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (dspDebugDisplay);
    
    // Live lyrics display (Whisper transcription)
    liveLyricsLabel.setText ("Whisper Heard:", juce::dontSendNotification);
    liveLyricsLabel.setFont (juce::Font (14.0f, juce::Font::bold));
    addAndMakeVisible (liveLyricsLabel);
    
    liveLyricsDisplay.setText ("", juce::dontSendNotification);
    liveLyricsDisplay.setFont (juce::Font (20.0f, juce::Font::bold));
    liveLyricsDisplay.setJustificationType (juce::Justification::centred);
    liveLyricsDisplay.setColour (juce::Label::backgroundColourId, juce::Colours::darkgrey);
    liveLyricsDisplay.setColour (juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible (liveLyricsDisplay);
    
    // Actual lyrics display (aligned/corrected)
    actualLyricsLabel.setText ("Actual Lyrics:", juce::dontSendNotification);
    actualLyricsLabel.setFont (juce::Font (14.0f, juce::Font::bold));
    addAndMakeVisible (actualLyricsLabel);
    
    actualLyricsDisplay.setText ("", juce::dontSendNotification);
    actualLyricsDisplay.setFont (juce::Font (24.0f, juce::Font::bold));
    actualLyricsDisplay.setMinimumHorizontalScale (0.5f);
    actualLyricsDisplay.setJustificationType (juce::Justification::centred);
    actualLyricsDisplay.setColour (juce::Label::backgroundColourId, juce::Colours::black);
    actualLyricsDisplay.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (actualLyricsDisplay);
    
    // Song recognition display
    songInfoLabel.setText ("Detected Song:", juce::dontSendNotification);
    songInfoLabel.setFont (juce::Font (14.0f, juce::Font::bold));
    addAndMakeVisible (songInfoLabel);
    
    songInfoDisplay.setText ("Pending...", juce::dontSendNotification);
    songInfoDisplay.setFont (juce::Font (18.0f, juce::Font::bold));
    songInfoDisplay.setJustificationType (juce::Justification::centred);
    songInfoDisplay.setColour (juce::Label::backgroundColourId, juce::Colours::darkgrey);
    songInfoDisplay.setColour (juce::Label::textColourId, juce::Colours::yellow);
    addAndMakeVisible (songInfoDisplay);
    
    // Paste lyrics (from any site) for the song playing now
    pasteLyricsButton.setButtonText ("Paste lyrics");
    pasteLyricsButton.onClick = [this] { showPasteLyrics(); };
    addAndMakeVisible (pasteLyricsButton);

    geniusKeyButton.setButtonText (LyricsGuide::hasGeniusToken() ? "Genius key: set" : "Genius key");
    geniusKeyButton.onClick = [this] { showGeniusKey(); };
    addAndMakeVisible (geniusKeyButton);

    // Debug Controls
    showRawJsonToggle.setButtonText ("Show Raw JSON");
    addAndMakeVisible (showRawJsonToggle);
    
    exportLogButton.setButtonText ("Export Debug Log");
    exportLogButton.onClick = [this] { exportDebugLog(); };
    addAndMakeVisible (exportLogButton);
    
    // Testing mode toggle
    testingModeToggle.setButtonText ("Testing Mode (Log Files)");
    testingModeToggle.setToggleState (false, juce::dontSendNotification);
    testingModeToggle.onClick = [this] {
        if (audioEngine != nullptr)
        {
            audioEngine->setTestingMode (testingModeToggle.getToggleState());
        }
    };
    addAndMakeVisible (testingModeToggle);
    
    // Initialize audio engine
    audioEngine = std::make_unique<AudioEngine>();
    
    // Set debug callback
    audioEngine->setDebugCallback ([this](const juce::String& msg) {
        auto msgCopy = msg;  // Ensure message is copied
        juce::MessageManager::callAsync ([this, msgCopy]() {
            if (isVisible())  // Safety check
                addDebugMessage (msgCopy, false);
        });
    });
    
    // Set lyrics callback (for Whisper transcription)
    audioEngine->setLyricsCallback ([this](const juce::String& words) {
        updateLiveLyrics(words);
    });
    
    // Set actual lyrics callback (for aligned/corrected lyrics)
    audioEngine->setActualLyricsCallback ([this](const juce::String& words) {
        updateActualLyrics(words);
    });
    
    // Set song info callback
    audioEngine->setSongInfoCallback ([this](const juce::String& artist, const juce::String& title, float confidence) {
        juce::MessageManager::callAsync ([this, artist, title, confidence]() {
            if (artist.isEmpty() || title.isEmpty())
            {
                songInfoDisplay.setText("Pending...", juce::dontSendNotification);
                songInfoDisplay.setColour(juce::Label::textColourId, juce::Colours::yellow);
            }
            else if (artist == "Unknown" && confidence == 0.0f)
            {
                // Song not recognized
                songInfoDisplay.setText("Song Not Recognized", juce::dontSendNotification);
                songInfoDisplay.setColour(juce::Label::textColourId, juce::Colours::orange);
            }
            else
            {
                // Song identified - UPDATE UI TO SHOW NEW SONG
                juce::String songText = artist + " - " + title + " (" + juce::String((int)(confidence * 100)) + "%)";
                songInfoDisplay.setText(songText, juce::dontSendNotification);
                songInfoDisplay.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
                
                // Clear actual lyrics display to show "Loading new lyrics..."
                recentActualLyrics.clear();
                actualLyricsDisplay.setText("🔄 Loading lyrics...", juce::dontSendNotification);
                actualLyricsDisplay.setColour(juce::Label::textColourId, juce::Colours::cyan);
                
                // Also flash the background to indicate song change
                actualLyricsDisplay.setColour(juce::Label::backgroundColourId, juce::Colours::darkblue);
            }
        });
    });
    
    // Local control port for the browser extension (YouTube video delay sync)
    controlServer = std::make_unique<ControlServer> (*audioEngine);
    controlServer->onFilterChanged = [this] { updateFilterButton(); saveSettings(); };
    controlServer->start();
    updateFilterButton();
    
    // Populate device lists, then restore last session (and auto-start if configured)
    updateDeviceList();
    restoreSettings();
    
    // Start timer for status updates (every 100ms)
    startTimer (100);
}

MainComponent::~MainComponent()
{
    stopTimer();
    controlServer.reset();
    if (isProcessing)
        stopProcessing();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (20);
    
    // Title
    titleLabel.setBounds (area.removeFromTop (40));
    area.removeFromTop (10);
    
    // Input Device
    auto inputRow = area.removeFromTop (30);
    inputDeviceLabel.setBounds (inputRow.removeFromLeft (120));
    inputDeviceCombo.setBounds (inputRow);
    area.removeFromTop (10);
    
    // Output Device
    auto outputRow = area.removeFromTop (30);
    outputDeviceLabel.setBounds (outputRow.removeFromLeft (120));
    outputDeviceCombo.setBounds (outputRow);
    area.removeFromTop (10);
    
    // Censor Mode
    auto censorRow = area.removeFromTop (30);
    censorModeLabel.setBounds (censorRow.removeFromLeft (120));
    censorModeCombo.setBounds (censorRow.removeFromLeft (200));
    area.removeFromTop (20);
    
    // Start/Stop Button
    auto buttonRow = area.removeFromTop (50).reduced (60, 0);
    startStopButton.setBounds (buttonRow.removeFromLeft (buttonRow.getWidth() / 2 - 10));
    buttonRow.removeFromLeft (20);
    filterToggleButton.setBounds (buttonRow);
    area.removeFromTop (30);
    
    // Status
    auto statusRow = area.removeFromTop (30);
    statusLabel.setBounds (statusRow.removeFromLeft (120));
    statusValueLabel.setBounds (statusRow);
    area.removeFromTop (10);
    
    // Latency
    auto latencyRow = area.removeFromTop (30);
    latencyLabel.setBounds (latencyRow.removeFromLeft (120));
    latencyValueLabel.setBounds (latencyRow);
    area.removeFromTop (10);
    
    // Phase 1: Level meter
    auto levelRow = area.removeFromTop (30);
    levelLabel.setBounds (levelRow.removeFromLeft (120));
    levelValueLabel.setBounds (levelRow);
    area.removeFromTop (20);
    
    // Actual lyrics display (large, prominent - shows correct lyrics)
    actualLyricsLabel.setBounds (area.removeFromTop (25));
    actualLyricsDisplay.setBounds (area.removeFromTop (90));   // Current line + next line
    area.removeFromTop (5);
    
    // Live lyrics display (shows what Whisper heard)
    liveLyricsLabel.setBounds (area.removeFromTop (25));
    liveLyricsDisplay.setBounds (area.removeFromTop (40));
    area.removeFromTop (10);
    
    // Song recognition display
    songInfoLabel.setBounds (area.removeFromTop (25));
    auto songRow = area.removeFromTop (50);
    geniusKeyButton.setBounds (songRow.removeFromRight (120).reduced (0, 8));
    songRow.removeFromRight (6);
    pasteLyricsButton.setBounds (songRow.removeFromRight (110).reduced (0, 8));
    songRow.removeFromRight (10);
    songInfoDisplay.setBounds (songRow);
    area.removeFromTop (10);
    
    // Debug displays - split remaining area
    auto debugArea = area;
    auto leftColumn = debugArea.removeFromLeft (debugArea.getWidth() / 2 - 5);
    debugArea.removeFromLeft (10);
    
    // Transcript (left)
    transcriptLabel.setBounds (leftColumn.removeFromTop (25));
    transcriptDisplay.setBounds (leftColumn);
    
    // DSP Debug (right)
    dspDebugLabel.setBounds (debugArea.removeFromTop (25));
    auto dspArea = debugArea;
    auto debugControls = dspArea.removeFromBottom (30);
    dspDebugDisplay.setBounds (dspArea.removeFromTop (dspArea.getHeight()));
    
    // Debug controls at bottom
    showRawJsonToggle.setBounds (debugControls.removeFromLeft (150));
    debugControls.removeFromLeft (10);
    exportLogButton.setBounds (debugControls.removeFromLeft (150));
    debugControls.removeFromLeft (10);
    testingModeToggle.setBounds (debugControls.removeFromLeft (200));
}

void MainComponent::timerCallback()
{
    if (isProcessing)
    {
        updateLatencyDisplay();
        updateFilterButton();
        updateLyricsStatus();
        
        // Phase 1: Update level meter
        float level = audioEngine->getCurrentInputLevel();
        levelValueLabel.setText (juce::String (level, 3), juce::dontSendNotification);
        
        // Color code: green if detecting audio (>0.01), grey if silent
        if (level > 0.01f)
            levelValueLabel.setColour (juce::Label::textColourId, juce::Colours::green);
        else
            levelValueLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    }
}

void MainComponent::startProcessing()
{
    // Get selected devices
    auto inputDeviceName = inputDeviceCombo.getText();
    auto outputDeviceName = outputDeviceCombo.getText();
    
    if (inputDeviceName.isEmpty() || outputDeviceName.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                "Device Selection",
                                                "Please select both input and output devices.");
        return;
    }
    
    // Get censor mode
    auto censorMode = (censorModeCombo.getSelectedId() == 1) 
        ? AudioEngine::CensorMode::Reverse 
        : AudioEngine::CensorMode::Mute;
    
    // Debug log
    juce::File logFile = juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
        .getChildFile("ExplicitlyStartup.log");
    logFile.appendText("Starting audio engine...\n");
    logFile.appendText("Input: " + inputDeviceName + "\n");
    logFile.appendText("Output: " + outputDeviceName + "\n");
    
    // Start audio engine
    bool startSuccess = false;
    try
    {
        startSuccess = audioEngine->start (inputDeviceName, outputDeviceName, censorMode);
        logFile.appendText("Audio engine start returned: " + juce::String(startSuccess ? "true" : "false") + "\n");
    }
    catch (const std::exception& e)
    {
        logFile.appendText("EXCEPTION during start: " + juce::String(e.what()) + "\n");
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                "Fatal Error",
                                                "Exception: " + juce::String(e.what()));
        return;
    }
    
    logFile.appendText("About to check startSuccess...\n");
    
    if (startSuccess)
    {
        logFile.appendText("Start successful, updating UI...\n");
        isProcessing = true;
        logFile.appendText("Set isProcessing = true\n");
        startStopButton.setButtonText ("Stop Processing");
        logFile.appendText("Updated button text\n");
        statusValueLabel.setText ("Processing", juce::dontSendNotification);
        logFile.appendText("Updated status text\n");
        statusValueLabel.setColour (juce::Label::textColourId, juce::Colours::green);
        logFile.appendText("Updated status color\n");
        
        saveSettings();
        
        // Disable device selection during processing
        inputDeviceCombo.setEnabled (false);
        outputDeviceCombo.setEnabled (false);
        censorModeCombo.setEnabled (false);
    }
    else
    {
        juce::String errorMsg = "Failed to start audio processing.\n\n";
        errorMsg += "Selected devices:\n";
        errorMsg += "Input: " + inputDeviceName + "\n";
        errorMsg += "Output: " + outputDeviceName + "\n\n";
        errorMsg += "Error: " + audioEngine->getLastError();
        
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                "Audio Engine Error",
                                                errorMsg);
    }
}

void MainComponent::stopProcessing()
{
    audioEngine->stop();
    
    isProcessing = false;
    startStopButton.setButtonText ("Start Processing");
    statusValueLabel.setText ("Idle", juce::dontSendNotification);
    levelValueLabel.setColour (juce::Label::textColourId, getLookAndFeel().findColour (juce::Label::textColourId));
    
    // Clear live lyrics
    recentLyrics.clear();
    liveLyricsDisplay.setText("", juce::dontSendNotification);
    
    // Clear actual lyrics
    recentActualLyrics.clear();
    actualLyricsDisplay.setText("", juce::dontSendNotification);
    
    // Reset song info
    songInfoDisplay.setText("Pending...", juce::dontSendNotification);
    songInfoDisplay.setColour(juce::Label::textColourId, juce::Colours::yellow);
    
    // Re-enable device selection
    inputDeviceCombo.setEnabled (true);
    outputDeviceCombo.setEnabled (true);
    censorModeCombo.setEnabled (true);
}

void MainComponent::restoreSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "ExplicitlyDesktop";
    options.filenameSuffix = ".settings";
    options.folderName = "ExplicitlyDesktop";
    options.osxLibrarySubFolder = "Application Support";
    appProperties.setStorageParameters (options);
    
    auto* settings = appProperties.getUserSettings();
    
    auto selectByText = [] (juce::ComboBox& combo, const juce::String& text)
    {
        for (int i = 0; i < combo.getNumItems(); ++i)
        {
            if (combo.getItemText (i) == text)
            {
                combo.setSelectedItemIndex (i, juce::dontSendNotification);
                return true;
            }
        }
        return false;
    };
    
    bool haveInput = selectByText (inputDeviceCombo, settings->getValue ("inputDevice"));
    bool haveOutput = selectByText (outputDeviceCombo, settings->getValue ("outputDevice"));
    censorModeCombo.setSelectedId (settings->getIntValue ("censorMode", 2), juce::dontSendNotification);
    audioEngine->setFilterEnabled (settings->getBoolValue ("filterEnabled", true));
    updateFilterButton();
    
    // Apps routed to VB-CABLE are silent unless we're processing, so start
    // automatically once devices have been chosen and started successfully before
    if (haveInput && haveOutput && settings->getBoolValue ("autoStart", false))
        juce::MessageManager::callAsync ([this] { if (! isProcessing) startProcessing(); });
}

void MainComponent::saveSettings()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr)
        return;
    
    settings->setValue ("inputDevice", inputDeviceCombo.getText());
    settings->setValue ("outputDevice", outputDeviceCombo.getText());
    settings->setValue ("censorMode", censorModeCombo.getSelectedId());
    settings->setValue ("filterEnabled", audioEngine->isFilterEnabled());
    if (isProcessing)
        settings->setValue ("autoStart", true);
    settings->saveIfNeeded();
}

void MainComponent::showGeniusKey()
{
    geniusKeyWindow = std::make_unique<juce::AlertWindow> ("Genius key",
        "Paste your Genius API client access token. It is saved on this PC only "
        "(%APPDATA%\\ExplicitlyDesktop\\genius_token.txt) and used to look up lyrics.",
        juce::AlertWindow::NoIcon);
    geniusKeyWindow->addTextEditor ("token", {}, "Access token", true);   // Shown as dots
    geniusKeyWindow->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    geniusKeyWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    geniusKeyWindow->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int result)
    {
        if (result == 1 && geniusKeyWindow != nullptr)
        {
            auto token = geniusKeyWindow->getTextEditorContents ("token").trim();
            if (token.isNotEmpty())
            {
                audioEngine->setGeniusToken (token.toStdString());
                geniusKeyButton.setButtonText ("Genius key: set");
            }
        }
        juce::MessageManager::callAsync ([this] { geniusKeyWindow.reset(); });
    }), false);
}

void MainComponent::showPasteLyrics()
{
    auto status = audioEngine->getLyricsStatus();
    if (status.song.empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon, "Paste lyrics",
                                                "Play the song first, so the lyrics can be saved for it.");
        return;
    }

    pasteLyricsEditor = std::make_unique<juce::TextEditor>();
    pasteLyricsEditor->setMultiLine (true, true);
    pasteLyricsEditor->setReturnKeyStartsNewLine (true);
    pasteLyricsEditor->setScrollbarsShown (true);
    pasteLyricsEditor->setSize (520, 320);

    pasteLyricsWindow = std::make_unique<juce::AlertWindow> ("Paste lyrics",
        "Lyrics for " + juce::String (status.song) + ". They are kept for this song "
        "and replace any lyrics found online.", juce::AlertWindow::NoIcon);
    pasteLyricsWindow->addCustomComponent (pasteLyricsEditor.get());
    pasteLyricsWindow->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::ctrlModifier, 0));
    pasteLyricsWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    pasteLyricsWindow->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int result)
    {
        if (result == 1 && pasteLyricsEditor != nullptr)
            audioEngine->setManualLyricsForCurrentSong (pasteLyricsEditor->getText().toStdString());
        if (pasteLyricsWindow != nullptr)
            pasteLyricsWindow->removeCustomComponent (0);
        juce::MessageManager::callAsync ([this] { pasteLyricsWindow.reset(); pasteLyricsEditor.reset(); });
    }), false);
}

void MainComponent::updateLyricsStatus()
{
    auto status = audioEngine->getLyricsStatus();
    if (status.song.empty())
        return;
    
    juce::String text = juce::String (status.song) + "   |   Lyrics: ";
    juce::Colour colour = juce::Colours::yellow;
    
    if (status.searching)
        text << "looking up...";
    else if (! status.hasLyrics)
        text << "none found (Whisper only)" << (status.geniusKeyRejected ? "  -  Genius rejected the key" : "");
    else if (status.following)
    {
        text << "following (" << juce::String (status.source) << ")";
        colour = juce::Colours::lightgreen;
    }
    else
        text << "loaded, listening for a match (" << juce::String (status.source) << ")";
    
    if (songInfoDisplay.getText() != text)
    {
        songInfoDisplay.setText (text, juce::dontSendNotification);
        songInfoDisplay.setColour (juce::Label::textColourId, colour);
    }
}

void MainComponent::updateFilterButton()
{
    auto status = audioEngine->getFilterStatus();
    
    juce::String text = "Filter Explicit Speech: ";
    if (! status.enabled)
        text << "OFF (no delay)";
    else if (status.running && status.buffering)
        text << "ON (buffering...)";
    else
        text << "ON";
    
    filterToggleButton.setButtonText (text);
    filterToggleButton.setColour (juce::TextButton::buttonColourId,
                                  status.enabled ? juce::Colours::darkgreen : juce::Colours::darkred);
}

void MainComponent::updateLatencyDisplay()
{
    // Display buffer size (grows over time as processing accumulates)
    double bufferSize = audioEngine->getCurrentBufferSize();
    bool isUnderrun = audioEngine->isBufferUnderrun();
    
    // Debug: Log every 50 calls (every 5 seconds @ 100ms timer)
    static int callCount = 0;
    if (++callCount % 50 == 0)
    {
        std::cout << "[UI] Buffer display update: " << bufferSize << "s, underrun=" 
                  << (isUnderrun ? "YES" : "NO") << std::endl;
    }
    
    if (bufferSize >= 0.0)
    {
        // Show warning if buffer is critically low
        if (isUnderrun)
        {
            latencyValueLabel.setText ("⚠ UNDERRUN - UNCENSORED", juce::dontSendNotification);
            latencyValueLabel.setColour (juce::Label::textColourId, juce::Colours::red);
        }
        else
        {
            // Show buffer capacity in seconds
            latencyValueLabel.setText (juce::String (bufferSize, 2) + " s buffer", juce::dontSendNotification);
            
            // Color code: green 14-17s (healthy), yellow 12-14s or 17-19s (borderline), red <12s or >19s (critical)
            if (bufferSize >= 14.0 && bufferSize <= 17.0)
                latencyValueLabel.setColour (juce::Label::textColourId, juce::Colours::green);
            else if (bufferSize >= 12.0 && bufferSize < 19.0)
                latencyValueLabel.setColour (juce::Label::textColourId, juce::Colours::yellow);
            else
                latencyValueLabel.setColour (juce::Label::textColourId, juce::Colours::red);
        }
    }
}

void MainComponent::updateDeviceList()
{
    // Create a temporary device manager just for enumeration
    juce::AudioDeviceManager tempDeviceManager;
    tempDeviceManager.initialiseWithDefaultDevices (0, 2);
    
    inputDeviceCombo.clear();
    outputDeviceCombo.clear();
    
    // Get the current device type
    juce::AudioIODeviceType* currentType = tempDeviceManager.getCurrentDeviceTypeObject();
    if (currentType == nullptr)
        return;
    
    // Get input device names
    juce::StringArray inputNames = currentType->getDeviceNames (true);
    int numInputs = inputNames.size();
    for (int i = 0; i < numInputs; ++i)
        inputDeviceCombo.addItem (inputNames[i], i + 1);
    
    // Get output device names
    juce::StringArray outputNames = currentType->getDeviceNames (false);
    int numOutputs = outputNames.size();
    for (int i = 0; i < numOutputs; ++i)
        outputDeviceCombo.addItem (outputNames[i], i + 1);
    
    // Try to select VB-Cable for input and speakers for output by default
    int vbCableInputIndex = -1;
    int speakersOutputIndex = -1;
    
    // Find VB-Cable for input (captures audio from apps)
    for (int i = 0; i < inputNames.size(); ++i)
    {
        if (inputNames[i].containsIgnoreCase("VB-Audio Virtual Cable") || 
            inputNames[i].containsIgnoreCase("CABLE Output"))
        {
            vbCableInputIndex = i;
            break;
        }
    }
    
    // Find speakers for output (plays filtered audio)
    for (int i = 0; i < outputNames.size(); ++i)
    {
        if (outputNames[i].containsIgnoreCase("Speakers") || 
            outputNames[i].containsIgnoreCase("Speaker") ||
            outputNames[i].containsIgnoreCase("Headphones") ||
            outputNames[i].containsIgnoreCase("Realtek"))
        {
            speakersOutputIndex = i;
            break;
        }
    }
    
    // Select input device (prefer VB-Cable)
    if (vbCableInputIndex >= 0)
        inputDeviceCombo.setSelectedItemIndex(vbCableInputIndex);
    else if (inputDeviceCombo.getNumItems() > 0)
        inputDeviceCombo.setSelectedItemIndex(0);
    
    // Select output device (prefer speakers)
    if (speakersOutputIndex >= 0)
        outputDeviceCombo.setSelectedItemIndex(speakersOutputIndex);
    else if (outputDeviceCombo.getNumItems() > 0)
        outputDeviceCombo.setSelectedItemIndex(0);
}

void MainComponent::addDebugMessage(const juce::String& message, bool isProfanity)
{
    // Timestamp
    auto now = juce::Time::getCurrentTime();
    juce::String timestamp = now.formatted ("[%H:%M:%S.%3") + "] ";
    
    // Build message
    juce::String fullMessage = timestamp + message + "\n";
    
    // Add to log
    debugLog += fullMessage;
    
    // Determine which display to update
    if (message.startsWith("[DSP]") || message.startsWith("Applied"))
    {
        // DSP debug messages
        dspDebugDisplay.moveCaretToEnd();
        dspDebugDisplay.insertTextAtCaret (fullMessage);
        dspDebugDisplay.moveCaretToEnd();
    }
    else
    {
        // ASR transcript messages
        if (isProfanity)
        {
            // Color profanity words in red (simplified - just mark them)
            transcriptDisplay.moveCaretToEnd();
            transcriptDisplay.insertTextAtCaret ("*** ");
            transcriptDisplay.insertTextAtCaret (fullMessage);
            transcriptDisplay.insertTextAtCaret (" ***");
        }
        else
        {
            transcriptDisplay.moveCaretToEnd();
            transcriptDisplay.insertTextAtCaret (fullMessage);
        }
        transcriptDisplay.moveCaretToEnd();
    }
}

void MainComponent::exportDebugLog()
{
    auto now = juce::Time::getCurrentTime();
    juce::String filename = "ExplicitlyDebug_" + now.formatted("%Y%m%d_%H%M%S") + ".txt";
    
    juce::File desktop = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
    juce::File logFile = desktop.getChildFile (filename);
    
    if (logFile.replaceWithText (debugLog))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                "Export Successful",
                                                "Debug log saved to:\n" + logFile.getFullPathName());
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                "Export Failed",
                                                "Could not write to file.");
    }
}

void MainComponent::updateLiveLyrics(const juce::String& words)
{
    // Keep last ~10 words visible (Whisper transcription)
    recentLyrics += " " + words;
    
    // Trim to last 10 words
    juce::StringArray wordArray;
    wordArray.addTokens(recentLyrics, " ", "");
    
    if (wordArray.size() > 10)
    {
        int startIndex = wordArray.size() - 10;
        juce::String trimmed;
        for (int i = startIndex; i < wordArray.size(); ++i)
        {
            if (i > startIndex)
                trimmed += " ";
            trimmed += wordArray[i];
        }
        recentLyrics = trimmed;
    }
    
    // Update display
    liveLyricsDisplay.setText(recentLyrics, juce::dontSendNotification);
}

void MainComponent::updateActualLyrics(const juce::String& words)
{
    // The engine sends the lyrics line(s) being sung right now (or Whisper's
    // words when the song has no lyrics), so replace rather than append
    recentActualLyrics = words;
    actualLyricsDisplay.setText(recentActualLyrics, juce::dontSendNotification);
    actualLyricsDisplay.setColour(juce::Label::textColourId, juce::Colours::white);
    actualLyricsDisplay.setColour(juce::Label::backgroundColourId, juce::Colours::black);
}
