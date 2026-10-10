#pragma once

#include <atomic>
#include <cstdint>

/** What the audio thread knows about the host transport at the start of a block. */
struct TransportSnapshot
{
    bool playing = false;
    double positionSeconds = 0.0;   // host timeline position of the block's first sample
    double wallMs = 0.0;            // Time::getMillisecondCounterHiRes() when the block was processed
    uint32_t discontinuity = 0;     // bumped on start, locate or loop wrap
    uint32_t blockCounter = 0;      // bumped every block
};

/** Single-producer (audio thread), multi-consumer seqlock. Wait-free for the
    writer, lock-free for readers, no allocation. */
class TransportExchange
{
public:
    void write (const TransportSnapshot& s) noexcept
    {
        const auto v = sequence.load (std::memory_order_relaxed);
        sequence.store (v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);

        playing.store (s.playing, std::memory_order_relaxed);
        position.store (s.positionSeconds, std::memory_order_relaxed);
        wall.store (s.wallMs, std::memory_order_relaxed);
        discontinuity.store (s.discontinuity, std::memory_order_relaxed);
        blockCounter.store (s.blockCounter, std::memory_order_relaxed);

        sequence.store (v + 2, std::memory_order_release);
    }

    TransportSnapshot read() const noexcept
    {
        for (;;)
        {
            const auto before = sequence.load (std::memory_order_acquire);

            if ((before & 1) != 0)
                continue;

            TransportSnapshot s;
            s.playing         = playing.load (std::memory_order_relaxed);
            s.positionSeconds = position.load (std::memory_order_relaxed);
            s.wallMs          = wall.load (std::memory_order_relaxed);
            s.discontinuity   = discontinuity.load (std::memory_order_relaxed);
            s.blockCounter    = blockCounter.load (std::memory_order_relaxed);

            std::atomic_thread_fence (std::memory_order_acquire);

            if (sequence.load (std::memory_order_relaxed) == before)
                return s;
        }
    }

private:
    std::atomic<uint32_t> sequence { 0 };
    std::atomic<bool> playing { false };
    std::atomic<double> position { 0.0 }, wall { 0.0 };
    std::atomic<uint32_t> discontinuity { 0 }, blockCounter { 0 };
};
