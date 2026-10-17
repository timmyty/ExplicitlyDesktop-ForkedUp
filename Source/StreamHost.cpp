/*
  ==============================================================================

    StreamHost.cpp

  ==============================================================================
*/

// winsock2.h has to come before windows.h, which AudioEngine.h pulls in (WindowsMediaInfo.h)
#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <iphlpapi.h>
 #pragma comment(lib, "iphlpapi.lib")
 #pragma comment(lib, "ws2_32.lib")
#endif

#include "StreamHost.h"
#include <condition_variable>
#include <iostream>

namespace
{
    juce::String fromStd (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

    bool isHex (const std::string& s, size_t length)
    {
        if (s.size() != length)
            return false;
        for (char c : s)
            if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        return true;
    }

    /** Ids and names go into "|"-joined MAC inputs, so "|" and control characters are refused. */
    bool isCleanField (const std::string& s, size_t maxBytes)
    {
        if (s.empty() || s.size() > maxBytes)
            return false;
        for (unsigned char c : s)
            if (c == '|' || c < 0x20 || c == 0x7f)
                return false;
        return true;
    }

    /** Ranks a LAN address: home networks first, then other private ranges. -1 = unusable. */
    int rankAddress (juce::uint8 a, juce::uint8 b)
    {
        if (a == 192 && b == 168)          return 3;
        if (a == 10)                       return 2;
        if (a == 172 && b >= 16 && b <= 31) return 1;
        if (a == 127 || (a == 169 && b == 254) || a == 0 || a >= 224)
            return -1;
        return 0;
    }
}

StreamHost::StreamHost (AudioEngine& engine, PairedDevices& devices, RemoteSessions& sessions, int p)
    : audioEngine (engine), pairedDevices (devices), remoteSessions (sessions), port (p),
      server ("StreamHost", maxConnections,
              [this] (juce::StreamingSocket& client, const std::atomic<bool>& stopping)
              {
                  handleConnection (client, stopping);
              })
{
}

StreamHost::~StreamHost()
{
    stop();
}

bool StreamHost::start()
{
    // All interfaces: the phone connects over the LAN
    if (! server.start (port, {}))
    {
        std::cout << "[Phone] Could not listen on port " << port << std::endl;
        return false;
    }

    std::cout << "[Phone] Phone streaming on http://" << findLanAddress() << ":" << port << std::endl;
    return true;
}

void StreamHost::stop()
{
    cancelPairing();
    server.stop();
}

//==============================================================================
StreamHost::PairingCode StreamHost::beginPairing()
{
    auto secret = StreamCrypto::randomBytes (16);
    auto b32 = StreamCrypto::base32Encode (secret);

    PairingCode code;
    for (size_t i = 0; i < b32.size(); ++i)
    {
        if (i > 0 && i % 4 == 0)
            code.typedCode << "-";
        code.typedCode << juce::String::charToString ((juce::juce_wchar) b32[i]);
    }

    auto address = findLanAddress();
    code.url << "http://" << (address.isNotEmpty() ? address : juce::String ("127.0.0.1")) << ":" << port
             << "/#pair=" << b32 << "&host=" << fromStd (pairedDevices.getHostId());

    auto now = juce::Time::currentTimeMillis();
    code.expires = juce::Time (now + pairingSeconds * 1000);

    std::lock_guard<std::mutex> sl (pairingLock);
    pairingSecret = std::move (secret);
    pairingExpiresMs = now + pairingSeconds * 1000;
    return code;
}

void StreamHost::cancelPairing()
{
    std::lock_guard<std::mutex> sl (pairingLock);
    std::fill (pairingSecret.begin(), pairingSecret.end(), (uint8_t) 0);
    pairingSecret.clear();
}

//==============================================================================
juce::File StreamHost::getClientFolder()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
               .getParentDirectory()
               .getChildFile ("MobileClient");
}

