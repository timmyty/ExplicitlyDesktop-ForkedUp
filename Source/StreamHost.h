/*
  ==============================================================================

    StreamHost.h

    The LAN-facing server for phone streaming (0.0.0.0:8767), off unless the
    user turns on "Allow phone streaming". docs/STREAMING_PROTOCOL.md:

      GET  /, /index.html, /app.js, ...   the phone page (MobileClient/ next to the exe)
      POST /api/pair                      pairing with the QR/typed code and a Windows prompt
      POST /api/hello                     signed: check pairing still works
      POST /api/session/offer             signed: SDP offer, relayed to the extension
      POST /api/session/hangup            signed
      POST /api/unpair                    signed: forget this phone

    Nothing privileged is served here: filter controls stay on 127.0.0.1:8766.
    Each connection gets its own thread (at most 8).

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include "AudioEngine.h"
#include "HttpServerUtil.h"
#include "PairedDevices.h"
#include "RemoteSessions.h"

class StreamHost
{
public:
    static constexpr int defaultPort = 8767;
    static constexpr int maxConnections = 8;
    static constexpr int pairingSeconds = 120;

    StreamHost (AudioEngine& engine, PairedDevices& devices, RemoteSessions& sessions, int port = defaultPort);
    ~StreamHost();

    bool start();
    void stop();
    bool isRunning() const { return server.isListening(); }
    int getPort() const { return port; }

    //==============================================================================
    struct PairingCode
    {
        juce::String typedCode;   // B32(P) in groups of 4: "ABCD-EFGH-..."
        juce::String url;         // http://<lan-ip>:8767/#pair=<B32(P)>&host=<hostId>
        juce::Time expires;
    };

    /** A fresh pairing secret P (16 bytes, 120 s, single use), replacing any earlier one. */
    PairingCode beginPairing();
    void cancelPairing();

    /** Asks the user to allow a phone. Called on a server thread: show the
        prompt on the message thread and call decide(true/false) once, from
        any thread. If nobody answers within 120 s the phone is refused. */
    std::function<void (const juce::String& deviceName, std::function<void (bool)> decide)> onApprovalRequest;

    /** Called on a server thread after a phone pairs or unpairs. */
    std::function<void()> onDevicesChanged;

    /** The PC's LAN IPv4 address (192.168/16, then 10/8, then 172.16/12;
        adapters with a gateway first). Empty if there is none. */
    static juce::String findLanAddress();

    /** MobileClient/ next to the exe. */
    static juce::File getClientFolder();

private:
    void handleConnection (juce::StreamingSocket& client, const std::atomic<bool>& stopping);
    Http::Response serveStatic (const Http::Request& request);
    Http::Response handlePair (const Http::Request& request, const std::atomic<bool>& stopping);
    void handleSigned (juce::StreamingSocket& client, const Http::Request& request);

    AudioEngine& audioEngine;
    PairedDevices& pairedDevices;
    RemoteSessions& remoteSessions;
    int port;
    Http::SocketServer server;

    std::mutex pairingLock;
    StreamCrypto::Bytes pairingSecret;   // P, empty when there is none
    juce::int64 pairingExpiresMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamHost)
};
