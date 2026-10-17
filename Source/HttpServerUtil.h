/*
  ==============================================================================

    HttpServerUtil.h

    The small HTTP/1.1 pieces shared by ControlServer (127.0.0.1:8766) and
    StreamHost (LAN :8767):

      - SocketServer: accepts connections on its own thread and handles each
        one on its own worker thread, up to a limit (extra connections get 503),
        so a 25 s long poll doesn't hold up other requests.
      - readRequest / sendResponse: one request per connection
        (Connection: close), with the whole Content-Length body read.
      - WebSocket framing for the /pcm stream (RFC 6455, server side).

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Http
{
    struct Request
    {
        juce::String method, path, query;          // path has no query string
        std::map<std::string, std::string> headers; // Lowercase names
        std::string body;                           // Raw bytes (UTF-8 for JSON)

        juce::String header (const char* lowercaseName) const
        {
            auto it = headers.find (lowercaseName);
            return it != headers.end() ? juce::String::fromUTF8 (it->second.c_str()) : juce::String();
        }
    };

    enum class ReadResult { ok, closed, tooLarge, badRequest };

    /** Reads the request line, headers and the whole body (Content-Length up
        to maxBody). Gives up after timeoutMs without data, or when stopping. */
    ReadResult readRequest (juce::StreamingSocket& socket, Request& request, size_t maxBody,
                            int timeoutMs, const std::atomic<bool>& stopping);

    struct Response
    {
        int code = 200;
        std::string body;
        juce::String contentType { "application/json" };
        juce::String cacheControl { "no-store" };
        juce::StringPairArray headers;              // Extra headers
    };

    /** Writes the response (Connection: close). headOnly skips the body (HEAD). */
    bool send (juce::StreamingSocket& socket, const Response& response, bool headOnly = false);

    /** Shorthand: JSON body with this status code. */
    Response json (int code, const juce::String& body);
    Response jsonError (int code, const juce::String& message);

    bool writeAll (juce::StreamingSocket& socket, const void* data, size_t size);
    const char* statusText (int code);

    /** JSON body to a var (an empty or invalid body gives a void var). */
    juce::var parseJsonBody (const std::string& body);

    //==============================================================================
    namespace WebSocket
    {
        enum Opcode : uint8_t { text = 0x1, binary = 0x2, close = 0x8, ping = 0x9, pong = 0xA };

        /** True if this is a valid RFC 6455 upgrade request. */
        bool isUpgradeRequest (const Request& request);

        /** Sends the 101 Switching Protocols reply. */
        bool acceptUpgrade (juce::StreamingSocket& socket, const Request& request);

        /** One unmasked server frame. */
        bool sendFrame (juce::StreamingSocket& socket, Opcode opcode, const void* data, size_t size);

        /** One complete frame from the client (unmasked here). */
        struct Frame { uint8_t opcode = 0; bool fin = true; std::string payload; };

        /** Takes complete frames off the front of buffer. False on a protocol
            error (unmasked client frame, or a frame over maxPayload). */
        bool parseFrames (std::string& buffer, std::vector<Frame>& frames, size_t maxPayload = 64 * 1024);
    }

    //==============================================================================
    class SocketServer : private juce::Thread
    {
    public:
        /** Runs on a worker thread; the socket is closed afterwards. Should
            return soon after stopping becomes true. */
        using Handler = std::function<void (juce::StreamingSocket&, const std::atomic<bool>& stopping)>;

        SocketServer (const juce::String& name, int maxConnections, Handler handler);
        ~SocketServer() override;

        /** bindAddress "127.0.0.1" for loopback only, empty for all interfaces. */
        bool start (int port, const juce::String& bindAddress);

        /** Stops accepting, waits for the workers (closing their sockets if they
            don't finish within a couple of seconds). */
        void stop();

        bool isListening() const { return listening.load(); }

    private:
        struct Worker
        {
            std::thread thread;
            std::unique_ptr<juce::StreamingSocket> socket;
            std::atomic<bool> done { false };
        };

        void run() override;
        void reapFinishedWorkers();

        Handler handler;
        int maxConnections;
        juce::StreamingSocket listener;
        std::atomic<bool> stopping { false }, listening { false };

        std::mutex workersLock;
        std::list<std::unique_ptr<Worker>> workers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SocketServer)
    };
}
