/*
  ==============================================================================

    ProtocolTests.cpp

    Pairing math and signed requests (docs/STREAMING_PROTOCOL.md sections 2-3),
    checked against tests/vectors/protocol_vectors.json, the same numbers the
    JavaScript tests use.

  ==============================================================================
*/

#include "TestFramework.h"
#include "StreamCrypto.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace StreamCrypto;
using TestFramework::Json;

#ifndef EXPLICITLY_VECTORS_FILE
 #define EXPLICITLY_VECTORS_FILE "tests/vectors/protocol_vectors.json"
#endif

namespace
{
    const Json& vectors()
    {
        static Json v = []
        {
            const char* path = std::getenv ("EXPLICITLY_VECTORS_FILE");
            std::ifstream in (path != nullptr ? path : EXPLICITLY_VECTORS_FILE, std::ios::binary);
            if (! in)
                throw std::runtime_error ("can't open protocol_vectors.json (set EXPLICITLY_VECTORS_FILE)");
            std::stringstream s;
            s << in.rdbuf();
            return Json::parse (s.str());
        }();
        return v;
    }

    Bytes hexBytes (const std::string& hex)
    {
        Bytes b;
        fromHex (hex, b);
        return b;
    }

    SignedRequest vectorRequest()
    {
        auto& r = vectors()["signedRequest"];
        SignedRequest req;
        req.method = r["method"].s;
        req.path = r["path"].s;
        req.deviceId = r["deviceId"].s;
        req.time = r["time"].s;
        req.nonce = r["nonce"].s;
        req.mac = r["mac"].s;
        req.body = r["body"].s;
        return req;
    }

    const int64_t vectorTime = 1760000000000LL;
}

TEST (vectors_file_loads)
{
    CHECK_EQ (vectors()["version"].n, 1.0);
    CHECK_EQ (vectors()["pairing"]["name"].s, std::string ("Tim\xE2\x80\x99s phone"));
}

TEST (vectors_primitives)
{
    auto& p = vectors()["primitives"];
    CHECK_EQ (sha256Hex (""), p["sha256_empty"].s);
    CHECK_EQ (sha256Hex ("abc"), p["sha256_abc"].s);

    auto key = p["hmac_rfc4231_tc2_key"].s;
    CHECK_EQ (hmacHex (Bytes (key.begin(), key.end()), p["hmac_rfc4231_tc2_data"].s), p["hmac_rfc4231_tc2"].s);
    CHECK_EQ (base32Encode (Bytes { 'f', 'o', 'o', 'b', 'a', 'r' }), p["base32_foobar"].s);
    CHECK_EQ (webSocketAccept (p["ws_key"].s), p["ws_accept"].s);
}

TEST (pairing_code_and_url)
{
    auto& v = vectors()["pairing"];
    auto P = hexBytes (v["P_hex"].s);
    CHECK_EQ (P.size(), (size_t) 16);
    CHECK_EQ (base32Encode (P), v["P_b32"].s);

    Bytes fromTyped;
    CHECK (base32Decode (v["typedCode"].s, fromTyped));
    CHECK (fromTyped == P);

    CHECK_EQ ("http://192.168.1.20:8767/#pair=" + base32Encode (P) + "&host=" + v["hostId"].s, v["qrUrl"].s);
}

TEST (pairing_macs_and_key_wrap)
{
    auto& v = vectors()["pairing"];
    auto P = hexBytes (v["P_hex"].s);
    auto K = hexBytes (v["K_hex"].s);

    CHECK_EQ (pairMac (P, v["deviceId"].s, v["name"].s, v["nonce"].s), v["pairMac"].s);
    CHECK_EQ (hmacHex (P, v["pairMacInput"].s), v["pairMac"].s);
    CHECK_EQ (hmacHex (P, v["padInput"].s), v["pad_hex"].s);

    auto wrapped = wrapKey (P, v["nonce"].s, K);
    CHECK_EQ (wrapped, v["wrapped"].s);
    CHECK_EQ (pairOkMac (P, v["deviceId"].s, wrapped), v["pairOkMac"].s);

    Bytes unwrapped;
    CHECK (unwrapKey (P, v["nonce"].s, wrapped, unwrapped));
    CHECK (unwrapped == K);

    // A different pairing secret gives a different key (and a MAC that fails)
    auto wrongP = P;
    wrongP[0] ^= 1;
    CHECK (unwrapKey (wrongP, v["nonce"].s, wrapped, unwrapped));
    CHECK (unwrapped != K);
    CHECK (pairMac (wrongP, v["deviceId"].s, v["name"].s, v["nonce"].s) != v["pairMac"].s);

    CHECK (! unwrapKey (P, v["nonce"].s, "abcd", unwrapped));
}