juce::String StreamHost::findLanAddress()
{
    juce::String best;
    int bestScore = -1;

    auto consider = [&] (juce::uint8 a, juce::uint8 b, juce::uint8 c, juce::uint8 d, bool hasGateway)
    {
        int rank = rankAddress (a, b);
        if (rank < 0)
            return;
        int score = rank + (hasGateway ? 10 : 0);   // Virtual adapters (Hyper-V, VPNs) usually have none
        if (score > bestScore)
        {
            bestScore = score;
            best = juce::String (a) + "." + juce::String (b) + "." + juce::String (c) + "." + juce::String (d);
        }
    };

   #ifdef _WIN32
    ULONG size = 16 * 1024;
    juce::HeapBlock<juce::uint8> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;

    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buffer.malloc (size);
        result = GetAdaptersAddresses (AF_INET,
                                       GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST
                                           | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                       nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.getData()), &size);
    }

    if (result == NO_ERROR)
    {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.getData()); adapter != nullptr; adapter = adapter->Next)
        {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;

            bool hasGateway = adapter->FirstGatewayAddress != nullptr;
            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
            {
                auto* sa = unicast->Address.lpSockaddr;
                if (sa == nullptr || sa->sa_family != AF_INET)
                    continue;

                auto* bytes = reinterpret_cast<const juce::uint8*> (&reinterpret_cast<sockaddr_in*> (sa)->sin_addr);
                consider (bytes[0], bytes[1], bytes[2], bytes[3], hasGateway);
            }
        }
    }

    if (best.isNotEmpty())
        return best;
   #endif

    for (auto& address : juce::IPAddress::getAllAddresses (false))
        consider (address.address[0], address.address[1], address.address[2], address.address[3], false);

    return best;
}

//==============================================================================
void StreamHost::handleConnection (juce::StreamingSocket& client, const std::atomic<bool>& stopping)
{
    Http::Request request;
    auto readResult = Http::readRequest (client, request, 64 * 1024, 10000, stopping);

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

    if (method == "GET" || method == "HEAD")
    {
        Http::send (client, serveStatic (request), method == "HEAD");
    }
    else if (method == "POST" && path == "/api/pair")
    {
        Http::send (client, handlePair (request, stopping));
    }
    else if (method == "POST" && (path == "/api/hello" || path == "/api/session/offer"
                                  || path == "/api/session/hangup" || path == "/api/unpair"))
    {
        handleSigned (client, request);
    }
    else
    {
        Http::send (client, Http::jsonError (404, "not found"));
    }
}

Http::Response StreamHost::serveStatic (const Http::Request& request)
{
    // Exactly these files, so no path can reach outside MobileClient/
    static const std::map<juce::String, juce::String> files {
        { "/",              "text/html; charset=utf-8" },
        { "/index.html",    "text/html; charset=utf-8" },
        { "/app.js",        "application/javascript; charset=utf-8" },
        { "/crypto.js",     "application/javascript; charset=utf-8" },
        { "/sw.js",         "application/javascript; charset=utf-8" },
        { "/style.css",     "text/css; charset=utf-8" },
        { "/manifest.json", "application/manifest+json" },
        { "/icon-192.png",  "image/png" },
        { "/icon-512.png",  "image/png" }
    };

    auto it = files.find (request.path);
    if (it == files.end())
        return Http::jsonError (404, "not found");

    auto name = request.path == "/" ? juce::String ("index.html") : request.path.substring (1);
    auto file = getClientFolder().getChildFile (name);

    juce::MemoryBlock data;
    if (! file.existsAsFile() || ! file.loadFileAsData (data))
        return Http::jsonError (404, "not found");

    Http::Response response;
    response.contentType = it->second;
    response.cacheControl = "no-cache";
    response.body.assign (static_cast<const char*> (data.getData()), data.getSize());
    response.headers.set ("X-Content-Type-Options", "nosniff");
    response.headers.set ("Referrer-Policy", "no-referrer");
    return response;
}

