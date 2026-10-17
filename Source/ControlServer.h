/*
  ==============================================================================

    ControlServer.h

    Minimal HTTP server bound to 127.0.0.1 so the browser extension can read
    the Filter Explicit Speech state and current audio delay (to delay YouTube
    video by the same amount), and flip the toggle.

      GET  /status        -> {"running":..,"enabled":..,"buffering":..,"delayMs":..,
                              "held":..,"localMuted":..,"phone":{..},"sampleRate":..}
      POST /filter/on     -> turn filter on,  returns status
      POST /filter/off    -> turn filter off, returns status
      POST /filter/toggle -> flip filter,     returns status

    Phone streaming (docs/STREAMING_PROTOCOL.md section 5):

      GET  /pcm            WebSocket: 20 ms frames of the filtered output
      POST /flush          drop the delayed audio after a seek, returns status
      POST /hold/on|off    freeze / resume the delay pipeline, returns status
      GET  /remote/next    long poll (25 s) for offers and hangups from phones
      POST /remote/answer  the extension's SDP answer (or error) for an offer
      POST /remote/state   the extension reports a session running or ended

    Requests with an http(s) Origin (web pages) get 403. Each connection is
    handled on its own thread (at most 8), so a long poll or the PCM stream
    doesn't hold up other requests.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include "AudioEngine.h"
#include "HttpServerUtil.h"
#include "PcmTap.h"
#include "RemoteSessions.h"

class ControlServer
{
public:
    static constexpr int defaultPort = 8766;
    static constexpr int maxConnections = 8;

    ControlServer (AudioEngine& engine, RemoteSessions& sessions, PcmTap& tap, int port = defaultPort);
    ~ControlServer();

    bool start();
    void stop();

    /** Called on the message thread after the toggle is changed remotely. */
    std::function<void()> onFilterChanged;

    /** True while the extension is receiving /pcm. */
    bool isPcmClientConnected() const { return pcmConnected.load(); }

private:
    void handleConnection (juce::StreamingSocket& client, const std::atomic<bool>& stopping);
    void servePcm (juce::StreamingSocket& client, const std::atomic<bool>& stopping);
    juce::String statusJson() const;
    juce::String remoteNext (const std::atomic<bool>& stopping);

    AudioEngine& audioEngine;
    RemoteSessions& remoteSessions;
    PcmTap& pcmTap;
    int port;
    Http::SocketServer server;

    // One /pcm client: a new one replaces the old (generation counter), and the
    // lock keeps the tap's consumer side to one thread at a time
    std::atomic<int> pcmGeneration { 0 };
    std::atomic<bool> pcmConnected { false };
    std::mutex pcmConsumerLock;

    std::shared_ptr<bool> alive = std::make_shared<bool> (true);   // Guards callAsync after deletion

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlServer)
};
