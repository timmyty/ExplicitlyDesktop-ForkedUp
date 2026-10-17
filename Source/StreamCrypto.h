/*
  ==============================================================================

    StreamCrypto.h

    Small, portable crypto and encoding helpers for phone streaming
    (docs/STREAMING_PROTOCOL.md). No JUCE, so the unit tests build anywhere.

      - SHA-1 (only for the WebSocket handshake), SHA-256, HMAC-SHA256
      - hex, base64 (standard), base32 (RFC 4648, no padding)
      - constant-time comparison
      - random bytes (BCryptGenRandom on Windows, /dev/urandom elsewhere)

    Also the protocol's MAC formulas (pairing, key wrap, signed requests and
    responses) and a nonce cache, so the app and the tests compute them the
    same way.

  ==============================================================================
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace StreamCrypto
{
    using Bytes = std::vector<uint8_t>;

    //==============================================================================
    std::array<uint8_t, 20> sha1 (const void* data, size_t size);
    std::array<uint8_t, 32> sha256 (const void* data, size_t size);
    std::array<uint8_t, 32> hmacSha256 (const void* key, size_t keySize, const void* msg, size_t msgSize);

    /** Lowercase hex of SHA-256 of a UTF-8 string. */
    std::string sha256Hex (const std::string& msg);

    /** HMAC-SHA256 as lowercase hex (64 chars). */
    std::string hmacHex (const Bytes& key, const std::string& msg);

    //==============================================================================
    std::string toHex (const void* data, size_t size);
    inline std::string toHex (const Bytes& b) { return toHex (b.data(), b.size()); }
    template <size_t N> std::string toHex (const std::array<uint8_t, N>& a) { return toHex (a.data(), N); }

    /** Accepts upper or lower case; false on odd length or a bad digit. */
    bool fromHex (const std::string& hex, Bytes& out);

    std::string base64Encode (const void* data, size_t size);
    inline std::string base64Encode (const Bytes& b) { return base64Encode (b.data(), b.size()); }

    /** RFC 4648 base32 (A-Z, 2-7) without padding. */
    std::string base32Encode (const Bytes& data);

    /** Lenient for typed codes: lower case is accepted and '-', ' ' and '='
        are skipped. False on any other character or non-zero leftover bits. */
    bool base32Decode (const std::string& text, Bytes& out);

    /** Compares without an early exit, so timing doesn't reveal a MAC prefix. */
    bool constantTimeEquals (const std::string& a, const std::string& b);

    /** Fills from the OS CSPRNG. False if that failed (the buffer is then unusable). */
    bool randomBytes (uint8_t* dest, size_t size);
    Bytes randomBytes (size_t size);   // Throws std::runtime_error on failure

    /** RFC 6455 Sec-WebSocket-Accept for a client's Sec-WebSocket-Key. */
    std::string webSocketAccept (const std::string& clientKey);

    //==============================================================================
    // Protocol formulas (docs/STREAMING_PROTOCOL.md sections 2 and 3)

    /** HMAC(P, "pair|" + deviceId + "|" + name + "|" + nonce) */
    std::string pairMac (const Bytes& P, const std::string& deviceId, const std::string& name, const std::string& nonce);

    /** hex(K XOR HMAC(P, "wrap|" + nonce)); K must be 32 bytes. */
    std::string wrapKey (const Bytes& P, const std::string& nonce, const Bytes& K);

    /** Inverse of wrapKey. False if wrapped isn't 64 hex chars. */
    bool unwrapKey (const Bytes& P, const std::string& nonce, const std::string& wrapped, Bytes& K);

    /** HMAC(P, "pair-ok|" + deviceId + "|" + wrapped) */
    std::string pairOkMac (const Bytes& P, const std::string& deviceId, const std::string& wrapped);

    /** HMAC(K, method|path|deviceId|time|nonce|SHA256hex(body)) */
    std::string requestMac (const Bytes& K, const std::string& method, const std::string& path,
                            const std::string& deviceId, const std::string& time,
                            const std::string& nonce, const std::string& body);

    /** HMAC(K, "resp|" + path + "|" + nonce + "|" + SHA256hex(body)) */
    std::string responseMac (const Bytes& K, const std::string& path, const std::string& nonce,
                             const std::string& body);

    //==============================================================================
    /** Nonces seen in the last windowMs, per device. Thread safe. */
    class NonceCache
    {
    public:
        explicit NonceCache (int64_t windowMs = 5 * 60 * 1000) : window (windowMs) {}

        /** True if (deviceId, nonce) is new (and remembers it); false for a replay. */
        bool checkAndInsert (const std::string& deviceId, const std::string& nonce, int64_t nowMs);

        size_t size() const;

    private:
        void prune (int64_t nowMs);

        int64_t window;
        mutable std::mutex lock;
        std::map<std::string, int64_t> seen;   // deviceId|nonce -> time seen
        int64_t lastPrune = 0;
    };

    enum class VerifyResult
    {
        ok,
        malformed,     // Missing header, or nonce/time in the wrong format
        clockSkew,     // |X-Time - now| > 120 s
        badMac,
        replay         // Nonce already used in the last 5 minutes
    };

    struct SignedRequest
    {
        std::string method, path, deviceId, time, nonce, mac, body;
    };

    /** Checks a phone request (section 3) against the device key K. The nonce is
        only remembered once the MAC is good, so junk can't fill the cache. The
        caller handles unknown or revoked devices (also 401). */
    VerifyResult verifyRequest (const SignedRequest& request, const Bytes& K, int64_t nowMs,
                                NonceCache& nonces, int64_t maxSkewMs = 120 * 1000);

    const char* toString (VerifyResult r);
}