//==============================================================================
Http::Response StreamHost::handlePair (const Http::Request& request, const std::atomic<bool>& stopping)
{
    auto json = Http::parseJsonBody (request.body);
    auto deviceId = json.getProperty ("deviceId", {}).toString().toStdString();
    auto name     = json.getProperty ("name", {}).toString().toStdString();   // Exactly as signed
    auto nonce    = json.getProperty ("nonce", {}).toString().toStdString();
    auto mac      = json.getProperty ("mac", {}).toString().toLowerCase().toStdString();

    if (! json.isObject() || (int) json.getProperty ("v", 0) != 1
        || ! isCleanField (deviceId, 64) || ! isCleanField (name, 128) || ! isHex (nonce, 32) || ! isHex (mac, 64))
        return Http::jsonError (400, "bad request");

    // Check the MAC against the live pairing secret, then use the secret up
    StreamCrypto::Bytes P;
    {
        std::lock_guard<std::mutex> sl (pairingLock);
        bool live = ! pairingSecret.empty() && juce::Time::currentTimeMillis() <= pairingExpiresMs;

        // A wrong MAC doesn't use the code up, so nobody on the LAN can cancel a pairing
        if (! live || ! StreamCrypto::constantTimeEquals (StreamCrypto::pairMac (pairingSecret, deviceId, name, nonce), mac))
            return Http::jsonError (403, "pairing code expired or wrong");

        P = pairingSecret;
        pairingSecret.clear();
    }

    // Ask on Windows; the request stays open until the user answers (or 120 s)
    struct Decision
    {
        std::mutex lock;
        std::condition_variable changed;
        bool decided = false, allowed = false;
    };
    auto decision = std::make_shared<Decision>();

    if (onApprovalRequest)
    {
        onApprovalRequest (fromStd (name), [decision] (bool allow)
        {
            std::lock_guard<std::mutex> sl (decision->lock);
            if (! decision->decided)
            {
                decision->decided = true;
                decision->allowed = allow;
            }
            decision->changed.notify_all();
        });
    }

    bool allowed = false;
    {
        std::unique_lock<std::mutex> sl (decision->lock);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (pairingSeconds);
        while (! decision->decided && ! stopping.load() && std::chrono::steady_clock::now() < deadline)
            decision->changed.wait_for (sl, std::chrono::milliseconds (250));

        // Too late answers are ignored from here on
        allowed = decision->decided && decision->allowed;
        decision->decided = true;
    }

    if (! allowed)
    {
        std::cout << "[Phone] Pairing denied for " << name << std::endl;
        return Http::jsonError (403, "denied");
    }

    auto K = StreamCrypto::randomBytes (32);
    if (! pairedDevices.addDevice (deviceId, name, K))
        return Http::jsonError (500, "could not save the device");

    auto wrapped = StreamCrypto::wrapKey (P, nonce, K);

    auto* reply = new juce::DynamicObject();
    reply->setProperty ("v", 1);
    reply->setProperty ("hostId", fromStd (pairedDevices.getHostId()));
    reply->setProperty ("hostName", juce::SystemStats::getComputerName());
    reply->setProperty ("deviceId", fromStd (deviceId));
    reply->setProperty ("wrapped", fromStd (wrapped));
    reply->setProperty ("mac", fromStd (StreamCrypto::pairOkMac (P, deviceId, wrapped)));

    std::fill (K.begin(), K.end(), (uint8_t) 0);
    std::fill (P.begin(), P.end(), (uint8_t) 0);
    std::cout << "[Phone] Paired " << name << std::endl;

    if (onDevicesChanged)
        onDevicesChanged();

    return Http::json (200, juce::JSON::toString (juce::var (reply), true));
}

