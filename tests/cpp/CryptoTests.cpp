/*
  ==============================================================================

    CryptoTests.cpp

    StreamCrypto against published test vectors (FIPS 180, RFC 4231,
    RFC 4648, RFC 6455).

  ==============================================================================
*/

#include "TestFramework.h"
#include "StreamCrypto.h"

#include <cctype>
#include <cstring>

using namespace StreamCrypto;

namespace
{
    std::string sha1Hex (const std::string& s)   { return toHex (sha1 (s.data(), s.size())); }
    std::string sha256HexOf (const std::string& s) { return toHex (sha256 (s.data(), s.size())); }
}

TEST (sha1_vectors)
{
    CHECK_EQ (sha1Hex (""), std::string ("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ (sha1Hex ("abc"), std::string ("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ (sha1Hex ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              std::string ("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    CHECK_EQ (sha1Hex (std::string (1000000, 'a')), std::string ("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

TEST (sha256_vectors)
{
    CHECK_EQ (sha256HexOf (""), std::string ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ (sha256HexOf ("abc"), std::string ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ (sha256HexOf ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              std::string ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ (sha256HexOf (std::string (1000000, 'a')),
              std::string ("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    CHECK_EQ (sha256Hex ("abc"), sha256HexOf ("abc"));

    // Lengths around the padding boundary (55, 56, 63, 64 bytes) agree with a split computation
    for (size_t len : { 55u, 56u, 63u, 64u, 65u, 119u, 120u })
        CHECK_EQ (sha256HexOf (std::string (len, 'x')).size(), (size_t) 64);
    CHECK_EQ (sha256HexOf (std::string (55, 'a')), std::string ("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    CHECK_EQ (sha256HexOf (std::string (56, 'a')), std::string ("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    CHECK_EQ (sha256HexOf (std::string (64, 'a')), std::string ("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
}

TEST (hmac_rfc4231)
{
    // Test case 1
    Bytes key1 (20, 0x0b);
    CHECK_EQ (hmacHex (key1, "Hi There"), std::string ("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    // Test case 2
    Bytes jefe { 'J', 'e', 'f', 'e' };
    CHECK_EQ (hmacHex (jefe, "what do ya want for nothing?"),
              std::string ("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    // Test case 6: key longer than the block size
    Bytes key6 (131, 0xaa);
    CHECK_EQ (hmacHex (key6, "Test Using Larger Than Block-Size Key - Hash Key First"),
              std::string ("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

    // Test case 7
    CHECK_EQ (hmacHex (key6, "This is a test using a larger than block-size key and a larger than block-size data. "
                             "The key needs to be hashed before being used by the HMAC algorithm."),
              std::string ("9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"));
}

TEST (hex_round_trip)
{
    Bytes b { 0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff };
    CHECK_EQ (toHex (b), std::string ("00017f80feff"));

    Bytes back;
    CHECK (fromHex ("00017F80FEff", back));
    CHECK (back == b);
    CHECK (! fromHex ("abc", back));
    CHECK (! fromHex ("zz", back));
    CHECK (fromHex ("", back) && back.empty());
}

TEST (base64_rfc4648)
{
    auto b64 = [] (const std::string& s) { return base64Encode (s.data(), s.size()); };
    CHECK_EQ (b64 (""), std::string (""));
    CHECK_EQ (b64 ("f"), std::string ("Zg=="));
    CHECK_EQ (b64 ("fo"), std::string ("Zm8="));
    CHECK_EQ (b64 ("foo"), std::string ("Zm9v"));
    CHECK_EQ (b64 ("foob"), std::string ("Zm9vYg=="));
    CHECK_EQ (b64 ("foobar"), std::string ("Zm9vYmFy"));
}

TEST (base32_rfc4648)
{
    const char* plain[]   = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    const char* encoded[] = { "", "MY", "MZXQ", "MZXW6", "MZXW6YQ", "MZXW6YTB", "MZXW6YTBOI" };

    for (int i = 0; i < 7; ++i)
    {
        Bytes in (plain[i], plain[i] + std::strlen (plain[i]));
        CHECK_EQ (base32Encode (in), std::string (encoded[i]));

        Bytes out;
        CHECK (base32Decode (encoded[i], out));
        CHECK (out == in);
    }
}

TEST (base32_round_trip_and_typed_codes)
{
    for (size_t len = 0; len < 40; ++len)
    {
        auto data = randomBytes (len);
        Bytes back;
        CHECK (base32Decode (base32Encode (data), back));
        CHECK (back == data);
    }

    // Typed pairing code: groups of 4, any case
    auto P = randomBytes (16);
    auto code = base32Encode (P);
    CHECK_EQ (code.size(), (size_t) 26);

    std::string typed;
    for (size_t i = 0; i < code.size(); ++i)
    {
        if (i > 0 && i % 4 == 0)
            typed += '-';
        typed += (char) std::tolower ((unsigned char) code[i]);
    }
    Bytes back;
    CHECK (base32Decode (typed, back));
    CHECK (back == P);

    CHECK (! base32Decode ("MZXW1", back));   // '1' isn't base32
    CHECK (! base32Decode ("MZ", back));      // Non-zero leftover bits ("MY" is "f")
    CHECK (! base32Decode ("MZXW6Y", back));  // 6 chars is never a whole number of bytes
}

TEST (constant_time_equals)
{
    CHECK (constantTimeEquals ("abc", "abc"));
    CHECK (! constantTimeEquals ("abc", "abd"));
    CHECK (! constantTimeEquals ("abc", "abcd"));
    CHECK (constantTimeEquals ("", ""));
}

TEST (random_bytes)
{
    auto a = randomBytes (32), b = randomBytes (32);
    CHECK_EQ (a.size(), (size_t) 32);
    CHECK (a != b);
    CHECK (a != Bytes (32, 0));
}

TEST (websocket_accept_rfc6455)
{
    // RFC 6455 section 1.3
    CHECK_EQ (webSocketAccept ("dGhlIHNhbXBsZSBub25jZQ=="), std::string ("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}
