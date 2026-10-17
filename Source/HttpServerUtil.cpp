/*
  ==============================================================================

    HttpServerUtil.cpp

  ==============================================================================
*/

#include "HttpServerUtil.h"
#include "StreamCrypto.h"
#include <cctype>
#include <iostream>

namespace Http
{
namespace
{
    constexpr size_t maxHeaderBytes = 16 * 1024;

    /** Waits for data in short slices so a stopping server isn't held up. */
    int waitForData (juce::StreamingSocket& socket, int timeoutMs, const std::atomic<bool>& stopping)
    {
        auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        for (;;)
        {
            if (stopping.load())
                return -1;

            auto now = juce::Time::getMillisecondCounter();
            if (now >= deadline)
                return 0;

            int ready = socket.waitUntilReady (true, (int) juce::jmin<juce::uint32> (deadline - now, 200));
            if (ready != 0)
                return ready;
        }
    }

    std::string toLower (std::string s)
    {
        for (auto& c : s)
            c = (char) std::tolower ((unsigned char) c);
        return s;
    }

    std::string trim (const std::string& s)
    {
        size_t a = s.find_first_not_of (" \t"), b = s.find_last_not_of (" \t");
        return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
    }
}

//==============================================================================
ReadResult readRequest (juce::StreamingSocket& socket, Request& request, size_t maxBody,
                        int timeoutMs, const std::atomic<bool>& stopping)
{
    std::string data;
    char buffer[4096];
    size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos)
    {
        if (data.size() > maxHeaderBytes)
            return ReadResult::tooLarge;

        if (waitForData (socket, timeoutMs, stopping) != 1)
            return ReadResult::closed;

        int n = socket.read (buffer, (int) sizeof (buffer), false);
        if (n <= 0)
            return ReadResult::closed;

        data.append (buffer, (size_t) n);
        headerEnd = data.find ("\r\n\r\n");
    }

    // Request line: METHOD SP target SP version
    auto lineEnd = data.find ("\r\n");
    auto requestLine = data.substr (0, lineEnd);
    auto sp1 = requestLine.find (' ');
    auto sp2 = requestLine.find (' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
        return ReadResult::badRequest;

    request.method = juce::String (requestLine.substr (0, sp1));
    auto target = requestLine.substr (sp1 + 1, sp2 - sp1 - 1);
    auto q = target.find ('?');
    request.path = juce::String::fromUTF8 (target.substr (0, q).c_str());
    request.query = q == std::string::npos ? juce::String() : juce::String::fromUTF8 (target.substr (q + 1).c_str());

    // Headers
    request.headers.clear();
    size_t pos = lineEnd + 2;
    while (pos < headerEnd)
    {
        auto end = data.find ("\r\n", pos);
        auto line = data.substr (pos, end - pos);
        pos = end + 2;

        auto colon = line.find (':');
        if (colon == std::string::npos)
            continue;
        request.headers[toLower (trim (line.substr (0, colon)))] = trim (line.substr (colon + 1));
    }

    // Body: exactly Content-Length bytes (SDP offers are several KB)
    size_t contentLength = 0;
    auto cl = request.headers.find ("content-length");
    if (cl != request.headers.end())
    {
        if (cl->second.empty() || cl->second.find_first_not_of ("0123456789") != std::string::npos
            || cl->second.size() > 9)
            return ReadResult::badRequest;
        contentLength = (size_t) std::stoul (cl->second);
    }
    else if (request.headers.count ("transfer-encoding") > 0)
    {
        return ReadResult::badRequest;   // Chunked bodies aren't needed by any client
    }

    if (contentLength > maxBody)
        return ReadResult::tooLarge;

    request.body = data.substr (headerEnd + 4);
    while (request.body.size() < contentLength)
    {
        if (waitForData (socket, timeoutMs, stopping) != 1)
            return ReadResult::closed;

        int want = (int) juce::jmin (sizeof (buffer), contentLength - request.body.size());
        int n = socket.read (buffer, want, false);
        if (n <= 0)
            return ReadResult::closed;
        request.body.append (buffer, (size_t) n);
    }
    request.body.resize (contentLength);
    return ReadResult::ok;
}

bool writeAll (juce::StreamingSocket& socket, const void* data, size_t size)
{
    auto* p = static_cast<const char*> (data);
    while (size > 0)
    {
        int n = socket.write (p, (int) juce::jmin<size_t> (size, 1 << 20));
        if (n <= 0)
            return false;
        p += n;
        size -= (size_t) n;
    }
    return true;
}

const char* statusText (int code)
{
    switch (code)
    {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 426: return "Upgrade Required";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Error";
    }
}

bool send (juce::StreamingSocket& socket, const Response& response, bool headOnly)
{
    juce::String head;
    head << "HTTP/1.1 " << response.code << " " << statusText (response.code) << "\r\n"
         << "Content-Type: " << response.contentType << "\r\n"
         << "Cache-Control: " << response.cacheControl << "\r\n"
         << "Content-Length: " << (int) response.body.size() << "\r\n";

    for (auto& key : response.headers.getAllKeys())
        head << key << ": " << response.headers[key] << "\r\n";

    head << "Connection: close\r\n\r\n";

    auto headUtf8 = head.toStdString();
    if (! writeAll (socket, headUtf8.data(), headUtf8.size()))
        return false;

    return headOnly || response.body.empty() || writeAll (socket, response.body.data(), response.body.size());
}

Response json (int code, const juce::String& body)
{
    Response r;
    r.code = code;
    r.body = body.toStdString();
    return r;
}

Response jsonError (int code, const juce::String& message)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("error", message);
    return json (code, juce::JSON::toString (juce::var (obj), true));
}

