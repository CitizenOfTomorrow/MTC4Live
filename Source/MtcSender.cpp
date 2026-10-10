#include "MtcSender.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace
{
    // No new audio block for this long while "playing" -> treat as stopped
    // (track frozen, device disabled, audio engine halted).
    constexpr double staleTransportMs = 250.0;

    // Smoothing of the audio-clock -> wall-clock mapping. Each block nudges the
    // estimate by this fraction, averaging out callback scheduling jitter while
    // still following sample-clock drift.
    constexpr double anchorSmoothing = 0.02;

    // If the sender is this many frames behind schedule, relocate instead of
    // bursting out the backlog.
    constexpr double maxLagFrames = 2.0;

    // Quarter frames are handed to CoreMIDI this far ahead, timestamped with
    // their exact due time, so late thread wakeups don't show up as jitter.
    // After a locate on a virtual port, up to this much stale MTC still goes out
    // (followed by the full frame).
    constexpr double lookaheadMs = 20.0;

    // Longest sleep while generating, so transport changes are picked up quickly.
    constexpr double maxSleepPlayingMs = 4.0;
    constexpr int idleSleepMs = 10;

    void sleepMs (double ms)
    {
        if (ms > 0.0)
            std::this_thread::sleep_for (std::chrono::microseconds (static_cast<int64_t> (ms * 1000.0)));
    }
}

MtcSender::MtcSender (const TransportExchange& t, Settings s)
    : juce::Thread ("MTC4Live sender"), transport (t), settings (s)
{
    jassert (settings.frameRateIndex != nullptr && settings.latencyMs != nullptr
             && settings.active != nullptr && settings.fullFrameWhenStopped != nullptr);
}

MtcSender::~MtcSender()
{
    stopThread (2000);
    setOutput (nullptr);
}

void MtcSender::startSending()
{
    // Time-constraint thread: wakes up on time for each quarter frame. It does
    // very little work per wakeup. Falls back to a normal high-priority thread.
    const auto options = juce::Thread::RealtimeOptions{}
                             .withPriority (8)
                             .withProcessingTimeMs (0.1)
                             .withMaximumProcessingTimeMs (1.0);

    if (! startRealtimeThread (options))
        startThread (juce::Thread::Priority::highest);
}

void MtcSender::setOutput (std::unique_ptr<CoreMidiOutput> newOutput)
{
    const auto newId = newOutput != nullptr ? newOutput->getIdentifier() : juce::String();

    {
        const std::lock_guard<std::mutex> lock (outputMutex);
        std::swap (output, newOutput);
        outputIdentifier = newId;
    }

    ++outputGeneration;
    newOutput.reset(); // old device closes here, outside the lock
}

bool MtcSender::hasOutput() const
{
    const std::lock_guard<std::mutex> lock (outputMutex);
    return output != nullptr;
}

juce::String MtcSender::getOutputIdentifier() const
{
    const std::lock_guard<std::mutex> lock (outputMutex);
    return outputIdentifier;
}

void MtcSender::sendBytes (const uint8_t* data, int size, double atMs)
{
    // Never schedule before something already sent, so a full frame can't
    // overtake quarter frames that are still in flight.
    lastStampMs = juce::jmax (atMs, lastStampMs + 0.01);

    const std::lock_guard<std::mutex> lock (outputMutex);
    if (output != nullptr)
        output->send (data, size, lastStampMs);
}

void MtcSender::sendQuarterFrame (uint8_t data, double atMs)
{
    const uint8_t bytes[] { mtc::quarterFrameStatus, data };
    sendBytes (bytes, 2, atMs);
}

void MtcSender::sendFullFrame (const mtc::Timecode& tc, mtc::FrameRate rate, double atMs)
{
    const auto bytes = mtc::fullFrameSysEx (tc, rate);
    sendBytes (bytes.data(), static_cast<int> (bytes.size()), atMs);
}

void MtcSender::flushScheduled (double now)
{
    const std::lock_guard<std::mutex> lock (outputMutex);

    // Destinations can drop not-yet-delivered messages; virtual ports can't,
    // so there the stamps stay ordered after what's in flight.
    if (output != nullptr && output->flushPending())
        lastStampMs = juce::jmin (lastStampMs, now);
}

