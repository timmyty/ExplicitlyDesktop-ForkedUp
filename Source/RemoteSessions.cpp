/*
  ==============================================================================

    RemoteSessions.cpp

  ==============================================================================
*/

#include "RemoteSessions.h"
#include "StreamCrypto.h"

#include <algorithm>
#include <chrono>

int64_t RemoteSessions::nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
}

void RemoteSessions::queueLocked (Message m)
{
    // Nobody polling for a while: keep the newest messages only
    while (queue.size() >= maxQueuedMessages)
        queue.pop_front();

    queue.push_back (std::move (m));
    messageAvailable.notify_one();
}

void RemoteSessions::endCurrentLocked (const std::string& reason)
{
    if (currentSessionId.empty())
        return;

    Message m;
    m.type = "hangup";
    m.sessionId = currentSessionId;
    m.reason = reason;
    queueLocked (std::move (m));

    if (state.sessionId == currentSessionId)
        state.active = false;   // Speakers back on now, not when the extension reports

    currentSessionId.clear();
    currentDeviceId.clear();
    currentDeviceName.clear();
    answerAvailable.notify_all();   // A waiting offer sees it was replaced
}

//==============================================================================
RemoteSessions::OfferResult RemoteSessions::submitOffer (const std::string& deviceId, const std::string& deviceName,
                                                         const std::string& sdp, std::string& sessionIdOut,
                                                         std::string& answerSdpOut, std::string& errorOut,
                                                         int timeoutMs)
{
    std::unique_lock<std::mutex> sl (lock);

    if (stopping)
        return OfferResult::shuttingDown;

    const int64_t now = nowMs();
    if (pollersWaiting == 0 && (lastPollMs < 0 || now - lastPollMs > pollFreshMs))
        return OfferResult::unavailable;

    endCurrentLocked ("replaced");

    const std::string sessionId = "s-" + StreamCrypto::toHex (StreamCrypto::randomBytes (12));
    currentSessionId = sessionId;
    currentDeviceId = deviceId;
    currentDeviceName = deviceName;
    answered = false;
    answerSdp.clear();
    answerError.clear();

    Message m;
    m.type = "offer";
    m.sessionId = sessionId;
    m.deviceId = deviceId;
    m.deviceName = deviceName;
    m.sdp = sdp;
    queueLocked (std::move (m));

    sessionIdOut = sessionId;
    answerAvailable.wait_for (sl, std::chrono::milliseconds (timeoutMs), [&]
    {
        return stopping || currentSessionId != sessionId || answered;
    });

    if (stopping)
        return OfferResult::shuttingDown;

    if (currentSessionId != sessionId)
        return OfferResult::replaced;

    if (! answered)
    {
        endCurrentLocked ("timeout");
        return OfferResult::timedOut;
    }

    if (! answerError.empty())
    {
        errorOut = answerError;
        currentSessionId.clear();
        currentDeviceId.clear();
        currentDeviceName.clear();
        return OfferResult::failed;
    }

    answerSdpOut = answerSdp;
    return OfferResult::answered;
}

bool RemoteSessions::hangup (const std::string& deviceId, const std::string& sessionId, const std::string& reason)
{
    std::lock_guard<std::mutex> sl (lock);
    if (currentSessionId.empty() || currentSessionId != sessionId || currentDeviceId != deviceId)
        return false;

    endCurrentLocked (reason);
    return true;
}

void RemoteSessions::endSessionsForDevice (const std::string& deviceId, const std::string& reason)
{
    std::lock_guard<std::mutex> sl (lock);
    if (! currentSessionId.empty() && currentDeviceId == deviceId)
        endCurrentLocked (reason);
}

void RemoteSessions::endAllSessions (const std::string& reason)
{
    std::lock_guard<std::mutex> sl (lock);
    endCurrentLocked (reason);
}

bool RemoteSessions::isExtensionAvailable() const
{
    std::lock_guard<std::mutex> sl (lock);
    return pollersWaiting > 0 || (lastPollMs >= 0 && nowMs() - lastPollMs <= pollFreshMs);
}

//==============================================================================
bool RemoteSessions::waitForMessage (Message& out, int timeoutMs, const std::atomic<bool>* cancel)
{
    std::unique_lock<std::mutex> sl (lock);
    ++pollersWaiting;
    lastPollMs = nowMs();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
    bool got = false;

    for (;;)
    {
        if (stopping || (cancel != nullptr && cancel->load()))
            break;

        if (! queue.empty())
        {
            out = std::move (queue.front());
            queue.pop_front();
            got = true;
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;

        // Short slices so a server shutting down (cancel) isn't kept waiting
        auto slice = std::min<std::chrono::steady_clock::duration> (deadline - now, std::chrono::milliseconds (250));
        messageAvailable.wait_for (sl, slice);
    }

    --pollersWaiting;
    lastPollMs = nowMs();
    return got;
}

bool RemoteSessions::deliverAnswer (const std::string& sessionId, const std::string& sdp, const std::string& error)
{
    std::lock_guard<std::mutex> sl (lock);
    if (sessionId.empty() || sessionId != currentSessionId || answered)
        return false;

    answered = true;
    answerSdp = sdp;
    answerError = (sdp.empty() && error.empty()) ? std::string ("empty answer") : error;
    answerAvailable.notify_all();
    return true;
}

void RemoteSessions::setState (bool active, const std::string& sessionId, const std::string& deviceName)
{
    std::lock_guard<std::mutex> sl (lock);

    if (active)
    {
        if (sessionId.empty() || sessionId != currentSessionId)
            return;   // A session we already ended (or never started)

        state.active = true;
        state.sessionId = sessionId;
        state.deviceName = deviceName.empty() ? currentDeviceName : deviceName;
    }
    else if (sessionId.empty() || sessionId == state.sessionId)
    {
        state.active = false;
    }
}

RemoteSessions::State RemoteSessions::getState() const
{
    std::lock_guard<std::mutex> sl (lock);
    return state;
}

std::string RemoteSessions::getCurrentDeviceId() const
{
    std::lock_guard<std::mutex> sl (lock);
    return currentDeviceId;
}

void RemoteSessions::shutdown()
{
    std::lock_guard<std::mutex> sl (lock);
    stopping = true;
    messageAvailable.notify_all();
    answerAvailable.notify_all();
}