juce::var parseJsonBody (const std::string& body)
{
    if (body.empty())
        return {};

    juce::var result;
    if (juce::JSON::parse (juce::String::fromUTF8 (body.data(), (int) body.size()), result).failed())
        return {};
    return result;
}

//==============================================================================
namespace WebSocket
{
    bool isUpgradeRequest (const Request& request)
    {
        return request.method == "GET"
            && request.header ("upgrade").equalsIgnoreCase ("websocket")
            && request.header ("connection").containsIgnoreCase ("upgrade")
            && request.header ("sec-websocket-key").isNotEmpty()
            && request.header ("sec-websocket-version").trim() == "13";
    }

    bool acceptUpgrade (juce::StreamingSocket& socket, const Request& request)
    {
        auto accept = StreamCrypto::webSocketAccept (request.header ("sec-websocket-key").trim().toStdString());

        std::string reply = "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        return writeAll (socket, reply.data(), reply.size());
    }

    bool sendFrame (juce::StreamingSocket& socket, Opcode opcode, const void* data, size_t size)
    {
        uint8_t header[10];
        size_t headerSize = 2;
        header[0] = (uint8_t) (0x80 | opcode);   // FIN, no fragmentation

        if (size < 126)
        {
            header[1] = (uint8_t) size;
        }
        else if (size <= 0xFFFF)
        {
            header[1] = 126;
            header[2] = (uint8_t) (size >> 8);
            header[3] = (uint8_t) size;
            headerSize = 4;
        }
        else
        {
            header[1] = 127;
            for (int i = 0; i < 8; ++i)
                header[2 + i] = (uint8_t) ((uint64_t) size >> (56 - 8 * i));
            headerSize = 10;
        }

        // One write for small frames, so a frame doesn't go out as two packets
        std::string frame ((const char*) header, headerSize);
        frame.append (static_cast<const char*> (data), size);
        return writeAll (socket, frame.data(), frame.size());
    }