//==============================================================================
void StreamHost::handleSigned (juce::StreamingSocket& client, const Http::Request& request)
{
    StreamCrypto::SignedRequest signedRequest;
    signedRequest.method   = request.method.toStdString();
    signedRequest.path     = request.path.toStdString();
    signedRequest.deviceId = request.header ("x-device").toStdString();
    signedRequest.time     = request.header ("x-time").toStdString();
    signedRequest.nonce    = request.header ("x-nonce").toStdString();
    signedRequest.mac      = request.header ("x-mac").toStdString();
    signedRequest.body     = request.body;

    StreamCrypto::Bytes K;
    std::string deviceName;
    if (! pairedDevices.getDevice (signedRequest.deviceId, K, deviceName))
    {
        Http::send (client, Http::jsonError (401, "unknown device"));
        return;
    }

    auto verdict = StreamCrypto::verifyRequest (signedRequest, K, juce::Time::currentTimeMillis(),
                                                pairedDevices.getNonceCache());
    if (verdict != StreamCrypto::VerifyResult::ok)
    {
        Http::send (client, Http::jsonError (401, StreamCrypto::toString (verdict)));
        return;
    }

    pairedDevices.touch (signedRequest.deviceId);

    const auto& path = request.path;
    auto json = Http::parseJsonBody (request.body);
    Http::Response response = Http::json (200, "{}");

    if (path == "/api/hello")
    {
        auto status = audioEngine.getFilterStatus();
        auto* filter = new juce::DynamicObject();
        filter->setProperty ("running", status.running);
        filter->setProperty ("enabled", status.enabled);
        filter->setProperty ("buffering", status.buffering);
        filter->setProperty ("bypassed", status.bypassed);
        filter->setProperty ("delayMs", juce::roundToInt (status.delaySeconds * 1000.0));

        auto* obj = new juce::DynamicObject();
        obj->setProperty ("hostName", juce::SystemStats::getComputerName());
       #ifdef JUCE_APPLICATION_VERSION_STRING
        obj->setProperty ("appVersion", JUCE_APPLICATION_VERSION_STRING);
       #else
        obj->setProperty ("appVersion", "1.0.0");
       #endif
        obj->setProperty ("filter", juce::var (filter));
        response = Http::json (200, juce::JSON::toString (juce::var (obj), true));
    }
    else if (path == "/api/session/offer")
    {
        auto sdp = json.getProperty ("sdp", {}).toString().toStdString();
        if (sdp.empty())
        {
            response = Http::jsonError (400, "bad request");
        }
        else
        {
            std::string sessionId, answer, error;
            auto result = remoteSessions.submitOffer (signedRequest.deviceId, deviceName, sdp,
                                                      sessionId, answer, error, 15000);
            switch (result)
            {
                case RemoteSessions::OfferResult::answered:
                {
                    auto* obj = new juce::DynamicObject();
                    obj->setProperty ("sessionId", fromStd (sessionId));
                    obj->setProperty ("sdp", fromStd (answer));
                    response = Http::json (200, juce::JSON::toString (juce::var (obj), true));
                    break;
                }
                case RemoteSessions::OfferResult::unavailable:  response = Http::jsonError (409, "host browser not available"); break;
                case RemoteSessions::OfferResult::timedOut:     response = Http::jsonError (504, "timeout"); break;
                case RemoteSessions::OfferResult::failed:       response = Http::jsonError (502, fromStd (error)); break;
                case RemoteSessions::OfferResult::replaced:     response = Http::jsonError (409, "replaced"); break;
                case RemoteSessions::OfferResult::shuttingDown: response = Http::jsonError (503, "host closed"); break;
            }
        }
    }
    else if (path == "/api/session/hangup")
    {
        remoteSessions.hangup (signedRequest.deviceId, json.getProperty ("sessionId", {}).toString().toStdString());
    }
    else if (path == "/api/unpair")
    {
        remoteSessions.endSessionsForDevice (signedRequest.deviceId, "revoked");
        pairedDevices.removeDevice (signedRequest.deviceId);
        if (onDevicesChanged)
            onDevicesChanged();
    }

    // The phone checks this, so nobody on the LAN can swap the SDP answer
    response.headers.set ("X-Mac", fromStd (StreamCrypto::responseMac (K, signedRequest.path, signedRequest.nonce, response.body)));
    std::fill (K.begin(), K.end(), (uint8_t) 0);
    Http::send (client, response);
}
