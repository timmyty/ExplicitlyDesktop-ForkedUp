/*
  ==============================================================================

    PcmTapTests.cpp

    The /pcm frame format (docs/STREAMING_PROTOCOL.md section 5.1), frame
    counter continuity, the 1 s cap and drops when the ring is full.

  ==============================================================================
*/

#include "TestFramework.h"
#include "PcmTap.h"
#include "StreamCrypto.h"

#include <cmath>
#include <cstring>
#include <thread>

namespace
{
    struct Header
    {
        char magic[5] {};
        uint32_t sampleRate = 0;
        uint16_t channels = 0, flags = 0;
        uint64_t startSample = 0;
        uint32_t frames = 0, delaySamples = 0;
    };

    uint64_t le (const uint8_t* p, int bytes)
    {
        uint64_t v = 0;
        for (int i = bytes - 1; i >= 0; --i)
            v = (v << 8) | p[i];
        return v;
    }

    Header parse (const std::vector<uint8_t>& f)
    {
        Header h;
        std::memcpy (h.magic, f.data(), 4);
        h.sampleRate   = (uint32_t) le (f.data() + 4, 4);
        h.channels     = (uint16_t) le (f.data() + 8, 2);
        h.flags        = (uint16_t) le (f.data() + 10, 2);
        h.startSample  = le (f.data() + 12, 8);
        h.frames       = (uint32_t) le (f.data() + 20, 4);
        h.delaySamples = (uint32_t) le (f.data() + 24, 4);
        return h;
    }

    int16_t sampleAt (const std::vector<uint8_t>& f, size_t index)
    {
        return (int16_t) le (f.data() + PcmTap::headerSize + index * 2, 2);
    }

    /** Pushes n frames of a ramp (left = counter, right = -counter, scaled). */
    void pushRamp (PcmTap& tap, int n, int rate, uint64_t& counter, uint16_t flags = 0, uint32_t delay = 0)
    {
        std::vector<float> l ((size_t) n), r ((size_t) n);
        for (int i = 0; i < n; ++i, ++counter)
        {
            l[(size_t) i] = (float) (counter % 1000) / 1000.0f;
            r[(size_t) i] = -l[(size_t) i];
        }
        const float* ch[] = { l.data(), r.data() };
        tap.push (ch, 2, n, rate, flags, delay);
    }
}

TEST (pcm_inactive_push_is_ignored)
{
    PcmTap tap;
    uint64_t c = 0;
    pushRamp (tap, 4800, 48000, c);
    CHECK_EQ (tap.framesAvailable(), (size_t) 0);

    std::vector<uint8_t> out;
    CHECK (! tap.popFrame (out, true));
}

TEST (pcm_header_matches_vector)
{
    PcmTap tap;
    tap.startClient();

    float l[] = { 0.0f, -1.0f }, r[] = { 1.0f, 0.5f };
    const float* ch[] = { l, r };
    tap.push (ch, 2, 2, 48000, PcmTap::flagFilterActive, 168000);

    std::vector<uint8_t> out;
    CHECK (! tap.popFrame (out));          // Less than 20 ms waiting
    CHECK (tap.popFrame (out, true));

    // tests/vectors/protocol_vectors.json "pcmFrame"
    CHECK_EQ (out.size(), PcmTap::headerSize + 8);
    CHECK_EQ (StreamCrypto::toHex (out), std::string ("4558504380bb000002000300000000000000000002000000409002000000ff7f01800040"));
}

TEST (pcm_int16_conversion)
{
    CHECK_EQ (PcmTap::toInt16 (0.0f), (int16_t) 0);
    CHECK_EQ (PcmTap::toInt16 (1.0f), (int16_t) 32767);
    CHECK_EQ (PcmTap::toInt16 (-1.0f), (int16_t) -32767);
    CHECK_EQ (PcmTap::toInt16 (2.0f), (int16_t) 32767);
    CHECK_EQ (PcmTap::toInt16 (-3.0f), (int16_t) -32767);
    CHECK_EQ (PcmTap::toInt16 (0.5f), (int16_t) 16384);
    CHECK_EQ (PcmTap::toInt16 (-0.5f), (int16_t) -16384);
    CHECK_EQ (PcmTap::toInt16 (std::nanf ("")), (int16_t) 0);
}