void MtcSender::run()
{
    mtc::QuarterFrameGenerator generator;

    bool running = false;
    uint32_t lastDiscontinuity = 0, lastBlock = 0, lastOutputGeneration = 0, lastOffset = 0;
    int lastRateIndex = -1;
    double anchorMs = 0.0; // wall time at which the host timeline was at 0 s

    // While stopped: last full frame we sent, to only resend on change
    int64_t lastStoppedFrame = std::numeric_limits<int64_t>::min();
    int lastStoppedRate = -1;
    uint32_t lastStoppedOffset = 0;

    while (! threadShouldExit())
    {
        const auto snap = transport.read();
        const auto now = juce::Time::getMillisecondCounterHiRes();

        const auto rateIndex = juce::jlimit (0, 3, static_cast<int> (settings.frameRateIndex->load()));
        const auto rate = mtc::frameRateFromIndex (rateIndex);
        const auto fps = mtc::actualFps (rate);
        const auto offsetPacked = startOffset.load();
        const auto offsetFrames = mtc::timecodeToFrames (mtc::unpack (offsetPacked), rate);
        const auto latencyMs = static_cast<double> (settings.latencyMs->load());
        const auto outGen = outputGeneration.load();
        const bool active = settings.active->load() > 0.5f;

        const bool fresh = snap.blockCounter != 0 && now - snap.wallMs < staleTransportMs;

        if (! (fresh && snap.playing && active))
        {
            if (running)
                flushScheduled (now);

            running = false;
            generating = false;

            const auto frame = mtc::secondsToFrames (snap.positionSeconds, rate);
            const auto tc = mtc::framesToTimecode (frame + offsetFrames, rate);
            currentTimecode = mtc::pack (tc);

            // Park the receiver on the stopped position (and follow locates
            // while stopped).
            if (fresh && active && settings.fullFrameWhenStopped->load() > 0.5f)
            {
                if (frame != lastStoppedFrame || rateIndex != lastStoppedRate
                    || offsetPacked != lastStoppedOffset || outGen != lastOutputGeneration)
                {
                    sendFullFrame (tc, rate, now);
                    lastStoppedFrame = frame;
                    lastStoppedRate = rateIndex;
                    lastStoppedOffset = offsetPacked;
                    lastOutputGeneration = outGen;
                }
            }

            wait (idleSleepMs);
            continue;
        }

        lastStoppedFrame = std::numeric_limits<int64_t>::min();

        const auto measuredAnchor = snap.wallMs - snap.positionSeconds * 1000.0;

        bool resync = ! running
                   || snap.discontinuity != lastDiscontinuity
                   || rateIndex != lastRateIndex
                   || offsetPacked != lastOffset
                   || outGen != lastOutputGeneration;

        if (! resync && snap.blockCounter != lastBlock)
            anchorMs += (measuredAnchor - anchorMs) * anchorSmoothing;

        lastBlock = snap.blockCounter;

        auto positionFrames = (now - anchorMs + latencyMs) * 0.001 * fps;

        if (! resync && positionFrames - generator.nextDueFrame() > maxLagFrames)
            resync = true;

        if (resync)
        {
            if (running)
                flushScheduled (now);

            anchorMs = measuredAnchor;
            positionFrames = (now - anchorMs + latencyMs) * 0.001 * fps;

            // The full frame goes out after anything still in flight; encode
            // the frame playing at that moment.
            const auto fullFrameAt = juce::jmax (now, lastStampMs + 0.01);
            const auto fullFramePosition = (fullFrameAt - anchorMs + latencyMs) * 0.001 * fps;
            const auto currentFrame = static_cast<int64_t> (std::floor (fullFramePosition));
            sendFullFrame (mtc::framesToTimecode (currentFrame + offsetFrames, rate), rate, fullFrameAt);

            // Quarter frames restart at piece 0 on the next frame boundary
            generator.restart (currentFrame + 1);

            running = true;
            generating = true;
            lastDiscontinuity = snap.discontinuity;
            lastRateIndex = rateIndex;
            lastOffset = offsetPacked;
            lastOutputGeneration = outGen;
        }

        currentTimecode = mtc::pack (mtc::framesToTimecode (
            static_cast<int64_t> (std::floor (positionFrames)) + offsetFrames, rate));

        // Hand over everything due within the lookahead, stamped with its due
        // time (host frame position -> wall time).
        const auto horizonFrames = positionFrames + lookaheadMs * 0.001 * fps;

        while (generator.nextDueFrame() <= horizonFrames)
        {
            const auto dueMs = anchorMs - latencyMs + generator.nextDueFrame() / fps * 1000.0;
            sendQuarterFrame (generator.next (offsetFrames, rate), dueMs);
        }

        const auto untilNextMs = (generator.nextDueFrame() - horizonFrames) / fps * 1000.0;
        sleepMs (juce::jmin (untilNextMs, maxSleepPlayingMs));
    }

    generating = false;
}
