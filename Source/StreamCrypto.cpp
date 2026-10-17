/*
  ==============================================================================

    StreamCrypto.cpp

  ==============================================================================
*/

#include "StreamCrypto.h"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <bcrypt.h>
 #pragma comment(lib, "bcrypt.lib")
#else
 #include <cstdio>
 #include <random>
#endif

namespace StreamCrypto
{
namespace
{
    inline uint32_t rotl (uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    inline uint32_t rotr (uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    inline uint32_t readBE32 (const uint8_t* p)
    {
        return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
    }

    inline void writeBE32 (uint8_t* p, uint32_t v)
    {
        p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16); p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v;
    }

    /** Merkle-Damgard padding shared by SHA-1 and SHA-256: calls block() for
        every 64-byte block of the message plus padding and bit length. */
    template <typename BlockFn>
    void forEachPaddedBlock (const void* data, size_t size, BlockFn&& block)
    {
        auto* bytes = static_cast<const uint8_t*> (data);
        size_t full = size / 64;
        for (size_t i = 0; i < full; ++i)
            block (bytes + i * 64);

        uint8_t tail[128] = {};
        size_t rest = size - full * 64;
        if (rest > 0)
            std::memcpy (tail, bytes + full * 64, rest);
        tail[rest] = 0x80;
        size_t tailSize = (rest + 1 + 8 <= 64) ? 64 : 128;
        uint64_t bits = (uint64_t) size * 8;
        for (int i = 0; i < 8; ++i)
            tail[tailSize - 1 - i] = (uint8_t) (bits >> (8 * i));

        block (tail);
        if (tailSize == 128)
            block (tail + 64);
    }

    const uint32_t k256[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    const char* hexDigits = "0123456789abcdef";
    const char* b32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    int hexValue (char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool isHexString (const std::string& s)
    {
        for (char c : s)
            if (hexValue (c) < 0)
                return false;
        return true;
    }
}

//==============================================================================
std::array<uint8_t, 20> sha1 (const void* data, size_t size)
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };

    forEachPaddedBlock (data, size, [&h] (const uint8_t* block)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = readBE32 (block + i * 4);
        for (int i = 16; i < 80; ++i)
            w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }

            uint32_t t = rotl (a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl (b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    });

    std::array<uint8_t, 20> out {};
    for (int i = 0; i < 5; ++i)
        writeBE32 (out.data() + i * 4, h[i]);
    return out;
}

std::array<uint8_t, 32> sha256 (const void* data, size_t size)
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

    forEachPaddedBlock (data, size, [&h] (const uint8_t* block)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = readBE32 (block + i * 4);
        for (int i = 16; i < 64; ++i)
        {
            uint32_t s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            uint32_t S1 = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k256[i] + w[i];
            uint32_t S0 = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    });