TEST (pcm_frames_are_20ms_and_continuous)
{
    PcmTap tap;
    tap.startClient();
    uint64_t c = 0;

    // Callback-sized blocks that don't divide 20 ms
    for (int i = 0; i < 30; ++i)
        pushRamp (tap, 512, 48000, c, PcmTap::flagFilterActive | PcmTap::flagHeld, 1234);

    std::vector<uint8_t> out;
    uint64_t expectStart = 0;
    int frames = 0;
    while (tap.popFrame (out))
    {
        auto h = parse (out);
        CHECK_EQ (std::string (h.magic), std::string ("EXPC"));
        CHECK_EQ (h.sampleRate, 48000u);
        CHECK_EQ (h.channels, (uint16_t) 2);
        CHECK_EQ (h.frames, 960u);
        CHECK_EQ (out.size(), PcmTap::headerSize + 960 * 4);
        CHECK_EQ (h.startSample, expectStart);
        CHECK_EQ (h.delaySamples, 1234u);
        CHECK_EQ ((int) (h.flags & PcmTap::flagDiscontinuity), frames == 0 ? 1 : 0);   // Only the first
        CHECK (h.flags & PcmTap::flagFilterActive);
        CHECK (h.flags & PcmTap::flagHeld);

        // Samples carry on from the previous frame, left/right interleaved
        CHECK_EQ (sampleAt (out, 0), PcmTap::toInt16 ((float) (expectStart % 1000) / 1000.0f));
        CHECK_EQ (sampleAt (out, 1), PcmTap::toInt16 (-(float) (expectStart % 1000) / 1000.0f));

        expectStart += h.frames;
        ++frames;
    }
    CHECK_EQ (frames, 30 * 512 / 960);
    CHECK_EQ (tap.framesAvailable(), (size_t) (30 * 512 - frames * 960));
}

TEST (pcm_mono_input_is_duplicated)
{
    PcmTap tap;
    tap.startClient();
    float m[] = { 0.25f, 0.25f };
    const float* ch[] = { m };
    tap.push (ch, 1, 2, 48000, 0, 0);

    std::vector<uint8_t> out;
    CHECK (tap.popFrame (out, true));
    CHECK_EQ (sampleAt (out, 0), sampleAt (out, 1));
    CHECK_EQ (sampleAt (out, 0), PcmTap::toInt16 (0.25f));
}

TEST (pcm_backlog_capped_at_one_second)
{
    PcmTap tap;
    tap.startClient();
    uint64_t c = 0;

    // A stalled client: 1.5 s waiting
    for (int i = 0; i < 75; ++i)
        pushRamp (tap, 960, 48000, c);
    CHECK_EQ (tap.framesAvailable(), (size_t) 72000);

    std::vector<uint8_t> out;
    CHECK (tap.popFrame (out));
    auto h = parse (out);
    CHECK_EQ (h.startSample, (uint64_t) 24000);          // The oldest 0.5 s was dropped
    CHECK (h.flags & PcmTap::flagDiscontinuity);
    CHECK_EQ (tap.getDroppedFrames(), (uint64_t) 24000);
    CHECK_EQ (sampleAt (out, 0), PcmTap::toInt16 ((float) (24000 % 1000) / 1000.0f));

    CHECK (tap.popFrame (out));
    h = parse (out);
    CHECK_EQ (h.startSample, (uint64_t) 24960);
    CHECK_EQ ((int) (h.flags & PcmTap::flagDiscontinuity), 0);

    int count = 2;
    while (tap.popFrame (out))
        ++count;
    CHECK_EQ (count, 50);                                // Exactly the newest second was sent
}

