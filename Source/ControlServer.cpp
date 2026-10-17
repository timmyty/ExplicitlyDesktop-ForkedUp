/*
  ==============================================================================

    ControlServer.cpp

  ==============================================================================
*/

#include "ControlServer.h"
#include <juce_events/juce_events.h>
#include <iostream>

ControlServer::ControlServer (AudioEngine& engine, int p)
    : juce::Thread ("ControlServer"), audioEngine (engine), port (p)
{
}

ControlServer::~ControlServer()
{
    stop();
}

bool ControlServer::start()
{
    // Loopback only: nothing outside this PC can reach the toggle
    if (! listener.createListener (port, "127.0.0.1"))
    {
        std::cout << "[Control] Could not listen on 127.0.0.1:" << port << std::endl;
        return false;
    }

    std::cout << "[Control] Listening on http://127.0.0.1:" << port << std::endl;
    startThread();
    return true;
}

void ControlServer::stop()
{
    signalThreadShouldExit();
    listener.close();
    stopThread (2000);
}

void ControlServer::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());

        if (client == nullptr)
        {
            if (threadShouldExit())
                break;
            continue;
        }

        handleConnection (*client);
        client->close();
    }
}

juce::String ControlServer::statusJson() const
{
    auto status = audioEngine.getFilterStatus();

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("running", status.running);
    obj->setProperty ("enabled", status.enabled);
    obj->setProperty ("buffering", status.buffering);
    obj->setProperty ("bypassed", status.bypassed);
    obj->setProperty ("delayMs", juce::roundToInt (status.delaySeconds * 1000.0));

    return juce::JSON::toString (juce::var (obj), true);
}

void ControlServer::handleConnection (juce::StreamingSocket& client)
{
    if (client.waitUntilReady (true, 1000) != 1)
        return;

    char buffer[2048] = {};
    int bytes = client.read (buffer, (int) sizeof (buffer) - 1, false);
    if (bytes <= 0)
        return;

    auto request = juce::String::fromUTF8 (buffer, bytes);
    auto requestLine = request.upToFirstOccurrenceOf ("\r\n", false, false);
    auto method = requestLine.upToFirstOccurrenceOf (" ", false, false);
    auto path = requestLine.fromFirstOccurrenceOf (" ", false, false)
                           .upToFirstOccurrenceOf (" ", false, false);

    // Only the browser extension (or local tools with no Origin) may talk to us.
    // Web pages send an http(s) Origin; refuse them so a site can't flip the filter.
    juce::String origin;
    for (auto line : juce::StringArray::fromLines (request))
        if (line.startsWithIgnoreCase ("Origin:"))
            origin = line.fromFirstOccurrenceOf (":", false, false).trim();

    int code = 200;
    juce::String body;

    if (origin.startsWithIgnoreCase ("http"))
    {
        code = 403;
    }
    else if (method == "GET" && path == "/status")
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
            juce::MessageManager::callAsync ([this] { if (onFilterChanged) onFilterChanged(); });
            body = statusJson();
        }
    }
    else
    {
        code = 404;
    }

    if (code == 404)
        body = "{\"error\":\"not found\"}";
    else if (code == 403)
        body = "{\"error\":\"forbidden\"}";

    juce::String statusText = code == 200 ? "OK" : (code == 403 ? "Forbidden" : "Not Found");
    auto bodyUtf8 = body.toStdString();

    juce::String response;
    response << "HTTP/1.1 " << code << " " << statusText << "\r\n"
             << "Content-Type: application/json\r\n"
             << "Cache-Control: no-store\r\n"
             << "Content-Length: " << (int) bodyUtf8.size() << "\r\n"
             << "Connection: close\r\n\r\n";

    auto header = response.toStdString();
    client.write (header.data(), (int) header.size());
    if (! bodyUtf8.empty())
        client.write (bodyUtf8.data(), (int) bodyUtf8.size());
}
