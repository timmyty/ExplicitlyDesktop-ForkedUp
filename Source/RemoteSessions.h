/*
  ==============================================================================

    RemoteSessions.h

    In-memory broker between the phone side (StreamHost, LAN) and the browser
    extension (ControlServer, loopback) for docs/STREAMING_PROTOCOL.md:

      - a queue of messages for the extension, handed out by GET /remote/next
      - the pending offer, waiting up to 15 s for POST /remote/answer
      - the current session (one at a time: a new offer replaces the old one
        and queues a hangup for it)
      - the session state the extension reports with POST /remote/state

    All methods are thread safe. No JUCE, so the unit tests build anywhere.

  ==============================================================================
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

class RemoteSessions
{
public:
    /** One message for the extension (GET /remote/next). */
    struct Message
    {
        std::string type;          // "offer" or "hangup"
        std::string sessionId;
        std::string deviceId;      // offer only
        std::string deviceName;    // offer only
        std::string sdp;           // offer only
        std::string reason;        // hangup only: "replaced", "revoked", "host closed", "hangup", "timeout"
    };

    struct State
    {
        bool active = false;       // As reported by the extension
        std::string sessionId;
        std::string deviceName;
    };

    enum class OfferResult
    {
        answered,      // answerSdp is set
        unavailable,   // No extension polling (409)
        timedOut,      // No answer within the timeout (504)
        failed,        // The extension answered with an error (502)
        replaced,      // Another offer took over before this one was answered (409)
        shuttingDown
    };

    RemoteSessions() = default;
    ~RemoteSessions() { shutdown(); }

    //==============================================================================
    // Phone side (StreamHost)

    /** Queues an offer and waits for the extension's answer. Ends any current
        session first (a hangup with reason "replaced" is queued for it). */
    OfferResult submitOffer (const std::string& deviceId, const std::string& deviceName,
                             const std::string& sdp, std::string& sessionIdOut,
                             std::string& answerSdpOut, std::string& errorOut,
                             int timeoutMs = 15000);

    /** Phone hung up. Only ends the session if it belongs to deviceId. */
    bool hangup (const std::string& deviceId, const std::string& sessionId, const std::string& reason = "hangup");

    /** Revoke or unpair: ends that device's session, if any. */
    void endSessionsForDevice (const std::string& deviceId, const std::string& reason);

    /** Ends whatever session runs (e.g. phone streaming turned off). */
    void endAllSessions (const std::string& reason);

    /** The extension counts as present while a /remote/next poll is waiting or
        one finished in the last 30 s. */
    bool isExtensionAvailable() const;

    //==============================================================================
    // Extension side (ControlServer)

    /** Long poll: waits up to timeoutMs for a message. False on timeout,
        on shutdown, or when cancel becomes true. */
    bool waitForMessage (Message& out, int timeoutMs, const std::atomic<bool>* cancel = nullptr);

    /** POST /remote/answer. False if no offer with that sessionId is waiting. */
    bool deliverAnswer (const std::string& sessionId, const std::string& sdp, const std::string& error);

    /** POST /remote/state. Inactive for another session than the current one
        is ignored (a late report from a replaced session). */
    void setState (bool active, const std::string& sessionId, const std::string& deviceName);

    State getState() const;

    /** Current session's device, empty if none. */
    std::string getCurrentDeviceId() const;

    /** Wakes every waiter; later calls return at once. */
    void shutdown();

    /** For tests: the clock used for "last poll" (ms, steady). */
    static int64_t nowMs();

    static constexpr size_t maxQueuedMessages = 32;
    static constexpr int64_t pollFreshMs = 30 * 1000;

private:
    void queueLocked (Message m);
    void endCurrentLocked (const std::string& reason);

    mutable std::mutex lock;
    std::condition_variable messageAvailable;   // For /remote/next
    std::condition_variable answerAvailable;    // For submitOffer

    std::deque<Message> queue;
    bool stopping = false;

    int pollersWaiting = 0;
    int64_t lastPollMs = -1;

    // Current session (the latest offer, answered or not)
    std::string currentSessionId, currentDeviceId, currentDeviceName;
    bool answered = false;
    std::string answerSdp, answerError;

    State state;
};
