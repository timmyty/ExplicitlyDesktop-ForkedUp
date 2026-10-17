/*
  ==============================================================================

    PcmTap.h

    Copies the app's final output (after muting/Reverse, before the speaker
    mute) to the browser extension for phone streaming.

    The audio thread calls push() at the end of every callback. It is
    wait-free: no locks, no allocation, no logging, and a single atomic load
    when no client is connected. A sender thread calls popFrame() to pack 20 ms
    frames in the /pcm wire format (docs/STREAMING_PROTOCOL.md section 5.1):

        "EXPC" | u32 sampleRate | u16 channels | u16 flags | u64 startSample
               | u32 frames | u32 delaySamples | int16[frames * channels]

    At most one second is kept for a slow client; older audio is dropped and
    the next frame carries the discontinuity flag. No JUCE, so the unit tests
    build anywhere.

  ==============================================================================
*/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class PcmTap
{
public:
    static constexpr int channels = 2;
    static constexpr size_t headerSize = 28;

    enum Flags : uint16_t
    {
        flagDiscontinuity = 1 << 0,   // Audio was dropped (or the client just joined) before this frame
        flagFilterActive  = 1 << 1,   // Filter on and not bypassed: this is delayed, censored audio
        flagHeld          = 1 << 2    // The delay pipeline is frozen (pause): output is silence
    };

    /** Allocates the ring (about 5 s at 96 kHz). Call off the audio thread. */
    explicit PcmTap (int capacityFrames = 1 << 19);

    //==============================================================================
    /** Audio thread. Copies up to two channels (mono is duplicated; a null
        channel is silence). Does nothing unless a client is connected. */
    void push (const float* const* channelData, int numChannels, int numSamples,
               int sampleRate, uint16_t flags, uint32_t delaySamples) noexcept;

    /** True while a client is connected; the audio thread checks this first. */
    bool isActive() const noexcept { return active.load (std::memory_order_acquire); }

    //==============================================================================
    // Consumer side: one sender thread at a time.

    /** A client connected: drops anything stale and starts a new run whose
        first frame has the discontinuity flag. */
    void startClient();

    /** The client went away: push() goes back to doing nothing. */
    void stopClient();

    /** Packs the next frame (sampleRate / 50 samples, or fewer with force)
        into out. False if not enough audio is waiting yet. */
    bool popFrame (std::vector<uint8_t>& out, bool force = false);

    /** Samples waiting in the ring (after the 1 s cap is applied by popFrame). */
    size_t framesAvailable() const;

    /** Frames dropped by the 1 s cap or a full ring since startClient (for logs). */
    uint64_t getDroppedFrames() const { return droppedFrames.load(); }

    int getSampleRate() const { return sampleRate.load(); }

    /** Converts one float sample to the wire's int16 (clamped, rounded). */
    static int16_t toInt16 (float x) noexcept;

private:
    struct Gap { uint64_t at; uint64_t count; };   // Ring index where `count` frames were dropped
    static constexpr size_t gapCapacity = 64;

    bool applyGapsUpTo (uint64_t index);

    std::vector<float> ring;            // Interleaved stereo
    uint64_t capacity;                  // Frames, power of two
    uint64_t mask;

    alignas (64) std::atomic<uint64_t> writeIndex { 0 };   // Producer
    alignas (64) std::atomic<uint64_t> readIndex { 0 };    // Consumer

    // Gaps recorded by the producer when the ring is full (SPSC)
    Gap gaps[gapCapacity] {};
    std::atomic<size_t> gapHead { 0 }, gapTail { 0 };
    uint64_t pendingGapAt = 0, pendingGapCount = 0;        // Producer only

    std::atomic<bool> active { false };
    std::atomic<int> sampleRate { 48000 };
    std::atomic<uint16_t> currentFlags { 0 };
    std::atomic<uint32_t> currentDelay { 0 };
    std::atomic<uint64_t> droppedFrames { 0 };

    // Consumer only
    uint64_t streamOffset = 0;          // startSample = ring index + streamOffset
    uint64_t clientStart = 0;           // Ring index where this client's stream began
    bool discontinuity = true;
};