    bool parseFrames (std::string& buffer, std::vector<Frame>& frames, size_t maxPayload)
    {
        for (;;)
        {
            if (buffer.size() < 2)
                return true;

            auto* p = reinterpret_cast<const uint8_t*> (buffer.data());
            bool fin = (p[0] & 0x80) != 0;
            uint8_t opcode = p[0] & 0x0F;
            bool masked = (p[1] & 0x80) != 0;
            uint64_t length = p[1] & 0x7F;
            size_t pos = 2;

            if (! masked)
                return false;   // Clients must mask (RFC 6455 5.1)

            if (length == 126)
            {
                if (buffer.size() < 4)
                    return true;
                length = ((uint64_t) p[2] << 8) | p[3];
                pos = 4;
            }
            else if (length == 127)
            {
                if (buffer.size() < 10)
                    return true;
                length = 0;
                for (int i = 0; i < 8; ++i)
                    length = (length << 8) | p[2 + i];
                pos = 10;
            }

            if (length > maxPayload)
                return false;

            if (buffer.size() < pos + 4 + length)
                return true;

            const uint8_t* mask = p + pos;
            pos += 4;

            Frame frame;
            frame.opcode = opcode;
            frame.fin = fin;
            frame.payload.resize ((size_t) length);
            for (size_t i = 0; i < length; ++i)
                frame.payload[i] = (char) (p[pos + i] ^ mask[i % 4]);

            frames.push_back (std::move (frame));
            buffer.erase (0, pos + (size_t) length);
        }
    }
}

//==============================================================================
SocketServer::SocketServer (const juce::String& name, int maxConns, Handler h)
    : juce::Thread (name), handler (std::move (h)), maxConnections (maxConns)
{
}

SocketServer::~SocketServer()
{
    stop();
}

bool SocketServer::start (int port, const juce::String& bindAddress)
{
    stop();
    stopping = false;

    if (! listener.createListener (port, bindAddress))
        return false;

    listening = true;
    startThread();
    return true;
}

void SocketServer::stop()
{
    stopping = true;
    signalThreadShouldExit();
    listener.close();
    stopThread (3000);
    listening = false;

    // Workers notice `stopping` within a fraction of a second; a socket stuck
    // in a write to a dead client is closed to free it
    for (int i = 0; i < 25; ++i)
    {
        {
            std::lock_guard<std::mutex> sl (workersLock);
            bool allDone = true;
            for (auto& w : workers)
                allDone = allDone && w->done.load();
            if (allDone)
                break;
        }
        juce::Thread::sleep (100);
    }

    std::list<std::unique_ptr<Worker>> remaining;
    {
        std::lock_guard<std::mutex> sl (workersLock);
        for (auto& w : workers)
            if (! w->done.load() && w->socket != nullptr)
                w->socket->close();
        remaining.swap (workers);
    }

    for (auto& w : remaining)
        if (w->thread.joinable())
            w->thread.join();
}

void SocketServer::reapFinishedWorkers()
{
    std::list<std::unique_ptr<Worker>> finished;
    {
        std::lock_guard<std::mutex> sl (workersLock);
        for (auto it = workers.begin(); it != workers.end();)
        {
            if ((*it)->done.load())
            {
                finished.push_back (std::move (*it));
                it = workers.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& w : finished)
        if (w->thread.joinable())
            w->thread.join();
}

void SocketServer::run()
{
    while (! threadShouldExit() && ! stopping.load())
    {
        std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());
        reapFinishedWorkers();

        if (client == nullptr)
        {
            if (threadShouldExit() || stopping.load())
                break;
            juce::Thread::sleep (10);
            continue;
        }

        size_t active;
        {
            std::lock_guard<std::mutex> sl (workersLock);
            active = workers.size();
        }

        if ((int) active >= maxConnections)
        {
            send (*client, jsonError (503, "busy"));
            client->close();
            continue;
        }

        auto worker = std::make_unique<Worker>();
        worker->socket = std::move (client);
        auto* w = worker.get();

        std::lock_guard<std::mutex> sl (workersLock);
        workers.push_back (std::move (worker));
        w->thread = std::thread ([this, w]
        {
            try
            {
                handler (*w->socket, stopping);
            }
            catch (const std::exception& e)
            {
                std::cout << "[" << getThreadName() << "] Connection error: " << e.what() << std::endl;
            }
            w->socket->close();
            w->done = true;
        });
    }
}
}