TEST (signed_request_vector)
{
    auto& v = vectors()["signedRequest"];
    auto K = hexBytes (v["K_hex"].s);

    CHECK_EQ (sha256Hex (v["body"].s), v["bodySha256"].s);
    CHECK_EQ (hmacHex (K, v["macInput"].s), v["mac"].s);
    CHECK_EQ (requestMac (K, v["method"].s, v["path"].s, v["deviceId"].s, v["time"].s, v["nonce"].s, v["body"].s), v["mac"].s);
    CHECK_EQ (responseMac (K, v["path"].s, v["nonce"].s, v["responseBody"].s), v["responseMac"].s);
    CHECK_EQ (hmacHex (K, v["responseMacInput"].s), v["responseMac"].s);
}

TEST (verify_request_accepts_and_rejects_replay)
{
    auto K = hexBytes (vectors()["signedRequest"]["K_hex"].s);
    NonceCache nonces;
    auto req = vectorRequest();

    CHECK (verifyRequest (req, K, vectorTime, nonces) == VerifyResult::ok);
    CHECK (verifyRequest (req, K, vectorTime + 1000, nonces) == VerifyResult::replay);

    // Uppercase hex MAC is the same MAC
    NonceCache fresh;
    auto upper = req;
    for (auto& c : upper.mac) c = (char) std::toupper ((unsigned char) c);
    CHECK (verifyRequest (upper, K, vectorTime, fresh) == VerifyResult::ok);
}

TEST (verify_request_clock_skew)
{
    auto K = hexBytes (vectors()["signedRequest"]["K_hex"].s);
    auto req = vectorRequest();

    NonceCache a, b, c, d;
    CHECK (verifyRequest (req, K, vectorTime + 120 * 1000, a) == VerifyResult::ok);
    CHECK (verifyRequest (req, K, vectorTime - 120 * 1000, b) == VerifyResult::ok);
    CHECK (verifyRequest (req, K, vectorTime + 120 * 1000 + 1, c) == VerifyResult::clockSkew);
    CHECK (verifyRequest (req, K, vectorTime - 120 * 1000 - 1, d) == VerifyResult::clockSkew);
    CHECK_EQ (c.size() + d.size(), (size_t) 0);   // Rejected requests don't use up a nonce
}

TEST (verify_request_bad_mac_and_tampering)
{
    auto K = hexBytes (vectors()["signedRequest"]["K_hex"].s);
    NonceCache nonces;

    auto body = vectorRequest();
    body.body += " ";
    CHECK (verifyRequest (body, K, vectorTime, nonces) == VerifyResult::badMac);

    auto path = vectorRequest();
    path.path = "/api/unpair";
    CHECK (verifyRequest (path, K, vectorTime, nonces) == VerifyResult::badMac);

    auto device = vectorRequest();
    device.deviceId = "someone-else";
    CHECK (verifyRequest (device, K, vectorTime, nonces) == VerifyResult::badMac);

    auto wrongKey = K;
    wrongKey[31] ^= 0x80;
    CHECK (verifyRequest (vectorRequest(), wrongKey, vectorTime, nonces) == VerifyResult::badMac);

    // A bad MAC doesn't burn the nonce: the real request still works
    CHECK_EQ (nonces.size(), (size_t) 0);
    CHECK (verifyRequest (vectorRequest(), K, vectorTime, nonces) == VerifyResult::ok);
}

TEST (verify_request_malformed)
{
    auto K = hexBytes (vectors()["signedRequest"]["K_hex"].s);
    NonceCache nonces;

    auto r1 = vectorRequest(); r1.nonce = "xyz";
    auto r2 = vectorRequest(); r2.time = "17600000000a0";
    auto r3 = vectorRequest(); r3.mac = "";
    auto r4 = vectorRequest(); r4.deviceId = "";
    CHECK (verifyRequest (r1, K, vectorTime, nonces) == VerifyResult::malformed);
    CHECK (verifyRequest (r2, K, vectorTime, nonces) == VerifyResult::malformed);
    CHECK (verifyRequest (r3, K, vectorTime, nonces) == VerifyResult::malformed);
    CHECK (verifyRequest (r4, K, vectorTime, nonces) == VerifyResult::malformed);
}

TEST (nonce_cache_window)
{
    NonceCache nonces (5 * 60 * 1000);
    CHECK (nonces.checkAndInsert ("d1", "n1", 1000));
    CHECK (! nonces.checkAndInsert ("d1", "n1", 1000 + 5 * 60 * 1000));      // Still inside 5 minutes
    CHECK (nonces.checkAndInsert ("d2", "n1", 2000));                        // Per device
    CHECK (nonces.checkAndInsert ("d1", "n1", 1000 + 5 * 60 * 1000 + 1));    // Expired
}
