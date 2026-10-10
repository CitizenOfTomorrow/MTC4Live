// Developer tool, not shipped.
//
//   MTC4LiveTool snapshot <out.png>   Render the editor to a PNG.
//   MTC4LiveTool e2e                  Drive the real processor with a fake
//                                     playhead in real time, receive its
//                                     virtual MIDI port via CoreMIDI, decode
//                                     the MTC and check lock, locate and jitter.

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <CoreMIDI/CoreMIDI.h>

#include <mach/mach_time.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

//==============================================================================
static int snapshot (const juce::File& out)
{
    MTC4LiveProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;

    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::printf ("could not write %s\n", out.getFullPathName().toRawUTF8());
        return 1;
    }

    std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
    return 0;
}

//==============================================================================
struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = false;
    double position = 0.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setTimeInSeconds (position);
        info.setBpm (120.0);
        info.setPpqPosition (position * 2.0);
        return info;
    }
};

struct Received
{
    double wallMs;
    std::vector<uint8_t> bytes;
};

struct Receiver
{
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    std::mutex lock;
    std::vector<Received> events;

    static void readProc (const MIDIPacketList* list, void* refCon, void*)
    {
        auto& self = *static_cast<Receiver*> (refCon);
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto* packet = &list->packet[0];

        // E2E_STAMP=1: use the CoreMIDI send timestamp instead of arrival
        // time, to separate sender lateness from delivery latency.
        static const bool useStamp = std::getenv ("E2E_STAMP") != nullptr;
        static const double machToMs = []
        {
            mach_timebase_info_data_t tb;
            mach_timebase_info (&tb);
            return (double) tb.numer / (double) tb.denom * 1.0e-6;
        }();
        const double offsetMs = now - (double) mach_absolute_time() * machToMs;

        const std::lock_guard<std::mutex> sl (self.lock);

        for (UInt32 i = 0; i < list->numPackets; ++i)
        {
            // Split running packets into messages (we only expect F1 xx and F0..F7)
            int pos = 0;
            while (pos < packet->length)
            {
                const auto status = packet->data[pos];
                int len = 1;

                if (status == 0xF1)
                    len = 2;
                else if (status == 0xF0)
                    while (pos + len < packet->length && packet->data[pos + len - 1] != 0xF7)
                        ++len;

                self.events.push_back ({ useStamp && packet->timeStamp != 0 ? (double) packet->timeStamp * machToMs + offsetMs : now, std::vector<uint8_t> (packet->data + pos, packet->data + pos + len) });
                pos += len;
            }

            packet = MIDIPacketNext (packet);
        }
    }

    bool connect (const juce::String& sourceName)
    {
        CFStringRef name = CFSTR ("MTC4Live e2e");
        if (MIDIClientCreate (name, nullptr, nullptr, &client) != noErr)
            return false;

       #pragma clang diagnostic push
       #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (MIDIInputPortCreate (client, CFSTR ("in"), readProc, this, &port) != noErr)
            return false;
       #pragma clang diagnostic pop

        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
        {
            const auto src = MIDIGetSource (i);
            CFStringRef s = nullptr;
            MIDIObjectGetStringProperty (src, kMIDIPropertyName, &s);
            const auto n = juce::String::fromCFString (s);
            if (s != nullptr) CFRelease (s);

            if (n == sourceName)
                return MIDIPortConnectSource (port, src, nullptr) == noErr;
        }

        return false;
    }

    ~Receiver()
    {
        if (port != 0) MIDIPortDispose (port);
        if (client != 0) MIDIClientDispose (client);
    }
};

/** One processBlock call: when, where, playing. */
struct BlockLog
{
    double wallMs, position;
    bool playing;
};

static double hostPositionAt (const std::vector<BlockLog>& blocks, double wallMs, bool& playing)
{
    // Latest block at or before wallMs, extrapolated
    const BlockLog* best = nullptr;
    for (const auto& b : blocks)
    {
        if (b.wallMs > wallMs) break;
        best = &b;
    }

    if (best == nullptr) { playing = false; return 0.0; }
    playing = best->playing;
    return best->playing ? best->position + (wallMs - best->wallMs) * 0.001 : best->position;
}