    std::array<uint8_t, 32> out {};
    for (int i = 0; i < 8; ++i)
        writeBE32 (out.data() + i * 4, h[i]);
    return out;
}

std::array<uint8_t, 32> hmacSha256 (const void* key, size_t keySize, const void* msg, size_t msgSize)
{
    uint8_t k[64] = {};
    if (keySize > 64)
    {
        auto hashed = sha256 (key, keySize);
        std::memcpy (k, hashed.data(), hashed.size());
    }
    else if (keySize > 0)
    {
        std::memcpy (k, key, keySize);
    }

    std::vector<uint8_t> inner (64 + msgSize);
    for (int i = 0; i < 64; ++i)
        inner[(size_t) i] = k[i] ^ 0x36;
    if (msgSize > 0)
        std::memcpy (inner.data() + 64, msg, msgSize);
    auto innerHash = sha256 (inner.data(), inner.size());

    uint8_t outer[64 + 32];
    for (int i = 0; i < 64; ++i)
        outer[i] = k[i] ^ 0x5c;
    std::memcpy (outer + 64, innerHash.data(), 32);
    return sha256 (outer, sizeof (outer));
}

std::string sha256Hex (const std::string& msg)
{
    return toHex (sha256 (msg.data(), msg.size()));
}

std::string hmacHex (const Bytes& key, const std::string& msg)
{
    return toHex (hmacSha256 (key.data(), key.size(), msg.data(), msg.size()));
}

//==============================================================================
std::string toHex (const void* data, size_t size)
{
    auto* bytes = static_cast<const uint8_t*> (data);
    std::string out;
    out.reserve (size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        out += hexDigits[bytes[i] >> 4];
        out += hexDigits[bytes[i] & 15];
    }
    return out;
}

bool fromHex (const std::string& hex, Bytes& out)
{
    out.clear();
    if (hex.size() % 2 != 0)
        return false;

    out.reserve (hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        int hi = hexValue (hex[i]), lo = hexValue (hex[i + 1]);
        if (hi < 0 || lo < 0)
        {
            out.clear();
            return false;
        }
        out.push_back ((uint8_t) ((hi << 4) | lo));
    }
    return true;
}

std::string base64Encode (const void* data, size_t size)
{
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto* bytes = static_cast<const uint8_t*> (data);
    std::string out;
    out.reserve ((size + 2) / 3 * 4);

    for (size_t i = 0; i < size; i += 3)
    {
        uint32_t v = (uint32_t) bytes[i] << 16;
        if (i + 1 < size) v |= (uint32_t) bytes[i + 1] << 8;
        if (i + 2 < size) v |= bytes[i + 2];

        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += (i + 1 < size) ? alphabet[(v >> 6) & 63] : '=';
        out += (i + 2 < size) ? alphabet[v & 63] : '=';
    }
    return out;
}

std::string base32Encode (const Bytes& data)
{
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;

    for (auto b : data)
    {
        buffer = (buffer << 8) | b;
        bits += 8;
        while (bits >= 5)
        {
            out += b32Alphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0)
        out += b32Alphabet[(buffer << (5 - bits)) & 31];
    return out;
}

bool base32Decode (const std::string& text, Bytes& out)
{
    out.clear();
    uint32_t buffer = 0;
    int bits = 0;

    for (char c : text)
    {
        if (c == '-' || c == ' ' || c == '=')
            continue;

        int v = -1;
        if (c >= 'A' && c <= 'Z')      v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        if (v < 0)
        {
            out.clear();
            return false;
        }

        buffer = (buffer << 5) | (uint32_t) v;
        bits += 5;
        if (bits >= 8)
        {
            out.push_back ((uint8_t) (buffer >> (bits - 8)));
            bits -= 8;
        }
        buffer &= (1u << bits) - 1;   // Keep only the bits not yet output
    }

    // Leftover bits are padding and must be zero (rejects mistyped last chars)
    if (bits >= 5 || (buffer & ((1u << bits) - 1)) != 0)
    {
        out.clear();
        return false;
    }
    return true;
}

bool constantTimeEquals (const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (uint8_t) (a[i] ^ b[i]);
    return diff == 0;
}

bool randomBytes (uint8_t* dest, size_t size)
{
    if (size == 0)
        return true;

   #ifdef _WIN32
    return BCryptGenRandom (nullptr, dest, (ULONG) size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
   #else
    if (auto* f = std::fopen ("/dev/urandom", "rb"))
    {
        size_t got = std::fread (dest, 1, size, f);
        std::fclose (f);
        if (got == size)
            return true;
    }

    // Test builds only (Windows always uses BCryptGenRandom)
    std::random_device rd;
    for (size_t i = 0; i < size; ++i)
        dest[i] = (uint8_t) rd();
    return true;
   #endif
}

Bytes randomBytes (size_t size)
{
    Bytes out (size);
    if (! randomBytes (out.data(), size))
        throw std::runtime_error ("random number generator failed");
    return out;
}

std::string webSocketAccept (const std::string& clientKey)
{
    auto text = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto digest = sha1 (text.data(), text.size());
    return base64Encode (digest.data(), digest.size());
}

//==============================================================================
std::string pairMac (const Bytes& P, const std::string& deviceId, const std::string& name, const std::string& nonce)
{
    return hmacHex (P, "pair|" + deviceId + "|" + name + "|" + nonce);
}

std::string wrapKey (const Bytes& P, const std::string& nonce, const Bytes& K)
{
    auto msg = "wrap|" + nonce;
    auto pad = hmacSha256 (P.data(), P.size(), msg.data(), msg.size());

    Bytes wrapped (pad.size(), 0);
    for (size_t i = 0; i < pad.size() && i < K.size(); ++i)
        wrapped[i] = K[i] ^ pad[i];
    return toHex (wrapped);
}

bool unwrapKey (const Bytes& P, const std::string& nonce, const std::string& wrapped, Bytes& K)
{
    Bytes w;
    if (wrapped.size() != 64 || ! fromHex (wrapped, w))
        return false;

    auto msg = "wrap|" + nonce;
    auto pad = hmacSha256 (P.data(), P.size(), msg.data(), msg.size());
    K.assign (32, 0);
    for (size_t i = 0; i < 32; ++i)
        K[i] = w[i] ^ pad[i];
    return true;
}

std::string pairOkMac (const Bytes& P, const std::string& deviceId, const std::string& wrapped)
{
    return hmacHex (P, "pair-ok|" + deviceId + "|" + wrapped);
}

std::string requestMac (const Bytes& K, const std::string& method, const std::string& path,
                        const std::string& deviceId, const std::string& time,
                        const std::string& nonce, const std::string& body)
{
    return hmacHex (K, method + "|" + path + "|" + deviceId + "|" + time + "|" + nonce + "|" + sha256Hex (body));
}

std::string responseMac (const Bytes& K, const std::string& path, const std::string& nonce, const std::string& body)
{
    return hmacHex (K, "resp|" + path + "|" + nonce + "|" + sha256Hex (body));
}

//==============================================================================
bool NonceCache::checkAndInsert (const std::string& deviceId, const std::string& nonce, int64_t nowMs)
{
    std::lock_guard<std::mutex> sl (lock);
    prune (nowMs);

    auto key = deviceId + "|" + nonce;
    auto it = seen.find (key);
    if (it != seen.end() && nowMs - it->second <= window)
        return false;

    seen[key] = nowMs;
    return true;
}

size_t NonceCache::size() const
{
    std::lock_guard<std::mutex> sl (lock);
    return seen.size();
}

void NonceCache::prune (int64_t nowMs)
{
    if (nowMs - lastPrune < 10 * 1000 && seen.size() < 10000)
        return;

    lastPrune = nowMs;
    for (auto it = seen.begin(); it != seen.end();)
    {
        if (nowMs - it->second > window)
            it = seen.erase (it);
        else
            ++it;
    }
}

VerifyResult verifyRequest (const SignedRequest& r, const Bytes& K, int64_t nowMs,
                            NonceCache& nonces, int64_t maxSkewMs)
{
    if (r.deviceId.empty() || r.mac.size() != 64 || r.nonce.size() != 32 || ! isHexString (r.nonce)
        || r.time.empty() || r.time.size() > 15)
        return VerifyResult::malformed;

    int64_t time = 0;
    for (char c : r.time)
    {
        if (c < '0' || c > '9')
            return VerifyResult::malformed;
        time = time * 10 + (c - '0');
    }

    if (time > nowMs + maxSkewMs || time < nowMs - maxSkewMs)
        return VerifyResult::clockSkew;

    auto expected = requestMac (K, r.method, r.path, r.deviceId, r.time, r.nonce, r.body);
    std::string given = r.mac;
    for (auto& c : given)
        if (c >= 'A' && c <= 'F')
            c = (char) (c - 'A' + 'a');

    if (! constantTimeEquals (expected, given))
        return VerifyResult::badMac;

    if (! nonces.checkAndInsert (r.deviceId, r.nonce, nowMs))
        return VerifyResult::replay;

    return VerifyResult::ok;
}

const char* toString (VerifyResult r)
{
    switch (r)
    {
        case VerifyResult::ok:        return "ok";
        case VerifyResult::malformed: return "malformed request";
        case VerifyResult::clockSkew: return "clock skew";
        case VerifyResult::badMac:    return "bad mac";
        case VerifyResult::replay:    return "replayed nonce";
    }
    return "unknown";
}
}
