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

    // Longest sleep while generating, so transport changes are picked up quickly.
    constexpr double maxSleepPlayingMs = 2.0;
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

void MtcSender::sendBytes (const uint8_t* data, int size)
{
    const std::lock_guard<std::mutex> lock (outputMutex);
    if (output != nullptr)
        output->send (data, size);
}

void MtcSender::sendQuarterFrame (uint8_t data)
{
    const uint8_t bytes[] { mtc::quarterFrameStatus, data };
    sendBytes (bytes, 2);
}

void MtcSender::sendFullFrame (const mtc::Timecode& tc, mtc::FrameRate rate)
{
    const auto bytes = mtc::fullFrameSysEx (tc, rate);
    sendBytes (bytes.data(), static_cast<int> (bytes.size()));
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
                    sendFullFrame (tc, rate);
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
            anchorMs = measuredAnchor;
            positionFrames = (now - anchorMs + latencyMs) * 0.001 * fps;

            const auto currentFrame = static_cast<int64_t> (std::floor (positionFrames));
            sendFullFrame (mtc::framesToTimecode (currentFrame + offsetFrames, rate), rate);

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

        // Send everything that is due (normally exactly one message)
        while (generator.nextDueFrame() <= positionFrames)
            sendQuarterFrame (generator.next (offsetFrames, rate));

        const auto untilNextMs = (generator.nextDueFrame() - positionFrames) / fps * 1000.0;
        sleepMs (juce::jmin (untilNextMs, maxSleepPlayingMs));
    }

    generating = false;
}