static bool runScenario (int rateIndex)
{
    const auto rate = mtc::frameRateFromIndex (rateIndex);
    const auto fps = mtc::actualFps (rate);
    const double sampleRate = 48000.0;
    const int blockSize = 256;

    MTC4LiveProcessor processor;
    processor.parameters.getParameter ("frameRate")->setValueNotifyingHost (rateIndex / 3.0f);
    processor.parameters.getParameter ("fullFrameWhenStopped")->setValueNotifyingHost (0.0f);
    FakePlayHead playHead;
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (sampleRate, blockSize);

    // Let the async port opening run, then attach the receiver
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

    Receiver receiver;
    if (! processor.isPortConnected() || ! receiver.connect (MTC4LiveProcessor::virtualPortName))
    {
        std::printf ("  could not connect to virtual port\n");
        return false;
    }

    // Script: (time from start in s, action)
    // 0.3 play from 10 s; 3.3 locate to 100 s; 5.3 loop wrap to 96 s;
    // 6.8 loop wrap to 96 s; 8.3 stop; 8.8 play from 40 s; 10.3 stop.
    struct Cue { double at; bool playing; double jumpTo; };
    const std::vector<Cue> cues {
        { 0.3, true, 10.0 }, { 3.3, true, 100.0 }, { 5.3, true, 96.0 },
        { 6.8, true, 96.0 }, { 8.3, false, -1.0 }, { 8.8, true, 40.0 }, { 10.3, false, -1.0 } };
    const double endAt = 10.6;

    std::vector<BlockLog> blocks;
    blocks.reserve (4000);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    bool audioUnchanged = true;

    const auto blockMs = blockSize / sampleRate * 1000.0;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    size_t nextCue = 0;

    for (int n = 0;; ++n)
    {
        const auto due = start + n * blockMs;
        const auto elapsed = (due - start) * 0.001;
        if (elapsed > endAt) break;

        while (nextCue < cues.size() && elapsed >= cues[nextCue].at)
        {
            playHead.playing = cues[nextCue].playing;
            if (cues[nextCue].jumpTo >= 0.0)
                playHead.position = cues[nextCue].jumpTo;
            ++nextCue;
        }

        // Sleep until the block is due (audio callbacks are periodic)
        for (auto now = juce::Time::getMillisecondCounterHiRes(); now < due; now = juce::Time::getMillisecondCounterHiRes())
            std::this_thread::sleep_for (std::chrono::microseconds (juce::jmax (50, (int) ((due - now) * 1000.0) - 200)));

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i)
                buffer.setSample (ch, i, std::sin ((float) (n * blockSize + i) * 0.01f + (float) ch));

        blocks.push_back ({ juce::Time::getMillisecondCounterHiRes(), playHead.position, playHead.playing });
        processor.processBlock (buffer, midi);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i)
                audioUnchanged = audioUnchanged
                    && buffer.getSample (ch, i) == std::sin ((float) (n * blockSize + i) * 0.01f + (float) ch);

        if (playHead.playing)
            playHead.position += blockSize / sampleRate;
    }

    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    std::vector<Received> events;
    {
        const std::lock_guard<std::mutex> sl (receiver.lock);
        events = receiver.events;
    }

    // ---- Analysis ----------------------------------------------------------
    const auto offsetFrames = mtc::timecodeToFrames ({ 1, 0, 0, 0 }, rate);
    const auto day = mtc::framesPerDay (rate);
    auto decodeFrames = [&] (const mtc::Timecode& tc) { return mtc::wrapFrames (mtc::timecodeToFrames (tc, rate) - offsetFrames, rate); };

    int fullFrames = 0, cycles = 0, badRate = 0, badFullFrame = 0;
    double maxPieceErrMs = 0.0, sumPieceErr = 0.0, sumSqPieceErr = 0.0;
    int pieceSamples = 0, lateCycles = 0, outOfOrder = 0, sentWhileStopped = 0;

    int nibbles[8] = {};
    double pieceTimes[8] = {};
    int expectedPiece = -1;

    for (const auto& e : events)
    {
        bool playing = false;
        const auto pos = hostPositionAt (blocks, e.wallMs, playing);

        if (e.bytes.size() == 10 && e.bytes[0] == 0xF0)
        {
            ++fullFrames;
            expectedPiece = 0;
            const mtc::Timecode tc { e.bytes[5] & 0x1f, e.bytes[6], e.bytes[7], e.bytes[8] };
            const auto frames = decodeFrames (tc);
            const auto err = std::abs (pos * fps - (double) frames - 0.5);
            if (((e.bytes[5] >> 5) & 3) != mtc::rateCode (rate) || err > 1.0)
            {
                ++badFullFrame;
                std::printf ("  full frame %s at host pos %.3f s (err %.2f frames)\n",
                             mtc::toString (tc, rate).c_str(), pos, err);
            }
            continue;
        }

        if (e.bytes.size() != 2 || e.bytes[0] != 0xF1)
            continue;

        if (! playing)
            ++sentWhileStopped;

        const int piece = e.bytes[1] >> 4;
        if (expectedPiece >= 0 && piece != expectedPiece)
            ++outOfOrder;
        expectedPiece = (piece + 1) & 7;

        nibbles[piece] = e.bytes[1] & 0x0f;
        pieceTimes[piece] = e.wallMs;

        if (piece == 7)
        {
            ++cycles;
            mtc::Timecode tc;
            tc.frames  = nibbles[0] | (nibbles[1] << 4);
            tc.seconds = nibbles[2] | (nibbles[3] << 4);
            tc.minutes = nibbles[4] | (nibbles[5] << 4);
            tc.hours   = nibbles[6] | ((nibbles[7] & 1) << 4);
            if (((nibbles[7] >> 1) & 3) != mtc::rateCode (rate))
                ++badRate;

            const auto cycleFrame = decodeFrames (tc);

            for (int k = 0; k < 8; ++k)
            {
                bool p = false;
                const auto hostFrames = hostPositionAt (blocks, pieceTimes[k], p) * fps;
                auto errFrames = hostFrames - ((double) cycleFrame + k * 0.25);
                if (errFrames > day / 2) errFrames -= day;
                const auto errMs = errFrames / fps * 1000.0;

                if (std::abs (errMs) > 1000.0 / fps)
                    ++lateCycles;

                if (std::abs (errMs) > 3.0 && std::getenv ("E2E_VERBOSE"))
                    std::printf ("    piece %d at t=%.3f s: err %+.2f ms\n", k, (pieceTimes[k] - start) * 0.001, errMs);
                maxPieceErrMs = juce::jmax (maxPieceErrMs, std::abs (errMs));
                sumPieceErr += errMs;
                sumSqPieceErr += errMs * errMs;
                ++pieceSamples;
            }
        }
    }

    const auto mean = pieceSamples > 0 ? sumPieceErr / pieceSamples : 0.0;
    const auto sd = pieceSamples > 0 ? std::sqrt (juce::jmax (0.0, sumSqPieceErr / pieceSamples - mean * mean)) : 0.0;

    // 2 starts + 1 locate + 2 loop wraps = 5 full frames expected
    const bool ok = audioUnchanged && fullFrames == 5 && badFullFrame == 0 && badRate == 0
                 && outOfOrder == 0 && lateCycles == 0 && sentWhileStopped == 0 && cycles > 100
                 && maxPieceErrMs < 5.0;

    std::printf ("  %-9s cycles %4d  full frames %d  QF timing error vs host: mean %+.3f ms, sd %.3f ms, max %.3f ms"
                 "  | out-of-order %d, >1 frame off %d, after stop %d, audio %s  -> %s\n",
                 rateIndex == 2 ? "29.97df" : juce::String (mtc::nominalFps (rate)).toRawUTF8(),
                 cycles, fullFrames, mean, sd, maxPieceErrMs, outOfOrder, lateCycles, sentWhileStopped,
                 audioUnchanged ? "unchanged" : "MODIFIED", ok ? "PASS" : "FAIL");

    processor.setPlayHead (nullptr);
    return ok;
}

static int e2e()
{
    bool ok = true;

    for (int rate = 0; rate < 4; ++rate)
        ok = runScenario (rate) && ok;

    std::printf (ok ? "E2E PASS\n" : "E2E FAIL\n");
    return ok ? 0 : 1;
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    if (argc >= 3 && juce::String (argv[1]) == "snapshot")
        return snapshot (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    if (argc >= 2 && juce::String (argv[1]) == "e2e")
        return e2e();

    std::printf ("usage: MTC4LiveTool snapshot <out.png> | e2e\n");
    return 2;
}
