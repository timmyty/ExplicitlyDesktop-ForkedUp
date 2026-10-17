/*
  ==============================================================================

    ControlServer.h

    Minimal HTTP server bound to 127.0.0.1 so the browser extension can read
    the Filter Explicit Speech state and current audio delay (to delay YouTube
    video by the same amount), and flip the toggle.

      GET  /status        -> {"running":..,"enabled":..,"buffering":..,"delayMs":..}
      POST /filter/on     -> turn filter on,  returns status
      POST /filter/off    -> turn filter off, returns status
      POST /filter/toggle -> flip filter,     returns status

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include "AudioEngine.h"

class ControlServer : private juce::Thread
{
public:
    static constexpr int defaultPort = 8766;

    ControlServer (AudioEngine& engine, int port = defaultPort);
    ~ControlServer() override;

    bool start();
    void stop();

    /** Called on the message thread after the toggle is changed remotely. */
    std::function<void()> onFilterChanged;

private:
    void run() override;
    void handleConnection (juce::StreamingSocket& client);
    juce::String statusJson() const;

    AudioEngine& audioEngine;
    int port;
    juce::StreamingSocket listener;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlServer)
};
