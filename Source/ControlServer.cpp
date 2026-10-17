/*
  ==============================================================================

    ControlServer.cpp

  ==============================================================================
*/

#include "ControlServer.h"
#include <juce_events/juce_events.h>
#include <iostream>

ControlServer::ControlServer (AudioEngine& engine, RemoteSessions& sessions, PcmTap& tap, int p)
    : audioEngine (engine), remoteSessions (sessions), pcmTap (tap), port (p),
      server ("ControlServer", maxConnections,
              [this] (juce::StreamingSocket& client, const std::atomic<bool>& stopping)
              {
                  handleConnection (client, stopping);
              })
{
}

ControlServer::~ControlServer()
{
    stop();
}

bool ControlServer::start()
{
    // Loopback only: nothing outside this PC can reach the toggle
    if (! server.start (port, "127.0.0.1"))
    {
        std::cout << "[Control] Could not listen on 127.0.0.1:" << port << std::endl;
        return false;
    }

    std::cout << "[Control] Listening on http://127.0.0.1:" << port << std::endl;
    return true;
}

void ControlServer::stop()
{
    server.stop();
}

juce::String ControlServer::statusJson() const
{
    auto status = audioEngine.getFilterStatus();
    auto phoneState = remoteSessions.getState();

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("running", status.running);
    obj->setProperty ("enabled", status.enabled);
    obj->setProperty ("buffering", status.buffering);
    obj->setProperty ("bypassed", status.bypassed);
    obj->setProperty ("delayMs", juce::roundToInt (status.delaySeconds * 1000.0));
    obj->setProperty ("held", status.held);
    obj->setProperty ("localMuted", status.localMuted);
    obj->setProperty ("sampleRate", status.sampleRate);

    auto* phone = new juce::DynamicObject();
    phone->setProperty ("active", phoneState.active);
    phone->setProperty ("deviceName", juce::String::fromUTF8 (phoneState.deviceName.c_str()));
    obj->setProperty ("phone", juce::var (phone));

    return juce::JSON::toString (juce::var (obj), true);
}

juce::String ControlServer::remoteNext (const std::atomic<bool>& stopping)
{
    RemoteSessions::Message m;
    auto* obj = new juce::DynamicObject();

    if (! remoteSessions.waitForMessage (m, 25000, &stopping))
    {
        obj->setProperty ("type", "none");
    }
    else
    {
        auto str = [] (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); };
        obj->setProperty ("type", str (m.type));
        obj->setProperty ("sessionId", str (m.sessionId));

        if (m.type == "offer")
        {
            obj->setProperty ("deviceId", str (m.deviceId));
            obj->setProperty ("deviceName", str (m.deviceName));
            obj->setProperty ("sdp", str (m.sdp));
        }
        else
        {
            obj->setProperty ("reason", str (m.reason));
        }
    }

    return juce::JSON::toString (juce::var (obj), true);
}

//==============================================================================
void ControlServer::servePcm (juce::StreamingSocket& client, const std::atomic<bool>& stopping)
{
    int generation;
    {
        std::lock_guard<std::mutex> sl (pcmConsumerLock);
        generation = ++pcmGeneration;   // An older client's loop sees this and ends
        pcmTap.startClient();
        pcmConnected = true;
    }
    std::cout << "[Control] PCM client connected" << std::endl;

    std::vector<uint8_t> frame;
    std::string incoming;
    std::vector<Http::WebSocket::Frame> frames;
    char buffer[2048];
    bool open = true;
    bool replaced = false;

    while (open && ! stopping.load())
    {
        // Send whatever full frames are waiting
        bool sentAny = false;
        for (;;)
        {
            {
                std::lock_guard<std::mutex> sl (pcmConsumerLock);
                if (pcmGeneration.load() != generation)
                {
                    replaced = true;
                    break;
                }
                if (! pcmTap.popFrame (frame))
                    break;
            }

            if (! Http::WebSocket::sendFrame (client, Http::WebSocket::binary, frame.data(), frame.size()))
            {
                open = false;
                break;
            }
            sentAny = true;
        }

        if (replaced || ! open)
            break;

        // Client frames: close and ping. The 5 ms wait paces the loop when idle.
        int ready = client.waitUntilReady (true, sentAny ? 0 : 5);
        if (ready < 0)
            break;
        if (ready == 0)
            continue;

        int n = client.read (buffer, (int) sizeof (buffer), false);
        if (n <= 0)
            break;
        incoming.append (buffer, (size_t) n);

        frames.clear();
        if (! Http::WebSocket::parseFrames (incoming, frames))
        {
            const char code[] = { 0x03, (char) 0xEA };   // 1002 protocol error
            Http::WebSocket::sendFrame (client, Http::WebSocket::close, code, 2);
            break;
        }

        for (auto& f : frames)
        {
            if (f.opcode == Http::WebSocket::close)
            {
                Http::WebSocket::sendFrame (client, Http::WebSocket::close, f.payload.data(), juce::jmin<size_t> (2, f.payload.size()));
                open = false;
                break;
            }
            if (f.opcode == Http::WebSocket::ping)
                Http::WebSocket::sendFrame (client, Http::WebSocket::pong, f.payload.data(), f.payload.size());
            // Text, binary and pong frames from the client are ignored
        }
    }

    if (replaced || stopping.load())
    {
        const char code[] = { 0x03, (char) 0xE9 };   // 1001 going away
        Http::WebSocket::sendFrame (client, Http::WebSocket::close, code, 2);
    }

    {
        std::lock_guard<std::mutex> sl (pcmConsumerLock);
        if (pcmGeneration.load() == generation)
        {
            pcmTap.stopClient();
            pcmConnected = false;
        }
    }
    std::cout << "[Control] PCM client " << (replaced ? "replaced" : "disconnected") << std::endl;
}