TEST (pcm_full_ring_records_a_gap)
{
    PcmTap tap (2048);   // Smaller than the 1 s cap, so the ring itself overflows
    tap.startClient();
    uint64_t c = 0;

    pushRamp (tap, 2048, 48000, c);      // Fills the ring
    pushRamp (tap, 512, 48000, c);       // Dropped: no space

    std::vector<uint8_t> out;
    CHECK (tap.popFrame (out));
    CHECK_EQ (parse (out).startSample, (uint64_t) 0);
    CHECK (tap.popFrame (out));
    CHECK_EQ (parse (out).startSample, (uint64_t) 960);
    CHECK (! tap.popFrame (out));       // 128 frames left: not a full frame yet

    pushRamp (tap, 100, 48000, c);       // Written after the gap

    // The 128 frames before the gap go out on their own, without the flag
    CHECK (tap.popFrame (out));
    auto h = parse (out);
    CHECK_EQ (h.startSample, (uint64_t) 1920);
    CHECK_EQ (h.frames, 128u);
    CHECK_EQ ((int) (h.flags & PcmTap::flagDiscontinuity), 0);

    CHECK (! tap.popFrame (out));
    CHECK (tap.popFrame (out, true));
    h = parse (out);
    CHECK_EQ (h.startSample, (uint64_t) (2048 + 512));   // Counter skips the dropped samples
    CHECK_EQ (h.frames, 100u);
    CHECK (h.flags & PcmTap::flagDiscontinuity);
    CHECK_EQ (sampleAt (out, 0), PcmTap::toInt16 ((float) (2560 % 1000) / 1000.0f));
    CHECK_EQ (tap.getDroppedFrames(), (uint64_t) 512);
}

TEST (pcm_reconnect_starts_fresh)
{
    PcmTap tap;
    tap.startClient();
    uint64_t c = 0;
    pushRamp (tap, 4800, 48000, c);
    tap.stopClient();

    pushRamp (tap, 4800, 48000, c);      // Ignored: nobody listening
    tap.startClient();
    CHECK_EQ (tap.framesAvailable(), (size_t) 0);

    pushRamp (tap, 960, 48000, c);
    std::vector<uint8_t> out;
    CHECK (tap.popFrame (out));
    auto h = parse (out);
    CHECK_EQ (h.startSample, (uint64_t) 0);
    CHECK (h.flags & PcmTap::flagDiscontinuity);
}

TEST (pcm_concurrent_producer_and_consumer)
{
    // The audio thread and the sender thread at once: every frame's first sample
    // must match its startSample, whatever got dropped along the way
    PcmTap tap (8192);
    tap.startClient();
    std::atomic<bool> done { false };

    std::thread producer ([&]
    {
        uint64_t c = 0;
        for (int block = 0; block < 2000; ++block)
        {
            pushRamp (tap, 256, 48000, c);
            if (block % 50 == 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        done = true;
    });

    std::vector<uint8_t> out;
    uint64_t lastEnd = 0;
    int frames = 0, mismatches = 0, backwards = 0;
    while (! done.load() || tap.framesAvailable() > 0)
    {
        if (! tap.popFrame (out, done.load()))
        {
            std::this_thread::yield();
            continue;
        }
        auto h = parse (out);
        if (sampleAt (out, 0) != PcmTap::toInt16 ((float) (h.startSample % 1000) / 1000.0f))
            ++mismatches;
        if (h.startSample < lastEnd || (h.startSample > lastEnd && ! (h.flags & PcmTap::flagDiscontinuity)))
            ++backwards;
        lastEnd = h.startSample + h.frames;
        ++frames;
    }
    producer.join();

    CHECK (frames > 0);
    CHECK_EQ (mismatches, 0);
    CHECK_EQ (backwards, 0);   // Gaps only ever come with the discontinuity flag
    CHECK_EQ (lastEnd, (uint64_t) (2000 * 256));
}
