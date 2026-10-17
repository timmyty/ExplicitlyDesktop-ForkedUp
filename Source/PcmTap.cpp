/*
  ==============================================================================

    PcmTap.cpp

  ==============================================================================
*/

#include "PcmTap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    inline void put16 (uint8_t* p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
    inline void put32 (uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t) (v >> (8 * i)); }
    inline void put64 (uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t) (v >> (8 * i)); }

    uint64_t roundUpToPowerOfTwo (uint64_t v)
    {
        uint64_t p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }
}

PcmTap::PcmTap (int capacityFrames)
    : capacity (roundUpToPowerOfTwo ((uint64_t) std::max (1024, capacityFrames))),
      mask (capacity - 1)
{
    ring.assign ((size_t) (capacity * channels), 0.0f);
}

int16_t PcmTap::toInt16 (float x) noexcept
{
    if (! (x == x))   // NaN
        return 0;
    x = std::min (1.0f, std::max (-1.0f, x));
    float v = x * 32767.0f;
    return (int16_t) (v >= 0.0f ? (int) (v + 0.5f) : -(int) (-v + 0.5f));
}

//==============================================================================
void PcmTap::push (const float* const* channelData, int numChannels, int numSamples,
                   int rate, uint16_t flags, uint32_t delaySamples) noexcept
{
    if (! active.load (std::memory_order_acquire) || numSamples <= 0)
        return;

    sampleRate.store (rate, std::memory_order_relaxed);
    currentFlags.store (flags, std::memory_order_relaxed);
    currentDelay.store (delaySamples, std::memory_order_relaxed);

    const uint64_t w = writeIndex.load (std::memory_order_relaxed);
    const uint64_t r = readIndex.load (std::memory_order_acquire);
    const uint64_t space = capacity - (w - r);

    // Report a gap from an earlier full ring before writing anything after it
    if (pendingGapCount > 0)
    {
        size_t tail = gapTail.load (std::memory_order_relaxed);
        if (tail - gapHead.load (std::memory_order_acquire) < gapCapacity)
        {
            gaps[tail % gapCapacity] = { pendingGapAt, pendingGapCount };
            gapTail.store (tail + 1, std::memory_order_release);
            pendingGapCount = 0;
        }
    }

    // The sender has stalled for seconds: drop this block and remember where
    if (pendingGapCount > 0 || (uint64_t) numSamples > space)
    {
        if (pendingGapCount == 0)
            pendingGapAt = w;
        pendingGapCount += (uint64_t) numSamples;
        droppedFrames.fetch_add ((uint64_t) numSamples, std::memory_order_relaxed);
        return;
    }

    const float* left  = numChannels > 0 ? channelData[0] : nullptr;
    const float* right = numChannels > 1 ? channelData[1] : left;

    for (int i = 0; i < numSamples; ++i)
    {
        float* dest = ring.data() + ((w + (uint64_t) i) & mask) * channels;
        dest[0] = left  != nullptr ? left[i]  : 0.0f;
        dest[1] = right != nullptr ? right[i] : 0.0f;
    }

    writeIndex.store (w + (uint64_t) numSamples, std::memory_order_release);
}

//==============================================================================
void PcmTap::startClient()
{
    // Consumer owns readIndex: skipping to the write position drops stale audio
    readIndex.store (writeIndex.load (std::memory_order_acquire), std::memory_order_release);
    gapHead.store (gapTail.load (std::memory_order_acquire), std::memory_order_release);

    clientStart = readIndex.load (std::memory_order_relaxed);
    streamOffset = 0 - clientStart;   // First frame starts at sample 0
    discontinuity = true;
    droppedFrames.store (0);
    active.store (true, std::memory_order_release);
}

void PcmTap::stopClient()
{
    active.store (false, std::memory_order_release);
}

size_t PcmTap::framesAvailable() const
{
    return (size_t) (writeIndex.load (std::memory_order_acquire) - readIndex.load (std::memory_order_relaxed));
}

bool PcmTap::applyGapsUpTo (uint64_t index)
{
    bool applied = false;
    for (;;)
    {
        size_t head = gapHead.load (std::memory_order_relaxed);
        if (head == gapTail.load (std::memory_order_acquire))
            break;

        const Gap& gap = gaps[head % gapCapacity];
        if (gap.at > index)
            break;

        if (gap.at >= clientStart)       // Ignore a drop left over from the previous client
        {
            streamOffset += gap.count;   // Those samples never reached the ring
            applied = true;
        }
        gapHead.store (head + 1, std::memory_order_release);
    }
    return applied;
}

bool PcmTap::popFrame (std::vector<uint8_t>& out, bool force)
{
    const int rate = std::max (1, sampleRate.load (std::memory_order_relaxed));
    const uint64_t frameSize = (uint64_t) std::max (1, rate / 50);   // 20 ms
    const uint64_t maxBacklog = (uint64_t) rate;                     // 1 s

    uint64_t r = readIndex.load (std::memory_order_relaxed);
    const uint64_t w = writeIndex.load (std::memory_order_acquire);

    // Slow client: keep only the newest second
    if (w - r > maxBacklog)
    {
        uint64_t skip = (w - r) - maxBacklog;
        r += skip;
        droppedFrames.fetch_add (skip, std::memory_order_relaxed);
        discontinuity = true;
        readIndex.store (r, std::memory_order_release);
    }

    if (applyGapsUpTo (r))
        discontinuity = true;

    // Don't let a frame straddle a gap: its samples must be contiguous
    uint64_t limit = frameSize;
    {
        size_t head = gapHead.load (std::memory_order_relaxed);
        if (head != gapTail.load (std::memory_order_acquire))
            limit = std::min (limit, gaps[head % gapCapacity].at - r);
    }

    const uint64_t available = w - r;
    uint64_t n = std::min (limit, available);
    if (n == 0 || (n < limit && ! force))
        return false;

    uint16_t flags = currentFlags.load (std::memory_order_relaxed);
    flags = (uint16_t) ((flags & ~flagDiscontinuity) | (discontinuity ? flagDiscontinuity : 0));

    out.resize (headerSize + (size_t) n * channels * 2);
    uint8_t* p = out.data();
    std::memcpy (p, "EXPC", 4);
    put32 (p + 4, (uint32_t) rate);
    put16 (p + 8, (uint16_t) channels);
    put16 (p + 10, flags);
    put64 (p + 12, r + streamOffset);
    put32 (p + 20, (uint32_t) n);
    put32 (p + 24, currentDelay.load (std::memory_order_relaxed));

    uint8_t* pcm = p + headerSize;
    for (uint64_t i = 0; i < n; ++i)
    {
        const float* src = ring.data() + ((r + i) & mask) * channels;
        for (int c = 0; c < channels; ++c)
        {
            put16 (pcm, (uint16_t) toInt16 (src[c]));
            pcm += 2;
        }
    }

    readIndex.store (r + n, std::memory_order_release);
    discontinuity = false;
    return true;
}