//==============================================================================
void ControlServer::handleConnection (juce::StreamingSocket& client, const std::atomic<bool>& stopping)
{
    Http::Request request;
    auto readResult = Http::readRequest (client, request, 256 * 1024, 5000, stopping);

    if (readResult == Http::ReadResult::closed)
        return;
    if (readResult != Http::ReadResult::ok)
    {
        Http::send (client, Http::jsonError (readResult == Http::ReadResult::tooLarge ? 413 : 400,
                                             readResult == Http::ReadResult::tooLarge ? "too large" : "bad request"));
        return;
    }

    const auto& method = request.method;
    const auto& path = request.path;

    // Only the browser extension (or local tools with no Origin) may talk to us.
    // Web pages send an http(s) Origin; refuse them so a site can't flip the filter.
    if (request.header ("origin").startsWithIgnoreCase ("http"))
    {
        Http::send (client, Http::jsonError (403, "forbidden"));
        return;
    }

    if (method == "GET" && path == "/pcm")
    {
        if (! Http::WebSocket::isUpgradeRequest (request))
        {
            Http::send (client, Http::jsonError (426, "websocket upgrade required"));
            return;
        }
        if (Http::WebSocket::acceptUpgrade (client, request))
            servePcm (client, stopping);
        return;
    }

    int code = 200;
    juce::String body;

    if (method == "GET" && path == "/status")
    {
        body = statusJson();
    }
    else if (method == "GET" && path == "/lexicon")
    {
        // The profanity word list, so the browser extension matches the same words
        juce::Array<juce::var> list;
        for (auto& w : audioEngine.getProfanityFilter().words())
            list.add (juce::String (w));
        body = juce::JSON::toString (juce::var (list), true);
    }
    else if (method == "POST" && path == "/bypass")
    {
        // Extension is muting a captioned video itself: pass audio through for 3 s
        audioEngine.setBypassLease (3.0);
        body = statusJson();
    }
    else if (method == "POST" && path == "/debug/dump")
    {
        body = audioEngine.dumpDebugRecording() ? "{\"dumped\":true}" : "{\"dumped\":false}";
    }
    else if (method == "POST" && path.startsWith ("/filter/"))
    {
        auto action = path.fromLastOccurrenceOf ("/", false, false);

        if (action == "on")           audioEngine.setFilterEnabled (true);
        else if (action == "off")     audioEngine.setFilterEnabled (false);
        else if (action == "toggle")  audioEngine.setFilterEnabled (! audioEngine.isFilterEnabled());
        else                          code = 404;

        if (code == 200)
        {
            std::weak_ptr<bool> weakAlive = alive;
            juce::MessageManager::callAsync ([this, weakAlive]
            {
                if (weakAlive.lock() != nullptr && onFilterChanged)
                    onFilterChanged();
            });
            body = statusJson();
        }
    }
    else if (method == "POST" && path == "/flush")
    {
        // After a seek: drop the delayed audio so the old 3.5 s isn't played
        audioEngine.requestFlush();
        body = statusJson();
    }
    else if (method == "POST" && (path == "/hold/on" || path == "/hold/off"))
    {
        audioEngine.setHeld (path == "/hold/on");
        body = statusJson();
    }
    else if (method == "GET" && path == "/remote/next")
    {
        body = remoteNext (stopping);
    }
    else if (method == "POST" && path == "/remote/answer")
    {
        auto json = Http::parseJsonBody (request.body);
        auto sessionId = json.getProperty ("sessionId", {}).toString().toStdString();
        auto sdp = json.getProperty ("sdp", {}).toString().toStdString();
        auto error = json.getProperty ("error", {}).toString().toStdString();

        if (! json.isObject() || sessionId.empty())
            code = 400;
        else if (! remoteSessions.deliverAnswer (sessionId, sdp, error))
        {
            code = 404;   // That offer was replaced, timed out or already answered
            body = "{\"error\":\"unknown session\"}";
        }
        else
            body = "{}";
    }
    else if (method == "POST" && path == "/remote/state")
    {
        auto json = Http::parseJsonBody (request.body);
        if (! json.isObject())
        {
            code = 400;
        }
        else
        {
            remoteSessions.setState ((bool) json.getProperty ("active", false),
                                     json.getProperty ("sessionId", {}).toString().toStdString(),
                                     json.getProperty ("deviceName", {}).toString().toStdString());
            body = "{}";
        }
    }
    else
    {
        code = 404;
    }

    if (code == 404 && body.isEmpty())
        body = "{\"error\":\"not found\"}";
    else if (code == 400)
        body = "{\"error\":\"bad request\"}";

    Http::send (client, Http::json (code, body));
}
