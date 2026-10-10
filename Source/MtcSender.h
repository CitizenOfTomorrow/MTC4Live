#pragma once

#include "CoreMidiOutput.h"
#include "MtcMessages.h"
#include "TransportExchange.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>

/** Owns the MIDI output and generates MTC on its own thread, following the
    transport snapshots published by the audio thread.

    Threading:
      - audio thread: only writes the TransportExchange (never touches this class)
      - message thread: setOutput() (CoreMIDI endpoints are created/destroyed there)
      - sender thread: reads transport + settings, sends MIDI
    The output mutex is shared only by the message and sender threads. */
class MtcSender : private juce::Thread
{
public:
    struct Settings
    {
        std::atomic<float>* frameRateIndex = nullptr;   // 0..3, see mtc::FrameRate
        std::atomic<float>* latencyMs = nullptr;        // positive = send earlier
        std::atomic<float>* active = nullptr;           // > 0.5 = send
        std::atomic<float>* fullFrameWhenStopped = nullptr;
    };

    MtcSender (const TransportExchange&, Settings);
    ~MtcSender() override;

    void startSending();

    /** Message thread only. Pass nullptr to close. The previous output is
        destroyed on the calling thread, outside the lock. */
    void setOutput (std::unique_ptr<CoreMidiOutput>);

    bool hasOutput() const;
    juce::String getOutputIdentifier() const;

    void setStartOffset (const mtc::Timecode& tc) noexcept { startOffset.store (mtc::pack (tc)); }
    mtc::Timecode getStartOffset() const noexcept         { return mtc::unpack (startOffset.load()); }

    // Status for the UI
    mtc::Timecode getCurrentTimecode() const noexcept { return mtc::unpack (currentTimecode.load()); }
    bool isGenerating() const noexcept                { return generating.load(); }

private:
    void run() override;

    // Sender thread only. atMs is the intended output time on the
    // getMillisecondCounterHiRes() clock; stamps are kept strictly increasing.
    void sendBytes (const uint8_t* data, int size, double atMs);
    void sendQuarterFrame (uint8_t data, double atMs);
    void sendFullFrame (const mtc::Timecode&, mtc::FrameRate, double atMs);
    void flushScheduled (double now);

    double lastStampMs = 0.0; // sender thread only

    const TransportExchange& transport;
    const Settings settings;

    mutable std::mutex outputMutex;
    std::unique_ptr<CoreMidiOutput> output;
    juce::String outputIdentifier;
    std::atomic<uint32_t> outputGeneration { 0 };

    std::atomic<uint32_t> startOffset { mtc::pack ({ 1, 0, 0, 0 }) };
    std::atomic<uint32_t> currentTimecode { 0 };
    std::atomic<bool> generating { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MtcSender)
};
